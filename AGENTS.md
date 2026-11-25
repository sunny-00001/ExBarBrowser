---
status: current
baseline: 2026-11-21 18:40
---

# AGENTS.md — 给 AI Agent 的工程入口

> 动代码前先读这一份。它只做三件事：**硬约束**、**可执行命令**、**文档索引**。
> 规则细节以 [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) 为准；本文件与它冲突时，
> 信 docs 并顺手改本文件。
>
> **每个新会话的固定开场**（详见 §8）：
>
> ```bash
> # 1) 用后台任务跑，它会构建（仅过期时）并常驻持有实例
> python tools/agentctl.py serve --wait 30
> # 2) 确认就绪：channel_ready=true 且 stale=false
> python tools/agentctl.py status
> # 3) 先看有没有现成能力，不猜命令名
> python tools/agentctl.py tools --grep bmk
> ```

---

## 0. 这是什么工程

独立进程的文件浏览器（360 文件夹路线）：自己的窗口、自绘 chrome、每个标签一个
`IExplorerBrowser`。**不注入 explorer、不装全局钩子、单 exe 零运行时依赖**。
全局钩子的唯一豁免是 `src/kb_hook.cpp` 的 `WH_KEYBOARD_LL`（中文输入法把字母吃成
`VK_PROCESSKEY`，不挂钩取不到真实 vk；`check_components.py` 的 HOOK_GUARD 强制只允许这一处）。
视图（右键菜单/拖放/重命名/缩略图/排序分组/导航窗格）全部由系统 shell 提供，不自绘。

现状要点：`src/` 是**扁平多文件**（`src/*.cpp`，由 `build_fast.py` 自动扫描），
而 docs 里的 `contract/ core/ platform/ ui/ pipeline/ features/ app/` 是**迁移目标**，
尚未落地。写新代码按目标分层的精神组织，但不要假设那些目录已经存在。

---

## 1. 三条不可协商

### 1.1 R21 输入纪律（最容易在写验收脚本时违反）

**禁止模拟鼠标/键盘、禁止抢焦点、禁止循环点击。** 这不是风格问题：本程序是用户
本机正在使用的窗口，抢光标会把用户下一次点击挪到别处，抢焦点会打断用户手上的操作。

| 禁止 | 判据 |
|---|---|
| `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos` / `BlockInput` / `AttachThreadInput` | 全仓（`src/` `tools/` `scripts/` `tests/`）零容忍 |
| `PostMessage`/`SendMessage` 投 `WM_LBUTTON*` / `WM_RBUTTON*` / `WM_MBUTTON*` / `WM_XBUTTON*` / `WM_MOUSEWHEEL` / `WM_KEY*` / `WM_CHAR` | 合成点击必然命中控件 → `ShowPopupList` → 弹层内 `SetForegroundWindow`，后果与 `SendInput` 相同 |
| 脚本里 `SetForegroundWindow` / `SetActiveWindow` / `BringWindowToTop` / `SwitchToThisWindow` | 自动化脚本零例外；需要前台才能测的场景打印 `SKIP` 退出 0 |
| `pyautogui` / `pynput` / `AutoIt` 一类库 | 底层就是 `SendInput`/钩子，一行绕过全部判据 |

**允许的验证路径，按优先级**：

| 要验证什么 | 用什么 |
|---|---|
| 逻辑 / 几何 / 状态机 | 抽纯函数写单测（`tests/*_test.cpp`，不需要窗口） |
| 一条动作能不能跑通 | 进程内 `CommandBus::Invoke(id, args, actor)`；进程外 AI 命名管道 `tools/call` |
| 只需读状态 | 管道 `exbar/state`，或命令 `OpListDir` / `OpStat` |
| 界面长什么样 | `python scripts\shot.py out.png`（`PrintWindow` 只读，被遮挡也能抓） |
| 必须真点才成立的流程（弹层开合、拖拽） | **人工点、脚本只观察**（`tools/probe_toggle.py` 是范例） |
| 验证某个 `WndProc` 分支本身 | 只投**非输入**消息：`WM_CLOSE` / `WM_THEMECHANGED` / `WM_APP_*` |

新增自动化脚本时，头注释必须写一句：**"不发合成输入，只读；不动光标、不抢焦点"**。

`src/` 里本来就有的 `SetForegroundWindow`（激活已有窗口、热键唤出、弹层取键盘输入）
**不受此禁令约束** —— 判据是"触发源是人还是脚本"，不是 API 名字。不要顺手删掉：
删了"第二次启动打开新路径"就变成"后台默默多开一个标签"。

