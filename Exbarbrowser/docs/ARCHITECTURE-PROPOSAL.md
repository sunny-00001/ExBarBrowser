---
status: proposed
baseline: 2026-11-19 20:32
---

# 架构方案（待拍板）

> **这份文档只做决策，不施工。** 拍板前不改任何源码。
> 拍板后按第 7 节的批次迁移，每批都能编译、能运行、能回退。

---

## 1. 现状（实测，2026-11-19 20:32）

| 指标 | 实测值 | 说明 |
|---|---|---|
| 源文件 | 19 个（10 `.cpp` + 9 `.h`） | 全平铺在 `src/` |
| 总行数 | 3,202 | 其中 `nav_bar.cpp` 557、`app.cpp` 438、`shell_view.cpp` 316 |
| 最大文件 | `nav_bar.cpp` 557 行 | 已超自己定的 500 行上限，且在继续长 |
| 头文件依赖 | 18/19 直接或间接包含 `common.h` | `common.h` 已变成"什么都有"的公共垃圾场（178 行：Win32 头 + 类名 + 消息 + ComPtr + 路径 + DPI + 日志 + 深色） |
| 重复实现 | `DrawChevron` 3 份（`nav_bar` / `popup_list` / `tab_strip`）、`ParentOf`/`LeafOf` 2 份（`common.h` 与 `nav_model.h`）、日志 2 份（`common.h::LogDbg` 与 `shell_view.cpp::LogLine`）、目录图标逻辑 2 份（`NavBar::UpdateLeadIcon` 与 `nav_bar_actions.cpp::IconFor`） | 都是"复制粘贴后各改各的" |
| 状态归属 | `App` 单例持有全部标签状态；chrome 无状态 | 这一点目前是对的，不要推翻 |
| 文件级耦合 | 无 L2 循环依赖；`app.h` 是唯一聚合点 | 结构没烂，但**没有层**，所以"谁能 include 谁"全靠自觉 |

**结论**：不是"烂架构"，而是**没有架构**——文件按"当时写哪个窗口"自然长出来，
所以 `nav_bar.cpp` 同时管布局、命中、绘制、图标、日志；`common.h` 同时管平台、契约、工具、业务常量。
下一步要加书签栏、搜索、会话、命令面板、预览窗格时，这些新东西没有"该放哪"的答案。

---

## 2. 架构目标（可检验，不是口号）

| 目标 | 判据（可脚本校验） |
|---|---|
| 单一职责 | 每个 `.cpp` ≤300 行；文件名能一句话说清职责；一个文件只有一个变化理由 |
| 高内聚 | 一个功能的全部代码在同一个目录里；改"书签栏"不需要打开 `app.cpp` 以外的 3 个目录 |
| 低耦合 | 依赖单向、无环；同层模块之间零 include；跨层只经 `contract/` 与显式端口 |
| 高扩展 | 新增一个功能 = 新增一个目录 + 在装配层注册 1 处；不改既有模块内部 |
| 可测 | `core/` 零 Win32/COM，可脱离窗口跑断言测试 |
| 可回退 | 每批迁移独立可编译、可运行；出问题只回退该批 |

---

## 3. 三个候选方案

### 方案 A：分层目录（按技术职责切）

```
src/
├─ contract/     窗口类名、消息、命令 ID（跨层契约）
├─ core/         纯逻辑：路径文本、面包屑模型、历史栈
├─ platform/     Win32/COM/注册表/文件：win_base、log、system、theme、shell/
├─ ui/           所有自绘控件：tab_strip、nav_bar、popup_list、status_bar
├─ app/          窗口、布局、消息路由、命令分发
└─ main.cpp
```

- **优点**：改动最小、心智负担最低；`common.h` 被拆掉，重复实现收敛到 `platform/` 与 `ui/components/`。
- **代价**：功能是横切的——"书签栏"会同时落在 `ui/`（控件）、`platform/`（`bookmarks.html` 读写）、
  `app/`（装配 + 命令），一个功能仍要动三处。
- **什么时候选它**：确定后续只加"控件级"功能，不打算做搜索/会话/命令面板/AI。

### 方案 B：分层 + 垂直切片（推荐）

在 A 的基础上，把"功能"也变成一个目录：

