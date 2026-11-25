# G3_shell 组代码审查（穷举，无抽样）

## 0. 快照与范围说明（必读）

审查期间仓库被并行重构，本报告基于 **2026-11-23 21:2x 的实际磁盘内容**：

- `src/shell_view.cpp` 从 **1176 行（已超基线 995）** 拆成 467 行 + 4 个新文件；
- `src/shell_view_theme.cpp` 从 233 行涨到 407 行（主题/OnViewCreated/RestoreThemeState 并入）。

任务清单 `review/G3_shell.txt` 的 15 个文件（4602 行）已变为 15 个原文件 + 4 个拆分新文件。
两种状态我都完整读过（旧 1176 行版全读，拆分后 5 个文件全读），报告**一律以当前快照的行号为准**，
4 个新文件也逐函数审过并计入覆盖清单。下面"严重/高/中/低"的每一条都基于当前代码复核过行号。

---

## 1. 文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|------|------|------|
| 1 | src/shell_columns.cpp | 170 | 可用；`Toggle` 死代码、pid 解析溢出被接受 |
| 2 | src/shell_columns.h | 47 | 契约清晰；`Toggle` 声明无调用点 |
| 3 | src/shell_input.cpp | 522 | 核心风险集中在 `Install` 无失败判据 + 上下文表生命周期 |
| 4 | src/shell_input.h | 25 | 与实现一致 |
| 5 | src/shell_op_exec.cpp | 42 | **合格**：唯一执行器，PerformOperations + GetAnyOperationsAborted 双判据 |
| 6 | src/shell_op_exec.h | 40 | 注释与实现一致（难得） |
| 7 | src/shell_ops.cpp | 1365 | 问题最多：删除/新建的排队失败被记成成功、SetOperationFlags 全未判 |
| 8 | src/shell_ops.h | 231 | 契约写得比实现细；`Undo`、`RenameTo` 的 why 约定未全部落地 |
| 9 | src/shell_ops_path.cpp | 97 | 判败齐全（走 Run），setter 未判（同全项目） |
| 10 | src/shell_ops_path.h | 31 | 作用域划分说明清晰 |
| 11 | src/shell_view.cpp | 467 | 拆分后职责单一；残留：定时器集中派发、陈旧排队导航 |
| 12 | src/shell_view.h | 229 | 有 2 个零调用入口（`ItemCount`、`SetNavPaneVisible`） |
| 13 | src/shell_view_preview.cpp | 270 | 诊断段已隔离（好）；`ApplyPreviewPaneState` 名不副实 |
| 14 | src/shell_view_search.cpp | 124 | 判据完整；PIDL 克隆失败静默丢导航 |
| 15 | src/shell_view_theme.cpp | 407 | 重建失败无出路（容器已毁、返回值被忽略）；主题套用违反静态检查 |
| 16 | src/shell_view_helpers.cpp | 31 | **新增**（拆分产物）；与 shell_input 的 `ClassIs` 重复实现且语义不同 |
| 17 | src/shell_view_helpers.h | 7 | 新增 |
| 18 | src/shell_view_navpane.cpp | 145 | 新增；`ApplyNavPaneWanted` 的 1px jitter 有实测依据，保留合理 |
| 19 | src/shell_view_selection.cpp | 206 | 新增；`CaptureSelectionPidls` 隐式清成员（所有权隐患） |
| 20 | src/shell_view_sink.cpp | 177 | 新增；`ReplayQueued` 与 view.cpp 的 `queued_` 跨 TU 共享状态 |

静态检查实测（`python tools/check_components.py`）：本组文件当前**未**触发"单文件 ≤500 行"
（shell_ops 1365 / shell_input 522 都在记账基线内），主题套用规则会命中 `shell_view_theme.cpp`。

---

## 2. 严重（正确性/数据安全，必须修）

### S1　删除的"排队失败"被记成成功
`src/shell_ops.cpp:313-318`

```cpp
for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> item;
    if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
    op->DeleteItem(item.get(), nullptr);        // ← HRESULT 丢弃
}
return shellopexec::Run(op.get());
```

`DeleteItem` 在**排队阶段**就可能失败（权限、只读、占用、路径失效）。全部失败时队列为空，
`PerformOperations` 返回 S_OK、`GetAnyOperationsAborted` 为 FALSE，`Run` 于是返回 **true**
—— 调用方与 AI 拿到"删除成功"，文件还在。`GetItemAt` 全失败（n>0 但一个也取不到）走同一条路。

