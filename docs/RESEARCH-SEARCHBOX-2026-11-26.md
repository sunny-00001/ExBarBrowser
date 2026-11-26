---
status: current
baseline: 2026-11-26
method: 多轮 WebSearch + WebFetch（Microsoft Learn / Chromium 设计文档 / Mozilla 支持 / Directory Opus 手册 / Total Commander 论坛与 wiki / XYplorer 帮助 / GitHub 源码与 issue 区）
去重: 与 docs/RESEARCH-OPENSOURCE-2026-11-26.md（开源实现架构）、docs/COMPETITIVE-GAP-2026-11-21.md（M1–M8 能力缺口）、docs/EXPLORER-GAP-AUDIT.md（Explorer 对齐审计）互补，不重复其结论；交集处显式标注"关联已知缺口"
范围: 只覆盖导航栏的**地址框**与**搜索框**两个控件（src/nav_bar*.cpp、src/search_scope.h、src/search_folder.h、src/app_search.cpp、src/app_commands_search.cpp）；书签栏、面包屑绘制细节不在此文
---

# 地址栏与搜索框交互调研报告（2026-11-26）

> 本文回答：**主流文件管理器与浏览器在"地址输入"和"搜索输入"两个控件上的交互范式，
> 哪些可以直接落到 ExBarBrowser 的导航栏**。
>
> 口径：每条改进都先写本仓**已核对的现状**（文件:行号），再给建议行为、参考产品与优先级。
> 所有"现状"均于 2026-11-26 逐条读码核实，非凭印象。

---

## 1. Windows 资源管理器（Win10/Win11）

### 小结

Win11 把地址栏与搜索框在视觉上做了整合（地址栏内直接输入即触发搜索建议），但 Win10 与
Win11 的键盘语义一致：

- **面包屑 ↔ 编辑双态**：点击面包屑空白处进入编辑态并**全选当前完整路径**；Esc 恢复面包屑
  显示（文本还原），Enter 提交导航。失败路径（不存在/拼写错）会给明确报错而非静默留在原地。
- **自动完成**：地址栏用系统 SHAutoComplete（文件系统目录优先），下拉式候选列表可见可选，
  不是"盲替换"。
- **搜索框**：输入即搜（每敲一个字符就更新结果），Esc 一次清词回原目录，焦点同时还给文件
  列表；搜索框占位文案会提示当前搜索位置（"在 xxx 中搜索"）。
- **修饰回车**：地址栏 Ctrl+Enter / Alt+Enter 在浏览器语义下开新标签（资源管理器地址栏本身
  不支持，但同窗口其他入口——如右键菜单——遵循"新标签/新窗口"双轨）。
- **拖放**：文件夹拖到地址栏 = 导航到该文件夹（系统级惯例）。

### 出处

- Microsoft Learn：SHAutoComplete（SHACF_FILESYS_DIRS 等标志语义）、EM_SETCUEBANNER、
  ISearchFolderItemFactory（shobjidl_core.h）、Windows Search 爬取范围（crawl scope）与索引
  排除位置的说明页。
- Microsoft Support：Explorer 键盘快捷键与搜索框行为说明页。
- docs/EXPLORER-GAP-AUDIT.md（本仓对 Explorer 行为的既有审计，F1 = search-ms: 导航被宿主拒绝）。

### 与本仓的关系

面包屑双态（BeginEdit 全选 / Esc 还原 / Enter 提交）、搜索框常驻、cue 占位文案三件事本仓**已达标**
（见 §6 现状盘点）。Explorer 真正多出来的是：可见的自动完成候选列表、失败反馈、拖放。

---

## 2. 浏览器 Omnibox（Chrome / Firefox）

### 小结

浏览器地址栏（omnibox / Awesome Bar）是"统一输入框"范式的标杆，对本仓可借鉴的不是"搜网页"，
而是**输入过程的反馈机制**：

- **输入即建议**：每个按键都刷新一个与输入框同宽的下拉建议列表（历史 + 匹配 + 补全），
  ↑↓ 在建议间移动、Enter 执行选中项、Shift+Delete **删除单条历史**。
- **粘贴并前往（Paste and Go）**：右键菜单对"剪贴板是 URL/路径"这一状态给出专用项，
  粘贴 + 提交一步完成。
