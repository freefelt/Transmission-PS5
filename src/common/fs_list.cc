// Transmission for the PS5: the shared path check and folder listing of the picker (see fs_list.h).
//
// License: GPL-3.0-or-later (like Transmission).

#include "fs_list.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

// The helpers of src/common/volume.c (compiled into both roles). Declared here so that this file does not need
// ps5tm_common.h (and with it the role macro): it is compiled once for the manager and once for the daemon.
extern "C" int ps5tm_resolve_alias(const char* in, char* out, size_t size);
extern "C" int ps5tm_volume_info(const char* root, char* fstype, size_t fssize, char* device, size_t devsize);
extern "C" int ps5tm_disk_kind_of_root(const char* root);
extern "C" int ps5tm_fs_needs_fix(const char* fstype);

namespace ps5tm_fs
{

namespace
{

void trim(std::string& s)
{
    std::size_t begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t'))
    {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t'))
    {
        --end;
    }
    s = s.substr(begin, end - begin);
}

bool lower_less(std::string const& a, std::string const& b)
{
    auto lower = [](std::string s)
    {
        for (auto& c : s)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };
    std::string const la = lower(a);
    std::string const lb = lower(b);
    return la != lb ? la < lb : a < b;
}

bool has_ext(std::string const& name, std::string const& ext)
{
    if (ext.empty() || name.size() < ext.size())
    {
        return false;
    }
    std::string const tail = name.substr(name.size() - ext.size());
    for (std::size_t i = 0; i < tail.size(); ++i)
    {
        char a = tail[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z')
        {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

// A name that is not valid UTF-8 cannot be shown, chosen or put into the JSON of the picker (the daemon's JSON parser may
// refuse the whole answer), so such entries are left out of the listing. Strict: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string const& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        unsigned char const c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        unsigned int cp = 0;
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
        {
            len = 2;
            cp = c & 0x1Fu;
        }
        else if (c >= 0xE0 && c <= 0xEF)
        {
            len = 3;
            cp = c & 0x0Fu;
        }
        else if (c >= 0xF0 && c <= 0xF4)
        {
            len = 4;
            cp = c & 0x07u;
        }
        else
        {
            return false;
        }
        if (i + len > s.size())
        {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k)
        {
            unsigned char const t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0u) != 0x80u)
            {
                return false;
            }
            cp = (cp << 6) | (t & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            return false;
        }
        i += len;
    }
    return true;
}

struct Volume
{
    bool mounted = false;
    double total = 0;
    double free = 0;
};

Volume probe_volume(std::string const& path)
{
    Volume v;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    {
        return v;
    }
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) == 0)
    {
        double const frsize = sv.f_frsize != 0 ? static_cast<double>(sv.f_frsize) : static_cast<double>(sv.f_bsize);
        v.total = static_cast<double>(sv.f_blocks) * frsize;
        v.free = static_cast<double>(sv.f_bavail) * frsize;
    }
    struct stat parent;
    v.mounted = stat("/mnt", &parent) == 0 && st.st_dev != parent.st_dev && v.total > 0;
    return v;
}

std::string fs_name(std::string const& raw)
{
    return raw == "exfatfs" || raw == "exfat" ? "exFAT" : raw == "msdosfs" || raw == "vfat" ? "FAT32" : raw == "ufs" ? "UFS" : raw == "bfs" ? "BFS" : raw;
}

} // namespace

std::string join_path(std::vector<std::string> const& comps, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count && i < comps.size(); ++i)
    {
        out += "/" + comps[i];
    }
    return out;
}

Code normalize_common(std::string const& in, std::vector<std::string>& comps, std::string& normalized, std::string& trimmed,
                      std::size_t max_len, bool space_hash)
{
    comps.clear();
    normalized.clear();
    trimmed = in;
    trim(trimmed);
    if (trimmed.empty())
    {
        return Code::Empty;
    }
    if (trimmed[0] != '/')
    {
        return Code::Relative;
    }
    if (trimmed.size() > max_len)
    {
        return Code::TooLong;
    }
    for (unsigned char const c : trimmed)
    {
        if (c < 0x20 || c == 0x7f)
        {
            return Code::Control;
        }
    }
    if (space_hash && trimmed.find(" #") != std::string::npos)
    {
        return Code::SpaceHash;
    }
    std::size_t i = 0;
    while (i <= trimmed.size())
    {
        auto j = trimmed.find('/', i);
        if (j == std::string::npos)
        {
            j = trimmed.size();
        }
        std::string const part = trimmed.substr(i, j - i);
        if (part == "..")
        {
            return Code::Dots;
        }
        if (!part.empty() && part != ".")
        {
            comps.push_back(part);
        }
        i = j + 1;
    }
    if (comps.empty())
    {
        return Code::Root;
    }
    normalized = join_path(comps, comps.size());
    return Code::Ok;
}

int folder_kind(std::vector<std::string> const& comps0, std::string& root)
{
    if (comps0.empty())
    {
        return 0;
    }
    // The path may be an alias (BindMount: /data/ext1 is /mnt/ext1): first the real path, then the disk kind.
    std::vector<std::string> comps = comps0;
    {
        std::string const full = join_path(comps0, comps0.size());
        char real[600] = { 0 };
        if (ps5tm_resolve_alias(full.c_str(), real, sizeof(real)) != 0 && real[0] == '/')
        {
            std::vector<std::string> parts;
            std::string const rs{ real };
            std::size_t i = 0;
            while (i <= rs.size())
            {
                auto j = rs.find('/', i);
                if (j == std::string::npos)
                {
                    j = rs.size();
                }
                if (j > i)
                {
                    parts.push_back(rs.substr(i, j - i));
                }
                i = j + 1;
            }
            if (!parts.empty())
            {
                comps = parts;
            }
        }
    }
    if (comps[0] == "mnt" && comps.size() >= 2 && comps[1].size() == 4 && comps[1][3] >= '0' && comps[1][3] <= '9' &&
        (comps[1].compare(0, 3, "ext") == 0 || comps[1].compare(0, 3, "usb") == 0))
    {
        root = "/mnt/" + comps[1];
        return ps5tm_disk_kind_of_root(root.c_str());
    }
    if (comps[0] == "data" && comps.size() >= 2)
    {
        root = "/data";
        return 3;
    }
    return 0;
}

