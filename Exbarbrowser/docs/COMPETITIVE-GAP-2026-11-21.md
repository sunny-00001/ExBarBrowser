---
status: current
baseline: 2026-11-21 22:50
method: 命令表逐条解析 + 命名管道实测 + 静态检查实跑 + 单测实跑 + 同赛道公开文档对标
---

# 全方位对标与缺口报告：ExBarBrowser vs 同类第三方资源管理器

> 回答两个问题：**本项目现在有哪些真错误**，以及**与同类产品比还缺什么**。
>
> **口径声明**：本文全部结论来自本仓代码与**实跑实测**（命名管道真实调用、日志取证），
> 不采信文档宣称。对标侧依据各产品**官方文档/官网/发布说明**（docs.dopus.com、ghisler.com、
> xyplorer.com、q-dir.com、files.community、voidtools.com、winscp.net、peazip.github.io、
> windirstat.net、learn.microsoft.com）。
> 凡与本仓文档冲突，**以实测为准**，并在文中标出冲突。

---

## 0. 一句话结论

**结构化操作面已超过绝大多数同类产品；桌面专业度与生态接入是短板。**

- 领先项：命令总线（唯一动作入口）、JSON Schema、前置条件声明、审计、事件订阅、
  多实例隔离、原子配置写入、零警告构建、44 个单测全绿 —— 这些在同类 Windows
  文件管理器里基本没有对等物。
- 短板项：① **只有命名管道，没有标准 MCP 端点**（AI 生态接不上）；
  ② **撤销在 IExplorerBrowser 里实际不可用**；③ **长任务在 UI 线程同步跑，无进度无取消**；
  ④ 显示偏好（字体/间距/图标大小）配置缺一大块；⑤ 归档/远程/剪贴板历史全无。

---

## 1. 真错误（8 条，全部已复现或已定位）

### E1〔已修〕工作区编译不过：`std::wstring` 当指针用
- **位置**：`src/ai_api.cpp` `CallImpl`
- **现象**：未提交改动把 `spec.assertion`（`ToolSpec::assertion` 是 `std::wstring`）
  写成 `if (spec.assertion && *spec.assertion)` + `str::FromUtf8(spec.assertion)`。
- **实测**：`bash tests/run_all.sh` → `indirection requires pointer operand ('std::wstring')`。
- **已修**：改为 `if (!spec.assertion.empty())` + `const std::wstring expr = spec.assertion;`
- **复验**：单测 44/44（`clipboard_test` 非交互环境退出码 77，不计失败）、
  `bash build.sh --out` 零警告、三个静态检查全绿。

### E2〔严重〕导航失败弹模态框 → AI 调用必然 10 秒超时
- **位置**：`src/app_nav.cpp` `App::NavigateActive`（`MessageBoxW` 于两处）
- **机制**：AI 命令在 **UI 线程**执行（`ai::PumpRequests` ← `WM_TIMER` 100ms，`src/app.cpp`）。
  模态框进消息循环 → 管道读不到响应 → 10 秒超时。
- **实测**：`fs.open {"path":"C://Windows"}` → `error: timeout`（10.5s）。
- **规模**：全仓 `MessageBoxW` **43 处**，仅 3 处用了 `CommandBus::fromMachine()` 分流，
  且与 MessageBox 无关。**机制现成但未系统应用**。
- **修法**：统一走 `CommandBus::Get().fromMachine()` 分流 —— 机器侧改 `NoteFailure` 返回，
  人侧保留弹窗。`fromMachine()` 已存在（`src/commands.h`），零新增概念。

### E3〔严重〕连续反斜杠导致解析失败
- **位置**：`src/shell_view.cpp` `ShellView::DoNavigate`
- **现象**：正斜杠→反斜杠归一化只替换不合并，`C://Windows` → `C:\\Windows`（双反斜杠）
  → `SHCreateItemFromParsingName` 返回 `0x80070057`（E_INVALIDARG）。
