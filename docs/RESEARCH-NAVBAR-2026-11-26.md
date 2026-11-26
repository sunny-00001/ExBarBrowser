---
status: current
baseline: 2026-11-26
method: 本仓导航栏全量读码（nav_bar.h、nav_metrics.h、nav_bar_{input,actions,search,draw,layout,hit}.cpp 逐行核实）+ 15 个对标产品的公开文档/源码交互范式沉淀（Microsoft Learn、Chromium Omnibox Design Document、Mozilla 支持、Directory Opus 手册、Total Commander wiki、XYplorer 帮助、GitHub 各开源仓库）
去重: 与 docs/RESEARCH-SEARCHBOX-2026-11-26.md（地址框/搜索框专项，17 条）互补——本文把口径扩到**整条导航栏**（导航按钮 + 面包屑 + 地址框 + 搜索框），该文 17 条全部并入本清单（编号 1–17 对应重排），交集处不重复论证；与 docs/RESEARCH-OPENSOURCE-2026-11-26.md（开源实现架构）、docs/COMPETITIVE-GAP-2026-11-21.md、docs/BOOKMARK-BAR-AUDIT-2026-11-26.md（书签栏另审）不重叠
范围: 只覆盖导航栏（src/nav_bar*.cpp、nav_metrics.h、nav_path.h 及其 App 侧回调 app_nav.cpp / app_search.cpp / app_commands_search.cpp）；标签栏、书签栏、状态栏不在此文
---

# 导航栏对标调研报告（2026-11-26）

> 本文回答：**15 个对标产品在整条导航栏上的形态与交互**，哪些可以直接落到 ExBarBrowser。
>
> 口径：每条结论先给本仓**已核对的现状**（文件:行号，全部于 2026-11-26 逐条读码核实），
> 再给参考产品的做法与优先级。清单 27 条 = P1×8 + P2×13 + P3×6。

---

## 0. 本仓导航栏现状坐标（读码核实）

导航栏 = **5 颗导航按钮 + 地址框（面包屑/编辑双态）+ 搜索框（常驻）**：

- **导航按钮**：后退 / 前进 / 向上 / 刷新 / 视图，后退前进带分裂箭头（点箭头开历史下拉）。
  `nav_bar.h:156`（BtnId）、`nav_bar_actions.cpp:266-291`（历史菜单）、`nav_bar_actions.cpp:293-368`
  （地址框 ⌄ 的"后退+前进+最近"合并下拉）。
- **地址框**：面包屑显示态（前导图标 + 折叠钮「‹」+ 每级 20px 段/chevron 区）与编辑态（EDIT
  全选完整路径）双态切换。`nav_bar_actions.cpp:30-55`（BeginEdit/EndEdit）、
  `nav_bar_layout.cpp:79-119`（段矩形与折叠裁剪）、`nav_bar_draw.cpp:52-115`（绘制）。
- **搜索框**：常驻右侧 200px@96，左带=范围菜单、中条=输入、右带=历史下拉，窄窗整体收起。
  `nav_metrics.h:30-34`、`nav_bar_search.cpp:67-105`（LayoutSearch）、`nav_bar_search.cpp:159-176`
  （弹层锚点）、`nav_bar_layout.cpp:57-66`（宽度让位判定）。
- **键位**：地址框 F4 / Alt+D / Ctrl+L；搜索框 Ctrl+F / Ctrl+E；F6 循环两站（keymap.gen.h:92-93、114-116）。

**已达标、不建议改**（与 §16 盘点表一致）：面包屑双态语义、提交后焦点回列表、地址框 ⌄
合并下拉当前行高亮、范围单源（searchscope::Resolve）、搜索与命令同走 App::RunSearch 唯一实现、
弹层与输入框同宽锚点、破坏性动作（清历史）带确认、后退/前进灰显由 App 状态驱动（nav_bar.h:116-128
的"亮着但点不动"已修）。

---

## 1. Windows 资源管理器（Win10 / Win11）

### 小结

Win11 把地址栏与搜索框视觉整合（地址栏输入即触发搜索建议），Win10/Win11 键盘语义一致：

- **面包屑 ↔ 编辑双态**：点击面包屑空白处进入编辑态并**全选当前完整路径**；Esc 恢复面包屑
  （文本还原），Enter 提交导航；失败给明确报错而非静默留在原地。
- **自动完成**：SHAutoComplete（文件系统目录优先），下拉式候选**可见可选**，不是盲替换。
- **搜索框**：输入即搜（每敲一个字符更新结果），Esc 一次清词回原目录并把焦点还给文件列表；
  占位文案提示当前搜索位置（"在 xxx 中搜索"）。
- **拖放**：文件夹拖到地址栏 = 导航到该文件夹（系统级惯例）。
- **每级 chevron 下拉**：点面包屑分隔符列出该级子文件夹——本仓已实现且口径一致
  （nav_bar_actions.cpp:128-196）。

### 出处

Microsoft Learn（SHAutoComplete / EM_SETCUEBANNER / ISearchFolderItemFactory / crawl scope）、
Microsoft Support（Explorer 键盘与搜索行为）、docs/EXPLORER-GAP-AUDIT.md（F1 = search-ms: 被宿主拒绝）。

### 与本仓的关系

双态、常驻搜索框、cue 占位、chevron 下拉四件事本仓已达标。Explorer 真正多出来的是：
**可见的自动完成候选列表**（清单 1）、**失败反馈**（清单 5）、**拖放导航**（清单 8）。

---

## 2. Google Chrome（Omnibox）

### 小结

统一输入框范式的标杆。对本仓可借鉴的不是"搜网页"，而是**输入过程的反馈机制**：

- **输入即建议**：每个按键刷新与输入框同宽的下拉建议列表，↑↓ 选择、Enter 执行选中项、
  Shift+Delete 删除单条历史。
