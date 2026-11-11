#!/usr/bin/env python3
# The release updater: check GitHub (a local HTTP stand), find the file PLDMGR launched through /history_list,
# verify the release SHA-256, replace the file and its metadata, and send the new ELF to elfldr.
#
# The test is offline: the GitHub API, the release assets and PLDMGR's history are local HTTP servers, and the
# ELF loader is a local socket. 0.5.2 (sidecar) and 0.6.x (meta.json) layouts are checked, then a wrong SHA-256 with a
# retry, a restart that fails and is pressed twice, and the once-a-day limit of the automatic check.

import hashlib, json, os, shutil, socket, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "lib"))
import harness
from harness import api, wait, kill_all, fresh, start_manager

OLD = b"\x7fELF" + b"old Transmission 0.4 " + b"x" * 200
NEW = b"\x7fELF" + b"new Transmission 0.5 " + b"y" * 200
OLD_SHA = hashlib.sha256(OLD).hexdigest()
NEW_SHA = hashlib.sha256(NEW).hexdigest()


class Catalog(BaseHTTPRequestHandler):
    history = []
    bad_sha = False        # the .sha256 of 0.5 does not belong to the file that is served
    latest = "0.5"
    hits = {}
    drop = {}              # path -> how many of the next requests get no answer at all (the connection is closed)
    latest_status = 200    # the status of /latest (500: an answer that is final)
    def log_message(self, *a):
        pass
    def do_GET(self):
        path = self.path.split("?")[0]
        Catalog.hits[path] = Catalog.hits.get(path, 0) + 1
        if Catalog.drop.get(path, 0) > 0:
            Catalog.drop[path] -= 1
            self.close_connection = True
            try: self.connection.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            return
        if path == "/latest" and Catalog.latest_status != 200:
            self.send_error(Catalog.latest_status); return
        if path == "/latest":
            body = json.dumps({"tag_name": Catalog.latest, "html_url": "http://example.invalid/releases/tag/" + Catalog.latest}).encode()
        elif path == "/history":
            body = json.dumps({"history": self.history}).encode()
        elif path == "/download/0.4/Transmission-0.4.elf.sha256":
            body = ("%s  Transmission-0.4.elf\n" % OLD_SHA).encode()
        elif path == "/download/0.5/Transmission-0.5.elf.sha256":
            body = ("%s  Transmission-0.5.elf\n" % ("0" * 64 if Catalog.bad_sha else NEW_SHA)).encode()
        elif path == "/download/0.5/Transmission-0.5.elf":
            body = NEW
        elif path == "/download/0.4/Transmission-0.4.elf":
            body = OLD
        else:
            self.send_error(404); return
        self.send_response(200)
        self.send_header("Content-Type", "application/json" if path == "/latest" or path == "/history" else "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Elfldr:
    def __init__(self, port):
        self.data = b""
        self.got = threading.Event()
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port)); self.srv.listen(1)
        threading.Thread(target=self.run, daemon=True).start()
    def run(self):
        try:
            c, _ = self.srv.accept()
            while True:
                d = c.recv(1 << 20)
                if not d: break
                self.data += d
            c.close()
        except OSError:
            pass
        finally:
            self.got.set()
    def close(self):
        try: self.srv.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        try: self.srv.close()
        except OSError: pass


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f: f.write(data)


def status():
    c, t = api("GET", "/api/update")
    try:
        return json.loads(t)
    except ValueError:
        return {"state": "?", "raw": t}


def state_is(name):
    return lambda: status().get("state") == name