- **修饰回车**：Alt+Enter = 在新标签打开当前输入（不改当前页）。
- **失败不丢输入**：无效输入提交后输入框**保留原文**并给出提示，用户可改一字重试。
- **Esc 三段语义**：清下拉 → 还原原文 → 退出编辑，逐级回退而不是一步丢光。
- **历史单条删除**与**清空**分开：误存一条不必清全部。

### 出处

- Chromium Omnibox Design Document（chromium.org → Developers → Design Documents → Omnibox，
  含模型选择/建议排序/Paste and go 的交互约定）。
- Mozilla 支持：Address bar autocomplete in Firefox（↑↓/Enter/Shift+Delete 行为）；
  Firefox source 的 urlbar 模块注释。
- 两者均为公开文档/源码，具体页面随版本更新，此处给入口不给快照 URL。

### 与本仓的关系

本仓地址框编辑态只有系统 SHAutoComplete + 纯 Tab 循环补全，**没有可视化建议下拉**；提交失败
会丢输入；搜索历史只能整列清空。这三点都是 omnibox 已验证的成熟范式，直接对应 §7 清单 1、8、14。

---

## 3. 商业文件管理器（Directory Opus / Total Commander / XYplorer）

### 小结

三家对"地址 + 搜索"的共识：

- **地址栏历史下拉**：除系统补全外，每家都有自己的路径历史下拉（TC 的路径行 ↓ 展开、
  Opus 的 location field 历史），历史是"可导航的最近位置"，不是只读列表。
- **即时过滤（Live Filter / 快速筛选）**：XYplorer 的 Live Filter 与 Opus 的 Filter Bar 都是
  "输入即筛当前列表"，与"全库搜索"是**两个不同工具**：过滤是本层瞬时操作（Esc 还原），
  搜索是跨目录查询（有独立结果视图）。本仓搜索框走的是后者。
- **范围/过滤状态可见**：过滤器激活时 Opus 在状态栏与过滤条上明确显示"过滤中"，不会出现
  "列表被筛过但界面上看不出"。
- **拖放**：地址栏普遍接受拖放导航（XYplorer/Opus 均支持）。
- **键盘可达**：TC 的路径行支持 ↑ 直接展开历史、Enter 重搜/导航；过滤框 Esc 逐级退出。

### 出处

- Directory Opus 官方手册（gpsoft.com.au → Manual）：Location Field、Filter Bar 章节。
- Total Commander 官方论坛与 ghisler wiki：路径行历史、快速搜索（quick search）参数。
- XYplorer 帮助文件（Live Filter / Find Files 章节）。

### 与本仓的关系

"过滤 vs 搜索是两个工具"的划分正是本仓搜索框现缺的表达：现状只有回车提交式搜索（见 §6），
输入过程零反馈。清单 4（即输即筛）、13（范围可见性）直接来自这组范式。

---

## 4. 开源文件管理器实现（Explorer++ / Files / Double Commander / Tablacus）

### 小结

- **Explorer++**（C++/Win32，已在 docs/RESEARCH-OPENSOURCE-2026-11-26.md §1 沉淀）：
  地址栏控件/视图两层分离；`AddressBar::OnKeyPressed` 处理 Enter 提交、Escape **还原原文**；
  支持环境变量展开与路径自动补全。环境变量展开对本仓是现成缺口（nav::FromUserText 明确不做）。
- **Files App**（files-community，C#/WinUI）：搜索框输入即筛 + 范围选择器（当前文件夹/
  递归）紧贴搜索框，范围状态始终可见；设置里可关闭搜索框。
- **Double Commander**：快速搜索（本层）与文件搜索（跨目录）双入口，Esc 从过滤逐级退出。
- **Tablacus Explorer**：地址栏是加载项，用户可换实现；右键"Paste and Go"以加载项交付
  （见 RESEARCH-OPENSOURCE §2）。

### 出处

- Explorer++ 源码（GitHub derceg/explorerplusplus，AddressBar.cpp / AddressBarView）。
- Files App 仓库（GitHub files-community/files）的搜索交互 issue/PR 区与设置页。
- Double Commander 官方文档（doublecmd.sourceforge.io / GitHub wiki）。
- 本仓 docs/RESEARCH-OPENSOURCE-2026-11-26.md。

### 与本仓的关系