修复：累计排队结果。
```cpp
HRESULT firstFail = S_OK; int queued = 0;
for (...) {
    const HRESULT h = op->DeleteItem(item.get(), nullptr);
    if (SUCCEEDED(h)) ++queued; else if (SUCCEEDED(firstFail)) firstFail = h;
}
if (!queued) return false;                       // 一条都没排上 = 整体失败
return shellopexec::Run(op.get());               // 部分失败仍交给 aborted 判据 + 记 queued/n
```

### S2　"新建文件夹"的 NewItem 结果未判（与 S1 同型）
`src/shell_ops.cpp:416`（对照 `:468` 的 NewTextFile **有**判）

```cpp
op->NewItem(folder.get(), FILE_ATTRIBUTE_DIRECTORY, folderName.c_str(), nullptr, nullptr);
return shellopexec::Run(op.get());
```
只有目录只读/名字非法时 `NewItem` 直接失败、队列为空 → 返回 true。修复：`if (FAILED(hr)) return false;`（并把 HRESULT 带进返回值或日志），与 NewTextFile 对齐。

### S3　`SetOperationFlags` 全项目未判 —— 有一条静默永久删除路径
`src/shell_ops.cpp:311,312,411,414,441,442,463,464,1232,1233`；
`src/shell_ops_path.cpp:33,34,61,63,91,92`

`DeleteSelectionImpl` 靠 `FOF_ALLOWUNDO` 决定"进回收站"（`:308-310`）。若 `SetOperationFlags`
失败而无人发现，删除会**绕过回收站且不提示**——这是全组唯一会静默丢数据的路径，而它现在就
无条件被忽略。修复（最小改动）：`if (FAILED(op->SetOperationFlags(flags))) return false;`
在删除两处（`shell_ops.cpp:311`、`shell_ops_path.cpp:91`）必须做；其余点可只 `LogDbg`。

---

## 3. 高（行为错误/静默失效）

### H1　陈旧的排队导航会把视图拉回去
`src/shell_view.cpp:344-356`（写 `queued_`、挂定时器）＋ `src/shell_view_sink.cpp:110-129`（消费）

`A` 因 `ERROR_BUSY` 排队后定时器已挂上；若定时器触发前 `B` 导航**成功**，`DoNavigate` 的成功
分支（`:357-358`）既不清 `queued_` 也不 `KillTimer`。`ReplayQueued` 只在 `p == path_` 时跳过，
于是把视图导航到 `A`。现象："点 B 打开了，过一会儿自己跳回 A"，且 `replayTries_` 已被
`OnNavigated` 清零，重放延迟也重置。`NavigatePid`（`shell_view_search.cpp:41-48`）同款。

修复：成功分支显式作废排队意图——
```cpp
// BrowseToObject 成功后
queued_.clear(); queuedPidlFree(); replayTries_ = 0;
if (container_) KillTimer(container_, kNavReplayTimer);
```
更强的做法是给每次导航一个单调序号，`ReplayQueued` 校验"我还是最后一次请求"。

### H2　主题重建失败后没有任何出路（窗口永久空白）
`src/shell_view_theme.cpp:176-187` + `:162-173` + `:288-310`

`doRecreate` 路径先 `DestroyWindow(container_)`，`CreateWindowExW` 失败时 `container_` 已是
`nullptr`，`fail()` 只清状态就 `return false`。此后 `Resize`（`shell_view.cpp:259`）、
`SetVisible`、`ApplyNavPaneWanted`、`Destroy` 全部早退；而 `ApplyTheme`（`:308`）**忽略返回值**，
界面就是一块永不重绘的空白，日志之外无人知晓。

修复：① `fail()` 里若 `container_ == nullptr` 允许再尝试建一次（或退化为"不重建容器"路径）；
② `ApplyTheme` 检查返回值并调 `failFn_()`（已有回调通道，`shell_view_sink.cpp:86-89`），
让 App 有机会提示/回滚；③ 至少在 `container_ == nullptr` 时把 owner 窗口挂到 App 的占位。

### H3　`shellinput::Install` 没有任何失败判据
`src/shell_input.cpp:469-493`

`Subclass()` 的三处返回值（`:482/484/487`）全部丢弃；`ctx->listView/defView/tree` 都为
`nullptr` 时仍 `g_contexts[container] = std::move(ctx)` 并 `return true`。后果：某个 Win11
构建改了层级，鼠标规则（双击空白上一级 / 单击项 / 滚轮）**全部静默退化**，而 `OnViewCreated`
（`shell_view_theme.cpp:327`）认为已安装。

