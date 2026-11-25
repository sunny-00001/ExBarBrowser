# G4_nav 组审查报告（nav_* 24 文件 / 5162 行）

审查方式：逐文件**完整读取**（无抽样），逐函数逐分支过一遍；快照时间 **2026-11-23 21:31**。
说明：审查期间工作区有并发改动，`nav_bar.cpp`(555→552)、`nav_pane.h`(240→236)、
`nav_pane_shell.cpp`(402→317)、`nav_pane_explorer.cpp`(404→406) 变动过，这 4 个文件已**重读并按当前内容定论**；
其余 20 个文件行数与读取时一致。原 `nav_pane_shell.cpp` 里调用 `ui::ShiftPixels()`（全仓不存在该符号）
的 `PaintExpandoOffset` 已在本轮改动中删除，相关声明也一并消失，**当前快照无该编译问题**。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|---|---|---|
| 1 | src/nav_bar.cpp | 552 | 有缺陷：超行数棘轮；UI 层文件级全局；搜索框内边距误开地址编辑；编辑态跳过 tooltip 重建 |
| 2 | src/nav_bar.h | 283 | 基本合格：2 个零调用访问器；`SettingsFn(POINT)` 参数已成摆设（注释已如实说明） |
| 3 | src/nav_bar_actions.cpp | 363 | 有缺陷：Tab 多候选无法轮换；`(void)screen` 全族死参；枚举先于"收起"判定（与注释顺序相反） |
| 4 | src/nav_bar_draw.cpp | 176 | 合格：绘制原语收口良好；仅裸 S(3)/S(4) 偏移与"paint 里写 moreRc_"两处风格问题 |
| 5 | src/nav_bar_search.cpp | 177 | 有缺陷：文件级全局 `g_oldSearchProc`；窄窗下 Ctrl+F 变死键；缺 `#include "str.h"` |
| 6 | src/nav_bar_view_menu.cpp | 183 | 有缺陷：排序方向翻转在"任意属性列排序"时退化成按名称排序；注释与参数不符 |
| 7 | src/nav_history.cpp | 136 | 合格：栈语义单点收口；`forward()` 的 `size_t(idx_+1)` 依赖"idx_ 只会是 -1"这条巧合 |
| 8 | src/nav_history.h | 97 | 有缺陷：`Request()` 返回值语义注释与实现相反（唯一一处硬错，调用方对） |
| 9 | src/nav_metrics.h | 32 | 合格：14 个常量全部有引用（无死常量）；注释里写外部项目行号，违反"不写行号" |
| 10 | src/nav_model.cpp | 116 | 有缺陷：`ChildFolders()` 进 shell 前不过 `ToShellPath`（Tab 补全吃不下连续分隔符）；无长路径处理 |
| 11 | src/nav_model.h | 42 | 合格：ghost 段语义清楚；头文件靠 common.h 间接拿 `<string>/<vector>` |
| 12 | src/nav_pane.cpp | 635 | 有缺陷：IUnknown 三件套死代码且 `Release()` 无定义；`width()` 死访问器；日志 sprintf 隐患；兄弟递归栈风险；已记账欠行（635/636） |
| 13 | src/nav_pane.h | 236 | 有缺陷：3 处注释与实现相反（SELECTED vs SELECTEDNOEXPAND、分隔线"覆盖层"、SyncTo"可见并选中"） |
| 14 | src/nav_pane_events.cpp | 267 | 合格：事件语义逐条对齐官方 Remarks；仅"只取 items[0]"这类可接受简化 |
| 15 | src/nav_pane_expand.cpp | 63 | 合格：迭代展开 + 10000 节点护栏；无锁需求 |
| 16 | src/nav_pane_explorer.cpp | 406 | 有缺陷：GUID 常量与 `ItemFromClsid` 与 nav_pane_root.cpp 重复实现；长路径注释写反 |
| 17 | src/nav_pane_explorer.h | 43 | 合格：数据源取舍有实测依据 |
| 18 | src/nav_pane_root.cpp | 423 | 有缺陷（本组最重）：9 个文件级可变全局 + 线程；跨层 include 发现层；`path:` 未归一化 |
| 19 | src/nav_pane_root.h | 128 | 有缺陷：头文件声称"纯数据语义/可单测"，却声明线程与注册表监听 API |
| 20 | src/nav_pane_root_watch.cpp | 184 | 有缺陷：4 个文件级可变全局；hook 在工作线程上调 `RefreshDefaultsAsync`（跨线程改建 `std::thread`） |
| 21 | src/nav_pane_sep.cpp | 21 | 合格：画刷走 `ui::FillRectColor` 唯一实现 |
| 22 | src/nav_pane_sep.h | 44 | 合格：三种被否决方案都有实测依据 |
| 23 | src/nav_pane_shell.cpp | 317 | 有缺陷：`UnhookTreeScroll()` 不摘子类（refData 悬垂风险）；`refreshing_` 期间的变更被丢弃 |
| 24 | src/nav_path.h | 238 | 有缺陷：`Normalize` 与 `pathutil::ToShellPath` 两套归一化（头注释却自称"唯一实现"）；扩展路径 `\\?\` 未排除 |

合计 **24 文件 / 5162 行**（与 `wc -l` 一致）。

---

## 二、P1（工程硬约束 / 生命周期）

### P1-1 文件级可变全局状态 15 个，超出 App/Config/ai_server 例外
- `src/nav_pane_root.cpp:108-135`：`g_rootsMu` `g_discovered` `g_discoverReady` `g_prefetchThread`
  `g_prefetchStarted` `g_refreshThread` `g_refreshRunning` `g_refreshHook` `g_readyHook`（9 个）
- `src/nav_pane_root_watch.cpp:28-31`：`g_watchThread` `g_watchStop` `g_watchMu` `g_watchHook`（4 个）
- `src/nav_bar.cpp:12`：`g_oldEditProc`；`src/nav_bar_search.cpp:21`：`g_oldSearchProc`

AGENTS §3 的口径是"无全局可变状态，例外只有 `App::Get()` / `Config::Get()` / `ai_server` 的管道线程状态；
栏与组件内部不得有文件级可变状态"。这 15 个全部是"组件内部"的。后果不只是合规：
- 根集缓存是**进程级单例**，与"导航窗格全局一棵"绑死，将来第二棵树（或单测同时起两个）就共享同一份状态；
- `g_oldEditProc` / `g_oldSearchProc` 是**单槽**，第二个 NavBar 实例会把第一个的旧 WNDPROC 覆盖掉（链条断）。

修复：把根集缓存 + 两个线程收成一个对象 `navpane::RootSource`（`Start/Shutdown/Take/OnChanged` 全做成员），
由 `App` 持有并在 `OnCreate/OnDestroy` 显式管；EDIT 子类化改用
`SetWindowSubclass(edit_, &NavBar::EditProc, kEditSubclassId, reinterpret_cast<DWORD_PTR>(this))`
（与 `nav_pane_shell.cpp:166` 同一做法），彻底删掉两个 `WNDPROC` 全局。

### P1-2 `nav_bar.cpp` 552 行 > 棘轮记账 547 行 → `check_components.py` 会判失败
`data/file-length-baseline.json:15` 记的是 `"nav_bar.cpp": 547`，判据是
`tools/check_components.py:670` `length_hits.append((name, n))`（`splitlines` 计数 = 552）。
自动复活一次 `python tools/check_components.py` 就是红灯。

修复：继续按职责拆（`Layout()`+`Hit*`+`NavTargetAt` 一族 ≈ 160 行 → `nav_bar_layout.cpp`），
拆完不要重签基线（棘轮只在降下来时自动收紧）。

### P1-3 数据层反向依赖发现层（`nav_pane_root.cpp` include `nav_pane_explorer.h`）
`src/nav_pane_root.cpp:5` `#include "nav_pane_explorer.h"`，为的是 `DiscoverExplorerRoots()`。
而 `nav_pane_root.h:6-7` 明写"这里是**纯数据语义**……全部可在没有窗口、没有消息循环的情况下单测"。
现在这个 TU 必须连同 `nav_pane_explorer.cpp` 一起链接，`tests/nav_root_test` 的"单独可测"承诺已经名存实亡；
`nav_pane_root.h:100-127` 还把 `PrefetchDefaultsAsync/RefreshDefaultsAsync/StartRootChangeWatch`
（线程 + 注册表通知）声明进了同一个"纯数据"头。

