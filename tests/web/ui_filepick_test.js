const puppeteer = require("puppeteer-core");
const fs = require("fs");
let pass = 0, fail = 0; const check = (n, ok, e) => { (ok ? pass++ : fail++); console.log((ok ? "PASS " : "FAIL ") + n + (e !== undefined ? "  -> " + String(e).slice(0, 260) : "")); };
const PS5_UA = "Mozilla/5.0 (PlayStation; PlayStation 5/11.20) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.4 Safari/605.1.15";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const PID = process.pid, NAME = "Filepick_" + PID + ".bin", DIR = "/mnt/ext1/torrents", FILE = DIR + "/pick_" + PID + ".torrent", TXT = DIR + "/not_a_torrent_" + PID + ".txt";
function torrent(name) {
  const bstr = (s) => Buffer.concat([Buffer.from(String(Buffer.byteLength(s)) + ":"), Buffer.from(s)]);
  const info = Buffer.concat([Buffer.from("d6:lengthi65536e4:name"), bstr(name), Buffer.from("12:piece lengthi65536e6:pieces20:"), Buffer.alloc(20, 7), Buffer.from("e")]);
  return Buffer.concat([Buffer.from("d8:announce27:http://127.0.0.1:1/announce4:info"), info, Buffer.from("e")]);
}
fs.mkdirSync(DIR, { recursive: true });
fs.writeFileSync(FILE, torrent(NAME));
fs.writeFileSync(TXT, "not a torrent");
// The console browser has no working file chooser: the Add Torrents dialog opens our picker, the file is added by its path.
(async () => {
  const browser = await puppeteer.launch({ executablePath: require("./chrome"), args: ["--no-sandbox", "--disable-gpu"], headless: "new" });
  const adds = [];
  const open = async (ua) => {
    const page = await browser.newPage(); await page.setViewport({ width: 1280, height: 900 });
    if (ua) await page.setUserAgent(ua);
    const errors = []; page.on("pageerror", (e) => errors.push(e.message));
    page.on("request", (r) => { const b = r.postData(); if (r.method() === "POST" && b && /torrent[_-]add/.test(b)) { try { adds.push(JSON.parse(b)); } catch (e) {} } });
    await page.goto("http://127.0.0.1:9091/transmission/web/", { waitUntil: "networkidle2" }); await sleep(2500);
    return { page, errors };
  };
  const rpc = (page, m, p) => page.evaluate(async (m, p) => { let sid = ""; for (let i = 0; i < 3; i++) { const r = await fetch("../rpc", { method: "POST", headers: { "X-Transmission-Session-Id": sid, "Content-Type": "application/json" }, body: JSON.stringify({ jsonrpc: "2.0", method: m, params: p || {}, id: 1 }) }); if (r.status === 409) { sid = r.headers.get("X-Transmission-Session-Id"); continue; } return await r.json(); } }, m, p);
  // ---- normal browser: no console button; our own button and line stand in for the native file control
  let { page, errors } = await open();
  await page.click("#toolbar-open"); await sleep(700);
  let f = await page.evaluate(() => { const i = document.querySelector('.open-torrent input[type="file"]'); return { btn: !!document.getElementById("ps5-file-btn"), input: i ? (getComputedStyle(i).display === "none" || getComputedStyle(i).opacity === "0" ? "hidden" : "visible") : "missing", pick: !!document.getElementById("ps5-pick-row"), names: (document.getElementById("ps5-pick-names") || {}).textContent || "" }; });
  check("a computer: no console button; the native file control (its 'Browse...' and 'No files selected.' are drawn by the browser in ITS language) is hidden and our own button and line stand in for it", !f.btn && f.input === "hidden" && f.pick && f.names === "No files selected.", JSON.stringify(f));
  check("no JS errors in a normal browser", errors.length === 0, errors.join(";"));
  await page.close();

  // ---- the console browser
  ({ page, errors } = await open(PS5_UA));
  await page.click("#toolbar-open"); await sleep(800);
  f = await page.evaluate(() => { const b = document.getElementById("ps5-file-btn"); const i = document.querySelector('.open-torrent input[type="file"]'); return { btn: !!b, shown: !!b && getComputedStyle(b).display !== "none" && b.getBoundingClientRect().width > 0, input: i ? getComputedStyle(i).display : "missing", hint: (document.querySelector("#ps5-file-row .ps5-file-hint") || {}).textContent || "" }; });
  check("the console: our button is shown in the Add dialog, the native chooser is hidden, and the hint explains where the file is", f.btn && f.shown && f.input === "none" && f.hint.length > 0, JSON.stringify(f));

  // the picker: the disks, the .torrent filter, the choice
  await page.evaluate(() => document.getElementById("ps5-file-btn").click()); await sleep(1000);
  const p = await page.evaluate(() => { const ov = document.getElementById("ps5-file-picker"); return { vis: ov ? getComputedStyle(ov).display : "missing", disks: [...document.querySelectorAll("#pkDisk option")].map((o) => o.value), rows: [...document.querySelectorAll("#pkList .pk-row")].map((r) => r.textContent) }; });
  check("the picker opens on the M.2 SSD with the same disk list as the folder picker (/data is there, no /)", p.vis === "flex" && p.disks.indexOf("/mnt/ext1") >= 0 && p.disks.indexOf("/data") >= 0 && p.disks.indexOf("/") < 0, JSON.stringify(p));
  await page.evaluate(() => { const r = [...document.querySelectorAll("#pkList .pk-row")].find((x) => /torrents/.test(x.textContent)); if (r) r.click(); }); await sleep(900);
  let rows = await page.evaluate(() => [...document.querySelectorAll("#pkList .pk-file-row")].map((r) => r.textContent));
  check("the folder shows the .torrent files only (our .txt file is not listed)", rows.some((r) => r.indexOf("pick_" + PID + ".torrent") >= 0) && !rows.some((r) => r.indexOf("not_a_torrent") >= 0), JSON.stringify(rows));
  await page.evaluate((n) => { const r = [...document.querySelectorAll("#pkList .pk-file-row")].find((x) => x.textContent.indexOf(n) >= 0); r.click(); }, "pick_" + PID + ".torrent"); await sleep(300);
  await page.evaluate(() => document.getElementById("pkChoose").click()); await sleep(400);
  const chip = await page.evaluate(() => { const c = document.getElementById("ps5-file-chip"); return { vis: c ? getComputedStyle(c).display : "missing", text: c ? c.textContent : "" }; });
  check("the chosen file is shown with its full path and can be removed", chip.vis === "flex" && chip.text.indexOf(FILE) >= 0, JSON.stringify(chip));

  // press Add: the file is added by its path
  await page.evaluate(() => { const t = document.querySelector('.open-torrent .ps5-tile[data-path="/mnt/ext1/torrents"]'); if (t) t.click(); }); await sleep(500);
  await page.evaluate(() => [...document.querySelectorAll(".open-torrent .dialog-buttons button")].pop().click()); await sleep(2500);
  const add = adds[adds.length - 1]; const prm = add && (add.params || add.arguments);
  check("Add sends torrent_add with the path of the file (filename), the folder of the chosen tile and the start checkbox", !!prm && prm.filename === FILE && prm.download_dir === "/mnt/ext1/torrents" && prm.paused === false, JSON.stringify(prm));
  let r = await rpc(page, "torrent_get", { fields: ["name", "download_dir"] });
  let t = (r.result && r.result.torrents || []).find((x) => x.name === NAME);
  check("the daemon added the torrent from the file on the disk (it read the .torrent itself)", !!t && t.download_dir === "/mnt/ext1/torrents", JSON.stringify(t || r));

  // a file that disappears before Add: our error window
  await page.click("#toolbar-open"); await sleep(700);
  await page.evaluate(() => document.getElementById("ps5-file-btn").click()); await sleep(800);
  const pkc = await page.evaluate(() => { const b = document.querySelector("#ps5-file-picker .ps5-modal-box"); return b ? { b: getComputedStyle(b).borderTopColor, h: getComputedStyle(b.querySelector("h3")).color } : null; });
  check("the .torrent file picker is BLUE (not a warning): blue border and title", !!pkc && pkc.b === "rgb(47, 111, 237)" && pkc.h === "rgb(143, 183, 255)", JSON.stringify(pkc));
  await page.evaluate(() => { const r = [...document.querySelectorAll("#pkList .pk-row")].find((x) => /torrents/.test(x.textContent)); if (r) r.click(); }); await sleep(800);
  await page.evaluate((n) => { const r = [...document.querySelectorAll("#pkList .pk-file-row")].find((x) => x.textContent.indexOf(n) >= 0); r.click(); }, "pick_" + PID + ".torrent"); await sleep(300);
  await page.evaluate(() => document.getElementById("pkChoose").click()); await sleep(300);
  fs.unlinkSync(FILE);
  await page.evaluate(() => [...document.querySelectorAll(".open-torrent .dialog-buttons button")].pop().click()); await sleep(2000);
  const err = await page.evaluate(() => { const m = document.querySelector(".ps5-modal:not(.ps5-picker-overlay)"); return { vis: !!m, text: m ? m.textContent : "", border: m ? getComputedStyle(m.querySelector(".ps5-modal-box")).borderTopColor : "" }; });
  check("the error window is RED", err.border === "rgb(192, 57, 43)", err.border);
  check("a .torrent file that disappeared: an error window with the file name is shown (no silent failure)", err.vis && err.text.indexOf("pick_" + PID + ".torrent") >= 0, JSON.stringify(err));

  // Escape closes the picker (the Add dialog was closed by the failed Add: open it again)
  await page.evaluate(() => document.getElementById("toolbar-open").click()); await sleep(700);
  await page.evaluate(() => document.getElementById("ps5-file-btn").click()); await sleep(700);
  await page.keyboard.press("Escape"); await sleep(400);
  const closed = await page.evaluate(() => { const ov = document.getElementById("ps5-file-picker"); return !ov || getComputedStyle(ov).display === "none"; });
  check("Escape closes the picker", closed, closed);
  check("no JS errors in the console browser", errors.length === 0, errors.join(";"));

  // clean up the torrents this test added
  r = await rpc(page, "torrent_get", { fields: ["id", "name"] });
  const ids = ((r.result && r.result.torrents) || []).filter((x) => x.name === NAME).map((x) => x.id);
  if (ids.length) await rpc(page, "torrent_remove", { ids });
  await page.close();
  fs.rmSync(FILE, { force: true }); fs.rmSync(TXT, { force: true });
  console.log(`\n${pass}/${pass + fail} file picker checks passed`); await browser.close(); process.exit(fail ? 1 : 0);
})().catch((e) => { console.log("crashed", e); try { fs.rmSync(FILE, { force: true }); fs.rmSync(TXT, { force: true }); } catch (x) {} process.exit(2); });
