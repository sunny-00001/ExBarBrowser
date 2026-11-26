---
status: current
baseline: 2026-11-26
method: 6 个子代理并行（3 域代码审查 + 3 路全网调研）+ 本地静态检查交叉验证；关键结论逐条实锤复核
related:
  - docs/REVIEW-UI-2026-11-26.md
  - docs/REVIEW-SHELL-2026-11-26.md
  - docs/REVIEW-COMMANDS-2026-11-26.md
  - docs/RESEARCH-NAVBAR-2026-11-26.md
  - docs/RESEARCH-SEARCHBOX-2026-11-26.md
  - docs/RESEARCH-OPENSOURCE-2026-11-26.md
---

# 综合审查与对标调研汇总（2026-11-26）

> 六份分报告已归档 docs/。本文只做三件事：**合并后的修复优先级**、**跨域共性结论**、
> **调研可借鉴清单的收敛**。细节、代码片段、行号一律以分报告为准。

---

## 1. 快照与可信度说明（必读）

- 审查期间工作区被**另一个会话并行重构**（书签栏拖拽/滚动修复、shell_view 拆分 1176→5 文件等）。
  Shell 域报告基于 2026-11-23~26 混合快照并对拆分前后两版都做了核对；UI 域与命令域报告
  的关键声明已由主控逐条实锤（`interrupted_` 无复位、`timeoutMs=90000` vs 配置 10000、
  `reject(r.errorCode, r.errorCode)` 5 处、豁免表按 CmdId 枚举匹配等），与当前磁盘一致。
- 消息通道中曾出现一版命令域"7 条发现"（豁免表大小写绕过、mcp_stdio 缓冲截断等），
  **与当前代码不符，已弃用**——以落盘的 REVIEW-COMMANDS 为准。
- 本地静态检查：`check_commands` / `check_ai_meta` 全部通过；`check_components` 无超棘轮
  （shell_ops 1078 / shell_input 522 在记账基线内，另有 12 个超限文件同样记在基线里）。
  **全量单测已有结论**：`bash tests/run_all.sh` 在**独立输出目录**（`EXBAR_TEST_OUT`）
  下实跑 —— **通过 73 / 无法运行 0 / 运行失败 0 / 编译失败 0**（2026-11-26 22:19）。
  此前"两次运行均被环境中断"的原因是**两个会话同时跑全量、共用同一个 `$OUT`**：
  两边写同一批 `*.build.log` / `*.run.log`，日志被互相覆盖，于是"编译失败"的条目
  实际带着**通过**的 run.log（`通过 18 / 失败 0`）。**结论：并发跑全量要各设
  `EXBAR_TEST_OUT`**，否则日志互踩、结论不可信。

## 2. 修复优先级（跨域合并，按"数据安全 > 行为正确 > 静默失效 > 质量"排序）

### 第一梯队：数据安全 / 假成功（Shell 域，必须先修）

| # | 位置 | 问题 | 一句话机理 |
|---|---|---|---|
| S1 | shell_ops.cpp:313-318 | **删除的排队失败被记成成功** | `DeleteItem` HRESULT 丢弃；全部排队失败时 `Run()` 仍返回 true，AI/用户拿到"删除成功"、文件还在 |
| S2 | shell_ops.cpp:416 | **新建文件夹同型假成功** | `NewItem` 未判 HRESULT（对照 ：468 NewTextFile 有判） |
| S3 | shell_ops.cpp:311 等十余处 | **SetOperationFlags 全项目未判** | 失败则删除绕过回收站且不提示——全组唯一静默丢数据路径 |

这三条直接决定"AI 通道报的 ok 是不是真的"，修法都已在 REVIEW-SHELL §2 给出（累计排队数、
一票失败即 false、删除两处必须判 flags）。

### 第二梯队：功能级 P1（三域各一条）

| # | 域 | 位置 | 问题 |
|---|---|---|---|
| P1-1 | 命令/AI | ai_queue.cpp:65 + ai_server.cpp:389 | **`interrupted_` 粘滞**：AI 通道停用再启用后所有请求立即超时，通道死到重启进程。`Queue::Interrupt()` 只置位无复位，`StartServer` 不调 Reset。修法：加 `Queue::Reset()`（锁内置 false）+ StartServer 启动线程前调用 |
| P1-2 | 命令/AI | ai_api.cpp:183/219 vs config.h:281 | **tools/list 声明 90s、管道实际 10s**，契约与现实差 9 倍，客户端据此做出错误的"该不该重试"决策。修法：`ListTools/FindTool` 从 `aiTimeoutMs` 取值 |
| P1-U | UI | status_zoom_slider.cpp | **缺 TrackMouseEvent**：`hot_` 悬停态永久残留（WM_MOUSELEAVE 永远不来） |

