---
status: current
baseline: 2026-11-21 11:00
---

# 开发与调试

> 本文只讲"怎么把它编出来、跑起来、看进去"。代码结构与不变量见 [ARCHITECTURE.md](ARCHITECTURE.md)，
> 提交与规范见 [../CONTRIBUTING.md](../CONTRIBUTING.md)。
>
> **动手前先记住 R21（输入纪律）**：禁止模拟鼠标/键盘（`SendInput` / `mouse_event` /
> `keybd_event` / `SetCursorPos` / `BlockInput` / `AttachThreadInput`），也禁止用
> `PostMessage`/`SendMessage` 投 `WM_LBUTTON*` / `WM_KEY*` / `WM_CHAR` —— 合成点击在本程序里
> 必然走到 `ShowPopupList` → `SetForegroundWindow`，与 `SendInput` 结果相同（详见 §2.4）。
> 自动化脚本也不得改用户的前台窗口。所有操作走结构化命令（`CommandBus` 或 AI 管道），
> 验证走单测 / 截图 / 人工点+脚本观察。
> 详见 [ARCHITECTURE-RULES.md §3.1](ARCHITECTURE-RULES.md)，机器校验在 §2.4。

---

## 1. 工具链

| 项 | 值 | 备注 |
|---|---|---|
| 编译器 | `C:\Program Files\Windhawk\Compiler\bin\clang++.exe` | 20.1.3，随 Windhawk 安装 |
| 目标三元组 | `x86_64-w64-mingw32` | 交叉编译，产物是原生 PE32+ |
| 语言标准 | `-std=c++20` | 用到 `std::wstring`、`std::function`、`unique_ptr`，无协程/概念 |
| 优化/调试 | `-O1 -g` | `-O1` 保证调试时变量不被过度优化；`-g` 便于 `.map`/栈回溯 |
| 子系统 | `-mwindows` | 无控制台窗口；入口是 `WinMain`（不是 `wWinMain`） |
| 运行库 | `-static-libstdc++ -static-libgcc` | 单 exe 零依赖 |
| 构建系统 | `build_fast.py` | Python 驱动的增量构建（PCH + 分 TU 缓存 + 并行）；`build.sh`/`build.bat` 是薄包装 |
| Python | 3.x | 仅构建期依赖，运行产物是零依赖单 exe |

**没有 CMake / Makefile 是刻意的**：保持无第三方构建系统的简洁，但用约 420 行 Python 实现增量，
解决"改一行全量重编"的痛点（实测单命令全量 88s → PCH 增量：全量 10.5s、改一行 ~1s、无改动 0.3s）。
清单只有**一份**（`build_fast.py` 顶部的 `EXCLUDE_PATTERNS`/`COMPILE_FLAGS`/`LIBS`），
`build.sh`/`build.bat` 只做参数透传，不存在多套清单漂移（见 §3.1）。源文件由 `src/*.cpp` **自动扫描**
（当前 60 个 TU），头文件依赖用 clang `-MMD` 精确追踪。

> **PCH 记绝对路径**：`obj/common.h.gch` 是 clang AST 二进制，内含生成时的源文件绝对路径。
> 仓库换目录/换机器后必须 `bash build.sh --clean`，否则每个 TU 都报
> `malformed or corrupted AST file`（2026-11-20 从 `D:\Windhawk\ExBarBrowser` 搬到
> `E:\ExBarBrowser` 时踩过）。

### 1.1 链接库与用途

| 库 | 用在哪 |
|---|---|
| `ole32` / `oleaut32` / `uuid` | COM 初始化、`CoCreateInstance`、`CoTaskMemFree`、CLSID/IID |
| `user32` / `gdi32` | 窗口、消息、自绘 |
| `shell32` / `shlwapi` | `SHCreateItemFromParsingName`、`SHGetFileInfo`、`PathRemoveFileSpec`、`StrCmpLogicalW` |
| `comctl32` | `InitCommonControlsEx`（shell 视图内部要用） |
| `dwmapi` | 深色标题栏 `DwmSetWindowAttribute` |
| `uxtheme` | `SetWindowTheme` + 未文档化序号 135/133 |
| `advapi32` | `RegGetValueW` 读系统深浅色 |

新增系统 API 前先想清楚：**能不能用已有库里的等价函数**。加库只需改 `build_fast.py` 的 `LIBS`
一处，再同步本文档与 README；两个入口脚本无需改动。

---

## 2. 构建

