---
status: current
baseline: 2026-11-21 22:50
method: 与 Windows 11 资源管理器逐项对照；结论以本仓实测（真实命名管道 + 源码阅读）为准
---

# 与资源管理器的差距审计（2026-11-21）

> **回答一个问题**：跟 Windows 11 资源管理器比，还差哪些、哪些是错的、哪些不用补。
>
> 口径声明：
> - 命令数取自 `tools/check_commands.py` 实跑：**175 条 / 已实现 166 / 未实现 9**；`tools/check_ai_meta.py` 实跑 AI 暴露 **139 条**。
> - 行为结论取自真实命名管道实测（`agentctl.py call`），不是读文档推断。
> - 与 Explorer 的功能对照来自微软官方文档与 Win11 实际界面。
> - 分类沿用三分法：**错误**（内部矛盾 / 可复现缺陷）、**缺失**（能力空白）、**有意取舍**（设计选择）。

---

## 0. 先说结论

| 维度 | 状态 | 一句话 |
|---|---|---|
| 日常浏览 / 导航 / 标签 / 书签 | ✅ 可替代 | 不需要回 explorer 了 |
| 文件操作（复制剪切粘贴改名删除属性） | ✅ 可替代 | 走 shell，语义与系统一致，回收站/冲突/撤销交给系统 |
| 视图模式 / 排序 / 分组 | ✅ 基本可替代 | 8 档视图 + 4 档排序 + 4 档分组，由 `IFolderView2` 驱动 |
| 快捷键 | ✅ **硬冲突已修，Explorer 常用键位已补齐** | 见 §1 |
| 搜索 | ⚠️ **假能力已降级为未实现；搜索框仍缺明确失败提示** | 见 §2.1 |
| 面板体系 | ⚠️ 缺详情列自定义 / 缩放 / 查看二级菜单 | 见 §3 |
| 显示细节 | ✅ 主题令牌已统一；视图状态不同步仍在 | 见 §4 |

**最该做的三件事**（按影响排序，均已在 P1 完成）：

1. ✅ **修 `search.find`** —— 已从 `implemented=true` 降级为 `false`，处理器与 AI 暴露同步撤除；`docs/PARITY.md` 同步改口。
2. ✅ **修 `Ctrl+Shift+A` 双绑** —— `hotkeyTabSwitcher` 改到 `Ctrl+Shift+G`，`CopyToFolder` 保住 `Ctrl+Shift+A`。
3. ✅ **补齐 Explorer 常用键位** —— `Alt+Enter`、`Ctrl+E`、`Alt+D`、`F2`、`Ctrl+L`、`Ctrl+Shift+1..8`、`Shift+Delete`、`Ctrl+Shift+E` 全部落地；前六条是纯绑键，后两条已补真实命令与处理器。

---

## 1. 快捷键

### 1.1 错误（内部矛盾，可复现）

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| **K1** ✅ 已修 | **`Ctrl+Shift+A` 硬冲突** | 原 `data/keymap.json` 里 `hotkeyTabSwitcher` 与 `hotkeyCopyToFolder` 绑同一个键；P1 已把 `TabSwitcher` 改为 `Ctrl+Shift+G` | `InputRouter::BuildBindings` 的 `add()` 是**先到先得**（`src/input_router.cpp`），旧状态下 `hotkeyTabSwitcher` 排在前面 → 「复制到文件夹」键盘路径被吞；现已恢复 |
| **K2** | **`TabNext` / `TabPrev` 完全无键** | 这两条 `key` 是 `"-"`（`kHotkeyDisabled` 哨兵），而 `kSysKeys` 兜底表里登记了 `hotkeyTabNext→Ctrl+Tab`；`BuildBindings` 遇 `-` 直接 `continue`，**不回落** | 两个命令在快捷键管理器里显示为"未绑定"。同时 `Ctrl+Tab` 已被 `TabCycleNext`（MRU 循环）占用 —— 两组语义（顺序下一 vs MRU 循环）都存在但只有一组有键 |
| **K3** ✅ 已修 | **`Ctrl+9` 语义与 Explorer 相反** | 旧 `keymap.json` 把 `Ctrl+9` 绑给 `TabLast`；P1 新增 `hotkeyGoTab9→Ctrl+9`，`TabLast` 顺移 `Ctrl+0` | 现在 `Ctrl+1..9` 对应第 1..9 个标签，`Ctrl+0` 对应最后，与 Explorer 一致 |
| **K4** ✅ 已修 | **`kExtra` 里的键不可配置、且可被静默抢占** | `src/input_router.cpp` 的 `kExtra[]` 原硬编码 `F2→CmdRename`、`Ctrl+L→FocusAddress`、`Ctrl+Shift+1..8→ViewMode:0..7`；P2 已全部收编进 `data/keymap.json` 并删除 `kExtra` | 这些键现在可见、可配，并与其他键位走同一套先到先得逻辑；用户改键不再被第二套真相静默覆盖 |
| **K5** | **4 条命令真正无键** | `hotkeyFileHash` / `hotkeyFolderCompare` / `hotkeySessionSave` / `hotkeySessionRestore` 的 `key` 为空，且不在 `kSysKeys` 兜底表内 | 只能走命令面板 / 右键菜单，键盘不可达 |
| **K6** | **序列前缀延迟 3 秒** | `data/keymap.json` 里 `x l`（`CloseLeftOne`）是 `x l a`（`TabCloseLeft`）的前缀 | `sequence.cpp` 的处理是"非唯一命中就挂起等超时"，所以 **`x l` 要等 3 秒才生效**，而 `x l a` 是即时的。同为"关左侧"，两条路径延迟不一致 |