修复：
```cpp
int hooked = 0;
if (ctx->listView) hooked += Subclass(ctx->listView, ctx.get()) ? 1 : 0;
else if (ctx->defView) hooked += Subclass(ctx->defView, ctx.get()) ? 1 : 0;
if (ctx->tree && ...) hooked += Subclass(ctx->tree, ctx.get()) ? 1 : 0;
if (!hooked) { LogDbg(L"[shell-input] install 失败 list=%p def=%p tree=%p", ...); return false; }
```

---

## 4. 中（健壮性/维护性，应在下一轮清掉）

### M1　`NewTextFile` 回退循环里重算本地化名（与自己的注释矛盾）
`src/shell_ops.cpp:503-507`（注释在 `:465-466` 写着"名字只算一次：每次要读 2-3 次注册表"）
```cpp
for (int i = 0; i < 1000; ++i) {
    std::wstring name = dir + L"\\" + LocalizedNewTextName();   // ← 每轮 2-3 次注册表读
```
修复：循环外 `const std::wstring base = LocalizedNewTextName();`。

### M2　`Undo` 的注释与代码直接冲突
`src/shell_ops.cpp:1127-1143`：注释"【不要传宿主窗口，也不要 SW_SHOWNORMAL】"，代码
`ici.nShow = SW_SHOWNORMAL;`。两者必有一错。撤销是无 UI 动作，建议按注释把 `nShow` 保守化
（`SW_SHOWNORMAL` 保留也可，但注释必须删掉那句"不要传"），否则下一个人会照注释改代码并困惑。

### M3　列 id 的 pid 溢出被静默接受
`src/shell_columns.cpp:47-49`
```cpp
const unsigned long pid = wcstoul(pidText.c_str(), &end, 10);
if (!end || *end != L'\0' || pid > 0xFFFFFFFFUL) return false;   // 后半句恒假
```
Win32 的 `unsigned long` 是 32 位，`pid > 0xFFFFFFFFUL` 永远不成立；而 `wcstoul("99999999999999")`
返回 `ULONG_MAX` 且 `end` 指向 `'\0'`，于是非法输入被当成合法 `0xFFFFFFFF`。修复：
`errno = 0;` 后判 `errno == ERANGE`；并限制 `pidText` 长度（≤10 位、无前导 `+`）。

### M4　`shellcolumns::Toggle` 死代码，且切换一次枚举两遍
`src/shell_columns.cpp:119-135`（`shell_columns.h:36` 声明）——全仓零调用点。
它先 `List()`，再调 `SetVisible()`，而 `SetVisible` 内部又 `List()` 一次（`:83-84`）：大目录下
一次列显隐要枚举两遍列信息。修复：删 `Toggle`，或让 `SetVisible` 接受已枚举的 `cols`。

### M5　两个零调用公开入口
- `ShellView::ItemCount()`：`src/shell_view.cpp:406-421`，全仓零调用（`shell_view_sink.cpp:174`
  的注释提到它，但那是"不要调用"的说明）。
- `ShellView::SetNavPaneVisible()`：`src/shell_view.h:85-89`，零调用；外部也无人直接写
  `navPaneWanted_`（已 grep 确认），所以它是纯装饰入口。
修复：删除，或真正接到 `app_panes.cpp` 的双栏逻辑上（那里的 `PresetNoNavPane` 只覆盖创建时）。

### M6　`CaptureSelectionPidls` 清的是成员、不是参数（所有权隐患）
`src/shell_view_selection.cpp:131-133`
```cpp
unsigned ShellView::CaptureSelectionPidls(unsigned cap, std::vector<PIDLIST_ABSOLUTE>& out) {
    ClearThemeRestorePidls();      // ← 清的是 themeRestorePidls_，不是 out
```
现在两个调用点（`shell_view_theme.cpp:79`、`:154`）都传 `themeRestorePidls_` 才没出事。
一旦有人传别的 vector：`out` 原有 PIDL 不释放（泄漏），`themeRestorePidls_` 反被清空
（下一次主题重建丢选中）。修复：要么去掉 `out` 参数固定用成员，要么只清 `out` 自己。

