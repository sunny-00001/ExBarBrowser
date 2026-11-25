---
status: proposed
baseline: 2026-11-22 20:25
---

# 目标架构：一切皆组件，命令是唯一管线

> 三条命题（本文全部结构都由它们推导）：
> **① 一切对象都是组件**——图标、字体、背景、光标、按钮、标签、列表行、弹窗项、菜单……
> **② 栏 / 面板 / 对话框 / 弹层 / 窗口是"载体"**——只负责布局、生命周期、焦点、绘制顺序；
>    新建载体时复用已有组件，不重复造轮子。
> **③ 鼠标 / 键盘 / 序列键 / IME / 命令行 / AI 工具调用是"手脚架"（输入端）**——
>    它们只做一件事：把输入变成**命令**；**命令是唯一基础管线**，人与 AI 都从这条管线走。
>
> 可机械校验的约束见 [ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md)（R1–R21）。
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
| `WebPanel`（候选） | 单块滚动内容，可增量更新 | 可显示/隐藏 | AI 面板消息区的**路线 B**：Markdown / diff / 流式 —— 可选运行时。**2026-11-22 定：P4-b 不采用，候选留到 P4-c 之后按实测需要再定** |
| `RichPanel`（候选） | 单块滚动内容，原生富文本，可增量更新 | 可显示/隐藏 | 同上的**路线 A**：DirectWrite 排版 + 自写 Markdown 子集。Pulse 实证可行（D3）。**同样 P4-b 不采用** |
| 纯文本消息区（P4-b 实际用） | 单块滚动文本，按段追加 | 可显示/隐藏 | **P4-b 的消息区就是它**：GDI 换行，不解析 Markdown。零新增依赖 |

### 3.1.1 `WebPanel`：唯一的 Web 化入口

**它是什么**：一个把 WebView2 装起来的 `Panel` 子类。除它之外，`ui/` 里没有任何东西碰 WebView2。

**为什么单独成一个载体而不是"Panel 的一种"**：因为它**有运行时依赖**——
WebView2 Runtime 在部分 Win10 机器上没装（见 D3：单 exe 零依赖是硬约束）。
`WebPanel` 必须能回答两个问题：

1. **Runtime 不可用时怎么办** → `WebPanel::Available()` 返回 false，
   调用方（`features/ai/ai_panel`）据此**降级到原生渲染**（纯文本 + 原始 Markdown 源码），
   面板其余部分（活动流、审批块）**完全不受影响**。
2. **它挂了怎么办** → WebView2 进程被杀 / 渲染线程卡住时，
   `WebPanel` 必须能被**单独丢弃并重建**，不影响 `Panel` 的其余子组件。

**这两条是硬要求，不是"最好有"**。理由见 D3：审批块是**安全边界**，
绝不能让它依赖一个可选运行时。所以规定：

> **凡是用 `WebPanel` 渲染的内容，都必须有一条等价的原生降级路径。**
> 安全关键内容（审批、命令结果、审计事实）**一律不得**只存在于 `WebPanel` 里。

**依赖方向**：`WebPanel` 属于 `ui/containers/`，但它需要读 `tokens.json` 里的色值
（`ui/tokens` 已在 `ui/` 内，不违反 `ui/` 不依赖 `features/` 的规则）。
它**不认识** `CommandSink` —— 面板要发命令，走 `Panel` 的宿主给的接口。

**它与 `ui/theme/tokens.css` 的关系**：CSS 是**唯一色值真源**（D6），
`tokens.json` 由它生成（`ui/tokens/tokens.json → tokens.gen.h`）。
WebView2 侧注入 CSS 变量，不允许在 HTML 里写死色值 —— 与 GDI 侧共用同一份真源，
这样"两份色值漂移"的风险被压到只剩"CSS 变量名映射"这一处可机械检查的差异。

### 3.1.2 `WebPanel` 不得承载的内容

以下是**清单式**的排除项。列进来是为了让"能不能放 Web 里"这个问题有确定答案，
而不是每次重新讨论：