```
src/
├─ contract/            跨层契约：window_ids.h、messages.h、commands.h、ports.h
├─ core/                纯逻辑（零 Win32）
├─ platform/            系统适配（Win32/COM/IO）
│  └─ shell/            pidl、child_folders、icons、shell_view
├─ ui/
│  ├─ kit/              自绘原语：draw、popup_list（唯一绘图实现）
│  └─ chrome/           窗口级控件：tab_strip、nav_bar(+paint/actions)、status_bar
├─ features/            垂直切片：一个功能一个目录
│  ├─ tabs/             标签生命周期 + 历史 + 关闭栈
│  ├─ navigation/       后退/前进/上级/地址跳转/视图模式
│  └─ (P2/P3) bookmarks/ sessions/ search/ palette/ preview/
└─ app/                 装配：窗口、布局、消息路由、命令注册表
```

- **优点**：新增功能 = 新增 `features/<name>/`，只改 `app/` 一处注册；功能内部可以自由组合
  `ui/components` 与 `platform/`；功能之间只经 `contract/ports.h` 的窄接口（如 `Navigator`、`PathSource`），
  互相不 include。
- **代价**：需要先定义端口接口（约 3 个：`Navigator`、`PathSource`、`CommandSink`）；
  目录更深（4 级），小改动要跳目录。
- **什么时候选它**：路线图里还有 6 个以上功能（书签、搜索、会话、命令面板、预览、AI）——**当前就是**。

### 方案 C：命令/消息驱动（中央 store + dispatcher）

所有用户意图变成 `Command`，中央 dispatcher 执行，UI 订阅状态变更。

- **优点**：命令面板、快捷键、AI 调用天然统一（正好对上 mod 的 127 条命令目录）。
- **代价**：Win32 同步消息模型下引入订阅生命周期、重入、异步刷新；3,000 行规模属于过度设计；
  调试时"谁改了状态"更难追。
- **结论**：**不选它作为整体架构**，但吸收其中一件事——**命令注册表**（同步调用，不做事件总线），
  放到阶段 3。

---

## 4. 推荐：方案 B，分三批落地

推荐理由一句话：**这个项目的增长点是"功能"，不是"控件"**。书签栏、搜索、会话、命令面板、预览
都是"一个功能 + 少量 UI + 一份持久化"，用 A 会让每个功能散在三层；用 C 会把 3,000 行撑成框架。

### 4.1 目标目录（含行数预算）

```
src/
├─ main.cpp                               ~80   进程入口：DPI/COM/激活优先启动/命令行
├─ contract/
│  ├─ window_ids.h                        ~40   6 个窗口类名 + WM_APP_TAB_*
│  ├─ messages.h                          ~30   CopyDataPayload + kCopyDataMagic
│  ├─ commands.h                          ~60   命令 ID + 命令表（P3 从这里生成 capabilities 子集）
│  └─ ports.h                             ~60   Navigator / PathSource / CommandSink 三个窄接口
├─ core/                                  零 Win32，可单测
│  ├─ path_text.{h,cpp}                   ~80   ParentOf/LeafOf/Normalize/FormatItemCount
│  ├─ crumb_model.{h,cpp}                 ~150  路径→分段/ghost/AddressText（现 nav_model 的纯逻辑部分）
│  └─ history.{h,cpp}                     ~100  线性历史：Push/Back/Forward/List/Jump/截断
├─ platform/
│  ├─ win_base.h                          ~80   windows 头 + ComPtr + SetThis/GetThis + DpiOf
│  ├─ log.{h,cpp}                         ~50   LogDbg（唯一实现）
│  ├─ system.{h,cpp}                      ~120  DPI 感知 / 深浅色 / 标题栏
│  ├─ theme.{h,cpp}                       ~170  令牌 + 字体 + 画刷 + S()
│  ├─ config/（P2）                         —    settings/state/bookmarks/sessions 读写
│  └─ shell/
│     ├─ pidl.{h,cpp}                     ~70   PidlName + 解析名规范化
│     ├─ child_folders.{h,cpp}            ~90   直接子文件夹枚举（自然排序、跳过隐藏）
│     ├─ icons.{h,cpp}                    ~50   目录/文件 HICON（含所有权约定）
│     └─ shell_view.{h,cpp}               ~330  一个标签 = 一个 IExplorerBrowser
├─ ui/
│  ├─ kit/
│  │  ├─ draw.{h,cpp}                     ~140  Fill/FillRound/DrawChevron/DrawGlyph
│  │  └─ popup_list.{h,cpp}               ~300  WS_POPUP + owner-draw LISTBOX
│  └─ chrome/
│     ├─ tab_strip.{h,cpp}                ~290
│     ├─ nav_bar.{h,cpp}                  ~300  窗口/布局/命中
│     ├─ nav_bar_paint.cpp                ~250  按钮与面包屑绘制（从 557 行里拆出）
│     ├─ nav_bar_actions.cpp              ~220  编辑/补全/四类菜单
│     └─ status_bar.{h,cpp}               ~90
├─ features/
│  ├─ tabs/tab_controller.{h,cpp}         ~220  标签生命周期 + 历史 + 激活（从 App 搬出）
│  └─ navigation/nav_controller.{h,cpp}   ~200  导航/视图模式/历史跳转（从 App 搬出）
└─ app/
   ├─ app.{h,cpp}                         ~200  窗口 + WndProc + 主题应用 + 装配
   ├─ layout.{h,cpp}                      ~60
   └─ commands.cpp                        ~90   ACCEL + 命令注册表 + 分发
```

