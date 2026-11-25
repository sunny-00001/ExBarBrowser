// exbar-context.js - 用 ExBarBrowser 的命令取"用户现在在看什么"。
//
// 【数据源有两条，各有长处】
//   `exbar/state`  —— 厂商扩展方法（非 MCP 标准），返回 activePath / tabs /
//                     选中集等**状态**。比 tool.list 便宜（不扫目录），
//                     且是"当前路径"的权威来源。
//   `tool.list`    —— 命令，返回当前目录的条目清单。要条目数才用它。
//
//   早期实现只用了 tool.list，结果拿不到 folder（它返回的是条目清单，
//   路径要额外解析，且视图未就绪时整条失败）。现在改成"状态优先、条目按需"。
"use strict";

const te = require("./tools-exbar");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 发一条厂商扩展方法（tools-exbar 的 dispatch 只转工具调用）。
async function callRaw(method, params) {
  const ok = await te.EnsureReady();
  if (!ok) return { ok: false, error: "没有连上任何 ExBarBrowser 窗口" };
  const client = te.RawClient();
  if (!client) return { ok: false, error: "客户端未就绪" };
  try {
    const res = await client.Request(method, params || {});
    return { ok: true, result: res };
  } catch (e) {
    return { ok: false, error: String(e && e.message ? e.message : e) };
  }
}

async function Probe() {
  const ok = await te.EnsureReady();
  const st = te.Status();
  return ok
    ? { ok: true, product: st.server || "exbarbrowser", pid: st.pid }
    : { ok: false, reason: "没有连上任何 ExBarBrowser 窗口" };
}

async function ContextLite() {
  const be = {
    ok: false, product: "", bridgeOk: false,
    folder: "", folderName: "", itemCount: 0,
    selectedCount: 0, selected: [], reason: "", windowTitle: "", pid: 0
  };

  const ready = await te.EnsureReady();
  if (!ready) {
    be.reason = "没有连上任何 ExBarBrowser 窗口";
    return be;
  }
  const st = te.Status();
  be.bridgeOk = true;
  be.ok = true;
  be.product = st.server || "exbarbrowser";
  be.pid = st.pid;

  // 1) 状态：当前路径 + 选中集（权威来源，且很便宜）
  const state = await callRaw("exbar/state", {});
  if (state.ok && state.result) {
    const s = state.result;
    be.folder = String(s.activePath || "");
    be.folderName = be.folder.replace(/[\\/]+$/, "").split(/[\\/]/).pop() || be.folder;
    if (typeof s.selectedCount === "number") be.selectedCount = s.selectedCount;
    if (Array.isArray(s.selected)) {
      be.selected = s.selected.slice(0, 8).map((x) => ({
        name: x && x.name ? x.name : String(x),
        path: x && x.path ? x.path : "",
        dir: !!(x && x.isDir)
      }));
      if (!be.selectedCount) be.selectedCount = be.selected.length;
    }
  } else if (state.error) {
    be.reason = state.error;
  }

  // 2) 条目数：tool.list 会真的扫目录，比状态贵；只为计数取一次。
  //    视图没就绪时会失败，重试一次（程序刚启动的常见情形）。
  if (be.folder && !be.itemCount) {
    let r = await te.dispatch("tool.list", { limit: 400 });
    if (r && r.error) {
      await sleep(1000);
      r = await te.dispatch("tool.list", { limit: 400 });
    }
    if (r && !r.error) {
      if (r.path && !be.folder) be.folder = String(r.path);
      be.itemCount = Array.isArray(r.entries) ? r.entries.length : 0;
    }
  }

  // 3) 窗口标题（顺带确认连的是哪个窗口）
  const w = await te.dispatch("win.list", {});
  if (w && Array.isArray(w.windows) && w.windows.length) {
    const self = w.windows.find((x) => x && x.pid === st.pid) || w.windows[0];
    be.windowTitle = self.title || "";
  }
  return be;
}

module.exports = { Probe, ContextLite };