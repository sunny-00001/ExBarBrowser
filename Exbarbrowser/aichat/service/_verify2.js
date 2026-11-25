"use strict";
const te = require("./tools-exbar");
const sleep = ms => new Promise(r=>setTimeout(r,ms));
(async () => {
  const eps = require("./pipe-exbar").ListEndpoints();
  console.log(`候选 ${eps.length} 个，存活 ${eps.filter(e=>e.alive).length} 个；前 3：`, 
    eps.slice(0,3).map(e=>`${e.profile}/pid=${e.pid}/alive=${e.alive}`).join(" | "));
  const ok = await te.EnsureReady();
  console.log("EnsureReady =", ok, "|", JSON.stringify(te.Status()));
  if (!ok) process.exit(1);
  const w = await te.dispatch("win.list", {});
  console.log("win.list =", JSON.stringify(w).slice(0, 260));
  const a = await te.dispatch("tool.list", { limit: 1 });
  console.log("当前目录 =", a.path || JSON.stringify(a).slice(0,150));
  const nav = await te.dispatch("go.desktop", {});
  console.log("go.desktop =", nav.text || JSON.stringify(nav).slice(0,100));
  await sleep(2000);
  const b = await te.dispatch("tool.list", { limit: 1 });
  console.log("导航后目录 =", b.path || JSON.stringify(b).slice(0,150));
  console.log(a.path && b.path && a.path !== b.path
    ? "\n✅ 命令作用在真实窗口上（目录变了）"
    : "\n❌ 目录未变");
  te.Stop();
})();
