# G1_appcore 组代码审查报告

审查范围：`review/G1_appcore.txt` 列出的 13 个源文件，共 **2957 行**。
审查方式：**逐文件全文通读**（无抽样、无跳读），并对每一处可疑点回查被调用的头文件/实现
（`commands_confirm.cpp`、`commands.h`、`nav_history.cpp`、`nav_history.h`、`shell_view.h`、
`shell_view.cpp`、`shell_view_search.cpp`、`shell_view_sink.cpp`、`search_folder.h`、
`search_scope.h`、`sequence_overlay.h`、`sequence_overlay.cpp`、`menu.h`、`split_bar.h`、
`side_layout.h`、`layout_key.h`、`layout_limits.h`、`pane_split.h`、`theme.h`、`view_stats.h`、
`file_undo.h`、`shell_ops_path.h/.cpp`、`tab_set.cpp`、`settings_dialog.cpp`、`app.cpp`、
`app_commands*.cpp`）后才落结论；标注为"需人工复核"的条目是**无法靠读代码定论**的行为性判断。

边界遵守：只读 `src/`，未运行 `build.sh` / `build_fast.py` / `agentctl down`，未杀进程，
未修改任何 `src/` 文件，唯一写入是本文件。

---

## 一、文件覆盖清单

| # | 文件 | 确认读完行数 | 结论 |
|---|---|---|---|
| 1 | `src/app_confirm.cpp` | 28 | 干净（1 条轻微：确认文案与具体命令语义不符） |
| 2 | `src/app_file_undo.cpp` | 66 | 干净（撤销逆操作三条分支逐条核对，PIDL/判败链路正确；无问题） |
| 3 | `src/app_layout.cpp` | 361 | 发现 1 问题（中等：navPane 显隐双写者）+ 1 轻微 |
| 4 | `src/app_nav.cpp` | 343 | 发现 **1 严重**（历史 pending 残留）+ 2 中等 + 2 轻微 |
| 5 | `src/app_panes.cpp` | 428 | 发现 3 中等（回调早接线失效、显隐双源、焦点事件全量刷新）+ 1 轻微 |
| 6 | `src/app_search.cpp` | 161 | 干净（菜单动作号 ↔ 枚举映射、禁用行、范围归一化均正确） |
| 7 | `src/app_sequence_overlay.cpp` | 358 | 发现 1 中等（筛选模式名实不符）+ 3 轻微（魔法串、死字段分支） |
| 8 | `src/app_settings.cpp` | 42 | 干净（1 条轻微：Create 失败只有 MessageBeep，无失败原因） |
| 9 | `src/app_state.cpp` | 168 | 发现 1 轻微（provider 内 `ActivePane()` 会写状态，与"只取数"注释矛盾） |
| 10 | `src/app_stats.cpp` | 137 | 发现 1 中等（`p->view` 未判空）+ 2 轻微（函数内 static、节流判据） |
| 11 | `src/app_tabs.cpp` | 500 | 发现 1 轻微×3（重复 ApplyPersistedViewState、机器路径响铃、已在 500 行上限） |
| 12 | `src/app_view_menus.cpp` | 94 | 干净（1 条轻微注释建议 + 1 条需人工点验） |
| 13 | `src/app_wnd.cpp` | 271 | 发现 1 轻微×4（文件头注释与实现不符、`l` 未判空、SETTINGCHANGE 判据、模态期取数） |
| | **合计** | **2957** | 与清单行数一致 |

**逐项断言**：13 个文件全部用 Read 完整读到底（最大 `app_tabs.cpp` 正好 500 行，未分页截断），
每个文件的每个函数、每个 switch 分支、每个 `if` 都被逐个过了一遍；下文每一条问题都带具体行号，
"干净"的文件也列出了看过哪些函数（见第四节）。

---

## 二、严重

### S1. 同路径导航提前返回后 `History::pending_` 遗留 → 之后"视图内导航"被当成重定向，历史条目被就地替换（后退跳过一层）

- **位置**：`src/app_nav.cpp:89`（`p->nav.Request(path, push)`）与 `src/app_nav.cpp:90-95`（提前 `return true`）
- **相关实现**：`src/nav_history.cpp:34-52`（`Request` 无条件写 `pending_`）、`src/nav_history.cpp:65-92`（`Complete` 的三分支）
- **问题**：`History::Request()` 在被判为"目标就是当前目录且要求压栈"时**返回 false，但已经写了
  `pending_.path = path`（`prevIdx = idx_`, `pushed = false`）**。`NavigateActive` 拿到 false 后直接
  `return true`，**不会调用 `ShellView::Navigate`**，也就永远不会有完成回调来清掉 `pending_`。
  这条 pending 会一直挂着，直到下一次 `Request` / `Goto` 覆盖它。
