---
status: current
baseline: 2026-11-21 11:00
---

# ExBarBrowser 架构

> **注意（2026-11-21 10:05 快照）**：模块清单已扩展到 **61 个 `.cpp` / 53 个 `.h`（约 2.33 万行）**（仓库在活跃并行开发，以 `ls src/*.cpp | wc -l` 实测为准），
> 最新目录以 [README §6](../README.md) 为准；本文的窗口树、导航时序、COM 所有权、ABI 补丁
> 与不变量仍然有效，新增的 `ai_api`/`ai_server`/`nav_path`/`nav_history`/`shell_input`/`disk_scan`/
> `folder_tree`/`popup_toggle`/`icon_resolve`/`menu`/`identity.h` 与基础设施收编
> （`platform_win`/`str`/`log`/`clipboard`/`ui_draw`/`ui_palette`/`tooltip_host`/`icon_glyph`）
> 见各自文件头注释与 [README §5](../README.md) 的架构图。
> 命名约束（品牌串唯一来源、`eb` 待拆解）见 [NAMING.md](NAMING.md)。
>
> 目标读者：要改代码的人。看完应能回答三个问题——状态存在哪、消息怎么流、为什么这几处写法看起来"反常"。
> 本文描述**当前实现**；**新增代码请遵守 [ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md)**（R1–R21 硬规则），
> 目标形态与组件/AI 设计见 [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)。
> 模块清单与构建细节见 [../README.md](../README.md)；决策理由见 [DECISIONS.md](DECISIONS.md)。

---

## 1. 一句话架构

**一个进程、一个 UI 线程、三层窗口：应用层编排状态，chrome 层只画自己的矩形，视图层把 `IExplorerBrowser`
包成"一个标签"**。所有真实文件操作与右键菜单都在 shell 里，本工程不重写视图。

```
┌──────────────────────────── 进程 ────────────────────────────┐
│ main.cpp  DPI / COM(STA) / 激活优先启动 / 命令行              │
│                                                              │
│ App（单例，唯一状态所有者）                                   │
│  ├ Theme        颜色令牌 / 字体 / DPI 缩放（唯一视觉来源）      │
│  ├ Menu         自绘菜单内容组件（唯一组装 PopupRow，返回 action）│
│  ├ TabStrip     画 + 命中 + PostMessage（无状态）             │
│  ├ NavBar       画 + 命中 + 回调（无业务状态）                 │
│  │   ├ NavModel 路径 → 分段 / 父级 / 同级枚举                 │
│  │   └ PopupList 自绘下拉弹层（模态，返回选中行）              │
│  ├ StatusBar    画（无状态）                                  │
│  └ Tab[] ── ShellView ── IExplorerBrowser ── Shell 视图       │
└──────────────────────────────────────────────────────────────┘
```

分层依赖是单向的，编译期由头文件包含关系保证：

```
app.h ──▶ nav_bar.h ──▶ nav_model.h ──┐
      ├─▶ tab_strip.h                 ├─▶ theme.h ──▶ common.h
      ├─▶ status_bar.h                │
      └─▶ shell_view.h ───────────────┘
```

下层头文件**不得**包含上层头文件。`TabStrip` 想通知"用户点了第 3 个标签"时，只能
`PostMessage(parent, WM_APP_TAB_SELECT, 3, 0)`——消息常量在 `common.h`，它不需要认识 `App`。

---

## 2. 窗口树

```
ExBarBrowser_Main                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN
├─ ExBarBrowser_TabStrip             y=0,        h=S(32)   自绘
├─ ExBarBrowser_NavBar               y=S(32),    h=S(30)   自绘
│   └─ EDIT (地址编辑框)              默认隐藏，进入编辑才显示
├─ ExBarBrowser_ViewArea             y=S(62),    h=剩余    仅作容器，背景刷 = Theme::brBg
│   └─ ExBarBrowser_TabView × N      每个标签一个，互斥显示
│       └─ ExplorerBrowserControl    IExplorerBrowser 自己创建
│           ├─ NamespaceTreeControl  左侧导航树（EBO_SHOWFRAMES 带来）
│           │   └─ SysTreeView32
│           └─ SHELLDLL_DefView
│               └─ DirectUIHWND      文件列表
└─ ExBarBrowser_StatusBar            y=底部,    h=S(22)   自绘

ExBarBrowser_Popup                   WS_POPUP | WS_EX_TOOLWINDOW | CS_DROPSHADOW
└─ LISTBOX                           LBS_OWNERDRAWFIXED，下拉弹层（按需创建、用完销毁）
```

要点：

- **视图区是独立窗口而不是主窗口的客户区**：这样切标签只需 `ShowWindow` 子容器，主窗口的
  `WM_ERASEBKGND` 不会把 shell 视图刷掉，也避免 `WS_CLIPCHILDREN` 与 shell 自绘互相打架。
- **每个标签一个容器窗口**：`ShellView` 的容器类 `ExBarBrowser_TabView` 只处理 `WM_ERASEBKGND`
  （用主题背景刷填充，防止切标签瞬间白闪），其余全部交给 `DefWindowProcW`。
- **导航树在浏览器内部**：由 `SetOptions(EBO_SHOWFRAMES)` 让 shell 自己画，本工程不创建、不管理它。
  因此"左侧宽度记忆""展开状态"都由 shell 负责，不在我们的状态模型里。
- **下拉弹层是顶层窗口**（父窗口只是 owner，不是 parent）：这样它能超出主窗口边界、不被
  `WS_CLIPCHILDREN` 裁剪，也不需要处理"主窗口移动时跟随"——它在打开期间是模态的，用户动不了主窗口。

### 2.1 窗口类注册策略

| 窗口 | 注册方式 | `this` 绑定 |
|---|---|---|
| `Main` | `win::RegisterClassOnce(kMainClass, WndProc, 0, LoadAppIcon())` | 注册时确定 |
| `TabStrip` / `NavBar` / `StatusBar` / `BookmarkBar` | `win::RegisterClassOnce(<类>, <类>::WndProc, CS_DBLCLKS?)`，全部以**真实成员 WndProc** 注册 | `win::CreateChild(类, parent, this, hPx)`，创建时经 `lpCreateParams`/`SetWindowLongPtrW` 挂上 |
| `ViewArea` / `TabView` | `RegisterClassOnce`，`DefWindowProcW` 或轻量 `ViewProc` | 注册时确定 |
| `Popup` / `Prompt` / `SeqHint` / `Hotkeys` | `RegisterClassOnce` + `win::CreatePopup(..., self, ...)` | 经 `CREATESTRUCTW::lpCreateParams`，在 `WM_NCCREATE` 里落 `GWLP_USERDATA` |

2026-11-20 之前的写法是"注册时填 `DefWindowProcW`，创建后用 `SetWindowLongPtrW(GWLP_WNDPROC)` 换掉"，
**这不是必要手段**：窗口还没创建，直接注册真实 `WndProc` 即可。现在只剩一处例外 ——
`NavBar` 给原生 `EDIT` 装子类（`SetWindowLongPtrW(edit_, GWLP_WNDPROC, EditProc)`），
那是子类化别人的窗口类，本来就必须这么做。

`CreatePopup` 必须用 `lpCreateParams` 传 `this` 而不是创建后补设：`WM_NCCREATE` 发生在
`CreateWindowExW` **返回之前**，任何在该消息里就要用 `this` 的窗口过程会拿到空指针
（`popup_list.cpp` / `prompt.cpp` 都在 `WM_NCCREATE` 里读它）。

`this` 存在 `GWLP_USERDATA`（`SetThis` / `GetThis<T>`），`WndProc` 取不到就回退 `DefWindowProcW`——
这条回退不是防御性代码，而是**创建过程中的真实路径**（`WM_CREATE` 早于子控件的 `SetThis`）。

