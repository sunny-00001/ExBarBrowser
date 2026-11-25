// turn-state.js - 回合身份与同会话串行化。
//
// 为什么单独成模块：
//   1. 服务端 SSE 需要单调 turnId。旧实现从历史里的 user 条数推算，历史一截断
//      （MAX_HISTORY_MSGS）下一个回合就会重新拿回已经用过的编号；活动流和
//      内联台账于是把两次不同的对话合并成"第 N 轮"。
//   2. 同一个 session 的两个 /api/chat 不能同时读同一份历史快照。一个停在审批卡、
//      另一个已经开始写回，后写的那个会静默覆盖前一个回合。
//
// 这里把两件事放在一起：turnId 的分配必须发生在拿到会话锁之后，否则排队中的
// 两个请求仍可能拿到同一个编号。

"use strict";

const fs = require("fs");
const path = require("path");

function defaultSafeId(s) {
  return String(s || "default").replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 80) || "default";
}

function createTurnState(dir, opts) {
  const options = opts || {};
  const safeId = options.safeId || defaultSafeId;
  const RUNS = new Map();
  // 元数据写盘失败时至少保证本次进程内不再复用编号；落盘成功与否由调用方日志体现。
  const FALLBACK = new Map();

  const ensureDir = () => fs.mkdirSync(dir, { recursive: true });
  const metaPath = (sessionId) => path.join(dir, safeId(sessionId) + ".turn.json");
  const historyPath = (sessionId) => path.join(dir, safeId(sessionId) + ".json");

  function readJson(p) {
    try { return JSON.parse(fs.readFileSync(p, "utf8")); }
    catch (e) { return null; }
  }

  // 从旧历史迁移：还没有 .turn.json 的会话，按历史里出现过的最大 turn 续号。
  function maxTurnFromHistory(sessionId) {
    const arr = readJson(historyPath(sessionId));
    if (!Array.isArray(arr)) return 0;
    let max = 0;
    for (const m of arr) {
      const raw = m && (m.turnId !== undefined ? m.turnId : m.turn);
      const n = Number(raw);
      if (Number.isFinite(n) && n > max) max = Math.floor(n);
    }
    return max;
  }

  function currentTurnId(sessionId) {
    const fallback = Number(FALLBACK.get(sessionId));
    if (Number.isFinite(fallback) && fallback >= 1) return fallback - 1;
    const meta = readJson(metaPath(sessionId));
    const stored = Number(meta && (meta.nextTurn !== undefined ? meta.nextTurn : meta.currentTurn));
    if (Number.isFinite(stored) && stored >= 1) return Math.floor(stored) - 1;
    return maxTurnFromHistory(sessionId);
  }

  function nextTurnId(sessionId) {
    ensureDir();
    const next = currentTurnId(sessionId) + 1;
    const p = metaPath(sessionId);
    const payload = JSON.stringify({ nextTurn: next + 1, updatedAt: Date.now() });
    const tmp = p + ".tmp-" + process.pid + "-" + Math.random().toString(36).slice(2, 8);
    try {
      fs.writeFileSync(tmp, payload, "utf8");
      try { fs.renameSync(tmp, p); }
      catch (e) {
        // Windows 上个别文件系统对覆盖 rename 敏感：删掉旧文件再试一次。
        try { fs.unlinkSync(p); } catch (e2) { }
        fs.renameSync(tmp, p);
      }
      FALLBACK.delete(sessionId);
    } catch (e) {
      try { fs.unlinkSync(tmp); } catch (e2) { }
      FALLBACK.set(sessionId, next + 1);
    }
    return next;
  }

  // 同一 session 的 fn 严格串行：后一个要等前一个的 promise settle 才开始。
  // 不同 session 互不阻塞。fn 抛错不算死锁，锁一定会释放。
  function withTurnRun(sessionId, fn) {
    const key = String(sessionId || "default");
    const prev = RUNS.get(key) || Promise.resolve();
    let begin;
    const ready = new Promise((resolve) => { begin = resolve; });
    const run = prev.catch(() => undefined).then(() => ready).then(fn);
    const tail = run.then(() => undefined, () => undefined);
    RUNS.set(key, tail);
    tail.then(() => { if (RUNS.get(key) === tail) RUNS.delete(key); });
    begin();
    return run;
  }

  function isBusy(sessionId) {
    return RUNS.has(String(sessionId || "default"));
  }

  return { nextTurnId, currentTurnId, withTurnRun, isBusy, metaPath, historyPath };
}

module.exports = { createTurnState, defaultSafeId };