- **Paste and Go**：右键菜单对"剪贴板是 URL/路径"状态给专用项，粘贴+提交一步完成。
- **修饰回车**：Alt+Enter = 新标签打开当前输入。
- **失败不丢输入**：无效提交后输入框**保留原文**并提示，可改一字重试。
- **Esc 三段语义**：清下拉 → 还原原文 → 退出编辑，逐级回退。

### 出处

Chromium Omnibox Design Document（chromium.org → Design Documents → Omnibox）。

### 与本仓的关系

本仓地址框只有系统 SHAutoComplete + 纯 Tab 盲替换（nav_bar_input.cpp:22-28、
nav_bar_actions.cpp:84-126），无可视建议下拉；提交失败丢输入（nav_bar_actions.cpp:78-81 先
EndEdit 再导航）。对应清单 1、5、10。

---

## 3. Mozilla Firefox（Awesome Bar）

### 小结

- 建议列表 ↑↓ 移动、Enter 执行、**Shift+Delete 删除单条历史**——历史管理的键盘通道。
- 历史单条删除与整列清空分开：误存一条不必清全部。
- 无效提交保留原文；Esc 逐级回退语义与 Chrome 一致。
- 地址栏 ↑（空框时）直接展开历史下拉。

### 出处

Mozilla 支持：Address bar autocomplete in Firefox；firefox 源码 urlbar 模块注释。

### 与本仓的关系

搜索历史现在只有整列清空（app_commands_search.cpp:105-113）与鼠标右带路径
（nav_bar_search.cpp:170-176），键盘 ↑ 无行为（SearchProc 只处理 Enter/Esc/Tab，
nav_bar_search.cpp:25-49）。对应清单 6、15。

---

## 4. Directory Opus

### 小结

- **Location Field**：路径历史下拉（可导航的最近位置）+ 拖放接受 + 过滤条（Filter Bar）
  独立于搜索。
- **Filter Bar**：输入即筛当前列表，激活时**状态栏与过滤条上都显示"过滤中"**——过滤状态
  永远可见，不存在"列表被筛过但界面上看不出"。
- **工具栏元素显隐可配置**：每个栏元素都能在设置里关掉。
- 面包屑/路径段右键 = 段级菜单（复制路径、新标签打开等），作用于右键的那一段。

### 出处

Directory Opus 官方手册（gpsoft.com.au → Manual）：Location Field、Filter Bar 章节。

### 与本仓的关系

段级菜单本仓已有（nav_bar_actions.cpp:198-241，语义一致）；"过滤状态可见"对应清单 14
（范围角标）、"元素显隐"对应清单 24（隐藏搜索框）。

---

## 5. Total Commander

### 小结

- **路径行 ↓ 展开历史**：路径历史是键盘可达的（↓ 直接展开、↑↓ 选择、Enter 导航）。
- **快速搜索（本层）与文件搜索（跨目录）双入口**，语义分开、不合并。
- 中键点路径 = 新标签打开。
- 修饰键开新标签/新窗口的双轨语义贯穿所有入口。

### 出处

Total Commander 官方论坛与 ghisler wiki（路径行历史、quick search 参数）。

### 与本仓的关系

历史键盘通道（清单 6）、中键新标签（清单 22）、过滤/搜索双工具口径（清单 4 的语义边界）。

---

## 6. XYplorer

### 小结

- **Live Filter**：输入即筛当前列表，Esc 逐级还原；与 Find Files（跨目录）是两个工具。
- 地址栏支持拖放导航。
- 树/路径的**键盘可达性极高**，几乎每个下拉都有键盘通道。

### 出处

XYplorer 帮助文件（Live Filter / Find Files 章节）。

### 与本仓的关系

清单 4（即输即筛）的语义模板：过滤是本层瞬时操作，不进搜索历史、不合并进跨目录搜索。

---

## 7. Explorer++（C++/Win32，开源）

### 小结

- 地址栏控件/视图两层分离；`AddressBar::OnKeyPressed` 处理 Enter 提交、**Escape 还原原文**。
- 支持**环境变量展开**（%TEMP% 等直接可用）与路径自动补全。
- 补全候选可视化下拉。

### 出处

GitHub derceg/explorerplusplus（AddressBar.cpp / AddressBarView）；另见
docs/RESEARCH-OPENSOURCE-2026-11-26.md §1。

### 与本仓的关系

本仓 nav::FromUserText 明确不展开环境变量且无调用方补做（nav_path.h:58-74）——输入 %TEMP%
回车直接失败。对应清单 2（成本极低、收益明确）。

---

## 8. Files App（files-community，C#/WinUI）

### 小结

- 搜索框输入即筛 + **范围选择器紧贴搜索框、常驻可见**（当前文件夹/递归，点一下就换）。
- 设置里可**关闭搜索框**。
- 范围状态从不需要"点开菜单才知道"。

### 出处

GitHub files-community/files 的搜索交互 issue/PR 区与设置页。

### 与本仓的关系

本仓范围是全局持久设置（app_search.cpp:44-69），当前范围要点开左带菜单才看到
（nav_bar_draw.cpp:117-145 只有放大镜+小箭头字形）。对应清单 7（cue 显示范围）、14（角标）、
16（仅本次）、24（可关）。

---

## 9. Double Commander

### 小结

- **快速搜索（本层）与文件搜索（跨目录）双入口**，Esc 从过滤逐级退出。
- 路径历史键盘可达；过滤状态在界面上有明确标注。

### 出处

doublecmd.sourceforge.io 官方文档与 GitHub wiki。

### 与本仓的关系

与 TC/XYplorer 同一共识：**过滤与搜索是两个工具**。清单 4、11 的范式来源。

---

## 10. Tablacus Explorer

### 小结