### 第三梯队：行为错误 / 静默失效（高/中）

| # | 域 | 问题 |
|---|---|---|
| H1 | Shell | 陈旧排队导航把视图拉回去：B 导航成功后不清 `queued_`/不 KillTimer，`ReplayQueued` 把视图带回 A（"点 B 打开又自己跳回 A"） |
| H2 | Shell | 主题重建失败后窗口永久空白：容器已毁、`ApplyTheme` 忽略返回值、无任何出路 |
| H3 | Shell | `shellinput::Install` 三处 Subclass 返回值全丢：Win11 层级变化时鼠标规则全部静默退化 |
| P2-1 | 命令/AI | mcp_stdio 桥接独占单实例管道：MCP 客户端接入后所有本地脚本直连报 PIPE_BUSY，文档未披露 |
| P2-2 | 命令/AI | `reject(r.errorCode, r.errorCode)` 5 处：拒绝审计的 reason 位落机器码，破坏"errorCode 机器读 / reason 人读"双轨约定（一行修复） |
| P2-3 | 命令 | `Register`/`RegisterWithResult` 互斥检查只做一个方向，反向静默遮蔽 |
| P2-U | UI | modal/popup_list `int gm` 未初始化读取；WM_APP_STATUS_MENU 死分支；WM_APP_TAB_CLOSE/MOVED 下标寻址竞态；RefreshStats 用已 Release 的 COM 指针做身份比较 |

### 第四梯队：质量与欠账（择机清）

- Shell 中档 9 条（M1-M9）：`NewTextFile` 回退循环重算本地化名（与自身注释矛盾）、`Undo`
  注释与 `SW_SHOWNORMAL` 冲突、列 pid 溢出恒假判断、死代码 `shellcolumns::Toggle` /
  `ItemCount` / `SetNavPaneVisible`、`CaptureSelectionPidls` 清成员而非参数、
  `ApplyPreviewPaneState` 名不副实、UIA 不可用无兜底、`g_contexts` 生命周期单点。
- Shell 低档 15 条（L1-L15）、命令域 P3 五条（令牌兜底可预测、g_pipe 无锁竞态、legacy
  热路径重复解析、订阅无 gap 标记、旧格式超时缺 errorCode）。
- 超 500 行：app.cpp 956、app_tabs.cpp 574（shell_ops 1365 在记账基线内但已是最大件）。

## 3. 跨域共性结论（三条）

1. **"批量操作的部分/排队失败"是全项目最大的正确性空洞**：`shellopexec::Run()` 的双判据
   （HRESULT + GetAnyOperationsAborted）只覆盖执行阶段，排队阶段（DeleteItem/NewItem/
   SetOperationFlags）的 HRESULT 无人看——S1/S2/S3 同根。修完这一层，AI 通道的 ok 才可信。
2. **"静默失效"模式反复出现**：Install 失败无判据（H3）、主题重建失败无出路（H2）、
   AI 停启后队列粘滞（P1-1）、拒绝审计丢 reason（P2-2）——共同点是"失败路径没有出口，
   也没有任何机器可读的告警"。建议把"失败必须可观测（日志+返回值+审计至少其一）"
   列为修复验收标准。
3. **注释与实现漂移**（M1/M2/M7 + 弃用的 7 条污染结论）恰好都落在写着"不要这样写"的
   位置旁边——印证开发流程硬规则：判据要跟着实现搬家，注释声明要随代码一起改。

## 4. 调研收敛：三个视角的交集（58 条清单的去重精华）

三份调研报告（导航栏 27 条 / 搜索框 17 条 / 开源实现 14 条）交叉后，落到本项目的
**高价值交集**如下（完整清单与产品出处见分报告）：

### 4.1 书签域（Explorer++ 是最佳参照）
- **数据模型 + 变更信号 + 多观察者 UI** 三层结构（B1）：拆 bookmark_bar.cpp（1073 行）时
  按"数据层/观察者"切而非按 UI 控件切；写操作继续走命令总线，信号只驱动 UI 刷新。