| 内容 | 载体 | 理由 |
|---|---|---|
| 危险操作审批块 | **原生 `Panel` 级固定条** | 安全边界（D3）：不能挂在可选运行时上，不能随页签切换而消失 |
| 命令活动流 / 事件时间线 | 原生 `ListRow` | 结构化数据，命中/滚动/键盘要自己掌控 |
| 审计事实、错误码、断言判据 | 原生 | 机器可读，不该经过 Markdown 渲染 |
| agent 的散文、代码块、diff | **内容区**：`RichPanel`（原生）或 `WebPanel`（Web），二选一 | 这才是真正需要富文本与流式的地方。**2026-11-22 定：P4-b 用纯文本，两个候选都排在 P4-b 之后**（D3） |

**推论**：AI 面板 = 原生外壳（头部 + 页签 + 审批条 + 输入区 + 活动视图）
+ 一块**内容区**（对话消息区）。**P4-b 的内容区是纯文本**；
原生富文本还是 Web，两条候选都排在 P4-b 之后，且两条路都必须满足
"缺席时面板仍然完整可用"。

### 3.1.3 AI 面板的宿主：同一客户区最右的一列

**落点**（2026-11-22 定）：AI 面板是**同一个客户区里最右边的一块**，
由我们的布局代码划出来，**把 Shell 窗格区往左挤**（squeeze），不盖在它上面。

```
┌──────────────────────────────────────────────────────────┐
│ 标签栏 / 导航栏 / 书签栏                                  │
├──────────┬──────────────────────────────┬────────────────┤
│ nav pane │ Shell 窗格区（inner）          │ AI 面板列      │
│ （自绘）  │  split pane + 预览窗格         │ （自绘）        │
└──────────┴──────────────────────────────┴────────────────┘
```

**为什么能划在最右**：`src/app_layout.cpp` 的横向分区是**我们自己的代码**算的：

```cpp
const int navW = navPane_.visible() ? Config::Get().s().navPaneWidth : 0;
navPane_.Resize(navW, viewH);
RECT inner{navW, 0, rc.right, viewH};   // ← 改成 rc.right - aiW 即让出最右一块
```

注释原文："占位由我们的布局算，不由 shell 说了算"。
而 **Shell 的预览窗格是 `inner` 内部的子窗口**（`shell_view_preview.cpp` 找的窗口类名是
`"Shell Preview Extension Host"`，由 `IExplorerPaneVisibility::GetPaneState` 驱动），
它**不占 `viewArea_` 的最右** —— 所以最右这块是我们的，AI 面板与预览窗格**不抢位**。

**为什么是 squeeze 而不是 overlay**：overlay（盖在列表右边）会**遮住文件列表的右半边**，
被遮住的恰好可能是选中项或正在看的内容。squeeze 让 Shell 自己重排列宽 ——
details 视图的列按比例重分配，与 explorer 拖窗口宽度是同一种行为，用户熟悉。
**代价（要实测，不是已知缺陷）**：Shell 列表是按 item 滚动的，宽度变化时滚动位置一般保持；
真正会变的是列宽分配；**details 视图宽于窗口时的横向滚动位置是否重置**需实测。

**宽度**：AI 面板宽度**复用 nav pane 的分割条机制**（拖动改配置项），
不写第二套拖拽。`inner` 要留下限，**否则 Shell 的预览窗格会被挤没**。

**启停**：`win.ai.toggle`（`CmdId::AiTogglePanel`，命令 ID 已预留、
名字已占位，只是 `implemented=false`）由快捷键控制，**不占宽度时宽度为零**。
状态栏另有一个 `✦ AI` chip 作为入口 —— 状态栏是段位图驱动的
（`StatusSegmentIcon` / `SetEnabledSegments` / `HitSegment`），加这一段要动段位图映射。

**焦点契约（两态，必须显式定义）**：

