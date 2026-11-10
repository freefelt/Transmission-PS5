// The release update dialog: the manager's /api/update is stubbed, the page must show the version and the release link,
// start the download, show progress and then offer Restart; an old error is not shown on a fresh page, a failed download
// offers "Try again", Restart is pressed once and a failed restart shows its reason and works again. Real Chrome, real settings page.
const puppeteer = require("puppeteer-core");
let pass = 0, fail = 0; const check = (n, ok, e) => { (ok ? pass++ : fail++); console.log((ok ? "PASS " : "FAIL ") + n + (e !== undefined ? "  -> " + String(e).slice(0, 300) : "")); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const browser = await puppeteer.launch({ executablePath: require("./chrome"), args: ["--no-sandbox", "--disable-gpu"], headless: "new" });
  const page = await browser.newPage(); await page.setViewport({ width: 900, height: 1100 }); const errors = []; page.on("pageerror", (e) => errors.push(e.message));
  let state = "available", asked = 0, failDownload = false, restartError = ""; const posts = [];
  const body = (s) => ({ state: s, current: "0.4", latest: "0.5", changelog: "https://example.test/releases/tag/0.5", progress: s === "downloading" ? 42 : s === "ready" ? 100 : 0, can_update: s === "available",
                         error: s === "error" ? "download failed: boom" : "", restarting: false, restart_error: s === "ready" ? restartError : "" });
  await page.setRequestInterception(true);
  page.on("request", (r) => {
    if (r.method() === "GET" && /\/api\/update$/.test(r.url())) {
      asked++;
      r.respond({ status: 200, contentType: "application/json", body: JSON.stringify(body(state)) });
    } else if (r.method() === "POST" && /\/api\/update$/.test(r.url())) {
      const action = JSON.parse(r.postData() || "{}").action; posts.push(action);
      if (action === "download") state = failDownload ? "error" : "downloading";
      if (action === "check") state = "available";
      if (action === "restart") restartError = "the ELF loader is not listening";
      r.respond({ status: 200, contentType: "application/json", body: '{"ok":true}' });
    } else r.continue();
  });
  const dlg = () => page.evaluate(() => { const d = (id) => getComputedStyle(document.getElementById(id)).display; return { modal: d("updModal"), title: document.getElementById("updTitle").textContent, status: document.getElementById("updStatus").textContent, go: d("updGo"), retry: d("updRetry"), restart: d("updRestart"), restartOff: document.getElementById("updRestart").disabled, progress: d("updProgressRow"), width: document.getElementById("updBar").style.width, ver: document.getElementById("updVer").textContent, notes: document.getElementById("updNotes").textContent, href: document.getElementById("updNotes").href }; });
  const load = async () => { await page.goto("http://127.0.0.1:9092/", { waitUntil: "networkidle2" }); await sleep(3600); };

  // ---- a newer release: offer, download with progress, ready
  await load();
  const offer = await dlg();
  check("a newer release opens the dialog with its version, the release link and Update", offer.modal === "flex" && offer.title === "Доступно обновление" && offer.ver === "0.5" && offer.notes === "Что нового" && /\/tag\/0\.5$/.test(offer.href) && offer.go !== "none" && offer.restart === "none" && offer.retry === "none", JSON.stringify(offer));
  await page.click("#updGo"); await sleep(900);
  const progress = await dlg();
  check("Update starts the download: progress is visible and Update is hidden", progress.progress !== "none" && progress.width === "42%" && /Скачивание обновления/.test(progress.status) && progress.go === "none", JSON.stringify(progress));
  state = "ready"; await sleep(1200);
  const ready = await dlg();
  check("when the download is ready the dialog offers Restart and hides the progress", ready.title === "Обновление готово" && /Перезапустите/.test(ready.status) && ready.restart !== "none" && ready.progress === "none", JSON.stringify(ready));
  check("the page made more than one status request and has no JS errors", asked >= 2 && errors.length === 0, JSON.stringify({ asked, errors }));
  await page.click("#updLater"); await sleep(300);
  check("Later closes the dialog", (await dlg()).modal === "none");

  // ---- an old error from an earlier attempt is not shown on a fresh page
  state = "error"; await load();
  const old = await dlg();
  check("an error that is already there when the page opens does not open the dialog", old.modal === "none", JSON.stringify(old));

  // ---- a failed download: the reason and "Try again", which asks for a new check and returns to the offer
  state = "available"; failDownload = true; posts.length = 0; await load();
  await page.click("#updGo"); await sleep(1300);
  const failed = await dlg();
  check("a failed download opens 'Не удалось обновить' with its reason and a Try again button (no Update, no Restart)", failed.modal === "flex" && failed.title === "Не удалось обновить" && /boom/.test(failed.status) && failed.retry !== "none" && failed.go === "none" && failed.restart === "none", JSON.stringify(failed));
  await page.click("#updRetry"); await sleep(2200);
  const again = await dlg();
  check("Try again asks the manager for a new check, and the dialog offers the update again", posts.includes("check") && again.modal === "flex" && again.title === "Доступно обновление" && again.go !== "none" && again.retry === "none", JSON.stringify({ posts, again }));
  failDownload = false;

  // ---- Restart: one press sends one request, a failed restart shows its reason and the button works again
  state = "ready"; restartError = ""; posts.length = 0; await load();
  await page.evaluate(() => { const b = document.getElementById("updRestart"); b.click(); b.click(); });
  await sleep(2600);
  const failedRestart = await dlg();
  check("two quick presses of Restart send one request", posts.filter((a) => a === "restart").length === 1, JSON.stringify(posts));
  check("a failed restart shows the real reason (not 'timed out') and the button is enabled again", failedRestart.status === "the ELF loader is not listening" && failedRestart.restart !== "none" && failedRestart.restartOff === false, JSON.stringify(failedRestart));
  await page.click("#updRestart"); await sleep(1500);
  check("pressing Restart again sends a new request", posts.filter((a) => a === "restart").length === 2, JSON.stringify(posts));
  check("no JS errors in the failure paths", errors.length === 0, errors.join(";"));
  await browser.close(); console.log(`\n${pass}/${pass + fail} update dialog checks passed`); process.exit(fail ? 1 : 0);
})().catch((e) => { console.log("crashed", e); process.exit(2); });
