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
1. **拖拽让位无动画**：Chromium 用 ideal bounds 起止快照 + QPC 插值（ease-out、时长恒定）；本项目是跳变。改法：记录 start/target 两份布局，WM_TIMER 每帧 lerp + `InvalidateRect(FALSE)`。
2. **拖出成窗缺磁吸判定**：垂直脱离磁吸（Chromium `kVerticalDetachMagnetism`≈15dp）+ 拖出透明度渐变反馈；`tab.move.window` 已有命令底座，只缺拖拽路径。
3. **锁后导航语义**（文件管理器特有，Explorer++/Opus 是规格来源）：锁路径标签内点子目录 → 询问开新标签。
4. **极窄窗口兜底**：minW 之下建议收纳下拉（WT 式）或保证选中标签始终可见（EnsureVisible 已有，激活变化路径需覆盖）。

## 4. 修复优先级（复审后不变，附影响面修正）

| 梯队 | 项 | 理由 |
|---|---|---|
| 第一 | S1（1 行）+ S2（约 3 行，**一并修好两栏**） | 崩溃级；触发条件（最小化 + 读屏软件、销毁竞态）窄但后果是 UB/UAF |
| 第二 | M3（两三行）→ M2（夹取后重算 rects） | 快速连续关闭下的错误命中（用户可感知）与一帧错位 |
| 第三 | M1 定夺语义（建议改注释就 Chrome 语义，不动实现免破单测） | 契约矛盾会传染给后续维护者 |
| 第四 | L2/L9/L1 等卫生项 + L6/L7/L8（UIA 精确性） | 择机清 |
| 第五 | 功能差距（拖拽动画 → 拖出成窗 → 锁导航语义 → 切换器） | 新特性，见 §3.2 |

## 5. 证据链

- 首审来源：`review/G10_tabjob.md`（S3/S4/M5/M6/M7 与本文 S1/S2/M1/M3/M2 同源编号）。
- 复核命令：`grep -n` 于 HEAD=`09e4a88` 磁盘内容（关键符号：`uia::kMsgItem`、`TabItemProvider::Root`、`scrollX_ > maxScroll_`、`hoverIndex_ >= static_cast<int>(items_.size())`、`Move(id, pinned`、`退出 ExBarBrowser`）。
- 调研产物：两份全网调研报告（浏览器/文件管理器/终端交互对照 + Win32 自绘实现检查清单）已在会话中归档，精华收进本文 §3。
- 本地验证：三道静态检查全绿（check_commands / check_ai_meta / check_components）；单测 73 个（events_test 首跑偶发失败已排除，见 §2.3）。
