// 端到端：一句话 → 模型 → 工具调用 → ExBarBrowser 执行 → 结果回模型。
// 不经过 HTTP/UI，直接调 runTurnStream，把 SSE 事件打到控制台。
// 用法：node aichat/service/_e2e.js "列出我主目录里有多少个文件夹"
"use strict";

const { runTurnStream } = require("./agent-stream");
const toolsExbar = require("./tools-exbar");

const text = process.argv[2] || "我主目录里有哪些文件夹？只回答前 5 个名字。";

(async () => {
  console.log("=== 握手 ExBarBrowser ===");
  const ok = await toolsExbar.EnsureReady();
  if (!ok) { console.log("MCP 未就绪"); process.exit(1); }

  console.log(`\n=== 用户：${text} ===\n`);

  let lastAssistantText = "";
  const ac = new AbortController();
  const t0 = Date.now();
  let firstTokenAt = 0;

  const result = await runTurnStream(text, {
    sessionId: "e2e-" + Date.now(),
    history: [],
    autoApprove: true,          // 先不管审批
    signal: ac.signal,
    waitForApproval: async () => ({ decision: "approve" })
  }, (ev) => {
    switch (ev.type) {
      case "delta":
        if (!firstTokenAt) firstTokenAt = Date.now();
        lastAssistantText += ev.text;
        process.stdout.write(ev.text);
        break;
      case "thinking":
        process.stdout.write(`\x1b[90m${ev.text}\x1b[0m`);
        break;
      case "tool":
        console.log(`\n  [工具] ${ev.label || ev.name} → ${ev.status}` +
          (ev.error ? ` (${ev.error})` : ""));
        break;
      case "approval":
        console.log(`\n  [审批] ${ev.title}：${ev.details}`);
        break;
      case "error":
        console.log(`\n  [错误] ${ev.message}`);
        break;
      case "status":
        console.log(`\n  · ${ev.text}`);
        break;
      case "done":
        break;
      default:
        break;
    }
  });

  const ms = Date.now() - t0;
  console.log("\n\n=== 结果 ===");
  console.log("steps =", result.steps, "| 耗时 =", ms + "ms",
    "| 首字 =", firstTokenAt ? (firstTokenAt - t0) + "ms" : "n/a");
  console.log("工具调用 =", (result.tools || []).map(t => `${t.name}:${t.status}`).join(", ") || "（无）");
  if (result.error) console.log("error =", result.error);

  toolsExbar.Stop();
  process.exit(result.error ? 1 : 0);
})();