- 地址栏是**加载项**，用户可换实现——架构上"栏元素可替换"。
- 右键 "Paste and Go" 以加载项交付。
- 极简内核 + 一切皆插件：导航栏本体可以只有面包屑，其余按需挂载。

### 出处

GitHub tablacus/TablacusExplorer；另见 docs/RESEARCH-OPENSOURCE-2026-11-26.md §2。

### 与本仓的关系

本仓不做插件架构，但"导航栏右键菜单可扩展"（清单 12 挂 CmdId）与其精神一致。

---

## 11. OneCommander

### 小结

- 现代化双栏管理器：**面包屑路径栏 + 独立搜索框**，搜索范围（当前目录/递归）以内联
  chip 形式**常驻显示在搜索框旁**。
- 输入即筛，无回车提交概念（对单层过滤而言）。
- 深浅主题下搜索框边框/底色与地址框保持**同色同粗**——两个输入框并排不一致会像坏了一个
  （本仓已达标，nav_bar_draw.cpp:171-174 注释即此原则）。

### 出处

onecommander.com 官方文档与更新日志。

### 与本仓的关系

范围 chip 常驻可见（清单 14）、输入即筛（清单 4）。

---

## 12. Q-Dir

### 小结

- 多窗格管理器：每个窗格有自己的地址栏，**地址栏接受拖放**、有路径历史下拉。
- 工具栏元素显隐可在设置中逐项开关。
- 键盘可达性一般（反面参照）：多数下拉只有鼠标路径——这正是本仓要避免的方向。

### 出处

Q-Dir 官方帮助（softwareok.com）。

### 与本仓的关系

反面参照确认"每个下拉都要有键盘通道"（清单 6、20）的必要性。

---

## 13. FreeCommander

### 小结

- 路径栏可切换"面包屑 / 可编辑路径"双态（与 Explorer 同构）。
- 过滤器（Filter）激活时在**路径栏与状态栏同时显示过滤条件**。
- 地址栏右键有"粘贴路径并前往"。

### 出处

freecomander.com 官方手册。

### 与本仓的关系

清单 10（粘贴并前往）、13（编辑态反馈）、14（过滤/范围可见性）。

---

## 14. Everything（voidtools）

### 小结

- **NTFS MFT 直读**的自有索引引擎：不依赖 Windows 索引服务，整机搜索即时出结果。
- 搜索框输入即搜、即时刷新结果列表；搜索语法（`path:`、`size:`）作为**输入内联提示**。
- 是"整机搜索做不到时"的业界替代路线：外部集成（命令行 es.exe）成本远低于自建引擎。

### 出处

voidtools Everything 官方文档（NTFS 索引方式、es.exe 命令行）。

### 与本仓的关系

本仓 kThisPC 档灰字+命令拒绝（search_scope.h:14-21、app_search.cpp:81-87）——search-ms
宿主没有就是没有。清单 25（长期自建）与 26（Everything 过渡集成）的两条路线都出自这里。

---

## 15. ExBar mod（本仓的对齐参照实现）

### 小结

- 导航栏形态母本：栏高 32 / 按钮框 24 / 图标 14（nav_metrics.h:1-3 的运行时生效值）、
  搜索框 SEARCH_W=200 / btnZone=26 / filterW=24 / rightPad=4（nav_metrics.h:27-29 实测值）。
- 面包屑 + 每级 chevron 下拉（ShowCrumbMenu）+ 搜索框左带范围/右带历史的三段形态——本仓
  LayoutSearch / DrawSearchBox 与它同源。
- mod 的局限也是本仓的镜子：**mod 同样没有可视建议下拉、没有拖放、没有失败反馈**——
  "对齐 mod"只能作为下限，不是上限。

### 出处

ExBar mod 源码（exbar.cpp:36505-36512、27590-27601、19883、20056、28756-28758 等实测锚点，
已逐条抄录进本仓注释）。

### 与本仓的关系

几何与形态已对齐（本仓 nav_metrics.h 逐值照抄）。本清单的全部改进都在 mod 之外——
**对标对象要从"mod"升级为"Explorer + 浏览器 + 商业管理器"的并集**。

---

## 16. 本仓现状盘点（逐条读码核实，2026-11-26）

