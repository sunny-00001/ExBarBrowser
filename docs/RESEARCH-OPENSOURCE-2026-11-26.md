---
status: current
baseline: 2026-11-26
method: 多轮 WebSearch + WebFetch（DeepWiki / KDE LXR / Debian Sources / GitHub 原始源码 / 官方文档 / issue 区）
去重: 与 docs/COMPETITIVE-GAP-2026-11-21.md（M1–M8 能力缺口）和 docs/GAP-ANALYSIS-EXTERNAL.md（P0–P2 AI 接口层）互补，不重复其结论；交集处显式标注"关联已知缺口"
---

# 开源文件管理器源码调研报告：导航栏 / 视图 / 书签 / 命令系统的可借鉴架构

> 本文回答：**同类开源项目的源码里，有哪些已验证的工程模式可以直接搬到 ExBarBrowser 的
> nav_bar / shell_view / bmk / commands 四个域**。
>
> 口径：只写源码级证据（文件名/组件名/行为细节），每条借鉴给出落地域与风险提示。
> 涉及本仓已知缺口（M1–M8、P0–P2）时不重复论证，只做关联引用。

---

## 1. Explorer++（C++/Win32，GPL-3.0，derceg/explorerplusplus）

### 1.1 架构简述
纯 Win32 + WTL 风格的轻量单进程架构。主窗口用 **rebar** 组织多条工具带
（MainToolbar / AddressBar / BookmarksToolbar / DrivesToolbar / ApplicationToolbar），
每条带可独立显隐、重排（`MainRebar.cpp` 的 band 结构：id/child/height/newLine/useChevron/idealLength）。
配置双轨：便携模式写 `config.xml`，否则写注册表 `HKCU\Software\Explorer++`。
插件系统为 Lua 脚本 + JSON 清单（`Plugins/` 目录）。

### 1.2 导航栏/书签/命令相关实现亮点（源码级）
- **地址栏**（`Explorer++/Explorer++/AddressBar.cpp` + `AddressBarView`）：
  控件/视图两层分离；`OnKeyPressed` 处理 Enter 提交、Escape 还原原文；
  `OnNavigationCommitted` 在从别处导航时自动回填文本；
  支持环境变量展开与路径自动补全。
- **书签系统**（`Explorer++/Bookmarks/` 全家桶）是同类项目里最完整的：
  - 模型：`BookmarkTree` + `BookmarkItem`（GUID 主键，name/location/dateCreated/dateModified），
    三个永久根文件夹：**Bookmarks Toolbar Folder / Bookmarks Menu Folder / Other Bookmarks**。
  - **信号/观察者同步**：树发生 add/update/remove/move 时发信号，
    `BookmarksToolbar`、`BookmarksMainMenu`、`BookmarkListView`、`ManageBookmarksDialog`
    各自订阅并自动刷新（如 `BookmarksToolbar.cpp:160-167`、`BookmarkListView.cpp:58-65`）。
    数据与 UI 彻底解耦——工具栏、菜单、管理对话框是同一份数据的三个观察者。
  - **拖放**：`BookmarkDropTargetWindow` 是共享基类，列表视图与工具栏都继承它处理落点
    （`BookmarkListView.cpp:782-813`、`BookmarksToolbar.cpp:329-361`）。
  - 打开方式统一走 `BookmarkHelper::OpenBookmarkItemWithDisposition`
    （当前标签/新标签/后台标签一个入口，中键=新标签）。
  - 管理对话框内嵌 `BookmarkNavigationController` 做书签树的后退/前进历史。
- **主工具栏可定制**（`MainToolbar.cpp`）：`MainToolbarButton` 枚举 + `GetButtonsForStorage()`
  序列化按钮集 + `StartCustomization()` 系统自定义对话框。

### 1.3 可借鉴点
1. 书签"数据模型 + 变更信号 + 多观察者 UI"三层结构（最值得搬）。
2. 三个永久书签根文件夹的语义划分（工具栏可见 / 菜单 / 其他）。
3. 拖放落点逻辑抽成共享基类，书签栏与列表复用。
4. 地址栏控件/视图分离 + Escape 还原语义。

