// The release update on the settings page: the manager's /api/update is stubbed. The dialog never opens by itself: it opens from the link "доступно обновление N"
// in the header or from the same words in the "Last check" row of the Settings card; it is green; the offer shows the version and "What's new", the progress and
// "ready" views do not; an old error is not shown on a fresh page, a failed download offers "Try again"; once the update is ready the header link ("перезапустить
// для обновления до N") and the row button ("Перезапустить") open the dialog, Restart is pressed there once and a failed restart shows its reason and works again.
// With nothing to install there is no link in the header (the page does not ask GitHub itself).
// Real Chrome, real settings page.
const puppeteer = require("puppeteer-core");
let pass = 0, fail = 0; const check = (n, ok, e) => { (ok ? pass++ : fail++); console.log((ok ? "PASS " : "FAIL ") + n + (e !== undefined ? "  -> " + String(e).slice(0, 320) : "")); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const browser = await puppeteer.launch({ executablePath: require("./chrome"), args: ["--no-sandbox", "--disable-gpu"], headless: "new" });
  const page = await browser.newPage(); await page.setViewport({ width: 900, height: 1100 }); const errors = []; page.on("pageerror", (e) => errors.push(e.message));
  let state = "available", asked = 0, failDownload = false, restartError = ""; const posts = [];
  let slowRestart = false, stepped = false, checkPolls = 0, nextState = "idle", nextResult = "latest", lastCheck = 0, lastResult = "";   // "Check now": checking for a few polls, then nextState
  const body = (s) => ({ state: s, current: "0.4", latest: "0.5", changelog: "https://example.test/releases/tag/0.5", progress: s === "downloading" ? 42 : s === "ready" ? 100 : 0, can_update: s === "available",
                         error: s === "error" ? "download failed: boom" : "", restarting: false, restart_error: s === "ready" ? restartError : "", enabled: true, last_check: lastCheck, last_result: lastResult });
  await page.setRequestInterception(true);
  page.on("request", (r) => {
    if (r.method() === "GET" && /\/api\/update$/.test(r.url())) {
      asked++;
      if (state === "checking" && --checkPolls < 0) { state = nextState; lastCheck = 1731330000; lastResult = nextResult; }
      r.respond({ status: 200, contentType: "application/json", body: JSON.stringify(body(state)) });
    } else if (r.method() === "POST" && /\/api\/update$/.test(r.url())) {
      const action = JSON.parse(r.postData() || "{}").action; posts.push(action);
      if (action === "download") state = failDownload ? "error" : "downloading";
      if (action === "check") { if (stepped) { state = "checking"; checkPolls = 2; } else state = "available"; }
      if (action === "restart") restartError = slowRestart ? "" : "the ELF loader is not listening";
      r.respond({ status: 200, contentType: "application/json", body: '{"ok":true}' });
    } else r.continue();
  });
  const dlg = () => page.evaluate(() => { const d = (id) => getComputedStyle(document.getElementById(id)).display, el = (id) => document.getElementById(id) || { textContent: "", className: "", disabled: false };
    return { modal: d("updModal"), title: el("updTitle").textContent, status: el("updStatus").textContent, offer: d("updOffer"), go: d("updGo"), retry: d("updRetry"), restart: d("updRestart"), restartOff: el("updRestart").disabled,
             progress: d("updProgressRow"), width: el("updBar").style.width, ver: el("updVer").textContent, notes: el("updNotes").textContent, href: el("updNotes").href,
             head: (document.querySelector("#top a") || {}).textContent || "", rowLink: el("updLink").textContent, line: el("updLast").textContent, nowText: el("updNow").textContent, nowClass: el("updNow").className, nowOff: el("updNow").disabled,
             border: getComputedStyle(document.querySelector("#updModal .modal-card")).borderTopColor, titleColor: getComputedStyle(document.getElementById("updTitle")).color }; });
  const load = async () => { await page.goto("http://127.0.0.1:9092/", { waitUntil: "networkidle2" }); await sleep(3600); };

  // ---- a newer release: no dialog by itself, two links, the offer, the progress, "ready"
  await load();
  const quiet = await dlg();
  check("a newer release does NOT open the dialog by itself; the header has the link 'доступно обновление 0.5' and the 'Last check' row has the link 'Доступно обновление 0.5'", quiet.modal === "none" && quiet.head === "доступно обновление 0.5" && quiet.rowLink === "Доступно обновление 0.5", JSON.stringify(quiet));
  await page.click("#top a"); await sleep(500);
  const offer = await dlg();
  check("the header link opens the offer: title, 'Доступна новая версия: 0.5', 'Что нового' with the release link, Update, no Restart", offer.modal === "flex" && offer.title === "Доступно обновление" && offer.offer !== "none" && offer.ver === "0.5" && offer.notes === "Что нового" && /\/tag\/0\.5$/.test(offer.href) && offer.go !== "none" && offer.restart === "none" && offer.retry === "none" && offer.progress === "none", JSON.stringify(offer));
  const rgb = (c) => (c.match(/\d+/g) || []).map(Number);
  check("the dialog is green, not red (border and title)", (() => { const b = rgb(offer.border), t = rgb(offer.titleColor); return b[1] > b[0] && b[1] > b[2] && t[1] > t[0] && t[1] > t[2]; })(), JSON.stringify({ border: offer.border, title: offer.titleColor }));
  await page.click("#updLater"); await sleep(300);
  check("Later closes the dialog", (await dlg()).modal === "none");
  await page.evaluate(() => document.getElementById("updLink").click()); await sleep(400);
  check("the link in the row opens it again", (await dlg()).modal === "flex");
  await page.click("#updGo"); await sleep(900);
  const progress = await dlg();
  check("Update: the title is 'Обновляю…', the version and 'Что нового' are gone, the progress and its text are shown, only Later is left", progress.title === "Обновляю…" && progress.offer === "none" && progress.progress !== "none" && progress.width === "42%" && /Скачивание обновления/.test(progress.status) && progress.go === "none" && progress.restart === "none", JSON.stringify(progress));
  check("during the download the left button says 'Скрыть' (the download goes on), not 'Позже'", await page.evaluate(() => document.getElementById("updLater").textContent) === "Скрыть");
  await page.click("#updLater"); state = "downloading"; await sleep(1500);
  check("closed during the download, the dialog does not come back by itself", (await dlg()).modal === "none");
  state = "ready"; await sleep(1800);
  const closedReady = await dlg();
  check("when the update is ready the dialog still stays closed, the header link becomes 'перезапустить для обновления до 0.5' and the row button the blue 'Перезапустить' (no 'alt' class)", closedReady.modal === "none" && closedReady.head === "перезапустить для обновления до 0.5" && closedReady.nowText === "Перезапустить" && !/\balt\b/.test(closedReady.nowClass) && /Перезапустите/.test(closedReady.line), JSON.stringify(closedReady));

  // ---- the dialog kept open while the download ends turns into "ready"
  state = "available"; await load();
  await page.click("#top a"); await sleep(400); await page.click("#updGo"); await sleep(900); state = "ready"; await sleep(1500);
  const ready = await dlg();
  check("kept open, the dialog turns into 'Обновление готово' without the version and 'Что нового', with Restart", ready.modal === "flex" && ready.title === "Обновление готово" && ready.offer === "none" && ready.progress === "none" && /Перезапустите/.test(ready.status) && ready.restart !== "none" && ready.go === "none", JSON.stringify(ready));
  check("the page made more than one status request and has no JS errors", asked >= 2 && errors.length === 0, JSON.stringify({ asked, errors }));
  await page.click("#updLater"); await sleep(300);

  // ---- an old error from an earlier attempt is not shown on a fresh page
  state = "error"; await load();
  const old = await dlg();
  check("an error that is already there when the page opens does not open the dialog", old.modal === "none", JSON.stringify(old));

  // ---- a failed download: the reason and "Try again", which asks for a new check and returns to the offer
  state = "available"; failDownload = true; posts.length = 0; await load();
  await page.click("#top a"); await sleep(400); await page.click("#updGo"); await sleep(1300);
  const failed = await dlg();
  check("a failed download shows 'Не удалось обновить' with its reason and a Try again button (no Update, no Restart)", failed.modal === "flex" && failed.title === "Не удалось обновить" && /boom/.test(failed.status) && failed.retry !== "none" && failed.go === "none" && failed.restart === "none", JSON.stringify(failed));
  await page.click("#updRetry"); await sleep(2200);
  const again = await dlg();
  check("Try again asks the manager for a new check, and the dialog offers the update again", posts.includes("check") && again.modal === "flex" && again.title === "Доступно обновление" && again.offer !== "none" && again.go !== "none" && again.retry === "none", JSON.stringify({ posts, again }));
  failDownload = false;

  // ---- Restart: the header link and the row button only open the dialog; Restart is pressed there (two quick presses send one request, a failure shows its reason)
  state = "ready"; restartError = ""; posts.length = 0; await load();
  const rd = await dlg();
  check("with a ready update the page opens without the dialog; the header link says 'перезапустить для обновления до 0.5' and the row button 'Перезапустить'", rd.modal === "none" && rd.head === "перезапустить для обновления до 0.5" && rd.nowText === "Перезапустить", JSON.stringify(rd));
  await page.click("#top a"); await sleep(500);
  const opened = await dlg();
  check("the header link does NOT restart: it opens 'Обновление готово' with the Restart button, and nothing is sent", opened.modal === "flex" && opened.title === "Обновление готово" && opened.restart !== "none" && posts.length === 0, JSON.stringify({ opened, posts }));
  await page.evaluate(() => { const b = document.getElementById("updRestart"); b.click(); b.click(); });
  await sleep(2600);
  const failedRestart = await dlg();
  check("two quick presses of Restart in the dialog send one restart request", posts.filter((a) => a === "restart").length === 1, JSON.stringify(posts));
  check("a failed restart shows the real reason (not 'timed out') in the dialog and in the row, the header link is back and the buttons work again", failedRestart.modal === "flex" && failedRestart.status === "the ELF loader is not listening" && failedRestart.line === "the ELF loader is not listening" && failedRestart.head === "перезапустить для обновления до 0.5" && failedRestart.restartOff === false && failedRestart.nowOff === false, JSON.stringify(failedRestart));
  await page.click("#updRestart"); await sleep(1500);
  check("Restart in the dialog sends a new request", posts.filter((a) => a === "restart").length === 2, JSON.stringify(posts));
  await page.click("#updLater"); await sleep(300);
  await page.evaluate(() => document.getElementById("updNow").click()); await sleep(600);
  check("the row button 'Перезапустить' does not restart either: it opens the dialog", (await dlg()).modal === "flex" && posts.filter((a) => a === "restart").length === 2, JSON.stringify(posts));
  slowRestart = true; restartError = ""; await page.evaluate(() => { const b = document.getElementById("updNow"); if (getComputedStyle(document.getElementById("updModal")).display === "none") b.click(); }); await sleep(500);
  check("before the restart the left button says 'Позже' again", await page.evaluate(() => document.getElementById("updLater").textContent) === "Позже");
  await page.click("#updRestart"); await sleep(700);
  const rs = await page.evaluate(() => ({ later: getComputedStyle(document.getElementById("updLater")).display, restart: document.getElementById("updRestart").disabled, status: document.getElementById("updStatus").textContent }));
  check("after Restart is pressed the 'Позже' button is hidden and Restart is off while the restart goes on", rs.later === "none" && rs.restart === true && /перезапускается/i.test(rs.status), JSON.stringify(rs));
  check("no JS errors in the failure paths", errors.length === 0, errors.join(";"));
  slowRestart = false;   // the dialog stays in the middle of a restart (Позже is hidden): the next part loads the page anew

  // ---- the Updates group of the Settings card: the checkbox, the "Last check" line and the Check now button
  stepped = true; state = "idle"; lastCheck = 0; lastResult = ""; nextState = "idle"; nextResult = "failed"; posts.length = 0; await load();
  const row = () => page.evaluate(() => ({ line: document.getElementById("updLast").textContent, off: document.getElementById("updNow").disabled, btn: document.getElementById("updNow").textContent,
    label: document.getElementById("f_row_update_check").querySelector("label").firstChild.textContent, box: document.getElementById("f_update_check").checked, head: [...document.querySelectorAll("#form .help")].some((x) => x.textContent === "Обновления"),
    modal: getComputedStyle(document.getElementById("updModal")).display, rowLink: (document.getElementById("updLink") || {}).textContent || "", top: (document.querySelector("#top a") || {}).textContent || "" }));
  const r0 = await row();
  check("Settings has 'Обновления': the checkbox 'Проверять обновления автоматически' (on by default), 'Ещё не проверялось' and an enabled 'Проверить сейчас'", r0.head && r0.label === "Проверять обновления автоматически" && r0.box === true && r0.line === "Ещё не проверялось" && r0.btn === "Проверить сейчас" && r0.off === false && r0.top === "", JSON.stringify(r0));
  await page.evaluate(() => document.getElementById("updNow").click()); await sleep(400);
  const r1 = await row();
  check("Check now sends a check, shows 'Проверка…' and disables the button meanwhile", posts.includes("check") && r1.line === "Проверка…" && r1.off === true, JSON.stringify({ posts, r1 }));
  await sleep(4300);
  const r2 = await row(), parts = r2.line.split(" · ");
  check("when the check ends the line says when (a date in the language of the page, no seconds) and how: 'Не удалось проверить'; the button works again", parts.length === 2 && /^\d{2}\.\d{2}\.\d{4}, \d{2}:\d{2}$/.test(parts[0]) && parts[1] === "Не удалось проверить" && r2.off === false, JSON.stringify(r2));
  nextState = "available"; nextResult = "available"; posts.length = 0;
  await page.evaluate(() => document.getElementById("updNow").click()); await sleep(4800);
  const r3 = await row();
  check("a check that finds a new version does not open the dialog: the line ends with the link 'Доступно обновление 0.5' and the header gets 'доступно обновление 0.5'", r3.modal === "none" && r3.rowLink === "Доступно обновление 0.5" && r3.line.startsWith(parts[0]) && r3.top === "доступно обновление 0.5", JSON.stringify(r3));
  // the spacing: a control that is taller than its text (the theme select has no description, the button) keeps the same distance from the line above as the other rows
  await page.evaluate(() => document.getElementById("accSettingsBtn").click()); await sleep(700);
  const gaps = await page.evaluate(() => { const g = (row, sel) => { const r = document.getElementById(row), c = r.querySelector(sel); return c.getBoundingClientRect().top - r.getBoundingClientRect().top; }; return { icons: g("f_row_web_icons", "select"), theme: g("f_row_web_theme", "select"), button: g("f_row_update_now", "button") }; });
  check("the theme select and the Check now button are as far from the line above as the other selects (within 2 px)", gaps.icons > 8 && Math.abs(gaps.theme - gaps.icons) <= 2 && Math.abs(gaps.button - gaps.icons) <= 2, JSON.stringify(gaps));
  check("no JS errors in the Updates row", errors.length === 0, errors.join(";"));
  await browser.close(); console.log(`\n${pass}/${pass + fail} update dialog checks passed`); process.exit(fail ? 1 : 0);
})().catch((e) => { console.log("crashed", e); process.exit(2); });
