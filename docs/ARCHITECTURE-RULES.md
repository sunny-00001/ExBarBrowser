---
status: proposed
baseline: 2026-11-19 20:45
---

# 架构规则（约束文档，未施工）

> **本文件只定规则，不含任何代码改动。** 规则先于重构生效：从现在起新增代码按此写，
> 存量按批次迁移（见 [ARCHITECTURE-PROPOSAL.md](ARCHITECTURE-PROPOSAL.md) §10）。
> **v3（2026-11-19 20:58）**：按"一切皆组件 / 载体 / 输入端 / 命令管线"重写第 1–2 节与 R11–R20；
> 设计依据与组件清单见 [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)。
> 规则依据：[DOPUS-NOTES.md](DOPUS-NOTES.md)（Opus 的公开契约）、Blender operator 模型与 MCP tools 规范。
> **命名已与并行会话对齐**：导航栏拆分使用 `nav_bar_draw.cpp`（不是 paint）+ `nav_metrics.h`，
> 这是 2026-11-19 20:35 已落地的现实，规则沿用，不再另起名字。

---

## 1. 对象模型（唯一命名口径）

```
Runtime（进程）                         app/runtime       进程装配：单实例、COM、命令行
└─ Lister（顶层载体）                   app/lister        布局：Bar 区 / 视图区 / Panel / 状态 Bar
   ├─ Tab（文件夹上下文） 1..n          core/tab          路径、历史、显示名、项目数、格式
   │  ├─ History（每 Tab 独立）         core/history      线性历史 + 截断 + 跳转
   │  ├─ TabFormat（P2）                core/tab_format   视图模式/排序/过滤
   │  └─ View（显示实现）1              platform/shell/shell_view
   └─ Component 树                      ui/               组件（资源/原子/控件/复合）
       └─ Container（载体）             ui/containers     Bar / Panel / Dialog / Popup

输入（手脚架）                           input/           鼠标/键盘/序列键/IME → 绑定表 → CommandCall
命令管线（唯一）                         pipeline/        validate→poll→authorize→execute→commit→emit→audit
Feature（业务组合，编译期）              features/<name>  实例化载体、注册命令、订阅事件
```

| 对象 | 拥有什么 | 不拥有什么 | 现状对应 |
|---|---|---|---|
| Runtime | 进程生命周期、单实例、COM、命令行 | 标签、视图 | `main.cpp` + `App::Get()` |
| Lister | Tab 集合、激活索引、布局、载体树、主题 | 单个 Tab 的路径/历史 | `App`（混在一起） |
| Tab | 路径、History、显示名、项目数、格式 | 视图窗口句柄、组件 | `App::Tab` |
| View | 一个 `IExplorerBrowser` 与容器窗口 | 历史、标题 | `ShellView` |
| Component / Container | 自己的尺寸、子节点、交互状态；载体额外管布局与生命周期 | 业务状态、窗口以外的资源所有权 | `NavBar` / `TabStrip` / `PopupList` 各自实现 |
| Input | 焦点、绑定表、序列状态 | 业务状态 | `ACCEL` 表 + 各控件 `WM_*` |
| Pipeline | 命令表、校验/授权/审计、事件 | 业务状态（状态在 Tab/Config） | `WM_COMMAND` switch |
| Feature | 自己的命令与载体实例 | 其他 Feature 的内部 | 无（P2 起） |

**所有权规则**：Lister 拥有 Tab；Tab 拥有 History 与 View；Command 只读写经 Ports 暴露的能力，
**不得直接持有 Tab 或 View 的指针**。

---

## 2. 目录与依赖

### 2.1 目标目录