### 1.2 缺失（Explorer 有、命令已在表里、只差一个键）

这几条**命令表里已经实现了**，只是没绑键 —— 属于纯低成本补齐：

| Explorer 键位 | 动作 | 本仓现状 | 补法 / 状态 |
|---|---|---|---|
| `Alt+Enter` | 所选项目属性 | `CmdProps` 已实现 | ✅ P2 已补 `hotkeyProperties` |
| `Shift+Delete` | 彻底删除（不经回收站） | 原无此能力 | ✅ P2 尾新增 `CmdDeletePermanent`，`IFileOperation` 去掉 `FOF_ALLOWUNDO` |
| `Ctrl+E` | 聚焦搜索框 | 原仅 `Ctrl+F` | ✅ P2 已补 `hotkeyFocusSearchAlt` 别名 |
| `Alt+D` | 选中地址栏 | 原有 `F4` + `Ctrl+L` | ✅ P2 已补 `hotkeyFocusAddressAlt`；同时收编了 `Ctrl+L` |
| `Ctrl+Shift+E` | 展开导航窗格整棵树 | 原仅有导航窗格显隐 | ✅ P2 尾新增 `NavTreeExpandAll`，在原生 `SysTreeView32` 上逐节点 `TVM_EXPAND` |

### 1.3 缺失（Explorer 有、本仓确实没有）

| 类别 | 缺什么 | 备注 |
|---|---|---|
| 选择 | `Shift+方向键` 范围多选 | shell 视图原生应具备，但未在 `data/mouse.json` 登记；`Ctrl+点击` 多选、`Shift+右键` 新窗口同理 |
| 缩放 | `Ctrl+鼠标滚轮` 改图标大小 | `ShellView::SetViewMode(mode, iconPx)` **已支持改 iconPx**，但 `mouse.json` 无 wheel 规则 → 差一个入口 |
| 导航 | `Backspace` 上一级、`F6`/`Shift+F6` 焦点循环 | `F6` 在 Explorer 是窗口元素间循环焦点，本仓四栏没有对应 |
| 预览 | `Space` 快速查看（Peek） | 需 PowerToys 式 Peek 实现，成本较高 |
| 菜单 | `Shift+F10` 上下文菜单 | 书签栏/标签栏有，**文件列表区没有** |

### 1.4 本仓有、Explorer 没有（超集，不必改）

`Ctrl+M`/`Ctrl+Shift+M` 开终端（普通/管理员）、`Ctrl+K` 复制标签、`Ctrl+Shift+W` 关闭其他标签、`Ctrl+Shift+←/→` 关闭左/右标签、`Ctrl+Shift+P` 命令面板、`Alt+K` 快捷键管理器、`Ctrl+H`/`Ctrl+Shift+H`/`Ctrl+Alt+E` 隐藏文件/系统文件/扩展名、`Ctrl+Shift+L` 导航窗格开关、`Ctrl+I` 反选、`Ctrl+Shift+F`/`Ctrl+Alt+D`/`Ctrl+Shift+K` 选择筛选、`Ctrl+Shift+R` 批量重命名、`Ctrl+Shift+Y` 重复文件查找、`Alt+O` 文件夹选项、`Alt+Home` 主文件夹 —— 这些 Explorer 都没有或无默认键。