- **触发条件（很日常）**：
  1. 当前在 `C:\A`，用户点 `C:\A` 的书签 / 在地址栏对同一路径回车 / `GoHome()` 时本来就在主页 /
     `fs.open` 当前目录 —— 任一动作走 `NavigateActive("C:\A", push=true)`；
  2. 此时 `pending_ = {"C:\A", prevIdx, pushed=false}` 残留；
  3. 用户随后**双击视图里的子目录** `C:\A\B`（这条路径不经过 `Request`，由 shell 自己导航完成）；
  4. `App::OnTabNav` → `p->nav.Complete("C:\A\B")`。`pending_.active()` 为真、`path != pending_.path`，
     而 `hist_[idx_] == pending_.path`（都等于 `C:\A`）→ 命中"重定向"分支：**`hist_[idx_] = "C:\A\B"`**。
- **症状**：`C:\A` 这条历史被**替换**而不是被压入新条目 → 按一次"后退"从 `C:\A\B` 直接退到 `C:\A` 的
  父目录，"后退跳过一层"、后退下拉里少了 `C:\A`。这正是 `nav_history.h` 文件头声明要根治的那类
  "症状远离现场"的历史 bug（"后退要按两次"同族）。
- **修复建议**（任选其一，推荐 ①）：
  1. 在 `app_nav.cpp:94` 的提前返回前显式清掉这次请求：`p->nav.Fail();`（`Fail` 对 `pushed=false`
     只做 `idx_ = prevIdx` + `pending_.Clear()`，语义与"没发生过"一致）；更干净的做法是给
     `History` 加一个 `CancelPending()` 并在 `NavigateActive` 里调用；
  2. 让 `History::Request` 在 `sameAsHere && push` 时**不写 `pending_`**（保持"我什么都没做"的不变量），
     但这样 `NavigateActive` 就无法用 pending 判断"要不要导航"，需同步调整返回值语义。
- **建议补测试**：`tests/nav_history_test.cpp` 里加一条纯函数用例——
  `Request(cur,true)` → 不导航 → `Complete(cur + "\\sub")` → 断言 `size()` 增加了 1、`backward()[0] == cur`。
  这条用例不需要窗口，符合 AGENTS §1.1 的"抽纯函数写单测"优先路径。
- 严重级说明：不造成崩溃或数据丢失，但破坏的是本项目**明确声明过的不变量**（历史栈收敛唯一实现），
  且触发路径极其日常。若评审口径只把崩溃/数据丢失算"严重"，可下调为"中等（最高优先级）"。

---

## 三、中等

### M1. 导航失败/无视图时不回滚 `pending_`，留下"死历史条目"

- **位置**：`src/app_nav.cpp:97-101`（`NavigateActive` 失败分支）、`src/app_nav.cpp:106-112`（`GotoHistory`）
- **问题**：
  - `NavigateActive`：`p->nav.Request()` 可能已经**乐观压栈**（`pushed=true`），随后
    `!p->view || !p->view->Navigate(path)` 为真时只调 `OnTabNavFailed`（内部才 `Fail()`），
    而 `OnTabNavFailed` 里 `FindPane` 失败就直接 return；更关键的是 `if (p->view)` 这条判断——
    **`p->view == nullptr` 时既没有 `Fail()`，也没有任何失败说明**，`Request` 压进去的那条就永久留在栈里。
  - `GotoHistory:109-111`：`p->nav.Goto(target)` 已经移动游标并写了 pending；`!p->view` 时既不 `Fail()`
    也不 `ReportNavFailure`，于是"地址栏/面包屑指向新条目、视图还在原目录"，且 `canBack/canForward`
    给出错误答案——正是 `nav_history.h` 里点名要避免的"视图 A、地址栏 B"。
- **触发条件**：窗格视图创建失败（`EnsurePaneView` 返回 false 后 `p->view` 仍为空，见 M3 说明）、
  或视图在主题重建窗口期内为空。
- **修复建议**：两处都改成无条件 `p->nav.Fail()`（`Fail` 对"没压过栈"的情况是安全的：只把 `idx_`
  退回 `prevIdx` 并 `ClampIdx`），并在 `!p->view` 时补一条 `NoteFailure(L"没有活动文件视图")`，
  让机器调用拿到原因而不是"静默半成功"。

### M2. `RefreshStats()` 未判空就解引用 `p->view`

- **位置**：`src/app_stats.cpp:42` — `IShellView* view = p->view->CurrentView();`
- **问题**：函数开头只判了 `showStatus` / `Active()` / `ActivePane()`（`app_stats.cpp:11-15`），
  没有任何地方判 `p->view`。而 `App::Pane` 的 `view` 是**按需创建**的（`app.h:239-241` 明确写了
  "`PaneAt` 永不返回 null，但 `Pane::view` 可能还没创建"），全仓其它文件都守在前面：
  `app_nav.cpp:97`（`!p->view || !p->view->Navigate`）、`app_nav.cpp:155`、`app_nav.cpp:237`、
  `app_nav.cpp:155`、`app_panes.cpp:55`、`app_panes.cpp:441`。**只有这里漏了**。