修复：新增 `nav_pane_roots_source.{h,cpp}` 装"缓存 + 线程 + 监听"（可 include explorer），
`nav_pane_root.{h,cpp}` 只留 `Parse/Format/ItemFromRootSpec/ItemFromPath/NormalizeParsingName/
IsKnownFolderName/KnownFolderNameForPath`。这样依赖方向恢复成
`source(线程层) → explorer(发现层) → root(数据层)`。

### P1-4 `UnhookTreeScroll()` 只清指针不摘子类 → refData 悬垂（潜在 UAF）
`src/nav_pane_shell.cpp:171-174`：
```cpp
void NavPane::UnhookTreeScroll() {
    treeHooked_ = nullptr;   // 子类链随窗口一起消失，不要对可能已死的窗口 RemoveWindowSubclass。
}
```
子类过程 `src/nav_pane_shell.cpp:121-159` 把 `refData` 当 `NavPane*` 用（`PaintSeparatorLines`、
`OnTreeGeometryChanged`）。内部树是 **NSTC 建的**：`NavPane::Destroy()` 里 `tree_->Release()` 之后若该窗口
仍存活（NSTC 内部还持有窗口 / 句柄被复用给别的窗口），`WM_PAINT` 就会带着已析构的 `NavPane*` 进来。
注释担心的"野句柄"其实可以用 `IsWindow()` 安全挡掉。