```
src/
├─ main.cpp
├─ contract/              跨层契约（唯一允许被所有层包含的目录）
│  ├─ object_ids.h        窗口类名、WM_APP_*、单实例 payload
│  ├─ commands.h          CommandId + CommandSpec（数据，含 argsSchema/risk/aiExposed）
│  └─ ports.h             Navigator / PathSource / CommandSink / StateSource
├─ core/                  零 Win32，可单测
│  ├─ path_text.{h,cpp}
│  ├─ crumb_model.{h,cpp}
│  ├─ history.{h,cpp}
│  └─ tab.{h,cpp}         Tab 状态 + TabFormat
├─ platform/              系统适配
│  ├─ win_base.h
│  ├─ log.{h,cpp}
│  ├─ system.{h,cpp}      DPI / 深色 / 标题栏
│  ├─ theme.{h,cpp}       tokens 的运行时解析（dark × dpi），不含色值字面量
│  ├─ config/（P2）       settings/state/bookmarks/sessions
│  └─ shell/
│     ├─ pidl.{h,cpp}
│     ├─ child_folders.{h,cpp}
│     └─ shell_view.{h,cpp}
├─ ui/
│  ├─ tokens/             tokens.json → tokens.gen.h（颜色/间距/字号/圆角/尺寸）
│  ├─ assets/             icon_registry、typography、brushes、cursors、metrics（唯一取图/建字体处）
│  ├─ component.h         Component / Painter / Layout / InputEvent
│  ├─ components/         icon、label、surface、separator、button、toggle、checkbox、edit、
│  │                      list_row、tab、breadcrumb、menu、toast、scroll_view
│  └─ containers/         bar、panel、dialog、popup、lister_surface（载体，只布局/生命周期）
├─ input/                 手脚架：input_router、binding_table、sequence、mouse_map、ime
├─ pipeline/              命令管线：pipeline、registry、validate、authorize、events、audit、state_view
├─ features/              tabs、navigation、bookmarks(P2)、sessions(P2)、search(P3)、ai(P4)
└─ app/
   ├─ runtime.{h,cpp}
   ├─ lister.{h,cpp}
   ├─ layout.cpp
   └─ assembly.cpp        注册 features / 命令 / 快捷键（唯一装配点）
```

### 2.2 依赖方向（单向，禁止反向与同层互引）

```
contract  ←  core  ←  platform  ←  pipeline  ←  features  ←  app
                          ↑           ↑
                          └──  ui  ───┘
                          input 依赖 ui（焦点/命中）与 pipeline（发命令）
```

- `contract/` 只能包含 `platform/win_base.h`（基础类型与 Win32 头），不得包含 core/ui/features/app。
- `core/` 只依赖 `contract/` 与标准库，**零 Win32**。
- `platform/` 依赖 contract + core。
- `pipeline/` 依赖 contract + core + platform；**不含 UI**（审批界面由 `features/ai` 的 `Toast` 订阅事件实现）。
- `ui/` 依赖 contract + core + platform（允许调用只读 shell 元数据：图标、子文件夹枚举）；
  **不得依赖 pipeline/features/app**，只能经 `CommandSink` 发命令、经 `StateSource` 读状态。
- `input/` 依赖 contract + ui（焦点/命中）+ pipeline（发命令）；**不得依赖 features**。
- `features/` 依赖 contract + core + platform + pipeline + ui（组件与载体），**不得**依赖其他 feature。
- `app/` 可以依赖全部，是唯一装配点。
- **同层目录之间零 include**：`ui/components/*` 互不引用（靠组合）；`ui/containers/*` 互不引用；
  `features/*` 互不引用（只经 Ports）；`pipeline/*` 内部按"注册表/状态/授权/审计"分层，禁止循环；
  `input/*` 内部按"事件源/绑定表/路由器"分层，禁止循环。

---

## 3. 硬规则（每条都有判据）

