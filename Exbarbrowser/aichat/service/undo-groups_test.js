// 撤销组数据模型的离线回归：组 ID、准入、逆序计划、人机边界。
// 不发命令、不写文件；只校验纯数据契约。
"use strict";
const g = require("./undo-groups");

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}
const fileMeta = { undoable: true, undoGroup: "join", undoDomain: "file" };
const bookmarkMeta = { undoable: true, undoGroup: "join", undoDomain: "bookmark" };

console.log("\n[1] 组 ID 与域");
ok(g.IsValidGroupId("turn-12"), "turn-12 合法");
ok(g.IsValidGroupId("a.b_c:d-1"), "点/下划线/冒号/连字符合法");
ok(!g.IsValidGroupId(""), "空 ID 拒绝");
ok(!g.IsValidGroupId("-bad"), "连字符开头拒绝");
ok(!g.IsValidGroupId("bad id"), "空格拒绝");
ok(!g.IsValidGroupId("x".repeat(65)), "超过 64 位拒绝");
ok(g.IsValidDomain("file") && g.IsValidDomain("bookmark"), "file/bookmark 域合法");
ok(!g.IsValidDomain("tab"), "tab 域未实现，拒绝");

console.log("\n[2] 步骤准入来自元数据，不靠命令名猜");
ok(g.CanJoin(fileMeta, "turn-1", "file").ok, "file 命令可入 file 组");
ok(!g.CanJoin(fileMeta, "turn-1", "bookmark").ok, "file 命令不能进 bookmark 组");
ok(!g.CanJoin(bookmarkMeta, "turn-1", "file").ok, "bookmark 命令不能进 file 组");
ok(!g.CanJoin({ ...fileMeta, undoable: false }, "turn-1", "file").ok,
   "不可撤销命令被拒绝");
ok(!g.CanJoin({ ...fileMeta, undoGroup: "none" }, "turn-1", "file").ok,
   "undoGroup=none 被拒绝");
ok(!g.CanJoin(fileMeta, "bad id", "file").ok, "非法组 ID 被拒绝");
ok(!g.CanJoin(null, "turn-1", "file").ok, "元数据缺失 fail closed");

console.log("\n[3] 入组与逆序计划");
const group = g.CreateGroup("turn-7", "file", 1000);
ok(group.id === "turn-7" && group.domain === "file" && group.steps.length === 0,
   "新组为空且带域");
ok(g.AddStep(group, {
  command: "fs.copy", argsHash: "a", undoRef: "u1", undoable: true, byMachine: true
}).ok, "第一步入组成功");
ok(g.AddStep(group, {
  command: "fs.rename", argsHash: "b", undoRef: "u2", undoable: true, byMachine: true
}).ok, "第二步入组成功");
ok(!g.AddStep(group, {
  command: "nav.back", argsHash: "c", undoRef: "u3", undoable: false
}).ok, "不可撤销命令不入组");
ok(!g.AddStep(group, {
  command: "fs.move", argsHash: "d", undoRef: "u2", undoable: true
}).ok, "重复 undoRef 不入组");
ok(!g.AddStep(group, {
  command: "fs.move", argsHash: "d", undoRef: "", undoable: true
}).ok, "缺少 undoRef 不入组");
ok(g.UndoPlan(group).length === 0, "未关闭的组不生成撤销计划");
ok(g.CloseGroup(group, true, "turn done").ok, "组已提交");
const plan = g.UndoPlan(group);
ok(plan.length === 2, "计划含 2 步");
ok(plan[0].undoRef === "u2" && plan[1].undoRef === "u1", "计划按逆序执行");
ok(plan[0].index === 0 && plan[0].command === "fs.rename", "计划保留原命令便于审计");

console.log("\n[4] 失败保留与人工边界");
const failed = g.CreateGroup("turn-8", "file");
g.AddStep(failed, {
  command: "fs.move", argsHash: "x", undoRef: "u9", undoable: true, byMachine: true
});
ok(g.CloseGroup(failed, false, "step failed").ok, "失败时也能关闭组");
ok(g.UndoPlan(failed).length === 0, "未提交的组不撤销");

const mixed = g.CreateGroup("turn-9", "bookmark");
g.AddStep(mixed, {
  command: "bmk.remove", argsHash: "m1", undoRef: "b1", undoable: true, byMachine: true
});
g.AddStep(mixed, {
  command: "bmk.sort", argsHash: "m2", undoRef: "b2", undoable: true, byMachine: false
});
g.CloseGroup(mixed, true);
ok(g.UndoPlan(mixed, true).length === 0, "机器不能跨过人工步骤整组撤销");
ok(g.UndoPlan(mixed, false).length === 2, "人工显式撤销仍可得到完整计划");

console.log("\n[5] 步数上限与关闭后拒写");
const big = g.CreateGroup("turn-10", "file");
for (let i = 0; i < g.MAX_STEPS; i++) {
  g.AddStep(big, {
    command: "fs.copy", argsHash: "h" + i, undoRef: "r" + i, undoable: true
  });
}
ok(big.steps.length === g.MAX_STEPS, `最多 ${g.MAX_STEPS} 步`);
ok(!g.AddStep(big, {
  command: "fs.copy", argsHash: "over", undoRef: "r-over", undoable: true
}).ok, "超过上限拒绝");
g.CloseGroup(big, true);
ok(!g.AddStep(big, {
  command: "fs.copy", argsHash: "late", undoRef: "r-late", undoable: true
}).ok, "关闭后拒绝追加");

console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