```bash
cd ExBarBrowser
bash build.sh          # Git Bash
build.bat              # cmd
```

两个入口都把参数透传给 `build_fast.py`。常用参数：

```bash
bash build.sh --clean           # 清空 obj 全量重建（CI / 排障）
bash build.sh -j 16             # 指定并行数（默认 8）
bash build.sh --stats           # 打印每个 TU 耗时
bash build.sh --no-pch          # 禁用 PCH（排查 PCH 相关问题）
bash build.sh --out /tmp/x.exe  # 输出到指定路径
```

成功输出 `[完成] 总耗时 N 秒 -> ExBarBrowser.exe`；失败输出 `BUILD FAILED` 或 `[编译失败]` 并返回非 0。

### 2.1 运行中的实例不再是障碍

旧版直接覆盖 `ExBarBrowser.exe`，遇到运行中的实例会报 `Permission denied`。现在构建流程是：

1. 每个 `.cpp` 独立编到 `obj/xxx.obj`（不受 exe 占用影响）；
2. 链接到临时文件 `ExBarBrowser.new.exe`；
3. 成功后用 `os.replace` 原子替换目标 exe。

若替换时 exe 仍被占用，**新版本保留为 `ExBarBrowser.new.exe` 并给出提示**，不会破坏正在运行的程序。
关掉实例后重跑一次即可完成替换（此时 obj 已缓存，只需 ~0.3s 重新链接）。
只想验证"能不能编过"而不打断手头实例，用 `--out`：

```bash
bash build.sh --out /tmp/ebb_check.exe
```

### 2.2 增量机制与正确性

`build_fast.py` 默认增量，只有变化才重编。判定依据：

- 每个 `.cpp` 经 `-MMD -MF` 生成 `obj/xxx.d` 依赖文件；
- 源码或**任一依赖头文件**比 `.obj` 新 → 重编该 TU（见 `obj/xxx.d`）；
- `common.h` 变化 → PCH 重建 → 所有 TU 重编；
- 编译选项/库/PCH 开关变化 → 配置指纹（`.build_sig`）失配 → 全量重编。

头文件依赖被精确追踪，不会"改了头文件忘了重编依赖方"。若怀疑缓存脏了，`--clean` 强制全量。
实测：全量（冷）6–8s，改一行 0.7–1.1s，无改动 0.3s，链接 0.2–0.3s。

### 2.3 构建产物

| 文件 | 说明 |
|---|---|
| `ExBarBrowser.exe` | 单文件可执行，约 12 MB（含 `-g` 调试符号；strip 后约 3.5 MB） |
| `debug.log` | 不是本程序写的（本程序只写 `%TEMP%\ExBarBrowser.log`）；可删 |

`*.exe` / `*.png` / `*.log` / `backup/` / `.old/` 已在 `.gitignore` 排除，不要提交。

### 2.4 自测（不需要 GUI）

> **规则 R21：禁止模拟鼠标点击、禁止抢焦点、禁止循环点击 —— 一条不让。**
> 应用是本机正在使用的窗口：`SetCursorPos` + `mouse_event` / `SendInput` 会抢走光标、
> 把焦点从用户手上夺走，`ShowPopupList` 这类模态循环还会把整个程序卡住等输入。
>
> **`PostMessage` 投鼠标/键盘消息同样禁止**（`WM_LBUTTONDOWN` / `WM_KEYDOWN` / `WM_CHAR` 等）。
> 它看起来"只是发消息"，但合成点击在本程序里必然命中控件、走到 `ShowPopupList`，
> 而弹层内部会 `SetForegroundWindow` —— 结果与 `SendInput` 完全相同。
> "扫描一行坐标、每隔几像素投一次点击"这种循环最容易造成"用户正在打字，字却进了别人家的窗口"。
>
> 这类代码不报错、不崩溃，只是**悄悄改变用户下一次点击的含义**，所以在
> `tools/` `scripts/` `tests/` 里也一并禁止（`tools/check_components.py` 的
> `INPUT_GUARD` 段，4 条规则、跨目录扫描）。完整清单与替代方案见
> [ARCHITECTURE-RULES.md §3.1](ARCHITECTURE-RULES.md)。
>
> 合规的验证方式，按优先级：
>
> | 要验证什么 | 用什么 |
> |---|---|
> | 逻辑/几何/状态机 | 抽出纯函数跑单测（下面的 `popup_layout_test` / `popup_toggle_test` 就是为此补的） |
> | 一条动作能不能跑通 | 进程内 `CommandBus::Invoke`，或进程外连 AI 命名管道发 `tools/call`（见 [AI-INTERFACE.md](AI-INTERFACE.md)） |
> | 界面长什么样 | `python scripts\shot.py out.png`（`PrintWindow` 只读，被遮挡也能抓，不动光标） |
> | **必须真点一下才知道的流程**（弹层开合、拖拽） | **人工点，脚本只观察**：`python tools/probe_toggle.py` 只数弹层窗口数，点击由人做 |
>
> 确实需要给某个窗口喂消息时，只投**非输入**消息（`WM_CLOSE` / `WM_THEMECHANGED` / `WM_APP_*`），
> 且只用于验证 `WndProc` 分支本身，不要拿它伪造本该由命令总线产生的业务动作。

