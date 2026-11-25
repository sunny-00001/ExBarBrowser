// 活动流视图逻辑的离线验证：stub 最小 DOM，加载 app.js，喂模拟 history/SSE，
// 断言分组、筛选、风险徽章、详情重建都正确。不发任何网络/输入。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
let src = fs.readFileSync(appPath, "utf8");

// 收集渲染锚点：给几个关键 id 造最小 stub 元素。
const nodes = {};
function mkNode(id) {
  return {
    id, hidden: false, textContent: "", innerHTML: "", style: {}, dataset: {},
    classList: { toggle() {}, add() {}, remove() {}, contains() { return false; } },
    querySelector() { return mkNode("q"); },
    querySelectorAll() { return []; },
    addEventListener() {}, appendChild() {}, focus() {}
  };
}
for (const id of ["activityCount", "activityList", "activityEmpty", "activityFilters",
                  "activityFilterSep", "messages", "activityView", "tabChat", "tabActivity"]) {
  nodes[id] = mkNode(id);
}

const sandbox = {
  console,
  document: {
    getElementById: (id) => nodes[id] || mkNode(id),
    querySelectorAll: () => []
  },
  localStorage: {
    _d: {},
    getItem(k) { return this._d[k] || null; },
    setItem(k, v) { this._d[k] = String(v); }
  },
  fetch: async () => ({ ok: false }),
  EventSource: function () {},
  setInterval: () => 0, clearInterval: () => {},
  setTimeout: () => 0, clearTimeout: () => {},
  requestAnimationFrame: () => 0,
  location: { href: "", search: "" },
  navigator: { userAgent: "" },
  URLSearchParams, Headers,
  AbortController: function () { this.signal = {}; this.abort = function () {}; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error, parseInt, parseFloat, isNaN
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

// 去掉末尾的 init() 自调用（离线环境不跑真实初始化）
src = src.replace(/\ninit\(\);\s*$/, "\n");

vm.createContext(sandbox);
try {
  vm.runInContext(src, sandbox, { filename: "app.js" });
} catch (e) {
  // 初始化期若因缺少别的 DOM 抛错也接受，只要目标函数已定义。
  console.log("加载提示（可忽略）:", e.message);
}

// const 声明不挂到 sandbox 全局；用 vm 表达式取出引用。
const S = vm.runInContext("state", sandbox);
const FN = (name) => vm.runInContext(name, sandbox);
let pass = 0, fail = 0;
function ok(cond, what) { if (cond) { pass++; console.log("  OK", what); } else { fail++; console.log("  FAIL", what); } }

console.log("\n[1] 历史重建：分组/提示词/风险字段");
const history = [
  { role: "user", content: "按类型整理 Pictures", ts: 1000 },
  { role: "assistant", content: "好的", ts: 1100, turn: 1, tools: [
    { id: "t1", name: "file.list", command: "file.list", label: "列出文件", status: "done", risk: "read", step: 1, ts: 1050, args: { path: "C:\\Pictures" }, result: "12 entries" },
    { id: "t2", name: "fs.rename", command: "fs.rename", label: "重命名", status: "mismatch", risk: "write", step: 2, ts: 1060, args: { path: "a.jpg" }, error: "效果未达成", assertions: [{ expr: "exists", expect: true, actual: false, ok: false }] }
  ]},
  { role: "user", content: "删掉临时文件", ts: 2000 },
  { role: "assistant", content: "已删除", ts: 2100, turn: 2, tools: [
    { id: "t3", name: "fs.delete", command: "fs.delete", label: "删除", status: "done", risk: "danger", step: 1, ts: 2050, args: { targets: ["tmp.txt"] } }
  ]}
];
S.activity.filter = "all";
FN('rebuildActivityFromHistory')(history);
ok(S.activity.turns.length === 2, "分成 2 个回合");
ok(S.activity.turns[0].prompt === "按类型整理 Pictures", "回合1提示词正确");
ok(S.activity.turns[0].tools.length === 2, "回合1有 2 条命令");
ok(S.activity.turns[1].prompt === "删掉临时文件", "回合2提示词正确");
ok(S.activity.turns[1].tools[0].risk === "danger", "危险命令 risk 保留");
ok(S.activity.currentTurn === 2, "currentTurn=2");

console.log("\n[2] 筛选");
S.activity.filter = "danger";
ok(FN('actMatchesFilter')({ risk: "danger", status: "done" }) === true, "危险档含 danger");
ok(FN('actMatchesFilter')({ risk: "read", status: "done" }) === false, "危险档排除 read");
S.activity.filter = "fail";
ok(FN('actMatchesFilter')({ risk: "write", status: "mismatch" }) === true, "失败档含 mismatch");
ok(FN('actMatchesFilter')({ risk: "read", status: "done" }) === false, "失败档排除成功");
S.activity.filter = "all";
ok(FN('actMatchesFilter')({ risk: "read", status: "done" }) === true, "全部档含所有");

console.log("\n[3] 实时事件合并（running→done 就地更新，不新增行）");
S.activity.turns = []; S.activity.pendingPrompt = "重命名";
FN('recordActivity')({ id: "x1", turn: 3, name: "fs.rename", command: "fs.rename", label: "重命名", status: "running", risk: "write", step: 1, ts: 3000, args: {} });
FN('recordActivity')({ id: "x1", turn: 3, name: "fs.rename", status: "done", result: "ok" });
ok(S.activity.turns.length === 1, "仍 1 个回合");
ok(S.activity.turns[0].tools.length === 1, "仍 1 行（就地更新）");
ok(S.activity.turns[0].tools[0].status === "done", "状态更新为 done");
ok(S.activity.turns[0].prompt === "重命名", "提示词被消费");

console.log("\n[4] 风险回落（旧 history 无 risk）");
ok(FN('rowRisk')({ name: "nav.back" }) === "read", "nav.* 归只读");
ok(FN('rowRisk')({ name: "fs.delete" }) === "write", "未知写命令归 write");

console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