def prepare(root, meta, loader_port, latest="0.5"):
    """The stand: the files of 0.4 where PLDMGR keeps them, the local GitHub and PLDMGR, the loader, the manager."""
    shutil.rmtree("/data/pldmgr", ignore_errors=True)
    path = os.path.join(root, "Transmission-0.4.elf")
    put(path, OLD)
    if meta is None:
        sidecar = path + ".json"
        with open(sidecar, "w", encoding="utf-8") as f:
            json.dump({"name": "Transmission", "filename": os.path.basename(path), "version": "0.4",
                       "checksum": OLD_SHA, "install_source": "web_upload", "url": "", "source": "",
                       "source_direct": "", "description": "", "last_update": "", "category": "",
                       "downloaded_at": "", "install_source_detail": "", "source_name": ""}, f)
        autoload = os.path.join("/data/pldmgr", "autoload.txt")
        os.makedirs("/data/pldmgr", exist_ok=True)
        open(autoload, "w").write("Transmission-0.4.elf\n")
    else:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(os.path.join(os.path.dirname(path), "meta.json"), "w", encoding="utf-8") as f:
            json.dump({"schema": 2, "kind": "local", "filename": os.path.basename(path),
                       "installed_at": "", "origin": "web_upload", "origin_detail": ""}, f)

    Catalog.history = [path]
    Catalog.bad_sha = False
    Catalog.latest = latest
    Catalog.hits = {}
    Catalog.drop = {}
    Catalog.latest_status = 200
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Catalog)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    loader = Elfldr(loader_port)
    os.environ.update({
        "PS5TM_UPDATER_API": f"http://127.0.0.1:{port}/latest",
        "PS5TM_UPDATER_RELEASES": f"http://127.0.0.1:{port}/download",
        "PS5TM_UPDATER_PLDMGR": f"http://127.0.0.1:{port}/history",
        "PS5TM_UPDATER_SOURCE": f"http://127.0.0.1:{port}/",
        "PS5TM_UPDATER_TAG_BASE": f"http://127.0.0.1:{port}/tag/",
        "PS5TM_UPDATER_ELFLDR": "127.0.0.1:%d" % loader_port,
    })
    fresh()
    mgr = start_manager("updater")
    wait(lambda: api("GET", "/api/state")[0] == 200, 40, 0.3)   # /api/state does not start the release check: the first /api/update below does
    return {"path": path, "dir": os.path.dirname(path), "httpd": httpd, "loader": loader, "mgr": mgr}


def finish(ctx):
    kill_all(); ctx["loader"].close(); ctx["httpd"].shutdown(); ctx["httpd"].server_close()
    for key in list(os.environ):
        if key.startswith("PS5TM_UPDATER_"): del os.environ[key]


