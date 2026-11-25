---
status: current
baseline: 2026-11-24
method: 本仓现状取自实跑（agentctl tools 173 条、commands_table*.cpp 的 implemented 位、README §2.2、三份 gap 文档）；
        下载素材侧逐项读 README / 说明文件 / 顶层结构，不采信宣传口径
---

# 下载素材 → ExBarBrowser 能力映射

> **回答一个问题**：下载目录里这一堆工具和源码，哪几份能直接补本仓缺口，
> 哪几份只是看看思路，哪几份与文件浏览器这条线根本无关。
>
> **本文与其它差距文档的分工**：`EXPLORER-GAP-AUDIT.md` 对 Windows 资源管理器，
> `COMPETITIVE-GAP-2026-11-21.md` 对同类第三方产品，`DOPUS-PARITY.md` / `PARITY.md` 对
> Directory Opus / ExBar mod。本文**不对标产品**，而是给"手上已有的下载素材"排优先级 ——
> 每条素材都落到本仓具体的命令位 / 模块 / 缺口上。
>
> **口径声明**：本仓"已实现 / 未实现"以 `tools/list` 与 `commands_table*.cpp` 为准，
> 不采信 `README.md` §2.2 的旧清单（已被实跑证伪两处，见 §3.1）。数字是快照，
> 加命令就变；须重新量时看 §6 的口径命令。

---

## 1. 图例

| 标记 | 含义 |
|---|---|
| **可直接接** | 本仓已有命令位或明确缺口，素材能直接补上（代码 / 数据 / 协议） |
| **抄思路** | 路线不同，但机制 / 避坑 / 交互可借鉴，不搬代码 |
| **看得见** | 只有成品可观测（界面 / 交互），无源码或商业软件 |
| **无关** | 与文件浏览器这条线无关 |

---

## 2. 第一梯队：直接对标（优先读）

| 素材 | 定位 | 映射到 ExBarBrowser | 判定 |
|---|---|---|---|
| **TabEx-main** | Python 多标签浏览器（Clover 式） | 功能清单≈本仓 `tab.*`(35) + `session.*`(8) + `bmk.*`(23) + `search.*`(6) + `file.select.*` + `tool.*`。其"自动监听 Explorer 新窗口→嵌标签"对应本仓激活优先 + `WM_COPYDATA`（magic `0xEBB1`，见 `identity.h`）；"鼠标手势""左右分屏""TortoiseGit 集成"本仓无对应命令 | **抄思路**（功能 checklist） |
| **RustExplorer-0.8.9** | Rust+GPUI 原生，系统右键菜单 / 缩略图 / 隔离 helper | 与本仓同走 Shell：`shell_view.cpp` 的 `IExplorerBrowser`、`README §1` 的"视图由 shell 提供"；其 `Everything64.dll` 集成正好补本仓 `CmdId::SearchEverything`（`commands_table_tools.cpp` 中 `implemented=false`）；`KNOWN_ISSUES.md` 对标本仓 `COMPETITIVE-GAP` M2 / M3 | **可直接接**（Everything SDK）+ **抄思路**（避坑） |
| **ExplorerTabUtility-master** | .NET，强制新 Explorer 窗口转标签 | 其"路径重复切已有标签 / `Show in folder` 选中 / 附着分离"对应本仓 `tab.new` / `fs.open.tab` / 命令行 `/select,<文件>`（`main.cpp` 的 `InitialPath()` 只打开父目录不选中，见 `COMPETITIVE-GAP` M6） | **抄思路**（M6 选中语义） |
| **OpenWindows-master** | Shell namespace extension 的 COM 范例 | 本仓不注入 explorer、不注册 NSE（`README §1`），代码不兼容；但 `nav_model` / `shell_view` 的 PIDL 操作正是它演示的 COM 面 | **抄思路**（COM / PIDL 心智模型） |
| **Files-main** | C#/WinUI3 全自绘 | 路线相反（本仓 GDI chrome + 托管 Shell 视图，见 `DECISIONS.md` ADR）；其 `AGENTS.md` 的 interop 规则可佐证本仓 `ARCHITECTURE-RULES.md` R21 与 D14 的 WebView2 边界 | **抄思路**（工程约束写法） |
| **qttabbar-master** | C++/MinHook 注入 Explorer 的标签增强 | 本仓明确不注入（`check_components.py` 的 HOOK_GUARD 只允许 `kb_hook.cpp` 一处 `WH_KEYBOARD_LL`）；标签逻辑看 `app_tabs.cpp` / `tab_set.cpp`，不要搬它的 hook | **抄思路**（仅限交互） |