- 三个永久书签根（工具栏/菜单/其他）+ GUID 稳定标识（B2）：命令以稳定 ID 而非索引寻址。
- 拖放落点抽共享基类（B3）：书签栏与未来面包屑复用同一落点判定。

### 4.2 地址栏/面包屑（KUrlNavigator 是行为规格教科书）
- 双模式地址栏：面包屑 ↔ 可编辑，提交按修饰符分流（就地/新标签/新窗口），
  Escape 还原原文（B5）。Files Omnibar 的 40+ 缺陷清单是反面前车：每加一种模式
  成对补 Esc/Enter/焦点/IME 测试。
- 溢出折叠规则：**根节点常驻**、中段折叠进 `...`、padding 保留触控宽度（B4，
  Files #17766 的教训）。按钮差量更新 + 子目录 300ms 延迟枚举 + 远程协议黑名单（B6）。
- 相对路径先 `KIO::stat` 式探测再拼接；修饰键点击语义矩阵（中键=新标签等）。

### 4.3 搜索框（omnibox 范式，落地清单见 RESEARCH-SEARCHBOX §清单）
- 输即建议 + 防抖 + 键盘/鼠标选择一致 + 匹配子串高亮 + 历史去重频次排序；
- 范围语法 token（`kind:`、路径前缀）与回车主操作/副操作（Cmd+Enter 打开所在文件夹）；
- IME 组合态陷阱（WAI-ARIA combobox 规范）：composition 期间不要抢 suggestion 导航键。
- `SHAutoComplete(SHACF_FILESYS_DIRS)` 可作自研补全的系统兜底层（B11）。

### 4.4 命令/导航历史
- **历史按标签隔离**（TC 全局历史是 16 年用户投诉点）——核查本项目 nav.history.* 的
  归属粒度；历史切换携带视图状态（滚动/选中，B7，KUrlNavigator saveLocationState）。
- 命令对象带 enabled 下沉（B8，muCommander）：菜单/工具栏/面板/AI 共享同一可用态判定，
  总线保持无状态。
- 外部命令（若引入）必须声明 collect_output 与 call_each/call_once 语义（B9，Krusader），
  占位符替换防注入。
- 偏好（静态）与快照（动态会话）分离持久化（B10，muCommander 双文件）。

### 4.5 自绘 chrome 的系统消息清单（B11，Explorer++ 深色崩溃是前车）
所有自绘控件核对三件事：`WM_UPDATEUISTATE`（焦点框/助记符）、顶层向子控件广播
`WM_SYSCOLORCHANGE`（DefWindowProc 不代发）、`WM_THEMECHANGED` 重取主题资源。

## 5. 建议的下一步（供决策）

1. **立即修**：S1/S2/S3（假成功三件套）→ P1-1（三行修复 + "停启"回归用例）→
   P2-2/P3-5（各一行）。
2. **本周**：P1-2 契约对齐、H1 排队导航作废、H3 Install 判据、P1-U TrackMouseEvent。
3. **立项**：H2 主题重建出路、P2-1 管道多实例化、书签域 B1 三层拆分、
   双模式地址栏（B5，先做交互矩阵测试设计再动手）。
4. **流程**：补跑全量单测；把本轮"批量排队失败"检查加入 check_ai_meta 的断言维度
   （AI 报 ok 必须有真实落盘/执行证据）。

## 6. 证据与产物索引

| 文件 | 内容 | 规模 |
|---|---|---|
| docs/REVIEW-UI-2026-11-26.md | 窗口/UI 绘制域审查（P0 无、P1×1、P2×4、超行×2） | 46 KB |
| docs/REVIEW-SHELL-2026-11-26.md | Shell 交互域审查（严重 3 / 高 3 / 中 9 / 低 15，穷举 20 文件） | 22 KB |
| docs/REVIEW-COMMANDS-2026-11-26.md | 命令总线与 AI 通道审查（P1×2、P2×3、P3×5） | 17 KB |
| docs/RESEARCH-NAVBAR-2026-11-26.md | 15 产品导航栏对标 + 27 条可借鉴清单 | 40 KB |
| docs/RESEARCH-SEARCHBOX-2026-11-26.md | 地址栏/搜索框交互范式 + 17 条落地清单 | 31 KB |
| docs/RESEARCH-OPENSOURCE-2026-11-26.md | 8 个开源项目源码级调研 + 11 条实现借鉴 | 28 KB |