- **实测对照**：

  | 输入 | 结果 |
  |---|---|
  | `C:\Windows` | ok=true |
  | `C:/Windows` | ok=true |
  | `C://Windows` | **timeout**（双反斜杠） |
  | `C:\\Windows`（两个真反斜杠） | **timeout** |
  | `C:\` | ok=true |
  | `C:\Windows\notepad.exe` | ok=true |

- **修法**：归一化后再折叠连续分隔符（`//` → `\`），UNC 的 `\\server\share` 前缀需保留。

### E4〔严重〕AI 调用被拒绝时不写审计
- **位置**：`src/ai_api.cpp` `CallImpl` 三条 early-return（schema 非法 / 未知命令 / 危险未批准）
- **实测**：连续 4 次被拒调用 → `audit.log` 行数 **0 增长**。
- **违反**：AGENTS.md §5「每次调用写 `audit.log`（不可关闭）」、§8.5「没有审计的『成功』不认」。
- **说明**：`CommandBus::Audit` 已覆盖 `blocked` 与 handler 失败，机制完整；
  缺的是 **CommandBus 之前**那一段的审计。

### E5〔中等〕`agentctl call` 输出丢弃 errorCode
- **位置**：`tools/agentctl.py`（输出摘要只取 `ok/verdict/error/assertions`）
- **现象**：`call` 结果无 `errorCode`，而 `gap` 子命令**依赖它**分类。
  实测 `view.sort` 参数越界只显示中文「参数大于最大值：by」，机器无法分类。
- **违反**：AGENTS.md §8.3 要求按 errorCode 分流 —— `call` 把这条路断了。
- **修法**：摘要里补 `errorCode` 字段即可（一行）。

### E6〔中等〕键位冲突无人拦
- **位置**：`data/keymap.json`（两处）、`src/keymap.gen.h`（两处）
- **现象**：`Ctrl+Shift+A` 同时绑定 `TabSwitcher` 与 `CopyToFolder`。
- **现状**：`src/hotkey_manager.cpp` 有冲突检测（第 79 行 `r.conflict`），但**只在 UI 里显示**；
  `tools/check_commands.py` 不查 keymap 内部冲突 → 冲突可一路进发布产物。

### E7〔中等〕17 个文件超 500 行，无任何检查覆盖
- **超限文件**（行数）：shell_ops 1286、app_commands_files 1165、shell_view 1116、
  bookmark_bar 1073、popup_list 952、bookmarks 867、ai_server 752、app_commands_misc 732、
  commands 676、ai_api 640、nav_bar 622、app 570、shell_input 522、app_commands 520、
  settings_reg 511、file_tools 506、input_router 505。
- **判据**：AGENTS.md §3「单文件 ≤500 行」。`tools/check_components.py` 无行数规则。

### E8〔中等〕文档数字全面过期
| 位置 | 文档写的 | 实测 |
|---|---|---|
| `docs/PARITY.md` §2 | 合计 **155** 条 / 已实现 **147** | **173** 条 / 已实现 **165** |
| `docs/PARITY.md` Tab / Nav / Bookmark / Session | 33 / 6 / 15 / 4 | **40 / 10 / 18 / 8** |
| `README.md` 规模 | 2.33 万行（61 cpp + 53 h） | **30722 行（75 cpp + 65 h）** |
| `README.md` 单测 | 39 个 / 548 项断言 | **44 个 / 598 项断言** |
| `README.md` 检查 | check_ai_meta **16** 组 | **22** 组 |
| `README.md` 快捷键 | **70** 条 | **84** 条 |
| `README.md` 产物 | 20.2 MB | 32.6 MB（未 strip） |

`PARITY.md` 另漏记 **14+ 条已实现**命令：`session.named.{save,list,restore,delete}`、
`tab.{color,rename,pin,cycle.next,closed.list,reopen.index,move.window}`、
`nav.{history.list,recent.list}`、`tool.compare`。

---

## 2. 能力缺失（按投入产出比排序）

### M1〔最高优先〕只有命名管道，没有标准 MCP 端点
- **已有**：`initialize` / `tools/list` / `tools/call` / `exbar/state` / `events/subscribe`，
  协议版本 `2024-11-05`，令牌认证 + 危险命令双门 —— 协议面做得不错。
- **缺口**：**只有 Windows 命名管道**。标准 MCP 客户端（Claude Code / Cursor / Claude Desktop）
  需 stdio 或 HTTP/SSE 端点。