| 域 | 能力 | 现状 | 出处 |
|---|---|---|---|
| 按钮 | 按钮组 | 后退/前进/向上/刷新/视图五颗，后退前进带分裂箭头；灰显由 App SetNavState 驱动 | nav_bar.h:156、nav_bar.h:123-128 |
| 按钮 | 历史下拉 | 分裂箭头左键 / 按钮右键 = 后退或前进历史菜单（各 24 条上限，独立锚点键） | nav_bar_actions.cpp:266-291 |
| 按钮 | 捕获时序 | DOWN 记态、UP 执行、拖出取消；ReleaseCapture 同步派发已处理 | nav_bar_input.cpp:108-153 |
| 地址框 | 双态 | 面包屑/编辑双态；BeginEdit 全选完整路径；Esc 还原退出；Enter 提交 | nav_bar_actions.cpp:30-82 |
| 地址框 | 输入清理 | nav::FromUserText：去空白/全角空格、剥一层配对引号、正斜杠归一；**不展开环境变量** | nav_path.h:58-74 |
| 地址框 | 友好名回解析 | 此电脑/回收站显示友好名，提交经 ico::ParsingPath 解析 | nav_bar_actions.cpp:77-81 |
| 地址框 | Tab 补全 | 按"最后一个反斜杠"切分、子目录循环替换；盘根/UNC 半截可用；**候选不可见** | nav_bar_actions.cpp:84-126、nav_path.h:221-236 |
| 地址框 | 系统补全 | SHAutoComplete(FILESYSTEM\|FILESYS_DIRS\|AUTOAPPEND_FORCE_OFF) | nav_bar_input.cpp:22-28 |
| 地址框 | 修饰键 | 编辑态只处理 Enter/Esc/Tab；Ctrl+Enter/Shift+Enter 无绑定；Alt+Enter 已被"属性"占用 | nav_bar_input.cpp:35-57、keymap.gen.h:113 |
| 地址框 | 失败路径 | 先 EndEdit(true) 再 onNavigate_，**失败时原文已丢**、无控件级反馈 | nav_bar_actions.cpp:78-81 |
| 地址框 | ⌄ 下拉 | 后退+前进+最近合并列表（当前行高亮），底部"清除历史记录"走命令总线带确认 | nav_bar_actions.cpp:293-368 |
| 地址框 | 拖放 | 导航栏未 DragAcceptFiles/RegisterDragDrop（仅书签栏有） | bookmark_bar.cpp:33 |
| 面包屑 | chevron 下拉 | 每级分隔符列出该级子文件夹，内联懒加载树；空目录直接导航不弹空菜单 | nav_bar_actions.cpp:128-196 |
| 面包屑 | 折叠钮 ‹ | 常驻；折叠时列被裁掉的段，未折叠时列"此电脑"顶层 | nav_bar_actions.cpp:243-264、nav_bar_layout.cpp:104-107 |
| 面包屑 | 段级菜单 | 右键段 = 新标签/新窗口/复制路径/终端/属性，作用于右键的那一段；ghost 段直接忽略 | nav_bar_actions.cpp:198-241 |
| 面包屑 | ghost 段 | 向上导航后被放弃的更深分支仍显示（disabled 色），无进一步说明 | nav_bar_draw.cpp:92-94、nav_model.h |
| 面包屑 | 中键 | WM_MBUTTONDOWN 只透传鼠标规则 sink_，无默认动作 | nav_bar_input.cpp:236-242 |
| 搜索框 | 形态 | 常驻右侧 200px@96（可设），窄窗整体收起（地址框最小宽可设默认 160）；左带范围/右带历史 | nav_metrics.h:30-34、nav_bar_layout.cpp:57-66 |
| 搜索框 | 提交 | 仅回车提交；空词=还原目录；提交后焦点回文件列表 | nav_bar_search.cpp:107-118 |
| 搜索框 | 规范化 | 仅提交时 Trim，输入过程无反馈 | nav_bar_search.cpp:113 |
| 搜索框 | cue | 固定"搜索（回车执行，Esc 取消）"，**不含当前范围** | nav_bar_search.cpp:57-59 |
| 搜索框 | Esc | 清词+还原目录，但**焦点留在搜索框** | nav_bar_search.cpp:120-128 |
| 搜索框 | 范围 | 四档，此电脑档灰字+命令拒绝；全局持久，无"仅本次"；卷根搜不到东西零解释 | search_scope.h:33-43、app_search.cpp:44-118、search_folder.h:20-21 |
| 搜索框 | 历史 | 去重置顶上限 100；菜单默认 15 行；**只有整列清空，无单条删除**；键盘 ↑ 无行为 | config.cpp:161-175、app_commands_search.cpp:92-113、nav_bar_search.cpp:25-49 |
| 搜索框 | 历史回填 | search.query.set 只回填不提交 | app_commands_search.cpp:117-121 |
| 键位 | 焦点 | 地址框 F4/Alt+D/Ctrl+L；搜索框 Ctrl+F/Ctrl+E；F6 循环 | keymap.gen.h:92-93、114-116 |

---

## 17. 可借鉴清单（27 条：P1×8 / P2×13 / P3×6）

> 每条格式：现状（出处）→ 建议行为 → 参考产品 → 优先级。
> P1 = 高频/诚实性缺口，下一轮迭代；P2 = 体验补齐，排期可让位；P3 = 锦上添花或成本高需立项。
> 改动落点全部限于导航栏/搜索域文件，不触碰 shell_view 与命令总线结构。
> 清单 1–17 承接 docs/RESEARCH-SEARCHBOX-2026-11-26.md 的 17 条（优先级按"整栏口径"重排）。

### 1. 地址栏输入建议下拉（历史 + 子目录 + 命名空间候选，Tab 补全可视化）【P1】

- **现状**：编辑态只有系统 SHAutoComplete + 纯 Tab 盲替换，候选不可见——用户不知道有几个
  候选、Tab 要按几轮（nav_bar_input.cpp:22-28、nav_bar_actions.cpp:84-126）。
- **建议**：输入变化时弹与地址框同宽的自绘建议列表（复用 popup_list）：候选 = 前缀匹配的
  路径历史 + 当前目录子目录（复用 NavModel::ChildFolders / nav::SplitForComplete）+ 命名空间
  静态候选；↑↓ 选择、Enter 导航选中项、Esc 关列表再还原原文（逐级回退）；Tab 改为"填入当前
  高亮候选"，候选数在行尾标注。
- **参考**：Chrome/Firefox omnibox（§2、§3）；Explorer++ AddressBarView；Explorer 系统补全下拉（§1）。
- **优先级**：P1。地址栏每天几十次，"盲替换"是与所有参考产品的最大差距。

### 2. 地址栏环境变量展开（%VAR%）【P1】

- **现状**：nav::FromUserText 不展开环境变量且注释称"留在调用方"，但 CommitEdit 没做
  （nav_path.h:58-74、nav_bar_actions.cpp:67-82）——输入 %TEMP% 回车直接失败，报错原因与
  用户心智完全无关。
- **建议**：CommitEdit 在 FromUserText 清理后、ParsingPath 解析前，对整串形如 %VAR% 的文本做
  ExpandEnvironmentStringsW（失败保留原文走正常报错）；顺带支持 shell: 别名可选。语义与
  Explorer++、Explorer 一致。展开规则加纯函数测试（tests/ 同层级）。
