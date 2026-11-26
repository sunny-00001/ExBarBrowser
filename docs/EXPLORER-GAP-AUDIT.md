---
status: current
baseline: 2026-11-21 22:50
method: 与 Windows 11 资源管理器逐项对照；结论以本仓实测（真实命名管道 + 源码阅读）为准
---

# 与资源管理器的差距审计（2026-11-21）

> **回答一个问题**：跟 Windows 11 资源管理器比，还差哪些、哪些是错的、哪些不用补。
>
> 口径声明：
> - 命令数取自 `tools/check_commands.py` 实跑：**241 条 / 已实现 234 / 未实现 7**；`tools/check_ai_meta.py` 实跑 AI 暴露 **194 条**。
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
| 面板体系 | ✅ 详情列自定义、缩放入口已补；查看二级菜单待补 | 见 §3 |
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
| 选择 | `Shift+方向键` 范围多选 | **输入路径审计（2026-11-26）**：规则不命中时点击/按键全部直通 shell（`input_router.cpp` 返回 false → `DefSubclassProc`），Ctrl+点击多选、Shift+点击、Shift+方向键走 shell 原生行为，**没有规则拦截**；有意往 `mouse.json` 加规则反而会吃掉原生行为。待人工实测勾销 |
| 缩放 | `Ctrl+鼠标滚轮` 改图标大小 | ✅ 已补：`view.zoom` / `view.zoom.set` + `mouse.json` 的 `wheel_up`/`wheel_down`（`mods=1` 即 Ctrl）+ 视图菜单「图标大小…」 |
| 导航 | `Backspace` 上一级、`F6`/`Shift+F6` 焦点循环 | ✅ 已补：`Backspace`→`NavUp`（keymap 既有）；`win.focus.next`（F6）/ `win.focus.prev`（Shift+F6）2026-11-26 落地，`view.pane.next` 迁 Alt+F6 让出 F6 |
| 预览 | `Space` 快速查看（Peek） | 需 PowerToys 式 Peek 实现，成本较高；与 P3-5 预览窗格合并评估 |
| 菜单 | `Shift+F10` 上下文菜单 | ✅ 已补：`file.context.menu`（Shift+F10），走 shell 背景 `IContextMenu` |

### 1.4 本仓有、Explorer 没有（超集，不必改）

`Ctrl+M`/`Ctrl+Shift+M` 开终端（普通/管理员）、`Ctrl+K` 复制标签、`Ctrl+Shift+W` 关闭其他标签、`Ctrl+Shift+←/→` 关闭左/右标签、`Ctrl+Shift+P` 命令面板、`Alt+K` 快捷键管理器、`Ctrl+H`/`Ctrl+Shift+H`/`Ctrl+Alt+E` 隐藏文件/系统文件/扩展名、`Ctrl+Shift+L` 导航窗格开关、`Ctrl+I` 反选、`Ctrl+Shift+F`/`Ctrl+Alt+D`/`Ctrl+Shift+K` 选择筛选、`Ctrl+Shift+R` 批量重命名、`Ctrl+Shift+Y` 重复文件查找、`Alt+O` 文件夹选项、`Alt+Home` 主文件夹 —— 这些 Explorer 都没有或无默认键。

> `Ctrl+Shift+A`→`TabSwitcher` 本身也属超集，问题只在与 `CopyToFolder` 撞键。

---

## 2. 功能

### 2.1 错误（声明与实现不符 —— 最严重的一条）

#### **F1：`search.find` 是假能力，恒失败** —— ✅ 已修（2026-11-22）