### 1.4 常见坑（issue 区/论坛抱怨）
- **深色模式长期不完整**：菜单栏、标签栏仍浅色；1.5.0 ARM64 切主题曾直接崩溃
  （官方论坛 #2736、GitHub #479）。→ 自绘 chrome 必须从第一天就吃透
  `WM_THEMECHANGED`/深色 API（关联已知缺口 M4）。
- **无法撤销删除**、无按类型分组（AlternativeTo 评论反复提及）。
- **config.xml 定位混乱**：用户升 nightly 后书签"消失"，论坛反复出现找回教程
  （其实文件在 exe 同目录或注册表）——配置/书签持久化位置必须有明确的 UI 入口和备份提示。
- 性能抱怨：大目录下响应慢（AlternativeTo 12 评分 3.8）。

---

## 2. Tablacus Explorer（C++ COM 核心 + JS 加载项，tablacus/TablacusExplorer）

### 2.1 架构简述
C++ 核心（`TE/objects.cpp`、`TE/api.cpp`，COM 对象 `CteFolderItems` 直接实现
`IDataObject` 供拖放）+ **JS 全家桶做 UI 与加载项**（`Debug/script/ui.js`、`common.js`、
`options.js`、`sync.js`）。Blink 版（WebView2）用**双 JS 引擎**：
UI 侧 V8（异步 Promise）+ 同步侧 Chakra（`sync.js`），用 `InvokeUI()`/`api.Invoke` 跨引擎投递；
跨引擎传对象必须用 `api.CreateObject("Object"/"Array")` 或 JSON 串（wiki 明文警告原生对象会炸）。

### 2.2 导航栏相关实现亮点
- **加载项系统**（`ui.js:1057-1091` LoadAddon）：扫描 `addons/` 下各插件的 `config.xml`
  动态加载；脚本注释里可内嵌默认参数。地址栏本身就是一个加载项（AddressBar addon），
  有共享状态槽 `Common.AddressBar`。
- **事件总线**（`ui_.eventTE` + `AddEventUI`/`RunEventUI1`）：生命周期
  （Finalize）、导航（ChangeView）、布局（Layout）、文件操作（DownloadFile）均为事件，
  多插件可同事件注册、带优先级。
- 地址栏右键"Paste and Go"、状态栏信息增强等用户需求全部以加载项增量交付
  （issue #908/#947），核心保持小。

### 2.3 可借鉴点
1. **把地址栏/工具栏按钮做成可配置槽位 + 事件总线**：核心只发事件，行为由注册者实现——
   对本项目即：命令总线已具备此形态，导航栏装饰层（按钮显隐、图标）可走同一注册机制。
2. 同步/异步双通道分离的思想（UI 停摆时同步通道仍可用）——反衬本仓 E2
   模态框阻塞管道的问题：**机器侧通道必须不依赖 UI 消息循环**（关联已知缺口 E2）。

### 2.4 常见坑（issue 区，密度极高）
- **复制大文件时主窗口整体无响应数小时**（#896）：进度在子窗口，UI 线程被占死
  → 正是本仓 M3"长任务不得跑 UI 线程"的活体反面教材。
- CPU 占用高（#890）、部分 Win11 机器启动极慢（#942）。
- **关闭不立即退出**：WebView2/IME 相关（#939、#940）——COM/WebView 引用清理要显式。
- Win11 输入法逐字确认（#924）、地址栏 XP 样式开启即崩溃且无法再启动（#927）
  → 主题化视觉选项要做降级护栏。
- 便携目录换机器失效（#873）、管理员权限下拖放重排标签失效（#860）。

---

## 3. Files community（C#/WinUI 3，files-community/Files）

### 3.1 架构简述
WinUI 3 + MVVM。枚举走 `Windows.Storage` 抽象（统一本地/网络/OneDrive 命名空间），
性能敏感的缩略图/图标/复制移动走 Win32 Shell COM（`IShellItem`、`IFileOperation`，
复用系统进度对话框、冲突处理与回收站语义）。v4.0 把旧 `PathBreadcrumb` 重构为
通用控件 `BreadcrumbBar` + `Omnibar`（地址栏/搜索/命令面板三合一）。

