# G2_appcmd 组审查报告（app_commands* 19 个文件 / 4907 行）

审查方式：清单 `review/G2_appcmd.txt` 内 19 个文件逐一完整 Read（>2000 行者分页读到底），
逐 handler、逐分支核对；未抽样、未跳读。仅读 `src/`，未运行 build / 未杀进程 / 未改动任何源码。

结论口径：**高**＝会误导调用方或阻塞 AI 管道（人机等价失效、返回值与注释相反）；
**中**＝假成功 / 原因缺失 / 同语义分叉，能被审计或断言发现但会浪费排查；
**低**＝风格、死代码、文档与实现小偏差。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|------|------|------|
| 1 | `src/app_commands.cpp` | 558 | **有问题（中）**：`TransferTargets` 部分命中返回 true（与 FsDelete 同病）；`ApplyViewMode`/`WithView` 恒 true 或失败无原因；`CoerceParam` 不校验 schema 约束且 int 强转回绕；558 行为已记账行数欠账 |
| 2 | `src/app_commands.h` | 29 | 干净：注册入口与 `RunTerminal` 声明，无实现、无问题 |
| 3 | `src/app_commands_ai.cpp` | 58 | 基本干净：端口/ShellExecuteW 双检查到位；唯一观察是表里 `win.ai.toggle`(implemented=false) 与本文件 `ai.panel.toggle` 语义重复（死条目） |
| 4 | `src/app_commands_archive.cpp` | 166 | 低风险：参数校验（trials 数组 / repeats 字符串 / limit 1..500）齐全；`record` 的 `out->Set` 未判空（与全组风格不一致） |
| 5 | `src/app_commands_bookmark_io.cpp` | 91 | 低风险：机器侧强制 path、mode 白名单、-2/-3 分类清楚；人侧 `-1` 同时代表"取消"与"文件无效"被一律报成"用户取消"（分类误导） |
| 6 | `src/app_commands_compare.cpp` | 294 | 基本干净：human 门控正确、预算/取消/超时字段齐全、部分失败语义交给作业层；仅 human+async 无任何反馈、`out->Set` 判空风格不统一 |
| 7 | `src/app_commands_dev.cpp` | 253 | **有问题（低-中）**：`dev.build`/`dev.test` 在 `StartJob` 返回空 id 时仍 `ok=true` 且 `jobId=""`（假成功）；`out->Set` 未判空 |
| 8 | `src/app_commands_external.cpp` | 38 | **有问题（中）**：`ext.terminal.at` 未校验目录存在、`RunTerminal` 无返回值 → 恒 true；`clipboard.copy` 允许空串（schema 已声明，可接受） |
| 9 | `src/app_commands_files.cpp` | 903 | **重灾区**：`SelectByPattern` 对 AI 弹模态框（**高**）、`BatchRename` 机器侧隐式用当前选中（**高**）、`CmdOpenWith` 不检查 ShellExecute 返回值、`SelectByType` 无门控 MessageBox；903 行为已记账欠账（compare/scan 注释自认"已超行数上限"） |
| 10 | `src/app_commands_fs.cpp` | 227 | **有问题（高）**：`FsDelete` 注释明写"部分命中返回 false"，实现 `return ok`（**注释与实现相反**）；多处空参静默 `return false` 无原因 |
| 11 | `src/app_commands_jobs.cpp` | 241 | 干净：`CollectResult` 把 cancelled/timedOut/failed 三分且都带 kind，是全组最好的失败语义样板；`result->Set` 未判空（小） |
| 12 | `src/app_commands_jobs.h` | 89 | 干净：接口与默认值注释自洽（`waitMs=30000` 默认仅机器同步未用到时兜底），无问题 |
| 13 | `src/app_commands_misc.cpp` | 388 | **有问题（中）**：`RunTerminal`/`FolderOptions`/`MapDrive`/`UnmapDrive` 全部忽略 `ShellExecuteW` 返回值 → 恒 true；`session.restore` 恒 true；`config.get` 空 key 无原因；`named.session.*` 参数校验与判空不一致 |
| 14 | `src/app_commands_misc_bookmarks.cpp` | 401 | **有问题（中）**：`bmk.show/hide/toggle-bar` 绕过 `config.set` 直接写 Settings（刚被 misc.cpp 收口的那类写路径又出现一份）；`bmk.add.selection` 忽略 `Save()` 结果、失败无原因 |
| 15 | `src/app_commands_panes.cpp` | 27 | 低风险：4 条命令与表内 `aiExposed=false` 一致；注释仍写"这三条"（第 4 条 `view.pane.vertical` 是后加的）；`percent` 靠 `SetPaneSplit` 内部夹取（静默改值） |
| 16 | `src/app_commands_scan.cpp` | 347 | **有问题（中）**：`tool.hash` 机器侧省略 `paths` 会隐式哈希"用户当前选中项"，且表里 `needsSelection=false`；全部文件失败仍返回 `ok=true` |
| 17 | `src/app_commands_search.cpp` | 122 | 干净：范围 id 严格拒绝、不可用档位拒绝、`search.find` 复用 `App::RunSearch` 唯一实现；无问题 |
| 18 | `src/app_commands_tabs.cpp` | 466 | **有问题（中）**：`go.*` 与 `nav.back/forward/up` 丢弃导航结果恒 true；`path.copy*` 目标为空时复制空串仍 `ok=true`；`tab.move.*`/`tab.close.prev|next` 恒 true，与同文件 `goTab`/`tab.prev.activated` 的"边界如实失败"自相矛盾 |
| 19 | `src/app_commands_window.cpp` | 209 | **有问题（中-低）**：`win.list` 在 `out==nullptr` 时 `return true`（与其他文件相反）；`win.tile` 非法 orientation 静默当 horizontal、`SetWindowPos` 未检查；`win.cascade` 无 Step 上限 |

