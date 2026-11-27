# CHANGELOG 0.2.1 详细记录

> 本文档从主 CHANGELOG.md 迁出，保留该版本的完整开发过程记录（验证、踩坑、逐条 why）。
> 主 CHANGELOG.md 对应版本段只保留精简摘要。

## [0.2.1] - 2026-11-20

**操作契约结构化 + AI 操作接口 + 便携存储 + 仓库独立。**  
这一版不是加功能，而是把"同一件事有多份实现"逐处收口：命令参数、效果、前置条件、返回值、  
命名、导航语义、历史栈、窗口类注册、日志、剪贴板、字形码位、tooltip 都各自收敛到一个实现，  
再用 3 个静态检查脚本 + 20 个单测（354 项断言）把回退挡住。

### 变更（仓库独立：从 Windhawk 主仓脱钩）

- **`E:\ExBarBrowser` 成为独立 git 仓库**（分支 `main`）。此前它靠 `D:\Windhawk` 根  
  `.gitignore` §8 的白名单放行，寄生在主仓里：改一个分片要在别人的仓库里 `git add ExBarBrowser/`，  
  提交历史与 ExBar mod 的 7.7 万行混在一条时间线上，两边互不相关的改动互相干扰。  
  迁到 E 盘后工作区已不在主仓路径下（主仓 `git status` 因此变成 95 条 `D`），继续寄生只会留下  
  一堆"已删除"的幻影，因此改为独立仓库。主仓里那 95 条删除由操作者自行决定何时清理 ——  
  本仓库不引用主仓，主仓也不引用本仓库，先清后清都不影响这边。
- **入库范围不变，但语义变了**：以前"不入库"是为了不给主仓添产物噪音；现在"不入库"意味着  
  **克隆即可构建、构建不产生提交**。`data/` 整目录仍不入库（书签/键位/窗口几何是个人数据），  
  新克隆首次启动会走内置默认表 + 空书签树，`settings.json` 首次保存时自动创建。
- **`src/keymap.gen.h` 明确入库**：它是 `data/keymap.json` 的生成物，但也是"配置文件缺失时"  
  的兜底默认表 —— 不入库会让新克隆在无 `data/` 时没有任何热键。`check_commands.py` 负责  
  保证两者一致，所以"生成物入库"在这里不是坏味道。
- **搬目录踩到 PCH 的绝对路径**：`obj/common.h.gch` 是 clang AST 二进制，里面记着  
  `D:/Windhawk/ExBarBrowser/src/common.h`；搬完直接 `bash build.sh` 会得到 57 个 TU 全报  
  `malformed or corrupted AST file`。`build_fast.py` 不会自动察觉路径变更，需要一次  
  `--clean`（冷 PCH 实测 29.8s，其中 PCH 14.1s；PCH 命中后的全量重建是 10.5s）。已写进 README §3.1 与 §8 坑表。
- **README / CHANGELOG 全量重核对**：现状表从"10,200 行 / 127 条命令 / 94 已实现 / exe 12 MB"  
  更新为实测值（当时 18,388 行、137 条、108 已实现、86 对 AI 暴露、18.9 MB / strip 1.3 MB），  
  §3.4 版本控制整节重写，§5 架构图补 `ai_api`/`ai_server`/`shell_input`/`nav::History`/  
  基础设施收编，§6 目录树补全 20 个新模块，§8 坑表补两条（PCH 路径、生成头不同步）。

### 变更（导航收口为组件，全局复用）

导航以前是"同一件事写多遍"：路径语义有两套实现（`common.h` 无长度限制、  
`NavModel` 基于 `PathRemoveFileSpecW` + MAX_PATH），历史栈的压栈动作在  
`NavigateActive` / `OnTabNav` / `OnTabNavFailed` / `GotoHistory` 四个函数里  
各手写一遍（`hist.resize(idx+1)` + `push_back` + `idx = size()-1` 共 4 份），  
"此电脑"的 CLSID 硬编码在 3 个文件里。这类重复的代价不是啰嗦，而是**症状远离  
现场**：用户看到"后退要按两次"，根因在别处的 `push_back`。

- **新增 `src/nav_path.h` —— 路径语义唯一实现**（纯字符串、无 MAX_PATH 限制）：  
  `IsNamespace` / `IsDriveRoot` / `Normalize` / `ParentOf` / `Leaf` / `UpTarget` /  
  `CanNavigateUp` / `kThisPc`。`common.h` 的旧名字保留为**转发**，不再有第二份实现。
- **新增 `src/nav_history.{h,cpp}` —— 历史栈事务组件**（每个标签一份）：  
  `Request` / `Goto` / `Complete` / `Fail` / `ClearKeepingHere` / `Restore`，  
  压栈动作只有 `History::Push` 一处。`App::Tab` 里原来的  
  `hist/idx/pending/pendingPushed/pendingPrevIdx` 五个字段收敛成一个 `nav::History`。
- **删除 `NavModel::ParentOf` / `LeafOfPath` 的实现**，改为转发 `nav::`。  
  顺带修掉一个静默缺陷：旧实现超过 MAX_PATH 直接返回空，导致**超长路径的地址栏  
  Tab 补全失效**；现在走无长度限制的实现（已有单测覆盖）。
- **删除 `common.h::CollapseHistoryToCurrent`**，规则并入 `History::ClearKeepingHere`。
- **导航按钮改走 `CommandBus`**：工具栏的 ←、→、↑、刷新以前直接调 `GoBack()` /  
  `GoUp()` 等，绕过命令总线（因而绕过了前置检查与审计）—— 这正是上一版  
  "按钮能点、双击没反应"的分裂来源。现在按钮与双击/热键/序列走同一条命令。
- **按钮灰显读同一个谓词**：`NavBar::SetPath` 用 `nav::CanNavigateUp`，  
  与命令前置检查 `Req::CanGoUp` 同源，"灰着"必然等于"点不动"。