窗口类名全部来自 `src/identity.h`（11 个 `id::kXxxClass`），本文件不再定义任何字面量。

---

## 3. 模块职责

| 文件 | 职责 | 明确不做 | 依赖 |
|---|---|---|---|
| `identity.h` | **品牌串、11 个窗口类名、互斥体、管道前缀、日志文件名、`WM_COPYDATA` 魔数**的唯一来源（`namespace id`） | 任何行为、任何 I/O | `<windows.h>` |
| `common.h` | `WM_APP_*`、`CopyDataPayload`、`ComPtr`、`DpiOf`、PIDL/路径工具、`SystemIsDark`、`ApplyDarkTitleBar`；并把 `identity.h` 的类名 `using` 出来供既有代码沿用 | 任何业务、品牌串定义 | 系统头、`identity.h` |
| `theme.h/.cpp` | 颜色令牌、字体、DPI 缩放 `S()`、进程级深色；**图标尺寸令牌**（`iconChevron` / `iconGlyph` / `iconCompact` / `iconSmall`，96 DPI 逻辑值，四档按角色分）。字形表只管码位、`DrawGlyph` 只管画法，"画多大"归这里 | 布局、绘制 | `common.h` |
| `icon_resolve.h/.cpp` | **图标识别的唯一入口**：友好名 → 命名空间 GUID → PIDL → `SHGetFileInfo`，四层分级解析（特殊命名空间/云盘 NSE 才有真实图标）。`ico::ParsingPath` 也在这里（友好名 → 解析串）；`Cached`/`ClearCache` 是菜单与树弹层唯一的路径图标入口 | 绘制（尺寸归 `Theme`、字形归 `icon_glyph`） | `common.h` |
| `icon_glyph.h/.cpp` | 字形码位表（`ico::IconId`）+ `ico::Glyph` 取名 | 取尺寸、绘制 | 无 |
| `ui_draw.h/.cpp` | **绘制原语唯一实现**（`namespace ui`）：`MeasureText` / `DrawGlyph` / `DrawChevron` / `FillRectColor` / `FrameRectColor` / `FillRound` / `DrawSeparator` / `DrawIconCentered` / `DrawIconVCentered` / `PaintBuffered`（`GetUiFont` / `GlyphFace` 在 `ui::detail`，不对外）。图标绘制只有一套规则：落笔尺寸 = 图标真实像素，槽宽只作上限不放大（`DrawIconFit` 已并入）。原 `eb_ui.*` + `namespace eb`（= Ex**B**ar，上一个项目的前缀），2026-11-20 改名 | 任何栏的业务布局、任何输入状态读取、色值 | `common.h`、`icon_glyph.h` |
| `ui_palette.h` | 色值令牌表（`UiPalette`）：栏基础色 / 编辑框 / 选中 / 分隔线。原 `eb_theme.h` | 逻辑与布局 | `common.h` |
| `tooltip_host.h/.cpp` | `ui::TooltipHost`：一次全量替换 tooltip 项，内部管好 id 与字符串生命周期 | 任何绘制 | `<windows.h>` |
| `mouse_rules.h/.cpp` | 鼠标规则模型 + **`CurrentMods()` 唯一实现**（含 AltGr 特判：右 Alt+左 Ctrl 是字符输入，返回 0） | 绘制 | `common.h` |
| `platform_win.h/.cpp` | `RegisterClassOnce` / `CreateChild` / `CreatePopup` / `ThemeControl` | 任何栏的业务 | `<windows.h>` |
| `shell_view.h/.cpp` | 一个标签 = 一个 `IExplorerBrowser`；导航、刷新、显隐、尺寸、主题、事件接收 | 标签顺序、历史、标题 | `theme.h` |
| `tab_strip.h/.cpp` | 标签栏生命周期与模型装配：建窗、吃 `core::TabRef`、图标缓存（DPI 变化时整批重取）、快速关闭宽度表、`WM_APP_TAB_*` 投递 | 绘制、命中、几何、菜单（各自分片） | `theme.h`、`icon_resolve.h` |
| `tab_strip_input.cpp` | 消息分发（含 `WM_CONTEXTMENU` 键盘菜单、焦点消息）、悬停态、命中测试（谁被点到） | 标签状态 | `mouse_rules.h` |
| `tab_strip_layout.cpp` | 标签宽度分配（含 rapid close 临时宽度）、溢出判定、滚动量、tooltip 热区与增量移动 | 绘制 | `theme.h` |
| `tab_strip_paint.cpp` | 底色/图标/标题/关闭钮/锁定字形/溢出箭头/新建 +/键盘焦点框 | 任何状态写入 | `ui_draw.h` |
| `tab_strip_actions.cpp` | 标签右键菜单、溢出 » 菜单（只描述动作与禁用态，只回传 action） | 执行业务、手写行 | `menu.h` |
| `nav_path.h` | **路径语义唯一实现**（纯字符串）：`IsNamespace`/`IsDriveRoot`/`Normalize`/`ParentOf`/`Leaf`/`UpTarget`/`CanNavigateUp`/`kThisPc` | 任何 I/O、Shell、窗口 | 无（只有 `<string>`） |
| `nav_history.h/.cpp` | **历史栈组件**（每标签一份）：`Request`/`Goto`/`Complete`/`Fail`/`ClearKeepingHere`/`Restore`；压栈只有 `Push` 一处 | Shell、窗口 | `nav_path.h`（仅语义） |
| `nav_model.h/.cpp` | 路径 → 面包屑分段、子目录枚举、地址文本、ghost 段（**路径语义转发到 `nav_path.h`**） | 绘制、菜单 | `nav_path.h`、`common.h` |
| `nav_bar.h/.cpp` | 5 个按钮 + 地址框内的面包屑：窗口、布局、命中；地址编辑框宿主；主题重建 | 绘制、导航语义 | `theme.h`、`nav_model.h`、`nav_metrics.h` |
| `nav_bar_draw.cpp` | 导航栏绘制：`Paint` / `DrawButton` / `DrawBreadcrumb` / `DrawGlyph` / `DrawChevron` | 布局与命中 | `nav_bar.h` |
| `nav_metrics.h` | 导航栏 96 DPI 基准尺寸常量（`kBtn96` 等） | 任何逻辑 | 无 |
| `nav_bar_actions.cpp` | 编辑提交/取消、`Tab` 补全、分段子目录/折叠/历史/视图菜单（菜单走 `ui::Menu`，树走 `folder_tree`） | 绘制、状态变更、手写行 | `nav_bar.h`、`menu.h`、`folder_tree.h` |
| `menu.h/.cpp` | **自绘菜单内容的唯一构建层**：`Add` / `AddChecked` / `AddPath` / `AddPathChecked` / `Separator` / `AppendLabel` / `Show`，统一勾选前缀、禁用态、初态行、路径图标与 action 返回 | 渲染与模态（归 `PopupList`）、业务执行 | `icon_glyph.h`、`icon_resolve.h`、`popup_list.h` |
| `popup_list.h/.cpp` | 自绘下拉弹层唯一渲染器：`WS_POPUP` + owner-draw `LISTBOX`，模态循环，返回选中行；平铺内容由 `Menu` 提供，树内容由 `folder_tree` 提供 | 菜单内容组装（归 `Menu`）、业务语义 | `theme.h`、`icon_glyph.h` |
| `popup_toggle.h/.cpp` | **「再点即收」的唯一状态**：只回答"某锚点的弹层是否开着"（单槽，无时间戳），供 `Show*` 兜底与面包屑箭头形态（› / ⌄）使用 | 消息捕获（在 `popup_list`）、业务 | `common.h` |
| `folder_tree.h/.cpp` | **子目录树弹层的唯一入口**：`Children` 建树 + `Show` 弹层，懒加载回调就是 `Children` 自己。图标走 `ico::Cached`，弹层只借不销毁。面包屑 chevron 与书签栏图标格共用 | shell 枚举（`NavModel::ChildFolders` 是唯一实现）、图标生命周期 | `nav_model.h`、`popup_list.h`、`icon_resolve.h` |
| `status_bar.h/.cpp` | 9 段状态栏：排版、绘制、命中、tooltip、段显隐 | 取数（由 `view_stats` + `App` 给） | `theme.h` |
| `view_stats.h/.cpp` | `IFolderView2` / `IShellItem2` 取数：计数、选中、容量、日期、排序方向、磁盘 | 绘制 | `common.h` |
| `bookmark_bar.h/.cpp` | 书签栏：图标/分类下拉/溢出/右键/拖入文件夹（菜单走 `ui::Menu`，树走 `folder_tree`） | 书签读写（`bookmarks`）、手写行 | `bookmarks.h`、`theme.h`、`menu.h`、`folder_tree.h` |
| `bookmarks.h/.cpp` | 书签树 + Netscape HTML 读写（`EXBAR_*`） | UI | `text_file.h` |
| `bookmark_io.h/.cpp` | 导入导出 `IFileOpenDialog`/`IFileSaveDialog`（Netscape HTML，与存储文件同格式互通） | 书签树模型（`bookmarks`）、手写行 | `bookmarks.h`、`menu.h` |
| `bookmark_drop.h/.cpp` | OLE `IDropTarget`：接 shell 项（含虚拟/命名空间项，走 `CFSTR_SHELLIDLIST`；`WM_DROPFILES` 只带 `CF_HDROP` 不够用）拖入添加 | 书签树模型 | `bookmarks.h` |
| `tab_set.h/.cpp` | 标签顺序 / 锁定 / MRU / 关闭历史（零 Win32） | 视图、消息 | 标准库 |
| `shell_ops.h/.cpp` | 文件操作：复制/剪切/粘贴/删除/重命名/属性/新建/选择/排序/分组/隐藏开关 | 自绘 UI | `view_stats.h` |
| `commands.h/.cpp` | 命令目录（id/name/group/scope/implemented）+ `CommandBus`（派发 + 审计） | 业务实现 | `config.h` |
| `chord.h/.cpp` | 组合键解析/格式化/与消息比对/保留键 | 键位表 | 系统头 |
| `keymap.h/.cpp` | 读 `data/keymap.json` + 系统原生键参考；兜底默认表来自生成头 `keymap.gen.h` | 绑定策略 | `json.h`、`keymap.gen.h` |
| `keymap.gen.h`（生成） | `data/keymap.json` 的 C++ 镜像：`kHotkeys` / `kSeqs`。**勿手改**，跑 `tools/gen_keymap.py` | 业务 | — |
| `sequence.h/.cpp` | 序列键状态机（纯逻辑） | UI | `keymap.h` |
| `sequence_hint.h/.cpp` | 序列候选提示弹层（无激活、置顶） | 状态机 | `theme.h` |
| `sequence_overlay.h/.cpp` | 序列模式总览弹层（打字过滤/导航/派发） | 注入的取数/派发回调 | `theme.h` |
| `mouse_rules.h/.cpp` | 鼠标规则模型 + 内置默认 + `mouse.json` 解析 + **`CurrentMods()` 唯一实现**（含 AltGr 特判：右 Alt+左 Ctrl 是字符输入，返回 0），因它读 `GetKeyState` 属输入端（R15） | 时序、绘制 | `json.h` |
| `input_router.h/.cpp` | 键盘/序列/鼠标总路由：单击延迟、双击、长按、滚轮、超时 | 业务状态 | `commands.h`、`mouse_rules.h` |
| `hotkey_manager.h/.cpp` | 快捷键管理器（筛选/改键/清除/恢复默认） | 键位表实现 | `input_router.h` |
| `prompt.h/.cpp` | 文本输入对话框（书签重命名/新建分类） | 业务 | `theme.h` |
| `config.h/.cpp` | 设置/会话/最近访问/键位覆盖；`<exe>\data` 路径 | UI | `json.h`、`text_file.h` |
| `json.h/.cpp` | 极简 JSON（读 keymap/settings/mouse） | 流式/注释 | 标准库 |
| `text_file.h/.cpp` | UTF-8 读写（原子替换）+ 路径工具 | 业务 | 系统头 |
| `app.h/.cpp` | 装配与生命周期：单例、窗口创建、`OnCreate/OnDestroy`、鼠标入口 | 绘制细节、shell 调用细节 | 全部 |
| `app_wnd.cpp` | 主窗口过程：消息分发、定时器、DPI 变化（只路由到成员函数） | 业务 | `app.h` |
| `app_tabs.cpp` | 标签生命周期：开/关/切/移动/锁定、`SyncChrome`（只喂模型，图标归 `TabStrip`） | 绘制细节、导航历史细节 | `app.h` |
| `app_nav.cpp` | 导航编排：`NavigateActive`/`GotoHistory`/`OnTabNav(Failed)`。**只做编排** —— 路径语义在 `nav_path.h`、历史事务在 `nav::History`，本文件不手写 `push_back`/`resize` | 标签生命周期 | `app.h` |
| `app_layout.cpp` | 布局与主题：四条栏高度/显隐、视图区回收、DPI/深浅色 | 栏内部绘制 | `app.h` |
| `app_stats.cpp` | 状态栏取数：枚举统计 + 节流 + `StatusModel` 填充 | 布局、标签 | `app.h` |
| `app_commands.cpp` | 命令注册调度 + 通用动作（`ActiveView`/`WithView`/`CopyText`）+ 四个菜单（走 `ui::Menu`） | 各组命令的细节、手写行 | `app.h`、`menu.h` |
| `app_commands.h` | 分片共享的 `cmd_reg::Register`（各片写 `auto reg = ...`） | 业务 | `commands.h` |
| `app_commands_tabs.cpp` | 标签 / 跳转 / 导航 / 路径 四组命令 | 视图内文件操作 | 同上 |
| `app_commands_files.cpp` | 文件操作 / 视图 两组命令（都经 `App::WithView` 判成败） | 标签与历史 | 同上、`shell_ops.h` |
| `app_commands_misc.cpp` | 窗口/系统、外部程序（`RunTerminal`）、书签、会话 | 单视图操作 | 同上、`bookmarks.h` |
| `main.cpp` | DPI、`InitCommonControlsEx`、进程深色、COM、激活优先启动（转发或新开）、命令行 | 窗口逻辑 | `app.h` |

