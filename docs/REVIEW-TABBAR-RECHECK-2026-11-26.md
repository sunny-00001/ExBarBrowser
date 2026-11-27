# 标签栏复审报告（REVIEW-TABBAR-RECHECK-2026-11-26）

> 对 `review/G10_tabjob.md`（标签栏首审）全部发现 + 本轮新增调研结论的**逐条复核与归档**。
> 首审由 3 子代理并行完成（代码审查 + 两路全网调研：优秀产品交互 / Win32 自绘实现），
> 复审在提交 `09e4a88`（双栏协同大批次入库）之后的磁盘最新内容上逐条 grep 验证。
> 归档日期 2026-11-26 23:00。

## 0. 快照与范围说明（必读）

- 复审快照：2026-11-26 22:58，HEAD = `09e4a88`（86 文件 / +9547 −4998 已入库），工作区剩 16 个未提交文件（文档与基线为主，src 零改动）。
- `09e4a88` 碰过标签栏相关文件（tab_strip.h / tab_strip_input.cpp / tab_strip_layout.cpp / tab_uia.{h,cpp}），改动内容是 **UIA provider 泛化（标签栏/书签栏共用 Traits）+ 诊断开关**，未触碰首审发现的任何一条 bug。
- 复审方式：14 条发现逐条 `grep -n` 在磁盘最新内容上复核，行号以快照时刻为准；若漂移请按符号名检索。
- 边界遵守：只读未改；本文只写文档。

## 1. 复核结论总表（14/14 全部仍存在，无一被顺手修复）

| 编号 | 严重度 | 位置（符号定位） | 复核状态 | 备注 |
|---|---|---|---|---|
| S1 | 崩溃级 | `tab_strip_input.cpp` WndProc 的 `uia::kMsgItem` 分支 | **仍在** | 只校验 `items_.size()` 不校验 `tabRects_.size()`；`Layout()` 在客户区为 0 时先 `tabRects_.clear()` 提前返回 → 最小化时 UIA 查询越界读 vector（UB）。一行修 |
| S2 | 崩溃级 | `tab_uia.cpp` `TabItemProvider::Root()`（398 行） | **仍在，且影响面扩大** | 锁内取裸指针不 AddRef、锁外 AddRef/use → UAF 竞态。**新增事实**：`09e4a88` 把 provider 泛化后书签栏（`bookmark_bar_uia.cpp`）也走同一 `uia::` 基建，同一竞态现在暴露**两份**（TabStrip 与 BookmarkBar 的根 provider） |
| M1 | 中 | `tab_set.h:76` 注释 vs `tab_set.cpp:121` `Move(id, pinned ? total-1 : total)` | **仍在** | 注释说「取消固定保持当前位置」，实现移到第一个未固定位——契约与行为相反 |
| M2 | 中 | `tab_strip_layout.cpp:146`（`scrollX_ > maxScroll_` 夹取） | **仍在** | 夹取发生在 rects 按**旧** scrollX_ 算完之后 → 关标签/缩窗错位一帧 |
| M3 | 中 | `tab_strip.cpp` SetTabs（97 行 `hoverIndex_` 夹取） | **仍在** | 只防越界不防漂移；`CloseVisible`/`HitClose` 依赖 hover → 关闭后关闭钮可能画错/命中错标签 |
| L1 | 低 | `tab_strip_layout.cpp` `EnsureVisible` | 仍在 | `ScrollBy(r.left)` 贴 0，吃掉 margin |
| L2 | 低 | `tab_strip_actions.cpp` `kTabMenuColor` 分支（117-119 行） | **仍在** | `POINT at = screen;` 死赋值 + `GetCursorPos` 覆盖 → 颜色菜单跟光标不跟被右键的标签 |
| L3 | 低 | `tab_strip.cpp` SetTabs 的 `iconCache_.size() >= 256` 裁剪 | 仍在 | 256 个 path 全在当前 refs 中时一个不清，缓存可超限继续涨 |
| L4 | 低 | `tab_strip.cpp:12` 空 `namespace { }` | 仍在 | 死代码 |
| L5 | 低 | `tab_strip_input.cpp` `UpdateHover` 末尾（347 行）` (void)0;` | 仍在 | 残留语句 |
| L6 | 低 | `tab_uia.cpp:164` `UIA_HasKeyboardFocusPropertyId` 返回 `info.selected` | 仍在 | 选中 ≠ 键盘焦点，AT 误报焦点 |
| L7 | 低 | `tab_uia.cpp` `ElementProviderFromPoint` | 仍在 | 不按 `tabViewRight_` 裁切，»/+ 遮挡下的标签仍被命中 |
| L8 | 低 | `tab_uia.cpp` TabItemProvider 以 `index_` 做身份 | 仍在 | 增删标签后旧 provider 静默指向另一个标签；理想用稳定 tab id |
| L9 | 低 | `app.cpp:693` `MessageBoxW(..., L"退出 ExBarBrowser", ...)` | 仍在 | 机械合规（两条品牌串正则都不命中）但违反「品牌串只在 identity.h」的规则精神 |