> `Ctrl+Shift+A`→`TabSwitcher` 本身也属超集，问题只在与 `CopyToFolder` 撞键。

---

## 2. 功能

### 2.1 错误（声明与实现不符 —— 最严重的一条）

#### **F1：`search.find` 是假能力，恒失败**

| 项 | 内容 |
|---|---|
| 声明 | `src/commands.cpp` 里 `CmdId::OpSearch` → `search.find`，**`implemented=true`**、`aiExposed=true`，`docs/PARITY.md` §3.12 也记为"已实现" |
| 实现 | `App::RunSearch`（`src/app_nav.cpp`）拼 `search-ms:` URL 后调 `t->view->Navigate(u)`；而 `ShellView::DoNavigate`（`src/shell_view.cpp`）**明确拒绝 `search-ms:` 前缀并 return false**（注释写明：`IExplorerBrowser` 没有搜索带，`BrowseToObject` 对它返回 `S_OK` 却把视图留在原地） |
| 实测 | `agentctl.py call search.find '{"query":"test"}'` → `ok=false`，`error="搜索视图没有接受这个查询：search-ms:query=test&crumb=location:..."` |
| 后果 | ①人类侧：导航栏搜索框提交后**必然失败**并 `MessageBeep` ②AI 侧：`search.find` 是 140 个暴露工具之一，调用方拿到的是**每次都失败的承诺** ③文档：`docs/PARITY.md` §3.12 的"已实现"与实现同源错误 |
| 修法 | 二选一：**A** 换宿主（用真 Explorer 的 `IExplorerBrowser` 搜索带）或接 `ISearchQueryHelper`，让搜索真能落地；**B** 把命令表改 `implemented=false`、撤掉搜索框、在 `docs/PARITY.md` 注明"宿主不支持 search-ms"。**不要维持现状** —— 现在是"宣称可用、实际全败"，比明确标未实现更坏 |

> 这条也说明 `docs/PARITY.md` §3.12 的自述不可尽信：它写"结果落在同一个 `IShellView` 里，所以排序 / 右键 / 拖放照旧可用"，但事实上根本没有结果。

#### **F2：`search.everything` 未实现** —— 这条**是对的**（`implemented=false`、不暴露），不算缺陷。但意味着 **AI 侧目前没有任何可用搜索能力**，文档应明示，避免调用方反复试探。

### 2.2 已核实的"真能力"（实测通过，不算缺口）

抽查实测全部通过（`ok=true` 且 `verdict=true`）：

| 命令 | 结果 |
|---|---|
| `view.mode {"preset":5}` | ✅ 断言 `viewMode=$arg:preset` 通过 |
| `view.sort {"by":2}` | ✅ 断言 `sortMode=$arg:by` 通过 |
| `tool.list` | ✅ 断言 `itemCount=$unchanged` 通过（新 `$changed`/`$unchanged` 断言机制可用） |
| `file.select.all` / `file.focus.top` / `file.focus.bottom` | ✅ |
| `nav.up` / `nav.refresh` | ✅ |

命令表里 8 条未实现命令（`tab.merge` / `path.copy.link` / `view.toggle.checkboxes` / `ext.run` / `ext.sendkeys` / `search.everything` / `win.ai.toggle` / `win.ai.stop`）**全部正确标了 `implemented=false`**，不是假能力 —— 这一点做得对。

### 2.3 缺失（相对 Explorer 的功能空白）

| 类别 | 缺失 | 性价比 | 备注 |
|---|---|---|---|
| 详情视图 | **详情列可自定义**（选列：名称/修改日期/类型/大小/创建日期/访问日期/属性/作者/路径/标签…） | **高** | `IColumnManager` 接口现成，路径是通的 |
| 视图 | **图标大小缩放**（Ctrl+滚轮 + 缩放滑块） | **高** | `SetViewMode` 已支持 iconPx，缺规则与滑块 |
| 视图 | 「排序方式」「分组依据」**二级菜单**（按任意列/任意属性） | 高 | `popup_list` 目前是平铺模型，要加一层子菜单 |
| 视图 | 按任意属性分组（现在只有 无/类型/日期/大小 四档） | 中 | `shellops::GetSort` 只认 4 个 `PROPERTYKEY`，用户按别的列排序后**视图菜单打勾会全部消失** |
| 导航窗格 | 主页/快速访问/OneDrive/此电脑/网络 的完整树、树右键菜单、「在文件夹中显示」 | 中 | `folder_tree.cpp` 有基础 |
| 标签栏 | **垂直标签**、拖出成新窗口、多标签拆分到新窗口、标签组 | 中 | 另有 `tab.pin`（固定标签）与 `tab.lock`（锁定标签）两条命令**已实现但无键**（只在右键菜单可达） |
| 状态栏 | 缩放滑块 | 中 | |
| 文件操作 | 压缩为 ZIP、发送到、还原旧版本、共享、离线可用 | 中 | 右键菜单走 shell 动词，需确认宿主是否提供 |
| 系统集成 | 已知文件夹/库/OneDrive/加密/备用数据流/挂载卷 | 低 | `Go` 组已有桌面/下载/文档等 10 条 |

