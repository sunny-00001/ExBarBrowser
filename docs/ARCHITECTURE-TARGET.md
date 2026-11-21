---
status: proposed
baseline: 2026-11-19 20:58
---

# 目标架构：一切皆组件，命令是唯一管线

> 三条命题（本文全部结构都由它们推导）：
> **① 一切对象都是组件**——图标、字体、背景、光标、按钮、标签、列表行、弹窗项、菜单……
> **② 栏 / 面板 / 对话框 / 弹层 / 窗口是"载体"**——只负责布局、生命周期、焦点、绘制顺序；
>    新建载体时复用已有组件，不重复造轮子。
> **③ 鼠标 / 键盘 / 序列键 / IME / 命令行 / AI 工具调用是"手脚架"（输入端）**——
>    它们只做一件事：把输入变成**命令**；**命令是唯一基础管线**，人与 AI 都从这条管线走。
>
> 可机械校验的约束见 [ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md)（R1–R20）。
> 现状见 [ARCHITECTURE.md](ARCHITECTURE.md)，选型与批次见 [ARCHITECTURE-PROPOSAL.md](ARCHITECTURE-PROPOSAL.md)。

---

## 1. 一张图

```
┌──────────────────────── 输入端（手脚架） ────────────────────────┐
│ 鼠标    键盘    序列键     IME     命令行 --cmd     AI tools/call │
└───────────────────────────┬──────────────────────────────────────┘
                            │  InputEvent / CommandCall
                            ▼
                   ┌──────────────────┐
                   │  绑定表 Bindings  │  键位/序列/鼠标 → CommandId + args
                   └────────┬─────────┘
                            ▼
┌──────────────────── 命令管线（唯一） ────────────────────────────┐
│ Validate(args,schema) → Poll(context) → Authorize(actor,risk)     │
│      → Execute(handler) → Commit(state) → Emit(events) → Audit    │
└───────────────────────────┬──────────────────────────────────────┘
                            │ 事件（StateChanged / CommandFinished）
                            ▼
┌──────────────────────── UI（组件树） ────────────────────────────┐
│ Lister（载体）                                                     │
│  ├─ Bar（载体）── Button / Breadcrumb / AddressEdit …（组件）      │
│  ├─ Panel（载体）── ListView / Label …（组件）                     │
│  ├─ 视图区（View：ShellView）                                      │
│  └─ Popup / Dialog（载体，独立窗口）── Menu / ListRow …（组件）    │
└──────────────────────────────────────────────────────────────────┘
```

**关键**：UI 不是状态的所有者，也不是动作的实现者。它只做两件事——把输入交给管线、
把管线产生的事件画出来。所以人用鼠标和 AI 调工具，走的是同一条路。

---

## 2. 组件（一切对象）

### 2.1 五类组件

| 类别 | 内容 | 契约 |
|---|---|---|
| **资源组件** | 图标、字体/字号、背景/表面、边框、光标、圆角、间距 | 只能经 `Assets` 取用；有缓存、随主题与 DPI 重建；**调用方不拥有、不销毁** |
| **原子组件** | `Icon`、`Label`、`Surface`、`Separator`、`Spacer` | 无子节点，`Measure/Paint` 即可 |
| **控件组件** | `Button`（图标/文字/开关/分裂）、`Toggle`、`Checkbox`、`Edit`、`ListRow`、`Tab`、`Breadcrumb` | 有交互状态（hover/pressed/disabled/checked），交互只产出命令 |
| **复合组件** | `Menu`（Popup + ListRow）、`AddressBar`（Bar + Edit + Button）、`CommandPalette`（Dialog + Edit + ListRow） | 只组合，不新增绘制 |
| **载体** | `Bar`、`Panel`、`Dialog`、`Popup`、`Lister` | 只做布局/生命周期/焦点/绘制顺序；**不知道业务** |

### 2.2 统一组件基类

