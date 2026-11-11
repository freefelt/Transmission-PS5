import os, sys; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lib"))
import re, sys
from harness import *
# "Diagnostics → Prepare report": GET /api/report is the report as a file to download, POST /api/report {"action":"save"} writes /data/Transmission/report.txt
# (the console's browser cannot save a file). The address of the console is replaced by <console-ip>.
REPORT = "/data/Transmission/report.txt"
fresh(); loader = Loader(); m = start_manager()
wait(lambda: daemon_pids() and rpc_ok() and dstate() is not None and dstate()["state"] == "running", 70); time.sleep(3)
st = json.loads(api("GET", "/api/state")[1]); ip = st.get("ip", "")

def download():
    r = urllib.request.urlopen(f"http://127.0.0.1:{PORT['api']}/api/report", timeout=60)
    return r.status, dict(r.headers), r.read().decode("utf-8", "replace")

print("== 1) the download")
c, h, text = download()
check("GET /api/report answers 200 with text/plain", c == 200 and h.get("Content-Type", "").startswith("text/plain"), (c, h.get("Content-Type")))
cd = h.get("Content-Disposition", "")
check("it is a file to download: Content-Disposition attachment with a plain file name (no spaces)", bool(re.fullmatch(r'attachment; filename="transmission-ps5-[A-Za-z0-9.\-]+-report\.txt"', cd)), cd)
check("it starts with the title and the warning to look it through before sending; the address is announced as hidden", text.startswith("# Transmission for the PS5: report\n# Look through this file before you send it") and "<console-ip>" in text.split("\n")[1], text[:200])
check("the first line of the data: the version, the commit, the SDK, the build time, the uptime, the Transmission version", bool(re.search(r"^=== version \S.*, commit \S+, SDK \S+, built .+, running \d+ s, Transmission \S+$", text, re.M)), [l for l in text.split("\n") if l.startswith("=== version")])
SECTIONS = ["system service", "power state", "updates", "settings (config.cfg)", "Transmission check", "disks", "mount points", "threads", "console processes and ports", "wrapper log (from memory)",
            "manager log file (shim.log, latest)", "sleep and daemon events", "Transmission log (latest)", "daemon output (console.log, latest)", "daemon wrapper log (daemon.log, latest)"]
missing = [s for s in SECTIONS if f"\n=== {s}\n" not in text]
check("all sections are there, in the order of the list", not missing and [text.index(f"\n=== {s}\n") for s in SECTIONS] == sorted(text.index(f"\n=== {s}\n") for s in SECTIONS if s not in missing), missing)
check("the report names the firmware of the console (the host stub answers 11.20)", "\nconsole firmware: 11.20 (0x11200000)\n" in text, [l for l in text.split("\n") if "firmware" in l])
open("/tmp/ps5tm_test_firmware", "w").write("0x13600000\n"); fw = download()[2]; open("/tmp/ps5tm_test_firmware", "w").write("0\n"); fw0 = download()[2]; os.remove("/tmp/ps5tm_test_firmware")
check("the firmware is decoded from the SDK number (0x13600000 is 13.60; 0 is unknown)", "\nconsole firmware: 13.60 (0x13600000)\n" in fw and "\nconsole firmware: unknown" in fw0, [l for l in (fw + fw0).split("\n") if "firmware" in l])
def model_line(name):
    if name is None:
        if os.path.exists("/tmp/ps5tm_sys_model"): os.remove("/tmp/ps5tm_sys_model")
    else: open("/tmp/ps5tm_sys_model", "w").write(name + "\n")
    return [l for l in download()[2].split("\n") if l.startswith("console model:")]
got = {n: model_line(n) for n in (None, "CFI-7019", "CFI-2000B01Y", "CFI-1215A", "Prospero Rev.A", "")}
if os.path.exists("/tmp/ps5tm_sys_model"): os.remove("/tmp/ps5tm_sys_model")
check("the report names the console model, with the family for CFI-7xxx (Pro), CFI-2xxx (Slim), CFI-1xxx (original); any other name is shown as it is; an empty one is unknown with the reason",
      got[None] == ["console model: CFI-1015A (PS5 (original))"] and got["CFI-7019"] == ["console model: CFI-7019 (PS5 Pro)"] and got["CFI-2000B01Y"] == ["console model: CFI-2000B01Y (PS5 Slim)"]
      and got["CFI-1215A"] == ["console model: CFI-1215A (PS5 (original))"] and got["Prospero Rev.A"] == ["console model: Prospero Rev.A"] and len(got[""]) == 1 and got[""][0].startswith("console model: unknown ("), got)