- **参考**：Explorer 地址栏（§1）；Explorer++（§7）。
- **优先级**：P1。一个纯函数 + 一处调用，失败体验却是"回车没反应"级别。

### 3. 搜索范围降级反馈：卷根 / 未索引位置提示【P1】

- **现状**：作用域为卷根（C:\、D:\）时搜索文件夹绑得上但查不到东西（卷根不是 crawl scope，
  search_folder.h:20-21）——用户只见 0 结果，零解释；"0 结果"与"根本搜不了"无法区分。
- **建议**：App::RunSearch 在 Resolve 得到卷根作用域时给与 kThisPC 同级的明确提示（"该卷不在
  Windows 索引范围内…"，文案风格沿用 app_nav.cpp:198-200）；中期可降级为自有枚举的慢速搜索
  （明确标注"慢速模式"），不静默返回空。
- **参考**：Explorer 未索引位置提示"可能较慢"（§1）；Everything（§14，文案里的替代建议）。
- **优先级**：P1。诚实反馈优先于新功能。

### 4. 搜索即输即筛（增量过滤，两阶段）【P1】

- **现状**：搜索词必须回车提交（CommitSearch 唯一入口，nav_bar_search.cpp:107-118），输入
  过程零反馈。
- **建议**：第一阶段：输入防抖（~200ms）后仅对"本层"档做轻量过滤高亮（匹配计数，不动
  shell 视图）；第二阶段：设置开关"输入即过滤当前文件夹"（XYplorer Live Filter 语义：本层
  瞬时、Esc 还原），与"回车跨目录搜索"并存——**过滤与搜索是两个工具**，不合并语义；过滤
  结果不进搜索历史（与 search.query.set 的"不提交"哲学一致）。
- **参考**：XYplorer Live Filter（§6）；Opus Filter Bar（§4）；Explorer 输入即搜（§1）；TC/DC
  双入口口径（§5、§9）。
- **优先级**：P1（第一阶段成本可控；第二阶段涉及列表过滤管线，单独立项）。

### 5. 地址提交失败的输入保留与反馈【P1】

- **现状**：CommitEdit 先 EndEdit(true) 再 onNavigate_（nav_bar_actions.cpp:78-81）——路径无效
  时编辑框已关、原文已丢，只能整串重输；失败原因无控件级反馈。
- **建议**：导航失败时（App 侧失败回调）回到编辑态并**恢复提交前的文本**（选中尾段便于改
  错字），地址框给一次性边框错误色/气泡；成功路径时序不变（先还焦点仍正确），只有失败分支
  找回文本。App 需把"失败 + 原文"回传 NavBar（新增回调或复用 SetPath 失败分支）。
- **参考**：Chrome/Firefox omnibox 无效提交保留原文（§2、§3）；Explorer 失败明确报错（§1）。
- **优先级**：P1。出错时的体验决定用户敢不敢用地址栏输长路径。（SEARCHBOX 报告列为 P2，
  整栏口径下升级：这是"诚实性缺口"而非普通体验项。）

### 6. 搜索历史键盘可达（↑ 展开历史、↑↓ 选择、Enter 执行选中词）【P1】

- **现状**：历史下拉只有鼠标路径（nav_bar_search.cpp:170-176）；SearchProc 只处理
  Enter/Esc/Tab（nav_bar_search.cpp:25-49），输入框按 ↑ 无任何行为。
- **建议**：聚焦时按 ↑ 打开历史弹层（复用 ShowSearchHistoryMenu 的锚点与宽度），弹层内 ↑↓
  移动、Enter **执行**该词（键盘路径用户明确按了 Enter = 提交；AI/回填路径维持"不提交"
  哲学不变）。
- **参考**：TC 路径行 ↓ 展开历史（§5）；Firefox 地址栏 ↑ 开历史、Enter 执行（§3）。
- **优先级**：P1。历史菜单已存在，只缺键盘通道；注意与单行 EDIT 的 ↑（无语义）不冲突。
  （SEARCHBOX 报告列为 P2，整栏口径下升级：Q-Dir 的反面参照说明这是高频缺口。）

### 7. 搜索框 cue 占位显示当前范围【P1】

- **现状**：cue 固定"搜索（回车执行，Esc 取消）"（nav_bar_search.cpp:57-59），当前范围要点
  开左带菜单才知道；范围改错时提交完才发现搜错了地方。
- **建议**：cue 动态拼接："在 <范围短标签> 中搜索（回车执行，Esc 取消）"，短标签复用
  searchscope::RangeShortLabel（app_nav.cpp:226 结果标题同源）；范围变化时重发
  EM_SETCUEBANNER；自定义档截断显示叶子名。
- **参考**：Explorer 占位提示搜索位置（§1）；Files App 常驻范围选择器（§8）；OneCommander
  范围 chip（§11）。
- **优先级**：P1。一处 EM_SETCUEBANNER + 一个拼接函数，成本接近零，直接消除"搜错地方"类
  事故。（SEARCHBOX 报告列为 P2，整栏口径下升级：与清单 3 同属诚实性缺口。）

### 8. 拖放路径到地址栏导航【P1】

- **现状**：导航栏未 DragAcceptFiles/RegisterDragDrop（仅书签栏有，bookmark_bar.cpp:33）——
  文件夹拖到地址栏毫无反应。
- **建议**：地址框接受 CF_HDROP：文件夹 → 导航到该目录；文件 → 导航到父目录并选中
  （Explorer 同语义）；命名空间项走 SHParseDisplayName。搜索框不接受拖放（避免与"拖文件
  设范围"的未定语义纠缠）。实现参考书签栏 WM_DROPFILES（bookmark_bar.cpp:384）。
