# G9_ui 组代码审查报告

- 审查范围：`review/G9_ui.txt` 列出的 40 个文件（theme / ui_draw / icon* / sys_icon / menu* / popup* / prompt / modal / split_bar / side_layout / pane_split / status_bar / text_file / tooltip_host / view_modes / view_stats）
- 审查方式：逐文件通读（>2000 行分页读完），逐函数逐分支；**每条结论的行号都用 `grep -n` 在磁盘最新内容上复核过**，不是凭记忆引用
- 边界遵守：只读 `src/`；未构建、未杀进程、未改源码；只写本文件
- 快照：2026-11-23 21:32，本组 40 文件合计 **6454 行**

> ⚠️ **审查期间 `src/` 被并发改动**：会话开始时本组为 6464 行，中途 `src/ui_draw.cpp` 308→287、`src/ui_draw.h` 159→177（新增 `ui::ScopedFont`、删掉了 `ShiftPixels`）、`src/theme.cpp` 278→271（删掉 `ApplyProcessDarkMode` 里的 GetTickCount64 计时块）。已按**磁盘最新内容**重新读并复核，本报告所有行号以快照时刻为准；若后续行号漂移，请按关键字检索。

---

## 一、文件覆盖清单（40/40，无一抽样）

| # | 文件 | 行数 | 结论 |
|---|------|-----:|------|
| 1 | src/icon_glyph.cpp | 101 | 通过（2 低：线性查表 + 静默兜底字形） |
| 2 | src/icon_glyph.h | 54 | 通过 |
| 3 | src/icon_resolve.cpp | 360 | 通过（2 低：缓存无上限、DefaultIcon 截断） |
| 4 | src/icon_resolve.h | 68 | 通过 |
| 5 | src/menu.cpp | 144 | 通过（1 中：构建期同步解析图标；1 低：死 API） |
| 6 | src/menu.h | 181 | 通过（设计说明与实现一致） |
| 7 | src/menu_context.cpp | 178 | 通过（1 中：O(n²) 量宽） |
| 8 | src/menu_context.h | 67 | 通过 |
| 9 | src/modal.cpp | 42 | 通过（1 低：`gm` 未初始化读取路径） |
| 10 | src/modal.h | 54 | 通过 |
| 11 | src/pane_split.cpp | 99 | 通过（1 低：注释与负宽矩形不符） |
| 12 | src/pane_split.h | 63 | 通过 |
| 13 | src/popup_list.cpp | 844 | **有风险**（1 高：模态期资源失效；2 中；4 低） |
| 14 | src/popup_list.h | 182 | 通过（1 低：注释引用不存在的 API 名） |
| 15 | src/popup_list_draw.cpp | 195 | 通过（1 低：树模式量宽少 16dp） |
| 16 | src/popup_list_draw.h | 64 | 通过（文件头行数描述已过期） |
| 17 | src/popup_toggle.cpp | 87 | 通过 |
| 18 | src/popup_toggle.h | 97 | 通过（1 低：注释与实现漂移） |
| 19 | src/prompt.cpp | 264 | 通过（2 中：键盘可达性；2 低：死分支、双份重复） |
| 20 | src/prompt.h | 16 | 通过 |
| 21 | src/side_layout.cpp | 62 | 有条件通过（1 低：整块死代码） |
| 22 | src/side_layout.h | 51 | 通过 |
| 23 | src/split_bar.cpp | 284 | 通过（2 低：CS_DBLCLKS 无处理、1px 不缩放） |
| 24 | src/split_bar.h | 129 | 通过 |
| 25 | src/status_bar.cpp | 375 | 通过（2 中：溢出重叠、右键 target 不一致；2 低） |
| 26 | src/status_bar.h | 106 | 通过 |
| 27 | src/sys_icon.cpp | 74 | 通过（与 S1 关联：Invalidate 时机） |
| 28 | src/sys_icon.h | 45 | 通过 |
| 29 | src/text_file.cpp | 265 | 通过（3 低） |
| 30 | src/text_file.h | 69 | 通过 |
| 31 | src/theme.cpp | 271 | 有小问题（1 中：死画刷；3 低：注释×2、缺 static_assert） |
| 32 | src/theme.h | 228 | 有小问题（1 中：`radiusPopup` 死令牌；死字段×3） |
| 33 | src/tooltip_host.cpp | 107 | 通过（1 低：Attach 换 owner） |
| 34 | src/tooltip_host.h | 64 | 通过 |
| 35 | src/ui_draw.cpp | 287 | 通过（2 低：位图泄漏路径、字体池只增） |
| 36 | src/ui_draw.h | 177 | 通过（新增 `ScopedFont` 已复核） |
| 37 | src/ui_palette.h | 138 | 通过（唯一令牌表，设计合规） |
| 38 | src/view_modes.h | 93 | 通过 |
| 39 | src/view_stats.cpp | 378 | 通过（2 低：双份 8000、列数夹 8） |
| 40 | src/view_stats.h | 91 | 通过 |

合计：**严重 1 / 中 11 / 低 24**。

---

## 二、严重（S1：可致崩溃或视觉永久错乱）

### S1 `popup_list.cpp:754` — 模态弹层开着时收到主题/DPI 变更，弹层借用的 HICON / HFONT 被销毁