行数约束：单文件 ≤300 行（[ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md) R3）。
2026-11-20：`app.cpp` 已按 `app_wnd` / `app_tabs` / `app_nav` / `app_layout` / `app_stats`
五片拆开（每片 ≤300 行）；标签图标（HICON）所有权下沉到 `TabStrip`，`App` 不再持有。
同日继续：`tab_strip` 按 输入/几何/绘制/菜单 拆成五片；`app_commands` 按命令域拆成
四片（核心/标签/文件/杂项），`tools/check_commands.py` 改为审计 `app_commands*.cpp` 全片。

目前有 **10 个文件超 500 行**（名单见下表）。分布与具体行数以现查为准，本表**只记名单与拆分方向**，不记数字。

| 文件 | 拆分方向 |
|---|---|
| `shell_ops.cpp` | 选择族 / 文件操作 / 属性查询 |
| `shell_view.cpp` | IExplorerBrowser 托管 / 主题下传 / 窗格可见性 / 拖放接收 |
| `popup_list.cpp` | 布局命中 / 绘制 / 二级展开 / 键盘导航 |
| `bookmark_bar.cpp` | 绘制 / 命中 / 菜单 |
| `bookmarks.cpp` | Netscape 解析 / 树模型 / 持久化 |
| `commands.cpp` | 目录表（纯数据）可以独立成 `.inc` |
| `app_commands_files.cpp` | 文件操作 / 视图开关 |
| `shell_input.cpp` | UIA 探测 / ListView 探测 / 子类生命周期 |
| `input_router.cpp` | 键 / 序列 / 鼠标三条入口 |
| `nav_bar.cpp` | 绘制已在 `nav_bar_draw`；剩 命中/布局/回调 |

