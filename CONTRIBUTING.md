# 贡献指南

本文面向要给 ExBarBrowser 加功能、修 bug 的人（包括 AI Agent）。
先读 [README.md](README.md) 的"定位与边界"和 [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md)
的硬规则（R1–R20），再动手。

---

## 1. 环境

| 项 | 要求 |
|---|---|
| OS | Windows 10 1903+ / Windows 11（深色模式依赖 1903 起的 uxtheme 导出） |
| 编译器 | Windhawk 自带 `C:\Program Files\Windhawk\Compiler\bin\clang++.exe`（20.1.3，`x86_64-w64-mingw32`） |
| 构建 | `bash build.sh`（Git Bash）或 `build.bat`（cmd） |
| 调试辅助 | Python 3（仅 `scripts/shot.py` 用，标准库即可，无需 pip 安装） |
| 不需要 | Visual Studio、CMake、vcpkg、.NET SDK、Node.js |

**不要引入第三方库**。目标产物是"单 exe、零运行时依赖"，任何新依赖都要先证明它不能自己写 200 行解决。

---

## 2. 改动流程

```
读文档/定位模块 → 改（≤1 个职责/文件） → 编译 → 运行验证 → 截图或看日志 → 更新 CHANGELOG → 提交
```

### 2.1 编译

```bash
bash build.sh
```

编译前**必须关掉正在运行的 ExBarBrowser**，否则链接失败（`Permission denied`，Windows 不允许覆盖运行中的映像）。
只想验证语法/链接而不打断手头的实例，直接让构建脚本把产物写到临时路径（它会自动全量/增量）：

```bash
bash build.sh --out /tmp/ebb_check.exe
```

**不要手写 `clang++ src/a.cpp src/b.cpp ...` 长命令**：源文件已 57 个，手抄清单必然与
`build_fast.py` 的自动扫描脱节（早期文档里的示例只列了 9 个 TU，照抄会链接失败）。
需要绕过构建脚本时请用 `python build_fast.py --out <path>`。

**零警告是基线**。新增警告要么修掉，要么在 PR 说明里写清为什么必须保留。

### 2.2 验证清单

改动提交前逐项过一遍（不适用的写"不适用"，不要跳过）：

- [ ] 编译零警告（`bash build.sh` 成功）
- [ ] 启动正常：默认路径能打开，标签可新建/切换/关闭
- [ ] 改动涉及的交互路径手点一遍（导航、面包屑、地址编辑、右键菜单…）
- [ ] 深色 / 浅色两种模式都看一遍（切换系统主题或改 `SystemUsesLightTheme` 后触发 `WM_SETTINGCHANGE`）
- [ ] 125% / 150% DPI 下不重叠、不裁切（拖到不同 DPI 的显示器上验证 `WM_DPICHANGED`）
- [ ] `%TEMP%\ExBarBrowser.log` 无新增 `hr=0x8...` 失败行
- [ ] 多标签连续开 10 个后切标签不闪、不残留
- [ ] 截图留档：`python scripts\shot.py out.png`（视觉改动必做）
- [ ] 三个静态检查全绿：`python tools/check_commands.py`、`tools/check_ai_meta.py`、`tools/check_components.py`
- [ ] 动到命令表/输入端/AI 元数据时，对应单测跑过（`tests/` 下每个 `.cpp` 自带 `main`）
- [ ] `CHANGELOG.md` 的 `[Unreleased]` 补了对应条目

### 2.3 提交

约定式提交，中文正文（这个约定来自 ExBar 主仓，脱钩后沿用，本仓库自有历史从这里开始）：

```
feat(nav): 面包屑分段支持 Ctrl+点击在新标签打开
fix(shell): 修网络路径导航失败后历史回退错位
docs(readme): 补标签溢出已知问题
refactor(app): 把历史栈操作收敛到 Tab 的成员函数
```

`scope` 取值：`app` / `shell` / `tabs` / `nav` / `status` / `theme` / `cmd` / `ai` / `input` /
`build` / `scripts` / `docs`。

一条提交只做一件事。格式化、重命名、逻辑修改不要混在一起——评审时看不出你在改什么。

---

## 3. 代码规范

### 3.1 硬规则

| 规则 | 判据 |
|---|---|
| 单文件 ≤500 行 | `wc -l src/*.cpp`；超了就按职责拆（`nav_bar.cpp` → `nav_bar_draw.cpp` / `nav_bar_actions.cpp` 就是这么来的）。当前超限 5 个：`shell_view.cpp` 991、`shell_ops.cpp` 934、`popup_list.cpp` 695、`bookmark_bar.cpp` 684、`app_commands_files.cpp` 557 —— 下次动它们先拆，别再往上加 |
| 一文件一职责 | 文件名能一句话说清它干什么；说不清就是两个文件 |
| 一个共享头 | 只有 `common.h` 被所有 TU 包含，且它只放类型/常量/纯内联工具，**不放业务** |
| 依赖单向 | `app` → `nav_bar`/`tab_strip`/`status_bar`/`shell_view` → `nav_model`/`theme` → `common`。下层不许反向包含上层头 |
| 无全局可变状态 | 唯一例外是 `App::Get()` 单例和文件级 `g_xxxClassReady` 注册标志 |
| 视觉只走 `Theme` | 任何颜色/字号/间距都用 `theme_.S()` 与 `theme_.xxx`，不许写死像素和 `RGB()` |
| 文案中文、注释中文 | 标识符英文（与 ExBar mod 保持同一口径） |
| 不写行号到文档 | 用符号名定位（行号必漂移） |

