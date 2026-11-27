# CHANGELOG 0.1.0 详细记录

> 从主 CHANGELOG.md 迁出，保留该版本完整开发过程（验证/踩坑/逐条 why）。

## [0.1.0] - 2026-11-19

**P1 骨架**：第一个可构建、可日常使用的版本。目标不是功能对齐 ExBar mod，而是把"独立进程 + 托管真实  
Shell 视图 + 自绘 chrome"这条路线跑通，并留下可继续生长的分层骨架。

### 新增

- **进程骨架**（`src/main.cpp`）
  - Per-Monitor V2 DPI 感知，逐级降级到 `SetProcessDpiAwareness(2)` / `SetProcessDPIAware()`。
  - STA COM 初始化；`ApplyProcessDarkMode` 在任何窗口创建前调用。
  - 单实例：`Local\ExBarBrowser.Instance` 命名互斥量；已有实例时用 `WM_COPYDATA`（magic `0xEBB1`）  
    把路径转给已有窗口开新标签，并还原/前置该窗口。
  - 命令行：目录参数、`/select <文件>`、`/select,<文件>`（打开父目录）、无参数时打开用户主目录。
- **窗口与布局**（`src/app.cpp`）
  - 主窗口 `ExBarBrowser_Main`，客户区自上而下：标签栏 32 px / 导航栏 30 px / 视图区 / 状态栏 22 px  
    （全部经 `Theme::S()` 按 DPI 缩放）。
  - 视图区 `ExBarBrowser_ViewArea` 作为所有标签容器的父窗口，切标签只做 `ShowWindow` 隐藏，不销毁视图。
  - 标签模型：每个标签持有 `ShellView` + 线性历史 `hist` + 当前索引 `idx` + 显示名 + 项目数。
  - 关闭最后一个标签即退出进程。
- **标签栏**（`src/tab_strip.cpp`）
  - 自绘、双缓冲；标签宽度随标题自适应并夹在 112–230 px；激活标签带 accent 下划线。
  - 悬停显示关闭按钮，中键点击关闭；右侧 `+` 新建标签。
  - 只负责绘制与命中，点击通过 `WM_APP_TAB_SELECT/CLOSE/NEW` 投递给 `App`，不持有标签状态。
- **导航栏**（`src/nav_bar.cpp` 窗口/布局/命中、`src/nav_bar_draw.cpp` 绘制、`src/nav_metrics.h` 尺寸常量、`src/nav_bar_actions.cpp` 交互）
  - 五个自绘按钮：后退 / 前进 / 向上 / 刷新 / 视图。后退与前进用系统真实图标（`SHGetStockIconInfo`  
    225/226/227）并带 12 px 分裂箭头（点箭头出单向历史）；向上与视图用 Segoe MDL2 字形。
  - 面包屑画在地址框内：前导文件夹图标 + 常驻折叠钮 `‹` + 每级 32 px chevron 区；  
    路径过长时从左侧折叠，折叠钮列出被折叠层级；未折叠时折叠钮列"此电脑"顶层（驱动器/用户目录）。
  - 地址框右侧内嵌 `⌄` 下拉：前进 + 后退合并历史（各最多 16 条，中间分隔线）。
  - 地址编辑：点空白或 `Ctrl+L` 进入，`Enter` 提交、`Esc` 还原、失焦退出；编辑框内 `Tab`  
    按前缀补全同级文件夹并循环。
  - 视图菜单：8 档模式（超大/大/中/小图标、列表、详细信息、平铺、内容），经  
    `IFolderView2::SetViewModeAndIconSize` 立即作用于当前标签。
- **自绘下拉弹层**（`src/popup_list.cpp`）
  - `ShowPopupList`：`WS_POPUP` + `CS_DROPSHADOW` 宿主 + owner-draw `LISTBOX`，24 px 行高、  
    16 px 图标、悬停/选中底色、深色 1 px 边框、文件夹行右端 chevron、分隔行、禁用行。
  - 自带模态消息循环：`Enter` 选中、`Esc` 取消、失焦自动关闭；返回行索引，图标由调用方释放。
  - 替代 `TrackPopupMenu`：系统菜单带不了图标、配色跟不了主题、行高不可控。
- **导航模型**（`src/nav_model.cpp`）
  - 解析名（`C:\...`）与命名空间（`::{GUID}` / `shell:`）双口径；命名空间用友好名显示、不提供"向上"。
  - 同级枚举走 `SHParseDisplayName` → `SHBindToParent` → `BindToObject` → `EnumObjects`，  
    跳过隐藏项，按 `StrCmpLogicalW` 自然排序。
- **Shell 视图**（`src/shell_view.cpp`）
  - 每个标签一个 `IExplorerBrowser`，`SetOptions(EBO_SHOWFRAMES)` 打开 shell 原生导航窗格，  
    `SetPropertyBag(L"ExBarBrowser")` 隔离视图状态。
  - `IExplorerBrowserEvents` 接收器：`OnViewCreated` / `OnNavigationComplete` 回填路径、显示名与项目数。
  - 主题联动：`SetWindowTheme` + `AllowDarkModeForWindow`，并只对 `ExplorerBrowserControl` /  
    `SHELLDLL_DefView` / `DirectUIHWND` 三类窗口补发 `WM_THEMECHANGED`，避免全树递归。
