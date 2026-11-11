// Transmission for the PS5: release update (check, download, replace the running payload, restart).
//
// License: GPL-3.0-or-later (like Transmission).
//
// The module is deliberately independent of Transmission: the repository, the release asset and the strings come from the
// constants below, and only the restart goes through the host (an ELF streamed to elfldr). A release is installed only when the
// file that PLDMGR launched is found and its SHA-256 matches the release's .sha256; a debug build is never updated.

#include "ui_internal.h"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ps5tm_ui
{
namespace
{

// ---------------------------------------------------------------- configuration

constexpr char const* DefaultApi = "https://api.github.com/repos/freefelt/Transmission-PS5/releases/latest";
constexpr char const* DefaultReleases = "https://github.com/freefelt/Transmission-PS5/releases/download";
constexpr char const* DefaultTag = "https://github.com/freefelt/Transmission-PS5/releases/tag/";
constexpr char const* DefaultSource = "https://github.com/freefelt/Transmission-PS5/releases";
constexpr char const* DefaultHistory = "http://127.0.0.1:8084/history_list";
constexpr char const* DefaultElfldr = "127.0.0.1:9021";
constexpr char const* PayloadsDir = "/data/pldmgr/payloads";
constexpr char const* AutoloadFile = "/data/pldmgr/autoload.txt";
constexpr std::size_t MaxElf = 64u << 20;
constexpr std::chrono::seconds CheckEvery{ 24 * 60 * 60 }; // an automatic check at most once a day
constexpr std::chrono::seconds CheckRetry{ 15 * 60 };      // ... but sooner again when GitHub could not be reached
constexpr std::chrono::seconds NoWait{ 0 };
constexpr char const* StateFile = "/data/Transmission/update_state.txt"; // last_check=<epoch seconds> and last_result=<word> of the last finished check
constexpr int MaxScanDepth = 3;

static std::string env_or(char const* name, char const* fallback)
{
    char const* value = std::getenv(name);
    return value && value[0] ? std::string{ value } : std::string{ fallback };
}

// Test hook: the manager does not start the release check by itself (the page tests must not reach GitHub).
static bool updates_disabled()
{
    char const* value = std::getenv("PS5TM_NO_UPDATE");
    return value && value[0];
}

static std::string api_url() { return env_or("PS5TM_UPDATER_API", DefaultApi); }
static std::string releases_base() { return env_or("PS5TM_UPDATER_RELEASES", DefaultReleases); }
static std::string tag_base() { return env_or("PS5TM_UPDATER_TAG_BASE", DefaultTag); }
static std::string source_page() { return env_or("PS5TM_UPDATER_SOURCE", DefaultSource); }
static std::string history_url() { return env_or("PS5TM_UPDATER_PLDMGR", DefaultHistory); }
static std::string elfldr_address() { return env_or("PS5TM_UPDATER_ELFLDR", DefaultElfldr); }

static std::string asset_url(std::string const& version)
{
    return releases_base() + "/" + version + "/Transmission-" + version + ".elf";
}
static std::string changelog_url(std::string const& version)
{
    return tag_base() + version;
}
static std::string sha_url(std::string const& version)
{
    return asset_url(version) + ".sha256";
}

// ---------------------------------------------------------------- state

enum class Phase
{
    Idle,
    Checking,
    Available,
    Downloading,
    Ready,
    Error,
};

static std::once_flag g_curl_once;
static void curl_init_once()
{
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static std::mutex g_mu;
static Phase g_phase = Phase::Idle;
static std::chrono::steady_clock::time_point g_next_check{}; // no automatic check before this moment
static bool g_restarting = false;
static std::string g_restart_error;
static std::time_t g_last_check = 0;   // when the last check ended (epoch seconds, 0 = never): kept in StateFile
static std::string g_last_result;      // how it ended: latest, available, notfound (the running file is not known) or failed
static bool g_state_loaded = false;
static std::string record_locked(char const* result); // g_mu held: remembers when and how the check ended, returns the text for StateFile
static std::string g_current;
static std::string g_latest;
static std::string g_changelog;
static std::string g_self;
static std::string g_old_sha;
static std::string g_error;
static std::string g_target;
static long long g_done = 0;
static long long g_total = 0;

static char const* phase_name(Phase phase)
{
    switch (phase)
    {
    case Phase::Idle: return "idle";
    case Phase::Checking: return "checking";
    case Phase::Available: return "available";
    case Phase::Downloading: return "downloading";
    case Phase::Ready: return "ready";
    case Phase::Error: return "error";
    }
    return "idle";
}

static void set_error(std::string const& text)
{
    std::lock_guard<std::mutex> lock{ g_mu };
    g_phase = Phase::Error;
    g_error = text;
    g_done = 0;
    g_total = 0;
    shim_log("update: %s", text.c_str());
}

static bool atomic_write(std::string const& path, std::string const& data);

// The check ended with nothing to offer: back to Idle. `result` says why; after a failure the next automatic check comes after `retry`.
static void set_none(char const* result, std::chrono::seconds retry)
{
    std::string saved;
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        g_phase = Phase::Idle;
        g_next_check = std::chrono::steady_clock::now() + retry;
        g_done = 0;
        g_total = 0;
        saved = record_locked(result);
    }
    atomic_write(StateFile, saved); // the disk is not touched while the lock is held
}

// The restart did not work: the update stays Ready (it is installed), the reason is shown and the button works again.
static void restart_failed(std::string const& text)
{
    std::lock_guard<std::mutex> lock{ g_mu };
    g_restarting = false;
    g_restart_error = text;
    shim_log("update: restart: %s", text.c_str());
}

// ---------------------------------------------------------------- HTTP

struct DownloadProgress
{
    long long total = 0;
    long long done = 0;
    long long last_percent = -1;
};

static size_t write_to_string(char* data, size_t size, size_t count, void* user)
{
    auto* out = static_cast<std::string*>(user);
    out->append(data, size * count);
    return size * count;
}

static size_t write_to_file(char* data, size_t size, size_t count, void* user)
{
    return fwrite(data, size, count, static_cast<FILE*>(user));
}

static int progress_cb(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    auto* p = static_cast<DownloadProgress*>(user);
    p->total = static_cast<long long>(dltotal);
    p->done = static_cast<long long>(dlnow);
    if (p->total > 0)
    {
        long long const percent = p->done * 100 / p->total;
        if (percent != p->last_percent)
        {
            p->last_percent = percent;
            std::lock_guard<std::mutex> lock{ g_mu };
            g_done = p->done;
            g_total = p->total;
        }
    }
    return 0;
}

static void curl_setup(CURL* curl, std::string const& url, char* error)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Transmission-PS5-Updater/1");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    error[0] = '\0';
    // The daemon writes the CA bundle here; on the host curl uses the system store.
    if (access("/data/Transmission/transmission/cacert.pem", R_OK) == 0)
    {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/data/Transmission/transmission/cacert.pem");
    }
}

static bool http_get(std::string const& url, std::string& body, long& status, std::string& error)
{
    curl_init_once();
    body.clear();
    status = 0;
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        error = "curl_easy_init failed";
        return false;
    }
    char curl_error[CURL_ERROR_SIZE];
    curl_setup(curl, url, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    CURLcode const rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
    {
        error = curl_error[0] ? std::string{ curl_error } : std::string{ curl_easy_strerror(rc) };
        return false;
    }
    return true;
}

static bool http_download(std::string const& url, std::string const& path, std::string& error)
{
    curl_init_once();
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        error = "cannot open " + path;
        return false;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file)
    {
        close(fd);
        unlink(path.c_str());
        error = "cannot write " + path;
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        fclose(file);
        unlink(path.c_str());
        error = "curl_easy_init failed";
        return false;
    }
    char curl_error[CURL_ERROR_SIZE];
    curl_setup(curl, url, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    DownloadProgress progress;
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    CURLcode const rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    bool flush_ok = fflush(file) == 0 && fsync(fd) == 0;
    if (fclose(file) != 0)
    {
        flush_ok = false;
    }
    if (!flush_ok)
    {
        unlink(path.c_str());
        error = "cannot flush " + path;
        return false;
    }
    if (rc != CURLE_OK || status != 200)
    {
        unlink(path.c_str());
        error = rc != CURLE_OK ? (curl_error[0] ? std::string{ curl_error } : std::string{ curl_easy_strerror(rc) })
                              : "the server answered " + std::to_string(status);
        return false;
    }
    std::lock_guard<std::mutex> lock{ g_mu };
    g_done = progress.done;
    g_total = progress.total;
    return true;
}

// ---------------------------------------------------------------- SHA-256 and files

static bool sha256_file(std::string const& path, std::string& hex)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
    {
        if (ctx) EVP_MD_CTX_free(ctx);
        close(fd);
        return false;
    }
    unsigned char buffer[64 * 1024];
    for (;;)
    {
        ssize_t const n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 || (n > 0 && EVP_DigestUpdate(ctx, buffer, static_cast<std::size_t>(n)) != 1))
        {
            EVP_MD_CTX_free(ctx);
            close(fd);
            return false;
        }
        if (n == 0)
        {
            break;
        }
    }
    close(fd);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    bool const digest_ok = EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!digest_ok || digest_len != 32)
    {
        return false;
    }
    static char const* digits = "0123456789abcdef";
    hex.clear();
    for (unsigned int i = 0; i < digest_len; ++i)
    {
        unsigned char const c = digest[i];
        hex += digits[c >> 4];
        hex += digits[c & 15];
    }
    return true;
}