Explorer++ 的"Esc 还原原文 + 环境变量展开"与本仓 nav::FromUserText 的清理职责相邻，
清单 2（环境变量）直接可落；Files 的"搜索框可关"对应清单 16。

---

## 5. Windows 搜索平台约束（本仓实测边界）

### 小结

搜索框的改进空间受**原生搜索文件夹**（ISearchFolderItemFactory）的硬约束限制，
以下均为 src/search_folder.h 2026-11-22 的实测记录：

- 作用域只能是**一个带尾斜杠的普通文件系统目录**；命名空间项、多目录数组、不给作用域都
  绑不上（BrowseToObject 返回 S_OK 但视图留在原地）。
- **卷根（C:\、D:\）绑得上但查不到东西**——卷根不是 crawl scope；用户只会看到"0 结果"，
  没有任何解释。
- **整机搜索（此电脑档）做不到**：那是 Explorer 进程内 search-ms 全局查询，本宿主没有该宿主；
  现状把该档摆成灰字并在命令路径直接拒绝（searchscope::RangeSupported）。
- 替代路线：自己查 Windows Search 查询 API（searchapi.h，本编译器缺）或做混合虚拟文件夹；
  业界另一条路是 Everything 式 NTFS MFT 直读（自有引擎，不依赖索引服务）。

### 出处

- src/search_folder.h 头注（实测矩阵）、src/search_scope.h（kThisPC 说明）、
  docs/EXPLORER-GAP-AUDIT.md §2.1 F1。
- Microsoft Learn：ISearchFolderItemFactory::SetScope、Windows Search 索引与爬取范围管理。
- voidtools Everything 官方文档（NTFS 索引方式说明）。

### 与本仓的关系

约束决定了改进优先做"**诚实的反馈**"（卷根/未索引提示，清单 3）而不是承诺搜不出来范围。
清单 17 是长期路线，不在近期承诺。

---

## 6. 本仓现状盘点（逐条读码核实，2026-11-26）

| 能力 | 现状 | 出处 |
|---|---|---|
| 地址栏双态 | 面包屑显示 / 点击进入编辑态（全选完整路径），Esc 退出还原，Enter 提交 | nav_bar_actions.cpp:30-82 |
| 输入清理 | nav::FromUserText：去首尾空白/全角空格、剥一层配对引号、正斜杠归一；**不展开环境变量**（注释明确留给调用方，且当前无调用方做） | nav_path.h:58-74 |
| 友好名回解析 | 「此电脑/回收站」等命名空间显示友好名，提交经 ico::ParsingPath 解析 | nav_bar_actions.cpp:70-81 |
| Tab 补全 | DoComplete 按"最后一个反斜杠"切分，子目录循环替换；盘根/UNC 半截可用；**无可视候选列表**；命名空间跳过 | nav_bar_actions.cpp:84-126、nav_path.h:221-236 |
| 系统补全 | SHAutoComplete(FILESYSTEM \| FILESYS_DIRS \| AUTOAPPEND_FORCE_OFF) | nav_bar_input.cpp:22-28 |
| 修饰键 | 编辑态只处理 Enter/Esc/Tab（nav_bar_input.cpp:35-57）；Ctrl+Enter/Shift+Enter 无绑定；Alt+Enter 已被"属性"占用（keymap.gen.h:113） | — |
| 右键菜单 | EDIT 默认系统菜单，无"粘贴并前往" | nav_bar_input.cpp:22-28（未自绘） |
| 拖放 | 导航栏未 DragAcceptFiles / RegisterDragDrop（仅书签栏有） | bookmark_bar.cpp:33 |
| 地址框下拉 ⌄ | 非编辑/编辑态均可点，弹后退+前进+最近合并列表（当前目录行高亮），底部"清除历史记录"（走命令总线，带确认） | nav_bar_actions.cpp:293-368 |
| 搜索框形态 | 常驻右侧 200px@96（可设），窄窗整体收起（地址框最小宽可设，默认 160）；左带=范围菜单、右带=历史下拉，弹层与搜索框同宽同锚 | nav_metrics.h:30-34、nav_bar_layout.cpp:57-70、nav_bar_search.cpp:159-176 |
| 搜索提交 | 仅回车提交（CommitSearch）；空词=还原目录；提交后焦点回文件列表 | nav_bar_search.cpp:107-118 |
| 搜索词规范化 | searchurl::Trim；**仅提交时 Trim**，输入过程无任何反馈 | app_nav.cpp:177、search_url.h:20 |
| 搜索执行 | 原生搜索文件夹（ISearchFolderItemFactory PIDL），不用 search-ms: URL（宿主拒绝） | search_folder.h:1-27、app_nav.cpp:163-226 |
| 范围 | 四档（本层/含子目录/此电脑/自定义），此电脑档灰字+命令拒绝；真相在 searchscope::Resolve 单源；**全局持久设置，无"仅本次"** | search_scope.h:33-43、app_search.cpp:44-118 |
| 搜索历史 | 提交时去重置顶（AddSearchHistory），上限 100 可调；菜单行数可设（默认 15）；**只有整列清空，无单条删除** | config.cpp:161-175、app_commands_search.cpp:92-113 |
| 历史回填 | search.query.set 只回填不执行（"替用户按回车=替他决定"） | app_commands_search.cpp:117-121 |
| cue 占位 | 固定文案"搜索（回车执行，Esc 取消）"，**不含当前范围**；地址框无 cue（面包屑即占位） | nav_bar_search.cpp:57-59 |
| Esc 语义（搜索） | 清词+还原目录，但**焦点留在搜索框**；Enter 路径才有 refocus | nav_bar_search.cpp:107-128 |
| 范围可见性 | 左带只有放大镜+小箭头字形，当前范围**点开菜单才知道**（搜索结果标题才显示范围短标签） | nav_bar_draw.cpp:117-145、app_nav.cpp:226 |
| 键位 | 地址框 F4 / Alt+D / Ctrl+L；搜索框 Ctrl+F / Ctrl+E；F6 循环含地址、搜索两站，FocusPaneOf 分开报 address_bar / search_box | keymap.gen.h:92-93、114-116、app_commands_focus.cpp:43-91、127-137 |
| 中键 | 导航栏中键只透传鼠标规则 sink_，无默认动作 | nav_bar_input.cpp:236-242 |

