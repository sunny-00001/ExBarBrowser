# ExBarBrowser

**独立进程的文件浏览器** —— 自己的窗口、自己的标签栏、自己的 Shell 视图。不注入 explorer，不装全局钩子，单 exe 零运行时依赖，配置便携可整体拷走。

> 当前版本：`0.3.3`（见仓库根 [VERSION](VERSION)） · 许可证：[MIT](LICENSE) · 平台：Windows 10/11 x64

---

## 为什么是它

| 痛点 | 本项目的做法 |
|---|---|
| 系统资源管理器标签能力有限，改它要注入进程、崩了连带系统 shell | **独立进程 + 托管视图**：每个标签一个 `IExplorerBrowser`，右键菜单、拖放、就地重命名、缩略图、排序分组、导航窗格全部由系统 shell 提供；不重写视图，也不碰 explorer |
| 文件管理高度依赖鼠标，重复操作难自动化 | **键盘优先**：组合键 + 169 条序列命令 + 可录制的快捷键管理器；按钮、热键、序列、鼠标、命令面板、AI 全走同一条命令总线，人机同源 |
| 自动化工具只能"看截图点鼠标" | **可编程**：命名管道 JSON-RPC 与 MCP over stdio，196 条命令暴露给 AI；令牌鉴权 + 危险命令双门 |
| 配置散落在注册表/AppData，换机器要重来 | **便携模式**：所有用户数据都在 `<exe>\data` 等分层目录，整目录拷走即可 |

---

## 功能一览

### 标签与导航

- 多标签：新建/关闭/切换/复制/重开/移动/MRU，关闭最后一个标签即退出；溢出横向滚动 + `»` 全部标签菜单，支持拖拽排序、锁定、中键关闭、双击空白新建
- 导航栏：后退/前进/向上/刷新/视图五按钮 + 分裂箭头方向历史；面包屑分段可点、折叠、右侧 chevron 逐级展开子目录；地址栏自动补全、最近访问、`Tab` 同级补全
- 双栏：一个标签两窗格，水平/垂直切换、分界可拖、互换/镜像目录、复制到/移动到对侧
- 会话：命名会话保存/恢复/删除，启动恢复上次标签组与关闭历史

### 书签与状态栏

- Netscape 书签树 + `EXBAR_*` 扩展：点左侧图标列展开子目录、点文字打开或下钻分类、溢出菜单、右键增删改、拖入文件夹、拖到分类中间归档
- 导入导出走系统对话框（UTF-8 百分号解码、UNC 双向支持）；外部修改检测，拒绝静默覆盖
- 状态栏 9 段：文件夹/文件/已选计数、修改日期、路径、选中容量、磁盘/容器、隐藏三态、类型；段点击 + 右键显隐

### 输入与命令

- 快捷键 112 条（`data/keymap.json`，可直接在文件里改）+ 序列键 169 条（前缀状态机、3000 ms 超时、候选弹层、`Esc` 取消，模式总览弹窗可打字过滤）
- 鼠标规则：目标区 × 按钮 × 触发（单击/双击/长按/四向滚轮）× 修饰键；shell 视图命中经子类 + UIA 汇入同一条规则引擎
- 快捷键管理器：非模态自绘三栏，录制按键、冲突分级、撤销/重做、五套键位预设
- 命令面板、命令自省（`cmd.help`）、配置导出/导入（`config.export` / `config.import`）

### 文件操作与扩展

- 复制/剪切/粘贴/删除（回收站）/重命名/属性/打开/新建/全选/定位，确定式 `SelectByPath` 等；`IFileOperation` 单一执行点 + 四路判败，复制/移动撤销记录按真实落点
- 撤销/重做双栈（机器不穿透人工）+ 状态栏撤销历史弹层（`file.undo.to` 可撤到任意一步）
- 目录统计：`OpTopN`（谁最占空间）、`OpDiskUsage`（这目录多大），带 deadline 与截断标志
- 搜索框（范围菜单/搜索历史/原生搜索文件夹）、预览窗格、导航窗格、复选框模式、深浅色跟随系统、Per-Monitor V2 高 DPI
- AI 面板：WebView2 + Node 服务，可折叠、未读标记、状态栏 chip；缺少运行时依赖时主浏览器照常可用
- 可选资源管理器右键菜单动词，全部写 `HKCU`，不需要管理员

