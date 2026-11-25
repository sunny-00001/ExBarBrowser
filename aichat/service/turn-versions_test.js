// 回合版本数据模型单测：旧尝试保留、活动分支唯一、分叉上下文不带旧尝试。
"use strict";
const V = require("./turn-versions");

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}
const u = (turnId, content, extra = {}) => ({
  role: "user", content, turnId, turn: turnId, ...extra
});
const a = (turnId, content, extra = {}) => ({
  role: "assistant", content, turnId, turn: turnId, ...extra
});

console.log("\n[1] 活动分支：同 root 只保留最大 attempt");
const h1 = [
  u(10, "原始", { rootTurnId: 10, attempt: 1 }),
  a(10, "旧回答", { rootTurnId: 10, attempt: 1 }),
  u(11, "重试", { retryOf: 10, rootTurnId: 10, attempt: 2 }),
  a(11, "新回答", { retryOf: 10, rootTurnId: 10, attempt: 2 }),
  u(12, "普通下一轮")
];
const active1 = V.activeHistory(h1);
ok(active1.length === 3, "4 条版本记录收敛成 2 轮 + 1 条旧数据");
ok(active1.some((m) => m.turnId === 11 && m.content === "新回答"), "保留新尝试");
ok(!active1.some((m) => m.turnId === 10), "排除被取代的旧尝试");
ok(active1.some((m) => m.turnId === 12), "无元数据的普通回合不受影响");

console.log("\n[2] 目标定位与版本血缘");
ok(V.findTurn(h1, 999) === null, "不存在的 turnId 返回 null");
const line = V.lineageFor(h1, 10);
ok(line && line.targetTurnId === 10, "目标回合正确");
ok(line.rootTurnId === 10 && line.attempt === 3, "attempt 从当前最大 2 递增到 3");
ok(line.prompt === "原始", "带回原提示词供 UI 重试");
ok(V.lineageFor(h1, 999) === null, "无目标不生成血缘");

console.log("\n[3] 分叉上下文：目标及其之后都不进模型上下文");
const ctx = V.contextForRetry(h1, 10);
ok(ctx && ctx.length === 0, "从第 1 轮分叉时上下文为空");
const h2 = [u(8, "更早"), a(8, "早回答"), u(10, "原始"), a(10, "旧回答"), u(12, "之后")];
const ctx2 = V.contextForRetry(h2, 10);
ok(ctx2.length === 2 && ctx2[0].turnId === 8, "只保留分叉点之前的历史");
ok(!ctx2.some((m) => m.turnId === 12), "目标之后的回合被排除");
ok(V.contextForRetry(h2, 999) === null, "目标不存在时拒绝分叉");

console.log("\n[4] 分叉上下文仍过滤旧版本");
const h3 = [
  u(20, "旧", { rootTurnId: 20, attempt: 1 }),
  a(20, "旧答", { rootTurnId: 20, attempt: 1 }),
  u(21, "新", { retryOf: 20, rootTurnId: 20, attempt: 2 }),
  a(21, "新答", { retryOf: 20, rootTurnId: 20, attempt: 2 }),
  u(30, "要重试")
];
const ctx3 = V.contextForRetry(h3, 30);
ok(ctx3.length === 2 && ctx3[0].turnId === 21, "分叉前只取活动分支");
ok(!ctx3.some((m) => m.turnId === 20), "旧尝试不借分叉复活");

console.log("\n[5] 落盘标记旧版本，不改新回合");
const mutable = JSON.parse(JSON.stringify(h1));
const changed = V.markSuperseded(mutable, line, 31);
ok(changed === 4, "旧 root 的 4 条记录被标记");
ok(mutable.find((m) => m.turnId === 10).supersededBy === 31, "旧用户条目指向新回合");
ok(mutable.find((m) => m.turnId === 11).supersededBy === 31, "旧的新尝试也被取代");
ok(!mutable.find((m) => m.turnId === 12).supersededBy, "其他 root 不受影响");

console.log("\n[6] 分支列表与选择校验");
const branchHistory = [
  u(10, "旧", { rootTurnId: 10, attempt: 1 }),
  u(11, "新", { rootTurnId: 10, attempt: 2, retryOf: 10 }),
  u(12, "后继", { rootTurnId: 12, parentTurnId: 11 })
];
const branches = V.branchList(branchHistory, { 10: 10 });
ok(branches.length === 2, "列出两个 root");
ok(branches[0].selectedTurnId === 10 && branches[0].attempts.length === 2, "选择反映到分支列表");
ok(V.canSelectBranch(branchHistory, 10, 10), "同 root 的旧尝试可选");
ok(!V.canSelectBranch(branchHistory, 12, 10), "跨 root 的选择被拒绝");
ok(!V.canSelectBranch(branchHistory, 10, 999), "不存在的回合被拒绝");

console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
