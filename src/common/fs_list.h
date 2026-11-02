/* Transmission for the PS5: the shared path check and folder listing of the picker
 * (the settings page on port 9092 and the Transmission web interface on port 9091).
 *
 * The same code is compiled into the manager and into the daemon. The callers keep their own rules:
 *   - the manager adds the config.cfg limits of download folders (max 300, no " #") and its translated texts;
 *   - the daemon allows " #" in .torrent file names and a longer path, and returns error codes to the web page.
 *
 * License: GPL-3.0-or-later (like Transmission). */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ps5tm_fs
{

enum class Code
{
    Ok = 0,
    Empty,      // an empty path
    Relative,   // the path is not absolute
    TooLong,    // longer than the caller's limit
    Control,    // a control character
    SpaceHash,  // " #" (the comment marker of config.cfg; the caller decides whether it applies)
    Dots,       // ".." in the path
    Root,       // "/" itself
    Outside,    // not on an allowed disk (/data, /mnt/extN, /mnt/usbN)
    NoDir,      // the folder does not exist or cannot be read
    NoMem       // the answer does not fit into the caller's buffer
};

/* The rules common to both pickers: a trimmed absolute path, no control characters, no ".."; "//" and "." are
 * collapsed. max_len is the caller's limit for the trimmed path; space_hash rejects " #" (config.cfg). Fills comps
 * (the path components), normalized (the joined path) and trimmed (the input after trimming). */
Code normalize_common(std::string const& in, std::vector<std::string>& comps, std::string& normalized, std::string& trimmed,
                      std::size_t max_len, bool space_hash);

/* 1 = M.2 SSD (/mnt/extN), 2 = USB (/mnt/usbN), 3 = internal (/data/...), 0 = another place. root: the disk root. */
int folder_kind(std::vector<std::string> const& comps, std::string& root);

std::string join_path(std::vector<std::string> const& comps, std::size_t count);

struct Entry
{
    std::string name;
    long long size = 0;
    bool is_dir = false;
};

struct Listing
{
    Code code = Code::Ok;
    std::string path;
    std::string root;
    std::vector<Entry> dirs;
    std::vector<Entry> files;
    bool truncated = false;
};

/* The entries of a folder: subfolders and, if want_files, regular files whose name ends with ext (case-insensitive).
 * Hidden names, the service folders of the disks and names that are not valid UTF-8 are skipped; at most 500 of each kind are returned, sorted
 * case-insensitively (the first 5000 are collected before the cut). */
Listing list_dir(std::string const& path, std::string const& root, bool want_files, std::string const& ext);

/* {"path":...,"root":...,"dirs":[...],"files":[{"name":...,"size":...}],"truncated":...} */
std::string listing_json(Listing const& listing, bool include_files);

std::string json_quote(std::string const& s);

struct Disk
{
    std::string root;
    std::string kind; // ssd | usb | int
    std::string fs;   // exFAT, FAT32, UFS, BFS ("" for the internal disk)
    bool fsbad = false;
    long long total = 0;
    long long free = 0;
};

/* The connected disks /mnt/ext0..7 and /mnt/usb0..7, then /data (always). */
std::vector<Disk> disks();
std::string disks_json();

} // namespace ps5tm_fs