修复：
```cpp
void NavPane::UnhookTreeScroll() {
    if (treeHooked_ && IsWindow(treeHooked_))
        RemoveWindowSubclass(treeHooked_, &NavPane::TreeSubclassProc, kTreeSubclassId);
    treeHooked_ = nullptr;
}
```
并把 `HookTreeScroll` 里的魔数 `1`（`src/nav_pane_shell.cpp:166`）提成 `kTreeSubclassId`。

---

## 三、P2（功能缺陷 / 跨线程契约）

### P2-1 Tab 补全：多候选**无法轮换**
`src/nav_bar_actions.cpp:84-126`。`DoComplete()` 用 `if (text != tabBase_)` 决定是否重算候选，
但补全成功后 `SetWindowTextW(edit_, out)`（:122）让编辑框文本变成 `dir + 候选`，
下一次 Tab 时 `text != tabBase_` 成立 → 以"候选本身"为前缀重算 → 通常只剩 1 个 → 永远补同一个。
`tabIdx_` 的自增在 `tabMatches_.size()==1` 时等于空转，用户只能手动删字才能看到第二个候选。

修复：补全成功后把 `tabBase_` 更新为 `out`（并把 `tabMatches_` 保留），只在"文本与 `dir+任一候选` 都不同"
时才重算；或用 `nav::SplitForComplete(text).dir` 与 `tabBase_` 的 dir 比较来判断"用户是否改过目录"。

### P2-2 `ChildFolders()` 进 shell 前不过 `pathutil::ToShellPath`
`src/nav_model.cpp:64-70`：`SHParseDisplayName(folder.c_str(), ...)` 直接吃调用方给的串。
唯一喂"用户输入"的调用点是 `nav_bar_actions.cpp:103`（Tab 补全），其 `dir` 来自
`nav::SplitForComplete` → `nav::Normalize`，而 **`Normalize` 不折叠连续分隔符**（见 P2-3）：
- 输入 `C:\\Windows`（连续反斜杠，从配置/URL 拼出来的常见形状）→ `dir = "C:\\\\"` → `SHParseDisplayName`
  返回 `0x80070057` → 候选为空 → Tab 静默失效；
- 同一串在地址栏回车是**能开的**（下游 `shell_view` 会归一化）→ "回车能开、Tab 补不出来"，
  正是 AGENTS §4.1 想消灭的那类"同一个输入两套表现"。

修复：`ChildFolders` 入口 `const std::wstring p = pathutil::ToShellPath(folder);`，
（长路径再按需 `tf::ToExtended`），一处修好同时覆盖 `folder_tree::Children`。