**一条命令跑全部单测（推荐）**：

```bash
bash tests/run_all.sh            # 当前 32 个单测，32/32 通过约 40 秒
bash tests/run_all.sh keymap     # 只跑名字含 keymap 的
```

它先跑一次 `bash build.sh` 刷新 `obj/`，然后**复用这批 obj** 链接每个测试
（排除 `main.obj` / `*_test.obj` / `*.check.obj`），所以：
- 不需要维护"每个测试要链哪些 `.cpp`"的依赖表（那种表必然过期，这正是下面那些长命令的由来）；
- 每条测试的链接只要 ~3 秒，而不是把 60 个 `.cpp` 重编一遍的 3 分钟；
- 依赖关系由主构建保证，改 `src/` 后不会出现"文档里的命令突然链接失败"。

> 为什么以前没人跑全量：每个测试都得手抄一串 `.cpp`，抄错就链接失败，于是只跑记得住的那几条。
> `tests/keymap_test.cpp` 因此**红着很久没人发现**（硬编码 70 条，实际早已 84），
> 见 [CHANGELOG](../CHANGELOG.md) 的"文档全量对账"条目。

下面是最常用的几条**手工**命令（想单独调一个测试时用）。每一条都实测过；
如果你只想知道"现在全绿吗"，用上面的 `run_all.sh` 就够了。

```bash
CC="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"

# 通用编译开关。以前这段文档只给 -Isrc，照抄会撞上宽字符 API 与 GDI 常量缺失。
FLAGS="-std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc"
# 常见库集合。单测用不到的库多链不会报错，缺了才会 —— 所以给一份"够用"的清单。
LIBS="-static -static-libstdc++ -static-libgcc -lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luxtheme -ldwmapi"

# 键位表：必须输出 hotkeys=84 sequences=38
#   依赖链：LoadKeymap → tf::ReadText → str::FromUtf8Strict → LogDbg → win::ThemeControl
#   少链任何一个都链接失败。早期版本漏了 str/log/platform_win，照抄必挂。
"$CC" $FLAGS tests/keymap_test.cpp src/keymap.cpp src/json.cpp src/text_file.cpp \
  src/str.cpp src/log.cpp src/platform_win.cpp $LIBS -o /tmp/keymap_test.exe
(cd tests && /tmp/keymap_test.exe)

# 序列状态机：前缀 / 超时 / 取消 / 候选
"$CC" $FLAGS tests/sequence_test.cpp src/sequence.cpp src/keymap.cpp src/json.cpp \
  src/text_file.cpp src/str.cpp src/log.cpp src/platform_win.cpp $LIBS -o /tmp/sequence_test.exe
(cd tests && /tmp/sequence_test.exe)

# 命令目录：keymap 引用的每条命令必须存在于目录（missingFromKeymap=0）
#   额外要 cmd_naming（FindCommand 双向解析），漏了报 AliasOf/LegacyOf 未定义。
#   注意：本测试读的是 "data/keymap.json"（相对仓库根），**必须从仓库根跑**，
#   不能像 keymap_test 那样 cd tests（那个读 "../data/keymap.json"）。
"$CC" $FLAGS tests/commands_test.cpp src/commands.cpp src/keymap.cpp src/json.cpp \
  src/text_file.cpp src/config.cpp src/str.cpp src/log.cpp src/platform_win.cpp \
  src/cmd_naming.cpp $LIBS -o /tmp/commands_test.exe
/tmp/commands_test.exe

# 弹层行高：分隔线 2px、普通行 itemH、总高累加（不需要窗口）
#   要 icon_glyph（行高常量依赖 ico::Glyph 的字形度量）
"$CC" $FLAGS tests/popup_layout_test.cpp src/icon_glyph.cpp $LIBS -o /tmp/popup_layout_test.exe
/tmp/popup_layout_test.exe

# 再点即收：开关/异键/嵌套/空键/失活判据（不需要窗口，纯逻辑）
"$CC" $FLAGS tests/popup_toggle_test.cpp src/popup_toggle.cpp $LIBS -o /tmp/popup_toggle_test.exe
/tmp/popup_toggle_test.exe

# 清除历史：清完只剩当前目录一条、idx 收敛、前后退不可用（不需要窗口）
#   要 nav_history（History::Goto/ClearKeepingHere/Request 的实现）
"$CC" $FLAGS tests/clear_history_test.cpp src/nav_history.cpp $LIBS -o /tmp/clear_history_test.exe
/tmp/clear_history_test.exe

# 导航栏输入语义：UNC/命名空间分段、粘贴路径剥引号、盘根与 UNC 的 Tab 补全切分
"$CC" $FLAGS tests/nav_input_test.cpp $LIBS -o /tmp/nav_input_test.exe
/tmp/nav_input_test.exe

# 导航模型显示：此电脑/回收站的地址文本与面包屑显示友好名（不是 ">::>"）
"$CC" $FLAGS tests/nav_model_test.cpp src/nav_model.cpp $LIBS -o /tmp/nav_model_test.exe
/tmp/nav_model_test.exe

# 统一显示名：历史/最近访问不显示裸 "::{GUID}"
#   依赖链：ico::DisplayName → SHLoadIndirectString / SHParseDisplayName → ui::StockIcon
"$CC" $FLAGS tests/display_name_test.cpp src/icon_resolve.cpp src/ui_draw.cpp \
  src/icon_glyph.cpp src/text_file.cpp src/str.cpp src/log.cpp src/platform_win.cpp \
  $LIBS -luser32 -lgdi32 -o /tmp/display_name_test.exe
/tmp/display_name_test.exe

# 静态对账：表 / keymap / 处理器三方一致 + keymap.gen.h 是否过期（不需要编译）
python tools/check_commands.py
```