- **命令拒绝原因对外可见**：`CommandBus` 新增 `lastBlockReason()`。  
  以前 `Dispatch` 算出了原因却只写日志与审计，AI/脚本侧只能看到笼统的  
  "命令执行失败"；现在返回真实原因（如"已经是顶层（此电脑或网络根）"）。

### 变更（存储改为便携模式：数据放 exe\data）

- **配置目录从 `%APPDATA%\ExBar` 迁到 `<exe>\data`**：整个程序目录可整体拷走，  
  不写注册表、不写用户目录，便于随身/绿色部署。迁过去的是**真实配置**：  
  `settings.json`、`bookmarks.html`、`keymap.json`、`mouse.json`、`audit.log`。  
  书签同时不再与 ExBar mod 共用同一份 `bookmarks.html` —— 两边各写各的，  
  避免互相覆盖。旧目录原样保留不删（可回退、可对照）。  
  代价：exe 放在 `Program Files` 这类只读目录时配置写不进去，需要把整个目录放到  
  用户可写位置；`Config::Writable()` 提供只读检测，写入失败只记一次日志不刷屏。
- **`browser.json` 改名 `settings.json`**：与前两项一致 —— `browser-` 前缀原本是为  
  避开 ExBar mod 的同名文件，现在数据目录独立，前缀没有存在意义。  
  代码里 `Config::SettingsPath()` 与相关日志一并改为 `settings.json`；  
  首次启动若只有旧的 `browser.json`，重命名即可（两者 Schema 完全相同）。
- **去掉 `browser-` 前缀，键位/鼠标规则合并为单文件**：`browser-keymap.json` →  
  `keymap.json`、`browser-mouse.json` → `mouse.json`。这个前缀是历史遗留：数据目录  
  曾是 `%APPDATA%\ExBar`，与 ExBar mod 共用，而 `keymap.json` / `mouse.json` 正是  
  mod 的文件名，不加前缀会互相覆盖。现在数据目录已与 mod 分离，撞名风险消失；  
  同时取消"种子 + 用户覆盖"的双文件机制，只保留一套真实配置，用户直接编辑。
- **不再有任何种子/默认值副本**：`Config::SeedAssets()` 整个删除，仓库里也不再带  
  示例 `bookmarks.html`。书签文件缺失时走空书签分支（`BookmarkStore::Load`  
  返回 false 即空树），不需要种子兜底。
- **`data/` 全部不入库**：这些文件因人而异（书签、最近访问、窗口几何、键位习惯），  
  已在 `.gitignore` 中排除，并用 `git rm --cached` 从索引中解除跟踪（磁盘文件保留）。

### 修复（持久化缺口）

- **`closedTabs` 只写不读**：退出时保存了关闭历史，但启动时从未调用  
  `TabSet::SetClosed`，导致 `Ctrl+Shift+T` 重开标签在重启后永远是空的。  
  现在 `App::Run` 启动即恢复。实测：注入两条关闭历史 → 启动 → 退出，条目完整保留。
- **视图模式不持久化**：`viewMode_` 只活在内存里，每次重启都回到"小图标"。  
  现在 `Settings::viewMode` 随 `ApplyViewMode` 落盘，新标签启动时套用。
- **排序/分组不持久化**：补 `Settings::sortMode` / `sortAscending` / `groupBy`，  
  通过 `ViewSort` / `ViewGroup` 命令写回。
- **导航窗格/预览窗格显隐不持久化**：补 `Settings::showNavPane` / `showPreviewPane`。  
  两个窗格的偏好必须在**首次导航前**就位（`EBO_SHOWFRAMES` 与  
  `IExplorerPaneVisibility` 只在建窗格时被消费），因此 `ShellView::Create`  
  一开始就把配置读进 `navPaneWanted_` / `g_previewPaneWanted`，避免"第一次打开时  
  窗格还在，切一次目录才生效"。
- **最近访问写盘时机**：`AddRecent` 只改内存、全程序仅"清除历史"处 `Save()`，  
  正常浏览产生的记录要等用户改别的设置或正常退出才落盘，强杀即丢。  
  现在 `AddRecent` 置脏位（`Config::dirty()`），退出时统一写盘，  
  浏览过程中不写文件；原地导航到同一路径不再无谓置脏。

### 修复（"上一级"在盘符根失效）

