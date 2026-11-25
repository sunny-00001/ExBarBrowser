---
status: current
baseline: 2026-11-24
method: 实跑核对——CHANGELOG 修复记录 + tools/check_commands.py + src 现状 grep + agentctl tools(173 条)；
        与 docs/COMPETITIVE-GAP-2026-11-21.md 的原始 E/M 编号逐条对账
---

# 缺口优先级与真实状态（2026-11-24 复核）

> `COMPETITIVE-GAP-2026-11-21.md` 列了 E1–E8 / M1–M8，但那是**11-21 快照**。
> 此后多轮提交已修掉大半。本文逐条核对"现在到底还缺什么"，供「按优先级依次改进」排期。
>
> **口径**：以 CHANGELOG 修复条目 + 实跑（命令表 / 静态检查 / 源码 grep）为准，
> 不采信 gap 文档的旧状态标注。数字是快照。

---

## 1. 结论：原始清单的状态重排

| 编号 | 原优先级 | 原缺口 | 复核状态 | 依据 |
|---|---|---|---|---|
| E1 | — | 编译不过 | **已修** | gap 文档自标 |
| E2 | P0 | 导航失败弹模态框 → AI 10s 超时 | **已修** | CHANGELOG「P0：模态框开着仍派发 AI 命令」→ `modal::Depth()/Scope` 拦重入 |
| E3 | P0 | 连续反斜杠解析失败 | **已修** | `src/path_util.cpp` `ToShellPath` 折叠连续分隔符、保 UNC 前缀 |
| E4 | P0 | AI 拒绝不写审计 | **已修** | CHANGELOG P0-6 审计结构化 JSONL，被拒调用也留痕 |
| E5 | P0 | `agentctl call` 丢弃 errorCode | **已修** | CHANGELOG「结构化 errorCode 10 类」+ `gap` 直接读产品 errorCode |
| E6 | — | 键位冲突无人拦 | **已修** | `tools/check_commands.py` `find_chord_conflicts`（三处冲突源全覆盖） |
| E7 | — | 17 文件超 500 行无检查 | **已修** | `check_components.py` FILE_LENGTH_MAX + 棘轮基线 |
| E8 | P0 | 文档数字过期 | **部分** | README 139 工具 vs 实跑 173；本次已订正索引，但正文数字仍是快照 |
| M1 | P1 | 无标准 MCP 端点 | **已修** | CHANGELOG「MCP over stdio：`--mcp-stdio`」+ `src/mcp_stdio.*` |
| M2 | P2 | 撤销不可用 | **已修** | `src/file_undo.*` 自建逆操作栈（上限 20，人机边界），`app_commands_files.cpp` 记 `NoteFileOp` |
| M3 | P1 | 长任务无进度/取消 | **已修** | CHANGELOG「M3：五条长任务命令作业化」工作线程 + job 命令族 |
| M4 | P2 | 显示偏好配置缺一大块 | **真缺** | 字体/字号/网格间距/图标大小仍无（但工作区有 `settings_dialog.*` 正在做，见 §3 阻断） |
| M5 | P3 | 归档/远程/剪贴板历史全无 | **真缺** | 需引第三方或自研，违反零依赖原则，先论证 |
| M6 | P2 | 命令行 `/select` 不选中文件 | **已修（本轮）** | `main.cpp` 解析选中目标 → `App::Run` → 复用 pendingSel 机制；转发路 select 作变长尾随数据，`CopyDataPayload` 零改动 |
| M7 | P2 | 目录变更无自动监听 | **已修（本轮）** | 复用 NavPane 的 `SHChangeNotifyRegister`，文件级事件回投 `WM_APP_VIEW_AUTO_REFRESH`，App 侧 300ms 去抖刷活动视图 |
| M8 | P3 | Shell 集成入口为零 | **真缺（可选）** | 与"不注入"不冲突（注册 ≠ 注入），属增量 |

---

## 2. 建议的改进顺序（只列真缺项）

| 序 | 项 | 理由 | 前置 |
|---|---|---|---|
| ~~1~~ | ~~M6~~ `/select` 选中语义 | **本轮已完成** | — |
| ~~2~~ | ~~M7~~ 文件视图自动刷新 | **本轮已完成** | — |
| 3 | **M4** 显示偏好 | 工作区已有人在做（settings 系列文件），**不要抢** | 等并行会话结束 |
| 4 | **M8** Shell 集成 | 可选增量，写 `HKCU`，不破坏定位 | 先论证 |
| 5 | **M5** 归档/远程 | 违反零依赖原则，投入产出比最低 | 先论证是否引依赖 |

---

## 3. 当前阻断（为什么本轮未动源码）

- **并行会话正在改源码**：复核时 `src/*.cpp` 最近修改时间为 20:01–20:06（当前 20:06），
  涉及 `nav_bar.cpp` / `nav_pane.cpp` / `app.cpp` / `app_settings.cpp` / `tab_strip.cpp` 等 8+ 文件，
  均非本会话改动。按 `AGENTS.md §8.4`：此时改代码会把对方半成品编进验证结论，
  并与对方抢改同一批文件 —— **报告并让出**。
- **评估面 fail-closed**：`check_components.py` 因并行会话改了 `tests/` / `tools/` 且未重签
  `tests/baselines/eval-surface.json` 而拒绝放行；此时任何"测试通过"都不具信任基础。
- **处理方式**：等并行会话收工（`git status` 稳定 + 评估面重签）后，从 §2 序号 1 开始逐项改，
  每改一项跑 `check_commands` / `check_ai_meta` / `check_components` + `tests/run_all.sh`，
  并留 `CHANGELOG.md` `[Unreleased]` 条目。

---

## 4. 与 Everything 的关系

Everything 集成（`search.everything` 命令位 `implemented=false`）**不在本轮优先级内**——
用户已明确"先不管 Everything"。其显示方案的约束（海量结果会使 shell 视图 / NSE 虚拟文件夹卡死，
必须走"只做查询后端 + 定位"或"自建 `LVS_OWNERDATA` 虚拟列表"）记录在
`docs/REFERENCE-ASSETS-MAP.md` §5 与本节之前的工作结论中，待排期时再决。
