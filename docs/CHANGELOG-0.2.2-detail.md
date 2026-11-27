# CHANGELOG 0.2.2 详细记录

> 本文档从主 CHANGELOG.md 迁出，保留该版本的完整开发过程记录（验证、踩坑、逐条 why）。
> 主 CHANGELOG.md 对应版本段只保留精简摘要。

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

|                                             |                           |
| ------------------------------------------- | ------------------------- |
| 顺带补上右键（\`WM_RBUTTON(DOWN                    | UP)\`）：标签/书签/状态栏的右键菜单开着时， |
| 用户的下一手可能是右键点别处，原先这条消息漏给 ListBox 默认处理、行为不可控。 |                           |
| 现在右键落在菜单内**只关菜单不选行**（`TrackPopupMenu` 同语义）。 |                           |

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
  | 令牌              | 96 DPI | 用途                                     |
  | --------------- | ------ | -------------------------------------- |
  | `iconChevron96` | 10     | 面包屑每级 / 书签栏分类 / 弹层展开器 / 导航栏分裂箭头与字段下拉箭头 |
  | `iconGlyph96`   | 12     | 标签栏 ✕ ＋ 溢出 锁定 / 书签栏溢出 / 状态栏段图标         |
  | `iconCompact96` | 14     | 导航按钮、面包屑前导文件夹                          |
  | `iconSmall96`   | 16     | 标签栏 / 书签栏 / 弹层行的条目图标                   |
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
  | 原                                | 现                                   | 说明                        |
  | -------------------------------- | ----------------------------------- | ------------------------- |
  | `eb_ui.{h,cpp}` + `namespace eb` | `ui_draw.{h,cpp}` + `namespace ui`  | 16 个绘制/量字/图标原语            |
  | `eb_theme.h`                     | `ui_palette.h`（`UiPalette`）         | 色值令牌是数据，绘制原语是动作，两者分开      |
  | `eb::TooltipHost`                | `ui::TooltipHost`（`tooltip_host.h`） | 同属 UI 原语                  |
  | `eb::CurrentMods`                | `mouse_rules.h` 的全局 `CurrentMods()` | 读 `GetKeyState`，属输入端（R15） |
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

| 项                        | 旧        | 实测           |
| ------------------------ | -------- | ------------ |
| 命令目录                     | 137      | **140**      |
| 已实现                      | 108      | **111**      |
| 对 AI 暴露                  | 86       | **89**       |
| 单测 / 断言                  | 20 / 354 | **24 / 464** |
| `check_components` 唯一性规则 | 21       | **28**       |
| `check_ai_meta` 检查组数     | 14       | **15**       |
| `src/` 规模                | 18,486 行 | **19,940 行** |

### 修复（第二轮）

- `cmd_naming` 两个查表方向大小写不对称：`AliasOf` 用 `find`（区分大小写）、`LegacyOf`  
  先 `Lower` —— 表现为"`nav.back` 认、`NavBack` 认、`navback` 不认"。`naming_test.cpp`  
  的断言一直失败。两个方向统一改 `_wcsicmp`；局部 `Lower()`（第三个大小写归一实现）随之  
  删除，同类工具统一在 `str::Lower`。
- `check_ai_meta.py` 四张硬编码旧名的表（`INDIRECT_ARGS` / `KNOWN_NO_TARGET` / `COVERS` /  
  `UI_ONLY`）在主名迁移后全部失配，一次误报 19 条。改为点分名（`c['name']` 现在就是这个值）。

---