- 位置与链路（已逐段确认）：
  - `popup_list.cpp:751-757` 的模态泵 `while (!st.done && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) { TranslateMessage; DispatchMessageW; }` —— **对全部消息一视同仁**，含 `WM_SETTINGCHANGE` / `WM_THEMECHANGED` / `WM_DPICHANGED`。
  - `app_wnd.cpp:192-203`（SETTINGCHANGE）与 `app_wnd.cpp:206-208`（THEMECHANGED）→ `App::ApplyTheme`（`app_layout.cpp:302`）→ `statusBar_.ReloadTheme()`（`app_layout.cpp:322`）→ **`status_bar.cpp:70` `sysicon::Invalidate()`** → `sys_icon.cpp:65-72` 对缓存句柄逐个 `DestroyIcon`。
  - 而状态栏右键菜单的行图标正是借用这些句柄：`app_commands.cpp:465` `menu.AddSystemIcon(kSegNames[i], on, i, sysicon::Get(StatusSegmentSysId(i)), …)`，`menu.AddSystemIcon` 把 HICON 直接存进 `PopupRow::icon`（`menu.cpp:41-49`，注释明确"借用，菜单不销毁"）。菜单在 `app_commands.cpp:475` 进入模态。
  - 于是：**菜单开着 → 系统切深浅色（或高对比度）→ 弹层继续用已 DestroyIcon 的句柄绘制**（`popup_list_draw.cpp:85-87` `ui::DrawIconCentered` → `DrawIconEx`）。轻则该行图标消失，重则 GDI 句柄复用后画到别的对象上/访问违例。
  - 同一路径还有第二个坑：`theme_.Reload` 在 DPI 变化时删字体（`theme.cpp:204-209`），而弹层的 LISTBOX 在 `popup_list.cpp:678` 用 `WM_SETFONT` 挂过一次字体句柄，之后不再重设 → 后续 `WM_DRAWITEM` 可能把已删除的 HFONT 选进 DC。
- 可达性：菜单开着时按 Win+ 切系统主题、或把窗口拖到另一块不同缩放比例的屏，都是日常操作；不需要脚本或调试器。
- 修复（二选一，建议都做）：
  1. 弹层模态泵过滤主题/DPI 消息：在 `RunModal` 的循环里对 `WM_SETTINGCHANGE`（`l` 为 `ImmersiveColorSet`/`HighContrast`/`WindowsThemeElement`）、`WM_THEMECHANGED`、`WM_DPICHANGED` 只置 `st.themeChanged = true` 并 `continue`，循环结束后 `InvalidateRect(owner)` 让主窗口自己去处理（消息不会丢语义，只是延后一帧）。
  2. `status_bar.cpp:70` 的 `sysicon::Invalidate()` 加守卫：`if (modal::Depth() == 0) sysicon::Invalidate();`（`modal.h:41` 的 `Depth()` 就是为这类"模态期间别动共享资源"准备的，弹层也已经在 `popup_list.cpp:751` 登记了 `modal::Scope`）。同理，任何 `ico::ClearCache()` 也应加同一守卫。

---

## 三、中（S2：功能/视觉可复现缺陷，或明显性能/资源浪费）

### S2-1 `status_bar.cpp:167-308`（`Layout`）— 段丢弃链耗尽后无兜底，左右两组会重叠

- 现状：`kDropOrder`（`status_bar.cpp:229`）依次丢掉 Type/Eye/Disk/Size/Date；`for (int guard = 0; guard <= Seg_Count; ++guard)`（`:232`）在"无可丢"时 `break`（`:244`）。此后**没有**任何"进一步压缩"或"拒绝右组"的分支：左段按 `x += w + gap`（`:267`）直接向右排，右组按 `rc.right - pad` 回填（`:297-303`）。
- 后果：窄窗口（例如 720px）+ 长目录名 + 全段开启时，左组宽度总和（已无段可丢）会越过右组的左边界 → 两组文字互相压字；`Paint`（`:345-369`）按 `layout_` 原样画，没有相交裁剪。
- 修复：丢弃链结束后若 `leftW + rightW + (showPath ? S(120) : 0) > avail`，再走一级降级（右组只留图标、或把 `Seg_Chk/Seg_Date` 也纳入丢弃链），并在 `Layout` 末尾对 `layout_` 做一次"右组左边界不得小于左组右边界"的夹取；`app_stats`/单测加一条"极窄窗口不重叠"的断言（`side_layout`/`pane_split` 都有纯函数单测，这里缺）。

### S2-2 `popup_list.cpp:641-651`（`RunModal`）— 高度夹进工作区，宽度没夹

- 现状：`if (height > workH) height = workH;`（`:641`）有；宽度只在 `:645-646` 做 `x` 的回退夹取：`if (x + width > mi.rcWork.right) x = rcWork.right - width; if (x < rcWork.left) x = rcWork.left;`。
- 后果：`width > 工作区宽`（`PopupMeasureWidth` 上限是 `theme.S(460)`，但 `exactWidth` 分支（`popup_list_draw.cpp:188-189`）直接用调用方给的 `minWidthPx`；多显示器 + 窄副屏时 460dp@200% = 920px 已可超）时，`x` 夹到 `rcWork.left`，右半截落在屏幕外，且**滚动条也在屏幕外**——正是 `:634-639` 注释里描述的同一类坑（那条注释只解决了高度）。
- 修复：与高度对称地加 `if (width > workW) width = workW;`（放在算 `x` 之前），同时让 `PopupMeasureWidth` 的 `exactWidth` 分支也受 `workW` 约束。

### S2-3 `status_bar.cpp:137` — 右键与左键报的 target 不一致（`MT_STATUS` vs `MT_STATUS_SEG`）