> 以上每一条都在 2026-11-21 09:05 实测跑过（退出码 0）。**改动 `src/` 的依赖关系后要重测一遍**
> —— 单测是直接链 `.cpp` 的，抽走一个符号就断，而主构建未必能暴露（PCH 会把缺失的
> `#include` 补齐）。
>
> **工作目录不统一，是这里最容易踩的一脚**：
> | 测试 | 从哪跑 | 为什么 |
> |---|---|---|
> | `keymap_test` / `sequence_test` | **`tests/`** | 默认路径写的是 `L"../data/keymap.json"` |
> | `commands_test` | **仓库根** | 写的是 `L"data/keymap.json"` |
> | 其余 | 哪都行 | 不读数据文件 |
>
> 写错目录的表现是"`load=0` + 退出码 2"，不是崩溃 —— 容易误判成"配置坏了"。
> `run_all.sh` 会自动判：源码里出现 `L"../data/` 就去 `tests/` 跑，否则留在仓库根。
> （判据必须看**字符串字面量**而不是注释 —— `commands_test` 的头注释里写着一句
> "不是 `../data/...`"，按注释判会把目录判反，这一点在写脚本时真的踩过。）

`commands_test` 与 `check_commands.py` 是"静默丢键"的守门员：keymap 里绑了目录里不存在的
命令时，绑定阶段会被静默跳过（按了没反应、管理器里也看不到）。改命令目录或 keymap 后必跑。

**改默认键位的唯一入口**是 `data/keymap.json`（与 ExBar mod 的 `data/defaults/keymap.json`
同格式，当前内容一致）。`src/keymap.gen.h` 由它生成，是"配置文件缺失/损坏"时的兜底默认表；
手改生成头会在下一次 `python tools/gen_keymap.py` 或 `check_commands.py` 时被覆盖/报错。

```bash
python tools/gen_keymap.py            # 由 data/keymap.json 重新生成 src/keymap.gen.h
python tools/gen_keymap.py --check    # 只校验是否过期（CI 用，过期退出码 1）
```