| 态 | 谁拿键盘 | 进出方式 |
|---|---|---|
| 文件区态 | Shell 列表 | 快捷键 → 面板态 |
| 面板态 | 面板输入框 / 面板内控件 | `Esc` 或快捷键 → 文件区态 |

`Esc` 三级（与 Pulse 一致）：面板内有菜单 → 关菜单；agent 正在跑 → **停这一轮**
（`win.ai.stop`）；否则 → 收起面板。**焦点必须能一键回到文件列表**，
不能出现"键入被面板吃掉但用户不知道焦点在哪"的状态。

**收起不是消失**：面板收起后，状态栏 chip 上方浮一张 **peek 气泡**
（完成摘要 + 撤销 + 打开对话），这是"命令在后台跑完了、需要时瞄一眼"的落点。
Pulse 的做法：318 DIP 宽的卡片、14 圆角、左下角一个尾巴指向 chip。

**跨标签页**：AI 列**全局**，切标签页时保持开着（D10 的角色是"常驻助手"）；
对话历史同样全局（`events.jsonl` 按 traceId + `chat.jsonl` 存散文），不按标签分。

**参照与取舍**（`jimmgreen/pulse`，在售的 Windows 文件管理器）：

| Pulse | 我们取 | 我们舍 |
|---|---|---|
| 同客户区一列，挤压重排 | **照抄** | — |
| Docked 宽 = clamp(剩余, 300, 408) DIP | 只取"给文件区留下限"这个思路，宽度改用分割条 | 它的固定区间 |
| 停靠时侧栏自动折叠成导轨 | **不抄** | 我们左侧是全局导航树，折叠会让用户失去参照 |
| Floating（408×600 卡片盖左下角） | **不抄** | 我们没有导轨概念，浮动卡没有锚点 |
| Peek 气泡 | **照抄** | — |
| 真 `EDIT` 控件 + 自绘外观 | **照抄** | 我们已有 `ui/components/edit`，直接复用 |
| Ctrl+J 三态 | **照抄** | — |

### 3.1.4 AI 面板的数据来源、审批通道与线程模型

面板的三样东西各有各的来源，都不是"现成的"，实现前必须先知道它们的真实形状。

**① 活动流：进程内事件订阅（新加）**

`src/ai_events.h` 的事件面只有三个入口：`Publish` / `Since`（拉）/ `WaitFor`（阻塞等一行）。
**没有进程内回调** —— 管道协议 `events/subscribe` 是给外部客户端的，面板用不上。

所以要新增：**一个订阅线程跑 `WaitFor(afterSeq, &line, -1)`，把行投递到 UI 线程**。
`Publish` 是线程安全的，而且**事件在返回值之前发布**
（`ai_api.cpp` 注释：订阅者拿到 command 事件后再去读 state，才能看到这次操作之后的状态）——
所以 UI 线程收到事件时，状态已经是新的。

**② 面板里显示什么：只有 AI 发起的命令**

`command` 事件的**唯一发布点**是 `src/ai_api.cpp:417`（在 `ai::Call` 里）。
用户手动点标签栏、排序、改视图走的是 `CommandBus` 的另一条路，**不发事件** ——
所以"AI 活动流只显示 AI 发起的"这件事**天然成立**，不需要额外过滤。

但 `data.actor` 目前**硬编码 `Actor::Ai()`**，于是
**管道 MCP 客户端的命令与内嵌 ACP agent 的命令分不出来** —— 要分就得加字段。

事件类型也不止一种：还有 `tool.job.progress` / `tool.job.done` / `dev.job.done`。
活动流要按类型分派，不能只认 `command`。

**③ 分组键：用 `traceId`，不用 `turn`**

`RequestScope` 在**请求边界**递增 turn（`ai_events.cpp`：`t_trace.turn = ++g_turn`），
所以 **MCP 的一次 `tools/call` = 一个 turn**。一次对话轮里的多个工具调用会是**多个 turn**。
把 turn 当"对话轮"是错的。

