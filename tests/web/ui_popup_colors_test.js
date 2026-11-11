const puppeteer = require("puppeteer-core");
let pass = 0, fail = 0; const check = (n, ok, e) => { (ok ? pass++ : fail++); console.log((ok ? "PASS " : "FAIL ") + n + (e !== undefined ? "  -> " + String(e).slice(0, 280) : "")); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// The colors of the pop-ups of the settings page: blue = information, help and pickers; red = a warning or a confirmation of an important operation
// (and an error); green = the update (red when it failed). Real Chrome, the real functions of the page open the windows.
const BLUE = "rgb(47, 111, 237)", RED = "rgb(192, 57, 43)", GREEN = "rgb(46, 158, 91)";
const BLUE_H = "rgb(143, 183, 255)", RED_H = "rgb(255, 107, 107)", GREEN_H = "rgb(95, 214, 138)";
(async () => {
  const browser = await puppeteer.launch({ executablePath: require("./chrome"), args: ["--no-sandbox", "--disable-gpu"], headless: "new" });
  const page = await browser.newPage(); await page.setViewport({ width: 900, height: 1100 }); const errors = []; page.on("pageerror", (e) => errors.push(e.message));
  await page.goto("http://127.0.0.1:9092/", { waitUntil: "networkidle2" }); await sleep(2600);
  const look = (id) => page.evaluate((i) => { const c = document.querySelector("#" + i + " .modal-card"), h = c.querySelector("h3"); return { b: getComputedStyle(c).borderTopColor, h: getComputedStyle(h).color }; }, id);
  const is = async (id, border, head) => { const l = await look(id); return l.b === border && l.h === head ? true : JSON.stringify(l); };
  const run = (code) => page.evaluate(code);
  const same = async (name, id, border, head) => { const r = await is(id, border, head); check(name, r === true, r === true ? undefined : r); };

  // the ones that ask for a confirmation or warn: red
  await same("the exit confirmation is RED", "quitModal", RED, RED_H);
  await run("actOpen()"); await same("the 'Activate' warning (sleep, power off) is RED", "actModal", RED, RED_H); await run("document.getElementById('actCancel').click()");
  await same("'Remove from the list' is RED", "fdDelModal", RED, RED_H);
  await same("'Unmount' is RED", "fdUmModal", RED, RED_H);
  await run("cfmAsk('Stop Transmission?','x','Stop',function(){})"); await same("a confirmation of an important operation (cfmAsk) is RED", "cfmModal", RED, RED_H); await run("document.getElementById('cfmCancel').click()");
  await run("cfmAsk('Remove the icon?','x','Remove',function(){},false)"); await same("a harmless question (remove the icon) is BLUE", "cfmModal", BLUE, BLUE_H); await run("document.getElementById('cfmCancel').click()");
  await run("cfmInfo('Failed','x')"); await same("an error window (cfmInfo) is RED", "cfmModal", RED, RED_H); await run("document.getElementById('cfmOk').click()");
  await sleep(400);

  // help and the information of the folder list: blue; the confirmation to add a folder on a bad drive: red
  await run("helpOpen('When all downloads finish',['text'])"); await same("the text of a '?' button is BLUE", "fdModal", BLUE, BLUE_H);
  await run("document.getElementById('fdCancel').click()"); await sleep(400);
  await run("fdWarn('int','',false)"); await same("the '?' next to an internal drive in the list is BLUE", "fdModal", BLUE, BLUE_H);
  await run("document.getElementById('fdCancel').click()"); await sleep(400);
  await run("fdWarn('es','ufs',false)"); await same("the '?' next to the extended storage is BLUE", "fdModal", BLUE, BLUE_H);
  await run("document.getElementById('fdCancel').click()"); await sleep(400);
  await run("fdWarn('fs','exFAT',true)"); await same("'Add the folder on an exFAT drive anyway?' is RED", "fdModal", RED, RED_H);
  await run("document.getElementById('fdCancel').click()"); await sleep(400);
  await run("fdWarn('int','',true)"); await same("'Add the folder on the internal drive anyway?' is RED", "fdModal", RED, RED_H);
  await run("document.getElementById('fdCancel').click()"); await sleep(400);
  await run("helpOpen('When all downloads finish',['text'])"); await same("a help window after a red warning is BLUE again (the color is set at every opening)", "fdModal", BLUE, BLUE_H);
  await run("document.getElementById('fdCancel').click()"); await sleep(400);

  // the folder picker: blue
  await run("pkOpen()"); await sleep(900);
  const pk = await page.evaluate(() => { const b = document.querySelector(".ps5-picker-box"); if (!b) return null; const h = b.querySelector("h3"); return { b: getComputedStyle(b).borderTopColor, h: h ? getComputedStyle(h).color : "" }; });
  check("the folder picker ('Choose a download folder') is BLUE", pk && pk.b === BLUE && pk.h === BLUE_H, JSON.stringify(pk));
  await page.evaluate(() => { const c = [...document.querySelectorAll(".ps5-picker-box button")].find((x) => /Cancel|Отмена/.test(x.textContent)); if (c) c.click(); }); await sleep(400);

  // the exit progress: blue while closing, red when it is still running
  await run("quitShow('Transmission is closing','',false)"); await same("the closing window is BLUE", "quitDone", BLUE, BLUE_H);
  await run("quitShow('Transmission is still running','x',true)"); await same("'Transmission is still running' is RED", "quitDone", RED, RED_H);
  await run("quitShow('Transmission has closed','')"); await same("'Transmission has closed' is BLUE again", "quitDone", BLUE, BLUE_H);

  // the update: green, red when it failed
  await same("the update window is GREEN", "updModal", GREEN, GREEN_H);
  await run("updView({state:'error',error:'x'})"); await same("'Update failed' is RED", "updModal", RED, RED_H);
  await run("updView({state:'ready'})"); await same("'Update ready' is GREEN again", "updModal", GREEN, GREEN_H);
  await run("updView({state:'downloading',progress:10})"); await same("'Updating…' is GREEN", "updModal", GREEN, GREEN_H);

  check("no JS errors", errors.length === 0, errors.join(";"));
  console.log(`\n${pass}/${pass + fail} pop-up color checks passed`); await browser.close(); process.exit(fail ? 1 : 0);
})().catch((e) => { console.log("crashed", e); process.exit(2); });