### 3.2 导航栏相关实现亮点（源码级）
- **BreadcrumbBar**（`src/Files.App/Controls/BreadcrumbBar/`，独立控件目录）：
  `BreadcrumbBar.cs`（容器）、`BreadcrumbBarItem.cs`（段按钮）、
  `BreadcrumbBarItem.Events.cs`、`BreadcrumbBarLayout.cs`（**溢出布局**：宽度不足时
  中段折叠进 `...` 下拉）、`EventArgs.cs`；数据模型 `PathBoxItem`（`Data/Items/PathBoxItem.cs`）。
  每段带 chevron 下拉列出兄弟文件夹；支持拖文件到段上移动/复制。
  另有 `tests/Files.App.UITests/Views/BreadcrumbBarPage.xaml` 专门做 UI 测试页。
- **Omnibar**（`src/Files.App/Controls/Omnibar/`）：多模式输入（Path / Command Palette /
  Search 三模式，`OmnibarMode.cs` + 模式独立分部类），建议管道按模式切数据源
  （`NavigationToolbarViewModel.cs:64-70`：路径→文件系统/历史、命令→`ICommandManager`、
  搜索→索引器）。
- **导航工具栏**（`UserControls/NavigationToolbar.xaml` + `NavigationToolbarViewModel.cs`）：
  后退/前进按钮长按弹出历史 flyout；StatusCenter 按钮可见性可配置
  （Always / DuringOngoingFileOperations）。

### 3.3 可借鉴点
1. **面包屑做成独立可测试控件 + 独立 Layout 对象**：布局/溢出逻辑从事件处理里剥离。
2. 溢出折叠规则（见 §5 借鉴清单 B4：根节点必须常驻）。
3. 地址栏"路径/命令/搜索"模式统一，命令面板数据源直接对接命令注册表
   ——本项目命令总线 + 命令面板已具备，差一个"地址栏模式切换"的 UI 形态。
4. `IFileOperation` 复用系统进度/冲突 UI 的取舍思路（本项目走 IExplorerBrowser 同理）。

### 3.4 常见坑（issue 区）
- **Omnibar 缺陷清单极长**（#16029 官方跟踪列表 40+ 条）：模式切换崩溃、切标签崩溃、
  焦点丢失、Esc/Enter 不还焦点、RTL 无支持、高对比度主题失效、建议列表不随输入刷新……
  → 多模式地址栏的交互矩阵是 bug 高发地；每加一种模式要成对补焦点/Esc/Enter/主题测试。
- **面包屑折叠把根盘符也藏掉**（#17766）：双窗口对拷时看不到根，用户强烈要求
  "根常驻，折叠从第二段开始"；官方还指出 padding 是触控可达性需求，砍 padding 不行。

---

## 4. Double Commander（FreePascal/LCL，doublecmd/doublecmd）

### 4.1 架构简述
Object Pascal（FPC 3.2.2+ / Lazarus 4.0+），LCL 控件 + 自绘面板，`components/` 目录自带
KASToolBar 等私有控件包。经典双面板 commander 架构；跨 4 个 widgetset（GTK2/3、Qt5/6），
平台差异收敛在独立目录。兼容 TC 插件体系（wcx/wdx/wfx），脚本用 Lua。

### 4.2 导航/命令相关实现亮点
- **目录热列（Directory Hotlist）**：Ctrl-D 弹出，完全仿浏览器书签交互；
  1.1.20 更新专门改进了**热键冲突检测**（与本仓 E6 的冲突检测只在 UI 显示同病相怜，
  DC 是发布说明级修复）。
- **快速搜索（Speed Search）**：直接敲字母在面板内增量过滤定位（支持拼音/正则变体）。
- 标签页持久化：未关闭标签跨会话恢复、可永久固定。
- 面板列配置、字体、间距、按钮图片均可深度定制（对比本仓 M4 显示偏好缺口）。

### 4.3 常见坑（issue 区）
- **自动刷新只在活动面板生效**（#2005）、读目录内容读两遍（#1926）
  → 目录监听要按面板粒度订阅、去重（关联已知缺口 M7）。