### M7　`ApplyPreviewPaneState` 名不副实（只打日志）
`src/shell_view_preview.cpp:58-68`；4 个调用点（`shell_view_theme.cpp:340`、
`shell_view_sink.cpp:160`、`shell_view_preview.cpp:89/260`）都指望它"把状态套上去"，实现只有
一行 mismatch 日志。真正的落地在**另一个文件**的 `GetPaneState`（`shell_view.cpp:152-171`）。
修复：改名 `LogPreviewPaneMismatch`，并在文件头一行写死"落地唯一入口 = GetPaneState"；
否则下一个人会来这里补"纠正逻辑"，与 GetPaneState 形成两个真相。

### M8　UIA 不可用时所有点击退化为 `MT_UNKNOWN`（无兜底、无出口）
`src/shell_input.cpp:196-213`（初始化失败冷却 2s、`:224-227` 连续 3 次失败冷却 500ms）
叠加 `DetectTarget`（`:508-520`）对 `SHELLDLL_DefView` 一律走 UIA 的分支：UIA 服务异常期间，
Win11 上的双击空白/单击项/滚轮全部落到 `MT_UNKNOWN`，只留日志。修复：给 `DetectTarget` 加
一个"UIA 不可用 → 用客户区几何兜底（是否命中项区域）"的降级路径，或把该状态经
`MouseSink` 显式上报（让 InputRouter 能告警），不要与"点在空白处"混成同一个值。

### M9　`g_contexts` 的生命周期只靠 `Uninstall` 一处
`src/shell_input.cpp:41,377-380,495-506`：表以 `container` HWND 为键，只在 `Uninstall` 里擦除。
当前调用链（`shell_view_theme.cpp:26` 的 `DestroyBrowser` → `Uninstall`）是对的，但容器若被
别处 `DestroyWindow`（`Destroy()` 里先 `DestroyBrowser()` 再毁窗口，暂时安全），表里会留下
已销毁句柄的条目；HWND 被复用后 `FindContext` 会命中错对象。修复：`Install` 时校验
`IsWindow`、`Uninstall` 之外再在 `Context` 上挂一个 `WM_NCDESTROY` 清理（子类化已在该消息里，
顺手 `g_contexts.erase(container)` 即可）。

---

## 5. 低（洁癖/一致性/注释）

