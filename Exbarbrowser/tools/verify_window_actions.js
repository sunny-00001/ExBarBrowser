// AI 面板标题栏动作的离线验证：WebView2 模式只发白名单动作，浏览器直开不发表空按钮。
// 不发合成输入、不抢焦点；用真实 app.js + 最小 DOM stub 直接调用 wireEvents。
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const appPath = path.join(__dirname, "..", "aichat", "ui", "app.js");
const src = fs.readFileSync(appPath, "utf8").replace(/\ninit\(\);\s*$/, "\n");

class FakeEl {
  constructor(id = "") {
    this.id = id;
    this.dataset = {};
    this.style = {};
    this.hidden = false;
    this.title = "";
    this.textContent = "";
    this.innerHTML = "";
    this.children = [];
    this.listeners = {};
    this.classList = {
      toggle() {}, add() {}, remove() {}, contains() { return false; }
    };
  }
  querySelector() { return new FakeEl(); }
  querySelectorAll() { return []; }
  addEventListener(type, fn) {
    (this.listeners[type] || (this.listeners[type] = [])).push(fn);
  }
  fire(type, ev = {}) {
    for (const fn of this.listeners[type] || []) fn(ev);
  }
  closest() { return null; }
  appendChild(c) { this.children.push(c); return c; }
}

const nodes = {};
const getEl = (id) => (nodes[id] || (nodes[id] = new FakeEl(id)));
const winButtons = [new FakeEl("min"), new FakeEl("close")];
winButtons[0].dataset.win = "minimize";
winButtons[1].dataset.win = "close";

const sandbox = {
  console,
  document: {
    getElementById: getEl,
    createElement: () => new FakeEl(),
    querySelector: (sel) => (sel === '[data-win="close"]' ? winButtons[1] : new FakeEl()),
    querySelectorAll: (sel) => {
      if (sel === "[data-win]") return winButtons;
      if (sel === "[data-win], #pinBtn") return winButtons.concat([getEl("pinBtn")]);
      return [];
    },
    addEventListener() {}
  },
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
  addEventListener() {}, removeEventListener() {}, innerWidth: 420, innerHeight: 800,
  URLSearchParams, Headers,
  AbortController: function () { this.signal = {}; this.aborted = false; this.abort = () => { this.aborted = true; }; },
  Date, JSON, Math, Number, String, Array, Object, Set, Map, RegExp, Error,
  Promise, Boolean, parseInt, parseFloat, isNaN
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "app.js" });
const FN = (name) => vm.runInContext(name, sandbox);
const S = vm.runInContext("state", sandbox);

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

console.log("\n[1] WebView2 模式：只发宿主能执行的动作");
const posted = [];
sandbox.chrome = { webview: { postMessage: (m) => posted.push(m) } };
FN("wireEvents")();

getEl("titlebar").fire("mousedown", { button: 0, target: { closest: () => null } });
ok(posted.length === 0, "不发 drag（子窗口不能拖整窗）");

getEl("titlebar").fire("dblclick", { target: { closest: () => null } });
ok(posted.some((m) => m.type === "window" && m.action === "maximize"), "双击标题栏发 maximize");

winButtons[0].fire("click");
winButtons[1].fire("click");
ok(posted.some((m) => m.action === "minimize"), "最小化按钮发 minimize");
ok(posted.some((m) => m.action === "close"), "关闭按钮发 close（宿主收起面板）");
ok(winButtons[1].title === "收起 AI 面板", "close 按钮文案改为收起面板");
ok(posted.every((m) => m.action !== "drag"), "所有已发动作都在白名单内");

console.log("\n[2] 浏览器直开：隐藏无宿主的窗口按钮");
delete sandbox.chrome;
posted.length = 0;
for (const el of winButtons) el.listeners = {};
getEl("titlebar").listeners = {};
FN("wireEvents")();
ok(winButtons.every((b) => b.hidden), "minimize/close 按钮被隐藏");
ok(getEl("pinBtn").hidden, "置顶按钮被隐藏");
getEl("titlebar").fire("dblclick", { target: { closest: () => null } });
ok(posted.every((m) => m.action !== "maximize"), "直开模式不再重复注册 maximize 监听");

console.log("\n[3] postWindow 在宿主缺失时保持静默");
posted.length = 0;
FN("postWindow")("minimize");
ok(posted.length === 0, "没有 chrome.webview 时不抛错也不发包");

console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