覆盖交叉验证：表内 218 个 `CmdId::` 引用中，全仓仅有 8 个无 handler（`TabMergeAll`/`PathCopyLink`/`ToggleCheckboxes`/`RunCommand`/`SendKeys`/`SearchEverything`/`AiTogglePanel`/`AiStop`），
它们在表里全部 `implemented=false`，**不属于漏注册**；本组 19 个文件共注册 200+ 条，无"表里有、handler 无"的组内缺口。

---

## 二、高危（必须改）

### H1 `src/app_commands_files.cpp:451-471` — `file.select.pattern` 对 AI 弹模态输入框
```
std::wstring pattern = args.Str(L"pattern");
if (pattern.empty()) { ... PromptText(...) ... }   // 451-461：没有 IsHumanCall 门控
```
- **问题**：表里 `file.select.pattern` 是 `aiExposed=true`、`needsSelection=false`，且 schema 的 `pattern`
  **不是 required**（表内 134 行：`"properties":{pattern,folders,caseSensitive}`，无 `required`）。
  于是 AI 传 `{}` 或 `{"folders":true}` 就进花括号里的人侧分支 → 在 UI 线程上开**模态** `PromptText`。
- **触发**：AI/脚本调 `file.select.pattern {}`。管道线程一直等到有人手动关掉那个框（或 10s 超时后对话框**仍留在屏幕上**）。
  人机等价在这里是完全破的：同一条命令，人点=输入框，机器调=卡死。
- **修复**：与本文件 `BatchRename`（175-185）保持同一口径 ——
  `if (pattern.empty() && IsHumanCall(args)) { 弹框 }`；机器侧空 pattern 就
  `NoteFailure(L"缺少必填参数：pattern")` 并 `return false`（或把 `pattern` 提升为 schema `required`）。

### H2 `src/app_commands_files.cpp:173-202` — `file.rename.batch` 机器侧隐式落到"用户当前选中项"
```
std::vector<std::wstring> targets = args.StrArray(L"targets");
if (targets.empty()) targets = shellops::SelectedPaths(v);   // 194-195：无 human 门控
```
- **问题**：`targets` 在 schema 里是可选（"省略则用当前选中项"），表里 `needsSelection=false`。
  机器调用只给 `pattern` 时，就会**批量重命名用户在界面上碰巧选中的文件**，且这条命令 `destructive=1`
  的确认只在人工路径（`MessageBoxW` 只在 `human` 时弹）。
- **触发**：AI 先做 `file.select.pattern {"pattern":"*.log"}`（或用户自己框选），再 `file.rename.batch {"pattern":"{name}_{n}"}`
  → 用户的文件被改名，AI 认为"我改的是我点名的那批"。这正是 `needsSelection` 字段存在的理由
  （`commands.h:44`：「依赖当前选中项（无选中时机器侧应拒绝而非猜测）」）。
- **修复**：`if (targets.empty() && !IsHumanCall(args)) { NoteFailure(L"机器调用必须显式给 targets"); return false; }`；
  或把该命令的 `needsSelection` 置 true，让 `ai_api` 在无选中时直接拒。

