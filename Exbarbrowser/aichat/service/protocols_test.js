// protocols.js 的离线回归：验证 prompt-cache 分段在 Anthropic 线路上生成
// cache_control，在 OpenAI 兼容线路上被干净抹掉，且不改循环里的原消息。
"use strict";
const assert = require("assert");
const { buildChatBody, CACHE_SPLIT, normalizeUsage } = require("./protocols");

let pass = 0;
function ok(cond, what) {
  assert.ok(cond, what);
  pass++;
  console.log("  OK", what);
}

const stable = "S".repeat(2100);
const messages = [
  { role: "system", content: stable + CACHE_SPLIT + "VAR" },
  { role: "user", content: "hi" }
];
const base = { model: "m", messages, temperature: 0.2, maxTokens: 16, tools: [], stream: true };

console.log("[1] Anthropic 稳定前缀可缓存");
const ant = buildChatBody("anthropic", base);
ok(Array.isArray(ant.system), "system 变成 text block 数组");
ok(ant.system.length === 2, "稳定段 + 易变段各一块");
ok(ant.system[0].text === stable, "第一块是原样稳定前缀");
ok(ant.system[0].cache_control && ant.system[0].cache_control.type === "ephemeral",
  "第一块带 cache_control");
ok(ant.system[1].text === "VAR" && !ant.system[1].cache_control,
  "第二块是易变内容，不缓存");
ok(ant.max_tokens === 16 && ant.temperature === 0.2, "原有字段没丢");

console.log("[2] OpenAI 兼容线路不泄漏内部标记");
const oa = buildChatBody("openai", base);
ok(!oa.messages[0].content.includes(CACHE_SPLIT), "NUL 标记已从消息里抹掉");
ok(oa.messages[0].content === stable + "VAR", "两段无缝拼回原文");
ok(messages[0].content.includes(CACHE_SPLIT), "没有原地修改循环里的 messages");
ok(oa.system === undefined, "OpenAI 请求没有顶层 system");

console.log("[3] 无标记时保持旧行为");
const plain = buildChatBody("anthropic", {
  ...base, messages: [{ role: "system", content: "only" }, { role: "user", content: "hi" }]
});
ok(plain.system === "only", "单段 system 仍是字符串");

console.log("[4] 命中缓存的 usage 仍计入 prompt");
const usage = normalizeUsage({ input_tokens: 10, cache_read_input_tokens: 5, output_tokens: 2 });
ok(usage.prompt_tokens === 15 && usage.completion_tokens === 2, "cache_read 计入 prompt");

console.log(`\n结果: ${pass} 通过 / 0 失败`);
