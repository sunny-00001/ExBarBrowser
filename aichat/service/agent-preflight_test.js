// 首轮工具元数据竞态与一次性回退的离线回归：用假配置/假桥/假 fetch 加载真实
// agent-stream，断言 EnsureReadyWithFallback 一定发生在第一次模型请求之前，
// 且桥忙时只重探一次、恢复后首轮就带工具、持续失败才明确降级。
"use strict";
const assert = require("assert");

function fake(rel, exports) {
  const p = require.resolve(rel);
  require.cache[p] = {
    id: p, filename: p, loaded: true, exports,
    children: [], paths: []
  };
}

const events = [];
const toolSchemas = [];
let ensureOk = true;
let recoverOnRetry = false;
let capturedBody = null;
let emitted = [];

function addTool() {
  if (!toolSchemas.length) toolSchemas.push({ name: "nav.back", description: "back" });
}

fake("./config", {
  config: {
    model: "test-model", baseUrl: "http://127.0.0.1:1", apiKey: "k",
    protocol: "openai", temperature: 0, maxTokens: 64, maxSteps: 3,
    timeoutMs: 1000, verbose: false, host: "127.0.0.1", port: 1,
    logDir: "", dryRun: false, denyCommands: [], allowedRoots: []
  },
  modelReadiness: () => ({ ok: true, reason: "" }),
  CONFIG_PATH: "aichat.json", MODEL_PATH: "models.json",
  reloadConfig: () => false,
  configStatus: () => ({ path: "", modelPath: "", error: "" }),
  saveAll: () => ({}), keyHint: () => ""
});
fake("./protocols", {
  requestSpec: () => ({ url: "http://127.0.0.1:1/chat", headers: {} }),
  buildChatBody: (_protocol, body) => { capturedBody = body; return JSON.parse(JSON.stringify(body)); },
  parseResponse: () => ({ message: { role: "assistant", content: "ok" }, finish: "stop", usage: null }),
  parseFrame: () => ({ done: false }),
  mergeUsage: (a, b) => a || b,
  CACHE_SPLIT: "\u0000EXBAR_CACHE_SPLIT\u0000"
});
fake("./project-knowledge", { knowledgeCard: () => "[KB]", knowledgeHits: () => "" });
fake("./intents", { matchFastIntent: () => null });
fake("./tools-exbar", {
  TOOL_SCHEMAS: toolSchemas,
  EnsureReady: async () => {
    events.push("ensure");
    if (ensureOk) addTool();
    return ensureOk && toolSchemas.length > 0;
  },
  EnsureReadyWithFallback: async () => {
    events.push("ensure");
    if (ensureOk) addTool();
    if (ensureOk && toolSchemas.length) {
      return { ok: true, tools: toolSchemas.length, retried: false, reason: "", cancelled: false };
    }

    events.push("retry");
    toolSchemas.length = 0;
    ensureOk = recoverOnRetry;
    events.push("ensure");
    if (ensureOk) addTool();
    const ok = ensureOk && toolSchemas.length > 0;
    return {
      ok,
      tools: toolSchemas.length,
      retried: true,
      reason: ok ? "" : "test bridge unavailable",
      cancelled: false
    };
  },
  dispatch: async () => ({ ok: true }),
  CapabilityNote: () => (toolSchemas.length ? `[TOOLS ${toolSchemas.length}]` : ""),
  ApprovalOf: () => ({ needed: false, known: true, readOnly: true, undoable: false }),
  ToolTitle: () => "", ToolMeta: () => null
});

delete require.cache[require.resolve("./agent-stream")];
const { runTurnStream } = require("./agent-stream");

global.fetch = async () => {
  events.push("fetch");
  return {
    ok: true,
    headers: { get: () => "application/json" },
    json: async () => ({ ok: true })
  };
};

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}
const systemOf = () => {
  const m = (capturedBody && capturedBody.messages || []).find((x) => x.role === "system");
  return m ? String(m.content) : "";
};
const statuses = () => emitted.filter((e) => e.type === "status").map((e) => String(e.text || ""));

(async () => {
  console.log("\n[1] 首轮：握手先于模型请求，工具清单已经填好");
  toolSchemas.length = 0;
  ensureOk = true;
  recoverOnRetry = false;
  events.length = 0;
  emitted = [];
  capturedBody = null;
  await runTurnStream("后退", { sessionId: "s", history: [] }, (e) => emitted.push(e));
  ok(events.join(" → ") === "ensure → fetch", "顺序 = ensure → fetch：" + events.join(" → "));
  ok(capturedBody && capturedBody.tools && capturedBody.tools.length === 1, "第一次请求就带 1 个工具");
  ok(systemOf().includes("[TOOLS 1]"), "system prompt 带上工具数提示");
  ok(!statuses().some((s) => /没有可用工具|回退重探/.test(s)), "首轮就绪不制造降级提示");
  ok(emitted.some((e) => e.type === "done"), "回合正常结束");

  console.log("\n[2] 首轮桥忙、重探恢复：仍然在第一次模型请求前拿到工具");
  toolSchemas.length = 0;
  ensureOk = false;
  recoverOnRetry = true;
  events.length = 0;
  emitted = [];
  capturedBody = null;
  await runTurnStream("后退", { sessionId: "s", history: [] }, (e) => emitted.push(e));
  ok(events.join(" → ") === "ensure → retry → ensure → fetch", "顺序 = ensure → retry → ensure → fetch：" + events.join(" → "));
  ok(capturedBody && capturedBody.tools && capturedBody.tools.length === 1, "恢复后第一次请求就带 1 个工具");
  ok(systemOf().includes("[TOOLS 1]"), "恢复后的 system prompt 带上工具数提示");
  ok(statuses().some((s) => /工具桥已恢复/.test(s)), "状态流明确提示工具桥已恢复");
  ok(!systemOf().includes("no tools this turn"), "恢复后不误报无工具");

  console.log("\n[3] 桥持续不可用：仍先回退重探，然后明确降级不假装能操作");
  toolSchemas.length = 0;
  ensureOk = false;
  recoverOnRetry = false;
  events.length = 0;
  emitted = [];
  capturedBody = null;
  await runTurnStream("后退", { sessionId: "s", history: [] }, (e) => emitted.push(e));
  ok(events.join(" → ") === "ensure → retry → ensure → fetch", "降级路径也只重探一次：" + events.join(" → "));
  ok(statuses().some((s) => /未连接|没有可用工具|回退重探/.test(s)), "发出明确的状态提示");
  ok(systemOf().includes("no tools this turn"), "system prompt 说明本轮没有工具");
  ok(capturedBody && capturedBody.tools && capturedBody.tools.length === 0, "空工具清单如实上报");

  assert.strictEqual(fail, 0, `${fail} 项失败`);
  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