GUI 冒烟：先关掉正在运行的 ExBarBrowser（否则链接报 `Permission denied`），
`EXBARBROWSER_MULTI=1 ./ExBarBrowser.exe C://Windows` 可与已有实例并存；
**动作由人做**（不注入键鼠，见 §2.4），证据只读：
检查 `%TEMP%\ExBarBrowser.log` 的 `[status]`（状态栏取数）与 `[seq]`/`[key]`（输入端），
必要时 `python scripts\shot.py out.png` 留一张图。

---

## 3. 加代码

### 3.1 新增源文件

1. 写 `src/xxx.h` + `src/xxx.cpp`（≤500 行，一文件一职责）。
2. **不用改任何清单**：`build_fast.py` 用 `discover_sources()` 自动扫描 `src/*.cpp`，
   只排除 `EXCLUDE_PATTERNS`（`*.bak.cpp` / `tmp_*` / `*_scratch*` / `_*` 开头）。
   所以文件名以下划线开头 = 不参与构建，适合放试验代码。

```python
# build_fast.py
EXCLUDE_PATTERNS = [
    "*.bak.cpp", "*.orig.cpp",          # 手工备份
    "tmp_*", "*_scratch*", "_*",        # 临时试验文件（下划线开头 = 不参与构建）
]
```

> 早期版本这里是手写 `SOURCES` 列表，文件数过 50 后已成必冲突改动，且冲突解决不当时会
> 静默漏文件（直到链接期才报未定义符号）。改成自动扫描后，多会话并行新增文件零冲突。

3. 若引入新系统库，在 `build_fast.py` 的 `LIBS` 加 `-lxxx`。
4. `README.md` §6 目录树 + `docs/ARCHITECTURE.md` 模块表同步；新模块若带"唯一实现"性质，
   在 [tools/check_components.py](../tools/check_components.py) 里补一条禁令，防止后来人写第二份。

漏加清单已不可能，但**排除表会误伤**：文件名叫 `_foo.cpp` 不会进构建，链接期才暴露。

**新增头文件必须自足**：PCH（`common.h`）会替所有 TU 补上 `<windows.h>` / `<string>` / `<vector>`，
所以"`src/xxx.cpp` 只 include 了自己的头却用了 `std::wstring`"这类缺口在正常构建里**不会报错** ——
一旦该文件被单独编进单测（`tests/*.cpp` 有些就是直接链 `src/xxx.cpp`），立刻链接失败。
真实案例：`log.cpp` 用了 `std::wstring` 却没 `#include <string>`，靠 PCH 编过了几个月，
2026-11-20 被单测链接暴露。**自检方法**：把该文件从测试里单独编一次，
`clang++ -fsyntax-only -Isrc src/xxx.cpp`（不带 `-include-pch`）。

### 3.2 加日志

统一用 `log.h` 的 `LogDbg(L"[模块] ...", ...)`（UTF-16 LE 追加到 `%TEMP%\ExBarBrowser.log`，
超过 4 MB 轮转为 `.log.1`）。**唯一实现**在 `src/log.cpp`；`shell_view.cpp` 里的 `LogLine`
已经是转发别名，语义完全相同 —— 新代码用哪个都行，但不要自己写第三份
（历史上有两份轮转策略不同的实现，其中一份完全不轮转，能把磁盘写满）。

约定：

- 行首带 `[模块]` 前缀：`[init]`、`[nav]`、`[tab]`、`[theme]`。
- HRESULT 一律 `0x%08lX`，便于搜索 `hr=0x8`。
- 路径用 `%s`（宽字符），不要转窄字符——中文路径会丢。
- **不要**在 `WM_PAINT`、`WM_MOUSEMOVE` 里写日志。

查看：

```powershell
Get-Content "$env:TEMP\ExBarBrowser.log" -Encoding Unicode -Tail 20
```

**不要用 `cmd` 的 `type`**：文件是 UTF-16 LE 且**无 BOM**，`type` 会按 ANSI 逐字节输出，结果是
每个字符后面跟一个 NUL 的乱码（`^@[^@n^@a^@v^@]`）。VS Code 打开时也要手动选 `UTF-16 LE`。

若日志里出现中文乱码而不是 NUL 交错，说明写入端格式串用错了（`%s` 配 `char*`，或源文件不是 UTF-8）。

### 3.3 调试 shell 视图

`IExplorerBrowser` 创建的窗口树不归我们管，出问题时用外部工具看：