- **盘符根双击文件列表空白无反应**：`C:\`、`D:\` 这类盘符根上，双击空白本该回到  
  "此电脑"（与资源管理器一致），实际毫无反应。根因不是漏写分支，而是**同一件事有  
  三套判定**：`App::GoUp()` 里写了"盘根 → 此电脑"的分支，但命令总线在  
  `Req::CanGoUp` 前置检查处就把它拒了 —— 状态探测（`app.cpp`）把盘根算成  
  "不能向上"。于是同一个动作两条路径行为分裂：工具栏的"上一级"按钮能跳（它绕过  
  命令总线直调 `GoUp()`），双击空白跳不动（走命令总线被拦）；  
  `NavModel::IsRoot()` 又是第三套口径（盘根不算 root），按钮因此还是亮的。  
  现在判定收敛成 `common.h` 的 `CanNavigateUp()`，前置检查、`GoUp()` 执行、  
  导航栏灰显三处共用。审计日志里原来是一串  
  `NavUp (blocked: 已经是顶层（盘根或网络根）) ok=0`，现为 `ok=1`。
- **顺带修掉 `ParentOf()` 的 UNC 判定**：`\\server\share\dir` 的父级是  
  `\\server\share`（有效上一级），原代码 `cut <= second` 把它一起判成"没有上级"，  
  网络共享下任意子目录的"上一级"都静默失效。改为 `cut < second`。
- 拒绝原因文案同步为"已经是顶层（此电脑或网络根）"—— 原来写"盘根"，  
  而现在盘根是**能**向上的，旧文案会误导。

### 移除

- **两个死配置**：`Settings::layout` 与 `Settings::mouseRules` 会读写但全项目无  
  使用点（鼠标规则实际只从 `mouse.json` 读）。字段与 `Config::Load/Save`  
  中对应行一并删除；旧配置文件里的这两个键在下次保存时自然消失。

### 安全

- **AI 令牌只在运行时有效**：`ai::StopServer()` 此前不清理发现文件，  
  `aiEnabled` 关掉后 `ai-token.txt` / `ai-pipe.txt` 仍留在磁盘上，  
  看起来像通道还开着。现在停服即删除这两个文件。

### 变更（命令契约结构化，第 1–10 步）

- **第 10 步：偿清"只能靠选中"的债务。** 债务表原本只登记 4 条，实际  
  "依赖选中且已实现"的有 15 条 —— 大部分从没被登记。补齐后新增 5 条确定式命令：

  `FsOpenInNewTab {path}`、`FsOpenInNewWindow {path}`、  
  `FsCopy {targets[]}`、`FsCut {targets[]}`、`FsProperties {target}`。

  三个值得记的点：  
  ① **`FsCopy` 不产生副本** —— 它与 `Ctrl+C` 同义，只是"准备好等粘贴"，  
  想真复制到某处要用 `FsCopyTo`。这两件事极易混淆，所以在注释、文档、  
  别名（`fs.copy.toclip` vs `fs.copy`）三处都标了。  
  ② 造 `IDataObject` 用 `CIDLData_CreateFromIDArray` 而非 `SHCreateDataObject`  
  （后者被 `NTDDI_VERSION` 包着，某些配置下不可见）。两者都比自己拼 `CF_HDROP`  
  好 —— 后者丢掉"来自哪个命名空间"，粘贴到某些目标会失败。  
  ③ **一个路径解析不了就整批失败**。漏一个会造成"我复制了 5 个实际只复制了 3 个"  
  而调用方以为全成了。

  另外：写这 5 个 handler 时我先把公共逻辑抽成 lambda，结果静态检查立刻报  
  "参数没取用"——第 9 步踩过同一个坑。这次改成不抽公共函数、三个 handler 各写  
  三行重复代码：**例外表越长检查能挡的问题越少，重复三行换覆盖值得**。这条写进  
  注释了，免得后来人"优化"回去。

  顺带修掉检查器的一个 bug：`COVERS` 遍历外套了 `if old in KNOWN_NO_TARGET`，  
  导致 `CmdRename`/`CmdDelete` 在覆盖表里却从不被打印 —— 覆盖情况看起来比实际更差。

  指标：命令 132 → 137，已实现 103 → 108，AI 可调用 81 → **86**，  
  债务表未覆盖 3 → **2** 条。

### 变更（命令契约结构化，第 1–9 步）

- **第 9 步：补齐三处能力缺口。** 按"机器最缺 → 失败最不可解释 → 最实用查询"的  
  优先级依次做，每项都先对照成熟项目的做法。

  **`FsOpen {path}`（新增）。** `CmdOpen` 依赖选中，机器无法表达"打开第 3 个文件"。  
  语义与 `CmdOpen` 一致：文件夹在当前标签进入，文件交 shell。不加 `mode` 参数是  
  参照 VS Code —— 它把打开拆成 `open`/`openWith`/`revealInExplorer` 三条独立命令，  
  因为那是三种意图，各值得一个名字。路径不存在时直接失败而不弹系统框  
  （确定式调用不该卡在没人看的对话框上）。

  **前置条件 16% → 30%。** 参照 VS Code 的 `when` 子句 + `enablement`：把"边界处  
  会静默失败"的条件声明出来。新增 4 个条件 —— `CanMoveTabLeft`/`CanMoveTabRight`  
  （`TabMoveLeft` 有 `toIndex<0→0` 钳制，已在最左时按了没反应）、`HasClosedTabs`、  
  `HasMultipleTabs`、`HasSelectionSingle`。共给 18 条命令补了条件，其中 `CmdRename`  
  原先完全没声明（无选中时会弹框）。

  **`OpTopN` / `OpDiskUsage`（新增 `src/disk_scan.{h,cpp}`）。** 三条硬约束：  
  ① 必须能中途停（给 deadline，超时带已有结果返回 —— 几万文件的递归卡在 UI 线程  
  就是假死）；② 必须报告完整性（`truncated`/`timedOut`/`elapsedMs`/`scannedFiles`，  
  只给数字而不说"没算完"会被当成真值）；③ 不跟随重解析点（跟 junction 无限递归，  
  且把链接目标大小算到链接处，数字是错的）。

  两条命令而不是一条带 mode：`OpTopN` 答"什么最占空间"（看 `entries`），  
  `OpDiskUsage` 答"这目录占多少"（看 `totalBytes`）—— Unix 上 `du` 与 `ncdu`  
  也是两个工具。`totalBytes` 统计截断前的全部，否则"总量 < 各子项之和"会让人困惑。

  新增 `tests/disk_scan_test.cpp` 25 项；`precondition_test` 从 26 → 42 项。  
  合计 257 项断言全通过。

### 变更（命令契约结构化，第 1–8 步）

按 `docs/OPERATION-MODEL.md` §7 的顺序推进"让每个操作同时对人可用、对机器可调"，  
七步机制 + 第八步内容补全已落地，共 219 项断言 + 14 组静态检查守住。

- **第 8 步：内容补全（AI 可暴露面 45 → 78）。** 七步做完的是**机制**，  
  不等于每条命令都填了内容。这一步按三条判据逐条判断该不该暴露：  
  ① 已实现（否则出现在 `tools/list` 里却调不动）；② 不依赖当前选中  
  （机器无法指定"选哪个"，暴露出去就是"AI 以为删了 A，实际删了 B"）；  
  ③ 不是纯人机 UI（弹菜单、开关面板、退出程序）。

  新暴露 33 条：`TabMove*`(4)、`GoTab1..9`、`CloseLeftOne`/`CloseRightOne`、  
  `PathCopyDir`/`PathCopyTab`、`CmdSelectAll`、`FileFocusTop/Bottom`、  
  `CmdNewFolder`/`CmdNewTextFile`/`CmdPaste`、`CycleHiddenSeg`、`FocusAddress`、  
  `BmkAddCurrent`、`SessionSave/Restore`、`NewWindow`、`OpenTerminal(Admin)`。

  仍不暴露 22 条，每条有理由，已由静态检查逐条标注：  
  17 条依赖选中（`CmdDelete`/`CmdRename`/`PathCopy*` 等 —— 其中 4 条有 `Fs*` 确定式版本  
  可替代，其余是真实缺口）；5 条纯人机 UI（`QuitExplorer`/`FolderOptions`/`MapDrive`/  
  `UnmapDrive`/`CommandPalette`/`HotkeyManager`）。

  **顺带修掉一个标注错误**：`PathCopyDir` / `PathCopyTab` 早先和 `PathCopy` 一起标了  
  `needsSelection=true`，但它们的实现用 `ActivePath()` / `tabSet_`，**根本不看选中**。  
  标注与行为不符，把这两条错误地排除在机器能力之外。这提醒 `needsSelection` 是人写的  
  标注，必须对照实现核实。

  新增静态检查第 14 项：暴露的命令必须"已实现 + 不依赖选中 + 非纯 UI"，  
  违反即报错。
- **第 7 步：对外暴露（MCP 兼容）。** `ai_server` 加 JSON-RPC 2.0 层：  
  `initialize` / `tools/list` / `tools/call` / `exbar/state`。  
  上一轮的自定义格式（`{"token":..,"method":..}`）对 MCP 客户端不可用 ——  
  每个客户端都得自己写转换层就失去了"开箱可连"的意义。现在靠**有无 `jsonrpc`  
  字段**区分两种协议，既有本地脚本零改动。

  三处关键决定：
  - **错误分两类**（MCP 硬要求）：请求不合法 → JSON-RPC `error` 带 `code`；  
    工具执行失败 → `result.isError=true`。混在一起客户端分不清"我调错了"  
    还是"环境不满足"，只能盲重试或盲放弃。
  - **令牌检查在方法分派之前**：否则能靠"返回 method-not-found 还是 bad-token"  
    枚举有哪些方法 —— 能力面泄露。单测断言"未知方法与已知方法的未授权响应完全一致"。
  - **`description` 从结构化字段生成，不手写**：手写的会与 `annotations` 脱节  
    （改了 `destructive` 忘了改描述），两者不一致时模型会做错判断。
  验证用了**真实管道往返**（`tests/pipe_e2e_test.cpp` 35 项）：进程内起服务、  
  自己当客户端连接、走完 initialize → tools/list → 成功调用 → 缺参数 →  
  未知工具 → 错误令牌 → 停止清理。47 个工具全部带 name/description/  
  inputSchema/annotations。
- **第 6 步：命名空间化。** 新增 `src/cmd_naming.{h,cpp}`：131 条命令的旧名 ↔ 点分名  
  双向映射（`NavBack` ↔ `nav.back`、`CmdDelete` ↔ `file.delete`、`FsDelete` ↔ `fs.delete`）。  
  `FindCommand` 两个方向都认，所以既有 keymap / 序列 / 鼠标规则**零改动**，  
  外部调用方可以开始用稳定名字。

  命名规则：全小写、点分、两段。域取自 `CommandDef::group` 的小写 —— 不另造分类，  
  否则两套"分组"迟早不一致。单测里有一条断言"每个点分名的域前缀必须与该命令的  
  group 对应"，这条在写测试时立刻抓出了 `AiTogglePanel` 被错标成 `tool.*`  
  （它是 `group=Window`）。

  两处有意为之：**大小写不敏感**（与旧名的 `_wcsicmp` 行为一致，避免"`NavBack`  
  能用但 `NAV.BACK` 不行"的半吊子行为）；**确定式命令用 `fs.*` 而非 `file.*`**  
  （"对指定路径操作"与"对当前选中操作"不是同一件事，同名域会让人以为只是参数不同）。

  `tools/list` 现在同时返回规范名 `name` 与 `legacyName`：新客户端只学一种，  
  既有脚本/审计日志用另一个对照。

  判据 `tests/naming_test.cpp` 31 项。
- **第 5 步：返回值。** `CommandBus` 新增一条**独立的**"带结果"注册通道 ——  
  `RegisterWithResult` / `InvokeResult` / `HasResult`，`CommandDef` 加 `outputSchema`。  
  为什么要独立通道而不是让 131 条 handler 全返回 `json::Value`：绝大多数命令是  
  "做了/没做成"，让它们都构造 JSON 是纯负担，还容易让写操作假装有返回值。

  头两条落地的是 `OpListDir`（列出当前目录）和 `OpStat`（查单项属性），  
  底层新增 `shellops::ListView` / `StatItem`。它们查的是**当前视图**而不是按路径  
  扫盘 —— 与用户看到的完全一致（含虚拟命名空间、搜索结果、过滤状态），  
  也不需要重新判断访问权限。

  一个容易忽略的细节：无返回值的命令必须给**空对象**而不是留旧数据。  
  调用方复用同一个 `json::Value` 时若不清空会读到上一次的结果 —— 那比"没有结果"  
  更危险，因为它看起来像真的。单测里专门断言了这件事。

  判据 `tests/result_test.cpp` 20 项。
- **第 4 步：交互与确定分离。** 新增四条确定式命令 —— `FsRename`（`{target,newName}`）、  
  `FsDelete`（`{targets[]}`）、`FsCopyTo` / `FsMoveTo`（`{targets[],to}`）：  
  目标由参数点名，绝不弹框、绝不读"用户碰巧选中了什么"。  
  旧的 `CmdRename` / `CmdDelete` / `CopyToFolder` / `MoveToFolder` **保持原样**  
  —— 它们的语义里"弹框问用户"是不可分割的一部分（`F2` 的体验就是弹输入框），  
  加参数会让"传了参数就不弹框"变成隐式分叉。拆开后人机各走入口，逻辑共用。

  实现上**不是**另写一份按路径的文件操作，而是 `shell_ops::SelectByPath` 先选中  
  目标、再复用既有的 Selection 实现 —— 否则回收站/冲突/权限/撤销这些语义会有  
  第二份实现，迟早与既有行为分叉（架构规则 R6）。

  `shell_ops` 新增：`SelectByPath` / `SelectNone` / `SelectedSinglePath` / `RenameTo`。  
  其中 `RenameTo` 用 `IFileOperation` + `FOF_SILENT` + `FOF_NOERRORUI`：  
  确定式调用遇到冲突时**返回失败**而不是弹框 —— 明确的失败比没人看的对话框更可预期。

  判据 `tests/fs_ops_test.cpp` 44 项。债务消化 4/7，剩余 `CmdCut`（剪切改的是剪贴板，  
  语义特殊）、`BatchRename`（等 PowerRename）、`PathCopyLink`（低优先级）已登记。
- **第 1 步：参数对象化。** `CommandBus::Handler` 从  
  `function<bool(const std::wstring&)>` 改为 `function<bool(const Args&)>`  
  —— 参数从"一个需要自己拼的字符串"变成具名对象，一次性解决多参数、可选参数、  
  类型校验、自动生成 UI、向后兼容加参数五件事。  
  旧的单值规格（keymap/序列/鼠标里的 `"ViewMode:3"`）经 `ArgsFromLegacyArg`  
  **从 `argsSchema` 推导键名**，因此删掉了 `ai_api` 里那张 `kArgKeys` 手工映射表  
  —— 那张表就是"参数是字符串"这个缺陷的症状。  
  判据 `tests/args_test.cpp` 22 项：新旧两条路径对同一操作结果一致。
- **第 2 步：效果四维化。** `Risk{Read,Write,Danger}` 拆成  
  `Effects{readOnly, destructive, idempotent, openWorld, undoable, scope}`，  
  127 条命令逐条按真实行为重标（只读 14 / 破坏性 21 / 跨进程 22 / 幂等 41）。  
  单轴表达不了"幂等的写"与"不可逆的写"的区别 —— `NavRefresh` 和 `CmdPaste`  
  原来同归 `Write`，但前者可安全重试、后者不可撤销。  
  `tools/list` 现在输出 MCP 风格的 `annotations`（四个 hint + scope）。
- **第 3 步：前置条件。** 新增声明式 `Requirements` + `CanExecute()` + `ReqReason()`，  
  21 条常用命令已标注。`Invoke` 现在先过前置检查，拒绝时审计记  
  `(blocked: 原因)`。这修掉了"无历史时点后退毫无反应"这类哑失败 ——  
  以前 handler 静默 `return false`，界面和日志都没有线索。  
  判据 `tests/precondition_test.cpp` 26 项。

顺带修掉两个静默缺陷：

- **参数被丢弃**：`TabNew` 声明了 `path` 参数但 handler 从没读过。新增的静态检查  
  （`check_ai_meta.py` 第 3 项）会比较"schema 声明的键"与"handler 实际取用的键"。
- **空 schema 的不安全默认**：122 条原为 `"{}"`，按 MCP 规范语义是"接受任意参数"，  
  已全部改为 `{"type":"object","additionalProperties":false}`。

已登记的债务（`check_ai_meta.py` 的 `KNOWN_NO_TARGET`）：这 6 条破坏性命令仍只能作用于  
"当前选中项"，本身没有目标参数。第 4 步已为其中 4 条提供确定式版本（机器改用新入口），  
剩余 `CmdCut` / `BatchRename` / `PathCopyLink` 待做 —— 它们的共同点是"改动的不只是文件"：  
剪切改剪贴板、批量重命名要等 PowerRename、快捷方式写入走另一套 shell 接口。

### 新增

- **AI 操作接口（人机同源）**：给命令表补上 `risk` / `argsSchema` / `aiExposed` /  
  `needsSelection` 元数据（架构规则 R18），并新增 `ai_api`（工具投影、参数校验、派发）  
  与 `ai_server`（命名管道 + 令牌 + 请求泵）。AI 的所有动作都走同一条 `CommandBus`，  
  审计记 `actor=ai`，不存在"AI 专用"的第二份实现。对外 41 条工具，集中在导航、  
  标签切换、视图调整、书签栏显隐。

  三条设计取舍：
  - **默认关闭**：需 `settings.json` 里 `aiEnabled=true` 才监听管道；
  - **依赖选中项的命令不暴露**（`CmdDelete`/`CmdRename`/`CmdPaste`…）：AI 无法用参数  
    指定目标，暴露出去会作用在"用户碰巧选中的"对象上。要支持这类操作需新增能用参数  
    指定目标的语义命令，而不是把 UI 命令硬暴露；
  - **危险命令双门**：`aiAllowDanger`（设置）+ `approve=true`（调用方）同时满足才执行。
  配套 `tools/check_ai_meta.py` 静态对账（暴露/参数/选中项三方一致），  
  `tests/ai_api_test.cpp`（34 项）+ `tests/ai_server_test.cpp`（8 项）单测，  
  说明见 [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md)。

### 变更

- **分隔符行高 24px → 2px**：地址栏下拉的分组分隔线原来占满一整行（120 DPI 下 30px），  
  看起来像个空条目。现在分隔线只占 2px：`PopupList` 从 `LBS_OWNERDRAWFIXED` 改为  
  `LBS_OWNERDRAWVARIABLE`，逐行设高（`LB_SETITEMHEIGHT`）+ `WM_MEASUREITEM`。
- **地址栏下拉新增"清除历史记录"**：位于列表底部（前面加分隔线），清空当前标签的  
  后退/前进栈与"最近访问"列表，保留当前目录作为唯一历史条且不触发导航。  
  同时修掉方向键会停在分隔行上导致"按回车没反应"的问题。

### 修复

- **双击文件列表空白区没有返回上一级**：规则引擎、序列键和命令总线早已具备，但 Shell  
  文件视图从未把命中的目标喂给 `InputRouter`——`mouse.json` 里也没有 `empty` 规则。现在  
  新增 `src/shell_input.{h,cpp}`：Win10 走 `SysListView32` hit-test，Win11 走 UIA  
  `ElementFromPoint` + 有限祖先上溯，把 `empty/file/folder/group/column/tree` 翻译成  
  `MouseTarget`，经 `MouseSink` 汇入现有规则引擎；`SHELLDLL_DefView` / `SysListView32`  
  / 导航树在每次视图重建后自动重装子类。默认规则补上 `empty × left × double → NavUp`。  
  实测从 `docs` 双击空白回到父目录，审计为 `mouse NavUp ok=1`。
- **冷启动第一次双击空白仍会偶发失效**：Win11 DirectUI 的点击只经 `WM_PARENTNOTIFY`  
  上抛，之前这条路径把目标直接写成 `unknown`，两次点击都进不了规则；同时 UIA 惰性启动  
  的单次失败会立刻进入冷却，首击目标拿不到。现在 `WM_PARENTNOTIFY` 的 `DOWN` 也做  
  UIA/ListView 命中测试（`UP` 沿用 pending 目标），并通过 `WM_APP_MOUSE_*_NOUP` 接上  
  `InputRouter` 的 `expectUp=false` 契约，派发后立刻释放 claim；UIA 失败累计 3 次探测才冷却，  
  `InputRouter` 为首击 `unknown` 保留匿名 pending、由第二击补上目标。冷启动冷进程复测：  
  `before=docs → after=ExBarBrowser`，审计 `mouse NavUp ok=1`。
- **面包屑分隔符列成了"同级目录"**：点 `D:\Windhawk` 右侧的 `›` 弹出的是 `D:\` 下的同级  
  文件夹（Downloads、Program Files…），而不是 `D:\Windhawk` 自己的内容。原因是  
  `NavBar::ShowSiblingMenu` 先对分段路径取 `ParentOf()` 再枚举，得到的是"当前级的兄弟"。  
  分隔符语义上归属于它**左边**那一级，改为直接枚举 `parts[partIndex].path` 的子文件夹，  
  与 mod `ShowCrumbMenu` 的 `EnumChildFoldersAt(crumbPidls[partIndex])` 对齐；函数随之  
  更名为 `ShowChildMenu`。副作用是盘根分段（`D:\`）的 `›` 现在也能列出该盘顶层文件夹。
- **隐藏预览窗格后一换目录又自动打开**：以前读 `FOLDERFLAGS` 的 `0x400` 当"预览窗格"  
  位——那是 `FWF_NOSCROLL`，几乎所有视图都置位，于是每次都判成"已打开"，导航时又把窗格  
  打开。`FOLDERFLAGS` 里根本没有"显示预览窗格"这一位；真正决定窗格是否创建的是宿主提供的  
  `IExplorerPaneVisibility::GetPaneState`。现在 `ShellView` 通过 `IServiceProvider`  
  进入 ExplorerBrowser 站点链并实现 `IExplorerPaneVisibility`：关闭态返回  
  |  |  |  |  
  | --- | --- | --- |  
  `EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE`；"是否已显示"改以窗格宿主窗口  
  （`Shell Preview Extension Host`）的真实可见宽度为准采样，并在 `OnNavigationPending`  
  里采样，覆盖双击文件夹等 shell 内部导航。`TogglePreviewPane`（`Ctrl+Alt+P`）与  
  shell 自带关闭按钮都形成持续意图。
- **选中文件不显示高亮**：根因是**我们对 shell 内部窗口调了 `SetWindowTheme`**。  
  `ApplyTheme`/`OnViewCreated` 会遍历视图子树，把 `ExplorerBrowserControl`、  
  `SHELLDLL_DefView`、`DirectUIHWND` 等一律设主题；DirectUI（Win11 文件列表）  
  因此改用另一套 `SubAppName` 样式字典重新解析 UIFILE，之后选中项不再绘制高亮底色  
  ——但选中集本身是完好的（状态栏"已选 N"正常），所以看起来像"焦点问题"，实际是绘制被改坏。  
  现在只对 `ExplorerBrowserControl` 这一层设主题（与同类宿主项目做法一致），交给 shell  
  自己把深色下传给内部 DirectUI；内部窗口一概不碰，只单独深色化 `ScrollBar` 小控件。  
  另移除了对 DirectUI 树反复广播 `WM_THEMECHANGED` 的 `PokeTheme`。  
  依据 `tools/probe_highlight.cpp`（离屏、不抢焦点、不模拟鼠标，选中前后像素差分判据）：
  | 动作                               | changed | peakCount | 结论      |
  | -------------------------------- | ------- | --------- | ------- |
  | 基线（不改主题）                         | 4784    | 174       | 有高亮     |
  | 只对顶层 `ExplorerBrowserControl`    | 4784    | 174       | 有高亮     |
  | 对全子树 `SetWindowTheme`            | 1224    | 14        | **无高亮** |
  | 只给 `ScrollBar` 设主题               | 4784    | 174       | 有高亮     |
  | 只调 `AllowDarkForWindow`          | 4784    | 174       | 有高亮     |
  | 只发 `WM_THEMECHANGED`             | 4784    | 174       | 有高亮     |
  | 先画坏再 `SetWindowTheme(NULL,NULL)` | 4784    | 174       | 可恢复     |
- **选中项不高亮时键盘也失灵（伴随问题）**：焦点此前只给到视图容器  
  `ExBarBrowser_TabView`，从未交给 `ExplorerBrowserControl`，框架因此不知道本视图被激活，  
  type-ahead/方向键/右键上下文都受影响。现在容器收到 `WM_SETFOCUS` 时先把焦点交给  
  `ExplorerBrowserControl`（IExplorerBrowser 自己的宿主窗口，它再走  
  `OnViewWindowActive → UIActivate`），随后补一次 `UIActivate(SVUIA_ACTIVATE_FOCUS)`。
- **历史栈每步压两条 → 后退要按两次**：以前 `NavigateActive` 先压"请求路径"，Shell 完成导航  
  后再压"真实路径"（重定向 `文档 → OneDrive\文档`、尾斜杠、正斜杠规范化都会触发）。  
  现在 `Tab::pending` 记录本次请求，完成时**就地校正** `hist[idx]`（多级重定向逐跳收敛），  
  视图内部双击才压新条；后退/前进统一走 `GotoHistory`，导航失败会把游标退回。
- **复制/剪切文件、拖放全部失效**：`WinMain` 只调了 `CoInitializeEx`，而 `OleSetClipboard` /  
  `OleFlushClipboard` 和 shell 视图的 `RegisterDragDrop` 都要 OLE。改为 `OleInitialize` /  
  `OleUninitialize`。
- **Ctrl+Shift+1..8 全部变成"超大图标"**：8 个键绑的是同一个 `ViewMode` 且没有参数，  
  `_wtoi("")==0`。命令规格现在支持 `Name:arg`（`ViewMode:3`），`CommandBus::InvokeByName`  
  统一拆分，键盘/序列/鼠标/面板同一条路径。
- **9 条序列 `g 1`…`g 9` 是空动作**：keymap 绑了 `GoTab1..9`，命令目录里却没有这些 id，  
  绑定阶段被静默跳过。补齐 9 条命令 + 处理器；目录同时对账 mod 的 127 条能力（新增  
  `CopyToFolder` / `MoveToFolder` 实现，`TabMergeAll` / `BatchRename` / `SelectByPattern` 标注未实现）。
- **书签栏气泡悬停读到已释放内存**：`TTM_ADDTOOLW` 只保存 `lpszText` 指针，原代码传的是  
  循环内的局部 `std::wstring::c_str()`。标签栏/状态栏同样问题一并改为持有副本。
- **新建文件夹第二次必失败**：`IFileOperation::NewItem` 少了 `FOF_RENAMEONCOLLISION`  
  （explorer 默认自动改名 "新建文件夹 (2)"）。
- **状态栏 800ms 全量枚举**：文件夹/文件分类按 `(path,total)` 缓存（`SplitCache`），  
  大目录不再每 800ms 做一遍 O(n) 的 COM 遍历。
- **地址栏/书签栏/标签栏图标不识别特殊命名空间**：三处此前各自裸调 `SHGetFileInfoW`  
  （且对 `::{GUID}` 还加了 `SHGFI_USEFILEATTRIBUTES`），此电脑、回收站、网络、云存储/NSE  
  扩展一律退成通用文件夹图标。新增 `src/icon_resolve.{h,cpp}`（移植 exbar.cpp 的  
  `GetPathIcon` 全链路）：友好名→解析串映射、虚拟命名空间转 PIDL 取真实图标、真实路径先  
  PIDL 解析（云盘等 NSE）、注册表 `CLSID\DefaultIcon` 兜底、stock 文件夹保底。四处调用点  
  统一为 `ico::Resolve`：`nav_bar.cpp`(lead 图标)、`bookmark_bar.cpp`(IconFor)、  
  `nav_bar_actions.cpp`(IconFor→弹层)、`app.cpp`(标签栏)。
- **鼠标滚轮规则从未生效**：`WM_MOUSEWHEEL` 没接到 `InputRouter`，且标签栏传的是客户区坐标  
  （契约是屏幕坐标）、水平滚轮方向反了。三处一起修。
- **复制路径在剪贴板失败时漏 HGLOBAL**：`CopyText` / 书签菜单改为失败即 `GlobalFree`。
- **链接失败**：`src/icon_resolve.cpp` 未列入 `build_fast.py` 的 `SOURCES`。

### 新增

- **输入端脚手架补齐 Shell 文件视图**：`src/shell_input.{h,cpp}` 把参考 `Exbar.cpp` 的目标  
  模型落到独立进程：目标枚举扩出 `unknown/group/column/tree/search/toolbar/preview`，  
  鼠标规则新增存在性早退（无对应按键规则不做 UIA 跨进程调用）、50ms 重复 DOWN 去重、  
  系统双击矩形 + 防三击、左键第一击放行。UIA 单次失败即时重试，连续 3 次探测失败才  
  500ms 冷却，避免第一次点击就丢规则。键盘组合键、序列键、快捷键管理器、命令总线仍保持单一入口。
- **纳入版本控制**：ExBarBrowser 全部源码、脚本、文档与构建配置纳入 `D:\Windhawk` 主仓 git  
  （根 `.gitignore` §8 放行、本目录 `.gitignore` 逐项排除产物），首次提交 95 个文本文件。  
  `*.exe` / `*.obj` / `*.d` / `*.gch` / `*.log` / `shot*.png` / `out*.png` 与 `*.bak` 类  
  编辑备份一律不入库；提交沿用主仓约定式提交，见 [README](README.md) §3.4 与  
  [CONTRIBUTING](CONTRIBUTING.md) §2.3。

  *（同日晚工作区迁到 `E:\ExBarBrowser` 后已改为独立仓库，见本版首节；此条保留为当时的事实记录。）*
- **复制到 / 移动到文件夹**（`CopyToFolder` / `MoveToFolder`）：`IFileOpenDialog(FOS_PICKFOLDERS)`
  - `IFileOperation::CopyItems/MoveItems`，撤销与冲突交给系统。
- **命令目录审计**：`tools/check_commands.py`（静态：表 / keymap / 处理器三方一致）与  
  `tests/commands_test.cpp`（运行期：keymap 引用的命令必须存在于目录）。
- **应用图标**：自绘「蓝色圆角底板 + 白色文件夹 + 双标签页」图标（品牌蓝 `#4C9FFF`），  
  经 `windres` 编进 exe 的 `.rsrc`（`RT_GROUP_ICON` + 8 档 `RT_ICON`：16–256px）。  
  标题栏/任务栏/Alt-Tab/资源管理器统一生效；`tools/gen_icon.py` 可再生成，`resources/app.rc`  
  声明资源，`src/app.cpp` 的 `LoadAppIcon()` 从资源加载并 `WM_SETICON` 设置大小两档。  
  `build_fast.py` 在 PCH 后增量编译资源对象（`app.res.obj`）并参与链接。