- 路径输入框不 trim 空格导致失败（#2000）——本仓 E3 路径归一化同族问题，归一化时连 trim 一起做。
- 热列条目无法用方向键选择（#1437）：弹层键盘导航要当一等公民测。
- Samba 共享跨面板时切换目录锁死（#1986）、暗色模式菜单栏偶尔变蓝（#1954）。
- 工具栏按钮移动到其他工具栏后热键丢失（#1402）：键位应绑定命令 ID 而非控件位置
  （本仓 keymap 按命令绑定，天然规避）。

---

## 5. KDE Dolphin / KIO —— KUrlNavigator（Qt/C++，面包屑的参照实现）

### 5.1 架构简述
Dolphin 的地址栏不在 Dolphin 里，而在 KIO 框架
（`kio/src/filewidgets/kurlnavigator.cpp` 等），是全 KDE 共用的成熟控件：
面包屑态 + 可编辑态双模式、历史栈（`KCoreUrlNavigator`）、Places 选择器、协议下拉、
badge 容器、拖放目标，全部信号化（`urlAboutToBeChanged`/`urlChanged`/`historyChanged`/
`tabRequested`/`urlsDropped`）。

### 5.2 实现亮点（源码级，lxr.kde.org 可交叉验证）
- **按钮复用**：`updateButtons(startIndex)` 对 `m_navButtons` 列表做差量更新——
  URL 变长补建、变短删余、尽量复用既有按钮，不整条重建。
- **溢出折叠**：`updateButtonVisibility()` 宽度不足时隐藏**上层**路径按钮并挪进
  顶部的 `KUrlNavigatorDropDownButton` 下拉菜单。
- **双模式切换**：`switchView()` + `slotToggleEditableButtonPressed()`；
  从编辑态回面包屑时先 `applyUncommittedUrl(ApplyUrlMethod)`——
  **Apply / Tab / ActiveTab / NewWindow 四分支**与 Return 键修饰符组合对应
  （Alt+Shift+Enter=新标签等）。相对路径输入先用 `KIO::stat` 探测"当前目录下是否有同名子目录"
  再决定拼当前路径还是按 URI 过滤器解析（`kshorturifilter`）。
- **修饰键点击语义**（`slotNavigatorButtonClicked`）：左键=就地打开、
  中键或 Ctrl+左键=`tabRequested`、Shift+左键=`newWindowRequested`、
  Shift+中键或 Ctrl+Shift+左键=`activeTabRequested`。
- **Places 根截断**：`retrievePlaceUrl()` 把显示路径截断到最近的 Places 项
  （如 `fish://root@192.168.0.2/var/lib` 显示为 `Custom Path > var > lib`）。
- **视图状态随历史**：`saveLocationState(QByteArray)` 在 `urlAboutToBeChanged` 时存
  滚动位置等视图状态，`urlChanged` 后用 `locationState()` 恢复——前进/后退体验的关键。
- **按钮级子目录菜单**：`KUrlNavigatorButton` 按下后 **300ms 定时器**延迟发起
  `KIO::listJob` 枚举子目录填充下拉（`kurlnavigatorbutton.cpp:46-52`）；滚轮在按钮上
  滚动可在兄弟目录间切换（`wheelEvent`）。远程协议有 stat 黑名单
  （nfs/fish/ftp/sftp/smb/webdav/mtp，`kurlnavigatorbutton.cpp:66-74`）防止慢协议卡 UI。
- **拖放**：段按钮 `setAcceptDrops(true)`，落上弹 复制/移动/链接 三选菜单（`dropUrls`）。
- 双栏友好：`setActive(false)` 让非活动栏的地址栏整体变灰（纯视觉，行为不变）；
  模式切换前 `setFixedHeight()` 一次，避免两种模式高度不同导致窗口抖动。

### 5.3 可借鉴点
这是**面包屑行为的教科书**：差量更新、折叠规则、修饰键矩阵、相对路径 stat 探测、
状态随历史、按钮延迟枚举、拖放三选——全部有源码可对照。