| 编号 | 规则 | 判据（可机械检查） |
|---|---|---|
| **R1** | 依赖单向、无环、同层不互引 | 解析 `#include` 图：只允许第 2.2 节的方向；同层目录之间出现边即失败 |
| **R2** | `core/` 零 Win32/COM | `core/*.{h,cpp}` 中不出现 `windows.h` / `windowsx.h` / `shlobj.h` / `shobjidl.h` / `HWND` / `HDC` |
| **R3** | 单文件 ≤300 行；单目录 ≤10 个文件 | `wc -l`；`ls | wc -l`（超出必须拆或合并职责） |
| **R4** | 跨层契约只在 `contract/` | 窗口类名、`WM_APP_*`、命令 ID 在其他目录出现定义（非引用）即失败 |
| **R5** | 命令是唯一动作入口 | `ui/` 与 `features/` 不得直接改 Tab/Lister 状态；只能 `CommandSink::Invoke(id)` |
| **R6** | 绘图原语唯一 | chevron / cross / 圆角填充 / glyph 只允许由 `Painter`（`ui/components` 内部）提供；其他文件出现同名实现即失败 |
| **R7** | 日志唯一 | 只允许 `platform/log` 写 `%TEMP%\ExBarBrowser.log`；出现第二份 `CreateFileW(...log)` 即失败 |
| **R8** | Tab 状态唯一 | 路径/历史/显示名/项目数/格式只存在 `core::Tab`；`ui/` 不得出现历史容器或路径缓存 |
| **R9** | 新功能必须新目录 | 新增功能在 `features/<name>/`；`app/` 只做注册；`app/lister.cpp` 行数不得随功能增长 |
| **R10** | 外部契约稳定 | 窗口类名、命令行开关、注册表动词、日志路径变更时，必须同批改脚本与文档 |
| **R11** | 资源组件唯一 | 图标只来自 `ui/assets/icon_registry`；字体/字号只来自 `typography`；背景/边框只来自 `brushes`；间距/圆角/尺寸只来自 `tokens.gen.h`。其他目录出现 `SHGetFileInfo`/`LoadIcon`/`CreateFontW`/`CreateSolidBrush`/`RGB(` 即失败 |
| **R12** | 单一绘制面 | B5 之后：Lister 客户区是唯一 chrome 绘制面，`Popup`/`Dialog` 是唯一额外窗口（`EDIT` 作为叶子原生控件例外）。判据：窗口类数量 ≤3（Lister + Popup + Dialog） |
| **R13** | 一切可见对象是组件 | 载体里不得出现 `CreatePen`/`FillRect`/`DrawText`/`DrawIconEx`/`CreateFontW`；载体只 `Arrange` 子组件。判据：`ui/containers/` 与 `features/*/` 的载体文件零绘制调用 |
| **R14** | 载体只组合 | 载体不得判断"这是导航栏还是书签栏"、不得引用具体业务类型；业务差异只能靠**子组件组合**表达。判据：`ui/containers/` 不 include `features/`、无 `CommandId` 分支 |
| **R15** | 输入端只产命令 | `input/` 不得改状态、不得调业务方法；只能产出 `CommandCall` 或把字符交给焦点组件。判据：`input/` 不 include `core/tab`、`platform/config` |
| **R16** | 管线唯一 | 命令只能在 `pipeline` 执行；`validate/poll/authorize/audit` 不可绕过。判据：`ui/` 与 `features/` 不出现 `Tab::SetPath`/`Config::Set`/`ShellView::Navigate` 直调 |
| **R17** | 命令必须带 actor | 所有调用是 `Run(CommandCall{id,args,Actor})`；审计记录 actor。判据：`CommandSink::Invoke` 签名含 `Actor` |
| **R18** | AI 安全三件套 | `aiExposed` 必须有 `argsSchema` 与 `risk`；`Write`/`Danger` 由 Agent 发起必须审批；每次调用写 `audit.jsonl`（不可关闭） |
| **R19** | 状态同源 | 只有一份 `StateView`（事件驱动、只读）；UI 与 AI 读同一快照；`ui/` 不得缓存路径/选中项 |
| **R20** | 人机等价 | 每个 `aiExposed` 命令必须有人工可达路径（鼠标/键盘/命令面板）；不得存在只有 AI 能触发的命令 |

---

## 4. 命名规则