Listing list_dir(std::string const& path, std::string const& root, bool want_files, std::string const& ext)
{
    Listing l;
    l.path = path;
    l.root = root;

    DIR* d = opendir(path.c_str());
    if (d == nullptr)
    {
        l.code = Code::NoDir;
        return l;
    }

    std::vector<std::string> dirs;
    std::vector<Entry> files;
    bool truncated = false;
    while (dirent* e = readdir(d))
    {
        std::string const name = e->d_name;
        if (name.empty() || name[0] == '.' || name == "lost+found" || name == "System Volume Information" || name == "$RECYCLE.BIN" ||
            (path == "/data" && name == "Transmission") || !valid_utf8(name))
        {
            continue;
        }
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
        {
            struct stat st;
            is_dir = stat((path + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir)
        {
            if (dirs.size() >= 5000)
            {
                truncated = true;
                break;
            }
            dirs.push_back(name);
            continue;
        }
        if (!want_files || !has_ext(name, ext))
        {
            continue;
        }
        struct stat st;
        if (stat((path + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        if (files.size() >= 5000)
        {
            truncated = true;
            break;
        }
        Entry f;
        f.name = name;
        f.size = static_cast<long long>(st.st_size);
        files.push_back(std::move(f));
    }
    closedir(d);

    std::sort(dirs.begin(), dirs.end(), lower_less);
    if (dirs.size() > 500)
    {
        dirs.resize(500);
        truncated = true;
    }
    std::sort(files.begin(), files.end(), [](Entry const& a, Entry const& b) { return lower_less(a.name, b.name); });
    if (files.size() > 500)
    {
        files.resize(500);
        truncated = true;
    }

    l.truncated = truncated;
    l.dirs.reserve(dirs.size());
    for (auto const& name : dirs)
    {
        Entry e;
        e.name = name;
        e.is_dir = true;
        l.dirs.push_back(std::move(e));
    }
    l.files = std::move(files);
    return l;
}

std::string json_quote(std::string const& s)
{
    std::string out = "\"";
    for (char const c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
        }
    }
    return out + "\"";
}

std::string listing_json(Listing const& listing, bool include_files)
{
    std::string out = "{\"path\":" + json_quote(listing.path) + ",\"root\":" + json_quote(listing.root) + ",\"dirs\":[";
    for (std::size_t i = 0; i < listing.dirs.size(); ++i)
    {
        out += (i != 0 ? "," : "") + json_quote(listing.dirs[i].name);
    }
    out += "]";
    if (include_files)
    {
        out += ",\"files\":[";
        for (std::size_t i = 0; i < listing.files.size(); ++i)
        {
            out += std::string{ i != 0 ? "," : "" } + "{\"name\":" + json_quote(listing.files[i].name) + ",\"size\":" + std::to_string(listing.files[i].size) + "}";
        }
        out += "]";
    }
    out += std::string{ ",\"truncated\":" } + (listing.truncated ? "true" : "false") + "}";
    return out;
}

std::vector<Disk> disks()
{
    std::vector<Disk> out;
    for (char const* const prefix : { "/mnt/ext", "/mnt/usb" })
    {
        for (int i = 0; i < 8; ++i)
        {
            std::string const root = std::string{ prefix } + std::to_string(i);
            Volume const v = probe_volume(root);
            if (!v.mounted)
            {
                continue;
            }
            Disk d;
            d.root = root;
            d.kind = ps5tm_disk_kind_of_root(root.c_str()) == 2 ? "usb" : "ssd";
            char fs[32] = { 0 };
            char dev[128] = { 0 };
            if (ps5tm_volume_info(root.c_str(), fs, sizeof(fs), dev, sizeof(dev)) != 0)
            {
                d.fs = fs_name(fs);
                d.fsbad = ps5tm_fs_needs_fix(fs) != 0;
            }
            d.total = static_cast<long long>(v.total);
            d.free = static_cast<long long>(v.free);
            out.push_back(std::move(d));
        }
    }
    {
        Disk d;
        d.root = "/data";
        d.kind = "int";
        struct statvfs sv;
        if (statvfs("/data", &sv) == 0)
        {
            double const frsize = sv.f_frsize != 0 ? static_cast<double>(sv.f_frsize) : static_cast<double>(sv.f_bsize);
            d.total = static_cast<long long>(static_cast<double>(sv.f_blocks) * frsize);
            d.free = static_cast<long long>(static_cast<double>(sv.f_bavail) * frsize);
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::string disks_json()
{
    std::vector<Disk> const list = disks();
    std::string out = "{\"disks\":[";
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        Disk const& d = list[i];
        out += std::string{ i != 0 ? "," : "" } + "{\"root\":" + json_quote(d.root) + ",\"kind\":" + json_quote(d.kind) + ",\"fs\":" + json_quote(d.fs) +
               ",\"fsbad\":" + (d.fsbad ? "true" : "false") + ",\"total\":" + std::to_string(d.total) + ",\"free\":" + std::to_string(d.free) + "}";
    }
    return out + "]}";
}

} // namespace ps5tm_fs