```cpp
// ui/component.h（示意，未落地）
class Component {
public:
    virtual ~Component() = default;

    void SetBounds(const RECT& rc) { bounds_ = rc; }     // 由载体调用
    virtual Size Measure(const Constraints&) = 0;
    virtual void Arrange(const RECT&) = 0;
    virtual void Paint(Painter&) = 0;

    virtual bool HitTest(POINT) const;
    virtual bool OnInput(const InputEvent&);             // 返回"已消费"

    Component* parent() const { return parent_; }
protected:
    RECT bounds_{};
    Component* parent_ = nullptr;
    std::vector<Component*> children_;
};
```

三条纪律：

- **组件不知道自己的绝对位置**，由载体 `Arrange` 决定；
- **组件不创建窗口**（唯一例外是 `Popup`/`Dialog` 载体，由 `ContainerHost` 统一创建）；
- **组件不直接改状态**，交互只产出 `CommandCall`。

### 2.3 资源组件的统一入口

```cpp
struct Assets {                 // 只有它能拿到"看得见的东西"
    IconRegistry&  icons;       // 字形 / 系统 stock / 文件类型（LRU + 主题着色）
    Typography&    type;        // caption / body / title × dpi
    Brushes&       brushes;     // surface / hover / pressed / border（来自 tokens）
    Cursors&       cursors;
    Metrics&       metrics;     // 间距阶、圆角、图标尺寸
};
```

- 图标只在这里 `SHGetFileInfo` / `LoadIcon` / `DrawIconEx` 取图；返回的 `HICON` 归注册表所有。
- 字体只在这里 `CreateFontIndirectW`；主题切换只重建一次。
- 背景只在这里把 token 变成 `HBRUSH`；组件用 `brushes.surface`，不 `CreateSolidBrush`。

---

## 3. 载体（栏 / 面板 / 对话框 / 弹层 / 窗口）

### 3.1 五种载体，一套接口

```cpp
class Container : public Component {          // 载体
public:
    virtual void Add(std::unique_ptr<Component> child, Slot slot = Slot::Body) = 0;
    virtual void Remove(Component* child) = 0;
    virtual void SetLayout(std::unique_ptr<Layout>) = 0;   // HBox/VBox/Grid/Stack
    virtual void Show();
    virtual void Hide();
    // 生命周期：Created → Shown → Hidden → Destroyed（载体负责，组件不感知）
};
```

| 载体 | 布局 | 生命周期 | 典型用途 |
|---|---|---|---|
| `Lister` | 垂直堆叠：Bar 区 / 视图区 / 状态 Bar / 侧 Panel | 与进程同寿命 | 主窗口（唯一顶层载体） |
| `Bar` | 横向或纵向，可溢出折叠 | 常驻 | 导航栏、状态栏、书签栏、搜索栏 |
| `Panel` | 标题 + 内容 + 可折叠/可拖宽 | 可显示/隐藏 | AI 面板、预览、详细信息、搜索结果 |
| `Dialog` | 内容 + 按钮区，模态 | 打开/关闭 | 首选项、重命名、批量操作确认 |
| `Popup` | 单列列表 / 网格，模态循环 | 弹出/关闭 | 菜单、下拉、命令面板候选 |

### 3.2 新建一个栏 = 组合，不写绘制

```cpp
// features/navigation/nav_bar.cpp（示意）
auto bar = std::make_unique<Bar>(Layout::HBox, assets);
bar->Add(std::make_unique<Button>(assets, IconId::Back,    CommandId::NavBack));
bar->Add(std::make_unique<Button>(assets, IconId::Forward, CommandId::NavForward));
bar->Add(std::make_unique<Button>(assets, IconId::Up,      CommandId::NavUp));
bar->Add(std::make_unique<Separator>(assets));
bar->Add(std::make_unique<Breadcrumb>(assets, pathSource, commandSink));
bar->Add(std::make_unique<AddressEdit>(assets, commandSink));
bar->Add(std::make_unique<DropButton>(assets, IconId::ChevronDown, CommandId::NavHistoryMenu));
```