**每个文件都 ≤300 行**（`shell_view` / `popup_list` 接近上限，超了就按"协议/绘制"再拆）。

### 4.2 依赖规则（7 条，可机械校验）

```
contract  ←  core  ←  platform  ←  ui  ←  features  ←  app
（箭头 = "可以被依赖"；上层可依赖下层，下层不得包含上层头文件）
```

1. `core/` 任何头文件**不得出现 `windows.h`**（脚本 grep 校验），保证可单测。
2. `ui/` 不得包含 `app/` 或 `features/`；`features/` 不得包含 `app/`。
3. 同层目录之间零 include：`ui/components` 内各组件互不引用；`features/*` 之间只经 `contract/ports.h`。
4. 窗口类名、`WM_APP_*`、命令 ID **只允许**定义在 `contract/`。
5. 绘图原语只允许来自 `ui/components`（内部统一走 `Painter`）；不得出现第二份 chevron/cross/fill 实现。
6. 一个 `.cpp` 一个职责；`*_paint.cpp` / `*_actions.cpp` 是"同一职责的实现分片"，可共享同名 `.h`。
7. 新增功能必须新增 `features/<name>/`，禁止往 `app/app.cpp` 里加业务。

### 4.3 三个端口（`contract/ports.h`，方案 B 的核心）

```cpp
struct Navigator {                 // 谁能导航
    virtual void Go(const std::wstring& path) = 0;
    virtual void Back() = 0;
    virtual void Forward() = 0;
    virtual void Up() = 0;
    virtual void SetViewMode(int fvm, int iconPx) = 0;
};

struct PathSource {                // 当前路径/选中项从哪来
    virtual std::wstring CurrentPath() const = 0;
    virtual std::vector<std::wstring> SelectedPaths() const = 0;   // P2 复制路径用
};

struct CommandSink {               // 命令往哪发（P3 命令面板/AI 共用）
    virtual bool Invoke(int commandId) = 0;
    virtual std::vector<CommandInfo> Commands() const = 0;
};
```

书签栏只需要 `PathSource + Navigator`，搜索只需要 `Navigator + CommandSink`，互不认识彼此。

---

## 5. 方案对比（一句话版）

| 维度 | A 仅分层 | **B 分层+切片** | C 命令驱动 |
|---|---|---|---|
| 新增"书签栏"要改几处 | 3 个目录 | **1 个新目录 + 1 处注册** | 1 个 reducer + 1 个视图 |
| 3,000 行时的复杂度 | 低 | **中** | 高（框架成本） |
| 命令面板/AI 统一入口 | 做不到 | 阶段 3 用命令注册表做到 | 天生就有 |
| 迁移风险 | 低 | **中（分 3 批可控）** | 高（全量重写） |
| 适合的团队规模 | 1 人短周期 | **1–2 人长期** | 3 人以上/多人并行 |

---

## 6. 不做的事（写进方案，避免以后反复讨论）

