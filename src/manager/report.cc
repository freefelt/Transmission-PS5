// Transmission for the PS5: the diagnostic report ("Diagnostics" on the settings page, the button "Prepare report").
//
// One text with everything the author needs to sort out a problem: the version, the build, the state of the system service and of the power monitor,
// the settings, the disks, the processes and the latest lines of every log. A computer downloads it as a file (GET /api/report); the console's browser
// cannot save files, so the manager writes it to /data/Transmission/report.txt (POST /api/report) and the page says where it is. The address of the
// console is replaced by <console-ip>; nothing else is hidden (the user is asked to look through the file: it has paths and the names of torrents).
//
// License: GPL-3.0-or-later (like Transmission).

#include "ui_internal.h"

#include <ctime>

extern "C"
{
char const* ps5tm_commit(void);
int write_file_atomic(char const* path, void const* data, std::size_t length);
}

namespace ps5tm_ui
{

namespace
{

constexpr char const* ReportFile = "/data/Transmission/report.txt";
constexpr char const* IpMask = "<console-ip>";

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// Replaces every whole occurrence of an IPv4 address (192.168.1.5 must not match inside 192.168.1.50 or 10.192.168.1.5)
void mask_address(std::string& text, std::string const& ip)
{
    if (ip.size() < 7 || ip == "127.0.0.1" || ip == "0.0.0.0")
    {
        return;
    }
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    for (;;)
    {
        std::size_t const at = text.find(ip, pos);
        if (at == std::string::npos)
        {
            break;
        }
        std::size_t const end = at + ip.size();
        bool const before_ok = at == 0 || !(is_digit(text[at - 1]) || (text[at - 1] == '.' && at >= 2 && is_digit(text[at - 2])));
        bool const after_ok = end >= text.size() || !(is_digit(text[end]) || (text[end] == '.' && end + 1 < text.size() && is_digit(text[end + 1])));
        out.append(text, pos, at - pos);
        out += before_ok && after_ok ? std::string{ IpMask } : ip;
        pos = end;
    }
    out.append(text, pos, std::string::npos);
    text.swap(out);
}

std::string section(char const* title, std::string body)
{
    if (!body.empty() && body.back() != '\n')
    {
        body += '\n';
    }
    return std::string{ "\n=== " } + title + "\n" + body;
}

// 0x11200000 -> "11.20 (0x11200000)"; the SDK version of the process parameter is the firmware as the SDK itself reads it
std::string firmware_text()
{
    unsigned const v = ps5tm_firmware();
    if (v == 0)
    {
        return "unknown (the SDK could not tell)";
    }
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%02x.%02x (0x%08x)", (v >> 24) & 0xffu, (v >> 16) & 0xffu, v);
    return buffer;
}

// "CFI-7019 (PS5 Pro)": the model name as the console reports it, with the family when the name has the CFI-Nxxx form (7 Pro, 2 Slim, 1 the original)
std::string model_text()
{
    std::string const m = ps5tm_hw_model();
    if (m.empty())
    {
        std::string const why = ps5tm_hw_model_note();
        return why.empty() ? "unknown" : "unknown (" + why + ")";
    }
    char const* family = "";
    if (m.size() > 4 && m.compare(0, 4, "CFI-") == 0)
    {
        family = m[4] == '7' ? "PS5 Pro" : m[4] == '2' ? "PS5 Slim" : m[4] == '1' ? "PS5 (original)" : "";
    }
    return *family != '\0' ? m + " (" + family + ")" : m;
}

std::string local_time_text()
{
    std::time_t const now = std::time(nullptr);
    char buffer[64] = "";
    std::tm const* const parts = std::localtime(&now);
    if (parts != nullptr)
    {
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", parts);
    }
    return std::string{ buffer } + " (unix time " + std::to_string(static_cast<long long>(now)) + ")";
}

} // namespace

// Sleep and daemon start/stop events from the manager log file (it survives a manager restart); the last 200 lines
std::string sleep_events_text()
{
    std::string const all = read_file_tail("/data/Transmission/logs/shim.log", 1500000);
    std::vector<std::string> keep;
    std::size_t pos = 0;
    while (pos < all.size())
    {
        std::size_t end = all.find('\n', pos);
        if (end == std::string::npos)
        {
            end = all.size();
        }
        std::string const line = all.substr(pos, end - pos);
        pos = end + 1;
        for (char const* key : { "sleep:", "frozen for", "resume detected", "daemon: start", "daemon: running", "daemon: paused", "daemon: SIGTERM", "daemon: restarting",
                                 "NOTIFY: Transmission stopped", "NOTIFY: Transmission started", "console will go to sleep", "====" })
        {
            if (line.find(key) != std::string::npos)
            {
                keep.push_back(line);
                break;
            }
        }
    }
    std::string text = "# sleep and daemon start/stop events from logs/shim.log (the last 200)\n";
    for (std::size_t i = keep.size() > 200 ? keep.size() - 200 : 0; i < keep.size(); ++i)
    {
        text += keep[i] + "\n";
    }
    return text;
}

std::string report_text()
{
    std::vector<char> ring(12000);
    std::size_t const ring_len = ps5tm_log_ring_copy(ring.data(), ring.size());

    std::string text = "# Transmission for the PS5: report\n";
    text += "# Look through this file before you send it: it has paths, the names of your torrents and the settings. The address of the console is replaced by ";
    text += std::string{ IpMask } + ".\n";
    text += "# created: " + local_time_text() + "\n";
    text += std::string{ "\n=== version " } + ps5tm_version() + ", commit " + ps5tm_commit() + ", SDK " + ps5tm_sdk_version() + ", built " + ps5tm_build_time() + ", running " +
        std::to_string(ps5tm_uptime_seconds()) + " s, Transmission " + PS5TM_TR_VERSION + "\n";
    text += "console firmware: " + firmware_text() + "\n";
    text += "console model: " + model_text() + "\n";
    text += section("system service", ps5tm_syssvc_json());
    text += section("power state", ps5tm_power_json());
    text += section("updates", updater_status_json() + "\n" + read_file_tail("/data/Transmission/update_state.txt", 1000));
    text += section("settings (config.cfg)", read_file_tail("/data/Transmission/config.cfg", 20000));
    text += section("Transmission check", run_isolated(ps5tm_rpc_probe_text, 8, "check"));
    text += section("disks", run_isolated(disks_report_text, 8, "disks"));
    text += section("mount points", run_isolated(mounts_report_text, 8, "mounts"));
    text += section("threads", ps5tm_thread_snapshot_text());
    text += section("console processes and ports", ps5tm_process_list_text());
    text += section("wrapper log (from memory)", std::string{ ring.data(), ring_len });
    text += section("manager log file (shim.log, latest)", read_file_tail("/data/Transmission/logs/shim.log", 30000));
    text += section("sleep and daemon events", sleep_events_text());
    text += section("Transmission log (latest)", read_file_tail("/data/Transmission/logs/transmission.log", 12000));
    text += section("daemon output (console.log, latest)", read_file_tail("/data/Transmission/logs/console.log", 6000));
    text += section("daemon wrapper log (daemon.log, latest)", read_file_tail("/data/Transmission/logs/daemon.log", 8000));

    mask_address(text, ps5tm_ip());
    char current[64] = "";
    if (ps5tm_current_ip(current, sizeof(current)) == 0)
    {
        mask_address(text, current);
    }
    return text;
}

// GET /api/report: the report as a file to download
void handle_report_download(int fd)
{
    std::string name;
    for (char const* p = ps5tm_version(); *p != '\0'; ++p)
    {
        bool const plain = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-';
        name += plain ? *p : '-';
    }
    send_response(
        fd,
        200,
        "OK",
        "text/plain; charset=utf-8",
        report_text(),
        ("Content-Disposition: attachment; filename=\"transmission-ps5-" + name + "-report.txt\"\r\n").c_str());
}

// POST /api/report {"action":"save"}: the report is written to /data/Transmission/report.txt (for the console's browser, which cannot save a file)
void handle_report_save(int fd, Request const& req)
{
    std::map<std::string, std::string> values;
    if (!parse_flat_json(req.body, values) || values["action"] != "save")
    {
        send_json(fd, 400, "Bad Request", std::string{ "{\"ok\":false,\"message\":" } + jstr(ps5tm_tr("mr_unknown_action")) + "}");
        return;
    }
    std::string const text = report_text();
    if (write_file_atomic(ReportFile, text.data(), text.size()) != 0)
    {
        shim_log("report: could not write %s", ReportFile);
        send_json(fd, 500, "Internal Server Error", std::string{ "{\"ok\":false,\"message\":" } + jstr(ps5tm_tr("mr_report_failed", { ReportFile })) + "}");
        return;
    }
    shim_log("report: %zu bytes written to %s", text.size(), ReportFile);
    send_json(fd, 200, "OK", std::string{ "{\"ok\":true,\"path\":" } + jstr(ReportFile) + ",\"size\":" + std::to_string(text.size()) + "}");
}

} // namespace ps5tm_ui