### 1.2 不引入第三方库 / 包管理器

目标是单 exe 零依赖。新依赖必须先证明"自己写 200 行解决不了"。
系统库加一项只改 `build_fast.py` 的 `LIBS` 一处。

**已豁免的一项（见 docs/DECISIONS.md D14）**：AI 面板的渲染层用 WebView2
（`WebView2Loader.dll` **运行时**加载，不进链接期依赖）+ Node 子进程。
这两个依赖只在"用户主动展开 AI 面板"时被用到；缺任意一个时面板显示占位页，
其余功能不受影响。新增**其它**第三方依赖仍需按上面的标准论证。

### 1.3 视觉与品牌串各只有一个来源

- 颜色/字号/间距一律 `theme_.S()` 与 `theme_.xxx`；不写死像素，不写 `RGB(`。
- 品牌串只出现在 `src/identity.h`（`id::kBrand` / `id::kBrandShort`）；
  其他文件出现 `L"ExBar…"` 会被 `check_components.py` 判失败。
- 图标/字体/画刷/日志/量字各有唯一实现，不要写第二份。

---

## 2. 命令速查

```bash
# 0) 闭环自举与诊断（新会话先跑这一段，理由见 §8）
python tools/agentctl.py serve --wait 30   # 构建(仅过期)+启动+常驻；**用后台任务跑**
python tools/agentctl.py status            # exe/源码新鲜度、pid、通道、工具数
python tools/agentctl.py tools --grep bmk  # 列出 AI 可见命令（名称/risk/必填参数）
python tools/agentctl.py call bmk.list '{}'           # 退出码 0=verdict 真，1=假，2=通道断
python tools/agentctl.py gap bmk.move '{"id":"x"}'    # 失败原因归类 + 补哪几处
python tools/agentctl.py down              # 只关 agentctl 自己启的实例

# 构建（Git Bash / cmd 二选一）
bash build.sh                  # 增量；关掉运行中的实例，否则链接 Permission denied
bash build.sh --clean          # 全量重建（换目录/换机器后必须，PCH 记绝对路径）
bash build.sh -j 16            # 并行数（默认 8）
bash build.sh --stats          # 每个 TU 耗时
bash build.sh --out /tmp/x.exe # 只验证能否编过，不碰正在运行的 exe
bash build.sh --no-gc          # 本次构建不做残留回收（排障时用）
bash build.sh --gc-age 60      # 回收的活跃保护窗口（分钟，默认 15）

# 构建残留回收（每次构建成功后自动跑一轮；这里是手动入口）
python tools/reclaim.py            # 预演：只列会删什么，不动手
python tools/reclaim.py --apply    # 真删
python tools/reclaim.py --apply --min-age 60   # 放宽"还在写"的保护窗口

# 单测（推荐；数量自动枚举，末行给通过/总数）
bash tests/run_all.sh          # 全量
bash tests/run_all.sh keymap   # 只跑名字含 keymap 的
# 退出码 = 失败条数；77 = 无法运行（如非交互会话下拿不到剪贴板），不计入失败但会打印

# 三个静态检查（改命令表 / AI 元数据 / 唯一实现时必跑）
python tools/check_commands.py     # 命令表 / keymap / 处理器三方对账 + CmdId 顺序与 legacy 基线
python tools/check_ai_meta.py      # AI 暴露命令的 schema/risk/断言/人机等价
python tools/check_components.py   # 唯一性 / 禁令 / 接线 / 令牌消费 / R21 INPUT_GUARD
python tools/check_components.py --list   # 看每条规则的命中与作用域

# 键位表生成（唯一入口是 data/keymap.json）
python tools/gen_keymap.py            # -> src/keymap.gen.h（兜底默认表）
python tools/gen_keymap.py --check    # 只校验是否过期（过期退出码 1）

# 结构化操作程序（不用鼠标、不改用户设置）
./ExBarBrowser.exe --ai                       # 或 EXBARBROWSER_AI=1：进程内开通道，不写 settings.json
python tools/ebbctl.py list                    # 列出全部工具
python tools/ebbctl.py call bmk.list '{}'      # 调命令，返回含顶层 verdict
python tools/ebbctl.py call view.sort '{"by":2}' --verdict
python tools/intents.py list                   # 声明式意图（配方在 tools/intents.py 的 INTENTS 表）
python tests/e2e/bookmark_move_e2e.py 坚果云    # E2E 闭环验收（落盘/审计/设置未被改）
```

