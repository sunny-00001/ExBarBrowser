# ExBarBrowser

**独立进程的文件浏览器**（360 文件夹路线）：自己的窗口、自己的标签栏、自己的视图，**完全不注入 explorer**。

本仓库**自包含**（2026-11-20 起从 `D:\Windhawk` 主仓独立出来，见 §3.4）：不引用、不修改任何
ExBar mod 分片，可并存、可整体拷走、可单独卸载。

| 项 | 现状（2026-11-25，发布构建入库当日） |
|---|---|
| 阶段 | **P3 主体完成 + P4 起步**：标签/导航/书签/状态栏 + 键盘/序列/鼠标 + 命令契约 + AI 接口 + 搜索/预览/会话/AI 面板已落地，日常浏览可用 |
| 规模 | `src/` 约 6.46 万行（198 个 `.cpp` + 130 个 `.h`）；`tests/` 73 个单测、1289 项断言（2026-11-26 实测；仓库在活跃并行开发，**行数每几分钟就变**，要看当前值见下方口径命令） |
| 产物 | 调试 exe 59.7 MB（含 `-g`）；`--release` 发布 exe **3.0 MB**（-O2 + `-s` strip，无调试符号，内嵌 `VS_VERSION_INFO`），静态链接 libstdc++/libgcc。两个文件名并存、互不覆盖；正式分发用 `scripts/package_release.py` 打成的 zip（含 WebView2Loader.dll 与 aichat/） |
| 命令 | `check_commands.py` 首行给出：命令表 241 条 / 已实现 234 / 处理器 234 / keymap 引用 243 条；`check_ai_meta.py` 给出对 AI 暴露 194 条。**这些数每加一条命令就变，本文不写死，以脚本输出为准** |
| 配置 | 便携模式，profile 根下分层：`data/`（settings/bookmarks/keymap/mouse/aichat/models/mcp）、`state/`（会话/历史/标签/窗口）、`logs/`（审计/事件）、`cache/`（WebView2）、`runtime/`（AI 发现文件） |
| 验证 | clang++ 20.1.3 零警告；调试全量约 20s（198 TU，自适应并行；带跨会话 obj 缓存），发布 `--release --clean` 首次全量约 21min（-O2、独立 obj 目录、无缓存命中）；冒烟为**人工操作 + 只读证据**（日志 / `exbar/state` / 截图），不注入键鼠（`docs/MIGRATION-PLAN.md` §8.2、规则 R21） |
| 检查 | `check_ai_meta`（28 组，含 R20 人机等价）与 `check_commands`（三方对账，含 CmdId 顺序与 legacy 名基线）通过；`check_components` 的单文件 ≤500 行门禁有 **14 个文件超限、全部记在 `tests/baselines/file-length-baseline.json` 欠账里**（棘轮只减不增：涨过报错、降下来自动收紧），评估面清单随本轮改动重签。CI 强门见 `.github/workflows/ci.yml`、发布门见 `.github/workflows/release.yml` |
| 版本控制 | **独立仓库**（`E:\ExBarBrowser`，分支 `main`）：源码/脚本/文档/生成头入库，构建与运行产物由 `.gitignore` 排除 |

> 本文所有路径均相对仓库根。文档基线：**2026-11-26**（代码仍在快速演进，
> `src/` 是唯一真相；发现文档与代码不符时先信代码，并顺手改文档）。
> 规模数字的口径（可直接复制执行）：
> ```bash
> ls src/*.cpp | wc -l                                  # .cpp 个数
> ls src/*.h   | wc -l                                  # .h 个数
> cat src/*.cpp src/*.h | wc -l                         # 总行数
> ls tests/*.cpp | wc -l                                # 单测个数
> grep -ho 'Check(' tests/*.cpp | wc -l                  # 断言总数
> grep -c '^    {CmdId::' src/commands_table.cpp src/commands_table_tools.cpp   # 命令条数
> ls -l ExBarBrowser.exe | awk '{printf "%.1f MB\n", $5/1048576}'   # exe 体积
> python tools/check_ai_meta.py | grep -cE '^[0-9]+\.'   # 静态检查组数
> ```
> 重新量之前不要凭印象改这几个数。仓库处于活跃并行开发，本文所有数字都是**快照**；
> 与代码不符时先信代码，并把本文更新到新值。
> 想先看"为什么这么设计"，读 [docs/DECISIONS.md](docs/DECISIONS.md)；
> 想知道"比 ExBar mod 少了什么"，读 [docs/PARITY.md](docs/PARITY.md)；
> 想知道"名字为什么这么起、哪些名字不许改"，读 [docs/NAMING.md](docs/NAMING.md)。

---

## 1. 定位与边界

```
ExBarBrowser.exe（本进程）
├─ 自绘标签栏        tab_strip.cpp（图标/溢出/拖拽/锁定/右键）
├─ 导航栏 + 地址框    nav_bar.cpp / nav_bar_actions.cpp（自动补全/最近访问/排序分组）
├─ 书签栏            bookmark_bar.cpp（Netscape 书签树 + 图标列子目录 + 分类下拉 + 溢出）
├─ 自绘下拉弹层      popup_list.cpp（菜单/历史/视图/文件夹树共用）
├─ 再点即收          popup_toggle.cpp（同一锚点连点第二下收起，所有弹层共用一份）
├─ 文件夹树下拉      folder_tree.cpp（面包屑 chevron 与书签图标共用的唯一入口）
├─ 状态栏            status_bar.cpp（9 段：计数/日期/路径/容量/磁盘/眼睛/类型）
├─ 输入端            input_router.cpp（组合键/序列/鼠标规则 → 命令）
└─ 每个标签一个 IExplorerBrowser（真实 Shell 视图）
        ├─ 左侧导航窗格：shell 原生（EBO_SHOWFRAMES，NamespaceTreeControl）
        └─ 右键菜单 / 拖放 / 就地重命名 / 缩略图 / 排序分组 由系统 shell 提供
```

- **不注入、不挂钩子**：explorer 崩不崩与本程序无关；本程序崩了 explorer 也不受影响。
- **多实例 + 激活优先**：再次启动默认把路径经 `WM_COPYDATA` 转给已有窗口开新标签并激活（360 行为）；显式 `--new-window`（"在新窗口打开"命令自动带上）直接新开一个进程实例。不存在单实例锁——永不因"已在运行"而拒绝启动。
- **视图不自绘**：`IExplorerBrowser` 是微软文档化的"在自有窗口里托管资源管理器视图"的 API，
  右键菜单、拖放、重命名、缩略图、排序分组、命名空间全部由 shell 提供，不需要重写视图。
- **导航窗格不自绘**：`SetOptions(EBO_SHOWFRAMES)` 让 shell 在浏览器内部画自己的导航树
  （快速访问 / 此电脑 / 驱动器 / 展开收起 / 右键 / 拖放 / 与视图自动同步）。

### 1.1 与 ExBar mod 的关系