### 5.4 常见坑
- 生态绑定 KIO/kioslave，Windows 侧不可直接复用，但**行为规格**可全部移植。
- 远程协议 stat/枚举必须黑名单或异步，否则面包屑本身成为卡顿源（源码注释自证）。

---

## 6. muCommander（Java/Swing，mucommander/mucommander）

### 6.1 架构简述
Gradle 多模块：`mucommander-core` + `mucommander-protocol-*`（每协议一模块：ftp/smb/s3/gdrive…）
+ `mucommander-format-*`（每归档格式一模块）+ `mucommander-os-*`（每平台一模块）。
`MainFrame` 聚合双 `FolderPanel`、工具栏、命令栏、终端集成（`TerminalIntegration` 随面板目录联动）。

### 6.2 命令系统相关实现亮点（源码级）
- **ActionManager**（`mucommander-core/.../ui/action/ActionManager.java`）：动作工厂注册表，
  **每个 MainFrame 一套有状态动作实例**——菜单、工具栏、快捷键全部引用同一实例，
  状态（如选中项变化导致的 enable/disable）天然一致。
- **ActionKeymap / ActionKeymapIO**：键位映射独立读写层；
  `ActionDescriptor` 承载元数据。
- **配置双轨**（`mucommander-preferences/.../conf/`）：`MuPreferences`（preferences.xml，
  静态偏好，类型安全枚举键 `MuPreference`）与 `MuSnapshot`（snapshot.xml，动态窗口状态：
  窗口位置、分栏比例、排序）**分离持久化**。
- `OpenLocationAction` 把"跳转到某位置"也做成动作（可绑快捷键/菜单项）。

### 6.3 可借鉴点
1. 命令实例持有状态 + 单窗口单实例——对本仓 commands 域：`CommandBus` 无状态总线可保留，
   但 **UI 可用态（enabled）计算**可下沉为命令对象自身状态，避免每处各算一遍。
2. 偏好与快照分离持久化（本仓 profile 隔离已有，可借鉴其"动态状态不进静态配置"的边界划分）。
3. "跳转到位置"做成一等命令（本仓 `fs.open` 已是，印证方向）。

### 6.4 常见坑（issue 区）
- 配置互斥 bug：`commands.yaml` 存在时 "Open with" 被禁用（#1519）——
  自定义命令系统与内置行为要有明确的优先级与共存规则。
- home 目录大时挂起（#1517）、驱动器按钮偶发无响应（#1370）——
  面板初始化与枚举必须异步。
- 快捷键与系统全局键冲突（#1381 Find vs Fill Window）。

---

## 7. Krusader（KDE，UserActions 占位符系统）

### 7.1 架构简述
KDE 双面板 commander，核心扩展机制是 **UserActions**：用户以 XML 定义动作
（`useractions.xml`），运行时经 `expander.h` 展开占位符后执行，
`app/UserAction/kraction.cpp` 承接（`KrActionProcDlg` 非模态输出对话框，stdout/stderr 分栏）。

### 7.2 实现亮点（官方文档 + 源码交叉验证）
- **XML 动作 schema**：`name/title/tooltip/icon/category/description/command/
  defaultshortcut`；`command` 带 `executionmode`（normal / **collect_output** 收集输出）
  与 `onmultiplefiles`（**call_each / call_once** 多选时的调用粒度）。
- **占位符语法**：`%<面板指示><名称>(参数)%`——面板指示 `a`=活动面板、`o`=另一面板、
  `l/r`=左右、`_`=与面板无关。内置展开器：`Path/Count/Filter/Current/List/Select/Goto/
  Ask/Clipboard/Sync/NewSearch` 等，可嵌套
  （如 `%_Copy("%aCurrent%", "%_Ask("new name", "%aCurrent%.bak")%")%`）。
- UserAction 自动接入 KDE ActionCollection：获得菜单/快捷键/工具栏全套集成。

### 7.3 可借鉴点（commands 域）
1. `executionmode=collect_output` ↔ 本仓命令的"返回值/断言"体系：外部命令也应声明
   是否收集输出作为机器可读回执。
2. `onmultiplefiles=call_each|call_once`：多选文件时命令的执行粒度应显式声明，
   不留给实现猜。