> **这张表故意不写行数**：本仓库处于活跃并行开发，行数每天都在涨，写进来第二天就是错的。
> 要当前值跑 `wc -l src/*.cpp | sort -rn | head -12`；要看分布跑
> `wc -l src/*.cpp | awk '$2!="total"{if($1<=300)a++; else if($1<=500)b++; else c++} END{print a,b,c}'`。

超限不是罪，**继续往上加才是**：下次动这些文件先拆，别在 1000 行的文件里再塞一个分支。
当前 10 个超 500 的文件都在"下次要动就先拆"的名单里（`CONTRIBUTING.md` §3.1 有同一份清单）。

---

## 4. 导航数据流

一次"用户在地址框输入 `C:\Windows` 并回车"：

```
EDIT WM_KEYDOWN(VK_RETURN)
  └ NavBar::EditProc → CommitEdit()
      ├ AddressOfEdit()  取文本、去尾空格
      ├ EndEdit()        隐藏编辑框、清补全缓存、重排
      └ onNavigate_(text)                      ← std::function，App 注入
          └ App::NavigateActive(path, push=true)
              ├ 截断前进分支 hist.resize(idx+1)
              ├ hist.push_back(path); idx = size-1
              └ Tab::view->Navigate(path)
                  └ ShellView::DoNavigate()
                      ├ 规范化：正斜杠→反斜杠、去尾分隔符（命名空间跳过）
                      ├ SHCreateItemFromParsingName → IShellItem
                      └ IExplorerBrowser::BrowseToObject(item, 0)
                                        │
                          （shell 异步完成导航）
                                        ▼
                  IExplorerBrowserEvents::OnNavigationComplete(pidl)
                      └ ShellView::OnNavigated(pidl)
                          ├ path_   = PidlName(pidl)           解析名
                          ├ display_= SIGDN_NORMALDISPLAY      友好名
                          └ navFn_(path_, display_, ItemCount())
                              └ App::OnTabNav(...)
                                  ├ 若路径与 hist[idx] 不同：push（shell 内部跳转也要记历史）
                                  ├ 若相同：用 shell 规范化写法覆盖（C:/x → C:\x）
                                  ├ t.items / t.display 更新
                                  └ i == active_ ? SyncChrome() : tabStrip_.SetTabs(...)
                                      └ SyncChrome()
                                          ├ SetWindowText("标题 — ExBar")
                                          ├ tabStrip_.SetTabs(Titles(), active_)
                                          ├ navBar_.SetPath(path, display)   → NavModel 重建分段
                                          └ statusBar_.SetText("N 个项目", 地址文本)
```

**为什么要"App 也记一次历史"**：用户可能在视图里双击进入子目录、点导航树、按 Backspace——这些都不经过
`App::NavigateActive`，只能靠 `OnNavigationComplete` 回填。因此历史有两个写入点（主动导航 + 导航完成），
两者用"路径是否等于 `hist[idx]`"去重。

**失败路径**：`DoNavigate` 在解析或 `BrowseToObject` 失败时返回 `false`；`NavigateActive` 弹一个
`MessageBoxW` 并把刚压入的历史项弹掉。日志里能看到对应 `hr`。

### 4.1 下拉弹层（`popup_list`）

所有自绘菜单的**内容**统一由 `ui::Menu` 构建，**渲染/模态**统一由 `PopupList` 承担：
平铺列表走 `Menu::Show → ShowPopupList`，文件夹树走 `folder_tree::Show → ShowFolderTree`，
**不用 `TrackPopupMenu`**。原因：系统菜单带不了图标、配色跟不了主题、行高不可控；
而 mod 的 `ebpopup::ShowPopupList` 已经是"图标 + 24 px 行 + 悬停/选中底色"的形态，两边要视觉一致。

调用形状（业务代码只描述动作，不接触 `PopupRow` 八字段）：

```cpp
ui::Menu menu;
menu.AddPath(LabelFor(path), path, action)   // 路径图标走 ico::Cached
    .Separator()
    .Add(L"清除历史记录", 3000, ico::IconId::Close);

ui::MenuOptions opt;
opt.toggleKey = L"nav.drop";
opt.initialRow = currentRow;
const int action = menu.Show(hwnd_, *theme_, screenPt, opt);  // 返回 action，不是行号
```

内部机制：

| 环节 | 做法 |
|---|---|
| 定位 | `MonitorFromPoint` + `rcWork` 夹取；下方放不下就翻到上方（`y = screen.y - height`） |
| 宿主 | `WS_POPUP` + `WS_EX_TOOLWINDOW` + `CS_DROPSHADOW`，父窗口只是 owner |
| 内容 | `LISTBOX` + `LBS_OWNERDRAWFIXED`，`LB_SETITEMHEIGHT` 统一 24 px；超过 14 行加滚动条 |
| 绘制 | `WM_DRAWITEM` 画行（图标/文字/chevron/分隔线）；`WM_NCPAINT` 画 1 px 边框 |
| 交互 | 子类化 `WM_MOUSEMOVE` 跟踪 hot 行；`WM_LBUTTONUP` 选中；`WM_KEYDOWN` 处理 `Esc`/`Enter`（方向键交给 ListBox 默认处理） |
| 关闭 | 选中、`Esc`、`Enter`、`WM_ACTIVATE(WA_INACTIVE)` 四种，统一 `PostMessage(WM_CLOSE)` |
| 返回 | 选中行索引；取消 `-1` |

**三条纪律**：

1. **它是模态的**：内部跑 `while (!done && GetMessage(...))` 嵌套消息循环。调用期间主窗口仍会收到消息
   （重绘、`WM_ACTIVATE`、定时器），所以**不要在 `WM_PAINT` 里调它**，也不要在调用点持有"只允许重入一次"
   的状态。
2. **内容归 `Menu` 所有**：业务侧构造的 `ui::Menu` 必须活到 `Show()` 返回。组件内部持有
   `PopupRow` 向量并交给渲染器；新增菜单不要再建 `std::vector<PopupRow>`。
3. **图标只有两条入口**：动作行写 `ico::IconId`，由 `ui::DrawGlyph` 复用字形表；
   路径行/树节点由 `ico::Cached` 给 `HICON`，进程内共享、弹层只借不销毁。
   旧写法里调用方 `Resolve` + `FreeRows`、弹层 `DestroyIcon` 混用，漏一处就是 GDI 泄漏或悬空句柄 ——
   现在这两条生命周期都收口在 `icon_resolve.cpp`。

**再点即收（`toggleKey`）**：所有弹层同一契约 —— **弹层开着时点它的锚点，收起；不重开**。

真因不是"连点判据"而是**关闭时机**。系统在点击派发前**同步**给弹层发
`WM_ACTIVATE(WA_INACTIVE)`；若在此就 `PostMessage(WM_CLOSE)`，消息会排在
`LBUTTONDOWN/UP` 之前被模态循环取走 → 弹层先销毁、捕获释放 → 剩下的 `LBUTTONUP`
落到锚点控件，而本工程各栏都在 **UP** 里执行动作 → 又调一次 `Show*` → 菜单"关了又开"。

