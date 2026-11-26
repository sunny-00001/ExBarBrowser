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

- ✅ P1（E1+E2+E4）：`edge_autoscroll.h` 落地，TabStrip 委托，书签栏接入；
  `ScrollBy` 统一收口滚动后重算（落点+hover）。
- ✅ P2a（E3）：两条键盘到达路径补 `EnsureVisible`。
- ✅ P2b（E5）：`tab_uia` Traits 化，`bookmark_bar_uia.cpp` 接入。
- ✅ P3：`bookmark_bar_itemmenu.cpp` 拆分。
- ✅ 验证：`bash build.sh` 全量编译 + `bash tests/run_all.sh` +
  `check_components.py` + `check_ai_meta.py` 全绿；滚轮/拖拽手感属 R21 禁合成输入域，
  交人工确认。

## 四、业界对标结论（全网调研，2026-11-26）

- 溢出模式三派：菜单派（Chrome/Safari/IE 桌面）、滚动派（Firefox scrollbox）、
  **混合派（chevron + 滚动共存，Chrome Android 146 最新方向）**——本项目已是混合派，
  选型与业界演进一致。
- `»` 按钮空间：Safari 式按需预留是最佳实践（Chrome 常驻占位浪费 10–20px、
  Firefox 闪烁均被批）——本项目已按需预留。
- 拖拽边缘自动滚动是 Chrome/Firefox 书签栏的业界标配——本项目此前缺失（E2），本次补齐。
- 半项显示：菜单派不显示半项，滚动派显示半项暗示还有内容——本项目显示半项，符合滚动语义。
- 键盘可达性：各浏览器书签栏键盘路径普遍弱于本项目（Tab/方向键/Enter/Shift+F10/Esc）。