| | ExBar mod（`Exbar.cpp`） | ExBarBrowser（本工程） |
|---|---|---|
| 载体 | Windhawk mod，注入 `explorer.exe` | 独立 EXE，自有进程 |
| 翻译单元 | 单 TU（引擎 stdin 单源调 clang++） | 真多文件、真命名空间 |
| 改造对象 | 系统资源管理器窗口 | 自己造窗口 |
| 能做的事 | 改 explorer 的标签栏/导航栏/命令栏，加书签栏、搜索、命令面板、AI 面板 | 只能做自己窗口里的东西；改不了系统 explorer |
| 崩溃面 | 崩了连带 explorer | 崩了只影响自己 |
| 现状 | 约 7.7 万行（17 个分片）、127 条命令目录（`data/capabilities.json`） | 约 6.1 万行、命令目录见 `check_commands.py`（219 条，规模接近但组织为真多文件，命令更多） |

两者**共享设计口径而非代码**：主题色令牌、面包屑交互语义、标签栏视觉、快捷键习惯都对齐 ExBar mod
（`ebnav` / `ebtab` / `ebstatus` 的行为），但一行代码都不互相引用，各自可独立演进、独立卸载。

### 1.2 与 `ExBarUI.exe` 的关系

`ExBarUI`（`D:\Windhawk\ExBarUI`）是"把 explorer 内嵌面板搬到 WebView2"的宿主进程，解决的是
**mod 的 UI 迭代速度**问题。ExBarBrowser 的 chrome 目前是 GDI 自绘（启动快、零依赖、无 250 MB WebView2 开销）。
两者的视觉令牌同源，未来若 chrome 要 Web 化，走 `ui/theme/tokens.css` 而不是另起一套色值 —— 详见
[docs/ROADMAP.md](docs/ROADMAP.md) P4。

---

## 2. 当前能力

### 2.1 已实现

| 能力 | 实现位置 | 说明 |
|---|---|---|
| 多标签 | `src/app_tabs.cpp`、`src/tab_set.cpp` | 新建/关闭/切换/复制/重开/移动/MRU；关闭最后一个标签即退出 |
| 自绘标签栏 | `src/tab_strip.cpp` + `_input`/`_layout`/`_paint`/`_actions` | 图标 + 标题 + 关闭钮 + 锁定态；宽度自适应（112–230 px）；激活下划线 |
| 标签溢出与手势 | `src/tab_strip_input.cpp`、`src/tab_strip_layout.cpp` | 溢出横向滚动 + `»` 全部标签菜单；拖拽排序；中键关闭；滚轮切换；双击空白新建 |
| 标签右键菜单 | `src/tab_strip_actions.cpp`、`src/app_commands_tabs.cpp` | 关闭族/复制/锁定/复制路径/重新打开 |
| 导航栏（5 按钮） | `src/nav_bar.cpp`、`src/nav_bar_draw.cpp` | 后退/前进/向上/刷新/视图；MDL2 字形；分裂箭头出方向历史；盘根时"向上"置灰 |
| 面包屑 | `src/nav_bar.cpp`、`src/nav_bar_draw.cpp`、`src/nav_model.cpp` | 前导图标 + 折叠钮 + 每级 20 px chevron；分段可点；向上导航保留 ghost 段 |
| 地址编辑与补全 | `src/nav_bar_actions.cpp` | 点空白/`Ctrl+L`/`F4` 进入；`Enter` 提交、`Esc` 还原；`Tab` 同级循环补全；`SHAutoComplete` 目录优先 |
| 方向历史 / 最近访问 | `src/nav_bar_actions.cpp`、`src/nav_history.cpp` | 分裂箭头与右键出单向历史（24 条）；地址框下拉合并前进/后退 + 最近访问（去重） |
| 视图菜单 | `src/nav_bar_actions.cpp` | 8 档模式 + 排序（名称/日期/类型/大小）+ 分组（无/类型/日期/大小） |
| 自绘下拉弹层 | `src/popup_list.cpp` | 图标 + 可变行高（分隔线 2 px）+ 悬停/选中底色 + 深色边框；替代 `TrackPopupMenu` |
| 再点即收 | `src/popup_toggle.cpp` | 所有弹层同一契约：弹层开着时点它的锚点 → **收起**而不是重弹。真因是 `WM_ACTIVATE` 抢在 DOWN/UP 前关窗（捕获不阻止激活切换）；失活按"持捕获+前景同线程"分流，外点等 UP 才关 |
| 书签栏 | `src/bookmark_bar.cpp`、`src/bookmarks.cpp` | Netscape + `EXBAR_*`；**点左侧图标列子目录**（与面包屑分隔符同一实现）/ 点文字打开或列分类 / 溢出 `»` / 右键增删改 / 拖入文件夹 / 双击空白显隐；书签存于 `data\bookmarks.html` |
| 文件夹树下拉 | `src/folder_tree.cpp`、`src/popup_list.cpp` | 面包屑 chevron 与书签图标共用：枚举直接子文件夹 → 懒加载树 → 就地逐级展开（`NavModel::ChildFolders` 是唯一枚举实现） |
| 书签导入导出 | `src/bookmark_io.cpp`、`src/bookmark_drop.cpp` | `IFileOpenDialog`/`IFileSaveDialog` 走 Netscape HTML；OLE 拖放接收虚拟项（`CFSTR_SHELLIDLIST`） |
| 状态栏 9 段 | `src/status_bar.cpp`、`src/view_stats.cpp` | 文件夹/文件/已选、修改日期（含排序箭头）、路径、选中容量、磁盘/容器、隐藏三态、类型；tooltip + 段点击 + 右键显隐 |
| 视图开关/排序/分组 | `src/shell_view.cpp` | 8 档模式、排序、分组、隐藏/系统/扩展名、隐藏三态、导航窗格、预览窗格开关（经 `IExplorerPaneVisibility` 跨目录强制保持用户意图） |
| 文件操作 | `src/shell_ops.cpp` | 复制/剪切/粘贴/删除（回收站）/重命名/属性/打开/新建文件夹/新建文本/全选/定位首末项；含确定式 `SelectByPath`/`SelectNone`/`RenameTo` |
| 键盘快捷键 | `src/keymap.cpp`、`src/chord.cpp`、`src/input_router.cpp` | 读 `data/keymap.json`（105 条热键，直接编辑该文件）；默认表由 `tools/gen_keymap.py` 生成到 `src/keymap.gen.h`，无双副本 |
| 序列键 | `src/sequence.cpp`、`src/sequence_hint.cpp`、`src/sequence_overlay.cpp` | **169 条**（`data/keymap.json` 的 `sequences`）；前缀状态机 + 3000 ms 超时 + 候选弹层 + `Esc` 取消；`/ ! # @ …` 唤起模式总览弹窗（命令/书签/标签/历史/跳转/…），打字过滤+上下导航+回车派发 |
| 鼠标规则 | `src/mouse_rules.cpp`、`src/shell_input.cpp` | 目标区 × 按钮 × 触发（单击/双击/长按/四向滚轮）× 修饰键；shell 视图命中经子类 + UIA 汇入同一条规则引擎 |
| 快捷键管理器 | `src/hotkey_manager.cpp` | 范围筛选、改键/清除/恢复默认、冲突/系统占用/未实现标记 |
| 命令管线 | `src/commands.cpp` | 命令目录（与 mod 同 id 同组），参数对象化 `Invoke(id, Args, actor)`；`audit.log`；`tools/check_commands.py` 三方对账 |
| 命令命名空间 | `src/cmd_naming.cpp` | 旧名 ↔ 点分名双向映射（`nav.back` / `fs.delete`），`FindCommand` 两个方向都认；既有 keymap/序列/脚本零改动 |
| 前置条件与结构化结果 | `src/commands.h`、`src/ai_api.cpp` | 声明式 `Requirements` + `CanExecute()`；`RegisterWithResult` 通道返回 JSON（`OpListDir`/`OpStat`/`OpTopN`/`OpDiskUsage`） |
| 磁盘统计 | `src/disk_scan.cpp` | `OpTopN`（谁最占空间）/ `OpDiskUsage`（这目录多大）；带 deadline、`truncated`/`timedOut`、不跟随重解析点 |
| AI 操作接口 | `src/ai_api.cpp`、`src/ai_server.cpp` | JSON-RPC 2.0 over 命名管道：`initialize`/`tools/list`/`tools/call`/`exbar/state`；令牌 + 危险命令双门；见 `docs/AI-INTERFACE.md` |
| 命令面板 | `src/app_commands.cpp` | 列出全部已实现命令，选中即执行（与快捷键同一条 `CommandBus`） |
| 配置与状态 | `src/config.cpp`、`src/json.cpp`、`src/text_file.cpp` | 便携模式：`data/` 放配置、`state/` 放会话/历史/窗口、`logs/` 放审计与事件、`cache/` 放 WebView2、`runtime/` 放 AI 发现文件；窗口几何、会话、锁定标签、最近访问、状态栏段、视图/排序/分组、窗格偏好均在 profile 根下 |
| 会话 | `src/app_commands_misc.cpp`、`src/config.cpp` | 启动恢复上次标签组与关闭历史；`SessionSave` / `SessionRestore` |
| 深浅色 / 高 DPI | `src/theme.cpp`、`src/main.cpp` | 跟随系统；Per-Monitor V2；`WM_DPICHANGED` 重建令牌与字体 |
| 激活优先启动 | `src/main.cpp`、`src/identity.h` | 有窗口则 `WM_COPYDATA` 转发 + 激活（magic `0xEBB1`，见 `id::kCopyDataMagic`）；`--new-window` / `EXBARBROWSER_MULTI` 直接新开 |
| 命令行 | `src/main.cpp` | 目录参数、`/select,<文件>`（打开父目录） |
| 基础设施收编 | `platform_win` / `str` / `log` / `clipboard` / `ui_draw` / `ui_palette` / `icon_glyph` | 窗口类注册、字符串、日志轮转、剪贴板、量字/填充/描边/双缓冲、色值令牌、字形码位各只有一份实现；`tools/check_components.py` 拦回退 |
| 工程标识 | `src/identity.h` | 品牌串、11 个窗口类名、管道、日志文件名、`WM_COPYDATA` 魔数的唯一来源；改名只动一个文件。规范见 `docs/NAMING.md` |
| 右键菜单动词 | `scripts/install.ps1` | 可选接管；全部写 `HKCU`，不需要管理员 |

