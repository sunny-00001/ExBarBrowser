// guard.js - 安全边界的唯一执行点。
//
// 【为什么需要这个文件】配置里有三个安全项（allowedRoots / denyCommands /
// dryRun），但它们此前**一个都没有活的执行点**：
//   · allowedRoots / denyCommands 只在 tools.js / opus.js 里被判，而这两个模块
//     在迁移到 ExBarBrowser 命名管道之后**已经没有任何调用方**
//     （server.js 只 require agent.js / agent-stream.js，两者都走 tools-exbar）；
//   · dryRun 只被打进 API 响应、在界面上显示成一个小标签，**从没据此拦过命令**。
// 于是真实链路上三道闸门全是关着的：配了白名单照样能操作全盘，开了 dryRun
// 照样真跑命令。这类"配置项存在但无人读取"比没有更糟 —— 用户以为设了边界。
//
// 【为什么放在 tools-exbar.dispatch 里】dispatch 是**所有**工具调用的唯一出口
// （agent.js 与 agent-stream.js 两条循环都只经它）。放在这里就不存在
// "某条路径忘了过闸"的可能 —— 这与宿主侧"全部经 CommandBus"是同一条纪律。
//
// 【它检查什么】
//   1. denyCommands：命令名黑名单（按宿主侧点分命名，如 fs.delete；支持 fs.* 前缀族）。
//   2. dryRun：只读命令放行，写命令拦下（判据 = 命令表 readOnlyHint）。
//   3. allowedRoots：参数里出现的**绝对路径**必须落在白名单内。
//
// 【它不检查什么（如实说明，别把它当沙箱）】
//   · 相对路径不拦截。`..\..\x` 由宿主按"当前视图目录"解析，而当前视图是
//     用户眼前看得到的那个文件夹 —— 拦它反而会误伤正常操作。真正的边界是
//     "模型不能把路径指到用户看不见的地方"，绝对路径已经覆盖这一条。
//   · 不限制命令的语义（比如"删除"本身）。那是宿主的危险双门 + 面板审批卡的
//     职责：destructiveHint 驱动的逐次确认，比任何名字黑名单都难绕过。
//   · 白名单为空 = 不限制（与 config.js 的约定一致）。这是刻意的：默认行为
//     必须与"没有这个功能"完全一致，否则升级会突然卡住老用户。
//   · dryRun 拦的是"会改状态的命令"，判据是命令表的 readOnlyHint，不是本文件
//     自己猜；所以新命令只要在命令表里标对了就自动被正确分类，不会漏。
"use strict";

const path = require("path");

// 参数里可能装路径的键。列出来是为了给错误信息一个人话（"哪个参数越界了"），
// 而不是靠猜键名判断要不要检查 —— 真正的判据是"值看起来像绝对路径"
// （见 CollectPaths），这样新命令加了没见过的路径参数名也不会漏。
const PATH_KEYS = [
  "path", "paths", "targets", "target", "from", "to", "dest", "destination",
  "src", "source", "dir", "folder", "roots", "root", "file", "files"
];

// 一个字符串值是否"看起来像绝对路径"。
//
// 判据宁可宽一点：漏判一个路径参数 = 边界失效；误判一个普通字符串 =
// 用户看到一条明确的拒绝原因，还能改写法绕过。两害相权，取后者。
//   C:\dir      /  C:/dir      /  C:\
//   \\server\share（UNC）
//
// 注意不把 `/foo` 当绝对路径：那是 URL 的路径部分，也是 shell: 命名空间之外的
// 相对写法；Windows 上真正的绝对路径一定带盘符或 UNC 前缀。
function LooksAbsolute(s) {
  if (typeof s !== "string" || s.length < 3) return false;
  if (/^[a-zA-Z]:[\\/]/.test(s)) return true;      // C:\ 或 C:/
  if (/^\\\\[^\\]/.test(s)) return true;           // \\server
  return false;
}

// 递归收集参数里所有"像绝对路径"的字符串，连同它所在的位置（给错误信息用）。
// 位置形如 `targets[0]`，让用户知道是哪个参数越界，而不是一句"不允许"。
function CollectPaths(value, where, out) {
  if (typeof value === "string") {
    if (LooksAbsolute(value)) out.push({ path: value, where });
    return;
  }
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i++) CollectPaths(value[i], `${where}[${i}]`, out);
    return;
  }
  if (value && typeof value === "object") {
    for (const k of Object.keys(value)) {
      CollectPaths(value[k], where ? `${where}.${k}` : k, out);
    }
  }
}