- 现状：`WM_LBUTTONDOWN`/`WM_RBUTTONDOWN`/`WM_MBUTTONDOWN` 分支（`:118-120`）按 `HitSegment(...) >= 0` 报 `MT_STATUS_SEG`，`WM_LBUTTONUP`（`:128`）同样按段报；但 `WM_RBUTTONUP`（`:137`）**恒报 `MT_STATUS`**，不看段。
- 后果：`mouse.json` 里按"状态栏段"配的右键规则永远命中不到（`:117` 的注释声称已修好，实际只修了左键/中键）；且同一次物理右键的 DOWN 与 UP 带不同 target，依赖"DOWN+UP 成对"的规则会失效。
- 修复：`WM_RBUTTONUP` 改为 `HitSegment(pt.x, pt.y) >= 0 ? MT_STATUS_SEG : MT_STATUS`（与左键同一条表达式，抽成局部 lambda 免得又漂一次）。

### S2-4 `menu.cpp:51-59`（`Menu::AddPath`）— 菜单构建期同步解析图标，长菜单首开可感知卡顿

- 现状：每个路径行在 `Add` 时就 `ico::Cached(path, true)`（`:55`）；冷缓存下每条会走 `icon_resolve.cpp:205-266` 的完整链路（`SHParseDisplayName` → `SHGetFileInfoW(PIDL)` → 兜底注册表 → stock）。
- 后果：历史下拉/书签菜单 20+ 行 = 20+ 次 shell 解析，全部在 UI 线程、在"点击到弹出"之间；SSD 上约 1~3ms/条，机械盘或网络路径可达几十 ms/条 → 菜单"点了半秒才出来"。`Cached` 只在同一次会话内缓解。
- 修复（择一）：(a) 先用 `ico::IconId::Folder`/同类字形占位，弹层画出后由空闲回调替换；(b) `Menu` 只存 path，图标在 `Show` 里逐行取并在后台线程预热（`ico::Cached` 需要加锁或改 per-thread）；(c) 至少把这条路收进启动期预热（历史/书签读盘后批量 `ico::Cached`）。

### S2-5 `menu_context.cpp:123-137`（`OnMeasureItem`）— 每行都按整表量一次宽，O(n²)

- 现状：`item->itemWidth = PopupMeasureWidth(nullptr, *theme_, &drawRows_, nullptr, …, false, 0)` —— 对**每一行**都遍历整表 + 每行各 `MeasureText`（`popup_list_draw.cpp:160-172`）。22 行菜单 = 约 484 次 `GetTextExtentPoint32W`，且每次都 `GetDC(nullptr)`/`ReleaseDC`。
- 后果：右键菜单"弹出到可见"多几十 ms；属纯浪费（整表宽度在同一轮里是常量）。
- 修复：`QueryContextMenu` 里算一次存成员 `itemW_`，`OnMeasureItem` 直接用它（`QueryContextMenu` 与 `MEASUREITEM` 之间 `drawRows_` 不变，安全）。

### S2-6 `status_bar.cpp:307`（`Layout` → `RebuildTooltip`）— 每次刷新全量重建全部 tool，`MoveTools` 却没用

- 现状：`SetModel`（`:51-57`）→ `Layout` → `RebuildTooltip`（`:310-323`）每次都 `tooltip_.Attach` + `tooltip_.SetTools(items)`，而 `SetTools` 内部先 `Clear()`（`tooltip_host.cpp:83`）→ 逐个 `TTM_DELTOOLW`，再逐个 `TTM_ADDTOOLW`（`:70-104`）。段数 7~9 → 每次 14~18 次跨窗口消息。
- 后果：状态栏模型在选文件/切目录/定时器里频繁更新 → 每秒数十次重建；而 `TooltipHost::MoveTools`（`tooltip_host.h:46`，已由 `tab_strip_layout.cpp:197` 使用）正是为这种"文本没变、只有矩形变"的场合准备的。
- 修复：缓存上一次的 `(seg, text)` 列表；文本没变就只 `MoveTools(rects)`，变了才 `SetTools`。

### S2-7 `prompt.cpp:162-173` / `prompt.cpp:233-252` — 键盘可达性：ESC 只在焦点位于 EDIT 时生效

- 现状：`onKey` 过滤条件是 `msg.hwnd != st.edit`（单字段版 `:163`）/`(msg.hwnd != st.edit && msg.hwnd != st.edit2)`（双字段版 `:234`）。焦点一旦落到"确定/取消"按钮（用户拖动窗口、或点了按钮又改主意、或 Tab 到按钮），ESC 与 Enter 都不再被拦截，会走 `DefWindowProc`——`WS_SYSMENU` 下 ALT+F4 可用，但**ESC 取消/Enter 确认失效**。
- 顺带：单字段版**完全没有** Tab 处理（`:162-173` 只处理 RETURN/ESCAPE），双字段版的 Tab（`:244-250`）只在两个 EDIT 之间来回，永远到不了按钮；两版都没有 `IsDialogMessage` 或 Tab 序。
- 修复：`onKey` 的判定改成"消息属于本对话框（`msg.hwnd == hwnd` 或其子窗口）"而不是枚举具体控件；Tab 用 `GetNextDlgTabItem` 或干脆在循环里调 `IsDialogMessageW(hwnd, &msg)`（它自带 Tab/Enter/ESC 语义，`modal.h:23-24` 的 `onKey` 约定也允许）。

### S2-8 `theme.h:69` / `theme.h:202` — `radiusPopup` 是死令牌，且与弹层现实矛盾