- **参考**：Explorer 地址栏拖放导航（§1，系统惯例）；Opus、XYplorer（§4、§6）；Q-Dir（§12）。
- **优先级**：P1。系统级惯例缺失属于"和系统对着干"级差距。（SEARCHBOX 报告列为 P2，
  整栏口径下升级。）

### 9. 地址栏修饰回车：Ctrl+Enter 新标签 / Shift+Enter 新窗口【P2】

- **现状**：编辑态只处理 Enter/Esc/Tab（nav_bar_input.cpp:35-57）；Alt+Enter 已被"属性"占用
  （keymap.gen.h:113）不可复用。
- **建议**：CommitEdit 加 disposition 参数（current/new tab/new window），EditProc 映射
  Ctrl+Enter/Shift+Enter；执行走既有命令总线（与面包屑段菜单 TabNew / FsOpenInNewWindow
  同源，nav_bar_actions.cpp:219-226）。键位进 data/keymap.json（勿手改 keymap.gen.h）。
- **参考**：浏览器 Alt+Enter = 新标签（§2、§3）；Opus/TC 修饰键双轨（§4、§5）；本仓段菜单
  已确立"新标签/新窗口"语义。
- **优先级**：P2。语义与现有命令完全同源，低成本补齐。

### 10. 地址栏"粘贴并前往"【P2】

- **现状**：地址框用默认 EDIT 右键菜单，无"粘贴并前往"；粘贴路径后还要再按回车
  （nav_bar_input.cpp:22-28 未自绘菜单）。
- **建议**：自绘编辑态右键菜单（系统标准项基础上追加）：剪贴板经 FromUserText 清理后是可
  解析路径时追加"粘贴并前往"，非路径灰显；判定复用 nav::FromUserText + ico::ParsingPath。
- **参考**：Chrome Paste and Go（§2）；Tablacus 加载项（§10）；FreeCommander（§13）。
- **优先级**：P2。"复制→粘贴→回车"是文件管理器最高频三步之一，合并为一步。

### 11. 搜索框 Esc 二段式：清词 → 焦点回列表【P2】

- **现状**：Esc 清词还原目录但焦点留在搜索框（nav_bar_search.cpp:120-128）；Enter 路径才有
  refocus（:117）。
- **建议**：Esc 二段式：框内有词时清词还原（焦点不动，便于改词重搜）；框内已无词时 Esc →
  焦点还文件列表（复用 CommitSearch 的 WM_APP_REFOCUS 通道）。
- **参考**：Explorer（§1）；Double Commander 过滤框逐级退出（§9）。
- **优先级**：P2。一处分支判断，纯键盘流收益。

### 12. 面包屑段右键菜单挂 CmdId 补齐（键位提示统一）【P2】

- **现状**：段菜单五项已挂 CmdId（nav_bar_actions.cpp:219-226），但 App 侧 SegmentCmdFn
  分发路径（app.cpp）若与命令总线键位表脱节，行尾快捷键提示会缺失或与实际不符——menu.h
  的"快捷键由键位表自动推出"只在挂 CmdId 的行生效。
- **建议**：核对 App 侧 switch（kSegOpenNewTab→TabNew 等）五项全部转投 CommandBus::Invoke
  而不是直调实现函数；与"清除历史记录"行（nav_bar_actions.cpp:341-342）同一口径。缺参数的
  行（clipboard.copy / fs.properties 需 path）确认 key_binding 空参判层不误吞。
- **参考**：Tablacus 的菜单=命令总线精神（§10）；本仓 menu.h 既有约定。
- **优先级**：P2。一致性核对，防"菜单显示 A、执行走 B"类漂移。

### 13. 地址栏编辑态输入反馈（有效性预检）【P2】

- **现状**：编辑态对输入内容零反馈——路径是否存在、引号是否配对，都要到回车才知道；
  失败后又丢输入（清单 5 的前半段）。
- **建议**：轻量预检：输入停顿（~300ms）后对文本做 FromUserText + 存在性检查（只查盘符
  前缀与首段，不全量 stat），无效时边框给警示色（不阻止提交）；有效时不打扰。
- **参考**：FreeCommander 路径校验（§13）；浏览器 omnibox 的 URL 状态着色（§2）。
- **优先级**：P2。与清单 5 配套（5 管失败后，13 管提交前）。

### 14. 非默认搜索范围的可见性（左带角标）【P2】

- **现状**：左带只有放大镜+小箭头字形（nav_bar_draw.cpp:117-145）；结果标题的范围短标签
  （app_nav.cpp:226）要搜完才看到。
- **建议**：当前范围 ≠ 默认档（kSubtree）时，左带放大镜旁叠微型文字徽标（"本层"/"自定义"，
  尺寸走 theme 令牌不引入新字号）；悬停 tooltip 显示完整说明；默认档不加角标保持安静。
- **参考**：Opus Filter Bar 状态可见（§4）；Files App（§8）；OneCommander chip（§11）。
- **优先级**：P2。防"菜单显示 A、实际搜 B"的 UI 保险丝。（SEARCHBOX 报告列为 P3，整栏
  口径下与 7、3 归为一组"范围可见性"提级。）

### 15. 搜索历史单条删除【P2】

- **现状**：只有整列清空（app_commands_search.cpp:105-113，带确认）；误存一条只能连好历史
  一起清。
- **建议**：历史下拉行 hover 显示 ✕（或右键"删除此项"）：定向 remove 该词并 Save，走命令
  总线新增 search.history.remove {query}（与 search.history.clear 同层级，返回 removed:bool）；
  popup_list 若不支持行内动作，先做右键菜单版。
- **参考**：Firefox Shift+Delete（§3）；Chrome 历史单条删除（§2）。
- **优先级**：P2。数据面小改，成本在菜单交互。（SEARCHBOX 报告列为 P3，整栏口径下随
  键盘通道（清单 6）一并提级——两者同属"历史管理"一组。）