干净的解法：**分组键用 `traceId`**（W3C trace context，32 hex）。
形态 A（D13）下请求是我们自己发的，所以更简单：**一次对话请求给它的所有
`tools/call` 注入同一个 traceparent** → traceId 相同 → 自然归成一组；
`turn` / `step` 退为组内的请求级细粒度。

**④ 审批：需要一条宿主主动询问的通道（新加）**

现有双门是**客户端声明式**的：`aiAllowDanger`（设置里的静态开关）+ `approve=true`
（调用方带上的参数）。宿主那个弹框回调 `confirmFn_`
**只对 `ActorKind::Human` 生效**（`commands_confirm.cpp`：
`if (actor.kind != ActorKind::Human) return true;`），AI actor 走不到；
而且它是**同步返回 bool** 的。

形态 A 下这个问题**依然存在，而且更隐蔽**：现在是**我们自己**当那个"客户端"，
所以最容易把审批写成"我声明过了"。方向仍然是反的 —— 需要的是
"执行方挂起 → 问用户 → 用户答 → 继续执行"的**异步往返**，而不是调用参数。

**要新增的是**：宿主侧审批请求通道 —— 宿主挂起一个待批请求 →
面板显示为**面板级固定条**（不随页签切换消失，D3）→ 用户点完宿主回执。
这条通道**当前不存在**，是 P4-a 审批 UI 的**隐藏前置**。

**⑤ 线程模型（三条硬约束）**

| 约束 | 为什么 |
|---|---|
| 审批回调**必须在 UI 线程**执行 | 审批块要画在面板上；调用方是 AI 引擎线程 |
| **UI 线程绝不做网络** | `WinHttpReadData` 阻塞到有数据为止 → 在 UI 线程调 = 界面假死 |
| **UI 线程不得同步等待模型响应** | 否则审批框弹不出来 → 死锁 |
| AI 引擎线程 **独立于 `ai_panel`** 生命周期 | 面板收起后请求仍在跑，结果要进 peek 气泡；并且**流式请求必须能跑完**，不能因为面板收起就丢结果 |

**⑥ 消息块与活动流的关联**

消息区的块（纯文本）目前**没有 traceId**，活动流有。
要让"这个回答背后跑了 5 条命令"成立，消息块也要带 traceId。

**⑦ 引擎线程归属：每窗口一条**

D4 定了多实例。两个窗口 = 两个 AI 面板 = **两条 AI 引擎线程**（原 D10 子决定①
"每窗口一个 agent 子进程"在形态 A 下变成"每窗口一条引擎线程"，理由不变）：
每个窗口的对话上下文里带着"用户在看哪个窗口、选中了哪些文件"，
而 `get_state` 类工具返回的是**当前进程**的状态 —— 共用一个引擎就会串上下文，
模型会做出错误推断。

**注意**：这条与 D10 的"每窗口一个 agent 进程"是同一件事在形态 A 下的对照物，
但**不再有"N 个 agent 进程占 N 倍内存"的问题** —— 引擎线程只占一个线程栈
（默认 1 MB 保留、实际提交远小于此），远小于一个独立的 agent 进程。

**⑧ 对话历史另存 `chat.jsonl`**

`events.jsonl` 存的是**事实**（谁调了什么命令、成没成），**不含 agent 的散文**。
只存事件的话，程序重启后面板只剩"命令活动"，看不到"当时说了什么"。

新增 `<dataDir>/chat.jsonl`，与 `events.jsonl` 同目录同约定
（`tf::Join(Config::Get().Dir(), L"chat.jsonl")`，追加写 + 按大小轮转，坏行跳过）。
**一条记录 = 一个对话轮**，带 `traceId` 与 `events.jsonl` 关联。

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
否则说明该命令依赖了 AI 专属通道，违规。