### 2.2 未实现（诚实清单）

| 缺口 | 影响 | 计划 |
|---|---|---|
| 搜索仅当前目录 / 最近历史 | 无 Everything 式全盘索引（`evipc` 未接） | P3 可选：全盘 `evipc` |
| 详细信息窗格 | 预览窗格已接（`view.toggle.preview` / `Alt+P`），但 Opus 式多列详细信息视图未做 | P3 |
| 无 i18n | 文案硬编码中文 | 不做 |

> 2026-11-25 更新：以下缺口已补齐，从本表移除 —— 搜索框（`Ctrl+F` /
> `search.query.set` / 搜索范围 / 搜索历史）、预览窗格（`Alt+P` / `view.toggle.preview`）、
> 复选框模式（`view.toggle.checkboxes`）、会话管理 UI（`session.list` / `session.named.*` /
> `session.save` / `session.restore`）、AI 面板（WebView2 + Node 服务，见
> [docs/DECISIONS.md](docs/DECISIONS.md) D14）。
>
> 更早（2026-11-21）已移除：工具集（哈希 / 查重 / 文件夹比对）、选择族（含按类型选择）、
> 撤销、批量重命名。撤销走 shell 自己的 `IFileOperation` 撤销栈（落盘操作都带
> `FOF_ALLOWUNDO`），不是自建栈 —— 见 CHANGELOG 本轮条目。

完整对照表（按 ExBar mod 的 127 条能力逐组比对，本工程自研命令另计），见
[docs/PARITY.md](docs/PARITY.md)。

---

## 3. 快速开始

### 3.1 构建

```bash
cd E:/ExBarBrowser
bash build.sh          # 或 build.bat（cmd）
```

- 工具链：Windhawk 自带 `C:\Program Files\Windhawk\Compiler\bin\clang++.exe`（20.1.3），
  `-target x86_64-w64-mingw32`，`-std=c++20 -O1 -g -mwindows`。
- 构建由 `build_fast.py` 驱动（PCH + 分 TU 增量 + 并行），`build.sh`/`build.bat` 只是透传参数的薄包装。
  实测全量 19.8s（PCH 3.5s + 76 TU 并行）、改一行 ~1s、无改动 0.3s。常用：`--clean` 全量重建、
  `-j 16` 并行数、`--stats` 看耗时、`--objdir <dir>` 多会话隔离中间目录。
- **换目录/搬机器后必须 `--clean`**：PCH 是 clang AST 二进制，里面记着绝对路径；
  仓库从 `D:\Windhawk\ExBarBrowser` 搬到 `E:\ExBarBrowser` 后，旧 `obj/common.h.gch`
  会让每个 TU 报 `malformed or corrupted AST file`（`build_fast.py` 不会自动察觉路径变更）。
- 产物：`ExBarBrowser.exe`。先链到 `ExBarBrowser.new.exe` 再原子替换；若 exe 正被占用，保留
  `.new.exe` 并提示，不破坏运行中的程序（关掉实例后重跑一次即可，只需 ~0.3s）。
  只验证能否编过用 `bash build.sh --out /tmp/x.exe`。
- 应用图标与版本信息：`resources/app.ico`（8 档 16–256px）+ `resources/app.rc` 经
  `D:\MinGW\bin\windres.exe` 编成 `obj/app.res.obj` 并链进 exe。版本号来自仓库根 `VERSION`
  当前版本以仓库根 `VERSION` 为准（发布包/manifest/tag 均从它派生），构建时生成
  `obj/version.h` 注入 `VS_VERSION_INFO`；改版本只改 `VERSION`，改 ico/rc/VERSION 都会自动
  触发资源重编。未找到 windres 时跳过资源、exe 回退到系统默认图标。
  设计见 `resources/icon_preview.png`。
- 清单（源文件/库）唯一真相源在 `build_fast.py` 顶部，新增文件只改这一处。详见 `docs/DEVELOPMENT.md`。

**发布构建**（`--release`）：与调试构建并存，不覆盖 `ExBarBrowser.exe`：

```bash
bash build.sh --release          # -> ExBarBrowser.release.exe（3.0 MB），独立 obj_release/
bash build.sh --release --clean  # 首次全量（-O2，约 21 min）；之后增量很快
```

- 发布标志由 `COMPILE_FLAGS` 推导（`RELEASE_COMPILE_FLAGS`）：`-O1`→`-O2`、去 `-g`、
  加 `-DNDEBUG`（源码实测 0 处 `assert`，不改变行为）；链接期再 `-s` 剥符号。
