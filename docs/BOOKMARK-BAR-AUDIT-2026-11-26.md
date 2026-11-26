# 书签栏复审与改进（2026-11-26）

> 范围：`src/bookmark_bar*`（h/cpp/draw/hit/wheel/popup/drag/drop）、`app.cpp` 滚轮转交链路；
> 对标：Chrome/Firefox/Safari 书签栏溢出模式、Chrome Android 146（2026-03）书签栏
> chevron+滚动混合模式、拖拽边缘自动滚动惯例。
> 状态标记：☐ 待办 / ✅ 完成（含验证方式）。

## 一、审查结论

静态检查（`check_components.py` / `check_ai_meta.py`）全绿；可见性判据已收敛到
`bookmark_bar.h` 的 8 个 inline 纯函数（单一来源），绘制/命中/提示/溢出菜单/键盘
五条路径口径一致；`»` 按钮空间按需预留（Safari 式，不溢出不占位）。

发现 5 个缺陷（均为复审新暴露，与滚轮功能本身无关）：

| # | 严重度 | 缺陷 | 根因 |
|---|---|---|---|
| E1 | 中 | 拖拽中滚滚轮 → 插入线错位，残留到鼠标再动 | 「滚动发生 → 重算拖拽落点」这条规则缺失；滚动源（滚轮/自动滚动/键盘）与落点计算脱钩 |
| E2 | 中 | 拖拽无边缘自动滚动，拖给视野外第 30 项够不到 | TabStrip 有 `UpdateAutoScroll`/`kDragScrollTimer`，书签栏整条链路缺失 |
| E3 | 低 | 重聚焦后 Shift+F10 菜单锚点可能开到栏外（kbdIdx_ 的 rc.left 为负） | `WM_SETFOCUS` 复用上会话 kbdIdx_ 但不 `EnsureVisible`；`WM_CONTEXTMENU(-1,-1)` 同 |
| E4 | 低 | 滚动后光标下高亮不跟随（hover_ 仍指旧项，直到鼠标再动） | 「滚动发生 → 重算 hover」同样缺失（滚轮滚动时鼠标不动，无后续 mousemove 兜底） |
| E5 | 低 | 无 UIA 无障碍树（读屏软件看这条栏是空白） | TabStrip 有 `tab_uia` provider，书签栏未接入 |

## 二、改进方案（根治式，全部复用既有基础设施）

**原则：同一机制一份实现。** 不抄 TabStrip 的代码，而是把它抽成共享组件再双向使用。

| 项 | 缺陷 | 方案 | 基础设施复用 |
|---|---|---|---|
| P1 | E1+E2 | 新增 `src/edge_autoscroll.h`：边缘自动滚动唯一实现（状态+起停+边缘判定+定时周期），TabStrip 与书签栏都委托它；`ScrollBy` 收口「滚动后重算拖拽落点与 hover」 | TabStrip `UpdateAutoScroll/StopAutoScroll` 改薄委托（行为零变化）；滚动周期共用 `tabDragScrollMs` 一个设置；落点重算走既有 `DragUpdate`；hover 重算走既有 `UpdateHover` |
| P2a | E3 | `WM_SETFOCUS` 与 `WM_CONTEXTMENU(-1,-1)` 两条键盘到达路径补 `EnsureVisible`（键盘到达必可见，与 `FocusBar` 同一规则） | 既有 `EnsureVisible` |
| P2b | E5 | `tab_uia` provider 泛化（root/item 的 ControlType、名称、automationId 前缀走 `Traits` 参数，provider 类只此一份），书签栏接入 `WM_GETOBJECT` + 3 个 `kMsg*` 消息分支（新文件 `bookmark_bar_uia.cpp`），析构 `uia::Release` | 既有 `tab_uia.cpp` 463 行 provider 整体复用；消息号 `WM_APP+0x51..53` 为窗口类私有语义，两栏同号不冲突 |
| P3 | E4 | 随 P1 一起根治（`ScrollBy` 统一重算） | 同上 |
| P3 | 结构 | `bookmark_bar_popup.cpp`（484/500 行）预拆 `ShowItemMenu`（~190 行）出 `bookmark_bar_itemmenu.cpp` | 纯移动不改逻辑 |
| 待定 | 观测面 | 书签栏 overflow/scrollX/maxScroll 接入 `ai::StateSnapshot` | R19 同源；待用户点头 |

