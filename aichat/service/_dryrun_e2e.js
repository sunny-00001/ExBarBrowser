// dryRun 在真实 dispatch 上的验证：只读放行、写命令拦下、什么都没改。
//
// 判据不能只看返回值（那可能是兜底文案），要看：
//   · 写命令返回 errorCode=blocked_by_policy 且理由提到 dry run；
//   · 只读命令照常返回数据（证明 dryRun 没有把整条链关掉）；
//   · 关掉 dryRun 后同一写命令不再被这道门拦（会落到宿主的双门，那是另一回事）。
//
// 用法：node aichat/service/_dryrun_e2e.js
"use strict";

const { config } = require("./config");
const te = require("./tools-exbar");

let pass = 0;
const fails = [];
function ok(cond, what, detail) {
  if (cond) { pass++; return; }
  fails.push(what + (detail ? `（${detail}）` : ""));
}

(async () => {
  if (!(await te.EnsureReady())) {
    console.log("SKIP 没有连上 ExBarBrowser");
    process.exit(0);
  }

  const saved = { dryRun: config.dryRun, deny: config.denyCommands.slice(), roots: config.allowedRoots.slice() };
  config.denyCommands = [];
  config.allowedRoots = [];

  try {
    // ---- 1) 只读命令在 dryRun 下必须照常可用 ----
    config.dryRun = true;
    {
      const r = await te.dispatch("tool.list", { limit: 2 }, null);
      ok(!r.error, "dryRun 下只读命令可用", r.error);
      ok(Array.isArray(r.entries), "只读命令真的返回了数据");
    }

    // ---- 2) 写命令必须被 dryRun 拦下 ----
    {
      const w = await te.dispatch("view.sort", { by: 2 }, null);
      // view.sort 是 write（readOnlyHint=false），dryRun 下必须拦。
      const blocked = w.errorCode === "blocked_by_policy";
      const saysDryRun = String(w.error || "").indexOf("dry run") >= 0;
      ok(blocked, "dryRun 下写命令被边界拦", w.errorCode);
      ok(saysDryRun, "拒绝理由提到 dry run", w.error);
    }

    // ---- 3) 关掉 dryRun 后同一写命令不再被这道门拦 ----
    config.dryRun = false;
    {
      const w2 = await te.dispatch("view.sort", { by: 2 }, null);
      ok(w2.errorCode !== "blocked_by_policy",
        "dryRun 关闭后写命令不再被边界拦", w2.errorCode + " / " + w2.error);
    }
  } finally {
    // 复原，别把用户配置留在收紧状态
    config.dryRun = saved.dryRun;
    config.denyCommands = saved.deny;
    config.allowedRoots = saved.roots;
    te.Stop();
  }

  console.log(`\n_dryrun_e2e: ${pass} 通过 / ${pass + fails.length} 总`);
  if (fails.length) {
    for (const f of fails) console.log("  FAIL " + f);
    process.exit(1);
  }
})().catch((e) => {
  console.log("ERR " + (e && e.message ? e.message : e));
  process.exit(1);
});