### 变更

- **默认键位表改成"数据 → 代码"生成**（消除第二副本）：`src/keymap.cpp` 里 70 条  
  `kDefaultHotkeys[]` + 38 条 `kDefaultSeqs[]` 是从 `data/keymap.json` 手抄的兜底表，  
  改默认键要同时改两处、漏一处就出现"配置文件读到新键、兜底还是旧键"的静默不一致。  
  现在由新增的 `tools/gen_keymap.py` 生成 `src/keymap.gen.h`，`BuildDefault()` 只遍历生成表。  
  `tools/check_commands.py` 增加一项对账：生成头与 json 不一致即 FAIL（已验证负向用例）。  
  `data/keymap.json` 与 `../data/defaults/keymap.json` 内容同步（70 热键 + 38 序列）；  
  `src/keymap.h` 的注释此前称两文件是"同一份资产"，实际是两份独立副本，已改正。
- **构建清单改自动扫描**：`build_fast.py` 的 `SOURCES` 从 45 行手写列表改为 `src/*.cpp`  
  自动扫描 + `EXCLUDE_PATTERNS` 排除表。多人/多会话并行时"新增 .cpp 就要在同一段字符串里  
  插一行"是必冲突改动，且冲突解决不当时会静默漏文件（直到链接期报未定义符号）。  
  已验证：并发会话新增 `src/shell_input.cpp` 后被自动收录，构建脚本零改动。
