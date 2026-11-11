const { JSDOM } = require("jsdom");
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
let pass = 0, fail = 0; const check = (n, ok, e) => { (ok ? pass++ : fail++); console.log((ok ? "PASS " : "FAIL ") + n + (e !== undefined ? "  -> " + e : "")); };
let confirmText = "";
(async () => {
  const errors = []; const dom = await JSDOM.fromURL("http://127.0.0.1:9092/", { runScripts: "dangerously", resources: "usable", pretendToBeVisual: true, beforeParse(w) { w.PS5_ANIM_MS = 0; w.confirm = (t) => { confirmText = t; return true; }; w.addEventListener("error", (e) => errors.push(String(e.message))); } });
  const d = dom.window.document; const $ = (i) => d.getElementById(i); await wait(2500);
  check("no JS errors; the Transmission card exists", errors.length === 0 && !!$("dmBtn") && !!$("dmState"), errors.join(";"));
  check("state shows the daemon as running with its pid; the button offers to stop it", /работает \(pid: \d+/.test($("dmState").textContent) && $("dmBtn").textContent === "Остановить Transmission", $("dmState").textContent + " | " + $("dmBtn").textContent);
  { let polled = false; for (let k = 0; k < 20 && !polled; k++) { if (/Последний опрос\s*\d+ с назад/.test($("dgStatus").textContent)) polled = true; else await wait(500); }
    check("while Transmission runs the main section does not show the Status row, the manager uptime or the last poll; they are in Diagnostics (daemon with pid and memory, manager uptime, last poll)", $("dmRow").style.display === "none" && !/Менеджер работает|Последний опрос/.test($("status").textContent) && /Transmission\s*работает \(pid: \d+/.test($("dgStatus").textContent) && /Менеджер работает\s*\d+ ч \d+ мин/.test($("dgStatus").textContent) && polled && $("accDiagBody").contains($("dgStatus")), $("dgStatus").textContent.replace(/\s+/g, " ")); }
  check("one merged section 'Transmission' (no separate 'Состояние' section)", [...d.querySelectorAll("h2")].map((h) => h.textContent).indexOf("Состояние") < 0 && $("dmBtn").closest(".card").contains($("status")));
  check("notification form has 10 inputs (incl. the two disk thresholds, the game pause messages checkbox and the Updates checkbox; the disk panel mode is a select now), the expert form 9 (incl. daemon settings, the restart after a network change and log retention)", d.querySelectorAll("#form input").length === 10 && d.querySelectorAll("#expertForm input").length === 9 && !!$("f_daemon_restart_after_seconds") && !!$("f_daemon_stop_grace_seconds"));
  check("old controls are gone (restart-from-file, socket recreation)", !$("f_restart_elf_path") && !$("dgRebind") && !$("dgToggle") && !$("dgRestart"));
  check("the Diagnostics card is one button 'Подготовить отчёт' with a short explanation; nothing of the old build row, buttons or output pane is left", !!$("dgReport") && !$("bdState") && !$("dgOut") && !["dgAll", "dgProbe", "dgThreads", "dgLog", "dgLogFile", "dgDlog", "dgTlog", "dgSelect", "dgDisks", "dgMounts"].some((i) => $(i)) && /отчёт/.test($("dgInfo").textContent));
  let native = 0; dom.window.confirm = () => { native++; return true; };
  $("dmBtn").click(); await wait(500);
  check("clicking 'Stop' asks for confirmation in OUR popup (not the system window) and explains what happens to the torrents", $("cfmModal").style.display === "flex" && /приостановлены/.test($("cfmText").textContent) && /возобновятся/.test($("cfmText").textContent) && native === 0, $("cfmText").textContent.slice(0, 60));
  $("cfmOk").click(); await wait(800);
  check("while stopping the button is disabled and the state says so", $("dmBtn").disabled || /останавливается|остановлен/.test($("dmState").textContent), $("dmState").textContent);
  let ok = false; for (let i = 0; i < 40 && !ok; i++) { await wait(1000); ok = /остановлен вручную/.test($("dmState").textContent); }
  check("after the stop: 'остановлен вручную', the button offers to start, the note explains", ok && $("dmBtn").textContent === "Запустить Transmission" && /сам не запустится/.test($("dmNote").textContent), $("dmState").textContent + " | " + $("dmBtn").textContent);
  check("when Transmission is not running the Status row appears in the main section, and Diagnostics shows the same state with a dash for the last poll", $("dmRow").style.display !== "none" && /остановлен вручную/.test($("dmRow").textContent) && /Transmission\s*остановлен вручную/.test($("dgStatus").textContent) && /Последний опрос\s*-/.test($("dgStatus").textContent), $("dmRow").textContent + " | " + $("dgStatus").textContent.replace(/\s+/g, " "));
  check("the web-UI link is on the right (margin-left:auto)", $("trLink").style.marginLeft === "auto");
  check("the web-UI link is hidden while Transmission is stopped", $("trLink").style.display === "none");
  check("the stale numbers are not shown while Transmission is stopped: both rows hold just a dash (no 'нет данных: …' text, no 'Последний опрос')", /Торренты\s*-/.test($("status").textContent) && /Скорость\s*-/.test($("status").textContent) && !/нет данных/.test($("status").textContent) && !/Последний опрос/.test($("status").textContent) && !/▼/.test($("status").textContent), $("status").textContent.replace(/\s+/g, " ").slice(0, 120));
  $("dmBtn").click(); let sawStarting = false; for (let i = 0; i < 30; i++) { await wait(100); if (/запускается/.test($("dmState").textContent) || $("dmBtn").disabled) sawStarting = true; }
  check("clicking 'Start' shows 'запускается…' (the button is disabled meanwhile); on a fast host the start may finish within a second, so the state is watched, not read once", sawStarting || /работает \(pid/.test($("dmState").textContent), $("dmState").textContent);
  ok = false; for (let i = 0; i < 60 && !ok; i++) { await wait(1000); ok = /работает \(pid/.test($("dmState").textContent); }
  check("the web-UI link is back after the start", $("trLink").style.display !== "none");
  check("the daemon is back: running with a pid, the button offers to stop", ok && $("dmBtn").textContent === "Остановить Transmission" && !$("dmBtn").disabled, $("dmState").textContent);
  check("after the start the Status row of the main section is hidden again", $("dmRow").style.display === "none");
  // the report button: in a browser it downloads /api/report (the click on a link is recorded: jsdom cannot download), in the console's browser it asks the manager to save a file
  const clicks = []; dom.window.HTMLAnchorElement.prototype.click = function () { clicks.push([this.getAttribute("href"), this.hasAttribute("download")]); };
  $("dgReport").click(); await wait(300);
  check("a desktop browser: the button downloads the report (a link to /api/report with the download attribute) and says so; the button is off for a moment", clicks.length === 1 && clicks[0][0] === "/api/report" && clicks[0][1] === true && /Отчёт скачивается/.test($("dgMsg").textContent) && $("dgMsg").style.display !== "none" && $("dgReport").disabled, JSON.stringify(clicks) + " | " + $("dgMsg").textContent);
  await wait(4300);
  check("and it is on again after a few seconds", !$("dgReport").disabled);
  const rep = await (await fetch("http://127.0.0.1:9092/api/report")).text();
  check("the report holds all the sections, incl. the version line with the commit and the daemon log", ["=== version ", "commit ", "=== Transmission check", "=== threads", "=== wrapper log", "=== daemon wrapper log", "=== mount points", "=== settings (config.cfg)", "=== sleep and daemon events"].every((s) => rep.includes(s)), rep.split("\n").slice(0, 4).join(" | "));
  check("the report starts with the version, the SDK and the build time", /=== version [\d.]+, commit \S+, SDK host, built /.test(rep), (rep.match(/=== version .*/) || [""])[0]);
  check("the report has the manager log FILE as well", /=== manager log file/.test(rep) && /\b(daemon|monitor|syssvc):/.test((rep.split("=== manager log file")[1] || "").split("=== sleep and daemon events")[0]));
  Object.defineProperty(dom.window.navigator, "userAgent", { value: "Mozilla/5.0 (PlayStation; PlayStation 5/11.20) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.4 Safari/605.1.15", configurable: true });
  try { require("fs").unlinkSync("/data/Transmission/report.txt"); } catch (e) { }
  clicks.length = 0; $("dgReport").click(); let saved = ""; for (let i = 0; i < 60 && !/report\.txt/.test(saved); i++) { await wait(500); saved = $("dgMsg").textContent; }
  check("the console's browser: nothing is downloaded, the manager saves /data/Transmission/report.txt and the page says where (and to look it through)", clicks.length === 0 && /Отчёт сохранён: \/data\/Transmission\/report\.txt/.test(saved) && /просмотрите/.test(saved) && require("fs").readFileSync("/data/Transmission/report.txt", "utf8").startsWith("# Transmission for the PS5: report"), saved);
  check("the button is on again after the answer", !$("dgReport").disabled);
  console.log(`\n${pass}/${pass + fail} page checks passed`); dom.window.close(); process.exit(fail ? 1 : 0);
})().catch((e) => { console.log("crashed", e); process.exit(2); });