**验收标准**：新建 `BookmarkBar` 时不允许出现 `CreatePen` / `FillRect` / `DrawIconEx` / `CreateFontW`，
只允许 `Bar + Button + Menu` 的组合。做不到就说明组件层缺东西——补组件，不在业务里补绘制。

---

## 4. 输入端（手脚架）

### 4.1 只做一件事：把输入变成命令

```cpp
struct InputEvent {                 // 归一化后的事件
    Kind kind;                      // Mouse / Key / Char / Ime / Wheel
    POINT pt; int key; int mods;
    std::wstring text;              // IME/字符
};

class InputRouter {                 // 手脚架：焦点 → 命中 → 绑定
public:
    bool Route(const InputEvent& e);          // 先给焦点组件，再查绑定表
private:
    BindingTable bindings_;                    // 键位 / 序列 / 鼠标
    Component* focus_ = nullptr;
};
```

| 输入源 | 产生什么 | 落到哪 |
|---|---|---|
| 鼠标（点击/双击/中键/滚轮） | `CommandId` + 命中对象 | 组件自身或绑定表 |
| 键盘（单键/组合键） | `CommandId` | 绑定表（`Ctrl+T` → `TabNew`） |
| **序列键**（`g` `g`、`Ctrl+K Ctrl+S`） | `CommandId`（带超时与前缀状态机） | 绑定表（序列解析器） |
| IME / 字符输入 | 文本 | 焦点 `Edit` 组件（不进管线） |
| 命令行 `--cmd` | `CommandCall` | 直接进管线（跳过绑定表） |
| AI `tools/call` | `CommandCall` | 直接进管线（跳过绑定表） |

**规则**：输入端不得改状态、不得调组件业务方法；它只能产出 `CommandCall`，或把字符交给焦点组件。
序列键与鼠标手势都是"绑定"而不是"功能"——新增 `g g` 只是绑定表加一行。

### 4.2 绑定的数据形态

```jsonc
// 与 mod 的 keymap.json 同结构，ExBarBrowser 只取自己支持的子集
{ "command": "TabNew",  "keys": ["Ctrl+T"] }
{ "command": "NavBack", "mouse": "XButton1" }
{ "command": "GoHome",  "keys": ["g", "h"], "timeoutMs": 800 }   // 序列
```

---

## 5. 命令管线（唯一基础管线）

```cpp
struct CommandCall { CommandId id; std::string argsJson; Actor actor; };

class Pipeline {
public:
    Result Run(const CommandCall&);      // 下面七步，顺序固定
};
```

| 步骤 | 做什么 | 失败表现 |
|---|---|---|
| 1 Validate | `argsJson` 对 `argsSchema` 校验 | 拒绝 + 日志（不改状态） |
| 2 Poll | 当前上下文是否可用（有没有标签、有没有选中） | 按钮灰显 / 工具返回"不可用 + 原因" |
| 3 Authorize | `Actor` × `Risk`：Human 直行、Agent 的 Write 需审批、Danger 永远确认 | 审批被拒 → 返回拒绝 |
| 4 Execute | 调 handler（薄），handler 调 core/platform/feature 的高层函数 | 异常/HRESULT 失败 → 回滚 |
| 5 Commit | 状态变更只在这里发生（Tab/History/Config） | — |
| 6 Emit | `PathChanged` / `SelectionChanged` / `CommandFinished` … | UI 与 AI 订阅同一份事件 |
| 7 Audit | 追加 `audit.jsonl`（actor/命令/参数哈希/结果/耗时/审批） | 审计不可关闭 |

**为什么"命令是基础管线"**：它把"谁能做、能不能做、做完怎么通知、出事谁负责"收在一处。
UI 只是管线的一个输入端，AI 只是另一个输入端。任何绕过管线的实现（直接 `SetPath`、直接改配置）
都会让审批、审计、撤销、测试同时失效。

---

## 6. 人机皆可操作