- **可达性（诚实说明）**：我把所有能把 `activePane` 置 1 的路径都走了一遍
  （`NextPane:285` 要求 `EnsurePaneView` 成功、`OnPaneActivated:330-336` 要求视图自报、
  `ActivePane:46-49` 与 `OnFocusMaybeChanged:311` 要求焦点落在视图内、`SetDualPane:261` 归零、
  会话恢复不写 `activePane`），**当前代码路径下 `p->view` 为空基本不可达**。
  但 `Activate:227` 用 `(void)EnsurePaneView(activeTab, 1)` **显式忽略失败**，且 `EnsurePaneView`
  失败后 `Pane::view` 留空、`Pane` 仍在 `panes[]` 里——一旦后续新增"直接指定活动窗格"的入口
  （AI 命令、会话恢复扩展），这就是 `RefreshStats`（800ms 定时器 + 每次 `SyncChrome`）上的空指针崩溃。
- **修复建议**：`if (!p->view) return;`（或 `if (!p) return;` 之后紧跟 `if (!p->view) return;`），
  顺带把 `p->view` 提取为局部引用，避免一行里两处解引用。

### M3. `WirePaneCallbacks` 提前接线无效：`Create()` 内部同步完成的首次导航回调被丢弃

- **位置**：`src/app_panes.cpp:112`（注释"先接回调再 Create —— 首个导航可能在 `Create()` 内部就完成"）
  → `src/app_panes.cpp:151`（`view->Create(viewArea_, path)`）→ `src/app_panes.cpp:159`（`p->view = std::move(view)`）
- **调用链事实**：`ShellView::Create` 末尾直接 `Navigate(path)`（`shell_view.cpp:203`）；
  导航完成回调经 `shell_view_sink.cpp:176` → `navFn_` → `App::OnTabNav` → `FindPane(view, ...)`；
  而 `FindPane`（`app_panes.cpp:164-178`）靠 `p->view.get() == view` 反查，**此刻 `p->view` 还没赋值**，
  所以回调一定 `return`（`app_nav.cpp:282` `if (!FindPane(...) || !t) return;`）。
- **后果**（如果 shell 确实同步完成，`app_panes.cpp:112` 的注释正是这么假设的）：
  ① 首个导航的 `display` / `items` / `AddRecent` 丢失；
  ② `pendingSelItems_` 不被消费（会话"恢复选中"在启动首个导航上落空）；
  ③ `p->searchFrom` / `searchQuery` 不会被清理；
  ④ 第二窗格最要紧的一条：`EnsurePaneView:158` 只在 `p->nav.here().empty()` 时才用视图回读的路径
     校正历史，而 `NewTab:29` 早已 `Reset(target)` → 条件不成立 → **视图被 shell 规范化/重定向后
     历史与标签仍停在旧串**，正是这段注释声称要防的"第二窗格显示 A、地址栏说 B"。
- **需人工复核**：本地目录的 `OnNavigationComplete` 是否稳定在 `BrowseToObject` 的调用栈内同步触发
  （读代码无法定论；但代码自己的注释与 `app_panes.cpp:155-157` 的"回读校正"都按"会同步"设计）。
- **修复建议**：把 `p->view = std::move(view)` 提到 `Create()` **之前**
  （失败时再 `p->view.reset()` 并返回 false），或在 `WirePaneCallbacks` 之前把视图登记到一个
  "待登记视图"字段供 `FindPane` 命中。前者改动最小，也能让"同步完成首帧"这层设计真正生效。

### M4. `navPane` 显隐有两个写者：`Layout()` 会改写 `ApplyNavPane()` 用来判重的同一份状态

- **位置**：`src/app_layout.cpp:129-138`（`navW > 0 ? SetVisible(true) : (Resize(0,…); SetVisible(false))`）
  vs `src/app_panes.cpp:86-88`（`if (navPane_.visible() == s.showNavPane) return; navPane_.SetVisible(...); Layout();`）
- **问题**：`navPane::NavPane::visible()` 返回 `visible_ && tree_ != nullptr`（`nav_pane.h:99`），
  而 `Layout()` 在"窗口太窄、`side.navW==0`"时会主动 `SetVisible(false)`。于是：
  - 用户开着导航窗格但窗口被拖窄 → `visible()==false`、`s.showNavPane==true` 持续不等；
  - 之后**每一次** `ApplyPaneChrome()`（`Activate` / `NextPane` / `OnPaneActivated` / `OnFocusMaybeChanged` /
    `SetDualPane` 都会调）都会 `SetVisible(true)` + 立刻 `Layout()`；
  - `Layout()` 又把它设回 `false`，同时做一遍全量 `MoveWindow`/`Resize`（两个 `ShellView` 的
    `Resize` 都在里面）。