---

## 3. 第二梯队：可复用组件 / 设计

| 素材 | 定位 | 映射到 ExBarBrowser | 判定 |
|---|---|---|---|
| **Everything-SDK-3.0.0.9** | `dll` / `include` / `lib` / `example` / `src` / `vs2019` 全套 | 直接补 `search.everything`（现有命令位未实现，`cmd_naming.cpp` 已登记 `SearchEverything` 双向映射）；`README §2.2` 与 `COMPETITIVE-GAP §4` 都点名"Everything 集成❌"。约束：按 `AGENTS.md §1.2`，Everything 走外部进程 + IPC（`evipc` 路线），不进链接期依赖，与 D14 的 WebView2 同类论证 | **可直接接**（最高优先） |
| **ZSDesk-0.4.3** | 自带搜索（共享内存索引 + USN 日志 + MFT 快照）+ 可选 Everything 后端 | 双后端设计正是 `search.find`（原生 `search-ms` PIDL，见 `shell_view_search.cpp`）与 `search.everything` 的分工范本；"默认自带、可选复用已运行 Everything"可抄进 `search.scope.*`。反例：其 `Alt+P` 预览在本仓已是 `view.toggle.preview`（已实现） | **抄思路**（双后端策略） |
| **Windhawk.zip / windhawk-main / windhawk-mods-main** | 编译器来源 + mods 仓库 | 编译器 `C:\Program Files\Windhawk\Compiler\bin\clang++.exe` 已是本仓工具链（`README §3.1`）；mods 里 Explorer 定制仅作 `kb_hook.cpp` 唯一豁免的背景，勿扩钩子 | **抄思路** |
| **thumbfast-master + mpvWrapper.dll / editorWrapper.dll / DirectoryOpus-CSV-viewer-plugin.dll** | 缩略图脚本 + 三个第三方包装 DLL | 本仓缩略图由 Shell 提供（`README §1`），不自建；`icon_resolve.cpp` 注释已明确移除 OpenList / Everything 专属品牌图标分支。这三个 DLL 只说明"单 exe 如何挂外部能力" | **无关 / 抄思路** |
| **OpenSync 最终架构设计文档.docx** | Windows CFAPI 事件驱动、无轮询的网盘同步架构 | 本仓 `COMPETITIVE-GAP` M5："FTP / SFTP / WebDAV 零命中"；`ZSDesk` 也是 SMB/UNC 走系统、SFTP 需 WinFsp。这份文档对应 M5 的架构层参考，但落地违反 §1.2 零依赖原则，需先论证 | **抄思路**（仅决策时） |
| **filemanager-portable.exe / ExploreBetter / Folderwing / XYplorer / doublecmd** | 成品二进制 | 无源码可搬；交互上 `Folderwing` 用 WebView2（对应 D14 的 `ai.panel.web`）、`XYplorer` / `doublecmd` 双栏（本仓无 `dual` / `pane` 命令） | **看得见** |

### 3.1 两处 README 已过期的判定（实跑证伪）

- `view.toggle.preview`：`commands_table.cpp` 中 `implemented=true`、`aiExposed=true`，已实现。
- 会话管理：`session.named.save` / `list` / `restore` / `delete` 均在 `tools/list` 中，已实现。

`README.md` §2.2 仍把这两项列在"未实现"，本文以实跑为准，未计入缺口。

---

## 4. 无参考价值（含易误判项）

