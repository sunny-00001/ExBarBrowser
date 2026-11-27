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
- ✅ P4（追加，2026-11-27）：**OLE 外部拖入（从文件视图拖文件夹进书签栏）也具备边缘自动滚动**。
  根因：外部拖放走 `IDropTarget::DragOver → UpdatePreview` 链路，不经过内部重排的
  `DragUpdate`，且 `ScrollBy` 重算守卫 `dragging_` 对外部拖放恒为 false——滚动后落点
  预览指旧项。根治手法（复用 P1 基础设施，不新增机制）：
  - `EdgeAutoScroll(clientX, clientY)` 抽为**唯一入口**（bookmark_bar_drag.cpp）：
    内部重排 `DragUpdate` 与外部拖放共同调用；缓存 `dragMouseX_/dragMouseY_` 成对坐标
    （定时器拍没有鼠标坐标，落点重算需要整点）。
  - `PreviewDropAt(POINT screen)`（bookmark_bar_drop.cpp）：落点预览 + 滚动起停一步做完，
    `UpdatePreview` 直接传屏幕坐标（省去 ClientToScreen 二次转换）。
  - `ScrollBy` 落点重算守卫统一为 `dropPreview_`（两条链路都写此标志）；
    悬停重算统一用 `GetCursorPos`（内部拖拽 SetCapture、OLE 拖放光标归系统，
    都无可靠 mousemove 兜底）。
  - 停表收口：`ClearDropPreview` 一处负责（Drop 提交 / DragLeave / 拖进分类弹层三条路
    都会走）；`WM_TIMER` 滚动拍守卫同时认 `dragging_ || dropPreview_`。
- ✅ 验证（2026-11-26 22:49）：
  - `bash build.sh` 主程序构建通过（`ExBarBrowser.exe` 22:48）。
  - 全量单测 **73/73 通过**（此前后台跑挂的 keymap/shell_columns 均为并发会话
    编辑窗口期的瞬态编译错，单跑与最终全量均复现通过；`app_panes_swap.cpp`
    duplicate symbol 同为并发会话拆文件中间态）。
  - `check_components.py`（33 唯一性 + 7 禁令 + R21）全绿；
    `check_ai_meta.py` 全绿；`check_commands.py`（241 条三方一致）全绿。
  - 滚轮/拖拽自动滚动手感属 R21 禁合成输入域，交人工确认。
- ✅ P4 验证（2026-11-27 07:01）：
  - 书签域单测 `bookmark_bar_scroll_test` PASS；tab_strip 域 `tab_strip_rules_test` PASS
    （隔离输出目录 `tmp_ole_test_out/`；首次链接失败系并发会话拆 `shell_input_uia.cpp`
    中间态——新文件落盘晚于构建扫描，复跑即过，非代码问题）。
  - `check_ai_meta.py` 全绿；`check_components.py` 唯一 FAIL 为并发会话当日新增的
    `tests/key_binding_test.cpp` 判据（快捷键「清除」vs「还原默认」语义）待人工重签，
    与书签域无关。
  - `bash build.sh` 主构建通过（`ExBarBrowser.exe` 07:01，208 TU 全量增量）。
  - 外部拖文件夹至栏边缘的滚动手感属 R21 禁合成输入域，交人工确认。

## 三之二、二次复审与交互缺口补齐（2026-11-27）

第二轮复审针对**交互语义**（第一轮查的是滚动/拖拽/UIA 这些"机制"层）。
对标 Chrome / Edge / Firefox / Vivaldi 书签栏的既有约定，发现本项目在"点一下
会发生什么"这一层几乎是空白 —— 而这些是用户的肌肉记忆，缺了会被直接判成
"这软件不支持"。

### 缺陷（E6–E10）

| # | 严重度 | 缺陷 | 业界依据 |
|---|---|---|---|
| E6 | 高 | 中键点书签无反应；点分类也无反应 | Chrome/Firefox/Edge 三家一致：中键 = 后台新标签 |
| E7 | 高 | Ctrl+左键 / Shift+左键 无差别（都进当前标签） | 三家一致：Ctrl = 新标签、Shift = 新窗口 |
| E8 | 中 | 没有"固定在新标签打开"的开关 | Edge 110+ 原生设置「Always open favorites in new tab」 |
| E9 | 中 | 书签栏上的 Delete 键穿透到文件视图（**误删背后文件**） | 列表类控件通用约定：选中项 + Delete = 删除该项 |
| E10 | 中 | 分类没有「打开全部」（想开整组只能逐个点） | Chrome/Edge「Open all」、Firefox「Open All in Tabs」、Vivaldi 同 |
| E11 | 低 | 书签栏拖拽自动滚动周期与标签栏共用 `tabDragScrollMs`，且无开关 | 两栏内容密度不同（书签项更窄），共用一个旋钮必有一边不合手 |

