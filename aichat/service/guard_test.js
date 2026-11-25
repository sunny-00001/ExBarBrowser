// guard 的单测（不需要连桥、不需要模型，纯函数）。
// 用法：node aichat/service/guard_test.js
"use strict";

const g = require("./guard");

let pass = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else failures.push(what);
}

// ---- LooksAbsolute：这决定"要不要查白名单"，漏判 = 边界失效 ----
ok(g.LooksAbsolute("C:\\dir") === true, "盘符反斜杠");
ok(g.LooksAbsolute("C:/dir") === true, "盘符正斜杠");
ok(g.LooksAbsolute("\\\\server\\share") === true, "UNC");
ok(g.LooksAbsolute("C:") === false, "裸盘符不算（长度不足）");
ok(g.LooksAbsolute("/foo/bar") === false, "URL 路径不算");
ok(g.LooksAbsolute("rel\\path") === false, "相对路径不算");
ok(g.LooksAbsolute("fs.delete") === false, "命令名不算");
ok(g.LooksAbsolute("") === false, "空串不算");
ok(g.LooksAbsolute(null) === false, "null 不算");
ok(g.LooksAbsolute("E:\\") === true, "盘根算");

// ---- InsideRoots：含子目录、大小写、正斜杠、尾分隔符 ----
const roots = ["E:\\ExBarBrowser"];
ok(g.InsideRoots("E:\\ExBarBrowser", roots), "根自身");
ok(g.InsideRoots("E:\\ExBarBrowser\\src\\a.cpp", roots), "子文件");
ok(g.InsideRoots("e:/exbarbrowser/src", roots), "小写正斜杠");
ok(g.InsideRoots("E:\\ExBarBrowser\\", roots), "尾反斜杠");
ok(g.InsideRoots("E:\\ExBarBrowserEvil\\x", roots) === false, "前缀相似但不是子目录");
ok(g.InsideRoots("E:\\Other\\x", roots) === false, "完全另一盘");

// ---- 裸盘符当根：这是踩过的真 bug ----
//
// `path.normalize("D:")` = `"D:."`，那是"D 盘当前目录"而不是盘根。
// 用户和白名单配置几乎都把盘根写成 `D:`。不特殊处理的话，归一化后根变成
// `d:.`，任何 `D:\x` 都判越界 —— 白名单方向反了（总是拒绝）。
for (const form of ["D:", "D:\\", "D:/"]) {
  ok(g.InsideRoots("D:\\Nextcloud", [form]) === true, `盘根写法 ${JSON.stringify(form)} 含子目录`);
  ok(g.InsideRoots("D:\\", [form]) === true, `盘根写法 ${JSON.stringify(form)} 匹配自身`);
  ok(g.InsideRoots("D:\\a\\b\\c.txt", [form]) === true, `盘根写法 ${JSON.stringify(form)} 深层子路径`);
  ok(g.InsideRoots("E:\\x", [form]) === false, `盘根写法 ${JSON.stringify(form)} 不跨盘`);
}

// ---- CollectPaths：嵌套结构里的路径都要找到（漏一个就等于漏一个入口）----
{
  const out = [];
  g.CollectPaths(
    { path: "E:\\a", targets: ["E:\\b", "E:\\c"], nested: { to: "E:\\d" }, name: "x" },
    "", out
  );
  const paths = out.map((x) => x.path).sort();
  ok(paths.length === 4, "嵌套收集到 4 个路径，实际 " + paths.length);
  ok(paths.includes("E:\\a") && paths.includes("E:\\d"), "含嵌套键");
  ok(out.every((x) => x.where), "每个路径都带位置信息");
}

// ---- Denied：点分名 + 前缀族 + 大小写 ----
ok(g.Denied("fs.delete", ["fs.delete"]) === "fs.delete", "精确命中");
ok(g.Denied("Fs.Delete", ["fs.delete"]) === "fs.delete", "大小写不敏感");
ok(g.Denied("fs.delete", ["Delete", "ShiftDelete"]) === null,
  "旧 Opus 动词不该命中点分名（否则是假防线）");