**关键事实：鼠标捕获不阻止激活切换。** 弹层是 `WS_POPUP` 顶层窗口，点主窗任一子控件
都会让主窗激活、弹层失活，哪怕捕获仍在弹层手里 —— 所以"持捕获就安全"不成立。

修法（对齐 `TrackPopupMenu` / 组合框下拉）：失活时按 `popup::ShouldCloseOnDeactivate` 分流 ——

| 条件 | 处置 |
|---|---|
| 持捕获 + 前景窗同线程（点的是本程序另一个窗口） | **不关**。这组 DOWN/UP 由列表吞着，等 UP 判到"点在列表外"再关。锚点收不到点击，重开在消息层不可能发生 |
| 其余（点别的程序 / Alt+Tab / 捕获已被抢走） | 关。这类失活不会再有 UP 回到手上，留着就是关不掉的菜单 |

配套：`ListSubclass::WM_CAPTURECHANGED` 区分"LISTBOX 自己 SetCapture"（忽略）与
"被抢走"（关）；外点的 DOWN 只记 `downOutside`，等 UP 才关并释放捕获，于是
"点别处只关菜单、不触发被点的控件"（资源管理器同语义）。

`popup_toggle` 现在只回答"某锚点的弹层是否开着"，供 `Show*` 兜底（键盘/命令重入）与
面包屑箭头形态（› / ⌄）使用。锚点键带索引（`nav.crumb.3` / `nav.drop` /
`bmk-icon:<id>` / `tab.menu.1`），不同锚点互不干扰。详见 `docs/DEVELOPMENT.md` §3.4。

**文件夹树（`ShowFolderTree`）**：面包屑分隔符下拉用的是树形态——顶层是**该分段自己的**
直接子文件夹（`NavModel::ChildFolders(parts[partIndex].path)`，与 mod 的
`EnumChildFoldersAt(crumbPidls[partIndex])` 同口径），点行右端 chevron 就地展开下一级。
它是另一个弹层**形状**，但共用同一套渲染、捕获、定位与"再点即收"逻辑；节点图标同样来自
`ico::Cached`，解析失败时用 `TreeNode::glyph`（`ico::IconId`）兜底，弹层不销毁任何 `HICON`。
`PopupRow::folder` 的平铺形态没有二级弹出，只用于不需要展开的简单列表。

---

## 5. 标签生命周期

```
NewTab(path)
  ├ make_unique<Tab> + make_unique<ShellView>(theme_)
  ├ ShellView::Create(viewArea_, path)
  │   ├ 建容器窗口（WS_CHILD，初始 10×10 不可见尺寸）
  │   ├ InitBrowser()：CoCreateInstance + vtable[3] Initialize + EBO_SHOWFRAMES + SetPropertyBag
  │   ├ new Sink(this) → Advise → 交引用给浏览器
  │   └ 首次 Navigate(path)（可能同步完成，回填 path_/display_）
  ├ 历史首项优先取 view->path()（shell 规范化后的写法）
  ├ tabs_.push_back
  └ Activate(size-1) → 逐个 SetVisible + SyncChrome + SetFocus(视图)
```

| 动作 | 实现 | 副作用 |
|---|---|---|
| 切换 | `App::Activate(i)` | 所有视图 `SetVisible(i==active)`；`SW_SHOWNA` 不抢焦点，随后显式 `SetFocus` |
| 关闭 | `App::CloseTab(i)` | `tabs_.erase` 触发 `ShellView::~ShellView` → `Destroy()`：`Unadvise` → `browser_->Destroy()` → `Release` → `DestroyWindow(container)` |
| 关到空 | `DestroyWindow(hwnd_)` | `WM_DESTROY` 里 `tabs_.clear()` + `theme_.Shutdown()` + `PostQuitMessage` |
| 外部打开 | `App::OpenExternal(path)` | 直接 `NewTab`（复用已有窗口，不新起进程） |

**不销毁隐藏标签**：切标签只是 `SW_HIDE`。理由：`IExplorerBrowser` 的重建成本高（枚举 + 视图初始化），
且用户期望"切回来还是原来的滚动位置与选中项"。代价是内存随标签线性增长，已在
[ROADMAP.md](ROADMAP.md) P2 记为待优化项（懒加载 / LRU 回收）。

**注意析构顺序**：`App::WM_DESTROY` 先 `tabs_.clear()` 再 `theme_.Shutdown()`。`ShellView` 持有
`const Theme&`，主题先销毁会让 `ApplyTheme`/`EraseBackground` 读到已删除的画刷。

---

## 6. 历史模型与 ghost 段

```cpp
struct Tab {
    std::unique_ptr<ShellView> view;
    std::vector<std::wstring> hist;   // 线性历史，元素是解析名
    int idx = -1;                     // 当前位置
    unsigned items = 0;               // 项目数（状态栏用）
    std::wstring display;             // 友好名（标签标题 / 命名空间地址）
};
```

- **新导航**：先截断 `idx` 之后的分支，再 `push_back`，`idx` 指向末尾。这是浏览器语义（前进栈被丢弃）。
- **后退/前进**：只移动 `idx` 再 `Navigate`，不 push。导航完成回调看到路径等于 `hist[idx]`，不会重复入栈。
- **历史菜单**：`HistoryList(backward)` 返回"最近的在前"的列表（后退取 `idx-1..0`，前进取 `idx+1..end`），
  UI 侧最多显示 24 条；`JumpHistory(depth, backward)` 把"菜单第 N 项"换算成绝对索引。
- **ghost 段**：`NavModel::SetPath` 发现新路径正好是旧路径的父级时，把旧路径记进 `ghostPath_`，
  面包屑尾部多画一段灰显的叶子。任何其他路径变化都清空。这是 Directory Opus 风格的"上一步在哪"提示，
  不参与导航（点击它等于回到那个目录，是正常导航）。

历史是**每标签独立**的，不共享；没有全局 MRU。

---

## 7. 主题与 DPI

`Theme` 是唯一视觉来源。令牌（2026-11-19 本机实测值，不是抄来的标准值）：

| 令牌 | 深色 | 浅色 | 用途 |
|---|---|---|---|
| `bg` | `#141414` | `#ffffff` | 文件区 / 视图底 / 激活标签 |
| `bar` | `#1e1e1e` | `#f3f3f3` | 标签栏 / 导航栏 / 状态栏底 |
| `barAlt` | `#262626` | `#ffffff` | 地址编辑框底 |
| `hover` | `#2d2d2d` | `#e8e8e8` | 悬停高亮 |
| `pressed` | `#3a3a3a` | `#dcdcdc` | 按下态 |
| `line` | `#303030` | `#dcdcdc` | 分隔线 |
| `text` | `#f0f0f0` | `#1a1a1a` | 主文字 |
| `sub` | `#9d9d9d` | `#5f5f5f` | 次要文字 / 图标 |
| `disabled` | `#6b6b6b` | `#a0a0a0` | 禁用文字 / ghost 段 |
| `chevron` | `#a8a8a8` | `#6a6a6a` | 面包屑 chevron / 折叠钮 |
| `accent` | `#4cc2ff` | `#0067c0` | 激活下划线 / 编辑框焦点环 |
| `editBg` | `#1f1f1f` | `#ffffff` | 地址框底（`brEdit`） |
| `editBorder` | `#333333` | `#d0d0d0` | 地址框 / 弹层边框 |
| `listBg` | `#2b2b2b` | `#ffffff` | 弹层底（备用；当前行底用 `bar`） |
| `listHot` | `#3a3a3a` | `#f0f0f0` | 弹层悬停行 |
| `listSel` | `#2a4a66` | `#cce8ff` | 弹层选中行 |