3. 占位符面板前缀（a/o/单双栏语义）：若未来做自定义命令/外部工具，参数模板可按
   "活动视图/非活动视图"取值，与本仓单窗口模型兼容。
4. 社区动作库形态（KDE Store 收集 useractions.xml）——命令总线 JSON 清单同样可分享，
   但 Krusader 官方明确警示 Store 插件非官方需谨慎 → 引入外部命令包需审核机制
   （本仓已有危险命令双门，机制对口）。

### 7.4 常见坑
- 仅 Linux；占位符嵌套过深时调试困难（文档用大量示例补偿）。
- KDE Store 加载项无官方审核。

---

## 8. Win32 自绘 chrome / 面包屑通用技术（CodeProject / MSDN）

- **CodeProject《Custom Controls in Win API》两篇**（646482 控件定制、624495 标准消息）：
  子类化模式（`GetWindowLongPtr(hwnd, 0)` 存 this 指针）、`WM_GETDLGCODE`、
  **`WM_UPDATEUISTATE`**（键盘导航时焦点框/快捷键下划线的显隐，绘制时用 `UISF_HIDEACCEL`
  配 `DT_HIDEPREFIX`）、**`WM_SYSCOLORCHANGE`**（注意 `DefWindowProc` 不会向子控件分发，
  顶层窗口要手动广播——自绘控件漏掉这条例必出现"改系统主题后颜色残留"）、
  **`WM_THEMECHANGED`**（重开 theme 句柄并重绘）。
- **Owner-draw vs Custom-draw 边界**：custom draw 仅适用 header/listview/rebar/toolbar/
  tooltip/trackbar/treeview；owner-draw 不适用于 edit 控件。rebar 恰好支持 custom draw
  ——本仓若在 rebar 上做自绘 chrome，custom draw 是正道，整窗 `WM_PAINT` 全接管是过度方案。
- **路径补全**：`SHAutoComplete(hwndEdit, SHACF_FILESYS_DIRS)` 一行即可获得
  文件系统目录补全（含 UNC）；需要自定义候选集时走 `CLSID_AutoComplete` +
  自实现 `IEnumString`（Next 返回串必须 `CoTaskMemAlloc`）。
  注意需 COM 已初始化。

---

## 9. 实现借鉴清单（11 条，按落地域分组）

> 格式：来源（项目 + 文件/组件）→ 落地建议（域）→ 风险提示。
> 标注〔关联〕的条目同时缓解已知缺口/错误，但不重复其论证。

### B1〔bmk〕书签数据模型 + 变更信号 + 多观察者 UI
- 来源：Explorer++ `BookmarkTree`/`BookmarkItem`（GUID 主键）+ 信号订阅
  （BookmarksToolbar/BookmarksMainMenu/BookmarkListView 三观察者）。
- 落地：把本仓 bmk 域整理为「数据层（书签树 + 变更通知）→ bookmark_bar/popup_list 等
  纯观察者」。当前 `bookmark_bar.cpp` 1073 行偏大〔关联 E7/P2-6 超 500 行治理〕，
  拆分时按"数据层 / 观察者"切而不是按"UI 控件"切。
- 风险：本仓所有写操作必须继续走命令总线（保证审计/撤销/Effects 完整），
  信号只驱动 UI 刷新，不得成为旁路写通道。

### B2〔bmk〕三个永久书签根 + GUID 稳定标识
- 来源：Explorer++ 三根文件夹（Toolbar/Menu/Other）语义划分；BookmarkItem 以 GUID 定位。
- 落地：bmk 域如果目前是平铺列表，升级为「工具栏区 / 菜单区 / 其他」三语义分区；
  命令（`bookmark.*`）以稳定 ID 而非索引寻址，避免增删后索引漂移。
- 风险：分区会增加配置迁移成本——需要一次性兼容旧配置的导入映射。

### B3〔bmk/nav_bar〕拖放落点抽共享基类
- 来源：Explorer++ `BookmarkDropTargetWindow`；KUrlNavigatorButton 拖放三选（复制/移动/链接）。
- 落地：书签栏与（未来的）面包屑段共享同一落点判定/高亮/落点菜单代码，
  行为统一为"落上高亮 + 松手三选（或默认移动）"。