- 调试 exe 仍带 `-g`（59.7 MB），方便排查；两者文件名不同，可同时保留、可一起跑
  （各自独立 profile 时设 `EXBARBROWSER_MULTI`）。发布模式默认输出到
  `ExBarBrowser.release.exe`、中间目录 `obj_release/`；也可用 `--out` / `--objdir` 覆盖。
- 配置签名含标志差异，调试与发布各自命中独立缓存；`--clean` 只清自己那套 obj。

**发布包**：单独一个 exe 不是完整交付物 —— AI 面板运行时还要 `WebView2Loader.dll`
（必须与 exe 同目录）和 `aichat/`（Node 服务 + Web UI）；`data/`、`state/`、`logs/`、
`cache/`、`runtime/` 都是首次启动自动生成的**用户私有数据**，不能带开发机的设置/书签/密钥。打包器把这些约束固化下来：

```bash
python scripts/package_release.py
# -> dist/ExBarBrowser-v<version>-win-x64.zip
#    dist/ExBarBrowser-v<version>-win-x64.zip.sha256
#    dist/RELEASE-MANIFEST-v<version>.json
```

- 版本号来自仓库根 `VERSION`，同时供 exe 版本资源、manifest、zip 文件名与 GitHub tag
  使用；tag 必须是 `v$(cat VERSION)`。打包器会扫描 JSON 里的 `apiKey`，发现非占位符立即失败。
- 包内不再带 `config.example.json` / `model.example.json`：AI 行为配置在 `data/aichat.json`，
  模型与密钥在 `data/models.json`，由服务首次启动/保存时生成；填好的 `models.json` 是私密数据，不要外发。
- 系统前置条件只有两个，且**只在打开 AI 面板时需要**：WebView2 Runtime（Evergreen）、
  Node.js >= 18（PATH 或 `EXBAR_NODE`）。没有它们，主浏览器照常可用。
- 打 tag 推送会触发 `.github/workflows/release.yml`：复用同一套发布构建 + 打包脚本，
  把 zip、sha256 与 manifest 上传到对应 Release。

### 3.2 运行

```bat
ExBarBrowser.exe                      :: 默认打开用户主目录
ExBarBrowser.exe "C:\Windows"         :: 打开指定目录
ExBarBrowser.exe /select,"C:\a\b.txt" :: 打开所在目录并选中该文件
ExBarBrowser.exe --ai                   :: 进程内开 AI 命名管道通道（不写 settings.json）
ExBarBrowser.exe --mcp-stdio          :: 以 MCP 服务器方式启动（stdio 进、stdio 出）
```

已有一个实例在跑时，上述命令不会新起进程，而是让已有窗口开一个新标签并前置。
`--mcp-stdio` 是例外：它**一定**新起进程（这个进程自己就是服务器），不转发。

### 3.3 注册右键菜单（可选）

```powershell
# 只加右键菜单动词，不改默认打开方式
powershell -ExecutionPolicy Bypass -File scripts\install.ps1

# 额外把"双击文件夹"的默认打开方式改成 ExBar（会先备份到 backup\Folder-shell.reg）
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Takeover

# 还原
powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1
```

全部写 `HKCU`，不需要管理员。**脚本不会自动运行**，需要手动执行。
若接管后本程序不可用，删掉 `HKCU\Software\Classes\Folder\shell` 的默认值即可回到系统行为。

### 3.4 版本控制

本项目是**独立 git 仓库**（`E:\ExBarBrowser`，分支 `main`）。2026-11-20 之前它曾是
`D:\Windhawk` 主仓的一部分（靠根 `.gitignore` §8 白名单放行），迁到 E 盘后已脱钩 ——
两个仓库互不引用，ExBar mod 与本工程各自独立演进。

- **入库**：`src/`（含生成的 `src/keymap.gen.h`，它是构建闭环的一部分）、`tests/`、`tools/`、
  `scripts/`、`docs/`、`resources/`、构建入口（`build.sh` / `build.bat` / `build_fast.py`）、
  `README.md` / `CHANGELOG.md` / `CONTRIBUTING.md`、`.editorconfig` 与 `.gitignore`。
- **不入库**：`*.exe` / `*.obj` / `*.d` / `*.gch` / `*.log` / `shot*.png` / `out*.png`、
  `obj*/`、`backup/` / `.old/`、`*.bak` 类编辑备份，以及**整个 `data/`、`state/`、`logs/`、`cache/`、`runtime/` 下的运行期数据**
  （书签、最近访问、窗口几何、键位习惯因人而异）。清单见 [.gitignore](.gitignore)。
- **提交**：约定式提交（中文正文，`feat|fix|docs|refactor|build|test|...` + scope），
  见 [CONTRIBUTING.md](CONTRIBUTING.md) §2.3。

### 3.5 MCP over stdio（让标准 MCP 客户端直接驱动）

命名管道是给本机脚本用的；Claude Desktop / VS Code / Cursor 这类 MCP 客户端
只认 stdio（拉子进程 + 换行分隔 JSON-RPC）。`--mcp-stdio` 就是这条入口：

```jsonc
// 客户端配置示例（各家字段名略有差别）
{ "command": "E:\\ExBarBrowser\\ExBarBrowser.exe", "args": ["--mcp-stdio"] }
```

令牌由程序自动注入，客户端**不用**配。客户端断开（stdin EOF）时进程静默退出。

细节见 [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md) §3「stdio 入口」。
端到端验证：`python3 tools/probe_mcp_stdio.py path/to/ExBarBrowser.exe`。
- **首次提交前的机器相关项**：`src/keymap.gen.h` 由 `data/keymap.json` 生成，
  已入库，改键后记得 `python tools/gen_keymap.py` 再提交（`check_commands.py` 会拦不一致）。

`data/`、`state/`、`logs/`、`cache/`、`runtime/` 不入库意味着**新克隆的仓库首次启动没有配置文件** —— 这是预期行为：
`keymap` 走 `src/keymap.gen.h` 的内置默认表，书签为空树，`settings.json` 首次保存时自动创建。

**换行符目前是混的**（`.editorconfig` 声明 CRLF，仓库里 66 个文件纯 CRLF、89 个纯 LF）。
编辑文档时如果编辑器按 `.editorconfig` 转成 CRLF，`git diff` 会把整文件算成改动。
要消除这类幻影 diff，两条路选一条：① 加 `.gitattributes` 统一 `text=auto`，
② 把 `.editorconfig` 的 `end_of_line` 改成 `lf` 并按仓库现状（LF 占多数）重排。
本仓库尚未做这个统一，改文档前先看一眼 diff 规模。

---

## 4. 界面与交互

### 4.1 布局

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

四栏高度（逻辑像素，`theme_.S()` 后随 DPI 缩放）：标签 26 / 导航 30 / 书签 26 / 状态 24。

三个 chrome 都是自绘窗口（`WM_PAINT` 里双缓冲 `BitBlt`），视图区是 `ExBarBrowser_ViewArea`，
每个标签在其下挂一个 `ExBarBrowser_TabView` 容器，容器里才是 `IExplorerBrowser`。
下拉菜单不是系统菜单，而是一个 `WS_POPUP` 自绘弹层（`ExBarBrowser_Popup` + owner-draw `LISTBOX`），
这样才带得上图标、跟得上深浅色。

### 4.2 快捷键