**书签栏对照**：`bookmark_bar.cpp` 的 `uia::kMsgItem` 分支读的是 `items_[i].rc`（矩形存在条目结构体内、同一次边界检查覆盖）——**S1 的跨 vector 形态只存在于标签栏**（items_ 与 tabRects_ 是两条平行 vector）。S1 修复时不要照抄到书签栏，那边没有这个问题。

## 2. 复审新增发现

1. **S2 影响面翻倍**（见总表备注）：`uia::Traits` 泛化让书签栏共享同一 `g_roots` 表与同一 `Root()` 路径。修 S2 时一处 `Root()` 改锁内 AddRef 即同时修好两栏——修法收益也翻倍。
2. **诊断开关入库**：`tab_uia.cpp` `HandleGetObject` 新增 `EXBAR_NO_UIA_PROVIDER=1` 环境开关（隔离 UIA 死锁排查用，注释自述「排查完可去掉」）——建议排障结束后移除，避免长期分叉。
3. **上轮单测偶发结论维持**：全量首跑 events_test 报「运行失败 1」，复跑「通过 46 / 失败 0」、`09e4a88` 提交记录里也是 73/73——判定为偶发（与运行中实例争用事件管道有关），非代码缺陷。

## 3. 对标调研结论归档（两路全网调研的落地精华）

### 3.1 优秀产品共性（12 条共识中本项目已具备 vs 缺失）

| 共识 | 本项目状态 |
|---|---|
| 中键关闭 | ✅ 有（WM_MBUTTONDOWN，锁定/固定标签豁免） |
| Ctrl+Tab 双模式（顺序/MRU） | ✅ 有（tab.cycle.next/prev + tab.activate.mru） |
| 撤销关闭 + 历史栈 | ✅ 有（tab.reopen / tab.closed.list，栈深 20，菜单列前 10） |
| 双击空白新建 | ❌ 刻意不做（注释写明与「+」语义重复），符合多数浏览器现状 |
| 拖拽重排 + 拖出成窗 | ⚠️ 半有：重排有，拖出仅有菜单入口 `tab.move.window`，无拖拽触发路径 |
| 固定 Pin | ✅ 有（40dp 图标化、不参与批量关闭、不画关闭钮） |
| 锁定语义 | ⚠️ 半有：只防关；TC/Opus 的「锁路径 + 切走重置时机 + 锁后导航改开新标签」三档语义缺失 |
| 溢出策略 | ✅ 等比收缩 + » 菜单 + rapid closure（连续关闭冻结宽度，Chromium 同款）已对齐 |
| 关闭钮渐进显示 | ✅ 有（激活/hover 才显示，锁定/固定豁免） |
| 悬停预览 | ❌ 只有 tooltip 路径；缩略图预览缺失（低优先） |
| 标签切换器覆盖层 | ⚠️ `tab.switcher` 已登记待决，菜单有入口 |
| 会话持久化分层 | ✅ 有（撤销栈 + 启动恢复 + 改名/颜色/锁定按路径持久化） |

