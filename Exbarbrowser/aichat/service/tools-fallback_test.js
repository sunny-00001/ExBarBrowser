// tools-exbar 首轮工具回退的边界测试：用假的 pipe/guard/config 加载真实模块，
// 断言回退只重连、不执行，且 deny/审批的 fail-closed 口径没有被绕过。
"use strict";
const assert = require("assert");

function fake(rel, exports) {
  const p = require.resolve(rel);
  require.cache[p] = {
    id: p, filename: p, loaded: true, exports,
    children: [], paths: []
  };
}

let connectCalls = 0;
let succeedFromCall = Infinity;
const calls = [];
const guardChecks = [];

const tool = {
  name: "nav.back",
  title: "后退",
  description: "返回上一页",
  risk: "",
  annotations: { readOnlyHint: false, destructiveHint: false }
};
const deleteTool = {
  name: "fs.delete",
  title: "删除",
  description: "删除文件",
  risk: "不可逆",
  annotations: { readOnlyHint: false, destructiveHint: true }
};

const client = {
  ready: true,
  inst: { pid: 4242 },
  serverInfo: { name: "fake-exbar" },
  tools: [tool, deleteTool],
  ToolSchemas() {
    return [tool, deleteTool];
  },
  Call(name, args, timeoutMs, approve, signal) {
    calls.push({ name, args, timeoutMs, approve, signal });
    return Promise.resolve({ ok: true, structured: { text: "ok" } });
  },
  Close() { this.ready = false; }
};

fake("./pipe-exbar", {
  ConnectAny: async () => {
    connectCalls++;
    if (connectCalls < succeedFromCall) {
      return { ok: false, reason: `test bridge busy (call ${connectCalls})` };
    }
    client.ready = true;
    return { ok: true, client, tried: 0, total: 1 };
  },
  ListEndpoints: () => []
});
fake("./config", { config: {} });
fake("./guard", {
  Check: (name, _args, _cfg) => {
    guardChecks.push(name);
    return name === "fs.delete" ? "blocked by test policy" : null;
  },
  SetMetaProvider: () => { }
});

const tools = require("./tools-exbar");

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

(async () => {
  console.log("\n[1] 桥持续不可用：只重探一次，明确失败，不执行命令");
  succeedFromCall = Infinity;
  const failed = await tools.EnsureReadyWithFallback(undefined, 0);
  ok(failed.ok === false, "持续失败时返回不就绪");
  ok(failed.retried === true, "标记做过重探");
  ok(failed.tools === 0, "工具清单为空，不留旧工具");
  ok(/busy/.test(String(failed.reason)), "保留失败原因");
  ok(connectCalls === 2, "只尝试两次：首轮 + 一次回退，实测 " + connectCalls);
  ok(calls.length === 0, "回退期间没有执行任何工具");

  console.log("\n[2] 首轮忙、重探恢复：带上工具，但不借机执行命令");
  succeedFromCall = connectCalls + 2;
  const recovered = await tools.EnsureReadyWithFallback(undefined, 0);
  ok(recovered.ok === true, "重探后就绪");
  ok(recovered.retried === true, "明确标记发生过回退");
  ok(recovered.tools === 2, "工具数如实回填：" + recovered.tools);
  ok(tools.TOOL_SCHEMAS.length === 2, "导出的 TOOL_SCHEMAS 引用被原地填充");
  ok(connectCalls === 4, "本轮同样只多试一次，实测 " + connectCalls);
  ok(calls.length === 0, "恢复连接不等于自动执行工具");

  console.log("\n[3] 安全边界：deny 在连桥之前拦下，回退不能绕过");
  const before = connectCalls;
  const blocked = await tools.dispatch("fs.delete", { path: "C:\\tmp\\x" }, undefined, true);
  ok(blocked && blocked.errorCode === "blocked_by_policy", "返回 blocked_by_policy");
  ok(guardChecks.includes("fs.delete"), "guard.Check 收到这次调用");
  ok(connectCalls === before, "被拒调用没有触发连接/回退");
  ok(calls.length === 0, "被拒调用没有到达宿主");

  console.log("\n[4] 审批边界：未知工具仍然 fail closed");
  const unknown = tools.ApprovalOf("tool.not.in.table");
  ok(unknown.needed === true, "未知工具按需要确认处理");
  ok(unknown.known === false, "不假装认识未知工具");

  assert.strictEqual(fail, 0, `${fail} 项失败`);
  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
