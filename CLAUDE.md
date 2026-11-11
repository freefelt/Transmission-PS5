# Transmission for PS5: project notes for coding agents and contributors

Transmission 4.1.4 as a PS5 homebrew payload. One file, `build/Transmission-<version>.elf` (the **manager**), carries the **daemon**
(`transmission_d.elf`) and a short-lived **icon helper** (`transmission_i.elf`). This file is the long-term memory of the project: the
decisions, the findings from real consoles, the traps. It is written for an AI coding agent that starts without any context (and for
humans). Read it before you change anything.

**Read first:**
- `README.md`: what the payload does, how to build, run and test.
- `patches/README.md`: how the patches to Transmission work and how to add one.
- **`docs/payload-safety-notes.md`: the safety model of PS5 payloads (there is no sandbox; a payload runs with effective uid 0 and can
  write kernel memory). Read it before you add any file, network, process or device access. The main goal of this project is to
  minimize the risk of harm to the console.**
- `THIRD_PARTY_NOTICES.md`: licenses. The whole project is GPL-3.0-or-later.

The maintainer (Alexander) tests on a real console (firmware 11.20) and works in Russian; users reported that it works on firmware 10.01 (2026-10-30: it starts and a torrent was added and is downloading), 11.60 and 13.60 (no detailed reports yet); the code, comments, commits and docs are English.

---

## 1. How to work here

1. **Discuss before you change.** For anything bigger than a small fix, say what you intend to do and wait for agreement. Answer a
   question first; if the answer implies a change, propose it instead of doing it.
2. **After a code change: build and check imports** (`./build.sh`; a few minutes, much less with a warm `.deps/`). A Markdown-only change (`*.md`, `CHANGELOG.md`) needs no build or tests; build when the code or the generated page changes, or when the maintainer asks. The PS5 loader crashes on an import it
   does not know; `scripts/check-imports.sh` (run by `build.sh`) must print "no extra imports" for `Transmission-*.elf` and
   `transmission_d.elf`. Never use a libc or SDK function that you have not run through the check (`isalnum` was caught once and replaced by explicit
   ranges). The check is the authority: if it passes, the loader will accept the file; if it reports an import, remove the call.
3. **Tests are slow** (about 30 minutes for all suites, about 12 more for the page tests) because they are real: real daemon, real
   timers. Run the tests that match your change (`scripts/test.sh <area>` / `pages <segment>`), and the full set once before a release.
   Do not run them after every edit unless the maintainer asks.
4. **Never claim something works on the console unless it was tried there.** Say exactly what was verified on the Linux host and what was not.
5. **Test builds between releases** are made with `PS5TM_DEBUG=N ./build.sh` (N counts the builds handed to the maintainer): the page and `shim.log` then say "<VERSION> debug N" (for example "0.4 debug 2") while the file stays `Transmission-<VERSION>.elf`, so two builds of one version can be told apart. Never in a release.
6. **Raise the version** (`VERSION`, `0.N`: raise the second number, to any larger one, there is no third number) after a change that goes to a console, **but only if the current version is already tagged (released)**: check with `git tag --list "$(cat VERSION)"`. If that tag does not exist yet, the version is still open: keep `VERSION` and add the change to its `CHANGELOG.md` section (a second section for the next number would skip an unreleased version). The first public release, `0.1`, was published on 2026-10-30; releases are made by a tag (`docs/releasing.md`).
7. **New functionality comes with tests.** A test must fail if the feature is broken; measure the real behaviour first, do not guess.
8. **Reports are short.** Do not list individual tests: "all tests passed", or a short statement of what failed and why.
9. **Never break the data of existing users** (settings, torrents, `/data/Transmission`): migrations are required.
10. Keep a change to Transmission itself small: put the logic into our own code (`src/daemon/`) and let the patch only add a call, with
   a check that fails the build if the anchor is not found.

**The SDK and the firmware.** The startup code of the SDK (`crt`) knows the firmware versions one by one: v0.40 added 13.00/13.20, v0.41 13.40, v0.42 13.60 (and 13.42 is in between), v0.45 also 14.00/14.10. A payload built with an SDK that does not know the firmware of the console does not even reach `main()` (on 13.60 with v0.41 nothing was created in `/data/Transmission` and no notification was shown). So when a new firmware appears, look for the SDK release first. ShadowMountPlus is always built with the newest SDK. The Diagnostics card shows the SDK version of the build; `scripts/deps.sh` keeps a stamp (`.deps/ps5-payload-sdk/.ps5tm-sdk-version`) so that a warm `.deps` is replaced when the pinned version changes.

## 2. Layout and build

```
build.sh                 deps -> build-elf -> check-imports; result build/Transmission-<VERSION>.elf
VERSION                  0.N; becomes -DSHIM_VERSION and the name of the ELF
scripts/                 deps.sh (SDK + libraries), build-elf.sh, check-imports.sh, allowed-imports.txt, test.sh
src/common/              code shared by daemon and manager (log, crash diagnostics, process, volume info, files); compiled twice, once per role;
                         fs_list.cc/.h is the shared core of the folder/file picker (path rules, folder listing, disk list), C++ and also compiled into both
src/daemon/              the wrapper around Transmission: guard (folder protection), settings enforcement, I/O watchdog, web files, log events;
                         fs_picker.cc is the read-only .torrent file picker behind the RPC method ps5_files
src/manager/             the manager: C (runtime, launch, procinfo, power, main) and C++ modules, page/ (the settings page and the shared picker)
src/icon-helper/         the process that registers the icon on the "Media" tab
src/asm/                 blobs that embed the daemon and the helper in the manager
src/ps5tm_cfg.h          the public interface between the manager modules
patches/transmission/    unified diffs for Transmission (PS5 and test build); patches/ps5/ PS5 only; patches/web-client/ web interface
locales/                 en.strings (the source), ru.strings; tools/ Node checkers for them
resources/               icons (PNG)
tests/                   see section 9
```

- `scripts/deps.sh` downloads the PS5 SDK (ps5-payload-sdk v0.45) and zlib, libevent, OpenSSL, curl into `.deps/`, every one pinned by
  version and SHA-256. Transmission 4.1.4 (`transmission-4.1.4.tar.xz`, SHA-256 `86e7aa02…5d22`) is downloaded by `build-elf.sh`.
  Nothing newer is ever picked up silently.
- `build-elf.sh` unpacks Transmission, applies `patches/ps5/` then `patches/transmission/` with `patch -p1` (the tree is unpacked again
  whenever the set of patches changes: a stamp file next to it), builds the Transmission library and the object `daemon/daemon.cc.o`
  (**not** the stock `transmission-daemon`: the functions `ps5tm_guard_*` live in our wrapper, so the daemon is linked by us), compiles
  our sources, links the daemon (`objcopy` renames `main` to `tr_daemon_main`; `--wrap` flags intercept `fork`, `execvp` and the I/O
  calls), links the icon helper, embeds both in the manager, and writes the ELF. The settings page is glued from `src/manager/page/` into
  `ps5tm_page.h` (the strings `@@STYLE@@` and `@@SCRIPT@@` are replaced).
- Generated files are in `.deps/build/generated` (`webfiles.h` with the web interface, `cacert.h`, icon arrays, `ps5tm_lang_data.h`,
  `ps5tm_page.h`, the object folders `obj-daemon` and `obj-manager`).
- Dynamic libraries (NEEDED): manager and daemon `libkernel_web.sprx libSceLibcInternal.sprx libSceNet.sprx`; the icon helper
  `libSceIpmi.sprx libSceAppInstUtil.sprx` plus those (**the order Ipmi before AppInstUtil is critical**). The import check also protects
  against a changed set.