- 风险：Win32 拖放（IDropTarget）状态机易漏 DragLeave，共享基类必须先过
  单元测试再上 UI；与本仓"拖放统一走 IFileOperation 语义"保持一致。

### B4〔nav_bar〕面包屑溢出折叠：根常驻、从第二段折叠
- 来源：KUrlNavigator `updateButtonVisibility()`（上层路径折叠进下拉）；
  Files issue #17766 的反面教训（根被折叠导致双窗口对拷失定位）。
- 落地：nav_bar 若引入面包屑，折叠规则定为：根节点（盘符/UNC 共享/命名空间根）
  永久显示；溢出时先折叠中段进 `...` 下拉；padding 保留触控可达宽度。
- 风险：UNC 长前缀（\\server\share）与盘符语义不同，根判定要覆盖
  `\\server\share`、shell 命名空间（::{GUID}）、回收站等非常规根〔关联 E3 的 UNC 保留规则〕。

### B5〔nav_bar〕双模式地址栏：面包屑 ↔ 可编辑，四分支提交
- 来源：KUrlNavigator `switchView()`/`applyUncommittedUrl(ApplyUrlMethod::Apply/Tab/
  ActiveTab/NewWindow)` + Return 修饰符组合；相对路径先 stat 探测子目录再拼接。
- 落地：地址栏在"面包屑显示态"与"可编辑态"间切换（点击空白区/快捷键）；
  提交时按修饰符分流：就地打开 / 新标签 / 新窗口（本仓单窗口模型可先实现前两个）；
  相对路径输入先探测当前目录子项。
- 风险：Files Omnibar 的 40+ 缺陷清单（#16029）证明多模式交互是 bug 高发地——
  每种模式必须成对测试 Esc/Enter/焦点转移/主题/IME〔关联 Tablacus #924 IME 坑〕；
  编辑态必须支持 Escape 还原原文（Explorer++ 语义）。

### B6〔nav_bar〕面包屑按钮差量更新 + 子目录延迟枚举
- 来源：KUrlNavigator `updateButtons()` 差量复用；`KUrlNavigatorButton` 按下后
  300ms 延迟枚举子目录 + 远程协议 stat 黑名单。
- 落地：段按钮不整条重建（避免闪烁与焦点丢失）；子目录下拉枚举延迟触发
  （按下后短延时再 IEnumIDList），大目录/慢路径有超时护栏。
- 风险：延迟窗口内用户松手应取消枚举；枚举需在后台线程 + 结果回 UI 线程投递，
  不得阻塞 UI〔关联 M3 教训〕。

### B7〔shell_view〕导航历史携带视图状态
- 来源：KUrlNavigator `saveLocationState()`/`locationState()` 随 `urlAboutToBeChanged`/
  `urlChanged` 存取（滚动位置、选择等）。
- 落地：本仓后退/前进（`nav.history.*` 已有命令位）在切换位置时保存/恢复
  视图状态（滚动位置、选中项、排序），历史导航才"像浏览器"。
- 风险：IExplorerBrowser 视图状态的存取受 shell 限制——能拿多少存多少，
  拿不到的字段如实缺省，不要伪造。

### B8〔commands〕命令对象带状态 + enabled 下沉
- 来源：muCommander `ActionManager`（每 MainFrame 一套实例）+ `ActionKeymap`/
  `ActionDescriptor`。
- 落地：commands 域的"前置条件声明"（Req）已存在；把**动态 enabled 计算**
  收敛到命令描述的单一函数，菜单/工具栏/命令面板/AI 侧共享同一判定结果，
  避免多处各自实现导致 UI 与机器行为不一致〔关联 P0-11 同义命令 Effects 一致性的精神〕。
- 风险：不要把 CommandBus 改成有状态总线——保持"总线无状态 + 命令描述可查询"，
  改动范围限制在 enabled 判定函数的收敛。

### B9〔commands〕外部命令声明执行语义：collect_output + call_each/call_once
- 来源：Krusader `useractions.xml` 的 `executionmode`/`onmultiplefiles`；
  `kraction.cpp` 非模态输出对话框（stdout/stderr 分栏）。