| 编号 | 规则 | 例 |
|---|---|---|
| N1 | 对象名固定为 Runtime / Lister / Tab / View / Command / Feature，不用 App/Window/Page 等别名 | `Lister::ActivateTab` |
| N2 | 文件名小写蛇形，`<对象>_<职责>`；同一职责的实现分片用 `_paint` / `_actions` / `_layout` | `nav_bar_paint.cpp` |
| N3 | 类型 PascalCase；函数 PascalCase；成员 `snake_case_`；常量 `kPascalCase`；枚举值 `kXxx` | `kMainClass`、`hover_btn_` |
| N4 | 命令 id 用 PascalCase 动词开头，与 mod 的 `capabilities.json` 同 id 优先 | `TabClose`、`PathCopyTab` |

---

## 5. 命令契约

所有用户可见动作（按钮、快捷键、菜单、命令行、将来的命令面板与 AI）都登记为命令：

```cpp
// contract/commands.h（示意，尚未落地）
enum class CommandId : int { TabNew = 1, TabClose, TabNext, TabPrev, NavBack, /* ... */ };

enum class Risk { Read, Write, Danger };        // 与 mod 的 risk 对齐
enum class Scope { CurrentTab, App };

struct CommandSpec {
    CommandId      id;
    const wchar_t* title;         // 中文显示名（命令面板用）
    const wchar_t* description;   // 给人与 AI 看：做什么、什么时候不要用
    const char*    argsSchema;    // JSON Schema（MCP inputSchema 直接复用；无参数给空对象）
    Risk           risk;          // Read / Write / Danger
    Scope          scope;         // CurrentTab / App
    bool           undoable;
    bool           aiExposed;     // 是否进 tools/list
    bool           needsSelection;
};

enum class ActorKind { Human, Agent };
struct Actor { ActorKind kind; std::wstring sessionId; };   // Agent 时带会话标识
```

规则：

- **C1** 一个命令一个 handler；handler 只做"取参数 → 调 Ports/对象 → 返回结果"。
- **C2** UI 不实现业务：按钮只发 `CommandId`，不直接调 `ShellView`。
- **C3** 快捷键是命令的映射（`id → 键`），不是独立分支；新增快捷键 = 表里加一行。
- **C4** 命令表是数据：P3 由它生成 `capabilities` 子集，供命令面板与 AI 使用。
- **C5** 破坏性命令（删除、覆盖、批量改名）必须标 `Danger` 并走确认，不允许默认执行。
- **C6** 每次调用必须带 `Actor`；审计记录 actor、命令、参数哈希、结果、耗时、是否审批。
- **C7** `aiExposed` 命令的 `description` 必须写"什么时候不要用"；缺 `argsSchema` 的不得暴露给 AI。

---

## 6. 扩展点契约

```cpp
// contract/ports.h（示意）
struct Navigator {                 // 谁能导航
    virtual void Go(const std::wstring& path) = 0;
    virtual void Back() = 0;
    virtual void Forward() = 0;
    virtual void Up() = 0;
    virtual void SetViewMode(int fvm, int iconPx) = 0;
};
struct PathSource {                // 当前路径/选中项
    virtual std::wstring CurrentPath() const = 0;
    virtual std::vector<std::wstring> SelectedPaths() const = 0;
};
struct CommandSink {               // 命令往哪发（UI 唯一出口）
    virtual bool Invoke(CommandId id, const std::wstring& args, Actor actor) = 0;
};
struct StateSource {               // 状态只读视图（UI 与 AI 同源）
    virtual std::wstring SnapshotJson() const = 0;
};

struct Feature {                   // 一个功能一个实现
    virtual const wchar_t* Name() const = 0;
    virtual void Init(Navigator&, PathSource&, CommandSink&, StateSource&) = 0;
    virtual void RegisterCommands(CommandRegistry&) = 0;
    virtual void Shutdown() = 0;
};
```

规则：