| 工具 | 看什么 |
|---|---|
| Spy++ / WinSpy / `probe` 下的树扫描脚本 | `ExplorerBrowserControl` 是否存在、`NamespaceTreeControl` 是否创建、尺寸是否为 0 |
| 任务管理器 | 标签数 vs 内存增长（判断是否泄漏视图） |
| `%TEMP%\ExBarBrowser.log` | `Initialize hr` / `parse hr` / `browse hr` 三步哪步失败 |
| Spy++ 抓 `ExBarBrowser_Popup` | 弹层位置/尺寸/子 `LISTBOX` 行高；行高不对说明 `LB_SETITEMHEIGHT` 没生效 |

常见 HRESULT 速查：

| HRESULT | 含义 | 常见原因 |
|---|---|---|
| `0x80070057` `E_INVALIDARG` | 参数非法 | 路径含正斜杠；`Initialize` 参数错（见 ABI 补丁） |
| `0x80070002` `ERROR_FILE_NOT_FOUND` | 找不到 | 路径不存在 / 已删除 |
| `0x80070005` `E_ACCESSDENIED` | 拒绝访问 | 无权限的目录、被占用的网络位置 |
| `0x80004005` `E_FAIL` | 未指定失败 | shell 内部失败，通常要配合视图树一起看 |

### 3.4 加 / 改下拉菜单

所有下拉都走 `ShowPopupList`（`src/popup_list.cpp`），三步：

```cpp
std::vector<PopupRow> rows;
rows.push_back({L"Windows", L"C:\\Windows", IconFor(L"C:\\Windows"),
                /*folder=*/true, /*separator=*/false, /*disabled=*/false, /*data=*/0});
// 末尾 "nav.drop" 是锚点键（见下"再点即收"）
int pick = ShowPopupList(hwnd_, *theme_, screenPt, rows, 180, false, 20, L"nav.drop");
if (pick >= 0) onNavigate_(rows[pick].path);
FreeRows(rows);                                                  // 图标必须由调用方销毁
```

坑位清单：

| 坑 | 现象 | 正确做法 |
|---|---|---|
| 传了客户区坐标 | 弹层出现在屏幕左上/别处 | `POINT pt{x,y}; ClientToScreen(hwnd_, &pt);` 再传 |
| `rows` 是临时对象 | 崩溃或乱码（弹层只存指针） | 用局部 `std::vector`，活到函数返回 |
| 忘了 `FreeRows` | HICON 泄漏（任务管理器 GDI 对象数持续涨） | 每个 `IconFor` 都要有对应 `DestroyIcon` |
| 在 `WM_PAINT` 里弹 | 界面卡死/重绘风暴 | 只在鼠标/键盘事件里弹 |
| 弹层"一闪就没" | 松手瞬间自己关了 | 见下"开菜单那次点击的松开"；这是导航栏下拉最常见的症状 |
| 行高不对 | 文字被裁/行过高 | `LB_SETITEMHEIGHT` 在 `LB_ADDSTRING` 之前设置；`PopupState::itemH` 用 `theme.S(24)` |
| **再点一次又弹出来** | 箭头变回 ›，列表却还是弹出来 | 见下"再点即收"；**不要在调用点自己记 `lastOpenTick_`** |

#### 开菜单那次点击的松开（"一闪而过"）

**症状**：点导航栏任一下拉，弹层出现后立刻消失，肉眼就是"闪一下"。

**真因**：导航栏各下拉在 **`WM_LBUTTONDOWN`** 里弹出（与资源管理器一致：按下即出，
可以一路按住拖到想要的项上再松手）。弹层一创建就 `SetCapture(list)`，于是
**同一次点击的松开**（`WM_LBUTTONUP`）会送到列表，而光标还停在锚点上 ——
锚点在列表**之外**。旧逻辑把"UP 落在客户区外"一律当成"点了外面该收起"，
菜单就在松手瞬间自己关掉了。

关键认识：**这一次 UP 不是新点击，它是开菜单那次点击的后半截。**
`TrackPopupMenu` 也是先等这一下松开，之后才把点击当作"关闭"。

**判据**（`popup_list.h` 的 `PopupUpIsOpeningRelease`，纯函数可单测）：
只有在**开菜单之后再没有按下过**（`downInList` / `downOutside` 都是 false）、
且这笔 UP 落在客户区外时，才认定它是收尾并忽略。一旦用户重新按过（无论内外），
后续 UP 都是真实点击，照常判定。

右键一并接管（`WM_RBUTTON(DOWN|UP)`）：右键菜单开着时用户的下一手可能是右键点别处，
不接管这条消息会漏给 ListBox 默认处理，行为不可控。右键落在菜单内**只关菜单不选行**
（与 `TrackPopupMenu` 同语义 —— 右键是"另起一次操作"，不该顺手执行当前项）。

