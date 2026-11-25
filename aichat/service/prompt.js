// prompt.js - the shared vocabulary of the agent loop.
//
// WHY THIS MODULE EXISTS
//   agent.js (classic AGENT/1) and agent-stream.js (SSE) were two copies of the
//   same loop. They drifted: only the streaming twin gained the approval gate.
//   The first step of unifying them is to extract everything that is not loop
//   control into one place, so a change cannot be made in one twin only.
//
//   It also breaks a circular import: agent-stream needs SYSTEM_PROMPT, and
//   agent.js needs needsApproval from agent-stream. With this module neither
//   twin imports the other for these helpers.

const SYSTEM_PROMPT = `You are the operator of ExBarBrowser, a Windows file manager (an alternative to Explorer). The user types instructions in plain language; you carry them out with structured commands and report back briefly.

HOW TO ACT
- Act through tools. Never claim something happened unless a tool returned success.
- Understand the intent FIRST. If one exact structured command satisfies it, call that command in your FIRST response. Do not narrate a plan, do not say "I will now...", and do not call unrelated tools to "think".
- For navigation/view/sort requests, do NOT call tool.list unless the answer depends on the current folder's contents or the user's selection. "上一级", "打开下载", "按大小排序" are direct commands.
- Tool names are dotted and domain-prefixed: nav.* (navigation), fs.* (file ops), tab.* (tabs),
  view.* (sorting/columns/preview), go.* (jump to known folders), bmk.* (bookmarks),
  tool.* (search/compare/hash/diskusage), session.* (workspace sessions), win.* (window geometry).
- Prefer the most specific command. Correct examples (arguments MUST match the tool schema):
  nav.up / nav.back / nav.forward / nav.refresh
  go.downloads / go.documents / go.desktop / go.home
  view.mode {"preset":5}        (5=details, 4=list, 1=large icons)
  view.sort {"by":3}            (0=name, 1=date, 2=type, 3=size)
  tab.new / tab.close / tab.activate
  file.select.pattern {"pattern":"*.jpg"}
  search.find {"query":"invoice"}
- Arguments are JSON objects with exactly the keys the tool's schema declares. Do not invent keys such as mode/key/desc when the schema says preset/by.
- One tool call is enough when one call is enough. Only batch calls that are truly independent.
- After a tool succeeds, STOP and answer. Do not start another model round just to "think more".

HARD LIMITS
- Never delete without being explicitly asked to delete. Moving is not deleting.
- If a request is ambiguous in a way that changes the outcome (which folder, overwrite or not,
  delete or move), ask ONE short question instead of guessing.
- Do not invent paths. If a path is unknown, list the parent folder first.
- If a tool returns an error, read it and adapt. Do not repeat the identical failing call.

STYLE
- Answer in the user's language.
- Final answer: at most 1-2 short sentences — what was done, and only anything the user must decide.
- Do not dump raw tool output. Summarise; mention counts and names, not every field.
- Never expose reasoning, internal plans, or "thinking". The activity view already shows every command.`;

function clip(s, n) {
  const t = String(s == null ? "" : s);
  return t.length <= n ? t : t.slice(0, n) + ` …[${t.length - n} chars trimmed]`;
}

// Tool results are serialized with a hard cap. A single readdir of a huge folder
// can otherwise consume the entire context window and the model stops reasoning
// about the actual request.
function summariseResult(obj) {
  try {
    const s = JSON.stringify(obj);
    if (s.length <= 8000) return s;
    return s.slice(0, 8000) + ` …[result trimmed, ${s.length} chars total]`;
  } catch (e) {
    return String(obj);
  }
}

function formatAction(name, args, result, ok) {
  // One short line per executed tool: what ran, and whether it worked. This is
  // what the panel prints under "ran:", so it must be readable at a glance.
  //
  // The old Opus-era branches (opus_run_command / opus_navigate / fs_delete with
  // a single `path`) were removed: after moving to ExBarBrowser's command bus the
  // tools are dotted and take argument objects, so those names can never match.
  // What remains is the generic shape: the command name plus its most specific
  // argument, which is what a reader needs to tell two calls apart.
  let what = name;
  const a = args || {};
  if (Array.isArray(a.targets) && a.targets.length) {
    what = `${name} ${a.targets.length === 1 ? a.targets[0] : `${a.targets.length} 项`}`;
  } else if (a.path) {
    what = `${name} ${a.path}`;
  } else if (a.query) {
    what = `${name} 「${a.query}」`;
  } else if (a.from && a.to) {
    what = `${name} ${a.from} → ${a.to}`;
  } else if (a.name) {
    what = `${name} ${a.name}`;
  }

  return ok ? what : `${what}  [FAILED]`;
}

// ---------------------------------------------------------------------------
// the approval gate rules (shared by both loops)
// ---------------------------------------------------------------------------

// 【判据不在本文件里】哪些工具需要人工确认，由命令表自己的
// `annotations.destructiveHint` 决定（见 tools-exbar.js 的 ApprovalOf）。
// 这里只做转发，避免再养一份会过期的手写清单。
//
// 为什么要保留这个函数而不是让调用方直接用 ApprovalOf：
//   agent.js / agent-stream.js 是两条循环，它们拿到的 ctx 可能没有连接
//   （例如模型服务没起来），此时 ApprovalOf 会 fail-closed 返回"需要确认"。
//   转发一次让两条循环共用同一个入口，规则就只可能在一个地方改。
function needsApproval(name, args) {
  return require("./tools-exbar").ApprovalOf(name).needed;
}

module.exports = {
  SYSTEM_PROMPT, clip, summariseResult, formatAction,
  needsApproval
};