> **覆盖到"所有操作"的完整模型**（四原语、三条可证伪判据、控制/观测/开发三平面、
> 以及 AI 自我改进的闭环）见 [FULL-COVERAGE.md](FULL-COVERAGE.md)。
> 本节只定义"人机等价"这一条约束；那份文档回答"是否真的覆盖了全部操作、以及怎么证明"。

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
│                       （web_panel / rich_panel 均为候选，排在 P4-b 之后）
├─ input/               手脚架：input_router、binding_table、sequence、mouse_map、ime
├─ pipeline/            命令管线：pipeline、registry、validate、authorize、events、audit、state_view
├─ features/            业务组合（每个功能一个目录，含自己的载体实例化）
│  ├─ tabs/             tab_controller + tab_strip（Bar 的组合）
│  ├─ navigation/       nav_controller + nav_bar（Bar 的组合）
│  ├─ bookmarks/（P2）  bookmark_controller + bookmark_bar
│  ├─ sessions/（P2）
│  ├─ search/（P3）
│  └─ ai/（P4）         ai_panel（Panel 的组合：activity_stream + approval_ui
│                       + agent_view[纯文本消息区，候选 RichPanel/WebPanel 排在 P4-b 之后]）
│                       + ai_sse（SSE 流式解析，纯函数）      ← D13 形态 A
│                       + ai_http（WinHTTP 传输）             ← D13 形态 A
│                       + ai_key（DPAPI 密钥）                ← D13 形态 A
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
| **B9 AI 面板载体** | `features/ai/ai_panel` 落地为 `Panel` 组合（activity_stream + approval_ui + **纯文本 agent_view**）。**`ui/containers/web_panel` 不在 B9 里** —— 两条富文本路线（`RichPanel` / `WebPanel`）都排在 P4-b 之后。**同时新增两条通道**：① 进程内事件订阅线程（`WaitFor` → UI 线程）；② 宿主侧审批请求通道（异步，与 `confirmFn_` 不是同一条） | 面板在**零新增依赖**下完整可用：活动流、审批块、纯文本消息区；`build_fast.py` 的 `LIBS` 不动；危险命令**真的**会弹框问用户（而不是靠客户端带 `approve=true` 声明） |
| **B10 模型通道（形态 A，D13）** | `features/ai/ai_sse`（SSE 解析）+ `ai_http`（WinHTTP 流式）+ `ai_key`（DPAPI）落地。~~原 B10 是 ACP 客户端，已被 D13 取代~~ | 面板输入框能发一句话给模型；模型返回的 `toolCall` 经 `CommandBus` 执行；危险命令仍需人工批准；长回复逐段出现、滚动不卡；断网/key 错显示具体原因 |

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
| **ACP（Agent Client Protocol）**（Zed 2025-08 发布，Spec 1.0 开放治理冻结，Apache-2.0；背书 Zed / JetBrains / GitHub / Google / Cognition；JSON-RPC 2.0 over stdio，四原语 dispatch/observe/interrupt/collect；`session/request_permission`；宿主把 MCP 服务器列表交给 agent） | ③ 人机同源 | 宿主↔agent 的通道用 ACP 标准；agent 回调工具走既有 MCP | 官方 SDK（禁第三方库，wire protocol 自研 ~400 行）；agent 的 fs/terminal 原语（会绕过危险双门） |
| **Obsidian Copilot v4**（Agent Chat 内嵌 OpenCode / Claude Code / Codex；"Claude uses the account held by the Claude Code CLI. You do not paste that account's key"；"asks before actions that need your approval"；BYOK key 存 Obsidian Keychain） | ③ + 审批模型 | 借用 agent 登录态（不存 key）；危险动作前必须人工批准；权限粒度对齐 `Effects` | 托管下载 agent 二进制（与"单 exe 零依赖"冲突）；多 agent 并行 |
| **Zed Agent Panel**（原生 GPUI，零 WebView；"built AI into our editor's foundation"） | ① + 载体 | 面板是原生组件，不是外挂网页 | WebView2 |
| **5 个在售产品的共同做法**（DeepSeek Desktop：C++/Win32 + WebView2，WinHTTP 工作线程按行切 SSE；PowerBox：MFC + WinHTTP + 6 家 provider；TizuMark：纯原生 DirectWrite/Direct2D，42 MB / 0.8 s；Lain AI Desktop：Tauri，"the API key never enters the webview"；PolyUI：Tauri，requestAnimationFrame 批量累加 token） | 形态 A（D13）的可实现性 | **没有一家在渲染层发 HTTP**：网络在原生命令后面，渲染层只显示 → 我们的三层架构（UI / 引擎线程 / WinHTTP）；**只投增量**、定时器合并重绘 → 长回复不卡 | Electron 全家桶、把 key 放进前端、每 chunk 重排全文 |
| **Pulse**（在售的 Windows 文件管理器，C++20 + Win32 + Direct2D，Apache-2.0；**全仓 `webview`/`webview2`/`cef` 命中 0**；富文本链路 = md4c → `ai_rich_text.cpp`(472) → `ai_panel_rich.cpp`(1460，DirectWrite 排版 + Direct2D 画 + 命中/选择) → `syntax_highlight.cpp`(654)；LumaText 只是 `delayimp` 可选加速件，缺失回落 DirectWrite；内置助手走 OpenAI chat format 自建 function-calling agent 循环，`kMaxRounds=12`；外部 MCP 客户端另走 `pulse.exe --mcp-stdio`，与内置助手是两条独立能力线；**面板宿主 = 同客户区一列，挤压重排，停靠时侧栏自动折叠成导轨，关闭后变状态栏 chip 上方的 peek 气泡；输入框是真 `EDIT` 控件 + 自绘外观；Ctrl+J 三态 / Esc 三级**） | ③ + 载体 | **富文本聊天不必 Web 化**（原生 DirectWrite 可行）→ D3 分流表补第三路；动作表检索层 `find_actions` + 多步编排 `run_actions`（≤12 步为一个 undo group）+ 计划卡确认 → D11；**宿主形态**：同客户区一列 + squeeze + peek 气泡 + 真 EDIT + 三态快捷键 → D12 | 自带 md4c 与 LumaText（第三方，禁）；10 档 risk（本工程 3 档 + `Effects` 已够）；~~形态 A 的出网与密钥管理（与 D10 冲突）~~ → **D13 已改向形态 A，这条不再是"舍"，改为"取"**：Pulse 的 WinHTTP + DPAPI 正是我们的实现参照；**停靠时侧栏折叠成导轨**（我们左侧是全局导航树，不折）；**Floating 态**（408×600 卡片盖左下角，我们没有导轨锚点） |