- 不引 CMake/Make/vcpkg：仍用两条 clang 命令，只把文件清单挪到 `build.sh`/`build.bat` 的同一段。
- 不引依赖注入容器、事件总线、反射式注册：端口用手写虚接口，命令表用静态数组。
- 不引单元测试框架：`core/` 配一个 `tests/core_test.cpp`（自带 `assert`，不进主构建，`-o /tmp` 单独跑）。
- 不改窗口类名（`ExBarBrowser_*` 是脚本与自动化的外部契约）。
- 不改任何用户可见行为：迁移批次里"行为零变化"是硬要求，视觉改动另开批次。
- 不为了分层而分层：`status_bar` 就 90 行，不再往下拆。

---

## 7. 迁移批次（每批可编译、可运行、可回退）

| 批次 | 范围 | 产出 | 验证 | 回退 |
|---|---|---|---|---|
| **B0 冻结** | 停止并行写入；`src` 打 tag/备份 | — | `git status` 干净 | — |
| **B1 去重 + 拆 god file** | 拆 `common.h`（→ contract + platform/win_base + core/path_text）；`DrawChevron` 三份合一（ui/components + Painter）；日志两份合一（platform/log）；`ParentOf/LeafOf` 两份合一（core/path_text）；`nav_bar.cpp` 拆 paint；`app.cpp` 拆 layout/commands | 目录变为 A 的形态，但文件已按职责切 | 编译零警告；20 项冒烟（见 DEVELOPMENT §2.2）；深/浅色截图对比 | 整目录还原备份 |
| **B2 引入 features** | `App` 的标签与导航职责搬到 `features/tabs`、`features/navigation`；`app.cpp` 目标 <200 行 | B 的形态 | 同上 + 标签/历史/视图模式逐项回归 | 只回退 features 两个目录 |
| **B3 命令注册表** | `contract/commands.h` + `app/commands.cpp`；快捷键从 `ACCEL` 表改为查命令表 | 命令可枚举 | 现有 11 条命令全部可经注册表调用 | 保留旧 `ACCEL` 分支 |
| **B4 首个新功能按模板落地** | P2 的 `features/bookmarks`（或 `sessions`） | 验证"不改 app 也能加功能" | 书签可读可点；`app.cpp` 行数不增 | 删 feature 目录 |

工期估算（单人、含验证）：B1 约 1.5–2 天；B2 约 1 天；B3 约 0.5 天；B4 视功能而定。

---

## 8. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| 并行会话同时改 `src/` | 互相覆盖，改动丢失 | **B0 必须先冻结**；迁移期间只允许一个写入者 |
| 构建脚本两份清单不同步 | `build.sh` 过、`build.bat` 挂 | 迁移时两个脚本同批改；`DEVELOPMENT.md` §3.1 已列为检查项 |
| 文档目录树过期 | 新人/AI 按旧路径找文件 | B1 合并后同步改 README/ARCHITECTURE/DEVELOPMENT/CONTRIBUTING 的树与表 |
| 拆文件引入 include 环 | 编译失败或隐藏耦合 | 规则 2/3 用脚本校验（可写 20 行 Python 扫 include 图） |
| 行为被"顺手改掉" | 回归难查 | 每批只做搬迁与去重；任何行为改动单独提交、单独验证 |

---

## 9. 需要你拍板的 4 件事

| # | 决策 | 选项 | 我的建议 |
|---|---|---|---|
| 1 | 目标形态 | A 仅分层 / **B 分层+垂直切片** / C 命令驱动 | **B**（增长点是功能） |
| 2 | 首批范围 | **只做 B1（去重+拆文件，目录先到 A 形态）** / 一次做到 B / 只出规则不动文件 | **只做 B1**（风险最小，先止血） |
| 3 | 命名空间 | 不加（目录表达分层） / 加 `exb::core`、`exb::ui` 等 | **不加**（单 exe、3,000 行，命名空间只增加跳转成本；将来拆静态库再加） |
| 4 | 并行写入 | **先冻结，再迁移** / 边改边迁 | **先冻结**（否则必然打架） |

拍板后我按批次执行，每批结束给你：编译结果、冒烟清单、改动文件列表、以及"行为零变化"的证据
（截图对比 + 日志对比）。

---

## 10. 增补：调研 Directory Opus 后的修订（2026-11-19 20:45）

**已定事项（你已确认）**

- 首批范围：**只定规则，不动文件**。
- 并行写入：迁移前**先冻结**。
- 目标形态：先看 "Directory Opus 是怎么做的"。

