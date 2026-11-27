# CHANGELOG 0.3.2 详细记录

> 从主 CHANGELOG.md 迁出，保留该版本完整开发过程（验证/踩坑/逐条 why）。

## [0.3.2] - 2026-11-25 · 配置分层收尾 + 文件操作能力补齐 + 发布治理

### 文件操作能力补齐：撤销重做、文件属性、复制为快捷方式

补上 Explorer 日常高频但本仓一直缺的四件事，全部走命令总线（与拖拽/AI 同源，  
撤销栈与审计一致）：

- **撤销重做对称**：`file.redo`（Ctrl+Y）与 `file.undo`（Ctrl+Z）对称。redo 栈与  
  undo 栈**分离**：撤销成功时把撤销前的完整记录压入 redo 栈（撤销动作会清空原记录，  
  必须在 apply 之前留副本），新的 Push 会清空 redo 栈（标准语义：做了新操作就不能  
  再重做被撤掉的那一步）。重做失败**不弹栈**（重做机会不能被一次失败吃掉），成功后  
  该记录压回 undo 栈（于是还能再 Undo 回来）。人机边界同 undo：机器的重做不穿透  
  人工操作。配套 `file.undo.list` 可只读枚举两侧栈。
- **文件属性写入**：`file.attr.set`（选中项）与 `fs.attr.set`（点名路径）。只改真正  
  传了的位（set/clear 两个掩码表达"要置 1 / 要清 0"，避免用 bool 默认值把"没传"  
  误当成"要关掉"）。**整批语义**：先全部预检，任一条不可写就一条都不改（避免改了一半）；  
  写后逐条回读校验 —— 写隐藏/系统位时 shell 与杀软可能改回去，不校验就会出现  
  "命令说成功、属性没变"。落点唯一实现 `src/file_attr.{h,cpp}`。
- **复制为快捷方式**：`path.copy.link` 把路径做成 .lnk **虚拟文件**放进剪贴板  
  （CFSTR_FILEDESCRIPTOR + CFSTR_FILECONTENTS），粘到资源管理器得到的是真正的  
  快捷方式而不是一行文本路径。**不落盘**：`path.copy*` 全家族只动剪贴板，写 .lnk 到  
  磁盘是另一条命令（Opus 的 "send to" 语义），会覆盖同名文件，不该挂在 copy 名下。  
  落点唯一实现 `src/shortcut_clipboard.{h,cpp}`。
- **打开去向补齐**：`fs.open.tab`（新标签）、`fs.open.pane`（另一窗格，单栏时先自动  
  开双栏）、`fs.open.window`（新窗口），三个入口与书签菜单的六条「打开」去处一一对应。

### 界面：书签栏滚轮 + 状态栏图标大小滑块

- **书签栏横向滚轮**：把放不下的书签滚进视野。滚轮消息只发给**焦点窗口** ——  
  光标停在书签栏上、焦点却在文件列表时，书签栏收不到 `WM_MOUSEWHEEL`，用户滚的  
  却是背后的文件列表。故宿主（App 消息循环）在派发前按光标位置转交，与序列弹层  
  同一手法。链路分三段（消息入口 / 规则优先+默认滚动 / 宿主转交），单独成文件  
  避免把 `bookmark_bar.cpp` 顶过 500 行上限。
- **状态栏「图标大小」滑块**（自绘）：做成独立子窗口而非状态栏的一段 —— 段的模型是  
  "文字+图标+气泡"，点一下触发动作；滑块要的是"按住拖、值连续变"，塞进段模型得  
  给排版/命中/气泡三处都开特例。**取值是离散档位**而非连续像素：shell 只认若干  
  尺寸，落在档位之间的值会被它归到最近一档，于是滑块显示 37px 而实际 32px，  
  看起来就是"滑块不准"。与 Ctrl+滚轮**双向同步**，任一侧只有一个真源  
  （shell 里的真实像素），滑块自己那份只是显示。

### 书签栏：二级菜单与命令目录自描述

- **「移到分类…」二级菜单**：平铺列出所有分类 + 顶层，选中走 `bmk.move`（与拖拽/AI  
  同源）。被移动项若是分类，它自己的整棵子树**不列为目标**（移到自己子孙非法）——  
  菜单里就该先不列，别让用户点了才失败。`ui::Menu` 不支持真子菜单，二层弹层沿用  
  「打开分类」下钻的既有形态。
- **`cmd.help` 自省命令**：运行期列出全部命令的用法（schema / risk / 必填参数 /  
  断言 / 豁免理由），机器侧可自描述地查命令契约，不必猜参数。

### 对标 Opus 的缺口闭合与文档同步

- **ContextMenu 缺口不存在**（`docs/DOPUS-PARITY.md`）：Opus 的"右键菜单"能力本仓  
  其实已具备等价路径 —— 菜单行普遍以 `.Cmd(CmdId)` 逐行绑定命令总线，AI 直接调对应  
  命令即等价（`check_ai_meta.py` 第 15 项的 `MENU_ACTIONS` 登记 73 项中 72 项有命令  
  别名，唯一 None 是「搜索·（暂无搜索历史）」占位文案），逐项反查确认无遗漏。  
  `cmd.help` 再补运行期"按名查用法"，AI 不必猜命令参数。Opus 那种"在指定文件上弹菜单"  
  的形态**本仓刻意不做** —— 弹模态菜单会把 AI 管道卡到超时（同 `win.activate` 不暴露  
  的同一理由）。