字体：`font` 9 pt Segoe UI、`fontBold` 9 pt Semibold（当前未用于面包屑，保留给标题类场景）、
`glyphFont` 10 pt **Segoe MDL2 Assets**（向上 `0xE74A`、视图 `0xE700` 等字形；缺字体时退化成方框）。

这套值对应 ExBar 的 `ui/theme/tokens.css` 口径。**改色只改 `theme.cpp` 一处**，不要在控件里写 `RGB()`。

### 7.1 深色是怎么生效的

三层，缺一层就会出现"工具栏深、文件区白"的割裂：

1. **进程级**：`ApplyProcessDarkMode` 调 uxtheme 序号 135
   `SetPreferredAppMode(2=ForceDark / 3=ForceLight)`。启动时必须在任何窗口创建之前调用
   （`main.cpp` 里紧跟 profile 选择）；运行时深浅切换必须**先拆掉所有 EBC，再切模式，最后重建**。
   顺序反了会让 shell 用旧 DirectUI 的颜色缓存创建新视图，表现就是"只有文件区不变，重启才好"。
2. **窗口级**：`AllowDarkForWindow`（序号 133）逐个窗口放行，容器用
   `SetWindowTheme(h, L"DarkMode_Explorer" / L"Explorer")`。只对 `ExplorerBrowserControl`
   这一层设主题；**不要**给 `SHELLDLL_DefView` / `DirectUIHWND` 设主题或发 `WM_THEMECHANGED`，
   实测会破坏选中高亮绘制（见 `shell_view.cpp` 的 `ApplyShellTheme` 注释与
   `tools/probe_highlight.cpp` 的像素差分）。
3. **运行时重建**：`App::ApplyTheme()` 三段式编排 ——
   `ShellView::SuspendForThemeSwitch()`（保存 PIDL/视图模式/几何/搜索 PIDL，拆 EBC）→
   `ApplyProcessDarkMode()` → `ShellView::ApplyTheme()` / `FinishThemeSwitch()`（连容器一起重建，
   延迟回灌状态）。`RefreshImmersiveColorPolicyState` / `FlushMenuThemes` 只作为 uxtheme
   缓存刷新的补充。**不要在运行时广播 `WM_SETTINGCHANGE("ImmersiveColorSet")`**：
   `HWND_BROADCAST` 会逐个等所有顶层窗口，实测单次 2.7~3.5s；旧 EBC 已先拆掉，
   广播不再是正确性的必要条件。

取不到序号 135/133（旧系统、被安全软件拦）时静默降级：chrome 仍按 `Theme::dark` 自绘，shell 视图保持浅色。
这是可接受的降级，不是 bug。

### 7.2 DPI

- 进程声明 Per-Monitor V2（`SetProcessDpiAwarenessContext(-4)`），失败退 `SetProcessDpiAwareness(2)`，
  再退 `SetProcessDPIAware()`。
- `Theme::dpi` 在 `Init` 时用 `DpiOf(hwnd)`（`GetDpiForWindow` 动态取，取不到按 96）。
- 所有像素都过 `Theme::S(px) = MulDiv(px, dpi, 96)`。
- 字体只跟 DPI/系统消息字体有关，与深/浅色无关：`Theme::Reload()` 在 DPI 未变时
  **保留字体句柄**，只有 DPI 真的变了才重建并递增 `fontGeneration`。地址栏 EDIT
  挂了 `SHAutoComplete`，收到新 `WM_SETFONT` 会重建补全列表（实测 110~170ms/次），
  所以 `NavBar::ReloadTheme()` 会在 `fontGeneration` 没变时跳过 `WM_SETFONT`。
  判据必须是代次而非 HFONT 值：GDI 删除对象后句柄会复用，句柄相同不代表字体没重建。
  切主题因此不必重造字体。
- `WM_DPICHANGED` 时：`ApplyTheme()`（重建令牌/字体/画刷 + 通知 shell）→ 按系统建议矩形 `SetWindowPos` → `Layout()`。
- `WM_GETMINMAXINFO` 的最小尺寸也走 `S()`，避免高 DPI 下窗口小到 chrome 互相重叠。

---

## 8. 启动形态与命令行

```
 WinMain
 ├ EnableDpiAwareness
 ├ InitCommonControlsEx(ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES)
+├ 创建 Local\ExBarBrowser.StartupMarker（只在进程期持有，不是单实例锁）
+├ OleInitialize(nullptr)              剪贴板 + 拖放需要的 OLE
+├ InitialPath()                       命令行解析
+├ profile 判定（PROFILE > MULTI > 默认 data\）
+├ ApplyProcessDarkMode(ThemePrefersDark())  必须在 profile 之后、任何窗口之前
+├ forceNew = EXBARBROWSER_MULTI 或 --new-window
+└ !forceNew && ForwardToExisting(path, hadEarlierInstance ? 400ms : 0) ? 转发+激活后退出 : App::Run
```

- **没有单实例锁**：不存在"已在运行 → 拒绝启动"的分支。默认启动是**激活优先** —— 找得到已有
  主窗口就把路径经 `WM_COPYDATA` 转发过去开新标签并前置窗口，新进程随后退出（360 行为）；
  找不到就直接新开实例。`Local\ExBarBrowser.StartupMarker` 只回答"是否已有更早实例"：
  只有后进者为"首实例还没建好窗口"等最多 400ms，首实例用 0 超时，冷启动不空等。
  句柄随进程退出关闭，因此对象不会跨会话残留。
- **显式多开**：`--new-window`（"在新窗口打开"命令启动子进程时自动带上，共享同一份用户数据）
  或 `EXBARBROWSER_MULTI`（同时落到独立 `data\profile-multi`，用于与正跑着的实例隔离测试）。
- **转发用 `SendMessageTimeout(SMTO_ABORTIFHUNG, 2s)`**：目标卡在模态框时不拖死新进程；
  处理结果必须校验（返回 FALSE = 没转成 → 按"没有实例"处理，继续往下新开）。
- **payload 校验**：`dwData == kCopyDataMagic` + `payload->magic == kCopyDataMagic`，
  路径按 `cbData` 的实际长度读（**不**要求 `cbData == sizeof(CopyDataPayload)`），
  因此深路径不会被静默截断，新旧版本之间也能互相转发。
- **命令行**：`/select,<path>` 与 `/select <path>` 都取父目录；其他 `/xxx` 开关忽略；第一个非开关参数当路径；
  都没有则用 `FOLDERID_Profile`。

---

## 9. COM 与所有权

| 对象 | 创建 | 持有者 | 释放点 |
|---|---|---|---|
| `IExplorerBrowser` | `CoCreateInstance(CLSID_ExplorerBrowser)` | `ShellView::browser_`（裸指针，手动 `Release`） | `ShellView::Destroy`：先 `Destroy()` 再 `Release()` |
| `IExplorerBrowserEvents`（`Sink`） | `new Sink(this)` | 浏览器（`Advise` 后） | `Unadvise` 后由浏览器 `Release`；`ShellView` 不 delete |
| `IShellItem` / `IShellFolder` / `IEnumIDList` | `SHCreateItemFromParsingName` 等 | `ComPtr` 局部 | 作用域结束 `Reset` |
| PIDL | `SHParseDisplayName` / `ILCombine` | 裸指针 | `CoTaskMemFree` |
| `PWSTR`（名字） | `SHGetNameFromIDList` / `GetDisplayName` | 裸指针 | `CoTaskMemFree` |

`Sink` 的引用计数路径：`new`（1）→ `AddRef`（2，"我们的引用"）→ `Advise`（浏览器持有）→ `Release`（1）。
`Destroy` 时 `Unadvise`，浏览器释放它持有的那份，之后 `sink_ = nullptr`。**不要**在 `Unadvise` 之后
再 `sink_->Release()`——那份引用已经没了。

