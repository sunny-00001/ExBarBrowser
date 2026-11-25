// project-knowledge.js - 把仓库文档前置成模型可直接读的上下文。
//
// 为什么要这一层：
//   模型每次靠 tool.list / 读 AGENTS.md 现场补课，既慢又烧 token，而且
//   同一个问题每次拿到的片段可能不同。这里做三件事：
//     1) 启动时把核心文档切成 section 索引（按 mtime 自动失效）；
//     2) 每轮把一组固定的"工程硬规则"压成常驻 system 片段；
//     3) 按当前提问做本地关键词召回，把相关 section 再补一小段。
//   全程纯 Node、零新依赖；索引只读仓库文档，不改写任何文件。
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..", "..");

// 文档源。顺序即优先级：冲突时前面的文件胜出（AGENTS > ARCHITECTURE-RULES
// > DECISIONS > ROADMAP > CHANGELOG）。README/CONTRIBUTING 只做召回，不进常驻卡。
const SOURCES = [
  { file: "AGENTS.md", weight: 6 },
  { file: "docs/ARCHITECTURE-RULES.md", weight: 5 },
  { file: "docs/DECISIONS.md", weight: 4 },
  { file: "docs/ROADMAP.md", weight: 3 },
  { file: "CHANGELOG.md", weight: 2 },
  { file: "README.md", weight: 1 },
  { file: "CONTRIBUTING.md", weight: 1 }
];

// 常驻卡：只取这些标题。每项给出标题正则与字符上限，避免把 500 行命令速查
// 整段塞进 prompt。常驻总量另有硬上限。
const PINNED = [
  // 先放"任何一轮都可能用到"的硬规则，再放决策与当前状态；架构细节排最后，
  // 超预算时由 retrieve() 按问题召回，而不是挤掉 R21 / 面板形态这类常驻事实。
  { file: "AGENTS.md", re: /^0\./, max: 450 },
  { file: "AGENTS.md", re: /^1\. 三条不可协商/, max: 320 },
  { file: "AGENTS.md", re: /^1\.1 R21/, max: 560 },
  { file: "AGENTS.md", re: /^1\.2 不引入/, max: 280 },
  { file: "AGENTS.md", re: /^1\.3 视觉/, max: 280 },
  { file: "AGENTS.md", re: /^3\. 代码规则/, max: 520 },
  { file: "AGENTS.md", re: /^4\. 命名与命令契约/, max: 520 },
  { file: "AGENTS.md", re: /^6\. 坑位/, max: 380 },
  { file: "AGENTS.md", re: /^7\. 收工判据/, max: 320 },
  { file: "docs/DECISIONS.md", re: /^D13\b/, max: 380 },
  { file: "docs/DECISIONS.md", re: /^D14\b/, max: 380 },
  { file: "docs/DECISIONS.md", re: /^D15\b/, max: 420 },
  { file: "docs/ROADMAP.md", re: /^0\. 三条原则/, max: 280 },
  { file: "docs/ROADMAP.md", re: /^1\. 阶段总览/, max: 420 },
  { file: "docs/ROADMAP.md", re: /^5\.1 P4-a/, max: 400 },
  { file: "docs/ROADMAP.md", re: /^5\.2 P4-b/, max: 380 },
  { file: "docs/ROADMAP.md", re: /^7\. 里程碑/, max: 380 },
  { file: "CHANGELOG.md", re: /^\[Unreleased\]$/, max: 750 },
  { file: "docs/DECISIONS.md", re: /^D1\b/, max: 260 },
  { file: "docs/DECISIONS.md", re: /^D2\b/, max: 260 },
  { file: "docs/DECISIONS.md", re: /^D3\b/, max: 260 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^1\. 对象模型/, max: 320 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^2\.2 依赖方向/, max: 320 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^3\. 硬规则/, max: 400 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^4\. 命名规则/, max: 260 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^5\. 命令契约/, max: 380 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^7\. 代码规则/, max: 320 },
  { file: "docs/ARCHITECTURE-RULES.md", re: /^10\. 变更流程/, max: 300 }
];

const MAX_CARD = 7600;
const MAX_RETRIEVED = 2400;
const MAX_SECTION = 750;

let cache = null;

