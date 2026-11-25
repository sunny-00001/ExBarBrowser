// SSE 事件归属的离线验证：旧回合的 done/error/tool 不能清掉新回合的 busy、
// 审批卡或把工具卡挂到新回答上。用最小 DOM + 真实 app.js，不发网络请求。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
let src = fs.readFileSync(appPath, "utf8").replace(/\ninit\(\);\s*$/, "\n");

class FakeEl {
  constructor() {
    this.className = "";
    this.dataset = {};
    this.style = {};
    this.hidden = false;
    this.textContent = "";
    this.innerHTML = "";
    this.children = [];
    this.classList = {
      toggle() {}, add() {}, remove() {}, contains() { return false; }
    };
  }
  querySelector() { return new FakeEl(); }
  querySelectorAll() { return []; }
  addEventListener() {}
  appendChild(c) { this.children.push(c); return c; }
}

const nodes = {};
const getEl = (id) => (nodes[id] || (nodes[id] = new FakeEl()));
const sandbox = {
  console,
  document: { getElementById: getEl, createElement: () => new FakeEl(), querySelectorAll: () => [], addEventListener: () => {} },
  localStorage: {
    _d: {},
    getItem(k) { return Object.prototype.hasOwnProperty.call(this._d, k) ? this._d[k] : null; },
    setItem(k, v) { this._d[k] = String(v); }
  },
  fetch: async () => ({ ok: false, status: 500 }),
  setInterval: () => 0, clearInterval: () => {},
  setTimeout: (fn) => { if (fn) fn(); return 0; }, clearTimeout: () => {},
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  location: { href: "", search: "" }, navigator: { userAgent: "" },
  URLSearchParams, Headers,
  AbortController: function () { this.signal = {}; this.aborted = false; this.abort = () => { this.aborted = true; }; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error,
  Promise, Boolean, parseInt, parseFloat, isNaN
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "app.js" });

// 把 handleEvent 的副作用的换成日志，避免测试依赖完整 DOM 结构。
vm.runInContext(`
  globalThis.__log = [];
  const L = (x) => globalThis.__log.push(String(x));
  appendTool = (ev) => L("tool:" + ev.id);
  recordActivity = (ev) => L("activity:" + ev.id);
  setStatus = (x) => L("status:" + (x || ""));
  setBusy = (on) => { state.busy = !!on; L("busy:" + on); };
  recordUsage = () => {};
  clearApprovals = () => L("clearApprovals");
  appendApproval = (ev) => L("approval:" + ev.id);
  loadContext = () => {};
  toast = (x) => L("toast:" + x);
  syncAttachChip = () => {};
  appendUser = (x) => L("user:" + x);
  globalThis.__assistant = {
    _text: "",
    el: { querySelector: () => null },
    bubble: { style: {}, classList: { remove() {} } },
    scheduleRender: () => {},
    addThinking: () => L("thinking"),
    finish: (x) => L("finish:" + (x || ""))
  };
  appendAssistant = () => globalThis.__assistant;
`, sandbox);

const S = vm.runInContext("state", sandbox);
const FN = (name) => vm.runInContext(name, sandbox);
const log = () => sandbox.__log;
const clearLog = () => { sandbox.__log.length = 0; };
let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}
const assistant = sandbox.__assistant;

console.log("\n[1] 当前 generation + 当前 turn 的事件正常处理");
S.generation = 1; S.activeTurn = 7; S.busy = true; clearLog();
FN("handleEvent")({ type: "start", turn: 7 }, assistant, 1);
FN("handleEvent")({ type: "tool", turn: 7, id: "t1" }, assistant, 1);
ok(log().includes("tool:t1"), "本回合 tool 事件被接受");
ok(log().includes("activity:t1"), "本回合同时进入活动流");

console.log("\n[2] 旧 generation 的 done 不能结束新回合");
clearLog();
FN("handleEvent")({ type: "done", turn: 7, reply: "旧回答" }, assistant, 0);
ok(!log().some((x) => x.startsWith("finish:")), "旧 done 没有写回答");
ok(S.busy === true, "旧 done 没有把 busy 清成 false");

console.log("\n[3] stop() 夺权后，迟到 error 全部丢弃");
const ctrl = new sandbox.AbortController();
S.abort = ctrl; clearLog();
FN("stop")();
ok(ctrl.aborted === true, "stop 中止了 fetch");
ok(S.generation === 2, "stop 推进了 generation");
ok(S.busy === false, "stop 立即释放 busy");
FN("handleEvent")({ type: "error", turn: 7, message: "旧错误" }, assistant, 1);
ok(!log().some((x) => x.startsWith("finish:")), "迟到 error 没有写警告");
ok(!log().includes("clearApprovals"), "迟到 error 没有清掉新回合审批卡");

console.log("\n[4] 同 generation 但 turnId 不同也拒绝（跨回合污染第二道闸）");
S.generation = 3; S.activeTurn = 0; S.busy = true; clearLog();
FN("handleEvent")({ type: "start", turn: 8 }, assistant, 3);
ok(S.activeTurn === 8, "start 写入服务端权威 turnId");
FN("handleEvent")({ type: "tool", turn: 7, id: "stale" }, assistant, 3);
ok(!log().includes("tool:stale"), "旧 turn 的 tool 被拒绝");
FN("handleEvent")({ type: "done", turn: 7, reply: "旧" }, assistant, 3);
ok(S.busy === true, "旧 turn 的 done 被拒绝");
FN("handleEvent")({ type: "done", turn: 8, reply: "新回答" }, assistant, 3);
ok(S.busy === false, "本 turn 的 done 正常结束");
ok(log().some((x) => x === "finish:新回答"), "本 turn 的回答正常写入");

console.log("\n[5] send() 在发请求前夺取新 generation");
const before = S.generation;
clearLog();
const sending = FN("send")("新消息");
ok(S.generation === before + 1, "send 推进 generation：" + before + " → " + S.generation);

(async () => {
  await sending;
  ok(log().includes("user:新消息"), "send 仍然正常发出消息");
  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})();