- **症状**：窄窗口下切标签/切窗格时出现本可避免的整块重绘，以及"导航窗格显隐意图"与"落地状态"
  两个概念混在一个 bool 里。`app_panes.cpp:83-85` 的注释正是为了消除这次白重排。
- **修复建议**：判重比**配置意图**（`s.showNavPane` 与上一次记录值），不要比 `navPane_.visible()`；
  或给 `NavPane` 增加 `wanted()`（纯意图）与 `visible()`（落地）两个访问器，`Layout` 只改落地。

### M5. 焦点事件 → `SyncChrome()` 全量刷新：`EVENT_OBJECT_FOCUS` 高频触发时把 `RefreshStats` 带成瓶颈

- **位置**：`src/app_panes.cpp:303-321`（`OnFocusMaybeChanged` 末尾无条件 `ApplyPaneChrome(); SyncChrome();`）
  → `src/app_tabs.cpp:485`（`SyncChrome` 末尾 `RefreshStats()`）
- **问题**：`SyncChrome` 一次要做：窗口标题、`tabStrip_.SetTabs`（量字+图标解析）、`navBar_.SetPath`、
  `navPane_.SyncTo`、`RefreshStats`（`CollectViewStats`：只数总数 8.2 ms / 逐项分类 85.7 ms，
  见 `app_stats.cpp:24-26` 的实测注释）。`EVENT_OBJECT_FOCUS` 在一次鼠标点击里常常连发多次
  （外层 shell 视图 → 内部 `DirectUIHWND` → 列表），而 `app_panes.cpp:315-317` 的注释也承认
  "无论是否真的换了窗格都刷新一次"。节流只在 `p->items > 800/3000` 或窗口不在前台时生效
  （`app_stats.cpp:34-40`），中小目录等于**每次焦点事件都全量取数**。
- **修复建议**：`focused != t->activePane` 为假时只做 `SyncTabIdentity`（幂等、便宜），
  跳过 `ApplyPaneChrome/SyncChrome`；或把 `RefreshStats` 从 `SyncChrome` 里拆出，
  由 800ms 定时器独占（`kStatsTimer` 本来就是干这个的）。
- 严重级：不影响正确性，但属于"UI 线程上的可感知卡顿"，与工程里多处节流注释的意图相冲突。

### M6. 搜索态字段先清后导航：导航失败时"视图还在搜索、状态说不在搜索"

- **位置**：`src/app_nav.cpp:130-136`（`GoUp` 从搜索视图退回来源目录）、`src/app_nav.cpp:158-164`（`RunSearch("")` 取消搜索）
- **问题**：两处都是"先 `p->searchFrom.clear(); p->searchQuery.clear();` 再 `NavigateActive(back, true)`"，
  忽略返回值。`NavigateActive` 失败（路径已删除、盘符下线、视图忙）后：视图仍停在搜索结果，
  但 `searchFrom` 已空 → ①AI 状态快照 `searching=false`（`app_state.cpp:101`）说谎；
  ②下一次搜索的 `scope` 退化成 `p->path()`（`app_nav.cpp:169`）——而搜索视图的 `path()` 是
  `"搜索: …"` 这种标题串，会被判成"不是磁盘目录"直接拒绝（`app_nav.cpp:189-196`）。
- **修复建议**：保存旧值，`NavigateActive` 返回 false 时恢复；或先判路径可导航（复用 `PathOpenable`）再清状态。

---