| 编号 | 位置 | 问题 | 修复 |
|---|---|---|---|
| L1 | `shell_input.cpp:119-120` | 日志里重试间隔写死 `5000`，与上行表达式各写一遍 | 提 `constexpr DWORD kUiaInitRetryMs = 5000;` 两处共用 |
| L2 | `shell_input.cpp:96-122` | `static IUIAutomation*` 全进程不释放，未注明"故意" | 加一行注释，否则后人当泄漏修 |
| L3 | `shell_input.cpp:46-50` vs `shell_view_helpers.cpp:4-9` | 同一件事两份实现，且**语义不同**：`ClassIs` 用 `str::IEquals`（不敏感），`ClassIsName` 用 `wcscmp`（敏感） | helpers 那份改 `str::IEquals`，`shell_input` 复用之；类名比较跨 Win 版本不应区分大小写 |
| L4 | `shell_ops.cpp:144,261,274` | `OleFlushClipboard()` 返回值丢弃：flush 失败时剪贴板可能为空，函数仍返回 true（"已复制"是假话） | flush 失败返回 false 或写 `why` |
| L5 | `shell_ops.cpp:81,300,427,531,812,879,1045`（全部 7 处 `GetCount`）；`:1285` | `sel->GetCount()` / `GetAttributes` 返回值未判；`FillItemInfo` 用 `ft.dwLowDateTime` 判时间有效性，低 32 位为 0 的合法时间被当成无效 | 判 HRESULT；时间有效性改 `ft.dwHighDateTime \|\| ft.dwLowDateTime` |
| L6 | `shell_ops.cpp:957-981` | `SelectWhere` 的 `Add` 分支自己重写了一遍"枚举 + SelectItem"循环，与 `:940-944` 的注释（"枚举/选中各只有一份实现"）不符，且复杂度 O(视图项数 × 命中数) | 给 `SelectByPaths` 加 `bool deselectOthers = true` 参数，Add 分支复用 |
| L7 | `shell_ops.cpp:1176-1199` | `RenameTo` 的"目标必须在当前视图"校验逐项比路径（O(视图项数)），大目录改名一次扫一万项；`SelectByPath`/`StatItem` 同款 | 用 `IShellFolder::CompareIDs` 或 `SHCreateItemFromParsingName` + `GetDisplayName` 对比，避免全表扫 |
| L8 | `shell_ops_path.cpp:89-91` | 注释讲"撤销复制就是删副本"，但 `FOF_WANTNUKEWARNING` 与"副本回回收站"两句与 `FOF_ALLOWUNDO` 的语义关系没写清 | 补一句：ALLOWUNDO 决定进回收站，WANTNUKEWARNING 只管超配额/网络盘 |
| L9 | `shell_view.cpp:237` | 留着一行注释掉的 `SetPropertyBag` "临时诊断" | 删除（同文件族对"注释掉的代码会腐烂"已有明确立场，见 `shell_view_preview.cpp:107`） |
| L10 | `shell_view_search.cpp:41-42` | `ILCloneFull` 失败（null）仍 `return true`，导航被静默丢弃且不回调 `OnNavigationFailed` | 克隆失败返回 false |
| L11 | `shell_view_preview.cpp:160-163` | 诊断段用 `SHCreateItemFromIDList(child pidl)`，正是 `shell_view_selection.cpp:37-40` 点名的错误写法（childPIDL 当绝对 PIDL 用 → 拼出桌面下的错项） | 改 `SHCreateItemWithParent(nullptr, sf2, pidl, ...)` |
| L12 | `shell_view_theme.cpp:192,267,281,292` | 4 处直接 `SetWindowTheme`；静态检查"控件主题套用"白名单只有 `platform_win.cpp` / `shell_view.cpp`，拆分后本文件不在名单（check 曾报 `shell_view_theme.cpp ×1`） | 收进 `win::ThemeControl`，或把规则白名单补成 `shell_view*.cpp` 并在规则里写清"DirectUI 例外" |
| L13 | `shell_view_theme.cpp:288-310`；`:325` | `ApplyTheme` 忽略重建返回值（同 H2）；`EnumChildWindows(..., (LPARAM)&theme_.dark)` 是对 `const bool` 取址强转，读起来像在改写主题 | 前者见 H2；后者改 `const bool dark = theme_.dark; ... &dark` |
| L14 | `shell_view.cpp:39-58` | 4 个定时器（nav replay / preview / search / theme restore）全在 `ViewProc` 集中派发，而 handler 分散在 4 个 TU：新增一个定时器必须回来改这个 dispatcher | 抽一张 `{id → member fn}` 小表，或在各 TU 的 WndProc 里自处理（拆分的初衷是"改预览不碰导航"，这里还留着一处交叉编辑点） |
| L15 | `shell_view_preview.cpp:138` | 诊断里硬编码 `"大小"` / `"Size"` 找列 | 属诊断开关内，可接受；若将来转正必须走 `PKEY_Size` + `IColumnManager`，不要按本地化列名匹配 |

---

## 6. 逐维度结论

**① IFileOperation 使用** —— 执行判败**全部**收敛到 `shellopexec::Run()`（`shell_ops.cpp` 4 处、
`shell_ops_path.cpp` 3 处，无旁路），双判据（HRESULT + `GetAnyOperationsAborted`）做得对，这是全组
最扎实的一块。缺口在**排队失败**：`DeleteItem`（S1）、`NewItem`（S2）的 HRESULT 丢弃导致"全失败
被记成成功"；`SetOperationFlags` 全未判（S3）里藏着一条静默永久删除路径。
`RenameItem/MoveItems/CopyItems/RenameItem(path)/DeleteItem(path)` 的返回值都检查了。

**② PIDL 生命周期** —— 克隆/释放整体配对良好（`ILCloneChild`+`CoTaskMemFree`、`ILCombine`+`ILFree`、
`SHParseDisplayName`+`ILFree` 均成对；`ILFindChild` 的返回值只借用不释放，正确）。
两处隐患：`CaptureSelectionPidls` 清成员而非参数（M6，当前无 bug 但所有权语义含糊）；
`ReplayQueued` 里 `NavigatePid` 失败会静默丢弃排队意图（L10 同源）。未发现双重释放或悬垂。

**③ shell_view 拆分后的职责** —— 拆分方向正确：预览（含诊断隔离）、搜索、导航窗格、选中观测、
Sink/重放、主题生命周期各自成文件，`ClassIsName/FindChildByClass` 也抽到了 helpers。
残留耦合 3 处：定时器仍集中在旧 `ViewProc`（L14）；`queued_` 在 `shell_view.cpp` 写、`shell_view_sink.cpp`
读、`shell_view.cpp` 挂定时器；预览意图的"写"（preview.cpp / GetPaneState）与"落地"（GetPaneState）
分居两处，而中间那个 `ApplyPreviewPaneState` 只打日志（M7）。主题重建状态又是 theme.cpp 写、
selection.cpp 清（M6）。