| 键 | 动作 |
|---|---|
| `Ctrl+T` / `Ctrl+W` | 新建 / 关闭标签 |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | 下一个 / 上一个标签 |
| `Ctrl+Shift+T` / `Ctrl+K` | 重新打开关闭的标签 / 复制标签 |
| `Ctrl+Shift+W` / `Ctrl+Shift+←` / `Ctrl+Shift+→` | 关闭其他 / 左侧 / 右侧 |
| `Ctrl+L` / `F4` | 聚焦地址栏 |
| `Alt+←` / `Alt+→` / `Alt+↑` / `Alt+Home` | 后退 / 前进 / 上级 / 主文件夹 |
| `F5` | 刷新 |
| `Ctrl+Shift+C` / `Ctrl+Alt+C` / `Ctrl+Alt+Shift+C` | 复制完整路径 / 文件名 / 当前目录 |
| `Ctrl+D` / `Ctrl+Shift+B` | 收藏当前目录 / 切换书签栏 |
| `Ctrl+H` / `Ctrl+Shift+H` / `Ctrl+Alt+E` | 隐藏文件 / 系统文件 / 扩展名 |
| `Ctrl+Shift+1..8` | 8 档视图模式 |
| `Ctrl+M` / `Ctrl+Shift+M` | 当前目录开终端 / 管理员终端 |
| `Ctrl+Shift+P` / `Alt+K` | 命令面板 / 快捷键管理器 |
| 序列 `g g` / `g t` / `x l a` … | **169 条**（`data/keymap.json` 的 `sequences`；前缀按命令组分配，每条已实现命令全覆盖；3000 ms 内连按） |
| `Enter` / `Esc` / `Tab` | 地址栏提交 / 还原 / 同级补全 |
| 鼠标中键点标签 / 滚轮在标签栏 | 关闭标签 / 切换标签 |
| 双击书签栏空白 / 状态栏空白 | 切换书签栏 / 复制当前目录 |
| 右键标签 / 后退前进 / 状态栏段 | 标签菜单 / 方向历史 / 段显隐 |

### 4.3 导航栏行为（对齐 ExBar mod 的 `ebnav`）

| 操作 | 行为 |
|---|---|
| 点击路径分段 | 直接跳到该级 |
| 点击分段右侧 chevron | 下拉**该级自己的**子文件夹（如 `D:\Windhawk` 右侧列出 Windhawk 的内容），层内可逐级展开 |
| 点击 `‹` 折叠钮 | 有折叠时列出被折叠的层级；没有折叠时列"此电脑"顶层（驱动器 / 用户目录） |
| 路径过长 | 从左侧开始折叠（`firstShown_` 递增），折叠后 `‹` 可展开 |
| 点击后退/前进按钮左半 | 执行一次后退/前进 |
| 点击后退/前进的分裂箭头（或右键按钮） | 单向方向历史菜单（最近在前，最多 24 条） |
| 点击地址框右侧 `⌄` | 前进 + 后退合并菜单（各最多 16 条，中间分隔线） |
| 点击视图按钮（最右） | 8 档视图模式菜单，立即作用于当前标签 |
| 点空白处 / `Ctrl+L` | 进入地址编辑（`Esc` 还原，`Enter` 提交） |
| 编辑框内 `Tab` | 按已输入前缀补全同级文件夹（再按循环） |
| 在盘根 | 向上按钮自动置灰 |
| 向上导航 | 面包屑尾部保留 ghost 段（灰显），表示"刚才离开的分支" |

> 盘根（`D:\`）分段的 chevron 同样列出该盘的顶层文件夹。命名空间（`::{GUID}` / `shell:`）里
> 能按名字解析的那些（如此电脑）也能列出子项；NSE 虚拟项不支持，因为 `ChildFolders` 走的是
> `SHParseDisplayName`（mod 那边对这类项是改用 PIDL 枚举的）。

### 4.4 书签栏行为

| 操作 | 行为 |
|---|---|
| 点击**左侧图标** | 下拉**该路径自己的**子目录（与上面"点击分段右侧 chevron"完全同一份实现：`src/folder_tree.*`），层内可逐级展开；没有子目录时直接进入该目录 |
| 点击右侧文字 | 分类 → 逐级下钻列其内容；书签 → 打开该目录 |
| **拖动书签** | 拖到另一位置 → 顶层重排（插入线）；拖到**分类中间** → 装进该分类（整项描边提示）。拒绝拖进自己的子孙（会让子树整棵脱落） |
| **键盘** | `Ctrl+B` 进入本栏；`←/→` 在可见项间移动，`Home/End` 首尾；`Enter`/空格打开（分类则展开）；`Shift+F10` 出该项菜单；`Esc` 焦点退回视图 |
| 溢出 `»` | 只列**被折叠**的项（已显示在栏上的不再重复列） |
| 右键项 | 打开 / 复制路径 / 编辑（名称+路径，清空路径=改成纯分类）/ 删除 / 新建分类 / 添加当前目录 / 撤销 / 清除全部 |
| 右键空白 | 添加当前目录 / 新建分类 / 撤销 / 递归按名称排序 / 清理重复书签 / 重新载入 / 导入 / 导出 / 清除全部 / 隐藏栏 |
| 从资源管理器拖入目录 | 追加为书签；**已在书签里的**会被移到落点的分类（不再静默忽略） |
| 双击空白 | 切换书签栏显隐（`data/mouse.json` 的 `bookmark_empty` 规则） |
| 悬停**图标格** | 该格单独高亮（`pressed` 圆角底），提示"这里能点开子目录" |
| 在图标格上拖动 | 不触发重排（图标格是独立动作，只有文字区可拖着重排） |

> 所有菜单动作都对应一条命令（`bmk.edit` / `bmk.remove` / `bmk.newfolder` / `bmk.undo` /
> `bmk.clear` / `bmk.sort` / `bmk.dedupe` / `bmk.move` / `bmk.add.path` / `bmk.list` / `bmk.focus`），
> 菜单与拖放都调命令 —— 鼠标、键盘、命令面板、AI 走同一份实现（R20 人机等价）。
>
> **导入/导出**按 UTF-8 正确解码百分号序列（`%E4%B8%AD` → 中），UNC 双向支持；
> **保存**遇到"文件已被外部修改"时拒绝覆盖，把内存书签另存为 `bookmarks.html.conflict.html`
> 并提示，绝不静默丢弃任何一方。
>
> 图标格与文字区是两个动作，因此 `Item::iconRc` 是独立命中矩形（几何与 `Paint` 里画图标的槽
> 完全重合）。纯分类节点（只装子书签、没有路径）点图标时退回"列分类内容"，避免点了没反应。
> 书签栏为空时显示一行浅色提示（`Ctrl+D` 收藏 / `Ctrl+B` 聚焦），而不是一条空灰带。
> 图标统一走 `ico::Cached`（进程内按路径共享），`Reload` 变成纯查表，不再每次重建 HICON。
| 点击溢出 `»` | 列出被折叠的书签项 |
| 右键项 / 空白 | 打开/编辑/删除/新建分类/撤销/排序/导入导出/隐藏栏 |
| 从资源管理器拖入目录 | 追加为书签 |
| 双击空白 | 切换书签栏显隐（`data/mouse.json` 的 `bookmark_empty` 规则） |

> 图标格与文字区是两个动作，因此 `Item::iconRc` 是独立命中矩形（几何与 `Paint` 里画图标的槽
> 完全重合）。纯分类节点（只装子书签、没有路径）点图标时退回"列分类内容"，避免点了没反应。

---

## 5. 架构速览

```
main.cpp            进程级：DPI → COM(STA)/OLE → 转发或新开 → 命令行
  └ App            窗口/布局/标签生命周期/命令注册/消息路由
      ├ identity.h 品牌串 / 11 个窗口类名 / 互斥体 / 管道 / 日志名 / 魔数（唯一来源）
      ├ Theme      颜色令牌 + 字体 + DPI 缩放（唯一视觉来源）
      ├ ui::      量字 / 填充 / 描边 / 分隔线 / 圆角 / 字形 / 图标 / 双缓冲 / tooltip（各一份，`ui_draw` + `tooltip_host`）
      ├ platform_win / str / log / clipboard / tooltip_host / icon_glyph   基础设施（各一份）
      ├ TabStrip   只画 + 命中 + PostMessage，不持有标签状态
      ├ NavBar     ├ NavModel    路径 → 分段/父级/子目录枚举（语义走 nav_path）
      │            ├ nav_bar_actions  编辑/补全/菜单/最近访问/排序分组
      │            └ PopupList   无状态弹层（模态，返回选中行）
      ├ BookmarkBar ── BookmarkStore   （Netscape HTML，data\bookmarks.html）
      │             ├ folder_tree     图标点击 → 子目录树（与 NavBar 分隔符共用）
      │             └ bookmark_io/drop  导入导出 / OLE 拖放
      ├ StatusBar  ── ViewStats（IFolderView2 / IShellItem2）
      ├ InputRouter ├ Chord / Keymap / Sequence / MouseRules
      │             ├ shell_input   shell 视图命中 → MouseTarget
      │             └ CommandBus   命令目录 + 前置检查 + 审计 + 结构化结果
      │                   └ ai_api / ai_server    JSON-RPC over 命名管道
      └ Tab[] ── ShellView ── IExplorerBrowser ── Shell 视图
             └ nav::History   每标签一份历史栈事务