ok(g.Denied("fs.delete.all", ["fs.*"]) === "fs.*", "前缀族命中");
ok(g.Denied("view.sort", ["fs.*"]) === null, "前缀族不误伤");
ok(g.Denied("x", []) === null, "空清单放行");

// ---- Check：组合行为 ----
{
  const cfg = { denyCommands: ["fs.delete"], allowedRoots: [] };
  ok(g.Check("fs.delete", {}, cfg) !== null, "黑名单拦下");
  ok(g.Check("view.sort", {}, cfg) === null, "未列出的放行");
}
{
  // 空白名单 = 不限制（必须与"没这个功能"完全一致，否则升级会卡住老用户）
  const cfg = { denyCommands: [], allowedRoots: [] };
  ok(g.Check("fs.delete", { targets: ["C:\\anywhere"] }, cfg) === null, "空白名单不限制");
}
{
  // 有白名单时：界内放行、界外拒绝，且拒绝理由要能指出是哪个参数
  const cfg = { denyCommands: [], allowedRoots: ["E:\\ExBarBrowser"] };
  ok(g.Check("tool.list", { path: "E:\\ExBarBrowser\\src" }, cfg) === null, "界内放行");
  const bad = g.Check("fs.delete", { targets: ["E:\\ExBarBrowser\\src", "D:\\secret"] }, cfg);
  ok(bad !== null, "界外拒绝");
  ok(bad && bad.indexOf("targets[1]") >= 0, "拒绝理由指出越界参数位置");
  ok(bad && bad.indexOf("D:\\secret") >= 0, "拒绝理由指出越界路径");
}
{
  // 非路径参数不该被误拦（否则正常的排序/视图命令全挂）
  const cfg = { denyCommands: [], allowedRoots: ["E:\\ExBarBrowser"] };
  ok(g.Check("view.sort", { by: 2, desc: true }, cfg) === null, "非路径参数放行");
  ok(g.Check("search.find", { query: "发票" }, cfg) === null, "搜索词放行");
}

// ---- dryRun：只描述不执行，判据来自命令表 readOnlyHint ----
//
// 此前 dryRun 只被打进 API 响应、显示成一个小标签，**没有任何拦截逻辑** ——
// 又一个"配置存在但无人读取"的假开关。现在它在 guard 里执行。
{
  // 注入一个假命令表：view.sort 只读，fs.delete 写。
  const table = {
    "view.sort": { annotations: { readOnlyHint: true } },
    "fs.delete": { annotations: { readOnlyHint: false } },
  };
  g.SetMetaProvider((n) => table[n] || null);

  const dry = { denyCommands: [], allowedRoots: [], dryRun: true };
  ok(g.Check("view.sort", { by: 2 }, dry) === null, "dryRun 下只读命令照常放行");
  const blocked = g.Check("fs.delete", { targets: ["E:\\a"] }, dry);
  ok(blocked !== null, "dryRun 下写命令被拦");
  ok(blocked && blocked.indexOf("dry run") >= 0, "拒绝理由说明是 dry run", blocked);

  // 未知命令：查不到元数据时按写操作处理（fail closed）
  const unknown = g.Check("brand.new.command", {}, dry);
  ok(unknown !== null, "dryRun 下未知命令 fail-closed 被拦");

  // dryRun 关掉后同一命令放行
  const live = { denyCommands: [], allowedRoots: [], dryRun: false };
  ok(g.Check("fs.delete", { targets: ["E:\\a"] }, live) === null, "dryRun 关闭后写命令放行");
}

// ---- 判定顺序：denyCommands 先于 dryRun（拒绝原因要落在最直接的开关上）----
{
  g.SetMetaProvider((n) => ({ annotations: { readOnlyHint: false } }));
  const cfg = { denyCommands: ["fs.delete"], allowedRoots: [], dryRun: true };
  const r = g.Check("fs.delete", {}, cfg);
  ok(r && r.indexOf("denyCommands") >= 0, "黑名单优先于 dryRun 报告", r);
}

console.log(`guard_test: ${pass} 通过 / ${pass + failures.length} 总`);
if (failures.length) {
  for (const f of failures) console.log("  FAIL " + f);
  process.exit(1);
}