#### 再点即收（`toggleKey`）

契约：**弹层开着时点它的锚点，收起；不重开。**

**真因是关闭时机，不是"连点判据"。** 系统在派发点击之前**同步**给弹层发
`WM_ACTIVATE(WA_INACTIVE)`。若在此刻 `PostMessage(WM_CLOSE)`，这条消息会排在
`LBUTTONDOWN/LBUTTONUP` 之前被模态循环取走：

```
① 点书签栏/面包屑 → 系统同步发 WM_ACTIVATE(WA_INACTIVE)
② 弹层在此 PostMessage(WM_CLOSE) → 入队
③ 这次点击的 LBUTTONDOWN / LBUTTONUP 才入队
④ 模态循环先吃 WM_CLOSE → DestroyWindow → 捕获释放
⑤ 剩下的 LBUTTONUP 落到锚点控件
⑥ 各栏都在 UP 里执行动作 → 又调一次 Show* → 菜单"关了又开"
```

**关键事实：鼠标捕获不阻止激活切换。** 弹层是 `WS_POPUP` 顶层窗口，点主窗任一子控件
都会让主窗激活、弹层失活，哪怕捕获仍在弹层手里。前几轮修法假定"持捕获就安全"，
那个前提不成立 —— 所以基于它的时间窗（`400ms`）方案必然失效：用户是"看一眼再点"，
间隔 1~2 秒，窗口早已过期。

正确做法（对齐 `TrackPopupMenu` / 组合框下拉）：**失活时按判据分流**
（`popup::ShouldCloseOnDeactivate`，见 `popup_toggle.h`）：

| 条件 | 处置 |
|---|---|
| 持捕获 + 前景窗同线程（点的是本程序另一个窗口） | **不关**。这组 DOWN/UP 由 `ListSubclass` 吞着，等 UP 判到"点在列表外"再关。锚点收不到点击，重开在消息层不可能发生 —— 与隔多久点无关 |
| 其余（点别的程序 / Alt+Tab / 捕获已被抢走） | 关。这类失活不会再有 UP 回到手上，留着就是关不掉的菜单 |

配套两条：

- `ListSubclass::WM_CAPTURECHANGED` 区分"LISTBOX 自己 SetCapture"（忽略）与
  "被别人抢走"（关）—— 后者收不到 UP，不关就是僵尸菜单。
- 外点的 DOWN 只记 `downOutside`，等 **UP** 才关并释放捕获。少吞 UP 的话，
  各栏会在 UP 里执行动作，于是"收起菜单"会顺带打开那个书签、切走那个标签。

`popup_toggle` 只回答"某锚点的弹层是否开着"，用于 `Show*` 兜底（键盘/命令重入）
与面包屑箭头形态（› / ⌄）。

规则：

- **每个弹层调用点都要传锚点键**，同一个控件的同一下拉传同一个串。
- 带索引的位置把索引编进去（`nav.crumb.3`、`tab.menu.1`）——
  `nav.crumb.3` 与 `nav.crumb.5` 是两个不同锚点。
- 有稳定 id 的（书签）**用 id 不用下标**：重排后下标会漂。
- 传 `nullptr` = 不参与判定。只在"根本不存在同一个锚点"时用。

有**可见状态**要置位的调用方（地址栏箭头 › → ⌄）先判 `popup::IsOpen(key)` 再置位，
`nav_bar_actions.cpp` 的 `ShowChildMenu` 是范例。

调试提示：`ShowPopupList` 内部是**嵌套消息循环**，断点停在里面时主窗口仍在处理消息（可能重绘、
可能切标签）。单步时不要假设"程序暂停了世界"。`Esc`/`Enter` 在 `ListSubclass` 里处理，
方向键由 `LISTBOX` 默认处理——要加 `Home`/`End`/`PageUp` 就在这里加。

目前**不支持二级弹出**：`PopupRow::folder` 只画 chevron，点击等于直接选中该行。
（"再点即收"不是二级弹出 —— 它管的是同一个锚点的开关，不是往右再开一层。）

---

## 4. 运行期工具

### 4.1 截图

```bash
python scripts\shot.py out.png
```

