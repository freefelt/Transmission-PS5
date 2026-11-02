import os, sys; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
import sys, json, subprocess, time, os, shutil, urllib.request, urllib.error, hashlib
from harness import *
# The read-only RPC method ps5_files of the web interface: the disks and the *.torrent files of the console,
# and adding a torrent by the path of a .torrent file (the stock torrent-add with "filename").
def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True)
def jrpc(method, params=None):
    sid = ""
    for _ in range(3):
        req = urllib.request.Request("http://127.0.0.1:9091/transmission/rpc", data=json.dumps({"jsonrpc": "2.0", "method": method, "params": params or {}, "id": 1}).encode(), headers={"Content-Type": "application/json", "X-Transmission-Session-Id": sid})
        try: return json.loads(urllib.request.urlopen(req, timeout=10).read())
        except urllib.error.HTTPError as e:
            if e.code == 409: sid = e.headers.get("X-Transmission-Session-Id"); continue
            return json.loads(e.read() or b"{}")
def cleanup():
    for d in ("/mnt/ext1", "/mnt/usb0"): sh(f"umount {d} 2>/dev/null; rm -rf {d}")
    sh("rm -rf /mnt/usb1 /data/torrents")
def meta(name):
    data = b"x" * 65536
    info = {"name": name, "length": 65536, "piece length": 65536, "pieces": hashlib.sha1(data).digest()}
    return bencode({"announce": "http://127.0.0.1:1/announce", "info": info})
cleanup(); shutil.rmtree("/user/app", ignore_errors=True); os.makedirs("/user/app")
for d in ("/mnt/ext1", "/mnt/usb0"): os.makedirs(d, exist_ok=True); sh(f"mount -t tmpfs -o size=128m tmpfs {d}")
os.makedirs("/mnt/usb1", exist_ok=True)          # exists but is not mounted: it must not be in the disk list
os.makedirs("/data/torrents", exist_ok=True)
os.makedirs("/mnt/ext1/torrents", exist_ok=True)
os.makedirs("/mnt/ext1/torrents/sub1", exist_ok=True)
for d in ("Alpha", "beta", ".hidden", "lost+found", "System Volume Information", "$RECYCLE.BIN"):
    os.makedirs(f"/mnt/ext1/torrents/{d}", exist_ok=True)