### H3 `src/app_commands_fs.cpp:75-99` — 注释说"返回 false"，实现返回 true（部分命中报成功）
```
 92→            // 只命中一部分也算部分成功：返回 false 让调用方知道"没全照办"，
 93→            // 但已完成的部分是真实发生的（审计里 ok=0 会让人误以为什么都没做，
 94→            // 所以这里附带命中数记日志）。
 95→            if (hit < static_cast<int>(targets.size()))
 96→                LogDbg(L"[cmd] FsDelete 只命中 %d/%zu 个目标", hit, targets.size());
 97→            return ok;
```
- **问题**：`ok` 是 `DeleteSelection` 的结果（"选中的那部分删掉了"），与"是否全部照办"无关。
  注释承诺 `return false`，代码 `return ok` → **部分命中时 ok=1**。命中数只进了 `LogDbg`，
  调用方（AI / 审计 / 序列）拿不到"少删了几个"，也拿不到被跳过的路径。
- **触发**：`fs.delete {"targets":["C:\\a\\x.txt","C:\\b\\y.txt"]}`，其中 `b` 不在当前视图
  （`hit==1`）→ 只删了 x.txt，命令 `ok=true`，审计 `ok=1`。AI 会认为两个都删了，接着去写 `C:\b`。
- **修复**：`return ok && hit == static_cast<int>(targets.size());`，并把命中数/未命中路径放进结果
  （该命令当前没有 `outputSchema`，加 `{requested, hit, deleted}` 比一句日志有用得多）。

### H4 `src/app_commands.cpp:276-335` — `fs.copy`/`fs.move` 同一处缺陷（部分命中返回 true）
```
291→        const int hit = shellops::SelectByPaths(v, targets);
292→        if (hit == 0) { NoteFailure(...); return false; }
...
302→        if (hit < static_cast<int>(targets.size()))
303→            LogDbg(L"[cmd] %ls 只命中 %d/%zu 个目标", ...);   // 只记日志
333→        return ok;
```
- **问题**：与 H3 完全同构 —— 命中数只写日志，返回的是"已完成的那些转移成功了"。
  另外 `targets.empty() || to.empty()` 时（285 行）直接 `return false`，**不调 `NoteFailure`**，
  调用方拿到 `ok=false` 且 `lastBlockReason` 为空（`commands.cpp:256` 只在 `pendingReason_` 非空时才填）。
- **触发**：`fs.move {"targets":["…\\a.txt","…\\b.txt"],"to":"D:\\dst"}`，只有 a 在当前视图
  → a 被移动、b 没动、`ok=true`。移动是 `destructive=1`（`kEffectMoveTo`），后果是"文件去哪了"。
- **修复**：与 H3 同批修（`return ok && hit==targets.size()`）；空参补 `NoteFailure(L"缺少必填参数：targets / to")`。

---

## 三、中危

### M1 `src/app_commands.cpp:264-274`（被 files.cpp 全线使用）— `WithView` 失败不报原因
```
bool App::WithView(...) { ... if (!ok) { LogDbg(L"[cmd] %s 失败", what); MessageBeep(...); } return ok; }
```
- **问题**：`file.copy`/`file.cut`/`file.paste`/`file.delete`/`file.rename`/`file.new.*`/`file.select.*`/
  `view.sort`/`view.group`/`file.focus.top|bottom` 全部靠它，失败时**没有任何 `NoteFailure`**。
  `commands.cpp:240/256` 只在 handler 自己报过原因时才写 `lastBlockReason_`，于是 AI 只能拿到
  `{"ok":false}`（无 reason）。而 `fs.*` 与 `scan.*` 的每个失败点都给了具体原因 → 同一族的"删除失败"，
  两条入口一个能解释、一个不能。
- **触发**：`file.delete` 在视图拒绝删除（只读/占用）时；`view.sort` 在不支持排序的视图时。
- **修复**：`WithView` 失败分支补 `CommandBus::Get().NoteFailure(std::wstring(what) + L"失败（视图拒绝了这次操作）")`，
  或让调用侧在返回 false 前各自补原因（工作量大，优先改 `WithView`）。

### M2 `src/app_commands_misc.cpp:18-27、110-117` / `src/app_commands_external.cpp:29-37` — 终端启动恒成功
```
21→    HINSTANCE r = ShellExecuteW(owner, ..., L"wt.exe", ...);
23→    if (r > 32) return;                                   // 成功
25→    ShellExecuteW(owner, ..., L"powershell.exe", ...);     // 兜底：返回值丢弃
```
- **问题**：`RunTerminal` 是 `void`，两条 `ShellExecuteW` 中第二条的返回值没人看；
  `ext.terminal`/`ext.terminal.admin` 恒 `return true`；`dir.empty()` 时（19 行）直接 `return`，
  命令仍 `ok=true`。`ext.terminal.at` 不校验 `path` 是目录也不校验存在性（机器侧唯一参数）。