// 路径归一化后比较。两侧都要归一化：白名单可能来自 aichat.json（JSON 转义）、
// 环境变量（shell 转义），也可能写成正斜杠；只比裸字符串会让"合法路径被判越界"
// 和"含 // 的根一个都匹配不上"同时发生（这在 tools.js 里踩过一次）。
//
// 【裸盘符必须先补分隔符】`path.normalize("D:")` 得到的是 `"D:."` ——
// Windows 上 `D:` 的意思是"**D 盘的当前目录**"，不是盘根。用户和白名单配置
// 几乎全部把盘根写成 `D:`（含本文件的测试），于是归一化后根变成 `d:.`，
// 任何 `D:\x` 都判越界 —— 白名单形同虚设且方向相反（总是拒绝）。
// 所以：先识别"盘符 + 可有可无的分隔符"，统一成 `D:\` 再归一化。
function Normalize(p) {
  let s = String(p).trim();
  // `D:` / `D:\` / `D:/` 一律当盘根 `D:\`
  if (/^[a-zA-Z]:[\\/]?$/.test(s)) s = s.slice(0, 2) + "\\";
  return path
    .normalize(s.replace(/\//g, "\\"))
    .toLowerCase()
    .replace(/[\\/]+$/, "");
}

// 检查一个绝对路径是否在白名单内（含子目录）。
function InsideRoots(abs, roots) {
  const norm = Normalize(abs);
  for (const root of roots) {
    if (typeof root !== "string" || root.trim() === "") continue;
    const r = Normalize(root);
    if (!r) continue;
    // 盘根（`d:`）在归一化后没有尾分隔符，拼起来是 `d:\...`；
    // 普通目录同理。两种情况都用同一个判据。
    if (norm === r || norm.indexOf(r + "\\") === 0) return true;
  }
  return false;
}

// 命令名黑名单。宿主侧命名是点分小写（fs.delete），旧的 Opus 动词
// （Delete / ShiftDelete）在这里永远匹配不上 —— 保留它们只会让人误以为
// 有一道防线。所以：只认点分名，且大小写不敏感（模型可能写 Fs.Delete）。
function Denied(name, denyCommands) {
  if (!Array.isArray(denyCommands) || !denyCommands.length) return null;
  const low = String(name || "").toLowerCase();
  for (const d of denyCommands) {
    if (typeof d !== "string") continue;
    const want = d.trim().toLowerCase();
    if (!want) continue;
    // 支持 `fs.*` 这类前缀写法：一条规则盖住一族命令，
    // 比逐个列（然后忘掉新加的那条）可靠。
    if (want.endsWith(".*")) {
      if (low.indexOf(want.slice(0, -1)) === 0) return d;
      continue;
    }
    if (low === want) return d;
  }
  return null;
}

// 命令的元数据查询（只读判定要用）。
//
// 【为什么不在这里 require("./tools-exbar")】tools-exbar 会 require 本文件来
// 在 dispatch 里做校验 —— 反向依赖就成环。改用注入：tools-exbar 加载时把
// 自己的 ToolMeta 交给 SetMetaProvider。这同时保证 guard_test.js 不连桥也能跑。
let MetaProvider = null;

function SetMetaProvider(fn) {
  MetaProvider = typeof fn === "function" ? fn : null;
}

// 返回 { readOnly } 或 null（未知命令 / 没连上桥）。
function MetaOf(name) {
  if (!MetaProvider) return null;
  try {
    const t = MetaProvider(name);
    if (!t) return null;
    const a = t.annotations || {};
    return { readOnly: a.readOnlyHint === true };
  } catch (e) {
    return null;
  }
}

// 对一次 dispatch 做完整校验。
// 返回 null = 放行；返回字符串 = 拒绝原因（已经是可以直接给用户/模型看的中文）。
//
// 判定顺序是刻意的：先 denyCommands（名字层面，成本最低且与路径无关），
// 再 dryRun（全局"只看不做"），最后 allowedRoots（要看参数才知道）。
// 早一步能拒绝的就不往下走 —— 也让拒绝原因落在最直接的那个开关上。
function Check(name, args, config) {
  const denied = Denied(name, config && config.denyCommands);
  if (denied) {
    return `命令「${name}」在禁用清单里（denyCommands: ${denied}），本次调用未执行。`;
  }

  // dryRun：只描述、不执行。
  //
  // 【为什么用 mutate 白名单反推，而不是"凡是写命令都拦"】判据来自命令表自己的
  // annotations.readOnlyHint —— 与审批卡、宿主的危险双门是同一个事实。
  // 只在 guard 里按名字猜"哪些是写"会立刻过时（命令表 171 条还在长）。
  if (config && config.dryRun) {
    const meta = MetaOf(name);
    // 查不到元数据 = 当成写操作（fail closed）。
    const isWrite = !meta || meta.readOnly === false;
    if (isWrite) {
      return `dry run 模式：命令「${name}」只做了描述，没有执行。`
        + `（关掉 aichat 的 dryRun 才会真的跑）`;
    }
  }

  const roots = config && Array.isArray(config.allowedRoots) ? config.allowedRoots : [];
  // 空白名单 = 不限制。见文件头的说明。
  if (!roots.length) return null;

  const found = [];
  CollectPaths(args || {}, "", found);
  for (const item of found) {
    if (!InsideRoots(item.path, roots)) {
      const where = item.where ? `（参数 ${item.where}）` : "";
      return `路径越界：${item.path}${where} 不在允许的范围内。`
        + `允许的根目录：${roots.join("；")}。`
        + `如果这是预期操作，请让用户调整 aichat 的 allowedRoots 配置。`;
    }
  }
  return null;
}

module.exports = {
  Check, InsideRoots, LooksAbsolute, CollectPaths, Denied, PATH_KEYS,
  SetMetaProvider, MetaOf
};