| | 人 | AI |
|---|---|---|
| 入口 | 鼠标/键盘/序列键/命令面板/`--cmd` | `tools/call`（MCP 子集） |
| 看到的状态 | 组件树 + `StateView` | `resources/read` → 同一份 `StateView` |
| 执行 | 绑定表 → 管线 | 工具调用 → 管线 |
| 授权 | 默认允许（Danger 需确认） | Write/Danger 需审批，超时拒绝 |
| 可发现性 | 命令面板列出全部命令 | `tools/list` 只列 `aiExposed` |
| 痕迹 | 审计（actor=Human） | 审计（actor=Agent）+ UI 上"AI 操作中"标记 |

**等价性验收**：任何 `aiExposed` 命令，必须能用鼠标/键盘完整做到同样的事；

> **覆盖到"所有操作"的完整模型**（四原语、三条可证伪判据、
> 控制/观测/开发三平面、AI 自我改进的闭环）见 
> [FULL-COVERAGE.md](FULL-COVERAGE.md)。本节只定义"人机等价"这一条约束；
> 那份文档回答"是否真的覆盖了全部操作、以及怎么证明"。
否则说明该命令依赖了 AI 专属通道，违规。

---

## 7. 目录与依赖

```
src/
├─ main.cpp
├─ contract/            契约（唯一跨层）
│  ├─ object_ids.h      窗口类名、WM_APP_*、payload
│  ├─ commands.h        CommandId + CommandSpec（argsSchema/risk/aiExposed）
│  └─ ports.h           CommandSink / StateSource / Navigator / PathSource
├─ core/                纯逻辑（零 Win32）
│  ├─ path_text / crumb_model / history / tab
├─ platform/            系统适配
│  ├─ win_base / log / system / theme
│  ├─ config/（P2）
│  └─ shell/{pidl, child_folders, shell_view}
├─ ui/                  组件框架（不含业务）
│  ├─ tokens/           tokens.json → tokens.gen.h（颜色/间距/字号/圆角/尺寸）
│  ├─ assets/           icon_registry、typography、brushes、cursors、metrics
│  ├─ component.h       Component / Painter / Layout / InputEvent
│  ├─ components/       icon、label、surface、separator、button、toggle、checkbox、
│  │                    edit、list_row、tab、breadcrumb、menu、toast、scroll_view
│  └─ containers/       bar、panel、dialog、popup、lister_surface
├─ input/               手脚架：input_router、binding_table、sequence、mouse_map、ime
├─ pipeline/            命令管线：pipeline、registry、validate、authorize、events、audit、state_view
├─ features/            业务组合（每个功能一个目录，含自己的载体实例化）
│  ├─ tabs/             tab_controller + tab_strip（Bar 的组合）
│  ├─ navigation/       nav_controller + nav_bar（Bar 的组合）
│  ├─ bookmarks/（P2）  bookmark_controller + bookmark_bar
│  ├─ sessions/（P2）
│  ├─ search/（P3）
│  └─ ai/（P4）         ai_panel（Panel 的组合）+ approval_ui
└─ app/                 runtime、lister、layout、assembly（唯一装配点）
```

依赖：

```
contract ← core ← platform ← pipeline ← features ← app
                     ↑           ↑
                     └──  ui  ───┘
                     input 依赖 ui（命中/焦点）与 pipeline（发命令）
```

- `ui/` 不认识 `pipeline/` 与 `features/`：组件只持有 `CommandSink` 接口。
- `input/` 可依赖 `ui/`（焦点与命中）与 `pipeline/`（发命令），**不得依赖 features**。
- `features/` 实例化载体、注册命令、订阅事件；**不写绘制**。

---

## 8. 迁移批次