- **触发**：`ext.terminal.at {"path":"D:\\typo"}`，`wt.exe` 因 `-d` 目录不存在而退出、`powershell` 同样失败
  → `ok=true`，AI 以为终端起来了并继续往那个目录写文件。
- **修复**：`RunTerminal` 改为 `bool`（两次 `ShellExecuteW` 都 `<=32` 才算失败），三个 handler 透传；
  `ExtTerminalAt` 增加 `tf::IsDirectory(path)` 预检。

### M3 `src/app_commands_misc.cpp:80-93` — `FolderOptions`/`MapDrive`/`UnmapDrive` 忽略返回值
- **问题**：三条 `ShellExecuteW(..., rundll32.exe ...)` 全部 `return true`，不看 `>32`。与 `app_commands_ai.cpp:49-55`
  （那里检查得很仔细）形成同一维度的分叉。
- **触发**：rundll32 起不来（策略禁用 / 参数被系统拒绝）→ `ok=true`，AI 报"已打开映射网络驱动器向导"。
- **修复**：统一 `const HINSTANCE r = ShellExecuteW(...); if (r <= 32) { NoteFailure(...); return false; } return true;`

### M4 `src/app_commands_files.cpp:520-526` — `file.openwith` 不检查 ShellExecute，恒 true
```
524→        ShellExecuteW(hwnd_, L"open", L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
525→        return true;
```
- **问题**：同 M3；另外 `paths.front()` 直接拼进参数字符串（`shellops::SelectedPaths` 给的是路径，
  含空格已加引号，但缺 `ShellExecute` 失败面）。`SelectedOrFolder()` 为空时 `return false` 无原因。
- **触发**：`OpenAs_RunDLL` 被系统策略拦下 → `ok=true`。
- **修复**：检查 `>32`；空目标补 `NoteFailure`。

### M5 `src/app_commands_scan.cpp:213-227、319` — `tool.hash` 机器侧隐式吃"当前选中项"，且全失败仍 true
```
213→        std::vector<std::wstring> paths = args.StrArray(L"paths");
214→        if (paths.empty()) { ... = shellops::SelectedPaths(v); }   // 无 IsHumanCall 门控
```
- **问题**（两件）：
  1. 表里 `needsSelection=false`、schema 说"省略则用当前选中项"，于是 AI 的 `tool.hash {}`
     哈希的是**用户此刻框选的文件**（只读，但结果会被当成"我点名的那些文件的摘要"）；
  2. 作业体在全部文件都失败时（`failCount==N`）仍 `return true`（319），`CollectResult` 转成 `ok=true`，
     结构里 `entries[].ok` 全 false。与 `BatchRename`（部分失败即 `ok=false`，301-306）语义相反。
- **触发**：AI `tool.hash {"paths":["C:\\nope\\a.bin"]}`（不存在）→ `ok=true`，`entries[0].ok=false`。
- **修复**：机器侧空 `paths` 直接拒；`if (okCount == 0 && failCount > 0)` 时 `return false` 并 `NoteFailure`
  （报告文本仍可给出，但成败要如实）。

### M6 `src/app_commands_tabs.cpp:286-289` — `go.*` 一族恒 true
```
auto go = [this](const std::wstring& path) {
    if (!path.empty()) NavigateActive(path, true);
    return true;
};
```
- **问题**：`KnownFolder(...)` 取不到（`SHGetKnownFolderPath` 失败）→ `path` 为空 → 什么都不做，`ok=true`；
  `NavigateActive` 返回 false（目标离线/被拒）也被丢弃。`go.home`/`go.desktop`/`go.downloads`…/`go.powershell` 全部
  `aiExposed=true`。
- **触发**：`go.downloads` 在 OneDrive 重定向失败/被策略移除的机器上 → `ok=true`，当前位置没变。
- **修复**：`if (path.empty()) { NoteFailure(L"取不到已知文件夹路径"); return false; }` + `return NavigateActive(path, true);`
  （若确实异步，至少保留 `path.empty()` 的拒绝）。

### M7 `src/app_commands_tabs.cpp:318-320` — `nav.back`/`nav.forward`/`nav.up` 丢弃结果
- **问题**：`GoBack(); return true;` 三条皆然。`nav.back` 有 `Req::HasHistoryBack` 前置，`nav.forward`/`nav.up`
  没有对应前置（`nav.forward` 顶层时仍 `ok=true`）。