**已达标、不建议改**：面包屑双态语义、提交后焦点回列表、历史下拉当前行高亮、范围单源
（searchscope::Resolve）、搜索与命令同走 App::RunSearch 唯一实现、弹层与搜索框同宽锚点、
破坏性动作（清历史）带确认。

---

## 7. 可落地改进清单（17 条）

> 每条格式：现状（含出处）→ 建议行为 → 参考产品 → 优先级。
> 优先级口径：P1 = 高频/诚实性缺口，下一轮迭代；P2 = 体验补齐，排期可让位；P3 = 锦上添花或成本高需立项。
> 改动落点全部限于导航栏/搜索域文件，不触碰 shell_view 与命令总线结构。

### 1. 地址栏输入建议下拉（历史 + 子目录 + 命名空间候选，Tab 补全可视化）

- **现状**：编辑态只有系统 SHAutoComplete（文件系统目录）+ 纯 Tab 循环替换补全，候选不可见——
  用户不知道有几个候选、Tab 要按几轮（nav_bar_input.cpp:22-28、nav_bar_actions.cpp:84-126）。
- **建议行为**：编辑态输入变化时弹出与地址框同宽的自绘建议列表（复用 popup_list）：候选 =
  输入前缀匹配的路径历史 + 当前目录子目录（复用 NavModel::ChildFolders / nav::SplitForComplete
  的切分）+ 命名空间静态候选（输入"此"建议"此电脑"）；↑↓ 选择、Enter 导航选中项、Esc 关列表
  再还原原文（逐级回退）；Tab 补全改为"填入当前高亮候选"，候选数在行尾标注。
- **参考产品**：Chrome/Firefox omnibox 输入即建议 + ↑↓ 选择（Chromium Omnibox Design Document、
  Mozilla Awesome Bar 文档）；Explorer++ AddressBarView 的补全下拉；Explorer 系统自动完成下拉。
- **优先级**：P1。地址栏是每天几十次的入口，"盲替换"是它与所有参考产品的最大差距。

### 2. 地址栏环境变量展开

- **现状**：nav::FromUserText 明确不展开环境变量且注释称"留在调用方"，但 CommitEdit 并没有做
  （nav_path.h:58-74、nav_bar_actions.cpp:67-82）——输入 `%TEMP%` 回车直接失败，报错原因
  （未展开的百分号）与用户心智完全无关。