---

## 界面

```
┌──────────────────────────────────────────────────────────────┐
│ [标签A ×][标签B ×][+]                          ← 26 px 自绘   │
├──────────────────────────────────────────────────────────────┤
│ [←][→][↑][⟳][▦] │ ‹ C: › Users › me › Desktop ⌄ │ ← 30 px   │
├──────────────────────────────────────────────────────────────┤
│ [书签] [书签] [分类 ⌄]                        ← 26 px（可隐） │
├──────────────────────────────────────────────────────────────┤
│ ┌──────────┬───────────────────────────────────────────────┐ │
│ │ 导航树    │  真实 Shell 视图（IExplorerBrowser）           │ │
│ │(shell 画)│  右键/拖放/重命名/缩略图/排序分组 全由 shell    │ │
│ └──────────┴───────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│ 12 个项目                                    C:\Users\me\... │ ← 24 px
└──────────────────────────────────────────────────────────────┘
```

四栏高度（逻辑像素，随 DPI 缩放）：标签 26 / 导航 30 / 书签 26 / 状态 24。下拉菜单不是系统菜单，而是自绘弹层（`WS_POPUP` + owner-draw `LISTBOX`），以便带图标、跟随深浅色。

---

## 快速开始

### 环境

- 运行：Windows 10/11 x64，无需安装运行时依赖。
- 构建：项目使用 Windhawk 自带的 `clang++`（20.1.3，`x86_64-w64-mingw32`），`-std=c++20`，静态链接 libstdc++/libgcc。无 CMake/Makefile —— 这是刻意的。
- AI 面板（可选）：WebView2 Runtime（Evergreen）+ Node.js >= 18（PATH 或 `EXBAR_NODE`）。缺失时只有 AI 面板显示占位页，浏览器主功能不受影响。

### 构建与运行

```bash
cd E:/ExBarBrowser
bash build.sh                 # 增量构建；build.bat 供 cmd 使用
bash build.sh --clean         # 换目录/换机器后必须（PCH 记绝对路径）
./ExBarBrowser.exe "C:\Windows"
```

- 构建由 `build_fast.py` 驱动（PCH + 分 TU 增量 + 并行），`build.sh` / `build.bat` 只是透传参数的薄包装。常用参数：`--release`（发布构建）、`-j 16`（并行数）、`--stats`（各 TU 耗时）、`--out <路径>`（只验证能否编过）。
- 调试产物 `ExBarBrowser.exe` 带 `-g`；`bash build.sh --release` 产出 strip 后的 `ExBarBrowser.release.exe`，两者并存互不覆盖。若 exe 正被占用，会保留 `.new.exe` 而不破坏运行中的程序。
- 清单（源文件/库）的唯一真相源在 `build_fast.py` 顶部；新增源文件无需改清单（自动扫描 `src/*.cpp`）。

命令行参数：

```bat
ExBarBrowser.exe                      :: 默认打开用户主目录
ExBarBrowser.exe "C:\Windows"         :: 打开指定目录
ExBarBrowser.exe /select,"C:\a\b.txt" :: 打开所在目录并选中该文件
ExBarBrowser.exe --ai                   :: 进程内开 AI 命名管道通道（不写 settings.json）
ExBarBrowser.exe --mcp-stdio          :: 以 MCP 服务器方式启动（stdio 进、stdio 出）
```

已有一个实例在跑时，上述命令不会新起进程，而是让已有窗口开新标签并前置；`--mcp-stdio` 例外（它自己就是服务器）。要显式新开进程用 `--new-window` 或设 `EXBARBROWSER_MULTI=1`（独立 profile）。