**执行运行时操作时，先问通道，别扫源码。** 能力面本来就是自描述的：

| 要知道什么 | 用什么 | 不要做什么 |
|---|---|---|
| 有没有某能力、叫什么、必填参数 | `agentctl tools --grep <词>` / `ebbctl list` | 不 Grep `src/` 找 `WinMain` / 命令表 / handler |
| 现在的状态（标签/路径/选中/计数） | `ebbctl state` | 不读配置文件猜 |
| 执行一个动作 | `agentctl call <名字> '<json>'` / `ebbctl call` | 不模拟输入、不抢焦点 |

扫源码只在**改代码**时做；只是"打开磁盘 / 切换标签 / 读状态"这类操作，
查一次工具清单就能确定名字和参数，直接调。通道返回的 `verdict` 和
断言就是回读，不必再回头翻实现确认它做没做。

其他环境事实：

- 编译器 `C:\Program Files\Windhawk\Compiler\bin\clang++.exe`（20.1.3，`x86_64-w64-mingw32`），
  `-std=c++20 -O1 -g -mwindows -static-libstdc++ -static-libgcc`。
- **没有 CMake/Makefile 是刻意的**，不要引入。
- 多实例并存：`EXBARBROWSER_MULTI=1 ./ExBarBrowser.exe C:\Windows`；
  默认启动会把路径经 `WM_COPYDATA` 转给已有窗口开新标签。
- 日志：`%TEMP%\ExBarBrowser.log`（UTF-16 LE，无 BOM）。
  用 `Get-Content ... -Encoding Unicode` 看，**不要用 cmd 的 `type`**（会出 NUL 交错乱码）。

---

## 3. 代码规则（可机械检查的部分）

| 规则 | 判据 / 做法 |
|---|---|
| 单文件 ≤500 行 | `wc -l src/*.cpp` 按行数倒序看头部；超了按职责拆（`nav_bar` → `nav_bar_draw` / `nav_bar_actions`）。棘轮在 `tests/baselines/file-length-baseline.json`：涨过记账行数即报错，降下来自动收紧 |
| 一文件一职责 | 文件名能一句话说清它干什么；说不清就是两个文件 |
| **按作用域拆，不按行数拆** | `shell_ops.cpp` 的"视图作用域"与 `shell_ops_path.cpp` 的"路径作用域"是两种语义；`app_commands_files.cpp` 的 `file.*`（选中集）与 `app_commands_fs.cpp` 的 `fs.*`（点名路径）入参就不同。混在一起时读的人要翻到文件末尾才发现还有另一套语义 |
| **共用机制只有一份** | `IFileOperation` 的执行与判败只有 `shellopexec::Run()`（`src/shell_op_exec.{h,cpp}`）一处 —— 抄一半就会漏 `GetAnyOperationsAborted` |
| 一个共享头 | 只有 `common.h` 被所有 TU 包含，只放类型/常量/纯内联工具，**不放业务** |
| 依赖单向 | `app` → 各栏 / `shell_view` → `nav_model` / `theme` → `common`；下层不得反向 include 上层 |
| 无全局可变状态 | 例外只有 `App::Get()` / `Config::Get()` / `ai_server` 的管道线程状态；栏与组件内部不得有文件级可变状态 |
| 头文件自足 | `xxx.h` 用到的类型自己 include，不靠 `common.h` 的 PCH 间接提供 —— 单测单独编 `src/xxx.cpp` 会立刻暴露 |
| 文案/注释中文 | 标识符英文 |
| 文档不写行号 | 用符号名定位（行号必漂移） |
| **构建残留必须自动回收** | 隔离构建（`--objdir`/`--out`）每次留下 100 MB+ 中间目录与 54 MB exe。回收只有 `tools/reclaim.py` 一处实现，由 `build_fast.py` 收尾自动调用。**不要**再写第二个清理脚本，也不要手工 `rm -rf obj_*`/`t*`（会抽走别人正在构建的那份） |

自检头文件是否自足（不带 PCH 编一次）：

```bash
"C:/Program Files/Windhawk/Compiler/bin/clang++.exe" -std=c++20 -fsyntax-only -Isrc src/xxx.cpp
```

**新增源文件不用改任何清单**：`build_fast.py` 自动扫描 `src/*.cpp`，
排除 `*.bak.cpp` / `tmp_*` / `*_scratch*` / `_*`。文件名下划线开头 = 不参与构建。

---