check("the settings section holds config.cfg (the download folder of the stand)", "download_folder=/data/torrents" in text.split("\n=== settings (config.cfg)\n")[1].split("\n=== ")[0])
check("the disks section lists which of /mnt/ext0..7 and /mnt/usb0..7 are connected", all(x in text.split("\n=== disks\n")[1].split("\n=== ")[0] for x in ("/mnt/usb0", "/mnt/ext7")), text.split("\n=== disks\n")[1][:120])
check("the Transmission check says that the daemon answers", "connect OK" in text.split("\n=== Transmission check\n")[1].split("\n=== ")[0])
check("the system service and the power state are there as data (JSON)", '"ready":' in text.split("\n=== system service\n")[1].split("\n=== ")[0] and '"state":' in text.split("\n=== power state\n")[1].split("\n=== ")[0])
check("the manager log file section has lines of the manager", re.search(r"\b(daemon|monitor|syssvc):", text.split("\n=== manager log file (shim.log, latest)\n")[1].split("\n=== ")[0]) is not None)
check("the old diagnostic buttons' route /api/diag/all is gone (the report replaced it)", api("GET", "/api/diag/all")[0] == 404)

print("== 2) the address of the console is hidden")
if ip and ip not in ("127.0.0.1", "0.0.0.0") and re.fullmatch(r"\d+\.\d+\.\d+\.\d+", ip):
    with open("/data/Transmission/logs/console.log", "a") as f:
        f.write(f"report-test: peer {ip}:51413 and {ip}. Next: {ip}0 and 1{ip} and 9.{ip} end\n")
    c, h, text = download()
    line = [l for l in text.split("\n") if l.startswith("report-test:")]
    check("the address is replaced where it stands alone (before a port, before a full stop)", bool(line) and "peer <console-ip>:51413 and <console-ip>. Next:" in line[0], line[:1])
    check("an address that only contains it (the same digits followed by 0, or preceded by 1 or by '9.') is left alone", bool(line) and f"{ip}0 and 1{ip} and 9.{ip} end" in line[0], line[:1])
    check("the address of the console is not in the report anywhere else", not re.search(r"(?<![\d.])" + re.escape(ip) + r"(?!\d|\.\d)", text), ip)
else:
    print(f"SKIP the address masking: the host has no address except loopback ({ip!r})")

print("== 3) saving for the console's browser")
if os.path.exists(REPORT): os.remove(REPORT)
c, t = api("POST", "/api/report", {"action": "save"}, timeout=60); j = json.loads(t)
check("POST /api/report save: ok, the path and the size", c == 200 and j.get("ok") is True and j.get("path") == REPORT and j.get("size", 0) > 1000, t[:200])
body = open(REPORT, encoding="utf-8", errors="replace").read() if os.path.exists(REPORT) else ""
check("the file holds the same kind of report (title, sections) and its size is the size announced", body.startswith("# Transmission for the PS5: report") and "\n=== threads\n" in body and len(body.encode()) == j.get("size"), len(body.encode()))
check("nothing is left next to it (no temporary file)", [f for f in os.listdir("/data/Transmission") if f.startswith("report.txt") and f != "report.txt"] == [], os.listdir("/data/Transmission"))
st_mode = os.stat(REPORT).st_mode & 0o777
check("the file is readable by anyone (to copy it out of the console)", st_mode & 0o444 == 0o444, oct(st_mode))
os.remove(REPORT)
c, t = api("POST", "/api/report", {"action": "nothing"}); check("an unknown action is refused with 400 and writes nothing", c == 400 and not os.path.exists(REPORT), (c, t[:100]))
r = urllib.request.Request(f"http://127.0.0.1:{PORT['api']}/api/report", data=b'{"action":"save"}', method="POST", headers={"Content-Type": "text/plain"})
try: code = urllib.request.urlopen(r, timeout=10).status
except urllib.error.HTTPError as e: code = e.code
check("a request that is not JSON (a form of another site) is refused with 415 and writes nothing", code == 415 and not os.path.exists(REPORT), code)
kill_all(); loader.close()
sys.exit(summary())