| 批次 | 内容 | 验收 |
|---|---|---|
| **B1 去重** | `common.h` 拆分；`Painter` 收编 chevron/cross/fill；`Assets` 收编两处取图与字体；日志/路径工具合并；`app.cpp` 拆 layout/commands | 编译零警告；冒烟 20 项；截图逐像素一致 |
| **B2 管线** | `contract/commands.h` + `pipeline/`（validate/poll/authorize/execute/emit/audit）；`ACCEL` 改绑定表；`--cmd` 进管线 | 11 条命令全部经管线；快捷键不变；audit.jsonl 有记录 |
| **B3 状态分离** | `core::Tab` + `core::History`；`App` → `app::Lister` + `app::Runtime`；`StateView` 只读快照 | 标签/历史/视图模式回归；`lister.cpp` <200 行 |
| **B4 组件化** | `ui/tokens`、`ui/assets`、`ui/component.h`、`ui/components` 落地；现有三个栏的绘制迁入组件（HWND 暂不变） | 三个栏由组件组合而成；`ui/` 无业务；新建 `BookmarkBar` 只写组合代码 |
| **B5 载体化** | `ui/containers` 落地；`Bar/Panel/Dialog/Popup/Lister` 统一接口；chrome 合并进 Lister 客户区 | 窗口类 ≤2（Lister + Popup）；IME/焦点/DPI 全绿 |
| **B6 输入端** | `input/` 落地：绑定表（键位/序列/鼠标）+ 路由器 + 焦点链；序列键可用 | 新增序列键只改 JSON；输入层零状态修改 |
| **B7 人机同源** | `ApprovalBroker` + `AgentBridge`（MCP 子集）+ 最小 UIA + UI 的 actor 标记 | AI 能读快照、执行 Read、Write 需审批；审计可追溯 |
| **B8 首个新功能** | 按"新增功能"流程落地书签栏（`features/bookmarks`：`Bar + Button + Menu` 组合 + 读 `bookmarks.html`） | 不改 `app/`、不写绘制即可完成；`ui/` 未新增重复实现 |

---

## 9. 业界参照（为什么这样定）

| 参照 | 它验证了哪条命题 | 我们取 | 我们舍 |
|---|---|---|---|
| **Blender operator**（约 2000 个 operator 注册表；`poll()` 判可用；`{REGISTER,UNDO}`；UI/搜索/快捷键/脚本都引用同一 id；官方要求 handler 只调高层函数） | ③ 命令是管线 | 注册表 + poll + undoable + 可搜索 + handler 薄 | 模态 operator、`bContext` 上下文注入 |
| **MCP tools**（`name/description/inputSchema`；`tools/list`/`tools/call`；规范要求 human-in-the-loop、危险操作确认、输入校验与限流；`readOnlyHint/destructiveHint`） | ③ 人机同源 | 命令表直接生成 `inputSchema`；risk 映射注解；审批/审计 | 完整 JSON-RPC 服务端、网络传输 |
| **Directory Opus**（命令是产品 API；`dopusrt /cmd`；插件按类型窄接口） | ③ + ② | 命令唯一入口 + 外部通道 + 载体/扩展窄接口 | 插件 DLL ABI、脚本引擎、双栏 |
| **VS Code workbench**（commands/views/menus 都是贡献点，UI 只是渲染） | ① 组件化 | 稳定 id、UI 与实现分离 | Electron/Web 栈 |
| **Chromium Views / Qt**（控件树 + 布局管理器 + 单一绘制面） | ① 组件 + ② 载体 | Component/Container 两分、Layout、单一 surface | 完整保留模式框架、动画/变换树 |
| **Design Tokens（DTCG）** | ① 资源组件 | 颜色/间距/字号/圆角/尺寸是数据 | 主题编辑器 |

---

## 10. 不做什么

- 不做脚本引擎、插件 DLL ABI、双栏、XML 工具栏。
- 不做完整保留模式框架（只做 Measure/Arrange/Paint，不做动画/变换树）。
- 不让组件持有 `HICON/HFONT/HBRUSH` 所有权；不让载体写业务。
- 不做 AI 专属通道：AI 的能力 = 命令表里 `aiExposed` 的那些命令。
- 不做"每个栏一个窗口"的新栏；新栏只能是 `Bar` 的实例。