- **对标事实**：13 个对标产品中只有 **Eagle** 做了 MCP server（本地 HTTP `127.0.0.1:41596/mcp`）
  + Eagle Skill 包；微软已把 **File Explorer MCP connector**（暴露 10 个工具，含
  zip/unzip/edit_text_file/search_files）与 **Windows ODR**（`odr mcp add --manifest`）做成系统级标准。
- **判断**：这是**定位性缺口**而非功能缺口 —— 140 个工具已够用，缺的是「让标准客户端连得上」。
  成本最低的做法是加一个 stdio 转发器（读 stdin 的 JSON-RPC → 写命名管道 → 回写 stdout）。

### M2〔高〕撤销实际不可用
- **实测**：`file.undo` → `当前视图没有可撤销的操作（系统撤销栈为空，或视图已重建）`。
- **根因**：`src/shell_ops.cpp` `shellops::Undo` 向视图背景菜单找 "undo" 谓词，
  IExplorerBrowser 背景菜单无此行 → 恒 false。
- **对标**：XYplorer 100 步跨会话撤销、Everything 1.5 有撤销系统；
  OneCommander 开发者公开说明「Windows 不暴露撤销 API」而放弃，用户仍在评论区反复索要。
  **可撤销性是专业度分水岭。**

### M3〔高〕长任务在 UI 线程同步执行，无进度、无取消
- **涉及**：`tool.dupfinder` / `tool.hash` / `tool.compare` / `tool.diskusage` / `tool.topn`
- **机制**：全部同步跑在 UI 线程（`ai::PumpRequests` ← `WM_TIMER`）。
- **已有**：deadline 预算（`src/file_tools.cpp` / `src/disk_scan.cpp`），会如实上报
  `truncated` / `timedOut` / `complete`。
- **缺**：**进度上报**、**用户取消**。大目录扫描期间界面冻结、管道 10 秒超时。
- **说明**：`PARITY.md` §3.11 自认未做 helper 进程 + IPC，理由「显式预算已够」——
  但预算只解决「不会假死到没完」，不解决「中途不能停」。

### M4〔中〕显示偏好配置缺一大块
- **现状**：44 个设置项里**没有**字体、字号、网格间距、图标大小、主题色/强调色、
  预览窗格尺寸、双栏布局。
- **对标**：Opus / Total Commander / XYplorer 均可调字体 + 网格间距 + 图标大小。

### M5〔中〕归档 / 远程 / 剪贴板历史全无
- 压缩包：全仓零命中（zip / 7z / archive / compress / extract）。
- FTP / SFTP / WebDAV：零命中。
- 剪贴板历史：零命中。
- 分割合并文件、目录同步、NTFS 权限编辑、ADS 读取：零命中。

### M6〔中〕命令行只接受单个路径；`/select` 不选中文件
- **位置**：`src/main.cpp` `InitialPath()`
- **现象**：循环里 `break`，**只取第一个路径**（Explorer 支持 `explorer.exe a b c`）；
  `/select,file` 与 `/select file` 只取父目录，**不选中目标文件**
  （Explorer 语义是打开父目录 + 选中该文件）。

### M7〔中〕目录变更无自动监听
- 全仓无 `ReadDirectoryChangesW` / `SHChangeNotify` 订阅。外部改动后需手动 F5。
- 对标：Opus / TC / XY 均支持自动刷新与变更通知。

### M8〔低〕Shell 集成入口为零
- 无右键菜单扩展、无属性页扩展、无注册能力。
- **注意**：这与「不注入 explorer」定位**不冲突**（shell 扩展是往系统注册，不是注入），
  属可选增量。对标：Everything Shell Extension、ExplorerPatcher。

---

## 3. 有意取舍 / 不是缺陷（已核实）