- **触发**：`nav.up` 在盘符根（`C:\`）→ `ok=true`，视图没动。
- **修复**：让 `GoBack/GoForward/GoUp` 返回 bool 并透传；或补齐 `Req`。

### M8 `src/app_commands_tabs.cpp:331-356、366-375` — `path.copy*` 目标为空时复制空串仍成功
- **问题**：`path.copy`（331）`path.copy.name`（340）`path.copy.unc`（357）`path.copy.ext`（366）都用
  `SelectedOrFolder()`，无视图/无选中时返回空 → 拼出空串 → `CopyText(L"")` 写剪贴板成功 → `ok=true`。
  `path.copy.dir`（351，`aiExposed=true`）`ActivePath()` 为空时同理，并返回 `text=""`。
  这与 `file.select.type`（477-482 明确"先选中一个文件"）的严格度不一致。
- **触发**：AI `path.copy.dir` 在无活动窗格/命名空间未解析时 → `ok=true`,`text=""`，AI 会认为剪贴板里是路径。
- **修复**：`paths.empty()` / `text.empty()` 时 `NoteFailure(L"没有可复制的路径")` 并 `return false`
  （注意 `path.copy*` 四条 `aiExposed=false`，但 `path.copy.dir`/`path.copy.tab` 是 true，影响面真实存在）。

### M9 `src/app_commands_tabs.cpp:193-208、225-234` — 移动/关闭恒 true，与同文件口径自相矛盾
- **问题**：`tab.move.left/right/first/last` 不看 `MoveTab` 结果（边界时无动作）恒 true；
  `tab.close.prev`/`tab.close.next`（`CloseLeftOne`/`CloseRightOne`）在边界或目标标签被锁定时恒 true。
  同一文件里 `goTab`（264-274，越界 `NoteFailure` + false）与 `tab.prev.activated`（209-220，"没有上一个...回退") 
  恰恰是"边界必须如实失败"的正面样板 —— 两条纪律并存。
- **触发**：`tab.close.prev` 在活动标签是第一个时 → `ok=true`（表里断言 `tabCount=$changed` 会红，但 handler 先撒了谎）。
- **修复**：`tab.move.*` 用 `MoveTab(...)` 的返回值；`CloseLeftOne/RightOne` 用 `CloseTab(...)` 的返回值
  （锁定被拒时给 `NoteFailure(L"标签已锁定")`）。

### M10 `src/app_commands_misc.cpp:319-323` — `session.restore` 恒 true
```
reg(CmdId::SessionRestore, [this](const Args&) {
    std::vector<std::wstring> paths = Config::Get().s().sessionPaths;
    for (const auto& p : paths) NewTab(p);
    return true;
});
```
- **问题**：没保存过（`paths` 空）→ 什么都没开，`ok=true`；`NewTab` 失败（路径失效）也丢弃。
  表里 `aiExposed=true`、`kNoArgs`，断言 `sessionSaved=$unchanged`。
- **触发**：AI 在从未 `session.save` 的机器上 `session.restore` → `ok=true`，标签数没变。
- **修复**：统计 `opened`；`opened==0` 时 `NoteFailure(L"没有可恢复的标签组（或全部路径已失效）")` + false。

### M11 `src/app_commands.cpp:225-237` + `src/app_commands_files.cpp:589-592` — `view.mode` 越界静默成功
- **问题**：`ApplyViewMode(index)` 越界直接 `return`（void），`!host` 也直接 `return`；
  handler 无条件 `return true`。schema 声明 `preset 0..7`，但 `CoerceParam`（M-附）与序列/内部调用不受 schema 约束。
- **触发**：序列或内部代码 `view.mode {"preset":99}` → `ok=true`，视图没变。
- **修复**：`ApplyViewMode` 返回 bool（越界/无宿主 → `NoteFailure`），handler 透传。

### M12 `src/app_commands_misc_bookmarks.cpp:135-153` — `bmk.show/hide/toggle-bar` 绕过 `config.set`
```
s.showBookmark = !s.showBookmark;  Config::Get().Save();  Layout();   // 135-141，另两条同构
```
- **问题**：`app_commands.cpp:480-509`（状态栏段/隐藏状态栏）刚把同类写路径收口到 `CmdId::ConfigSet`，
  理由是「机器侧既看不见也调不到」。书签栏显隐这三条又是"直接改 Settings + Save + Layout"的旧形态：
  `config.set {"key":"bookmark"}` 与 `bmk.hide.bar` 是两条不共享校验、不共享审计的写路径（同一语义两处实现）。
- **触发**：AI 用 `config.set bookmark=false` 看不到效果时，无法推断还有 `bmk.hide.bar` 这条旁路；
  反过来 `bmk.*` 的写入不进 `settings_reg` 的审计面。
- **修复**：三条改成转投 `CommandBus::Get().Invoke(CmdId::ConfigSet, {key:"bookmark", value:...}, Actor::Ui())`
  （与 `ShowStatusMenu` 同款）。

### M13 `src/app_commands_misc_bookmarks.cpp:65-84` — `bmk.add.selection` 失败无原因 + 忽略 Save
- **问题**：`sel.empty()` → `return false`（无 `NoteFailure`）；`store.Save()`（81）返回值丢弃，
  写盘失败（只读盘/冲突）仍 `ok=true`。对比 `AddCurrent`（48-51）至少把 `saved` 记进日志。
- **触发**：磁盘只读时收藏"成功"，重启后丢失。
- **修复**：`if (!store.Save()) NoteFailure(...)`；空选中补原因。

### M14 `src/app_commands_window.cpp:136-137` — `win.list` 在无 result 时 `return true`
```
regR(CmdId::WinList, [](const Args&, json::Value* out) {
    if (!out) return true;      // 全组其它 regR 都是 return false
```
- **问题**：`out==nullptr` 却报成功，调用方拿不到 `windows` 数组；与 `ConfigList`/`SearchScopeList`/`SessionList`
  的 `if (!out) return false;` 相反。
- **触发**：任何走"只要成败不要结果"的调用路径（`Invoke` 分支）都会得到 `ok=true` + 空结果。
- **修复**：改成 `return false;`（或删掉判空，`Dispatch` 对 regR 恒传非空）。

### M15 `src/app_commands.cpp:57-114` — `CoerceParam` 不校验 schema 约束（面板把非法值当合法）
- **问题**：命令面板按 `type` 解释输入，但**完全忽略** `minimum`/`maximum`/`enum`/`minLength`。
  `view.mode` 输 `99`、`tab.color` 输 `99`、`window.resize` 输 `abc`（后者会被类型挡下）都会通过收集，
  再进 handler：`tab.color` 越界 `return false` 但**无 `NoteFailure`**（files/tabs 多处同款），
  面板用户只看到"这条命令没执行"，没有任何解释。
- **触发**：命令面板执行 `tab.color`，颜色输入 99 → 无反应、无提示。
- **修复**：`CoerceParam` 增加 `minimum`/`maximum`/`enum` 校验（schema 已经是唯一真源，读它就够）；
  handler 越界分支统一补 `NoteFailure`。
- **附带**：79 行 `*out = json::Value::MakeInt(static_cast<int>(v));` —— `wcstoll` 出 64 位，
  强转 `int` 会静默回绕（`budgetMs` 之类大数输入会变成负数）。

---

## 四、低危 / 一致性 / 文档

| 编号 | 位置 | 问题 | 触发 | 修复 |
|------|------|------|------|------|
| L1 | `fs.cpp:43,77,184,195,212` | `targets/target` 空时 `return false` 无 `NoteFailure`，而 `FsOpen`(116) 就有"缺少必填参数：path" | AI 传 `{"targets":[]}` | 统一补 `NoteFailure` |
| L2 | `fs.cpp:131-134` | `FsOpen` 目录分支 `NavigateActive(path,true)` 返回值被丢弃，恒 true | 目录被占用/被策略拒 | 透传返回值 |
| L3 | `app_commands.cpp:217-223` | `OpenPathInNewWindow` 的 `ShellExecuteW` 不检查；`fs.open.new.window`(fs.cpp:171) 因此恒 true | 新实例起不来 | 检查 `>32` |
| L4 | `tabs.cpp:88-92、93-106` | `tab.rename`/`tab.color` 所有失败分支（标签不存在、index 越界、color 越界）都 `return false` 无原因 | AI 传 `index:9` | 补 `NoteFailure` |
| L5 | `tabs.cpp:221-224、250-260` | `tab.last` 在 `Count()==0` 时 true；`tab.next/prev` 单标签时 true（边界静默） | 单标签 | 与 M9 同批统一 |
| L6 | `misc.cpp:137-149` | `config.get` key 为空 `return false` 无原因；`settings_reg::Get` 失败也无原因 | `config.get {}` | 补原因（`ConfigSet` 就写了，风格不一致） |
| L7 | `misc.cpp:279-295、296-308` | `name` 为空时会走到"没有名为「」的会话"，错误信息错位；`NamedSessionRestore` 在 `opened==0` 时 `return false` 无原因 | AI 省略 `name` | 先判 `name.empty()` → "缺少必填参数：name" |
| L8 | `misc.cpp:258,269,293`；`jobs.cpp:204,228`；`archive.cpp:126`；`tabs.cpp:393,404`；`dev.cpp:172,196` | `out->Set(...)` 有的判空（`BmkAddCurrent:54`、`NamedSessionSave:258`）有的不判 | result 为空时 | 统一 `if (!out) return false;` 开头（或统一不判 + 注释说明总线恒传非空） |
| L9 | `dev.cpp:149-178、180-202` | `StartJob` 返回空 id（CreateProcess 失败）时仍 `ok=true`、`jobId=""`；compare/scan 对 `RunTask` 空 id 都判了 false | python/bash 不在 PATH | `if (id.empty()) { NoteFailure(...); return false; }` |
| L10 | `dev.cpp:238` | `ValidRepoPath(path, nullptr)` 丢弃原因，回一句通用文案 | 路径含 `..` | 传 `&err` 并转发 |
| L11 | `window.cpp:161-185、186-208` | `orientation` 非 `"vertical"` 一律当 horizontal（乱写值不报错）；`SetWindowPos` 返回值未检查；`win.cascade` 的 `kStep*i` 无上限，8 个以上窗口会被摆到工作区外 | AI `win.tile {"orientation":"vert"}`, `win.cascade` ×10 实例 | 白名单校验 orientation；`i` 取模或按 n 压缩步长 |
| L12 | `panes.cpp:3-4、19-22` | 注释"这三条都 aiExposed=false"实为 4 条（`view.pane.vertical` 后加）；`percent` 无范围校验（靠 `SetPaneSplit` 夹到 20..80，静默改值） | 注释误导后续维护者 | 注释改"这四条"；如需严格，越界 `NoteFailure` |
| L13 | `app_commands.cpp:506` | 一行两条语句、缩进错乱（`reg(CmdId::CmdNewFolder, [this](const Args&) {        return WithView(`） | — | 拆行 |
| L14 | `bookmark_io.cpp:19-24` | 人侧 `-1` 同时表示"用户取消"和"文件无效"（`bookmark_io.h:15`），一律报"用户取消导入"+`CanceledByUser` | 选中一个坏 HTML | 让 `BmkImportHtml` 区分 -1/-2，或文案改成"取消或文件无效" |
| L15 | `search.cpp:27-37` | `title` 在 `RunSearch` 失败时不会被写，`result` 却只在成功分支填（当前实现正确，但 `title` 无初值，日后改动容易带出空串） | — | `std::wstring title;` 后置兜底或注释说明 |
| L16 | `compare.cpp:194、291`；`scan.cpp:179、344` | `human && !async` 才 `ShowReport`；`human && async` 完全没有反馈（弹窗会卡 UI，取舍合理，但没写进注释） | 人在面板里给 `async:true` | 补一句注释，或 async 人侧给状态栏提示 |
| L17 | `ai.cpp:17-26` vs `commands_table_tools.cpp:106-107` | 表里 `win.ai.toggle`(implemented=false) 与本文件 `ai.panel.toggle` 是同一功能的两条 id；`win.ai.stop` 同样永久未实现 → 死条目/命名分叉 | `tools/list` 里查不到 `win.ai.toggle`，说明里却写着"AI 助手" | 删掉这两条死条目，或改成 alias 注释 |
| L18 | `misc.cpp:18-27` | `-NoExit -Command "Set-Location -LiteralPath '<dir>'"`：目录名含单引号时 PowerShell 会解析出错（未转义） | 目录名含 `'` | 用 `--%` 或双写单引号 |
| L19 | `app_commands.cpp:155-161` | `err` 文案把 `type` 直接拼进去（`"需要 integer"`），而 111 行对未声明类型保留原文 —— 类型名对终端用户是术语；可接受，但与全仓中文口径略有出入 | — | 可选：映射成中文类型名 |

### 横切一致性问题（同一语义多处实现，建议一并收口）

1. **"部分成功"三套语义并存**（本次审查最值得修的一致性问题）
   - `files.cpp:291-306` `BatchRename`：任一失败即 `ok=false` + 结构化 `failed[]`（**最正确的样板**）；
   - `fs.cpp:97` / `app_commands.cpp:333`：部分命中 **`ok=true`**，命中数只进日志（H3/H4）；
   - `scan.cpp:319`：全部失败仍 `ok=true`。
   建议定一条口径：**只要请求集合未被全部照办，`ok=false`，并把 `requested/hit/failed[]` 放进结果**。
2. **失败原因的有无**：`fs.*`/`scan.*`/`search.*`/`misc`(config) 每处失败都 `NoteFailure`；
   `file.*`（经 `WithView`）、`tabs.*`（越界分支）、`window.*` 大量无原因（M1/L1/L4）。
3. **`out` 判空**：同组内两种写法并存（L8）。
4. **ShellExecute/COM 返回值**：`ai.cpp` 检查得最完整（`>32` 判据 + 日志），
   `misc.cpp`/`files.cpp`/`window.cpp` 基本不查（M2/M3/M4/L3/L11）。

### 工程约束核对（R19–R21、品牌串、色值、命名空间）

- **R21**：本组 19 个文件**无** `SendInput`/`mouse_event`/`keybd_event`/`SetCursorPos`/`BlockInput`/`AttachThreadInput`，
  无输入类 `PostMessage`；`window.cpp:7-11` 明确写出 `ShowWindow`/`SetWindowPos`/`SetForegroundWindow`
  在 `src/` 属豁免范围（AGENTS.md:64-66），`win.activate` 也确实是 `aiExposed=false`（表 179 行）→ **合规**。
- **品牌串**：全部走 `id::kBrandShort`（`files.cpp`×5、`compare.cpp`、`scan.cpp`、`misc_bookmarks.cpp`），无硬编码品牌名 → **合规**。
- **硬编码色**：本组无 `RGB(`/`SetTextColor`，一律 `theme_` 传入菜单/报告 → **合规**。
- **命名空间**：均为 `App::` 成员 + 匿名 namespace 内辅助函数，与仓库风格一致。
- **单文件行数**：`app_commands_files.cpp` 903、`app_commands.cpp` 558 均超 500 硬上限，
  但已在 `data/file-length-baseline.json` 记账（`max:500`，`903`/`558`），属**已记账欠账未回退**，
  按棘轮规则不算新增违规。`compare.cpp:4` 与 `scan.cpp:4` 的注释自认"app_commands_files.cpp 已超行数上限"
  —— 拆分只把代码挪走，欠账仍在，值得同期销账（把 `files.cpp` 的视图族拆出去）。

---

## 五、总体评价

**做对的地方（这批文件的骨架是好的）**：
- 失败语义最完整的是 `app_commands_jobs.cpp` 的 `CollectResult`（cancelled / timedOut / failed 三分 + `FailKind` 转交），
  以及 `compare.cpp`/`scan.cpp` 把"有没有扫完"写进 `complete/timedOut/cancelled` 三个诚实字段 —— 这是全仓最好的样板，
  建议作为"长任务命令"的标准形态固化。
- 人机双入口的门控**大部分正确**：`BmkImport/BmkExport`、`DupFinder`、`FolderCompare`、`FileHash`、`BatchRename`、
  `NamedSessionSave`、`SessionList/SessionDelete` 都用了 `IsHumanCall(args)`，而不是危险的 `args.IsEmpty()` ——
  `scan.cpp:194-197` 那段注释（"只看空参数会把管道卡到超时"）说明这个坑是被认真填过的。
- 参数校验的正面样板也在组内：`archive.cpp` 的 repeats 逐项类型校验、`search.cpp` 的 range id 严格拒绝、
  `fs.cpp` 的 `ToShellPath` 归一化、`compare.cpp` 的 `minBytes<=0` 归零。

**必须马上处理的三件事**（其余可排队）：
1. **`file.select.pattern` 的模态框要加 `IsHumanCall` 门控**（H1）—— 这是"aiExposed 命令在 AI 空参时弹模态框"的教科书案例，
   而 schema 里 `pattern` 连 required 都不是，等于把触发条件写在了对外契约上。
2. **`FsDelete` / `fs.copy` / `fs.move` 的"部分成功"必须与注释对齐**（H3/H4）—— 注释已经写对了（"返回 false 让调用方知道没全照办"），
   代码没跟上；这三条都是破坏性命令，`ok=true` 的代价是"AI 以为已经清空/已搬完"。
3. **机器侧不得隐式依赖用户选中项**（H2/M5）—— `BatchRename`（改用户文件）与 `FileHash`（读用户文件）都把
   "省略就落到当前选中"写进了 schema，却没有 `needsSelection=true` 也没有 `IsHumanCall` 门控，
   等于让"机器动作"借用了"人的上下文"。这两条要么改声明、要么改行为，不能两头都不动。

**整体判断**：这批文件不是"写得糙"，而是**同一批纪律在不同文件里执行深度不同** ——
`fs.*`/`scan.*`/`jobs.*`/`search.*` 明显是"被审计过一遍"的写法（原因、kind、完整性字段齐全），
`file.*`（`files.cpp`）与 `tabs.cpp` 的边界分支则停留在"能跑就行"的旧形态（恒 true、无原因、注释与实现相反）。
审查发现的 4 条高危里有 3 条集中在 `app_commands_files.cpp` 一个文件上，而它恰好也是 903 行的欠账文件 ——
**拆文件与修缺陷应该同批做**：把 `files.cpp` 的"视图/选择族"拆出去时，顺手把 `WithView` 的原因缺失与
两条 `aiExposed` 选择类命令的门控补齐，成本最低。