## 4. 命名与命令契约

- **命名空间按领域，不按来源项目**：`win` / `ico` / `str` / `nav` / `tf` / `json` / `chord` /
  `shellops` / `ai` / `ui` / `core`。禁止 `namespace eb`（上一个项目的前缀，已整体拆解）。
- 类 `PascalCase`；函数 `PascalCase`；成员 `snake_case_`；常量 `kPascalCase`。
- **命令主名是点分小写**（`tab.close.others` / `nav.back` / `fs.delete`），第一段等于 `group`；
  驼峰旧名进 `legacyName`，**永不删**。所有读命令名的地方必须走 `FindCommand`（双向都认），
  不得直接字符串比较。
- **`CmdId` 枚举值一经分配不改**，删除命令保留枚举项标废弃，不得重排。
  基线在 `tests/baselines/commands-baseline.json`，`check_commands.py` 比对（前缀判据，能抓中间插入）。
- 窗口类名/管道名/日志名/环境变量全部从 `identity.h` 派生；**窗口类名是外部契约**
  （`scripts/shot.py` 按 `ExBarBrowser_Main` 找窗口），改名要同批改脚本与文档。
- 跨软件契约永不改名：`EXBAR_ID` / `EXBAR_ICON` / `EXBAR_FLAGS` 书签属性、
  `data/` 下文件名、`settings.json` 键名、`WM_COPYDATA` 魔数 `0xEBB1`。

### 4.1 三条"说真话"的口径（2026-11-21 起）

**① 外部路径进 shell 前必须过 `pathutil::ToShellPath`**

`SHCreateItemFromParsingName` / `SHParseDisplayName` **只认反斜杠**，
收到 `C:/dir/file.txt` 直接返回 `E_INVALIDARG(0x80070057)`；而正斜杠在
Windows 里是完全合法的路径写法（`CreateFile` 也接受），AI 也天然会这么写。

规则：**任何来自外部（AI / 脚本 / 命令行 / 配置）的路径，
在交给 shell API 之前一律 `pathutil::ToShellPath()`**（正斜杠→反斜杠、
折叠连续分隔符、去尾分隔符；`::` / `shell:` 命名空间原样）。
唯一实现在 `src/path_util.{h,cpp}`，不要另抄一份。

不归一化的后果不是"报错"，而是"报一个错的错"——"无法定位源文件
（HRESULT=0x80070057）"，使用者完全看不出真因是分隔符。

**② 声明 `undoable=1` 就要真的有撤销机制**

`Effects::undoable` 的语义是"**我们**能不能撤销"（不是"系统理论上能不能"）。
声明了却没有撤销栈 = 对调用方说谎。`tools/check_ai_meta.py` 第 25 项按
`UNDO_MECHANISM` 清单做**双向**校验：声明了必须有机制，登记了也必须真声明。

文件操作的撤销栈是**应用内自建**的（`src/file_undo.{h,cpp}`，逆操作式，
上限 20，同 `bmk.*` 口径）。两条边界规则：
- 机器**不能**撤销人工的操作（栈顶是人工操作时，机器调用直接失败）；
- 撤销失败**不弹栈**（记录还在，可重试）；部分失败时把已撤成功的项剔掉。

`Effects` 的常量在 `src/commands.cpp` 顶部（`kEffectDelete` / `kEffectRename` …），
`file.*` 与 `fs.*` 引用**同一份** —— 同义命令两套声明是 P0-11 那类缺陷。

**③ `actor` 是身份类型，不是字符串**

`ActorKind { Human, Ai }` + `Actor{kind, source}`。工厂：`Actor::Ui()` /
`Key()` / `Mouse()` / `SysMenu()` / `Palette()` / `Seq()` / `Drop()` /
`Settings()` / `Ai()`。`fromMachine()` 判的是 `kind == Ai`，
**不是**"来源字符串不是 ui/key/mouse"—— 那样会把真人输入判成机器
（P0-10：撤销边界、审计归类、分级审批全都以它为前提）。

---

## 5. 加东西的路径

**新增命令（所有动作的唯一入口）**

1. `src/commands.cpp` 的 `kTable` 加一行（`CmdId` / 点分主名 / `legacyName` / group / 中文标题 /
   `argsSchema` / `Effects` / `aiExposed`）；`src/commands.h` 的枚举**只在末尾追加**。
2. 写 handler：只做"取参数 → 调高层函数 → 返回结构化结果"；前置条件用声明式
   `Requirements` + `CanExecute()`，不在 handler 里散写判断。
