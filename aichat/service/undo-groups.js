// 撤销组数据模型（P1：先定契约，不接 fileundo::Stack）。
//
// 为什么先做纯模型：AI 一次任务常常是「复制 → 重命名 → 移动」这种多步写，
// 单步撤销会把一次逻辑操作拆成多次 Ctrl+Z。但当前 fileundo::Stack 只认一条
// 记录一步撤销，书签栈又是另一套实现 —— 在没有组模型之前就把 AI 的 undo
// 接上去，只会把「一次任务」错误地映射成「N 次撤销」。
//
// 本模块只做四件可测的事：组 ID 校验、步骤准入、关闭组、生成逆序撤销计划。
// 不发命令、不写文件、不碰 UI；真正的 Apply 由后续接线提供。
"use strict";

const GROUP_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$/;
const MAX_STEPS = 20;                 // 与 fileundo::Stack::kMax 同口径
const DOMAINS = new Set(["file", "bookmark"]);

function IsValidGroupId(id) {
  return typeof id === "string" && GROUP_ID_RE.test(id);
}

function IsValidDomain(domain) {
  return typeof domain === "string" && DOMAINS.has(domain);
}

// metadata 来自 tools/list 的 annotations：undoable + undoGroup + undoDomain。
// 三者必须同时成立才允许加入，避免 AI 凭命令名猜域。
function CanJoin(metadata, groupId, domain) {
  if (!metadata) return { ok: false, reason: "工具元数据缺失" };
  if (metadata.undoable !== true) return { ok: false, reason: "命令不可撤销" };
  const policy = String(metadata.undoGroup || "none");
  if (policy !== "join") return { ok: false, reason: "命令未声明 undoGroup=join" };
  if (!IsValidGroupId(groupId)) return { ok: false, reason: "撤销组 ID 非法" };
  if (!IsValidDomain(domain)) return { ok: false, reason: "撤销组域非法" };
  if (metadata.undoDomain !== undefined && metadata.undoDomain !== domain) {
    return { ok: false, reason: "命令的撤销域与组不一致" };
  }
  return { ok: true, reason: "" };
}

function CreateGroup(groupId, domain, now = Date.now()) {
  if (!IsValidGroupId(groupId)) throw new Error("撤销组 ID 非法");
  if (!IsValidDomain(domain)) throw new Error("撤销组域非法");
  return {
    id: groupId,
    domain,
    createdAt: now,
    closed: false,
    committed: false,
    closeReason: "",
    steps: []
  };
}

// step: { command, argsHash, undoRef, undoable, byMachine, ts }
//   · undoRef 是后续撤销栈里的记录引用（现在先占位，不解释）；
//   · byMachine=false 的步骤不允许被机器整组撤销（同 fileundo::Stack 的边界）。
function AddStep(group, step) {
  if (!group || group.closed) return { ok: false, reason: "撤销组已关闭" };
  if (!step || typeof step.command !== "string" || !step.command) {
    return { ok: false, reason: "步骤缺少 command" };
  }
  if (typeof step.argsHash !== "string" || !step.argsHash) {
    return { ok: false, reason: "步骤缺少 argsHash" };
  }
  if (step.undoable !== true) return { ok: false, reason: "不可撤销的步骤不能入组" };
  if (typeof step.undoRef !== "string" || !step.undoRef) {
    return { ok: false, reason: "步骤缺少 undoRef" };
  }
  if (group.steps.length >= MAX_STEPS) {
    return { ok: false, reason: `撤销组最多 ${MAX_STEPS} 步` };
  }
  if (group.steps.some((s) => s.undoRef === step.undoRef)) {
    return { ok: false, reason: "undoRef 重复" };
  }
  const byMachine = step.byMachine !== false;
  group.steps.push({
    command: step.command,
    argsHash: step.argsHash,
    undoRef: step.undoRef,
    undoable: true,
    byMachine,
    ts: Number.isFinite(step.ts) ? step.ts : Date.now()
  });
  return { ok: true, reason: "" };
}

// commit=false 用于"多步里某一步失败，调用方选择保留已做的结果"：
// 保留步骤用于审计，但不生成撤销计划。
function CloseGroup(group, commit = true, reason = "") {
  if (!group || group.closed) return { ok: false, reason: "撤销组已关闭" };
  group.closed = true;
  group.committed = commit !== false;
  group.closeReason = String(reason || "");
  return { ok: true, reason: "" };
}

// 逆序撤销计划。机器代撤时遇到人工步骤整组拒绝，不做"撤一半"。
function UndoPlan(group, machineCaller = true) {
  if (!group || !group.closed || !group.committed) return [];
  if (!group.steps.length) return [];
  if (machineCaller && group.steps.some((s) => !s.byMachine)) return [];
  if (group.steps.some((s) => !s.undoable || !s.undoRef)) return [];
  return group.steps.slice().reverse().map((s, i) => ({
    index: i,
    command: s.command,
    argsHash: s.argsHash,
    undoRef: s.undoRef
  }));
}

module.exports = {
  MAX_STEPS,
  IsValidGroupId,
  IsValidDomain,
  CanJoin,
  CreateGroup,
  AddStep,
  CloseGroup,
  UndoPlan
};