**④ COM/系统调用返回值** —— 主体路径判得细（`GetCurrentView`、`GetFolder`、`GetItemAt`、`SHCreate*`
几乎都判了，`Virtual 目录 GetFolder(IShellFolder2)` 的空指针坑也已修）。薄弱点是"批量里的单个调用"：
`GetCount`、`GetAttributes`、`SetOperationFlags/SetOwnerWindow`、`OleFlushClipboard`、诊断段里的
`GetDetailsEx`/`GetCurrentName`（L4/L5）。未初始化读取未发现（结构体普遍 `{}`）。

**⑤ 工程约束**
- **R21**：本组**无违规** —— 不产生任何合成输入（无 `SendInput`/`mouse_event`/`keybd_event`/
  `SetCursorPos`/`AttachThreadInput`），`shell_input` 只**消费**真实鼠标消息并可选吞掉，属产品行为；
  没有 `SetForegroundWindow`/抢焦点调用。静态检查对 `src/` 的判定与人工核对一致。
- **品牌串**：无 `L"ExBar…"` 硬编码（仅 `shell_view.cpp:237` 的注释、`shell_input.h:3` 的说明文字）。
- **硬编码色**：无 `RGB(`、无 `0x00RRGGBB` 字面量；背景走 `theme_.brBg`。
- **命名空间**：`shellopexec` / `shellops` / `shellcolumns` / `shellinput` 划分清晰，无 `namespace eb`。
  例外：`shell_view_helpers.cpp` 的两个自由函数在全局命名空间，且与 `shellinput::ClassIs` 重复（L3）。
- **硬编码中文本地化串**：`NewFolder`/`NewTextFile` 的中文只是 `SHLoadIndirectString` 的**失败回退**，
  有注释说明，可接受；`Undo` 的 `撤销/復原/取り消し` 词表是判定用途，硬编码但有注释。
- **文件行数**：`shell_view.cpp` 从 1176（超基线 995）降到 467 —— 棘轮债已还；`shell_ops.cpp` 1365、
  `shell_input.cpp` 522 与基线持平（不许再涨）。4 个新文件都在 500 以下。

**⑥ 死代码/注释不符** —— 死代码 3 处：`shellcolumns::Toggle`（M4）、`ShellView::ItemCount`、
`ShellView::SetNavPaneVisible`（M5）。注释不符 3 处：`Undo` 的 `SW_SHOWNORMAL`（M2）、
`NewTextFile` 的"名字只算一次"（M1）、`ApplyPreviewPaneState` 的名字/实现（M7）；
另有 `shell_view.cpp:237` 的注释代码（L9）。

---

## 7. 总体评价

**基本面是好的，而且是"有纪律的好"**：执行判败收敂成唯一实现并给了失败原因、每个"踩过的坑"
都在注释里留了实测数据（DirectUI 主题、EPS_FORCE、`PathMatchSpecExW` 的 S_FALSE、childPIDL 当绝对
PIDL）、诊断代码默认编译关闭而不是注释掉。这套"坑-证据-修法"的注释密度在本仓里是标杆级，
审查时省了大量猜测成本。

**主要问题集中在"批量操作的部分失败/排队失败"这一层**：`Run()` 的双判据只覆盖"执行阶段"，
而 `DeleteItem`/`NewItem` 的**排队阶段**失败没人看 —— 于是出现 S1/S2 两条"假成功"，以及 S3 那条
"SetOperationFlags 失败 = 静默永久删除"。这三条必须先修，它们直接决定 AI 通道报的 ok 是不是真的。
其次是 H1（陈旧排队导航把用户拉回旧目录）与 H2/H3（重建或安装失败后无出口、无判据）：
都不是崩溃，但都是"用户看到的行为和应用自己的账不一致"的静默失效，而这正是本工程注释里反复强调
要消灭的那类问题。

**拆分后的结构值得肯定**（1176 → 5 个 200 行上下的文件，职责可一句话说清），但拆分只做了一半：
定时器派发、导航排队、预览意图的"观测 vs 落地"仍跨 TU 交叉，建议按 L14/M6/M7 再收一轮。
死代码（M4/M5）和注释不符（M1/M2/M7）数量不多但都很"打脸"—— 它们恰好落在文件里
写着"不要这样写"的位置旁边，建议与本轮其他修复一起清掉。

修复优先级建议：**S1 → S2 → S3 → H1 → H3 → H2 → M1/M2/M4/M5 → 其余**。