### 可选：注册右键菜单

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1           # 只加右键动词
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Takeover # 额外接管"双击文件夹"默认打开方式
powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1          # 还原
```

脚本不会自动运行，需手动执行。全部写 `HKCU`，不需要管理员；Takeover 会先把原设置备份到 `backup\Folder-shell.reg`。

### AI / MCP 接入

命名管道供本机脚本调用（`tools/ebbctl.py`、`tools/agentctl.py`），标准 MCP 客户端走 stdio：

```jsonc
{ "command": "E:\\ExBarBrowser\\ExBarBrowser.exe", "args": ["--mcp-stdio"] }
```

令牌由程序自动注入，客户端不用配；stdin EOF 时进程静默退出。细节见 [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md)。

---

## 操作速查

| 键 | 动作 |
|---|---|
| `Ctrl+T` / `Ctrl+W` | 新建 / 关闭标签 |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | 下一个 / 上一个标签 |
| `Ctrl+Shift+T` / `Ctrl+K` | 重新打开关闭的标签 / 复制标签 |
| `Ctrl+Shift+W` / `Ctrl+Shift+←` / `Ctrl+Shift+→` | 关闭其他 / 左侧 / 右侧 |
| `Ctrl+L` / `F4` | 聚焦地址栏（`Enter` 提交、`Esc` 还原、`Tab` 补全） |
| `Alt+←` / `Alt+→` / `Alt+↑` / `Alt+Home` | 后退 / 前进 / 上级 / 主文件夹 |
| `F5` | 刷新 |
| `Ctrl+Shift+C` / `Ctrl+Alt+C` / `Ctrl+Alt+Shift+C` | 复制完整路径 / 文件名 / 当前目录 |
| `Ctrl+D` / `Ctrl+Shift+B` | 收藏当前目录 / 切换书签栏 |
| `Ctrl+H` / `Ctrl+Shift+H` / `Ctrl+Alt+E` | 隐藏文件 / 系统文件 / 扩展名 |
| `Ctrl+Shift+1..8` | 8 档视图模式 |
| `Ctrl+M` / `Ctrl+Shift+M` | 当前目录开终端 / 管理员终端 |
| `Ctrl+Shift+P` / `Alt+K` | 命令面板 / 快捷键管理器 |
| `Shift+F10` / `F6` / `Shift+F6` | 文件区右键菜单 / 焦点循环 |
| 序列 `g g` / `g t` / `x l a` … | 169 条，3000 ms 内连按；`/ ! # @ …` 出模式总览 |
| 鼠标中键点标签 / 滚轮在标签栏 | 关闭标签 / 切换标签 |
| 双击书签栏空白 / 状态栏空白 | 切换书签栏 / 复制当前目录 |

导航栏与书签栏的交互细节（分段 chevron、折叠钮、图标列展开、键盘导航等）见 [docs/INPUT.md](docs/INPUT.md)。

---

## 当前限制

| 限制 | 说明 |
|---|---|
| 搜索范围 | 当前目录 + 搜索历史；无 Everything 式全盘索引 |
| 详细信息窗格 | 预览窗格已有（`Alt+P`），多列详细信息视图未做 |
| 无 i18n | 文案目前硬编码中文 |

---

## 文档

| 分组 | 文档 |
|---|---|
| 上手与规范 | [CHANGELOG.md](CHANGELOG.md) · [CONTRIBUTING.md](CONTRIBUTING.md) · [DEVELOPMENT.md](docs/DEVELOPMENT.md) · [INPUT.md](docs/INPUT.md) · [AI-INTERFACE.md](docs/AI-INTERFACE.md) · [NAMING.md](docs/NAMING.md) |
| 架构与设计 | [ARCHITECTURE.md](docs/ARCHITECTURE.md) · [ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) · [ARCHITECTURE-TARGET.md](docs/ARCHITECTURE-TARGET.md) · [DECISIONS.md](docs/DECISIONS.md) · [FULL-COVERAGE.md](docs/FULL-COVERAGE.md) · [ROADMAP.md](docs/ROADMAP.md) |
| 对标与差距 | [DOPUS-PARITY.md](docs/DOPUS-PARITY.md) · [EXPLORER-GAP-AUDIT.md](docs/EXPLORER-GAP-AUDIT.md) · [COMPETITIVE-GAP-2026-11-21.md](docs/COMPETITIVE-GAP-2026-11-21.md) · [PARITY.md](docs/PARITY.md) · [PRIORITY-STATUS.md](docs/PRIORITY-STATUS.md) |

