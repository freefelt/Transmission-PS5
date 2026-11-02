// Transmission for the PS5: the read-only file picker of the Transmission web interface (RPC method ps5_files).
//
// The path rules of the file picker: the common checks of the shared module (absolute, no control characters, no ".."),
// no config.cfg limits (" #" is allowed in a .torrent file name, the path may be longer than 300). The listing itself
// (subfolders, *.torrent files with their sizes, sorting, the 500-entry cut) is the shared code.
//
// License: GPL-3.0-or-later (like Transmission).

#include "fs_list.h"

#include <cstring>
#include <string>

namespace
{

// 0 = allowed (path and root are filled); 1 = outside the allowed disks; 3 = a bad path.
int check_path(std::string const& in, std::string& path, std::string& root)
{
    std::vector<std::string> comps;
    std::string normalized;
    std::string trimmed;
    if (ps5tm_fs::normalize_common(in, comps, normalized, trimmed, 1024, false) != ps5tm_fs::Code::Ok)
    {
        return 3;
    }
    if (comps.size() == 1 && comps[0] == "data")
    {
        path = "/data";
        root = "/data";
        return 0;
    }
    if (ps5tm_fs::folder_kind(comps, root) == 0)
    {
        return 1;
    }
    path = ps5tm_fs::join_path(comps, comps.size());
    return 0;
}

} // namespace

/* The listing of a folder for the web interface: JSON in out (zero-terminated). want_files: also return the *.torrent
 * files of the folder. Returns 0 on success; on failure -1 and *error is 1 (outside), 2 (the folder is unavailable)
 * or 3 (a bad path). */
extern "C" int ps5tm_files_json(char const* in, int want_files, char const* ext, char* out, size_t out_size, int* truncated, int* error)
{
    if (in == nullptr || out == nullptr || out_size < 2 || truncated == nullptr || error == nullptr)
    {
        return -1;
    }
    *truncated = 0;
    std::string path;
    std::string root;
    int const bad = check_path(std::string{ in }, path, root);
    if (bad != 0)
    {
        *error = bad;
        return -1;
    }
    ps5tm_fs::Listing const listing = ps5tm_fs::list_dir(path, root, want_files != 0, ext != nullptr ? std::string{ ext } : std::string{});
    if (listing.code != ps5tm_fs::Code::Ok)
    {
        *error = listing.code == ps5tm_fs::Code::NoDir ? 2 : 3;
        return -1;
    }
    std::string const json = ps5tm_fs::listing_json(listing, want_files != 0);
    if (json.size() + 1 > out_size)
    {
        *error = 3;
        return -1;
    }
    std::memcpy(out, json.c_str(), json.size() + 1);
    *truncated = listing.truncated ? 1 : 0;
    *error = 0;
    return 0;
}

/* The connected disks (/mnt/extN, /mnt/usbN) and /data for the disk list of the picker. Returns 0 on success. */
extern "C" int ps5tm_disks_json(char* out, size_t out_size)
{
    if (out == nullptr || out_size < 2)
    {
        return -1;
    }
    std::string const json = ps5tm_fs::disks_json();
    if (json.size() + 1 > out_size)
    {
        return -1;
    }
    std::memcpy(out, json.c_str(), json.size() + 1);
    return 0;
}