- 现状：`radiusPopup96 = 6` + `int radiusPopup() const`，全工程**零调用**（`grep -rn "radiusPopup" src/` 只命中 theme.h 自身）。而弹层实际是**直角**：`popup_list.cpp:547-565` 的 `WM_NCPAINT` 用四条 `DrawSeparator` 画 1px 矩形边框，没有 `FillRound`，也没有 `SetWindowRgn`。
- 后果：`theme.h:66-67` 的注释（"弹层圆角统一到这里"）给人"弹层是圆角"的错觉；后来人照 `radiusPopup()` 去改绘制会发现改了没效果（无人消费），或误以为别处在画圆角。
- 修复：要么删掉该令牌（视觉来源唯一 = 不留无人消费的承诺），要么在 `WM_NCPAINT` 里真的落地圆角（`SetWindowRgn` + `FillRound` 底板），并同步删除"直角四条线"。

### S2-9 `theme.cpp:143` / `:148` / `:149` + `theme.h:171` / `:176` / `:177` — 三个画刷建了、删了、从没用过

- 现状：`brPressed`、`brSelSolid`、`brAccent` 在 `Theme::Init` 创建、在 `Theme::Shutdown` 删除，全工程 `src/` 内**零使用**（`grep -rn "brPressed|brSelSolid|brAccent" src/` 只命中 theme.cpp 自身）。
- 影响：每次 `Reload`（切主题/DPI）多 3 次 `CreateSolidBrush` + 3 次 `DeleteObject`；且与 `ui_draw.cpp:118-122` 的 `FillRectColor`（每次调用临时建刷/删刷）形成"有刷不用、用时现建"的反向浪费——`popup_list_draw.cpp:58/61`、`status_bar.cpp:349/373`、`split_bar.cpp:283` 都在高频路径上现建刷。
- 修复：删除三个死字段/死刷；反过来让 `FillRectColor` 增加一个"借用主题刷"的重载（或让弹层/状态栏直接用 `brSelSolid`/`brAccent` 而非 `FillRectColor`），把高频 Create/Delete 消掉。注意 `selSolid` **颜色**仍被 `sequence_overlay_paint.cpp:89` 使用，删的是刷不是色。

### S2-10 `modal.cpp:15` / `:22` — `gm` 存在"未初始化即读"的路径

- 现状：`int gm; while (!done() && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {…}`，随后 `if (gm == 0) PostQuitMessage(...)`。若首拍 `done()` 已为真，循环体与赋值都不执行，`gm` 是未初始化值 → 读它是 UB（`popup_list.cpp:753-758` 的 `int gm;` 同型）。
- 可达性：当前所有调用点都在建窗后才进 `Run`，`done()` 首拍为假，所以是**潜在**问题；但这是"复制粘贴就中招"的模式，而 `modal.h:8-12` 正是为了消灭这类模式才存在的。
- 修复：`int gm = 1;`（或 `bool quit = false`）。

### S2-11 `prompt.cpp` 结构 — 两个函数约 100 行逐字重复

- 现状：`PromptText`（`:122-184`）与 `PromptText2`（`:186-264`）在窗口创建、控件摆放、`ApplyDarkTitleBar`、`EnableWindow`、`modal::Run`、焦点回还、`DestroyWindow` 上完全重复；差异只有"一个编辑框 vs 两个"和 Tab 处理。
- 后果：改一处（例如 S2-7 的键盘修复、或加"必填校验"）必须记得改两处；`PromptState.edit2` 的存在说明作者已按"两版合一"设计过状态结构，却没合 UI 代码。
- 修复：内部 `PromptImpl(owner, theme, title, {label1, initial1}, {label2, initial2}, out1, out2)`，两版变薄壳。

---

## 四、低（S3：注释不符 / 死代码 / 防御性缺口 / 一致性）

### 注释与实现不符（维度 7）

| 位置 | 问题 | 修复 |
|------|------|------|
| `theme.cpp:93-94` | 注释写"深色文件区 #141414、工具栏 #1e1e1e；浅色 #ffffff、工具栏 #f3f3f3"，而代码 `:105/:110` 两者都取 `T.viewBg`（深 `#191919`、浅 `#ffffff`）。这条注释描述的正是 `:106-109` 注释里说"已废弃"的旧口径，两段注释自相矛盾。 | 删掉 `:93-94` 的旧色值清单，只留"色值唯一来源 = ui_palette.h"。 |
| `theme.cpp:256` | "填充只上 22%" ，代码 `Mix(bar, c, dark ? 26 : 18)` —— 两个数字都不是 22。 | 改成"深 26% / 浅 18%（22% 是初值，实测深色 26 更易辨识）"或把数字统一。 |
| `popup_list.h:66` | "见 menu.h 的 `SetShortcutProvider`" —— 该名字不存在，实际是 `SetShortcutResolver`（`menu.h:147`）。 | 改名。 |
| `popup_toggle.h:22-27` | 描述"弹层因外部点击而失活时…先 `SetCapture` 到自己，吞掉紧随其后的那一次 DOWN/UP"。实际实现是**开窗时就 `SetCapture`**（`popup_list.cpp:744`），`WM_ACTIVATE`（`:592-612`）里只做 `ShouldCloseOnDeactivate` 判定，没有任何再捕获。 | 把这段时序图改成"开窗即持有捕获；失活时按'是否仍持有捕获 + 前台是否同线程'决定关不关"。 |
| `popup_list_draw.h:5` | 文件头称"它让 popup_list.cpp 冲到 974 行" —— 当前 `popup_list.cpp` 是 844 行（拆分后值），974 是拆分前的历史值 | 改成"拆分前 974 行，拆分后 844"或删数字。 |
| `pane_split.cpp:78-81` | 注释称极窄窗格区下"两栏各为 0"。实际 `gap > avail → gap = avail` 后 `ClampCenter` 返回 `avail/2`（`:47`），于是 `pane0` 宽 `avail/2 - gap/2`、`pane1 = {s1, …, inner.right}` 中 `s1 > inner.right` → **负宽矩形**（不是 0）。 | 要么在 `avail <= gap` 时直接把两栏置成 0 宽（`g.pane0 = {inner.left, …, inner.left, …}`），要么改注释。 |