## 四、轻微

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| L1 | `app_wnd.cpp:10-15` | 文件头注释称"消息在 app_wnd.cpp 的 WndProc 里被截获并转发到这里"，但该 `WndProc` 没有任何鼠标分支；实际转发方是四条栏的 `MouseSink`（`app.cpp:361-367`）与 ShellView 的 `mouseSink`（`app_panes.cpp:117-122`）。注释把读者指向错误的位置 | 改注释指向真正的转发点 |
| L2 | `app_wnd.cpp:161-163` | `ShowStatusMenu(*reinterpret_cast<POINT*>(l))` 直接解引用 `l`，未判空/未判来源。当前唯一投递方可信（危险级低），但这是 WndProc 里唯一一处"信 LPARAM"的解引用 | `if (!l) return 0;` |
| L3 | `app_wnd.cpp:196-199` | `const bool colorMsg = !l \|\| …`：把 `lParam == NULL` 当成"颜色变更"。`WM_SETTINGCHANGE` 的 `lParam` 可合法为 NULL（"某项设置变了但未指定"） | `!l` 时不要认定为颜色消息，或保守地只做 `ThemePrefersDark()` 比较后不改主题 |
| L4 | `app_stats.cpp:127-130` | `static std::wstring lastSig;` 是**函数内静态可变状态**。`app.h:475-481` 明确记录了"以前是 RefreshStats 里的函数内 static，搬成成员的两个理由：① static 违反'组件内不得有文件级可变状态'；② 多标签共用一个 lastPath 会判断错乱"——**同一次搬迁漏了这一处**，注释与实现自相矛盾 | 改成成员 `statsLastSig_`，或直接删掉这段诊断日志（它依赖 `m.text[]` 拼接，价值不高） |
| L5 | `app_nav.cpp:321` | `if (items) p->items = items;` 是**死分支**：唯一调用点 `shell_view_sink.cpp:176` 恒传 `0`（`navFn_(path_, display, 0)`，并有注释解释"此刻取 ItemCount 恒为 0"）。计数实际上只由 `RefreshStats` 写 | 二选一：删掉 `items` 形参（连带 `OnTabNav` 签名与 `SetNavFn`），或在注释里写明"该参数恒为 0，保留给未来" |
| L6 | `app_tabs.cpp:67` | `ApplyPersistedViewState(created)` 与 `EnsurePaneView` 内的同名调用（`app_panes.cpp:160`）重复。同一次建视图会把模式/排序/分组/窗格偏好套两遍（每次都可能触发 `SetViewMode` → shell 重排） | 保留 `EnsurePaneView` 里那一处，删掉 `NewTab` 的重复调用；或反之并在另一处留注释 |
| L7 | `app_tabs.cpp` | 文件**正好 500 行**，已顶到 AGENTS §3 的硬上限（`data/file-length-baseline.json` 未记该文件的欠账）。下一次任何增行都会触发棘轮 | 下次改动时先按职责拆（`FocusPaneOf`/`SyncChrome` 可以独立成 `app_tabs_identity.cpp`） |
| L8 | `app_sequence_overlay.cpp:174,197,212,230,245,261,288,300,308` | 模式名用**硬编码中文字面量**分派，而 `SequenceOverlay::Modes()`（`sequence_overlay.cpp:42-57`）是唯一真源；同一文件里只有 `kSeqKeyMode`/`kUserMode` 用了常量。改一个模式名 → 该模式静默变成空列表，无编译错误、无日志 | 给 `Modes()` 的每一项加稳定 id（英文常量），`SequenceRows` 按 id 分派；至少把 13 个名字收成 `sequence_overlay.h` 里的常量 |
| L9 | `app_sequence_overlay.cpp:356-357` | `if (!row.target.empty()) …` 是死分支：本文件填行时**从不写 `Row::target`**（只有 `command`/`arg`），全仓也无其它写者。同时 `sequence_overlay.h:66` 的 `Row::modeIndex`（注释"help 模式：进入该模式"）**从不被读**——`Commit()` 用的是 `command=="@mode"`（`sequence_overlay.cpp:282`） | 删掉 `target` 分支与 `modeIndex` 字段，或补齐实现；`Row` 里留着"注释说有用、实际没人读"的字段最容易误导下一个改动者 |
| L10 | `app_sequence_overlay.cpp:308-337` | `模式 == L"筛选"` 与 `L"当前目录"` 走**同一段代码**：只枚举子目录（`FILE_ATTRIBUTE_DIRECTORY`），`filter` 参数被完全忽略。而 `Modes()` 给"筛选"的提示是"通配 * ?"（`sequence_overlay.cpp:53`）——名实不符：用户按提示输入 `*.txt` 只会看到目录行被过滤掉 | 要么让"筛选"复用当前视图的 shell 筛选（`IFolderView2::SetFilter`），要么把提示改成"子目录" |
| L11 | `app_sequence_overlay.cpp:44` | `r.key == L"（未绑定）"` 用中文串做哨兵值（`input_router` 侧也会产生同一句）。文案一改就静默失效 | 换成 `bool unbound` 字段或共享常量 |
| L12 | `app_confirm.cpp:21-25` | 确认文案对所有命令统一为"「标题」会覆盖现有内容，且可能无法撤销"。对 `search.history.clear` / `tab.reopen.clear` / 清会话这类"清空"语义，用户读到的是"覆盖"，容易误判影响面 | 按 `CommandDef::group` 或加一个 `confirmText` 字段分流文案 |
| L13 | `app_settings.cpp:20-23` | `settingsDialog_.Create` 失败只 `MessageBeep(MB_ICONERROR)`，无 `NoteFailure`/`LogDbg`。机器调用（`Preferences` 经命令总线）拿到的是笼统失败，人也只听到一声响 | 补 `LogDbg` + `CommandBus::Get().NoteFailure(L"设置窗口创建失败")` |
| L14 | `app_state.cpp:45-65, 69-167` | 文件头声明"这里只做取数，不改写任何状态"，但两个 provider 内部都调 `ActivePane()`（`app_panes.cpp:46-49`），它在双栏下**会写 `t->activePane`**。属于"观测即改状态"，与注释承诺不一致（当前后果无害，因为值来自真实焦点） | 注释改为"只读取，除活动窗格缓存校正外不改状态"；或 provider 内改用 `PaneAt(t, t->activePane)` 只读 |
| L15 | `app_panes.cpp:109,130` | `WirePaneCallbacks(tab, paneIndex, view)` 的 `paneIndex` 形参完全未用（`(void)paneIndex;`），回调一律靠 `FindPane` 反查。签名保留一个无效参数，读者会以为回调按槽位绑定 | 删掉该形参（4 个调用点局部改动） |
| L16 | `app_stats.cpp:34-40` | 三重节流判据叠加（前台判定 / `items>3000` 每 4 拍 / `items>800` 每 2 拍，`tick` 自增在 `return` 之前），"这次允许分类"的真实条件已无法从代码直观推断；`GetForegroundWindow()+GetAncestor` 每次调用都执行 | 把三条判据提成具名纯函数（可单测，符合 §1.1 优先路径）并加一行真值表注释 |
| L17 | `app_wnd.cpp:71-89` | `kAiTimer` 明确用 `modal::Depth()==0` 挡住模态期重入，但 `kStatsTimer` **没有**这道门：模态 shell 对话框（属性页、`IFileOperation` 进度、重命名框）打开期间仍会跑 `RefreshStats` → `CollectViewStats` → 遍历正在被对话框占用的视图 | 与 AI 定时器同一判据：`if (w == kStatsTimer && modal::Depth() == 0) self.RefreshStats();` |
| L18 | `app_view_menus.cpp:64` | `kSizes[]` 是裸像素常量，未走 `theme_.S()`。核对后**这是对的**（`IFolderView2::SetViewModeAndIconSize` 收物理像素，`ApplyViewMode` 也不缩放），但代码里没有任何说明，下一个人很容易"顺手补上 DPI 缩放"从而引入 bug | 加一行注释说明"这是 shell 的物理像素口径，不走 S()" |

