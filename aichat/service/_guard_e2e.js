// 安全边界在真实 dispatch 路径上的集成验证。
//
// 与 guard_test.js 的分工：那个只测纯函数（不需要桥、不需要实例），
// 这个测"边界真的挡在 dispatch 出口上"，需要一个活着的 ExBarBrowser 实例。
//
// 判据不是"返回值里有 error"（那可能是兜底文案），而是：
//   · 越界请求返回 errorCode=blocked_by_policy（说明是边界拦的，不是宿主拒的）；
//   · **宿主侧完全没收到这条调用** —— 用一次界内的 tool.list 前后对比可观测
//     状态无法直接证明，所以改用 allowlist 里不含目标 + 拒绝理由带参数位置来判。
//
// 用法：node aichat/service/_guard_e2e.js
"use strict";

const assert = require("assert");
const { config } = require("./config");
const te = require("./tools-exbar");

let pass = 0;
const fails = [];
function ok(cond, what, detail) {
  if (cond) { pass++; return; }
  fails.push(what + (detail ? `（${detail}）` : ""));
}

(async () => {
  const ready = await te.EnsureReady();
  if (!ready) {
    console.log("SKIP 没有连上 ExBarBrowser（程序没开或管道被占）");
    process.exit(0);
  }
  const st = te.Status();
  console.log(`连上 pid=${st.pid}，工具 ${st.tools} 个`);

  // ---- 1) 白名单为空 = 不限制（默认行为不能变）----
  config.allowedRoots = [];
  config.denyCommands = [];
  {
    const r = await te.dispatch("tool.list", { limit: 5 }, null);
    ok(!r.error, "空白名单下正常命令仍可用", r.error);
  }

  // ---- 2) 设白名单后，界内放行、界外拒绝 ----
  // 根目录的取法：优先用实例当前目录，**取不到就换一个确定的本地目录**。
  // 只看实例的状态会让测试对"视图刚好还没就绪 / 当前在某命名空间"敏感 ——
  // 那种情况下 path 是空的，白名单会变空，后面全是假失败。
  // 判据是"边界是否按白名单判定"，不是"实例当前在哪"，所以根目录可以自己给。
  const cur = await te.dispatch("tool.list", { limit: 1 }, null);
  let curPath = String(cur.path || "");
  if (!curPath) {
    curPath = process.env.TEMP || process.env.USERPROFILE || "C:\\Windows";
    console.log(`  （实例当前目录读不到，改用 ${curPath} 当测试根）`);
  }
  // 取"当前路径的上一级"当白名单根。
  // 【不能简单削掉最后一段】`D:\Nextcloud` 削完是 `D:`（盘根），
  // 而盘根语义在 guard 里已经特殊处理过了 —— 用它测"界内放行"虽然能过，
  // 但会让测试**绕过**"普通目录当根"这条更常见的路径。
  // 这里改用 path.dirname，并对盘根做回落，两种根都覆盖到。
  const path = require("path");
  let root = path.dirname(curPath.replace(/\//g, "\\"));
  if (!root || root === "." ) root = curPath;
  if (/^[a-zA-Z]:$/.test(root)) root = root + "\\";   // 盘根补分隔符，别留裸 `D:`
  const insideItem = path.join(root, "__probe__");
  ok(!!root, "测试根目录可用", root);

  config.allowedRoots = [root];
  {
    // 界内：用一条**确实带路径参数**的命令证明放行，而不是靠 tool.list
    // （tool.list 没有 path 参数，用它测"界内放行"只会得到 bad_args，
    //  那会把"参数写错"误读成"被边界拦了"）。
    const inside = await te.dispatch("fs.stat", { path: curPath }, null);
    const blockedByPolicy = inside.errorCode === "blocked_by_policy";
    ok(!blockedByPolicy, `界内路径不被边界拦（root=${root}）`, inside.errorCode + " / " + inside.error);
  }
  {
    // 界外：明确另一个盘的路径
    const outside = await te.dispatch("fs.stat", { path: "Z:\\__not_allowed__" }, null);
    ok(!!outside.error, "界外路径被拒");
    ok(outside.errorCode === "blocked_by_policy",
      "拒绝来自边界（errorCode=blocked_by_policy）", outside.errorCode);
    ok(String(outside.error).indexOf("allowedRoots") >= 0,
      "拒绝理由提到 allowedRoots", outside.error);
  }
  {
    // 数组参数里的第二个越界也要被抓到（不能只查第一个）
    const arr = await te.dispatch("fs.delete",
      { targets: [insideItem, "Z:\\__b__"], approve: true }, null);
    ok(!!arr.error, "数组里的越界项被拒");
    ok(String(arr.error).indexOf("targets[1]") >= 0,
      "拒绝理由指出 targets[1]", arr.error);
  }

  // ---- 3) denyCommands 用点分名生效；旧 Opus 动词不该是唯一防线 ----
  config.allowedRoots = [];
  config.denyCommands = ["fs.delete"];
  {
    const d = await te.dispatch("fs.delete", { targets: [insideItem], approve: true }, null);
    ok(!!d.error, "黑名单命令被拒");
    ok(d.errorCode === "blocked_by_policy", "黑名单拒绝同样标记来源", d.errorCode);
    ok(String(d.error).indexOf("denyCommands") >= 0, "理由提到 denyCommands", d.error);
  }
  {
    const other = await te.dispatch("tool.list", { limit: 1 }, null);
    ok(!other.error, "黑名单不误伤其它命令", other.error);
  }

  // ---- 4) 前缀族写法 ----
  config.denyCommands = ["fs.*"];
  {
    const d2 = await te.dispatch("fs.rename", { from: "a", to: "b" }, null);
    ok(!!d2.error && d2.errorCode === "blocked_by_policy", "前缀族 fs.* 命中 fs.rename", d2.errorCode);
  }

  // ---- 复原：不能让测试把用户配置留在收紧状态 ----
  config.allowedRoots = [];
  config.denyCommands = [];
  {
    const back = await te.dispatch("tool.list", { limit: 1 }, null);
    ok(!back.error, "复原后命令可用", back.error);
  }

  te.Stop();
  console.log(`\n_guard_e2e: ${pass} 通过 / ${pass + fails.length} 总`);
  if (fails.length) {
    for (const f of fails) console.log("  FAIL " + f);
    process.exit(1);
  }
})().catch((e) => {
  console.log("ERR " + (e && e.message ? e.message : e));
  process.exit(1);
});