### 落地（全部走既有基础设施，无新机制）

- **新增 `src/bookmark_bar_open.cpp`**（打开语义唯一实现）：
  - `ConsumeLeafClickMods`：Shift → `fs.open.window`；Ctrl → `fs.open.tab`；
    无修饰时看 `bookmarkOpenInNewTab` 开关。**修饰键反向覆盖开关** ——
    开着开关时 Ctrl 回当前标签，否则开关一开就再也无法原地打开（死锁）。
  - `OnMiddleDown`：叶子 → `fs.open.tab{background:true}`；分类 → 整组打开。
    中键恒为后台（Chrome 语义：中键的价值就是不打断当前视图）。
  - `OpenFolderChildrenInTabs`：递归收集可打开叶子（跳过纯分类与空 path），
    先收集后派发（Invoke 会触发 Reload → BuildItems，边遍历边派发是 UAF）。
- **`bookmark_bar_keys.cpp`**：`VK_DELETE` 删除焦点书签，确认框与右键删除同款；
  删除后焦点项按新 size 夹取（否则 `kbdIdx_` 指向已不存在的下标，下次 Delete 静默失效）。
- **`bookmark_bar_itemmenu.cpp`**：分类右键新增「打开全部（N 项）」（action 21，
  前台打开）；`CountOpenableChildren` 与 `OpenFolderChildrenInTabs` **同口径** ——
  菜单写 N、实际开 M 是比不写数字更糟的反馈。
- **`bookmark_bar_drag.cpp`**：`EdgeAutoScroll` 读 `bookmarkDragAutoscroll`（总开关，
  关掉连表都不挂）与 `bookmarkDragScrollMs`（独立周期）；标签栏 `tabDragScrollMs` 不动。

### 设置项（`bar_bookmark` 分组，+3 项；界面/AI/命令自动跟随）

| key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `bookmarkOpenInNewTab` | Bool（下拉：当前标签 / 固定新标签） | false | Edge「固定打开新标签页」同款 |
| `bookmarkDragAutoscroll` | Bool（下拉：关闭 / 开启） | true | 书签栏拖拽边缘自动滚动总开关 |
| `bookmarkDragScrollMs` | Int（10..200） | 30 | 书签栏独立周期，与标签栏各管各的 |

登记五处：`config.h` 字段 + `settings_bounds.h` 常量 + `config_io.cpp` 读写 +
`settings_reg_table_bookmark.cpp` 条目 + `config_apply.cpp` IsPureData 列名。

### 顺带：`settings_reg_table.cpp` 超限拆分

加三项后涨到 804 行、越过棘轮记账（786）。照并发会话已建立的
`settings_reg_table_hotkey.cpp` 先例，把书签段整体拆成
`settings_reg_table_bookmark.{h,cpp}`（7 项，含原有的显隐/高度/提示窗宽/滚轮步进）。
主表回到 752 行，棘轮恢复绿。分段理由：这一组同时含"几何"与"交互策略"两类
语义，且是功能增长最快的一块，留在总表里每加一项都往上限推一次。

### 验证（2026-11-27 09:27）

- `bash build.sh` 主构建通过（`ExBarBrowser.exe`）。
- 书签域 `bookmark_bar_scroll_test` PASS；`config_apply_test` / `config_roundtrip_test`
  双 PASS（前者正是"每个可写项都必须有落地分支"的全覆盖判据 —— 新增三项登记无误）。
- `check_commands.py`（243 条三方一致）OK；`check_ai_meta.py` 全绿；
  `check_components.py` 行数棘轮恢复绿，唯一 FAIL 为并发会话新增的
  `tests/key_preset_test.cpp` 待人工重签（与书签域无关）。
- 中键/Ctrl/Shift/Delete/「打开全部」的实际手感属 R21 禁合成输入域，交人工确认。

## 四、业界对标结论（全网调研，2026-11-26）