---

## 10. 不做什么

- 不做脚本引擎、插件 DLL ABI、双栏、XML 工具栏。
- 不做完整保留模式框架（只做 Measure/Arrange/Paint，不做动画/变换树）。
- 不让组件持有 `HICON/HFONT/HBRUSH` 所有权；不让载体写业务。
- 不做 AI 专属通道：AI 的能力 = 命令表里 `aiExposed` 的那些命令。
- 不做"每个栏一个窗口"的新栏；新栏只能是 `Bar` 的实例。
- **不做"AI 专属通道"**：AI 的能力 = 命令表里 `aiExposed` 的那些，经 `CommandBus` 执行。
  **不给 AI 任何直通文件系统的旁路**（这条与形态无关，D10/D13 一致）。
- ~~不做内置 LLM 客户端~~ → **2026-11-22 21:54 改向（D13）**：改为形态 A，
  **做**内置模型客户端（WinHTTP + SSE + DPAPI key）。放弃的是"零出网"，
  零依赖仍成立（WinHTTP/Crypt32 都是系统库，单 exe 不变）。
- **不做 ACP / 不拉起外部 agent 子进程**（D10 的形态选择已被 D13 取代）。
- **P4-b 不引入 WebView2，也不引入 DirectWrite/Direct2D**：消息区先做纯文本流式，
  渲染层排在 P4-b 之后（2026-11-22 决定，D3）。`build_fast.py` 的 `LIBS` 本期不动。
