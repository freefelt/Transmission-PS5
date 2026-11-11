// Transmission for the PS5: the model of the console (CFI-xxxx) for the report, to tell a PS5 Pro from the others.
//
// The SDK samples (hwinfo) read it with sceKernelGetHwModelName(char*), which lives in libkernel_sys; our ELF does not import that library, so it is
// loaded at run time (dlopen/dlsym, like the system service), once, in a thread of its own: a hang or a crash there must not touch anything else.
// Like the system service it leaves a probe file before the risky step; if the next start finds it, the step is skipped from then on (hwmodel_skip.txt).
// The serial number (sceKernelGetHwSerialNumber) is deliberately NOT read: the report is sent to other people.
// Not verified on a console: what string the function returns (the report shows it as it is and adds "PS5 Pro" and the like only for CFI-7xxx, CFI-2xxx, CFI-1xxx).
//
// License: GPL-3.0-or-later (like Transmission).

#include "core_internal.h"

#include <cstring>
#include <dlfcn.h>

namespace ps5tm_core
{

namespace
{

[[maybe_unused]] constexpr char const* ProbeFile = "/data/Transmission/hwmodel_probe.txt";
[[maybe_unused]] constexpr char const* SkipFile = "/data/Transmission/hwmodel_skip.txt";

std::mutex g_hw_mutex;
std::string g_hw_model;
std::string g_hw_note = "not asked yet";

[[maybe_unused]] void hw_set(std::string const& model, std::string const& note)
{
    std::lock_guard<std::mutex> lock(g_hw_mutex);
    g_hw_model = model;
    g_hw_note = note;
}

// Printable ASCII only, at most 48 characters, no spaces at the ends
std::string hw_clean(char const* raw)
{
    std::string out;
    for (std::size_t i = 0; raw[i] != '\0' && out.size() < 48; ++i)
    {
        unsigned char const c = static_cast<unsigned char>(raw[i]);
        out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    while (!out.empty() && out.back() == ' ')
    {
        out.pop_back();
    }
    std::size_t start = 0;
    while (start < out.size() && out[start] == ' ')
    {
        ++start;
    }
    return out.substr(start);
}

[[maybe_unused]] void hw_mark(char const* what)
{
    if (FILE* f = std::fopen(ProbeFile, "w"))
    {
        std::fputs(what, f);
        std::fclose(f);
    }
}

} // namespace

void hwmodel_main()
{
#ifdef __linux__
    return; // the host answers from the file /tmp/ps5tm_sys_model (see ps5tm_hw_model)
#else
    if (FILE* sf = std::fopen(SkipFile, "r"))
    {
        std::fclose(sf);
        hw_set("", "skipped: an earlier start did not return from it (delete /data/Transmission/hwmodel_skip.txt to try again)");
        return;
    }
    if (FILE* f = std::fopen(ProbeFile, "r"))
    {
        char what[96] = {};
        if (std::fgets(what, sizeof(what), f) == nullptr)
        {
            what[0] = 0;
        }
        std::fclose(f);
        std::remove(ProbeFile);
        if (FILE* sf = std::fopen(SkipFile, "w"))
        {
            std::fprintf(sf, "%s\n", what[0] != 0 ? what : "?");
            std::fclose(sf);
        }
        shim_log("hwmodel: the previous start did not return from %s: not asking again", what);
        hw_set("", std::string{ "skipped: an earlier start did not return from " } + what);
        return;
    }
    hw_set("", "loading");
    hw_mark("dlopen libkernel_sys.sprx");
    void* const lib = dlopen("libkernel_sys.sprx", RTLD_LAZY);
    if (lib == nullptr)
    {
        std::remove(ProbeFile);
        char const* const e = dlerror();
        shim_log("hwmodel: dlopen(libkernel_sys.sprx) failed: %s", e != nullptr ? e : "?");
        hw_set("", "libkernel_sys.sprx could not be loaded");
        return;
    }
    auto const fn = reinterpret_cast<int (*)(char*)>(dlsym(lib, "sceKernelGetHwModelName"));
    if (fn == nullptr)
    {
        std::remove(ProbeFile);
        shim_log("hwmodel: sceKernelGetHwModelName was not found");
        hw_set("", "sceKernelGetHwModelName was not found");
        return;
    }
    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));
    hw_mark("sceKernelGetHwModelName");
    int const rc = fn(buffer);
    std::remove(ProbeFile);
    buffer[sizeof(buffer) - 1] = '\0';
    if (rc != 0)
    {
        char note[64];
        std::snprintf(note, sizeof(note), "sceKernelGetHwModelName returned 0x%08x", static_cast<unsigned>(rc));
        shim_log("hwmodel: %s", note);
        hw_set("", note);
        return;
    }
    std::string const model = hw_clean(buffer);
    shim_log("hwmodel: the console model is \"%s\"", model.c_str());
    hw_set(model, model.empty() ? "the function returned an empty string" : "");
#endif
}

} // namespace ps5tm_core

using namespace ps5tm_core;

// The model name as the console reports it ("" if unknown) and why it is unknown
std::string ps5tm_hw_model()
{
#ifdef __linux__
    if (FILE* f = std::fopen("/tmp/ps5tm_sys_model", "r")) // test hook: the model name, absent = CFI-1015A
    {
        char b[96] = {};
        if (std::fgets(b, sizeof(b), f) == nullptr)
        {
            b[0] = 0;
        }
        std::fclose(f);
        b[std::strcspn(b, "\r\n")] = 0;
        return hw_clean(b);
    }
    return "CFI-1015A";
#else
    std::lock_guard<std::mutex> lock(g_hw_mutex);
    return g_hw_model;
#endif
}

std::string ps5tm_hw_model_note()
{
#ifdef __linux__
    return ps5tm_hw_model().empty() ? "no model in the test hook file" : "";
#else
    std::lock_guard<std::mutex> lock(g_hw_mutex);
    return g_hw_note;
#endif
}