- 溢出模式三派：菜单派（Chrome/Safari/IE 桌面）、滚动派（Firefox scrollbox）、
  **混合派（chevron + 滚动共存，Chrome Android 146 最新方向）**——本项目已是混合派，
  选型与业界演进一致。
- `»` 按钮空间：Safari 式按需预留是最佳实践（Chrome 常驻占位浪费 10–20px、
  Firefox 闪烁均被批）——本项目已按需预留。
- 拖拽边缘自动滚动是 Chrome/Firefox 书签栏的业界标配——本项目此前缺失（E2），本次补齐。
- 半项显示：菜单派不显示半项，滚动派显示半项暗示还有内容——本项目显示半项，符合滚动语义。
- 键盘可达性：各浏览器书签栏键盘路径普遍弱于本项目（Tab/方向键/Enter/Shift+F10/Esc）。

## 五、点击打开语义对标（全网调研，2026-11-27）

| 动作 | Chrome | Edge | Firefox | 本项目（本轮前 → 后） |
|---|---|---|---|---|
| 左键 | 当前标签 | 当前标签 | 当前标签 | 当前标签（不变） |
| 中键 | 后台新标签 | 后台新标签 | 后台新标签 | **无反应 → 后台新标签** |
| Ctrl+左键 | 新标签 | 新标签 | 新标签 | **同左键 → 新标签** |
| Shift+左键 | 新窗口 | 新窗口 | 新窗口 | **同左键 → 新窗口** |
| 固定新标签开关 | 无原生（靠扩展） | 110+ 原生 | `about:config` | **无 → 有（对齐 Edge）** |
| 分类「打开全部」 | Open all (N) | Open all (N) | Open All in Tabs | **无 → 有（含中键路径）** |
| Delete 删除选中 | 书签管理器内 | 同 | 同 | **穿透到文件视图 → 删书签** |

结论：本项目此前在"点一下会发生什么"这一层几乎是空白 —— 而这恰好是用户
迁移过来的第一层肌肉记忆。本轮全部补齐，且**只走既有命令**
（`fs.open` / `fs.open.tab` / `fs.open.window` / `bmk.remove`），
没有为书签栏新造任何打开/删除实现。

一处刻意的偏离：**中键打开分类 = 整组后台打开**。Chrome/Edge 的中键在分类上
只打开菜单，但"中键 = 不打断当前视图地批量取用"这一意图在分类上最自然；
而菜单路径（右键 → 打开全部）仍然保留且是前台，两种意图各有入口。

## 六、三次复审：一致性缺口与能力缺口（2026-11-27 09:33）

第三轮把范围扩到**全库书签实现**（`bookmarks*.cpp` 数据层 / `bookmark_io.cpp`
导入导出 / 18 条 `bmk.*` 命令 / UIA / state 观测面），对标浏览器与文件管理器
两个领域。数据层的护栏是扎实的（加载失败拒写、外部变更检测、冲突另存、
`WriteText` 的 pid 临时名 + FlushFileBuffers + MOVEFILE_WRITE_THROUGH 原子写），
**没有发现数据丢失链**。问题集中在三类：

### 6.1 前两轮改动留下的不一致（新引入，优先修）

| # | 缺陷 | 成因 | 后果 |
|---|---|---|---|
| E12 | 溢出菜单点书签走 `Navigate(idx)`，绕过修饰键语义 | `ShowOverflowMenu` 是 P2 之前写的，第二轮只改了 `WM_LBUTTONUP` | 同一个书签，栏上点 Ctrl 开新标签、» 菜单里点 Ctrl 还是当前标签 |
| E13 | 分类弹层点子项直接调 `onNavigate_`，绕过 `fs.open` 命令 | `bookmark_category_popup.cpp` 早于命令总线收口 | 该路径无审计、无修饰键、AI 侧看不到这次导航 |
| E14 | 空状态提示硬编码 `Ctrl+D` / `Ctrl+B` | 文案写于键位可配之前 | 用户改了 `hotkeyAddBookmark` / `hotkeyFocusBookmarkBar` 后，提示指向的键按下去没反应 |

E12/E13 是"同一个动作有两个入口，只改了一个"的典型。**根因不是漏改某一行，
而是打开语义没有唯一收口点** —— 第二轮把它做成 `ConsumeLeafClickMods`，
但只接在 `WM_LBUTTONUP` 一处。修法：把三个入口（栏、溢出菜单、分类弹层）
统一走同一条派发。