| 项 | 内容 |
|---|---|
| 原声明 | `src/commands_table.cpp` 里 `CmdId::OpSearch` → `search.find`，**`implemented=true`**、`aiExposed=true`，`docs/PARITY.md` §3.12 也记为"已实现" |
| 原实现 | `App::RunSearch`（`src/app_nav.cpp`）拼 `search-ms:` URL 后调 `t->view->Navigate(u)`；而 `ShellView::DoNavigate`（`src/shell_view.cpp`）**明确拒绝 `search-ms:` 前缀并 return false**（注释写明：`IExplorerBrowser` 没有搜索带，`BrowseToObject` 对它返回 `S_OK` 却把视图留在原地） |
| 原实测 | `agentctl.py call search.find '{"query":"test"}'` → `ok=false`，`error="搜索视图没有接受这个查询：search-ms:query=test&crumb=location:..."` |
| **修法** | **走原生搜索文件夹**：`ISearchFolderItemFactory`（`CLSID_SearchFolderItemFactory`）造 PIDL → `ShellView::NavigatePid` 以 **PIDL** 形式导航。`DoNavigate` 拒绝的是**字符串前缀**，不是文件夹类型 —— 同一个文件夹以 PIDL 交出去，shell 能绑定，导航过去就是**真的搜索结果视图**（排序 / 右键 / 拖放照旧可用）。同一套做法见 `D:\Windhawk\exbar.cpp:26215 RunSystemSearchFolder`。实现见 `src/search_folder.{h,cpp}`、`src/shell_view_search.cpp` |
| 现实测 | `search.find {"query":"a"}` → `ok=true`，断言 `searchQuery=$arg:query.trim` 通过；日志 `[nav] browse(pidl) hr=0x00000000` → `[nav] complete pidl=搜索: a · E:\ExBarBrowser（含子目录）`；结果条目数按范围变化（仅本层 19 条 / 含子目录 1371 条） |
| 结果视图 | 默认**详细信息**（`IFolderView2::SetViewModeAndIconSize(FVM_DETAILS, 16)`），日志 `[search] 详情视图 hr=0x00000000 now=4 icon=16`。原生搜索文件夹的视图是**异步**创建的，所以挂 120ms 定时器等新视图就位再补设（`ShellView::OnSearchViewTimer`，最多 ~3 秒） |

> 这条也说明 `docs/PARITY.md` §3.12 的自述不可尽信：它写"结果落在同一个 `IShellView` 里，所以排序 / 右键 / 拖放照旧可用"，但事实上根本没有结果。

##### F1 遗留：「此电脑」这一档在本宿主做不到（不是假能力，是明确报错）

原生搜索文件夹的**作用域只能是一个普通文件系统目录**（全部实测，2026-11-22，详见 `src/search_folder.h` 头注释）：