### 3.2 实现层最佳实践对照（Chromium/WT 对标）

已做对（无需动）：WM_CAPTURECHANGED 拖拽复位、Esc 取消拖拽、SetCapture 前的 4dp 死区、WM_CONTEXTMENU 键盘路径、双缓冲经 ui::PaintBuffered、DPI 单一 Metrics 派生、纯函数（TabDropLineX/TabHitInView/TabNewButtonX/TabMouseTarget）可单测。

差距（按优先级）：
1. ~~**拖拽让位无动画**~~ → **已落地（2026-11-27）**：Chromium 式 ideal bounds 起止快照 + 三次 ease-out（`TabAnimDx`，纯函数可单测）+ `kAnimTimer` 每帧重绘，时长 `tabAnimMs` 当帧读。实现见 `src/tab_strip_anim.cpp`；启动点在 `SetTabs`（按 `TabStripItem::id` 配对旧左缘，顺序真变了才起表）。
2. ~~**拖出成窗缺磁吸判定**~~ → **已落地（2026-11-27）**：`TabIsTearOff(dy, height, threshold)` 纯函数实现 Chromium `kVerticalDetachMagnetism` 语义（垂直越栏底 + `tabTearOffDy` 才算脱离；阈值 0 = 关闭磁吸）。拖拽路径经 `WM_APP_TAB_TEAROFF` 转投 `tab.move.window`，与菜单/热键同一实现；脱离期间整条换成「松开：移到新窗口」提示。
3. ~~**锁后导航语义**~~ → **已落地（2026-11-27）**：锁定路径标签内点子目录会弹询问框（留在原地 / 在此标签打开），拦截点是 `IExplorerBrowserEvents::OnNavigationPending`（唯一能截住 shell 内部导航的位置，返回失败码即取消导航）。判据收进纯函数 `pathutil::IsUnder`。详见 §7.4。
4. **极窄窗口兜底**：minW 之下建议收纳下拉（WT 式）或保证选中标签始终可见（EnsureVisible 已有并留 margin，激活变化路径已覆盖）。**部分**：EnsureVisible 已修，收纳下拉未做。

## 4. 修复优先级（复审后不变，附影响面修正）

| 梯队 | 项 | 理由 |
|---|---|---|
| 第一 | S1（1 行）+ S2（约 3 行，**一并修好两栏**） | 崩溃级；触发条件（最小化 + 读屏软件、销毁竞态）窄但后果是 UB/UAF |
| 第二 | M3（两三行）→ M2（夹取后重算 rects） | 快速连续关闭下的错误命中（用户可感知）与一帧错位 |
| 第三 | M1 定夺语义（建议改注释就 Chrome 语义，不动实现免破单测） | 契约矛盾会传染给后续维护者 |
| 第四 | L2/L9/L1 等卫生项 + L6/L7/L8（UIA 精确性） | 择机清 |
| 第五 | 功能差距（拖拽动画 ✅ → 拖出成窗 ✅ → 锁导航语义 ✅ → 切换器 ✅） | 四项全落地（2026-11-27）；仅剩「极窄窗口收纳下拉」未做 |

## 5. 证据链

- 首审来源：`review/G10_tabjob.md`（S3/S4/M5/M6/M7 与本文 S1/S2/M1/M3/M2 同源编号）。
- 复核命令：`grep -n` 于 HEAD=`09e4a88` 磁盘内容（关键符号：`uia::kMsgItem`、`TabItemProvider::Root`、`scrollX_ > maxScroll_`、`hoverIndex_ >= static_cast<int>(items_.size())`、`Move(id, pinned`、`退出 ExBarBrowser`）。
- 调研产物：两份全网调研报告（浏览器/文件管理器/终端交互对照 + Win32 自绘实现检查清单）已在会话中归档，精华收进本文 §3。
- 本地验证：三道静态检查全绿（check_commands / check_ai_meta / check_components）；单测 73 个（events_test 首跑偶发失败已排除，见 §2.3）。

