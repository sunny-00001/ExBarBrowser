// 工具桥取消/超时的离线回归：用假桥验证 notification 发出、Promise 不悬挂，
// 以及 dispatch 按 tools/list 的 timeoutMs 设置每次调用的时限。
"use strict";
const assert = require("assert");
const { PipeClient } = require("./pipe-exbar");

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function stubClient() {
  const c = new PipeClient({ pipe: "fake", token: "t", pid: 1 });
  const notes = [];
  c._notify = (method, params) => notes.push({ method, params });
  c._send = () => { };
  return { c, notes };
}

(async () => {
  console.log("\n[1] 请求超时：本地立刻失败并发出 notifications/cancelled");
  {
    const { c, notes } = stubClient();
    const p = c._request("tools/call", { name: "fs.copy" }, 20);
    const id = p.requestId;
    const err = await p.then(() => null, (e) => e);
    ok(!!err && err.code === "timeout", "Promise 以 timeout 失败：" + (err && err.code));
    ok(!c.pending.has(id), "pending 表已清理，不悬挂");
    ok(notes.some((n) => n.method === "notifications/cancelled" &&
      n.params.requestId === id && /timeout/.test(n.params.reason)),
      "向宿主发出带 requestId 的取消通知");
  }

  console.log("\n[2] 调用方 abort：Call 返回 cancelled 而不是 transport");
  {
    const { c, notes } = stubClient();
    c.ready = true;
    const ctrl = new AbortController();
    const p = c.Call("fs.delete", { targets: ["a"] }, 1000, false, ctrl.signal);
    ctrl.abort();
    const r = await p;
    ok(r.ok === false && r.errorCode === "cancelled", "errorCode=cancelled：" + r.errorCode);
    ok(notes.some((n) => n.method === "notifications/cancelled" && n.params.name === "fs.delete"),
      "通知里带真实工具名，便于宿主审计");
    ok(c.pending.size === 0, "abort 后没有遗留 pending");
  }

  console.log("\n[3] dispatch 使用工具元数据里的 timeoutMs");
  {
    const calls = [];
    const fake = {
      ready: true,
      inst: { pid: 9 },
      tools: [
        { name: "fs.copy", timeoutMs: 1234, annotations: { readOnlyHint: false } },
        { name: "nav.back", annotations: { readOnlyHint: false } }
      ],
      ToolSchemas: () => [],
      Call: async (...a) => { calls.push(a); return { ok: true, text: "ok", structured: null, assertions: null }; },
      Close() { this.ready = false; }
    };
    const pipePath = require.resolve("./pipe-exbar");
    const realPipe = require(pipePath);
    require.cache[pipePath].exports = {
      PipeClient: realPipe.PipeClient,
      ListEndpoints: () => [],
      ConnectAny: async () => ({ ok: true, client: fake, tried: 0, total: 1 }),
      RUNTIME_DIR: realPipe.RUNTIME_DIR
    };
    delete require.cache[require.resolve("./tools-exbar")];
    const te = require("./tools-exbar");
    const { config } = require("./config");
    config.denyCommands = [];
    config.allowedRoots = [];
    config.dryRun = false;

    ok(await te.EnsureReady() === true, "假桥握手成功");
    await te.dispatch("fs.copy", {}, null, false);
    await te.dispatch("nav.back", {}, null, false);
    ok(calls[0][2] === 1234, "fs.copy 用元数据 1234ms：" + calls[0][2]);
    ok(calls[1][2] === 90000, "没有元数据的命令回落 90000ms：" + calls[1][2]);
    ok(calls[0][4] === null, "signal 原样透传给桥");
    te.Stop();
  }

  assert.strictEqual(fail, 0, `${fail} 项失败`);
  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