### 死代码 / 未消费 API

| 位置 | 问题 | 修复 |
|------|------|------|
| `side_layout.cpp:24-31` | 整块是死代码：`:21-22` 之后不变量已是 `totalW - nav - ai >= kMinFileAreaPx`，所以 `:27-28` 的 `std::min` 永远不会生效；`:16-17` 已保证 `nav >= kNavPaneWidthMin`、`ai >= kAiPanelWidthMin`，所以 `:30-31` 也永不触发。注释描述的"压到刚好留住文件区"实际不会发生。另外这两行 `min` 缺 `std::max(0, …)` —— 一旦未来放宽前面条件，会写出**负宽度**（`Solve` 里 `out.fileLo = out.navW` 直接把负值传给布局）。 | 删掉 `:24-31`；若要保留防御，写成 `nav = std::max(0, std::min(nav, totalW - ai - kMinFileAreaPx))` 并更新注释。 |
| `menu.cpp:96-106` | `Menu::Shortcut(row, spec)` / `Menu::Key(spec)`（`menu.h:134-135` 声明）全工程零调用 —— "例外通道"目前没有使用者。 | 保留可以，但注明"暂无调用点，留给无 CommandDef 的局部动作"；或删掉（`menu.h:39-43` 已把口径讲清，删 API 不影响文档价值）。 |
| `prompt.cpp:77-84` | `WM_CTLCOLORSTATIC` 分支：本对话框**没有任何 static 子控件**（标签是 `WM_PAINT` 自绘，`:93-107`），分支不可达。若将来加 static，它返回 `brEdit`（`#2A2A2A`）作背景，会在 `brBar`（`#191919`）的窗体上画出一块更亮的方块。 | 删掉 `WM_CTLCOLORSTATIC`（保留 `WM_CTLCOLOREDIT`）；加 static 时再按 `brBar` 单独处理。 |

### 资源 / 防御缺口