- **构建产物可隔离**：新增 `--objdir <dir>`，obj/pch/资源对象与该会话的暂存 exe 一起进  
  独立目录（暂存名 `ExBarBrowser.<objdir>.exe`）。此前两个会话共用 `obj/` 和  
  `ExBarBrowser.new.exe`：一方重建 PCH、另一方正拿旧 PCH 编译；或一方刚链接完就被另一方覆盖。
- **地址栏分隔符间距收紧**：每级 chevron 区宽 `kSep96` 32→20px、绘制内边距 `S(8)`→`S(3)`，  
  前导/面包屑图标绘制槽 `kIcon96` 16→14px（对齐 `Exbar.cpp`），`›` 与前后面包屑文字的留白  
  由约 16px 降到约 6px；点击热区仍保留 20px，箭头不缩小。
- **弹窗列表深色滚动条**：自绘弹层里的原生 `LISTBOX` 以前没套主题，深色背景上是一条亮灰宽条；  
  现在深色用 `DarkMode_Explorer`、浅色用 `Explorer`，并给列表窗口 `AllowDarkForWindow`，  
  滚动条颜色与 hover 展开宽度与 ExBar mod 一致。
- **标签栏高度 32 → 26**（逻辑像素，`theme_.S()` 后随 DPI 缩放）：新增常量  
  `kTabBarHeightDp`（`tab_strip.h`），`App::Layout` 与 `TabStrip::Create` 共用一处数字，  
  不再各写一个。四栏高度现状：标签 26 / 导航 30 / 书签 26 / 状态 24。