function splitSections(text, file, weight) {
  const lines = String(text || "").split(/\r?\n/);
  const out = [];
  let cur = { heading: "", level: 0, lines: [] };
  for (const line of lines) {
    const m = line.match(/^(#{1,6})\s+(.*)$/);
    if (m) {
      if (cur.lines.length) out.push(finish(cur, file, weight));
      cur = { heading: m[2].trim(), level: m[1].length, lines: [] };
    } else {
      cur.lines.push(line);
    }
  }
  if (cur.lines.length) out.push(finish(cur, file, weight));
  return out;
}

function finish(sec, file, weight) {
  // 去掉 fenced code block：命令速查里的长代码块对语义召回噪声很大，
  // 真正的命令参数由 TOOL_SCHEMAS 提供，那里才是活的真相。
  const body = sec.lines.join("\n")
    .replace(/```[\s\S]*?```/g, "")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
  return {
    file, weight, heading: sec.heading || "(文档开头)", level: sec.level,
    text: `${sec.heading ? sec.heading + "\n" : ""}${body}`,
    search: `${sec.heading}\n${body}`.toLowerCase()
  };
}

function stampOf(file) {
  try {
    const st = fs.statSync(path.join(ROOT, file));
    return `${st.mtimeMs}:${st.size}`;
  } catch (e) {
    return "missing";
  }
}

function buildIndex() {
  const stamp = SOURCES.map((s) => `${s.file}|${stampOf(s.file)}`).join("\n");
  if (cache && cache.stamp === stamp) return cache;

  const sections = [];
  for (const src of SOURCES) {
    let text = "";
    try { text = fs.readFileSync(path.join(ROOT, src.file), "utf8"); }
    catch (e) { continue; }
    for (const s of splitSections(text, src.file, src.weight)) sections.push(s);
  }

  const cardParts = [];
  let cardLen = 0;
  for (const pin of PINNED) {
    if (cardLen >= MAX_CARD) break;
    const hit = sections.find((s) => s.file === pin.file && pin.re.test(s.heading));
    if (!hit) continue;
    const piece = `[${pin.file} · ${hit.heading}]\n${compact(hit.text, pin.max)}`;
    const sep = cardParts.length ? 2 : 0;   // join("\n\n") 也要算进预算
    if (cardLen + sep + piece.length > MAX_CARD) break;
    cardParts.push(piece);
    cardLen += sep + piece.length;
  }

  cache = {
    stamp, sections,
    card: cardParts.join("\n\n"),
    files: SOURCES.length,
    bytes: sections.reduce((n, s) => n + s.text.length, 0)
  };
  return cache;
}

function compact(text, max) {
  const t = String(text || "").replace(/\r/g, "").trim();
  if (t.length <= max) return t;
  return t.slice(0, max).replace(/\n[^\n]*$/, "") + ` …[已压缩，原文 ${t.length} 字]`;
}

// 中文按 bigram，英文/数字按词。没有第三方分词依赖，够做标题/正文命中。
function termsOf(text) {
  const raw = String(text || "").toLowerCase();
  const out = new Set();
  for (const w of raw.match(/[a-z0-9_.-]{2,}/g) || []) out.add(w);
  const han = raw.replace(/[^一-龥]+/g, " ");
  for (const chunk of han.split(/\s+/)) {
    if (chunk.length === 1) out.add(chunk);
    for (let i = 0; i + 1 < chunk.length; i++) out.add(chunk.slice(i, i + 2));
  }
  return [...out].filter((x) => x && x.length <= 16);
}

function scoreSection(sec, terms) {
  let score = 0;
  const head = sec.heading.toLowerCase();
  for (const t of terms) {
    if (head.includes(t)) score += 6;
    else if (sec.search.includes(t)) score += 1;
  }
  // 同分时优先权威文件，其次短节（信息密度高）。
  return score * 100 + sec.weight * 10 - Math.min(sec.text.length, 3000) / 1000;
}

function retrieve(query, topK, maxChars) {
  const k = Math.max(0, Number(topK) || 3);
  const cap = Math.max(0, Number(maxChars) || MAX_RETRIEVED);
  if (!k || !cap) return [];
  const terms = termsOf(query);
  if (!terms.length) return [];
  const idx = buildIndex();
  const scored = idx.sections
    .map((s) => ({ s, n: scoreSection(s, terms) }))
    .filter((x) => x.n > 100)
    .sort((a, b) => b.n - a.n);

  const out = [];
  let used = 0;
  for (const { s } of scored) {
    const room = cap - used;
    if (room <= 80) break;
    const piece = `[${s.file} · ${s.heading}]\n${compact(s.text, Math.min(MAX_SECTION, room))}`;
    out.push(piece);
    used += piece.length;
    if (out.length >= k) break;
  }
  return out;
}

// 给 system message 用。拆成常驻/按题召回两段，调用方可以把前者作为
// prompt-cache 的稳定前缀，后者每轮随问题变化。
function knowledgeCard() {
  const idx = buildIndex();
  return idx.card
    ? `\n\nPROJECT KNOWLEDGE (local index of this repository; authoritative for rules/status; do not spend a tool call to re-read docs unless the user asks for verbatim/latest text)\n${idx.card}`
    : "";
}

function knowledgeHits(query) {
  const hits = retrieve(query, 3, MAX_RETRIEVED);
  return hits.length ? `\n\nRELEVANT SECTIONS FOR THIS QUESTION\n${hits.join("\n\n")}` : "";
}

function knowledgePrompt(query) {
  return knowledgeCard() + knowledgeHits(query);
}

function knowledgeStatus() {
  const idx = buildIndex();
  return {
    files: idx.files,
    sections: idx.sections.length,
    bytes: idx.bytes,
    cardChars: idx.card.length,
    stamp: idx.stamp
  };
}

module.exports = {
  knowledgePrompt, knowledgeCard, knowledgeHits,
  knowledgeStatus, retrieve, buildIndex
};
