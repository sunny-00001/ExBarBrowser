// approval-policy.js - Web 面板三种执行模式的审批判据。
//
// 判据只吃 tools-exbar.ApprovalOf() 的结果，不自己查命令表：
//   known=false        → 未知工具，fail closed，必须问
//   needed=true        → destructiveHint，无论什么模式都必须人工确认
//   readOnly=true      → 纯读，任何模式都不问
//   其余（普通写）      → confirm 模式问；auto 模式不问
//
// 这里没有 "auto 自动批准危险命令" 这一档。会话级免确认不是逐次人工批准，
// 宿主侧危险双门（aiAllowDanger + approve=true）也不会认它。
"use strict";

function isMutating(meta) {
  if (!meta || meta.known === false) return true;
  return meta.readOnly !== true;
}

function shouldAskApproval(meta, autoApprove) {
  if (!meta || meta.known === false) return true;
  if (meta.needed === true) return true;      // danger 是硬门，模式不能绕过
  if (meta.readOnly === true) return false;
  return autoApprove !== true;
}

module.exports = { isMutating, shouldAskApproval };
