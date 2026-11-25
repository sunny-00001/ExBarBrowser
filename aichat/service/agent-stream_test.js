// agent-stream_test.js - 流式 tool_call 合并的纯函数自测。
// 重点覆盖中转反复下发完整值、以及真正分片两种形状。
"use strict";

const assert = require("node:assert/strict");
const { mergeToolCallChunk } = require("./agent-stream");

let pass = 0;
const blank = () => ({ id: "", type: "function", function: { name: "", arguments: "" } });

function nameCase(label, chunks, expected) {
  const slot = blank();
  for (const c of chunks) mergeToolCallChunk(slot, c);
  assert.equal(slot.function.name, expected, label);
  pass++;
}
function argsCase(label, chunks, expected) {
  const slot = blank();
  for (const c of chunks) mergeToolCallChunk(slot, c);
  assert.deepEqual(JSON.parse(slot.function.arguments), expected, label);
  pass++;
}

// 名字：真分片要拼；中转重发完整前缀/完整值不能拼重复。
nameCase("正常分片", [{ name: "nav." }, { name: "up" }], "nav.up");
nameCase("重发累计前缀", [{ name: "nav." }, { name: "nav.up" }], "nav.up");
nameCase("重发完整值", [{ name: "nav.up" }, { name: "nav.up" }], "nav.up");
nameCase("JSON 字符串完整值", [{ name: '"nav.up"' }, { name: "nav.up" }], "nav.up");
nameCase("id 保留", [{ id: "call-1", name: "view.sort" }], "view.sort");

// 参数：半截 JSON 要拼；完整 JSON 重发要覆盖。
argsCase("参数分片", [{ arguments: '{"by":' }, { arguments: '3}' }], { by: 3 });
argsCase("参数完整值重发", [{ arguments: '{"by":3}' }, { arguments: '{"by":3}' }], { by: 3 });
argsCase("参数先分片后完整值", [
  { arguments: '{"by":' }, { arguments: '{"by":3}' }
], { by: 3 });

console.log(`agent-stream_test: ${pass} checks passed`);