| 作用域 | 结果 |
|---|---|
| `C:\Windows\`、`C:\Program Files\`、`C:\ProgramData\`、`C:\Users\X\Documents\`、`E:\某目录\` | ✅ 能绑、能出结果 |
| `C:\Users\X`（**不带尾斜杠**） | ❌ 被 `SHParseDisplayName` 规范化成 CSIDL_PROFILE 那个命名空间项 → 绑不上（所以作用域一律补尾斜杠） |
| `C:\`、`D:\`（卷根） | ❌ 绑上了但查不到东西（卷根不是 crawl scope） |
| `::{20D04FE0-3AEA-1069-A2D8-08002B30309D}`（此电脑命名空间） | ❌ 绑不上 |
| 多个目录组成的 `IShellItemArray` | ❌ 绑不上（哪怕每个单独都能用） |
| **不给作用域** | ❌ 绑不上 —— 整机范围正是靠它 |

「整台电脑」要的是 **Explorer 进程内**的 `search-ms:` 全局查询宿主；本宿主是自托管的 `IExplorerBrowser`，没有那个宿主。所以这一档的处理是**明确报错**（`search.scope.set` 直接拒绝、范围菜单里那一行摆成灰字"（本宿主暂不可用）"），**不悄悄退回别的档** —— 退回的话用户以为搜了整台电脑，实际只搜了当前目录。

真正的整机搜索有两条路，都还没做：①接 Windows Search 的**查询 API**（`ISearchQueryHelper`，`searchapi.h` 本编译器没有）；②自己做**混合虚拟文件夹**（像 Everything 的结果文件夹那样，把多个作用域的结果并到一个视图里）。


#### **F2：`search.everything` 未实现** —— 这条**是对的**（`implemented=false`、不暴露），不算缺陷。F1 修好之后 **AI 侧已有可用搜索能力**（`search.find` 走系统索引）；缺的只是"能搜未编入索引的位置"（Everything 那套），文档应明示，避免调用方反复试探。

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
| 详情视图 | **详情列可自定义**（选列：名称/修改日期/类型/大小/创建日期/访问日期/属性/作者/路径/标签…） | ✅ 已补 | 视图菜单 →「选择详细信息…」；`shell_columns` 封装 `IColumnManager`，列 id 稳定为 `{GUID}:pid` |
| 视图 | **图标大小缩放**（Ctrl+滚轮 + 缩放滑块） | ✅ 滚轮已补 | `view.zoom` + mouse.json `wheel_up`/`wheel_down`（mods=1）；状态栏缩放滑块未做，改用视图菜单「图标大小…」 |
| 视图 | 「排序方式」「分组依据」**二级菜单** | ✅ 已补 | 父层小结 + 子层四档 + 方向翻转；下钻式不侵入 `popup_list` 模态循环 |
| 视图 | 按任意属性排序/分组（原只有 无/类型/日期/大小 四档） | ✅ 已补 | `view.sort.bykey` / `view.group.bykey` 收 `{GUID}:pid`；`view.props.list` 列可用属性；快照回读 `sortKey`/`groupKey` |
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

1. ✅ 详情列自定义（`IColumnManager`）—— 已由 `shell_columns` + 视图菜单「选择详细信息…」补齐
2. ✅ 图标缩放 —— `view.zoom`/`view.zoom.set` + `mouse.json` 的 `wheel_up`/`wheel_down`（mods=1）；视图菜单补「图标大小…」离散档位
3. 「查看」二级菜单（排序方式/分组依据）—— `popup_list` 是平铺模型，要加子菜单层
4. 属性面板 / 库 / 共享 / 垂直标签 —— 都要新组件，工作量最大

---

## 4. 显示

### 4.1 错误

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| **D1** ✅ 已修 | **主题令牌有两个来源** | 原 `src/theme.cpp` 里 `bg`/`bar` 直接硬编码，并有 `kTabColorsDark`/`kTabColorsLight` 两组标签色板；P1 已并入 `src/ui_palette.h`，`theme.cpp` 只从 `UiPalette` / `TabColorsFor()` 取值 | 模板用色不再两处各写一份 |
| **D2** ✅ 已修 | **视图档位与视图真实状态不同步** | 原 `App::SetViewStateFn` 的 `vs.group` 抄配置、`vs.mode` 抄 `viewMode_`。现改为**四项全从 shell 现读**：排序/方向走 `GetSort`/`GetSortKey`，分组走 `GetGroupKey`（内置四档映射下标、任意键留 -1 由小结字符串表达），档位走 `GetViewMode` + `viewmode::ModeIndexFor(fvm, iconPx)`（`FVM_THUMBNAIL` 一档三用，按像素最接近匹配） | 菜单打勾从此回答"现在是什么"，不再说谎；`tests/view_modes_test.cpp` 覆盖反查 |
| **D3** 复核后不成立 | **状态栏窄窗丢弃顺序** | 审计原记"`Seg_Date` 有动作却最先消失"，但现行 `kDropOrder = {Seg_Type, Seg_Eye, Seg_Disk, Seg_Size, Seg_Date}` 是**第一个丢 Seg_Type**（唯一无点击动作的段），`Seg_Date` 最后才丢 —— 已符合"无动作的先丢" | 原描述与实际代码顺序相反，无需改动；保留记录避免下次重复排查 |
| **D4** ⚠️ 已缓解 | **`ShellView::ApplyNavPaneWanted` 隐藏后留空位** | 原只 `InvalidateRect`（只重画不重排）。现隐藏/显示后用当前 client rect 再调一次 `browser_->SetRect(nullptr, rc)`，迫使 IExplorerBrowser 重排框架子窗口；尺寸不变故不闪烁 | 比单纯重画更接近正解；是否完全收起空位需用户界面复核（shell 框架内部布局不透明） |
| **D5** | **`GetPaneState` 的 `EPS_FORCE` 与采样值自压** | `CapturePreviewPaneState` 采回来的 `previewPaneWanted_` 与 `EPS_FORCE` 压进去的值是同一个变量 | 采样值没有权威来源，真实状态可能被自己覆盖 |
| **D6** | **导航窗格/预览窗格状态时序竞争** | `CapturePreviewPaneBeforeNavigation` / `Sink::OnNavigationPending` / `ShellView::SetVisible` 三处各自采样 | 采样点不一致时最终值取决于哪条路径先到 |
| **D7** ✅ 已修 | **`ShowStatusMenu` 两处「静默吞掉」** | 删掉冗余的第二次 `if (data<0) return`，把 100/101 提成 `enum StatusAction { kCopyPath, kHideStatusBar }`，越界 `data >= Seg_Count` 一律不当段处理 | 动作号与段下标的分离从隐式约定变成显式枚举，与 `NavBar::SegCmd` 同一做法 |
| **D8** | **`Theme::dark` 默认值 `true`** | `src/theme.h` 成员默认值 `dark = true`，只有 `Theme::Init` 里才 `dark = SystemIsDark()` | 任何在 `Init` 之前读 `theme_.dark` 的路径（如 `ApplyDarkTitleBar`）会拿到错的"深色" |
| **D9** ✅ 已修 | **`ViewSort` 方向落盘有洞** | 原成功后用 `GetSort` 读回方向，任意列时它返回 false → 方向不落盘。现改用 `GetSortKey`（任意列都成立）读方向 | 配置方向不再与视图实际方向相反 |

### 4.2 显示层缺失

| 缺失 | 说明 |
|---|---|
| 详情列自定义 | ✅ 已补：`shell_columns` + `view.columns.list/set/choose` + 视图菜单入口 |
| 图标大小缩放 | ✅ 已补：`view.zoom`/`view.zoom.set`/`view.zoom.choose` + `mouse.json` 滚轮规则 + 视图菜单「图标大小…」 |
| 深浅色以外的模式（跟随系统的第三态） | 现在只有跟随系统 |
| 排序/分组的二级菜单 | `popup_list` 平铺模型限制 |

---

## 5. 文档与数字漂移（治理类）

| # | 问题 | 证据 |
|---|---|---|
| **G1** ✅ 已修 | **`docs/PARITY.md` 的总数全错** | 原记 "155 / 已实现 147 / 未实现 8" 且写死 "总数 140"；现改为动态口径，实测为 **184 / 175 / 9**（`search.find` 降级 164/9 → P2 两条命令 166/9 → 并发作业域 169/9 → P3 详情列三条 172/9 → P3 缩放三条 175/9） |
| **G2** ✅ 已修 | **`docs/PARITY.md` 把假能力记成已实现** | §3.12 现记 `OpSearch` 已实现 0，并附真实命名管道失败证据（见 F1） |
| **G3** ✅ 已修 | **`docs/ROADMAP.md` 写"89 工具"** | 已不再写死工具数，改为"以 `tools/check_commands.py` / `tools/check_ai_meta.py` 实测为准"；当前 AI 暴露数见本文开头口径 |
| **G4** | **`docs/FULL-COVERAGE.md` §9 的"假缺陷"** | 记"主构建 `build_fast.py` 不清理已删源文件的 `obj/*.obj`（会重复符号）"；实测 `build_fast.py` 的 `link_all` 只链接当前 `SOURCES` 清单，残留 `.obj` **不参与链接** → 该缺陷不成立，浪费修复预算 |
| **G5** ✅ 部分已修 | **`docs/PARITY.md` §5 快捷键对照表已过期** | `sequences[]` 38 条仍正确；P1/P2 已消除 `Ctrl+Shift+A` 撞键，并把原先无键/隐藏键位补进 `keymap.json`；对照表本身仍需随新键位再校一轮 |

> 修法口径：这些文档要么改成"不写死数字"（AI-INTERFACE.md 已这么做了，是对的），要么用脚本从命令表生成。`AGENTS.md` §9 已经写明"所有规模数字都是快照且必须标口径；与代码不符时先信 `src/`" —— 现在这几处正是没照做。
>
> 附实测口径（2026-11-22 P3 缩放完成后）：`data/keymap.json` 的 `hotkeys[]` **100 条**（其中 18 条 `key` 为空、靠 `kSysKeys` 兜底生效；2 条为 `-` 哨兵），`sequences[]` **38 条**。`src/keymap.gen.h` 同为 100/38。命令侧：`check_commands.py` 实跑 **184 条 / 已实现 175 / 处理器 175 / keymap 引用 105 条**；`check_ai_meta.py` 实跑 **AI 暴露 143 条**。

---

## 6. 建议的落地顺序

### 第 1 批 —— 先修"宣称可用但不是"（✅ 已完成）

| 序 | 项 | 涉及文件 | 状态 |
|---|---|---|---|
| 1 | **F1 `search.find`**：~~改 `implemented=false` + 撤处理器/AI 暴露~~ → **改走原生搜索文件夹**（`ISearchFolderItemFactory` 造 PIDL + PIDL 形式导航），搜索真能执行；结果视图默认详细信息。遗留：「此电脑」档在本宿主做不到（作用域只能是一个普通目录），如实报错 | `src/search_folder.{h,cpp}`、`src/shell_view_search.cpp`、`src/app_nav.cpp`、`src/commands_table.cpp`、`docs/PARITY.md` | ✅ |
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

1. ✅ 详情列自定义（`IColumnManager`）—— `shell_columns` + 三条命令 + 视图菜单入口；`tests/shell_columns_test.cpp` 覆盖列 id 往返
2. ✅ `Ctrl+滚轮` 缩放 + 视图菜单离散档位（`view.zoom` / `view.zoom.set` / `view.zoom.choose`；`tests/view_modes_test.cpp` 钳制边界）
3. ✅ 「排序方式 / 分组依据」二级菜单 —— 父层两项带 chevron 显示当前值与方向，子层列四档并可直接翻转方向；用下钻式而非横向 flyout（`popup_list` 是 `SetCapture` 单模态循环，真 flyout 要重构捕获模型，波及所有下拉）
4. ✅ 排序/分组维度放开到任意属性 —— 新增 `view.sort.bykey` / `view.group.bykey`（收 `{GUID}:pid`，与 `view.columns.list` 同格式）+ `view.props.list` 给机器一份可抄的属性表；底层 `SetSortByKey`/`SetGroupByKey` 复用 IFolderView2，state 快照加 `sortKey`/`groupKey` 回读供自断言。旧 0..3 档位命令保留为快捷方式，不制造双真相
5. ✅ 显示状态同步 —— D2（档位/排序/分组/方向全从 shell 现读 + `ModeIndexFor` 反查）、D7（状态菜单动作号改枚举）、D9（方向读回走 `GetSortKey`）；D3 复核后不成立（现行丢弃顺序已把无动作的段先丢）；D4 缓解（`SetRect` 迫使重排，是否完全收起需 UI 复核）。D5/D6/D8 未列本批（需真实 UI 采样点验证，属另一轮）

---

## 7. 本轮未覆盖（留待下一轮）

- 拖放、排序分组、缩略图、右键菜单的**深度行为**对比（这些走 shell，本仓基本白送，优先级低）
- 缩略图/超大图标的实际渲染质量
- 与 Explorer **并排使用**时的系统级冲突（全局钩子、DDE、文件关联抢注）
- 多显示器 / 混合 DPI 下的布局
- 无障碍（UIA）树完整度 —— `src/tab_uia.cpp` 已有部分