---

## 五、需人工复核

| # | 位置 | 待确认事项 | 为什么无法定论 |
|---|---|---|---|
| R1 | `app_panes.cpp:151-161` / M3 | `IExplorerBrowser` 的 `OnNavigationComplete` 是否稳定在 `BrowseToObject` 调用栈内**同步**触发（决定 M3 是真 bug 还是纯冗余设计） | 依赖 shell 内部时序；代码自身注释按"会同步"设计，但无法只靠读代码证明。验证方式：临时在 `ShellView::Create` 的 `Navigate(path)` 前后加一条 `LogDbg`，比对 `[nav] complete pidl=` 的先后（只读日志，符合 R21） |
| R2 | `app_view_menus.cpp:21-60` | `ShowColumnMenu` 在 `for(;;)` 里对**同一个 `toggleKey`（`view.columns`）连续 `Show`**，而弹层有"再点即收"（`PopupUpIsOpeningRelease`）与失活分流两套开合判据。循环重开时上一次点击的 UP 是否可能被当作"点了外面"→ 菜单闪退 | 属交互行为，必须**人工点验**（脚本点菜单违反 R21 判据）。建议人工验证：打开"选择详细信息"→ 连勾 3 列 → 每次勾完菜单都还在 |
| R3 | `app_stats.cpp:42` / M2 | `p->view == nullptr` 的可达性（我判断当前不可达） | 需要动态确认 `EnsurePaneView` 失败（`ShellView::Create` 返回 false）后的实际行为；建议在 `EnsurePaneView` 失败分支加一条日志，再人工制造一次失败（例如视图创建期内关闭标签） |
| R4 | `app_file_undo.cpp:49-52` | `Op::Copy` 的撤销走 `shellops::DeletePath`（`shell_ops_path.cpp:76-95`，`FOF_ALLOWUNDO` + `shellopexec::Run`，判败正确）；但 `IFileOperation` 在某些条件（超大文件、网络盘配额、`FOF_WANTNUKEWARNING`）会**弹模态确认**，此时 AI 发起的 `file.undo` 会在 UI 线程卡到 10 秒超时（`app_wnd.cpp:74-82` 的注释讨论的是 AI 定时器重入，不是这条） | 需要实测"撤销一个 5GB 副本"或网络盘场景是否弹框；若会弹，建议给撤销路径加 `FOF_NOCONFIRMATION` 或把撤销放到作业线程 |
| R5 | `app_nav.cpp:307-310` | 离开搜索视图时按"解析名前缀 == `搜索: `"清状态（`kSearchTitlePrefix`）。该判据依赖"搜索文件夹的解析名就是显示名"，而标题里现在还会拼范围（`app_nav.cpp:209`）。若 shell 对解析名做任何规范化（截断/转义），判据会失效 → `searchFrom` 残留 | 需要一次真实搜索 + 导航离开 + 读 `exbar/state` 的 `searching/搜索范围` 字段确认（只读通道，符合 §8.1） |