### 2.4 有意取舍（**不算缺陷**）

| 取舍 | 理由 |
|---|---|
| `tab.merge`（与 explorer 标签合并） | 独立进程模型，与 explorer 不共享标签系统 |
| `ext.run` / `ext.sendkeys` | 安全边界；R21 输入纪律下无意义 |
| `view.toggle.checkboxes` | shell 视图未暴露该接口；预览窗格已改用 `IFolderView2` 的 folder flag，无需自绘 |
| `win.ai.toggle` / `win.ai.stop` | 缺窗口内对话 UI，接口层已就绪（ROADMAP P4） |

---

## 3. 面板体系

本仓外壳共 **4 条自绘栏** + 1 个系统 shell 视图区：

| 栏位 | 绘制方 | 显隐开关 | 备注 |
|---|---|---|---|
| 标签栏 | 自绘（`tab_strip*`） | — | 溢出 `»`、中键关闭、拖拽排序、双击空白新建 |
| 导航栏 | 自绘（`nav_bar*`） | — | 面包屑分段/折叠、地址编辑、搜索框、系统自动补全、方向历史 |
| 书签栏 | 自绘（`bookmark_bar*`） | `bmk.show`/`bmk.hide`/`bmk.toggle` | 分类下拉、拖入文件夹、双击空白切显隐 |
| 状态栏 | 自绘（`status_bar*`） | — | 9 段（路径/类型/眼睛/磁盘/容量/大小/日期…），**这是超出 Win11 Explorer 的能力** |
| 文件列表区 | **系统 `IExplorerBrowser`** | — | 视图/排序/分组由 `IFolderView2` 驱动；内含系统导航树与预览窗格 |

**相对 Win11 Explorer 面板体系缺的：**

1. 详情列自定义（`IColumnManager`）—— 工程里已有 `IColumnManager` 的调试代码，路径通
2. Ctrl+滚轮缩放 —— `InputRouter::OnWheel` 与 `MouseRules::TR_WHEEL_*` 骨架已在，只缺规则与图标尺寸映射
3. 「查看」二级菜单（排序方式/分组依据）—— `popup_list` 是平铺模型，要加子菜单层
4. 属性面板 / 库 / 共享 / 垂直标签 —— 都要新组件，工作量最大

---

## 4. 显示