- **建议行为**：CommitEdit 在 FromUserText 清理后、ParsingPath 解析前，对含 `%VAR%` 的文本做
  ExpandEnvironmentStringsW（仅整串形如环境变量引用时展开，失败保留原文走正常报错）；
  顺带支持 `shell:` 别名可选。语义与 Explorer++（AddressBar 支持环境变量展开）、Explorer 一致。
  展开规则加测试（tests/ 现有 search_url_test.cpp 同层级的纯函数测试）。
- **参考产品**：Windows 资源管理器地址栏（原生支持 %VAR% 与 shell: 别名）；Explorer++
  （源码已验证支持展开）。
- **优先级**：P1。实现成本极低（一个纯函数 + 一处调用），失败体验却是"回车没反应"级别的。

### 3. 搜索范围降级反馈：卷根 / 未索引位置提示

- **现状**：作用域为卷根（C:\、D:\）时搜索文件夹绑得上但查不到东西（卷根不是 crawl scope，
  search_folder.h:20-21）——用户只会看到 0 结果，零解释；"0 结果"与"根本搜不了"在界面上
  无法区分。
- **建议行为**：App::RunSearch 在 Resolve 得到卷根作用域时给出与 kThisPC 同级的明确提示
  （"该卷不在 Windows 索引范围内，结果可能为空；建议改用子目录或自定义路径"，文案风格沿用
  app_nav.cpp:198-200 的整电脑报错）；中期可对该档降级为自有枚举的慢速搜索（明确标注"慢速
  模式"），不静默返回空。
- **参考产品**：Explorer 在未索引位置搜索时提示"可能较慢"并显示降级状态；Everything（不依赖
  索引服务的另一条路线，作为文案里给用户的替代建议）。
- **优先级**：P1。"诚实反馈"优先于新功能——搜不出东西还不说为什么，等于搜索坏了。

### 4. 搜索即输即筛（增量过滤，可选模式）

- **现状**：搜索词必须回车提交（CommitSearch 是唯一入口，nav_bar_search.cpp:107-118），输入
  过程零反馈；参考产品全部有输入即反馈。
- **建议行为**：分两阶段落地。第一阶段：输入防抖（~200ms）后仅对"本层"档做轻量过滤高亮
  （列出匹配项计数，不动 shell 视图）；第二阶段：提供设置开关"输入即过滤当前文件夹"
  （XYplorer Live Filter 语义：本层瞬时、Esc 还原），与现有"回车跨目录搜索"并存——
  **过滤与搜索是两个工具**，不合并语义。注意与 search.query.set 的"不提交"哲学一致：
  过滤结果不进搜索历史。
- **参考产品**：XYplorer Live Filter（帮助文档 Find Files/Live Filter 章节）；Opus Filter Bar
  （手册 Filter Bar 章节，过滤状态在状态栏可见）；Explorer 搜索框输入即搜。
- **优先级**：P1（第一阶段成本可控；第二阶段涉及列表过滤管线，单独立项）。

### 5. 地址栏修饰回车：Ctrl+Enter 新标签 / Shift+Enter 新窗口

- **现状**：编辑态只处理 Enter/Esc/Tab（nav_bar_input.cpp:35-57），Ctrl+Enter/Shift+Enter 落到
  默认处理（无效）；想在新标签打开输入的路径，只能先导航再对标签操作。Alt+Enter 已被
  "属性"占用（keymap.gen.h:113），不可复用。
- **建议行为**：CommitEdit 增加 disposition 参数（current/new tab/new window），EditProc 把
  Ctrl+Enter/Shift+Enter 映射进去；执行走既有命令总线（TabNew / FsOpenInNewWindow 同源，
  与面包屑段菜单"在新标签/新窗口打开"完全同一套命令，见 nav_bar_actions.cpp:219-226）。
  键位进 data/keymap.json（生成键位表，勿手改 keymap.gen.h）。
- **参考产品**：浏览器 Alt+Enter = 新标签打开输入（Chromium/Mozilla 文档）；Opus/TC 地址栏
  修饰键开新标签；本仓面包屑段菜单已确立"新标签/新窗口"双轨语义。
- **优先级**：P2。语义与现有命令完全同源，属于低成本补齐。

### 6. 地址栏"粘贴并前往"