| 素材 | 为什么无关 |
|---|---|
| `HEU_KMS_Activator_63.3.4` / 各类 `Setup.exe` / `RunningCheeseFirefox.7z` / `QQPCDownload*.exe` | 激活工具与安装包，非文件浏览器技术面 |
| `agent2api-*` / `workbuddy2api-*` / `qoder2api-*` / `trae2api*` / `credits.rs` / `official_usage.rs` / `usage_history.rs` / `*_accounts*.json` / `workbuddy-accounts-*.json` | API 中转与积分 / 账号体系（`credits.rs` 头注释自证是 WorkBuddy 积分域），与 Shell 视图无关 |
| `settings.json`（下载根那份） | **不是本仓的** —— 本仓偏好在 `<exe>\data\settings.json`，高频状态在 `<exe>\state\*.json`（会话/最近访问/标签/窗口几何）；这份含 `language` / `accent` / `favorites` / `sidebar_width`，属别的面板项目 |
| `exbar-session-1795078005875.txt` | 内容是"ExBar 助手 - 会话导出（空会话）"，**不是本仓格式** —— 本仓会话走 `<exe>\state` 与 `session.named.*` 命令，无此文件名 / 文案 |
| `_probe_db.py` / `dav自动映射.txt` / `maintenance-mode.bat` / `portable.flag` / 滴滴发票 PDF | 一次性探针、WebDAV 备忘、便携标记、生活文件 |
| `go/` / `Everything/`（1.5.0.1423b.x64 独立包，非 SDK） / `nextcloud/` / `rclone-v1.75.0` / `cyberduck-master` / `EasySFTP-main` | 语言工具链 / 成品客户端 / 远程协议实现；对应 M5 远程缺口但均需引第三方，非即时参考 |

---

## 5. 按命令缺口反向索引（素材 → 该补哪条）

| 本仓缺口（实跑口径） | 对应命令位 / 位置 | 最该看的素材 |
|---|---|---|
| Everything 搜索未实现 | `CmdId::SearchEverything` → `search.everything`（`implemented=false`、`aiExposed=false`）；`cmd_naming.cpp` 已登记 | **Everything-SDK-3.0.0.9** + RustExplorer 的 `Everything64.dll` 集成 + ZSDesk 双后端 |
| 选中文件不落 `/select` | `main.cpp` 的 `InitialPath()`（`COMPETITIVE-GAP` M6） | ExplorerTabUtility 的 `Show in folder` 处理 |
| 撤销不可用 | `file.undo` 已暴露但背景菜单无 undo 谓词（M2） | RustExplorer 的 `KNOWN_ISSUES.md`（同款 Shell 撤销困境） |
| 长任务无进度 / 取消 | `tool.dupfinder` / `tool.hash` / `tool.compare` / `tool.diskusage`（M3） | RustExplorer 的"后台队列 + 隔离 helper + 超时回退" |
| 双栏 / 分屏 | 无命令位 | TabEx 的 F3 左右对等标签组 |
| Everything 品牌图标 | `icon_resolve.cpp` 已主动移除该分支 | RustExplorer 的 `FluentUI-System-Icons-SOURCES.json`（若要恢复） |

---

## 6. 落地建议（顺序）

1. **先接 Everything**：`Everything-SDK` 的 `example/` + `include/` 就能让 `search.everything` 从
   `implemented=false` 变真；走 `evipc` 外部进程路线，不违反零依赖链接期约束。
2. **再抄 TabEx 功能清单**做一次自检：它的每一条勾掉 `tools/list` 有没有，缺的记进
   `docs/PARITY.md`。
3. **翻 RustExplorer `KNOWN_ISSUES.md`**：把它的 Shell 菜单 / 拖放坑与本仓
   `COMPETITIVE-GAP §1` 的 E1–E8 合并成一份避坑清单。

---

## 7. 口径命令（数字过期时重量）

```bash
python tools/agentctl.py tools | python -c "import sys,json;d=json.load(sys.stdin);print(len(d.get('tools',d)))"  # AI 暴露工具数（本文基线 173）
python tools/check_commands.py      # 首行给命令目录 / 已实现 / 处理器总数
grep -c 'CmdId::' src/commands_table*.cpp | awk -F: '{s+=$2} END{print s}'  # 命令位计数（含未实现）
grep -n 'SearchEverything' src/commands_table_tools.cpp   # search.everything 的 implemented 位
```

> 三个数不同口径、不可混用：`tools/list` 是**对 AI 暴露**的条数（173），
> `check_commands.py` 是**命令目录**总数（含未暴露 / 未实现），两者都随加命令而变。