`ComPtr` 故意不支持拷贝，且 `operator&` 会先 `Reset`：`&item` 用在 `SHCreateItemFromParsingName` 这类
"出参"场景，语义就是"重新取"。这是刻意的窄接口，不要扩成通用智能指针。

---

## 10. 两处 ABI 补丁（不要"修好"它们）

### 10.1 `IExplorerBrowser::Initialize` 第三参

```cpp
// 真实签名：Initialize(HWND, const RECT*, FOLDERVIEWOPTIONS 按值)
// vtable: 0 QueryInterface / 1 AddRef / 2 Release / 3 Initialize
using InitFn = HRESULT(STDMETHODCALLTYPE*)(IExplorerBrowser*, HWND, const RECT*, int);
void** vt = *reinterpret_cast<void***>(browser_);
auto init = reinterpret_cast<InitFn>(vt[3]);
init(browser_, container_, &rc, 0 /* FVO_DEFAULT */);
```

mingw 的 `shobjidl.h` 把第三参声明成 `const FOLDERSETTINGS*`（指针），Windows 实际按
`FOLDERVIEWOPTIONS`（枚举，4 字节）传值。按头文件调用等于把指针当枚举塞进寄存器：轻则视图不显示，
重则崩溃。所以按 vtable 索引直接调。

**如果将来换 MSVC 或更新 mingw 头**：先验证头文件签名是否已修正，再决定是否删掉这段。

### 10.2 `OnNavigationComplete` 的参数个数

Windows 实际以 4 个参数调用 `OnNavigationComplete(PCIDLIST_ABSOLUTE, IDataObject*, UINT, ...)`，
mingw 头只声明 1 个。多出的参数走寄存器、被忽略即可，**不影响 vtable 布局**。因此 `Sink` 里
按头文件签名实现是安全的，不要为了"对齐"去改签名——改了反而可能与真实栈帧不符。

---

## 11. 线程与性能约束

- **单 UI 线程**：所有窗口、所有 COM（STA）、所有 shell 调用都在主线程。`Sink` 回调也在主线程。
- **禁止在 UI 线程做**：目录枚举（`NavModel::ChildFolders` 是例外——只在用户点开菜单时做一次，
  且只枚举直接子文件夹）、网络访问、`Sleep`、大文件 IO。
- **绘制**：`WM_PAINT` 里建内存 DC + `BitBlt`，`WM_ERASEBKGND` 返回 1（不让系统刷背景，避免闪烁）。
  实色画刷/画笔由 `ui_draw.cpp` 按 `COLORREF` / `(COLORREF, 线宽)` 缓存复用，不在每次绘制里创建销毁；
  `Theme` 里的画刷是长生命周期对象，只随主题重建。所有绘制层 GDI 句柄在 `ui::ClearGdiCaches()`
  中统一释放（主流程在 `App::Run()` 返回后调用）。
- **日志**：`common.h` 的 `LogDbg` 每次 `CreateFileW`/`WriteFile`/`CloseHandle`（UTF-16 LE 追加，
  `%TEMP%\ExBarBrowser.log`）。这是排障用的低频路径，不要在热路径（`WM_MOUSEMOVE`、`WM_PAINT`）里调。
  导航路径上不要仅为日志调用 `IShellItem::GetDisplayName()`；`GetPaneState()` 的逐窗格 GUID
  诊断输出也已移除。`shell_view.cpp` 里还留着一份功能重复的 `LogLine`（导航五步用它），P1.1 应合并到 `LogDbg`。
- **地址栏 lead icon**：`NavBar::UpdateLeadIcon()` 一律用 `ico::Cached()`，不自行销毁 HICON；
  句柄归进程级缓存所有，和书签栏/弹层共享同一份。这样切标签/导航不会重复做 shell 解析。
- **Shell 视图重绘**：普通导航/视图重建只 `InvalidateRect()`；`RDW_ALLCHILDREN | RDW_ERASE`
  只留给 `ShellView::FinishThemeSwitch()`，在进程深浅色切换后真正重建 EBC 时做一次。
- **常驻记录有上限**：`nav::History::kMaxEntries = 512`（超限时裁头部并同步 `idx_`，
  `Restore()` 同样裁剪）；`job::Service::kMaxRetainedRecords = 32`（只淘汰已完成且已 join 的旧记录）。
  这两类数据都会进 settings.json 或深拷贝进命令结果，不能随浏览时长无限增长。
- **GDI 失败路径**：`PaintBuffered()` 在 `CreateCompatibleDC` / `CreateCompatibleBitmap` /
  `SelectObject` 任一失败时，必须按已取得的资源逐个释放并 `EndPaint`；GDI 对象不在 RAII 里。
- **Tooltip 高频更新**：`TooltipHost::SetTools()` 先比对非空文本；未变化时只调 `MoveTools()`
  更新热区，不再反复 `TTM_DELTOOLW` / `TTM_ADDTOOLW`。状态栏 800ms 刷新与标签滚动走这条快路径。
- **切标签**：O(N) 次 `ShowWindow` + 一次 `SyncChrome`。N 是标签数，目前没有上限，P2 要加溢出处理。
- **弹层是嵌套消息循环**：打开期间主窗口的消息仍会被派发，等于在"菜单打开"这个时间窗口里重入了主循环。
  所以弹层打开前要把需要的状态拷进 `ui::Menu` 的行数据（不要持有指向 `model_` 的引用再在循环里读），
  也不要在循环期间销毁 `NavBar` 或 `Theme`。

---

## 12. 不变量（改动前逐条确认）

> 前 10 条是"改代码不能破的语义"；11–14 是**机械约束**（11–13 自 2026-11-20 起，
> 14 自 2026-11-21 起，均已由 `tools/check_components.py` 强制，违反即 CI 红）。

1. **状态只有一份**：标签历史/索引/标题/项目数只在 `App::Tab`；chrome 是纯投影，不缓存副本。
   历史栈的**唯一实现**是 `nav::History`（`App::Tab::nav`），路径语义的唯一实现是
   `nav_path.h` —— 任何地方都不得再手写 `hist.push_back` / `hist.resize` / 自算父目录。
2. **导航只有一个出口**：所有"去哪"的入口（地址栏、面包屑、书签、双击目录、
   工具栏按钮、热键、鼠标规则、命令总线、AI）最终都走 `App::NavigateActive` /
   `GoUp` / `GotoHistory`。工具栏按钮**也必须经过 `CommandBus`**，不得直调
   `GoUp()` —— 绕过总线就等于绕过前置检查，历史上这正是"按钮能点、双击没反应"
   的分裂来源。
3. **"能不能向上"只有一个谓词**：`nav::CanNavigateUp`（盘符根算可向上 = 去此电脑）。
   命令前置检查 `Req::CanGoUp` 与按钮灰显读同一个函数，"灰着"必须等于"点不动"。
4. **chrome 不调 shell**：`TabStrip`/`StatusBar` 不认识 `ShellView`；`NavBar` 只经 `std::function` 回调。
5. **`ShellView` 是唯一接触 `IExplorerBrowser` 的类**；其他模块不得包含 `shobjidl.h` 的浏览器接口。
6. **视觉只走 `Theme`**：颜色、字号、间距、圆角、线宽都从 `theme_` 取。
7. **窗口类名是外部契约**：`ExBarBrowser_*` 11 个类名被脚本、路径转发、自动化依赖；
   定义只在 `identity.h`，改一个名字要同步 `scripts/shot.py` 与文档（规则 R10）。
