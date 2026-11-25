// project-knowledge.js 的离线回归：只读仓库文档，断言常驻卡存在、召回命中、
// 缓存按 mtime 生效。不发网络，不碰窗口。
"use strict";
const assert = require("assert");
const k = require("./project-knowledge");

let pass = 0;
function ok(cond, what) {
  assert.ok(cond, what);
  pass++;
  console.log("  OK", what);
}

const status = k.knowledgeStatus();
console.log("[1] 索引");
ok(status.files >= 5, "至少索引 5 份核心文档");
ok(status.sections > 30, `切出 ${status.sections} 个 section`);
ok(status.cardChars > 500 && status.cardChars <= 7600, `常驻卡 ${status.cardChars} 字，在预算内`);
ok(k.buildIndex() === k.buildIndex(), "同一批 mtime 复用同一份缓存");

console.log("[2] 常驻卡内容");
const card = k.knowledgePrompt("");
ok(card.includes("PROJECT KNOWLEDGE"), "system 片段有明确来源声明");
ok(card.includes("R21"), "R21 输入纪律常驻");
ok(card.includes("D13") || card.includes("形态 A"), "AI 客户端形态决策常驻");
ok(card.includes("Unreleased") || card.includes("活动台账") || card.includes("执行台账"),
  "当前未发布状态常驻");

console.log("[3] 本地召回");
ok(k.knowledgeCard() === k.knowledgeCard("ignored"), "常驻卡与提问无关，可作 cache key");
ok(k.knowledgeHits("R21 输入纪律") !== k.knowledgeHits("WebView2 D14"), "召回段随提问变化");
const r21 = k.retrieve("R21 输入纪律 禁止模拟鼠标", 3, 2400);
ok(r21.length > 0 && r21.some((x) => x.includes("R21")), "R21 提问召回输入纪律");
const wv = k.retrieve("WebView2 D14 面板渲染", 3, 2400);
ok(wv.some((x) => x.includes("D14") || x.includes("WebView2")), "WebView2 提问召回 D14");
const cmd = k.retrieve("命令命名 点分小写 legacyName", 2, 1600);
ok(cmd.some((x) => x.includes("命令") || x.includes("legacyName")), "命令契约提问召回命名规则");
ok(k.retrieve("", 3, 2400).length === 0, "空提问不召回");

console.log(`\n结果: ${pass} 通过 / 0 失败`);