### 6.2 真实的边界缺陷

| # | 缺陷 | 判据 | 后果 |
|---|---|---|---|
| E15 | 极窄窗口下 `viewRight_` 为负、`ovfRc_` 成倒置矩形 | 推演：宽 10 时 `fullRight=4`、`viewRight=-24` | `IntersectClipRect(mem,0,0,-24,…)` 传负右边界；`ovfRc_.left > .right`。窗口最小宽 560 使常态不可达，但**栏高/边距可配 + 多显示器 DPI 混用**下仍可能构造出来 |

### 6.3 能力缺口（对标浏览器 / Opus）

| # | 能力 | 业界 | 本项目 | 价值 |
|---|---|---|---|---|
| E16 | 书签栏自动隐藏（仅悬停/仅新标签时显示） | Chrome flag `bookmark-bar-hover-reveal`、Edge「仅在新标签显示」 | 只有显/隐两态 | 高：省垂直空间的头号诉求 |
| E17 | 书签搜索 | Chrome/Edge/Firefox 书签管理器、Opus 收藏夹过滤框 | 无（`bmk.list` 只能全列） | 高：条目一多就靠 » 菜单翻，不可检索 |
| E18 | 从书签栏向外拖出 | 浏览器可拖到地址栏/别的窗口；Opus 收藏可拖出 | 只 `RegisterDragDrop`（收），无 `DoDragDrop`（发） | 中 |
| E19 | 分隔符（separator） | Vivaldi/Chrome 书签管理器支持分组 | 无 | 中：分类之外没有轻量分组手段 |
| E20 | SmartFavorites（自动学习常用位置） | Opus 核心能力 | 无 | 低（可选增强） |
| E21 | 多行书签栏 | Firefox 曾支持、Edge 无 | 单行 + 滚动（已是业界主流选择） | 低（不建议做，与混合派选型冲突） |

### 6.4 本轮处置

按"先修自己引入的不一致，再修边界，能力缺口进待办"的顺序：

- **E12/E13/E14 本轮修复**：打开语义收口成唯一派发点，三个入口共用；
  空状态提示改为从键位表**现查**（键名不再写死）。
- **E15 本轮修复**：`Layout` 里夹取 `viewRight_` 与 `ovfRc_`，负宽即视为不可见。
- **E16–E20 进待办**（见 §6.5），不在本轮动：它们都是**新能力**，
  而本轮的约束是"根治既有缺陷、不打补丁"。

### 6.5 待办（按价值/成本排序）

1. **E17 书签搜索**：`bmk.find{query}` 命令 + 溢出菜单顶部搜索框。
   复用既有 `bmk.list` 的遍历与 `ui::Menu` 的过滤能力，成本中等，价值最高。
2. **E16 自动隐藏**：三态设置（常显 / 悬停显示 / 仅单标签时隐藏），
   复用 `showBookmark` 已有的显隐链路 + `TrackMouseEvent`。
3. **E18 向外拖出**：`DoDragDrop` + `IDataObject`（CF_HDROP）。
   注意必须与现有内部拖拽状态机互斥，否则一次按下会同时进两条链路。
4. **E19 分隔符**：`BookmarkNode` 加 `isSeparator`，需改 HTML 序列化
   （跨软件契约 `EXBAR_*` 要新增属性，属于对外契约变更，需单独评审）。

### 6.6 本轮修复记录（2026-11-27 09:58 完成）

**E12/E13：打开语义收口到唯一派发点**

新增 `BookmarkBar::OpenPathWithMods(path, mods)` —— 打开动作的**唯一收口**。
三个入口全部改调它：

| 入口 | 文件 | 改动前 | 改动后 |
|---|---|---|---|
| 栏上左键 | `bookmark_bar.cpp` `WM_LBUTTONUP` | `ConsumeLeafClickMods` | `OpenPathWithMods` |
| `»` 溢出菜单 | `bookmark_bar_popup.cpp` `ShowOverflowMenu` | `Navigate(idx)`（Ctrl 失效） | `OpenPathWithMods`，无覆盖才 `Navigate` |
| 分类弹层 | `bookmark_category_popup.cpp` `ShowFolderContents` | 直接 `onNavigate_`（绕过总线） | `OpenPathWithMods`，无覆盖才 `onNavigate_` |