os.makedirs("/mnt/ext1/torrents/Ёж".encode(), exist_ok=True)               # valid UTF-8, not ASCII: shown
os.makedirs(b"/mnt/ext1/torrents/bad\xffdir", exist_ok=True)                # not valid UTF-8: left out
open("/mnt/ext1/torrents/Ёж.torrent".encode(), "wb").write(meta("Filepick_yo.bin"))
open(b"/mnt/ext1/torrents/bad\xc0\xafname.torrent", "wb").write(meta("Filepick_bad.bin"))   # an overlong form: left out
open("/mnt/ext1/torrents/A.torrent", "wb").write(meta("Filepick_A.bin"))
open("/mnt/ext1/torrents/b.TORRENT", "wb").write(meta("Filepick_B.bin"))
open("/mnt/ext1/torrents/note.txt", "wb").write(b"not a torrent")
open("/mnt/ext1/torrents/sub1/deep.torrent", "wb").write(meta("Filepick_deep.bin"))
os.makedirs("/mnt/usb0/many", exist_ok=True)
for i in range(520): os.makedirs(f"/mnt/usb0/many/d{i:03d}", exist_ok=True)
fresh(); loader = Loader(); m = start_manager()
try:
    wait(lambda: daemon_pids() and rpc_ok() and dstate()["state"] == "running", 70); time.sleep(3)

    print("== the disk list")
    r = jrpc("ps5_files"); d = r.get("result", {}).get("disks", [])
    check("ps5_files {} returns the connected disks and /data: /mnt/ext1 (ssd), /mnt/usb0 (usb), /data (int); /mnt/usb1 is NOT mounted and is not listed; there is no /",
          [x.get("root") for x in d] == ["/mnt/ext1", "/mnt/usb0", "/data"] and [x.get("kind") for x in d] == ["ssd", "usb", "int"] and all(x.get("root") != "/" for x in d), [(x.get("root"), x.get("kind")) for x in d] if d else r)
    check("the disks have the same fields as the manager's folder picker (root, kind, fs, fsbad, total, free) and real numbers",
          all(set(x) == {"root", "kind", "fs", "fsbad", "total", "free"} for x in d) and d[0]["total"] == 128 * 1024 * 1024 and 0 < d[0]["free"] <= d[0]["total"] and d[1]["fs"] == "exFAT" and d[1]["fsbad"] is True and d[2]["total"] > 0, d)

    print("== the contents of a folder")
    r = jrpc("ps5_files", {"path": "/mnt/ext1/torrents"}); j = r.get("result", {})
    check("only folders and *.torrent files: hidden/service folders are skipped, ordinary files are not shown; the path and the disk root come back",
          j.get("path") == "/mnt/ext1/torrents" and j.get("root") == "/mnt/ext1" and j.get("dirs") == ["Alpha", "beta", "sub1", "Ёж"] and
          [x.get("name") for x in j.get("files", [])] == ["A.torrent", "b.TORRENT", "Ёж.torrent"] and j.get("truncated") is False, j)
    check("names that are not valid UTF-8 (a stray byte, an overlong form) are left out, the answer is still valid; a valid non-ASCII name is shown",
          bool(j) and not any("bad" in n for n in j.get("dirs", []) + [x.get("name", "") for x in j.get("files", [])]), j)
    check("the files have their real size", all(x.get("size") == os.path.getsize("/mnt/ext1/torrents/" + x["name"]) for x in j.get("files", [])), j.get("files"))
    r = jrpc("ps5_files", {"path": "/mnt/ext1/torrents/sub1"}); j = r.get("result", {})
    check("a subfolder is browsable and shows its own .torrent file", j.get("dirs") == [] and [x.get("name") for x in j.get("files", [])] == ["deep.torrent"], j)
    r = jrpc("ps5_files", {"path": "/data"}); j = r.get("result", {})
    check("/data is browsable, the service folder Transmission is hidden", j.get("root") == "/data" and "Transmission" not in j.get("dirs", []) and "torrents" in j.get("dirs", []), j.get("dirs"))

    print("== the 500-entry cut")
    r = jrpc("ps5_files", {"path": "/mnt/usb0/many"}); j = r.get("result", {})
    check("more than 500 folders: the first 500 alphabetically and truncated=true", len(j.get("dirs", [])) == 500 and j.get("truncated") is True and j["dirs"][0] == "d000" and j["dirs"][-1] == "d499", (len(j.get("dirs", [])), j.get("truncated")))

    print("== refused paths")
    for path, code in (("/etc", "outside"), ("/mnt/sandbox", "outside"), ("/mnt/ext1/../../etc", "badpath"), ("relative", "badpath"), ("/mnt/ext1/nothing", "nodir")):
        r = jrpc("ps5_files", {"path": path}); j = r.get("result", {})
        check(f"a path outside the disks or a bad path is refused with the code '{code}': {path!r}", j.get("ok") is False and j.get("error") == code, (r.get("result"), r.get("error")))

    print("== adding a torrent by the path of a .torrent file")
    r = jrpc("torrent_add", {"filename": "/mnt/ext1/torrents/A.torrent", "download_dir": "/data/torrents", "paused": True}); res = r.get("result", {})
    t = res.get("torrent_added") or res.get("torrent_duplicate") or {}
    check("the stock torrent-add with 'filename' (a local path) adds the torrent: the daemon reads the file itself", t.get("name") == "Filepick_A.bin", (r, t))
    r = jrpc("torrent_get", {"fields": ["name", "download_dir", "status"]}); ts = r.get("result", {}).get("torrents", [])
    t = next((x for x in ts if x.get("name") == "Filepick_A.bin"), {})
    check("the torrent is in the list, paused, in the folder that was chosen", t.get("download_dir") == "/data/torrents" and t.get("status") == 0, t)
    r = jrpc("torrent_add", {"filename": "/mnt/ext1/torrents/note.txt", "download_dir": "/data/torrents", "paused": True})
    check("a file that is not a torrent is refused by the daemon (the page shows the error)", "error" in r or (r.get("result", {}).get("torrent_added") is None and r.get("result", {}).get("torrent_duplicate") is None), r)
finally:
    kill_all(); loader.close(); cleanup()
sys.exit(summary())