| 位置 | 问题 | 修复 |
|------|------|------|
| `ui_draw.cpp:272-283` | `PaintBuffered`：`bmp` 创建成功但 `SelectObject(mem, bmp)` 失败（`mem` 为 NULL，`CreateCompatibleDC` 失败）时，`bmp` 只在 `if (bmp && oldBmp)` 里被 `DeleteObject` → 该分支下**位图泄漏**（每次 WM_PAINT 一个）。 | 把 `DeleteObject(bmp)` 移到 `if` 外（`if (bmp) DeleteObject(bmp);`），或直接 `if (!mem || !bmp) { 清理并 EndPaint; return; }` 早退。 |
| `ui_draw.cpp:47-59` | `GlyphFace()` 未判 `GetDC(nullptr)` 是否为 NULL 就 `SelectObject`/`GetTextFaceW`（虽然 NULL HDC 下这些调用只会返回 0，不会崩，但 `probe` 会被永久定成 0，之后全进程退 MDL2）。 | `if (!dc) return L"Segoe MDL2 Assets";`（不写 `probe`，下次重试）。 |
| `ui_draw.cpp:16-19` | `FontPool()` 进程级只增不减；DPI 变化 + 多档图标尺寸会累积几十个 `HFONT`，进程退出才释放（无 `DeleteObject` 路径）。 | 接受现状可以，但请在 `ui_draw.h` 的"字体池"注释里写明"进程级，不回收"（当前只说"不对外"）。 |
| `tooltip_host.cpp:44-54` | `Attach(owner, …)` 只更新 `owner_`，已存在 `tip_` 时不重建；若换了 owner，先前 `TTM_ADDTOOLW` 登记的是**旧** hwnd，而 `Clear()`/`MoveTools()` 用**新** `owner_` 拼 `TTTOOLINFO` → `TTM_DELTOOLW` 删不掉旧工具（残留在旧窗口上 + 计数泄漏）。 | `Attach` 里 `if (owner_ && owner_ != owner) Reset();` 后再建。 |
| `icon_resolve.cpp:278-307` | `IconCache()` 无上限、无淘汰；`ClearCache()` 会 `DestroyIcon` 全部句柄——若在弹层打开期间被调用同一 S1 的悬空问题（当前只在 `main.cpp:219/226` 退出路径调用，风险低，但 API 本身不安全）。 | 文档标注"仅可在无弹层/无绘制时调用"，或与 S1 的 `modal::Depth()` 守卫共用。 |
| `icon_resolve.cpp:184-198` | `LoadDefaultIconForGuid` 用 `wchar_t val[MAX_PATH]` + `DWORD vlen = sizeof(val)`，注册表值超过 259 字符会被**静默截断**（图标路径断了就取不到图标，落到 stock）。 | 截断时 `val` 仍以 NUL 结尾（安全），但建议用 `RegQueryValueExW` 两段式（先取长度）或 `std::wstring` + `MAX_PATH*2`。 |
| `text_file.cpp:171-181` | `AppendLine` 的 `static int sinceCheck` 是**跨 path 共享**的（注释承认），于是 `log.cpp`（每行）与 `CommandBus::Audit`（每 200 行）互相消耗对方的计数 → 实际检查频率不确定；且非线程安全（若审计日志将来从工作线程写就会花）。 | 改成 `static std::unordered_map<std::wstring, int>`（或把计数交给调用方持有），并在注释里去掉"差几次不影响正确性"的妥协说明。 |
| `text_file.cpp:243-250` | `ExeDir()` 用 `wchar_t buf[MAX_PATH*2]`（520）；`GetModuleFileNameW` 在超长安装路径下会截断并按"缓冲区满"返回 520，随后 `s(buf,n)` 得到半截路径，`find_last_of('\\')` 截出的目录是错的。 | 用 `MAX_PATH*8` 或两段式（先 `GetModuleFileNameW(nullptr, nullptr, 0)` 拿长度——Win10 1511+ 支持）；至少判断 `n == _countof(buf)` 时返回空。 |
| `text_file.cpp:28` | `ReadAllBytes` 有 64MB 硬上限（合理），但 `text_file.h` 完全没写这条契约 —— 调用方读到 `false` 只会以为是 IO 失败。 | 在 `ReadAllBytes`/`ReadText` 的头注释里写明"超过 64MB 返回 false"。 |
| `split_bar.cpp:259-262` | `theme_ ? theme_->bg : GetSysColor(COLOR_WINDOW)` / `GetSysColor(COLOR_3DSHADOW)` —— 系统色构成第二视觉来源。虽然 `Create()` 必定设 `theme_`（`split_bar.cpp:21`），永不走到兜底，但代码里留着就是"第二条路"。 | 改成 `if (!theme_) return;` 早退（或断言），删掉 `GetSysColor` 兜底。 |
| `theme.cpp:265-271` | `TabColorName` 的 `names[kTabColorCount]` 只初始化 8 项；`kTabColorCount` 来自 `core::kTabColorCount`（`tab_set.h:20` = 8）。一旦该常量改成 9，`names[8]` 为 `nullptr` 且 `:269` 的越界检查放行 → 返回 `nullptr` → 调用方 `std::wstring(label)` 用空指针构造 = UB。 | 加 `static_assert(kTabColorCount == 8, "TabColorName 的表要跟着改");` 并把数组声明成 `std::array<const wchar_t*, 8>`。 |
| `icon_glyph.cpp:87-92` | `Glyph()` 对每个图标做一次线性查表（约 45 项），而 `DrawGlyph` 的调用频率是"每行每图标每帧"。另：未登记的 id 返回放大镜（`kFallback`），新增 `IconId` 忘了填表时不会报错，只会**静默画成放大镜**。 | `IconId` 是连续枚举，可改成按 id 索引的 `constexpr wchar_t kGlyphById[]` + `static_assert(sizeof…== 项数)`；调试版对未命中 id 加 `LogDbg`/断言。 |
| `status_bar.cpp:44` | 注册了 `CS_DBLCLKS`，并把 `WM_LBUTTONDBLCLK` 当一次 `MBTN_LEFT` DOWN 上报（`:102-112`），于是双击一次段会向 `MouseSink` 报两次 DOWN、一次 UP。 | 明确语义：要么不注册 `CS_DBLCLKS`（双击 = 两次完整 DOWN/UP），要么把 DBLCLK 上报成独立事件。 |
| `view_stats.cpp:147` | 局部 `const int kSplitCap = 8000;` 与 `view_stats.h:82` 的 `constexpr unsigned kStatsSplitCap = 8000` 是同一个数的两份（后者被 `app_stats.cpp:47` 用）。 | 直接用 `kStatsSplitCap`，删掉局部常量。 |
| `view_stats.cpp:307-318` | `GetSortColumnCount` 的结果被 `std::min(cols, 8)` 夹住；若用户按 9 列以上排序、且日期列排在 9 位之后，`dateSort` 会静默为 0（状态栏日期段的升降序箭头消失）。 | 至少 `LogDbg` 一次"列数被截断"；或改成只取前 8 列 + 明确注释影响面。 |
| `popup_list.cpp:153` | `ResizeToFit` 内 `const int maxVis = 20;` 与 `PopupState::maxRows`（`:48`）、`ShowFolderTree`（`:837` 的 `std::min<int>(topCount, 20)`）是**三份 20**。 | 统一用 `st->maxRows`。 |
| `popup_list_draw.cpp:148-159` | 树模式的量宽只加了 `n.depth * S(16)`，没有算 `DrawTreeRow` 实际占用：左边距 `S(6)`（`:21`）+ 图标槽 + `S(4)`（`:29`）+ 右侧给 chevron 留的 `S(26)`（`:30`）。列表模式的统一尾项只有 `iconS + S(8) + S(8) + S(10)` → 树弹层比实际需要窄约 16dp，长目录名会提前省略。 | 树分支单独加一项 `S(26)`（或把"行内几何"抽成一个纯函数，两边共用——现在 `popup_list_draw.cpp:178-186` 的注释已经把公式写死在这里，正是以后会漂的地方）。 |
| `popup_list.cpp:555-562` + `split_bar.cpp:271-276` | 静止态的 1px 分隔/边框是**物理像素**（`wndW-1`、`x+1`），未过 `S()`；其余所有间距都过 `S()`。200% DPI 下这条线相对更细，与"DPI 全量缩放"的口径不一致。 | 用 `std::max(1, theme->S(1))` 计算线宽，`S(1)` 在 96dpi = 1，在 200% = 2。 |