3. 快捷键/序列加进 `data/keymap.json`（`hotkeys[]` / `sequences[]`），再 `gen_keymap.py`。
4. `python tools/check_commands.py` 与 `bash tests/run_all.sh commands` 必须绿。
5. 更新 `docs/PARITY.md` 状态列与 `CHANGELOG.md` 的 `[Unreleased]`。

> 大括号按位置序初始化聚合（`CommandDef` 等）之前，**必须 Read 完整 struct 定义**核对字段顺序。
> 拿 grep 字段名拼凑会把 `outputSchema` 写到 `assertion` 位：不报编译错，只在
> `check_ai_meta` 第 18 项才炸，白跑一轮全量重编。能用指定初始化器就用。

**新增快捷键**：只改数据（`data/keymap.json`）+ 重新生成 `keymap.gen.h`。
不要在 `app.cpp` 里加 `ACCEL` 表或 `WM_COMMAND` case —— 那套早已删除。

**新增自绘栏**

1. `xxx_bar.h/.cpp`：`Create(parent, theme)` / `hwnd()` / `SetXxx(...)` / `ReloadTheme()`。
2. 类名加到 `src/identity.h`，用 `win::RegisterClassOnce(...)` 注册**真实 WndProc**
   （不要"先 DefWindowProc 再 SetWindowLongPtr 换掉"）。
3. 建窗用 `win::CreateChild(...)`（内部走 `lpCreateParams` 挂 `this`，不要事后 `SetThis`）。
4. `WM_ERASEBKGND` 返回 1；`WM_PAINT` 走 `ui::PaintBuffered(hwnd, bg, draw)`，不自建离屏 DC。
5. 量字用 `ui::MeasureText(dc, theme.font, text)`，不自己 `SelectObject` + `GetTextExtentPoint32W`。
6. `App::OnCreate` 创建、`App::Layout` 定位（高度走 `theme_.S()`）、`App::ApplyTheme` 调 `ReloadTheme`。
7. 若带"唯一实现"性质，在 `tools/check_components.py` 补一条规则防复发。

**新增 AI 可调用工具**

1. 先按"新增命令"落地，并保证**有人工可达路径**（鼠标/键盘/命令面板）—— 不允许只有 AI 能触发。
2. 标 `aiExposed=true`，补 `argsSchema` 与 `risk`（`Read`/`Write`/`Danger`）；
   `description` 必须写"什么时候不要用"。
3. `Write`/`Danger` 必须能被审批拦截；每次调用写 `audit.log`（不可关闭）。
4. `docs/PARITY.md` 标注"AI 可调用"；命令 `id` 与 ExBar mod 的 `data/capabilities.json` 保持同名
   （该文件属于 mod，本仓库没有；对齐是为了让 mod 的 `keymap.json` 能原样读入）。

**新增下拉菜单**：一律 `ShowPopupList`（`src/popup_list.cpp`），不要新增 `TrackPopupMenu`。
三个必记点：`rows` 要活到函数返回；图标由调用方 `FreeRows` 释放；不要在 `WM_PAINT` 里弹。
每个调用点都要传锚点键（`nav.drop` / `nav.crumb.3` / `tab.menu.1`），用于"再点即收"。

---

## 6. 坑位（真实踩过）