| 项 | 依据 |
|---|---|
| 视图不自绘 | 定位明确；代价是自定义列 / 选择框 / 逐文件预览做不到（架构天花板） |
| `ext.run` / `ext.sendkeys` 不做 | 安全边界，AGENTS.md 与 docs 一致 |
| `tab.merge` 不做 | 单窗口模型无意义 |
| `view.toggle.checkboxes` 不做 | shell 未暴露该接口 |
| **事件缓冲线程安全** | **已排除嫌疑**：`src/ai_events.cpp` 读写两侧均有 `g_mu` 保护 |
| **配置文件原子写** | **已排除嫌疑**：`src/text_file.cpp` 带 pid 的 tmp + FlushFileBuffers + MOVEFILE_WRITE_THROUGH |
| **头文件自足** | **已排除嫌疑**：抽查 3 个 TU 以 `-fsyntax-only -Isrc` 独立编译通过 |
| **零警告** | **已排除嫌疑**：`bash build.sh --out` 零 warning |
| **三个静态检查** | **已排除嫌疑**：全绿 |

---

## 4. 同赛道标配清单（供后续排期参考）

**文件操作**：正则/模板/编号批量重命名（✅ 已有模板串，缺正则）、分割合并文件、
复制为链接（`path.copy.link` 已有命令位未实现）、永久删除 vs 回收站（✅ 回收站）、
NTFS 权限与 ADS、目录同步。

**视图**：自定义列（仅自绘列表可做）、选择框、逐文件预览（图片/文本/PDF/视频/代码高亮）、
双栏/多栏布局、状态栏统计（✅ 已有 9 段且可配置）、字体与间距（❌）、
深浅色主题（✅ 跟随系统）、标签颜色/固定（✅ 已有）、标签分组（❌）。

**导航**：地址栏补全（✅）、多级收藏夹（✅）、最近访问（✅）、驱动器/UNC（✅）、
FTP/SFTP/WebDAV（❌）、Shell 扩展搜索（❌）。

**搜索**：文件名搜索（✅ `search-ms`）、全文/内容搜索（❌）、
Everything 集成（❌ `search.everything` 已有命令位）、正则（❌）。

**集成扩展**：Shell 扩展（❌）、插件/VFS（❌）、脚本 Lua/JS（❌）、
自定义命令（✅ 命令总线即是）、外部工具配置（❌）、MCP/AI 接口（⚠️ 有协议无标准端点）。

**可用性**：vim-like 键盘导航（❌）、命令面板（✅）、类型即跳（⚠️ 仅弹层内有）、
撤销/重做历史（❌ 实际不可用）、多实例（✅）、多配置文件（✅ profile 隔离）、便携模式（✅）。

---

## 5. 建议排期

| 优先级 | 项 | 理由 |
|---|---|---|
| P0 | E2 模态框分流 + E3 路径折叠 + E4 拒绝审计 + E5 errorCode | 都是小改动，直接提升 AI 可用性与可信度 |
| P0 | E8 文档数字订正 | 文档与代码脱节会误导后续所有判断 |
| P1 | M1 stdio MCP 端点 | 生态接入的唯一入口，140 个工具已就绪 |
| P1 | M3 长任务 helper + 进度 + 取消 | 决定大目录场景能不能用 |
| P2 | M2 撤销 | 需要自建撤销栈（与系统栈错位风险），成本高 |
| P2 | M4 显示偏好 / M6 命令行多路径 / M7 自动刷新 | 日常体验补齐 |
| P3 | M5 归档 / 远程 | 需引第三方或自研，违反「零依赖」原则，需先论证 |

---

## 6. 复现命令

```bash
bash tests/run_all.sh                    # 44 个单测（clipboard_test 非交互环境跳过）
bash build.sh --out /tmp/x.exe           # 零警告验证
python tools/check_commands.py           # 命令表/keymap/处理器三方对账
python tools/check_ai_meta.py            # 22 组 AI 元数据检查
python tools/check_components.py         # 唯一性/禁令/接线/令牌/输入纪律

# 路径解析复现（E3）
python tools/agentctl.py serve --wait 45 &     # 后台起
python tools/agentctl.py call fs.open '{"path":"C://Windows"}'   # → timeout
```

> 审计期间有**另一个会话在并发改代码**（`commands.cpp` / `app.cpp` / `bookmarks.cpp` 等），
> 期间出现过一次 `commands.cpp` 编译错误（173 参数传入 vector 构造），
> 属对方改动过程中的中间态，非本仓稳定缺陷。本文所有实测均在可编译状态下复现。