## 6. 修复应用记录（2026-11-27 追记）

### 6.1 首轮修复（11-26 晚）与覆盖事件

首轮修复（S1/S2/M1/M2/M3/L1-L7/L9）于 11-26 晚完成并通过全量构建与 tab 域单测，
**但当时未提交**。并行会话随后的大提交（4cc6dbb、2a484c5）覆盖了工作区中这些
未提交的改动 —— 11-27 复查发现：

- **丢失**（磁盘回到修复前）：S1、S2、M2、M3、L3、L4、L5、L6、L7。
- **存活**（已随提交入库）：
  - L9：ConfirmExit 搬家到 `src/app_commands_window.cpp` + 品牌串 `id::kBrand`（2a484c5）。
  - L2：颜色二级菜单锚定被右键标签的 screen 坐标（tab_strip_actions.cpp，随 4cc6dbb 或 2a484c5 入库）。
  - M1：tab_set.h unpin 注释改为 Chrome 语义（tab_set.h 11-26 23:21 版本，未提交但未被覆盖）。
- 修改前快照完好保留在 `backup/tabbar-20261126/`（11 个文件）。

### 6.2 二次恢复（11-27）

逐项重放丢失的修复（对照备份确认基线一致后，按当前 HEAD 重写）：

| 项 | 文件 | 内容 |
|---|---|---|
| S1 | tab_strip_input.cpp | kMsgItem 补 `tabRects_.size()` 边界检查（items_ 与 tabRects_ 独立容器） |
| L7 | tab_strip_input.cpp | 溢出时 `r.right <= 0` 的标签对 UIA 返回 FALSE（不列幽灵项） |
| L5 | tab_strip_input.cpp | 删 UpdateHover 尾部 `(void)0;` |
| S2 | tab_uia.cpp | `Root()` 锁内 AddRef 带引用返回；Navigate/get_FragmentRoot/get_SelectionContainer 去掉锁外二次 AddRef；HandleGetObject 锁内 AddRef、`UiaReturnRawElementProvider` 后 Release 借出引用 |
| L6 | tab_uia.cpp | `UIA_HasKeyboardFocusPropertyId` 改为 `info.selected && GetFocus() == hwnd_` |
| M2 | tab_strip_layout.cpp | 取宽收进唯一 `widthAt` lambda（共用机制唯一实现）；`contentEnd = margin + contentW`；scrollX_ 夹取移到排矩形**之前** |
| L1 | tab_strip_layout.cpp | EnsureVisible 左滚留 margin |
| M3 | tab_strip.cpp | SetTabs 在 Layout() **之后**按 GetCursorPos 重新命中 hover（不按下标夹取） |
| L3 | tab_strip.cpp + tab_strip.h | 图标缓存 `iconCacheGrew_` 标志：缓存新装填过且 ≥256 才 PruneIcons（没涨不白扫） |
| L4 | tab_strip.cpp | 删空匿名命名空间 |

与并行会话动画批次（tab_strip_anim.cpp / TabAnimDx / tearOff 字段）的交集核对：
动画接线只读 `tabRects_` 最终位置，`clientH_` 字段仅声明未赋值（并行会话半成品，
本次不动）；M2 重排了 Layout 内部顺序但产出契约（tabRects_/lastWidths_/maxScroll_）不变，
不冲突。

验证：`EXBAR_TEST_OUT` 隔离输出目录跑 `tests/run_all.sh tab` 2/2 PASS；
全量单测与构建另行记录（见 CHANGELOG [Unreleased]）。

> 教训（写给后续批次）：**修复完成后必须立即提交**，多会话并行时未提交的工作区
> 改动随时会被对方会话的 `git checkout`/大提交吞掉。本文件的 §1 总表在二次恢复
> 后不再反映当前状态 —— 当前状态以本节与 CHANGELOG 为准。