- **现状**：地址框用默认 EDIT 右键菜单，无"粘贴并前往"；粘贴路径后还要再按一次回车
  （nav_bar_input.cpp:22-28 未自绘菜单）。
- **建议行为**：自绘编辑态右键菜单（在系统标准项基础上追加）：剪贴板文本经 FromUserText
  清理后是可解析路径时，追加"粘贴并前往"（粘贴+提交一步）；剪贴板非路径时该项灰显。
  判定逻辑复用 nav::FromUserText + ico::ParsingPath，不另写一套。
- **参考产品**：Chrome omnibox 的 Paste and Go（Chromium Omnibox Design Document）；
  Tablacus Explorer 地址栏加载项的"Paste and Go"（RESEARCH-OPENSOURCE §2）；Explorer++。
- **优先级**：P2。"复制路径→粘贴→回车"是文件管理器最高频三步之一，合并为一步收益明确。

### 7. 拖放路径到地址栏导航

- **现状**：导航栏未 DragAcceptFiles/RegisterDragDrop（仅书签栏有，bookmark_bar.cpp:33）——
  把文件夹从别的窗口拖到地址栏没有任何反应。
- **建议行为**：地址框接受 CF_HDROP 拖放：文件夹 → 导航到该目录；文件 → 导航到其父目录
  并选中该文件（Explorer 同语义）。命名空间项（拖自"此电脑"）走 SHParseDisplayName 解析。
  搜索框不接受拖放（避免与"拖文件设范围"的未定语义纠缠）。
- **参考产品**：Explorer 地址栏拖放导航（系统惯例）；Opus、XYplorer 地址栏均支持。
- **优先级**：P2。实现模式可直接参考书签栏 WM_DROPFILES（bookmark_bar.cpp:384）。

### 8. 地址提交失败的输入保留与反馈

- **现状**：CommitEdit 先 EndEdit(true) 再 onNavigate_（nav_bar_actions.cpp:78-81）——路径无效
  导航失败时编辑框已关闭、原文已丢，用户只能整串重输；且失败原因没有任何控件级反馈。
- **建议行为**：导航失败时（App 侧失败回调）回到编辑态并**恢复提交前的文本**（选中尾段便于
  改错字），导航栏对地址框给一次性边框错误色/气泡提示；不改成功路径的时序（先还焦点仍是
  正确的，只有失败分支需要把文本找回来）。App 需要把"失败 + 原文"回传给 NavBar（新增一个
  回调或复用 SetPath 失败分支）。
- **参考产品**：Chrome/Firefox omnibox 无效提交保留原文（Chromium Omnibox Design Document）；
  Explorer 地址栏失败给明确报错对话框。
- **优先级**：P2。影响的是出错路径，但出错时的体验决定用户是否敢用地址栏输入长路径。

### 9. 搜索框 cue 占位显示当前范围

- **现状**：cue 固定为"搜索（回车执行，Esc 取消）"（nav_bar_search.cpp:57-59），当前范围要
  点开左带菜单才知道；范围改错时用户提交完才发现搜错了地方。
- **建议行为**：cue 动态拼接范围短标签："在 <范围短标签> 中搜索（回车执行，Esc 取消）"，
  短标签复用 searchscope::RangeShortLabel（app_nav.cpp:226 已在结果标题使用同一来源）；
  范围变化时更新 cue（EM_SETCUEBANNER 重发即可）。自定义档带路径时截断显示叶子名。
- **参考产品**：Explorer 搜索框占位提示搜索位置；Files App 搜索框旁常驻范围选择器。
- **优先级**：P2。一处 EM_SETCUEBANNER 调用 + 一个拼接函数，成本接近零。

### 10. 搜索历史键盘可达（↑ 展开历史、↑↓ 选择、Enter 执行选中词）

- **现状**：历史下拉只有鼠标路径（右带点击，nav_bar_search.cpp:170-176）；SearchProc 只处理
  Enter/Esc/Tab（nav_bar_search.cpp:25-49），输入框里按 ↑ 没有任何行为。
- **建议行为**：输入框聚焦时按 ↑ 打开历史弹层（复用 ShowSearchHistoryMenu 的锚点与宽度），
  弹层内 ↑↓ 移动、Enter **执行**该词（与 search.query.set 的"回填不执行"区分：键盘路径用户
  明确按了 Enter，等于提交；AI/回填路径维持不提交哲学不变）。