### 16. "仅本次"搜索范围（Shift+点击范围菜单）【P2】

- **现状**：范围是全局持久设置（app_search.cpp:44-69 经 config.set 落盘）；临时换范围得记着
  改回来。
- **建议**：范围菜单 Shift+点击 = 仅本次（内存覆盖不落盘，下次提交或清词后回弹默认档）；
  普通点击维持持久；菜单行提示"Shift+点击仅本次"；AI/命令路径不变（临时档也必须走同一
  searchscope::Resolve 单源）。
- **参考**：Opus 一次性过滤（§4）；浏览器临时引擎切换（§2）。
- **优先级**：P2。状态机加一层临时覆盖，注意单源原则。

### 17. 面包屑 ghost 段的降噪处理【P2】

- **现状**：向上导航后，被放弃的更深分支仍以 disabled 色显示在面包屑里
  （nav_bar_draw.cpp:92-94），段菜单对 ghost 段直接忽略（nav_bar_actions.cpp:206）——用户
  看得见却点不动，没有任何解释。
- **建议**：两选一：(a) ghost 段仅保留一级（紧邻当前级的那个），更深的不显示；(b) 保留但
  tooltip 说明"历史位置（已被放弃）"。倾向 (a)：Explorer/Opus 的面包屑都不显示幽灵层级。
- **参考**：Explorer、Opus 面包屑（§1、§4，均无幽灵段）。
- **优先级**：P2。"看得见点不动"是 nav_bar.h:139-141 已修的"亮着但点不动"在面包屑上的残余。

### 18. 折叠钮（‹）在未折叠时合并历史入口【P2】

- **现状**：折叠钮未折叠时列"此电脑"顶层（nav_bar_actions.cpp:253-256），与地址框 ⌄ 的
  合并下拉（后退+前进+最近）功能互补但入口分散——用户要记住两个箭头各是什么。
- **建议**：保持现状或（低成本版）在折叠菜单顶部加"最近访问"分区（复用 recentList_），
  让未折叠态的 ‹ 也能触达最近位置；不合并两颗箭头的职责（锚点键已分开，nav_bar_actions.cpp:288）。
- **参考**：Opus location field 的历史+层级合一（§4）。
- **优先级**：P2。入口语义微调，注意不与 ⌄ 下拉重复。

### 19. 地址框中键 = 粘贴并前往【P2】

- **现状**：地址框区域中键只透传鼠标规则 sink_（nav_bar_input.cpp:236-242），无默认动作；
  编辑态也没有中键语义。
- **建议**：中键点击地址框（非编辑态进入编辑态时）或编辑态中键：剪贴板是路径时直接粘贴
  并提交（与清单 10 同一判定函数）；不是路径时维持透传 sink_。鼠标规则 sink_ 仍优先。
- **参考**：浏览器的中键粘贴语义（部分发行版）；FreeCommander 粘贴并前往（§13）。
- **优先级**：P2。与清单 10 共享判定，增量成本极小。

### 20. 键盘打开搜索范围菜单（Alt+↓ / Ctrl+↓）【P2】

- **现状**：范围菜单只有鼠标路径（左带点击，nav_bar_input.cpp:204-207）；SearchProc 键盘
  只处理 Enter/Esc/Tab（nav_bar_search.cpp:25-49）——纯键盘用户换不了范围。
- **建议**：搜索框聚焦时 Alt+↓（或 Ctrl+↓）触发 ShowSearchScopeMenu（键盘重入路径已有
  先例：chevron 的 popup::IsOpen 判重入，nav_bar_actions.cpp:168-176）；方向键在菜单内选择
  由菜单组件既有键盘通道承接。
- **参考**：浏览器地址栏 ↓ 打开建议（§2、§3）；TC 路径行 ↓（§5）。
- **优先级**：P2。与清单 6 归为同一组"下拉键盘通道"。

### 21. 搜索框宽度自适应输入内容【P2】

- **现状**：搜索框固定 200px@96（可设但全局静态，nav_bar_layout.cpp:57-66）；长词输入时
  可视区域只有 ~150px，长查询编辑体验差。
- **建议**：输入超过可视宽度时允许输入条横向滚动（EDIT 已具备，ES_AUTOHSCROLL 已开），
  但聚焦时把搜索框临时加宽（如 +80px，失焦还原），或提供"聚焦时展开"设置项；宽度变化走
  Layout 单源，不新开几何出处。
- **参考**：Explorer Win11 搜索框聚焦展开（§1）；Chrome omnibox 聚焦加宽（§2）。
- **优先级**：P2。注意与窄窗收起判定（nav_bar_layout.cpp:63-65）的相互作用。

### 22. 中键点击面包屑段 = 在新标签打开【P3】

- **现状**：段左键导航、右键段菜单有"在新标签打开"（nav_bar_input.cpp:197-201、
  nav_bar_actions.cpp:198-241）；中键只透传 sink_（nav_bar_input.cpp:236-242）——"中键=新
  标签"的通用心智在面包屑上断了。
- **建议**：WM_MBUTTONDOWN 命中 seg（非 ghost）时默认走段菜单同款"在新标签页打开"
  （segmentCmd_ + kSegOpenNewTab 同一出口）；sink_ 仍优先（自定义规则不被默认覆盖）。
- **参考**：浏览器中键=新标签（§2）；TC/Opus 中键路径=新标签（§4、§5）。
- **优先级**：P3。语义与既有出口完全同源。

### 23. 地址栏显示名 ↔ 完整路径切换【P3】

- **现状**：面包屑显示友好名（此电脑/回收站等经 ico::DisplayName，nav_bar_actions.cpp:24-26）；
  完整路径只在编辑态可见——想"看一眼完整路径"必须进入编辑态。