## 7. 第五梯队落地记录（2026-11-27）

### 7.1 落地前的真实状态（关键发现）

并行会话已写好**绘制层与动画算法**（`tab_strip_anim.cpp` 的 `StartReorderAnim` /
`AnimDx` / `DrawDragGhost`，`tab_strip_paint.cpp` 的 tear-off 提示条），但**输入层
完全没接线**，表现为三个"画了但永远不触发"的死代码：

| 成员/函数 | 落地前状态 |
|---|---|
| `dragMouse_` / `dragGrabDx_` | 只被 `DrawDragGhost` 读，**从未赋值** → 浮层恒在 (0,0) |
| `tearOff_` | 只被 paint 读，**从未赋值** → 提示条永不出现 |
| `clientH_` | 只声明，**从未赋值** → 无法判"栏底在哪" |
| `StartReorderAnim` | **零调用点** → 让位动画永不启动 |
| `kAnimTimer` | 无 `WM_TIMER` 分支 → 即使起表也不会推进/收尾 |
| `TabStripItem::id` | 字段已加但 `SetTabs` **没赋值** → 动画按 id 配对会全部落空 |

结论：这批功能的"下半截"（算法+绘制）在并行会话里，"上半截"（输入接线）是空的。
本次补的就是上半截。

### 7.2 接线内容

**拖拽浮层坐标（`tab_strip_input.cpp` / `tab_strip.cpp`）**
- `WM_LBUTTONDOWN` 命中标签时定格 `dragGrabDx_ = x - tabRects_[tab].left` 与
  `dragMouse_`。必须在按下时算：拖到边缘自动滚动会改 `tabRects_`，绘制时现算会跳位。
- `WM_MOUSEMOVE` 拖拽分支每帧更新 `dragMouse_`（自动滚动期间也跟手）。

**tear-off 磁吸判定（新增纯函数 `TabIsTearOff`）**
- 语义取 Chromium `kVerticalDetachMagnetism`：**垂直**越栏底 + 阈值才算脱离，
  横向挪出去不算。理由写进函数注释：拖拽重排时手会上下抖，按"出栏即脱离"判会让
  想换位置的人频繁误触发"移到新窗口"（那条路径会关掉源标签，代价远大于误排序）。
- `threshold <= 0` = 关闭磁吸（任何出栏都算脱离，含上方）；`height <= 0`（未布局）
  一律不脱离。
- `Layout()` 设 `clientH_`（拖拽期间不再每帧 `GetClientRect`）。

**tear-off 提交路径**
- 新增 `WM_APP_TAB_TEAROFF`（wParam=index）；`app_wnd.cpp` 转投
  `CmdId::TabMoveNewWindow`（`Actor::Ui()`）。与菜单项、热键 `t m` 共用同一份实现
  与同一条审计留痕 —— 不直调 `App` 方法。
- 判定用 `tearOff_` 而非松手瞬间现算坐标：用户看到的提示条是"松开：移到新窗口"，
  行为必须与提示一致（松手时光标可能已滑回栏内）。
- `CancelDrag` 清 `tearOff_`，避免取消后残留"移到新窗口"提示。

**让位动画启动**
- `SetTabs` 在覆盖 `items_`/`tabRects_` **之前**取旧快照（id 序 + id→left），
  仅在"规模没变"时取（增删各有既有路径，rapid close 还要冻结宽度）。
- `Layout()` 之后调 `StartReorderAnim(oldOrder, oldLeft)`；它内部再核对 id 顺序
  真的变了才起表（切标签/改色/改名都走 SetTabs，那些是纯噪声）。
- `WM_TIMER` 加 `kAnimTimer` 分支：到时长 `StopReorderAnim()`，否则
  `InvalidateRect(FALSE)` 推下一帧。结束判定放在定时器而非绘制里 —— 窗口被遮挡时
  Paint 不跑，只靠绘制自检会让定时器永远活着。