**调研结论**（详见 [DOPUS-NOTES.md](DOPUS-NOTES.md)）：Opus 值得抄的是三条**契约**，不是目录形状——

1. **对象模型**：`DOpus → Lister（窗口）→ Tab（文件夹上下文）→ Item`，Tab 自带路径/双向历史/格式；
2. **命令是唯一动作入口**：工具栏、菜单、热键、脚本、外部 `dopusrt /cmd` 全走同一张内部命令表；
3. **扩展点按类型分窄接口**：Viewer / VFS / 元数据插件 + 脚本 add-in，每种只拿到一个接口。

它不值得我们抄的：插件 DLL ABI、脚本引擎、双栏、XML 工具栏（规模不匹配，见 DOPUS-NOTES §3）。

**因此目标形态修订为「Opus 契约模型」**（= 方案 B 的具体化，不新增第 4 个方案）：

```
Runtime → Lister → Tab → View        +  CommandRegistry（唯一动作入口）
                                      +  Feature（编译期扩展，经 Ports 访问对象）
```

**批次顺序随之调整**（命令层从原 B3 提前到 B2——因为 Opus 的核心就是命令，先有它，
后续 Tab 抽取与 Feature 化才有落点）：

| 批次 | 原计划 | 修订后 | 理由 |
|---|---|---|---|
| B1 | 去重 + 拆 god file | **范围缩小**：`nav_bar` 拆分已由并行会话在 20:35 完成（`nav_bar` / `nav_bar_draw` / `nav_metrics.h`）；B1 只剩 `common.h` 拆分、绘图/日志/路径三组去重、`app.cpp` 拆 layout/commands | 先止血，与对象模型无关 |
| B2 | 引入 features（Tab/导航抽取） | **命令层**：`contract/commands.h` + `app/commands.cpp`，`ACCEL` 改为查表 | 命令是产品 API，越早统一越好；且改动面小于抽 Tab |
| B3 | 命令注册表 | **Tab/History 抽取 + `App` → `Lister`** | 状态与窗口分离，为 Feature 提供稳定对象 |
| B4 | 首个新功能 | **组件化**：`ui/tokens` / `ui/assets` / `ui/components` 落地；现有三个栏的绘制迁入组件（HWND 暂不变） | "一切皆组件"是硬要求，组件层必须先于新功能 |
| B5 | — | **载体化**：`ui/containers`（Bar/Panel/Dialog/Popup/Lister）统一接口；chrome 合并进 Lister 客户区 | 窗口类 ≤3；消除 4 份 WndProc/命中/重绘 |
| B6 | — | **输入端**：`input/`（绑定表 + 序列键 + 鼠标映射 + 路由器） | 新增序列键只改数据；输入层零状态修改 |
| B7 | — | **人机同源**：`StateView` + `EventBus` + `ApprovalBroker` + `AgentBridge`（MCP 子集）+ 最小 UIA | AI 与人走同一管线、同一状态快照 |
| B8 | — | 首个新功能（书签或会话），验证"不改 app、不写绘制也能加功能" | 组件/载体/管线就绪后的真实验收 |

**迁移规格**：mod 侧七类自定义功能（标签栏/导航栏/书签栏/状态栏/键盘/序列/鼠标）的逐项清单与顺序见
[MIGRATION-PLAN.md](MIGRATION-PLAN.md)（M0–M7，约 20 人日）。

**规则已先行**：[ARCHITECTURE-RULES.md](ARCHITECTURE-RULES.md) 从即日起对**新增代码**生效，
存量按上表批次迁移；规则里 R1–R21 每条都有可机械校验的判据，B1 开工前先把
`tools/check_arch.py` 写出来，用它验收每一批。

**v3 增补（20:58）**：目标形态收敛为"一切皆组件 + 载体 + 输入端 + 命令管线"——
① 组件：`ui/tokens`（令牌）、`ui/assets`（图标/字体/背景/光标）、`ui/components`（原子/控件/复合）、
`ui/containers`（Bar/Panel/Dialog/Popup/Lister 载体，只布局不绘制）；② 输入端：`input/`
（键位/序列/鼠标/IME → 绑定表 → 命令）；③ 管线：`pipeline/`（校验/可用性/授权/执行/事件/审计）；
④ 人机同源：所有输入源都进同一条管线，AI 只是其中一个源。设计细节见
[ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)，对应规则 R11–R20。

