## [Unreleased]

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

## [0.3.1] - 2026-11-25 · 发布流程完善

### 发布流程：完整便携包 + exe 内嵌版本号

原先 GitHub Release 只挂一个 `ExBarBrowser.release.exe`。它能跑主浏览器，但 AI 面板
还缺 `WebView2Loader.dll` 与 `aichat/`；而 `data/` 里有开发机的真实设置、书签与 API key，
又绝不能整目录发出去。这一轮把"怎么发"变成可重复的脚本：

- 新增 `VERSION`（当前 `0.3.1`）作为**唯一版本号真相源**。构建器生成 `obj/version.h`，
  经 `windres` 注入 `resources/app.rc` 的 `VS_VERSION_INFO`；资源管理器 / PowerShell 里
  能看到 FileVersion、ProductVersion、ProductName。zip 名、manifest 与 Release tag 也读同一处。
  踩过的坑：这套 `windres` 会把 `VS_VERSION_INFO VERSIONINFO` 编成 RT_VERSION 下的
  **命名资源**而非数字 ID 1，Windows 版本 API 读不到；资源脚本已显式写成 `1 VERSIONINFO`。
- 新增 `scripts/package_release.py`：产出 `dist/ExBarBrowser-v<ver>-win-x64.zip`、
  `.sha256` 与 `RELEASE-MANIFEST-v<ver>.json`。包内是 `ExBarBrowser.exe` +
  `WebView2Loader.dll` + `aichat/`（服务代码与 Web UI，排除测试/日志/备份）+
  脱敏配置模板 + `data/README.md` + `START-HERE.txt` + manifest。
- `aichat/service/model.example.json` 用 `YOUR_API_KEY_HERE` 占位；打包器会扫描所有
  JSON 的 `apiKey`，出现非占位符直接失败，避免把真实密钥带进包里。
- `.gitignore` 只放行仓库根的官方 `WebView2Loader.dll`，`dist/` 整体忽略；
  新增 `.github/workflows/release.yml`：tag `vX.Y.Z` 与 `VERSION` 不一致就拒绝发布，
  一致则复用发布构建 + 打包脚本并上传三个资产。
- README 的构建/发布段落同步：明确 WebView2 Runtime 与 Node.js >= 18 只在 AI 面板时需要。

**验证**：`python -m py_compile build_fast.py scripts/package_release.py` 通过；
`python scripts/package_release.py --help` 正常；发布 exe 的 `VersionInfo` 与 `VERSION` 一致；
发布包 zip 可从干净暂存目录解压启动。评估面清单因 `build_fast.py` 变更重新签发。

### 设置对话框：每个栏一个导航项，分组重梳

设置面板的左栏是"分类"、中栏是"这一类的设置项"、右栏是"这一项的说明"。
原来的分类是**按设置项的类型堆出来的**（栏显隐 / 视图 / 历史上限 / 搜索 / 弹层 /
序列键 / 标签 / 窗口几何 / 会话与历史 / 状态栏段 / 键位 / AI 通道 / 审计与作业 /
设置窗口），于是改一条栏的设置得先在四栏之间猜它在哪儿：

- 四条自绘栏（标签栏 / 导航栏 / 书签栏 / 状态栏）的显隐、高度、宽度、提示宽度
  全塞在同一个「栏显隐」组里（12 项）—— **没有一个导航项对应一条栏**。
- 「状态栏段」另立一组，离另外三个状态栏设置项很远；「历史上限」把导航最近
  访问、搜索历史、已关标签三个不同功能的上限混成一组。
- 一组只放一项（`themeMode` / `hotkeys`）、一组只放两项（弹层）也各有各的怪。

**改法：16 组，每条栏一个导航项，顺序按它们在窗口里自上而下。**

| 导航项 | 项数 | 收进去的东西 |
|---|---|---|
| 外观 | 1 | 深浅色 |
| 标签栏 | 8 | 显示标签栏 / 标签栏高度 / 标签最小宽 / 标签最大宽 / 新建标签位置 / 关闭前确认 / 标签拖拽滚动周期 / 关闭标签保留数 |
| 导航栏 | 6 | 显示导航栏 / 导航栏高度 / 搜索框宽 / 地址框最小宽 / 最近访问上限 / 导航历史深度 |
| 书签栏 | 3 | 显示书签栏 / 书签栏高度 / 书签提示窗最大宽 |
| 状态栏 | 4 | 显示状态栏 / 状态栏高度 / 状态栏提示窗最大宽 / 状态栏显示哪些段 |
| 视图 | 4 | 视图模式 / 排序方式 / 升序 / 分组方式 |
| 窗格与面板 | 10 | 导航窗格 / 预览窗格 / 双栏 / 分栏比例 / 双栏方向 / 导航树宽行高缩进 / 导航树根 / AI 面板宽 |
| 搜索 | 5 | 搜索范围 / 自定义路径 / 搜索历史上限 / 下拉行数 / 搜索历史（只读） |
| 按键与手势 | 8 | 序列键八项 + 长按阈值 |
| 弹层 | 2 | 键入定位间隔 / 弹层行数 |
| AI 通道 | 4 | 启用 / 允许危险命令 / 调用超时 / 泵间隔 |
| 审计与作业 | 4 | 审计轮转 / 审计明文上限 / 作业超时 / 会话落盘去抖 |
| 设置窗口 | 3 | 分类栏宽 / 说明栏宽 / 值控件宽 |
| 键位 | 1 | 快捷键覆盖（只读） |
| 窗口几何 | 5 | 窗口位置大小（只读） |
| 会话状态 | 12 | 会话与最近访问（只读） |

顺带修掉几处**语义错位**：视图模式不再混着窗格布局（拆成「视图」与「窗格与
面板」）；会话落盘去抖不再挂在「窗口几何」下（挪到「审计与作业」，它是落盘策略
不是窗口状态）；AI 面板宽度不再挂在「AI 通道」下（它是面板宽度不是通道参数）；
「序列键」改名「按键与手势」—— 长按阈值是鼠标手势，本来就不该和按键序列混在一
组里。只读三组挪到最下面，写得动的在前。

**分组键是外部契约**（`config.list` 的 `group` 字段，AI 与脚本都看得到），所以
组键随分组一起改：`bars` → `bar_tab` / `bar_nav` / `bar_bookmark` / `bar_status`，
`history` 拆散（进 bar_nav / bar_tab / search），`status` → `bar_status`，
`view` 拆成 `view` + `pane`，`tabs` → `bar_tab`，`sequence` 保留键名只改中文名。

**图标**：四条栏各有一个专属字形（导航栏=前进箭头、标签栏=标签牌、书签栏=图钉、
状态栏=分段），登记在 `src/icon_glyph.*`；随「栏显隐」组一起撤掉的
`IconId::Bars`（横线菜单）也不再占着枚举。

**改动落点**：`src/settings_reg_table.cpp`（表本体与分组表，条目顺序 = 左栏顺序 =
中栏顺序）、`src/icon_glyph.{h,cpp}`、`src/settings_dialog.h`（头注释里的分组
说明）。`src/settings_reg_table.cpp` 行数 785 → 786（多了一组），
`data/file-length-baseline.json` 同步记账。

**验证**：`settings_reg_test` 900 条断言全过（含"每项归在一个已登记的分组、
每个分组至少一项、分组中文名唯一"这三条分组不变量）、`config_apply_test` 全过、
`check_ai_meta` / `check_commands` 全过；新构建经 AI 通道 `config.list` 回读，
16 组 80 项与表一致；设置窗口按新分组渲染并截图核对（左栏 16 个导航项、每项带
自己的图标与项数）。

### 书签：分类可解散、分类内可拖动排序

分类原先**只能读不能组织** —— 顶层那条自绘横条早就能拖动重排，而分类里的条目
只能点，右键菜单也只有顶层那一套。这一轮把分类补成和顶层同一套规则。

**解散分类**（`bmk.dissolve`，新增命令，编号 219）

- 书签栏分类右键菜单新增「解散分类」：把该分类的**直接子项**提到它所在的上一层
  （顶层分类 → 书签栏），分类本身消失。嵌套分类解散只降一层，不一次拍平。
- 子项**带着自己的子树**一起走 —— 散掉的是"这一层壳"，不是把整棵树摊平。
- 空分类解散 = 分类自己消失；叶子不是分类，没有壳可散，拒绝。
- 走 `CommandBus`（同 `bmk.move` / `bmk.remove` 一条路），因此有审计、有撤销栈，
  也满足 R20 人机等价：菜单动作与 AI 调用是同一件事。
- 改树**之前**拍快照（`PushUndo`），一次能挪动很多条，点错一步退回来。

**分类内拖动排序**（`bmk.move` 复用）

- 分类内容弹层支持按住条目拖动改顺序，落点语义与顶层那条栏一致
  （"插到第几行之前"），命令也是同一条 `bmk.move`。
- 落点之后**就地刷新**弹层（宿主重读 store → 重建行），窗口不关，
  用户不会看到"拖了没动"。
- 行号 ↔ 子项下标必须一一对应，所以弹层改为列出分类的**全部**直接子项
  （早先的实现会跳过没有路径的叶子，那会让落点错一位，把条目放到隔壁）。

**分类内行内右键菜单**（新增）

- 打开 / 复制路径 / 编辑 / 复制书签 / 上移 / 下移 / 移到书签栏 / 删除；
  子分类另有「解散分类」。上移下移给的是**拖不动时**的键盘等价路径。

**排序本分类**（`bmk.sort` 带 `parentId`）

- 只排这个分类的直接子项，不动别的层；与「按名称排序（含分类内）」的
  递归排序是两条不同的路。参数缺失时行为不变（仍递归排整棵树）。

**实现落点**

- 新增 `src/bookmarks_ops.cpp`（65 行）：`Dissolve` / `SortChildren` —— 层级重组
  与日常增删改查的失败口径、撤销口径都不同，分文件才读得动。
- 新增 `src/bookmarks_internal.h`：`ListContaining`（找父列表）与 `BookmarkLess`
  （排序规则）各只有一份。整树排序与"只排某个分类"共用同一个比较器，
  不会在"大小写怎么处理"上分叉。
- 新增 `src/bookmark_category_popup.cpp`（263 行）：可拖动的分类内容弹层。
- 新增 `src/bookmark_bar_popup.cpp`（345 行）：从 `bookmark_bar.cpp` 拆出的四套旧弹层
  （项右键 / 空白区 / 溢出 / 图标子目录树）。
- 新增 `src/bookmark_bar_drag.cpp`：顶层拖动的落点计算与提交。
- `bookmark_bar.cpp` 864 → 441 行（行数棘轮销账）。
- 通用弹层新增一条**可选**拖放通道 `PopupReorder`（`canDrag` / `onReorder` /
  `rebuildRows` / `onRowRightClick`）。共用机制只有一份：模态循环、鼠标捕获、
  type-ahead、惰性加载仍然只有弹层这一处实现，业务侧不自己造第二套弹层。

**顺带修掉两处真问题**（都是这轮改动照出来的，不是新功能本身）

- `MoveTo` 的"同容器内移动"分支**没有下标上界检查** —— `insert()` 不会自己检查，
  传一个超出末位的 `toIndex` 就是往 vector 之外写（未定义行为）。
  顶层那条路走 `Move()`，它有检查；嵌套这条以前没有。
  现在在 store 里拒（`index > 容器大小` → 返回 false）。
  命令侧不夹取 —— 夹取会把"位置非法"与"移到末尾"混成同一个结果。
- `Menu::Show` 在弹层**就地重建行**之后，仍用**重排之前**那份行把选中的行号
  映射成动作号。行序变了、动作号也跟着变，于是用户拖完之后点的那一行会被
  映射成旧顺序的动作号：**点 A 打开 C**。现在改成"用当前显示的那份行做映射"
  （新增纯函数 `PopupActionFor`，可单测）。

**验证**

- `bash build.sh` 全量通过；`check_commands.py`（219 条命令 / 211 已实现 / 211 处理器）
  与 `check_ai_meta.py`（28 项全绿）通过。
- `tests/run_all.sh`：**70 通过 / 1 无法运行（`clipboard_test` 需交互桌面）/ 0 失败**。
- 新增单测（`tests/test_store.cpp`）：`Dissolve`（子项提层 / 子树跟着走 /
  嵌套只降一层 / 叶子与空 id 被拒 / 可撤销）、`SortChildren`（只排一层 /
  不动别的层 / 升降序 / 单子项不留撤销快照）、`MoveTo` 同容器越界下标被拒。
  新增 `tests/popup_layout_test.cpp`：`PopupDropIndex` 落点换算、
  `PopupActionFor` 重排后的动作号映射（钉住"点 A 打开 C"那个形状）。

### 重构（行数棘轮）：导航栏拆出布局、命中与输入状态机

`nav_bar.cpp` 565 → 172 行，主文件只留生命周期与对外状态入口：

- 新增 `src/nav_bar_layout.cpp`（125 行）：按钮 / 地址框 / 搜索框 / 面包屑段的全部矩形，
  以及宽度夹取与 `RebuildTooltip` 热区同步。绘制与命中读的是这一份矩形。
- 新增 `src/nav_bar_hit.cpp`（62 行）：`HitButton` / `HitArrow` / `HitMore` / `HitDrop` /
  `HitSeg` / `HitChev` / `NavTargetAt`。只读布局产物，不改状态。
- 新增 `src/nav_bar_input.cpp`（259 行）：窗口过程、地址框子类、鼠标捕获与键盘输入。
  **时序原样保留**：DOWN 只记 `downBtn_` 并 `SetCapture`；动作在 UP 执行且抬起点必须
  仍落在同一按钮；先清 `downBtn_` 再 `ReleaseCapture`，避免 `WM_CAPTURECHANGED`
  同步复位把下一次点击误触发成导航。
- `g_oldEditProc` 随之收进 `nav_bar_input.cpp` 单一 TU：新增私有 `InstallEditSubclass()`，
  由 `Create` 调用，生命周期文件不再持有子类回调的旧过程指针。
- 顺手修掉一处隐藏依赖：`BuildButtons()` 用的 `ico::Glyph` 原先靠 `ui_draw.h` 间接带进来；
  拆布局时改为直接包含 `icon_glyph.h`，不再依赖 transitive include。
- 头文件只加分工注释与 `InstallEditSubclass()` 声明；类的数据成员未改。
- 基线 `nav_bar.cpp` 记账 565 → 172（只收紧）。

**验证**：`bash build.sh --no-gc --objdir ... --out ...` 隔离构建 176 TU 零错误零警告；
`check_commands.py` / `check_ai_meta.py` 全绿。`check_components.py` 仍只被并行会话
改动的 `app.h` / `nav_pane.cpp` 与未重签的评估面卡住，本轮没有动
`data/eval-surface.json`。`run_all.sh` 70 通过 / 1 失败 / 0 编译失败，唯一失败
`config_roundtrip_test` 的键是 `tabNewPos`，属配置边界改动；上一轮全量同项是 71/71
通过，导航相关 `nav_input_test` / `nav_root_test` / `upnav_test` 均通过。
单独复跑 `config_roundtrip` 被 `obj_cache` 的 safe-delete fail-closed 挡在构建阶段，
不是测试结论。

补记：本轮收尾核对时，工作树又被并行改动推着走：`popup_list.h` 与
`popup_list_input.cpp` 的 `ShowPopupList` 签名短暂不一致，随后一次增量构建链接失败；
命令表同时涨到 219 条，`check_ai_meta.py` 因新增的 `bmk.dissolve` 缺撤销机制转红。
这些变动发生在导航拆分验证通过之后，本轮没有触碰，也没有代修。

### 重构（行数棘轮）：设置对话框拆出布局与命中，模型只留可见行与当前值

`settings_dialog_model.cpp` 632 → 213 行：

- 新增 `src/settings_dialog_layout.cpp`（435 行）：布局常量、三栏宽夹取、
  `MinClientSize` / `Layout` / `LayoutRows` / `LayoutRight` / `PlaceValueEdit`，
  以及 `HitGroup` / `HitItem` / `HitRightAction` / `HitHeadAction`。排布产出的
  `RowLayout` / `RightLayout` 与命中读的是同一批矩形，两者同文件，避免
  "绘制一份、命中另一份"再次分叉。
- `UnitOf` / `RowHeight` / `CtrlHeight` / `EditLineHeight` 也归布局文件；绘制侧
  （`PaintNumber` / `PaintStatus` / `PaintSectionHeader`）仍通过唯一实现读取，
  没有复制单位表或行高表。
- 模型文件只留 `DisplayOf` / `Matches` / `RebuildModel` / `RefreshValues` /
  `RightDescText` 与搜索状态。`RightDescText` 特意不跟布局走：`LayoutRight` 量块高、
  `PaintRight` 画文本，必须共用同一份说明文本。
- `settings_dialog.cpp` / `settings_dialog.h` / `settings_dialog_draw.cpp` 的分工注释
  同步到新文件；类声明零改动，成员函数跨 TU 的布局不变。
- 基线 `settings_dialog_model.cpp` 记账 632 → 213（只收紧）。

**验证**：`bash build.sh --no-gc --objdir ... --out ...` 隔离构建 173 TU 零错误零警告；
`check_commands.py` / `check_ai_meta.py` 全绿。`check_components.py` 仍只被并行会话
改动的 `app.h` / `nav_pane.cpp` 与未重签的评估面卡住；本轮没有动
`data/eval-surface.json`。`EXBAR_TEST_OUT=... bash tests/run_all.sh` 71 通过 /
0 失败 / 0 编译失败，含 `settings_reg_test`、`config_roundtrip_test` 与
`popup_layout_test` / `popup_toggle_test`。

### 重构（行数棘轮）：AI 通道拆出协议分派，令牌比较只留一份

`ai_server.cpp` 790 → 429 行：

- 新增 `src/ai_server_rpc.cpp`（377 行）：简格式与 JSON-RPC 2.0（MCP）的方法分派、
  `tools/call` 结果/自断言、事件方法与能力面。`ai_server.cpp` 只留管道、发现文件、
  监听线程、UI 队列与启停。
- `CheckToken` 与 `RpcHasToken` 原先各有一份定长比较；现在统一走 `TokenMatches()`，
  经 `ai::CurrentToken()` 读传输层唯一的 `g_token`。空令牌在两条协议上都判失败。
- `tools/check_ai_meta.py` 的通道判据原先只读 `ai_server.cpp`；拆片后按逻辑模块读
  `ai_server.cpp + ai_server_rpc.cpp`，判据本身没有放松。**这是 `tools/` 改动，
  评估面需要人确认后重签；本轮没有动 `data/eval-surface.json`。**
- 基线 `ai_server.cpp` 记账 790 → 429（只收紧）。

**验证**：`bash build.sh --no-gc --objdir ... --out ...` 隔离构建 172 TU 零错误零警告；
`check_commands.py` / `check_ai_meta.py` 全绿。`run_all.sh` 70 通过 / 1 失败 /
0 编译失败，唯一失败 `config_roundtrip_test` 的键是 `settingsGroupWidth`，
属并行会话的配置边界改动；`ai_server_test` / `rpc_test` / `pipe_e2e_test` 均通过。

### 重构（行数棘轮）：弹层拆出树几何与输入状态机，主文件只留窗口外壳

`popup_list.cpp` 849 → 343 行，三个作用域彻底分开：

- 新增 `src/popup_list_tree.cpp`（152 行）：可见节点收集/懒加载、行高与总高、
  hover 重算、自适应尺寸、树节点展开/收回。它不处理捕获，也不决定"选行后导航到哪"。
- 新增 `src/popup_list_input.cpp`（320 行）：type-ahead、鼠标捕获语义、键盘导航、
  选中提交。**捕获顺序原样保留**：外部按下只记状态，等 UP 再关窗，避免同一下
  点击落到锚点控件（这条时序错了就是"点一下收菜单顺带打开书签"）。
- 新增 `src/popup_list_internal.h`（70 行）：`PopupState` 唯一定义 + 跨 TU 辅助声明。
  状态机字段（捕获、type-ahead、懒加载、toggleKey）只能有一份；各文件抄字段迟早
  会在"新增一个字段却漏改另一处"时把弹层行为分叉。
- 基线 `popup_list.cpp` 记账 849 → 343（只收紧）。

**验证**：`bash build.sh --no-gc --objdir ... --out ...` 隔离构建 171 TU 零错误零警告；
`check_commands.py` / `check_ai_meta.py` 全绿，`check_components.py` 仍只被并行会话
改动的 `app.h` / `nav_pane.cpp` 与评估面重签卡住。`run_all.sh` 70 通过 / 1 失败 /
0 编译失败，唯一失败 `config_roundtrip_test` 的键是 `navBarAddressMinWidth`，
属并行会话的配置边界改动，与弹层拆分无关（弹层相关 `popup_layout_test` /
`popup_toggle_test` 均通过）。

### 重构（行数棘轮）：书签 store 拆出 Netscape IO，树模型只留内存操作

`bookmarks.cpp` 893 → 595 行，仍超 500，但"文件级语义"已经整体搬走：

- 新增 `src/bookmarks_io.cpp`（319 行）：内部 store 的 Netscape 解析/序列化
  （`ParseLevel` / `Serialize` / `SerializeDocument`）、磁盘读写（`Load` / `Save`）、
  外部变更检测（`LastWriteOf` / `SizeOf` / `FileChanged`）、路径校验与 URL 编解码
  （`IsValidShellNamespacePath` / `BookmarkPathToUrl` / `BookmarkUrlToPath`）。
  `Load` 先解析到临时树再整体换入、`Save` 的 `loadFailed` / 外部改动两道护栏原样保留，
  拆分没有改判据。
- 新增 `src/bookmarks_internal.h`：`AssignIds` 声明在此、实现唯一留在树模型文件。
  内部文件解析与树模型新增都要给空 id 发号；若各写一份，导入文件已带 id 时
  `Find()` 会命中错节点，删除/改名作用到别人身上。
- **没有合并两个 `ParseLevel`**：这里读 `EXBAR_ID` 是跨软件稳定 id，
  `bookmark_io.cpp::ParseLevel` 故意不读，避免外部导入撞 id；两套语义仍分居两文件。
- 基线 `bookmarks.cpp` 记账 893 → 595（只收紧）。

**验证**：`bash build.sh --no-gc --objdir ... --out ...` 隔离构建 169 TU 零错误零警告；
`python tools/check_commands.py` / `check_ai_meta.py` 全绿，`check_components.py`
仍只被并行会话改动的 `app.h` / `nav_pane.cpp` 与评估面重签卡住，与本次拆分无关。
`EXBAR_TEST_OUT=... bash tests/run_all.sh` 71 通过 / 0 失败 / 0 编译失败（含
`test_store` / `encoding_test` / `config_roundtrip_test`）。

### 重构（行数棘轮）：shell_ops 拆出查询作用域，QI/路径辅助收进唯一内部头

`shell_ops.cpp` 1365 → 1078 行，仍超 500，但"只读查询/视图设置"作用域已单列：

- 新增 `src/shell_ops_query.cpp`（292 行）：排序/分组（`SetSort*` / `GetSort*` /
  `SetGroup*` / `Get*Key`）、隐藏项开关与读面（`ToggleHidden` / `CycleHiddenState` /
  `GetHiddenFlags`）、只读取数（`FillItemInfo` / `ListView` / `StatItem`）。
- 新增 `src/shell_ops_internal.h`：`GetFv2` / `GetFolder` / `HiddenFlags` 是薄包装与
  掩码，inline 在此；`ItemFullPath` / `SamePath` 有实质逻辑，声明在此、唯一实现仍在
  `shell_ops.cpp`。此前匿名命名空间里的这些辅助只能被单 TU 用，拆片后若各抄一份就
  会在 QI 的 `ComPtr::Reset` 细节与路径 ordinal 比较上分叉 —— 收进头文件保证一份。
- 写面与读面的隐藏项掩码同源（`HiddenFlags()` 唯一），避免"读到的是另一个开关"。
- 基线 `shell_ops.cpp` 记账 1365 → 1078（只收紧）。

**验证**：`bash build.sh --objdir` 隔离构建零错误零警告（168 TU 缓存一致）。
全量单测见本文件上一节（书签栏拆分）说明，唯一失败 `config_roundtrip_test` 属并行
会话的配置边界改动，与本次拆分无关。

### 重构（行数棘轮）：书签栏按职责拆出绘制与命中两片

`bookmark_bar.cpp` 1077 → 864 行，仍超 500，但绘制/命中两个作用域已经带走：

- 新增 `src/bookmark_bar_draw.cpp`（183 行）：`Layout` / `RebuildTooltip` / `Paint` /
  `PaintFocusRing` —— 只读 `items_` 与主题，不处理输入。几何令牌（`iconW` / `padX`）
  与绘制共用同一处，注释同步到新文件。
- 新增 `src/bookmark_bar_hit.cpp`（43 行）：`HitItem` / `HitIcon` / `HitOverflow` /
  `FolderItemAt` / `FolderIdAt` —— 客户区/屏幕点映射，不写状态、不弹菜单。
- `bookmark_bar.cpp` 只留生命周期、消息分发、输入、拖放、菜单与导航。
- 头文件零改动（成员函数分散到多个 TU 是合法 C++，`BookmarkBar` 声明不变）。
- 基线 `bookmark_bar.cpp` 记账 1077 → 864（只收紧，不追认其它文件的并行涨债）。

**验证**：`bash build.sh --out` 零错误零警告；`check_commands.py` / `check_ai_meta.py`
全绿；71 个单测 70 通过，唯一失败 `config_roundtrip_test`（`bookmarkBarTooltipWidth`
边界值，属并行会话的配置改动，与本次拆分无关，未改）。`check_components.py` 当前硬红
（`app.h` / `nav_pane.cpp` 涨过记账 + 评估面未重签）均来自并行会话，未替人处理。

### 新增（M7）：文件视图自动刷新 —— shell 变更不再需要手动 F5

**症状**：外部程序在该目录增删改文件后，视图不更新，必须手动 F5（COMPETITIVE-GAP M7）。

**做法**：复用导航窗格已有的 `SHChangeNotifyRegister`（原来只订阅目录结构事件刷树），
把文件级事件也纳入订阅，回投主窗口刷文件视图。
- `nav_pane_shell.cpp`：新增 `kViewShellEvents`（CREATE/DELETE/UPDATEITEM/RENAMEITEM +
  目录结构事件）；收到后 `PostMessage(root, WM_APP_VIEW_AUTO_REFRESH)`。
  与刷树的 `kNavShellEvents` 分开处理，文件级频繁事件不去推后导航树的 300ms 定时器。
  依赖方向保持 app → nav_pane：下层只投消息，不反向调 App。
- `common.h`：新增 `WM_APP_VIEW_AUTO_REFRESH = WM_APP + 56`（跨进程消息号，只加在末尾）。
- `app.cpp` / `app_wnd.cpp` / `app_nav.cpp`：主窗口收到消息后用 `kViewRefreshTimer`
  300ms 合并去抖（批量操作连发几十条通知只刷一次），到点调 `RefreshActive()`；
  OnDestroy 停表。

**验证**：`bash build.sh --out` 零错误零警告（165 TU）。**未跑** `check_components` / 单测——
评估面 `data/eval-surface.json` 因并行会话未重签而 fail-closed，重签须人确认。

### 修复（M6）：`/select,<文件>` 现在真选中文件，不再只打开父目录

**症状**：`ExBarBrowser.exe /select,C:\a\b.txt` 只打开 `C:\a`，`b.txt` 不选中；
转发到已有窗口时同样只开目录（COMPETITIVE-GAP M6）。

**做法**：复用会话选中恢复已有的 `pendingSelPath_` / `pendingSelItems_` 机制
（`app_nav.cpp` 导航完成回调里按路径匹配调 `SelectPaths`），启动前挂起选中目标即可。
- `main.cpp`：`InitialPath()` 增输出参数 `selectTarget` 填完整路径；`/select` 与 `/select,` 两种写法都认。
- 新进程路：`App::Run(hInst, path, selectTarget)` 在 `NewTab` 之前写 pendingSel。
- 转发路：`CopyDataPayload` **零改动**（跨软件契约）；`select` 作为变长尾随数据跟在
  `path` 的 NUL 之后，发送端按实际长度打包、接收端按 `cbData` 剩余长度读。
  新→老（老实例忽略尾部）/ 老→新（cbData 只够 path）天然兼容。
- `common.h` 只补注释说明契约；`app_wnd.cpp` WM_COPYDATA 接收端读出 select 传 `OpenExternal`。

**验证**：`bash build.sh --out` 零错误零警告（165 TU）；打包/解包对称性 4 组边界
（普通 / 盘根 / UNC / 无 select）全过。**未跑** `check_components` 与单测——
评估面 `data/eval-surface.json` 因并行会话改了 9 个 tests/tools 文件未重签而 fail-closed，
重签须人确认；本次不宣称"测试通过"。

### 构建提速：并行度自适应 + 跨会话 obj 缓存

**症状**：16 核机器默认只用 8 路编译；高并发多会话各用独立 objdir，同一批 165 个 TU
被每个会话重复编译一遍（冷编 ~30s × 会话数）。链接与日常增量其实很快，不是瓶颈。

**做法**：

- `build_fast.py` 的 `-j` 默认改为自适应：单会话吃满 CPU（clamp 16），多会话按
  `.reclaim-lock` 活跃数平分，避免互抢核；`EXBAR_JOBS` 可钉住。实测 2 会话降到 5。
- 有 `ld.lld.exe` 时用 lld 链接（0.49s→0.31s）；链接器不进配置签名，不触发全量重编。
- 新增 `tools/objcache.py`：按 (源内容 + 配置签名 + `pch_deps_hash`) 内容寻址的跨会话
  obj 缓存，命中即硬链接复用、零额外磁盘；并发写用 `os.replace` 原子落键；缓存是加速
  非资产，`obj_cache/` 进 `reclaim.py` 的 KEEP_DIRS，陈旧条目由 `prune()` 自管。
  `--no-objcache` 可关闭；任何缓存异常降级为正常编译，不影响构建结论。
- 评估后决定**不拆 PCH**：真实依赖面只有 5 个头，且缓存已按语义键正确处理公共头变更；
  拆出反而因重复解析变慢并可能破坏头文件自足约定。详见 `docs/BUILD-PERFORMANCE.md`。

**验证**：冷编 -j16 29.5s（旧 -j8 37.2s）；全新会话命中 165/165、总耗时 4.6s（提速 ~6×）；
改 `str.h` 内容 → 0/165 命中全量重编、改回后重新命中（内容寻址安全）；两会话 exe 大小一致。
`check_commands` / `check_ai_meta` 全过；`check_components` 的超限与评估面重签项均为并发
会话的既有工作区状态，非本轮改动引入。

### 测试产物根治：从 %TEMP% 搬进工程内、exe 跑完即删

**症状**：高并发开发时 `tests/run_all.sh` 每跑一次全量，就在 `%TEMP%/ebb_tests` 留下
70+ 个独立静态链接的 ~55 MB 测试 exe（~4 GB）。`tools/reclaim.py` 的 ROOT 是工程根、
扫不到系统临时目录，于是不回收；多会话各用一份前缀叠到 9.6 GB，占满临时盘。

**做法**：

- `tests/run_all.sh` 的 `OUT` 从 `${TMPDIR:-/tmp}/ebb_tests` 改为工程根 `tmp_tests/`，
  高并发会话可用 `EXBAR_TEST_OUT` 指到工程内其它子目录，产物绝不外溢到 `%TEMP%`。
- 每个测试 exe 编完跑完当场删除（含编译失败/NORUN 出口），只留 KB 级 `.log` 供排查；
  `build.sh --out` 的 `ExBarBrowser.test.exe` 测试本身不用，构建后即删。峰值占用从
  ~4 GB 降到 1 个 exe（~55 MB）。
- `tmp_tests/` 名字符合 `reclaim.py` 的 `^tmp.*$` 会话目录规则，闲置 15 分钟后由
  `build_fast.py` 收尾回收整目录（含 obj 缓存），复用既有回收设施，不新增清理脚本。
- `.gitignore` 增 `tmp*/` 整目录忽略；`docs/DEVELOPMENT.md` 同步落点说明。

**验证**：`bash -n tests/run_all.sh` 通过；`bash tests/run_all.sh keymap` PASS，
`tmp_tests/` 下 exe 残留 0、日志保留、obj 缓存 105 MB 复用；`reclaim.py` dry-run 将
`tmp_tests` 识别为 SESSION_DIR 且因刚写入受保护。

### AI 面板：WebView2 就绪前状态栏显示原生启动态

**症状**：面板占位页会画"准备中 / 创建中 / 加载界面"，但状态栏 chip 在此之前
只有页面 WebMessage 一个来源；WebView2 还没创建、页面脚本还没跑、或运行时
缺失时，chip 一直显示"AI · 空闲"。用户收起面板时看不出 AI 通道正在启动，
也看不出它已经失败。

**做法**：

- `ai_status_peek.{h,cpp}` 新增 `starting → 准备中` 与
  `ResolveAiNativeActivity()`：按 AI 通道开关、WebView2 三态和错误原因解析出
  `idle / starting / error`，文案与 chip 共用同一套口径。
- `Panel` 在展开、服务未启动、创建中、失败、重试、端口变化时回调宿主；
  `App::RefreshAiNativeStatus()` 把原生态落到同一个 chip / tooltip。
- 页面第一条合法状态到达后原生启动态让位；`starting` 会让页面状态观察标志
  归零，WebView2 重建后由新页面重新接管。页面不能伪造 `starting`，白名单
  仍只接受原有五态。
- `starting` 不产生未读；原生失败按既有 `error` 口径处理。
- `ai::StateSnapshot` / `StateToJson()` 新增 `aiState`，与 chip 同源。
- 决策记录 D27。

**验证**：`ai_status_peek_test.cpp` 覆盖准备中文案、非终态判定与原生状态解析；
`webview_actions_test.cpp` 钉住页面不能上报 `starting`；`ai_api_test.cpp` 通过。
全量 C++ 与服务测试见本轮验证记录。

### AI 面板：首轮工具桥忙时做一次受控回退

**症状**：并行开发或程序刚重启时，旧命名管道刚退场、新管道还没建好。首轮
`tools/list` 失败或返回空表后，模型会被明确告知"本轮没有工具"，用户不得不重发
提示词；而稍等片刻重连本来就能拿到完整命令表。

**做法**：

- `tools-exbar.js` 新增 `EnsureReadyWithFallback()`：首轮失败或工具表为空时只重探
  一次连接，返回是否恢复、工具数、是否回退与原因；连接失败时清空旧工具清单，
  工具数为 0 时不再输出"已连接"提示。
- `agent-stream.js` 改为在第一次模型请求前调用回退预检。恢复时状态流提示
  "工具桥已恢复"；持续失败时提示"未连接 / 已回退重探一次"，system prompt 仍如实写
  `no tools this turn`。
- 回退只重连，不执行命令、不重放被拒操作；`denyCommands / dryRun / allowedRoots`
  与危险确认仍只在 `dispatch()` 唯一出口生效，未知工具继续 fail closed。
- 新增 `tools-fallback_test.js`，扩展 `agent-preflight_test.js`，并把两者纳入默认
  `npm test` 与 `test:fallback`。
- 决策记录 D26。

**验证**：`npm test` 全过；`tools-fallback_test.js` 18/18，覆盖持续失败、重探恢复、
deny 不连桥与未知工具审批；`agent-preflight_test.js` 14/14，覆盖
`ensure → retry → ensure → fetch` 的顺序、恢复/降级状态与工具清单如实上报。

### 性能审查：绘制原语的 GDI 句柄复用

**做法**：

- `ui::FillRectColor()` / `FrameRectColor()` / `FillRound()` / `DrawSeparator()`
  不再每次绘制都 `CreateSolidBrush` / `CreatePen` + `DeleteObject`；按 `COLORREF`
  与 `(COLORREF, 线宽)` 缓存到进程级表，命中后直接借用。
- 新增 `ui::ClearGdiCaches()`，在 `App::Run()` 返回后统一释放字体池、实色画刷
  与画笔缓存；这些句柄只在对应绘制调用期间被选中，退出时没有在绘制的窗口。
- 这组原语全项目只有一份实现，因此 92 处调用点无需各自改造。

**验证**：`ui_draw.cpp` / `main.cpp` 在完整警告集下单独编译通过；全量单测与
隔离构建在并发源码稳定后复跑。

### 性能审查：作业与导航历史的无界增长收口

**做法**：

- `job::Service` 新增 `kMaxRetainedRecords = 32`：只淘汰"已完成且已 join"的旧记录，
  从 `jobs_` 头部开始保留最近 32 条。`tool.job.get` 在紧随其后的 `CollectResult` 中
  仍能查到刚完成的作业；更早的扫描/哈希结果不再常驻到进程退出。
- `nav::History` 新增 `kMaxEntries = 512`：`Push()` 超限时从头部裁剪并同步左移 `idx_`；
  `Restore()` 也裁一次，避免旧 settings.json 里已攒出的上万条历史继续放大配置文件。
- `ui::PaintBuffered()` 修掉 GDI 失败路径的 bitmap 泄漏：`CreateCompatibleDC` /
  `CreateCompatibleBitmap` / `SelectObject` 任一失败都按已取得的资源逐个释放并 `EndPaint`。
- `BookmarkBar::Create()` 不再在 `BookmarkStore::Load()` 之前空跑一次 `Reload()`。
- `TooltipHost::SetTools()` 文本未变时改用 `MoveTools()` 只更新热区；状态栏 800ms
  定时刷新和标签栏滚动不再每次发 2N 条 TTM_DELTOOLW/ADDTOOLW。

**验证**：`job_test` 覆盖保留上限与最近结果可读性；`nav_history_test` 覆盖
`Push` / `Restore` 两条裁剪路径与游标同步。全量构建/单测在并发源码稳定后复跑。

### 性能审查：启动、导航与重绘的同步热点收口

**做法**：

- 启动：`main.cpp` 用 `Local\ExBarBrowser.StartupMarker` 事件对象判断"是否已有更早实例"。
  只有后进者为"首实例还没建出窗口"保留 400ms 等待；冷启动以 0 超时直接判定，
  不再为一个不存在的窗口固定空等。标记不是单实例锁，仍允许显式多开。
- 地址栏：`NavBar::UpdateLeadIcon()` 从 `ico::Resolve()` 改为 `ico::Cached()`；
  HICON 归进程级图标缓存持有，`NavBar` 不再自行 `DestroyIcon`，避免每次导航/切标签
  都重做 `SHParseDisplayName` + `SHGetFileInfoW`，也避免与书签栏共享图标时被误销毁。
- Shell 视图：普通导航/视图重建只做 `InvalidateRect()`，不再每次
  `RDW_ALLCHILDREN | RDW_ERASE` 全子树同步重绘；主题切换的全量重绘移到
  `ShellView::FinishThemeSwitch()`，只在真正重建 EBC 后做一次。
- 去掉两处热路径日志成本：每次导航仅为日志调用的
  `IShellItem::GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING)`，以及
  `IExplorerPaneVisibility::GetPaneState()` 的逐窗格 GUID 诊断输出。

**验证**：隔离 exe / profile / TEMP 构建通过；启动日志中 `[ai] 通道已启动` 为
578ms（上一轮基线约 859ms），`ShowWindow → 窗口已显示` 约 94ms（上一轮约 359ms）。
全量单测 70 通过、1 项因本机剪贴板权限无法运行、0 失败；
`check_commands.py` / `gen_keymap.py --check` / `check_ai_meta.py` 全部通过。

### AI 面板：收起期间的新结果有未读标记

**症状**：D24 的 peek 会在终态到达时自动弹出，但用户离开电脑一会儿回来时，
状态栏只显示最后一次状态，无法区分"这是新到的结果"还是"早就看过的旧结果"。

**做法**：

- `ai_status_peek.{h,cpp}` 收口状态显示名、终态判定、未读判定与 chip 文案：
  `approval / done / error` 在面板收起时点亮未读，`busy / idle` 不点亮。
- 状态栏 chip 在未读时追加 `●`，tooltip 补"有未读结果，点击 chip 查看"。
  自动弹 peek 不算已读；点 chip、关闭气泡、打开对话、撤销或展开面板才清。
- 修正 chip 点击口径：按 D24 切换 peek（而不是直接切面板），真正打开对话走
  peek 的"打开对话"按钮或 `Ctrl+J`。
- `ai::StateSnapshot` 与 `StateToJson()` 新增 `aiUnread`，与 chip 同源；
  这是观测字段，不提供 AI 清除命令。
- 决策记录 D25。

**验证**：`ai_status_peek_test.cpp` 覆盖终态判定、面板可见性门闩、状态文案与
未读圆点；`ToggleAiPanel()` / `ToggleAiPeek()` 覆盖消费路径。全量测试与构建
结果见本轮验证记录。

### AI 面板：收起后仍有状态（状态栏 chip + peek 气泡）

**症状**：面板默认收起，后台回合仍在跑，但界面上没有任何"运行中 / 待确认 /
已完成 / 失败"的落点；用户只能重新展开面板才知道结果，也无法在不展开面板
的情况下撤销一次可回退的写操作。

**做法**：

- `ui/app.js` 在关键 SSE 事件上向宿主发 `type=status` 的 WebMessage；状态、
  摘要、详情、撤销域都有上限。工具事件补 `undoGroup / undoDomain`，只有成功
  且 `undoGroup=join` 的工具才进入 `state.lastUndo`。
- `aichat_webview_msg.h` 把 WebMessage 白名单从单一 `window` 扩成
  `window + status`，沿用 origin 校验；状态字段与撤销域在宿主侧再校验一次。
- 状态栏新增常驻 `Seg_Ai` chip：`AI · 空闲 / 运行中 / 待确认 / 完成 / 失败`，
  异步事件直接写段文本，`RefreshStats()` 重建模型时带回现值；`statusSegs`
  不再能把它藏掉。
- 新增 `aiui::StatusPeek`：点击 chip 弹出气泡，显示完成摘要，并提供
  "打开对话 / 撤销 / 关闭"；面板展开、窗口布局变化、主窗口销毁时收起。
  撤销按域派发 `file.undo` 或 `bmk.undo`，仍用 `Actor::Ai()` 保持人工边界。
- 文档记录 D24。

**验证**：主构建通过；`npm test` 与 `test:ui` 全过（新增 verify_ai_status
13 项）；C++ 定向单测 `status_bar_test` / `webview_actions_test` /
`ai_status_peek_test` 通过。评估面重签仍需人工执行。

### AI 面板：重新审查后的生命周期与安全边界加固

**做法**：

- WebView2：`WebView2Loader.dll` 改为 exe 目录绝对路径优先；完成回调改为
  `weak_ptr + generation` 门闩，`Stop()` 后可真正 Retry；异步失败上报到面板
  并显示具体原因/重试按钮；主窗口移动与 DPI 变化会通知控件刷新位置；
  新增 `NavigationCompleted` 处理器，页面 reload 后重发主题 WebMessage。
- 服务：`node` 子进程增加 `Restart()` 与运行态检测；AI 通道热插拔时同步
  启动/停止 node；原生侧生成随机 `EXBAR_HTTP_TOKEN` 并注入页面 URL。
- 本地 HTTP：校验 Host / Origin；`/api/*`（health 除外）与 `/agent` 要求
  `X-ExBar-Token`；JSON body 限 256 KiB；SSE 按 `drain` 背压、写失败中止上游，
  每个事件带 `run`/`seq`；`agent` 回合开始时冻结配置快照。
- 流式工具调用：名字分片按"完整值 or 片段"合并，避免中转重发时名字被覆盖；
  审批提交失败不再先显示"已确认"，而是恢复按钮并允许重试。
- 测试：新增 `agent-stream_test.js`、`server_security_test.js`；
  三个 UI 离线验证脚本补 `URLSearchParams` / `Headers` 沙箱注入。

**验证**：隔离构建通过；`check_commands.py` / `check_ai_meta.py` /
`gen_keymap.py --check` 通过；`npm test` 52+25+8+19 条全过，`test:ui`
17+24+16 条全过。`check_components.py` 仍有并行改动带来的行数/评估面欠账，
本轮只把自己的两个 WebView/Panel 文件行数同步到棘轮基线，未替人重签评估面。

### AI 面板：回合身份与确认语义收口（P0）

**症状**：历史截断或「新对话」后活动流会把不同对话并回同一个「第 N 轮」；
同一会话两个回合重叠时会读写同一份历史快照；停止后旧 SSE 的 `done/error`
还能清掉新回合的 busy 与审批卡；`undoable` 让可撤销的破坏性命令免确认；
「自动执行」文案让人以为危险命令会被自动批准。

**做法**：

- 新增 `service/turn-state.js`：`.turn.json` 持久记录单调回合号，旧会话从历史
  最大 `turn` 续号；同会话用 promise 链串行整段 SSE 回合，异常也释放锁。
- `server.js` 在锁内分配 `turnId` → 读历史 → 写回；每个 SSE 事件都带
  `turn` / `turnId`。历史与每条工具记录都落盘权威回合号。
- `ui/app.js` 用 `generation + activeTurn` 双身份过滤事件：`stop()` 与每次
  `send()` 都夺权，迟到事件不能写回答、清审批、改 busy 或写 EOF 警告。
- `service/approval-policy.js` 统一三模式：confirm 问所有写；auto 只免普通写，
  **危险命令任何模式都问**；readonly 拦所有写；未知工具 fail closed。
  `ApprovalOf()` 补 `readOnly`，`agent-stream.js` 不再在 auto 模式跳过审批卡，
  也不会把会话级免确认伪造成 `approve=true`。
- `commands_confirm.cpp` 删除 `undoable → 免确认`；可撤销只影响事后补救，
  不免除事前知情权。宿主侧 `aiAllowDanger + approve` 双门保持不变。
- composer 模式文案改为「自动（危险确认）」，tooltip 明示普通写自动、
  危险仍需确认且受 `denyCommands` 与宿主开关约束。

**验证**：`turn-state` 14/14、`approval-policy` 11/11、`verify_turn_guard`
16/16 通过；活动流 17/17、内联台账 24/24、guard 52/52、intents 25/25、
知识 14/14、协议 12/12 全过。新增 `commands_confirm_test.cpp`；
完整 C++ 套件 67/67 通过（独立临时目录，未抢占其他会话的测试锁）。

### AI 面板：窗口动作不再空点击（P1）

**症状**：页面自定义标题栏的 `postWindow(action)` 没有宿主消费者；最小化、关闭、
置顶点了没反应。`drag` 即使接上也无法安全拖动作为子窗口的面板。

**做法**：

- 新增 `aichat_webview_msg.h`：补齐 `WebMessageReceived` 处理器所需的 GUID、
  args vtable 与窄动作白名单；来源必须是启动 URL 的 origin，消息必须是
  `type=window`，动作只能是 `minimize / close / maximize / pin / unpin`。
- `aichat_webview.cpp` 的 `ICore` vtable 延到 `remove_WebMessageReceived`；
  在首次导航前注册处理器，`Stop()`/替换 core 时注销；页面动作回调转发给宿主。
- `App::InitAiPanel()` 落地动作：`close` 收起面板并重排，`minimize/maximize`
  操作主窗口，`pin/unpin` 用 `HWND_TOPMOST/NOTOPMOST + SWP_NOACTIVATE`。
- `ui/app.js` 在 WebView2 模式不再发 `drag`，close 文案改为「收起 AI 面板」；
  浏览器直开时隐藏无宿主的窗口按钮。

**验证**：隔离构建通过；`webview_actions_test.cpp` 17 项、`verify_window_actions.js`
10 项通过；`test:ui` 合计 67 项通过。新增决策 D19。

### AI 工具：撤销组元数据与纯模型（P1）

**症状**：AI 一次「复制 → 重命名 → 移动」是多步写，现有 `fileundo::Stack`
一条记录一步撤销；直接接线会让用户按多次 Ctrl+Z 才撤完一次任务。命令名本身
也不能可靠判断能否合并：`tab.close` 虽可撤销，却没有同域撤销栈。

**做法**：

- `ai_api.h/.cpp/.json` 给 `ToolSpec` 增加 `UndoGroupPolicy` 与 `undoDomain`；
  只有 `effects.undoable` 且属于 `File` / `Bookmark` 的命令才声明
  `undoGroup=join` 及对应域，`tools/list` 同步暴露 annotations。
- 新增 `service/undo-groups.js`：组 ID / 域校验、元数据准入、最多 20 步、
  `undoRef` 唯一、逆序撤销计划、失败关闭不生成计划；机器代撤遇到人工步骤
  整组拒绝。模型不发命令、不写文件、不碰 UI。
- 新增 `undo-groups_test.js`（33 项）与 `ai_api_test.cpp` 断言；修正
  `UndoDomainOfGroup()` 对无域分组返回空串指针导致 `tab.close` 被误判 join 的问题。

**验证**：隔离构建通过；完整 C++ 套件 69/69 通过；Node 侧撤销组 33/33，
连同 guard / intents / agent-stream / server-security / turn-state / approval-policy /
preflight / pipe-cancel / knowledge / protocols 全过。新增决策 D20。
**尚未接线**：真实 `fileundo::Stack` 与书签栈仍是单步撤销，本次只交付契约与模型。

### AI 面板：活动流与回答台账共享权威回合号（P2）

**症状**：服务端已给 SSE 事件写 `turnId`，但活动流按 `turn` 分组，回答气泡
自己不带身份。活动流显示「第 N 轮」时无法可靠定位回对应回答；历史里同时
存在冲突的 `turn` / `turnId` 时，两个视图可能显示不同轮号。

**做法**：

- `ui/app.js` 统一用 `turnIdOf()` 读取身份：`turnId` 优先、旧 `turn` 回落。
  事件归属、活动流分组、历史重建都走同一口径。
- 回答气泡和内联 `.step-ledger` 同步落 `data-turn-id`；实时由 `start` 写入，
  历史由落盘条目写入，工具事件到达时补写。台账头显示「第 N 轮」。
- 活动流分组保存同一 `turnId`，轮次标题变成按钮；点击后切回聊天、按身份找到
  回答、展开台账并滚动过去。旧数据没有 `turnId` 时只保留展示，不生成反查按钮。
- 新增 `tools/verify_turn_identity.js`（17 项），并接入 `npm run test:ui`。

**验证**：`test:ui` 合计 84 项通过（活动流 17 / 内联台账 24 / 事件归属 16 /
回合身份 17 / 窗口动作 10）。原有事件归属测试仍全过，共享身份没有削弱旧事件隔离。
新增决策 D21。

### AI 面板：重试与编辑重试走分叉回合（P2）

**症状**：失败/停止的回合只能整段重发；改一句提示词再问会把旧回答一起塞进
上下文，或者直接截断历史，丢掉审计链。旧 `turn` 是从消息条数推算的，不能
当分叉坐标。

**做法**：

- 新增 `service/turn-versions.js`：同 root 只保留最大 attempt 的活动分支；
  `retryOf` 从旧用户条目之前分叉；生成 `rootTurnId / attempt / supersededBy`
  血缘，旧尝试保留在历史里。
- `server.js` 新增 `historyForChat()`：普通 send 只看活动分支，retry 取分叉点
  之前的历史；无效 `retryOf` 返回 400 且不消耗回合号。新回合的事件、历史、
  工具记录都带版本字段，旧记录加 `supersededBy`。
- `ui/app.js` 在失败/停止回答上显示“重试 / 编辑重试”；前者用原提示词发新回合，
  后者预填输入框并把下一次 send 标成 `retryOf`。活动流显示尝试号与“旧版本”。

**验证**：`npm test` 全过（guard 52 / intents 25 / agent-stream 8 /
server-security 24 / undo-groups 33 / turn-versions 19）；`test:ui` 合计 97 项
（活动流 17 / 内联台账 24 / 事件归属 16 / 回合身份 17 / 版本 UI 13 / 窗口动作 10）。
新增决策 D22。**尚未交付**：任意旧分支的切换与回看，目前只标记旧版本。

### AI 面板：旧版本可切换，父链收窄上下文（P2）

**症状**：分叉后默认总是最大 attempt 为活动分支，用户切回旧版本下一次发送又
跳回最新；即使只改选中回合，挂在其他分支后面的普通回合还会混进上下文，
形成“旧版本 + 另一分支后半截”的假对话。

**做法**：

- 新增 `service/branch-state.js`：按会话把 `rootTurnId -> selectedTurnId`
  持久化到 `<session>.branches.json`，`/api/clear` 同步清空。
- `turn-versions.js` 的 `activeHistory()` 接受 selection；新回合记录
  `parentTurnId`，retry 复用分叉目标的父节点，普通 send 挂当前最后一个回合。
  活动分支只启用选中回合及其已启用的后代，跨分支后继一并排除。
- `server.js` 新增 `GET /api/branches` / `POST /api/branch`，在会话锁内校验
  同血缘后写选择；新回合自动成为对应 root 的活动分支。
- `ui/app.js` 在版本化活动组提供“切换”按钮，成功后重载历史；活动流与回答
  台账继续共享 D21 的 turnId 锚点。

**验证**：`npm test` 全过（guard 52 / intents 25 / agent-stream 8 /
server-security 30 / undo-groups 33 / turn-versions 24 / branch-state 9）；
`test:turns` 14+24+9 全过；`test:ui` 合计 101 项（活动流 17 / 内联台账 24 /
事件归属 16 / 回合身份 17 / 版本 UI 17 / 窗口动作 10）。新增决策 D23。

### 导航树：撤销展开符像素搬移，消除悬浮闪烁/截断

**症状**：鼠标在导航树上移动时，折叠展开按钮反复闪现、像被重建销毁，部分行
出现边缘截断；此前启动的进程也出现无响应。

**根因**：`PaintExpandoOffset()` 在内部树的 `WM_PAINT` / `WM_PRINTCLIENT` 之后
再次取 DC，逐行发 `TVM_GETITEMW` / `TVM_GETITEMRECT` / `TVM_GETNEXTITEM`，再用
`GetPixel` 单点采样底色 + `BitBlt` 把原生三角形搬到右移位置。热态重绘与搬移后的
像素两套状态交替出现，单点采样又抹掉热态/选中态边缘。

**做法**：删除 `PaintExpandoOffset()`、`TreeDepth()`、`ui::ShiftPixels()` 及两处
绘制末尾调用；展开符恢复为 SysTreeView32 原生绘制，命中区、热态、选中态同一来源。
分隔线仍在原生树绘制后补画，不受影响。

**验证**：隔离构建通过；修复版独立实例 `Responding=True`，并按 PID 只读截图
（`PrintWindow`，不模拟输入、不抢焦点）。

### 文件行数治理：先销账 `commands.h` 与 `shell_view.cpp`

**做法**：

- `commands.h` 拆成 facade + `commands_args.h` / `commands_actor.h` / `commands_state.h`。
  `CommandBus::fromMachine()` 与 `IsHumanCall()` 继续留在 `commands.h`，因为它们是
  `check_components.py` 登记的人机判定唯一真源；调用点仍只 include `commands.h`。
- `shell_view.cpp` 按职责拆出 `shell_view_sink.cpp`（Sink + 导航回调/重放）、
  `shell_view_selection.cpp`（路径/PIDL 选中快照与恢复）、`shell_view_navpane.cpp`
  （原生导航树查找/展开/显隐）、`shell_view_helpers.cpp`（宿主链类名查找），
  并把主题相关代码收进 `shell_view_theme.cpp`。
- `check_components.py` 的“控件主题套用” allow 增加 `shell_view_theme.cpp`：
  它是 `shell_view.cpp` 的 DirectUI 例外延展，不是新增第二套主题入口。
- `data/file-length-baseline.json` 只做棘轮收紧：删掉已降到 500 行以下的
  `shell_view.cpp`，收紧 `app_commands.cpp` / `app_commands_files.cpp` / `nav_pane.cpp`；
  没有用 `--update-length-baseline` 抬高任何仍超限文件的基线。

**验证**：隔离构建通过；`check_commands.py` / `check_ai_meta.py` 全通过；
全量单测 66/66。`check_components.py` 不再报这两个文件；剩余行数失败来自并行改动的
`aichat_webview.cpp` / `nav_bar.cpp`，以及评估面尚未由人重签。

### AI 助手：项目知识前置，回答不再"现场翻说明书"

**症状**：问「R21 是什么」「AI 面板入口在哪」「P4-a 做到哪了」这类项目事实时，
模型要先 `tool.list` 或读仓库文档再答，多一轮工具往返、token 花在说明书上，
而且不同轮次拿到的片段可能不一致。

**做法**：

- 新增 `aichat/service/project-knowledge.js`：只读索引 `AGENTS.md`、
  `ARCHITECTURE-RULES.md`、`DECISIONS.md`、`ROADMAP.md`、`CHANGELOG.md`、
  `README.md`、`CONTRIBUTING.md`，按标题切成 268 个 section。
- 常驻卡把 R21 / 零第三方依赖 / 品牌串 / 命令命名 / D13-D15 / P4 阶段状态 /
  Unreleased 压到 7600 字以内；按 mtime + size 缓存，文档不改不重复读。
- 每轮按提问做本地召回（中文 bigram + ASCII 词），最多补 3 个相关 section、
  2400 字以内；`agent.js` 与 `agent-stream.js` 在第一次模型调用前统一注入
  system message。工具参数仍以 `TOOL_SCHEMAS` 为准，文档片段不覆盖工具契约。
- System message 用内部标记分成「稳定前缀 / 每轮易变段」：`protocols.js`
  在 Anthropic 线路上把前缀转成带 `cache_control: ephemeral` 的 text block，
  OpenAI 兼容线路则抹掉标记、两段无缝拼回；循环里的原消息不被改写。
- 新增 `npm run test:knowledge` 与 `npm run test:protocols --prefix aichat`，
  覆盖索引、常驻预算、缓存边界、R21/D13/D14/Unreleased 常驻、召回、
  Anthropic cache block 与 OpenAI 标记清理。

**验证**：`test:knowledge` 14/14；`test:protocols` 12/12；`test:ui` 17/17 + 24/24；
`guard_test` 52/52；五个改动 JS 的 `node --check` 与 `package.json` 解析全过。
决策依据记入 `docs/DECISIONS.md` D16。

### AI 助手：文案统一、常用指令扩充、高频意图直调

**症状**：面板仍写着「ExBar 助手」；`/` 菜单只有 8 条面板自身动作；「上一级」「按大小排序」这类
一句话请求也要等模型推理一轮，还会附上一长段说明。

**做法**：

- 用户可见文案统一为「AI 助手」：`index.html` 的标题与品牌、`app.js` 的发送状态、`app.css`
  的空态提示、`server.js` 的启动日志；`docs/DECISIONS.md` 的历史验证记录同步。
- 空态建议从 4 条扩到 11 条；`/` 菜单从 8 条扩到 31 条，其中 22 条是文件管理快捷话术，
  9 条保留面板自身动作（原 `/刷新` 改名为 `/刷新上下文`，避免与导航刷新混淆）。
- 新增 `aichat/service/intents.js`：对高频、无歧义、无破坏性的请求做确定性匹配。命中后
  `agent-stream.js` 在**调用模型之前**直接 `dispatch()` 结构化命令，回复一句话，不产生 token 用量。
  未命中（复杂、含糊、会改文件、需要分析）仍走模型 + 审批门。
- `prompt.js` 收紧：首轮就用准确命令，禁止无关 `tool.list`，参数按真实 schema（`preset` / `by`），
  最终回答最多 1–2 句，不暴露推理。
- `/api/chat` 首个状态改为「正在理解意图…」；直调命中时改为「正在执行 <命令>…」。

**验证**：`intents_test: 25 checks passed`；`node --check aichat/ui/app.js` 通过；
`check_ai_meta.py` / `check_commands.py` / `gen_keymap.py --check` 通过；隔离构建通过；
独立实例上 7 条真实 SSE 对话均为 `steps=1`、`usage=null`，命令名/参数与回复一一对应
（`nav.up` / `go.downloads` / `view.sort` / `view.mode` / `search.find` / `file.select.pattern` / `tab.new`）。
全量单测 66/66（两条因编译器 OOM 中断的用例单独补跑通过）。
`check_components.py` 仍有 7 条与本轮无关的既有欠账。

### AI 面板：跟随深浅主题，聊天输入框不再触发序列键弹窗

**症状**：主程序切到浅色后右侧 WebView2 面板仍保持深色；在聊天输入框里打字
（`g` / `x` / `y` 等）会弹出序列键候选层，后续字符被弹窗吃掉。

**做法**：

- `aichat/ui/index.html` 在 CSS 加载前从 `?exbarTheme=` 写入 `data-theme`，
  `app.css` 增加浅色语义令牌；`app.js` 监听宿主 WebMessage，切换主题不重载页面。
- `src/aichat_webview.cpp`：手写 WebView2 绑定补到 `ICoreWebView2` 槽位 32，
  用 `PostWebMessageAsJson` 下发主题；显式打开 `IsWebMessageEnabled`。
- `src/aichat_panel.cpp` / `src/app_layout.cpp`：首次导航带主题参数，
  `App::ApplyTheme()` 调 `Panel::SyncTheme()` 热切换。
- `src/kb_hook.cpp` / `src/input_router.{h,cpp}` / `src/app.cpp`：把 AI 面板宿主
  登记为文本输入区。钩子不再吞 WebView2 输入框字符，输入路由按 EDIT 同等对待，
  并在焦点进入面板时收掉残留序列弹窗。

### 主题切换：双栏文件视图即时换色（无需重启）

**症状**：切换深浅主题时，标签栏、导航栏、书签栏、状态栏、导航窗格都立即变化，
只有文件视图（尤其双栏）保持旧色，必须重启程序才更新。

**根因**：`SetPreferredAppMode` 是进程级开关，只对**新建**的
`IExplorerBrowser` / DirectUI 窗口生效；原实现先切进程模式，再让各 `ShellView`
重建。切回浅色时旧深色 DirectUI 仍活着，shell 用它缓存的颜色策略创建"新"视图，
于是文件区粘在深色。仅重建 EBC、重建容器、补发 `WM_THEMECHANGED` /
`WM_SETTINGCHANGE` 都不足以覆盖这一顺序问题。

**做法**：

- `src/app_layout.cpp`：`App::ApplyTheme()` 改成严格三段式 —— 先让所有
  `ShellView` 拆掉浏览器，再 `ApplyProcessDarkMode(theme_.dark)` 明确压到
  `ForceDark(2)` / `ForceLight(3)`，最后逐个重建。
- `src/theme.cpp`：恢复双向进程模式切换，刷新颜色策略并 `FlushMenuThemes`。
  运行时**不**广播 `WM_SETTINGCHANGE("ImmersiveColorSet")`（见下方"切换速度"）。
- `src/shell_view.{h,cpp}` / `shell_view_theme.cpp`：新增
  `SuspendForThemeSwitch()` / `FinishThemeSwitch()`；重建时连容器窗口一起换掉，
  保住样式、位置、Z 序、可见性与焦点。
- 视图模式与选中项改为**延迟回灌**：`OnViewCreated` 早于 `GetCurrentView` 就绪，
  短定时器重试直到视图可用；重建期间的选中项保存 Shell PIDL（而非仅路径），
  实测 145/145 条无损恢复，视图模式也保持。

**验证**：独立 obj 目录 clean build 通过；`bash tests/run_all.sh` 66/66；
`check_commands.py` / `gen_keymap.py --check` / `check_ai_meta.py` 全部通过。
`PrintWindow` 像素判据：单栏与双栏各做 7 轮 `light → dark → light`，左/右文件区
背景中位数始终同步（浅色 255 / 深色 25）；带全选状态切换后 `selectionCount`
仍为 145，`viewMode` 不变。

### 主题切换：把单次耗时从约 4 秒压到 0.4~0.8 秒

**症状**："拆-切-建"修好了颜色，但每次切换肉眼可见地卡 3~5 秒才完成。

**逐段计时定位**（`GetTickCount64` 打点）：

| 阶段 | 优化前 | 优化后 | 说明 |
| --- | --- | --- | --- |
| `HWND_BROADCAST(WM_SETTINGCHANGE)` | 2687~3500ms | 0（移除） | 逐个等所有顶层窗口；三段式已先拆 EBC，运行时不再需要广播 |
| 导航栏 `WM_SETFONT` | 110~170ms | 0 | 地址栏 EDIT 挂 `SHAutoComplete`，每次新 HFONT 都重建补全列表 |
| 其余 UI chrome（标签/书签/状态/导航窗格） | 60~230ms | 46~94ms | — |
| 两个 Shell 视图 EBC 重建 | 400~760ms | 375~640ms | 换色必需，保留 |
| **合计** | **3438~5250ms** | **407~766ms** | 约 5~13× |

**做法**：

- `src/theme.cpp`：`ApplyProcessDarkMode()` 删掉 `BroadcastImmersiveColorChange()`。
  三段式顺序保证广播时旧 DirectUI 已不存在，shell 无须这条通知；各控件也有
  `ReloadTheme()`。保留 `RefreshImmersiveColorPolicyState()` 与 `FlushMenuThemes()`。
- `src/theme.{h,cpp}`：`Theme::Reload()` 在 DPI 未变时保留字体句柄
  （`Shutdown(keepFonts)` / `Init(skipFonts)`）。字体只跟 DPI 与系统消息字体有关，
  与深/浅色无关；只有 DPI 真的变了才连字体一起重建，并给 `fontGeneration` 加一。
- `src/nav_bar.{h,cpp}`：`editFontGen_` 记住上次给地址栏 EDIT 设字体时的代次，
  `ReloadTheme()` 里代次没变就跳过 `WM_SETFONT`。这是单项收益最大的一处。
  比代次而非 HFONT 值：GDI 删除对象后句柄值会复用，"句柄相同"时旧句柄可能已失效。

**验证**：优化后双栏 7 轮 `PrintWindow` 像素判据仍全绿（浅 255 / 深 25）；
全选 142 项切换主题后 `restore selection pidl=142`（142/142 无损）；
`tests/run_all.sh` 66/66、三项元数据检查全部通过。诊断计时统一收口成
`ApplyTheme` 的一行总耗时日志，不再逐子步骤打点。

### 导航窗格：按 Explorer 配置源自动换根，右键交给 NSTC 契约

**症状**：在 Explorer 侧边栏取消固定/新增项后，本程序导航树仍保持旧结构；
选中文件夹右键、空白区右键不弹菜单甚至整窗卡死。

**根因**（上一版是补丁式做法，已整体移除）：

- 只在启动时发现一次根集，后续仅重建已展开节点。Explorer 侧边栏项增删是
  **导航配置变化**，不是普通文件系统子项变化；即便监听 `SHChangeNotify`，
  快速访问的 Pin/Unpin 也不发这类事件。
- 上一版在内部 `SysTreeView32` 子类里拦 `WM_CONTEXTMENU` 并同步
  `TrackPopupMenuEx`，重进 NSTC 的消息/菜单跟踪状态导致卡死；事件适配器里
  `OnBeforeContextMenu` / `OnAfterContextMenu` 返回 `S_OK` 却不写 `ppv`，
  系统拿空指针当 `IContextMenu` 用，是同一处卡死的第二个来源。

**做法**：

- `src/nav_pane_root.cpp`：运行期根集重发现 + 三路变更监听 ——
  `HKCU\...\Explorer\Desktop` 子树（命名空间项）、
  `HKCU\...\Explorer\Advanced`（Folder Options 开关）、
  `%APPDATA%\...\AutomaticDestinations`（主页/快速访问 Pin/Unpin）。
  合并 150ms 防抖后走 `RefreshDefaultsAsync` → `WM_APP_NAV_ROOTS_READY` →
  `PrefetchTake()` 换树；退出时 `PrefetchShutdown()` 先停监听再 join。
- `src/nav_pane_shell.cpp`：shell 结构事件合并 300ms；根集/结构刷新前快照
  已展开路径，刷新后按前序恢复，并保住当前高亮。只重建已物化的展开节点，
  折叠分支不枚举。子类过程只剩绘制/滚动钩子，不再碰右键。
- `src/nav_pane.cpp` / `nav_pane_events.cpp`：用 `INameSpaceTreeControl2`
  打开 `NSTCS2_SHOWNULLSPACEMENU`；`OnBeforeContextMenu` /
  `OnAfterContextMenu` 在不替换菜单时严格返回 `E_NOTIMPL`，选中项菜单完整
  交回 NSTC/shell。空白区再用内部树命中测试区分行与空白处，避免有些 Windows
  版本把点中项的 `psi` 也传成 null。
- 空白区菜单最终与书签栏右键菜单**走同一个弹层**：`OnBeforeContextMenu`
  交出的 `DeferredBlankMenu` 不往原生菜单插任何项，只投
  `WM_APP_NAV_BLANK_MENU`；`nav_pane_shell.cpp` 在正常消息循环里调用
  `ui::Menu::Show(host, theme, screen, opt)`。于是边框、阴影、行高、图标槽、
  图标到文字间距、快捷键列、悬停/选中底色全部逐像素复用 `ShowPopupList`，
  不再经过 NSTC 原生菜单窗口那套边框和留白。
- 显示时机必须延后：NSTC 的事件回调栈里直接跑弹层模态循环会重入 shell。
  `PostMessage` 让 NSTC 先结束自己的流程，再从宿主消息过程里显示；
  选中项仍走 shell，空白区才走我们自己的弹层，两条路径互不替换。
- `src/menu_context.{h,cpp}`：保留为“宿主必须自己跟踪原生菜单”时的
  `ui::Menu → IContextMenu3` 桥接；导航窗格不再使用它，因为原生菜单窗口本身
  无法与自绘弹层的边框/留白逐像素一致。

**验证**：`python build_fast.py -j 8 --no-gc` 通过；`bash tests/run_all.sh` 66/66；
`check_commands.py` / `gen_keymap.py --check` / `check_ai_meta.py` 通过。
`check_ai_meta` 的菜单区域登记随内容构建位置从 `nav_pane_events.cpp` 改到
`nav_pane_shell.cpp`，避免第 16 项把这两条菜单当成"源码里不存在"。
`menu_context_test` 在无合成输入的前提下仍验住桥接层的行数、图标行、动作号、
快捷键自动出现和 `Theme` 底色/高亮/行高；导航窗格现在走的是同一 `ui::Menu`
的 `Show()` 路径，视觉由 `popup_layout_test` 与共享渲染器保证。
`check_components` 的菜单实现规则无新增违规；剩余历史欠账与评估面重签
（`check_ai_meta` 改动、新增 `menu_context_test`）按纪律留给人确认。

### AI 回答：结构化活动内嵌为执行台账（聊天体验优化）

**动机**：独立的「活动流」页签适合事后审计，但正常对话里用户不想为了看
"助手刚刚做了什么"而切页。参考 IDE / agent 客户端的 tool-use transparency
做法：过程可折叠、结果先露出、异常不能被藏起来。

**做法**：

- `ui/app.js`：`appendAssistant()` 在回答气泡上方挂一个 `step-ledger`，
  同一批 SSE `tool` 事件由 `appendTool(ev, assistant)` 同时写进
  ①全局事件数据（活动流仍可用）②本条回答的执行台账。`mountTool()` 按 id
  就地更新，`running → done` 不新增行。
- 摘要行显示 `执行 N 步 · 完成 M` 与风险分布（只读/写/危险）+ 时间；
  每个步骤一行：状态图标、中文标题、点分命令名、风险徽章、状态徽章，
  点行展开参数 / 结果 / 自断言 / 失败原因。
- 折叠策略：成功步骤默认收起；`error / mismatch / cancelled / rejected`
  或有危险命令时整块台账默认展开，避免"效果未达成"被绿色成功淹没。
  用户手动折叠/展开后，后续流式事件不再覆盖他的选择。
- `loadHistory()` 用 `m.tools` 为旧回答重建台账；没有 id 的历史步骤补
  稳定 id，防止同轮多步塌到同一张卡片。独立活动流仍由同一份历史重建，
  两条路径不产生两种状态。
- `ui/app.css`：补 `.step-ledger / .ledger-head / .ledger-step / .step-row /
  .step-detail` 等样式，沿用浏览器暗色 chrome 的低对比色板与 20px 行高。

**验证**：`node --check aichat/ui/app.js` 通过；新增离线回归
`tools/verify_inline_ledger.js` **24/24**（生成、就地更新、摘要、异常展开、
手动折叠保持、自断言、历史恢复）；原 `tools/verify_activity_view.js` **17/17** 仍过。

### AI 面板：Web UI 补结构化活动流视图（P4-a Web 侧）

**症状**：浏览器里打开面板服务（`Ctrl+Alt+J`）只有聊天气泡 + 内联工具卡，
ROADMAP P4-a 约定的"结构化活动流"（按回合分组、风险徽章、筛选、详情展开）
从未落地 —— 用户原话"只是能聊天，结构化视图呢"。

**做法**（同一批 SSE `tool` 事件的第二种看法，不另建数据源）：

- 服务端 `service/agent-stream.js`：`toolRisk(name)` 从命令表 `annotations`
  推导风险档（destructiveHint→danger / readOnlyHint→read / 其余 write /
  未知 fail-closed 到 danger），与审批门同源，不另写会过期的 switch。
  四处工具卡构造补 `command/risk/step/ts`。
- `service/server.js`：`/api/chat` 的 emit 给 tool 事件注入 `turn`（按历史里
  用户消息数推算，刷新前后分组稳定）；历史落盘保留完整工具字段
  （含 `risk/args/result/assertions/errorCode`），刷新后活动流能重建。
- Web UI `ui/index.html` + `app.css` + `app.js`：新增视图切换栏
  （对话 / 活动流 + 全部/只读/写/危险/失败筛选）；`activity.turns` 为唯一
  数据真源，实时事件（`recordActivity`）与历史恢复（`rebuildActivityFromHistory`）
  都只改数据再调 `renderActivity()`。行展示状态点 + 中文标题 + 点分命令名 +
  风险徽章 + 时间，点行展开参数/结果/自断言/失败原因；running→done 就地更新
  不新增行。

**验证**：`node --check` 三个改动 JS 全过；`guard_test` 52/52；离线 vm 加载
`app.js` 喂模拟 history/SSE，**17/17**（分组、提示词、风险保留、五档筛选、
就地更新、旧数据风险回落）；启动独立服务（8799）curl 首页确认新元素送达、
静态资源 200、注入完整 history 经 `/api/history` 回读 `risk/command/step/turn`
全部保留。回归脚本 `tools/verify_activity_view.js`。

### 布局：三条边界都能拖（导航 | 文件区 | AI 面板 + 双栏）

参考 Explorer 的手感，把"两块内容之间的分界线"统一成一种真窗口拖拽条：
导航窗格与文件区之间、文件区与 AI 面板之间、双栏之间都可拖动改尺寸。
以前只有双栏那条能拖，另两条是画在父窗口上的死线（没有命中测试，
鼠标永远落不到线上）。

- 新增 `src/split_bar.{h,cpp}`：一份拖拽状态机服务三条边界，
  用 `Metric::Percent`（双栏，落盘 `paneSplit`）与 `Metric::Pixels`
  （侧栏，落盘 `navPaneWidth` / `aiPanelWidth`）区分落点口径。
  抓取偏移、松手才写盘、悬停光标、拖动去重都只有一处 ——
  三条边界的表现因此一致（不会出现"某条拖完粘手"）。
- 新增 `src/side_layout.{h,cpp}` + `src/layout_limits.{h}`：宽度上下限、
  默认值、文件区最小宽度与"窗口太窄谁先让位"收在一处，
  界面拖到的范围 = 命令可写的范围 = 读盘夹取的范围。
  侧栏边界压在交界线上、不额外占宽：`navPaneWidth=220` 仍然意味着
  "文件区从 x=220 开始"，旧配置无需迁移。
- 新增 `src/split_bar`/`side_layout` 通信所需的配置字段 `aiPanelWidth`
  （240..900，与 `navPaneWidth` 对称），经 `settings_reg` + `config_io` +
  `cfgapply::AffectsLayout` 全链路接上；`config.set aiPanelWidth` 立即重排。
  AI 面板宽度不再写死在 `aichat_panel.cpp` 的 DIP 常量里。
- 双栏夹取区间从 `ClampCenter` 里拆出 `panesplit::Bounds()`，
  拖动路径与布局路径共用同一区间（极窄窗口不再算出负边长）。

**验证**：`tests/side_layout_test.cpp` 新增纯几何单测（宽窗口原样落地、
窄窗口 AI 先收、极窄两块都收、超范围夹取、边界中心位置）；
`side_layout` / `pane_split` / `config_apply` / `settings_reg` 单测全绿，
`build.sh` 增量链接通过。按 pid 枚举子进程确认三条边界都是真实窗口：双栏沿用旧类名
`ExBarBrowser_PaneSplitter`（外部脚本契约不变），两条侧栏用新类名
`ExBarBrowser_SideSplitter`；导航条展开时可见、AI 面板展开后右侧那条也出现、
双栏存在时中间那条可见（单栏隐藏）。
未用合成输入拖鼠标（R21），拖动状态由纯函数单测覆盖。

### AI 面板：解除构建阻塞 + 可见性验收结论

**构建阻塞**：`src/app_layout.cpp` 两处 `std::max` 把 `LONG`（`rc.right`）与
`int` 混推，clang 报 `deduced conflicting types`，链接未进行。给 `rc.right`
加 `static_cast<int>` 后 154 个 TU 增量构建、链接通过。

**"始终看不到面板"的结论：面板默认收起，不是渲染坏了。** `Panel::visible_`
初值为 `false`，`Create()` 里 `ShowWindow(SW_HIDE)`，必须主动展开才能看到：

- `Ctrl+J`（`hotkeyAiPanel` → `ai.panel.toggle`）展开/收起右侧面板；
- `Ctrl+Alt+J`（`hotkeyAiPanelWeb` → `ai.panel.web`）用系统浏览器打开同一份 UI，
  是内嵌视图不可用时的退路。

日志与几何实测证明**展开后链路是通的**（不是白屏）：占位页自绘
`bg=0xECECEC fg=0x1A1A1A`（有对比度，非同色白底）、面板几何 `420x604` 可见、
`OnEnvironmentReady` / `OnControllerReady` 回调均到达、`WebView2 已就绪`、
导航 `http://127.0.0.1:<port>/` 成功、服务 GET `/` 返回完整 UI。
`exbar/state` 的四个排查字段（`aiPanelVisible/Port/WebView/Error`）由
`StateToJson` 输出、宿主 `RpcResult` 包在 `result` 下、`exbar-context` 走
`RawClient().Request()` 取 `msg.result` 读取，链路正确。

**验收**：`guard_test` 52/52、`check_commands.py`（218 条命令、237 条 keymap 引用）
通过。`_guard_e2e` / `_dryrun_e2e` 需连接活实例桥，本机旧实例（v1.exe）的管道
不响应 MCP 请求而挂起，属环境问题（此前已 14/5 全绿），非代码缺陷。

### 构建：隔离构建的残留改为自动回收

**症状**：仓库根目录按每次会话 150 MB+ 的速度膨胀。实测一个上午堆出
`obj_rev2/s3/s4/s5/s6`（5×101 MB）+ 3 个 54 MB 的 exe，把当轮手工清掉的 607 MB
全部抵掉**还倒涨 1 GB**（`df` 125G → 126G）。

**根因**：`build_fast.py` 的 `--objdir` / `--out` 让多会话可以并行构建，但没有任何
环节回收它们的产物。手工 `rm -rf` 又不敢跑 —— 会抽走别人正在构建的那份，
报出来的是链接期 `undefined symbol`，看着像代码坏了。

**做法**：新增 `tools/reclaim.py`（回收的唯一实现），`build_fast.py` 在每次构建
收尾自动调用，**成功与失败两条路径都调**（失败构建同样留下上一轮的残留）。

跳过判据四条，任一命中就保留：

- 同名 `ExBarBrowser*.exe` 进程在跑；
- 目录树最新 mtime 在 15 分钟内（`--min-age` 可调）—— **本轮自己的产物天然落在窗口内**；
- 目录持有 `.reclaim-lock` 且持有者进程存活；
- 目录下有任一项被 git 跟踪。

**为什么还要显式锁**：mtime 只证明"最近写过"，不证明"仍然持有"。卡住的构建
（`windres` 遇损坏 `.ico` 会静默挂 20 分钟）期间不写任何文件，窗口一过就被判成
陈旧，中间目录被抽走 → 链接期 `undefined symbol`。所以构建开始时在 `OBJ` 写
`.reclaim-lock{pid=…}`，`atexit` 释放。

**两处实测踩到的坑**（都已修正）：

- 脚本最初叫 `tools/gc.py`，`import gc` 只会拿到 Python 内建模块（`hasattr(m,'sweep')` 为假），
  回收静默失效 —— 改名 `reclaim.py`。
- 判进程存活最初用 `os.kill(pid, 0)`。实测 Windows 上对**已退出**进程直接返回成功
  （`subprocess` 起一个 `pass` 进程、等它退出再 `kill(pid,0)`，不抛异常）。
  于是崩溃留下的陈旧锁被当成活锁、目录永久不回收。改用 `OpenProcess` +
  `GetExitCodeProcess`，且判不清时按**活着**处理 —— 宁可多留 100 MB，
  也不要抽走别人正在构建的目录。

**验证**（都是实跑，不是推演）：

- 造 `obj_s9`(2M) + `t90/` + `t90.py` + `ExBarBrowser.old9.exe`，跑一次构建 →
  四类全部自动回收，打印 `[回收] 清掉 4 项历史残留`；
- 把 `obj_selftest` 改老到 3 小时前再 `--objdir obj_selftest` 构建 →
  目录仍在（锁保护生效），锁文件已释放；
- 活锁（`sleep` 进程持锁）→ 保留并打印 `构建正在用（pid N）`；
  陈旧锁（持有者已 kill）→ 正常回收；
- 判活单元：活进程 `True` / 已退出 `False` / 不存在 pid `False` / 自身 `True`。

**顺带**：`--no-gc` 关掉本次回收，`--gc-age N` 调保护窗口。

### 修复：双栏只有一条分界线 / 拖动不残留 / 单栏右侧留白 / 双栏开关改为下拉

**① 双栏之间的分界线**（这一处返工三次，前两版都是错的，结论记在这里免得再走）

前两版都试图"在两栏之间留一条缝、再往缝里填色"：
- 第一版留 2px 缝露 `viewArea` 底色 + 分割条在缝中线画 `separator` 线
- 第二版留 3px 缝、整条填成 `paneSep`（与视图边框同色）

两次都没解决，因为**两栏的 Shell 视图各自带一条 1px 边框**（`IExplorerBrowser`
自己画的，我们关不掉）。那条边框始终在缝的外侧可见，于是无论我们往缝里填什么，
交界的像素永远是"视图边框 + 我们的填充"两段 —— 同色也只是变成一根粗线。

**第三版按业界做法（VS Code 的 sash、DOpus、JetBrains 的 divider）改成实体条**：

- 两栏之间留 6px 缝（`Theme::splitter96`），分割条窗口比缝**两侧各宽 1px**
  （`Theme::splitterHit()` = 缝 + 2）—— 把那两条视图边框**压在自己底下**，
  物理上不显示，也就不可能数出第二条。
- 窗口内：`theme.bg` 打底（与两栏内容同色，看不出边界）+ **中线 1px** 画 `line`
  （悬停/拖动换 `accent`）。
- 实测像素（浅色，窗口 x=545..554）：`545..549` 白底、`550` 一条 `#D0D0D0`、
  `551..554` 白底；两侧视图边框（原本 `#646464`）已完全被盖住。

**② 拖动时线条残留 / 悬浮后消失** —— 三个独立成因，缺一个就会残留：

- **`SWP_NOCOPYBITS`**：缺了它 `SetWindowPos` 会把窗口原有像素**拷贝**到新位置
  （系统默认优化）。拖动分割条时每移动一步，旧线被贴到新位置一份；
  视图容器 `ShellView::Resize` 也有同样问题（缩小时缩掉的一条留在屏幕上）。
  两处都补上。
- **父窗口腾出的那块必须 `RedrawWindow` 而不是 `InvalidateRect`**：
  `viewArea` 是 `WS_CLIPCHILDREN`，`InvalidateRect` 标脏的区域在绘制时会避开子窗口 ——
  可是子窗口已经移走了，那一带反而没人画，屏幕上留着上一步的位图。
  改为 `RedrawWindow(RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW|RDW_ALLCHILDREN)`。
- **兄弟窗口的 z 序**：分割条原本带 `WS_CLIPSIBLINGS`，而视图容器不带 ——
  视图后创建就把分割条整条盖住（`HWND_TOP` 也没用，兄弟窗口默认互不裁剪）。
  现在反过来：视图容器加 `WS_CLIPSIBLINGS`（不覆盖兄弟），分割条去掉它（要盖住视图）。
- 验证：比例 50→30→65→40→75→50 连续改 6 次，每次截图扫三行像素，
  命中窄线**恒为 3 条**（窗口左边框 / 中线 / 窗口右边框），中线位置随比例精确移动，
  无任何残留。

**③ 单栏时右侧还有一块占位** —— 两个独立成因：

- **`Layout()` 的指纹被过早记住**。`LayoutKey` 只描述输入（客户区尺寸、四条栏显隐、
  栏高、导航宽、双栏/比例/方向），而启动期 `WM_SIZE` 早于 `OnCreate` 建栏 ——
  那一次 `MoveWindow(tabStrip_.hwnd()==nullptr, ...)` 是空操作，指纹却照样被记下。
  之后 `ShowWindow` 触发的那次**真正的** `Layout()` 输入完全相同，整段被跳过，
  四条栏与窗格就停在创建时的 `100x26` / `10x10`。
  修法：控件（四条栏 / viewArea / 双栏时的主窗格视图）**都建好之后才认指纹**；
  判据用"控件是否都在"而不是"第几次调用"，后者会被任意一次早到的 `WM_SIZE` 打乱。
- **第二窗格只隐藏、不收回几何**。单栏时只 `SetVisible(false)`，窗口仍占着双栏
  时的右半区。现在单栏时把 `pane1` 收成 `0x0` 零矩形（视图对象不销毁 ——
  路径/历史/滚动位置要留住，切回双栏即刻恢复）。
- 实测：单栏 `TabView (201,334,1283,913)` 与 `ViewArea` 完全重合，右侧无占位。

**④ 设置项"双栏"从复选框改为下拉** —— 落盘键 `dualPane` 是 `true/false` 的既有
契约（跨版本、AI、脚本都在读），不能为了换控件改成 `0/1`。所以把"界面怎么表达"
与"数据是什么类型"解耦：

- `settings_reg` 的 `Entry::options` 本来就支持"枚举项画下拉"，只是三处分派都把
  `Kind::Bool` 排在前面，于是布尔项永远走复选框。现在三处统一改成
  **有 options 就优先走下拉**（`settings_dialog_model.cpp` 的布局盒与 `DisplayOf`、
  `settings_dialog_draw.cpp` 的控件绘制、`settings_dialog_actions.cpp` 的点击分派）。
- `dualPane` 登记两个选项：「单栏」(`false`) /「双栏（左右/上下两个窗格）」(`true`)，
  `Kind` 仍是 `Bool`、落盘仍是布尔。
- 验证：`config.list` 的 `DumpAll` 已带出 `options`（label 为中文、value 为布尔），
  说明数据面与界面读的是同一张表。

**⑤ 三条分割线"超出边界、有粗有细、连续断开"** —— 根因是坐标系错配，不是画线不一致：

- 双栏分割条的父是 `viewArea_`，而**导航边界 / AI 边界 / AI 面板宿主的父是主窗口
  `hwnd_`**（见 `CreateSplitBars`）。`Layout()` 里 `inner` 与双栏几何用的是
  `viewArea` 局部坐标（y 从 0 起），但主窗口的子窗口必须用主窗口客户区坐标。
  把 `[0,viewH]` 直接套到主窗口子窗口上，边界条就从 `y=0` 伸进标签栏、
  底部只到 `viewH`（够不到状态栏）—— 看起来"超出内容区上边界、下方断开"，
  而三条线因为起点不同显得长短参差。
- 修法：`const int viewTop = y;`（顶部三条栏的高度和），给两条侧栏边界与
  AI 面板宿主的 y 一律用 `[viewTop, viewTop + viewH]`，与 `ViewArea` /
  两个 `TabView` 完全对齐。
- 确定性验证（按 pid 枚举子窗口、`MapWindowPoints` 换算到主窗口客户区）：
  顶部栏 `y=0..104`、内容区 `ViewArea y=104..683`、状态栏 `y=683..713`；
  AI 面板开/关两种状态下，`navEdge` / `panesBar` / `aiEdge` 三条边界与两个
  `TabView` 的 y **恒为 `104..683`**，无一越界、无一断开。
  画线上也做了对照：三条 `SplitBar::Paint` 的 `theme` / `bg=FFFFFF` /
  `line=#D0D0D0` / 静止态 1px 中线完全一致 —— 线条粗细差异的观感来自
  错配的 y 起点，而非绘制参数不同。
- 顺手修正 `theme.h` 的注释：侧栏边界悬停时是"中线换 accent 并加粗"，
  不是旧注释写的"整条 6px 换 accent"（与实际 `Paint` 不符）。

实现落点：`src/theme.h`（`splitter96` / `splitterHit`）、`src/split_bar.cpp`
（通用实体拖条：导航树 / 双栏 / AI 三条边界共用；实体条绘制 + 防拖影 + 父窗口重绘）、
`src/shell_view.cpp`（`WS_CLIPSIBLINGS` + `SWP_NOCOPYBITS`）、`src/app_layout.cpp`（分隔条定位 + `viewTop` 坐标系对齐 + 指纹就位判据 + 单栏收回第二窗格）、`src/app_panes.cpp`（三条边界的父/坐标系说明 + 拖动回调接线）、
`src/settings_reg_table.cpp`（`MakeBoolOptions`）、`settings_dialog_{model,draw,actions}.cpp`、
`tests/pane_split_test.cpp`（钉住"窗口 = 缝 + 两侧各 1px"）。

**⑥ 边界线仍然"有粗有细、连续断开"** —— 上面的 `viewTop` 修正只解决了上下越界，
真正让观感分裂的是**父窗口/层级不统一**与**原生边框重复**：

- `panesBar_` 与两个文件视图同为 `viewArea_` 的子窗口，能稳定压住 Shell 视图的
  原生边框；`navEdge_` / `aiEdge_` 却挂在主窗口 `hwnd_` 上，处在另一条兄弟裁剪链。
  `PrintWindow` 实测：双栏中线是 `1px #D0D0D0`，导航边界却露出原生树的
  `2px #646464`，AI 边界又与面板自绘的 `2px paneSep` 叠在一起。三处用的是同一份
  `SplitBar::Paint`，差异全在被谁盖住。
- 修法：三条边界与 AI 面板宿主统一改为 `viewArea_` 的子窗口，全部用 viewArea 局部
  坐标；`viewArea_` / `SplitBar` / 导航宿主补 `WS_CLIPSIBLINGS`，让重绘的内容窗口
  不能盖住上层边界。删掉 `aichat_panel.cpp` 自绘的 AI 左边缘，颜色/厚度/命中只归
  `SplitBar` 一处。`viewTop` 坐标系转换随之删除 —— 不再是两套父窗口的补丁。
- 上下栏交界原先也是两条：书签栏底边 `y=103` 的 `#D0D0D0` 紧挨 Shell 视图顶行
  `y=104` 的 `#646464`；状态栏顶边同理。新增两条 1px 内容区覆盖带（复用
  `kViewAreaClass` 的背景刷，无新颜色令牌）压住内容侧原生边框，上下栏各自那条
  `theme.line` 成为唯一可见线。覆盖带位于内容 Resize 之后、边界条抬层级之前，
  三线交叉点仍是完整中线。

**验证**：离屏 `PrintWindow(PW_RENDERFULLCONTENT)` 与屏幕 DC 抓图（稳定度采样差值 0）
一致：三条竖线在 `viewArea y=104..683` 的 **576/576 行**都是纯 `#D0D0D0`，
`y=104/105/393/682` 三处中心全命中；逐像素检查确认每条只有 **1px**，左右相邻为
`bg`。上下交界 `y=103` / `y=683` 各一条贯通 `#D0D0D0`，`y=104` / `y=682`
内容侧已无 `#646464`。放大对照图见 `separator_verification.png`。
`python build_fast.py --out %TEMP%\\sfix.exe` 通过；`bash tests/run_all.sh` 65/65；
`check_commands.py` / `check_ai_meta.py` 全过。

实现落点：`src/app.cpp`（viewArea 裁剪样式、面板父窗口、覆盖带创建）、
`src/app_layout.cpp`（统一 viewArea 坐标 + 覆盖带摆位 + 边界条抬层级）、
`src/app_panes.cpp`（三边界同父）、`src/nav_pane.cpp`（宿主裁剪）、
`src/split_bar.cpp`（裁剪样式）、`src/aichat_panel.{h,cpp}`（删除重复左边缘）。

### 未纳入结构化交互的三类操作补齐（拖放 / 撤销承诺 / 序列键）

这一轮修的三件事，共同点是「人能做到，但做的不是命令、或者命令说了假话」：

**① 书签撤销栈的空承诺（`undoable=1` 但撤不回来）**

`bmk.add.path` / `bmk.add.current` / `bmk.add.selection` / `bmk.add.tabs` / `bmk.newfolder`
五个命令都声明 `undoable=1`，但 `BookmarkStore::AddPath` / `AddFolder` 从不压撤销栈 ——
只有 Remove/Copy/MoveTo/Sort/Dedup/ReplaceAll 压。也就是**新增这一族一条都撤不掉**。
空承诺比不声明更坏：AI 据此敢试错，试完发现退不回来。

- `AddPath` / `AddFolder` 在改树**之前**拍快照（失败早退不占额度）。
- `Merge` 的快照原来是在合并**之后**拍 —— 那份快照存的是合并结果，撤销它等于"撤到一个和现在一样的树"，
  界面还给"已撤销"的成功回执。改成先拍、再无变化时弹掉。
- 单测 `tests/test_store.cpp` 加三组判据钉住（新增可撤、新增分类可撤、合并快照在改前）。

**② 书签拖放是唯一绕过命令总线的写入路径**

`bookmark_drop.cpp` 自己 `AddPath` / `MoveTo` / `Save`，于是同一件事：从菜单做有审计、
进撤销栈、走 `bmk.add.path` 的"已收藏则移动"语义；从拖放做**一条审计都没有**，
撤销栈也不记。现在落点统一经 `bmk.add.path`（`Actor::Drop()`），
显示名仍由拖放侧给（`ico::DisplayName`，否则命名空间项会显示成裸 `::{GUID}`）。

**③ 38 条序列键绑在"有必填参数又没带内联参数"的命令上**

序列派发的参数恒为空，这类绑定每按一次都注定失败；其中三条还会**静默改状态**：
`tab.color` → 标签变无颜色、`win.move` → 窗口挪到左上角、`view.pane.split` → 分栏比例重置 50%。
另五条（`bmk.import` 等）会当场弹输入框。删除 38 条绑定（207 → 169），
带参命令的正确入口是命令面板（按 schema 追问）。

- **防复发机制**：`tools/check_commands.py` 新增规则 C4 —— 从 `argsSchema` 的 `required`
  判定"序列绑了必填参数却没有内联参数"，命中即报错。新增命令时自动生效，不靠有人记得。

**④ 结构检查自身的假绿与假红**

- `check_ai_meta` 第 16 项的菜单扫描面原来写死 7 个文件路径；视图菜单当天拆到
  `nav_bar_view_menu.cpp`（行为不变），于是 5 条视图菜单项立刻报"表已腐烂"。
  改成"发现所有构建 `ui::Menu` 的源文件 + 区域名映射"，未登记区域名即报错。
- 新增 `MENU_DISABLED_HINTS`：「（暂无搜索历史）」这类禁用提示行不再混进"待办缺口"清单。
- 命令表 `argsSchema` 是 C 转义字符串，直接查 `"required"` 永远不命中 ——
  该判据若不先解转义就是**恒绿**（假绿）。已修。
- `tests/run_all.sh` 的库表补齐 `-lws2_32`（与 `build_fast.py` 对齐）：新增的
  `aichat_service.cpp` 用了 winsock，缺这个库导致 62 个单测全 COMPILE-FAIL。

### 设置体系补齐：硬编码搬进配置、缺失项登记、面板与命令同源可见

这一轮处理的是三类"设置债"，它们的共同点是**没有任何检查会报**：硬编码涨价
（改行为要重编译）、字段存在但机器侧看不见（AI/脚本够不着）、可选项缺失
（用户只能改系统来凑）。

**① 硬编码搬进设置（4 项，全部立即生效）**

| 新键 | 原位置 | 为什么必须可调 |
|---|---|---|
| `themeMode` | `theme.cpp` 里恒 `SystemIsDark()` | 深浅色是系统级设置：晚上想用深色得把整个 Windows 一起改。文件管理器有自己的场景（长时间扫目录、对着图片做浅色比对），强制某一档是这类工具的标配 |
| `recentLimit` | `config.cpp` 裸 `30` | 决定地址栏下拉能看多少条历史。是使用习惯问题，不是性能问题 |
| `searchLimit` | `config.cpp` 裸 `20` | 同上，搜索历史 |
| `closedTabLimit` | `tab_set.cpp` 裸 `20`（两处） | 决定 Ctrl+Shift+T 能连退几步 —— 这个数字直接决定"误关的标签还能不能救回来" |
| `aiTimeoutMs` | `ai_server.cpp` 的 `kUiWaitMs=10000` | 长命令（扫描大目录）跑满 10 秒后客户端已收到 `timeout`，而命令还在跑 —— 两端对"成没成"的看法不一致 |

`themeMode` 的判据收成一个纯函数 `ModePrefersDark(mode)`：三个调用点
（`Theme::Init`、`WM_SETTINGCHANGE`、`WM_THEMECHANGED`）此前各自 `SystemIsDark()`，
强制档下会被系统主题变更顶掉；现在三处共用，认不出的取值回落到 `system`。

**② 已存在但没登记的字段（10 项，此前只有手改 json 一条路）**

`navPaneWidth` / `navPaneRoots` / `searchRange` / `searchRangePath` / `searchHistory` /
`sessionSelection` / `sessionHistory` / `sessionPane2Paths` / `sessionPane2History`。
前 4 个开了写入口（宽度与搜索范围立即生效），其余按"运行时另有一份活状态"如实标只读。

`navPaneWidth` 之前**只有拖动分割条一条写路径**，AI 与脚本完全够不着 —— 这正是
"配置文件里有名字、config.list 里没有"的典型能力缺失。

**③ 可选项缺失：`searchRange=2`（此电脑）以前能被 config.set 写进配置**

`search.scope.set` 会拒绝这一档（本宿主做不到整机搜索），但 `config.set searchRange`
只查 `0..3` 范围 —— 于是"永远搜不动的一档"可以躺进配置，用户以为设上了、
之后每次搜索都失败。新增 `IntCheckedSet`（范围 + 业务判据两处拒绝理由分开报），
两处判据共用 `searchscope::RangeSupported`。

**顺手修的两处真错**

- **`sessionPane2Paths` 读写口径不一致**：写盘时**保留**空串（空串 = 该标签当时
  没有右窗格，是与 `sessionPaths` 按下标对齐的占位符），读盘却走会丢空串的
  `ReadStringArray` —— 后面所有标签整体前移一格，重启后右窗格挂到别人的标签上。
  新增 `ReadStringArrayKeepEmpty`，存与读同口径。
- **四条栏高在五处各写一遍**：`tab_strip.h` 的 `kTabBarHeightDp(26)` + 三条栏
  `Create()` 各写 `theme.S(30/26/24)` + `Layout()` 与建树高预算**各自重算一遍**
  （后者注释里写着"公式必须与 Layout() 保持一致"）。收成 `Theme::barTab/barNav/
  barBookmark/barStatus` 与 `Theme::ChromeHeight()`，`kTabBarHeightDp` 改为
  引用 `Theme::barTab96`（`static_assert` 钉住）。改栏高从此只动一处。

**按作用域拆文件**

`config.cpp` 里混着两件事：路径/写入口（被"用户动作"驱动）与 `settings.json` 读写
（被"存盘契约"驱动）。后者迁到 `config_io.cpp`（393 行），两边各自回到 500 行以内。

**验收**

- `settings_reg_test`：444 → **471 项断言全绿**（新增越界拒绝、枚举、`searchRange`
  不可用档位、14 个新项已登记）。
- `tab_set_test`：新增关历史上限三态（默认 20 / 收紧立刻裁 / 调大能多存 / 非法按 0）。
- 新增 `theme_mode_test`：档位 -> 深浅色的确定分支（强制档与系统无关、认不出回落 system）。
- 活体验证（独立 profile，`EXBARBROWSER_MULTI=1`）：`config.list` 54 项 / 12 分组，
  14 个新项全部就位；7 条写入路径（成功 3 / 拒绝 4）判定与报错全对；
  `themeMode=dark` 实测截图亮度中位 25（深色生效），`navPaneWidth=320` 实测
  竖直分界 x=330 / 占 30%；改回 `system`+`220` 后分界回到 230 / 21%。
- 全量构建零 warning 零 error。

### 新增：AI 聊天面板补上真安全边界（两个"配置了但没人读"的假开关）

上一轮修通了 `aichat/` 的管道链路。顺着"审批为什么能绕过"继续查，发现两个安全项
**写在配置里、打在响应里、显示在界面上，但没有任何代码据此拦截**——
比没有更糟，因为用户以为设了边界。

**① `allowedRoots` 路径白名单从未生效。** 它只在 `tools.js` / `opus.js` 里被执行，
而这两个模块在迁移到命名管道后**已无任何调用方**（`server.js` 只 require
`agent.js` / `agent-stream.js`，两者都走 `tools-exbar`）。于是配了白名单也照样
能操作全盘。新增 `service/guard.js` 作为**唯一执行点**，挂在
`tools-exbar.dispatch` 上——那是两条 agent 循环调工具的唯一出口，与宿主侧
"全部经 CommandBus"是同一条纪律。检查参数里所有**看起来像绝对路径**的值
（递归、含数组元素），越界时拒绝并指出是哪个参数（`targets[1]`）。

**② `denyCommands` 的默认值是一串永不匹配的死词。** `["Delete", "Shift",
"ShiftDelete"]` 是 Opus 时代的动词；迁移到点分命令（`fs.delete`）后这三个词
匹配不上任何命令，是一道零拦截的假防线。默认改为空，只认点分名（大小写不敏感，
支持 `fs.*` 前缀族）。

**③ `dryRun` 只显示不拦截。** 它被打进 API 响应、在界面上渲染成 "dry run" 小标签，
但没有任何地方据此拒绝命令。现在接进同一个 guard：按命令表
`annotations.readOnlyHint` 判定，只读放行、写命令拦下（查不到元数据时
fail-closed 当写处理）。此前所有 `dryRun=true` 的部署实际都在真跑命令。

**顺带修一个会让白名单"总是拒绝"的归一化缺陷**：`path.normalize("D:")`
得到 `"D:."`——Windows 上 `D:` 意思是"**D 盘当前目录**"，不是盘根。
而用户和白名单配置几乎都把盘根写成 `D:`，归一化后根变成 `d:.`，
任何 `D:\x` 都判越界。现在裸盘符先补分界符再归一化。

**品牌串回到 `identity.h` 的唯一来源**：UI 里 15 处 `Opus 助手 / Directory Opus /
Opus 文件夹` 全部改为 `ExBar 助手 / ExBarBrowser`；启动横幅不再打印已废弃的
Opus 桥路径（那会把排查方向引到"桥目录配错了"上），改打三个真实生效的开关。

**删除孤儿模块**：`tools.js` / `opus.js` / `mcp-exbar.js` 互相引用成一个闭包，
全仓无人调用（已移到 `_removed_opus/`）。把它们留在 `service/` 里，下一个人会
以为那份 `allowedRoots` 执行点还是活的。

**密钥不入库**：`service/model.json` 里有明文 API key。`.gitignore` 新增
`model.json` / `config.json` / `*.bak-*` / `*.opus.bak` / `logs/`，
只跟踪代码。`package.json` 里 8 个 npm 脚本原先全指向不存在的 `scripts/`
（迁移时没搬），改为真实入口。

**测试**（三套，都能离线跑或对活实例跑）：

| 入口 | 覆盖 | 结果 |
|---|---|---|
| `npm test`（`guard_test.js`） | 纯函数：路径识别、盘根归一化、嵌套收集、黑名单前缀族、dryRun 判定、判定顺序 | **52 通过** |
| `npm run test:policy`（`_guard_e2e.js`） | 真实 dispatch：白名单界内放行 / 界外拒绝、数组第 2 项越界、`errorCode=blocked_by_policy`、复原 | **14 通过** |
| `npm run test:dryrun`（`_dryrun_e2e.js`） | 真实 dispatch：dryRun 下只读可用 / 写被拦 / 关闭后恢复 | **5 通过** |

真实模型对话（`npm run test:e2e`）复测通过：模型调 `tool.list` → 正确回答当前路径。

---

### 新增：双栏之间的分界可以拖（左右 / 上下都行）

之前双栏中间只是 `viewArea` 底色露出的 2px 缝：`paneSplit` 只能靠命令
（`view.pane.split`）或设置面板改，鼠标移上去没有任何反馈 —— 用户看到两栏之间
有条线，自然会去拖它。

- **缝变成真窗口**（`src/pane_splitter.{h,cpp}`，类名 `ExBarBrowser_PaneSplitter`）。
  不是"在 viewArea 上画一条线再靠 `WM_MOUSEMOVE` 反推命中"：`viewArea` 的子窗口是
  两个 Shell 视图容器，鼠标在视图上时消息**不上抛**，那条路永远收不到"靠近缝"
  这件事，光标也永远变不成 ↔。真窗口把命中/悬停/按下/拖动全交给系统。
- **看起来 1px，抓起来 6px**：缝宽是新的主题令牌 `Theme::splitter()`
  （96 DPI 6，可用 `S()` 随 DPI 缩放），中间画 1px 线表示分割位置，其余是抓取余量。
  悬停与拖动时线换成 `accent`，其余时间用 `separator`。
- **光标自动换**：左右并排给 `IDC_SIZEWE`，上下堆叠给 `IDC_SIZENS`
  （`WM_SETCURSOR` 里按当前方向给）。
- **不抢焦点**：分割条不是 `WS_TABSTOP`，按下也不 `SetFocus` —— 否则正在用键盘
  选文件的用户会被打断，双栏"活动窗格"还会跳到一个不是视图的窗口上。
- **抓取偏移**：按下时记下鼠标相对**分割线中心**的偏移，整场拖动都用
  "鼠标 − 偏移"作线位置。不记的话，抓在缝边缘按下会先跳半条缝宽。
- **拖动中不落盘、松手才写**：`OnSplitterDragged(percent, final)` 里
  `final=false` 只改内存并 `Layout()`，`final=true` 才 `Config::Save()`。
  每像素写一次 `settings.json` 没有意义，而崩溃时留下的半截值会盖掉用户
  上一次明确选的比例。百分比未变时也跳过重排（一次拖动几百个像素，
  每个都让两个 Shell 视图整块重绘就是"拖起来一顿一顿"）。
- **几何只有一份**：新增 `src/pane_split.{h,cpp}` 放纯函数 ——
  `ClampCenter` / `CenterFromPercent` / `PercentFromCenter` / `Compute`。
  以前"两栏各占多少"在 `Layout()` 的左右、上下两个分支里各写一遍；
  拖动是第三个调用点，复制第三份必然漂成"左右能拖、上下拖到边界出负边长"。
  `Compute()` 同时给出两栏矩形与分割条矩形，分割线画在哪、两栏就在哪分开。
- **顺带修掉两处真的算错**：
  1. 比例的分母原来是整段窗格区，50% 落成主栏 497 / 次栏 446（缝在次栏里
     又扣了一次）—— 现在分母是"扣掉缝之后的可用长度"，50% 真的均分；
  2. 极窄窗格区（宽度 < 缝宽）会算出右 < 左的矩形，`ShellView::Resize`
     收到负宽直接黑一块 —— 现在缝自动收到窗格区宽度以内，三个矩形宽度恒非负。
- **顺带修掉 `config.set paneVertical` 不生效**：那条路径先把新值写进 `Settings`
  再调 `SetPaneVertical()`，而它开头是"同值就 return" —— 必然短路，方向改了却
  一次 `Layout()` 都不发生（表现为"设了上下堆叠，界面还是左右，要等下次改比例
  才顺带生效"）。现在把"按当前值重排"拆成 `ApplyPaneVertical()`，判重留给
  "我要翻转"的入口。
- 验证：`bash tests/run_all.sh pane_split`（8 组断言，含 50% 均分、比例往返、
  上下限夹取、最小边长、极窄窗格区非负）；隔离 profile 实测几何
  （左右 50%：`TabView (233,366,770,945)` + `(778,366,1315,945)`，分割条
  `(770,366,778,945)`；上下 70%：分割条 `(233,766,1315,774)`），
  单栏下 `IsWindowVisible=0`，`settings.json` 三项落盘值正确。

### 新增：垂直双栏（上下堆叠）

`view.pane.vertical`（`Ctrl+Shift+F6`）把一个标签里的两个窗格从"左右并排"切成
"上下堆叠"，与 Explorer / DOpus 的垂直分栏一致。配置键 `paneVertical`，
AI 快照新增 `paneVertical` 字段。

- **槽位与方向是两回事**：模型里始终是两个槽位（0/1），左右并排时 0=左 1=右，
  上下堆叠时 0=上 1=下。路径 / 历史 / 选中 / 视图档位全挂在**槽位**上，
  **方向只决定这两个槽位怎么摆** —— 所以切方向不丢任何状态：
  另一栏的目录、滚动位置、视图模式全部原样保留。
- **分栏比例复用 `paneSplit`**：左右并排时它表示左栏**宽度**百分比，
  上下堆叠时表示上栏**高度**百分比（同一把尺子，同一条命令 `view.pane.split`）。
- **导航树不搬家**：树始终占内容区最左侧、占满整个高度；上下堆叠时两个窗格
  都在它右边。窗口尺寸与方向无关，树的宽度仍由 `navPaneWidth` 决定。
- **活动窗格与方向无关**：真源仍是 `GetFocus()`（见"双栏活动窗格由焦点驱动"），
  上下堆叠时 `F6` 在上/下两个窗格之间切。
- **中间 6px 缝 = 可拖的分割条**（`src/pane_splitter.*`，见上一条"分界可以拖"）；
  每栏最小边长 80px，空间不够时直接对半分（避免算出负边长）。
- **切换不闪，但"应用"路径不能早退**：`SetPaneVertical` 是**翻转**语义
  （`view.pane.vertical` 走它），方向没变就直接返回，省掉一次让两个 `ShellView`
  整块重绘的 `Layout()`。而 `config.set` 是**应用**语义 —— 设置注册表先把新值写进
  `Settings` 再通知应用层生效，于是拿 `Settings` 里的值做早退判断**必然命中**：
  方向改了却一次 `Layout()` 都不发生，状态快照报"上下堆叠"、界面还是左右并排。
  这条路径单独走 `ApplyPaneVertical()`（无条件落地当前值）。
- 每个视图在一轮布局里**只 `Resize` 一次**，不先撑满再缩回（否则用户看到整块重排）。
- 措辞统一：全局把"左栏 / 右栏"改成"**主窗格 / 第二窗格**"（槽位 0 / 槽 1），
  字段名 `leftPanePath` / `rightPanePath` **不改**（AI 契约），只补注释说明是槽位语义。
- 验证（隔离副本 + 真实树各一轮，只读观测，不发合成输入）：
  - 静态：`check_commands`（命令表 216 条 / 已实现 208）/`gen_keymap --check`
    （103 热键）全通过；`check_ai_meta` 的 28 项里 23 项通过、5 项报错，
    报错全部落在**并发会话正在改的视图菜单**（`src/nav_bar_actions.cpp` 把
    「选择详细信息…/图标大小…/排序方式/分组依据/设置…」从导航栏视图菜单里
    移走了，`tools/check_ai_meta.py` 的 `MENU_ACTIONS` 登记表还写着旧文案），
    与双栏方向无关。
  - 启动即上下堆叠：`TabView0 (670,370,1708,687) 1038x317`、
    `TabView1 (670,693,1708,974) 1038x281`，分割条 `1038x6` 横贯，
    导航树仍占满左侧全高 `176x604`。
  - 运行时热切（AI 通道 `config.set paneVertical`，非模拟输入）：分割条由
    `6x463`（竖）变 `690x6`（横），两栏由左右 342x463 并排改为上下
    690x229 / 690x228，导航树位置与宽度不变 —— 方向只改摆法，状态不丢；
    再切回 `false` 精确复原成 `6x463` + 342x463（往返一致，无残留）。
  - 真实树全量编译 **150** 个 TU 通过并链接成功（`ExBarBrowser.chk.exe`；
    默认输出 `ExBarBrowser.exe` 正被另一个会话的运行实例占用（PID 36548），
    替换被 `Permission denied` 拒 —— 不强杀别人的实例，等它退出后重跑
    `python build_fast.py --out ExBarBrowser.exe --objdir obj_chk` 即可）。

### 新增：导航树组间分隔线（画在原生树的 WM_PAINT 里）

原生导航树的根节点按 Explorer 的习惯分三组（"主文件夹/图库/TAN-个人"、
"桌面/Downloads/文档/…"、"123云盘/Links/…/网络"），Explorer 在组与组之间有一条
1px 横线，我们此前只有组、无线。

- **画法**：把 NSTC 内部的 `SysTreeView32` **子类化**，在它的 `WM_PAINT` 走完
  （`DefSubclassProc` 返回后）用 `GetDC(tv)` 在**同一个窗口**上补画；
  `WM_PRINTCLIENT` 同样处理（否则 `PrintWindow` 抓的图上没有线）。
- **为什么不是"在外面盖一层"**（三条路都实测否决，记录在 `src/nav_pane_sep.h`）：
  1. 往 NSTC 内部树插"外来"空行树项 → **NSTC 崩**（它把树项 `lParam` 当自己的
     节点指针解引用，外来项一插就访问野指针）；
  2. 叠一个分层窗口（`WS_EX_LAYERED`）→ **本机根本创建不出来**
     （`CreateWindowExW` 返回 NULL 且 `GetLastError()==0`；事后补 `GWL_EXSTYLE`
     读回 `LAYERED=0`；`UpdateLayeredWindow`/`SetLayeredWindowAttributes` 报 `0x57`）；
  3. 非分层子窗口 + `CreateWindowRgn` → 形状可行，但线位要靠外部缓存，
     树的滚动/展开会让缓存过期。
- **线位每次绘制现算**：`tree_->GetItemRect()` 给的是**屏幕坐标**，换算到树客户区后
  取"新组第一行的上边界 − 1"。滚动 / 展开 / 收起天然跟随，
  **没有任何同步逻辑**（重绘本身会把旧位置的线擦掉，不留鬼影）。
- **像素基线对齐 Explorer**（实测 `tools/explorer_navpane.bmp`，bg `0x191919`）：
  线高 **1px**，深色主题色 **`0x383838`**（与 Explorer 实测**完全一致**），
  左端从树客户区 `x=10` 起一直到右边缘（不贴左边缘）。
  两条线的**组界与 Explorer 相同**：一条在 `TAN-个人 | 桌面` 之间，
  一条在 `tools | 123云盘` 之间。
- **实测证据**（抓图 = 树客户区 BMP，`PrintWindow` 只读；分析脚本见
  `tools/probe_sep_verify.cpp` + 逐像素比对）：
  - 树客户区 159x604（隔离副本那轮）/ 159x463（当前树那轮，窗口更矮），
    背景 `0x191919`；
  - 分隔线两条：`y=89` 与 `y=299`，色均 `0x383838`，`x=10..158`（149 px）；
  - 两线相隔 210px = 7 行 x 30px 行距；运行日志的根清单是 25~27 项
    （组0 3 项 / 组1 7 项 / 组2 15~17 项 —— Nextcloud 客户端在跑时多 2 项），
    组界始终落在第 3 行（`桌面`）与第 10 行（`123云盘`）—— 行距差正好 7，
    **组界与 Explorer 逐项对齐**（`TAN-个人 | 桌面`、`tools | 123云盘`）。
- **已知差异**（NSTC 固有限制，本轮未修）：Explorer 组间有 30px 空行、线画在空行正中，
  我们只能在行边界画线；Explorer 顶部有 15px 留白、我们没有（整棵树比 Explorer 高 15px）；
  文字左缘比 Explorer 靠右 4px。
- 顺带修掉两处 comctl32 崩溃源：死句柄上的 `RemoveWindowSubclass`
  （改为只在 `WM_NCDESTROY` 里摘）、`sepItems_` 越界读野指针
  （组界项曾按 `roots_` 下标去取 `sepItems_[i-1]`）。

### 修复：AI 聊天面板的管道链路从未真正跑通（4 处断点）

`aichat/`（WebView2 面板 + 本地 Node 服务）接到 ExBarBrowser 命名管道后，
**第一次握手就必然失败**，而且失败被三层各自"看起来正常"的日志掩盖了。
本轮逐条实测定位并修掉四处，其中前两处是致命的（100% 失败）：

1. **`StreamWriter` 写了 UTF-8 BOM**（`service/exbar-bridge.ps1`）。
   `New-Object System.IO.StreamWriter($pipe, [Text.Encoding]::UTF8)` 在第一次写入前
   先吐出 `EF BB BF`，宿主侧按字节解析 JSON → **每个会话的第一条请求都是
   `invalid_json`**。改用 `UTF8Encoding($false)`。
2. **空 `params:{}` 多拼一个 `}`**（同一文件）。原实现"替换整个 `{}`"时把闭合括号
   也算进了替换串，结果 `tools/list`（恰好发空 params）必然 JSON 非法。
   `initialize` 不空所以能过 —— 于是症状是"握手成功、列工具失败"。
   顺带把 `params` 的定位从正则换成花括号深度扫描：工具参数里有叫 `params` 的键时，
   正则会认错对象。
3. **批准后没有把 `approve` 传下去**（`service/pipe-exbar.js` → `tools-exbar.js`）。
   宿主侧危险双门是 `aiAllowDanger` + `approve` 两道。面板里点完"确认执行"，
   调用链却始终不带 `approve` → 用户批准了也只会拿到 `need_approval`，
   表现为"审批点了没用"，而两边都以为自己是对的。
4. **审批判定用的是一份过期的手写清单**（`service/prompt.js`）。
   清单来自 Directory Opus 时代，与命令表已**双向漂移**：漏问 12 条真破坏性命令
   （`tab.close.others`/`left`/`right`/`prev`/`next`、`bmk.remove`、`dev.build`、
   `dev.test`、`search.history.clear`、`nav.history.clear`、`tab.close.duplicates`、
   `ext.terminal.admin`），多问 9 条普通写入。改为直接读命令表自己的
   `annotations.destructiveHint`（与宿主侧双门同源）；查不到元数据时 **fail-closed
   按"需要确认"处理**。

另外修掉两处会在运行时崩掉的问题：

- **`commandLines is not a function`**：迁移到 ExBarBrowser 工具后 `prompt.js` 删掉了
  这个导出，`agent-stream.js` 仍在审批路径上调用它 —— 任何需要审批的操作用户一点
  "确认"就抛异常，整轮中断。审批卡文案改为从命令表元数据推导。
- **桥在重试时偷偷改连别的实例**：父进程已用 `EXBAR_PIPE` 钉住端点（表示"就是那个
  窗口"），重试时却仍会重读 `ai-pipe.txt` 换目标 —— 命令会打到用户没在看的窗口上，
  审批卡描述错对象。现在钉住的端点只原地重试。

> 同一个文件还写明"必须保持纯 ASCII"（PowerShell 5.1 按 GBK 读 `.ps1`，
> UTF-8 中文注释会变成乱码并破坏解析）。本次一度加入中文注释，立刻复现了
> "管道名读到旧值"的错乱，已改回英文注释并加字节级校验。

**UI 侧补上产品最关键的结构化信号**（`ui/app.js` + `app.css`）：

- **自断言明细**：宿主的 `assertions`（`{expr, expect, actual, ok}`）此前被整层丢弃，
  于是"handler 说成了、效果其实没成"这种最危险的结果在面板上显示成绿色"完成"。
  现在逐条列出期望值/实际值，并新增 `mismatch` 档位（"效果未达成"，独立配色，
  与 `done` 视觉上分开）。
- **`errorCode` 与中文原因并列**：客户端要按类别决策（改参数 / 要权限 / 要审批 /
  等用户），只给中文串读不出这一维。
- 工具卡标题改为读命令表的 `title` + 最具体参数（旧代码按 Opus 工具名写的
  `switch` 迁移后一条都命中不了，每张卡都退化成工具名本身）。

**实测记录**（隔离 profile + 独立实例，全程结构化命令，不模拟输入）：

| 项 | 结果 |
|---|---|
| 桥握手 / `tools/list` / `tools/call` / 空 params | 4 种请求形态全部通过 |
| 连上活实例 | 171 个工具，`tool.list` 真实返回目录内容 |
| 审批判定与命令表 | 对比 31 条命令，**0 处不一致**；未知工具 fail-closed 为"需要确认" |
| 不批准 | `need_approval` 拒绝，**文件仍在** |
| 批准（`aiAllowDanger=true`） | 命令真的执行，**文件被删除**（`exists=false`） |
| 真实模型对话 | 模型调 `tool.list` → 拿到结果 → 正确回答"文件夹是空的" |
| 真实模型请求删除 + 用户拒绝 | 审批卡标题/风险文案来自命令表；文件保留；模型不再重试 |
| 桥脚本字节校验 | 非 ASCII 字节 = 0 |

---

### 新增：设置对话框（三栏版式，按 `tmp_settings_mockup` 原型实现）

设置面板从"一个弹出菜单"换成**独立窗口**：左栏分类、中栏表单、右栏说明、
底部状态栏。旧面板把所有项平铺在一张菜单里，项数一多只能靠滚动找，
而"这项属于哪一类"完全没有表达 —— 设置项本来就有明确的领域分组
（栏 / 视图 / 序列键 / 标签 / 窗口 / 会话 / 状态栏 / 键位 / AI），分组就该出现在界面上。

- **表驱动**：每一行都来自 `settings_reg::Entries()`，加一项就多一个可调项，
  界面 / 命令 / AI 三边同时看到它。手写控件的话，"表里有、界面没有"会变成静默的能力缺失。
- **人机同源**：面板的每次改动都走 `config.set` 命令（`CommandBus`），不直接改
  `Settings` 字段。于是界面改动与 AI 改动落在同一条路上：同一份校验、同一份审计、
  同一个"写后回读"；失败时状态栏原样报出命令层给的原因（"点了没反应"是设置里最贵的体验）。
- **非模态**：相当一部分设置立即生效（栏显隐、排序分组、AI 通道），模态窗会把主窗口挡住，
  用户改完只能凭记忆猜"到底变了没有"。它是主窗口的 owned 窗口，主窗口关掉时跟着走。
- **控件按类型分派**：`Bool` → 复选框；带枚举的 `Int`/`Str` → 下拉（`ui::Menu`）；
  带范围的 `Int` → 行内数字编辑框（带单位：毫秒 / % / 像素）；数组 / 对象 → 只读摘要。
  搜索框与数字框是**真 EDIT 控件**（输入法、撤销、Ctrl+A 都有），下拉统一走 `ui::Menu`。
- **只读项照实显示**：窗口几何 / 会话 / 键位覆盖这些项只提供读取，界面上灰显且点不动，
  右栏改说"该项由程序维护"。比"干脆不列出来"诚实 —— 用户不会以为程序没有这个设置。
- **右栏三问**：这一项叫什么、它是什么键（可复制键名 / 命令模板）、什么时候生效
  （"立即生效" / "只读：下次启动生效"）。第三问最容易漏，而"改了没反应"多半就差这一句。
- **搜索**：按名称 / 键名 / 说明 / 分组名过滤（大小写不敏感），结果按组分段并显示命中数
  （`3/8`）。搜索框有占位文字"搜索设置"。
- **密度三档**：行高 32 / 28 / 24，控件高由行高推导（行高 − 6），不另立一张表。
- **左栏滚轮切分类**（原型行为），并有一行提示写明这个手势。

实现落点：`src/settings_dialog.{h,cpp}` + `settings_dialog_model.cpp`（模型 / 排布 / 命中）、
`settings_dialog_actions.cpp`（交互）、`settings_dialog_draw.cpp`（左栏 / 中栏）、
`settings_dialog_draw_help.cpp`（右栏 / 状态栏）—— 按"做什么"拆，不按行数拆。
入口换线：旧 `src/settings_panel.cpp` 删除，改由 `App::ShowSettingsDialog()` 打开
（导航栏设置项与 `win.preferences` 命令都走它）。

配套修了两处：

- **`SplitDoc` 的括号规则**：只读项写成「名称（只读：细节）」时，按第一个冒号拆出来的
  名字是半个括号（「名称（只读」）。现在冒号落在圆括号内时改在括号前拆：名字取括号外，
  说明取括号内（连「只读：」一起）。
- **只读标记不重复**：中栏说明列原先给只读项统一加"只读 · "前缀，而表里的说明本身就
  以「只读：」开头 —— 拼起来是「只读 · 只读：…」。现在只在说明**没提**只读时才补前缀。

再修三处（验收阶段暴露的）：

- **中栏的行原先点不动**。`LayoutRows` 只给段标题条赋了 `Item::rc`，普通行拿到的是
  默认空矩形，而命中测试（`HitItem`）读的正是 `items_[i].rc` —— 于是"点行没反应"，
  点标题条倒是有反应，很容易误判成"只能点分类"。绘制读的是 `rowLayout_`（另一份矩形），
  所以截图照常画得出来，任何检查都不会报。现在每一项都有自己的矩形，且行的矩形**就是**
  项的矩形（一份几何，两个消费者）。
- **搜索框的占位文字原先不显示，且会爆栈崩溃**。`EM_SETCUEBANNER` 需要 comctl32 v6
  清单，本工程没有清单 —— 调用不报错，框里一个字都不出现。改为把占位文字放进 EDIT
  自己的文本里，规则是"框里的文字等于占位文字时不算搜索词"。第一版在控件子类回调里
  读控件文本判占位，而读控件就是发 `WM_GETTEXT` → 回到同一回调 → 无限递归爆栈；
  现在文本在父窗口侧缓存一份（`searchText_` / `searchPlaceholder_`），子类回调与
  绘制只读缓存，`WM_GETTEXT` 只在 `EN_CHANGE` 时发一次。
- **两处空态**：搜索命中 0 项时中栏原先是一片空白（看起来像界面坏了），现在居中写
  「没有匹配「xxx」的设置项」——说清是"没匹配到"而不是"没有这个设置"，前者提醒换词。
  说明列为空的项改画「—」，不留白（原型同此口径）。

### 右键菜单勾选态改用系统标准画法（左独立勾选列 + 系统勾形字形）

菜单里"这一项是开的"以前是**拼进文案**的（`label = "✓ " + 文案`，未勾选行用三个
ASCII 空格占位）。这在自绘弹层里露了三个马脚：勾号走的是**正文字体**里的 U+2713，
跟同一行的 Segoe Fluent Icons 图标不是一个体系；空格比 `✓` 窄，勾选行与未勾选行的
文字**横向对不齐**；勾号混在文案里还让弹层量宽忽宽忽窄。

现按**行业标准**改：

- **勾号位置：最左侧独立一列**（gutter）。与 Win32 `MFT_CHECK`、Windows 11 Fluent
  菜单、W3C ARIA `menuitemcheckbox`、Directory Opus / XYplorer 同一口径
  （macOS / VS Code 放右侧，但本项目是 Windows 平台，不跟）。
- **整列要么全在要么全不在**：只要有一行可勾选就留整列（`HasCheckedRow`），
  否则勾号会忽左忽右；没有可勾选项时这一列不出现，不留空槽。
- **未勾选的行留空**，不再用空格占位；勾选列宽 = 图标槽宽（`PopupCheckColWidth`）。
- **勾号用系统字形** `ico::IconId::Check`（Segoe Fluent Icons U+E73E），
  与同行图标同一字体同一尺寸，不再用正文字体的 `✓`。
- **弹层宽度把勾选列算进去**（`PopupMeasureWidth`），否则勾号会被右边距压掉。

实现落点：`src/menu.{h,cpp}` 只负责把勾选态落进 `PopupRow::checked`（**独立字段**，
label 保持干净文案）；绘制与量宽搬到新文件 `src/popup_list_draw.{h,cpp}`。

### 弹层绘制拆出独立 TU（`popup_list.cpp` 974 → 844 行）

弹层窗口过程原本同时装着消息分发、鼠标捕获、type-ahead、树懒加载、行绘制五件事，
行绘制是唯一"纯看数据画一画"的事，却挤在同一个文件里（974 行，超单文件 500 行闸门）。

- 新增 `src/popup_list_draw.{h,cpp}`：承担行绘制（树模式 / 平铺模式）与弹层宽度计算。
  绘制只拿一份**只读快照**（`PopupDrawParams`），不认识捕获 / type-ahead / 懒加载，
  状态机以后加字段不会顺带影响绘制。
- 行右端的 chevron 命中区（`PopupExpanderRect`）提到头里 inline：**画 chevron 的位置**
  与**能被点中的区域**必须是同一个矩形，两处共用一份而不是复制两份
  （复制的那份迟早和画的那份对不上，于是"看得见但点不动"）。

### 状态栏：图标改用系统默认图标 + 宽度按位数口径

状态栏的图标口径定成**系统默认图标**（设计基线见 `docs/STATUSBAR-DESIGN.md`），
三级降级链：`SHGetStockIconInfo(SIID_*)` → 系统 DLL 序号资源 → Segoe Fluent Icons 字形。

- **新增 `src/sys_icon.{h,cpp}`**：系统图标的**唯一取用口**。语义 → 系统资源一张表，
  句柄进程级缓存、归本模块持有与销毁，调用方只读借用。主题切换 `Invalidate()` 重取。
  - 取 `SIID_FOLDER` / `SIID_DOCNOASSOC`（文件夹 / 文档）、
    `imageres.dll` 序号 **228**（绿圈对勾）、**203**（带底座的容量文件夹）、
    **30**（硬盘）。序号资源跨 Windows 版本不稳定，**取不到即静默退字形**。
  - 系统资源里**没有**日历位图与眼睛位图（实测扫过 shell32/imageres 0..130），
    这两类走字形：`U+E787` 日历、`U+E7B3` 眼睛、`U+E8EC` 标签 —— 均为导出 PNG 核对过。
- **段图标映射只留一份语义**：`StatusSegmentIcon`（字形档）与 `StatusSegmentSysId`
  （位图档）成对维护，状态栏本体与**状态栏右键菜单**共用 —— 同一段在两处长得一样。
  新增 `ui::Menu::AddSystemIcon`，菜单行也走系统位图。
- **数值段宽度改按位数**：`ui::MeasureStableText`（数字换 `'8'` 再量）。
  旧口径"历史最大宽"只增不减，进过一次大目录后该段永久偏宽，窄窗口下白占地方。
- **路径段画当前目录的真实系统图标**（借用 `ico::Cached`，不销毁）。
- **类型段给真实值**：选中项扩展名（`PKEY_FileExtension`，回退显示名后缀），去重上限 32；
  原来这一段是写死的"类型"字面量。
- **隐藏文件三态统一成"当前状态"措辞**：全显 / 隐系统 / 隐全部，不再混用动作式。
- **修：路径段被画两遍**。右段回填只排了 `seg <= Seg_Date`，路径段编号 4 > 3 于是
  被右组又收了一份 —— 同一段画两遍，右组还白占一份宽度。路径段归中间弹性位，独占。
- **新增 `tests/status_bar_test.cpp`**：钉死"每段必有图标（位图或字形兜底）"、
  段位图不撞车、稳定宽按位数不变三���不变量。

### 序列键全量覆盖 + 模式总览弹窗（复刻 ExBar mod 的 ExBarSeqOverlay）

参照 `D:/Windhawk/exbar.cpp` 的序列键弹层，把本工程的序列从 38 条扩到 **209 条**：
保留全部 38 条默认绑定（`g g` / `g t` / `x l a` …）与 3000 ms 前缀状态机不变，
按命令组前缀（`b`书签 `c`配置 `d`开发 `e`外部 `f`文件 `g`前往 `n`导航 `p`路径
`q`搜索 `s`会话 `t`标签 `v`视图 `w`窗口 `x`关闭 `y`复制 `z`工具）为**其余 171 条
已实现命令**各补一条唯一序列 —— 现在全部 207 条已实现命令都能用连击键到达。

- **模式总览弹窗**（`src/sequence_overlay.{h,cpp}`）：与命令面板**同一个弹层**。
  `Ctrl+Shift+P` 或弹窗外按 `/ ! # @ $ & % ^ * ? < > ~` 唤起对应模式；打字过滤、
  ↑↓/PgUp/PgDn 导航、回车派发、Esc 收起。13 个模式复刻 mod 的
  快速跳转 / 当前目录 / 书签 / 标签 / 搜索 / 命令 / 历史 / 路径 / 最近文件 / 筛选 /
  帮助 / 后退栈 / 前进栈，取数全部来自本工程现有数据源（命令目录 / `BookmarkStore`
  / `TabSet` / `Config::recentPaths` / `App::HistoryList` / 当前目录子项），
  回车一律回 `CommandBus`（`Actor::Palette`），不另造导航链。
- 破坏性命令确认、必填参数追问逻辑抽成 `App::InvokeFromPalette`，命令面板与新弹层
  共用一份，避免"弹窗这条绕过批准"的后门。
- `tests/keymap_test.cpp` 期望序列数改为 209；`data/keymap.json` +
  `src/keymap.gen.h` + `check_commands.py` 三方一致（209 序列 / 275 命令引用）。

### 修复：搜索真的能跑了（改走原生搜索文件夹）+ 结果视图默认详细信息

参照 `D:/Windhawk/exbar.cpp` 的做法，搜索从"恒失败的 `search-ms:` URL"改成
**原生搜索文件夹**（`ISearchFolderItemFactory` 造的 PIDL，以 **PIDL** 形式导航）——
`DoNavigate` 拒绝的是**字符串前缀**、不是文件夹类型，同一个文件夹以 PIDL 交出去
shell 就能绑定，导航过去就是**真的搜索结果视图**（排序 / 右键 / 拖放照旧可用）。
实现见 `src/search_folder.{h,cpp}`、`src/shell_view_search.cpp`、`src/app_nav.cpp`。

- **结果视图默认详细信息**：原生搜索文件夹的视图模板默认"内容视图"，且视图是
  **异步**创建的 —— 导航完成那一刻 `GetCurrentView` 还指着旧视图。所以挂 120ms
  定时器等新视图就位再补设 `IFolderView2::SetViewModeAndIconSize(FVM_DETAILS, 16)`
  （`ShellView::OnSearchViewTimer`，最多 ~3 秒）。
- **修掉"换范围不刷新"**：shell 靠**解析名**认"是不是同一个搜索文件夹"，两次搜索
  显示名相同就会直接复用上一次的**结果集**（换到"仅本层"看到的还是递归的全量）。
  所以结果视图的标题改成 `搜索: <词> · <范围落点>`，随范围变 —— 顺带把"在哪儿搜的"
  显示给用户（与资源管理器地址栏的"搜索: xxx  在 <目录> 中"同一路数）。
- **修掉"离开搜索视图后范围残留"**：以前从搜索结果导航去别处（地址栏 / 书签 /
  双击目录）不会清掉窗格的搜索状态，于是下一次搜索拿**旧的来源目录**当范围
  （实测：scope 停在旧的 ThisPC CLSID 上，"当前目录/含子目录"两档直接报
  "它不是磁盘目录"，而视图明明停在 `E://ExBarBrowser`）。现在导航完成时按
  完成路径判定（搜索视图的解析名就是标题），离开就清干净。
- **「仅本层」真的只圈本层**：原生搜索的作用域天生递归，所以"仅本层"用条件里的
  `PKEY_ItemFolderPathDisplay = 作用域目录` 圈出来。实测同一目录同一词：
  仅本层 19 条 / 含子目录 1371 条。
- **「此电脑」这一档：本宿主做不到，如实报错**。原生搜索文件夹的**作用域只能是
  一个普通文件系统目录**（全部实测，见 `src/search_folder.h` 头注释）：不带尾斜杠的
  已知 shell 位置（`C://Users//X`）、卷根（`C://`）、此电脑命名空间项、多元素数组、
  以及**不给作用域**，都绑不上（`BrowseToObject` 返回 S_OK、视图留在原地、随后
  `OnNavigationFailed`）。整机范围要的是 **Explorer 进程内**的 `search-ms:` 全局查询
  宿主，本宿主是自托管的 `IExplorerBrowser`，没有那个宿主。所以 `search.scope.set
  {scope:"thispc"}` **直接拒绝**、范围菜单里那一行摆成灰字「（本宿主暂不可用）」，
  **不悄悄退回别的档**。真正的整机搜索待做：接 Windows Search 查询 API
  （`ISearchQueryHelper`，本工具链无 `searchapi.h`）或自己做混合虚拟文件夹。
- **命令侧**：`search.find` 从 `implemented=false` 恢复为已实现并重新暴露给 AI
  （`ok=true`，断言 `searchQuery=$arg:query.trim` 通过）；`search.scope.list` 的
  每档增加 `supported` 字段，AI 不至于照着菜单去选一个搜不动的档位。
- **自定义范围的路径先规范化再存**：外部路径（命令参数 / 弹窗输入 / AI）里正斜杠
  与重复分隔符都是**合法写法**，但存下来的值会被直接当目录用（`IsDirectory`、
  索引作用域），于是出现"存的是 `E://ExBarBrowser//src`、用的时候报'它不是磁盘
  目录'"这种看不出真因的失败。现在统一过 `pathutil::ToShellPath`
  （`App::SetSearchRange` 存规范形式、`searchscope::Resolve` 出规范形式）——
  存的与用的必须是同一个值，否则范围菜单里显示的路径与实际搜的目录不是一回事。
- **文件拆分**（行数棘轮）：`src/shell_view_search.cpp`（搜索导航 + 详情视图补设
  从 `shell_view.cpp` 迁出）、`search.find` 的 handler 从 `app_commands_files.cpp`
  迁到 `app_commands_search.cpp`（搜索域的命令住在一起）。


### 修复：双栏启动卡慢

- 会话恢复不再按"标签数 × 2"预建 `IExplorerBrowser`：路径/历史先入模型，最后只为
  活动标签建一次右视图；切到其他双栏标签时再由 `Activate` 按需补建。左右目录、
  历史、焦点与活动视图驱动的 chrome 语义都不变。
- `ShellView::SetViewMode()` 移除启动热路径上的列枚举 / 逐项读 `PKEY_Size` 临时诊断
  （每次调用 336 列 + 最多 12 项属性读取，日志达数十 KB）；同类诊断只在
  `EXBAR_DIAG_PREVIEW` 编译开关下保留，正常启动不再跑。

### 修复：双栏活动窗格由焦点驱动（不再绑定左栏）

- 活动窗格此前只由 F6 手动切换，点击右视图不会切换，导致命令/导航/状态栏
  一律打到左栏。现按 DOpus / XYplorer / xplorer² 的契约修正：**焦点进入哪个视图，
  哪个视图就是活动窗格**。
  - **真源是焦点，不是触发方式**：`App::ActivePane()` 每次都按 `GetFocus()` 校正
    `activePane` 缓存（`FocusedPaneIndex`）。键盘 Tab/F6、程序化 `SetFocus`、
    shell 内部焦点迁移、鼠标点击都自动覆盖 —— 不依赖任何单一触发路径。
  - 焦点不在本标签的视图里（地址栏/书签栏/标签栏/弹层）时**保留上次活动窗格**：
    这样"点地址栏输入路径、回车"仍打到最后在用的那一栏（与 DOpus 一致）。
  - `EVENT_OBJECT_FOCUS`（`SetWinEventHook`，out-of-context、**只读观察**）
    在焦点换视图时回投 `WM_APP_FOCUS_CHANGED`，同步标签/地址栏/模式指示/侧栏。
    它不改变焦点、不注入输入，不违反 R21。
  - 鼠标左键按下视图也激活（shell 焦点常落在深层子窗口，容器未必收到
    `WM_SETFOCUS`）。`OnPaneActivated` 仍以 `GetFocus` 的实测结果为准，
    视图"自报激活"不当作真相。
- 切换活动窗格**只搬"活动"身份**，不改另一窗格的路径/选中/视图模式/历史。
- 观测面新增 `leftPanePath` / `rightPanePath`：双栏两栏目录各自可回读，
  用于验证"导航只打到活动窗格、没把另一栏一起带走"。
- 观测面 `focus` 在双栏下区分 `file_view_left` / `file_view_right`（单栏仍为 `file_view`）。

### 新增：导航栏搜索框分裂按钮（左=范围菜单 / 右=历史下拉）+ 5 条搜索域命令

参照 `D:/Windhawk/exbar.cpp` 的搜索框形态改造：搜索框**常驻导航栏右侧**（不再只在
`Ctrl+F` 时出现，窄窗口下自动收起隐藏），并从中间劈成两半 —— **左带点击弹"搜索范围"
菜单，右带点击弹"搜索历史"下拉**。

- **四档范围**（真源 `src/search_scope.{h,cpp}` 的 `searchscope::Resolve`）：
  - `current` 当前目录仅本层 → 作用域收窄到当前目录，再用条件圈"父目录恰好等于它"
  - `subtree` 含子目录 → 作用域 = 当前目录（默认档）
  - `custom` 自定义路径 → 作用域 = 自定义路径
  - `thispc` 此电脑 → **本宿主做不到**（见下面「搜索真的能跑了」那条）
  范围的语义只在 `searchscope::Resolve` 一处；递归与否不再是 crumb 里的一串标志，
  而是"作用域 + 条件"两件确定的事。
- **搜索历史**：记"提交过的词"（与是否成功无关 —— 宿主拒绝 `search-ms` 时，若只在成功后
  记录，历史永远是空的，右带下拉就成了死功能）；去重、置顶、上限 20，随 `settings.json` 落盘。
- **选中历史条目 = 回填不执行**（不替用户按回车），与 `search.query.set` 同语义。
- **5 条新命令**（全经命令总线，菜单项全部转投命令，R20 人机等价已登记）：
  `search.scope.list` / `search.scope.set {scope,path}` / `search.history.list` /
  `search.history.clear` / `search.query.set {query}`。
- **状态回读面新增 4 个字段**：`searchRange`（档位 id）、`searchRangePath`、
  `searchHistoryCount`、`searchInput`。注意与既有 `searchScope`（来源目录）区分：
  前者是"搜哪一片"，后者是"从哪儿搜"。
- **诚实边界（当时）**：这 5 条**都不执行搜索**。执行搜索受制于宿主 ——
  `IExplorerBrowser` 没有搜索带，`search-ms:` 被 `ShellView::DoNavigate` 明确拒绝
  （`docs/EXPLORER-GAP-AUDIT.md` §2.1 F1）。`search.find` 当时维持 `implemented=false`，
  不跟着这批新命令假绿。**（2026-11-22 已改走原生搜索文件夹，见下面「搜索真的能跑了」。）**
- **文件拆分**（行数棘轮销账）：`nav_bar_search.cpp`（搜索框实现迁出）、
  `search_scope.{h,cpp}`（范围真源）、`app_search.cpp`（两个菜单）、
  `app_commands_search.cpp`（5 条 handler）、`cmd_req.{h,cpp}` + `commands_args.cpp`
  （从 commands.h/cpp 迁出，给命令表腾行数）、`settings_reg_valid.{h,cpp}`
  （取值校验与写入构造器从键表迁出）。

### 新增：最小双栏（一个标签两个窗格）

- `App::Tab` 下挂两个 `Pane`，各自持有 `ShellView`、`nav::History`、显示/计数缓存与
  搜索来源；右窗格按需创建，隐藏时不销毁、下次打开沿用原目录和历史。
- 新增 3 条命令：`view.dualpane.toggle`（`Ctrl+F6`，开合双栏）、
  `view.pane.next`（`F6`，切活动窗格）、`view.pane.split {percent}`（20..80，左栏占比）。
  三者都经 `CommandBus`；人侧（热键/命令面板）与机器侧同源，`aiExposed=false`。
- 活动窗格成为导航、后退/前进、搜索、状态栏、文件视图命令与选中恢复的默认目标；
  切换窗格时标签标题、地址栏、视图档位指示与 AI 快照都切到该窗格。
- 标签身份改为跟随活动窗格：`SyncTabIdentity` 把 tabSet 的 path/title/别名/颜色/
  锁定态同步为"当前显示的视图"；两个窗格各自的目录与历史仍独立保存。
  会话落盘固定按左/右窗格写，切焦点不会把两边目录在磁盘上对调。
- 原生导航窗格按"一套共享侧栏"处理：双栏时只给活动视图显示，切窗格时侧栏跟着焦点走，
  不会再出现左右各一条侧栏；单栏时仍显示在唯一视图里。
- 持久化：`dualPane` / `paneSplit` 入设置，`sessionPane2Paths` / `sessionPane2History`
  入会话；单栏启动时只恢复右栏内存状态，不创建隐藏 Shell 视图。
- 新分片：`src/app_panes.cpp`（窗格生命周期/反查/比例）、`src/app_state.cpp`
  （状态/AI 快照）、`src/app_commands_panes.cpp`（命令处理器）。
- 验证：隔离 `obj_dual_verify` 全量编译 112 个 TU 通过；`check_commands` /
  `gen_keymap --check` / `check_ai_meta` 通过；`keymap_test` 期望值 100→102
  （本地 `data/keymap.json` 是个人配置）。`check_components` 现仅剩流程红灯：
  并行会话已重签 `data/eval-surface.json` 但未提交，需人工确认后提交该基线。

### 新增：窗口管理命令体系（11 条，经命令总线）

补齐最小化 / 最大化 / 还原 / 置顶 / 居中 / 移动 / 缩放 / 列出窗口 / 并排 / 级联，
实现集中在 `src/app_commands_window.cpp`（新分片，按作用域从 misc 独立）：

- 单窗口：`win.minimize` / `win.maximize` / `win.restore`（`ShowWindow`）、
  `win.topmost {on}`（`SetWindowPos` HWND_TOPMOST/NOTOPMOST，`SWP_NOACTIVATE`，省略 on=切换）、
  `win.center`（按当前显示器工作区居中）、`win.move {x,y}` / `win.resize {width,height}`。
- 多窗口：`win.list`（只读，列出所有 `ExBarBrowser_Main` 顶层窗口的 hwnd/pid/标题/矩形/状态，
  按 hwnd 排序稳定回读）、`win.tile {orientation}`（横排/纵排）、`win.cascade`（级联）。
  枚举用 `EnumWindows`（不靠 `FindWindowW` 只拿第一个），布局统一带 `SWP_NOACTIVATE`。
- `win.activate` 切前台但 **`aiExposed=false`**：抢焦点不该由机器替人决定，同 `win.quit`。
- 全部经 `CommandBus`，人侧（快捷键/命令面板）与机器侧同源。断言统一走
  `kExemptNoReadback`——窗口矩形/z 序/前台窗口不在 `exbar/state`，回读面是 `win.list`。
- **未做合并窗口**：需要跨进程标签传输协议（ShellView 是进程内 COM，搬不了）；
  拆分复用已有 `tab.move.window`（有损，历史不迁移）。两者都在 `docs/PARITY.md` §3.7 说明。
- 契约同步：`commands-baseline.json` 重签（213 id / 188 旧名）、
  `file-length-baseline.json` 因命令表/枚举真实增长重签（`commands.cpp` 870→885、
  `commands.h` 688→704，**功能增长非欠账恶化**）、`PARITY.md` 总数 196→207 / 已实现 187→198。

R21 边界（已核 AGENTS.md §R21 与 check_components）：禁的是**自动化脚本**模拟输入/抢焦点，
产品代码里由命令触发的 `ShowWindow`/`SetWindowPos`/`SetForegroundWindow` 明确豁免；
并排/级联一律 `SWP_NOACTIVATE`，不抢前台。

### 闭环强门与文档订正（全网闭环范式对标后落地）

对标 Loop Engineering / closed-loop codebase 三层反馈环后，补上"环 2（开发者反馈）"
缺失的 CI 强门：把三个静态检查 + 评估面信任锚从"靠人/agent 自觉跑"变成
push/PR 自动执行的结构性门（`.github/workflows/ci.yml`）。分两层、不伪造绿灯：

- `static-gates`：三个 `check_*` + `gen_eval_baseline.py --check` + `gen_keymap.py --check`，
  纯 Python，任何 runner 都能跑，永远执行。
- `native-build`：真实编译 + 全量单测，硬依赖 Windhawk clang/MinGW，用 self-hosted
  标签 `native-build`；无匹配 runner 时 GitHub 保持 pending（不是绿），如实反映
  "编译门未覆盖"，绝不伪造一个假绿。

README 规模/检查数字订正到 2026-11-22 实测：101 cpp + 82 h / 3.91 万行、
56 单测 / 838 断言、check_ai_meta 28 组；命令表数字后续因窗口管理 +11 条更新为
207 条 / 已实现 198（见本节顶「窗口管理命令体系」）。

> 仍未修、已精确定位的真 bug：`agentctl serve` 在"exe 被占 + 源码更新"时，
> 因残留 `ExBarBrowser.new.exe` 被占用导致 lld 链接阶段 Permission denied，
> `build_fast.py` 抛异常 → `agentctl.ensure_built` 一律报 `build_failed`，
> 已备好的 `exe_replace_blocked` 降级分支被短路（详见审计报告）。这两个文件
> 都在评估面保护内，修复后**必须由人重签** `gen_eval_baseline.py --human-confirm`
> 并提交，故未在本次未经授权的会话中擅自改动 + 自我签发。

### 标准落地与组件检查（本轮收尾）

- 命令处理器按作用域分片：`app_commands_external.cpp` 收 `clipboard.copy` /
  `ext.terminal.at`（只由参数点名目标，不读当前选中或目录），
  `app_commands_bookmark_io.cpp` 收 `bmk.import/export/reload`（书签文件级 IO）；
  两者从 `app_commands_misc.cpp` 迁出，并由 `RegisterCommands()` 显式接线。
  修复拆分时误删的 `AffectsStatusBar()` 签名（曾导致 misc 编译失败）。
- `check_components` 的单文件行数棘轮在 `HEAD` 上本来就是红的：11 个文件超账，
  且基线在签发提交时就已与其内容不一致。本轮先把 `bookmark_bar.cpp` /
  `bookmarks.cpp` 拉回原基线，`app_commands_misc.cpp` 从 850 行降到 732 行；
  其余属历史欠账，重签基线到当前真实行数（17 个文件）。**这是追认现状，
  不是销账**：此后棘轮从当前行数继续收紧，旧账只能降不能涨。
- 验证：`build_fast` 通过；`check_commands` / `check_ai_meta`（28 项）/
  `check_components` 全绿；`run_all.sh` 55 通过 / 1 环境跳过（clipboard_test
  因沙箱无交互桌面权限无法运行，非代码结论）。

### 修复（全量覆盖审计：反向检查器过去是假绿）

`tools/check_ai_meta.py` 第 16 项「UI 菜单动作 → 结构化命令」原先只扫
`tab_strip_actions.cpp` 与 `bookmark_bar.cpp`，且正则只认 `kXxxMenuYyy`
这类符号动作号。书签栏、状态栏、面包屑、导入弹层用的**数字动作号**
（1/2/4/6/7、900、3000…）一个都扫不到，于是「全部通过」只覆盖了标签右键。

- 扫描面扩到 `nav_bar_actions.cpp` / `app_commands.cpp` / `bookmark_io.cpp` /
  `app_commands_files.cpp`；正则放宽到任意动作号；新增 16c 反查过期的登记项。
- 修复后真实报出 9 个缺口（书签打开/复制路径/重载/导入/导出、面包屑复制路径/
  在此开终端、导入模式的追加/覆盖），全部在本批次补齐，不是把检查器改回绿。
- `MENU_ACTIONS` 从 45 项扩到 54 项；第 16b 现在能报告源码里未登记的菜单项。

### 修复（批次 1：命令存在但 UI 绕过了命令总线）

- 状态栏「隐藏状态栏」与段显隐改走 `config.set`；`statusSegs` 从只读改成
  可写（带 0..Seg_Count-1 范围校验），`ConfigSet` 写完后重建状态栏并 `Layout()`。
- 面包屑段右键「新标签 / 新窗口 / 属性」原本直接调 `App` 方法，现改走
  `tab.new` / `fs.open.window` / `fs.properties`，目标路径来自被右键的那一段。
- `settings_reg::SameValue` 原来只比基本类型，数组写后回读恒判失败；
  改为递归比较数组/对象，`config.set statusSegs` 的写后回读才真正可信。

### 新增（批次 2：补 5 条命令，关闭人机等价缺口）

- `bmk.import {path, mode}` / `bmk.export {path}` / `bmk.reload`：
  新增 `BmkExportHtmlTo(path)` 与 `BmkImportHtmlFromFile(path, mode)` 两个
  **确定式后端**；人工菜单仍弹文件对话框，但只是 UI 适配层，读写共用同一份实现。
  覆盖模式是破坏性操作（带 AI 审批语义），两种模式都进书签撤销栈；为此给
  `BookmarkStore::Merge` 补了此前缺失的 `PushUndo()`，撤销声明不再是空承诺。
- `clipboard.copy {text}` / `ext.terminal.at {path}`：书签「复制路径」与面包屑
  「复制此路径 / 在此打开终端」从直连底层函数改为调命令，菜单、热键、
  面板、AI 同源；书签「打开」复用已有的 `fs.open`（参数是书签项的完整路径）。
- `data/commands-baseline.json` / `data/eval-surface.json` / `docs/PARITY.md`
  同步重签：命令表实测 196 条 / 已实现 187 / 未实现 9。

### 变更（"被拦住"不再一锅端：`blocked` 拆成四类 + 一类超时）

以前"被拦住"只有一个 `blocked`。但**被拦住有四种，下一步完全不同**：

| 类别 | 含义 | 调用方该做什么 |
|---|---|---|
| `precondition_unmet` | 前置条件不满足（没选中、目标不在视图、参数缺失） | 换目标 / 先做前置动作，**重试有用** |
| `lock_conflict` | 执行期被占用/被锁（剪贴板、文件被别的程序占着） | 关掉占用方后重试，或换一个目标 |
| `canceled_by_user` | 用户主动取消（拒绝批准、在进度窗口点了取消） | **不要重试** —— 停下来问用户 |
| `not_supported` | 该环境/该对象不支持这件事（视图模式不对、能力不存在） | 换一条路，**重试永远没用** |
| `timed_out` | 等待时限到了，后台作业**仍在跑** | 用 `tool.job.get` 继续查那个 jobId，别重发命令 |

一锅端成 `blocked` 等于让调用方去猜，而它猜错的方式通常是
"一直重试到超时"或"直接放弃"—— 两种都是错的，而且都会变成静默的行为缺陷。

- **新增 `FailKind` 枚举**（`src/commands.h`）—— 类别由**命令调用点**声明：
  `CommandBus::NoteFailure(reason, kind)`。这是关键：`ai_api` 不去匹配中文字符串
  （文案一改就断，那是文案该干的事），"剪贴板被占"在写它的地方就知道它是锁冲突。
  `FailKindCode` 是唯一的枚举 → errorCode 映射点。
- **默认值是 `precondition_unmet`** —— 绝大多数失败确实是"前提没满足"
  （没选中、目标不存在、参数缺失、id 不存在…）。不确定时**不要**硬凑一个类别：
  留默认比猜一个错类别好。
- **`CommandBus` 新增 `lastFailKind()`** —— 与 `lastBlockReason()` 同源：
  UI、AI、审计看到的是同一件事。
- **顺带修掉一个真缺陷**：作业被取消时 `job::Status` 是
  `done=true, cancelled=true, failed=false`，而 `CollectResult` 只判 `failed` ——
  于是被取消的作业会走**成功**分支，把空结果当成"跑完了"报出去
  （"查重没查到重复"其实可能是"用户中途叫停了"）。现在单独判 `cancelled`，
  报 `canceled_by_user`，并在结果里带上 `cancelled: true`。
- **`tools/agentctl.py` 的 `gap` 诊断**同步细分：四类给出四条**不同的**处置建议
  （混成一条，agent 就会把"稍后重试"用在"能力不存在"上）。
- **静态判据**（`tools/check_ai_meta.py` 第 22 项）：白名单加上五个新码，
  并加判据「`blocked` 不许退化回一锅端」—— 码表是从 `FailKindCode` 反查的，
  不是从 `ai_api.cpp` 扫的（后者看不到这几类码）。
- **新增 `tests/fail_kind_test.cpp`**：映射唯一性、类别透传、默认类别、
  前置不满足的归类，以及"被取消的作业不算成功"。
- **兼容性**：旧版本程序仍可能返回一锅端的 `blocked`，客户端按
  `precondition_unmet` 处理（`agentctl.py` 里保留了这条映射）。

### 新增（M3：五条长任务命令作业化 —— 工作线程 + 进度 + 取消 + 异步）

`tool.dupfinder` / `tool.hash` / `tool.compare` / `tool.diskusage` / `tool.topn`
过去**同步跑在 UI 线程**上。一个几万文件的目录能把界面冻住十几秒，
用户连"取消"都按不了 —— UI 线程被占着，连定时器都不转。
`budgetMs` 只保证"不会假死到没完"，不解决"中途不能停"，也不解决"界面不冻结"。

按 `docs/ROADMAP.md` §4.5 的判断，正确形态是**把这五条挪到工作线程 + 进度窗口**，
而不是再造一个 helper 进程（helper 进程要把结果整份搬回主进程，
而这里要的就是"边跑边看进度、随时能停"）。

- **新增 `src/job.{h,cpp}`** —— 进程级作业服务：一条命令对应一个作业，
  作业在**工作线程**上跑，产出结构化结果，并给出**进度**与**取消**。
  三条硬约束：工作线程产出 / UI 线程取走（共享记录 + 互斥锁，调用方永远拿到
  **快照**，不会看到写了一半的中间态）；取消是**协作式**的（置原子位，
  数据层在循环里查 —— 已经被系统调用卡住的那一步中断不了，这是诚实的边界）；
  完成与进度都可订阅（钩子 → 事件推送，客户端不必轮询）。
- **新增 `src/scan_ctl.h`** —— 数据层与命令层之间的控制通道
  （取消标志 + 进度回调）。取消与进度这两件事在三个数据层都要用，
  各自加一份字段就是三份实现，而三份实现迟早分叉。
- **新增 `src/job_ui.{h,cpp}`** —— 进度窗口（自绘进度条 + 「取消」按钮）。
  人工路径弹窗（模态，owner 被禁用），机器路径用 `HWND_MESSAGE` 静默等待 ——
  没人盯着屏幕，却把整个界面锁住，比冻结好不到哪去。
  **人侧不再"等超时就把窗口关掉"**：作业还在后台跑却把窗口关了，
  用户会以为"停了"，于是既看不到结果也不敢做别的事。
  现在人侧等到作业自己结束或用户按「取消」为止；机器路径保留时限。
- **五条长任务命令全部走作业**：准备参数在 UI 线程（`ActiveView()` 是 COM 调用，
  只能在 UI 线程），重活在工作线程，收尾（弹报告）在 UI 线程。
- **新增 `async` 参数**（五条都有）：`true` = 立刻返回 `{jobId, state:"running"}`。
  这是**必须的**：AI 管道的监听线程只有 10 秒超时，同步扫一个大目录必然超时，
  而超时之后命令仍在跑 —— 调用方拿不到结果，也没法叫停。
  异步把"跑多久"与"等多久"拆开。
- **新增三条作业命令**：`tool.job.get`（查状态与结果）、`tool.job.cancel`
  （请求取消）、`tool.job.list`（列出全部）。
  作业是**进程级**对象，所以 scope 是 App —— 写成 CurrentTab 会骗调用方。
- **新增事件 `tool.job.progress` / `tool.job.done`**：完成事件**带结果**，
  否则订阅者收到"做完了"还要再调一次才拿到东西，那等于把轮询换成了两段式轮询。
- **结果新增 `cancelled` 字段**：与 `timedOut` **分开**。超时是"没那么多时间"
  （可以加大预算重试），取消是"我不要了"（重试只是白费力气）。
  把两者混成一个字段，调用方就没法决定下一步该做什么。
- **时间预算的语义统一了**：`budgetMs` 不传 = **不限时**（异步与人工路径的默认）；
  机器同步调用仍默认 1500ms（管道 10 秒就超时，等太久没有意义）。
  三层数据层对"0"的原有解释并不一致（摘要那层 0 = 不限，体积统计那层 0 = 立刻超时，
  且 `tests/disk_scan_test.cpp` 钉着这个行为），所以"不限时"走**新增的 `noTimeLimit` 标志**，
  由命令层统一翻译 —— 不动任何历史语义。
- **顺带补上人侧的空白**：`tool.topn` / `tool.diskusage` 过去**没有任何人可见的输出**
  （命令面板只调 `Invoke` 不调 `InvokeResult`，返回值被丢弃）——
  人在界面上点了"最大的 N 项"，什么都没发生。现在跑完给一份可读报告。
- **命令层按职责拆开**：`app_commands_files.cpp`（1356 → 759）里那五条长任务命令
  搬去了 `app_commands_scan.cpp`（体积统计 + 摘要）与 `app_commands_compare.cpp`
  （查重 + 比较），作业域的命令与事件接线在 `app_commands_jobs.cpp`。
- **新增 `tests/job_test.cpp`**：钉住作业服务的关键契约 ——
  正常路径 / 失败路径 / 取消（取消**不是**失败）/ 进度快照 /
  取消已结束的作业会失败 / 等待超时不装作等到 / `Shutdown` 取消全部并 join。
  退出时不 join 的话，工作线程会往已析构的 Service 上写 ——
  崩在关窗口那一刻，而且崩得莫名其妙（用户只看到"关窗口时闪退"）。

### 变更（按职责拆开两个超长文件：行数棘轮第一次真的收紧了）

上一笔提交引入了"单文件 ≤500 行"的棘轮，但**基线是在编辑中途量的**，
记的行数比真正提交进去的少（如 `shell_ops.cpp` 记 1384、实际提交 1502）。
于是棘轮在自己诞生的那笔提交上就报错 —— 判据从第一天起就是假的。

本轮先把两处按职责切开，再把账改正：

- **`shell_ops.cpp`（1502 → 1338）** 拆成三份：
  - `shell_ops.cpp` —— **视图作用域**的 shell 操作（选中/排序/分组/隐藏/新建…）；
  - `shell_ops_path.{h,cpp}`（新）—— **路径作用域**的文件操作（撤销栈专用：
    `RenamePath` / `MovePathTo` / `CopyPathTo` / `DeletePath`）；
  - `shell_op_exec.{h,cpp}`（新）—— `IFileOperation` 的**唯一**执行器
    （`Run` + `HexHr`）。
  两种作用域混在一个文件里时，读的人要一路翻到文件末尾才发现还有另一套语义；
  而"执行 + 判成败"这套判据（必须同时看 `PerformOperations` 的 HRESULT **和**
  `GetAnyOperationsAborted`）抄一半就会漏 —— 同一件事在删除路径上查 aborted、
  在改名路径上不查。
- **`app_commands_files.cpp`（1356 → 1140）** 拆出 **`app_commands_fs.{h,cpp}`**：
  `fs.*`（按路径点名目标）与 `file.*`（作用于用户碰巧选中的项）**入参语义就不同**，
  混在一起时读的人要翻到中段才知道"这些命令的入参根本不是选中集"。
- **`pathutil::ToShellPaths()`**：批量版归一化。`fs.*` 的 `targets` 本来就是数组，
  批量版让"整批一起归一化"成为**一次**调用 —— 逐项在调用点写循环，
  迟早有人漏掉其中一项（漏掉的那一项就是那个报 `E_INVALIDARG` 的）。
- **`data/file-length-baseline.json` 改正**：记账值改为**提交态的真实行数**。
  棘轮本身没有被绕过（已验证：压低记账值仍会报错；普通检查与构建脚本都不改写基线），
  错的只是量表。
- **`mcp_stdio.cpp` 进"长路径前缀"白名单**：它开的是命名管道
  （`\\.\pipe\...`），不是文件系统路径，加 `\\?\` 前缀会彻底打不开 ——
  与 `ai_server.cpp` 同理。

两个最大文件都降到了**记账值以下**，棘轮第一次真的收紧了（`shell_ops.cpp` 1384→1338、
`app_commands_files.cpp` 1240→1140）。

### 修复（P0：AI 请求队列的写已释放栈）

监听线程把请求交给 UI 线程执行并同步等结果，**10 秒**超时。请求对象原先是
`RunOnUiThread` 的**栈上局部变量**，队列里存裸指针。于是这条路径成立：

1. UI 线程已把请求取走，开始执行（handler 跑得久，比如一次大目录的磁盘扫描）；
2. 监听线程等到 10 秒超时 → 返回 → **栈帧销毁**；
3. UI 线程这时才处理完 → 往 `job->response` 写结果
   → **写已释放的栈**（踩的是监听线程 `ServeLoop` 的栈帧，未定义行为：
   可能当场崩，也可能悄悄改掉别人的数据）。

触发条件很现实：任何跑得比等待时限长的命令都会走到（大目录扫描、大树复制）。

- **请求对象改成共享所有权**（`shared_ptr`）：提交方、队列、UI 线程各持一份引用，
  谁先放手都不影响对象寿命。超时放弃的只是"等结果"这件事，
  UI 线程仍然会把那个请求安全地处理完。
- **抽成独立模块 `src/ai_queue.{h,cpp}`**：这段线程契约（谁拥有请求、提交方能等
  多久、超时之后谁还在用它）是最容易写错的地方，值得单独一个文件、单独单测。
  `ai_server.cpp` 顺带瘦了 30 行。
- **`tests/ai_queue_test.cpp`**：钉住超时路径 —— 提交方超时走人之后，
  UI 线程仍能安全处理完那个请求，队列之后仍然可用。
- 附赠可观测性：`Queue::Abandoned()` 统计"客户端超时之后才处理完"的请求数，
  写进日志 —— 这就是"界面卡了多久"的账。

### 修复（P0：超时响应用错了协议格式）

超时响应过去不论请求是什么格式，一律回 `{"ok":false,"error":"timeout"}`。
对 JSON-RPC / MCP 客户端来说这是个**非法响应**：没有 `id`，客户端无法把它
认成"我那条请求的回复" —— 要么当乱码丢掉，要么当服务端推送，
然后**永远等一个不会来的结果**（挂死）。

MCP stdio 桥接更是直接栽在这里：它靠"有 id、无 method"判响应，
旧格式的超时响应两条都不满足 → 被当成推送转发出去，客户端那头就一直挂着。

- 现在按**请求的格式**给：JSON-RPC 请求回
  `{"jsonrpc":"2.0","id":<id>,"error":{"code":-32000,"message":"请求超时…","errorCode":"timeout"}}`；
  旧格式请求仍旧回 `{"ok":false,"error":"timeout"}`。
- `code` 用 -32000（JSON-RPC 留给实现自定义的区间），语义是"服务端没能在时限内
  处理完"，不是"请求不合法"（那是 -32600）。
- `errorCode` 一并带上：项目口径是 `errorCode` 才是给机器分支的稳定标识。

### 修复（P0：模态框开着的时候仍在派发 AI 命令）

模态框有自己的消息循环，而那个循环里**照样收得到 `WM_TIMER`** ——
于是 AI 通道的泵定时器会在模态框开着的时候照常触发，命令 handler
**重入**地跑在模态框的循环里。那不是"慢一点"，是能出事的：

- handler 可能再开一个模态框（框套框，用户看到两个）；
- handler 可能改选中集/视图，而那个模态框正在等的就是这份状态
  （"重命名"对话框正开着，AI 把它正在改的那一项换掉了）。

- **`modal::Depth()` / `modal::Scope`**：模态层数计数（文件内唯一一份，
  自写模态循环的地方用 `Scope` 包住，别自己维护计数）。
- **派发点避让**：`app_wnd.cpp` 的定时器分支在 `modal::Depth() > 0` 时**不派发**
  AI 请求 —— 请求留在队列里等模态框关掉，客户端那边照旧按 10 秒时限拿超时
  （语义不变，只是不再有重入）。
- **`tests/modal_depth_test.cpp`**：钉住计数必须配平（少减一次 → AI 泵被永久
  关掉；多加一次 → 防护形同虚设。两种都不报错，只是功能不对）。

### 修复（P1：`tool.hash` 没有时间预算，大文件能把界面按死）

`filehash::HashFile` 一直有 `cancel` 参数，但**命令层从来没传过**，
也没有时间预算 —— 于是 `tool.hash` 一个文件就是算到底。
这条命令跑在 **UI 线程**上（AI 请求由定时器泵派发，人按快捷键也是 UI 线程）：
一个几十 MB 的文件要十几秒，那段时间整个界面没有响应，
用户**连取消都按不了**（UI 线程被占着，定时器都不转）。

- `filehash::HashFile` 新增 `budgetMs`（0 = 不限），每 64 KB 检查一次，
  超了就停并置 `Result::timedOut`。
- `tool.hash` 新增 `budgetMs` 参数（默认 1500，与 diskscan / filetools 一致，
  夹在 100–30000）。**预算按整次调用计，不是每个文件一份** ——
  否则 N 个文件就是 N 倍预算，界面照样能卡很久。
- 结果里新增 `timedOut` / `complete`，**如实说"没算完"**：
  只给一串哈希而不说算没算完，调用方会把"预算用完"当成"这些文件就是这些摘要"。
- 预算耗尽时**绝不给半截哈希**（`hex` 留空），只报"未计算"。

### 修复（外部路径没归一化：`fs.*` 收到正斜杠路径就失败）

`SHCreateItemFromParsingName` **只认反斜杠**，收到 `C:/dir/file.txt` 直接返回
`E_INVALIDARG(0x80070057)`。而正斜杠在 Windows 里是完全合法的路径写法
（`CreateFile` 也接受），AI 也天然会这么写 —— 于是同一类输入：
`fs.open "C:/x"` 能导航（那一层有归一化），`fs.rename "C:/x/a.txt"` 却失败。

实测症状：`fs.rename` 报"重命名失败（名称非法或目标被占用）"，
而真因（分隔符）在错误信息里完全看不出来 —— **报一个错的错比报错更糟**。

- **新增 `pathutil::ToShellPath()`**（`src/path_util.{h,cpp}`）：全项目
  "外部路径 → shell API"的**唯一收口**（正斜杠→反斜杠、折叠连续分隔符、
  去尾分隔符；`::` / `shell:` 命名空间原样）。
- `shell_view.cpp` 里那份手抄的归一化删掉，改调同一份。
- `shell_ops.cpp` 的路径入口（`RenameTo` / `CopyPaths` / `RenamePath` /
  `MovePathTo` / `CopyPathTo` / `DeletePath`）全部归一化；
  `fs.*` 的 10 条命令在命令层也归一化（这样撤销记录与回执里存的是规范形式）。
- `tests/path_util_test.cpp` 补 `ToShellPath` 的用例（正斜杠、连续分隔符、
  盘根、UNC 前缀、命名空间路径）。

### 修复（失败原因细分：重命名不再"猜"错因）

`RenameTo` 以前只返回 `bool`，失败原因**只存在于 shell 的 HRESULT 里**，
于是所有失败都被压成同一句"重命名失败（名称非法或目标被占用）"——
这句话对每种原因都不算错，对使用者却毫无用处（该换名？该关掉占用它的程序？
还是权限问题？）。

- `shellops::RenameTo` 增加可选的 `std::wstring* why`，区分五种原因：
  目标不在当前视图 / 目标已存在 / 无法定位源文件（带 HRESULT）/ 重命名请求被拒 /
  文件操作被中止。
- `file.rename.batch` 的 `failed[]` 从"字符串数组"改成"对象数组"
  （`{from, reason}`），`outputSchema` 同步更新 —— 失败项必须带原因，
  只报文件名对使用者没用。
- `RunOp` 拆出 `RunOpEx`：把 `PerformOperations` 的 HRESULT 与
  `GetAnyOperationsAborted` 一起报出来（后者为 TRUE 时 S_OK 也是失败）。

### 修复（撤销失败不再"改撤别的操作"）

`file.undo` 在**应用内撤销栈非空但撤销失败**时会落回 shell 的撤销栈 ——
于是"撤 A 失败"会变成"居然撤了 B"。用户以为退回了 A，实际丢的是 B。

现在：栈非空但撤不动时**到此为止**（两种情形都必须停）：
① 栈顶是人工操作而调用方是机器（让机器撤人工的操作 = 穿透人工操作）；
② 撤销执行本身失败（记录还在栈里，可重试）。
只有"我们的栈本来就是空的"才轮到 shell 的撤销栈。

### 新增（文件操作撤销栈：`file.undo` 真能撤）

`file.undo` 以前只有一条兜底路径：去 shell 视图背景菜单里找 undo 谓词。
那条路对"右键菜单做的操作"有效，对 `fs.*` 这类**按路径点名**的操作无效
（菜单里根本没有对应项）。

- **`src/file_undo.{h,cpp}`**：逆操作式撤销栈（LIFO，上限 20，同 `bmk.*` 口径），
  纯策略、无 COM，可单测。
- 记栈点：`fs.rename` / `file.rename.batch`（批量=一条）/ 交互式 `file.rename`
  （按目录清单差集推新旧名）/ `fs.copy` / `fs.move` / `file.copy.to` / `file.move.to`。
- 撤销用**按路径**的那组 shell 操作（`src/shell_ops.{h,cpp}` 新增
  `RenamePath` / `MovePathTo` / `CopyPathTo` / `DeletePath`）——
  撤销时目标已经不在当前视图里，`Selection` 语义与 `RenameTo` 的
  "必须在视图里"校验都用不了。
- 两条边界规则：机器**不能**撤销人工的操作；撤销失败**不弹栈**
  （部分失败时把已撤成功的项剔掉，让重试只试剩下的）。
- `tests/file_undo_test.cpp` 覆盖：空栈 / LIFO / 上限 / 机器不穿透人工操作 /
  失败不弹栈 / 部分失败 prune / 空记录不入栈。

### 新增（`dev.snapshot` 真落盘 + `dev.rollback` 可逆）

`dev.rollback` 以前直接 `git checkout -- <path>`：**不可逆**（改到哪算哪，
回滚掉的东西没有副本），而文档指引的 `dev.snapshot` **不保存内容**
（只记路径 + 大小 + mtime），照着做等于"先扔掉备份再撤销"。

- **`src/dev_snapshot.{h,cpp}`**：快照把改动文件的**内容副本**写到
  `<repo>/snapshots/<id>/files/<rel>` + `manifest.json`；
  单文件 ≤4MB、单份快照 ≤64MB。
- `dev.snapshot` 现在真的落盘，返回 `snapshotId` / `snapshotDir` /
  `createdAt` / `savedFiles` / `totalFiles` / `files`。
- `dev.rollback` **必须带 `snapshotId`**（缺了就明确失败），按快照还原；
  三分支：有副本 → 写回 / 快照时干净且 git 跟踪 → `git checkout -- path` /
  未跟踪 → 不动并如实报告。
- **回滚本身也可逆**：回滚前先把当前内容存进 `pre-rollback/`，撤错了还能撤回来。

### 修复（`undoable` 不许说谎）

`Effects::undoable` 的语义是"**我们**能不能撤销"，不是"系统理论上能不能"。
11 条命令声明了 `undoable=1` 却没有撤销栈（`tab.move.*` / `tab.pin` /
`file.undo` / `session.named.*`）—— 对调用方说谎。

- 这 11 条的 `undoable` 归零，各带解释注释。
- `tools/check_ai_meta.py` 新增**第 25 项**"可逆性不许说谎"：
  按 `UNDO_MECHANISM` 清单做**双向**校验（声明了必须有机制；
  登记了也必须真声明 `undoable=1`）。

### 修复（`search.find` 的契约不成立）

宿主是 `IExplorerBrowser`（没有搜索带），`search-ms:` 协议走不通 ——
上一轮把 `implemented` 降级为 `false` 时只翻了那一个字段，
`aiExposed` 仍是 `true`（AI 看得到、调不动）、`outputSchema` 仍声明着
（承诺兑现不了）。现已 `aiExposed=false` + `outputSchema=nullptr`，
并在 `check_ai_meta.py` 第 10 项加判据 c：**未实现的命令不得声明 outputSchema**。

### 新增（MCP over stdio 入口：`--mcp-stdio`）

AI 通道原本只有命名管道一种入口，那是**给本机脚本**用的；标准 MCP 客户端
（Claude Desktop / VS Code / Cursor）只认 stdio 或 HTTP。业界现状是
Eagle 随附本地 HTTP MCP 服务器、微软给资源管理器做了官方 MCP 连接器、
Windows ODR 把 `odr mcp add --manifest` 做成系统级标准 —— 没有 stdio 入口，
别的 MCP 客户端要接本程序就得先自己写一个转发器。

- **`src/mcp_stdio.{h,cpp}`**：stdin 一行 → 注入 `params.__token` → 命名管道
  → 管道回一行 → stdout。是**桥接**而不是"再实现一遍协议"：协议、鉴权、
  `errorCode`、自断言、事件流全走原来那条路，stdio 与管道不可能出现两套行为。
  零依赖，只用 Win32。
- **`--mcp-stdio` 启动语义**：等价于 `--new-window` —— 本进程自己当服务器，
  **不**转发给已有实例（转发 = 进程立刻退出 = 客户端只看到管道断开）；
  同时强制开启 AI 通道（客户端用 stdio 拉起它，本身就说明要 AI 通道）。
- **JSON-RPC 通知按规范不回响应**，也不会污染响应流。
- **断开即退**：stdin EOF → 客户端断开 → 进程静默退出（退出码 0），
  不留半个进程在后台；MCP 模式下不弹"确定退出吗"（会把客户端晾在原地）。
- **退出时的 `std::terminate()` 已修**：原先 `Stop()` 从未被调用，进程退出时
  静态 `std::thread` 析构时仍 joinable → `libc++abi: terminating`，退出码 3。
  现在 `Stop()` 由 `App::OnDestroy` 与 `App::Run` 收尾各调一次（幂等），
  用 `CancelSynchronousIo` 唤醒阻塞在 stdin 上的读；线程赖着不走时改为
  detach 而非 join（join 会卡死 UI 线程，detach 后析构也不会 terminate）。
- **`tools/probe_mcp_stdio.py`**：端到端验证（真实进程 + 真实管道，
  不注入键鼠，符合 R21）。覆盖握手、工具清单形状、`tools/call`（含
  `structuredContent`）、未知工具的 `errorCode` 归类、`exbar/state` 扩展方法、
  通知不串位、断开后干净退出。
- 文档：`README.md` §3.2 / §3.5、`docs/AI-INTERFACE.md` §3「stdio 入口」。

### 新增（静态关卡：快捷键冲突）

同一组合被绑两次时，后一条**永远按不出来**（`InputRouter::BuildBindings`
的 `add()` 是先到先得且静默丢弃）。原先只有管理界面能看出来，源码里查不到。

- **`tools/check_commands.py`**：新增冲突检查，覆盖**全部三个来源** ——
  `data/keymap.json` 热键、`src/keymap.cpp` 的 `kSysKeys`（系统原生兜底）、
  `src/input_router.cpp` 的 `kExtra`（应用内附加）。检查器里复刻了一份
  `chord::Parse`（含修饰键别名、大小写、`PgUp`/`PageUp`、单字符落到 VK 表
  等），所以 `Alt+Left` 与 `Alt+←` 会被认出是同一个键。
- **`src/input_router.cpp`**：冲突同时写进日志，不再只标在管理界面上 ——
  `[keymap] 快捷键冲突：Ctrl+C 被绑了 2 次（先到先得，后一条永远按不出来）：…`。

### 新增（静态关卡：单文件 500 行）

`ARCHITECTURE-RULES` §3 要求单文件 ≤500 行，但没有任何机器检查，
靠人记着 → 事实上已经漂了。

- **`tools/check_components.py`**：新增行数检查 + **棘轮**。
  超限文件记进 `data/file-length-baseline.json`（git 跟踪，改它就是一次可见的 diff），
  之后只许降不许升。18 个历史超限文件（`shell_ops.cpp` 1384 行等）先记账，
  从此每长一行都要有人显式承认。

### 修复（文档数字过期）

`README.md` 里的规模数字全部是旧的（写着 2.33 万行 / 39 个单测 / 20.2 MB），
与仓库实际（3.14 万行 / 46 个单测 / 31.5 MB）对不上。已按实测校正，
并补了两条口径命令，下一个读者几秒内能自己复核：

```bash
ls -l ExBarBrowser.exe | awk '{printf "%.1f MB\n", $5/1048576}'
python tools/check_ai_meta.py | grep -cE '^[0-9]+\.'
```

### 新增（AI 通道：失败可机器分类 + 能力面自述）

让"新会话 → 提需求 → 调命令 → 缺什么补什么"这条闭环真正落在产品通道里，而不是外部脚本里：

- **`errorCode`（10 类）**：`tools/call` 的响应与 `command` 事件新增机器码，与中文 `error` 并存。
  `unknown_tool` / `not_exposed` / `not_implemented` 三态分离 —— 以前同一句
  "未知或未暴露给 AI 的命令" 混着三种修法完全不同的情况（新增命令 / 补元数据 / 写 handler）。
  另有 `bad_args` / `schema_invalid` / `danger_disabled` / `need_approval` / `blocked` /
  `failed` / `effect_not_met`（handler 成功但断言不符）。
- **`exbar/capabilities`（旧格式也可 `capabilities`）**：直接由命令表算出
  `{total, implemented, aiExposed, notImplemented[], notExposed[]}`。
  客户端不再需要 grep 源码推断"是没这条命令，还是有但没开放"。
- **`check_ai_meta.py` 第 22 项**：钉住 errorCode 白名单、三态可区分、
  两条通道都暴露能力面、能力面必须数命令表（不另存清单）、单测必须守着。
- `tests/ai_api_test.cpp`：新增 errorCode 五类断言与能力面计数一致性
  （`aiExposed` 必须等于 `tools/list` 条数，两个面不能各说各话）。

### 修复

- `app_commands_misc.cpp` 缺 `#include "prompt.h"`：`NamedSessionSave` 调用 `PromptText` 时
  `use of undeclared identifier`，整仓编不过（其余用 `PromptText` 的分片都包含该头）。
- `tests/run_all.sh` 的链接库补 `-luiautomationcore`：`shell_input` / `tab_uia` 引入 UIA 后，
  主构建的 `LIBS` 有这一项而测试脚本漏了，导致全量单测在链接期
  `undefined symbol: UiaReturnRawElementProvider`（与任何单个测试无关）。

### 新增（导航栏能力缺口：历史 / 最近访问 / 复制内容回读）

导航栏全操作面复核（AI 通道逐条实测）列出的缺口，逐条补成结构化入口：

- **`nav.history.list`**：列出当前标签可后退/可前进的条目（最近的在前）。过去下拉历史只有
  鼠标能看，机器侧只能盲按 `nav.back` 再回读路径 —— 跳到第 3 条得试三次，且不知道会路过哪。
- **`nav.history.jump`**：`{depth, backward?}` 跳到第 N 条。越界先拒绝（`GotoHistory` 对越界
  是静默忽略，机器侧会误以为跳过了）；`to` 取自历史条目而不是跳完再读 `activePath` ——
  导航是异步的，立刻回读只会读到出发地。
- **`nav.history.clear`**：与地址框下拉底部「清除历史记录」同一实现（`App::ClearHistory`），
  返回被清掉的「最近访问」条数。破坏性，需 `aiAllowDanger` + `approve`。
- **`nav.recent.list`**：地址框下拉里「最近访问」的结构化读面。
- **`path.copy.dir` / `path.copy.tab` 返回 `text`**：剪贴板没有读回面（也不打算给 AI 读剪贴板
  ——里面可能有密码），"到底复制了什么"只能由命令自己回报。
- **`state` 新增 `searching` / `searchScope` / `searchQuery`**：搜索态的唯一可回读来源；
  `check_ai_meta.py` 第 17 项把它登记进 `WRITABLE_STATE`，缺回读会直接 FAIL。

### 修复（导航栏复核发现的 5 处缺陷）

- **选中路径错挂到桌面（严重）**：`ShellView::SelectionPaths` 拿 `IFolderView::Items` 的子 PIDL
  去 `SHCreateItemFromIDList` 解析，等于按桌面根拼接 —— 在 `E:\ExBarBrowser` 选中 10 个文件夹，
  回读全成了 `C:\Users\<me>\Desktop\src`。改用 `SHCreateItemWithParent(当前 folder, pidl)`。
  受影响的不只是观测面：`bmk.add.selection` 直接采信这些路径且不校验存在性，
  会把桌面前的假路径写进书签。
- **搜索"假成功"**：`search.find` 与导航栏搜索框都返回 ok + `searching:true`，但视图原地不动
  （零结果查询后 `itemCount` 一个不少）。根因是 `search-ms:` 是协议处理器 URL，
  `IExplorerBrowser` 没有搜索带，`BrowseToObject` 返回 S_OK 却不切视图。
  现在 `ShellView::DoNavigate` 明确拒绝 `search-ms:`，`RunSearch` 只在导航成功后才记
  `searchFrom_/searchQuery_`（先记账再导航会留下假搜索态）。**真正的搜索仍是缺口**，
  见 docs/AI-INTERFACE.md §8。
- **`fs.open` 错误不透明**：目标不存在时只报"命令执行失败"，而人侧 `NavigateActive` 会弹
  "无法打开：…"。补 `NoteFailure("目标不存在：<path>")`；`fs.open.tab` / `fs.open.window` 同步。
- **`go.powershell` 名实不符**：标题"PowerShell 目录"、分组 `Go`（跳转到已知位置），
  实现却是 `RunTerminal(当前目录)` —— 起终端、不导航，且与 `ext.terminal` 完全重复。
  改为真导航到 `%SystemRoot%\System32\WindowsPowerShell\v1.0`（不存在则退回 `System32`）。
- **`nav.up` 在命名空间根的拒绝文案偏**：站在控制面板上按向上得到"已经是顶层（此电脑或网络根）"，
  与现场不符。改为"已经是顶层（此电脑、命名空间或网络根）"。

### 新增（开发期进程生命周期：`tools/agentctl.py`）

闭环的主路径是产品通道（`CommandBus` + 命名管道），**本工具只负责产品不该管的那一段**：
换 exe 必须跨进程，所以需要有人把新实例拉起来并保活。

- **`serve`**：构建（仅当源码比 exe 新）+ 以 `--ai` + `EXBARBROWSER_MULTI=1` 启动 + 等通道就绪，
  然后常驻持有实例。本环境会回收命令的子进程，一次性启动的实例在命令结束时就被收走。
- **`status`**：exe/源码新鲜度（`stale`）、pid 存活、通道可用、工具数、当前 profile。
  `stale=true` 是"正在验证旧 exe"的唯一可靠信号。
- **`gap`**：读产品的 `errorCode`（不解析中文、不 grep 源码）并翻成"改哪几处"；
  依赖选中项的命令先读状态再决定调不调（诊断工具不得有副作用）。
- **profile 感知**：`MULTI` 会把发现文件写到 `data\profile-multi\`，本工具在两个候选目录里
  找"pid 还活着且连得上"的那一份，不写死 `data\ai-pipe.txt`。
- **`down` 只关自己启的实例**（`data\.agentctl.pid` + 管道名里的 hwnd，走 `WM_CLOSE`），
  用户手开的窗口一律不碰。

### 新增（`AGENTS.md`）

给 AI Agent 的工程入口：三条不可协商（R21 / 无第三方库 / 视觉与品牌串单一来源）、命令速查、
代码规则、命名与命令契约、加东西的五条路径、真实坑位表、收工判据、文档索引。
§8「闭环流程」明确写成**通道优先**：结构化 = `tools/list` + `CommandBus` + `verdict/assertions`
+ `errorCode` + `exbar/capabilities`，外部脚本只做进程生命周期。

### 修复

- `app_wnd.cpp` 的 `WM_TIMER` 分支把成员函数 `App::hwnd()` 当成 HWND 值传给 `KillTimer`
  （`WndProc` 是静态成员，没有 `this`），编译报 "call to non-static member function without
  an object argument"；改用消息参数 `h`。

### 修复（结构化审计缺陷 #1–#4：撤销边界/路径校验/返回值/错误透明）

- **撤销按 actor 划边界**：快照栈带来源标签，AI 的撤销只到自己的上一次修改为止；
  顶快照来自界面操作时机器侧 undo 被拒（"上一次修改来自界面操作，AI 撤销到此为止"），
  不再穿透回滚用户几天前的手工整理。界面侧撤销不变（完整 LIFO）。
- **shell 命名空间路径入口校验**：`::{8-4-4-4-12}` 纯格式校验（40 字符），
  `bmk.add.path` / `bmk.edit` 双入口拦截——缺 `}` 的 CLSID 曾照单落盘，
  之后"打不开 + 通用图标"两头排查（当日真实踩坑两次）。
- **三条旧命令补返回值契约**：`bmk.add.path` / `bmk.add.current` / `bmk.newfolder`
  声明 outputSchema 并真实返回 `{"id": ...}`（add.path 移动场景返回 `{"id","moved"}`）；
  连带修复 AI 调 add.current 遇已收藏路径会弹 MessageBox 卡管道线程的问题
  （机器侧改结构化失败）。
- **8 个 handler 补 NoteFailure**：缺少参数 / 幽灵 id / 路径已收藏 / parentId 非分类 /
  空书清空 / 无历史撤销，均报具体原因而非笼统"命令执行失败"。

### 新增（书签能力缺口：批量收藏与复制）

书签全操作面结构化审查确认的两处能力缺口，本次补齐（命令 + 右键菜单双入口，R20 人机等价）：

- **新命令 `bmk.add.tabs`（收藏所有打开标签页）**：把当前所有标签页的目录批量加入书签栏，
  已收藏/空标签跳过（`skipped` 计数）。返回 `added` / `skipped` / `ids`（新条目 id，
  顺序同标签顺序）。入口：书签栏空白处右键 →「收藏所有标签页」（带报数反馈）。
- **新命令 `bmk.copy`（复制书签）**：复制一条书签，分类连子树深拷贝；副本名加「副本」
  后缀、插到原条目之后，返回新条目 `id`。子树所有 id 重新分配——原样拷贝会让副本与
  原件撞 id（Find 命中前一条，删除/改名作用到别人身上）。入口：书签右键 →「复制书签」。
- 两条新命令都带 outputSchema 并**真实返回 id**——与既有 `add.current`/`add.path`/
  `newfolder` 成功时不返回 id 的缺口相反，新命令从第一天就是全返回值契约。

验证：单测 42/42 绿；`check_ai_meta` 21 项全过；`check_commands` 三方一致；E2E 实测
`add.tabs`（added=2/skipped=1 → 二次调用 added=0/skipped=3 幂等）、`copy`（书签副本
紧挨原件；分类副本内子项 id 全新）。测试产物已清理，书签恢复基线。

### 变更（多实例与 AI 通道形态）

- **移除单实例锁**：删除 `Local\ExBarBrowser.Instance` 命名互斥体，不存在任何
  "已在运行 → 拒绝启动"的分支。默认启动改为**激活优先**：找得到已有主窗口就把路径经
  `WM_COPYDATA` 转发过去开新标签并前置窗口，随后新进程退出（360 行为）；找不到
  （最长等 400ms 吃掉双击竞态）就直接新开实例。显式多开：`--new-window`
  （"在新窗口打开"命令自动带上，共享同一份数据）或 `EXBARBROWSER_MULTI`
  （同时落到独立 `data\profile-multi`）。（docs/DECISIONS.md D4 已按复检条件修订）
- **AI 通道默认开启**：`aiEnabled` 默认值由 `false` 改为 `true`；settings.json 里
  显式写 `false` 才关闭。安全口径不变：仍需令牌（`ai-token.txt`），fail-closed 的是
  `aiAllowDanger`（高危命令开关，仍默认关）。
- **AI 通道热插拔**：`aiEnabled` 从"只读、重启生效"改为运行期可切——设置面板/
  `config.set` 改完立即启停管道监听，无需重启（内部走 `WM_APP_SYNC_AI`，避免在
  通道自己的连接上执行 Stop）。
- **每实例发现文件**：新增 `ai-token.<pid>.txt` / `ai-pipe.<pid>.txt`（数据目录下），
  多实例并存时客户端可枚举全部在监听的实例；规范名 `ai-token.txt` / `ai-pipe.txt`
  指向最后启动的实例。退出时只删属于自己的文件（内容校验），不会误删别人的。

### 修复

- **宽字符 printf 的 `%s` 截断**（4 处）：此编译环境下宽字符 printf 的 `%s` 按
  `char*` 解析，`wchar_t*` 实参只剩首字节。实测后果：AI 每实例发现文件被写成
  `a.<pid>.txt`（token/pipe 两份同名互相覆盖）；JSON 报错信息只剩首字符；状态栏
  容量单位只剩首字符；.txt 关联读取异常。统一改 `%ls`（`ai_server.cpp` / `json.cpp` /
  `shell_ops.cpp` / `view_stats.cpp`）。
- AI 订阅推送由 `Sleep(20)` 轮询改为事件等待（最多 20ms）唤醒，空闲订阅者不占 CPU；
  订阅确认写回后才切推模式（客户端看到的顺序恒为「订阅成功 → 事件」）。
- **订阅者断开后通道不再被堵死**：订阅分支原先忽略 `PeekNamedPipe` 的失败返回值，
  客户端一断开，服务端就在订阅态里无限空转 —— 既不重建监听实例，也不释放那个唯一的
  管道实例，之后所有客户端都拿到 `ERROR_PIPE_BUSY`（实测 err=231），直到重启程序才恢复。
  现在断开的管道会被立刻识别并退出该连接，服务端重建监听实例。
- 无订阅者时跳过状态快照轮询：AI 通道默认开启后，快照生成开销不应在没人看时每
  100ms 付一次。
- **命令枚举顺序**：`bmk.add.selection` 从枚举中间移到末尾。`CmdId` 无显式赋值，
  往中间插会让后面所有 id 整体位移，基线（`data/commands-baseline.json`）失配
  （`check_commands.py` 的 C1b 规则）。

### 新增（观测面：选中与焦点）

实测「将选中的文件夹添加到书签」暴露的观测面缺口：`ShellView::SelectedCount()` 内部存在
但仅用于命令前置，`state` 快照看不到选中与焦点，AI 只能靠猜目标。本次补齐：

- **状态快照新增 5 个字段**：`selectionCount` / `selection`（全部选中路径）/
  `selectionFolders`（其中文件夹）/ `selectionTruncated`（超过枚举上限时的截断标记，
  上限 256）/ `focus`（焦点面板：file_view / nav_tree / bookmark_bar / address_bar /
  unknown，按 `GetGUIThreadInfo` 焦点窗口的祖先链判定）。核心在 `ShellView::SelectionPaths`
  （IFolderView 枚举 + `SHParseDisplayName` 查 isFolder）。
- **新增命令 `bmk.add.selection`**：收藏选中的文件夹（过滤非文件夹与已收藏项，返回
  added/skipped）。第一条 `needsSelection=true` 暴露给 AI 的命令——AI 先读快照确认选中，
  再执行。`check_ai_meta.py` 第 4/14 项升级：观测面读不到选中（源码无 `selectionCount`）
  时这类命令自动收紧不暴露；第 8 项顺带修复 requires 元素匹配 bug。
- **会话持久化补 `sessionSelection`**：退出保存选中、切标签恢复选中，
  重开程序不再丢失现场。
- **`ShellView::SelectPaths`**：按路径列表恢复选中（IPersistFolder2 定位文件夹 +
  ILFindChild + SelectItem），供会话恢复使用。

文档同步：FULL-COVERAGE §4.2、OPERATION-MODEL 第 12 步、AI-INTERFACE（needsSelection
例外规则 + 快照字段示例）。

### 修复（审查 explore 对齐：功能断线接线）

全面审查本项目相对 explore 的输入/命令/UI 可达性，修复 4 处"数据或链路齐备但没接上"的断线：

- **Alt+P 预览窗格开关**：`data/keymap.json` 中 `hotkeyTogglePreviewPane` 原绑 `Ctrl+Alt+P`，
  与 `keymap.cpp` 系统键兜底表声明的 `Alt+P` 不一致（快捷键管理器显示前者、用户按后者无反应）。
  改为 `Alt+P` 并重跑 `gen_keymap.py` 同步 `keymap.gen.h`，出厂文件与编译期兜底一致。
- **侧键前进/后退无默认规则**：按钮枚举、shell_input 解析、InputRouter 派发都就绪，但内置默认表
  与 `mouse.json` 都没有侧键规则，侧键完全无反应。补内置默认：X1=后退、X2=前进，覆盖
  空白/标签/导航/地址/树/状态/预览/书签等全部可达 target；文件/文件夹项留给用户自配。
- **侧键消息未接入四个自绘栏**：标签栏、导航栏、书签栏、状态栏原本都不处理
  `WM_XBUTTONDOWN/UP`，侧键落在这些栏上被 `DefWindowProc` 静默吞掉。四栏统一接 XBUTTON，
  经新增的 `XButtonFromWParam`（与 shell_input 同口径，含 PARENTNOTIFY 物理状态兜底）
  解析按钮编号后交给 `MouseSink`，规则引擎在所有区域可达。
- **系统菜单缺应用入口**：窗口有标准系统菜单（`WS_OVERLAPPEDWINDOW` 含 `WS_SYSMENU`），
  但视图菜单/命令面板/首选项/快捷键管理器/退出只有导航栏与命令面板入口。追加到标题栏右键 /
  Alt+Space 系统菜单，`WM_SYSCOMMAND` 自定义 ID 段转投命令总线，与按钮同源，不复制视图菜单实现。

新增 `tests/mouse_rules_test.cpp` 固化侧键默认规则与方向（X1=后退/X2=前进）、target 覆盖、
wParam 解析；全量 42/42 通过，静态检查（keymap/component/command）全绿，全量重编通过。

### 移除（6 条假能力命令）

- **`tool.readtext` / `tool.navigate` / `tool.reveal` / `tool.select` / `tool.open` / `tool.listview`**：
  六条 `Op*` 声明从命令表删除。它们是 `fs.open` / `tool.list` 的语义重复，且**从未有 handler、
  没有键位、没有 agent 暴露**（`aiExposed=false`）—— 挂在目录里只是永远不会兑现的假能力。
  命令表 161 → 155，未实现 14 → 8，Tool 组 7/7 全实现。
  **枚举项保留并标废弃**（C1b：`enum class` 无显式赋值，删中间项会让后续 id 位移）；
  **legacy 映射与基线旧名同步删除**：C2"legacy 名只增不减"保护的是"用户配置引用过的
  工作命令"，而这 6 条从未实现、无 handler、无键位、无 agent 暴露——命令面板过滤
  `implemented=false`，内置 keymap 也没绑，不存在"用户 keymap 会失效"的场景。
  留着指向不存在命令的别名只会让 `check_ai_meta` 报"源命令不存在"。基线 id 数不变（161），
  旧名数 150 → 147。


### 修复（AI 管道并发串线）

- **管道名进程唯一**：无窗口进程（单测、headless e2e、启动早期）原来一律用
  `ExBarBrowser_ai_default`，两个并发实例会连到同一个管道，各自拿自己的令牌去
  握手，随机报 `bad_token`。受控复现：先起一个占位进程，`pipe_e2e_test` 立刻
  12 条失败；修好后同样条件下 35/35 全绿。现在后缀 = 窗口句柄（有窗口时）
  **+ pid**；客户端本来就只读 `ai-pipe.txt`，无需改。`pipe_e2e_test` 增加
  "管道名包含本进程 pid"断言，锁住契约。


**两轮：把"按了没反应"的命令清零，再补上文件工具集。**

这两轮解决的是同一类问题：命令目录里挂着名字、keymap 里绑着键，按下去却什么都不发生，
而且**没有任何提示**。静态审计一次抓出 13 条（`implemented=false` 但 keymap 有引用），
`input_router.cpp` 对它们直接 `continue`。现在剩 0 条 —— `check_commands.py` 的 PENDING
表已空，做不到的那三条键位连同键一起删掉了（见下面「移除」）。

### 新增（文件工具集：`tool.hash` / `tool.dupfinder` / `tool.compare`）

- **`tool.hash`**：MD5 / SHA-1 / SHA-256，走系统 BCrypt，**流式读**（64 KB 块，
  内存占用与文件大小无关）。算法名认不出就失败，不静默退回 SHA-256 —— 否则
  "我算的是 SHA-512"和实际结果无法区分，拿错摘要去核对下载包会得出错误结论。
- **`tool.dupfinder`**：先按大小分组（便宜且能排除绝大多数），命中的候选才读内容算
  SHA-256。判据是"大小 + 内容"，**不靠文件名** —— 改名后的副本一样能找出来
  （实测：`dup1.bin` / `dup2-renamed.bin` / 另一个目录下的 `dup1.bin` 被归为一组）。
- **`tool.compare`**：两侧按相对路径（大小写不敏感）配对，同大小再比内容；
  `byContent=false` 只比大小，此时报告里明确写"同大小不同内容会算作相同"。
  实测能把"1024 字节但内容不同"判成 differ，不是"大小相同即相同"。
- 三条**同一套双入口**（R20）：不带参数调用（热键 / 命令面板）＝ 目标取界面现状
  （选中项 / 当前目录 / 两个路径输入框），结果进**只读报告窗口**；带参数调用
  （agent）＝ 返回结构化 JSON，不弹窗。
- 结果里必带 `scannedFiles` / `hashedFiles` / `deniedDirs` / `timedOut` / `complete`：
  只给"没有重复"而不说"扫没扫完"、"有没有目录打不开"，用户会据此删错东西。

### 新增（`session.list` / `session.delete`）与修复（无参命令从 AI 调会卡死管道）

- **`session.list` / `session.delete` 落地，语义按真实模型收窄**：本工程只有**一条**
  已保存的标签组（`session.save` 覆盖它），所以 list = 读出那一条
  （`{paths, count, active}`，人侧进只读报告窗口）、delete = 清空那一条
  （`{cleared, count}`，破坏性 → agent 需 `approve`，人侧先弹确认）。
  没有假装"多个命名标签组"——那需要名字、重名与覆盖策略，是另一件事。
  至此 Session 组 4/4 全实现；顺带补上原来缺的回读：能存、能恢复，但看不到存了什么。
- **修复：无参数命令从 AI 调用会弹模态窗口，把管道卡到超时。**
  双入口命令一直用 `args.IsEmpty()` 判断"这次是人点的"，这对**无参数**命令是错的：
  agent 传来的也是 `{}`，于是走"人侧"分支弹报告窗口，而 AI 侧在等一个永远不会来的
  应答（实测 `session.list` → `{"ok":false,"error":"timeout"}`，`tool.dupfinder {}` 同理）。
  命令总线本来就有 `actor`（`ui` / `ai`），只是 handler 看不到。现在 Dispatch 记录并
  还原本次来源（`CommandBus::fromMachine()`），双入口统一用新判据
  `IsHumanCall(args)` = 空参数 **且** 不是机器调用；9 处判定全部换掉。
  返回值改成**"成功 / 失败"之外还要能说清"谁触发的"**，这类误判才不会再靠人肉发现。

### 新增（`search.find` 接上命令总线）

- **搜索从"只有搜索框能用"变成命令**：导航栏搜索框早就存在，但命令目录里
  `search.find` 一直是 `implemented=false` —— 命令面板搜不到、键位绑不上、
  agent 完全没有"搜索"这个能力。现在 `{"query":"…"}` 走同一条实现，
  返回 `{query, scope, url, searching}`。
- **搜索是导航，不是只读查询**：结果落在同一个 `IShellView` 里（排序 / 右键 / 拖放
  照旧），所以它会换掉当前标签的内容 —— 原声明 `Effects.readOnly=1` 写错了，
  撤销方式是再搜一次空串（= 回到来源目录）。
- **URL 拼装抽成纯函数**（`searchurl::Build` / `Trim`，`src/search_url.*`）：
  原来这段长在导航栏回调里，既不可复用也不可测。`tests/search_url_test.cpp`
  按住两件事：搜索词里的引号要翻倍转义（否则 `search-ms` 会把短语提前截断）；
  范围**原样**进 `crumb=location:`（现状行为 —— 路径含 `&` 会被当分隔符，
  这是已知局限，固定成契约等有测试兜底时再改）。
- **只打空格不再当成搜索词**：搜索框提交时不做 trim，于是敲几个空格回车就进入
  一个搜不到东西的搜索视图，看起来像"搜索坏了"。现在首尾空白在 `App::RunSearch`
  统一去掉（导航栏与命令共用这一处），空白即"退出搜索"。
- 端到端：`obj/e2e_search.py`（真实管道）实测四种输入 —— 正常词、首尾空格被 trim、
  只有空格 → `searching:false, url:""`、缺 `query` → `失败：缺少必填参数：query`。

### 新增（批量重命名 `Ctrl+Shift+R`、按类型选择）

- **`file.rename.batch`**：模式串 `{name}` / `{ext}` / `{old}` / `{n}` / `{nn}` / `{nnn}`。
  三条安全约束：**全部先校验才动手**（重名 / 撞已有文件 / 非法字符 / 空名）、
  预览确认（前 20 条「旧 → 新」）、逐项 `shellops::RenameTo`（`FOF_ALLOWUNDO`，`Ctrl+Z` 可退）。
  模式里没写 `{ext}` 也**不会丢扩展名**——"改前半段"是最常见的意图，悄悄丢掉 `.txt`
  会让文件双击打不开。纯函数 `filetools::PlanBatchRename`，单测 20 项。
  没有接 PowerRename：外部依赖不保证存在，而"按了没反应"正是要解决的问题本身。
- **`file.select.type`**：以首个选中项为样板选同扩展名的文件（Total Commander 的 Num+ 口径）。
  不取"所有选中项类型的并集"——那样选两三种之后点一次就等于全选，命令会退化成"全选"。

### 修复

- **AI 侧失败不再只说 `{}`**（`ai_server.cpp`）：声明了 `outputSchema` 的命令失败时，
  `hasResult` 为真会把空的 `structuredContent` 当正文输出，真正的原因
  （`r.error`，例如"目标已存在：gamma2.log"）被丢掉。现在失败优先、原因进文本块。
- **新命令失败时给出具体原因**（`CommandBus::NoteFailure`）：`tool.hash` 的算法名打错、
  `tool.dupfinder` 的目录不存在、`file.rename.batch` 的第几条冲突 —— 不再统一报
  "命令执行失败"。名字打错和值超范围该做的下一步完全不同。

### 新增（选择族、撤销、搜索框）

- **选择族四条**：`file.select.invert`（`Ctrl+I`）、`file.select.files`、
  `file.select.folders`（`Ctrl+Alt+D`）、`file.select.pattern`（`Ctrl+Shift+F`/`Ctrl+Shift+K`）。
  匹配语义交给系统的 `PathMatchSpecExW`，不自创通配语法。
- **撤销 `Ctrl+Z`**（`shellops::Undo`）：把撤销转发给 shell 视图自己的撤销栈
  （落盘操作都带 `FOF_ALLOWUNDO`，系统栈里本来就有）。定位菜单项用**菜单文本前缀匹配**，
  不用 `GetCommandString(GCS_VERBW)` —— 后者会逐个懒加载第三方 shell 扩展，
  扩展一阻塞，跑在 UI 线程上的函数永不返回，**整个消息泵卡死**（实测踩到）。
- **搜索框 `Ctrl+F`**：默认隐藏（地址栏是高频控件，不该为偶尔搜索让位），
  显示时占地址框右侧约 1/3，回车走 `search-ms:` 协议，空串＝取消搜索回到原目录。
- **命令面板支持带参命令**：此前含 `required` 的命令在面板里被静默跳过，等于
  "只有 AI 能用"。现在按 `inputSchema` 逐项追问（`CollectParamsInteractive`）。

### 变更

- **模态消息泵收成一份**（`src/modal.cpp` 的 `modal::Run`）：`prompt.cpp`（两个函数
  各一份）、`report.cpp` 以前各写一遍同样的循环，每处都要重新踩
  "`GetMessageW` 返回 -1 是错误 / 0 是 WM_QUIT"和"处理过的按键必须 `continue`"两个坑。
  `check_components.py` 的"模态消息泵"规则现在只允许 `modal.cpp`（另有 app/popup_list
  两处历史实现），`prompt.cpp` 移出白名单。

### 移除

- **三条永远做不到的键位**：`hotkeyMergeAllWindows`（`Ctrl+Alt+W`，单窗口模型没有
  explorer 标签可合并）、`hotkeyToggleCheckboxes`（`Ctrl+Alt+M`，shell 视图无公开接口，
  DECISIONS D2）、`hotkeyAiTogglePanel`（`Ctrl+Alt+A`，本工程的 AI 是**外部 agent**
  通过命名管道驱动窗口，窗口里没有也不需要"聊天"；真该给用户看的是管道名/令牌/最近调用，
  那是另一条命令）。
  留着键位等于对用户撒谎：按了没反应，且永远不会有。至此**占位键位归零**：
  `check_commands` 的 PENDING 表已空。

### 修复（首选项面板接上命令总线）

- **`win.preferences` 从"声明了没接线"变成真命令**：设置面板早就存在，但唯一入口是
  导航栏右键 →「设置…」（动作号 900），命令目录里那条 `CmdId::Preferences` 一直是
  `implemented=false` —— 于是命令面板搜不到它、键位绑不上、agent 也看不见，
  用户被告知"有首选项"却只能靠右键碰运气。现在 `reg(CmdId::Preferences, …)` 打开
  **同一个**面板（不是另写一个），弹窗锚点复用 `NavBar::SettingsAnchor()`：
  鼠标路径与命令路径各算一份位置，布局一调就会指向不同按钮。
- **那条声明自己的三个字段都是错的**：`implemented=false`（面板明明在）、
  `destructive=1`（开一扇窗，不改任何不可逆状态）、`Scope::System`（不碰进程外）。
  元数据写错不会报错，只会让"能不能用 / 要不要确认 / 该不该在面板里出现"全错 ——
  这正是它挂了很久没人发现的原因。
- 顺带纠正文档里一句错话：旧版 PARITY 写"设置面板已存在且视图菜单能打开"，
  实际入口是**导航栏右键菜单**，视图菜单里从来没有这一项。

### 变更（测试不再把"没测成"报成"测不过"）

有两条测试长期"存在但结论不可信"，处理方式是同一条：把**环境不满足**和**代码不合格**
分开报，而不是让假红长期挂在全量跑里 —— 一条永远红的用例，最后会让真正的回归也被当成
"这条一直红"。

- **`tests/run_all.sh` 新增退出码 77 = 无法运行**（对齐 autotools 的 77 约定）：
  单列计数、不计入退出码，但一定打印。此前沿用 "非 0 即失败"，于是同一类问题
  既可能真的是 bug，也可能只是这台机器测不了，看的人无从区分。
- **`clipboard_test` 先探前置条件**：它要打开系统剪贴板，而剪贴板是**全系统独占**的 ——
  别的进程正持有、或本进程没有交互桌面权限，`OpenClipboard` 都返回
  `ERROR_ACCESS_DENIED(5)`，9 条 CHECK 会全红。实测过这个来回：同一批 CHECK 上午 9 红、
  下午两次全绿，被测代码一个字节没变。现在探不通就打印
  "OpenClipboard 失败，GetLastError=5（进程会话 N）" 后按 77 退出，一句话说清是环境。
  探针等 0.5s，比产品代码自己的 100ms 重试预算宽 —— 那个预算管"一次复制别偶发失败"，
  探针管"这台机器现在能不能测"，标准不该一样。
  （诊断文本用窄字符串：`printf("%ls")` 在 C locale 下遇到非 ASCII 就截断，
  第一次改的时候正好把最要紧的错误码截掉了。）
- **`test_store` 的夹具写入改成必须成功**：`tf::WriteText` 是"写 `.tmp` → `MoveFileEx`
  替换"的原子写，改名那一下会被 AV / 搜索索引器瞬时占用挡住，而 12 处夹具调用
  **都不看返回值** —— 写失败后 `store.Load` 读到的是上一段留下的内容，于是报出假红。
  实测过一次：全量跑时 `无重复 -> 删 0 条` / `无重复时不产生撤销快照` 两条失败，
  同一个二进制单独重跑 3 次全过。现在重试 0.5s，仍失败按 77 退出。

---

**书签栏对标补强（DOpus / Total Commander / XYplorer）。**
围绕三条"结构上缺失"的东西：导入/导出的编码会**改坏路径**、分类**只能读不能组织**、
整栏**没有键盘路径**。

### 修复

- **书签 URL 编解码按 UTF-8 还原百分号序列**（`bmk_html.cpp` 的 `UrlToPath`/`PathToUrl`）。
  旧实现把每个 `%XX` 单独 `static_cast<wchar_t>(wcstol(hex))`，于是 `%E4%B8%AD`（"中"）
  被解成三个拉丁补充字符 `ä¸­` —— **从 Chrome/Edge 导入中文书签，路径整条写坏**，
  且每导一次坏一次。现在按 UTF-8 字节序列整体还原；非法序列返回空（拒绝把坏路径写进书签）。
  UNC 同时支持 `file://server/share` 与旧版多斜杠 `file://///srv/share`；`http:` 等非文件
  协议原样返回（不改成本地路径）。
- **`Unescape` 认数字实体**（`&#39;` / `&#x27;`）。Chrome 把单引号写成 `&#39;`，
  旧注释断言"Netscape 格式不会出现"——于是导入后名叫 `Bob's` 的分类变成字面的 `Bob&#39;s`。
- **`MoveTo` 的悬空指针**：曾在 `roots_` 里 `erase` 之后继续用旧 `dest` 指针
  `push_back`，把书签拖进分类是**访问违例**（实测崩溃）。改为 erase 后重新取目标容器。
- **首次运行写不出书签**：`Load` 失败分支没有清 `fileStamp_`，文件不存在时
  `FileChanged()` 保守判为"有变化"，`Save` 永远被拦。现在读失败同时清基线。
- **「按名称排序」只排顶层**：分类内部的顺序纹丝不动，等于排了一半。改为 `SortRecursive`。
- **导入会丢空分类**：没有 `H3` 文本的分类被当成空名，`Merge` 按名字对齐时全部塌成一个。
- **溢出菜单列出全部书签**（含已显示在栏上的），等于「点了没意义」。现在只列被折叠的项。
- **分类下拉是"整棵子树拍平 + 两空格缩进"**：几十行长菜单、层级看不出来。改为逐级下钻。
- **`Save` 静默覆盖外部修改**：本程序开着时，在编辑器里改 `bookmarks.html`，
  之后本程序加一个书签就把外部修改整份冲掉。现在拒绝覆盖、把内存书签另存
  `bookmarks.html.conflict.html` 并弹窗说明，两边都不丢。
- **`test_store` 会改用户的真实书签**：测试原本读写 `<exe>\data\bookmarks.html`，
  跑一次就把用户书签换成测试种子。现在全程在自己的 `%TEMP%` 目录里，用户文件零改动
  （已用 md5 验证）。
- **单实例转发按 1024 字符截断路径**（`main.cpp`）：深路径/长命名空间被截断后解析失败，
  表现为"从资源管理器打开深目录没反应"。改为按实际长度发送（接收端本来就是变长读取）。
- **书签图标两份所有权**：`BookmarkBar` 自建 `map<路径,HICON>` 并在 `Reload` 里全部
  `DestroyIcon`，而其余栏早已走进程级 `ico::Cached`。改为统一 `Cached`，`Reload` 变纯查表。

### 新增

- **书签栏键盘操作**：`Ctrl+B` 进入本栏；`←/→`、`Home/End` 移动，`Enter`/空格打开，
  `Shift+F10`/应用键出菜单，`Esc` 退回视图。此前整栏**没有任何键盘路径**，只能用鼠标点。
- **拖书签到分类**：拖到分类中间即装进去（整项描边提示），`MoveTo` 拒绝成环
  （拖进自己的子孙会让子树整棵脱落）。此前分类只能读，不能组织。
- **递归排序 / 清理重复书签**（`bmk.sort` / `bmk.dedupe`，均可撤销）。
- **菜单动作全部补成命令**：`bmk.edit` / `bmk.remove` / `bmk.newfolder` / `bmk.undo` /
  `bmk.clear` / `bmk.move` / `bmk.add.path` / `bmk.list`（读）/ `bmk.focus`。
  菜单与拖放改为**调用命令**，于是鼠标、键盘、命令面板、AI 走同一份实现（R20）。
  `check_ai_meta` 第 16 项（反向覆盖）从 7 项缺口降到 0（书签部分）。
- **空书签栏提示行**（`Ctrl+D` 收藏 / `Ctrl+B` 聚焦），而不是一条什么都没有的灰带。
- 单测：`bmk_html_test` 51 项（新增 URL 往返 / UTF-8 解码 / UNC / 数字实体）、
  `test_store` 65 项（新增 MoveTo 成环拒绝 / 递归排序 / 跨层级去重 / 冲突另存）。

---

### 修复（P0-6：审计改结构化 JSONL；被拒的调用也必须留痕）

审计过去只有一种形状：`data/audit.log` 里一行文本，记着"谁、什么命令、什么参数、成没成"。
它撑不住三件事，于是三件事都答不了：

1. **谁做的** —— `actor` 是个字符串，`palette` / `key` / `mouse` / `seq` / `drop` / `sysmenu`
   全是真人，却和 `ai` 一样只是字符串。分不清人和机器，审批与审计就没法分级。
2. **为什么没成** —— 失败原因被压成一句中文，客户端只能猜。
3. **机器可判读** —— 文本行里没有稳定的字段名，脚本要靠正则抠。

- **新增 `src/audit_log.{h,cpp}`**：审计的**唯一事实源**是结构化的 `auditlog::Record`，
  `data/audit.jsonl`（R18 要求的那份，一行一条 JSON）与 `data/audit.log`
  （旧文本镜像，给既有脚本 grep）**由同一条记录渲染** —— 两个来源对不上这种事从此不可能发生。
- **字段补齐到 15 项**：`ts / actorKind / actorName / cmd / args / argsHash / ok / verdict /
  errorCode / durationMs / approval / approvedBy / sessionId / traceId / pid`。
  其中 `actorKind` 用 P0-10 建立的 `ActorKind`（`human` / `ai`，不再只有字符串）；
  `traceId` 取 JSON-RPC / MCP 的**请求 id** —— 客户端手里只有它自己发的 id，
  审计里记着同一个 id，两边才能对账；自己编的序号客户端无从校验，等于没写。
- **拒绝路径全部落审计**：此前 AI 通道有一整段在进 CommandBus **之前**就返回了
  （未知命令 / 未暴露 / 未实现 / schema 非法 / 参数不合法 / 危险未批准），
  这些**一条审计都不写** —— 实测连续 4 次被拒，审计文件 0 增长。
  而被拒的调用恰恰最值得追（反复撞同一个错 = 调用方用错了方法；
  反复撞 `danger_disabled` = 有人在试探破坏性操作的边界）。
  现在每条拒绝都带机器可判读的 `errorCode`：`unknown_tool` / `not_exposed` /
  `not_implemented` / `schema_invalid` / `bad_args` / `danger_disabled` / `need_approval`。
- **审批状态与"没批"分开**：`not_required` / `granted` / `required` / `denied`。
  `denied` 与 `required` 差一个字，含义差很多：前者是"根本不让批"，后者是"要批，但这次没批"。
- **参数哈希算在完整参数上**：`argsHash` 用 `json::Stringify(args.raw(), 0)` 全量算，
  不像给人看的 `args` 那样截断到 160 字符 —— 否则两组只差末尾的长参数会撞成同一个哈希。
- 单测：`tests/audit_log_test.cpp`（哈希确定性 / 差异性 / **长参数只差末尾也必须不同哈希** /
  JSON 转义往返 / 文本镜像格式 / `Scope` 出作用域恢复）、`tests/audit_test.cpp` 重写。

### 修复（P1-7：`dev.build` / `dev.test` 的"超时"从"报一句"改成"真杀"）

自我迭代的闭环里，最危险的一步是"在开发机上跑 agent 写的代码"。这条路上的闸门有两个洞：

- **超时不是硬的** —— 旧实现里 `kJobTimeoutMs`（30 分钟）只用于**查询作业时现编一个"超时"结论**，
  进程其实还在跑。于是 agent 交一份死循环的代码 = 开发机上多一个永远停不下来的进程，
  而 agent 还以为"编译卡住了"，去改本来没问题的地方。
- **读输出这一步本身没有时限** —— 旧实现先在主线程里读干管道（阻塞到 EOF），
  **然后**才去等进程。"进程不退、管道不 EOF"这一种情况会卡在读上，超时根本轮不到。
  而孙子（编译器、测试二进制）攥着管道写端不撒手，正是最容易触发这一种情况。

修法：

- **作业对象（Job Object）当进程树的天花板**：进程一启动就 `CREATE_SUSPENDED`，
  **先归组再解冻**（否则孙子在归组之前就 fork 出来了，收不进来），
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` + `TerminateJobObject` 一次收掉整棵树。
  只杀直接进程是不够的 —— 孙子照样活着，而且管道写端还被它们攥着。
- **读与等分开**：读输出放到独立线程，主线程只负责盯时限并负责杀。
  于是"到点就返回"是真的到点，不是"等它自己结束"。
- **时限是常量，不给调用方自选**：`kBuildTimeoutMs`（30 分钟）/ `kTestTimeoutMs`（15 分钟）
  写在 `dev_tools.h` 里。让 agent 自己填超时 = 让被看管的人决定看管多久，那就没有闸门了。
  真实需求（编译很慢）应该**改这里**，那道改动会留在 git 里；运行时参数不会。
- **"被超时杀掉"与"跑失败"分成两个字段**（`timedOut` / `exitCode`）：
  前者指向环境或死循环，后者指向测试没过。混成一个字段，agent 就会改错地方。
- 单测 `tests/dev_proc_test.cpp` 新增 6 项断言，其中最关键的一条用
  "**孙子迟到 3 秒写标记文件**"来验进程树：标记文件没出现，才说明整棵树真的死了
  （只杀直接进程的话它会出现）。

### 修复（`FailKindCode` 落在匿名 namespace 里 → 链接不上）

`commands.h` 对外声明了 `FailKindCode`，定义却在 `commands.cpp` 的**匿名 namespace** 内 ——
定义只有内部链接，别的 TU 链不上（症状是 `undefined symbol: FailKindCode(FailKind)`，
而头文件与实现都"看着对"）。已把定义移到匿名 namespace 之外。

### 杂项（按职责拆分与工具链修补）

- **按职责拆分**（行数棘轮不许抬基线，只许把代码搬走）：
  `commands.cpp` → `commands_audit.cpp`（审计落盘）、
  `ai_api.cpp` → `ai_api_json.cpp`（JSON 序列化段）。
  拆分的判据是"一个文件只做一件事"，不是行数。
- **`tests/run_all.sh` 剔除孤儿 obj**：脚本用 `ls $OBJDIR/*.obj` 收集对象，
  源文件改名后旧 `.obj` 留在原地 → 链接期 duplicate symbol。
  现在按 `src/<base>.cpp` 是否存在来剔（`app.res` 例外）。
  推翻了报告 P2-5「该缺陷不成立」的结论：它在主构建不成立，在测试脚本成立。
- **`RiskName` / `ScopeName` / `TraceIdOf` 提升进 `ai_api.h`**：它们是**协议的一部分** ——
  `tools/list` 里写的、结果里回的、审计里记的，必须是同一个词。各写一份迟早分叉，
  而分叉之后客户端按 `danger` 过滤、审计按 `write` 记录，两边都"对"，但对不上。

---

## [0.2.2] - 2026-11-21 · 导航栏对标补强

**导航栏（DOpus / Total Commander / XYplorer）。**
围绕"看起来能用、实际是死的"这一类：盘根与 UNC 的补全、命名空间地址栏回车、
带引号的粘贴路径、后退/前进恒亮、面包屑 UNC 塌成一段。

### 新增

- **面包屑段右键菜单**（`nav.seg.<n>` 锚点）：在新标签页打开 / 在新窗口打开 /
  复制此路径 / 在此打开终端 / 属性。动作作用于**右键的那一段**，不是当前目录 ——
  用户右键 `D:\` 却复制出 `D:\Projects\foo` 是纯粹的错。幽灵段（向上导航后被放弃的
  更深分支）不弹菜单：它对应的目录可能已被删除。
- **导航按钮气泡提示**：5 颗纯图标按钮此前没有任何文字说明。灰掉的按钮提示会追加
  「（当前不可用）」，避免"按钮画着禁用色却写着『后退』"让人以为程序卡了。
- **视图菜单标注当前状态**：8 档模式 / 4 种排序 / 4 种分组都用 `✓ ` 打勾，排序项
  再追加「（升序）/（降序）」。此前菜单完全不标当前值，用户点完不知道有没有生效。
  排序方向从**视图**现读（`shellops::GetSort`）而不是抄配置 —— 用户点列头反向排序时，
  配置里还留着旧方向。
- **弹层预选并滚到当前项**（`PopupInitial`）：地址栏 ⌄ 下拉里混着后退/前进/最近访问
  三段，当前目录常在中后段，此前要在 20 行里自己找。视图菜单预选当前档位，
  键盘用户按 ↓ 即从这里继续。
- **弹层 `Home` / `End` / `PgUp` / `PgDn`**：命令面板与标签菜单都有 20+ 行，
  此前只能靠方向键逐行挪。翻页按"一次能看见几行"跳，不是固定 5 行。
- **弹层键入首字母跳转（type-ahead）**：本列表是 `LBS_OWNERDRAWVARIABLE` 且插入的是
  空字符串（文字在 `WM_DRAWITEM` 里按行号自绘），系统默认的 `LB_FINDSTRING` 完全失效。
  自建 900ms 累积缓冲，反复按同一字母在同类条目间轮换；前缀无匹配时退回单字符重试
  （否则打错一个字母后越打越没反应）。
- **标签右键菜单接入既有字形图标体系**：新建 / 关闭 / 重命名 / 着色 / 复制 /
  移动 / 锁定 / 重新打开各项现在都有图标，颜色二级菜单同步带图标。
  扩展现有 `PopupRow` 的图标槽为 `glyph`，由 `ico::IconId` 登记表提供唯一码位来源，
  绘制直接走已有的 `ui::DrawGlyph`；不建 `HICON`、不建字体、不写第二套菜单渲染器。
  7 字段聚合初始化保持不变，禁用项图标跟随禁用色。
- **其余自绘右键菜单同步接入同一套图标**：面包屑段菜单、书签项菜单、书签空白菜单、
  状态栏菜单全部补上图标。新增的删除 / 撤销 / 新窗口 / 终端 / 排序 / 导入 / 导出
  语义码位仍只登记在 `icon_glyph.cpp`，业务代码只写 `IconId`。
  状态栏段图标抽成 `StatusSegmentIcon()` 供栏体与右键菜单共用，
  不再复制一份 `switch` —— 同一段不会出现两个不同图标。
- **视图与排序菜单也补上图标**：刷新按钮右侧的 `kView` 按钮（气泡提示「视图与排序」）
  弹出的菜单，8 档视图 / 4 种排序 / 4 种分组现在逐行带图标。
  档位图标直接挂在唯一表 `viewmode::kModes` 上，排序/分组按列语义取既有字形；
  没有为这个菜单另写一份档位到图标的映射。
- **新增统一菜单组件 `ui::Menu`，从根上收口"菜单显示 / 图标显示"**：所有自绘菜单的内容
  只由 `Add` / `AddChecked` / `AddPath` / `AddPathChecked` / `Separator` / `AppendLabel` 组装，
  `Show()` 统一交给既有 `ShowPopupList` 并返回业务 action（不是行号）。
  标签菜单、面包屑段菜单、历史/地址下拉、视图排序菜单、书签菜单、状态栏菜单、命令面板、
  书签导入模式选择全部迁移完毕；业务代码不再手写八字段 `PopupRow`，也不再各自拼 `✓ `、
  数初态行、管 `HICON` 生命周期。勾选前缀、分隔行、禁用态、空路径字形兜底只在一处实现。
- **路径图标统一走 `ico::Cached`**：`ui::Menu::AddPath` 与文件夹树 `TreeNode` 共用进程级
  `(路径, 尺寸)` 缓存，弹层只借不销毁；解析失败时两者都回退到 `ico::IconId` 字形。
  `ClearCache()` 在进程退出时统一释放。标签栏/书签栏/导航栏这些长生命周期 chrome 图标
  仍走 `ico::Resolve` 并各自管理 DPI 重取，不与弹层混用同一套所有权。
- **`tools/check_components.py` 新增两条机械判据**：业务文件里出现 `PopupRow{...}` 或直接
  调 `ShowPopupList(` 即失败；`ico::Resolve(` 只允许出现在图标入口与三类长生命周期 chrome 中，
  菜单/树必须走 `Cached`。这两条把"补一个菜单就又散一次"的回退路直接堵死。
- **`tests/nav_input_test.cpp`**（36 项断言）：UNC 分段、引号剥离、盘根/UNC 补全切分。
- **`tests/menu_component_test.cpp`**：验证统一菜单的行构建、勾选前缀、分隔行、禁用态、
  路径图标兜底与追加文案；`tests/popup_layout_test.cpp` 补树节点字形槽回归。

### 修复

- **历史/最近访问下拉不再显示裸 `::{CLSID}`**。此前 `nav::History` 与
  `Config::recentPaths` 只存解析串，而下拉直接把 `path` 当 `label`；Shell 的
  `SIGDN_NORMALDISPLAY` 对 `::{GUID}` 在本机返回空串，无法事后补救。新增
  `ico::DisplayName` / `ico::RememberDisplay` 作为全项目唯一的"解析串 → 显示名"入口：
  导航期的权威名先登记，其后依次用 CLSID 的 `LocalizedString`、注册表默认名兜底；
  后退/前进下拉、地址栏下拉、书签自动生成名、标签标题全部复用它，解析串本身仍作为
  导航目标原样保留。
- **导航到「此电脑」「回收站」时地址栏显示 `>::>`**。面包屑标签 `nav::CrumbLabel`
  用 `s[1] == L':'` 判断盘根并截取前两个字符；命名空间解析串 `::{GUID}` 的第二个
  字符恰好也是冒号，于是被误判成盘根、只剩 `::`，画出来就是「左折叠箭头 + `::` +
  分段箭头」。盘根判定改用 `nav::IsDriveRoot`（`C:` / `C:\` 才认），命名空间标签
  保持完整解析串；同时 `NavModel::Rebuild` 对命名空间单段改用 shell 友好名
  （与 `AddressText` 同一口径），地址栏和面包屑都显示「此电脑」「回收站」而不是裸 GUID。
  同类的 `ToUnc` 也已排除命名空间，避免「复制为 UNC 路径」产出 `\\localhost\:$...`。
- **激活标签不再出现焦点边框**：`WM_LBUTTONDOWN` 会把焦点留在标签栏（裸键 Shift+←/→、
  Home/End 靠它生效），于是每次点标签激活都会画一圈 XOR 点线框。焦点语义保留，
  视觉边框移除：`tab_strip_paint.cpp` 不再调 `DrawFocusRect`，
  `WM_SETFOCUS/WM_KILLFOCUS` 也不再为此触发重绘。
- **地址栏对命名空间回车是死的**。命名空间在地址栏里显示的是**友好名**
  （"此电脑"、"回收站"），用户回车交回来的也是它，而 `SHCreateItemFromParsingName("此电脑")`
  必然失败。新增 `ico::ParsingPath` 作为"友好名 → 解析串"的公共入口（幂等，
  认不出的名字原样返回，不吞掉失败原因）。
- **粘贴带引号的路径导航失败**。资源管理器「复制为路径」给的是 `"C:\Program Files"`，
  从聊天窗口复制的常带首尾空格/换行，直接丢给 shell 得到 `E_INVALIDARG` ——
  症状是"粘贴路径回车没反应"，与引号毫无关联。新增 `nav::FromUserText`：
  去首尾空白（含全角空格）、剥一层配对引号、正斜杠归一。只做不改语义的清理，
  不展开环境变量、不改大小写、不猜相对路径的基准。
- **后退/前进按钮恒亮**。`SetPath` 只更新了「向上」一颗（因为它能由路径推出来），
  后退/前进从来没被喂过状态 —— 没历史可退时按钮亮着、点了没反应。
  新增 `NavBar::SetNavState`，由 `SyncChrome` 喂入（任何导航完成/失败都会经过那里，
  视图内部发起的导航也能跟上）。"按钮灰显 == 点不动"这条口径现在三颗按钮一致。
- **面包屑把 UNC 路径塌成一段**。`NavModel::Rebuild` 只认 `X:\` 一种形状，
  `\\server\share\dir` 落进 else 分支整条变一格 —— 面包屑点不回 `\\server\share`，
  而同一路径的「上一级」按钮是好的（`ParentOf` 早就支持 UNC）。分段抽成纯函数
  `nav::Prefixes`（盘根/UNC 共享根/命名空间/相对路径四类各自成规则）。
- **地址栏 `Tab` 补全在盘根与 UNC 上失效**。补全用 `ParentOf + Leaf` 切分，而
  `ParentOf("D:\\")` 与 `ParentOf("\\\\server\\sh")` 都返回空 → 回退到"当前目录 + 整串前缀"
  → 一个候选都没有。这两处恰是用户最需要 Tab 的地方（换盘、敲共享名）。
  改为 `nav::SplitForComplete`：按**最后一个反斜杠**切。
- **地址栏输入文件路径回车报"无法打开"**。`IExplorerBrowser::BrowseToObject` 对文件项
  返回失败，于是 `C:\Windows\notepad.exe` 回车得到一句警告。资源管理器 / Opus / TC
  在同一处的语义都是"打开这个文件"。`NavigateActive` 现在先判目录：文件交给 shell
  打开，且**不压历史栈**（文件从来不是一个目录，后退不该退到刚打开的 exe）。
- **面包屑分隔符在空目录上"点了没反应"**。`ShowChildMenu` 在无子目录时直接 return，
  箭头不闪、列表不出、也不导航，看起来就是"这个三角是坏的"。现在确认是目录才直接进入
  （与书签栏 `ShowChildTree` 同口径），命名空间项不动作。
- **导航按钮在 DOWN 触发动作**。本工程其余控件（标签、书签、状态栏段）全部在 UP 执行，
  弹层"点别处只关菜单、不触发那个控件"也是靠这一点成立的 —— 导航按钮是唯一的例外，
  于是菜单开着时点它，会"关菜单 + 立刻导航"两件事一起发生。改为 UP 触发，
  并补 `SetCapture`（按下后拖出栏外再拖回，抬起仍能正确判定）与 `WM_CAPTURECHANGED` 复位。

**输入纪律立规（R21）：禁止模拟输入抢焦点，一切操作走结构化命令。**
这不是新功能，是**边界**：本程序是本机正在使用的窗口，任何"替用户按一下"的做法都会把
光标/焦点/前台从人手上夺走 —— 代价不在编译期、不在脚本日志里，而在用户的下一次点击上。
第一版规则只禁了 `SendInput` 一族，漏掉了"用 `PostMessage` 投合成点击"这条合法外衣，
本次复查一并堵上，并把唯一一个违规探针改成观察式。

### 变更

- **架构规则新增 R21「输入纪律」**（`docs/ARCHITECTURE-RULES.md` §3 + §3.1）：
  - 全仓禁止 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos` / `BlockInput` /
    `AttachThreadInput`；自动化脚本另外禁止 `SetForegroundWindow` / `SetActiveWindow` /
    `BringWindowToTop` / `SwitchToThisWindow`。
  - **同时禁止用 `PostMessage`/`SendMessage` 投输入类消息**（`WM_MOUSEMOVE` /
    `WM_LBUTTONDOWN` / `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK` / `WM_KEYDOWN` / `WM_CHAR` 等），
    以及"扫一行坐标、每隔几像素投一次点击"的循环扫描法。理由：合成点击在本程序的真实代码
    路径上**必然**命中控件 → `ShowPopupList` → 弹层内部 `SetForegroundWindow`，
    结果与 `SendInput` 完全相同。判据是"有没有让被测窗口走一遍用户点它的路径"，
    不是"有没有动光标"。这条口子是第一版规则漏掉的 —— 原规则把 `PostMessage` 写成合规做法，
    而那正是循环点击的合法外衣。
  - 允许的入口按优先级：进程内 `CommandBus::Invoke(id, args, actor)` → 进程外 AI 命名管道
    `tools/call` → 只读 `exbar/state` → 纯函数单测 → `scripts/shot.py` 截图 →
    **人工点、脚本只观察**（必须真点才成立的流程）→ `PostMessage` 投**非输入**消息
    （`WM_CLOSE` / `WM_THEMECHANGED` / `WM_APP_*`，仅验证 `WndProc` 分支）。
  - 写清了一条容易误伤的边界：`src/` 里的 `SetForegroundWindow`（`app_wnd.cpp` 单实例转发、
    `popup_list.cpp` 弹层取键盘输入、`hotkey_manager.cpp` 唤出、`main.cpp` 探活）是
    **用户触发**的产品行为，不受 R21 约束 —— 判据是"触发源是人还是脚本"，不是 API 名字。
  - 说明为什么暂不用 UIA 代替：chrome 是自绘、无原生控件，UIA 需要自己实现 provider；
    且命令总线已经覆盖"机器要做什么"，R21 正是说**不需要点像素**。
- **`docs/ARCHITECTURE.md` §12 不变量扩到 14 条**（新增第 14 条"不模拟输入、不抢焦点、不循环点击"）。
- **`CONTRIBUTING.md`** 开头的必读段落加入 R21 摘要（原来只提 R1–R20），
  验证清单加一条"没有为了让测试跑通而加模拟输入"，禁止事项表加三条
  （裸 API / 合成输入消息 / 第三方自动化库）。
- **`docs/DEVELOPMENT.md` §2.4** 从"不要用模拟鼠标点击来测试"的劝告，升级为
  规则 + 四类合规验证方式的对照表（纯函数单测 / 命令总线 / 截图 / 人工点+脚本观察）。
- **`docs/MIGRATION-PLAN.md` §8.2 与 `README.md` 验证行**：原记"真实键盘 `SendInput` + 窗口消息"
  冒烟，按 R21 改为"人工操作 + 只读证据"，并补一张"要看的项 → 只读手段"的对照表
  （`exbar/state` / 日志 / 截图 / 观察式探针）。冒烟清单本身不变 —— 它描述的一直是人做的动作。

### 新增

- **`tests/run_all.sh`：一键跑全部单测**（32 个，实际 32/32 通过，约 40 秒）。
  做法是**复用主构建的 `obj/*.obj`**（排除 `main.obj` / `*_test.obj` / `*.check.obj`）链接每个测试，
  而不是为每个测试手推一份 `.cpp` 依赖表：
  - 依赖关系交给主构建保证 → 不会出现"文档里的编译命令悄悄失效"；
  - 每条测试只花 ~3 秒链接，而不是把 60 个 `.cpp` 重编一遍的 3 分钟；
  - 工作目录自动判定：源码里出现 `L"../data/` 就去 `tests/` 跑，否则留在仓库根。
    判据取**字符串字面量**而不是注释 —— `commands_test` 头注释里写着"不是 `../data/...`"，
    按注释判会把目录判反（写脚本时真踩过，已记在 `DEVELOPMENT.md`）。
  - 这个脚本补上了一个长期空洞：文档一直说"tests/ 有 N 个单测"，但每条都要手抄一串 `.cpp`，
    抄错就链接失败，于是实际只跑记得住的几条。`keymap_test` 正是因此**红着很久没人发现**。
- **`tools/check_components.py` 增加 `INPUT_GUARD` 段（4 条规则）**，让 R21 成为机器判据
  而不是文档里的一句话。与前两类检查的区别是**扫描范围**：跨 `src/` `tools/` `scripts/`
  `tests/`（出事的多半是验收脚本，而它在 `src/` 之外）。
  - `模拟输入注入`（`scope="all"`）：`SendInput` / `mouse_event` / `keybd_event` /
    `SetCursorPos` / `BlockInput` / `AttachThreadInput`，`allow` 为空。
  - `合成输入消息`（`scope="all"`）：`PostMessage*` / `SendMessage*` 投鼠标键盘消息。
    正则同时抓"消息号写宏"与"消息号写字面量"两种写法；字面量只认
    `0x0100–0x0109`（键盘）与 `0x0200–0x020E`（鼠标）两段，不误伤 `WM_CLOSE=0x0010`。
  - `第三方自动化库`（`scope="all"`）：`pyautogui` / `pynput` / `AutoIt` 等 —— 它们底层就是
    `SendInput`，引入一行就绕过上面全部判据。要求带括号/`import` 才算，不误伤
    `CmdId::SendKeys`（那是命令枚举，不是调用）。
  - `自动化抢前台`（`scope="automation"`）：只看 `tools/scripts/tests`，`allow` 为空
    （产品代码里的前台调用是用户行为，不该被这条扫到）。
  - 三种绕过写法（字面量消息号 / 消息宏 / `pyautogui.click`）都用注入文件验证过确实会红，
    退出码 1 并报出文件名与次数，验证后已删除。
  - `--list` 输出每条规则的命中情况与作用域（`all` / `automation` / `放行`），便于核对口径。
  - 修了一处**规则自伤**：`strip_comments` 原来只剥 C 风格注释，而 R21 会扫 `tools/*.py`，
    于是检查器自己注释里写的反例（`PostMessageW(h, 0x0201)`）被自己判红 ——
    **文档里写"不要这样做"反而触发失败**。新增 `hash_comments` 参数，`py`/`ps1` 同时剥 `#`，
    `scan()` 与 `--list` 用同一口径。

### 修复

- **文档全量对账：所有规模数字重新实测，并修掉文档里跑不通的单测命令**（2026-11-21 09:05）。
  这一轮不改产品行为，只让"文档说的"和"实际是的"对上。
  - **规模数字全线更新**（`README` / `ARCHITECTURE` / `OPERATION-MODEL` / `ROADMAP` /
    `CONTRIBUTING` / `DEVELOPMENT` / `DECISIONS` / `INPUT` / `MIGRATION-PLAN`）：
    `src/` 18,486 → **22,065 行**、57 → **60 个 `.cpp`**、48 → **52 个 `.h`**；
    `tests/` 24 → **30 个**、464 → **444 项断言**；检查 15 → **16 组**；
    exe 19.4 → **20.2 MB**；`check_ai_meta` 报的 `aiExposed` 仍是 89（未变）。
  - **超 500 行文件清单从 7 个改为 10 个**（`CONTRIBUTING` §3.1 与 `ARCHITECTURE` §3 两处），
    按实测行数重排：`shell_ops` 1013 / `shell_view` 1003 / `popup_list` 948 /
    `bookmark_bar` 856 / `bookmarks` 706 / `commands` 565 / `app_commands_files` 557 /
    `shell_input` 521 / `input_router` 505 / `nav_bar` 501。原文的 7 个里
    `shell_view` 与 `shell_ops` 的先后顺序也是反的（`shell_ops` 已经超过 `shell_view`）。
  - **`docs/DEVELOPMENT.md` §2.4 的单测命令有 6 条照抄会链接失败**，逐条实测后重写：
    缺 `src/str.cpp`（`str::FromUtf8Strict`）、`src/log.cpp`（`LogDbg`）、
    `src/platform_win.cpp`（`win::ThemeControl` → 还要 `-luxtheme -ldwmapi`）、
    `src/cmd_naming.cpp`（`AliasOf`/`LegacyOf`）、`src/icon_glyph.cpp`（`ico::Glyph`）、
    `src/nav_history.cpp`（`History::Goto`）的情况全都不存在。现在提供一个 `FLAGS` / `LIBS`
    变量，十段命令一律实测退出码 0。另外补了一句"单测为什么比主构建脆"的原因
    （PCH 会把缺失的 `#include` 补齐，单测不会）。
  - **`tests/keymap_test.cpp` 硬编码 `hotkeys != 70` 就失败**，而实际早在 83 条 ——
    这条单测长期是红的（退出码 3），只是没人跑到。改为 `kExpectedHotkeys = 83` 常量
    并注释"加键位要同步改这里"，同时补上完整依赖链的编译命令（原注释只列 3 个 TU）。
  - **`docs/ARCHITECTURE.md` §3 模块表漏了 4 个已存在的模块**：`icon_resolve`
    （图标识别的唯一入口）、`popup_toggle`（再点即收状态）、`folder_tree`（子目录树唯一入口）、
    `bookmark_io` / `bookmark_drop`（导入导出与 OLE 拖放）。补行并写清各自"明确不做"什么。
  - **键位规模从"70 热键 + 38 序列"改为"83 + 38"**（`INPUT` / `ARCHITECTURE` / `MIGRATION-PLAN`），
    并在 `INPUT.md` 补一条容易踩的事实：**83 条里有 13 条指向 `implemented=false` 的命令**，
    按下无反应是预期行为（`check_commands.py` 会逐条列出）。
  - 文档 `baseline` 统一到 `2026-11-21 09:05`（`README` 现状表另行标注 09:55 快照）。
  - **易变数字不再写死**：`src/` 行数、超 500 行文件的具体行数这类"每天都在涨"的数字，
    改为**只记名单 + 给出可直接复制的度量命令**。理由是这轮整理本身证明了问题 ——
    整理过程中并行会话又新增了 `.cpp` / `.h`、命令表 140 → 152、单测 24 → 32，
    写进文档的 22,065 行在 50 分钟内就过期了三次。规则改为：**数字必须带日期与口径，
    且口径要能一条命令复现**；无法复现的数字不如不写。
  - `README` 现状表新增一条"命令数每加一条就变、以脚本首行为准"的提示。
  - 整理期间曾有一刻 `check_commands` / `check_ai_meta` 各报 1 项未通过（并行会话在加
    `bmk.add.path`/`bmk.move`/`bmk.list`/`TabLock`/`TabCopyTitle`，参数与处理器尚未补齐）。
    当时在文档里**如实标为"在途状态"而不是假装全绿**；随后并行会话补齐处理器，
    10:05 复核三者均通过，标注已撤回并更新为最新数字。
    这条记录留在这里，是因为"检查红着但在修"与"检查绿了"必须能被区分 —— 下次遇到同样情形照此处理。

- **导航栏所有下拉"点一下就一闪而过"**。点面包屑分隔符 / 折叠 ‹ / 地址框 ⌄ /
  后退前进箭头，弹层出现后立刻消失，肉眼只看到闪一下。

  **真因是松手那一下把自己关掉了。** 导航栏各下拉在 `WM_LBUTTONDOWN` 里弹出
  （与资源管理器一致：按下即出，可以按住一路拖到想要的项上再松手）。弹层一创建
  就 `SetCapture(list)`，于是**同一次点击的松开**也送到列表，而光标还停在锚点上
  —— 锚点在列表**之外**。旧逻辑把"UP 落在客户区外"一律当成"点了外面该收起"，
  菜单便在松手瞬间自己关掉。

  这一次 UP 根本不是新点击，它是**开菜单那次点击的后半截**。`TrackPopupMenu`
  也是先等这一下松开，之后才把点击当作"关闭"。

  修法：判据抽成纯函数 `PopupUpIsOpeningRelease(downInList, downOutside, upInside)` ——
  只有"开菜单之后再没按下过 + 松开在客户区外"才认定为收尾并忽略；一旦用户重新
  按过（无论内外），后续 UP 都是真实点击，照常判定。

  顺带补上右键（`WM_RBUTTON(DOWN|UP)`）：标签/书签/状态栏的右键菜单开着时，
  用户的下一手可能是右键点别处，原先这条消息漏给 ListBox 默认处理、行为不可控。
  现在右键落在菜单内**只关菜单不选行**（`TrackPopupMenu` 同语义）。

  验证（结构化，未使用鼠标模拟）：`tests/popup_layout_test.cpp` 新增 9 项 ——
  5 项逐条覆盖判据，4+1 项用状态机推演完整消息序列（开菜单松手 / 点菜单内 /
  点外面 / 拖出去取消 / 右键点外），断言"开菜单那下松手菜单不关"且
  "点别处仍能关"。全量构建 60 TU 通过，`check_components` 29 条全绿。

- **`tools/probe_toggle.py` 改为纯观察式（原为 R21 违规）**。旧版从 x=150 到 700 每隔 6px
  向主窗口投 `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP`，命中就建模态弹层、再点一次 ——
  这就是"模拟鼠标循环点击抢占焦点"：弹层内部 `SetForegroundWindow` 会把前台从用户手上拿走。
  现在脚本**一次也不投点击**：只轮询弹层窗口数量，点击由人做。判据反而更强了
  （原版只能测"脚本点开的那条路径"，现在测的是用户真实操作的那条），并且不需要坐标、
  不需要扫描、不依赖布局。
- **`tools/probe_reopen.py` 删除**（未入库的历史探针，同样在循环投合成点击）。
  它要断言的"关掉之后必须能立刻重开"已并入 `probe_toggle.py` 的最后一步（收起后再盯 1.5s
  不得重弹），不必单独存在。

**标签栏手感收尾 + 缺口按优先级补齐。**
围绕"标签一多就天天撞"的几处：双击标签关闭、`+` 回到标签右侧并固定打开此电脑、
拖拽边缘滚动、Esc 取消拖拽、关闭后焦点显式化。

### 新增

- **双击标签正文关闭**：双击标签标题区关闭该标签（Chrome/Edge/VS Code 惯例）。
  - 双击**关闭钮**仍什么都不做 —— 此前这里写 `NotifyClose` 造成"双击一次关两个"：
    UP 已关掉标签 A，DBLCLK 按新布局又命中接管它的标签 B 的关闭钮。
  - 锁定标签跳过（与中键关闭同口径），鼠标规则（`sink_`）= MT_TAB 双击优先，
    规则吞掉就不再走默认关闭。DBLCLK 后的尾随 UP 由 `suppressDblUp_` 吞掉默认选择：
    它若再 `NotifySelect(旧下标)`，会在 CLOSE 消息之后选中"顶上来"的标签。
- **新建按钮 `+` 紧跟最后一个标签**，不再钉在标签栏最右端（空栏贴左、铺满贴右）。
  - 溢出时 `+` 固定在可视右缘、`»` 贴栏最右，两者不再重叠；
    标签裁切线抽成成员 `tabViewRight_` 显式表达 —— 过去从 `ovfRc_.left-S(2)` 反推，
    `+` 一移到标签右侧该反推就把 `+` 区域误算成标签区，导致 `+` 点不到。
    `HitTab` / `Paint` / `EnsureVisible` / 溢出菜单可见性四处统一读它。
  - 纯函数 `TabNewButtonX(tabsRight,minX,maxX)` 收口夹逼规则，
    `tests/tab_strip_rules_test.cpp` 覆盖空栏/未铺满/溢出/滚出栏外/窄栏五种边界。
- **`+` 按钮固定打开此电脑（`nav::kThisPc`）**，不再是复制当前目录。
  - 复制当前目录是"复制标签"（`tab.clone` / 右键"复制标签"）的职责，两条语义分开。
  - 右键菜单"新建标签页"、`tab.new` 无参默认值同步为此电脑（R20 人机等价）。
    此前"新建标签页"和"复制标签"两项实现完全相同（都复制当前路径），名不副实。
- **拖拽到可视区边缘自动滚动**（溢出时）：拖到左/右边缘带内触发 30ms 定时器持续滚动，
  让标签能拖到视野外；离开边缘即停。落点计算 `DropTargetAt` 抽成一份，
  MOUSEMOVE 与定时器共用同一口径，避免"滚一下插入线就跳到别处"。
  定时器在松手/Esc/捕获丢失三条路径上都停（`CancelDrag` 统一收口），不会残留。
- **Esc 取消拖拽**：拖到一半反悔，Esc 复位拖拽态并释放捕获，松手不会真的移位。
  `CancelDrag` 同时服务 WM_CAPTURECHANGED 与 Esc，两处状态清理不再各写一份。

### 修复

- **单击 `+` 没反应、双击才开此电脑**：根因是标签栏 DOWN 只报 `empty/tab/tab_close`，
  UP 才报 `new_tab/overflow`。点在 `+` 上时 DOWN 命中内置"双击空白 -> 上一级"规则并被
  `InputRouter` claim，UP 却被当成另一个目标然后被吞；双击时第一次 UP 没生效，
  最后一次 UP 又真的执行了默认新建。现在三处消息共用 `MouseTargetAt`/`TabMouseTarget`
  同一口径（关闭钮 > 正文 > `+` > `»` > 空白），并给 `InputRouter::OnMouseUp` 加了
  跨目标隔离：已知目标不一致时不再拿 DOWN 的 claim 吞掉 UP 所在控件的默认行为
  （Shell 的 `MT_UNKNOWN` 仍按沿用 pending 处理）。新增
  `tests/input_router_claim_test.cpp` 钉住这四个分支。
- **双击标签后不再误选"顶上来"的标签**：双击消息序列是 DOWN → UP → DBLCLK → UP。
  DBLCLK 已 `PostMessage(WM_APP_TAB_CLOSE)`，尾随 UP 再 `NotifySelect(旧下标)` 时，
  旧下标在 CLOSE 处理完后已经指向别的标签。现在 DBLCLK 置 `suppressDblUp_`，
  尾随 UP 只交给规则引擎清状态、不再跑默认选择；下一次 DOWN 会清掉该标记，
  即使 UP 落到窗口外也不会影响下一次点击。
- **移除"双击标签栏空白 = 新建标签"**：该隐式默认与 `+` 按钮语义重复、无提示，
  正是"双击变成打开我的电脑"的另一来源。空白双击现在只走鼠标规则
  （默认配置是"上一级"），不再偷偷建标签。
- **关闭活动标签后焦点显式落回新视图**：过去依赖"销毁视图后 `GetFocus()` 返回 null、
  `Activate` 恰好走 `!focus` 分支搬回焦点"的侥幸。现在关闭**前**就记录焦点是否在被关
  视图里，关掉后显式 `FocusActiveView()` —— 不依赖销毁次序，换关闭入口也不落空。

**命名与标识收编 + 已建入口接线。**
这一轮不改行为，只做两件事：① 独立后遗留的旧命名（品牌串三套、`eb_` 来源前缀、
`QuitExplorer` 语义错位）收口；② 上一轮建好却没人调用的入口（`MeasureText` /
`CreateChild` / `CreatePopup` / `ThemeControl`）接线，并删掉 4 个零调用死入口。

### 新增

- **标签右键菜单补齐 7 项**：重命名标签页、设置标签页颜色、向左/向右移动、
  移动到开头/末尾、关闭重复标签页。分三组插进原有菜单（关闭族 / 编辑族 / 移动族），
  边界项与"无重复可关"一律**灰显**而不是点了没反应（P4）。
  - 改名存 `TabRef::alias`，**不覆盖** `title`。`OnTabNav` 每次导航都会 `SetTitle(Shell 名)`，
    直接改 `title` 的表现是"改完进一次目录就变回去了"。新增 `core::DisplayTitle`
    作为唯一口径（改名 > Shell 名 > 路径末级），标签栏与窗口标题都读它 ——
    此前两处各写一遍 `display.empty() ? LeafOf(p) : display`，
    改名的标签会出现"标签栏新名、标题栏旧名"。
  - 颜色存**下标**（`TabRef::color`，0 = 无颜色）而不是 `COLORREF`：`core/` 零 Win32，
    且颜色必须与深浅主题配套换算。色板 7 档 + "无颜色"固定在 `Theme::TabColorFill`
    （深色 26% 混底、浅色 18%，两套色值 —— 同一份 RGB 在浅底上过重、深底上过暗）。
    激活标签画通栏色条（无颜色时退回主题 accent，保持原观感），
    非激活但着色的标签画内缩细色条：26% 的底色在深色主题下太弱，只靠底色分不出组。
  - 改名/颜色**按路径持久化**（`Settings::tabMeta`，与 `lockedPaths` 同一取舍）。
    不按标签序号（每次启动重排必然错位）、不按标签 id（进程内自增，跨会话无意义）。
    标签导航到别的目录时同步换成新路径的标记 —— 名字说的是那个目录，不是这个标签。
  - 关闭重复按路径**大小写不敏感**（`core::TabSet::DuplicatesOf`），锁定标签跳过并计数；
    一个也没关掉时 `tab.close.duplicates` 返回 false（否则审计记成成功、AI 以为清理过了）。
  - 三项同时登记为命令（`tab.rename` / `tab.color` / `tab.close.duplicates`，均 `aiExposed`）：
    右键菜单里的每个动作都必须有键盘/面板可达路径（R20 人机等价），
    否则只有鼠标能做。命令与菜单共用同一份实现，避免"菜单改名能存、面板改名存不住"。
- **`tests/tab_set_test.cpp`**：改名不覆盖 Shell 名、导航后改名仍生效、
  颜色越界夹回 0、`DuplicatesOf` 大小写不敏感且不含自己。
- **书签左侧图标 → 子目录下拉**。点书签项的图标格不再是"打开书签"，而是列出该路径的
  **直接子目录**，可就地逐级展开 —— 与"点地址栏分隔符（面包屑 chevron）"完全同一效果。
  两者此前是两套代码：枚举逻辑藏在 `nav_bar_actions.cpp` 的匿名命名空间里（`ChildTreeNodes`），
  书签栏要用只能复制一份。现在抽出 `src/folder_tree.{h,cpp}` 作为唯一入口
  （`Children` 建树 / `Show` 弹层，懒加载回调就是 `Children` 自己），
  `NavModel::ChildFolders` 仍是唯一的 shell 枚举实现，
  地址栏的两处调用点（面包屑 chevron、`‹` 折叠钮）一并改调它。
  - `BookmarkBar::Item` 新增 `iconRc`，与 `Paint` 里画图标的槽严格重合；`Handle` 的
    `WM_LBUTTONUP` 在调用默认行为前先判 `HitIcon`。
  - 图标格按下不预备拖动重排（`dragArm_` 排除图标区），否则在图标上稍一移动就变成重排书签。
  - 悬停图标格时该格单独画 `pressed` 圆角底（`hoverIcon_`），让"这一格能点开子目录"可见。
  - 纯分类节点（只装子书签、无路径）点图标退回"列分类内容"；空目录则直接进入该目录
    （与 mod 的 `ShowFsMenuForPath` 同口径 —— 弹空菜单等于点了没反应）。
- **`src/identity.h`**：品牌串、11 个窗口类名、单实例互斥体、命名管道、
  日志文件名、AI 服务名、环境变量、`WM_COPYDATA` 魔数的唯一来源。
  此前这些字面量散在 9 个文件里，改名要动 9 处。
- **`docs/NAMING.md`**：命名与标识规范。含"什么必须改、什么必须留"的处置表
  （跨软件契约 `EXBAR_*` 书签属性、数据文件名永不改）、命名空间按领域的规则、
  命令点分名与 legacy 名三条硬规则（稳定 id / 旧名永不删 / 消费方全覆盖），
  以及 9 个业界项目的比对依据（Blender 强制点分、VS Code legacy 数组、
  Qt 框架前缀归属、Opus 粗粒度命令、TC 前缀、Far 路由前缀、Chromium 三分载体、
  Tauri 窗口类名教训、Emacs Lisp 双连字符）。
- `tools/check_components.py` 从 15 条扩到 **24 条**（21 唯一性 + 2 禁令 + 1 接线）：
  新增品牌串（正式名/显示名/窗口类名）三条唯一性（`docs/NAMING.md` B1）、`MeasureText`、
  `SetWindowTheme`、`CurrentMods` 各一条，另新增 **`STALE` 段**——"入口已建但零调用"的检查
  （这类问题查重抓不到、编译不报错）。`STALE` 分三态，只有"`home` 内也无人调用"才判失败；
  "只有 `home` 内自用"降级为**提示**（该私有化，但代码是活的，不该让 CI 红）。
  据此把 `GetUiFont` / `GlyphFace` 收进 `ui::detail`（公开等于开了"绕过 DrawGlyph 自己建字体"的口子），
  `IconIdFromName` 登记为**预留**（数据驱动处按名字取图标，当前 keymap 不含图标名）。

### 变更

- **标签栏跨项目对标修正**（Chromium 标签栏设计文档 / VS Code Tabs / Mozilla 键盘无障碍 /
  Windows Terminal 主题设置）。28 项功能清单已补齐，这批修的是交互质量：
  - **快速连续关闭不再点空**。Chromium 把"关闭后下一个关闭钮留在鼠标下"列为明确需求；
    原实现关掉一个标签后按内容宽/均分/溢出重新分配，右侧标签整体左移，连点第二下必空。
    `TabStrip` 现在保存关闭前的实际宽度表，关闭后让接管位置的标签临时用**被关标签的宽度**，
    鼠标离开标签栏（`WM_MOUSELEAVE` → `CancelRapidClose`）才恢复。三种宽度模式下均实测对齐。
  - **DPI 切换重建图标缓存**。`ico::Resolve` 按 `SM_CXSMICON` 取图（实测 96dpi=16px、
    120dpi=20px），而 `ReloadTheme` 以前只重建 tooltip 和重算颜色，标签图标停留在旧 DPI
    尺寸 —— 与刚统一的图标尺寸令牌再次脱节。现在整批丢弃按新 DPI 重取。
  - **标签栏键盘焦点可见**。`SetFocus(h)` 后没有任何焦点绘制，Shift+←/→、Home/End 这些
    "标签栏作用域"的裸键用户看不到焦点在哪。`WM_SETFOCUS/KILLFOCUS` 现在触发重绘，
    `Paint` 在最后用 `DrawFocusRect` 画激活标签的点线框。
  - **锁定标签批量关闭有反馈**。Close Others/Left/Right 以前逐个调 `CloseTab`，每留一个
    锁定标签就响一次铃，命令仍返回 ok=true。`App::CloseOthers/CloseSide` 改为先收集再关
    （避免边遍历边 erase 导致迭代器失效）、过滤锁定项、只响一次铃并记日志，返回**实际关闭数**；
    全被锁定时命令如实报失败。对应菜单项在不可执行时灰掉（"重新打开"在历史为空时也灰掉）。
  - **Shift+F10 / VK_APPS 可打开标签右键菜单**（`WM_CONTEXTMENU`），落在激活标签上。
    此前菜单只有 `WM_RBUTTONUP` 一条鼠标路径，键盘用户够不到那 22 项。
  - **tooltip 不再随滚动全量重建**。新增 `ui::TooltipHost::MoveTools`，滚动时只发
    `TTM_NEWTOOLRECT` 更新热区矩形；全量 `SetTools` 只在标签集/DPI/窗口宽度变化时调用。
- **标签栏的关闭 ✕ 与新建 ＋ 改为字形，尺寸对齐其余栏图标**。
  这两个图标此前是全仓仅存的手绘折线（`tab_strip_paint.cpp` 的 `DrawCross` / `DrawPlus`），
  尺寸由两套互不相干的手算公式给出：✕ 跨度 = 框宽 − `thickness*2*2` = **12dp**，
  ＋ 跨度 = `(plusW/2 − 4) * 2` = **21dp**。21dp 落在 26dp 高的栏里几乎顶满上下边缘，
  同为"小图标"的锁（9×11）与溢出箭头（9×5）则小一圈 —— 三个图标挤在同一行，
  比例却来自三个不同的算式。
  改成 `ui::DrawGlyph` + 字形表：新增 `ico::IconId::Add`（`0xE710`），复用已登记但零调用的
  `ico::IconId::Close`（`0xE711`），两者统一 `kGlyphDp = 13`（实测墨迹 ✕ 10×10 / ＋ 11×11，
  与锁 `S(11)` 的 9×11 同一量级）。原注释"10px 字形在小尺寸下发虚"不成立 ——
  紧邻的锁与溢出箭头就是同尺寸字形。
  `tools/check_components.py` 同步收紧：`MoveToEx` / `CreatePen` / "绕过 DrawChevron 自绘箭头"
  三条规则的 `tab_strip_paint.cpp` 白名单全部移除，折线从此没有豁免位。

- **图标尺寸收成 `Theme` 令牌：4 档，取代散在 6 个文件里的 7 个裸数字**。
  上一轮把 ✕/＋ 改走字形后暴露了更根本的问题：`icon_glyph` 管"哪个码位"、`DrawGlyph`
  管"怎么画"，**唯独"画多大"没有归属** —— 于是 `S(9)`（书签栏分类箭头）、`S(10)`
  （面包屑/弹层 chevron）、`S(11)`（标签栏锁/溢出/书签栏 More）、`S(12)`（状态栏段图标）、
  `S(14)`（导航栏 `kIcon96`）、`S(16)`（标签栏/书签栏/弹层条目图标）各自为政，
  同一行里的三个图标可能来自三个不同的数字。`Theme` 此前有 `radiusBtn96` /
  `radiusPopup96`，却没有一个图标尺寸令牌。
  现在按**角色**分四档（不再按"哪个栏想画多大"）：

  | 令牌 | 96 DPI | 用途 |
  |---|---|---|
  | `iconChevron96` | 10 | 面包屑每级 / 书签栏分类 / 弹层展开器 / 导航栏分裂箭头与字段下拉箭头 |
  | `iconGlyph96` | 12 | 标签栏 ✕ ＋ 溢出 锁定 / 书签栏溢出 / 状态栏段图标 |
  | `iconCompact96` | 14 | 导航按钮、面包屑前导文件夹 |
  | `iconSmall96` | 16 | 标签栏 / 书签栏 / 弹层行的条目图标 |

  下限（8 物理像素）仍只在 `ui::DrawGlyph` 内部夹一次 —— 令牌 accessor 不再重复夹，
  两个下限就是两套口径，正是本工程删掉 `FontPx`/`Scale`/`IconSize` 的理由。
- **图标绘制原语从三个收成一个规则**。此前 `DrawIconCentered` / `DrawIconVCentered`
  按调用方给的 `size` **拉伸**，`DrawIconFit` 却按 `IconPx(icon)` **真实像素**，于是
  同一枚图标画出来的尺寸取决于走了哪个函数：120 DPI 下源图标 20px，导航栏 14dp 槽
  （18px）被压成 18px，弹层 16dp 槽保持 20px。现在 `DrawIconFit` 并入
  `DrawIconCentered` / `DrawIconVCentered`，统一规则是**落笔尺寸 = 图标真实像素，
  槽宽只作上限不放大**，槽比图标小（真放不下）时才缩到槽宽。
  顺带纠正一条错论据：实测硬边合成图标 20px→18px 时中间 alpha 为 **0** 个 ——
  `DrawIconEx` 缩放图标不做插值，"发虚/糊边"不成立；真问题是**尺寸不一致**。
- **导航栏分裂箭头在 96%~168% DPI 下完全不随 DPI 变化**（真 bug）。
  它调的是 `ui::DrawChevron(..., theme_->S(4), ...)`，而 `DrawGlyph` 有 `if (px < 8) px = 8`
  的下限 —— 于是按钮从 24px 长到 42px，箭头恒为 8px。实测：DPI 96/120/144 下
  `S(4)` 分别算出 4/5/6px，全部被夹成 8px。改走 `iconChevron()`（10px > 下限），
  "传进去的值"与"画出来的值"不再有落差。
- **面包屑分隔符的第二套 DPI 公式**。`nav_bar_draw.cpp` 的 `DrawSep` 用
  `MulDiv(12, 区高, 24)` 按区域高度反推字号，而它的注释写着"按 DPI 字号取" ——
  代码与注释各说一套，结果是同一套 chevron 在面包屑里随栏高漂移、在标签栏/弹层里固定。
  改为 `th.iconChevron()`，三处同源。
- `src/nav_metrics.h` 删除 `kIcon96 = 14`：零引用，且是"图标尺寸有两个来源"的实证。
  该文件现在只放导航栏**几何**（按钮框 24、分裂箭头宽 12、chevron 区 20…），
  图标大小一律归 `Theme`。
- `src/nav_bar.h` 文件头注释修正：原文写"按钮 28px、图标 16px"，而代码取的是
  `kBtn96=24` / `kIcon96=14`。注释与代码各说一套，是"图标大小莫名其妙变了"时最难查的
  那种不一致。
- `tools/check_components.py` 增加两类机械校验（现共 23 唯一性 + 3 禁令 + 1 接线）：
  ① 禁令「图标尺寸绕过尺寸令牌」—— 直接把 `S(数字)` 交给图标/字形原语即失败；
  ② 尺寸令牌三段检查 —— `icon*96` 字段必须被 accessor 读、accessor 必须在业务代码里
  有调用点。两条都用注入违规的方式验证过确实会红（不是白名单式的形同虚设）。


  | 原 | 现 | 说明 |
  |---|---|---|
  | `eb_ui.{h,cpp}` + `namespace eb` | `ui_draw.{h,cpp}` + `namespace ui` | 16 个绘制/量字/图标原语 |
  | `eb_theme.h` | `ui_palette.h`（`UiPalette`） | 色值令牌是数据，绘制原语是动作，两者分开 |
  | `eb::TooltipHost` | `ui::TooltipHost`（`tooltip_host.h`） | 同属 UI 原语 |
  | `eb::CurrentMods` | `mouse_rules.h` 的全局 `CurrentMods()` | 读 `GetKeyState`，属输入端（R15） |

- **`WM_COPYDATA` 魔数改回 `0xEBB1`**（撤销本轮早先的换值）。原打算换成 `0x45584242`，
  理由只是"`EB`+`B1` 像 mod 期的玩笑"。但窗口类名 `ExBarBrowser_Main` 没变 —— 升级后若旧
  进程还在跑，新进程会找到它、用新魔数发消息、被静默拒绝，用户看到"已有一个 ExBar 窗口
  未响应"，而那个进程其实好好的。独立免除了"与 mod 兼容"的义务，但**同一程序的前后版本
  之间仍要兼容**，这是当时漏掉的一层。换值只买到审美收益，代价是一次真实断裂，故撤销。

  没有按原计划拆成 `Painter` / `Assets` / `Metrics` 三个类：本工程 16 个函数、约 200 行，
  拆三个命名空间只是多一层跳转。**判断依据是规模，不是教条** —— 等真长到需要分组再拆。
  全仓已无 `namespace eb` / `eb::` 残留，由 `check_components.py` 的 `N1/N2` 口径继续守着。
- **`CurrentMods` 从 UI 层移到输入端**（`eb::CurrentMods` → `mouse_rules.h` 的全局函数）。
  它读 `GetKeyState`，是**输入状态**而不是绘制资源；以前放在 `eb_ui`（绘制原语命名空间）里，
  于是每个栏在自己的 `WM_MOUSEMOVE` 里调 UI 层的函数去问输入状态，分层方向是反的
  （`ARCHITECTURE-RULES.md` R15）。顺带补上 **AltGr 特判**：右 Alt + 左 Ctrl 在德语/波兰语等
  布局下是字符输入修饰（`AltGr+E` = 「ę」），不能被当成 `Ctrl+Alt+E` 吃掉 —— 以前没有这个判断，
  非美式键盘上 `Ctrl+Alt+E`（切换扩展名）会在输入字符时误触发。
  `input_router` 的成员版本因需按"当前聚焦控件"决定是否抑制，保留自身一份。
- **`QuitExplorer` 主名改为 `win.quit`**。它的实际行为是"退出本程序"，而 mod 期
  这个名字是"重启 explorer.exe"。枚举名保留（`CmdId` 值一经分配不改），
  旧名作为 legacy 名仍可解析；`keymap.json` / `keymap.gen.h` / 校验器白名单同步。
- **`FindCommand` 改为三路解析**：精确名 → 旧名转点分名 → 点分名转旧名。
  此前是"先精确、再试点分"两段式，要求调用方知道自己手里是哪种名字；
  迁移期间三种组合都会出现，必须对任何输入都试三条路。
- `win::ThemeControl` 签名改为 `subApp` 后缀（`"Explorer"` / `"CFD"`），
  深浅两套 SubAppName 由它统一拼装，不再让调用方写三元表达式。
- `win::CreateChild` 参数 `h96` 改名 `heightPx`（它接收的是已缩放像素），
  并开放 `style` 参数（`StatusBar` / `BookmarkBar` 不需要 `WS_CLIPCHILDREN`）。

### 修复

- **标签栏：溢出时 » 按钮点不动**。判据是 `TabHit` 只跳过"起点在裁切区右侧"的标签
  （`left >= clipRight`），于是**跨过**裁切线的那一个标签在 » 按钮下面仍然命中 ——
  点 » 的结果是切到被遮住的标签，溢出菜单永远打不开。标签刚好多到溢出时，
  可见区的最后一个标签几乎必然跨界，不是理论边界。
  改为按**点**判（`TabHitInView`：横坐标落在裁切线上即不属于任何标签）。

- **标签栏：拖拽排序实际失效**。`WM_LBUTTONUP` 里先 `ReleaseCapture()` 再读
  `dragIdx_/dropIdx_`，而 `ReleaseCapture` 会**同步**派发 `WM_CAPTURECHANGED`；
  新加的复位分支把两个下标清成 -1，于是读到的永远是无拖拽 —— 拖了没反应、也不报错。
  `BookmarkBar` 早就在同一处写了"先提交再释放"的注释，标签栏一直漏着。
  改为先取快照、清状态，再释放捕获、投递移动。

- **标签栏：捕获被抢走后拖拽状态残留**。标签栏缺 `WM_CAPTURECHANGED` 处理：
  拖到一半右键弹菜单 / Alt+Tab 切走 → 收不到 `LBUTTONUP` → `dragging_` 一直是真，
  下一次鼠标移进来（哪怕没按键）标签就跟着光标乱跳。补上复位分支
  （`closeDownIdx_` 一并复位，否则残留索引会让下一次松开误关别的标签）。

- **标签栏：双击关闭钮关掉两个标签**。一次双击的消息序列是 DOWN → UP → DBLCLK，
  UP 已经关掉了标签 A，标签左移；DBLCLK 按**新布局**命中，而快速关闭模式刻意
  保住了关闭钮的位置——于是第二下又关掉标签 B，用户只双击了一次却少了两个标签。
  改为双击落在关闭钮上**不做任何事**（关闭钮不是可双击的控件）。

- **弹层高度未夹进工作区**。`RunModal` 先按原始高度决定向上/向下展开，再把 `y`
  推回 `rcWork.top` —— 于是 `top + height` 越过工作区底边，最后几行连同滚动条一起
  画到屏幕外，既点不到也滚不到。标签右键菜单 22 行 × 24dp，125% DPI 下 660px，
  768 高的屏去掉任务栏就顶到边。改为**先夹高度**再算位置。

- **标签栏：颜色档数有三份真相**。`Theme::kTabColorCount` 与 `tab_set.cpp` /
  `config.cpp` 两处硬编码的 `> 7` 并存，一旦色板扩容就会漏改 ——
  表现是"选了第 8 档，重启后变回无颜色"，且没有任何报错。
  档数收成 `core::kTabColorCount`（唯一定义，校验层也读它），`Theme` 转发。

### 变更

- **标签栏拖拽改为画插入竖线，不再整格刷悬停底色**。整格高亮读起来是"这个标签被
  选中了"，而拖拽时用户真正要看的是**插到哪一侧**。改由 `TabDropLineX`
  （纯函数，已单测）按拖动方向决定落点：往右拖画目标格右边，往左拖画左边，
  原地不画。Chrome / Edge / VS Code / Opus 一律用竖线，就是这个原因。

- **标签溢出菜单只列当前看不见的标签**。原来列全部 —— 点 » 的动机是"我看不到的
  那个在哪"，把看得见的也塞进来等于每次让用户在 15 行里找 3 个。部分可见
  （被 » 切掉一半）也算隐藏；激活标签打 ✓（溢出时最常问的第二件事是"我在第几个"）。

- **补 `Ctrl+1..8` / `Ctrl+9` 与 `Ctrl+PageUp/PageDown`、`Ctrl+Shift+PageUp/PageDown`**。
  `Ctrl+1..9` 在 ROADMAP 里列了很久但一直没绑（`g 1..9` 序列不是同一件事：
  序列要先想起前缀键）。移动标签用资源管理器/浏览器的惯例键，且带 Ctrl
  不受"标签栏"作用域限制——原来只有裸 `Shift+←/→`，必须先把焦点移到标签栏才能用。

- **下拉菜单"再点一次收不掉，反而重新弹出"**。地址栏分隔符最明显：点一下箭头变
  「⌄」并弹出子目录列表，再点一下箭头变回「›」——但列表又弹出来了，用户根本关不掉。

  **真因是关闭时机，不是"连点判据"。** 系统在派发点击之前**同步**给弹层发
  `WM_ACTIVATE(WA_INACTIVE)`；旧代码在此立刻 `PostMessage(WM_CLOSE)`，这条消息
  排在 `LBUTTONDOWN/UP` 之前被模态循环取走 → 弹层先销毁、捕获释放 → 剩下的
  `LBUTTONUP` 落到锚点控件，而各栏都在 **UP** 里执行动作 → 又调一次 `Show*`
  → 菜单"关了又开"。

  **关键事实：鼠标捕获不阻止激活切换。** 弹层是 `WS_POPUP` 顶层窗口，点主窗任一
  子控件都会让弹层失活，哪怕捕获仍在手里。此前两轮修法（400 ms 时间窗、关闭后留
  记录）都建立在"持捕获就安全"这个不成立的前提上，因此必然失效 —— 何况用户是
  "看一眼再点"，间隔 1~2 秒，任何毫秒级窗口都过期。

  修法对齐 `TrackPopupMenu` / 组合框下拉：失活时按 `popup::ShouldCloseOnDeactivate` 分流 ——
  **持捕获 + 前景窗同线程**（点的是本程序另一个窗口）→ 不关，这组 DOWN/UP 由
  `ListSubclass` 吞着，等 UP 判到"点在列表外"再关，锚点收不到点击、重开在消息层
  不可能发生；**其余**（点别的程序 / Alt+Tab / 捕获被抢走）→ 关，否则就是关不掉的菜单。
  配套：`WM_CAPTURECHANGED` 区分"LISTBOX 自己 SetCapture"（忽略）与"被抢走"（关）；
  外点的 DOWN 只记 `downOutside`，等 **UP** 才关并释放捕获 —— 少吞 UP 会顺带打开
  那个书签、切走那个标签。

  **所有弹层统一遵循**（`PopupProc` 是唯一实现），不只面包屑：后退/前进历史、
  地址框 ⌄、视图菜单、面包屑折叠 ‹、书签（图标树 / 分类 / 溢出 / 右键 / 空白右键）、
  标签右键与颜色二级、标签溢出、状态栏菜单、命令面板。锚点键带索引
  （`nav.crumb.3`、`nav.drop`、`bmk-icon:<id>`、`tab.menu.1`…），书签与标签用稳定
  id 而非下标（重排后下标会漂）。

  `popup_toggle` 现在只回答"某锚点的弹层是否开着"，供 `Show*` 兜底（键盘/命令重入）
  与面包屑箭头形态（› / ⌄）使用，不再有时间窗。

  验证：`tests/popup_toggle_test.cpp` 21 项（含一条枚举四种组合、断言"只有
  持捕获+同线程 不关"，防条件写反）；`popup_layout_test` 回归全绿；`check_components`
  29 条全过。进程外实测（真实鼠标点击）：点开 → 等 1.5 s（远超旧 400 ms 窗口）
  → 点同一处 → 弹层数归零（收起且不重弹）；点外部关掉后再点锚点 → 能重新打开
  （没有过度抑制）。

- **`win::CreatePopup` 的 this 指针设置时机晚于 `WM_NCCREATE`**。原实现在
  `CreateWindowExW` 返回后才 `SetWindowLongPtrW(GWLP_USERDATA)`，而弹层/对话框的
  窗口过程都在 `WM_NCCREATE` 里读 `CREATESTRUCTW::lpCreateParams`。改为直接用
  `lpCreateParams` 传递，并保留零值兜底。
- `log.cpp` 靠 `common.h` 间接取到 `identity.h`，单测独立编译时报
  "use of undeclared identifier 'id'"。改为显式包含。
- `tools/check_commands.py` 的三个盲点：
  ① 主名正则 `L"(\w+)"` 不认点分名，`win.quit` 那行被静默跳过（137 条变 136）；
  ② `implemented` 用主名比对处理器（处理器按 `CmdId` 枚举注册），改名后误报
  "已实现但没处理器"；③ 不认 `cmd_naming` 表里的旧名，用户已有 keymap 会被判失效。
- `tools/check_ai_meta.py` 第 11 项的两个缺陷（与 `check_commands.py` 同源）：
  ① 命令名正则 `L"(\w+)"` 不认点分名，`win.quit` 被整行跳过，于是误报
  "别名 QuitExplorer -> win.quit：源命令不存在"——**那恰恰是迁移完成的标志**；
  ② 覆盖率只按别名表左列（旧名）统计，迁移后的命令会被算成"尚无显式别名"
  （显示 1/137）。改为按两个方向取并集。修完输出 `137 条别名，全部有显式别名`。
- `tests/naming_test.cpp` 同类误判：点分主名被当成"缺别名"。改为
  "点分主名 + 旧名可解析 = 通过"。

### 移除

- `eb::FontPx` / `eb::Scale` / `eb::IconSize`（删除时的名字）：三个零调用的 DPI 换算入口。
  真实缩放一律走 `Theme::S()`；一个概念两个公式是"两套观感"的成因。
  其中 `FontPx` 还把枚举值当数字直接乘（`enum FontRole { FR_UI = 12 }`），
  枚举名义上是"字号角色"，实际只承载一个 int 槽位，而建字体的
  `GetUiFont` 又只收尺寸+字符串——角色在传递链上丢失，只能靠字体名反推。
- `eb::IconGlyph`（删除时的名字）：字形表已由 `ico::Glyph` 统一，这个按名字取的中间层零调用
  （`DrawChevron` 改为直接查 `ico::IconId`，不再经名字字符串绕一圈）。
- `eb::CurrentMods`：不是删除，是**搬家**（移到 `mouse_rules.h` 的 `ui` 之外），见"变更"首条。

### 文档

- 全部文档基线重核到 **2026-11-20 19:40**，并按代码实测修正数字：
  `src/` 18,464 行 / 57 `.cpp` + 48 `.h`；全量构建 10.5s（上一版误记为 29.8s，
  那是冷 PCH 一次性开销，PCH 命中后是 2.2s）；`check_components` 规则数 23。
- `docs/ARCHITECTURE.md` §2.1 重写窗口类注册策略（不再有"注册 `DefWindowProcW` 再替换"的
  过时描述，只剩 `EDIT` 子类化一处例外）；§3 模块表补 `identity.h` / `eb_ui` / `platform_win`；
  §12 不变量从 10 条扩到 13 条（新增 3 条纯机械约束）；§13 补 `ebop` 与 `ebai` 的对应关系。
- `docs/DECISIONS.md` 补 **D8**（从主仓独立为自包含仓库）与 **D9**（品牌串收编到 `identity.h`）；
  D4 补魔数换值；D7 标注"复检条件已触发但决策维持"及原因（不是引 CMake，而是把清单改成自动扫描）。
- `docs/NAMING.md` 实施状态表按实际补全（`MeasureText`/`ThemeControl`/`CreateChild`/`CreatePopup`
  接线、`QuitExplorer` → `win.quit`、三个校验器改动），并如实记录 1/137 的迁移进度。
- `docs/DEVELOPMENT.md` 补"新增头文件必须自足"（PCH 会掩盖缺失的 `#include`，
  单测单独编译才暴露 —— `log.cpp` 就是这样被发现的）；发布前检查补静态检查与基线更新两项。

### 接线（建好但此前无人调用）

- `eb::MeasureText` → 8 处裸调 `GetTextExtentPoint32W` 全部改用，
  顺带删掉 6 处冗余的 `SelectObject` / 还原样板（每处都各自维护一个 `old`）。
- `win::CreateChild` → 4 个栏的 `CreateWindowExW` + `SetThis` 两行变一行。
- `win::CreatePopup` → 弹层 / 对话框 / 序列提示共 4 处。
- `win::ThemeControl` → `nav_bar`（`CFD`）、`popup_list`（`Explorer`）共 3 处。

### 第二轮：命名收尾 + 校验器补硬约束

**① 命令主名全部迁到点分名（137/137）。**
`commands.cpp` 的 `d.name` 从旧驼峰名改成点分名（`TabCloseOther` → `tab.close.others`），
`Cmd` 前缀随之自然消失（零残留）。`legacyName` 改为 `cmdnaming::LegacyOf(name)` 反查 ——
此前是直接取 `d.name`，主名迁移后会让 `name` 与 `legacyName` 变成同一个值，
client 的"旧名对照"静默失效。

**`file.*` 与 `fs.*` 不合并。** 原计划"`Cmd*` 15 条并入 `fs.*`"作废：`file.*` 是交互式
（弹输入框/作用于当前选中），`fs.*` 是确定式（目标由参数点名）。`commands.h:39-47`
早已论证两套语义应当共存，合并会丢掉这个区分，并改写已暴露给 AI 的 `fs.*` 名。
`Cmd` 前缀去掉之后，两者的差异由第二段表达（`file.copy` vs `fs.copy.toclip`），足够清晰。

**② `eb` 命名空间拆解。**
`eb_ui.h/.cpp` → `ui_draw.h/.cpp`（`namespace eb` → `ui`）、`eb_theme.h` → `ui_palette.h`
（`EbColors`/`EbPalette` → `UiPalette`/`PaletteFor`）。21 个文件、约 77 处调用点。
`shell_view.cpp` 里同名的局部变量 `HWND eb`（ExplorerBrowser 缩写）一并改名为 `ebw` ——
它和旧命名空间同名，读代码时容易误认。
`ui::GetUiFont` / `ui::GlyphFace` 收进 `ui::detail`：它们本来零外部调用，
公开等于给了"绕过 `DrawGlyph` 自己建字体"的口子。

**③ `CurrentMods` 移到输入端并去重。**
`eb::CurrentMods` → `mouse_rules.h` 的 `CurrentMods()`（它读 `GetKeyState`，属输入状态，
放在 UI 层是分层反向）。同时发现 `input_router::CurrentMods` 与它**一字不差**（含 AltGr
特判）—— 校验器此前以"需按焦点抑制"为由白名单放行，读代码后确认成员函数没有任何额外
逻辑，改为 `return ::CurrentMods();`，白名单从 2 个文件收紧到 1 个。

**④ 校验器补三条硬约束。**
- **C1 命令 id 稳定**：`check_commands.py` 与 `data/commands-baseline.json` 比对枚举顺序。
  `CmdId` 没有显式赋值，值 = 声明顺序；中间插一条会让后面所有 id 位移，外部脚本/AI
  按 id 调用就会"调 A 执行 B"。现在只允许在末尾追加。
- **C2 旧名只增不减**：基线的 `legacyNames` 集合只许扩大。用户已有的 `keymap.json`
  写的是旧名，删一条就是静默丢键。
- **`eb` 废弃前缀**：3 条 `FORBID`（`namespace eb` / `eb::` / `#include "eb_`），防回流。

**⑤ `STALE` 检查从两态改三态。**
原来"外部有调用就算通过"太宽：`GetUiFont` 只在 `ui_draw.cpp` 内自用也会被判通过。
现在区分——外部有调用（通过）/ 仅内部自用（提示"应私有化"）/ 连内部都没调用（失败）。
顺带修掉"同名 `.h` 的声明行被算作外部调用"的漏检。

### 第二轮审查：发现并修正 7 个问题

以"假设自己改错了"为前提重审，以下都是**第一轮没发现**的：

- **`WM_COPYDATA` 魔数换值撤销**（恢复 `0xEBB1`）。见上方"变更"段的说明。
- **C1 规则漏报中间插入**：原判据比"已有序项的相对顺序"，而 `[A,B,C]` vs `[A,X,B,C]`
  的相对顺序**没变** —— 恰好漏掉最危险的情况（后续 id 全部位移）。改为前缀判据。
- **C1 读错数据源**：基线存的是表行顺序，但 `CmdId` 数值来自枚举声明顺序，两者当前
  确实不同（枚举 @69 = `CopyToFolder`，表 @69 = `FileFocusTop`）。新增 `EnumOrder()`
  从 `commands.h` 读，重建基线。
- **`FindCommand` 第三路已是死路径**：注释还写着"迁移期间三种组合"，但迁移已完成。
  删掉该分支；三路线性扫描改为 `unordered_map` 索引。
- **`UiPalette` 6 个令牌零消费**：`editFocus`/`editPlaceholder`/`listText`/
  `listInertText`/`chevronHot`/`panelBorder` 定义了但从不被取用。删掉，并新增校验规则
  "令牌未被消费"；位置式初始化改指定初始化器（23 个字段逐位核对，颜色值零变化）。
- **`CreateChild` 与 `CreatePopup` 的指针约定不一致**：前者传 `nullptr` 再事后设置。
  栏目前不用 `WM_NCCREATE` 所以侥幸无事，但下一个加该处理的人会拿到空指针。统一。
- **R12（窗口类 ≤3）被错列为独立待办**：R12 原文即"B5 之后"，它依赖载体合并。
  标注前置条件，当前 11 个类暂不启用。

### 第三轮审查：生产路径上的诊断代码（性能）

前两轮查的是"名字"和"规则"，这一轮查**运行时到底做了什么**，发现的问题比命名问题严重：

- **`ReapplyPreviewPane` 里有 ~160 行诊断代码，而它由 120ms 定时器反复调用。**
  诊断内容包括：在最多 40 列里逐个 `GetDetailsOf` 找 Size 列、对前 4 项取
  `PKEY_Size`（网络路径 = 真实 I/O）、用 `IColumnManager` 把 Size 列**移除再加回**
  （视图会闪）、以及一段 **UIA 全后代遍历**（跨进程调用）。用户每次把鼠标移过
  文件列表都要付一遍这个代价。→ 抽出 `DiagnosePreviewPaneState()`，由
  `-DEXBAR_DIAG_PREVIEW` 控制；函数本身从 168 行回到 13 行。
- `SetViewMode` 里 93 行同类诊断 → `-DEXBAR_DIAG_COLUMNS` 控制。
- 这些诊断虽在 `#if` 里但从**没有任何开关**（等于永久开启）。加开关后实测：
  产物里 `[cols]`/`[uia]`/`[late]` 三个特征串**均 0 次**；运行 5 次视图切换
  **新增日志 0 行**。
- **`LogLine` 与 `LogDbg` 是两个名字、一份实现。** `shell_view.cpp` 用前者 31 处、
  其余用后者 54 处；`log.h` 称 LogLine 是"兼容名"，但它实际已是并列主名。
  → 统一到 `LogDbg` 并删除 `LogLine`，新增校验规则"日志入口唯一"。
- 删掉 `shell_view.cpp` 里一行注释掉的 `SetPropertyBag` 诊断；修正 `common.h`
  里"两份轮转策略还不一致"的过时注释。

**方法上的教训**：前两轮只查"是否违反既有规则"，规则之外的问题（性能、过时注释、
注释掉的代码）全部漏掉。这类问题静态检查抓不到，只能逐行读生产路径在运行时做了什么。

### 第四轮审查：静态检查器本身失效（比报错更危险）

前三轮查的是代码与名字。这一轮查**检查器有没有真在工作**，结果发现一个静默失效：

- **`check_ai_meta.py` 的条目正则只命中 21 / 140 条。**
  `116241c` 把 119 条无参 schema 提成 `kNoArgs` 常量后，正则在 `argsSchema` 位置
  要求字符串字面量，一条也匹配不上。脚本照旧打印「全部通过」，第 7 项还写着
  「18/21 条有明确特性」—— 分母从 140 掉到 21，输出里没有任何异常信号。
  检查面静默缩水 85%。
  → 改为**按结构解析**：定位 `kTable`，深度感知地按顶层花括号切条目、按顶层逗号切
  字段（跳过 `Effects{0, 0, ...}` / `Requirements{...}` 内的分隔符）；字段值允许是常量名，
  去 `constexpr const char* NAME = ...` 处取值。再硬断言
  **「解析条目数 == 源码中 `CmdId::` 出现次数」**，任何解析退化当场失败。
  第 2/5/13 项现在对全部 140 条生效（此前只对 21 条）。
- **R20（人机等价）从未被实现。** `ARCHITECTURE-RULES.md` 写了这条规则，
  `check_ai_meta.py` 的 docstring 也声称对账「R18 / R20」，但没有任何检查项对应它。
  实际上只保证了"AI 能调的都已实现"，没保证"AI 能调的人也能调"。
  → 新增第 15 项，按两条路判：
  - **无必填参数** → 命令面板（`Ctrl+Shift+P`）列出全部已实现命令并以 `Args::Empty()`
    调用，机制保证可达。检查器断言 `ShowCommandPalette` 仍保持这个行为，改了立即报错。
  - **有必填参数** → 面板点了必然失败，必须另有能把参数递进去的路。17 条中 14 条是靠
    「确定式双胞胎」（`fs.delete` ↔ `Delete` 键等），3 条是结构化查询（`tool.list` /
    `tool.topn` / `tool.diskusage`），单独登记为 `AI_ONLY_QUERIES`。
- **低估过的坑**：第一版 R20 按"命令 id 在人类侧有没有调用点"扫，把
  `nav.home` / `session.save` / `tab.close.duplicates` 全报成不可达 —— 其实人在面板里
  点得到。面板是**一条**入口覆盖全部无参命令，不该按命令逐个找。规则写成机制判定后
  误报清零。
- `HUMAN_PATH` 里 `fs.properties` 原写「`Alt+Enter` → `CmdProps`」，核实后**并不存在**
  这个绑定（`keymap.json` 与 `input_router.cpp` 的 `kExtra` 都没有）。真实入口是状态栏
  「选中容量」段的点击（`app.cpp` 的 `SetClickFn`）。登记表写错入口比不写更糟 ——
  会让下一个人以为已经验证过。
- 登记表加两道自检：`HUMAN_PATH` / `AI_ONLY_QUERIES` 的键必须仍在 `aiExposed` 集合内，
  否则报「登记失效」。名字写错时不会报错、只会静默不生效，这是登记表最典型的腐烂方式。

**破坏性验证**（确认检查器真能抓，不是空转）：把未实现的 `DupFinder` 标成 `aiExposed`
→ 第 1 项报出（旧版会放过）；把 `NavHome` 的 schema 改成 `"{}"` → 第 5 项报出；
给 `bmk.add` 加必填 `path` 但不改 handler → 第 3 项报出；删掉 `tab.rename` 的
`HUMAN_PATH` 登记 → 第 15 项报出。

**文档数字全线失准**，一并按实测更正（README / AI-INTERFACE / OPERATION-MODEL /
NAMING / PARITY / ROADMAP / ARCHITECTURE）：

| 项 | 旧 | 实测 |
|---|---|---|
| 命令目录 | 137 | **140** |
| 已实现 | 108 | **111** |
| 对 AI 暴露 | 86 | **89** |
| 单测 / 断言 | 20 / 354 | **24 / 464** |
| `check_components` 唯一性规则 | 21 | **28** |
| `check_ai_meta` 检查组数 | 14 | **15** |
| `src/` 规模 | 18,486 行 | **19,940 行** |

### 修复（第二轮）

- `cmd_naming` 两个查表方向大小写不对称：`AliasOf` 用 `find`（区分大小写）、`LegacyOf`
  先 `Lower` —— 表现为"`nav.back` 认、`NavBack` 认、`navback` 不认"。`naming_test.cpp`
  的断言一直失败。两个方向统一改 `_wcsicmp`；局部 `Lower()`（第三个大小写归一实现）随之
  删除，同类工具统一在 `str::Lower`。
- `check_ai_meta.py` 四张硬编码旧名的表（`INDIRECT_ARGS` / `KNOWN_NO_TARGET` / `COVERS` /
  `UI_ONLY`）在主名迁移后全部失配，一次误报 19 条。改为点分名（`c['name']` 现在就是这个值）。

---

## [0.2.1] - 2026-11-20

**操作契约结构化 + AI 操作接口 + 便携存储 + 仓库独立。**
这一版不是加功能，而是把"同一件事有多份实现"逐处收口：命令参数、效果、前置条件、返回值、
命名、导航语义、历史栈、窗口类注册、日志、剪贴板、字形码位、tooltip 都各自收敛到一个实现，
再用 3 个静态检查脚本 + 20 个单测（354 项断言）把回退挡住。

### 变更（仓库独立：从 Windhawk 主仓脱钩）

- **`E:\ExBarBrowser` 成为独立 git 仓库**（分支 `main`）。此前它靠 `D:\Windhawk` 根
  `.gitignore` §8 的白名单放行，寄生在主仓里：改一个分片要在别人的仓库里 `git add ExBarBrowser/`，
  提交历史与 ExBar mod 的 7.7 万行混在一条时间线上，两边互不相关的改动互相干扰。
  迁到 E 盘后工作区已不在主仓路径下（主仓 `git status` 因此变成 95 条 `D`），继续寄生只会留下
  一堆"已删除"的幻影，因此改为独立仓库。主仓里那 95 条删除由操作者自行决定何时清理 ——
  本仓库不引用主仓，主仓也不引用本仓库，先清后清都不影响这边。
- **入库范围不变，但语义变了**：以前"不入库"是为了不给主仓添产物噪音；现在"不入库"意味着
  **克隆即可构建、构建不产生提交**。`data/` 整目录仍不入库（书签/键位/窗口几何是个人数据），
  新克隆首次启动会走内置默认表 + 空书签树，`settings.json` 首次保存时自动创建。
- **`src/keymap.gen.h` 明确入库**：它是 `data/keymap.json` 的生成物，但也是"配置文件缺失时"
  的兜底默认表 —— 不入库会让新克隆在无 `data/` 时没有任何热键。`check_commands.py` 负责
  保证两者一致，所以"生成物入库"在这里不是坏味道。
- **搬目录踩到 PCH 的绝对路径**：`obj/common.h.gch` 是 clang AST 二进制，里面记着
  `D:/Windhawk/ExBarBrowser/src/common.h`；搬完直接 `bash build.sh` 会得到 57 个 TU 全报
  `malformed or corrupted AST file`。`build_fast.py` 不会自动察觉路径变更，需要一次
  `--clean`（冷 PCH 实测 29.8s，其中 PCH 14.1s；PCH 命中后的全量重建是 10.5s）。已写进 README §3.1 与 §8 坑表。
- **README / CHANGELOG 全量重核对**：现状表从"10,200 行 / 127 条命令 / 94 已实现 / exe 12 MB"
  更新为实测值（当时 18,388 行、137 条、108 已实现、86 对 AI 暴露、18.9 MB / strip 1.3 MB），
  §3.4 版本控制整节重写，§5 架构图补 `ai_api`/`ai_server`/`shell_input`/`nav::History`/
  基础设施收编，§6 目录树补全 20 个新模块，§8 坑表补两条（PCH 路径、生成头不同步）。

### 变更（导航收口为组件，全局复用）

导航以前是"同一件事写多遍"：路径语义有两套实现（`common.h` 无长度限制、
`NavModel` 基于 `PathRemoveFileSpecW` + MAX_PATH），历史栈的压栈动作在
`NavigateActive` / `OnTabNav` / `OnTabNavFailed` / `GotoHistory` 四个函数里
各手写一遍（`hist.resize(idx+1)` + `push_back` + `idx = size()-1` 共 4 份），
"此电脑"的 CLSID 硬编码在 3 个文件里。这类重复的代价不是啰嗦，而是**症状远离
现场**：用户看到"后退要按两次"，根因在别处的 `push_back`。

- **新增 `src/nav_path.h` —— 路径语义唯一实现**（纯字符串、无 MAX_PATH 限制）：
  `IsNamespace` / `IsDriveRoot` / `Normalize` / `ParentOf` / `Leaf` / `UpTarget` /
  `CanNavigateUp` / `kThisPc`。`common.h` 的旧名字保留为**转发**，不再有第二份实现。
- **新增 `src/nav_history.{h,cpp}` —— 历史栈事务组件**（每个标签一份）：
  `Request` / `Goto` / `Complete` / `Fail` / `ClearKeepingHere` / `Restore`，
  压栈动作只有 `History::Push` 一处。`App::Tab` 里原来的
  `hist/idx/pending/pendingPushed/pendingPrevIdx` 五个字段收敛成一个 `nav::History`。
- **删除 `NavModel::ParentOf` / `LeafOfPath` 的实现**，改为转发 `nav::`。
  顺带修掉一个静默缺陷：旧实现超过 MAX_PATH 直接返回空，导致**超长路径的地址栏
  Tab 补全失效**；现在走无长度限制的实现（已有单测覆盖）。
- **删除 `common.h::CollapseHistoryToCurrent`**，规则并入 `History::ClearKeepingHere`。
- **导航按钮改走 `CommandBus`**：工具栏的 ←、→、↑、刷新以前直接调 `GoBack()` /
  `GoUp()` 等，绕过命令总线（因而绕过了前置检查与审计）—— 这正是上一版
  "按钮能点、双击没反应"的分裂来源。现在按钮与双击/热键/序列走同一条命令。
- **按钮灰显读同一个谓词**：`NavBar::SetPath` 用 `nav::CanNavigateUp`，
  与命令前置检查 `Req::CanGoUp` 同源，"灰着"必然等于"点不动"。
- **命令拒绝原因对外可见**：`CommandBus` 新增 `lastBlockReason()`。
  以前 `Dispatch` 算出了原因却只写日志与审计，AI/脚本侧只能看到笼统的
  "命令执行失败"；现在返回真实原因（如"已经是顶层（此电脑或网络根）"）。

### 变更（存储改为便携模式：数据放 exe\data）

- **配置目录从 `%APPDATA%\ExBar` 迁到 `<exe>\data`**：整个程序目录可整体拷走，
  不写注册表、不写用户目录，便于随身/绿色部署。迁过去的是**真实配置**：
  `settings.json`、`bookmarks.html`、`keymap.json`、`mouse.json`、`audit.log`。
  书签同时不再与 ExBar mod 共用同一份 `bookmarks.html` —— 两边各写各的，
  避免互相覆盖。旧目录原样保留不删（可回退、可对照）。
  代价：exe 放在 `Program Files` 这类只读目录时配置写不进去，需要把整个目录放到
  用户可写位置；`Config::Writable()` 提供只读检测，写入失败只记一次日志不刷屏。
- **`browser.json` 改名 `settings.json`**：与前两项一致 —— `browser-` 前缀原本是为
  避开 ExBar mod 的同名文件，现在数据目录独立，前缀没有存在意义。
  代码里 `Config::SettingsPath()` 与相关日志一并改为 `settings.json`；
  首次启动若只有旧的 `browser.json`，重命名即可（两者 Schema 完全相同）。
- **去掉 `browser-` 前缀，键位/鼠标规则合并为单文件**：`browser-keymap.json` →
  `keymap.json`、`browser-mouse.json` → `mouse.json`。这个前缀是历史遗留：数据目录
  曾是 `%APPDATA%\ExBar`，与 ExBar mod 共用，而 `keymap.json` / `mouse.json` 正是
  mod 的文件名，不加前缀会互相覆盖。现在数据目录已与 mod 分离，撞名风险消失；
  同时取消"种子 + 用户覆盖"的双文件机制，只保留一套真实配置，用户直接编辑。
- **不再有任何种子/默认值副本**：`Config::SeedAssets()` 整个删除，仓库里也不再带
  示例 `bookmarks.html`。书签文件缺失时走空书签分支（`BookmarkStore::Load`
  返回 false 即空树），不需要种子兜底。
- **`data/` 全部不入库**：这些文件因人而异（书签、最近访问、窗口几何、键位习惯），
  已在 `.gitignore` 中排除，并用 `git rm --cached` 从索引中解除跟踪（磁盘文件保留）。

### 修复（持久化缺口）

- **`closedTabs` 只写不读**：退出时保存了关闭历史，但启动时从未调用
  `TabSet::SetClosed`，导致 `Ctrl+Shift+T` 重开标签在重启后永远是空的。
  现在 `App::Run` 启动即恢复。实测：注入两条关闭历史 → 启动 → 退出，条目完整保留。
- **视图模式不持久化**：`viewMode_` 只活在内存里，每次重启都回到"小图标"。
  现在 `Settings::viewMode` 随 `ApplyViewMode` 落盘，新标签启动时套用。
- **排序/分组不持久化**：补 `Settings::sortMode` / `sortAscending` / `groupBy`，
  通过 `ViewSort` / `ViewGroup` 命令写回。
- **导航窗格/预览窗格显隐不持久化**：补 `Settings::showNavPane` / `showPreviewPane`。
  两个窗格的偏好必须在**首次导航前**就位（`EBO_SHOWFRAMES` 与
  `IExplorerPaneVisibility` 只在建窗格时被消费），因此 `ShellView::Create`
  一开始就把配置读进 `navPaneWanted_` / `g_previewPaneWanted`，避免"第一次打开时
  窗格还在，切一次目录才生效"。
- **最近访问写盘时机**：`AddRecent` 只改内存、全程序仅"清除历史"处 `Save()`，
  正常浏览产生的记录要等用户改别的设置或正常退出才落盘，强杀即丢。
  现在 `AddRecent` 置脏位（`Config::dirty()`），退出时统一写盘，
  浏览过程中不写文件；原地导航到同一路径不再无谓置脏。

### 修复（"上一级"在盘符根失效）

- **盘符根双击文件列表空白无反应**：`C:\`、`D:\` 这类盘符根上，双击空白本该回到
  "此电脑"（与资源管理器一致），实际毫无反应。根因不是漏写分支，而是**同一件事有
  三套判定**：`App::GoUp()` 里写了"盘根 → 此电脑"的分支，但命令总线在
  `Req::CanGoUp` 前置检查处就把它拒了 —— 状态探测（`app.cpp`）把盘根算成
  "不能向上"。于是同一个动作两条路径行为分裂：工具栏的"上一级"按钮能跳（它绕过
  命令总线直调 `GoUp()`），双击空白跳不动（走命令总线被拦）；
  `NavModel::IsRoot()` 又是第三套口径（盘根不算 root），按钮因此还是亮的。
  现在判定收敛成 `common.h` 的 `CanNavigateUp()`，前置检查、`GoUp()` 执行、
  导航栏灰显三处共用。审计日志里原来是一串
  `NavUp (blocked: 已经是顶层（盘根或网络根）) ok=0`，现为 `ok=1`。
- **顺带修掉 `ParentOf()` 的 UNC 判定**：`\\server\share\dir` 的父级是
  `\\server\share`（有效上一级），原代码 `cut <= second` 把它一起判成"没有上级"，
  网络共享下任意子目录的"上一级"都静默失效。改为 `cut < second`。
- 拒绝原因文案同步为"已经是顶层（此电脑或网络根）"—— 原来写"盘根"，
  而现在盘根是**能**向上的，旧文案会误导。

### 移除

- **两个死配置**：`Settings::layout` 与 `Settings::mouseRules` 会读写但全项目无
  使用点（鼠标规则实际只从 `mouse.json` 读）。字段与 `Config::Load/Save`
  中对应行一并删除；旧配置文件里的这两个键在下次保存时自然消失。

### 安全

- **AI 令牌只在运行时有效**：`ai::StopServer()` 此前不清理发现文件，
  `aiEnabled` 关掉后 `ai-token.txt` / `ai-pipe.txt` 仍留在磁盘上，
  看起来像通道还开着。现在停服即删除这两个文件。

### 变更（命令契约结构化，第 1–10 步）

- **第 10 步：偿清"只能靠选中"的债务。** 债务表原本只登记 4 条，实际
  "依赖选中且已实现"的有 15 条 —— 大部分从没被登记。补齐后新增 5 条确定式命令：

   `FsOpenInNewTab {path}`、`FsOpenInNewWindow {path}`、
   `FsCopy {targets[]}`、`FsCut {targets[]}`、`FsProperties {target}`。

  三个值得记的点：
  ① **`FsCopy` 不产生副本** —— 它与 `Ctrl+C` 同义，只是"准备好等粘贴"，
  想真复制到某处要用 `FsCopyTo`。这两件事极易混淆，所以在注释、文档、
  别名（`fs.copy.toclip` vs `fs.copy`）三处都标了。
  ② 造 `IDataObject` 用 `CIDLData_CreateFromIDArray` 而非 `SHCreateDataObject`
  （后者被 `NTDDI_VERSION` 包着，某些配置下不可见）。两者都比自己拼 `CF_HDROP`
  好 —— 后者丢掉"来自哪个命名空间"，粘贴到某些目标会失败。
  ③ **一个路径解析不了就整批失败**。漏一个会造成"我复制了 5 个实际只复制了 3 个"
  而调用方以为全成了。

  另外：写这 5 个 handler 时我先把公共逻辑抽成 lambda，结果静态检查立刻报
  "参数没取用"——第 9 步踩过同一个坑。这次改成不抽公共函数、三个 handler 各写
  三行重复代码：**例外表越长检查能挡的问题越少，重复三行换覆盖值得**。这条写进
  注释了，免得后来人"优化"回去。

  顺带修掉检查器的一个 bug：`COVERS` 遍历外套了 `if old in KNOWN_NO_TARGET`，
  导致 `CmdRename`/`CmdDelete` 在覆盖表里却从不被打印 —— 覆盖情况看起来比实际更差。

  指标：命令 132 → 137，已实现 103 → 108，AI 可调用 81 → **86**，
  债务表未覆盖 3 → **2** 条。

### 变更（命令契约结构化，第 1–9 步）

- **第 9 步：补齐三处能力缺口。** 按"机器最缺 → 失败最不可解释 → 最实用查询"的
  优先级依次做，每项都先对照成熟项目的做法。

  **`FsOpen {path}`（新增）。** `CmdOpen` 依赖选中，机器无法表达"打开第 3 个文件"。
  语义与 `CmdOpen` 一致：文件夹在当前标签进入，文件交 shell。不加 `mode` 参数是
  参照 VS Code —— 它把打开拆成 `open`/`openWith`/`revealInExplorer` 三条独立命令，
  因为那是三种意图，各值得一个名字。路径不存在时直接失败而不弹系统框
  （确定式调用不该卡在没人看的对话框上）。

  **前置条件 16% → 30%。** 参照 VS Code 的 `when` 子句 + `enablement`：把"边界处
  会静默失败"的条件声明出来。新增 4 个条件 —— `CanMoveTabLeft`/`CanMoveTabRight`
  （`TabMoveLeft` 有 `toIndex<0→0` 钳制，已在最左时按了没反应）、`HasClosedTabs`、
  `HasMultipleTabs`、`HasSelectionSingle`。共给 18 条命令补了条件，其中 `CmdRename`
  原先完全没声明（无选中时会弹框）。

  **`OpTopN` / `OpDiskUsage`（新增 `src/disk_scan.{h,cpp}`）。** 三条硬约束：
  ① 必须能中途停（给 deadline，超时带已有结果返回 —— 几万文件的递归卡在 UI 线程
  就是假死）；② 必须报告完整性（`truncated`/`timedOut`/`elapsedMs`/`scannedFiles`，
  只给数字而不说"没算完"会被当成真值）；③ 不跟随重解析点（跟 junction 无限递归，
  且把链接目标大小算到链接处，数字是错的）。

  两条命令而不是一条带 mode：`OpTopN` 答"什么最占空间"（看 `entries`），
  `OpDiskUsage` 答"这目录占多少"（看 `totalBytes`）—— Unix 上 `du` 与 `ncdu`
  也是两个工具。`totalBytes` 统计截断前的全部，否则"总量 < 各子项之和"会让人困惑。

  新增 `tests/disk_scan_test.cpp` 25 项；`precondition_test` 从 26 → 42 项。
  合计 257 项断言全通过。

### 变更（命令契约结构化，第 1–8 步）

按 `docs/OPERATION-MODEL.md` §7 的顺序推进"让每个操作同时对人可用、对机器可调"，
七步机制 + 第八步内容补全已落地，共 219 项断言 + 14 组静态检查守住。

- **第 8 步：内容补全（AI 可暴露面 45 → 78）。** 七步做完的是**机制**，
  不等于每条命令都填了内容。这一步按三条判据逐条判断该不该暴露：
  ① 已实现（否则出现在 `tools/list` 里却调不动）；② 不依赖当前选中
  （机器无法指定"选哪个"，暴露出去就是"AI 以为删了 A，实际删了 B"）；
  ③ 不是纯人机 UI（弹菜单、开关面板、退出程序）。

  新暴露 33 条：`TabMove*`(4)、`GoTab1..9`、`CloseLeftOne`/`CloseRightOne`、
  `PathCopyDir`/`PathCopyTab`、`CmdSelectAll`、`FileFocusTop/Bottom`、
  `CmdNewFolder`/`CmdNewTextFile`/`CmdPaste`、`CycleHiddenSeg`、`FocusAddress`、
  `BmkAddCurrent`、`SessionSave/Restore`、`NewWindow`、`OpenTerminal(Admin)`。

  仍不暴露 22 条，每条有理由，已由静态检查逐条标注：
  17 条依赖选中（`CmdDelete`/`CmdRename`/`PathCopy*` 等 —— 其中 4 条有 `Fs*` 确定式版本
  可替代，其余是真实缺口）；5 条纯人机 UI（`QuitExplorer`/`FolderOptions`/`MapDrive`/
  `UnmapDrive`/`CommandPalette`/`HotkeyManager`）。

  **顺带修掉一个标注错误**：`PathCopyDir` / `PathCopyTab` 早先和 `PathCopy` 一起标了
  `needsSelection=true`，但它们的实现用 `ActivePath()` / `tabSet_`，**根本不看选中**。
  标注与行为不符，把这两条错误地排除在机器能力之外。这提醒 `needsSelection` 是人写的
  标注，必须对照实现核实。

  新增静态检查第 14 项：暴露的命令必须"已实现 + 不依赖选中 + 非纯 UI"，
  违反即报错。

- **第 7 步：对外暴露（MCP 兼容）。** `ai_server` 加 JSON-RPC 2.0 层：
  `initialize` / `tools/list` / `tools/call` / `exbar/state`。
  上一轮的自定义格式（`{"token":..,"method":..}`）对 MCP 客户端不可用 ——
  每个客户端都得自己写转换层就失去了"开箱可连"的意义。现在靠**有无 `jsonrpc`
  字段**区分两种协议，既有本地脚本零改动。

  三处关键决定：
  - **错误分两类**（MCP 硬要求）：请求不合法 → JSON-RPC `error` 带 `code`；
    工具执行失败 → `result.isError=true`。混在一起客户端分不清"我调错了"
    还是"环境不满足"，只能盲重试或盲放弃。
  - **令牌检查在方法分派之前**：否则能靠"返回 method-not-found 还是 bad-token"
    枚举有哪些方法 —— 能力面泄露。单测断言"未知方法与已知方法的未授权响应完全一致"。
  - **`description` 从结构化字段生成，不手写**：手写的会与 `annotations` 脱节
    （改了 `destructive` 忘了改描述），两者不一致时模型会做错判断。

  验证用了**真实管道往返**（`tests/pipe_e2e_test.cpp` 35 项）：进程内起服务、
  自己当客户端连接、走完 initialize → tools/list → 成功调用 → 缺参数 →
  未知工具 → 错误令牌 → 停止清理。47 个工具全部带 name/description/
  inputSchema/annotations。

- **第 6 步：命名空间化。** 新增 `src/cmd_naming.{h,cpp}`：131 条命令的旧名 ↔ 点分名
  双向映射（`NavBack` ↔ `nav.back`、`CmdDelete` ↔ `file.delete`、`FsDelete` ↔ `fs.delete`）。
  `FindCommand` 两个方向都认，所以既有 keymap / 序列 / 鼠标规则**零改动**，
  外部调用方可以开始用稳定名字。

  命名规则：全小写、点分、两段。域取自 `CommandDef::group` 的小写 —— 不另造分类，
  否则两套"分组"迟早不一致。单测里有一条断言"每个点分名的域前缀必须与该命令的
  group 对应"，这条在写测试时立刻抓出了 `AiTogglePanel` 被错标成 `tool.*`
  （它是 `group=Window`）。

  两处有意为之：**大小写不敏感**（与旧名的 `_wcsicmp` 行为一致，避免"`NavBack`
  能用但 `NAV.BACK` 不行"的半吊子行为）；**确定式命令用 `fs.*` 而非 `file.*`**
  （"对指定路径操作"与"对当前选中操作"不是同一件事，同名域会让人以为只是参数不同）。

  `tools/list` 现在同时返回规范名 `name` 与 `legacyName`：新客户端只学一种，
  既有脚本/审计日志用另一个对照。

  判据 `tests/naming_test.cpp` 31 项。

- **第 5 步：返回值。** `CommandBus` 新增一条**独立的**"带结果"注册通道 ——
  `RegisterWithResult` / `InvokeResult` / `HasResult`，`CommandDef` 加 `outputSchema`。
  为什么要独立通道而不是让 131 条 handler 全返回 `json::Value`：绝大多数命令是
  "做了/没做成"，让它们都构造 JSON 是纯负担，还容易让写操作假装有返回值。

  头两条落地的是 `OpListDir`（列出当前目录）和 `OpStat`（查单项属性），
  底层新增 `shellops::ListView` / `StatItem`。它们查的是**当前视图**而不是按路径
  扫盘 —— 与用户看到的完全一致（含虚拟命名空间、搜索结果、过滤状态），
  也不需要重新判断访问权限。

  一个容易忽略的细节：无返回值的命令必须给**空对象**而不是留旧数据。
  调用方复用同一个 `json::Value` 时若不清空会读到上一次的结果 —— 那比"没有结果"
  更危险，因为它看起来像真的。单测里专门断言了这件事。

  判据 `tests/result_test.cpp` 20 项。

- **第 4 步：交互与确定分离。** 新增四条确定式命令 —— `FsRename`（`{target,newName}`）、
  `FsDelete`（`{targets[]}`）、`FsCopyTo` / `FsMoveTo`（`{targets[],to}`）：
  目标由参数点名，绝不弹框、绝不读"用户碰巧选中了什么"。
  旧的 `CmdRename` / `CmdDelete` / `CopyToFolder` / `MoveToFolder` **保持原样**
  —— 它们的语义里"弹框问用户"是不可分割的一部分（`F2` 的体验就是弹输入框），
  加参数会让"传了参数就不弹框"变成隐式分叉。拆开后人机各走入口，逻辑共用。

  实现上**不是**另写一份按路径的文件操作，而是 `shell_ops::SelectByPath` 先选中
  目标、再复用既有的 Selection 实现 —— 否则回收站/冲突/权限/撤销这些语义会有
  第二份实现，迟早与既有行为分叉（架构规则 R6）。

  `shell_ops` 新增：`SelectByPath` / `SelectNone` / `SelectedSinglePath` / `RenameTo`。
  其中 `RenameTo` 用 `IFileOperation` + `FOF_SILENT` + `FOF_NOERRORUI`：
  确定式调用遇到冲突时**返回失败**而不是弹框 —— 明确的失败比没人看的对话框更可预期。

  判据 `tests/fs_ops_test.cpp` 44 项。债务消化 4/7，剩余 `CmdCut`（剪切改的是剪贴板，
  语义特殊）、`BatchRename`（等 PowerRename）、`PathCopyLink`（低优先级）已登记。

- **第 1 步：参数对象化。** `CommandBus::Handler` 从
  `function<bool(const std::wstring&)>` 改为 `function<bool(const Args&)>`
  —— 参数从"一个需要自己拼的字符串"变成具名对象，一次性解决多参数、可选参数、
  类型校验、自动生成 UI、向后兼容加参数五件事。
  旧的单值规格（keymap/序列/鼠标里的 `"ViewMode:3"`）经 `ArgsFromLegacyArg`
  **从 `argsSchema` 推导键名**，因此删掉了 `ai_api` 里那张 `kArgKeys` 手工映射表
  —— 那张表就是"参数是字符串"这个缺陷的症状。
  判据 `tests/args_test.cpp` 22 项：新旧两条路径对同一操作结果一致。

- **第 2 步：效果四维化。** `Risk{Read,Write,Danger}` 拆成
  `Effects{readOnly, destructive, idempotent, openWorld, undoable, scope}`，
  127 条命令逐条按真实行为重标（只读 14 / 破坏性 21 / 跨进程 22 / 幂等 41）。
  单轴表达不了"幂等的写"与"不可逆的写"的区别 —— `NavRefresh` 和 `CmdPaste`
  原来同归 `Write`，但前者可安全重试、后者不可撤销。
  `tools/list` 现在输出 MCP 风格的 `annotations`（四个 hint + scope）。

- **第 3 步：前置条件。** 新增声明式 `Requirements` + `CanExecute()` + `ReqReason()`，
  21 条常用命令已标注。`Invoke` 现在先过前置检查，拒绝时审计记
  `(blocked: 原因)`。这修掉了"无历史时点后退毫无反应"这类哑失败 ——
  以前 handler 静默 `return false`，界面和日志都没有线索。
  判据 `tests/precondition_test.cpp` 26 项。

顺带修掉两个静默缺陷：

- **参数被丢弃**：`TabNew` 声明了 `path` 参数但 handler 从没读过。新增的静态检查
  （`check_ai_meta.py` 第 3 项）会比较"schema 声明的键"与"handler 实际取用的键"。
- **空 schema 的不安全默认**：122 条原为 `"{}"`，按 MCP 规范语义是"接受任意参数"，
  已全部改为 `{"type":"object","additionalProperties":false}`。

已登记的债务（`check_ai_meta.py` 的 `KNOWN_NO_TARGET`）：这 6 条破坏性命令仍只能作用于
"当前选中项"，本身没有目标参数。第 4 步已为其中 4 条提供确定式版本（机器改用新入口），
剩余 `CmdCut` / `BatchRename` / `PathCopyLink` 待做 —— 它们的共同点是"改动的不只是文件"：
剪切改剪贴板、批量重命名要等 PowerRename、快捷方式写入走另一套 shell 接口。

### 新增

- **AI 操作接口（人机同源）**：给命令表补上 `risk` / `argsSchema` / `aiExposed` /
  `needsSelection` 元数据（架构规则 R18），并新增 `ai_api`（工具投影、参数校验、派发）
  与 `ai_server`（命名管道 + 令牌 + 请求泵）。AI 的所有动作都走同一条 `CommandBus`，
  审计记 `actor=ai`，不存在"AI 专用"的第二份实现。对外 41 条工具，集中在导航、
  标签切换、视图调整、书签栏显隐。

  三条设计取舍：
  - **默认关闭**：需 `settings.json` 里 `aiEnabled=true` 才监听管道；
  - **依赖选中项的命令不暴露**（`CmdDelete`/`CmdRename`/`CmdPaste`…）：AI 无法用参数
    指定目标，暴露出去会作用在"用户碰巧选中的"对象上。要支持这类操作需新增能用参数
    指定目标的语义命令，而不是把 UI 命令硬暴露；
  - **危险命令双门**：`aiAllowDanger`（设置）+ `approve=true`（调用方）同时满足才执行。

  配套 `tools/check_ai_meta.py` 静态对账（暴露/参数/选中项三方一致），
  `tests/ai_api_test.cpp`（34 项）+ `tests/ai_server_test.cpp`（8 项）单测，
  说明见 [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md)。

### 变更

- **分隔符行高 24px → 2px**：地址栏下拉的分组分隔线原来占满一整行（120 DPI 下 30px），
  看起来像个空条目。现在分隔线只占 2px：`PopupList` 从 `LBS_OWNERDRAWFIXED` 改为
  `LBS_OWNERDRAWVARIABLE`，逐行设高（`LB_SETITEMHEIGHT`）+ `WM_MEASUREITEM`。
- **地址栏下拉新增"清除历史记录"**：位于列表底部（前面加分隔线），清空当前标签的
  后退/前进栈与"最近访问"列表，保留当前目录作为唯一历史条且不触发导航。
  同时修掉方向键会停在分隔行上导致"按回车没反应"的问题。

### 修复

- **双击文件列表空白区没有返回上一级**：规则引擎、序列键和命令总线早已具备，但 Shell
  文件视图从未把命中的目标喂给 `InputRouter`——`mouse.json` 里也没有 `empty` 规则。现在
  新增 `src/shell_input.{h,cpp}`：Win10 走 `SysListView32` hit-test，Win11 走 UIA
  `ElementFromPoint` + 有限祖先上溯，把 `empty/file/folder/group/column/tree` 翻译成
  `MouseTarget`，经 `MouseSink` 汇入现有规则引擎；`SHELLDLL_DefView` / `SysListView32`
  / 导航树在每次视图重建后自动重装子类。默认规则补上 `empty × left × double → NavUp`。
  实测从 `docs` 双击空白回到父目录，审计为 `mouse NavUp ok=1`。
- **冷启动第一次双击空白仍会偶发失效**：Win11 DirectUI 的点击只经 `WM_PARENTNOTIFY`
  上抛，之前这条路径把目标直接写成 `unknown`，两次点击都进不了规则；同时 UIA 惰性启动
  的单次失败会立刻进入冷却，首击目标拿不到。现在 `WM_PARENTNOTIFY` 的 `DOWN` 也做
  UIA/ListView 命中测试（`UP` 沿用 pending 目标），并通过 `WM_APP_MOUSE_*_NOUP` 接上
  `InputRouter` 的 `expectUp=false` 契约，派发后立刻释放 claim；UIA 失败累计 3 次探测才冷却，
  `InputRouter` 为首击 `unknown` 保留匿名 pending、由第二击补上目标。冷启动冷进程复测：
  `before=docs → after=ExBarBrowser`，审计 `mouse NavUp ok=1`。
- **面包屑分隔符列成了"同级目录"**：点 `D:\Windhawk` 右侧的 `›` 弹出的是 `D:\` 下的同级
  文件夹（Downloads、Program Files…），而不是 `D:\Windhawk` 自己的内容。原因是
  `NavBar::ShowSiblingMenu` 先对分段路径取 `ParentOf()` 再枚举，得到的是"当前级的兄弟"。
  分隔符语义上归属于它**左边**那一级，改为直接枚举 `parts[partIndex].path` 的子文件夹，
  与 mod `ShowCrumbMenu` 的 `EnumChildFoldersAt(crumbPidls[partIndex])` 对齐；函数随之
  更名为 `ShowChildMenu`。副作用是盘根分段（`D:\`）的 `›` 现在也能列出该盘顶层文件夹。
- **隐藏预览窗格后一换目录又自动打开**：以前读 `FOLDERFLAGS` 的 `0x400` 当"预览窗格"
  位——那是 `FWF_NOSCROLL`，几乎所有视图都置位，于是每次都判成"已打开"，导航时又把窗格
  打开。`FOLDERFLAGS` 里根本没有"显示预览窗格"这一位；真正决定窗格是否创建的是宿主提供的
  `IExplorerPaneVisibility::GetPaneState`。现在 `ShellView` 通过 `IServiceProvider`
  进入 ExplorerBrowser 站点链并实现 `IExplorerPaneVisibility`：关闭态返回
  `EPS_DEFAULT_OFF | EPS_INITIALSTATE | EPS_FORCE`；"是否已显示"改以窗格宿主窗口
  （`Shell Preview Extension Host`）的真实可见宽度为准采样，并在 `OnNavigationPending`
  里采样，覆盖双击文件夹等 shell 内部导航。`TogglePreviewPane`（`Ctrl+Alt+P`）与
  shell 自带关闭按钮都形成持续意图。
- **选中文件不显示高亮**：根因是**我们对 shell 内部窗口调了 `SetWindowTheme`**。
  `ApplyTheme`/`OnViewCreated` 会遍历视图子树，把 `ExplorerBrowserControl`、
  `SHELLDLL_DefView`、`DirectUIHWND` 等一律设主题；DirectUI（Win11 文件列表）
  因此改用另一套 `SubAppName` 样式字典重新解析 UIFILE，之后选中项不再绘制高亮底色
  ——但选中集本身是完好的（状态栏"已选 N"正常），所以看起来像"焦点问题"，实际是绘制被改坏。
  现在只对 `ExplorerBrowserControl` 这一层设主题（与同类宿主项目做法一致），交给 shell
  自己把深色下传给内部 DirectUI；内部窗口一概不碰，只单独深色化 `ScrollBar` 小控件。
  另移除了对 DirectUI 树反复广播 `WM_THEMECHANGED` 的 `PokeTheme`。
  依据 `tools/probe_highlight.cpp`（离屏、不抢焦点、不模拟鼠标，选中前后像素差分判据）：

  | 动作 | changed | peakCount | 结论 |
  |---|---|---|---|
  | 基线（不改主题） | 4784 | 174 | 有高亮 |
  | 只对顶层 `ExplorerBrowserControl` | 4784 | 174 | 有高亮 |
  | 对全子树 `SetWindowTheme` | 1224 | 14 | **无高亮** |
  | 只给 `ScrollBar` 设主题 | 4784 | 174 | 有高亮 |
  | 只调 `AllowDarkForWindow` | 4784 | 174 | 有高亮 |
  | 只发 `WM_THEMECHANGED` | 4784 | 174 | 有高亮 |
  | 先画坏再 `SetWindowTheme(NULL,NULL)` | 4784 | 174 | 可恢复 |
- **选中项不高亮时键盘也失灵（伴随问题）**：焦点此前只给到视图容器
  `ExBarBrowser_TabView`，从未交给 `ExplorerBrowserControl`，框架因此不知道本视图被激活，
  type-ahead/方向键/右键上下文都受影响。现在容器收到 `WM_SETFOCUS` 时先把焦点交给
  `ExplorerBrowserControl`（IExplorerBrowser 自己的宿主窗口，它再走
  `OnViewWindowActive → UIActivate`），随后补一次 `UIActivate(SVUIA_ACTIVATE_FOCUS)`。
- **历史栈每步压两条 → 后退要按两次**：以前 `NavigateActive` 先压"请求路径"，Shell 完成导航
  后再压"真实路径"（重定向 `文档 → OneDrive\文档`、尾斜杠、正斜杠规范化都会触发）。
  现在 `Tab::pending` 记录本次请求，完成时**就地校正** `hist[idx]`（多级重定向逐跳收敛），
  视图内部双击才压新条；后退/前进统一走 `GotoHistory`，导航失败会把游标退回。
- **复制/剪切文件、拖放全部失效**：`WinMain` 只调了 `CoInitializeEx`，而 `OleSetClipboard` /
  `OleFlushClipboard` 和 shell 视图的 `RegisterDragDrop` 都要 OLE。改为 `OleInitialize` /
  `OleUninitialize`。
- **Ctrl+Shift+1..8 全部变成"超大图标"**：8 个键绑的是同一个 `ViewMode` 且没有参数，
  `_wtoi("")==0`。命令规格现在支持 `Name:arg`（`ViewMode:3`），`CommandBus::InvokeByName`
  统一拆分，键盘/序列/鼠标/面板同一条路径。
- **9 条序列 `g 1`…`g 9` 是空动作**：keymap 绑了 `GoTab1..9`，命令目录里却没有这些 id，
  绑定阶段被静默跳过。补齐 9 条命令 + 处理器；目录同时对账 mod 的 127 条能力（新增
  `CopyToFolder` / `MoveToFolder` 实现，`TabMergeAll` / `BatchRename` / `SelectByPattern` 标注未实现）。
- **书签栏气泡悬停读到已释放内存**：`TTM_ADDTOOLW` 只保存 `lpszText` 指针，原代码传的是
  循环内的局部 `std::wstring::c_str()`。标签栏/状态栏同样问题一并改为持有副本。
- **新建文件夹第二次必失败**：`IFileOperation::NewItem` 少了 `FOF_RENAMEONCOLLISION`
  （explorer 默认自动改名 "新建文件夹 (2)"）。
- **状态栏 800ms 全量枚举**：文件夹/文件分类按 `(path,total)` 缓存（`SplitCache`），
  大目录不再每 800ms 做一遍 O(n) 的 COM 遍历。
- **地址栏/书签栏/标签栏图标不识别特殊命名空间**：三处此前各自裸调 `SHGetFileInfoW`
  （且对 `::{GUID}` 还加了 `SHGFI_USEFILEATTRIBUTES`），此电脑、回收站、网络、云存储/NSE
  扩展一律退成通用文件夹图标。新增 `src/icon_resolve.{h,cpp}`（移植 exbar.cpp 的
  `GetPathIcon` 全链路）：友好名→解析串映射、虚拟命名空间转 PIDL 取真实图标、真实路径先
  PIDL 解析（云盘等 NSE）、注册表 `CLSID\DefaultIcon` 兜底、stock 文件夹保底。四处调用点
  统一为 `ico::Resolve`：`nav_bar.cpp`(lead 图标)、`bookmark_bar.cpp`(IconFor)、
  `nav_bar_actions.cpp`(IconFor→弹层)、`app.cpp`(标签栏)。
- **鼠标滚轮规则从未生效**：`WM_MOUSEWHEEL` 没接到 `InputRouter`，且标签栏传的是客户区坐标
  （契约是屏幕坐标）、水平滚轮方向反了。三处一起修。
- **复制路径在剪贴板失败时漏 HGLOBAL**：`CopyText` / 书签菜单改为失败即 `GlobalFree`。
- **链接失败**：`src/icon_resolve.cpp` 未列入 `build_fast.py` 的 `SOURCES`。

### 新增

- **输入端脚手架补齐 Shell 文件视图**：`src/shell_input.{h,cpp}` 把参考 `Exbar.cpp` 的目标
  模型落到独立进程：目标枚举扩出 `unknown/group/column/tree/search/toolbar/preview`，
  鼠标规则新增存在性早退（无对应按键规则不做 UIA 跨进程调用）、50ms 重复 DOWN 去重、
  系统双击矩形 + 防三击、左键第一击放行。UIA 单次失败即时重试，连续 3 次探测失败才
  500ms 冷却，避免第一次点击就丢规则。键盘组合键、序列键、快捷键管理器、命令总线仍保持单一入口。
- **纳入版本控制**：ExBarBrowser 全部源码、脚本、文档与构建配置纳入 `D:\Windhawk` 主仓 git
  （根 `.gitignore` §8 放行、本目录 `.gitignore` 逐项排除产物），首次提交 95 个文本文件。
  `*.exe` / `*.obj` / `*.d` / `*.gch` / `*.log` / `shot*.png` / `out*.png` 与 `*.bak` 类
  编辑备份一律不入库；提交沿用主仓约定式提交，见 [README](README.md) §3.4 与
  [CONTRIBUTING](CONTRIBUTING.md) §2.3。

  *（同日晚工作区迁到 `E:\ExBarBrowser` 后已改为独立仓库，见本版首节；此条保留为当时的事实记录。）*
- **复制到 / 移动到文件夹**（`CopyToFolder` / `MoveToFolder`）：`IFileOpenDialog(FOS_PICKFOLDERS)`
  + `IFileOperation::CopyItems/MoveItems`，撤销与冲突交给系统。
- **命令目录审计**：`tools/check_commands.py`（静态：表 / keymap / 处理器三方一致）与
  `tests/commands_test.cpp`（运行期：keymap 引用的命令必须存在于目录）。
- **应用图标**：自绘「蓝色圆角底板 + 白色文件夹 + 双标签页」图标（品牌蓝 `#4C9FFF`），
  经 `windres` 编进 exe 的 `.rsrc`（`RT_GROUP_ICON` + 8 档 `RT_ICON`：16–256px）。
  标题栏/任务栏/Alt-Tab/资源管理器统一生效；`tools/gen_icon.py` 可再生成，`resources/app.rc`
  声明资源，`src/app.cpp` 的 `LoadAppIcon()` 从资源加载并 `WM_SETICON` 设置大小两档。
  `build_fast.py` 在 PCH 后增量编译资源对象（`app.res.obj`）并参与链接。

### 变更

- **默认键位表改成"数据 → 代码"生成**（消除第二副本）：`src/keymap.cpp` 里 70 条
  `kDefaultHotkeys[]` + 38 条 `kDefaultSeqs[]` 是从 `data/keymap.json` 手抄的兜底表，
  改默认键要同时改两处、漏一处就出现"配置文件读到新键、兜底还是旧键"的静默不一致。
  现在由新增的 `tools/gen_keymap.py` 生成 `src/keymap.gen.h`，`BuildDefault()` 只遍历生成表。
  `tools/check_commands.py` 增加一项对账：生成头与 json 不一致即 FAIL（已验证负向用例）。
  `data/keymap.json` 与 `../data/defaults/keymap.json` 内容同步（70 热键 + 38 序列）；
  `src/keymap.h` 的注释此前称两文件是"同一份资产"，实际是两份独立副本，已改正。
- **构建清单改自动扫描**：`build_fast.py` 的 `SOURCES` 从 45 行手写列表改为 `src/*.cpp`
  自动扫描 + `EXCLUDE_PATTERNS` 排除表。多人/多会话并行时"新增 .cpp 就要在同一段字符串里
  插一行"是必冲突改动，且冲突解决不当时会静默漏文件（直到链接期报未定义符号）。
  已验证：并发会话新增 `src/shell_input.cpp` 后被自动收录，构建脚本零改动。
- **构建产物可隔离**：新增 `--objdir <dir>`，obj/pch/资源对象与该会话的暂存 exe 一起进
  独立目录（暂存名 `ExBarBrowser.<objdir>.exe`）。此前两个会话共用 `obj/` 和
  `ExBarBrowser.new.exe`：一方重建 PCH、另一方正拿旧 PCH 编译；或一方刚链接完就被另一方覆盖。
- **地址栏分隔符间距收紧**：每级 chevron 区宽 `kSep96` 32→20px、绘制内边距 `S(8)`→`S(3)`，
  前导/面包屑图标绘制槽 `kIcon96` 16→14px（对齐 `Exbar.cpp`），`›` 与前后面包屑文字的留白
  由约 16px 降到约 6px；点击热区仍保留 20px，箭头不缩小。
- **弹窗列表深色滚动条**：自绘弹层里的原生 `LISTBOX` 以前没套主题，深色背景上是一条亮灰宽条；
  现在深色用 `DarkMode_Explorer`、浅色用 `Explorer`，并给列表窗口 `AllowDarkForWindow`，
  滚动条颜色与 hover 展开宽度与 ExBar mod 一致。
- **标签栏高度 32 → 26**（逻辑像素，`theme_.S()` 后随 DPI 缩放）：新增常量
  `kTabBarHeightDp`（`tab_strip.h`），`App::Layout` 与 `TabStrip::Create` 共用一处数字，
  不再各写一个。四栏高度现状：标签 26 / 导航 30 / 书签 26 / 状态 24。
- **`tab_strip` 拆成五片**：`tab_strip.cpp`(建模/图标/生命周期) + `_input`(消息/悬停/命中) +
  `_layout`(宽度/溢出/tooltip) + `_paint`(绘制) + `_actions`(右键与 » 菜单)。纯移动。
- **`app_commands` 拆成四片**：核心(调度 + `WithView` + 菜单) / `_tabs`(标签·跳转·导航·路径) /
  `_files`(文件操作·视图) / `_misc`(窗口系统·外部·书签·会话)。`withView` 从函数内 lambda
  提升为 `App::WithView`（两个分片共用），`tools/check_commands.py` 改为审计
  `app_commands*.cpp` 全片，处理器计数仍为 94。
- **`app.cpp` 按职责拆成六片**（`ARCHITECTURE-RULES` R3：单文件 ≤300 行）：
  `app.cpp`(装配/生命周期 286 行) + `app_wnd`(消息路由) + `app_tabs`(标签生命周期) +
  `app_nav`(导航与历史栈) + `app_layout`(布局与主题) + `app_stats`(状态栏取数)。
  纯移动，无逻辑改动；`HomePath()` 改为 `App` 的静态成员，供各分片共用。
- **标签图标所有权下沉到 `TabStrip`**：`SetTabs` 现在直接吃 `core::TabRef` 列表，标题兜底、
  `ico::Resolve` 与 HICON 缓存（上限 256）都在组件内完成，`App` 不再持有 `tabIcons_`。
  缓存满时只回收"本次不展示"的 path —— 原实现在 `App` 里是整表 `clear()`，会把已经在
  `items_` 里、正要绘制的句柄一起销毁。

---

## [0.2.0] - 2026-11-19

**功能迁移**：把 ExBar mod 的七类自定义能力迁到独立进程（M0–M7）。宿主接线（DUI/XAML 子类化、
低级钩子、跨进程 RPC）不迁移，改为自有窗口 + 组件 + 命令管线。

### 新增

- **命令管线**（`commands.cpp`）：127 条命令目录（与 `data/capabilities.json` 同 id，90 条已实现 / 36 条占位），
  统一 `Invoke(id, arg, actor)`；`audit.log` 记录 actor（key/seq/mouse/ui/palette/ai）。
- **键盘快捷键**（`keymap.cpp` / `chord.cpp`）：直接读 mod 的 `keymap.json`（**70 条**），
  支持用户覆盖、冲突与系统保留键标记。
- **序列键**（`sequence.cpp` / `sequence_hint.cpp`）：**38 条**（`g g` / `g t` / `g u` …），
  前缀状态机 + 提示弹层 + 超时 + `Esc` 取消。
- **鼠标规则**（`mouse_rules.cpp` / `input_router.cpp`）：目标区 × 按钮 × 触发（单击/双击/长按/四向滚轮）
  × 修饰键；内置"双击书签栏空白 → 切换书签栏"。
- **标签栏**（`tab_strip.cpp` / `tab_set.cpp`）：拖拽排序、锁定、关闭历史、MRU、溢出滚动与 `»` 菜单、
  右键菜单、克隆、中键关闭。
- **导航栏**：地址自动补全、最近访问、视图排序/分组、隐藏文件三态、导航窗格开关。
- **书签栏**（`bookmark_bar.cpp` / `bookmarks.cpp`）：与 mod 共用 `bookmarks.html`
  （Netscape + `EXBAR_*`），拖拽添加、右键增删改、显隐。
- **状态栏**（`status_bar.cpp` / `view_stats.cpp`）：9 段（文件夹/文件/已选、修改日期、路径、
  选中容量、磁盘/容器、隐藏三态、类型），tooltip、段点击、右键显隐。
- **命令面板**（`app_commands.cpp`）与**快捷键管理器**（`hotkey_manager.cpp`）、
  **文本输入对话框**（`prompt.cpp`，书签重命名/新建分类）。
- **配置与状态**（`config.cpp` / `json.cpp` / `text_file.cpp`）：`%APPDATA%\ExBar\browser.json`，
  首次运行把 keymap/mouse/bookmarks 复制到用户目录。
- **会话**：保存 / 恢复命名标签组。

### 修复（2026-11-19 代码审查）

- `build.bat` 与 `build.sh` 的文件清单/库列表同步，并补 `-lpropsys`（PKEY 需要）。
- 标签栏作用域的 `Shift+←/→`（`TabMoveLeft/Right`）不再抢文件视图的 Shift 选择：
  只在"无 Ctrl/Alt 且焦点位于标签栏"时生效；编辑框内的 `Ctrl+C/V/X/A/Z`、方向键、Delete 让给 EDIT。
- `tf::WriteText` 改为写 `.tmp` + `MoveFileEx(REPLACE_EXISTING)` 原子替换，避免写一半截断配置/书签。
- `ShellView::Sink` 补 `virtual ~Sink()`（`Release()` 里 `delete this` 的基类析构要求）。
- 退出顺序修正：`WM_CLOSE` 只保存配置并销毁视图；字体与图标在子窗口销毁后的 `WM_DESTROY` 释放，
  避免销毁过程中重绘用到已删字体。
- 键位/鼠标用户文件改用 `keymap.json` / `mouse.json`，不再与 mod 的
  `%APPDATA%\ExBar\keymap.json`（`bindings[]` 格式）互相覆盖。
- 删除未使用的 `app_commands.cpp::ArgInt` 与 `tab_strip.cpp` 的重复 `DrawChevron`。
- 标签栏溢出时不再每次刷新把横向滚动位置重置到最左；切换/新建标签会自动把激活标签滚入可视区。
- 状态栏取数节流：导航变化立即取数；后台窗口每 5 拍、>800 项每 2 拍、>3000 项每 4 拍，
  避免每 800ms 全量枚举大目录。
- `audit.log` 增加轮转（每 200 条检查一次，超过 2 MB 重命名为 `audit.log.1`，只保留一份）。
- `tabNewPos` 生效：`right`（默认，当前标签右侧）/ `end`（末尾），新建、克隆、右键新建都遵守。
- 修复：地址栏右侧 `⌄` 下拉被长路径撑得比地址框宽 → `ShowPopupList` 新增 `exactWidth`，
  该菜单严格等于地址框宽度（文字用省略号收）；超出屏幕时保持**左边缘对齐**（缩窄而不是整体左移）。
- 修复：保存的窗口几何可能落在屏幕外（地址框右端跑出屏幕，弹层只能被拉回屏内，看起来不对齐）
  → 启动时把窗口整体夹回当前显示器工作区。

### 已知问题

- 审计与调试日志都是同步 `CreateFile/WriteFile`（已加 2 MB / 4 MB 轮转，高频操作仍有可测开销）。
- 多个文件超过 300 行（`app.cpp` / `app_commands.cpp` / `tab_strip.cpp` / `bookmark_bar.cpp`…）。
- 代码为平铺结构，与 [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) 的分层规则不一致（待定）；
  `src/contract|input|pipeline` 是并行架构批次的占位，**未接入构建**，与 `src/commands.*` 并存待合并。
- 未实现：搜索（Everything）、工具集、预览/详细信息窗格、复选框、AI 面板、批量选择/复制到/移动到。

---

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
    + 画刷 `brBg/brBar/brBarAlt/brHover/brEdit` + 字体 `font/fontBold/glyphFont`（Segoe MDL2 Assets）。
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

---

## 版本号约定

`0.x` 阶段：`0.1.0` 为 P1 骨架；`0.2.x` 起按 P2 功能递增（`0.2.1` = P2 收口 + 操作契约 + AI 接口），
`0.3.x` 对应 P3，`1.0.0` 留给"可作为系统默认文件夹处理器日常使用"的里程碑
（P2 全绿 + 连续一周无崩溃）。

一个版本号对应一次**可回退的交付**，不对应一次提交。版本内允许多次提交，
但 `[Unreleased]` 只能在版本发布时整段下移，不要边做边改已发布的段落。