- **E1** Feature 之间不互相 include；需要协作时向 Ports 加能力，或由 Lister 转发。
- **E2** Feature 不创建顶层窗口；需要 UI 时实例化 `ui/containers` 的载体（`Bar`/`Panel`/`Dialog`）
  并组合 `ui/components`，由 Lister 布局。
- **E3** Feature 的持久化只经 `platform/config`，不自己拼路径、不自己写文件。
- **E4** 任何 Feature 都必须能"关掉"（编译期开关或运行时配置），关掉后其余功能不受影响。
- **E5** Feature 的 UI 只能用 `ui/components` 组合；不得直接 `RegisterClassW`、不得自绘 chevron/图标。
- **E6** Feature 不得持有 `HICON`/`HFONT`/`HBRUSH` 的所有权（借 `IconRegistry`/`Typography`/`Theme` 的）。
- **E7** Feature 的 AI 能力只能通过注册 `aiExposed` 命令获得；不得自建 IPC/工具通道。

---

## 7. 代码规则

| 编号 | 规则 |
|---|---|
| S1 | 一个 `.cpp` 一个变化理由；`*_paint` / `*_actions` 视为同一职责的实现分片 |
| S2 | 头文件自包含：只 include 自己需要的；禁止在头里 `using namespace` |
| S3 | 不在头文件里定义可变全局；常量用 `constexpr`，工具用 `inline` |
| S4 | 所有 GDI 对象有明确所有者；跨函数传递的 `HICON` 必须写明"谁销毁" |
| S5 | 所有路径在进入 shell 前统一反斜杠；命名空间（`::{}` / `shell:`）原样透传 |
| S6 | 注释写"为什么"，不写"是什么"；对外契约（ABI 补丁、外部键名）必须写清来源与验证方式 |

---

## 8. 机械校验清单（待批准后实现 `tools/check_arch.py`）

| 检查 | 对应规则 | 实现方式 |
|---|---|---|
| include 图：方向、环、同层互引 | R1 | 正则抽 `#include "..."` → 目录级图 → 拓扑/白名单 |
| core 零 Win32 | R2 | grep 禁用符号 |
| 行数与文件数 | R3 | `wc -l` / `ls` |
| 契约符号位置 | R4 | grep `constexpr wchar_t k.*Class`、`WM_APP_`、`enum class CommandId` |
| UI 直调状态 | R5/R16 | 白名单：`ui/` 只允许 include `contract/`、`core/`、`platform/`、`ui/*` |
| 绘图原语重复 | R6 | 符号表：`DrawChevron`、`DrawCross`、`FillRound` 只允许 1 个定义 |
| 日志唯一 | R7 | grep `ExBarBrowser.log` |
| Tab 状态泄漏 | R8 | grep `hist`、`std::vector<std::wstring>` 在 `ui/` |
| 功能增长 | R9 | `app/lister.cpp` 行数基线（当前 438，目标 <200） |
| 外部契约 | R10 | 文档中的类名/开关/键路径 与代码常量比对 |
| 资源组件唯一 | R11 | `SHGetFileInfo`/`LoadIcon`/`CreateFontW`/`CreateSolidBrush`/`RGB(` 只允许出现在 `ui/assets/`、`ui/tokens/`、`platform/theme.cpp` |
| 窗口类数量 | R12 | grep `RegisterClassW`：只允许 `Lister` / `Popup` / `Dialog`（+ 原生 `EDIT`） |
| 载体零绘制 | R13 | `ui/containers/` 与 `features/*/` 中出现 `CreatePen`/`FillRect`/`DrawText`/`DrawIconEx` 即失败 |
| 载体无业务 | R14 | `ui/containers/` 不 include `features/`；无 `CommandId` 分支；无 `if (navBar)` 式判断 |
| 输入端纯净 | R15 | `input/` 不 include `core/tab`、`platform/config`；无状态写入 |
| 管线唯一 | R16 | `ui/`、`features/` 中禁止 `SetPath(`/`SetViewMode(`/`Config::Set`/`ShellView::Navigate` 直调 |
| 命令签名 | R17 | `CommandSink::Invoke` 必须含 `Actor`；审计写入点唯一 |
| AI 安全 | R18 | 遍历 `CommandSpec`：`aiExposed` 必须有 `argsSchema` 与 `risk` |
| 状态同源 | R19 | `ui/` 中禁止 `hist`/`selection` 容器；`StateView` 只有一份实现 |
| 人机等价 | R20 | 每个 `aiExposed` 命令必须在绑定表或命令面板中有可达路径 |