`OpenPathWithMods` 内部构造临时 `Item` 复用 `ConsumeLeafClickMods`，判据不抄第二遍。
修饰键在菜单 `Show` 返回后由 `CurrentMods()` 现读 —— `ui::Menu` 是纯 UI 组件，
不为了"这一下按没按 Ctrl"去改它的返回契约。

**E14：空状态提示键名现查**

`BookmarkBar::SetShortcutFn(std::function<std::wstring(CmdId)>)` 注入，
App 侧接 `input_.ShortcutText(id)`（与菜单右端快捷键同一条 `keybinding` 推导）。
提示文案改为按实际绑定拼装：没绑 `bmk.add.current` 就不提收藏，没绑 `bmk.focus`
就不提聚焦；两个都没绑则只显示"书签栏"。宁可少说一句，也不显示按不出来的键。

**E15：窄窗口负矩形**

`Layout` 里两处夹取：
- `viewRight_ < 0` 时夹到 0（下游 `BmkInView`/`IntersectClipRect` 自动安全）；
- `ovfRc_` 用 `std::max<LONG>(0, …)` 并保证 `right >= left`、`bottom >= top`。
  倒置矩形在 `FillRect`/`DrawGlyph` 下是未定义行为，夹成空矩形让"没有按钮"
  成为唯一解释。

顺带修掉 `bookmark_bar_draw.cpp` 里拖拽插入线的 `rc.bottom - 2` 硬编码像素
（同一行的其他尺寸都走了 `theme_->S()`）。

**验证**

- `bash build.sh` 通过（218 TU）。
- `bookmark_bar_scroll_test` PASS；`key_binding_test` / `key_preset_test` /
  `keymap_test` 三个键位测试全 PASS（E14 改的是键位消费路径，必须验这一组）。
- `check_commands.py` 243 条三方一致 OK；`check_ai_meta.py` 全绿；
  `check_components.py` 行数棘轮绿（`app.cpp` 压到 958 行，低于基线 959）。
- 唯一 FAIL 仍是并发会话的 `path_util_test.cpp` / `key_preset_test.cpp` 待人工重签。

**工具链缺陷（顺带发现）**：`tests/run_all.sh` 的 `EXBAR_TEST_OUT` 传**相对路径**时，
第 195 行的 `>"$OUT/$name.run.log"` 在 `cd "$rundir"` 之后失效，报
`No such file or directory` 并被计成"运行失败"——而测试本身是通过的。
传绝对路径即正常。这个假失败会让人误判代码坏了。

## 七、结论

三轮复审后，书签栏在**机制层**（滚动/拖拽/UIA/数据安全）与**交互语义层**
（中键/Ctrl/Shift/Delete/打开全部）都已对齐业界。剩余缺口全是**新能力**
（搜索、自动隐藏、拖出、分隔符），不是缺陷。

值得记下的一条工程教训：**"同一个动作有多个入口"是这类复审最容易漏的地方**。
第一轮补了滚动收口（`ScrollBy`），第二轮补了打开语义，但打开语义只接了
一个入口 —— 于是溢出菜单和分类弹层立刻成了新的不一致。判据应该是
"这个语义有几个触发点"，而不是"我改的那个分支对不对"。

---

## 八、四次复审：跨层一致性（2026-11-27 12:45）

前三轮都在**书签栏内部**看（机制层 / 交互语义 / 入口一致性）。第四轮换了
判据：**不看"书签栏写得对不对"，看"同一个语义在跨层边界上有没有被截断"** ——
三条边界：UI 层 ↔ 命令总线层、鼠标/键盘 ↔ 无障碍（UIA）层、
"打开"语义 ↔ 它的全部触发点。

结论：**E12/E13 的教训只修了当时看得见的两处，同一个根因还有三处在**。
另外发现一条跨层缺陷（E24）和一个无障碍语义缺陷（E23），都是前三轮
完全没碰过的层。

### 8.1 缺陷（E22–E26）