```

七条不变量（改代码前先读；完整 14 条见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §12）：

1. **状态只有一份**：标签状态（历史、当前索引、显示名、项目数）只在 `App::Tab` 里；chrome 控件是纯投影。
2. **chrome 不调 shell**：`TabStrip` / `StatusBar` 只发 `WM_APP_TAB_*`；`NavBar` 只回调 `std::function`。
3. **`ShellView` 是唯一接触 `IExplorerBrowser` 的地方**；它对外只暴露 `Navigate/Refresh/Resize/SetVisible`。
4. **`Theme` 是唯一视觉来源**：任何颜色、字号、间距都必须走 `theme_`，不许写死。
5. **所有动作走 `CommandBus`**：按钮、热键、序列、鼠标规则、命令面板、AI 是同一条入口 ——
   任何"绕过总线直调"的分支都会造成"按钮能点、双击没反应"这类行为分裂（`docs/ARCHITECTURE-RULES.md` R1/R16）。
6. **品牌串只出现在 `identity.h`**：其他文件写 `L"ExBar…"` 即失败（`docs/NAMING.md` B1）。
7. **绘制原语只有一个入口**：量字 `ui::MeasureText`、主题 `win::ThemeControl`、双缓冲 `ui::PaintBuffered`、
   填充/描边/分隔线 `ui::Fill*` —— 自写一份即失败（`tools/check_components.py` 的 `UNIQUE` 段）。

模块职责、消息流时序、COM 所有权与 ABI 补丁细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)；
写新代码前必读的硬规则见 [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md)（R1–R21）。

> **R21 输入纪律**：**禁止模拟鼠标/键盘、禁止抢焦点、禁止循环点击**。
> `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos` / `BlockInput` /
> `AttachThreadInput` 全仓零容忍；**`PostMessage` / `SendMessage` 投 `WM_LBUTTON*` / `WM_KEY*` /
> `WM_CHAR` 同样禁止** —— 合成点击在本程序里必然走到 `ShowPopupList` → `SetForegroundWindow`，
> 与 `SendInput` 结果相同；自动化脚本也不得改用户的前台窗口。
> 所有操作走**结构化命令**：进程内 `CommandBus::Invoke`，进程外 AI 命名管道（`docs/AI-INTERFACE.md`）；
> 验证逻辑用单测、看界面用 `scripts\shot.py`、验证点击流程用「**人工点、脚本只观察**」。
> 由 `tools/check_components.py` 强制（4 条规则）。详见 [docs/ARCHITECTURE-RULES.md §3.1](docs/ARCHITECTURE-RULES.md)。

---

## 6. 目录

```
ExBarBrowser/                       独立 git 仓库（main）
├─ src/                             按职责命名（平铺，181 .cpp / 125 .h，约 6.1 万行）
│  ├─ aichat_panel / aichat_service / aichat_webview   AI 面板（WebView2 渲染 + Node 服务）
│  ├─ main.cpp                      进程入口：DPI / COM+OLE / 转发或新开 / 命令行
│  ├─ app.{h,cpp}                   装配：单例 / 窗口创建 / 生命周期 / 鼠标入口
│  ├─ app_wnd / app_tabs / app_nav  主窗口过程、标签生命周期、导航与历史栈
│  ├─ app_layout / app_stats        布局与主题、状态栏取数
│  ├─ commands.{h,cpp} / cmd_naming 命令目录 + CommandBus（actor/审计/前置/结果）+ 点分名
│  ├─ ai_api / ai_server            AI 工具投影（参数校验/派发）与管道服务（令牌/泵）
│  ├─ chord / keymap(+.gen.h) / sequence / sequence_hint   键位解析、键位表、序列、前缀提示
│  ├─ input_router / mouse_rules / shell_input             输入端路由（键/序列/鼠标/视图命中）
│  ├─ hotkey_manager / prompt                              快捷键管理器、文本输入对话框
│  ├─ tab_strip + _input/_layout/_paint/_actions            标签栏（建模/命中/几何/绘制/菜单）
│  ├─ tab_set                                              标签集合（顺序/锁定/MRU/关闭历史）
│  ├─ app_commands + _tabs/_files/_misc                    命令注册四片（核心/标签/文件/杂项）
│  ├─ nav_path / nav_history                                路径语义与历史栈（导航唯一实现）
│  ├─ nav_bar / nav_bar_draw / nav_bar_actions / nav_model / nav_metrics   导航栏
│  ├─ bookmark_bar / bookmarks / bookmark_io / bookmark_drop               书签
│  ├─ status_bar / view_stats                               状态栏 9 段与取数
│  ├─ shell_view / shell_ops / disk_scan                    视图托管、文件操作、目录统计
│  ├─ popup_list / popup_toggle / folder_tree               弹层、「再点即收」状态、子目录树（唯一入口）
│  ├─ ui_draw / ui_palette / theme / icon_glyph / icon_resolve  绘制原语、色值令牌、主题、字形、图标识别
│  ├─ identity.h                                            品牌串 / 窗口类名 / 进程级资源名（唯一来源）
│  ├─ platform_win / str / log / clipboard / tooltip_host   收编后的基础设施
│  ├─ config / json / text_file                             配置、JSON、原子文本写
│  └─ common.h                                              共享类型与常量（并转发 identity 的类名）
├─ tests/                           71 个单测（每个 .cpp 自带 main + Check；`bash tests/run_all.sh` 一键跑全量，数量自动枚举）
├─ tools/                           check_commands / check_ai_meta / check_components / gen_keymap / gen_icon / objcache / reclaim / probe_* …
├─ scripts/                         shot.py / install.ps1 / uninstall.ps1
├─ docs/                            见下表（23 篇）
├─ aichat/                          AI 面板前端 + Node 服务（代码入库；data/aichat.json、models.json、mcp.json 与 logs 不入库）
├─ resources/                       app.ico / app.rc / icon_preview.png
├─ data/                            配置（**不入库**，首次启动自动生成：settings/bookmarks/keymap/mouse/aichat/models/mcp）
├─ state/                           会话/历史/标签/窗口状态（**不入库**，首次启动自动生成）
├─ logs/                            审计与事件日志（**不入库**，首次启动自动生成）
├─ cache/                           WebView2 用户数据（**不入库**，可整体删除）
├─ runtime/                         AI 通道发现文件（**不入库**，进程退出即删）
├─ profiles/                        MULTI/显式 profile 的完整隔离根（**不入库**）
├─ build.sh / build.bat             构建入口（薄包装，透传参数；`--release` 发布构建）
├─ build_fast.py                    增量构建实现 + 清单唯一真相源（含 RELEASE_COMPILE_FLAGS）
├─ ExBarBrowser.exe                 调试产物（带 -g，不入库）
├─ ExBarBrowser.release.exe         发布产物（strip，3.0 MB，不入库；`bash build.sh --release` 生成）
└─ README.md / CHANGELOG.md / CONTRIBUTING.md / LICENSE / SECURITY.md / AGENTS.md
```

## 7. 文档索引

| 文档 | 读者 | 内容 |
|---|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 要改代码的人 | **当前**分层、窗口树、模块职责、导航/标签时序、所有权、ABI 补丁、不变量 |
| [docs/ARCHITECTURE-TARGET.md](docs/ARCHITECTURE-TARGET.md) | 要理解目标形态的人 | **目标架构**：一切皆组件（tokens/assets/components）、载体（Bar/Panel/Dialog/Popup/Lister）、输入端（键位/序列/鼠标）、命令管线、人机同源 |
| [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) | 要写新代码的人 | **架构规则**：对象模型、目录与依赖、R1–R21 硬规则（组件/载体/输入端/管线/AI 安全/输入纪律）、机械校验清单 |
| [docs/ARCHITECTURE-PROPOSAL.md](docs/ARCHITECTURE-PROPOSAL.md) | 要参与定架构的人 | 候选方案 A/B/C 对比、推荐理由、迁移批次与风险（含 Opus 调研后的修订） |
| [docs/DOPUS-NOTES.md](docs/DOPUS-NOTES.md) | 想参考成熟产品的人 | Directory Opus 的公开契约（对象模型/命令/插件/外部调用）与"抄什么不抄什么" |
| [docs/DOPUS-PARITY.md](docs/DOPUS-PARITY.md) | 要知道跟 Opus 还差什么的人 | **对 Directory Opus 的逐项能力对标**：机器接口（我们更强）vs 领域广度（Opus 更强）、缺口清单、该抄/不该抄、优先级与两处待拍板分叉 |
| [docs/EXPLORER-GAP-AUDIT.md](docs/EXPLORER-GAP-AUDIT.md) | 要判断能不能替代 explorer 的人 | 与 Windows 11 资源管理器的逐项差距审计（错误/缺失/有意取舍三分法） |
| [docs/COMPETITIVE-GAP-2026-11-21.md](docs/COMPETITIVE-GAP-2026-11-21.md) | 要看同类产品对比的人 | 与 13 款同类资源管理器的对标、真错误清单、优先级建议 |
| [docs/REFERENCE-ASSETS-MAP.md](docs/REFERENCE-ASSETS-MAP.md) | 手里攒了一批参考素材的人 | 下载目录素材逐项映射到本仓命令位 / 模块 / 缺口，标出"可直接接 / 抄思路 / 看得见 / 无关" |
| [docs/PRIORITY-STATUS.md](docs/PRIORITY-STATUS.md) | 要排下一步改进的人 | E1–E8 / M1–M8 的实跑复核（哪些已修 / 真缺）、真缺项排序、当前并行会话阻断说明 |
| [docs/GAP-ANALYSIS-EXTERNAL.md](docs/GAP-ANALYSIS-EXTERNAL.md) | 要看外部产品差距的人 | 对外部成熟产品的能力差分析 |
| [docs/MIGRATION-PLAN.md](docs/MIGRATION-PLAN.md) | 要迁移 mod 功能的人 | **迁移规格**：标签栏/导航栏/书签栏/状态栏/键盘/序列/鼠标的功能清单、可移植性分级、数据资产、顺序与估时 |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | 要构建/调试的人 | 工具链、构建、日志、截图、注册脚本、常见故障、扩展步骤 |
| [docs/PARITY.md](docs/PARITY.md) | 要判断"能不能替代 explorer"的人 | 与 ExBar mod 127 条命令的逐组对照（已实现 / 未实现）、差异根因 |
| [docs/INPUT.md](docs/INPUT.md) | 要改键位/序列/鼠标规则的人 | 输入端主线、keymap/mouse 文件格式、时序语义、管理器用法、排查 |
| [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md) | 要接 AI / 写 MCP 封装的人 | **AI 操作接口**（已实现）：命令白名单规则、JSON-RPC 管道协议、stdio 入口（`--mcp-stdio`）、令牌与双门授权、错误码 |
| [docs/OPERATION-MODEL.md](docs/OPERATION-MODEL.md) | 要定操作契约的人 | **操作模型**（已实现 1–10 步）：MCP / VS Code / Blender / Opus 四家的做法、七条原则、与现状的差距、迁移的做法与取舍 |
| [docs/FULL-COVERAGE.md](docs/FULL-COVERAGE.md) | 要让「所有操作」都可程序化/可观测/可闭环的人 | **全覆盖模型**（设计）：四原语（Action/Query/Navigation/Event）、覆盖的三条可证伪判据、控制/观测/开发三平面、能力注册表、闭环模板、分层授权、差距清单、落地顺序 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 要排期的人 | P1 收尾 → P4 的里程碑、验收标准、风险与回退 |
| [docs/DECISIONS.md](docs/DECISIONS.md) | 想推翻现有设计的人 | ADR：独立进程、托管视图、GDI chrome、多实例、无配置 |
| [docs/NAMING.md](docs/NAMING.md) | 要起名/改名的人 | **命名与标识规范**：品牌串唯一来源、跨软件契约（不许改）、命名空间按领域、命令点分名与 legacy 规则、业界比对（Blender/VS Code/Qt/Opus/TC/Far/Chromium/Tauri） |
| [CHANGELOG.md](CHANGELOG.md) | 所有人 | 变更记录（Keep a Changelog 中文版） |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献者 / AI Agent | 代码规范、提交规范、验证清单、禁止事项 |

---

## 8. 已知坑（先看这里再动手）

| 现象 | 原因 | 处理 |
|---|---|---|
| 链接报 `failed to write output: Permission denied` | 目标 exe 正在运行 | 关掉 ExBarBrowser，或 `-o` 到临时路径 |
| 视图固定白底、工具栏却是深色 | uxtheme 序号 135/133 取不到（旧 Win10 / 被安全软件拦） | 功能不受影响，仅视觉降级；`ApplyProcessDarkMode` 静默失败 |
| 导航失败、日志 `parse hr=0x80070057` | 路径含正斜杠，`SHCreateItemFromParsingName` 返回 `E_INVALIDARG` | 已在 `ShellView::DoNavigate` 统一转反斜杠；新入口仍要遵守 |
| `Initialize` 崩溃或视图空白 | mingw 的 `shobjidl.h` 把第三参写成 `const FOLDERSETTINGS*`，真实 ABI 是 `FOLDERVIEWOPTIONS` 按值 | 已按 vtable 索引 3 直接调真实签名（`InitBrowser`），不要改回头文件声明 |
| 第二次启动没开新窗口 | 默认启动把路径转发给了已有窗口并激活 | 预期行为（360 形态）；要新开窗口用"在新窗口打开"命令 / `--new-window`，或设 `EXBARBROWSER_MULTI`（独立 profile） |
| 日志看起来是乱码/带空字符 | 日志是 UTF-16 LE 追加写、**无 BOM**，`cmd` 的 `type` 会按 ANSI 解释 | 用 `powershell -Command "Get-Content $env:TEMP\ExBarBrowser.log -Encoding Unicode -Tail 20"`，或 VS Code 手动选 UTF-16 LE |
| 找不到窗口 | 脚本按类名 `ExBarBrowser_Main` 找 | 类名常量在 `src/identity.h`（`id::kMainClass`），改名要同步 `scripts/shot.py` |
| 下拉菜单点一下就没 | 弹层在 `WM_ACTIVATE(WA_INACTIVE)` 时自动关闭；前台锁定/焦点被抢会立刻触发 | 确认点击前主窗口是前台；排查见 `docs/DEVELOPMENT.md` §3.4 |
| 弹层出现在屏幕角落 | 传了客户区坐标而不是屏幕坐标 | `ClientToScreen` 后再调 `ShowPopupList` |
| 每个 TU 都报 `malformed or corrupted AST file` | `obj/common.h.gch` 里记着生成时的绝对路径；仓库搬目录后即失效 | `bash build.sh --clean` 重建 PCH（约 10s） |
| 改了 `data/keymap.json` 但默认键没变 | 内置兜底表在 `src/keymap.gen.h`，需要重新生成 | `python tools/gen_keymap.py`；`check_commands.py` 会对账 |

---

## 9. 路线（摘要）

- **P1（已完成）**：窗口骨架、多标签、导航栏（5 按钮 + 分裂箭头 / 面包屑 / 地址编辑 / Tab 补全 /
  方向历史 / 分段子目录与折叠下拉 / 8 档视图模式）、自绘下拉弹层、状态栏、深色模式、激活优先启动（支持多实例）、
  注册表动词、原生导航窗格（`EBO_SHOWFRAMES`）。
- **P1.1（已完成）**：标签溢出滚动与 `»` 菜单、弹层、视图菜单、`nav_bar` 拆分。
- **P2 / M0–M7（已完成，2026-11-19）**：命令管线与命令面板、键盘/序列/鼠标输入端、
  标签栏（图标/溢出/拖拽/锁定/右键/关闭历史）、导航栏（自动补全/最近访问/排序分组）、
  状态栏 9 段、书签栏（`data\bookmarks.html`）、配置与会话、快捷键管理器。
  详见 [CHANGELOG](CHANGELOG.md) 0.2.0 与 [MIGRATION-PLAN](docs/MIGRATION-PLAN.md) §8。
- **P2.5（已完成，2026-11-20 / v0.2.1）**：操作契约结构化（10 步）—— 参数对象化、效果四维化、
  前置条件、交互与确定分离、结构化返回值、点分命名空间、MCP 兼容层、内容补全、能力缺口补齐；
  AI 操作接口（JSON-RPC over 管道，当时 86 工具，现 139）；导航收口为组件（`nav_path` / `nav_history`）；
  存储改便携模式（`<exe>\data`）；基础设施收编（窗口类/字符串/日志/剪贴板/tooltip/字形）。
- **P3（主体完成，2026-11-25）**：搜索（当前目录 + 历史，全盘 `evipc` 未接）、预览窗格、会话管理 UI、
  复选框模式、可选 Shell 扩展 DLL。已落地：目录统计（`OpTopN` / `OpDiskUsage`）、文件集工具
  （`tool.hash` / `tool.dupfinder` / `tool.compare`）、选择族五条、批量重命名（`Ctrl+Shift+R`）、
  撤销（`Ctrl+Z`）、搜索框（`Ctrl+F`）、书签导入导出与拖放。
- **P4（起步）**：UI 资产互通（`tokens.css`）；AI 面板已接（WebView2 + Node 服务，
  `src/aichat_*` + `aichat/`），下一步是把 chrome 的色值令牌与 Web 侧统一。

细节与验收标准见 [docs/ROADMAP.md](docs/ROADMAP.md)。

---

## 10. 诊断

导航链路日志：`%TEMP%\ExBarBrowser.log`（UTF-16，追加写），记录
`Initialize / parse / item / browse / complete` 五步 HRESULT 与路径。

```
[init] Initialize hr=0x00000000
[nav] request path=C:\Windows viewReady=1
[nav] parse 'C:\Windows' hr=0x00000000
[nav] item=C:\Windows
[nav] browse hr=0x00000000
[nav] complete pidl=C:\Windows
```

日志按 4 MB 轮转为 `ExBarBrowser.log.1`（只保留一份），实现集中在 `src/log.cpp`（`LogDbg` 唯一入口，
`LogLine` 仅为兼容名转发）。除导航五步外还有两类调试行：`[click]`（导航栏命中分解：
`btn/more/drop/chev/seg`）与 `[popup]`（弹层创建位置/尺寸、失焦关闭）。

另外两份运行期可观测文件（都在 `<exe>\data`，**不入库**）：

| 文件 | 内容 |
|---|---|
| `audit.log` | 每条命令的 `actor`（`user`/`ai`/`mouse`/`seq`/`key`）、参数、`ok=`、被拒原因（`blocked: …`） |
| `ai-token.txt` / `ai-pipe.txt` | AI 通道运行期发现文件：管道名与一次性令牌；`ai::StopServer()` 停服即删除 |

截图：`python scripts\shot.py out.png`（`PrintWindow` + `PW_RENDERFULLCONTENT`，窗口被遮挡也能抓；
但抓不到 WebView2 的 GPU 合成内容 —— **AI 面板展开时**其中的网页部分是 WebView2，截图会缺那块，
主界面其余 GDI chrome 正常可见）。

---

## 11. 静态检查现状（2026-11-25）

| 检查 | 结果 | 说明 |
|---|---|---|
| `python tools/check_commands.py` | 通过 | 命令表 219 / 已实现 211 / 处理器 211 / keymap 引用 237，三方对账一致 |
| `python tools/check_ai_meta.py` | 通过 | 28 组断言全过；AI 暴露 174 条（105 带自断言、69 显式豁免） |
| `python tools/check_components.py` | 1 类待办 | 单文件 ≤500 行：并行开发累计 11 个超限文件（含 `aichat_panel.cpp` / `aichat_webview.cpp` / `nav_pane.cpp`，均按记账行数棘轮看住，未新增）。评估面基线已随本轮发布改动重签并通过 `--check`；超限文件按职责拆分属后续欠账 |

> 数字均为快照，最终以命令输出为准。仓库处于多会话并行开发，检查状态会随提交波动。