### P2-3 `nav::Normalize` 与 `pathutil::ToShellPath` 是两套归一化，而头注释自称"唯一实现"
`src/nav_path.h:37-43`：只做 `'/'→'\'` + 去尾分隔符，**不折叠连续分隔符**、不处理 `\\?\`。
`src/path_util.h:35-51` 的 `ToShellPath` 才是"外部路径 → shell API"的唯一收口（含折叠）。
`nav_path.h:1` 写的是"导航路径语义：全项目唯一实现"，容易被读成"归一化也只有这一份"。

修复：`Normalize` 内部转发 `pathutil::CollapseSeparators`（仍保留"命名空间原样"那条），
或在文件头把边界写死：`nav::Normalize` 只负责**显示层**清理，凡进 shell 一律 `ToShellPath`。

### P2-4 `RootSpec::Kind::Path` 的配置值未归一化就进 shell
`src/nav_pane_root.cpp:392-395`：`SHCreateItemFromParsingName(spec.value.c_str(), ...)`。
配置里写 `path:C:/Users/x`（正斜杠）在 `CreateFile` 侧合法、在 shell 侧必然 `E_INVALIDARG`，
这与 AGENTS §4.1 第一条直接冲突，且症状是"根静默消失"（`RebuildRoots` 只记一行"根解析失败"）。

修复：Path 分支套 `pathutil::ToShellPath(spec.value)`；`Parse` 里顺手也归一化一次（配置写回更干净）。

### P2-5 注册表监听线程直接调 `RefreshDefaultsAsync`，跨线程改写 `std::thread`
`src/nav_pane_root_watch.cpp:156-161` 在**监听线程**上 `hook()`；
`src/app.cpp:190-194` 这个 hook 就是 `navpane::RefreshDefaultsAsync([this]{...})`；
而 `src/nav_pane_root.cpp:279-322` 里 `g_refreshThread.join()` / `g_refreshThread = std::thread(...)`
（:297、:299）**不在锁内**，锁只保护 `g_refreshRunning`。

现在能工作只靠一条隐式约定："只有把 `g_refreshRunning` 置位的那个线程会碰 `g_refreshThread`"。
两条现实风险：
1. `PrefetchShutdown`（`src/nav_pane_root.cpp:243-277`）改了顺序（先 join 后 `StopRootChangeWatch`）
   就会变成"UI 线程 join / 监听线程赋值"同时发生 = `std::thread` 数据竞争（UB）；
2. hook 在工作线程里读 `App::hwnd_`（`app.cpp:191-193`），那是非原子成员，析构后读即 UAF
   （目前靠 `PrefetchShutdown` 的 join 顺序兜住，没有 RAII 守护）。

修复：把 `{thread, running, hook}` 收进一个小结构体，**所有**读写（含 join 与赋值）都在同一把锁内做，
并把"hook 在工作线程被调、不得碰 UI（只能 PostMessage）"写进 `nav_pane_root.h` 的契约里；
`PrefetchShutdown` 里加一个 `g_stopping` 标志，之后 `RefreshDefaultsAsync` 直接返回 false。

### P2-6 `PrefetchTake()` 在"就绪但空"时返回 false，与头文件语义冲突
`src/nav_pane_root.cpp:235-241`：`return !g_discovered.empty();`；
`src/nav_pane_root.h:102-104` 写的是"返回 true = 取到了；没算完/没启动过返回 false"。
`src/app_panes.cpp:105` 据此 `if (!PrefetchTake(&roots)) return;` → 一台"发现不出任何东西"的机器会
永远停在兜底三项，且没有任何日志说明原因（`ApplyDiscoveredRootsIfReady` 静默返回）。

修复：改成"就绪即返回 true，空结果由 `roots->empty()` 表达"，或在失败分支补一行日志。

### P2-7 点在搜索框的内边距上 → 打开的是**地址栏**编辑框
`src/nav_bar.cpp:340-348`：搜索框两条带（`searchMenuRc_`/`searchHistRc_`）判完就直接
`if (x >= fieldRc_.left) BeginEdit();`。搜索框整体是 `searchRc_`（`nav_bar_search.cpp:82`），
两条带各有 `S(2)` 内缩（:90-94）、输入条又夹在两条带之间（:99-100）——
点这些"框内但不属于任何带、且不在 EDIT 上"的像素，会打开**地址栏**编辑框而不是搜索框。

修复：`if (searchShown_ && PtInRect(&searchRc_, pt)) { FocusSearch(); return 0; }` 放在 `BeginEdit` 之前。

### P2-8 编辑态 `Layout()` 提前返回，跳过 `RebuildTooltip()`；`ReloadTheme()` 会留下空 tooltip
`src/nav_bar.cpp:464` `if (editing_) return;` 之后才是 `RebuildTooltip()`（:500）；
`src/nav_bar.cpp:129-130` 先把 `tooltip_ = ui::TooltipHost();`（销毁旧窗）再 `Layout()`。
于是"地址栏编辑中切主题/缩放"→ 气泡窗口没了、热区没建，直到下一次非编辑态 Layout。

修复：把 `RebuildTooltip()` 提到 `if (editing_) return;` 之前（按钮矩形在 :406-410 已算完），
或在 `ReloadTheme` 里显式 `RebuildTooltip()`。

### P2-9 视图菜单"改为降序"在**任意属性列**排序时会变成"按名称排序"
`src/nav_bar_view_menu.cpp:159-173`：注释（:154）说"arg 用 100 表示翻转方向"，但代码
`onSortGroup_(0, current >= 0 && current < kSortGroupCount ? current : 0)` 传的是 `current`，
`100` 只是菜单行号。`vs.sort` 在"按任意属性列排序"时是 **-1**（`app.cpp:266-272` 的注释也承认
GetSort 认不出任意列 → 保留配置档位/留 -1），于是 `current = -1` → 落成 `0` = 名称列。

修复：方向翻转走独立参数（真的把 `100` 传给 `onSortGroup_`，App 侧转
`view.sort.bykey {key, ascending: !asc}`），或干脆不传 int 而传一个小枚举；同时把 :154 的注释改成与代码一致。

### P2-10 窄窗口下 Ctrl+F 是死键（无任何反馈）
`src/nav_bar_search.cpp:131-137`：`FocusSearch()` 只看 `hwnd_ && search_`，不看 `searchShown_`；
窗口窄到 `LayoutSearch` 收起搜索框时（`nav_bar_search.cpp:73-81`，EDIT 被 `SW_HIDE`），
`SetFocus(search_)` 对隐藏控件无效 —— 命令成功返回、界面上什么都没发生。

修复：`if (!searchShown_) { FocusAddress(); return; }`（或临时强制展开一次宽度）。

### P2-11 `refreshing_` 期间的 shell 变更被丢弃（不重排）
`src/nav_pane_shell.cpp:289-293`：`if (... || refreshing_ || refreshTimer_) return;`
刷新过程中到来的变更既不定时也不补一次，树会停在旧状态直到**下一次**外部变更。

修复：加 `pendingRefresh_`，`RefreshExpandedItems()` 末尾（:314 之前）若置位则再 `ScheduleShellRefresh()`。

---

## 四、P3（注释与代码不符 / 死代码 / 风格）

### 注释与实现相反（会误导下一个改这块的人）
1. `src/nav_pane.h:31-33` vs `src/nav_pane.cpp:612-634`：头注释写"视图 → 树：`SetItemState(...NSTCIS_SELECTED)`
   + `EnsureItemVisible`"，实现刻意用 `NSTCIS_SELECTEDNOEXPAND` 且**不**调 `EnsureItemVisible`
   （cpp 里写明了"只能展开不能折叠"的修复）。按头注释改回去 = 复活自激回环。
2. `src/nav_pane.h:88-90`：`SyncTo` 注释"让某个路径在树里**可见**并选中" —— 实现只改高亮、不展开父链。
3. `src/nav_pane.h:144-145`："分隔线画在树**外面**的覆盖层上"与 `nav_pane_sep.h:20-26`
   "在内部树的 WM_PAINT 里画"矛盾（实现是后者）。
4. `src/nav_history.h:63-66`：`Request()` 注释"返回 false = 目标就是当前路径且**要求不压栈**"，
   实现是 `return !(sameAsHere && push)`（要求**压栈**时返回 false）；调用方
   `src/app_nav.cpp:89-95` 与实现一致 —— 只有注释错。
5. `src/nav_pane_explorer.cpp:149`："不过 `tf::ToExtended` 会静默判成不存在" —— 语义写反了：
   `:150` 正是**用** `tf::ToExtended` 来支持 `>MAX_PATH`（长路径这条**合规**，不用改代码，只改注释）。
6. `src/nav_bar_view_menu.cpp:59` `(void)screen;` 之后 `:120` 又用了 `screen`（`screen.x + 8`）——
   死语句；同类：`nav_bar_actions.cpp:129/199/244/267/294` 五个函数的 `POINT screen` 参数全部
   `(void)`（调用方仍在算 `ClientToScreen`），签名可以直接删参数。
7. `src/nav_bar_actions.cpp:139-149`：注释"先判再置位"，实际 `folder_tree::Children(target)`
   （:141，一次 shell 枚举）跑在 `popup::IsOpen()` 判定（:168）**之前**。
8. `src/nav_pane_root.cpp:112-115`："`g_prefetchThread` + 是否启动过……不需要锁" ——
   实现里 `g_prefetchStarted` 的读写**都在** `g_rootsMu` 内（:192-197、:284），注释与代码相反。

### 死代码 / 未定义符号
9. `src/nav_pane.h:117-120` + `src/nav_pane.cpp:122-131` + `src/nav_pane.h:229`：`QueryInterface/AddRef/Release`
   **零调用**，`NavPane::Release()` **只有声明没有定义**（谁调用谁链接失败），`comRef_` 只增不减；
   头注释"事件适配器持有的是回指"也不成立（`Events` 从不 `AddRef`）。建议整组删除。
10. `src/nav_pane.h:96-97` `width()`：注释"App 布局用它决定窗格区左边界"，实际 `app_layout.cpp`
   用的是 `side.navW`（配置求解），`width()` 零调用（`app_panes.cpp:65` 的注释同样过期）。
11. `src/nav_bar.h:70-71` `DisplayText()` / `IsEditing()`：零调用。
12. `src/nav_pane.h:214-216` `SepItem::rootIndex`：只在 `nav_pane.cpp:442` 初始化，从未被读
    （注释说"诊断/日志用"，但 `UpdateSeparatorLines` 的日志只打 `sepItems_.size()`）。

### 唯一实现被复制
13. `src/nav_pane_root.cpp:22-23` 与 `src/nav_pane_explorer.cpp:19-20`：`kClsidHome` / `kClsidGallery`
    **两个文件各定义一份**；`nav_pane_explorer.cpp:41-48` 的 `ItemFromClsid` 与
   `nav_pane_root.cpp:374-382` 的 Clsid 分支是同一段"GUID → `shell:::{GUID}` → IShellItem"。
    修复：把两个 CLSID 与"GUID→IShellItem"提到 `nav_pane_root.h`（或 `identity` 侧），只留一份。

### 边界/健壮性（低概率）
14. `src/nav_path.h:85-105` `ParentOf` 对扩展路径 `\\?\C:\dir`：被当作 UNC → 返回 `"\\?\C:"`（非法父级）。
    函数注释里划清"不接受 `\\?\` 前缀"即可。
15. `src/nav_history.cpp:11-15` `forward()` 用 `static_cast<size_t>(idx_ + 1)`：只有 `idx_ == -1` 时安全
    （靠 `ClampIdx` 保证），建议写成 `idx_ + 1 < 0 ? 0 : static_cast<size_t>(idx_ + 1)`。
16. `src/nav_pane.cpp:163-167`：`swprintf(ys + wcslen(ys), 96 - wcslen(ys), ...)` 循环内重复 `wcslen`，
    且 `96 - wcslen(ys)` 是 `size_t` 减法（截断时变成超大值）。改 `std::wstring` 拼接更稳。
17. `src/nav_pane.cpp:74-113` `CaptureExpandedPaths`：兄弟节点递归 `self(self, next, depth)` **不增加 depth**，
    深度 = 同级节点数；`out` 满 256 才停，若整棵树一个展开项都没有就会走遍全树（含网盘节点）→ 栈风险。
    建议改成显式栈（与 `nav_pane_expand.cpp` 的迭代写法一致）。
18. `src/nav_pane_shell.cpp:108-110`：`if (FAILED(hr)) hr = tree_->EnsureItemVisible(item);`
    之后 `if (SUCCEEDED(hr)) ++restored;` —— `EnsureItemVisible` 成功但**没展开**也算"已恢复"，日志虚报。
19. `src/nav_bar.cpp:105-115` `UpdateLeadIcon`：图标解析失败也把 `leadIconPath_` 记下来 → 之后不再重试
    （网络路径未就绪的典型场景）。
20. `src/nav_bar.cpp:171-193` `EditProc` 与 `SHAutoComplete`（:40）的顺序：子类化在
    `SHAutoComplete` **之前**装，所以 autocomplete 的 proc 在链的外层，`VK_TAB` 先经它再到
    `DoComplete()`。"有候选才吃 Tab"的拦截对它无效 —— 若 autocomplete 在补全列表打开时吃掉 Tab，
    `DoComplete()` 永不执行。**需实测确认**（本条不确定，仅提示排查方向）。

### 工程约束（可机械检查项）
21. `src/nav_bar.cpp` 552 行 > 棘轮 547（见 P1-2）。
22. 裸像素：`nav_bar.cpp:427`（`S(2)`）、`:428`（`S(8)`）、`:450`（`- 1`）、`:452`（`+ 1`）；
    `nav_bar_draw.cpp:71,73,88,100,103`（`S(3)`/`S(4)`）。AGENTS §1.3 是"不写死像素"，
    这些偏移应进 `nav_metrics.h`（如 `kFieldFrame96` / `kHotPad96`）或明确标注为 1px 边框类豁免。
23. 注释里写**外部项目行号**（AGENTS §3"文档不写行号，用符号名定位"的同一理由，行号必漂）：
    `nav_metrics.h:2-3`（`Exbar.cpp:36505-36512`）、`nav_metrics.h:25`（`exbar.cpp:27590-27601`）、
    `nav_pane.cpp:188`（`mod 28756-28758`）、`nav_bar_actions.cpp:188`（同）、
    `nav_bar_search.cpp:164-165`（`exbar.cpp:19883/:20056`）。
24. 命名空间：`nav_pane*` 用 `namespace navpane`，不在 AGENTS §4 的领域清单
    （`nav/win/ico/str/tf/json/chord/shellops/ai/ui/core`）里，而同域已有 `nav`。
    要么并入 `nav`（`nav::pane`），要么把 `navpane` 补进清单 —— 否则按清单审查的人会判不合规。
25. `src/nav_bar_search.cpp:114` 用 `str::Trim` 但只 include 了 `nav_bar.h`（经 common.h 间接拿到 `str.h`）；
    同目录 `nav_pane.cpp:9` 是直接 include 的。补一行 `#include "str.h"`。