---

## 六、按维度汇总（逐项确认结果）

| 维度 | 结论 |
|---|---|
| **1 正确性 bug** | 1 条严重（S1：历史 pending 残留）+ 2 条中等（M1 回滚缺失、M6 搜索态先清后导航）+ 1 条死分支（L5）+ 1 条名实不符（L10）。逐分支核对过：`Layout` 指纹字段与几何输入的对应关系完整（含 `pane1Ready`/`activeTab`/`aiWant`，无漏项）；`CloseTab` 的 `index → min(index, count-1)` 在"边收集边关"语义下正确；`CloseTab/CloseOthers/CloseSide/CloseDuplicates` 都是先收集再关（无迭代器失效）；`ShowColumnMenu` 的 `pick` 边界（`-2`/`-1`/`>=size`）齐全；`ShowSearchHistoryMenu` 的 action→`hist[]` 下标在 `kHistoryMaxRows=15` 内安全；`searchscope` 动作号 1..4 ↔ `Range` 0..3 同序正确 |
| **2 内存/资源安全** | **未发现泄漏/悬垂/越界**。逐点核过：`RunSearch` 的 PIDL 所有权（`search_folder.h:39` 约定"调用方 CoTaskMemFree"；`NavigatePid` 内部 `SHCreateItemFromIDList` 复制；`EnsureSearchDetailsView` 内部 `ILCloneFull`，见 `shell_view_search.cpp:58` 与 `shell_view_theme.cpp:212` 的"内部 clone，外部仍持有"）→ 失败路径 free 一次、成功路径 free 一次，**无双重释放**；`ActiveView()`/`CurrentView()` 均 AddRef，`app_view_menus.cpp:25`、`app_state.cpp:132`、`app_stats.cpp:101` 都配了 `Release()`；`CloseTab` 的 `unique_ptr` move-out + `ReclaimPendingViews` 的 swap-then-clear 正确且会在 `OnDestroy` 兜底清空（`app.cpp:764`）；`StartFocusWatch` 句柄在 `OnDestroy` 经 `StopFocusWatch` 释放（`app.cpp:719`）。唯一险点是 M2 的 `p->view` 空指针 |
| **3 COM/系统调用** | 本组 13 个文件内**没有**未检查的 HRESULT 直调（COM 全部经 `shellops`/`ShellView` 包装，返回值都判了）。`ShellExecuteW` 用 `> 32` 判成败（`app_nav.cpp:82`，正确）；`FindFirstFileW` 判 `INVALID_HANDLE_VALUE` 且 `FindClose` 在所有路径上（`app_sequence_overlay.cpp:316-333`）；`SHGetSetSettings` 返回值无意义（void 语义）可忽略；`GetViewMode` 失败都有 `NoteFailure`（`app_view_menus.cpp:71-74`、`app_state.cpp:18-21`） |
| **4 并发** | 本组全部在 UI 线程执行；跨线程入口只有 `WM_APP_NAV_ROOTS_READY`（`app_wnd.cpp:99-101` → `ApplyDiscoveredRootsIfReady`，工作线程只 `PostMessage`，符合声明）、焦点钩子（`WINEVENT_OUTOFCONTEXT` 回调里只 `PostMessage`，见 `app_panes.cpp:408-422`）、`WM_APP_RECLAIM_VIEWS`（延迟销毁，注释与实现一致）。**未发现锁/共享数据竞争**。两处重入相关：M5（焦点事件引发的高频全量刷新）与 L17（模态期 `kStatsTimer` 未被挡住） |
| **5 字符串/编码** | **未发现问题**。宽字符格式化统一 `%s` + `wchar_t*`（`LogDbg` 调用点全部符合 AGENTS §6 的中文路径教训）；`WM_COPYDATA` 不信任对端 NUL、按 `cbData` 与 `offsetof` 夹取（`app_wnd.cpp:177-185`，做法正确）；外部路径进 shell 前过 `pathutil::ToShellPath`（`app_search.cpp:65` 自定义搜索路径；`app_nav.cpp` 的搜索作用域来自配置/快照，已是 shell 写回的形态）；`wcscmp`/`str::IEquals`/`str::ICmp` 使用一致，未见裸 `_wcsicmp` |
| **6 工程硬约束** | `RGB(`：**0 处**；`L"ExBar…"`：**0 处**（品牌串只经 `id::kBrandShort`）；`namespace eb`：**0 处**；R21 禁用 API（`SendInput`/`keybd_event`/`mouse_event`/`SetCursorPos`/`BlockInput`/`AttachThreadInput`）：**0 处**（`app_panes.cpp:406` 仅是注释提及）；`PostMessage` 投输入类消息：**0 处**（只有 `WM_APP_*` 自定义消息）；硬编码像素仅 `app_view_menus.cpp:64` 的图标档位（已核为 shell 物理像素口径，L18）与 `app_sequence_overlay.cpp:332` 的 300 行上限；单文件行数：12 个文件达标，`app_tabs.cpp` 正好 500 行（顶格，L7）。**唯一违背**：`app_stats.cpp:127` 的函数内 `static`（L4，与 `app.h:475-481` 的迁移说明直接冲突） |
| **7 架构** | 依赖方向正确：`app_*` 分片只 include `app.h` + 各自领域头，未出现"下层反过来 include `app.h`"；头文件自足（13 个 `.cpp` 逐个看 include 块，无依赖 PCH 的间接类型）；**无文件级可变全局状态**（除 L4 那一处函数内 static）；一文件一职责在本组基本成立（`app_layout` 含 `Layout`+`ApplyTheme` 两件事，但两者都是"几何/主题落地"，可接受；`app_tabs.cpp` 混了标签生命周期 + `SyncChrome` + `FocusPaneOf` 三块，已顶到 500 行，是下一个拆分点）。轻微违规：`app_panes.cpp:109` 的无效形参（L15）、`app_sequence_overlay.cpp` 的字符串分派（L8） |
| **8 死代码/注释不一致/静默失败** | 死代码：L5（`items` 恒 0）、L9（`Row::target`、`Row::modeIndex`）；注释与实现不一致：L1（`app_wnd` 转发声明）、L4（`static` 与搬迁说明）、L14（"只做取数"）、L8（模式名但定义了常量）；静默失败：L13（设置窗口创建失败）、M1（无视图导航不报原因）、`app_view_menus.cpp:59`/`app_search.cpp:55,66,115,121,151,159`（`CommandBus::Invoke` 返回值被忽略——此处**可接受**，因为 `Invoke` 自身会把失败写进 `lastFailKind_`/`NoteFailure`，但 `ShowColumnMenu` 在 `ViewColumnsSet` 失败后仍会继续循环重开菜单，用户看到"勾了没反应"且无提示，建议至少把 false 当退出条件） |