- **文档布局同步**：`docs/INPUT.md` 的配置分层表（`data/` 偏好 vs `state/` 状态 vs  
  `logs/` 审计）、`docs/AI-INTERFACE.md` 的发现文件位置（`runtime\`）、`docs/PARITY.md`  
  与 `docs/FULL-COVERAGE.md` 的 profile 根路径（`profiles\multi`）全部按新布局订正。

### 配置分层收尾

单文件 `settings.json` → `data/` 偏好 + `state/` 四份状态 + `logs/` 日志的分层改造  
全部落地：审计与事件日志迁到 `logs/`，AI 发现文件（管道名/令牌）迁到 `runtime/`，  
配置分层迁移代码本身作为**一次性过渡手段**已彻底删除（见下节）。

**C++ 侧与 Node 侧同源**：AI 面板服务（`aichat/service/`）的配置也跟着分层 ——  
行为配置 `aichat.json`（端口/超时/安全边界/allowedRoots，低频、无密钥，可进版本库）  
与模型目录 `models.json`（含 apiKey，gitignore）都落在 `<profile 根>\data\`；  
会话日志落 `logs\`；发现文件落 `runtime\`。C++ 启动器经 `EXBAR_DATA_DIR` /  
`EXBAR_LOGS_DIR` / `EXBAR_RUNTIME_DIR` 把这三类目录传给 Node 服务，因此  
**profile 隔离在两侧是同一套根**（不会出现 C++ 读 A、Node 读 B）。  
模型配置优先级沿用业界通行做法：环境变量 > `models.json` > `aichat.json` > 内置默认值。

### 配置分层迁移代码清理：一次性过渡代码不再留在主流程

单文件 `settings.json` → `data/` 偏好 + `state/` 四份状态的分层改造，迁移逻辑本身是  
**一次性过渡手段**。核实旧配置的状态键已完整落进 `state\*.json`（差异项只是迁移后程序  
继续运行的正常演进）后，把迁移代码与旧配置备份彻底删除，主读写流程只留干净的新格式：

- 删除 `Config::Load()` 里的旧格式检测（`HasLegacyStateKeys`）、迁移前备份  
  （`BackupLegacySettings`）、旧状态键兼容读、一次性拆分迁移分支；删 `config.cpp` 的  
  `MigrateLegacyLayout()`、`config.h`/`config_state_io.cpp` 的 `StateFilesMissing()`，以及  
  `config_io_internal.h` 里仅服务旧格式的 4 个死 helper。
- 删除 `data/settings.json.legacy-*` 旧配置备份。
- 改动前文件备份在 `backup/20261125-2115/`（被 gitignore，不入库）。

**版本号管理**：仓库根 `VERSION` 是唯一真相源（`0.3.1` → `0.3.2`），exe 的 `VS_VERSION_INFO`、  
发布包目录名、`RELEASE-MANIFEST` 的 `version`/`tag`、`verify_release.py` 的对齐校验全部  
从它派生；README 不再硬编码具体版本号，只写"以 `VERSION` 为准"，消除文档漂移点。

**验证**：主构建 182 TU 零错误；改动的配置 TU 单独 `-fsyntax-only` 通过；迁移符号全仓  
零引用。本轮未重发 Release（0.3.1 资产仍是上一轮产物），0.3.2 版本段待下次发布打包。

### 发布治理：最小权限 + draft 发布 + 可复现验证门禁

按对标优秀开源项目的优化清单收口本轮（此前 GitHub Release 只有裸 exe、仓库 private、  
无社区入口、发布流程无人工确认点）：

- **社区与法律入口**：新增 MIT `LICENSE`、`SECURITY.md`、`CODE_OF_CONDUCT.md`、  
  `THIRD-PARTY-NOTICES.md`（WebView2Loader.dll）、issue/PR 模板；仓库转 public，  
  开 Discussions，设 topics，`main` 开分支保护（必需 `static-gates`、禁强推/删除），  
  启用私密漏洞报告。
- **文档口径对齐**：`CONTRIBUTING.md` / README / CI 注释里的测试数、Node/WebView2  
  依赖、配置分层（`data/` `state/` `logs/` `cache/` `runtime/`）、超限文件数全部  
  按实测收敛；打包器生成的 `START-HERE.txt` / `data/README.md` 同步新口径。
- **发布工作流加固**：release job 拆成 `build`（只读权限，构建+打包+验证+抽取 notes）  
  与 `publish`（仅此 job 提 `contents: write`）；Release 先建 **draft** 供人工复核，  
  notes 从 CHANGELOG 对应版本段抽取而非 GitHub 自动生成；顶层 `permissions: read-all`。
- **可复现发布**：manifest schema 升 2，`git_commit` 改完整 40 位 SHA，新增 `tag` /  
  `source_date_epoch` / `toolchain` 指纹；`created_utc` 取 commit time 而非墙钟；  
  打包时把 exe 的 PE COFF `TimeDateStamp` 归一化为 commit time。实测同一 commit  
  连续两次打包 zip SHA256 完全一致。
- **`scripts/verify_release.py`**：把发布核验固化成门禁（zip 完整、必需文件、  
  逐文件哈希、版本/tag/commit 对齐、确定性时间、密钥扫描、Node 依赖闭合、  
  VERSIONINFO）；本地实测 15/15 PASS。

**验证**：两个脚本 `py_compile` 通过；重新打包后 verify 15/15 PASS；两次打包 hash  
一致；release.yml 关键字段（draft / read-all / 双 job）核对通过。