| # | 缺陷 | 判据 | 后果 |
|---|---|---|---|
| E22 | 键盘 `Enter` / `空格` 打开书签走 `Navigate(kbdIdx_)`，绕过 `OpenPathWithMods` | `bookmark_bar_keys.cpp` 的 `VK_RETURN` 分支直接调 `Navigate` | 键盘用户按住 Ctrl+Enter 仍是当前标签；这是"打开语义"的**第三个**触发点（E12/E13 修了前两个） |
| E23 | UIA `IInvokeProvider::Invoke()` 与 `ISelectionItemProvider::Select()` 发**同一条** `kMsgSelect` | `tab_uia.cpp` 两个方法体逐字相同，都发 `kMsgSelect` | 读屏软件"激活书签"只把键盘焦点移过去、**不打开**。Invoke 与 Select 在 UIA 规范里是两个不同语义（激活 vs 选中），混同等于"无障碍入口的打开动作是坏的" |
| E24 | 书签删除/清空/解散在 UI 层弹 `MessageBoxW`，命令又是 `destructive` 且不在豁免表 → 总线再问一遍 | 4 处 UI 弹框 + `NeedsConfirm` 返回 true | 同一次删除**连问两次**（先"删除书签「x」？"，再"「删除书签」会覆盖现有内容…"）。项目里已有正确写法（`SessionDelete` 用 `IsCallConfirmed()` 判断），书签这条链路没用 |
| E26 | 图标子目录树的两处导航直接调 `onNavigate_` | `ShowChildTree` 的空目录直进、`folder_tree::Show` 选中路径 | 与 E22 同源：绕过命令总线（无审计、AI 侧看不到）与修饰键语义 |

E22/E26 是**同一个根因的第三、第四、第五个触发点**。前三轮修 E12/E13 时
只找了"我当时改的那个分支"的两个邻居，没有做一次**全库搜索**把所有
"打开一个书签路径"的地方列出来。这条教训值得单独记：**收口点的正确性
不由"我改对了"证明，而由"再没有别的地方绕过它"证明** —— 判据是一条
grep（`Navigate(` / `onNavigate_(` 的全部调用点），不是一次代码阅读。

### 8.2 非缺陷（已核实撤销）

| 疑点 | 核实结果 |
|---|---|
| 空状态提示的键名拼装（`kAdd`/`kFocus` 四种组合的前缀与分隔符） | 四种组合逐个推演，括号与逗号都正确；两键都没绑时只显示"书签栏"。**不是缺陷**，不改 |

### 8.3 修法（全部复用既有机制，无新机制）

**E22 + E26：把"打开一个路径"的触发点收敛到 3 处**
`OpenPathWithMods` 已是唯一收口，但调用点只有 3 个（栏上左键、» 菜单、
分类弹层）。本轮把剩下 3 处全部接上：
- 键盘 `Enter`/`空格`（`bookmark_bar_keys.cpp`）→ `OpenPathWithMods`，
  无覆盖才 `Navigate`；
- 图标树空目录直进（`bookmark_bar_popup.cpp`）→ 同上；
- 图标树选中路径（`bookmark_bar_popup.cpp`）→ 同上。

**E23：给 UIA 的 Invoke 一条独立消息**
`tab_uia.h` 加 `kMsgInvoke = WM_APP + 0x54`；provider 的 `Invoke()` 改发它。
两栏各自处理：
- 标签栏（`tab_strip_input.cpp`）→ `NotifySelect(i)`（切标签就是它的"激活"）；
- 书签栏（`bookmark_bar.cpp`）→ `OpenPathWithMods` + `Navigate`（与鼠标
  左键同一条语义），分类则弹分类菜单。

`Select()` 保持原样（只移动焦点）—— 这正是 UIA 规范里两者的区别。

**E24：UI 层确认过的，总线不再问**
复用既有的 `IsCallConfirmed()` 反向写入：新增
`CommandBus::MarkCallConfirmed()`，UI 层在弹框得到"是"之后调用它。
`ConfirmIfDestructive` 已经在读 `callConfirmed_`（`if (callConfirmed_) return true;`），
所以这是**把已有字段的写入端补齐**，不是加第二套机制。
`Dispatch` 里的 `ConfirmScope` 保证它只对本次调用有效，嵌套调用不串味。

判据：`SessionDelete` 早就是这么写的（`!IsCallConfirmed() && MessageBoxW(...)`），
书签这条链路只是漏了同一个约定 —— 与 E22 同类的"约定存在但没铺开"。

### 8.4 验证

见本轮修复记录（§8.5）。

### 8.5 本轮修复记录（2026-11-27 13:20 完成）