- **参考产品**：TC 路径行 ↓ 展开历史（官方 wiki）；浏览器地址栏 ↑ 打开历史下拉、Enter 执行
  选中项（Mozilla Awesome Bar 行为）。
- **优先级**：P2。历史菜单已存在，只缺键盘通道；注意与输入框光标移动键（↑ 在单行 EDIT 无
  语义）不冲突。

### 11. 搜索框 Esc 二段式：清词 → 焦点回列表

- **现状**：Esc 清词并还原目录（nav_bar_search.cpp:120-128），但焦点**留在搜索框**；Enter 路径
  才有 refocus（:117）。用户搜完想用 Esc 退回列表，还得再点一下文件区。
- **建议行为**：Esc 二段式：框内有词时清词还原（保持现状、焦点不动，便于改词重搜）；框内
  已无词时按 Esc → 把焦点还给文件列表（复用 CommitSearch 的 WM_APP_REFOCUS 通道）。
  与 Explorer 搜索框 Esc 语义对齐，也与 §2 的"逐级回退"原则一致。
- **参考产品**：Explorer 搜索框（Esc 清词回原目录、焦点回列表）；Double Commander 过滤框
  Esc 逐级退出。
- **优先级**：P2。一处分支判断，纯键盘流体验收益。

### 12. 中键点击面包屑段 = 在新标签打开

- **现状**：面包屑段左键导航、右键有"在新标签打开"菜单（nav_bar_input.cpp:197-201、
  nav_bar_actions.cpp:198-241），但中键只透传鼠标规则 sink_（nav_bar_input.cpp:236-242），
  无默认动作——"中键=新标签"的通用心智在面包屑上断了。
- **建议行为**：WM_MBUTTONDOWN 命中 seg（非 ghost 段）时默认走段菜单同款"在新标签页打开"
  （segmentCmd_ + SegCmd::kSegOpenNewTab 同一出口）；鼠标规则 sink_ 仍优先（用户自定义规则
  不被默认行为覆盖，与现有侧键处理同序）。
- **参考产品**：浏览器中键链接=新标签；TC/Opus 中键路径=新标签打开；本仓标签栏已确立
  中键语义（关标签）。
- **优先级**：P3。右手鼠标用户的顺手性改进，语义与既有出口完全同源。

### 13. 非默认搜索范围的可见性（左带角标）

- **现状**：左带只有放大镜+小箭头字形（nav_bar_draw.cpp:117-145），当前范围点开菜单才知道；
  结果标题里的范围短标签（app_nav.cpp:226）要搜完才看到——"菜单纯默认时还无所谓，改过一次
  范围后每次搜索都是盲的"。
- **建议行为**：当前范围 ≠ 默认档（kSubtree）时，在左带放大镜旁叠加一个微型角标（如小型
  文字徽标"本层"/"自定义"，尺寸走 theme 令牌不引入新字号）；悬停 tooltip（复用 TooltipHost）
  显示完整范围说明。默认档不加角标，保持安静。
- **参考产品**：Opus Filter Bar 激活状态在条上可见（手册）；Files App 范围选择器常驻可见；
  本仓"不写兑现不了的选项"原则（search_scope.h 头注）同样适用于"不画看不见的状态"。
- **优先级**：P3。防"菜单显示 A、实际搜 B"类漂移的 UI 侧保险丝。

### 14. 搜索历史单条删除

- **现状**：只有整列清空（SearchHistoryClear，带破坏性确认，app_commands_search.cpp:105-113）；
  误存一条（如手滑搜了"asdf"）只能连好历史一起清。
- **建议行为**：历史下拉行 hover 显示 ✕（或右键"删除此项"）：从 Config::Get().mutableSettings()
  定向 remove 该词并 Save，走命令总线新增 search.history.remove {query}（与 search.history.clear
  同层级注册，返回 removed: bool）；菜单组件需支持行内动作（若 popup_list 无此能力，先做
  右键菜单版）。
- **参考产品**：Firefox 地址栏历史 Shift+Delete 删除单条（Mozilla 文档）；Chrome 历史下拉
  单条删除。
- **优先级**：P3。数据面是小改，成本主要在菜单交互（行内 ✕ 或右键）。

### 15. "仅本次"搜索范围（Shift+点击范围菜单）