> 更多过程性调研件（架构提案、专项审查、平行产品笔记等）在 `docs/` 下按文件名查阅。
> 本文不复制版本更新记录 —— 变更历史看 [CHANGELOG.md](CHANGELOG.md)。

---

## 开发

### 命令速查

```bash
bash build.sh                        # 增量构建
bash build.sh --clean                # 全量重建
bash build.sh --release              # 发布构建（独立 obj_release/）
bash tests/run_all.sh                # 全部单测（数量自动枚举）
bash tests/run_all.sh bookmark       # 只跑名字含 bookmark 的
python tools/check_commands.py       # 命令表 / keymap / 处理器三方对账
python tools/check_ai_meta.py        # AI 元数据 / schema / 断言 / 人机等价
python tools/check_components.py     # 唯一实现 / 禁令 / 接线 / 输入纪律
python tools/gen_keymap.py           # data/keymap.json -> src/keymap.gen.h
python tools/reclaim.py              # 构建残留回收（预演）
python tools/agentctl.py serve --wait 30   # 构建 + 启动 + 常驻持有实例
python tools/agentctl.py status            # exe/源码新鲜度、通道、工具数
python tools/agentctl.py tools --grep bmk  # 列出 AI 可见命令
python tools/agentctl.py call bmk.list '{}'  # 调命令（进程外，无输入模拟）
```

跑全量单测时，并行会话必须各设 `EXBAR_TEST_OUT=工程内/各自子目录`：共用同一个输出目录会互踩日志，导致"编译失败"的条目其实带着别的会话的通过日志。

### 关键约束

1. **状态只有一份**：标签状态只在 `App::Tab` 里，chrome 控件是纯投影；`ShellView` 是唯一接触 `IExplorerBrowser` 的地方。
2. **所有动作走命令总线**：按钮、热键、序列、鼠标规则、命令面板、AI 是同一条入口，绕过会造成行为分裂。
3. **单一来源**：颜色/字号/间距走 `theme_`；品牌串、窗口类名、管道名、日志名只在 `src/identity.h`；有关键盘/鼠标输入与外部数据读取只有一处实现，由 `check_components.py` 守护。
4. **R21 输入纪律**：禁止模拟鼠标/键盘、抢焦点、循环点击。`SendInput` / `SetCursorPos` / `PostMessage(WM_LBUTTON*)` / `pyautogui` 一类全仓零容忍；验证逻辑写单测、看界面用 `scripts/shot.py`、必须真点才成立的流程用「人工点、脚本只观察」。`src/` 内本就有的 `SetForegroundWindow` 不受限（判据是触发源是人还是脚本）。
5. **单文件 ≤500 行棘轮**：超限文件记在 `tests/baselines/file-length-baseline.json`，欠账只减不增；按职责拆，不按行数硬切。
6. **无第三方依赖**：单 exe 零运行时依赖是目标；新增依赖须先证明自写 200 行解决不了（现有豁免见 [docs/DECISIONS.md](docs/DECISIONS.md) D14）。

### 目录结构

```
ExBarBrowser/                       独立 git 仓库（main）
├─ src/                             按职责命名的平铺多文件
│  ├─ main.cpp / app.*              进程入口、窗口装配与生命周期
│  ├─ tab_strip* / tab_set          标签栏与标签集合
│  ├─ nav_bar* / nav_model / nav_path / nav_history   导航
│  ├─ bookmark_bar* / bookmarks*    书签
│  ├─ status_bar / view_stats       状态栏
│  ├─ shell_view / shell_ops / disk_scan              Shell 视图与文件操作
│  ├─ commands* / cmd_naming        命令目录与总线
│  ├─ ai_api / ai_server            命令的 AI 投影与管道服务
│  ├─ aichat_* + aichat/            AI 面板（WebView2 + Node）
│  ├─ chord / keymap* / sequence* / input_router / mouse_rules   输入端
│  ├─ ui_* / theme / icon_*         绘制原语与视觉令牌
│  ├─ platform_win / str / log / clipboard / config / json       基础设施
│  └─ identity.h / common.h         工程标识、共享类型（唯一来源）
├─ tests/                           单测（每个 .cpp 自带 main + Check）
├─ tools/                           静态检查、构建缓存、诊断探针
├─ scripts/                         截图、右键菜单注册、发布打包与校验
├─ docs/                            设计、规则、对标与调研文档
├─ resources/                       应用图标与版本资源
├─ data/ state/ logs/ cache/ runtime/ profiles/   运行期用户数据（不入库）
├─ build.sh / build.bat / build_fast.py            构建入口与实现
└─ README.md / CHANGELOG.md / CONTRIBUTING.md / AGENTS.md / LICENSE
```