- 落地：若本仓引入"自定义命令/外部工具"位，schema 必须显式声明：
  是否收集输出作为机器回执（对应自断言体系）、多选时逐项调用还是整批调用、
  输出展示非模态（绝不模态阻塞机器通道〔关联 E2〕）。
- 风险：占位符替换必须转义防注入（外部命令拼接路径是命令注入高发区）；
  引入第三方命令包须经双门审核（机制已有）。

### B10〔shell_view/commands〕偏好（静态）与快照（动态）分离持久化
- 来源：muCommander `MuPreferences`（preferences.xml）vs `MuSnapshot`（snapshot.xml）。
- 落地：本仓配置已是原子写；把"用户显式设置的偏好"与"运行时恢复状态
  （窗口位置、排序、会话）"分文件/分节持久化，避免动态状态回写污染可读配置，
  也让"重置偏好"不误伤会话恢复。
- 风险：两类文件的写入时机不同（偏好=设置变更时；快照=退出/导航时），
  需明确写盘策略避免高频 IO。

### B11〔nav_bar〕自绘 chrome 必须处理的系统消息清单 + SHAutoComplete 兜底
- 来源：CodeProject 624495（WM_UPDATEUISTATE/WM_SYSCOLORCHANGE 分发缺口/
  WM_THEMECHANGED）+ MSDN SHAutoComplete。
- 落地：nav_bar/shell_view 的所有自绘控件核对三件事：① `WM_UPDATEUISTATE`
  焦点框/助记符显隐；② 顶层窗口向子控件广播 `WM_SYSCOLORCHANGE`；③
  `WM_THEMECHANGED` 重取主题资源（深色模式切换路径， Explorer++ 崩溃案例即此类）。
  地址栏编辑框补全可用 `SHAutoComplete(SHACF_FILESYS_DIRS)` 作为自研补全的兜底层。
- 风险：SHAutoComplete 的候选集不可控（系统历史/文件系统），与自研补全同屏时
  要么二选一要么明确分工；需 COM 初始化先行。

---

## 10. 去重说明（与既有两份对标文档的边界）

- `COMPETITIVE-GAP-2026-11-21.md`（M1–M8、E1–E8）：能力缺口与真错误层面。
  本文不重复其结论，仅在 B4/E3（UNC 归一化）、B5/E2（模态阻塞）、B6/B9/M3（UI 线程）、
  B11/M4（主题）处做**关联引用**。
- `GAP-ANALYSIS-EXTERNAL.md`（P0–P2、A/B/C/E）：AI 接口层与工程治理层面。
  本文 B8/B9 涉及命令元数据一致性处仅引用其精神（P0-11、P1-4），
  不新增 AI 协议层建议。
- 本文的增量价值：**全部为源码级实现模式**（文件/组件/行为规格），
  两份既有文档均未覆盖该层面。

---

## 11. 主要证据源

- DeepWiki：derceg/explorerplusplus §4/§5/§5.1；tablacus/TablacusExplorer §1.2/§5.1；
  files-community/Files §3.5；mucommander/mucommander §3/§5.1。
- 源码：KDE kio `kurlnavigator.cpp`（GitHub raw 全文抓取）、`kurlnavigatorbutton.cpp/_p.h`
  （lxr.kde.org）；Debian Sources kf6-kio 交叉验证。
- 官方文档：KDE Dolphin Location Bar（docs.kde.org）；Krusader useractions.docbook +
  useractions-xml 附录（docs.kde.org）；Tablacus wiki「JavaScript for Blink Edition」。
- Issue 区：Tablacus #860-#947 摘录；Files #16029（Omnibar 缺陷总表）、#17766（面包屑折叠）；
  doublecmd #1926-#2005、#1437、#1402；mucommander #1287-#1519；
  Explorer++ 官方论坛 #1276、#2736，GitHub #479；AlternativeTo 评分评论。
- Win32 技术：CodeProject 646482 / 624495；MSDN SHAutoComplete；owner-draw/custom-draw 对比文。