## 三、执行记录

- ✅ P1（E1+E2+E4）：
  - 新增 `src/edge_autoscroll.h`——边缘自动滚动唯一实现（`edge::Update/Stop/TickGuard`）。
    TabStrip 的 `UpdateAutoScroll/StopAutoScroll` 改为薄委托（行为零变化）；
    书签栏接入同一组件（边缘带以 `viewRight_` 为准，周期共用 `tabDragScrollMs` 设置）。
  - `BookmarkBar::ScrollBy` 收口为**唯一滚动入口**：滚轮/键盘跟随/自动滚动拍全走它，
    并在滚动后统一重算拖拽落点（`DropSpotAtClient`，用缓存坐标）与悬停高亮
    （`UpdateHover`）——根治"滚了但插入线/高亮指旧项"（E1/E4），任何新的滚动源自动继承。
  - 停表收口：`DragFinish` / `HandoffDragToFolder` / `WM_CAPTURECHANGED` / `WM_MOUSELEAVE`
    四处全部经 `edge::Stop`（滚着松手/被夺捕获/滑出栏外都不留表）。
  - 悬停重算的坐标来源：拖拽中用 `dragMouseX_+dragStart_.y` 缓存，空闲悬停用
    `GetCursorPos`（`WM_MOUSEMOVE` 顺带缓存 `dragMouseX_`）。
- ✅ P2a（E3）：`WM_SETFOCUS` / `WM_CONTEXTMENU(-1,-1)` / `VK_RETURN|VK_SPACE` /
  `VK_APPS|Shift+F10` 四条键盘到达路径全部先 `EnsureVisible` 再取锚点 ——
  "键盘到达必可见"成为统一规则，与 `FocusBar` 一致。
- ✅ P2b（E5）：`tab_uia.h/.cpp` 泛化——provider（463 行）本体只此一份，
  差异面收敛为 `uia::Traits`（根名/控件类型/automationId 前缀/宿主指针）；
  `TabTraitsFor`（tab_uia.cpp）与 `bmuia::TraitsFor`（bookmark_bar_uia.cpp）各给一份。
  书签栏在 `Handle` 加 `WM_GETOBJECT` + `kMsgCount/kMsgItem/kMsgSelect` 分支
  （kMsgSelect = 键盘走到该项 + EnsureVisible，不导航），析构 `uia::Release`。
- ✅ P3：`bookmark_bar_popup.cpp`（484→205 行）拆出 `bookmark_bar_itemmenu.cpp`
  （291 行，`ShowItemMenu` + `ShowMoveToMenu` + `CollectMoveTargets`）。
- ✅ 验证（2026-11-26 22:49）：
  - `bash build.sh` 主程序构建通过（`ExBarBrowser.exe` 22:48）。
  - 全量单测 **73/73 通过**（此前后台跑挂的 keymap/shell_columns 均为并发会话
    编辑窗口期的瞬态编译错，单跑与最终全量均复现通过；`app_panes_swap.cpp`
    duplicate symbol 同为并发会话拆文件中间态）。
  - `check_components.py`（33 唯一性 + 7 禁令 + R21）全绿；
    `check_ai_meta.py` 全绿；`check_commands.py`（241 条三方一致）全绿。
  - 滚轮/拖拽自动滚动手感属 R21 禁合成输入域，交人工确认。

## 四、业界对标结论（全网调研，2026-11-26）

- 溢出模式三派：菜单派（Chrome/Safari/IE 桌面）、滚动派（Firefox scrollbox）、
  **混合派（chevron + 滚动共存，Chrome Android 146 最新方向）**——本项目已是混合派，
  选型与业界演进一致。
- `»` 按钮空间：Safari 式按需预留是最佳实践（Chrome 常驻占位浪费 10–20px、
  Firefox 闪烁均被批）——本项目已按需预留。
- 拖拽边缘自动滚动是 Chrome/Firefox 书签栏的业界标配——本项目此前缺失（E2），本次补齐。
- 半项显示：菜单派不显示半项，滚动派显示半项暗示还有内容——本项目显示半项，符合滚动语义。
- 键盘可达性：各浏览器书签栏键盘路径普遍弱于本项目（Tab/方向键/Enter/Shift+F10/Esc）。