26. `src/nav_model.h:7` include `common.h`（PCH 级），而它只需要 `<string>/<vector>`；
    单测单独编 `nav_model.cpp` 时会连带拖进整个 common.h。

---

## 五、逐维度结论

**1) 路径解析 / root 判定 / ghost crumb / UNC / 盘根 / 尾分隔符（nav_path、nav_model、nav_history）**
- `Prefixes`/`ParentOf` 的 UNC 与盘根分支正确（`\\server\share\dir → \\server\share`、`C:\A → C:\`、
  `C:\ → 空`、`UpTarget(盘根) = 此电脑`），`CrumbLabel` 用 `IsDriveRoot` 而非 `s[1]==':'` 的回归修正是对的。
- ghost 段语义正确：`SetPath` 只在"新路径正是旧路径父级"时保留，`ShowSegmentMenu` 明确拒绝对 ghost 出菜单。
- 历史栈边界干净：`Push` 截断前进分支、`Fail` 弹栈 + `ClampIdx`、`Complete` 的重定向就地收敛都成立；
  唯一硬错是 `Request()` 的返回值注释（P3-4）。
- 弱点集中在**归一化分裂**（P2-2/P2-3/P2-4）与扩展路径（P3-14）。

**2) nav_pane_root\*：线程缓存 / 监听线程生命周期 / join 时序 / 死锁 / 锁**
- 无死锁路径：`PublishDiscovered` 只写缓存、hook 在锁外调（`nav_pane_root.cpp:201-206、224-230` 的注释与
  实现一致）；`PrefetchShutdown` 先 `StopRootChangeWatch()` 再 join，顺序是对的；
  `PrefetchShutdown` 对"不可中断 join"的诚实说明（:248-257）值得保留。
- 问题在**全局状态**（P1-1）、**锁覆盖面**（P2-5）、**语义返回值**（P2-6）；
  `refreshing_` 期间丢变更这一类"忙时丢通知"在根集侧只是理论风险（hook 只在发现完成时触发一次），
  在树侧是实际缺陷（P2-11）。

**3) nav_pane_explorer/shell：shell 枚举 / 长路径 / 命名空间**
- 长路径**走了** `tf::ToExtended`（`nav_pane_explorer.cpp:150`），这一条合规（注释写反，见 P3-5）。
- 命名空间解析统一补 `shell:` 前缀，`NormalizeParsingName` 已下沉到 `pathutil::ToParsingName`，收口正确。
- `nav_pane_events.cpp` 的回调返回值（点击 S_OK / 展开按钮 S_FALSE / 键盘 S_OK vs S_FALSE）逐条对齐 Remarks，
  是本组最扎实的一块。
- 弱点：`UnhookTreeScroll` 的悬垂 refData（P1-4）、`refreshing_` 丢事件（P2-11）、GUID 常量重复（P3-13）。

**4) nav_bar\*：地址栏/搜索框子类化 WndProc、绘制、状态**
- WndProc 结构清晰：UP 触发动作 + `SetCapture` + `WM_CAPTURECHANGED` 复位 + `WM_MOUSELEAVE` 不清 pressed
  这一套是对的；`NavTargetAt` 的 target 划分与 mouse_rules 同口径。
- 弱点：两个 `WNDPROC` 全局（P1-1）、搜索框内边距误开地址编辑（P2-7）、tooltip 重建时机（P2-8）、
  Tab 轮换（P2-1）、Ctrl+F 窄窗失效（P2-10）。

**5) 文件级可变全局（g_ 变量）**
15 个，**全部**不在 App/Config/ai_server 例外之内（P1-1）。

**6) 工程约束**
- R21：**无违规**。24 个文件里没有 `SendInput`/`mouse_event`/`keybd_event`/`SetCursorPos`/
  `AttachThreadInput`，也没有投 `WM_LBUTTON*`/`WM_KEY*`/`WM_CHAR` 的合成消息
  （`nav_bar_search.cpp:43,118` 投的是 `WM_APP_REFOCUS`，属焦点回还，不是输入合成）。
- 品牌串：**无** `L"ExBar…"` 字面量（只有注释里的"ExBar mod"参照说明）。
- 硬编码色：**无**代码级 `RGB(`（`nav_pane.cpp:506,550` 两处只在注释里讲历史），`sepColor_` 已改走
  `theme.navSep`，这条修得很干净。
- 命名空间：`nav` / `navpane` 并存（P3-24）。
- 跨层 include：唯一越界是 `nav_pane_root.cpp → nav_pane_explorer.h`（P1-3）。

**7) 死代码 / 注释不符**
死符号 4 处（P3-9~12），注释与实现相反 8 处（P3-1~8），重复实现 1 处（P3-13）。
本组的注释密度极高、质量总体偏上（大量"为什么不是另一种写法"的实测依据），
但正因为密度高，**注释与代码脱钩的代价也更大** —— 上面 8 处里有 3 处（SELECTED/覆盖层/SyncTo"可见"）
会把后来的人直接引回已经修过的 bug。

---

## 六、总评

这一组的**核心语义层质量明显高于平均值**：`nav_path.h` 把父级/叶子/盘根/面包屑分段收成纯函数并有回归注释，
`nav_history` 把四处手写压栈合成三个动作，`nav_pane_events` 逐条对齐 COM 契约的返回值语义，
`nav_pane_sep`/`nav_pane_expand` 短小且理由充分。R21、品牌串、硬编码色三条硬红线全部干净。

需要立刻处理的是三类：

1. **合规红灯**：`nav_bar.cpp` 552 行 > 棘轮 547（P1-2，跑一次 `check_components.py` 就会红），
   以及 15 个文件级可变全局（P1-1，AGENTS §3 明令）。
2. **会咬人的运行期问题**：`UnhookTreeScroll` 的悬垂 refData（P1-4）、注册表监听线程跨线程改建
   `std::thread`（P2-5）、Tab 补全轮换失效（P2-1）、Tab 路径不过 `ToShellPath`（P2-2）。
3. **注释债**：8 处注释与实现相反，其中 3 处描述的是**已被否定的旧设计**，是最高性价比的修复项
   （改注释即可，且能防止回退）。

架构上唯一的结构性问题是 `nav_pane_root.cpp` 这一个 TU 同时承担"纯数据 / 异步发现 / 线程 / 注册表监听"
四件事（P1-1 + P1-3），把它拆成 `roots_source` + `root` 两层，本组的全局状态与跨层依赖会同时消失。