- **建议**：低优先方案：地址框 ⌄ 下拉顶部加"复制当前完整路径"行（走 ClipboardCopy 命令），
  覆盖 90% 场景；完整"点击栏空白切换显示"涉及 NavModel 双轨文本，成本高收益低。
- **参考**：Explorer 的地址栏复制路径右键项（§1）；FreeCommander（§13）。
- **优先级**：P3。有更便宜的替代（复制路径行）。

### 24. "隐藏搜索框"开关【P3】

- **现状**：搜索框常驻（nav_bar_search.cpp 头注），窄窗自动收起，但无主动关闭设置——不用
  搜索的用户只能把 navBarSearchWidth 压到最小间接腾地方。
- **建议**：设置新增"显示搜索框"开关（默认开）：关闭时 Layout 不切搜索框宽度（searchW 恒
  0），FocusSearch/Ctrl+F 自动降级聚焦地址框（命令回执说明）；设置面板、config.set、config_io
  三处同步（沿用 navBarSearchWidth 既有管线）。
- **参考**：Files App 可关搜索框（§8）；Opus/TC 工具栏元素显隐（§4、§5）。
- **优先级**：P3。尊重"搜索不是每个人的高频动作"，默认仍常驻。

### 25. 整机搜索的替代实现（长期）【P3】

- **现状**：kThisPC 档灰字"（本宿主暂不可用）"+ 命令路径直接拒绝（app_search.cpp:81-87、
  search_scope.h:14-21）——整机搜索要 Explorer 进程内的 search-ms 宿主，本宿主没有。
- **建议**：中期二选一：(a) 自建 Windows Search 查询（searchapi.h 的 ISearchQueryHelper，
  需先补编译器支持，search_folder.h:26 已注明缺失）；(b) 混合虚拟文件夹（多作用域聚合）。
  落地前维持现状不动——**不写兑现不了的选项**是本仓既定原则。
- **参考**：Explorer 全局 search-ms（§1）；Everything（§14）。
- **优先级**：P3。成本高需立项。

### 26. Everything 外部集成过渡（es.exe 检测跳转）【P3】

- **现状**：整机搜索做不到（清单 25），但用户机器上常已装有 Everything——现状没有任何
  利用。
- **建议**：小步过渡版：启动/首次展开此电脑档时检测本机 Everything（安装路径或 es.exe 在
  PATH），kThisPC 档的菜单行从纯灰字升级为"用 Everything 搜索（本机已安装）"可点项（外部
  Process 启动 es.exe -p <当前目录>）；未安装维持灰字。不引入对 Everything 的运行时依赖。
- **参考**：Everything es.exe 命令行（§14）；清单 25 的过渡方案。
- **优先级**：P3。小步、可回退、诚实（不假装是自己搜的）。

### 27. 搜索框宽度拖拽调节【P3】

- **现状**：搜索框宽度是全局静态设置（设置面板数值 + config.set），没有拖拽——调宽度要开
  设置面板，且调完看不到即时效果。
- **建议**：地址框与搜索框交界处提供拖拽手柄（hover 显示 ⇔ 光标），拖动实时更新
  navBarSearchWidth（下限 = 地址框最小宽 kAddrMin96，上限走既有 clamp）；拖完落盘一次。
  几何仍由 Layout 单源产出。
- **参考**：Opus 可拖拽的分隔与栏宽（§4）；Explorer Win11 搜索框拖宽（§1）。
- **优先级**：P3。交互成本（热区/光标/拖拽时序）不小，收益是设置路径的便利化。

---

## 附：清单速览

| # | 改进 | 优先级 |
|---|---|---|
| 1 | 地址栏输入建议下拉（历史+子目录+命名空间，Tab 可视化） | P1 |
| 2 | 地址栏环境变量展开（%VAR%） | P1 |
| 3 | 搜索范围降级反馈（卷根/未索引提示） | P1 |
| 4 | 搜索即输即筛（增量过滤，两阶段） | P1 |
| 5 | 地址提交失败的输入保留与反馈 | P1 |
| 6 | 搜索历史键盘可达（↑↓ + Enter 执行） | P1 |
| 7 | 搜索框 cue 占位显示当前范围 | P1 |
| 8 | 拖放路径到地址栏导航 | P1 |
| 9 | 地址栏修饰回车（Ctrl+Enter 新标签 / Shift+Enter 新窗口） | P2 |
| 10 | 地址栏"粘贴并前往" | P2 |
| 11 | 搜索框 Esc 二段式（清词 → 焦点回列表） | P2 |
| 12 | 面包屑段右键菜单挂 CmdId 补齐 | P2 |
| 13 | 地址栏编辑态输入反馈（有效性预检） | P2 |
| 14 | 非默认搜索范围可见性（左带角标） | P2 |
| 15 | 搜索历史单条删除 | P2 |
| 16 | "仅本次"搜索范围（Shift+点击） | P2 |
| 17 | 面包屑 ghost 段的降噪处理 | P2 |
| 18 | 折叠钮（‹）未折叠时合并最近访问入口 | P2 |
| 19 | 地址框中键 = 粘贴并前往 | P2 |
| 20 | 键盘打开搜索范围菜单（Alt+↓ / Ctrl+↓） | P2 |
| 21 | 搜索框宽度自适应（聚焦展开） | P2 |
| 22 | 中键点击面包屑段 = 新标签打开 | P3 |
| 23 | 地址栏显示名 ↔ 完整路径切换 | P3 |
| 24 | "隐藏搜索框"开关 | P3 |
| 25 | 整机搜索替代实现（长期） | P3 |
| 26 | Everything 外部集成过渡（es.exe 检测跳转） | P3 |
| 27 | 搜索框宽度拖拽调节 | P3 |

**P1×8 / P2×13 / P3×6 = 27 条。**