**E22 + E26：打开语义的触发点从 3 处收敛到 7 处**

`OpenPathWithMods` 本来就是唯一收口，本轮把剩下所有旁路都接上：

| 触发点 | 文件 | 改动前 | 改动后 |
|---|---|---|---|
| 键盘 `Enter`/`空格` | `bookmark_bar_keys.cpp` | `Navigate(kbdIdx_)` | `OpenPathWithMods` → 无覆盖才 `Navigate` |
| 图标树·空目录直进 | `bookmark_bar_popup.cpp` | `onNavigate_(it.path)` | 同上 |
| 图标树·选中路径 | `bookmark_bar_popup.cpp` | `onNavigate_(picked)` | 同上 |
| UIA 激活 | `bookmark_bar_open.cpp` `UiaInvoke` | （原先根本没有"激活"） | `OpenPathWithMods` → 无覆盖才 `Navigate` |

栏上左键、» 溢出菜单、分类弹层三处是前两轮修的，未动。

**E23：UIA 的 Invoke 与 Select 分开**

`tab_uia.h` 新增 `kMsgInvoke = WM_APP + 0x54`；provider 的 `IInvokeProvider::Invoke()`
改发它（原先与 `Select()` 逐字相同）。两栏各自处理：
标签栏 → `NotifySelect`（切标签就是它的激活）；书签栏 → `UiaInvoke`
（叶子走收口点、分类弹分类菜单，与鼠标左键同一语义）。

`Select()` 保持"只移焦点"—— 这正是 UIA 规范里两者的区别。

**E24：UI 层确认过的，总线不再问**

`CommandBus::MarkConfirmedForNextCall()` + `confirmNext_` 一次性标志，
`Dispatch` 入口消费它并并入 `callConfirmed_` 的初始值。六个 UI 入口接上：

| 入口 | 文件 |
|---|---|
| 项右键「删除」 | `bookmark_bar_itemmenu.cpp` |
| 项右键「清除全部书签」 | `bookmark_bar_itemmenu.cpp` |
| 项右键「解散分类」 | `bookmark_bar_itemmenu.cpp` |
| 分类弹层「删除」 | `bookmark_category_popup.cpp` |
| 分类弹层「解散分类」 | `bookmark_category_popup.cpp` |
| 键盘 `Delete` | `bookmark_bar_keys.cpp` |
| 空白区菜单「清除全部」 | `bookmark_bar_popup.cpp` |

`bmk.import` 例外处理：它进 `kNoConfirmDestructive` 豁免表，理由与 `win.quit`
同构 —— 总线的泛化确认发生在 handler **之前**，那时用户连文件都没选、模式也没选，
"会覆盖现有内容"这句话对选"追加"的人是**错的**（假警报比不报警更伤信任）；
而 handler 自己在 `pick == 1` 时已经问过一句知道要覆盖什么的话。豁免后追加路径
不再白问、覆盖路径不再连问两遍。

**新增机械判据：`BMK_OPEN_GUARD`**

这是本轮最重要的一条 —— E12/E13/E22/E26 是**同一个根因的五次复发**，
每轮都靠人工阅读发现，说明"收口点的正确性"一直没有机械保障。
`check_components.py` 新增 `check_bmk_open_guard()`：书签域 8 个文件里，
**每一个** `Navigate(` / `onNavigate_(` 调用点都必须受 `OpenPathWithMods` 保护
（同语句内，或所属 `if` 条件头内）。

判据的形态经过两次自我推翻，值得记下：
1. 第一版用行级正则（"行里没 OpenPathWithMods 却有 Navigate"）→ 把
   `void BookmarkBar::Navigate(...)` 的**定义体**和合法的跨行回落全判红了，
   而真正的整段绕过因为换行写法差异被漏掉；
2. 第二版按函数体判（"体内有 Navigate 就必须也有 OpenPathWithMods"）→
   **实测注入 3 个破坏点只报出 1 个**：一个函数里只要有一处走了收口点，
   同函数内另一处旁路就被掩盖了。
3. 第三版（现在）：逐调用点判，往上回溯 6 行找所属 `if` 头，遇到分号/右括号
   即停止（防跨函数误认）。

负向验证：注入 3 个破坏点（E22 复发 / E26 复发 / E13 复发）→ **3/3 全部捕获**；
还原后回绿。这条判据现在是"再没有别的地方绕过它"的证明，而不是一句注释。