### 4.1 错误

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| **D1** | **主题令牌有两个来源** | `src/ui_palette.h` 头注释写"色值令牌表（唯一来源）"，`AGENTS.md` §1.3 写"不写 `RGB(`"；但 `src/theme.cpp` 里 `bg`/`bar` 直接硬编码 `RGB(0x14,0x14,0x14)` / `RGB(32,32,32)` / `RGB(0xff,0xff,0xff)`，另有 `kTabColorsDark`/`kTabColorsLight` 两组标签色板 | 违反自己的唯一性纪律。且浅色 `bar` 取 `GetSysColor(COLOR_WINDOW)`（≈纯白），深色取 `RGB(32,32,32)`（#202020）—— **浅深两态不对称**；`theme.cpp` 注释里实测记录写的是"工具栏 #f3f3f3"，代码却取 `COLOR_WINDOW`，**注释与实现不一致** |
| **D2** | **视图档位与视图真实状态不同步** | `App::SetViewStateFn`（`src/app.cpp`）：`vs.sort`/`vs.ascending` 从视图现读（正确），但 `vs.group` 抄配置、`vs.mode` 抄 `viewMode_` | 用户在 shell 里点列头改排序 → 菜单打勾跟着变（好）；但**改分组或视图档位则不同步** → 菜单打勾在旧档位。同一个坑在 mode/group 上仍在 |
| **D3** | **状态栏窄窗丢弃顺序把"有动作"的段先丢掉** | `StatusBar::Layout` 的 `kDropOrder = {Seg_Type, Seg_Eye, Seg_Disk, Seg_Size, Seg_Date}`；而 `Seg_Date` 在 `App::SetClickFn` 里**是有动作的**（点它 → 按日期排序） | 窄窗下最有用的"点日期改排序"入口**最先消失**。丢弃顺序应按"有无点击动作"排，不该按枚举顺序倒着排 |
| **D4** | **`ShellView::ApplyNavPaneWanted` 隐藏后留空位** | 通过 `EnumChildWindows` 找 `NamespaceTreeControl` 后 `ShowWindow(SW_HIDE)`，但**shell 视图内容区不自动重排**（代码只 `InvalidateRect(container_)`） | "隐藏导航窗格"是**留空位**而不是真收起。与 `AGENTS.md` 的"导航窗格开关"承诺有落差 |
| **D5** | **`GetPaneState` 的 `EPS_FORCE` 与采样值自压** | `CapturePreviewPaneState` 采回来的 `previewPaneWanted_` 与 `EPS_FORCE` 压进去的值是同一个变量 | 采样值没有权威来源，真实状态可能被自己覆盖 |
| **D6** | **导航窗格/预览窗格状态时序竞争** | `CapturePreviewPaneBeforeNavigation` / `Sink::OnNavigationPending` / `ShellView::SetVisible` 三处各自采样 | 采样点不一致时最终值取决于哪条路径先到 |
| **D7** | **`ShowStatusMenu` 两处「静默吞掉」** | `src/app_commands.cpp`：`if (data < 0) return;` 在 `data==100/101` 判断**之后**又出现一次；段勾选逻辑把菜单 `data` 直接当段下标 | 依赖"菜单 action 号 0..8 与 100/101 不冲突"这一**隐式约定**，没有枚举保护。对比 `NavBar::SegCmd` 用枚举（头注释明写"用枚举而不是裸 int"）—— 同一工程内两种做法不一致 |
| **D8** | **`Theme::dark` 默认值 `true`** | `src/theme.h` 成员默认值 `dark = true`，只有 `Theme::Init` 里才 `dark = SystemIsDark()` | 任何在 `Init` 之前读 `theme_.dark` 的路径（如 `ApplyDarkTitleBar`）会拿到错的"深色" |
| **D9** | **`ViewSort` 方向落盘有洞** | `App::ViewSort` 成功后才写配置，方向从 `GetSort` **读回**；若 `GetSort` 返回 false（用户按了 4 个键之外的列），`s.sortAscending` **保留旧值** | 配置里的排序方向可能与视图实际方向相反 |

### 4.2 显示层缺失

| 缺失 | 说明 |
|---|---|
| 详情列自定义 | 见 §2.3 |
| 图标大小缩放 | 已有底层能力，缺入口 |
| 深浅色以外的模式（跟随系统的第三态） | 现在只有跟随系统 |
| 排序/分组的二级菜单 | `popup_list` 平铺模型限制 |

---

## 5. 文档与数字漂移（治理类）

| # | 问题 | 证据 |
|---|---|---|
| **G1** ✅ 已修 | **`docs/PARITY.md` 的总数全错** | 原记 "155 / 已实现 147 / 未实现 8" 且写死 "总数 140"；现改为动态口径，实测为 **175 / 166 / 9**（`search.find` 降级后 164/9，P2 再补两条命令后 166/9） |
| **G2** ✅ 已修 | **`docs/PARITY.md` 把假能力记成已实现** | §3.12 现记 `OpSearch` 已实现 0，并附真实命名管道失败证据（见 F1） |
| **G3** ✅ 已修 | **`docs/ROADMAP.md` 写"89 工具"** | 已不再写死工具数，改为"以 `tools/check_commands.py` 实测为准"；当前 AI 暴露实测 **139** |
| **G4** | **`docs/FULL-COVERAGE.md` §9 的"假缺陷"** | 记"主构建 `build_fast.py` 不清理已删源文件的 `obj/*.obj`（会重复符号）"；实测 `build_fast.py` 的 `link_all` 只链接当前 `SOURCES` 清单，残留 `.obj` **不参与链接** → 该缺陷不成立，浪费修复预算 |
| **G5** ✅ 部分已修 | **`docs/PARITY.md` §5 快捷键对照表已过期** | `sequences[]` 38 条仍正确；P1/P2 已消除 `Ctrl+Shift+A` 撞键，并把原先无键/隐藏键位补进 `keymap.json`；对照表本身仍需随新键位再校一轮 |