### 一致性 / 风格

- **间距魔数系统性存在**：本组 6 个绘制文件里 `S(字面量)` 共 28 处（`S(16)`×7、`S(8)`×5、`S(4)`×4、`S(120)`×2、`S(12)`×2，其余 1 次），全部过了 DPI，但**没有 spacing 令牌**（Theme 只有栏高/圆角/图标四档）。后果：同一个"图标后间距"在 `popup_list_draw.cpp:95` 是 `S(8)`、在 `status_bar.cpp:358` 是 `S(4)`、在 `popup_list_draw.cpp:82` 是 `S(4)`。建议像 `iconChevron96` 那样补 `space1/2/3`（4/8/12）三档，先在弹层+状态栏落地。
- `popup_list.cpp:543`/`prompt.cpp:92` 用 `FillRect` + 主题刷，其余绘制用 `ui::FillRectColor` + 颜色值 —— 两条路并存（配合 S2-9 一起收）。
- 弹层的双缓冲靠 `WS_EX_COMPOSITED`（`popup_list.cpp:656`），没有走 `ui::PaintBuffered`；`prompt.cpp:86-111` 也直接 `BeginPaint`。前者有注释解释，后者（纯色填充 + 一行文字）可接受，但两者都不在"双缓冲唯一实现"的口径里 —— 建议在 `ui_draw.h` 的 `PaintBuffered` 注释里点明"轻量单窗口可不用"的例外条件，否则下次评审会当违规报。

---

## 五、按维度的结论

**1. 视觉来源唯一（通过，2 条例外）**
本组 40 个文件**无一处硬编码颜色**——`grep -rn "RGB(" ` 只命中三处合法点：`ui_palette.h`（令牌表本体）、`theme.cpp:245`（`Mix` 的 `RGB()` 组装）、`ui_draw.cpp:248-249`（WCAG 兜底的纯黑/纯白，不属于调色）。**无一处自建字体**：`CreateFontW/IndirectW` 只在 `ui_draw.cpp:39`（字形字体池）与 `theme.cpp:169-183`（正文/等宽）。字号/栏高/圆角/图标尺寸全部走 `Theme` 成员或 `S()`。例外两条已报：`split_bar.cpp:259-262` 的 `GetSysColor` 兜底（不可达但存在），`theme.h:69` 的 `radiusPopup`（无人消费的承诺）。

**2. 绘制（GDI 卫生通过；两处资源/缩放缺口）**
本组除 `ui_draw.cpp` 外**零** `CreateSolidBrush/CreatePen/CreateFont/CreateCompatibleDC`（grep 证实）；`DrawTextRow`/`MeasureText`/`MeasureTextHeight` 全部"选字体→无条件还原"（`ui_draw.cpp:190-194`、`:201-205`），`DrawGlyph` 还额外 `SaveDC/RestoreDC`（`:66/:73`）。双缓冲：`split_bar`/`status_bar` 走 `ui::PaintBuffered`，弹层走 `WS_EX_COMPOSITED`，`prompt` 直画（可接受）。缺口：`PaintBuffered` 的位图泄漏路径（`:272-283`）、1px 边框未缩放（`popup_list.cpp:555-562`、`split_bar.cpp:271-276`）。裁剪区：`PaintBuffered` 不读 `ps.rcPaint`（全客户区重绘），本组窗口都是窄条，可接受。

**3. popup_list（核心状态机质量高，1 高危 + 2 中）**
- 命中：`LB_ITEMFROMPOINT`（`:98-102`）+ `PopupExpanderRect`（`popup_list_draw.h:55-64`）"画与点共用同一矩形"，是本组最扎实的设计之一。
- 溢出：**高度**夹进 `rcWork` 且顺序正确（`:634-641`，注释解释了"先夹高度再算 y"的坑）；**宽度**没夹（S2-2）。
- 模态消息泵：`GetMessageW` 三分支（`>0/0/-1`）处理正确（`:754-758`），`modal::Scope` 正确登记（`:751`）；但泵对主题/DPI 消息无防护（S1）。
- 焦点：`SetFocus(st.list)`（`:731`）+ 捕获模型（`:744`）——`SetForegroundWindow`（`:730`）**确认是 UI 触发**：`ShowPopupList`/`ShowFolderTree` 全工程只被 `ui::Menu::Show`（`menu.cpp:137`）与 `folder_tree.cpp:31` 调用，全部由鼠标/键盘交互进入，无脚本、无 AI 命令直接调用路径。属 src 内允许范围。

**4. menu / menu_context（命令身份链路合规）**
菜单行只声明 `CmdId`（`menu.h:122-127`），文案在 `Show` 时由注入的解析器现取（`menu.cpp:108-121`），解析器实现方按 `FindCommand(CmdId)` 反查（`commands.h:110`、`key_binding.h:67`）——**本组没有任何"按字符串解析命令名"的路径**，符合要求。`menu_context.cpp` 只做 owner-draw 桥接与 `data` 回传（`:109-115`），不猜命令名，`HIWORD(lpVerb) != 0` 时拒绝字符串动词（`:111`）——正确。