- `sync` is an allowed import since 0.0.57 (checked on a console). Do not add others without the maintainer testing on a console.
- Linux builds without root (verified on Ubuntu 24.04; CI is Linux only). macOS should work (the scripts install the tools with Homebrew) but is **not verified and not in CI** (the maintainer's decision). Windows: WSL2 only.
- The ELF is about 17 MB since 0.4 (it was 12 MB: libcurl and OpenSSL are linked into the manager for the updater, besides the 10 MB daemon inside). Payloads of this size and larger run fine.

## 3. Architecture

**Processes.** The manager (`Transmission-<ver>.elf`, sent to the loader on port 9021) serves the **settings page** on port 9092 (the first
free port from `settings_port`), supervises the daemon, handles sleep and power, keeps the notification journal (the "bell"), and installs
the icon. The daemon is Transmission plus our wrapper; its web interface is on port 9091 (patched, section 5). The helper registers the
icon through `SceIpmi`/`SceAppInstUtil` and exits.

**Manager modules** (`src/manager/`): `config` (config.cfg, settings keys, migrations), `supervisor` (start, stop, restart, hang
detection, network-change restart), `rpc_client`/`rpc_json` (talks to the daemon over RPC), `journal` and `logs` (bell, ring log files),
`sleep` (rest mode, auto-sleep/power-off at the end of all downloads), `game_pause`, `syssvc` (the system-service thread), `guard_events`
(refusals of the daemon become bell entries), `web_files` (theme/icons/language files for the web interface), `monitor` (a once-a-second
loop that drives everything), `diag`, `updater` (release check, download, replace the file PLDMGR launched, restart); the UI side: `server`, `http`, `handlers` (the API), `folders`, `disks`, `language`, `icon`,
`platform`, `settings_keys`, `json_util`. Two internal headers: `core_internal.h` (namespace `ps5tm_core`) and `ui_internal.h`
(namespace `ps5tm_ui`); the public functions `ps5tm_*` and the globals `g_cfg`, `g_stats`, `g_language` are in the global scope
(`src/ps5tm_cfg.h`). Beware of global objects with constructors in different translation units (initialization order).

**Daemon wrapper** (`src/daemon/`): `guard.c` (the download folders protection: `ps5tm_guard_*`), `settings.c` (`guard_enforce_settings`),
`dns.c` (a time limit and a circuit breaker for `getaddrinfo`, linked with `--wrap`), `iowatch.c` (the I/O watchdog), `web_setup.c` (writes the web theme, icons, language and config files; the CA bundle), `log_event.c`,
`main.c`. Shared: `src/common/volume.c` (`ps5_volume_info`, `ps5tm_disk_kind_of_root`, `ps5tm_fs_needs_fix`, `ps5tm_resolve_alias`).

**What the daemon does besides being Transmission:**
- `fork` and `execvp` are intercepted at link time and always fail (the scripts of Transmission cannot run). Access to the web
  interface is limited to private networks (`rpc-whitelist`); there is deliberately no password.
- At **every** start `guard_enforce_settings()` rewrites `settings.json`: `preallocation` 0, `incomplete-dir-enabled` false, and
  `download-dir` either a folder from the list (if the user chose it as the default) or the placeholder `/dev/null/ps5tm-no-download-folder`
  (a path that cannot be created).
- The I/O watchdog wraps `open close pwrite pread ftruncate rename mkdir unlink fsync stat lstat statvfs` (the daemon already imported
  them), keeps a slot per thread (no `__thread`: not guaranteed under the loader), and a thread writes
  `io-watch: blocked <s> <op> off=<bytes> len=<bytes> path=<path>` to `daemon.log` at 3, 10, 60, 300, 900 s and then every 30 minutes, and
  `io-watch: done <s> ...` when an operation longer than 3 s ends. The manager turns them into bell entries ("The disk does not
  answer: write to X (offset N GB) for N s"; entries shorter than 10 s only in the log).

**Where things are on the console** (`/data/Transmission/`): `config.cfg` (the manager's settings), `daemon.desired`,
`paused_by_stop.txt`, `game_paused.txt`, `logs/` (shim.log, daemon.log, console.log, transmission.log, icon.log, notifications.log;
rotated by day), `icon/`, `locales/` (user language files),
`syssvc_probe.txt`/`syssvc_skip.txt`, `update_state.txt` (time and result of the last release check), `report.txt` (the report of "Diagnostics", written only when the console's browser asks for it), `sys/` (a leftover of the old helper `transmission_s.elf`: `transmission_s.elf` and `skip.txt` with a function name; nothing reads it any more, the current file is `syssvc_skip.txt`; it may be deleted by hand, the manager cannot remove a folder: no `rmdir` import), `transmission/` (daemon.port, `guard_messages.txt` (translated refusals for the daemon; in the root of `/data/Transmission` up to 0.3), web_theme, web_icons, web_diskbar, web_openfolder, web_lang.js,
usbfix_off.txt, `config/` = the settings and the state of Transmission, `web/`, tmp, cacert.pem). The icon is in `/user/app/TRNS00001/sce_sys/`
and is updated only after "Remove" and "Install" in the icon section. "Installed" = the folder exists **and** the marker `/data/Transmission/icon/registered` (the param.json that was registered); an installation of an older version has no marker and is adopted when `param.json` has our title id, the helper's last answer in `icon/result` is `ok` and `icon/attempt` does not exist.

**The system-service thread** (`src/manager/syssvc.cc`, inside the manager; there is no helper process any more): loads
`libSceIpmi`, `libSceUserService`, `libSceSystemService` through `dlopen` in that order (like Payload Manager) and takes functions with
`dlsym` by name: `sceLncUtilGetAppIdOfRunningBigApp`, `sceLncUtilGetAppTitleId`, `sceSystemServiceGetUIStatus`,
`sceSystemServiceParamGetInt`, `sceSystemServicePowerTick`, `sceShellCoreUtilResetAutoPowerDownTimer`, `sceSystemServiceRequestPowerOff`.
No new imports. It provides the game state, the console language (Russian is code 8), "keep awake" and the system sleep request.
Protection: a call that does not return in 30 s (`SysHangSeconds`, checked by the monitor) switches the service off until that call returns (the SIGCONT / gap rule that restarts the 30 s count after a wake-up is kept, but it never fires on this firmware; when the thread polls again the service is on again; the silence goes to the log at once but to the bell only if it lasts 90 s more after the service was switched off (`/tmp/ps5tm_sys_announce_delay` on the host), because the console's services are slow right after a wake-up (a call stayed blocked for 35 s); both changes are bell
entries); before loading and before the first call of every function its name is written to `syssvc_probe.txt`; if the previous run crashed
or hung there, the name goes to `syssvc_skip.txt` and the service does not start until that file is deleted. Loading `libSceSystemService`
from the main process used to hang; the order and the separate thread solved it. State: `stats.syssvc` in `/api/state` and the "system service" section of the report. On the Linux host `dlopen` is replaced by stubs driven by files (section 9).

## 4. The settings page and the manager API

- The page is `src/manager/page/` (`index.html`, `style.css`, `settings.js`, and `ps5-picker.js` / `ps5-picker.css`, the picker that is shared with the web interface) and is served on port 9092. Sections: Settings, Advanced
  settings, Diagnostics, Credits and licenses are "cards" that are collapsed at every open (the state is not remembered). Tints: settings
  greenish, advanced orange, diagnostics reddish. The notification panel (bell) is at the top right, the full-exit button top left.
- Animations: `slideShow` (height, padding, opacity in `PS5_ANIM_MS`, 200 ms; the first 2 s after load have none; `prefers-reduced-motion`
  turns them off), `popOpen`/`popClose` for the bell and the dialogs. jsdom tests set `PS5_ANIM_MS = 0` (no layout in jsdom); real
  Chrome tests check the animation.
- The page uses its own dialogs (`cfmModal`, `actModal`, `fdModal`, ...) and never `confirm()`/`alert()`.
- API (selection): `GET /api/state`, `GET/POST /api/config`, `GET/POST /api/folders` (`add`, `remove`, `unmount`), `POST /api/fs`
  (`list`, `mkdir`: the folder picker), `POST /api/daemon`, `POST /api/quit`, `GET/POST /api/update`, `GET/POST /api/report`, `GET /api/diag/*` (threads, probe, disks, mounts, logs, sleeplog; no button of the page uses them any more, they are kept for tests and manual use), `/api/sleep` (kept
  for tests and manual use).
- **Diagnostics** is ONE button, "Prepare report" (`src/manager/report.cc`, since 0.4; it replaced the rows Build / System service / Power state, the eight small buttons and the output pane). The card also holds three live rows moved from the main section (`#dgStatus`, built by `render()`): "Transmission" (state, pid, memory), "Manager is running" and "Last poll"; the main section shows its "Status" row (`#dmRow`) only while Transmission is NOT running (stopped, starting, not responding, ...), because those states must stay visible. The report is one text: the version, the console model (`hwmodel.cc`: `sceKernelGetHwModelName(char*)` from the SDK sample `hwinfo`, which lives in `libkernel_sys` that our ELF does not import, so it is loaded with `dlopen`/`dlsym` once in a thread of its own, with `hwmodel_probe.txt` / `hwmodel_skip.txt` like the system service; the serial number is deliberately not read; the report adds "PS5 Pro" / "PS5 Slim" / "PS5 (original)" only for names of the form `CFI-7xxx` / `CFI-2xxx` / `CFI-1xxx`; **verified on the maintainer's console (a PS5 Pro, firmware 11.20, 2026-11-11): the function returns `CFI-7000 B01`**; host hook `/tmp/ps5tm_sys_model`), the console firmware (`ps5tm_firmware()` = the SDK's own `kernel_get_fw_version()` from `crt1.o`, a read of the SDK version of the process parameter that the SDK start code already calls, so no new import; `0x11200000` = 11.20; **verified on the same console: 0x11200005, printed as 11.20 (the last byte is not part of the version)**; the host stub reads `/tmp/ps5tm_test_firmware`), the commit (`git describe --always --dirty --abbrev=12` passed by `build-elf.sh` as `PS5TM_COMMIT`; `unknown` outside a git checkout unless `PS5TM_COMMIT` is set in the environment; `-dirty` = the tree had changes), the SDK, the build time, the uptime, the system service and power state (JSON), the update state, `config.cfg`, the Transmission check, the disks (which of `/mnt/ext0..7`, `/mnt/usb0..7` are connected), the mount points (`getmntinfo` on the PS5, `/proc/self/mounts` on the host; USB and M.2 first; the sandboxes of system apps `/mnt/sandbox/...` only as a number), the threads, the processes and ports, the memory ring of the log, and the tails of `shim.log`, the sleep/daemon events, `transmission.log`, `console.log`, `daemon.log`. The address of the console (`ps5tm_ip()` and the current one) is replaced by `<console-ip>` (whole occurrences only); nothing else is hidden, so the file starts with a request to look it through (it has paths and torrent names) and the page says so. **The console's browser cannot save a file** (user agent has `PlayStation`): there the page does `POST /api/report {"action":"save"}` and the manager writes `/data/Transmission/report.txt` (atomic); any other browser downloads `GET /api/report` (`Content-Disposition: attachment`). Tests: `core/test_report.py`, the Diagnostics part of `daemon_card_test.js` and `settings_page_test.js`.
- **The colors of the pop-ups** (settings page and web client): blue is the default (information, the "?" help, the pickers), **red only for a warning, an error or a confirmation of an important operation**, green is the update window (red when it failed). Settings page: the class `red` or `green` on the `.modal` overlay (`cfmAsk` toggles `red` by its `danger` argument, `fdWarn` by `confirm`, `quitShow` by `closable`, `updView` by the state); web client (`app-additions.css`, the palettes define `--ps5-pop-blue*` / `--ps5-pop-red*`): the class `ps5-red` on the `.ps5-modal` overlay (`modal(title, paragraphs, buttons, warn)` in `ps5-compat.js`, `showError` in `ps5-files.js`). A new window must choose its color on purpose. Test: `ui_popup_colors_test.js` (page), checks in `ui_usbfix_test.js` and `ui_filepick_test.js` (web client).
- Settings of note: `download_folder=` lines (any number; the first is the default); `language` (`ru`, `en`, `file:<name>.strings`);
  `web_theme`, `web_icons` (color or mono), `web_diskbar` (`off|all|active`), `web_openfolder` (`off|bfpilot|webfm`); `autosleep_action`
  (`sleep|poweroff`), `sleep_method` (`native|direct`), `keep_awake`, `game_pause` (`off|screen|running`), `notify_game_pause`, `update_check` (yes by default: look for a new release by itself),
  `daemon_restart_after_seconds` (default 120), `daemon_stop_grace_seconds` (20), `daemon_network_restart_seconds` (15, 0 = off),
  `low_disk_*` thresholds ("almost full" < "low", enforced at input, not by an error on save).
- Language selection, first start: the language of the console decides (Russian if the system language code is 8, else English); if the
  system service did not answer, the page is English this time and the choice is **not** saved.

## 5. The web interface of Transmission (port 9091)

`patches/web-client/web_patch.pl` copies `web/public_html` from the archive and patches it; the JS/CSS it adds are in `js/` and `css/`.
**Every substitution in the minified code must match exactly once or the build fails**: that is how a move to a new Transmission version announces itself.

Limits of the console's browser (WebKit): no `@media (prefers-color-scheme)` (both palettes are added unconditionally, the light one
before the media blocks), no favicon in the title (not fixable), square native `<select>` (replaced by our own look), older
JavaScript (polyfills in `ps5-compat.js`, e.g. `toSorted`, `Object.hasOwn`), no `navigator.getGamepads()`, the cursor cannot be
moved or hidden from JS (section 8).

What we add (the main items):
- Themes (dark/light, rounded "cards", fields and menus), two icon sets (color and monochrome, CSS in `patches/web-client/icons/`),
  progress bars, favicon and apple-touch-icon.
- A **disk panel** (`ps5-panel.js`): free space per disk; mode `off`, `all` (disks of all torrents), `active` (disks of torrents that are
  not stopped); the kind of disk comes from `ps5_folders`.
- **Folder tiles** instead of the text field in "Add torrents", "Set location" and Preferences "Download to": two columns, path, kind,
  "free X of Y", a disk that is not connected is grey and disabled, an internal disk has a red "not recommended" or a FAT disk "(exFAT)".
  The hidden field keeps the value so the rest of the application works as before.
- **"USB disk fix"** row in "Add torrents": on for torrents on a disk that needs it, a "?" explains it, turning it off asks for confirmation
  with the cost. It is sent as `ps5_usb_fix` in `torrent-add`.
- **Open folder** (`web_openfolder`): a menu item and a toolbar button "Folder" that open `http://<host>:5905/#<path>` (BFpilot) or
  `:8888` (web file manager). A torrent with a single file inside a folder is detected through the `torrent-get` field `ps5_in_folder`
  (the stock client treats only `file_count > 1` as a folder). In the console's browser the page is opened in the same window,
  elsewhere in a new one.
- Error texts: the web client reads `error.data.errorString`, the daemon (JSON-RPC 2.0) sends `error_string`; `ps5-compat.js` wraps
  `fetch` and shows an `alert` for `session_set`, `torrent_set_location` and the like. Duplicate alerts ("Error adding ...") are dropped.
  On the console (`PlayStation` in the user agent, class `ps5-console` on `<html>`) the file chooser of "Add torrents" is hidden.
  Outside the console it is hidden too and replaced by our own control (`ps5-compat.js`, `#ps5-pick-row`): the browser draws the button
  "Browse…" and the line "No files selected." itself, in the language of the BROWSER, not of the page (a Russian page in an English browser
  stayed English, and the page cannot translate them). Our button opens the same native dialog, and our line shows the chosen file names in
  the page language; the input stays in the form (the dialog reads its files on submit) and is hidden visually (class `ps5-vh`, a transparent pixel-sized box), not with `display:none`, so that Safari and the iPad can open its dialog from a script. Tests: `ui_web_lang_test.js` checks our texts in Russian and English.
- **The .torrent file picker of the console** (`ps5-files.js`, plus the shared `ps5-picker.js` / `ps5-picker.css` that the settings page uses
  for download folders). The stock file chooser does not work in the PS5 browser, so the Add dialog gets a "Choose a .torrent file" button:
  the same picker lists the connected disks and folders, shows only `*.torrent` files and returns the chosen path. The file is added by
  `torrent-add` with `filename` (the daemon reads the .torrent itself; nothing is uploaded), and a failure is shown in our own window. The
  list comes from the read-only RPC method `ps5_files` (`{}` = the disks: `/data` and the connected `/mnt/extN`, `/mnt/usbN`; `{path}` =
  the subfolders and the `*.torrent` files); the manager is not involved. The listing and the path rules are the shared C++ code of
  `src/common/fs_list.cc`, so the folder picker and the file picker use the same logic (the folder picker keeps the config.cfg limits:
  300 characters and no " #"; a .torrent file may contain " #"). The button is in the Add dialog only on the console. Verified by the maintainer on a console (0.4): a file is chosen and added.
  `ps5_files` runs in the RPC thread of the daemon (the folder picker of the settings page runs in the manager): a USB disk that hangs inside `opendir`/`readdir` (the
  I/O watchdog does not wrap them) holds up the answers of the daemon until the call returns; accepted, because a daemon that has such a disk is usually already stuck on writes to it.
  A file or folder name that is not valid UTF-8 is left out of the listing (`valid_utf8` in `fs_list.cc`; it could not be chosen anyway, and the daemon's JSON parser might refuse the whole answer); the manager's folder picker shares this rule.
  The picker adds the torrent by its own `torrent_add` (path, folder, "start when added"); the stock 4.1.4 Add dialog has no priority field, and the USB disk fix is added by the `fetch` wrapper of `ps5-compat.js`.
- **The `.torrent` filter of the file picker** (`accept=".torrent,application/x-bittorrent"` of the add dialog): `web_patch.pl` clears
  `accept` on touch devices (`navigator.maxTouchPoints > 0`), because the iOS Files picker greys out `.torrent` files when the filter is
  set and iPadOS in desktop mode reports a "Macintosh" UA. The 4.1.4 release archive ships a **prebuilt** `web/public_html/transmission-app.js`
  that is older than its own `web/src` (it still has the 4.1.3 check), so our replacement is needed although the source already has the fix.
  **On 4.1.5 (or a newer archive): check whether the released `transmission-app.js` already has `maxTouchPoints`; if it does, remove the
  replacement from `web_patch.pl` and the iPad check from `tests/web/ui_console_test.js`.**
- Look and motion: our own dialogs and menus with enter/exit animations (`Element.prototype.remove` is deferred while the exit animation
  runs: dialogs 150 ms, menus 120 ms, the inspector 240 ms), the inspector slides in, tab content slides left/right, rounded tabs and priority
  buttons, a file icon and bold folders in the file tree, the torrent path as plain text on the right of the size line.
- Our icon (`images/apple-touch-icon.png`, the one of the payload and of the tab) instead of the stock Transmission logo in the dialogs (`.dialog-logo` is overridden in `css/app-additions.css`).
- Translation inside the client (section 6).

## 6. Translations

- The source text of the settings page is **English**. `locales/en.strings` holds the exact English fragments (anchors, the values),
  `locales/ru.strings` the translation (same keys). `lang_localize` replaces only **whole** fragments equal to an English anchor, so
  a fragment must not be a bare lowercase word (`running` would match the code `d.state==='running'`): write such texts capitalized.
  A page text may not contain an apostrophe inside a phrase (write `&#39;`).
- New text on the page: write it in English in the page, add `"slug" = "English text"` to `en.strings` and the translation with the
  same slug to `ru.strings`, then run `node tools/lang-tool.js check src/manager/page locales/en.strings locales/ru.strings`
  (`candidates` lists fragments that look like text but are not in the table).
- Message keys: `me_*` (English) and `mr_*` (Russian) are for notifications, bell entries and API errors, used as `ps5tm_tr("key", {args})`,
  placeholders `{1}`..`{9}`, `\n` is a newline. Lookup order: the chosen language (file, ru, en), then English, then Russian, then the key.
  The bell history is not translated (the text is translated when the entry is created).
- Texts of the **web client**: the key is the English text itself as written in the client (`"Open torrent…" = "Открыть торрент…"`), templates with
  `{1}`, plural forms separated by `|` (one|few|many for Russian). Not in the table = the English text stays. The client calls `ps5t(key, [values])`
  and `ps5n(...)`; the dictionary `window.PS5_I18N = {lang, plural, map}` is written by the manager to `web/ps5-lang.js` (a persistent copy in
  `transmission/web_lang.js`, copied by the daemon at start). Check: `node tools/web-i18n-check.js locales/ru.strings locales/en.strings .deps/build/generated/web_public_html`.
- Refusals of the daemon (`me_g_*`): the manager writes `transmission/guard_messages.txt` (English format with `%s`, a tab, the translation with `{1}`); the
  daemon looks the English text up and substitutes the first string argument by hand. The English text in the table must equal the
  `GUARD_MSG_*` macro **verbatim**. The daemon log always gets the English text.
- User languages: files `/data/Transmission/locales/*.strings` (Xcode format but **without** the trailing `;`, which is tolerated;
  comments `//` or `#` on their own line; the first entry is `"LanguageName"`). Rules: single line, no `<`, `>`, `\`; quotes become typographic;
  a missing key falls back to English; only placeholders present in the English text are allowed. Limits: key 1000 bytes, value 4000
  (above that the manager turns the whole translation off: "the embedded language tables are broken"). Error codes: `size utf8 syntax:N first
  name chars:N dup:key empty notfound read bad ph:key`. A file named `en.strings` does not replace the built-in English.
- Everything that cannot be localized (logs, `config.cfg` comments, diagnostics, internal errors, refusals in the daemon log) is English only.
- **Postponed by the maintainer:** embedding all `locales/*.strings` (a set of the main languages) in the ELF; it needs the two built-in
  languages (ru/en) generalized (identifiers, the plural rule of the web client as a field of the language file, the checks).

## 7. Download folders protection (the core safety feature)

**Idea:** a list of allowed paths (a rule, not an action). New torrents are added and moved only to folders from the list. The daemon and
the manager create nothing and write nothing to a disk when a folder is added (Transmission creates folders with parents itself on the first write).
A folder has no name, only a path. The path of a torrent chosen at add time is **never changed** by us. With an empty list nothing is added
(`/data/torrents` is never substituted).

- **List:** `download_folder=/path` lines in `config.cfg`. Rules: absolute; normalized (`//`, `.`, trailing slash); no `..`, no control
  characters or "space#"; up to 300 characters; allowed `/mnt/extN/...`, `/mnt/usbN/...`, `/data/...` (not `/data` itself and not
  `/data/Transmission`); duplicates (ignoring case) refused; an external disk only if it is **connected**; `/data` (the internal disk) and
  FAT disks only after a warning and confirmation. The UI limit is 8 folders. Migration: at the first start with the feature, the
  `download-dir` of `settings.json` becomes the first folder once.
- **"Connected" criterion:** `st_dev` of the disk root differs from the `st_dev` of `/mnt` **and** the capacity (`statvfs`) is greater than 0.
  The folders `/mnt/usb0..7` and `/mnt/ext0` always exist, and a write there succeeds even without a disk (it lands on the system
  `/mnt`, `dev=2264989442`, capacity 0). Connected disks have their own `dev` and size.
- **Daemon guard** (`src/daemon/guard.c`, calls added by `patches/transmission/0002-0003`): `torrent-add` (a path only from the list; without
  a path the default folder = the one the user chose (`session-set download-dir`, kept in `transmission/default_folder.txt` since 0.0.74; the path held by the running session is deliberately NOT used: it can be stale, and adding that folder again used to make it the default) if it is in the list, else the first folder; the manager deletes the file when that folder is removed from the list, the daemon adopts a choice from `settings.json` at start; an empty list or a disk that
  is not connected is a refusal), `torrent-set-location` (the destination from the list; the torrent's own disk connected),
  `session-set/get` (`download-dir` only from the list and shown as the default folder; `incomplete-dir` and enabling
  `incomplete-dir-enabled` refused). **Start and Verify** (`tr_torrent::start`, the common place for buttons, queue and
  restore at daemon start; and `tr_torrentVerify`): a torrent on a disk that is not connected stays stopped with a local error, Verify is refused
  (the progress is not reset: without the disk it would find "no files" and then re-download to the wrong place), there is no auto-resume
  (the user presses Start after the disk is back). A torrent that is already running when the disk is pulled is not stopped (writes fail in
  Transmission). Every refusal is `guard: rejected: ...` in `daemon.log`; the manager reads new lines once a second and writes "Rejected: ..."
  to the bell (deduplicated once a minute per text, at most 5 per second). The list is read from `config.cfg` on **every** check; strange hand-written lines are ignored (the safe side).
- A torrent without saved state loads paused with the placeholder download dir and will not start (`Set location` into a list folder fixes it).
- `ps5_folders` (RPC method, `0009`, `0010`): `result.folders = [{path, kind (ssd|usb|int), mounted, total, free, fs, fsbad}]` in list order.
  `torrent-get` field `ps5_in_folder` (`0011`).

**Disk kinds and file systems (what real consoles showed, firmware 11.20).** Kind is decided by device and file system, not by the
mount name: `/dev/ssd*` is M.2, `/dev/es*.crypt` and `/dev/da*` are USB.
- Internal extended M.2: `/mnt/ext1`, `bfs`, `/dev/ssd1.user`. With BindMount (another project of the maintainer) it is also visible as
  `/data/ext1` and `/user/data/ext1` (`nullfs` over `/mnt/ext1`). **Never hard-code such paths**: `ps5tm_resolve_alias` asks the system
  (`statfs` of the deepest existing parent; if `nullfs`, the "from" name replaces the "on" name, up to 4 times) and substitutes only
  when the result is `/mnt/extN` or `/mnt/usbN`. `/data` is itself `nullfs` over `/user/data`, so substituting everything would turn `/data/torrents` into `/user/data/torrents`.
- A USB disk formatted by the console as **extended storage**: `/mnt/ext0`, `ufs`, `/dev/es0.crypt` (an encrypted layer over USB). UFS
  has sparse files: no problem for torrents.
- The same disk formatted as exFAT: `/mnt/usb0`, `exfatfs`, `/dev/da1p1`. **exFAT and FAT32 (`msdosfs`) have no sparse files**; a write
  far from the start fills the gap with zeroes and everything hangs. So the "USB disk fix" depends on the file system
  (`ps5tm_guard_prefer_sequential`: `exfatfs`, `msdosfs`), not on `/mnt/usbN`.
- Host test hooks: `PS5TM_TEST_MOUNTINFO="/mnt/usb2=exfatfs=/dev/da1p1;/mnt/ext0=ufs=/dev/es0.crypt;..."` and `PS5TM_TEST_ALIAS="/data/ext1=/mnt/ext1;..."`;
  without them the kind follows the mount name.

**The USB/exFAT hang: causes and fixes (found by experiments, host swarm tests in `tests/suites/network/`).**
1. `preallocation` is forced to 0 at every start (the default "fast/sparse" stretches a file to full size on the first write).
2. Pieces at the start and the end of files got a priority boost in Transmission, so the last piece of **every** file was requested at once and all
   files were stretched to full size (35 GB: 9 minutes, the console hung). The boost is removed (`0005`); "last piece first" is removed in sequential mode.
3. Sequential mode ("USB disk fix") originally only guaranteed the worst case. The candidate comparison in `peer-mgr-wishlist.cc` was
   "fewer unsent blocks first, then priority, then rarest, then salt", so in a real swarm the rare far pieces won. Now in this mode a piece is chosen only by file priority and piece number (`0006`).
4. If the connected peers have only the far part of the torrent, the nearest *available* piece is far away. A **window** of 64 MiB from the first
   missing block (at least two pieces) bounds it (`0007`); if nobody has a near piece, the download waits instead of hanging.
5. The torrent state (which holds its folder) was saved only every 360 s and at shutdown, so a daemon killed early returned the torrent
   to the default folder and started it. Now `save_resume_file()` runs right after adding and the interval is 20 s (`0005`).
6. The "USB disk fix" is chosen **per torrent** (`ps5_usb_fix` in `torrent-add`; stored by hash in `usbfix_off.txt`, a list of torrents where it is OFF; applied at every
   start, since the flag in resume is not reliable; `torrent-set-location` onto such a disk turns it on unless it was turned off) (`0008`).
   Not reproducible on the host: the hang of a second torrent made of 1500 small files added to a big running one (cause not established).
- The daemon hang handling: `daemon_restart_after_seconds` 120 (the maintainer sets 660 himself), a soft stop waits `daemon_stop_grace_seconds`;
  if the process does not die even after SIGKILL (a thread inside a disk system call, can last minutes), no second instance is started and
  it is retried after 60 s (`kill_failed`); the page says "busy, not answering N s" and "forced restart in M s", and for a stuck process advises to unplug the USB disk.
  The bell records hangs of 10 s and longer with the duration (one entry per episode).
- **DNS must never hold up the daemon (0.2).** A home network that lets only some DNS servers through made every lookup of a name last 30 s: the DHT bootstrap lookup (in the session thread, at the start) stopped the daemon for 30 s before it listened on 9091, and the "stopped" announces at shutdown waited for the trackers' names (27-30 s). Now `patches/transmission/0012` looks the DHT name up in a thread, and `src/daemon/dns.c` (`--wrap=getaddrinfo`, so libtransmission and the static libcurl of the PS5 build are covered) runs every lookup of a name in a helper thread with a 4 s limit; two timeouts in a row open a breaker (30 s of immediate `EAI_AGAIN`, then one probe). Numeric addresses and `AI_NUMERICHOST` never wait. Tested on the host (`test_dns_does_not_block.py`: a silent DNS server on 127.0.0.1:53, `/etc/resolv.conf` replaced and restored): start in 1 s; the wrapper itself by a small C program (`tests/unit/dns_limit_test.c`). **Not covered on the host: the libcurl of the test machine is a shared library, so the tracker lookups and the shutdown were not measured with a dead DNS; on the console they go through the wrapper.** A DNS that is merely slow (more than 4 s) now fails, by design.
- **Network change restart** (`daemon_network_restart_seconds`): events are the network going away and back, a changed IP, waking from rest mode. After an
  event, every 2 s the manager checks whether the daemon accepts connections **on the current IP** (connect up to 1.5 s, then `GET /transmission/rpc`; any HTTP answer will do); if it does not
  answer for N s it restarts softly (once per event, not while the daemon waits for a disk, the check ends after N + 180 s). Test hook: `/tmp/ps5tm_test_ip`.

**Unmount.** "Unmount disk" on a connected USB folder (window `fdUmModal`): the plan unmounts nullfs links first, then **all partitions of the same physical
disk**, stops the torrents on them through RPC (up to 15 s + 2 s), unmounts without force, three tries with a 1 s pause on EBUSY. On EBUSY the "Disk busy"
window lists who holds the disk (`ps5tm_disk_holders`: PS5 `sysctl kern.proc.filedesc` per process; host `/proc`; kinds `ours`, `system` (name `Sce*`), `app`) and
offers "Eject anyway" (`sync` then `unmount(MNT_FORCE)`). **Extended storage (`/dev/es*`) and the system eject (since 0.0.72).** An ordinary `unmount(2)` of extended storage from our process leaves the console showing the disk connected with a size of 2.10 MB and complaining about an improper
removal. So the unmount asks the system to disconnect the drive, like "Safely remove from PS5" (`src/manager/usb_eject.cc`, see the ShadowMountPlus note below); `folder_unmount` (`folders.cc`) does: stop the torrents, unmount our nullfs aliases (BindMount links), **system eject** of the mount
point, wait up to 5 s until the mount is gone, then unmount whatever partition of the same disk is left. For ordinary USB drives, if the system cannot be asked or says "not found", the old unmount of all partitions follows (and "Eject anyway" = sync + `MNT_FORCE` stays for them). For extended storage there is no fallback and no force: whatever the failure, one message (`mr_um_ext_fail`) asks the user to eject it with the console (Settings → Storage, "Safely remove from PS5"); the details (errno, native status) are only in `shim.log`.
**Verified on firmware 11.20 (2026-10-30):** for an exFAT drive the system eject works (`usb eject: /mnt/usb0: status 0 ... the system lists 1 device(s)`), so the method ids from ShadowMountPlus are right on 11.20 too. For extended storage (`/mnt/ext0`, `/dev/es0.crypt`) the system lists 0 devices (`status 2`, ENOENT; ShadowMount+ fails the same way), so it is handled by another service that we do not know. Unknown: how the system disconnects it. The system eject runs in a worker thread, the caller waits 25 s; a marker file `usb_eject_probe.txt` is written before and removed after; if it is found at the next start, or the thread does not return, the system eject is off (`usb_eject_skip.txt`, delete it to try again).
**Written without a console test, by the maintainer's decision (ShadowMountPlus ships the same calls to many users); the IDs are verified by them on 9.40 and, for ordinary USB, by the maintainer on 11.20.** Host test hook: `/tmp/ps5tm_test_eject` with `ok` (detach the mount), `busy`, `gone` (ENOENT) or `fail` (EIO); absent = not available.
Mounts under `/mnt/sandbox/` are never touched.
Observations (three snapshots of the mount table with extended storage on USB): wrong state after our unmount: no `/mnt/ext0`, 1175 mounts, 1156 sandboxes; after the
proper removal and reconnect 1059/1040; after the proper removal 1056/1038. The proper removal drops `ext0` and only 2 sandbox mounts; the ~116 extra
sandboxes in the wrong state are unexplained (the report counts them but does not list them). The console also asks to close running games before a proper removal
(a running game holds the storage inside its sandbox). A proposal, not done: show the sandboxes grouped ("N × type source") and all `ext`/`usb`/`es*` lines separately.
**How ShadowMountPlus does a safe eject (found 2026-10-30, `drakmor/ShadowMountPlus`, `src/sm_usb.c`, GPL-3.0; release note "safe USB drive ejection from the web interface").**
It does **not** call `unmount(2)`. It asks the system's own automounter in ShellCore over IPMI, which is what "Safely remove from PS5" is: a client of the service `SceAutoMounterIpc`
made with the C++ symbols of `libSceIpmi` (`IPMI::Client::Config::Config()`, `estimateClientMemorySize`, `Client::create`, `ClientImpl::connect`, `invokeSyncMethod`, `terminateConnection`, `destroy`; the config is
a 0x200-byte buffer with the service name at offset 0, flag 1 at 0x1c, request size 0x200 at 0x28, response size 0xf800 at 0x30). Methods (verified by them on firmware 9.40 in ShellCore's dispatch table and `libSceUsbStorage`):
`0x30015` list devices (input: u64 capacity 8; outputs: up to 8 u32 device ids and an i32 count), `0x30013` mount point of a device (input: id and buffer size; output: a 128-byte path), `0x30025` eject (input: the device id; no output).
Flow: find the device whose mount point equals the path, re-check that the mount is still the same volume (`f_mntfromname`, `f_fsid`), eject **once** (never retry), then poll up to 5 s until the mount is gone. Their `sm_usb_mount_path` accepts `/mnt/usb0..7` **and `/mnt/ext0`**,
so this path covers the extended storage that our unmount broke. Before the call they refuse when a game is active, when another mount of the same physical disk exists (partitions, image mounts: EBUSY) and while a scan is running. Errors: the native status `0x8002xxxx` carries an errno in the low 16 bits.
Caveats for us: the ids are verified only on 9.40 (not on 11.20: probe first with the two read-only list calls); the IPC can hang (run it in a thread with a watchdog and a skip file like the system-service thread); our manager already loads `libSceIpmi` with `dlopen`, so the symbols can be taken
with `dlsym` and **no new static import** is needed (the import check stays green). Their code is GPL-3.0: write our own implementation from the protocol and credit them in `THIRD_PARTY_NOTICES.md`. Open question 19 is therefore probably answered; the eject is implemented from it (0.0.72), without a separate probe (decision of the maintainer).
Not verified on a console: whether the system allows unmounting from our process for ordinary USB, whether `kern.proc.filedesc` works on every firmware.

## 8. Sleep, power, game pause, firmware notes, input

- **Rest mode** (`enter_rest_mode_impl`): by default the system call `sceSystemServiceRequestPowerOff` (`sleep_method=native`, the same as the menu, with the
  animation); if the service does not answer, the direct way (like ps5-linux-loader: open the event flag `SceSystemStateMgrStatus`,
  `sceKernelNotifySystemSuspendStart`, `sceKernelSetEventFlag(0x400)`). PS5-Power-Payloads-Project uses `sceSystemStateMgrEnterStandby()` from
  `libSceSystemService`; that was not verified on 11.20. After sleep the manager checks the service and restarts it if needed.
- **Power off:** `ps5tm_power_off_syscall()` = `syscall(55, 0x4008)` = `reboot(RB_HALT | RB_POWEROFF)`, like the Power Payloads project. No `sync` (FreeBSD `reboot` without
  `RB_NOSYNC` syncs itself; not verified on the PS5 kernel). A successful call does not return; if it returns, the daemon is started again after 10 s. Before it Transmission is stopped cleanly:
  active torrents are paused and remembered and resume at the next manager start. An unused official path exists: `sceSystemStateMgrTurnOff` (parameters unknown).
- **At the end of all downloads** (`autosleep_action`): "Activate" (the warnings about sleep, firmware, kernel panic and about kstuff, or about HALT/POWEROFF,
  are shown in the `actModal` window; the sleep warning has a "do not show until the next version" box = `sleep_warnings_hidden`). Waking up from sleep (SIGCONT or a time jump
  larger than `ResumeGapSeconds`) deactivates auto-sleep with a bell entry (otherwise the accumulated time would trigger at once).
- **Keep awake** (`keep_awake`, off by default; only after "Activate"): `PowerTick` and `ResetAutoPowerDownTimer` right after activation and then every 10 minutes (the console's
  shortest timer is 20 minutes). Verified by the maintainer: the console did not sleep for 25 minutes and the gamepad stayed on.
- **Full exit** (`#quitBtn` → `POST /api/quit` → `worker_quit`): wait for a free daemon, stop it gracefully (pause and remember torrents), notification, `ps5tm_exit_now()`; if the daemon does not stop, the manager stays.
- **Rest mode does NOT freeze the payload (found in console logs, 2026-10-31).** The manager kept running in rest mode (its `HTTP self-test` ran twice with the network down, the 10-minute maintenance came on schedule) and the daemon used 618 s of CPU in a 10-minute rest: no SIGSTOP, no time gap, so the SIGCONT / gap detection (`ResumeGapSeconds`) **never fires on this firmware**; the only trace of a wake-up is the network going away and coming back, and the power state below. Do not build anything on "the process was frozen" (a heartbeat thread for that was tried and thrown away). What does happen at the wake-up: the system calls of the system-service thread block for about 35 s (`sceLncUtilGetAppIdOfRunningBigApp` while the interface of the console comes back), `/data` may answer EIO for a few seconds (log lines written then are lost from `shim.log`, they are still in the memory ring: the "Manager log" button), and a daemon that was left running spins on the CPU while the network is down (threads 17 -> 4, connection refused, SIGTERM not enough): that is why 0.3 stops it before the sleep.
- **The power state of the console (0.3, `powerstate.cc`, `sleep.cc`).** Rest mode does not stop the payload, so the manager reads the system's event flags (found in ShadowMountPlus, `sm_shellcore_flags.c`; opened by name with `sceKernelOpenEventFlag`, polled without waiting with `sceKernelPollEventFlag`, NID `9lvj5DjHZiA`, all resolved by NID like the rest, no new imports) every 300 ms in a thread of its own. `SceSystemStateMgrInfo`: the low 16 bits are the state (10 starting, 100 shutdown on going, 200 power saving, 300 suspend on going, 500 main on standby, 1000 working), bits 32-47 the trigger code, 48-63 the reboot cause; `SceSystemStateMgrStatus` bit `0x200000` is "the shell UI is shutting down". 100, 300, 500 and that bit are "going to sleep": `sleep_power_enter` sets the same `hold` as our own sleep and stops the daemon cleanly (`sleep_stop_daemon`: torrents paused and remembered, the desired state stays); `working` after that (`sleep_power_leave`, two polls in a row) releases the hold, the supervisor starts the daemon after `wake_delay_seconds` and the remembered torrents resume; it also switches an armed auto-sleep off, like the old SIGCONT rule. While the power monitor is alive the hold ends only by `working` (the network rule stays as the fallback when the monitor is silent for 2 minutes). Crash protection like the system service: `power_probe.txt` / `power_skip.txt` in `/data/Transmission`. The state and the raw patterns are in `shim.log` (`power:` lines; the Status flag up to 60 changes, the rest of the Info pattern up to 60) and in the "power state" section of the report. Host test hook: `/tmp/ps5tm_power_state` (the number, default 1000) and `/tmp/ps5tm_power_status` (hex); `PS5TM_NO_POWER=1` switches the thread off.
  **Seen on a console (11.20, beta 1, 2026-10-31):** from the menu to rest mode: `Status` 0x04080004 (idle: bit 0x04000000 VSH_AUTO_UPDATE_VERIFY_DONE, 0x80000 EXTEND_SHUTDOWN_TIMER, 0x4 BD_DRIVE_READY) -> 0x040c0004 (+0x40000 START_STANDBY_TIMER) -> 0.3 s later 0x04200004 (**0x200000 SHELLUI_SHUTDOWN_IN_PROGRESS**: our trigger, the daemon was stopped in 4 s) -> about 5 s later `Status` 0x04000000 and `Info` standby (500, 0x00000001000001f4) -> after 42 s of rest `Info` working (1000, 0x00000003000003e8: the trigger code in bits 32-47 is 3 after a wake-up, 1 otherwise) -> the daemon came back after `wake_delay_seconds` and resumed 5 torrents. A **reboot** and a **power off** looked the same in the end: `Status` 0x04200004, then 5 s later `Info` shutdown (100, 0x0000000100000064), and the process was gone. The shell UI bit comes first for all three and does not tell them apart; a timer bit of `Status` about 0.3 s before it does: **0x40000 START_STANDBY_TIMER = sleep, 0x10000 START_SHUTDOWN_TIMER = power off, none (0x20000 START_REBOOT_TIMER was not seen, maybe too short for the 300 ms poll) = reboot**. Not used yet, the message says "before sleep or shutdown". The bit and the state change a few milliseconds apart, so a wake-up needs two polls in a row. Unknown: what 200 (power saving) is, whether the flags are the same on 10.01 and 13.60.
- **Game pause** (`game_pause`: off, screen, running): when a game is on the screen/running the active torrents are paused after 5 s (`GamePauseDelaySeconds`) and
  only those are resumed (`game_paused.txt`), 30 s after the game is closed or minimized (`GameResumeDelaySeconds`; so that visits to settings do not make them jump). Torrents the user
  stopped are never touched; torrents started during a game are not touched. If the state is unavailable for a minute, they are resumed (fail-safe). The page says "paused N (a game is running)".
- **Firmware 11.20, `sceSystemServiceGetUIStatus`** (first 4 bytes as one little-endian number): home screen `0x20010`; game on the screen `0x20036`; the system
  menu over a game `0x20036`; controller settings opened from a game `0x20010` (back in the game `0x20036`); just before sleep (yellow breathing) `0x20000`; after wake and
  after reboot `0x20010`. **Other firmwares were not checked.** Rule in the code: ONE signal, the number compared with `0x20010`; any other number while a game is running means "on the screen".
  Past mistakes: reading the number one byte at a time (`0x10`/`0x36`), a wrong "home" fallback, and the "focused" flag (`sceLncUtilGetAppFocusedAppStatus`) that once stuck on the home screen.
  The auto power-down remaining-time call returns nothing usable (it writes nothing). A minimized game is **not** suspended (`sceLncUtilIsAppSuspended` returns 0).
- **Input in the console's browser:** Square arrives as a `keydown` `F2` (keyCode 113, `code: "Unidentified"`); the D-pad as ordinary `ArrowUp/Down/Left/Right`;
  there is no `navigator.getGamepads()`; reading the pad from a background payload with `libScePad` fails (`scePadOpen` returns `0x80920005` / `0x809b0081`).
  The native cursor cannot be hidden, moved or made to follow focus from JS (`cursor: none` is ignored, synthetic `mousemove` does not move it, spatial navigation does not move it).

### The context menu on Square: DEFERRED (2026-10-30)

Goal: open the stock Transmission context menu (`.context-menu`) of a torrent with Square. **It did not work well enough and was postponed; none of the
attempts is in the repository.** The menu is Transmission's own; it opens on a synthetic `contextmenu` on the torrent row (checked with a "Test menu" button)
and is placed by the app's `pointer_device`, which is updated only by a **real** `mousemove`: with a synthetic `mousemove` it opens at the row, without it at the cursor.
There is no keyboard navigation of that menu in the app; arrow keys are handled in `_keyDown` only when no popup is open, yet Transmission kept selecting torrents.

| Try | What | Result |
|---|---|---|
| 0.0.71 | A probe overlay `?inputprobe=1` plus `/api/pad` | Square = F2; the pad is not reachable from a background payload |
| 0.0.72 | F2 → synthetic right click | The menu opens; position and control are raw |
| 0.0.73 | Menu at the selected row, highlight, arrows, Cross intercepted | Works, but submenus did not open |
| 0.0.74 | Cross and Right/Left for submenus, click outside | Functionally better; not confirmed on the console |
| 0.0.75 | Menu at the cursor | Cursor and highlight diverge |
| 0.0.76 | Hide the cursor with CSS | Does not work: the browser draws the cursor |
| 0.0.77 | Menu items focusable, arrows left to the browser | The cursor did not follow |
| 0.0.78 | Also stop Transmission's arrows while the menu is open | "Nothing changed" |
| 0.0.79 | Stop ArrowUp/Down globally in the `window` capture phase (`stopImmediatePropagation`, no `preventDefault`) | Not verified |
| 0.0.80 | "Right click under the cursor": Square sends `contextmenu` to the torrent under the last real mouse position, no synthetic `mousemove`, items used like with a mouse, arrows suppressed while open | Maintainer: "still works badly" |

If you come back to it: first measure **how Cross arrives** (an Enter key or a native click under the cursor) and where exactly the app takes the menu position. Options:
(A) the 0.0.80 mouse variant; (B) your own navigation with your own highlight (as 0.0.74) and Cross handled, ignoring the cursor; (C) a "⋯" button on the toolbar (like the existing "Folder" button) that opens the same menu from the button
and is clicked with the cursor, which does not depend on the Square key at all. Also consider an own action dialog (Start, Pause, Verify, Remove, Set location, Open folder) with big buttons.
A side effect of the global arrow suppression is that the arrows stop selecting torrents on the console.

## 9. Tests

The tests start the real daemon and manager on **Linux x86_64 with root** and emulate disks with `tmpfs`; they write to `/data`, `/mnt`, `/user`. Use a VM made for this.

- Environment: `sudo tests/setup-env.sh [--install-chrome]` (folders; the address `192.0.2.2/32` on `lo`, **not persistent**: repeat after a reboot; `npm ci` in `tests/web`; Chromium
  via Playwright into `/opt/pw-browsers`, or set `CHROME_BIN`). `tests/check-env.sh` checks it. Needs Node 20+ and Python 3.
- The swarm tests (`tests/suites/network/`): Transmission rejects loopback peers, so the test tracker hands out the peer `192.0.2.2`, which must be on `lo`. Without it
  `test_piece_order` and the other network suites fail with "it downloads completely → []"; that is the environment, not a regression.
- Build for the host (needs the packages `libc++-18-dev libc++abi-18-dev libssl-dev libcurl4-openssl-dev zlib1g-dev` besides clang-18 and cmake; the CI installs them): `./build.sh` first (the tests use the generated web files and icons), then `tests/build/build-test-binaries.sh` (the test tree: only `patches/transmission/` are applied; the build folder is cleaned when
  the set of patches changes; output `transmission_d_host` and `Transmission_mgr_host`; stubs in `tests/build/stubs/`). Repeat it after any change in `src/`.
- Run: `scripts/test.sh list|smoke|full|<area>|suite <file>|pages [segment...]`. Areas: `core`, `sleep`, `webconfig`, `folders`, `network`, `syssvc`, `language`. `smoke` = daemon_lifecycle, config_load,
  disk_panel_modes, download_folders_guard, folders_rpc, system_service_and_game_pause, page_language. Page segments: `settings`, `daemon-card`, `sleep`, `webui`, `effects`, `language`, `webui-language`.
  Results: `/tmp/ps5tm-test-suites.txt`, `/tmp/ps5tm-test-timing.txt`, `/tmp/ps5tm-test-pages.txt`. Ports 9091 and 9092 must be free. Before a run the runner kills `Transmission.el`/`transmission_d.` and resets `/user/app`.
- Screenshots of the README (`docs/images/<lang>-*.png`) are made by `tests/screenshots/make_screenshots.py <en|ru> docs/images` (same environment as the tests; a stand with big tmpfs disks, sparse-file torrents and one throttled real download; Chrome takes the pictures; the IP of the machine is replaced). Regenerate them when the look of a page changes.
- Layout: `tests/lib/` (`harness.py` the stand: fresh state, the loader emulator on 127.0.0.1:9021, start of the manager; `web_stack.py` for the web client; `swarm.py` a mini seeder and tracker;
  `sleep_helpers.py`, `delayed_client.py`), `tests/suites/<area>/test_*.py`, `tests/web/*.js` (jsdom tests against the real manager, Chrome tests against the real daemon files, `chrome.js`), `tests/run_pages.py`,
  `tests/experiments/` (reproductions, not part of a run).
- Since 0.0.31 an add without a folder is rejected, so the harness writes `download_folder=/data/torrents` into `config.cfg` (flag `AUTO_FOLDER`; the suites about an empty list switch it off). `web_stack.setup()` mounts `tmpfs`
  on `/mnt/ext1` and `/mnt/usb0`. A suite that writes `config.cfg` whole must include that line.
- Host test hooks (environment variables or files): `PS5TM_TEST_MSG=mixed` (`me_` English, `mr_` Russian, web dictionary off, so old text checks stay valid; the language suites run without it), `PS5TM_TEST_LANG`,
  `PS5TM_TEST_POWEROFF` (a mark file `/tmp/ps5tm_test_poweroff` instead of the real call), `PS5TM_NO_SYS_HELPER=1` (no system-service thread), `PS5TM_NO_UPDATE=1` (no automatic release check; the harness sets it unless `PS5TM_UPDATER_API` is set), `PS5TM_HOST_TEST` (`/tmp/ps5tm_test_io_delay`: "<ms> <path part>" delays one write),
  `PS5TM_TEST_MOUNTINFO`, `PS5TM_TEST_ALIAS`, `/tmp/ps5tm_test_ip`, `/tmp/ps5tm_test_eject` (the system eject stub, see section 7). System-service stubs on Linux: `/tmp/ps5tm_sys_app` (a game is running, holds `PPSA#####`), `/tmp/ps5tm_sys_suspended`, `/tmp/ps5tm_sys_ui_value`,
  `/tmp/ps5tm_sys_lang`, `/tmp/ps5tm_sys_mode` (the word `hang`), and the calls are logged to `/tmp/ps5tm_sys_calls.log`.
- Traps: in JavaScript regular expressions `\b` does not work for Cyrillic; a window is 762 px wide because of the frame; the card test of the daemon must start from a clean state; a helper that stops and starts the
  daemon must wait for "not busy" and retry (otherwise "the manager is busy" leaves it stopped: a race in the test); `pkill -f <name>` kills the shell that has the same name in its command line, use `pkill -x` with the name cut to 15 characters;
  in tests of a bell entry, check "new entry" in the first 100 ms (the list is refreshed by a timer).
- **Timing** (full run of 0.0.60, 2026-10-23): suites about 30 minutes (slowest: system service and game pause 267 s, network change restart 180 s, autosleep about 106 s, sleep actions and the no-restart test about 100 s), pages about 12 minutes (settings 110 s,
  daemon card 55 s, sleep 48 s, webui 276 s, effects 91 s, language 28 s, webui-language 77 s). The full run on 0.0.70 (34 suites) passed. A time accelerator for the app timers was proposed and **declined**; parallel suites were not chosen.
- Patches the tests do not need: the old libevent poll patch was removed after a full run without it passed.
- **The picker**: `webconfig/test_torrent_files_rpc.py` checks the new `ps5_files` method (the disk list, the `.torrent`-only listing, the hidden/service names, the 500-entry cut, the refused paths) and adding a torrent by the path of a `.torrent` file; `tests/web/ui_filepick_test.js` (Chrome, UA "PlayStation") checks the console button, the picker, the `filename` request and the error window. The settings page's folder picker is still covered by `tests/web/ui_picker_test.js`; its ids and classes are kept by the shared picker.

## 10. Open questions to testers (firmware and disks)

Please send the report ("Diagnostics" → "Prepare report"; it has the mount table, the disks and the logs) with answers:
1. The mount and device of an internal extended M.2 (here `/mnt/ext1`, `bfs`, `/dev/ssd1.user`); always so (other numbers, `/dev/ssd2.user`)?
2. An external USB disk formatted by the console as extended storage (here `/mnt/ext0`, `ufs`, `/dev/es0.crypt`); always `/dev/esN.crypt` and `ufs`? Which N for a second disk?
3. A USB disk formatted on a computer (exFAT here: `/mnt/usb0`, `exfatfs`, `/dev/da1p1`); how FAT32 (`msdosfs`?), NTFS (supported at all?) and a multi-partition disk look.
4. Can extended storage on USB be mounted as `/mnt/usbN`, or M.2 as `/mnt/extN`; does the mount name always match the device kind?
5. Two USB disks at once: which mounts and devices; the same number after reconnecting?
6. BindMount or other nullfs links to disks (`/data/ext1`, `/data/usbN`): how they look in the table.
7. Firmware and model (Phat, Slim, Pro); does the behavior, including the game-on-screen values `0x20010`/`0x20036`, match other firmwares?
8. Does a big torrent on a USB disk hang Transmission: exFAT (without the fix), FAT32, UFS (extended storage)? Does the fix help on exFAT and FAT32?
9. Unmount: does it work on USB (exFAT) and on extended storage; what does the bell say (EBUSY, EPERM); does closing file managers (BFpilot, FTP) help?
10. What does the system's safe removal do to the mount table (all partitions and `es*.crypt`)?
11. How many nullfs links and sandboxes (`/mnt/sandbox/...`) are in the table (1042-1177 seen); does their number slow the "Mounts" button?
12. Does the console treat the disk as internal after the formatting, or as USB? The mount name may depend on how it was formatted.
13. When a disk is removed without unmounting, does a lost mount point (an empty `/mnt/usbN`) stay or disappear? (Needed for the "disk connected" check.)
14. Does the translation of the web interface and the folder picker work with your console language; what is your console language code (the "system service" section of the report)?
15. Unmount of extended storage: what does the system do afterwards; do games on it start after reconnecting without reformatting?
16. Does the "Disk busy" window list who holds the disk? An empty list means `kern.proc.filedesc` cannot be read on that firmware.
17. Does "Eject anyway" help when the system holds the disk; any data loss or console hangs?
18. The exact name of the console's menu item for formatting a disk for use by the system (we write "Settings → Storage"); needed for the FAT warning.
19. (answered, see the ShadowMountPlus note in section 7; implemented in 0.0.72, to be verified on 11.20) Can extended storage on USB be disconnected programmatically like "Safely remove from PS5"? What exactly does it do: unmount (and the sandbox mounts), close `es*.crypt`, detach USB, ask a system service (which: `sceUsbStorage*`, `sceSystemService*`, `SceShellCore`)?
    Ways to find out: testers with debug settings; the mount table and `/dev` before and after the proper removal; other projects (ps5upload "Eject", etaHEN, ShadowMount+).
20. Send the mount table with extended storage on USB, then use the proper removal and send it again: which lines vanished (`/mnt/extN`, sandbox mounts, `/dev/esN.crypt`, `/dev/daN`)? Does the disk mount as before when reinserted?
21. If the device stays in `/dev` after the proper removal, the complaint about improper removal has another cause. Does the message persist if only the sandbox mounts are unmounted first, then `/mnt/extN`?
22. Does the console show the right size of the extended storage after only the sandbox mounts are unmounted (the 2 MB was the size of an empty mount point)?

## 11. Status, plans and ideas

**Refactoring for open source (the version in the repository is the state after it):** stage 1 everything out of the build script into files; 2 the folder layout; 3 English as the main language of the settings
page; 4 splitting of the big files (web client, settings page, `ui` and `notify` C++ into modules, `main.c` into common/daemon/manager); 5 tests renamed and tools rewritten in Node; 6 documentation.
Done in stage 6: `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `patches/README.md`, this file, the safety notes translated, comments translated to English, the old history and migration notes removed from the tree.
**CI (green since 2026-10-30: Build about 1.5 min with a warm cache, Tests about 21 min):** `.github/workflows/` `build.yml` (build + import check + translation check + lint on every push and pull request; no macOS job, by decision), `tests.yml` (smoke suites and page tests on every push/PR, all suites on request and before a release), a branch push whose commit already has a tag is skipped by Build and Tests (a `gate` job looks at `git ls-remote --tags`; the Release of that tag runs the same checks, **not yet seen working on GitHub**), `release.yml` (a tag equal to `VERSION` (no `v`: `0.1`): full tests, build, a GitHub release with `Transmission-<version>.elf`, its `.sha256` and the notes of `CHANGELOG.md`); how: `docs/releasing.md`. `scripts/test.sh` and `tests/run_pages.py` now return a failure status. **Build and Tests skip docs-only changes** (`paths-ignore` in both: `*.md`, `**/*.md`, `docs/**`, `LICENSE`); a tag push (`release.yml`) and the `workflow_call` from Release are not affected. Lessons of the first runs (and of 0.2: the PS5 build folder of Transmission used to be the same for every version and configured once, so after a version bump a warm `.deps` silently built the OLD Transmission; it is now `.deps/build/transmission-<version>` with a check of the source path and a check that the daemon contains the sources of the pinned version, and CI restores caches by prefix, so it could have hit the same): a clang-tidy found on the runner broke the Transmission build (`-DRUN_CLANG_TIDY=OFF` now), the host test build needs libc++/OpenSSL/curl/zlib dev packages, and tests that look at the manager's once-a-second state must wait for it (a slow runner exposes such races).
**Not done yet:** `docs/` pages in English (architecture, adding a patch or a language), a final full test run, and the clean git history (the maintainer will force-push when 0.1 is ready; the old transcripts must stay out of it).
The icons are shipped as ready CSS (`patches/web-client/icons/README.md` says where they come from); regenerating them is not supported by the repository. The home-screen tile editor was a separate project (not here).

**Ideas and plans (not started; discuss first):**
- **Make the ELF smaller** (only a nicety, no loader problem is known: the worry came from the payload that did not start on 13.60, and that was the old SDK, not the size; 0.2 was 12.3 MB, the manager about 1.2 MB, the embedded daemon 10.5 MB; 0.4 is 17.4 MB with curl and OpenSSL in the manager). Measured on 2026-10-31: (1) `strip --strip-all` (keeps `.dynsym`) takes the daemon from 10.5 to 8.5 MB, about 10 MB in total, near-zero risk; today only `--strip-debug` is used. (2) Keep the daemon deflated in the manager (gzip -9 gives 3.45 MB for the stripped daemon, about 4.7 MB in total) and inflate it into the file at start: new code, zlib has to be linked into the manager, must be tried on a console. (3) `-Os`, `-ffunction-sections` and `--gc-sections` for the daemon and its libraries: maybe 1-2 MB more, needs the full tests and a console. Not worth trying: DT_RELR (`-z pack-relative-relocs`), the loader may not know it. A 349 KB closed-source client on 4pda is not comparable (it cannot contain all of Transmission, OpenSSL, curl and libevent).
- **Updating from the settings page (0.4, `updater.cc`; the page itself never asks GitHub: the browser-side "update available" notice of 0.2/0.3 was removed, everything goes through the manager).** The manager itself (libcurl + OpenSSL are linked into it since 0.4, the CA bundle is the daemon's `cacert.pem`) asks `api.github.com/.../releases/latest`, **only while `update_check` is on** (Settings → Updates): the first time the page asks `/api/update`, then **at most once a day**. The time and the result of the last finished check (`latest`, `available`, `notfound`, `failed`) are kept in `/data/Transmission/update_state.txt`, so the limit survives a restart; after a failure the retry comes in 15 minutes, and an offer (`available`) is asked again at every manager start because the offer itself is not saved. The Settings card has the row "Last check" (date and time in the language of the page, and how it ended) with the button "Check now", which checks at once whatever the setting and the limit say (so does "Try again" in the failure dialog); once the update is ready the button becomes "Restart" and the header link "restart to update to N", and both only open the dialog, where Restart is pressed (a failed restart shows its reason there). **The update dialog (green) never opens by itself**: it opens from the header link "update available N" (shown only when the manager has an update to install) or from the same words, a link, in the "Last check" row. Its content follows the state: the offer (version, "What's new"), "Updating…" with the progress, "Update ready" with Restart, or the failure with "Try again"; the version and "What's new" are shown in the offer only. A newer release is offered only if the running file is found: the file that PLDMGR launched (`/history_list` of PLDMGR, then a scan of `/data/pldmgr/payloads` and `/mnt/usbN`) whose SHA-256 equals the `.sha256` of the running version's release asset; a debug build never updates. "Update" downloads the asset to a hidden `.part` file next to the old one, compares its SHA-256 with the release's `.sha256`, replaces the file **in its own folder** (nothing is renamed but the file: PLDMGR does not care what the folder or the file is called), rewrites the PLDMGR sidecar (`<file>.json`, layout 0.5.2) or `meta.json` (0.6.x) and the name in `autoload.txt`, and "Restart" streams the new ELF to elfldr (127.0.0.1:9021) and exits; the new manager stops the old one and takes over the running daemon. A failed restart leaves the update Ready with its reason. The curl connect phase (which includes the name lookup) is 15 s, not 5 s: on the maintainer's network the first lookup after the start took more than 5 s (`Resolving timed out after 5001 milliseconds`, the check failed at the first page load); a request of the check that got no answer at all (the name lookup, the connection, TLS; the request for the release and the one for the `.sha256`) is made up to 3 times with a 60 s pause (`RequestAttempts`, `RetryPauseSeconds`; test hook `PS5TM_UPDATER_RETRY_PAUSE`, seconds), all in the thread of the check, so no page is needed and the state stays `checking` meanwhile; an HTTP answer, whatever its status, is final. Why: on the maintainer's console the DNS is slow in the first minute after the start (the report showed `Resolving timed out` at 6 s and a success at 85 s; the daemon's own lookups of trackers fail the same way, and `nanodns.elf` runs on that console), so one retry 2 s later was not enough. **There is no signature: the SHA-256 comes from the same release over HTTPS, so it protects against a damaged download but not against a compromised release (an Ed25519 signature checked against a key inside the ELF would).** **Decided by the maintainer: no signature.** elfldr (9021) runs any ELF from anyone on the network, any web page or payload can already start whatever it wants, so a signature on our update would protect nothing; SHA-256 against a damaged download is enough. Test hooks: `PS5TM_UPDATER_API|RELEASES|TAG_BASE|SOURCE|PLDMGR|ELFLDR` point the module at local stands (`tests/suites/core/test_update.py`), `PS5TM_NO_UPDATE=1` stops the automatic check (the harness sets it unless `PS5TM_UPDATER_API` is set, so the page tests never reach GitHub). `updater.cc` is deliberately a self-contained module (the maintainer reuses it in other projects): it has its own small helpers (atomic write, JSON, SHA-256, file reading) instead of the common ones of this project; do not "deduplicate" it with `src/common`.
- **One page with tabs (Settings | Transmission, L1/R1).** Needs a probe of how L1/R1 arrive in the console browser first. Cheapest: the same tab strip on both pages (port 9092 and 9091), switching by navigation. A manager-side reverse proxy (`/transmission/*` to the local daemon) would make it one origin; it must keep the private-network check that the daemon does (the daemon would see 127.0.0.1) and the daemon may redirect the web pages of port 9091 to 9092 but not the RPC calls. A fully own UI on the RPC API is possible, big, and can come tab by tab.
- **Downloading while the console is in rest mode** (like ps5-library, "Rest Mode"): downloading and unpacking continue only on the internal drive (a USB disk may vanish in rest mode); while there is work the payload keeps the CPU active, then the
  console sleeps by itself; the console's "stay connected" setting must be on. How the CPU is kept active is not stated (hypothesis: `sceSystemStateMgrRequestToKeepMainOnStandby` from `libSceSystemService`). Interesting but hard and dangerous.
- **Protect the settings page**: it is open to the whole local network, and now it can power the console off or exit. Idea from ps5-library: pairing a phone or a computer with a code shown on the console.
- A `payloads.json` source for Payload Manager (as ps5-library).
- Hiding home-screen tiles ("PlayStation Store", "All apps"): worked in a separate experiment by editing the tile database (`/system_data/priv/mms/app.db`, column `visible`), not part of this project.
- In the sleep mode, count torrents with no seeds and zero speed for N minutes as finished (RPC: `rateDownload`, `peersSendingToUs`, `peersConnected`; tracker counters are unreliable).
- Show a notification when a folder with active torrents is removed from the list; the disk panel could show folders (not only disks); subfolders are just separate entries (`/mnt/usb0/torrents/Games`).
- The status line of the page ("running; game: None; ... keep awake: Off") is built from pieces with capitalized words in the middle of a phrase; it could be rewritten as whole texts.
- **Reminder, a clean-up to remove in 0.6 or later:** the manager deletes the old `/data/Transmission/guard_messages.txt` (`web_lang_apply()` in `src/manager/web_files.cc`, the constant `OldGuardMessagesFile`); the file moved to `transmission/` in 0.4 and the deletion only tidies up consoles that update from 0.3 or older. Remove the `unlink`, the constant and the stale-file lines of `tests/suites/language/test_web_client_translation.py` (`GM_OLD`).
- **Test noise to clean up:** `tests/web/settings_page_test.js` (line ~82) replaces `window.api` with a stub that answers everything but `/api/state` with `{ok:true}`; the page's own 10-second `fdRefresh` then gets that for `GET /api/folders`, and `fdRenderNow` throws `Cannot read properties of undefined (reading 'forEach')` in a jsdom timer (an uncaught error in stderr after the checks; all 139 pass; not a product bug: the real answer always has `folders`, and a failed one is skipped by `fdRefresh`). Fix: let the stub pass `GET /api/folders` to the real `api`; optionally `&&j.folders` in `fdRefresh`. `tests/run_pages.py` shows only the last 150 characters of stderr: widen it when something like this has to be read.
- Known: the panic when entering rest mode with Transmission running did not reproduce on the latest version.

**Verified by the maintainer on 11.20 before 0.1:** power off at the end of downloads, full exit, the network-change restart.
**Not verified on a console (as of 0.1):** the "Disk busy" window with the list of holders (`kern.proc.filedesc`); the icon of "a single file inside a folder"; disabling auto-sleep on wake; behavior of the USB
fixes on a real exFAT disk beyond the maintainer's reports; other firmware versions than 11.20 (users reported that it works on 10.01, 11.60 and 13.60; no detailed test reports).

## 12. Decisions of the maintainer worth remembering

- The path of a torrent chosen at add time is never changed by us. An empty folder list means nothing can be added.
- Read-only disks are not our concern (Transmission shows its own error). The main goal is to minimize the risk to the console; the rest is the maintainer's call.
- A page for the whole network without a password is deliberate (`rpc-whitelist` limits it to private networks).
- Python host tests stay (they drive the real daemon); the build and developer tools are shell and Node.
- The version scheme was `0.0.N` until the first public release `0.1` (2026-10-30); `VERSION` now holds `0.N` (the second number only: `0.2`, `0.3`, even `0.42`; no third number), raise it for the next change that goes to a console or a release **only when the current version is already tagged** (until then new changes go into its `CHANGELOG.md` section), and write a `CHANGELOG.md` section for each release.
- **Every `CHANGELOG.md` version section ends with a `### Install` line — `[How to install](https://github.com/freefelt/Transmission-PS5#-install)`** (it becomes the GitHub release notes; see `docs/releasing.md`). **Every bullet of a section is ONE line** (no wrapped lines). Do not drop it or change the link text when writing a release. The version heading has no date (`## [<version>]`): a release only needs `VERSION` to be raised and the section to be written; the release date is shown on the GitHub Releases page.
- **`CHANGELOG.md` is the user-facing release note, not a technical log.** A bullet is one short sentence with the essence: what changed for the user, not how it works. Write "Improved the closing flow.", not a paragraph about polling, states, buttons or files. No implementation details, file or module names, test descriptions or a retelling of commits.
- **One user-visible change = one bullet.** Merge related edits; do not list every file, commit or fix. Keep a version section short; leave out internal changes with no user-visible effect.
- **Do not rebuild or run tests for a `CHANGELOG.md`-only edit.** The existing build/test rules apply to code and generated pages.
- The license is GPL-3.0-or-later for the whole project.