校验器本身遵守同一套规则：`tools/` 不进入构建，不依赖第三方库。

---

## 9. 与现状的差距（gap，按优先级）

| 差距 | 现状 | 目标 | 归属批次 |
|---|---|---|---|
| `common.h` 是杂物间（178 行，18/19 文件依赖） | 平台头 + 契约 + 工具 + 常量混在一起 | 拆成 `contract/*` + `platform/win_base.h` + `core/path_text` | B1 |
| 绘图原语三份 | `DrawChevron` 在 nav_bar / popup_list / tab_strip 各一份 | 只留 `ui/components`（内部用 `Painter`） | B1 |
| 日志两份 | `common.h::LogDbg` 与 `shell_view.cpp::LogLine` | 只留 `platform/log` | B1 |
| 路径工具两份 | `common.h` 与 `nav_model.h` 各一套 `ParentOf/LeafOf` | 只留 `core/path_text` | B1 |
| ~~`nav_bar.cpp` 557 行~~ | **已拆（20:35）**：`nav_bar` 356 + `nav_bar_draw` 191 + `nav_metrics.h` | 保持 | 已完成 |
| `app.cpp` 438 行 | 标签生命周期 + 导航 + 布局 + 命令分发 + 主题 | 拆 `layout` / `commands`，标签与导航搬到 `features` | B2/B3 |
| 无命令管线 | `ACCEL` 表 + `WM_COMMAND` switch | `contract/commands.h` + `pipeline/` | B2 |
| 无 Tab 对象 | `App::Tab` 内嵌历史操作 | `core::Tab` + `core::History` | B3 |
| `App` 兼任 Lister | 窗口 + 标签 + 布局 + 消息 + 主题 | `app::Lister` + `app::Runtime` | B3 |
| 无 Feature 概念 | 功能直接写在 App/chrome | `features/*` + `contract/ports.h` | B4 |
| 无令牌/资源/组件层 | 色值写死在 `theme.cpp`；取图两处；每栏自绘自命中 | `ui/tokens`、`ui/assets`、`ui/components`、`ui/containers` | B4 |
| 4 个 chrome 窗口各自 WndProc | 悬停/焦点/重绘各一套 | Lister 单一绘制面 + `Popup`/`Dialog` 载体 | B5 |
| 无输入端抽象 | 快捷键在 `ACCEL`、鼠标在各控件、无序列键 | `input/`（绑定表 + 路由器 + 序列） | B6 |
| 无 AI 协作面 | 无 actor、无状态快照、无审批、无审计 | `pipeline/*` + `AgentBridge` + 最小 UIA | B2（审计）/ B7（AI） |

---

## 10. 变更流程（写代码前先过这张表）

**新增一个组件**

1. 先看 `ui/components` 能否用现有组件组合出来（`Bar + Button + Breadcrumb` 已能拼出大多数栏）。
2. 确实要新组件才建文件；只依赖 `ui/component.h`（基类）、`ui/assets`、`ui/tokens`，不 include 其他组件。
3. 图标用 `IconRegistry`、字体用 `Typography`、背景用 `Brushes`——不要自己取图/建字体/建画刷。
4. 交互只产出 `CommandCall`，不改状态；需要弹层用 `Popup` 载体。

**新建一个载体（栏 / 面板 / 对话框）**

