// server_security_test.js - 本地 HTTP 安全边界的纯函数自测。
// 不监听端口；只验证 Host / Origin / token / 请求体上限的判据。
"use strict";

process.env.EXBAR_HTTP_TOKEN = "test-http-token-9f2c";

const assert = require("node:assert/strict");
const { config } = require("./config");
const { security, readJson, historyForChat } = require("./server");

let pass = 0;
const ok = (value, label) => { assert.ok(value, label); pass++; };
const eq = (a, b, label) => { assert.equal(a, b, label); pass++; };
const req = (headers) => ({ headers });
const url = (query = "") => ({ searchParams: new URLSearchParams(query) });

// Host：防 DNS rebinding / 跨端口页面。
ok(security.loopbackName("127.0.0.1"), "IPv4 loopback");
ok(security.loopbackName("[::1]"), "IPv6 loopback 去括号");
ok(!security.loopbackName("evil.example"), "外站不是 loopback");
ok(security.hostAllowed(req({ host: `127.0.0.1:${config.port}` })), "本机正确端口");
ok(security.hostAllowed(req({ host: "localhost" })), "无端口 localhost");
ok(!security.hostAllowed(req({ host: `127.0.0.1:${Number(config.port) + 1}` })), "本机错误端口");
ok(!security.hostAllowed(req({ host: "evil.example" })), "外站 Host");

// Origin：浏览器跨站请求必须失败；无 Origin 的 curl / native 仍走 token 门。
ok(security.originAllowed(req({})), "无 Origin");
ok(security.originAllowed(req({ origin: `http://127.0.0.1:${config.port}` })), "同源页面");
ok(!security.originAllowed(req({ origin: `https://127.0.0.1:${config.port}` })), "https 页面");
ok(!security.originAllowed(req({ origin: "http://evil.example" })), "跨站页面");
ok(!security.originAllowed(req({ origin: `http://127.0.0.1:${Number(config.port) + 1}` })), "跨端口页面");
ok(!security.originAllowed(req({ origin: "not-a-url" })), "畸形 Origin");

// token：header 与 URL query 都接受，错的都拒绝。
ok(security.tokenAllowed(req({ "x-exbar-token": "test-http-token-9f2c" }), url()), "正确 header");
ok(security.tokenAllowed(req({}), url("?exbarHttpToken=test-http-token-9f2c")), "正确 query");
ok(!security.tokenAllowed(req({}), url()), "缺 token");
ok(!security.tokenAllowed(req({ "x-exbar-token": "wrong" }), url()), "错误 token");

// 回合版本上下文：普通 send 只看活动分支，retryOf 从分叉点之前取历史。
const versionedHistory = [
  { role: "user", content: "原始", turnId: 10, turn: 10 },
  { role: "assistant", content: "旧答", turnId: 10, turn: 10 },
  { role: "user", content: "之后", turnId: 11, turn: 11 }
];
const forked = historyForChat(versionedHistory, 10);
eq(forked.ok, true, "存在的 retryOf 可以分叉");
eq(forked.history.length, 0, "第一轮分叉没有前置上下文");
eq(forked.lineage.attempt, 2, "新回合是第二次尝试");
eq(historyForChat(versionedHistory, 999).ok, false, "不存在的 retryOf 被拒绝");
const plain = historyForChat(versionedHistory, 0);
eq(plain.history.length, 3, "普通 send 仍看完整活动历史");

// 切回旧版本：父链必须一起收窄，不能把另一条分支的后代留在上下文里。
const branched = [
  { role: "user", content: "旧尝试", turnId: 10, turn: 10, rootTurnId: 10, attempt: 1 },
  { role: "assistant", content: "旧答", turnId: 10, turn: 10, rootTurnId: 10, attempt: 1, tools: [] },
  { role: "user", content: "新尝试", turnId: 11, turn: 11, rootTurnId: 10, attempt: 2, retryOf: 10 },
  { role: "assistant", content: "新答", turnId: 11, turn: 11, rootTurnId: 10, attempt: 2, parentTurnId: 11, tools: [] },
  { role: "user", content: "后继", turnId: 12, turn: 12, rootTurnId: 12, parentTurnId: 11 },
  { role: "assistant", content: "后继答", turnId: 12, turn: 12, rootTurnId: 12, parentTurnId: 12, tools: [] }
];
const latest = historyForChat(branched, 0, {});
ok(latest.history.some((m) => m.turnId === 11), "默认保留最大 attempt");
ok(latest.history.some((m) => m.turnId === 12), "保留活动分支的后继回合");
ok(!latest.history.some((m) => m.turnId === 10), "旧尝试不进上下文");
const switched = historyForChat(branched, 0, { 10: 10 });
ok(switched.history.some((m) => m.turnId === 10), "选中旧版本后旧尝试回到上下文");
ok(!switched.history.some((m) => m.turnId === 11), "未选中的新尝试被排除");
ok(!switched.history.some((m) => m.turnId === 12), "跨分支的后继回合一并排除");

// body 上限：防止本地页面把大对象塞进 JSON 把服务打爆。
async function body(chunks) {
  const stream = {
    async *[Symbol.asyncIterator]() {
      for (const c of chunks) yield Buffer.isBuffer(c) ? c : Buffer.from(c);
    }
  };
  return readJson(stream);
}
(async () => {
  eq((await body(['{"text":"hi"}'])).text, "hi", "小 JSON 正常解析");
  await assert.rejects(() => body([Buffer.alloc(256 * 1024 + 1)]), /too large/);
  pass++;
  console.log(`server_security_test: ${pass} checks passed`);
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