- 按窗口类名 `ExBarBrowser_Main` 找窗口，`PrintWindow(..., PW_RENDERFULLCONTENT)` 抓完整内容。
- **窗口被遮挡也能抓**（这是选 `PrintWindow` 而不是 `BitBlt` 的原因）。
- 纯 ctypes + zlib 手写 PNG，不需要 Pillow。
- 抓不到的情形：最小化窗口；某些 GPU 合成内容（本工程目前没有 WebView2，不受影响）。

失败提示 `window not found: ExBarBrowser_Main` 表示没有运行实例，或类名被改了（改类名要同步这里）。

### 4.2 注册 / 卸载

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1            # 只加右键动词
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Takeover  # 接管默认打开
powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1          # 还原
```

写入了什么：

| 键 | 值 |
|---|---|
| `HKCU\Software\Classes\Folder\shell\ExBarBrowser` | `(default)` = `Open with ExBar`，`command` = `"<exe>" "%1"` |
| `HKCU\Software\Classes\Directory\Background\shell\ExBarBrowser` | `(default)` = `Open in ExBar`，`command` = `"<exe>" "%V"` |
| `HKCU\Software\Classes\Folder\shell`（仅 `-Takeover`） | `(default)` = `ExBarBrowser` |

`-Takeover` 前会导出 `backup\Folder-shell.reg` 并写 `.takeover` 标记；`uninstall.ps1` 只在看到标记时
才删除 `Folder\shell` 的默认值，避免误删用户自己设的默认处理器。

### 4.3 最小回归（手工）

```powershell
# 1. 冷启动
.\ExBarBrowser.exe "C:\Windows"
# 2. 单实例转发：再开一个，应复用窗口并新增标签
.\ExBarBrowser.exe "C:\Windows\System32"
# 3. 截图
python scripts\shot.py after.png
# 4. 关窗口后检查日志尾
Get-Content "$env:TEMP\ExBarBrowser.log" -Tail 20 -Encoding Unicode
```

---

## 5. 常见故障

| 现象 | 诊断 | 处理 |
|---|---|---|
| 链接 `Permission denied` | exe 正在运行 | 关实例或改 `-o` |
| 启动即退，无窗口 | 单实例互斥量残留 / 已有隐藏实例 | 任务管理器结束 `ExBarBrowser.exe`；检查是否有 `Local\ExBarBrowser.Instance` 占用 |
| 窗口出来但视图区全白 | `Initialize` 失败或视图尺寸 0 | 看日志 `[init] Initialize hr=`；确认 `Layout` 里 `viewH > 0`（窗口最小 560×360） |
| 视图白底、工具栏深色 | uxtheme 序号取不到 | 旧 Win10（<1903）或被安全软件拦；可接受降级 |
| 地址栏输入路径回车无反应 | 解析失败 | 看 `[nav] parse` 的 hr；多为路径不存在或权限 |
| 中文路径导航失败 | 编码 | 源文件必须是 UTF-8；`_vsnwprintf` 用 `%s` 且参数是 `wchar_t*` |
| 高 DPI 下文字被裁 | 某处写死了像素 | 全量搜 `S(` 之外的裸数字，改成 `theme_.S()` |
| 切标签闪白 | 容器没填背景 | `ShellView::EraseBackground` 是否被调用；`ViewArea` 的背景刷是否随主题更新 |
| `shot.py` 抓到黑图 | 窗口最小化 / `PrintWindow` 被拦 | 先 `ShowWindow(SW_RESTORE)`；换 `PW_CLIENTONLY` 试 |
| 编过但行为没变 | 跑的是旧 exe | 确认 `build.sh` 输出时间戳；单实例会复用旧进程 |

---

## 6. 发布前检查

- [ ] `bash build.sh` 零警告
- [ ] 三个静态检查全绿：`check_commands.py` / `check_ai_meta.py` / `check_components.py`
- [ ] 冷启动 → 默认目录可浏览
- [ ] 5 个标签连续开关 20 次，无残留、无崩溃
- [ ] 深/浅色各截图一张（`scripts/shot.py`），视觉无明显错位
- [ ] 100% / 150% DPI 各看一次
- [ ] `install.ps1` → 右键菜单可用 → `uninstall.ps1` 干净还原
- [ ] `CHANGELOG.md` 有对应版本条目，版本号与 `README.md` 状态表一致
- [ ] 文档基线时间已更新（README / ARCHITECTURE / PARITY / ROADMAP 头部的 `baseline`）
- [ ] 产物不入库（`git status` 里没有 exe/png/log）