def run_case(root, meta, loader_port):
    ctx = prepare(root, meta, loader_port)
    path = ctx["path"]; mgr = ctx["mgr"]
    try:
        first = status()
        harness.check("update: the FIRST status request starts the check and says 'checking' (not 'idle': the page stops asking on idle)", first.get("state") in ("checking", "available"), first)
        r = wait(state_is("available"), 15, 0.5)
        harness.check("update: the release and the launched file are found", r, api("GET", "/api/update")[1])
        j = status()
        ok = j.get("latest") == "0.5" and j.get("changelog", "").endswith("/tag/0.5")
        harness.check("update: status has 0.5 and a changelog URL", ok, j)
        harness.check("update: the finished check is remembered (time, result 'available') and the setting is on", j.get("last_result") == "available" and j.get("last_check", 0) > 0 and j.get("enabled") is True, j)
        if not ok:
            return
        c, t = api("POST", "/api/update", {"action": "download"})
        harness.check("update: download starts", c == 200 and json.loads(t)["ok"] is True, t)
        r = wait(state_is("ready"), 20, 0.5)
        new_path = os.path.join(ctx["dir"], "Transmission-0.5.elf")
        harness.check("update: the new file is in place and the old one is gone", r and os.path.exists(new_path) and sha(new_path) == NEW_SHA and not os.path.exists(path),
                      api("GET", "/api/update")[1])
        harness.check("update: the folder keeps its name and nothing else is left in it (no .part)", os.path.isdir(ctx["dir"]) and not [f for f in os.listdir(ctx["dir"]) if f.endswith(".part")], os.listdir(ctx["dir"]))
        if meta is None:
            sc = json.load(open(new_path + ".json", encoding="utf-8"))
            harness.check("update: the 0.5.2 sidecar is complete", sc.get("version") == "0.5" and sc.get("checksum") == NEW_SHA and sc.get("category") == "Networking & Servers" and sc.get("filename") == "Transmission-0.5.elf", sc)
            harness.check("update: the old sidecar is gone", not os.path.exists(path + ".json"), os.listdir(ctx["dir"]))
            harness.check("update: autoload uses the new file name", "Transmission-0.5.elf" in open("/data/pldmgr/autoload.txt").read(), open("/data/pldmgr/autoload.txt").read())
        else:
            m = json.load(open(os.path.join(ctx["dir"], "meta.json"), encoding="utf-8"))
            harness.check("update: the 0.6 meta points at the new file", m.get("filename") == "Transmission-0.5.elf" and m.get("version") == "0.5" and m.get("checksum") == NEW_SHA and m.get("kind") == "local", m)
        c, t = api("POST", "/api/update", {"action": "check"})
        harness.check("update: a check is refused while the installed update waits for the restart", c == 200 and json.loads(t)["ok"] is False and state_is("ready")(), t)

        # the loader is not listening: the restart fails, the update stays Ready and the reason is reported
        ctx["loader"].close(); time.sleep(0.3)
        c, t = api("POST", "/api/update", {"action": "restart"})
        harness.check("update: restart is accepted", c == 200 and json.loads(t)["ok"] is True, t)
        r = wait(lambda: status().get("restart_error"), 10, 0.3)
        j = status()
        harness.check("update: a failed restart keeps the state Ready, says why, and is not 'restarting' any more", r and j.get("state") == "ready" and "not listening" in j.get("restart_error", "") and j.get("restarting") is False, j)
        harness.check("update: the manager is still alive after the failed restart", mgr.poll() is None)

        # the loader is back: two presses of Restart in a row send the file once
        loader = ctx["loader"] = Elfldr(loader_port)
        c1, t1 = api("POST", "/api/update", {"action": "restart"})
        c2, t2 = api("POST", "/api/update", {"action": "restart"})
        harness.check("update: the first Restart is accepted, the second is refused as busy", json.loads(t1)["ok"] is True and json.loads(t2)["ok"] is False and "busy" in t2, (t1, t2))
        harness.check("update: the new ELF reaches elfldr and the manager exits", loader.got.wait(10) and loader.data == NEW and wait(lambda: mgr.poll() is not None, 5, 0.2), "%d bytes" % len(loader.data))
    finally:
        finish(ctx)


def run_bad_sha(root, loader_port):
    """The release says another SHA-256 than the file has: nothing is installed; after an error a new check finds it again."""
    ctx = prepare(root, None, loader_port)
    path = ctx["path"]
    try:
        Catalog.bad_sha = True
        harness.check("update (wrong SHA-256): the release is offered", wait(state_is("available"), 15, 0.5), api("GET", "/api/update")[1])
        c, t = api("POST", "/api/update", {"action": "download"})
        r = wait(state_is("error"), 20, 0.5)
        j = status()
        harness.check("update (wrong SHA-256): the download ends in an error that names the SHA-256", c == 200 and r and "SHA-256" in j.get("error", ""), j)
        harness.check("update (wrong SHA-256): the old file and its sidecar are untouched, no new file, no .part", os.path.exists(path) and sha(path) == OLD_SHA and not os.path.exists(os.path.join(ctx["dir"], "Transmission-0.5.elf"))
                      and not [f for f in os.listdir(ctx["dir"]) if f.endswith(".part")] and os.path.exists(path + ".json"), os.listdir(ctx["dir"]))
        c, t = api("POST", "/api/update", {"action": "restart"})
        harness.check("update (wrong SHA-256): Restart is refused and the loader got nothing", json.loads(t)["ok"] is False and ctx["loader"].data == b"" and ctx["mgr"].poll() is None, t)
        Catalog.bad_sha = False
        c, t = api("POST", "/api/update", {"action": "check"})
        harness.check("update (retry): a new check is accepted from the error state", c == 200 and json.loads(t)["ok"] is True, t)
        harness.check("update (retry): it finds the release again", wait(state_is("available"), 15, 0.5), api("GET", "/api/update")[1])
        api("POST", "/api/update", {"action": "download"})
        r = wait(state_is("ready"), 20, 0.5)
        new_path = os.path.join(ctx["dir"], "Transmission-0.5.elf")
        harness.check("update (retry): the second download installs the file", r and os.path.exists(new_path) and sha(new_path) == NEW_SHA and not os.path.exists(path), api("GET", "/api/update")[1])
    finally:
        finish(ctx)