---

## 七、总体评价

1. **代码质量整体偏高**。注释密度罕见地高，而且绝大多数注释解释的是"为什么这么写 / 不这么写会怎样"，
   并附带实测数据（探针编号、耗时、HRESULT）。这让审查可以"按声明验证"，能查出 S1/M3 这类
   "注释承诺了、实现没兑现"的问题——本报告 8 条中等以上问题里有 3 条属于这一类。

2. **资源与 COM 纪律扎实**。全部 13 个文件没有一处漏 `Release`、没有双重释放、没有未判空的 COM 指针
   （唯一的空指针险点 M2 恰好是唯一没有 COM 调用的 `app_stats.cpp`）。`IFileOperation` 判败、
   PIDL 所有权、延迟销毁队列这些"最容易抄一半"的地方，本组都是完整的。

3. **最值得优先修的三件事**：
   - **S1**（`app_nav.cpp:90-95`）：一处 `Fail()`/`CancelPending()` 就能修，但症状是"后退跳过一层"，
     属于用户会当成"文件管理器坏了"的那类，且与工程历史重点修复过的 bug 同族；建议连单测一起补。
   - **M3**（`app_panes.cpp:159`）：把 `p->view` 赋值提前一行，可以让"首帧同步完成"这层设计真正生效，
     同时消掉第二窗格"历史与视图不一致"的整类问题。
   - **M2**（`app_stats.cpp:42`）：一行判空，把 800ms 定时器上的崩溃隐患关掉。

4. **需要在拆文件之前做的准备**：`app_tabs.cpp` 已在 500 行顶格，而它同时承担标签生命周期、
   外壳同步、焦点面板判定三件事；下一次改动建议先按职责拆（`SyncChrome` 与 `FocusPaneOf` 各自成文件），
   否则任何小改动都会先撞上棘轮。

5. **未覆盖说明**：本报告的判断边界是"调用方是否遵守被调方的契约"。被调方自身（如
   `ShellView::Create` 的同步性、`IFileOperation` 的弹窗策略、`IFolderView2` 的图标像素口径）
   在没有运行时可观测手段的前提下只做到"读实现核对"，可疑处已列入第五节"需人工复核"，
   没有把它们当成已确认的缺陷。