- **主题**（`src/theme.cpp`）
  - 颜色令牌单一来源：深色文件区 `#141414` / 工具栏 `#1e1e1e`，浅色 `#ffffff` / `#f3f3f3`（本机实测值）。
  - 令牌覆盖：`bg/bar/barAlt/hover/pressed/line/text/sub/disabled/chevron/accent/editBg/editBorder/listBg/listHot/listSel`
    - 画刷 `brBg/brBar/brBarAlt/brHover/brEdit` + 字体 `font/fontBold/glyphFont`（Segoe MDL2 Assets）。
  - 跟随系统 `SystemUsesLightTheme`（回退 `AppsUseLightTheme`）；`WM_SETTINGCHANGE(ImmersiveColorSet)`  
    与 `WM_THEMECHANGED` 时整体重建令牌、字体、画刷与 shell 视图主题。
  - 进程级深色走 uxtheme 未文档化导出（序号 135 `SetPreferredAppMode`、133 `AllowDarkModeForWindow`），  
    取不到就静默降级为"仅工具栏深色"。
  - 深色标题栏：`DwmSetWindowAttribute` 属性 20，失败退 19（1809）。
- **状态栏**（`src/status_bar.cpp`）：左侧"N 个项目"，右侧当前路径，超长省略。
- **诊断**（`src/common.h` 的 `LogDbg`）
  - `%TEMP%\ExBarBrowser.log`（UTF-16 LE，追加写）记录 `Initialize / parse / item / browse / complete`  
    五步 HRESULT 与路径，以及 `[click]`（导航栏命中分解）与 `[popup]`（弹层创建/失焦）两类调试行。
- **注册脚本**（`scripts/install.ps1`、`scripts/uninstall.ps1`）
  - `HKCU\Software\Classes\Folder\shell\ExBarBrowser`（"Open with ExBar"）与  
    `Directory\Background\shell\ExBarBrowser`（"Open in ExBar"）两个动词，全部写 HKCU、不需要管理员。
  - `-Takeover` 可把"双击文件夹"重定向到本程序，改动前先导出  
    `backup\Folder-shell.reg` 并留 `.takeover` 标记；卸载脚本据此还原系统默认。
- **开发工具**（`scripts/shot.py`）：`PrintWindow(PW_RENDERFULLCONTENT)` 抓主窗口整图，按类名  
  `ExBarBrowser_Main` 定位，纯 ctypes + zlib 手写 PNG，不依赖 Pillow。
- **构建**（`build.sh` / `build.bat`）：真多 TU，clang++ 20.1.3 `-target x86_64-w64-mingw32`  
  `-std=c++20 -O1 -g -mwindows`，静态链接 libstdc++/libgcc，单 exe ≈3.2 MB。

### 修复

开发期在代码注释中留档的四个坑（均为实测，不是推测）：

- **`WM_COPYDATA` 解错一层**：`lParam` 是 `COPYDATASTRUCT*` 而不是 payload 本身，之前直接把  
  `lParam` 当 `CopyDataPayload*` 用，magic 校验永远失败、转发静默不生效。现在先解 `COPYDATASTRUCT`  
  再校验 `dwData` / `cbData` / `magic` 三重条件。
- **同级下拉列成了同级**：`SHBindToParent` 给出的是"父文件夹 + 本项子 PIDL"，直接 `EnumObjects`  
  枚举到的是兄弟项；必须再 `BindToObject` 拿到目标文件夹自己的 `IShellFolder` 才是子文件夹。
- **正斜杠导航失败**：`SHCreateItemFromParsingName` 对 `C:/x/y` 返回 `E_INVALIDARG`（实测），  
  命令行与用户输入都常带正斜杠。`ShellView::DoNavigate` 统一转反斜杠并去掉尾部多余分隔符，  
  命名空间路径不转换。
- **首屏路径不规范化**：`ShellView::Create` 里的首次导航可能同步完成，`App::NewTab` 原来用调用方  
  传入的原始字符串当标题与历史首项，会出现 `C:/x` 与 `C:\x` 两份。现在优先取 shell 回填的  
  `view->path()`。

### 已知问题

- 标签栏不处理溢出：标签总宽超过窗口后，右侧标签与 `+` 按钮被裁到窗口外，无法滚动或压缩。
- 盘根分段仍绘制 chevron 区，点击无菜单（无父级可枚举同级）。
- 弹层里 `folder` 行画了 chevron 但没有二级弹出，点击等于直接选中该行。
- 视图菜单不标当前档位（弹层暂无"选中态"）。
- `app.cpp` 438 行，仍同时承担标签生命周期、导航、布局与命令分发，待按架构规则拆分（B2/B3）。
- 日志有两份实现（`common.h` 的 `LogDbg` 与 `shell_view.cpp` 的 `LogLine`），待合并。
- `/select,<文件>` 只打开父目录，不选中目标项。
- 无任何配置与状态持久化：窗口几何、标签组、视图设置都不保存。
- 日志无轮转；`%TEMP%\ExBarBrowser.log` 会一直追加。
- 每个标签一个 `IExplorerBrowser`，内存随标签数线性增长，未做懒加载或回收。

### 安全

- 注册脚本只写 `HKCU`，不请求提权；`-Takeover` 前先导出备份，卸载时只删自己写的键值。
- 不注入 explorer、不装全局钩子、不注册 shell 扩展 DLL，卸载后系统行为完全恢复。