- **现状**：范围是全局持久设置——SetSearchRange 经 config.set 落盘（app_search.cpp:44-69），
  临时想换范围搜一次，得记着改回来。
- **建议行为**：范围菜单支持 Shift+点击 = 仅本次生效（内存覆盖，不落盘；下次提交或清词后
  回弹默认档）；普通点击维持现状（持久）。菜单行文案提示"Shift+点击仅本次"；AI/命令路径
  不变（search.scope.set 仍是持久语义，另加 same_only 可选参数）。
- **参考产品**：Opus 一次性过滤/临时位置栏（手册）；浏览器 per-搜索的临时引擎切换。
- **优先级**：P3。状态机多一层"临时覆盖"，要与 searchscope::Resolve 单源原则兼容
  （临时档也必须走同一 Resolve）。

### 16. "隐藏搜索框"开关

- **现状**：搜索框常驻（nav_bar_search.cpp 头注），窄窗自动收起，但没有主动关闭的设置——
  不需要搜索的用户只能靠把 navBarSearchWidth 压到最小值间接腾地方（nav_bar_layout.cpp:57-66）。
- **建议行为**：设置新增"显示搜索框"开关（默认开）：关闭时 Layout 直接不切搜索框宽度
  （searchW 恒 0），FocusSearch/Ctrl+F 自动降级为聚焦地址框（并在命令回执说明）；设置面板、
  config.set、config_io 三处同步（沿用 navBarSearchWidth 的既有管线，config_apply.cpp 同款）。
- **参考产品**：Files App 设置可关搜索框（files-community 设置页）；TC/Opus 工具栏元素均可
  自定义显隐。
- **优先级**：P3。尊重"搜索不是每个人的高频动作"，与常驻哲学不冲突（默认仍常驻）。

### 17. 整机搜索的替代实现（长期）

- **现状**：kThisPC 档灰字"（本宿主暂不可用）"+ 命令路径直接拒绝（app_search.cpp:81-87、
  search_scope.h:14-21）——整机搜索要 Explorer 进程内的 search-ms 宿主，本宿主没有。
- **建议行为**：中期路线二选一：(a) 自建 Windows Search 查询（searchapi.h 的
  ISearchQueryHelper/目录 OLE DB，需先补编译器支持，search_folder.h:26 已注明缺失）；
  (b) 混合虚拟文件夹（多作用域聚合）。更长期的第三条路是 Everything 式 NTFS MFT 直读
  （自有引擎，不依赖索引服务，成本最高）。落地前维持现状（灰字+拒绝）不动——**不写兑现
  不了的选项**是本仓既定原则。
- **参考产品**：Explorer 全局 search-ms 查询；Everything（voidtools，NTFS MFT 直读文档）；
  Everything 亦可作为外部集成建议（命令行 es.exe 调用）作为过渡方案。
- **优先级**：P3。成本高需立项；过渡期可先做"检测本机装有 Everything 时提供跳转入口"
  的小步版本。

---

## 附：清单速览

| # | 改进 | 优先级 |
|---|---|---|
| 1 | 地址栏输入建议下拉（历史+子目录+命名空间，Tab 可视化） | P1 |
| 2 | 地址栏环境变量展开 | P1 |
| 3 | 搜索范围降级反馈（卷根/未索引提示） | P1 |
| 4 | 搜索即输即筛（增量过滤，两阶段） | P1 |
| 5 | 地址栏修饰回车（Ctrl+Enter 新标签 / Shift+Enter 新窗口） | P2 |
| 6 | 地址栏"粘贴并前往" | P2 |
| 7 | 拖放路径到地址栏导航 | P2 |
| 8 | 地址提交失败的输入保留与反馈 | P2 |
| 9 | 搜索框 cue 占位显示当前范围 | P2 |
| 10 | 搜索历史键盘可达（↑↓ + Enter 执行） | P2 |
| 11 | 搜索框 Esc 二段式（清词 → 焦点回列表） | P2 |
| 12 | 中键点面包屑段 = 新标签打开 | P3 |
| 13 | 非默认搜索范围可见性（左带角标） | P3 |
| 14 | 搜索历史单条删除 | P3 |
| 15 | "仅本次"搜索范围（Shift+点击） | P3 |
| 16 | "隐藏搜索框"开关 | P3 |
| 17 | 整机搜索替代实现（长期） | P3 |