### 8.6 验证结果

- `bash build.sh` 通过（218 TU，增量 4 个文件）。
- `bookmark_bar_scroll_test` PASS；`key_binding_test` / `key_preset_test` /
  `keymap_test` 全 PASS（E22/E24 改的是键位与确认路径，必须验这一组）。
- `check_commands.py` 243 条三方一致；`check_ai_meta.py` 全绿。
- `check_components.py` 新增 `BMK_OPEN_GUARD` 绿，负向验证 3/3 捕获。
- 仅剩两个 FAIL 均为**并发会话**改动（`settings_dialog_actions.cpp` 651/648 行、
  `tests/config_apply_test.cpp` 待重签），不代签。

### 8.7 未处理（明确不做）

| 项 | 理由 |
|---|---|
| 书签栏内部 `onNavigate_` 与 `fs.open` 两条命令的选择 | 分类弹层用 `onNavigate_`（不压历史栈）而栏上用 `fs.open`（压栈）—— 这是**语义差异不是缺陷**：弹层里点书签是"就地进目录"，栏上点是"导航"。要统一得先定"书签点击该不该进历史"，属产品决策 |
| `Navigate()` 内部不走命令总线 | 它是 `onNavigate_` 的唯一调用者，而 `onNavigate_` 是 App 注入的 `NavigateActive`。再套一层 `fs.open` 会让每次书签点击多一次审计记录，收益是"AI 能看到用户点了书签"—— 价值存疑，且会让审计噪音翻倍 |

### 8.8 用户追加决策：删除单条书签不弹确认（2026-11-27 13:40）

第四轮收口后用户直接反馈：**"删除书签不要弹窗"**。这取代了 E24 对
`bmk.remove` 采用的方案（UI 弹一句带书名的确认框 + `MarkConfirmedForNextCall`
压住总线的泛化确认）—— 用户要的是完全不弹。E24 的历史记录保留在 §8.3/§8.5，
但对这一条命令已经失效。

**修法（改唯一真相源，不在 UI 打补丁）**

`NeedsConfirm` 的免问通道只有 `kNoConfirmDestructive` 一张表，所以把
`CmdId::BmkRemove` 加进这张表，所有人工入口（右键 / 键盘 / 命令面板 / 热键）
一并免问；三个 UI 入口的 `MessageBoxW` 与 `MarkConfirmedForNextCall` 随即删掉。

| 入口 | 文件 | 改动 |
|---|---|---|
| 项右键「删除」 | `bookmark_bar_itemmenu.cpp` | 去掉弹框与标记，直接 `Invoke(BmkRemove)` |
| 键盘 `Delete` | `bookmark_bar_keys.cpp` | 同上；保留删除后焦点顺移逻辑 |
| 分类弹层行内右键「删除」 | `bookmark_category_popup.cpp` | 同上 |

`MarkConfirmedForNextCall` **保留**给「清除全部书签」「解散分类」—— 用户只说
单条删除免问，这两条一次影响一大片，仍是"UI 问具体的 + 总线不重复问"。
`bmk.remove` 依旧是 `destructive + undoable`、依旧进撤销栈与审计，只是不再
事前问；误删用「撤销删除」恢复。命令表与 AI 元数据一个字没改。

**为什么不能只删三处 UI 判断**：那样总线的泛化确认会顶上来（判据层仍返回
`NeedsConfirm=true`），用户只是从"带书名的框"变成"泛化框"，等于没修。
判据层与 UI 层两半必须一起改。

**判据同步**

- `tests/commands_confirm_test.cpp`：原"undoable 不再免除确认"一项改为
  "单条删除在白名单里，不弹确认框"；`bmk.clear` 仍断言要确认 —— 保住
  "undoable 本身不免问"这条通则没被一起放宽。
- `tools/check_components.py` 新增 `BMK_DELETE_NO_DIALOG`：① 白名单里必须有
  `BmkRemove`；② 三个 UI 删除语句块里不得出现 `MessageBoxW` /
  `MarkConfirmedForNextCall`。按"调用点所在最小语句块"判而非整文件 ——
  同文件里清全部/解散分类的确认框是合规的，不能连坐。
- 负向验证：注入 3 个破坏点（白名单条目删除 / UI 弹框复发 / 确认标记复发）
  → **3/3 全部捕获**，还原后回绿。