**5. 内存/资源/编码（良好，细项见上）**
- HICON：`ico::Cached`/`sysicon::Get` 都是"进程级持有、调用方只借"，`PopupRow::icon`/`TreeNode::icon` 的注释都写明不销毁；`popup_list.cpp:768-773` 的 `ClearTree` 从"销毁每个 icon"改成"只清结构"，注释解释了不这么改会拿到悬空句柄 —— 这条纪律执行到位。唯一破口是 S1（主题切换销毁借用中的句柄）。
- HFONT：唯一来源 `Theme`，`Reload` 在 DPI 未变时保字体（`theme.cpp:217-227`），理由（SHAutoComplete 重建 110~170ms）写清了。
- 字符串/编码：本组全部 `std::wstring`，UTF-8/UTF-16 转换只在 `text_file.cpp`/`str::` 边界；`ReadText` 三种 BOM 都认，UTF-16LE 分支用 `memcpy` 而非 `reinterpret_cast` 避免 ARM64 未对齐读（`:56-57`）——细节到位。`WriteText` 的"pid 后缀 tmp + FlushFileBuffers + MOVEFILE_WRITE_THROUGH"三点理由与实现一致（`:90-127`）。
- 空指针：`prompt.cpp` 对 `st.edit` 可能为 NULL 的情况未判就 `SendMessageW`（`:160`、`:231`）——`SendMessageW(NULL,…)` 是 no-op 不会崩，但建议判一下，否则 `EM_SETSEL` 静默失效时很难查。

**6. 工程约束（通过）**
- 命名空间：`ui::`（绘制/菜单/提示）、`ico::`、`sysicon::`、`popup::`、`modal::`、`panesplit::`、`sidelayout::`、`tf::`、`viewmode::`、`thememode::` —— 无 `eb::` 残留、无匿名全局符号外泄（`namespace {}` 用得规整）。
- 品牌串：`ExBar` 只出现在注释里说明来历（`theme.cpp:93`、`popup_list.cpp:80`），标识串唯一来源仍是 `identity.h`（本组只 `using id::kPopupClass/kPromptClass/kStatusBarClass`）。
- 文件行数：`popup_list.cpp` **844 行**，与 `data/file-length-baseline.json` 记账值一致（棘轮允许，未越界），但它仍是全工程第 6 长、且 AGENTS §3 的 500 行目标未达成；`popup_list_draw.*` 的拆分方向正确，建议下一步把 `ListSubclass` 的输入状态机（type-ahead/键盘/命中，约 300 行）再拆一个 `popup_list_input.cpp`。
- 硬编码中文（"确定"/"取消"/菜单文案）与全工程一致，未见 i18n 层，不报。

**7. 死代码/注释（已逐条列在第四节）**
6 处注释与实现不符（含 2 处"两段注释互相矛盾"）、3 处死代码块/死 API、3 个死画刷 + 1 个死令牌。集中成因是**实现改了、解释没跟着改**（`theme.cpp` 的底色调平、`popup_toggle` 的捕获时序、`popup_list.h` 的 API 改名）。这类"注释承诺"在本工程里被当作设计约束用（多处注释写着"这正是 X 的成因"），所以失真比一般项目更值钱。

---

## 六、总体评价

**本组质量高于平均水平，主要原因是"每处取舍都留下了可核对的原因"**：`popup_list` 的"再点即收"、`pane_split` 的"百分比分母是 content 不是 avail"、`text_file` 的"tmp 名必须带 pid + 必须 FlushFileBuffers"、`sys_icon` 的"序号资源不是稳定 API"、`theme` 的"切主题前先拆 EBC"——这些注释不是复述代码，而是记录了**为什么不是另一种写法**，且能被当前实现一一验证（我逐条对过，除第四节列出的 6 处外全部成立）。这类文档密度在 6000 行规模上是真正的资产。

**风险集中在两处：**
1. **模态弹层与全局资源生命周期的交叉（S1）**。弹层是"借用 + 模态长驻"的组合，而它借的东西（HICON/HFONT/画刷）由主题系统在任意时刻销毁重建。当前唯一的安全边界是"主题切换只由用户点击触发、用户不会在菜单开着时切主题"——这是**靠用户行为而不是靠代码**保证的，一旦有脚本/自动化/AI 命令能触发 `ApplyTheme`，就立刻变成悬空句柄。`modal::Depth()` 已经存在（正是为这类问题设计的），但只用来挡 AI 派发，没用来挡资源销毁——这是最值得先修的一条。
2. **纯几何文件的"看起来有防御、其实不可达"（S2-8/S2-9/side_layout 死块/S2-1）**。`side_layout.cpp` 的 24-31 行、`theme` 的三个死画刷、`radiusPopup` 令牌，都是"写了一个正确但永不执行的保障"，读者会以为边界已被处理。这类代码比缺失更危险：它让人**不去查**真正的溢出点（`status_bar` 左右组重叠就是真溢出处，S2-1）。

**建议的修复顺序**：S1（一条守卫 + 一条消息过滤，改动 10 行内，收益最大）→ S2-1/S2-2（两处真实几何溢出，各 5~10 行 + 单测）→ S2-3（一行表达式，且是 `mouse.json` 规则失效的真因）→ S2-9 + 间距令牌（视觉来源唯一性的收尾，顺带消掉高频 Create/Delete 画刷）→ 第四节的注释校准（纯文档，可批量做，建议顺手把"注释承诺"全部再对一遍实现）。

**可以延后的**：S2-4/S2-5/S2-6（性能，量级几十 ms，不影响正确性）、S2-11（`prompt` 两版合一，属重构）、`popup_list.cpp` 再拆 300 行（棘轮未超，不阻塞）。