def run_no_update(root, loader_port):
    """Nothing newer on GitHub: one request, and the next page loads do not ask GitHub again (once a day)."""
    ctx = prepare(root, None, loader_port, latest="0.4")
    try:
        status()
        r = wait(lambda: status().get("state") == "idle" and Catalog.hits.get("/latest", 0) >= 1, 10, 0.3)
        harness.check("update (no news): the check asks GitHub once and ends 'idle'", r, (Catalog.hits, status()))
        for _ in range(4):
            status(); time.sleep(0.4)
        harness.check("update (no news): more status requests (page loads) do not ask GitHub again", Catalog.hits.get("/latest") == 1 and status().get("state") == "idle", Catalog.hits)
        c, t = api("POST", "/api/update", {"action": "check"})
        r = wait(lambda: status().get("state") == "idle" and Catalog.hits.get("/latest", 0) == 2, 10, 0.3)
        harness.check("update (no news): an explicit check (the Try again button) ignores the limit", c == 200 and r, (t, Catalog.hits))
    finally:
        finish(ctx)


def run_retries(root, loader_port):
    """A request of the check that gets no answer at all (a slow DNS, a dropped connection) is made up to 3 times with a pause (here 2 s, the console: 60 s); an HTTP answer is final."""
    os.environ["PS5TM_UPDATER_RETRY_PAUSE"] = "2"
    sha_path = "/download/0.4/Transmission-0.4.elf.sha256"
    ctx = prepare(root, None, loader_port)
    try:
        Catalog.drop = {"/latest": 2}
        status(); seen = set()
        t0 = time.time()
        while time.time() - t0 < 25 and status().get("state") != "available":
            seen.add(status().get("state")); time.sleep(0.3)
        harness.check("update (retries): two requests without an answer, the third one gets it: the check ends 'available' after exactly 3 requests", status().get("state") == "available" and Catalog.hits.get("/latest") == 3, (Catalog.hits, status()))
        harness.check("update (retries): while the attempts go on the state is 'checking' (the page shows it)", "checking" in seen, seen)
        log = harness.shim()
        harness.check("update (retries): the log says that it tries again, with the attempt number", "trying again in 2 s (attempt 2 of 3)" in log and "trying again in 2 s (attempt 3 of 3)" in log, [l for l in log.split("\n") if "update:" in l][:8])
    finally:
        finish(ctx)
    os.environ["PS5TM_UPDATER_RETRY_PAUSE"] = "2"
    ctx = prepare(root, None, loader_port)
    try:
        Catalog.drop = {"/latest": 99}
        status()
        r = wait(lambda: status().get("state") == "idle" and status().get("last_result") == "failed", 25, 0.3)
        harness.check("update (retries): no answer three times: the check ends as failed after exactly 3 requests (not more)", r and Catalog.hits.get("/latest") == 3, (Catalog.hits, status()))
        for _ in range(4):
            status(); time.sleep(0.4)
        harness.check("update (retries): after the failure the page loads do not ask again at once (the next try is in 15 minutes)", Catalog.hits.get("/latest") == 3, Catalog.hits)
    finally:
        finish(ctx)
    os.environ["PS5TM_UPDATER_RETRY_PAUSE"] = "2"
    ctx = prepare(root, None, loader_port)
    try:
        Catalog.latest_status = 500
        status()
        r = wait(lambda: status().get("state") == "idle" and status().get("last_result") == "failed", 15, 0.3)
        time.sleep(3)
        harness.check("update (retries): an HTTP answer that is not 200 is final: one request, no more attempts", r and Catalog.hits.get("/latest") == 1, (Catalog.hits, status()))
    finally:
        finish(ctx)
    os.environ["PS5TM_UPDATER_RETRY_PAUSE"] = "2"
    ctx = prepare(root, None, loader_port)
    try:
        Catalog.drop = {sha_path: 1}
        status()
        r = wait(state_is("available"), 25, 0.3)
        harness.check("update (retries): the request for the .sha256 of the running version is made again too", r and Catalog.hits.get(sha_path) == 2, (Catalog.hits, status()))
    finally:
        finish(ctx)