1. 优先用现有载体：栏 = `Bar`，面板 = `Panel`，对话框 = `Dialog`，菜单 = `Popup + ListRow`。
2. 载体只写三件事：`Add(组件, 槽位)`、`SetLayout(...)`、事件订阅。
3. 出现 `CreatePen`/`FillRect`/`DrawText`/`DrawIconEx`/`CreateFontW` 即违规（R13）；
   出现"这是导航栏所以……"的业务判断即违规（R14）。
4. 载体实例放在 `features/<name>/` 里；`ui/containers/` 只放通用载体。

**新增一个命令（走管线）**

1. `contract/commands.h` 加 `CommandId` 与 `CommandSpec`：`title/description/argsSchema/risk/scope/undoable/aiExposed`。
2. 在 Feature 或 `pipeline` 注册 handler；handler 只做"取参数 → 调高层函数"。
3. 需要快捷键/序列/鼠标 → `input/binding_table` 加一行；需要菜单 → 对应 `Menu` 行引用 `CommandId`。
4. `docs/PARITY.md` 更新状态列。

**新增一个输入端绑定**

1. 只改数据（`binding_table` 的键位/序列/鼠标映射），不改代码。
2. 序列键（`g g`）用 `timeoutMs` 声明；冲突检测由绑定表负责。
3. 若发现"这个输入必须改状态"，说明它其实是命令——回到"新增一个命令"。

**新增一个功能**

1. 建 `features/<name>/`，实现 `Feature` 接口（`Init/RegisterCommands/Shutdown`）。
2. 需要路径/选中 → `PathSource`；需要导航 → `Navigator`；需要状态 → `StateSource`；持久化 → `platform/config`。
3. 在 `app/assembly.cpp` 注册（唯一装配点）。
4. UI：实例化载体（`Bar`/`Panel`/`Dialog`）并组合组件；不新建窗口类。
5. 文档：`docs/ARCHITECTURE.md` 模块表 + `docs/PARITY.md` 状态。

**新增一个 AI 可调用工具**

1. 命令本身先按"新增一个命令"落地，并保证有人工可达路径（R20）。
2. 标 `aiExposed=true`，补 `argsSchema` 与 `risk`；`description` 写清"什么时候不要用"。
3. `Write`/`Danger` 必须能被 `Authorize` 拦截；确认审计有记录。
4. `docs/PARITY.md` 标注"AI 可调用"，同步 `data/capabilities.json` 同 id 项。

**新增一个视图实现**（如预览窗格）

1. 在 `platform/` 或 `features/preview/` 实现 `View` 抽象（`Create/Resize/SetVisible/ApplyTheme`）。
2. 只由 Tab 持有；Lister 不直接操作 View；视图内容不是组件（shell 自己画）。
3. `IExplorerBrowser` 相关代码不得出现在新视图里。

---

## 11. 待定（需要你确认后才写进规则）

| 项 | 选项 | 建议 |
|---|---|---|
| 类名 `App` 是否改名为 `Lister` | 改名（概念清晰）/ 保留 `App` 但文档称 Lister | 保留 `App`，文档与规则用 Lister 指代；改名放到 B3 一起做，避免两次动全仓 |
| 命令表放头文件还是 JSON | `contract/commands.h`（编译期）/ `data/commands.json`（生成头） | 先头文件；P3 需要给 AI 用时再改生成 |
| `core` 是否引入最小测试 | 不测 / `tests/core_test.cpp`（assert，不进主构建） | B3 之后加，先保证分层正确 |
| 规则校验器何时写 | 现在 / B1 之后 | B1 之前写，用它对 B1 的产出做验收 |
| B5 是否合并 chrome 窗口 | 合并（单一绘制面）/ 保留多 HWND | 先做 B4 组件化，B5 用编译开关验证 IME/焦点后再合并 |
| AI 协作面先做哪一层 | 审计 + 审批先做 / 直接做 MCP | **审计与审批先做**（B2 起），MCP 桥放 B7，避免先有通道没有护栏 |
| 序列键的解析位置 | `input/` 统一解析 / 各组件自己处理 | **统一在 `input/binding_table`**，组件只收"已解析的字符/命令" |
