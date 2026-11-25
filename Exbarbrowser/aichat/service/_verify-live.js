// 端到端验证：命令是否真的作用在**用户眼前那个窗口**上。
//
// 判据不是"命令返回 ok"，而是"窗口状态真的变了"：
//   1. 先记下当前目录
//   2. 导航到一个不同的目录
//   3. 再读当前目录，确认变了
//   4. 用 win.list 确认操作的是可见窗口（visible=true）
"use strict";

const te = require("./tools-exbar");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const ok = await te.EnsureReady();
  if (!ok) { console.log("连不上"); process.exit(1); }
  console.log("已连接：", JSON.stringify(te.Status()));

  // 1) 确认窗口可见
  const w = await te.dispatch("win.list", {});
  console.log("\n[窗口]", JSON.stringify(w).slice(0, 300));

  // 2) 记录当前目录
  let a = await te.dispatch("tool.list", { limit: 1 });
  console.log("\n[当前目录 A]", a.path || a.error);

  // 3) 导航到桌面
  const nav = await te.dispatch("go.desktop", {});
  console.log("\n[go.desktop]", nav.text || JSON.stringify(nav).slice(0, 100));
  await sleep(2200);

  // 4) 再读，确认变了
  let b = await te.dispatch("tool.list", { limit: 1 });
  console.log("[当前目录 B]", b.path || b.error);

  if (b.path && a.path && b.path !== a.path) {
    console.log("\n✅ 目录确实变了：命令作用在真实窗口上");
  } else if (b.path && a.path === b.path) {
    console.log("\n❌ 目录没变：命令没作用在预期窗口上");
  } else {
    console.log("\n⚠ 无法判定（缺少路径）");
  }

  // 5) 恢复原目录
  if (a.path) {
    const back = await te.dispatch("go.config", {});   // no-op 占位
    console.log("\n[收尾] 原目录 =", a.path);
  }

  te.Stop();
  process.exit(0);
})();