- `SetTabs` 补 `it.id = ref.id`（并行会话漏填，动画配对的唯一身份键）。

### 7.3 验证

- 新增 12 条 `TabIsTearOff` 断言（栏内/出栏/阈值边界/磁吸关闭/上方/未布局）。
- `tests/run_all.sh tab`：2/2 PASS。
- 全量构建 214 TU + 链接通过；`check_commands` 三方一致（243 条）；
  `check_ai_meta` 全绿；`check_components` 仅剩两项非本批问题
  （`settings_reg_table.cpp` 826>786 是并行会话的棘轮欠账；评估面重签已提交）。
- 目视验收待人工：拖拽换位看滑动、拖出栏体看提示条与成窗、设置 `tabTearOffDy=0`
  看磁吸关闭、`tabAnimMs=0` 看动画关闭。

### 7.4 锁后导航语义（2026-11-27 第二批）

**问题**：锁定（`tab.lock`）的语义是"这个标签钉在这个目录上"。但用户双击子目录时
shell 会直接导航 —— 锁定形同虚设，用户回来发现钉住的标签已经跑到别处。

**关键发现（决定了实现位置）**：`IExplorerBrowser` 没有 BeforeNavigate 事件，
`ShellView::Navigate()` / `DoNavigate()` 只覆盖程序发起的导航；**双击文件夹这类
shell 内部导航完全绕过它们**。唯一能拦住的位置是 `IExplorerBrowserEvents::
OnNavigationPending` —— 微软文档明确："Returning any failure code from this method,
including E_NOTIMPL, will cancel the navigation."

**实现**：
- `ShellView::SetNavGuard(fn)`：`std::function<bool(const std::wstring&)>`，
  返回 false = 拦下。未设置时一律放行（不改变既有行为）。
- `ShellView::ShouldBlockNavigation(pidl)`：把 pidl 解析成 `PidlName`（与 `path_`
  同口径，避免 `C:\A` vs `C:\A\` 形式差异导致字符串比较落空），**同目录（刷新）
  不拦** —— 否则 F5 会失效。拦下时返回 `E_ABORT` 而非 `E_NOTIMPL`：后者在日志里
  与"未实现"混在一起，排查"点了没反应"会指向错误方向。
- `App::GuardLockedNav(view, target)`：判据三条 —— ① 本窗格路径在
  `Config::s().lockedPaths` 里（查路径表而非标签镜像，镜像可能滞后）；
  ② 目标是锁定路径的**真子目录**（`pathutil::IsUnder`）；
  ③ 目标不是锁定路径本身。三条都成立才弹询问框。
- 询问用 `MB_YESNO`（默认按钮为"否"）：是 = 放行，否/关闭 = 留在原地。
  不留"本次直接导航"这第三个选项 —— 绕过锁定的正路是显式解锁，不在弹框里开后门。

**新增纯函数 `pathutil::IsUnder(child, parent)`**：真子路径判定。
核心坑是分隔符：`C:\A` 是 `C:\AB` 的字符串前缀，但不是它的父目录 ——
比较完前缀必须要求下一字符是分隔符。尾部反斜杠先统一去掉（`C:\A\` 与 `C:\A`
是同一个目录）。大小写不敏感。

**验证**：
- 新增 16 条 `IsUnder` 断言（含前缀陷阱、方向反、不同盘、盘根作父、空串、正斜杠）。
- **负向验证**：把「下一字符必须是分隔符」判据改成 `return true`，
  `path_util_test` 立即报 FAIL（"前缀陷阱：C:\AB 不是 C:\A 的子路径"）——
  证明判据不是空的。还原后复跑 PASS。

### 7.5 仍未做（下一批候选）

- **极窄窗口收纳下拉**（WT 式）：`EnsureVisible` 已修（留 margin），但窗口窄到
  minW 之下时仍只能横向滚动。
