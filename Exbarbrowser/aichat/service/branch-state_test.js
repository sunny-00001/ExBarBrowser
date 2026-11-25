// 分支选择状态的持久化与输入校验。
"use strict";
const assert = require("node:assert/strict");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { createBranchState } = require("./branch-state");

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "exbar-branches-"));
let pass = 0;
const ok = (v, label) => { assert.ok(v, label); pass++; };
const eq = (a, b, label) => { assert.equal(a, b, label); pass++; };

const state = createBranchState(dir);
eq(state.activeFor("s1", 10), 0, "初始没有选择");
ok(state.select("s1", 10, 11), "写入选择");
eq(state.activeFor("s1", 10), 11, "读回选择");
eq(state.activeFor("s1", 99), 0, "其他 root 不受影响");
ok(state.select("s1", 20, 21), "写入第二个 root");
eq(Object.keys(state.all("s1")).length, 2, "两个 root 共存");
ok(!state.select("s1", 0, 5) && !state.select("s1", 10, 0), "非法参数拒绝");

const reloaded = createBranchState(dir);
eq(reloaded.activeFor("s1", 10), 11, "新实例从磁盘读回");
reloaded.clear("s1");
eq(createBranchState(dir).activeFor("s1", 10), 0, "清空后不再选中");

fs.rmSync(dir, { recursive: true, force: true });
console.log(`branch_state_test: ${pass} checks passed`);