入库的是源码、脚本、文档、生成头与构建入口；`*.exe` / `obj*/` / `backup/` 及整个运行期数据目录由 [.gitignore](.gitignore) 排除。新克隆仓库首次启动没有配置文件是预期行为：键位走内置默认表，书签为空树，`settings.json` 首次保存时创建。

> 规模数字随活跃开发持续变化；以口径命令为准：
>
> ```bash
> ls src/*.cpp | wc -l; ls src/*.h | wc -l; cat src/*.cpp src/*.h | wc -l
> ls tests/*.cpp | wc -l; grep -ho 'Check(' tests/*.cpp | wc -l
> python tools/check_commands.py | head -1
> python tools/check_ai_meta.py | head -3
> ```
>
> 最近一次实测（2026-11-27）：218 个 `.cpp` / 137 个 `.h` / 约 7.07 万行，75 个单测 / 1350 项断言，命令表 243（已实现 236）、AI 暴露 196。

---

## 排错

| 现象 | 原因 / 处理 |
|---|---|
| 链接报 `Permission denied` | 目标 exe 正在运行；关掉实例，或 `--out` 到临时路径 |
| 每个 TU 报 `malformed or corrupted AST file` | PCH 里记着旧绝对路径（搬目录后失效）；`bash build.sh --clean` |
| 改了 `data/keymap.json` 但默认键没变 | 内置兜底在 `src/keymap.gen.h`；`python tools/gen_keymap.py` |
| 导航失败、日志 `parse hr=0x80070057` | 路径含正斜杠；应在入口统一转反斜杠 |
| 下拉菜单点一下就没 | 弹层在 `WM_ACTIVATE(WA_INACTIVE)` 时关闭；确认主窗口是前台 |
| 弹层出现在屏幕角落 | 传了客户区坐标；先 `ClientToScreen` |
| 视图白底但工具栏深色 | uxtheme 未文档化导出取不到（旧 Win10 / 安全软件拦截）；仅视觉降级 |
| 第二次启动没开新窗口 | 默认把路径转发给已有窗口并激活（预期行为）；要新开用 `--new-window` 或 `EXBARBROWSER_MULTI` |
| 日志像乱码/带空字符 | 日志是 UTF-16 LE 无 BOM；用 `Get-Content $env:TEMP\ExBarBrowser.log -Encoding Unicode` |
| 配置回环偶发失败 | 杀软/索引服务短暂占用文件；已有占用类错误有限次重试 |

诊断入口：

- 导航链路日志 `%TEMP%\ExBarBrowser.log`（UTF-16 LE，4 MB 轮转），记录 `Initialize / parse / item / browse / complete` 五步 HRESULT 与路径，另有 `[click]` / `[popup]` 两类调试行。
- `data/audit.log`：每条命令的 actor、参数、结果与拒绝原因。
- `python scripts/shot.py out.png`：`PrintWindow` 截图，被遮挡也能抓；AI 面板的 WebView2 GPU 合成内容抓不到。

---

## 贡献与许可

欢迎提交 issue 与 PR；代码规范、提交约定和验证清单见 [CONTRIBUTING.md](CONTRIBUTING.md)。本项目以 [MIT](LICENSE) 许可证发布，第三方组件见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)，安全策略见 [SECURITY.md](SECURITY.md)，行为准则见 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。