static bool read_file(std::string const& path, std::string& out)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || static_cast<std::size_t>(st.st_size) > MaxElf)
    {
        close(fd);
        return false;
    }
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < out.size())
    {
        ssize_t const n = read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            close(fd);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    close(fd);
    return true;
}

static bool atomic_write(std::string const& path, std::string const& data)
{
    std::string const tmp = path + ".tmp";
    unlink(tmp.c_str());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        return false;
    }
    std::size_t done = 0;
    while (done < data.size())
    {
        ssize_t const n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (ok && rename(tmp.c_str(), path.c_str()) != 0)
    {
        ok = false;
    }
    if (!ok)
    {
        unlink(tmp.c_str());
    }
    return ok;
}

static bool is_elf(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    unsigned char magic[4];
    bool const ok = read(fd, magic, sizeof(magic)) == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    close(fd);
    return ok;
}

static bool ends_with(std::string const& text, std::string const& suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

static std::string parent_of(std::string const& path)
{
    std::size_t const at = path.find_last_of('/');
    return at == std::string::npos || at == 0 ? "/" : path.substr(0, at);
}

static std::string name_of(std::string const& path)
{
    std::size_t const at = path.find_last_of('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}

static std::string real_path(std::string const& path)
{
    char buffer[PATH_MAX];
    if (!realpath(path.c_str(), buffer))
    {
        return "";
    }
    return buffer;
}

static bool regular_elf(std::string& path)
{
    std::string const resolved = real_path(path);
    if (resolved.empty())
    {
        return false;
    }
    struct stat st {};
    if (stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || static_cast<std::size_t>(st.st_size) > MaxElf)
    {
        return false;
    }
    if (!ends_with(lower(resolved), ".elf"))
    {
        return false;
    }
    path = resolved;
    return true;
}

// ---------------------------------------------------------------- when was the last check

static bool known_result(std::string const& word)
{
    return word == "latest" || word == "available" || word == "notfound" || word == "failed";
}

static void load_state_locked()
{
    if (g_state_loaded)
    {
        return;
    }
    g_state_loaded = true;
    std::string text;
    if (!read_file(StateFile, text))
    {
        return;
    }
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        std::string const line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.compare(0, 11, "last_check=") == 0)
        {
            long long const value = std::strtoll(line.c_str() + 11, nullptr, 10); // strtoll is allowed on the PS5, atoll is not
            g_last_check = value > 0 ? static_cast<std::time_t>(value) : 0;
        }
        else if (line.compare(0, 12, "last_result=") == 0 && known_result(line.substr(12)))
        {
            g_last_result = line.substr(12);
        }
    }
}

static std::string record_locked(char const* result)
{
    load_state_locked();
    g_last_check = std::time(nullptr);
    g_last_result = result;
    return "last_check=" + std::to_string(static_cast<long long>(g_last_check)) + "\nlast_result=" + g_last_result + "\n";
}

// An automatic check is due when the retry delay of this run has passed and the last answer is old enough.
static bool check_due_locked()
{
    load_state_locked();
    if (std::chrono::steady_clock::now() < g_next_check)
    {
        return false;
    }
    std::time_t const now = std::time(nullptr);
    if (g_last_check == 0 || now < g_last_check)
    {
        return true; // never checked, or the clock was set back
    }
    if (g_last_result == "failed" || g_last_result == "available")
    {
        return true; // not a settled answer (an offer is forgotten when the manager restarts)
    }
    return now - g_last_check >= std::chrono::duration_cast<std::chrono::seconds>(CheckEvery).count();
}

// ---------------------------------------------------------------- JSON (small)

static bool json_string_at(std::string const& text, std::size_t pos, std::string& out)
{
    if (pos >= text.size() || text[pos] != '"')
    {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < text.size())
    {
        char const c = text[pos++];
        if (c == '"')
        {
            return true;
        }
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (pos >= text.size())
        {
            return false;
        }
        char const e = text[pos++];
        switch (e)
        {
        case '"':
        case '\\':
        case '/':
            out += e;
            break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        default: return false;
        }
    }
    return false;
}

static std::string json_field(std::string const& text, char const* field)
{
    std::string const key = std::string{ "\"" } + field + "\"";
    std::size_t pos = text.find(key);
    if (pos == std::string::npos)
    {
        return "";
    }
    pos = text.find(':', pos + key.size());
    if (pos == std::string::npos)
    {
        return "";
    }
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t'))
    {
        ++pos;
    }
    std::string out;
    return json_string_at(text, pos, out) ? out : "";
}

static bool history_paths(std::string const& text, std::vector<std::string>& out)
{
    std::size_t pos = text.find("\"history\"");
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = text.find('[', pos);
    if (pos == std::string::npos)
    {
        return false;
    }
    ++pos;
    for (;;)
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t' || text[pos] == ','))
        {
            ++pos;
        }
        if (pos >= text.size() || text[pos] == ']')
        {
            return true;
        }
        std::string path;
        if (!json_string_at(text, pos, path))
        {
            return false;
        }
        if (!path.empty())
        {
            out.push_back(path);
        }
        // skip the string just read: find its closing quote, then move past it
        ++pos; // opening quote
        bool escaped = false;
        while (pos < text.size())
        {
            char const c = text[pos++];
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- versions

static bool parse_version(std::string text, std::vector<long>& out)
{
    while (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t'))
    {
        text.pop_back();
    }
    out.clear();
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t end = text.find('.', pos);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        if (end == pos || end - pos > 9)
        {
            return false;
        }
        long value = 0;
        for (std::size_t i = pos; i < end; ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
            {
                return false;
            }
            value = value * 10 + (text[i] - '0');
        }
        out.push_back(value);
        pos = end + 1;
    }
    return !out.empty();
}

static bool newer(std::string const& latest, std::string const& current)
{
    std::vector<long> a;
    std::vector<long> b;
    if (!parse_version(latest, a) || !parse_version(current, b))
    {
        return false;
    }
    std::size_t const n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        long const x = i < a.size() ? a[i] : 0;
        long const y = i < b.size() ? b[i] : 0;
        if (x != y)
        {
            return x > y;
        }
    }
    return false;
}

static bool debug_version(std::string const& version)
{
    return version.find(" debug ") != std::string::npos;
}


static bool fetch_sha(std::string const& version, std::string& hash, bool& not_found)
{
    not_found = false;
    std::string body;
    long status = 0;
    std::string error;
    if (!http_get(sha_url(version), body, status, error))
    {
        shim_log("update: %s: %s", sha_url(version).c_str(), error.c_str());
        return false;
    }
    if (status == 404)
    {
        not_found = true;
        return false;
    }
    if (status != 200)
    {
        shim_log("update: %s: HTTP %ld", sha_url(version).c_str(), status);
        return false;
    }
    std::size_t pos = 0;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
    {
        ++pos;
    }
    hash.clear();
    while (pos < body.size() && hash.size() < 64 && std::isxdigit(static_cast<unsigned char>(body[pos])))
    {
        hash += static_cast<char>(std::tolower(static_cast<unsigned char>(body[pos++])));
    }
    return hash.size() == 64;
}

// ---------------------------------------------------------------- finding the launched file

static bool path_contains_transmission(std::string const& path)
{
    return lower(path).find("transmission") != std::string::npos;
}

static int canonical_score(std::string const& path)
{
    std::string const p = lower(path);
    if (p.find("/payloads/transmission/") != std::string::npos || p.find("/local/transmission/") != std::string::npos ||
        p.find("/repo/transmission@") != std::string::npos)
    {
        return 2;
    }
    if (p.find("/transmission/") != std::string::npos)
    {
        return 1;
    }
    return 0;
}

static bool hash_matches(std::string const& path, std::string const& expected)
{
    std::string got;
    return sha256_file(path, got) && got == expected;
}

static bool find_in_history(std::string const& old_sha, std::string& found)
{
    std::string body;
    long status = 0;
    std::string error;
    if (!http_get(history_url(), body, status, error) || status != 200)
    {
        shim_log("update: history: %s (HTTP %ld)", error.c_str(), status);
        return false;
    }
    shim_log("update: history: %s", body.c_str());
    std::vector<std::string> paths;
    if (!history_paths(body, paths))
    {
        shim_log("update: history: bad JSON");
        return false;
    }
    int best = -1;
    for (std::string const& item : paths)
    {
        std::string path = item;
        // Only a regular .elf file with "transmission" in its path is hashed (never an arbitrary file from the history).
        bool const elf = regular_elf(path);
        bool const named = elf && path_contains_transmission(path);
        bool const same = named && hash_matches(path, old_sha);
        if (!same)
        {
            shim_log("update: history candidate %s: elf=%d name=%d same=0", path.c_str(), elf ? 1 : 0, named ? 1 : 0);
            continue;
        }
        int const score = canonical_score(path);
        if (score > best)
        {
            best = score;
            found = path;
            if (score == 2)
            {
                break;
            }
        }
    }
    return !found.empty();
}

static void scan_for_self(std::string const& dir, std::string const& old_sha, int depth, std::set<std::string>& seen,
                          std::string& found, int& best)
{
    if (depth > MaxScanDepth)
    {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (!d)
    {
        return;
    }
    for (dirent* ent = readdir(d); ent; ent = readdir(d))
    {
        if (ent->d_name[0] == '.')
        {
            continue;
        }
        std::string const path = dir + "/" + ent->d_name;
        struct stat st {};
        if (lstat(path.c_str(), &st) != 0)
        {
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            scan_for_self(path, old_sha, depth + 1, seen, found, best);
            continue;
        }
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
        {
            continue;
        }
        if (S_ISREG(st.st_mode) && !ends_with(lower(ent->d_name), ".elf"))
        {
            continue; // not an ELF by its name: no realpath/stat for it
        }
        std::string resolved = path;
        if (!regular_elf(resolved) || !path_contains_transmission(resolved) || !seen.insert(resolved).second)
        {
            continue;
        }
        if (!hash_matches(resolved, old_sha))
        {
            continue;
        }
        int const score = canonical_score(resolved);
        if (score > best)
        {
            best = score;
            found = resolved;
        }
    }
    closedir(d);
}

static bool scan_for_self(std::string const& old_sha, std::string& found)
{
    std::set<std::string> seen;
    int best = -1;
    scan_for_self(PayloadsDir, old_sha, 0, seen, found, best);
    for (int i = 0; i < 8; ++i)
    {
        scan_for_self(std::string{ "/mnt/usb" } + std::to_string(i) + "/pldmgr", old_sha, 0, seen, found, best);
        scan_for_self(std::string{ "/mnt/usb" } + std::to_string(i), old_sha, 0, seen, found, best);
    }
    return !found.empty();
}

// ---------------------------------------------------------------- metadata

struct SelfInfo
{
    std::string path;
    std::string dir;
    std::string filename;
    std::string sidecar;
    std::string meta;
    std::string kind;
    int layout = 0; // 52: sidecar, 60: meta.json
    std::map<std::string, std::string> json;
};

static void inspect_self(SelfInfo& self)
{
    self.dir = parent_of(self.path);
    self.filename = name_of(self.path);
    self.sidecar.clear();
    self.meta.clear();
    self.layout = 0;
    self.kind.clear();
    self.json.clear();
    std::string const sidecar = self.path + ".json";
    struct stat st {};
    if (stat(sidecar.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    {
        self.sidecar = sidecar;
        self.layout = 52;
        std::string text;
        if (read_file(sidecar, text))
        {
            parse_flat_json(text, self.json);
        }
    }
    std::string const meta = self.dir + "/meta.json";
    if (stat(meta.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    {
        self.meta = meta;
        self.layout = 60;
        std::string text;
        if (read_file(meta, text))
        {
            parse_flat_json(text, self.json);
        }
        self.kind = self.json.count("kind") ? self.json["kind"] : "";
    }
}

static std::string today()
{
    char buffer[32];
    std::time_t const now = std::time(nullptr);
    std::tm const* tm = std::localtime(&now);
    if (!tm)
    {
        return "";
    }
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", tm);
    return buffer;
}

static std::string timestamp()
{
    char buffer[32];
    std::time_t const now = std::time(nullptr);
    std::tm const* tm = std::localtime(&now);
    if (!tm)
    {
        return "";
    }
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", tm);
    return buffer;
}

static std::string json_object(std::map<std::string, std::string> const& values, bool schema_number)
{
    std::string out = "{";
    bool first = true;
    for (auto const& item : values)
    {
        if (!first)
        {
            out += ",";
        }
        first = false;
        out += "\"" + item.first + "\":";
        out += schema_number && item.first == "schema" ? item.second : jstr(item.second);
    }
    return out + "}";
}

static bool write_sidecar(SelfInfo const& self, std::string const& target_file, std::string const& version, std::string const& sha)
{
    std::map<std::string, std::string> values = self.json;
    values["name"] = "Transmission";
    values["filename"] = name_of(target_file);
    values["url"] = asset_url(version);
    values["source"] = source_page();
    values["source_direct"] = asset_url(version);
    if (values["description"].empty())
    {
        values["description"] = "Transmission 4.1.4 as a PS5 homebrew payload";
    }
    values["last_update"] = today();
    values["version"] = version;
    values["checksum"] = sha;
    values["category"] = "Networking & Servers";
    values["downloaded_at"] = timestamp();
    if (values["install_source"].empty())
    {
        values["install_source"] = "web_upload";
    }
    std::string const sidecar = target_file + ".json";
    if (!atomic_write(sidecar, json_object(values, false)))
    {
        return false;
    }
    if (!self.sidecar.empty() && self.sidecar != sidecar)
    {
        unlink(self.sidecar.c_str()); // the sidecar of the old file name
    }
    return true;
}

static bool write_meta(SelfInfo const& self, std::string const& target_dir, std::string const& target_file, std::string const& version, std::string const& sha)
{
    std::map<std::string, std::string> values = self.json;
    values["schema"] = "2";
    values["filename"] = name_of(target_file);
    values["version"] = version;
    values["checksum"] = sha;
    values["url"] = asset_url(version);
    values["source"] = source_page();
    values["source_direct"] = asset_url(version);
    values["last_update"] = today();
    values["installed_at"] = timestamp();
    return atomic_write(target_dir + "/meta.json", json_object(values, true));
}

static void update_autoload(std::string const& old_name, std::string const& new_name)
{
    std::string text;
    if (!read_file(AutoloadFile, text))
    {
        return;
    }
    std::vector<std::string> lines;
    std::size_t pos = 0;
    bool changed = false;
    while (pos <= text.size())
    {
        std::size_t const end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (line == old_name)
        {
            line = new_name;
            changed = true;
        }
        lines.push_back(line);
        if (end == std::string::npos)
        {
            break;
        }
        pos = end + 1;
    }
    if (changed)
    {
        std::string out;
        for (std::string const& line : lines)
        {
            out += line + "\n";
        }
        atomic_write(AutoloadFile, out);
    }
}

// ---------------------------------------------------------------- apply

struct ApplyResult
{
    bool ok = false;
    std::string target;
    std::string error;
};

static ApplyResult apply_update(SelfInfo const& self, std::string const& version, std::string const& sha, std::string const& temp)
{
    ApplyResult result;
    // The file stays in its folder: PLDMGR does not care what the folder is called. A payload that PLDMGR describes (sidecar or
    // meta.json) gets the canonical name of the new version, a file found elsewhere keeps its name.
    std::string const target = self.dir + "/" + (self.layout ? "Transmission-" + version + ".elf" : self.filename);
    if (rename(temp.c_str(), target.c_str()) != 0)
    {
        result.error = "cannot replace " + target;
        return result;
    }
    // The metadata comes before the old file is removed: if it cannot be written, a file with a new name is taken back and the
    // old one is still there, listed and working.
    if ((self.layout == 52 && !write_sidecar(self, target, version, sha)) || (self.layout == 60 && !write_meta(self, self.dir, target, version, sha)))
    {
        if (target != self.path)
        {
            unlink(target.c_str());
        }
        result.error = "cannot write the metadata of " + target;
        return result;
    }
    if (target != self.path)
    {
        unlink(self.path.c_str());
    }
    if (self.layout == 52 && self.filename != name_of(target))
    {
        update_autoload(self.filename, name_of(target));
    }
    result.ok = true;
    result.target = target;
    return result;
}

// ---------------------------------------------------------------- threads

static void check_thread()
{
    std::string const current = ps5tm_version();
    shim_log("update: checking %s via %s", current.c_str(), api_url().c_str());
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        g_current = current;
        g_latest.clear();
        g_changelog.clear();
        g_self.clear();
        g_old_sha.clear();
        g_error.clear();
        g_target.clear();
    }
    if (debug_version(current))
    {
        set_none("latest", NoWait);
        return;
    }
    std::string body;
    long status = 0;
    std::string error;
    if (!http_get(api_url(), body, status, error) || status != 200)
    {
        shim_log("update: latest release: %s (HTTP %ld)", error.c_str(), status);
        set_none("failed", CheckRetry);
        return;
    }
    std::string const tag = json_field(body, "tag_name");
    if (tag.empty() || !newer(tag, current))
    {
        shim_log("update: no newer release (tag %s)", tag.c_str());
        set_none("latest", NoWait);
        return;
    }
    std::string latest = tag;
    while (!latest.empty() && (latest.front() == 'v' || latest.front() == 'V'))
    {
        latest.erase(latest.begin());
    }
    std::string old_sha;
    bool old_missing = false;
    if (!fetch_sha(current, old_sha, old_missing) || old_missing || old_sha.empty())
    {
        shim_log("update: no usable .sha256 for %s (404=%d)", current.c_str(), old_missing ? 1 : 0);
        if (old_missing)
        {
            set_none("notfound", NoWait);
        }
        else
        {
            set_none("failed", CheckRetry);
        }
        return;
    }
    std::string self;
    if (!find_in_history(old_sha, self) && !scan_for_self(old_sha, self))
    {
        shim_log("update: the running file was not found");
        set_none("notfound", NoWait);
        return;
    }
    shim_log("update: found %s", self.c_str());
    std::string saved;
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        g_current = current;
        g_latest = latest;
        g_changelog = changelog_url(latest);
        g_self = self;
        g_old_sha = old_sha;
        g_phase = Phase::Available;
        g_error.clear();
        saved = record_locked("available");
    }
    atomic_write(StateFile, saved);
}

static void download_thread()
{
    std::string self;
    std::string latest;
    std::string old_sha;
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        self = g_self;
        latest = g_latest;
        old_sha = g_old_sha;
    }
    std::string new_sha;
    bool missing = false;
    if (!fetch_sha(latest, new_sha, missing) || missing || new_sha.empty())
    {
        set_error("cannot read the new version's SHA-256");
        return;
    }
    SelfInfo info;
    info.path = self;
    inspect_self(info);
    std::string const temp = info.dir + "/." + "Transmission-" + latest + ".elf.part";
    unlink(temp.c_str());
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        g_done = 0;
        g_total = 0;
    }
    std::string error;
    if (!http_download(asset_url(latest), temp, error))
    {
        unlink(temp.c_str());
        set_error("download failed: " + error);
        return;
    }
    std::string downloaded;
    if (!sha256_file(temp, downloaded) || downloaded != new_sha)
    {
        unlink(temp.c_str());
        set_error("the new file does not match its SHA-256");
        return;
    }
    ApplyResult const applied = apply_update(info, latest, new_sha, temp);
    if (!applied.ok)
    {
        unlink(temp.c_str());
        set_error(applied.error);
        return;
    }
    std::lock_guard<std::mutex> lock{ g_mu };
    g_target = applied.target;
    g_phase = Phase::Ready;
    g_error.clear();
}

static bool parse_elfldr(std::string const& text, sockaddr_in& address)
{
    std::string host = text;
    std::string port = "9021";
    std::size_t const at = text.rfind(':');
    if (at != std::string::npos)
    {
        host = text.substr(0, at);
        port = text.substr(at + 1);
    }
    address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(std::atoi(port.c_str())));
    return inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1 && address.sin_port != 0;
}

static void restart_thread()
{
    std::string target;
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        target = g_target;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::string elf;
    if (!read_file(target, elf) || !is_elf(target))
    {
        restart_failed("cannot read the updated file");
        return;
    }
    sockaddr_in address {};
    if (!parse_elfldr(elfldr_address(), address))
    {
        restart_failed("bad ELF loader address");
        return;
    }
    int const sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        restart_failed("cannot open the loader connection");
        return;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        close(sock);
        restart_failed("the ELF loader is not listening");
        return;
    }
    std::size_t sent = 0;
    while (sent < elf.size())
    {
        ssize_t const n = send(sock, elf.data() + sent, elf.size() - sent, 0);
        if (n <= 0)
        {
            close(sock);
            restart_failed("the ELF could not be sent to the loader");
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
    shutdown(sock, SHUT_WR);
    close(sock);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ps5tm_exit_now();
}

} // namespace

std::string updater_status_json()
{
    bool start = false;
    {
        std::lock_guard<std::mutex> lock{ g_mu };
        if (g_phase == Phase::Idle && g_cfg.update_check.load() && !updates_disabled() && !debug_version(ps5tm_version()) && check_due_locked())
        {
            shim_log("update: check requested");
            g_phase = Phase::Checking; // so that the page keeps asking while the check runs
            start = true;
        }
    }
    if (start)
    {
        std::thread(check_thread).detach();
    }
    std::lock_guard<std::mutex> lock{ g_mu };
    load_state_locked();
    long long const percent = g_total > 0 ? std::min<long long>(100, g_done * 100 / g_total) : 0;
    std::string json = "{";
    json += "\"state\":" + jstr(phase_name(g_phase));
    json += ",\"current\":" + jstr(g_current);
    json += ",\"latest\":" + jstr(g_latest);
    json += ",\"changelog\":" + jstr(g_changelog);
    json += ",\"progress\":" + std::to_string(percent);
    json += ",\"downloaded\":" + std::to_string(g_done);
    json += ",\"total\":" + std::to_string(g_total);
    json += ",\"error\":" + jstr(g_error);
    json += ",\"can_update\":" + jbool(g_phase == Phase::Available);
    json += ",\"target\":" + jstr(g_target);
    json += ",\"enabled\":" + jbool(g_cfg.update_check.load());
    json += ",\"last_check\":" + std::to_string(static_cast<long long>(g_last_check));
    json += ",\"last_result\":" + jstr(g_last_result);
    json += ",\"restarting\":" + jbool(g_restarting);
    json += ",\"restart_error\":" + jstr(g_restart_error);
    return json + "}";
}

std::string updater_action(std::string const& body)
{
    std::map<std::string, std::string> args;
    if (!parse_flat_json(body, args) || !args.count("action"))
    {
        return "{\"ok\":false,\"message\":\"bad request\"}";
    }
    std::string const& action = args["action"];
    if (action == "check")
    {
        {
            std::lock_guard<std::mutex> lock{ g_mu };
            if (g_phase == Phase::Checking || g_phase == Phase::Downloading)
            {
                return "{\"ok\":false,\"message\":\"busy\"}";
            }
            if (g_phase == Phase::Ready)
            {
                return "{\"ok\":false,\"message\":\"the update is already installed: restart\"}";
            }
            g_phase = Phase::Checking; // from Idle, Available or Error (an explicit check ignores the once-a-day limit)
        }
        std::thread(check_thread).detach();
        return "{\"ok\":true}";
    }
    if (action == "download")
    {
        {
            std::lock_guard<std::mutex> lock{ g_mu };
            if (g_phase != Phase::Available)
            {
                return "{\"ok\":false,\"message\":\"no update\"}";
            }
            g_phase = Phase::Downloading;
            g_done = 0;
            g_total = 0;
        }
        std::thread(download_thread).detach();
        return "{\"ok\":true}";
    }
    if (action == "restart")
    {
        {
            std::lock_guard<std::mutex> lock{ g_mu };
            if (g_phase != Phase::Ready || g_target.empty())
            {
                return "{\"ok\":false,\"message\":\"no update\"}";
            }
            if (g_restarting)
            {
                return "{\"ok\":false,\"message\":\"busy\"}"; // a second click while the first restart is under way
            }
            g_restarting = true;
            g_restart_error.clear();
        }
        std::thread(restart_thread).detach();
        return "{\"ok\":true}";
    }
    return "{\"ok\":false,\"message\":\"unknown action\"}";
}

} // namespace ps5tm_ui