> 修法口径：这些文档要么改成"不写死数字"（AI-INTERFACE.md 已这么做了，是对的），要么用脚本从命令表生成。`AGENTS.md` §9 已经写明"所有规模数字都是快照且必须标口径；与代码不符时先信 `src/`" —— 现在这几处正是没照做。
>
> 附实测口径（2026-11-21 P2 完成后）：`data/keymap.json` 的 `hotkeys[]` **100 条**（其中 18 条 `key` 为空、靠 `kSysKeys` 兜底生效；2 条为 `-` 哨兵），`sequences[]` **38 条**。`src/keymap.gen.h` 由 `tools/gen_keymap.py` 重新生成，同为 100/38。命令侧：`check_commands.py` 实跑 **175 条 / 已实现 166 / 处理器 166 / keymap 引用 105 条**；`check_ai_meta.py` 实跑 **AI 暴露 139 条**。

---

## 6. 建议的落地顺序

### 第 1 批 —— 先修"宣称可用但不是"（✅ 已完成）

| 序 | 项 | 涉及文件 | 状态 |
|---|---|---|---|
| 1 | **F1 `search.find`**：改 `implemented=false` + 撤处理器/AI 暴露，`docs/PARITY.md` 同步 | `src/commands.cpp`、`src/app_commands_files.cpp`、`docs/PARITY.md` | ✅ |
| 2 | **K1 `Ctrl+Shift+A` 双绑**：`hotkeyTabSwitcher` 改 `Ctrl+Shift+G` | `data/keymap.json` + `src/keymap.gen.h` | ✅ |
| 3 | **K3 `Ctrl+9`**：改绑 `GoTab9`，`TabLast` 移到 `Ctrl+0` | `data/keymap.json` + `src/keymap.gen.h` | ✅ |
| 4 | **G1/G2/G3/G5 数字漂移** | `docs/PARITY.md`、`docs/ROADMAP.md`、本审计 | ✅ |
| 5 | **D1 主题令牌双源**：`theme.cpp` 的 `bg`/`bar`/标签色板并入 `UiPalette` | `src/theme.cpp`、`src/ui_palette.h` | ✅ |

### 第 2 批 —— 键位补齐（纯绑键部分 ✅ 已完成）

✅ 已补：`Alt+Enter`→`CmdProps`、`Ctrl+E`→`FocusSearch`、`Alt+D`/`Ctrl+L`→`FocusAddress`、`F2`→`CmdRename`、`Ctrl+Shift+1..8`→`ViewMode:0..7`。

✅ 已把 **`kExtra` 收进 `data/keymap.json`**（K4）：`src/input_router.cpp` 不再有第二套硬编码键位；100 条热键 / 38 条序列均由生成头兜底，且 `keymap_test` 与三方一致性检查同步更新。

✅ 剩余两项也已补完（都不是纯绑键）：

- `Shift+Delete` → `CmdDeletePermanent`：`shell_ops::DeleteSelectionPermanently()` 复用 `IFileOperation`，仅去掉 `FOF_ALLOWUNDO`，保留 `FOF_WANTNUKEWARNING`；`aiExposed=false`，不进撤销栈，并显式登记为“人用、无目标参数”的设计债务。
- `Ctrl+Shift+E` → `NavTreeExpandAll`：在 `NamespaceTreeControl` 内找到原生 `SysTreeView32`，递归 `TVM_EXPAND`（上限 10000 节点护栏）；找不到树时如实返回失败，不假报成功。

### 第 3 批 —— 补齐"替代 Explorer"的真实缺口

1. 详情列自定义（`IColumnManager`）
2. Ctrl+滚轮缩放 + 缩放滑块
3. 「排序方式 / 分组依据」二级菜单（`popup_list` 加子菜单层）
4. 排序/分组维度放开到任意属性（现在硬限死 4 个 `PROPERTYKEY`）
5. 修复 D2/D3/D4/D7/D9 这几个显示状态不同步与丢弃顺序问题

---

## 7. 本轮未覆盖（留待下一轮）

- 拖放、排序分组、缩略图、右键菜单的**深度行为**对比（这些走 shell，本仓基本白送，优先级低）
- 缩略图/超大图标的实际渲染质量
- 与 Explorer **并排使用**时的系统级冲突（全局钩子、DDE、文件关联抢注）
- 多显示器 / 混合 DPI 下的布局
- 无障碍（UIA）树完整度 —— `src/tab_uia.cpp` 已有部分
