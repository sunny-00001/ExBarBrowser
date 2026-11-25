// 回合版本基础：历史里保留旧尝试，但模型上下文只看当前活动分支。
// 这是一个纯数据模块：不写文件、不发命令、不碰 UI。
"use strict";

function num(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n : 0;
}

function metaOf(entry) {
  if (!entry || (entry.role !== "user" && entry.role !== "assistant")) {
    return { turnId: 0, rootTurnId: 0, attempt: 0, tool: false };
  }
  const turnId = num(entry.turnId) || num(entry.turn);
  return {
    turnId,
    rootTurnId: num(entry.rootTurnId) || turnId,
    attempt: num(entry.attempt) || (turnId ? 1 : 0),
    parentTurnId: num(entry.parentTurnId),
    tool: false
  };
}

function selectionGet(selection, root) {
  if (!selection) return 0;
  if (typeof selection.get === "function") return num(selection.get(root));
  return num(selection[root]);
}

function turnMap(messages) {
  const list = Array.isArray(messages) ? messages : [];
  const turns = new Map();
  for (const m of list) {
    if (!m || m.role !== "user") continue;
    const meta = metaOf(m);
    if (meta.turnId) turns.set(meta.turnId, { ...meta, content: String(m.content || "") });
  }
  return turns;
}

// 同一个 root 的多个尝试里默认只取最大 attempt；显式 selection 可以选中旧版本。
// 仅挑中 root 还不够：若后续回合挂在未选中的兄弟尝试下，也必须随父链一起排除，
// 否则“切回旧版本”会带着另一条分支的后半截历史。
function activeHistory(messages, selection) {
  const list = Array.isArray(messages) ? messages : [];
  const turns = turnMap(list);
  const byRoot = new Map();
  for (const meta of turns.values()) {
    if (!byRoot.has(meta.rootTurnId)) byRoot.set(meta.rootTurnId, []);
    byRoot.get(meta.rootTurnId).push(meta);
  }
  const preferred = new Map();
  for (const [root, metas] of byRoot) {
    const selected = selectionGet(selection, root);
    const chosen = selected && metas.some((m) => m.turnId === selected)
      ? metas.find((m) => m.turnId === selected)
      : metas.reduce((best, m) =>
          !best || m.attempt > best.attempt || (m.attempt === best.attempt && m.turnId > best.turnId)
            ? m : best, null);
    preferred.set(root, chosen ? chosen.turnId : 0);
  }

  const enabled = new Set();
  for (const meta of turns.values()) {
    if (preferred.get(meta.rootTurnId) !== meta.turnId) continue;
    const parent = meta.parentTurnId;
    // 老历史没有 parentTurnId；截断后父记录不在文件里也不该让子回合凭空消失。
    if (parent && turns.has(parent) && !enabled.has(parent)) continue;
    enabled.add(meta.turnId);
  }

  return list.filter((m) => {
    const meta = metaOf(m);
    return !meta.turnId || enabled.has(meta.turnId);
  });
}

function lastTurnId(messages) {
  const list = Array.isArray(messages) ? messages : [];
  for (let i = list.length - 1; i >= 0; i--) {
    const meta = metaOf(list[i]);
    if (list[i] && list[i].role === "user" && meta.turnId) return meta.turnId;
  }
  return 0;
}

function findTurn(messages, targetId) {
  const id = num(targetId);
  if (!id) return null;
  const list = Array.isArray(messages) ? messages : [];
  for (let i = 0; i < list.length; i++) {
    const m = list[i];
    if (m && m.role === "user" && (num(m.turnId) || num(m.turn)) === id) {
      return { index: i, user: m, meta: metaOf(m) };
    }
  }
  return null;
}

// 重试/编辑不是修改旧回合，而是从它之前的历史点分叉出一个新回合。
function lineageFor(messages, targetId) {
  const found = findTurn(messages, targetId);
  if (!found || !found.meta.turnId) return null;
  const rootTurnId = found.meta.rootTurnId;
  let maxAttempt = found.meta.attempt;
  for (const m of (Array.isArray(messages) ? messages : [])) {
    const meta = metaOf(m);
    if (meta.rootTurnId === rootTurnId && meta.attempt > maxAttempt) {
      maxAttempt = meta.attempt;
    }
  }
  return {
    targetTurnId: found.meta.turnId,
    rootTurnId,
    attempt: maxAttempt + 1,
    retryOf: found.meta.turnId,
    parentTurnId: found.meta.parentTurnId,
    prompt: String(found.user.content || "")
  };
}

// 分叉点之前的历史仍要经过“只取活动分支”过滤，否则会把已被取代的旧尝试
// 重新带进模型上下文。
function contextForRetry(messages, targetId, selection) {
  const found = findTurn(messages, targetId);
  if (!found) return null;
  return activeHistory((Array.isArray(messages) ? messages : []).slice(0, found.index), selection);
}

function branchList(messages, selection) {
  const turns = turnMap(messages);
  const groups = new Map();
  for (const meta of turns.values()) {
    if (!groups.has(meta.rootTurnId)) groups.set(meta.rootTurnId, []);
    groups.get(meta.rootTurnId).push(meta);
  }
  const out = [];
  for (const [rootTurnId, metas] of groups) {
    const selected = selectionGet(selection, rootTurnId);
    const chosen = selected && metas.some((m) => m.turnId === selected)
      ? selected
      : metas.reduce((best, m) =>
          !best || m.attempt > best.attempt || (m.attempt === best.attempt && m.turnId > best.turnId)
            ? m.turnId : best, 0);
    out.push({
      rootTurnId,
      selectedTurnId: chosen,
      attempts: metas
        .map((m) => ({
          turnId: m.turnId,
          attempt: m.attempt,
          parentTurnId: m.parentTurnId,
          prompt: m.content
        }))
        .sort((a, b) => a.attempt - b.attempt || a.turnId - b.turnId)
    });
  }
  return out.sort((a, b) => a.rootTurnId - b.rootTurnId);
}

function canSelectBranch(messages, rootId, turnId) {
  const id = num(turnId);
  if (!id) return false;
  const target = turnMap(messages).get(id);
  if (!target) return false;
  const root = num(rootId);
  return !root || target.rootTurnId === root;
}

function markSuperseded(messages, lineage, newTurnId) {
  const root = num(lineage && lineage.rootTurnId) || num(lineage && lineage.targetTurnId);
  const next = num(newTurnId);
  if (!root || !next) return 0;
  let changed = 0;
  for (const m of (Array.isArray(messages) ? messages : [])) {
    const meta = metaOf(m);
    if (meta.rootTurnId !== root || meta.turnId === next) continue;
    if (!m.supersededBy) { m.supersededBy = next; changed++; }
  }
  return changed;
}

module.exports = {
  num,
  metaOf,
  activeHistory,
  lastTurnId,
  findTurn,
  lineageFor,
  contextForRetry,
  branchList,
  canSelectBranch,
  markSuperseded
};