- **`tab_strip` 拆成五片**：`tab_strip.cpp`(建模/图标/生命周期) + `_input`(消息/悬停/命中) +  
  `_layout`(宽度/溢出/tooltip) + `_paint`(绘制) + `_actions`(右键与 » 菜单)。纯移动。
- **`app_commands` 拆成四片**：核心(调度 + `WithView` + 菜单) / `_tabs`(标签·跳转·导航·路径) /  
  `_files`(文件操作·视图) / `_misc`(窗口系统·外部·书签·会话)。`withView` 从函数内 lambda  
  提升为 `App::WithView`（两个分片共用），`tools/check_commands.py` 改为审计  
  `app_commands*.cpp` 全片，处理器计数仍为 94。
- **`app.cpp` 按职责拆成六片**（`ARCHITECTURE-RULES` R3：单文件 ≤300 行）：  
  `app.cpp`(装配/生命周期 286 行) + `app_wnd`(消息路由) + `app_tabs`(标签生命周期) +  
  `app_nav`(导航与历史栈) + `app_layout`(布局与主题) + `app_stats`(状态栏取数)。  
  纯移动，无逻辑改动；`HomePath()` 改为 `App` 的静态成员，供各分片共用。
- **标签图标所有权下沉到 `TabStrip`**：`SetTabs` 现在直接吃 `core::TabRef` 列表，标题兜底、  
  `ico::Resolve` 与 HICON 缓存（上限 256）都在组件内完成，`App` 不再持有 `tabIcons_`。  
  缓存满时只回收"本次不展示"的 path —— 原实现在 `App` 里是整表 `clear()`，会把已经在  
  `items_` 里、正要绘制的句柄一起销毁。

---