| 坑 | 表现 | 处理 |
|---|---|---|
| 换目录/换机器后不 `--clean` | 每个 TU 报 `malformed or corrupted AST file` | PCH 记绝对路径，必须全量重建 |
| 手写 `clang++ a.cpp b.cpp ...` | 源文件数量大，手抄清单必然与自动扫描脱节 | 用 `build.sh` / `build_fast.py --out` |
| 单测跑错工作目录 | `load=0` + 退出码 2（像"配置坏了"） | `keymap_test`/`sequence_test` 要在 `tests/` 跑；`commands_test` 要在仓库根；`run_all.sh` 自动判 |
| 文件名叫 `_foo.cpp` | 不进构建，链接期才报未定义符号 | 下划线开头 = 故意排除 |
| 弹层"一闪就没" | 开菜单那次点击的 UP 被当成"点了外面" | 判据 `PopupUpIsOpeningRelease`；见 DEVELOPMENT §3.4 |
| 弹层"关了又开" | `WM_ACTIVATE(WA_INACTIVE)` 抢在 DOWN/UP 前关窗 | 失活按 `popup::ShouldCloseOnDeactivate` 分流；时间窗方案必然失效 |
| 中文路径导航失败 | 源文件非 UTF-8，或 `%s` 配了 `char*` | 源文件 UTF-8；宽字符格式化用 `%s` + `wchar_t*` |
| 高 DPI 文字被裁 | 某处写死像素 | 全量搜裸数字改 `theme_.S()` |
| 编过但行为没变 | 跑的是旧 exe，或启动被转发给已有窗口 | 看构建时间戳；要独立实例用 `--new-window` |
| 在 `WM_PAINT` 里写日志/弹层/建画刷 | 重绘风暴、卡死、GDI 泄漏被放大 | 日志只在事件里写；弹层只在鼠标/键盘事件里弹 |
| `EXBARBROWSER_MULTI=1` 后连不上通道 | 发现文件被写到 `profiles\multi\runtime\`，不是程序根的 `runtime\` | 用 `agentctl`（自动在 profile 根与旧 `data\` 布局里找活的端点），不要写死某一层 |
| 启动成功但下一次调用 `channel_down` | 命令结束时子进程被连带回收 | 用后台任务跑 `agentctl serve`，不要用一次性 `up` |
| `--help` 打不出用法就以为脚本坏了 | `tools/check_components.py` 没有 `--help`，任何参数都会直接跑检查 | 看脚本头注释；查规则用 `--list` |
| 给 shell 的路径忘了转义分隔符 | `SHCreateItemFromParsingName` 对 `C:/x` 返回 `E_INVALIDARG(0x80070057)`，报错却是"无法定位源文件" | 外部路径一律 `pathutil::ToShellPath()`；见 §4.1 ① |
| `fs.rename` 报"目标不在当前视图" | 目标**确实**不在视图里（路径写错 / 目标已改名），不是 bug | 先 `tool.list` 看当前目录里到底有什么 |
| 改了 `tests/` 或 `tools/` 就跑 `dev.build` | 评估面清单过期 → 信任层 fail-closed → 直接拒绝启动 | `python tools/gen_eval_baseline.py --human-confirm` **并提交**，见 §7 |
| 行数棘轮在**编辑中途**量基线 | 记账值比真正提交进去的少（如 `shell_ops.cpp` 记 1384、实际提交 1502）→ 棘轮在自己诞生的那笔提交上就报错 | 基线必须在**提交态**量：`git stash` 掉改动后量，或提交后再跑 `--update-length-baseline` |
| 抄一半 `IFileOperation` 的判败逻辑 | 删除路径查 `GetAnyOperationsAborted`、改名路径不查 → "用户取消了删除"被记成 ok=1 | 执行器只有一份：`shellopexec::Run()`（`src/shell_op_exec.{h,cpp}`），两边都调它 |

---

## 7. 收工判据（不要跳过）

- [ ] `bash build.sh` 零警告（零警告是基线，不是"尽量"）
- [ ] `agentctl.py status` 的 `stale=false` —— 验证的是本次构建的产物，不是旧 exe
- [ ] `bash tests/run_all.sh` 全绿（动到命令表 / 输入端 / AI 元数据时必跑）
- [ ] `check_commands.py` / `check_ai_meta.py` / `check_components.py` 三个全绿
- [ ] **动过 `tests/` 或 `tools/` 的话**：`python tools/gen_eval_baseline.py --human-confirm`
      重新签发评估面，**并把 `tests/baselines/eval-surface.json` 提交** ——
      清单的信任锚就是"已被 git 跟踪 + 工作树干净"，没提交的话
      `dev.build` / `dev.test` 会 fail-closed 拒绝启动（这是对的，不是 bug）
- [ ] 没有新增模拟输入；新脚本头注释写了"不发合成输入，只读；不动光标、不抢焦点"
- [ ] 改到交互路径的：手点一遍；深/浅色、100%/150% DPI 各看一次；必要时 `scripts\shot.py` 留图
- [ ] `CHANGELOG.md` 的 `[Unreleased]` 有对应条目
- [ ] 提交：约定式提交 + 中文正文，`scope` ∈ `app/shell/tabs/nav/status/theme/cmd/ai/input/build/scripts/docs`；
      一条提交只做一件事，格式化/重命名/逻辑改动不混提
- [ ] `git status` 里没有 exe / png / log / backup（已在 `.gitignore`）

---

## 8. 闭环流程（提需求 → 跑命令 → 补能力 → 再验证）

**闭环的主体是产品内的结构化通道，不是任何外部脚本。** 命令经 `CommandBus` 执行，经命名管道
以 JSON-RPC 收发；判定读 `verdict` / `assertions` / `errorCode`；能力缺口由
`exbar/capabilities` 回答。`tools/agentctl.py` 只解决一件产品不该管的事 ——
把 exe 拉起来并保活（换 exe 必然跨进程，任何进程内通道都做不到）。

| 所谓"结构化" | 具体落在哪 |
|---|---|
| 命令有稳定名 + 参数 schema | `tools/list` 的 `name` / `legacyName` / `inputSchema` |
| 动作只有一条路径 | `CommandBus`（AI、快捷键、菜单、命令面板同源，R5/R16） |
| 结果可判定 | `ok` / `verdict` / `assertions` / `structuredContent` |
| 失败可分支 | `errorCode`（10 类，见 `docs/AI-INTERFACE.md` §3） |
| 缺什么能力 | `exbar/capabilities` 的 `notImplemented` / `notExposed` |
| 谁做的、可追溯 | `actor` + `audit.log` + `events/subscribe` |

### 8.1 固定开场

```bash
python tools/agentctl.py serve --wait 30    # 仅进程生命周期：构建(仅过期)+启动+保活；后台任务跑
python tools/agentctl.py status             # 必须看到 channel_ready=true、stale=false
python tools/agentctl.py tools --grep bmk   # 通道可用后，一切以 tools/list 为准
```

> 通道起来之后，判定与诊断都直接读产品字段：`ok` / `verdict` / `assertions` / `errorCode`，
> 能力清单读 `exbar/capabilities`。`agentctl gap` 只是"读 errorCode + 翻成改哪几处"的便利壳，
> 它不再猜中文串 —— 分类来自 `src/ai_api.cpp` 的 `errorCode`。

三个为什么：

- **必须后台跑 `serve`**：本环境会回收命令的子进程。一次性 `up` 虽然能起来，但命令一结束实例就没了，
  下一次调用只会得到 `channel_down`。`serve` 靠父进程常驻把实例按住；`up` 会在 2 秒后自检并在
  `launch_not_persistent` 里说清这一点。
- **`serve` 用 `EXBARBROWSER_MULTI=1` + `--ai`**：MULTI 才不会被"路径转发"给用户已开的窗口
  （否则你测的是用户的实例，不是本次构建的产物）；同时 profile 根切到 `profiles\multi`，
  六类数据（data/state/logs/cache/runtime）整套隔离，不会改用户的 `settings.json`。
  `agentctl` 会在新 profile 根和旧 `data\` 布局里找活的那份。
- **先 `tools --grep` 再动手**：命令名是点分名，猜错就是一次无谓的失败往返。

### 8.2 六步循环

| 步 | 动作 | 判据 |
|---|---|---|
| 1 | 需求 → 命令。`tools --grep` 没有命中就先读 `capabilities` 分清"没这条 / 没开放 / 没实现"；能用 `tools/intents.py` 的声明式意图就用，别写一次性脚本 | 命中 `tools/list` 或落到某张缺口清单 |
| 2 | 执行：`agentctl.py call <name> <json>`（危险命令**由人**批） | 读四样：`ok`、`verdict`、`assertions`、`errorCode` |
| 3 | 判缺口：有 `errorCode` 就直接按 §8.3 分流；没有（老版本）才用 `agentctl.py gap` | 拿到 `errorCode` + `fix` |
| 4 | 补能力：按 §8.3 分流，**一次只改一处** | 改动可指到具体文件/表行 |
| 5 | 重验：`serve` 会重建；若 `status` 仍 `stale=true`，说明 exe 没换，先解决 | `verdict=true` |
| 6 | 留痕：`CHANGELOG.md` + 三个静态检查 + `tests/run_all.sh` | 全绿 |

### 8.3 缺口分类（`errorCode`，由产品给出）

| `errorCode` | 含义 | 补法 |
|---|---|---|
| （通道不通） | 没有实例 / 发现文件是残留 | 后台跑 `serve`；仍不通看 `%TEMP%\ExBarBrowser.log` 的 `[init]` `[ai]` |
| `unknown_tool` | 命令表里根本没有 | 走 §5「新增命令」；`CmdId` 只在末尾追加 |
| `not_exposed` | 有实现、没进 `tools/list` | 补 `aiExposed` / `argsSchema` / `risk`，跑 `check_ai_meta.py` |
| `not_implemented` | 有表项、没 handler | 写 handler 并注册，再把 `implemented` 置 true |
| `bad_args` | 缺必填、多传、类型错 | 改**请求**：`tools --grep <name>` 看 `inputSchema`；不要改代码迁就调用 |
| `schema_invalid` | 命令表的 `argsSchema` 写错了 | 修 `src/commands.cpp` 那一行（实现缺陷，不是调用方的错） |
| `danger_disabled` / `need_approval` | 危险命令未获人类批准 | 把命令+参数交给用户等明确同意。**AI 不给自己加 `--approve`** |
| `precondition_unmet` | 前置条件不满足（error 里是具体原因），含"作用于当前选中而现在没选中" | 先读状态、改目标或先做前置动作，不是 bug。长期要"指定目标"就加确定式命令（`Op*`/`Fs*` 模式）；`needsSelection` 是命令的属性（见 `tools/list`），不是失败码 |
| `lock_conflict` | 执行期被占用/被锁（剪贴板、文件被别的程序占着） | 关掉占用方后重试，或换一个目标。**重试有意义** |
| `canceled_by_user` | 用户主动取消（拒绝批准、在进度窗口点了取消） | **不要重试** —— 停下来问用户。绕过用户的决定比失败更糟 |
| `not_supported` | 该环境/该对象不支持这件事（视图模式不对、能力不存在） | 换一条路。**重试永远没用** |
| `timed_out` | 等待时限到了，后台作业**仍在跑**（不是失败） | 用 `tool.job.get` 继续查那个 jobId，别重发同一条命令；要停用 `tool.job.cancel` |
| `failed` | handler 真失败 | 看 `error` 与日志对应模块行 |
| `effect_not_met` | `ok=true` 但 `verdict=false` | 读 `assertions`；先独立回读，再决定修 handler 还是修判据 |
| （`error: timeout`） | 10 秒超时，UI 卡在模态对话框 | 请人关掉对话框；**不要循环重发** |

### 8.4 停止规则（比"继续重试"更重要）

- **同一 `errorCode` 连修 2 次仍不通过 → 停下报告**。继续改只会把问题埋得更深。
- `danger_disabled` / `need_approval` → 必须问人，不得自我批准破坏性操作。
- `timeout` → 不重试，请人关对话框。
- `stale` 反复为真、且 `src_newest` 每次都是不同文件 → 有别的会话在同时改代码。
  报告并让出，**不要跟它抢**（会把对方的改动编译进你的验证结论）。
- `verdict=false` 但独立回读显示已生效 → 修**判据**，不改实现（判据错也是 bug）。

### 8.5 判定优先级

```
verdict > ok           # ok 只说明"命令跑了"，verdict 才是"效果对了"
独立回读 > 命令自报     # 不信意图自报，回读 bmk.list / state 再下结论
磁盘与审计 > 内存状态   # bookmarks.html 落盘、audit.log 有记录，才算真的做完
```

每次迭代必须留下 `actor=ai` 的审计条目；没有审计的"成功"不认。

---

## 9. 文档索引（按问题找）

| 想知道 | 读 |
|---|---|
| 定位、边界、已实现能力、缺口 | [README.md](README.md) |
| 硬规则 R1–R21、目标对象模型与依赖方向、变更流程 | [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) |
| 现状结构与不变量 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 怎么编、怎么跑、怎么调、怎么加代码 | [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) |
| 名字怎么起、哪些名字不许改 | [docs/NAMING.md](docs/NAMING.md) |
| 为什么这么设计（决策记录） | [docs/DECISIONS.md](docs/DECISIONS.md) |
| 比 ExBar mod 少了什么 | [docs/PARITY.md](docs/PARITY.md) |
| 迁移批次与路线 | [docs/MIGRATION-PLAN.md](docs/MIGRATION-PLAN.md) / [docs/ROADMAP.md](docs/ROADMAP.md) |
| AI 通道协议（JSON-RPC over 命名管道） | [docs/AI-INTERFACE.md](docs/AI-INTERFACE.md) |
| 输入语义（键/序列/鼠标） | [docs/INPUT.md](docs/INPUT.md) |
| 提交规范与验证清单 | [CONTRIBUTING.md](CONTRIBUTING.md) |
| 变更历史（事实上的工作日志） | [CHANGELOG.md](CHANGELOG.md) |

> 一致性约定：`docs/*.md` 带 front-matter（`status: current|reference|archived` + `baseline`）。
> 状态变了就改 `status`，不要留"过时但没人知道"的文档。
> 所有规模数字都是**快照**且必须标口径；与代码不符时**先信 `src/`**，并顺手把文档改对。