def run_setting(root, loader_port):
    """The setting 'Check for updates automatically', the saved time of the last check, and the once-a-day limit across a restart."""
    ctx = prepare(root, None, loader_port, latest="0.4")
    cfg, state_file = "/data/Transmission/config.cfg", "/data/Transmission/update_state.txt"
    up = lambda: wait(lambda: api("GET", "/api/state")[0] == 200, 40, 0.3)
    try:
        c, t = api("GET", "/api/state")
        harness.check("update (setting): update_check is on by default and is in the state", json.loads(t)["config"].get("update_check") is True, t[:200])
        c, t = api("POST", "/api/config", {"update_check": False})
        harness.check("update (setting): it can be switched off, config.cfg says so", c == 200 and json.loads(t).get("ok") and "update_check=no" in open(cfg).read(), t)
        for _ in range(3):
            status(); time.sleep(0.3)
        j = status()
        harness.check("update (setting off): page loads do not ask GitHub; the status says disabled and never checked", Catalog.hits.get("/latest", 0) == 0 and j.get("state") == "idle" and j.get("enabled") is False and j.get("last_check") == 0, (Catalog.hits, j))
        c, t = api("POST", "/api/update", {"action": "check"})
        r = wait(lambda: status().get("state") == "idle" and status().get("last_check", 0) > 0, 10, 0.3)
        j = status()
        harness.check("update (setting off): the Check now button still asks, the answer and its time are remembered", c == 200 and r and Catalog.hits.get("/latest") == 1 and j.get("last_result") == "latest", (t, Catalog.hits, j))
        saved = open(state_file).read() if os.path.exists(state_file) else ""
        harness.check("update (setting off): the state file has the time and the result", ("last_check=%d" % j.get("last_check", -1)) in saved and "last_result=latest" in saved, saved)
        first = j.get("last_check", 0)

        # a restart: the time survives, the setting too; with it on again a settled answer under a day old is not asked again
        kill_all(); ctx["mgr"] = start_manager("updater"); up()
        j = status()
        harness.check("update (restart): the time of the last check and the switched-off setting survive", j.get("last_check") == first and j.get("enabled") is False, j)
        api("POST", "/api/config", {"update_check": True})
        for _ in range(3):
            status(); time.sleep(0.3)
        j = status()
        harness.check("update (limit): switched on again, an answer under a day old is not asked again", j.get("last_check") == first and j.get("enabled") is True and Catalog.hits.get("/latest") == 1 and j.get("state") == "idle", (Catalog.hits, j))

        # more than a day later the next status request starts a check
        kill_all()
        open(state_file, "w").write("last_check=%d\nlast_result=latest\n" % (first - 2 * 86400))
        ctx["mgr"] = start_manager("updater"); up()
        status()
        r = wait(lambda: Catalog.hits.get("/latest", 0) == 2 and status().get("state") == "idle", 10, 0.3)
        harness.check("update (limit): more than a day later the first status request checks again and the time moves on", r and status().get("last_check", 0) >= first, (Catalog.hits, status()))
    finally:
        finish(ctx)


if __name__ == "__main__":
    harness.SYS_HELPER = False
    run_case("/data/pldmgr/payloads/Transmission", None, 19021)
    run_case("/data/pldmgr/payloads/local/transmission", "local", 19022)
    run_bad_sha("/data/pldmgr/payloads/Transmission", 19023)
    run_no_update("/data/pldmgr/payloads/Transmission", 19024)
    run_setting("/data/pldmgr/payloads/Transmission", 19025)
    run_retries("/data/pldmgr/payloads/Transmission", 19026)
    sys.exit(harness.summary())
