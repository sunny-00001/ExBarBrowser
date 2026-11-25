// branch-state.js - 每个会话"哪个尝试是当前分支"的持久选择。
//
// turnId 是身份，attempt 只是默认顺序。用户切回旧版本后，不能靠"最大 attempt"
// 再算一遍，否则下一次 send 又跳回最新尝试。这里保存 rootTurnId -> selectedTurnId。
"use strict";

const fs = require("fs");
const path = require("path");
const { defaultSafeId } = require("./turn-state");

function num(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
}

function createBranchState(dir, opts) {
  const options = opts || {};
  const safeId = options.safeId || defaultSafeId;
  const CACHE = new Map();
  const fileFor = (sessionId) => path.join(dir, safeId(sessionId) + ".branches.json");

  function read(sessionId) {
    const key = String(sessionId || "default");
    if (CACHE.has(key)) return CACHE.get(key);
    let selected = {};
    try {
      const raw = JSON.parse(fs.readFileSync(fileFor(key), "utf8"));
      const src = raw && typeof raw.selected === "object" && raw.selected ? raw.selected : raw;
      if (src && typeof src === "object") {
        for (const [root, turn] of Object.entries(src)) {
          const r = num(root), t = num(turn);
          if (r && t) selected[String(r)] = t;
        }
      }
    } catch (e) { selected = {}; }
    CACHE.set(key, selected);
    return selected;
  }

  function all(sessionId) {
    return { ...read(sessionId) };
  }

  function activeFor(sessionId, rootTurnId) {
    const root = num(rootTurnId);
    if (!root) return 0;
    return num(read(sessionId)[String(root)]);
  }

  function select(sessionId, rootTurnId, turnId) {
    const root = num(rootTurnId), turn = num(turnId);
    if (!root || !turn) return false;
    fs.mkdirSync(dir, { recursive: true });
    const selected = { ...read(sessionId), [String(root)]: turn };
    const p = fileFor(sessionId);
    const payload = JSON.stringify({ selected, updatedAt: Date.now() }, null, 2);
    const tmp = p + ".tmp-" + process.pid + "-" + Math.random().toString(36).slice(2, 8);
    try {
      fs.writeFileSync(tmp, payload, "utf8");
      try { fs.renameSync(tmp, p); }
      catch (e) {
        try { fs.unlinkSync(p); } catch (e2) { }
        fs.renameSync(tmp, p);
      }
      CACHE.set(String(sessionId || "default"), selected);
      return true;
    } catch (e) {
      try { fs.unlinkSync(tmp); } catch (e2) { }
      return false;
    }
  }

  function clear(sessionId) {
    CACHE.delete(String(sessionId || "default"));
    try { fs.unlinkSync(fileFor(sessionId)); } catch (e) { }
  }

  return { all, activeFor, select, clear, fileFor };
}

module.exports = { createBranchState };