8. **路径内部一律反斜杠**，命名空间保持原样；跨模块传递路径不做二次规范化。
9. **`Theme` 的生命周期覆盖所有 `ShellView`**：析构顺序必须是"先视图后主题"。
10. **自绘菜单只有一条内容链**：平铺菜单一律 `ui::Menu::Add*` → `ShowPopupList`，树菜单一律
    `folder_tree` → `ShowFolderTree`，不再新增 `TrackPopupMenu`，业务侧不再直接建 `PopupRow`。
    动作行图标走 `ico::IconId`，路径行/树节点图标走 `ico::Cached`；弹层只借不销毁，
    `Menu` 必须活到 `Show()` 返回。`tools/check_components.py` 已把这两条写成机械判据。
11. **品牌串只出现在 `identity.h`**：其他文件写 `L"ExBar…"` 会被 `tools/check_components.py` 判失败
    （规则 B1，见 [NAMING.md](NAMING.md)）。
12. **绘制原语只有一份**：量字 `ui::MeasureText`、填充/描边/分隔线 `ui::FillRectColor` 等、
    双缓冲 `ui::PaintBuffered`、控件主题 `win::ThemeControl` —— 自写一份即失败。
13. **命令名两个方向都要认**：读命令名一律经 `FindCommand`（旧名 ↔ 点分名），
    不得直接字符串比较（规则 C3）。检查脚本也必须同口径，否则改名后整行被跳过。
14. **不模拟输入、不抢焦点、不循环点击**：产品代码与自动化脚本都不得出现 `SendInput` /
    `mouse_event` / `keybd_event` / `SetCursorPos` / `BlockInput` / `AttachThreadInput`；
    也不得用 `PostMessage`/`SendMessage` 投 `WM_LBUTTON*` / `WM_RBUTTON*` / `WM_KEY*` /
    `WM_CHAR` 等**输入类**消息 —— 合成点击在本程序里必然命中控件并走到 `ShowPopupList`，
    而弹层内部会 `SetForegroundWindow`，结果与 `SendInput` 相同。脚本也不得用
    `SetForegroundWindow` / `SetActiveWindow` / `BringWindowToTop` 改用户的前台窗口。
    一切操作走结构化命令：进程内 `CommandBus::Invoke`，进程外 AI 命名管道；
    需要验证必须真点才成立的流程时，**人工点、脚本只观察**（`tools/probe_toggle.py`）。
    `src/` 里的 `SetForegroundWindow`（转发激活已有窗口、弹层取键盘输入）是**用户触发**的产品行为，
    不在此列。规则 R21，判据在 `tools/check_components.py` 的 `INPUT_GUARD` 段（4 条），
    理由与替代方案见 [ARCHITECTURE-RULES.md §3.1](ARCHITECTURE-RULES.md)。

---

## 13. 与 ExBar mod 的模块对应

不是代码复用，是**语义对齐**——改交互前先看 mod 里同名模块怎么做的，避免两边手感分裂。

| ExBar mod（`src/parts/`） | 命名空间 | 本工程对应 | 差异 |
|---|---|---|---|
| `005-ebtail.cpp` 等 | `ebnav` | `nav_bar.cpp` / `nav_bar_actions.cpp` / `nav_model.cpp` | mod 有搜索框、Everything 结果面板、DUI 子类；本工程只有文件系统导航 + 视图模式 |
| L1 服务层 | `ebpopup` | `popup_list.cpp` | mod 的弹窗壳还承载路径下拉、书签弹出、多级子菜单；本工程只有单层通用列表 |
| `001-eb.cpp` / `005-ebtail.cpp` | `ebtab` | `tab_strip.cpp` / `tab_set.cpp` / `app.cpp` | 已有拖拽排序、锁定、右键菜单、关闭历史、MRU、溢出；窗口控制按钮不迁移 |
| `007-ebstatus.cpp` | `ebstatus` | `status_bar.cpp` / `view_stats.cpp` | 9 段对齐；遮挡系统状态栏不迁移（我们就是自己的状态栏） |
| `004-ebbmk.cpp` | `ebbmk` | `bookmark_bar.cpp` / `bookmarks.cpp` | 共用 `bookmarks.html`；不可变快照/跨线程发布不需要（单 UI 线程） |
| `011-ebsr.cpp` + `evipc` | `ebsr` / `evipc` | `disk_scan.cpp`（部分） | 搜索未实现（P3，复用 Everything IPC）；目录体积统计已落地 |
| `010-ebms.cpp` | `ebms` | `mouse_rules.cpp` / `shell_input.cpp` / `input_router.cpp` | 规则模型与触发时序对齐；低级鼠标钩子换成本进程子类 + UIA |
| `006-ebch.cpp` | `ebch` | `keymap.cpp` / `chord.cpp` / `sequence.cpp` / `input_router.cpp` / `hotkey_manager.cpp` | 热键 + 序列 + 管理器（当前 83 热键 / 38 序列，含 13 条指向未实现命令的占位）；低级键盘钩子与注入抑制不迁移 |
| `002-ebda.cpp` / `003-olns.cpp` | `ebda` / `olns` | — | 命名空间扩展：本工程直接用 shell 原生，不需要 |
| `012-ebai.cpp` / `014-ebaiui.cpp` | `ebai` | `ai_api.cpp` / `ai_server.cpp` | 接口层已实现（JSON-RPC over 管道，与 mod 的 `ebai` 同源设计）；窗口内 AI 面板未做（P4） |
| `ebop` 操作层 | `ebop` | `commands.cpp` + `cmd_naming.cpp` | 参数对象化、效果四维、前置条件、结构化结果、点分命名 —— 即 0.2.1 的"操作契约 10 步" |

完整能力对照（按命令逐组，含未实现项的差异根因）见 [PARITY.md](PARITY.md)；
当前条数以 `python tools/check_commands.py` 首行为准（它打印"命令表 N 条，已实现 M"）。

---

## 14. 扩展点（按"接入成本从低到高"）

| 想加的东西 | 落点 | 需要动的文件 | 注意事项 |
|---|---|---|---|
| 一个新快捷键 | 改 `data/keymap.json`，再跑 `python tools/gen_keymap.py`（更新兜底默认表） | 无代码 | 与 mod 默认键冲突要在 PARITY 记一笔；命令必须已在 `commands.cpp` 注册 |
| 一个新命令 | `commands.cpp` 目录加一行 + `App::RegisterCommands()` 注册处理器 | `commands.*`、`app_commands.cpp` | 未实现时 `implemented=false`，管理器会标灰 |
| 一个新下拉菜单 | 在业务文件里写 `ui::Menu menu; menu.Add...; menu.Show(...)` | 无新文件 | 动作图标写 `ico::IconId`，路径行交给 `AddPath` 走 `ico::Cached`；只在调用点描述动作，不手写 `PopupRow` |
| 状态栏多一段信息 | `StatusBar` 的 `SegId` + `StatusModel` 加字段，`RefreshStats` 填值 | `status_bar.h/.cpp`、`app.cpp` | 逻辑段号只用于数组/位掩码，绘制顺序单独维护 |
| 一个新的鼠标手势 | 改 `data/mouse.json`（target 见 `mouse_rules.h`） | 无代码 | 组件需把 `(target,button,mods)` 交给 `MouseSink` |
| 搜索框 | `NavBar` 里加控件 + `evipc` 客户端 | `nav_bar*`、新 `search_*.cpp` | Everything IPC 是 mod 的既有资产，先读 `src/parts/` 的 `evipc` |
| 预览窗格 | `IPreviewHandler` + 自绘面板 | 新模块 | `IExplorerBrowser` 无公开接口，只能自己做 |
| chrome Web 化 | WebView2 托管 | 新模块 | 见 [DECISIONS.md](DECISIONS.md) D3、[ROADMAP.md](ROADMAP.md) P4 |