### 3.2 命名与结构

- 类：`PascalCase`（`ShellView`、`NavBar`）；成员：`snake_case_` 带尾下划线；常量：`k` 前缀（`kMainClass`）。
- 窗口过程统一形态：`static LRESULT CALLBACK WndProc(...)` 取 `GetThis<T>(h)`，转发给成员 `Handle(...)`；
  拿不到 `this` 就 `DefWindowProcW`。
- 子窗口 → 主窗口用 `WM_APP_TAB_*`（`common.h`），不许 `#include "app.h"` 反向依赖。
- 跨模块回调用 `std::function` 注入（`NavBar::SetNavFn` 等），不在控件里 `new` 别的控件。

### 3.3 Win32 / COM 注意事项

- **UI 单线程**：所有窗口操作、`IExplorerBrowser` 调用都在主线程（STA）。不要在 UI 线程做文件枚举、
  网络访问、`Sleep`；必须做的放到工作线程再 `PostMessage` 回来。
- **COM 所有权**：`ComPtr` 只提供 `&`/`->`/`Reset`，不支持拷贝（防误共享）。`Sink` 用引用计数，
  `Advise` 后由浏览器持有，`Unadvise` 后不要再碰。
- **不要改回 `IExplorerBrowser::Initialize` 的头文件声明**：mingw 的 `shobjidl.h` 第三参是
  `const FOLDERSETTINGS*`，真实 ABI 是 `FOLDERVIEWOPTIONS` 按值。`ShellView::InitBrowser` 按 vtable
  索引 3 调真实签名，这是刻意的。
- **路径**：内部一律反斜杠；`SHCreateItemFromParsingName` 遇正斜杠返回 `E_INVALIDARG`。
  命名空间（`::{GUID}` / `shell:`）保持原样。
- **下拉菜单**：一律用 `ShowPopupList`（`popup_list.h`），不要新增 `TrackPopupMenu`。
  三个必须记住的点：`rows` 活到函数返回、图标由调用方 `FreeRows` 释放、不要在 `WM_PAINT` 里弹
  （它是模态嵌套消息循环）。
- **窗口类名**是外部契约（脚本、单实例查找、自动化都靠它），改名要同步 `scripts/shot.py` 与文档。

### 3.4 新增一个自绘栏的最小步骤

1. `xxx_bar.h/.cpp`：`Create(parent, theme)`、`HWND hwnd()`、`SetXxx(...)`、`ReloadTheme()`。
2. 类名常量加到 `common.h`（`kXxxBarClass`），注册用文件级 `g_xxxClassReady` 幂等。
3. `WM_ERASEBKGND` 返回 1，`WM_PAINT` 里内存 DC 双缓冲后 `BitBlt`。
4. `App::OnCreate` 创建、`App::Layout` 定位（高度走 `theme_.S()`）、`App::ApplyTheme` 调 `ReloadTheme`。
5. `README.md` 的"已实现"表和布局图同步更新。

### 3.5 新增一个快捷键

1. `src/app.cpp` 的匿名 `enum` 加命令 ID，`ACCEL acc[]` 加一行。
2. `WM_COMMAND` 加 `case`。
3. `README.md` 快捷键表加一行；若与 ExBar mod 的默认键冲突，先在 `docs/PARITY.md` 记差异。

---

## 4. 禁止事项

| 禁止 | 原因 |
|---|---|
| 注入 explorer / 装全局钩子 / `SetWindowsHookEx` | 本工程的全部价值就是"不碰 explorer" |
| 未经确认修改系统默认打开方式 | `install.ps1 -Takeover` 已做备份与标记，其他脚本一律不许写 `HKCR`/`HKLM` |
| 引入第三方库或包管理器 | 破坏"单 exe 零依赖" |
| 在 `WM_PAINT` 里 `CreateSolidBrush` 之外的资源泄漏 | 双缓冲路径高频调用，任何 GDI 泄漏都会被放大 |
| 在 UI 线程做同步 IO | 卡界面；用户感知为"程序挂了" |
| 把配置/状态写进 exe 同目录 | 用户目录可能只读（Program Files）；P2 统一走 `%APPDATA%\ExBar` |
| 提交 `*.exe` / `*.png` / `*.log` / `backup/` | 已在 `.gitignore` 排除 |
| 只改代码不改 `CHANGELOG.md` | 变更记录是发布依据 |

---

## 5. 文档规范

- 每份 `docs/*.md` 带 front-matter：`status: current|reference|archived` + `baseline: YYYY-MM-DD`。
- 状态变了就改 `status`，不要留"过时但没人知道"的文档。
- 数字（行数、命令数、体积）必须标日期与口径；不确定就写"约"并说明怎么量的。
- 引用别处代码用符号名（`NavModel::ChildFolders`）而不是行号。
- 新增能力先改 [docs/PARITY.md](docs/PARITY.md) 的状态列，再改代码——避免"文档追着代码跑"。
