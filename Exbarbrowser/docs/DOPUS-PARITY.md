---
status: current
baseline: 2026-11-22 10:50
method: 读 D:\Dopus（Opus 安装本体）、D:\dopus-mcp（MCP 服务器）、E:\Dopus（AgentChat 侧边栏 + VFS 逆向）三个工作副本，
        交叉核对 docs/DOPUS-NOTES.md 的公开契约调研；本仓结论以实跑（check_commands / check_ai_meta / ebbctl list / 源码阅读）为准
---

# 能力对标：ExBarBrowser vs Directory Opus

> **回答一个问题**：对着一个成熟的专业文件管理器，本项目**还缺什么**、**不该补什么**、
> 以及**哪些"缺口"其实已经不存在了**（文档过期）。
>
> **口径声明**：
> - 本仓数字全部来自实跑：`tools/check_commands.py`（命令表三方对账）、
>   `tools/check_ai_meta.py`（AI 暴露面）、`tools/ebbctl.py list`（管道真实返回）。
> - Opus 侧数字来自 `D:\dopus-mcp\data\*.json`（从 `dopus.exe` 二进制提取 + 官方文档比对），
>   不采信二手转述。
> - **本文与其它差距文档的分工**：`EXPLORER-GAP-AUDIT.md` 对 Windows 资源管理器，
>   `COMPETITIVE-GAP-2026-11-21.md` 对同类第三方产品，本文**对 Directory Opus**。
>   `DOPUS-NOTES.md` 是 Opus 的**架构**调研（对象模型 / 该抄什么不抄什么），
>   本文是 Opus 与本仓的**能力逐项对照**。

---

## 0. 一句话结论

**本项目在"机器接口"这条线上比 Opus 强，在"领域广度"这条线上比 Opus 弱。**

| 维度 | 判定 | 依据 |
|---|---|---|
| 机器可调用面 | **本项目更强** | Opus 只有内部命令 + 脚本 API，AI 侧靠 `dopus-mcp` 转译；本项目命令总线是**产品 API**，带 JSON Schema、前置条件声明、自断言、审计、事件流、危险双门 |
| 领域能力广度 | **Opus 更强** | 搜索、结构化属性写入、重命名规则、压缩归档、视图筛选、文件类型关联，Opus 都有，本项目**没有或只有一半** |
| 扩展点 | **Opus 更强** | Opus 有 16 个 VFS 插件 / 133 个脚本 / 49 个事件 / 162 个脚本对象；本项目无脚本引擎（**但这是有意的**，见 §4） |
| 自身接缝 | **本项目有洞** | AI 面板（`win.ai.toggle`）未实现；MCP 端点已实现但**未注册**进客户端生态 |

**最该做的三件事**：① 把已实现的 MCP 端点注册进 `~/.workbuddy/mcp.json`（只差接线）；
② 搜索打穿（AI 侧目前**完全空白**）；③ 属性写入（现在只能看不能改）。

---

## 1. 数量对照（两边各有多少）

### 1.1 本项目（实跑，2026-11-22 10:50）

| 口径 | 值 | 来源 |
|---|---:|---|
| 命令表条目 | **210** | `check_commands.py` |
| 已实现 | **201** | 同上 |
| 处理器已注册 | **201** | 同上 |
| keymap 引用 | 105 | 同上 |
| AI 暴露 | **165** | `check_ai_meta.py` |
| 管道 `tools/list` 实际返回 | 155 | `ebbctl.py list`（**旧构建**，见 §5.4） |

> ⚠️ 运行中的 `ExBarBrowser.exe` 建于 2026-11-22 01:20，源码最新改动在 09:55 之后，
> `agentctl.py status` 报 `stale=true`。**管道返回的 155 条是旧构建的表**，
> 源码口径是 165 条。本文一律用**源码口径**。

### 1.2 Opus 侧（从 `dopus.exe` 提取 + 官方文档）

| 项 | 值 | 来源 |
|---|---:|---|
| 内部命令（可提取签名） | **31**（官方 29） | `opus-internal-commands.json` |
| 内部命令参数总数 | **900+** | 同上（`Set` 157 / `Go` 116 / `Copy` 66 / `Find` 64 / `Prefs` 60 / `Select` 58） |
| 脚本 API | **162 对象 / 468 方法 / 1041 属性** | `opus-api-inventory.json` |
| 脚本事件 | **49** | 同上 |
| 列关键字 | **178** | 同上 |
| 脚本 | **133**（活动 129 / 禁用 4） | `opus-scripts.json` |
| 脚本模块 | 9 个（数据库型 2 / 代码型 7），**1824 字段** | `opus-script-modules.json` |
| VFS 插件 | **16** | `opus-vfs-plugins.json` |
| 默认热键 | **118 动作 / 136 键位** | `opus-default-hotkeys.json` |
| COM 接口 | 10 个类型（5 coclass + 5 dispatch），**122 方法** | `opus-com-typelib.json` |
| `dopus-mcp` 暴露的 MCP 工具 | **32** | `dopus-mcp/src/index.ts` |
| `AgentChat` 侧边栏工具 | **15** | `E:\Dopus\AgentChat\service\tools.js` |

**读法**：Opus 的 31 条内部命令参数极厚（`Set` 一个命令 157 个参数），
本项目 210 条命令**平均参数少得多**。这说明差距**不在命令条数，在每条命令的参数深度**。

---

## 2. Opus 有、本项目确实没有（按缺口大小排序）

### 2.1 搜索 —— 最大的一块空白

| | Opus | 本项目 |
|---|---|---|
| 命令 | `Find`（64 参数），引擎可切 `everythingglobal` / `everything` / `windows` / `opus` | `search.find`、`search.everything` **两条都是 `implemented=false`** |
| 实现 | `opus_search` 走原生 `Find QUERYENGINE` | `search-ms:` 被宿主 `IExplorerBrowser` 明确拒绝，**恒失败** |

`docs/EXPLORER-GAP-AUDIT.md` §2.1（F1）已把它记为"假能力"并降级 —— **降级是对的**，
但**能力本身仍然是空的**：AI 侧目前没有任何可用搜索。

Opus 的 `Find` 还带 64 个参数（`MD5` / `DUPES` / `ARCHIVES` / `COLLNAME` / `OF` / `SIMPLE` / `ADVANCED` …），
本项目连最基础的一条都没通。

### 2.2 结构化文件属性写入（`SetAttr`，21 参数）

Opus 的 `SetAttr` 覆盖 `SETATTR` / `CLEARATTR` / `TOGGLEATTR` / `MODIFIED` / `CREATED` / `RECURSE` /
`DESCRIPTION` / `CHMOD` / `ZIPCOMMENT` / `META`。

本项目：`fs.properties` 只是**弹对话框**（`src/app_commands_fs.cpp` 的 `FsProperties`），
`SetFileAttributesW` 全仓零调用 —— **属性只能看，不能改**。
`bookmark_bar.cpp` / `bookmarks.cpp` 里的 `GetFileAttributesW` 是**读**（判断图标），不是写。

### 2.3 重命名规则引擎（`Rename`，31 参数）

| Opus 参数 | 本项目 |
|---|---|
| `REGEXP` / `FINDREP` | ❌ 无 |
| `AUTONUMBER` / `NUMBER` | ❌ 无 |
| `BY` / `TYPE` / `PRESET` / `SIMPLE` | ❌ 无 |
| `CASE` / `RECURSE` / `INLINE` | ❌ 无 |
| `PATTERN` + `TO` | ✅ `file.rename.batch`（仅 pattern） |

`src/file_rename_plan.h` 的接口只有 `pattern` 一个自变量，没有正则、没有自动编号。
「把选中的照片按拍摄日期重命名」这类需求（`E:\Dopus\AgentChat` README 里的示例提示词）
在本项目做不了。

### 2.4 归档 / 压缩

Opus 的 `Copy` 带 `ARCHIVE` / `EXTRACT` 参数，能压缩 / 解压目录。

⚠️ **易误判点**：本项目 `src/app_commands_archive.cpp` 里的 `dev.archive.*`
是**自我迭代用的源码快照归档**，**不是文件压缩**。全仓无任何 zip/compress 实现。

### 2.5 视图筛选（`Set` 的 `FILTERS` / `COPYFILTER` / `DELFILTER`）

Opus 的 `Set` 是 157 参数的最大命令，含筛选、输出、双栏、树、格式等一整套视图配置。

本项目：`src/shell_view.cpp` 里 filter 零命中。`file.select.pattern` 是**选中**，
不是**过滤视图**（筛掉不显示）。「只看 `.jpg`」在本项目做不到。

### 2.6 其余命令域缺口速查

| Opus 命令（参数数） | 本项目对应 | 缺什么 |
|---|---|---|
| `Prefs`(60) | `config.set`（36 个平铺键） | 无分组、无**导出/还原**（Opus 的 `.ocb`） |
| `Favorites`(36) | `bmk.*`（21 条） | 无 **Smart Favorites**（条件化动态收藏夹） |
| `FileType`(24) | `file.open.with` | 只有一次性打开，无**全局扩展名关联表** |
| `Undo`(7) | `file.undo` | **无 redo**（`fileundo::Stack` 只有 `Undo`）；无撤销历史浏览 UI |
| `Image`(35) | — | 无图片转换 / 旋转 / 缩放 |
| `Print`(20) | — | 无 |
| `GetSizes`(10) | `tool.hash` | 有 MD5/SHA1/SHA256，无 `USEHASHCACHE`（缓存复用） |
| `Clipboard`(25) | `path.copy.*` | 无复制队列（`COPYQUEUE`） |
| `Recent`(17) | `nav.recent.list` | 只读，无管理 |
| `Split`(7) / `Join`(4) | — | 无文件拆分 / 合并 |
| `ContextMenu`(14) | 右键菜单 | 有 UI，无**结构化调用入口** |
| `CreateFolder`(14) | `file.new.folder` | 无 `FROMFILES` / `UNDORENAME` |
| `Delete`(20) | `fs.delete` | 无 `SECURE` / `EMPTYRECYCLE` |
| `CLI`(26) | — | 无命令行模式（Opus 的 `dopusrt.exe /cmd` 走这条路） |
| `Help`(27) | — | 无内置帮助 / 版本信息 |

### 2.7 脚本扩展点（Opus 最强的一块）

| 项 | Opus | 本项目 |
|---|---:|---|
| 脚本 | 133（注册 131 个命令、63 个列） | 0 |
| 事件 | 49 | 0（本项目自有 `events` 流是**观测**，不是**扩展点**） |
| 脚本 API | 162 对象 / 468 方法 | 0 |
| 脚本模块字段 | 1824 | 0 |
| 列关键字 | 178 | 0 |

**但这不该抄** —— 见 §4。本项目的 `dev.*` 命令族 + 命令总线是"可编排"的替代形态，
引入脚本引擎是另一个工程。

---

## 3. Opus 的机器接口分层法：本项目该抄的三条

`D:\dopus-mcp\README.md` §三 明确警告**三个层次的工具不要混用**。这张表是本文最有价值的部分：

| 层 | `dopus-mcp` 的工具 | 本项目对应 | 判定 |
|---|---|---|---|
| **内部命令层**（驱动产品本身） | `opus_run_command`、`opus_navigate`、`opus_command_help` / `_list` / `_find_arg` / `_validate` | ✅ 命令总线已有 | **缺"命令自省"工具**（见 3.1） |
| **运行时查询层**（实时读） | `opus_fsutil`(13 子命令)、`opus_item_info`、`opus_selected_items`、`opus_lister_tabs` | ✅ `tool.*`（10 条）覆盖大半 | 缺路径语义子命令（见 3.3） |
| **文档声明层**（写脚本用） | `opus_api_inventory`、`opus_event_list`、`opus_column_keywords` | 无 | 不需要（无脚本引擎） |

### 3.1 `command_help` / `command_validate` —— 运行期命令自省

`dopus-mcp` 的做法：从 `dopus.exe` 提取命令签名做成静态数据（29 条全覆盖，含 `Go` 的 116 个参数），
供 AI 查"这条命令怎么用、参数名对不对"。

**本项目已有等价数据源**（`src/commands.cpp` 的 `argsSchema` + `outputSchema`），
做成只读工具的成本很低。现在 `tools/check_ai_meta.py` 只在**构建期**校验，
**运行期** AI 拿不到"这条命令的正确用法"。

### 3.2 配置写入的快照 + 回滚

| | `dopus-mcp` | 本项目 |
|---|---|---|
| 写配置 | `opus_config_write`：**每次写前自动快照**，快照失败即中止，写后**读回校验** | `config.set` 是裸写 `settings.json` |
| 看历史 | `opus_config_snapshots`（id / 时间 / 原因 / 涉及文件） | 无 |
| 回滚 | `opus_config_restore`（覆盖文件、删新建文件、**逐文件哈希校验**） | 无 |

⚠️ **易误判点**：本项目的 `dev.snapshot` / `dev.rollback` 覆盖的是**源码工作树**
（`RepoRoot()` = exe 所在目录），**不覆盖 `settings.json` 与 `bookmarks.html`**。
AI 通过 MCP 改配置时**没有回滚保险**。

### 3.3 `fsutil` 的路径语义子命令

`opus_fsutil` 有 13 个子命令，其中这几个本项目没有对应：

`normalize` / `resolve` / `comparePath` / `sameDrive` / `pathType` / `displayName` / `adsNames` / `drives` / `metadata`

本项目的 `pathutil::ToShellPath` 已经在**内核**解决了"正斜杠进 shell 会报 `0x80070057`"的坑
（`docs/ARCHITECTURE-RULES.md` R21 口径），但 **AI 侧看不见这个能力**：
AI 传 `C:/dir` 时无法先确认自己理解对不对。

### 3.4 顺带可抄的：安全闸的分层

`E:\Dopus\AgentChat\service\tools.js` 的头注释里有三条设计，本项目部分已有、部分缺：

| AgentChat 的做法 | 本项目状态 |
|---|---|
| 工具只收**扁平标量**参数，返回**短**结构化结果（注释明说：Opus bridge 的嵌套巨物喂给模型会烧 context） | ✅ 大体符合（`tool.*` 返回扁平结构） |
| `allowedRoots` **在代码里强制**路径边界，不靠 prompt 里的句子劝阻 | ❌ **全仓无路径白名单**（`allowedRoots` / `PathGuard` 零命中）——只有 `aiAllowDanger` 危险命令开关 |
| 破坏性操作先出**确认卡片**，用户点"允许执行"才跑 | ✅ 已有（`approve` + `aiAllowDanger` 双门） |

**路径白名单是本项目目前最实际的一个安全缺口**：AI 通过 MCP 能操作全盘任意路径，
唯一的闸是"这命令危不危险"，而不是"这路径该不该碰"。

---

## 4. Opus 有、本项目**不该**抄（复核 `DOPUS-NOTES.md` §3）

`docs/DOPUS-NOTES.md` §3 已经判过一遍，本文**复核后同意**，只补一条时效修正：

| Opus 的做法 | 抄不抄 | 理由 |
|---|---|---|
| VFS 插件（16 个：`Rclone://` / `openlist://` / `reg://` / `taskschd://`） | **不抄** | 无 VFS 需求；引 DLL ABI 带来版本与崩溃面。`E:\Dopus\VFS\` 那 90 个脚本是**逆向调研产物**（针对 `dopus.exe` 的 PE/反汇编），不是待移植清单 |
| 脚本 add-in 引擎（133 脚本 / 49 事件） | **不抄** | 另一个工程。本项目 `dev.*` + 命令总线是等价替代 |
| 声明式工具栏 / 菜单（`Toolbar`/`Marker` 命令） | **不抄** | chrome 用 GDI 自绘（`docs/DECISIONS.md` D3），不打算让用户改 chrome |
| `Prefs` 的 FTP 站点 / 布局定制 | **不抄** | 无 FTP 需求 |
| 双栏 / 标签组 | **见下方时效修正** | — |
| `Clipboard` 复制队列 / `Image` / `Print` / `Split` / `Join` | **暂不抄** | 真实但低频，按 P0/P1 收口后再排 |

### ⚠️ 时效修正：双栏已在落地，`DOPUS-NOTES.md` §3 与 §4 的相关结论已过期

`DOPUS-NOTES.md` §3 写「双栏 / 标签组 / 布局样式 → **不抄**（单栏单窗口是当前产品定位）」，
§4 的映射表也把 `dual` 归到"不抄"。**但工作树正在改这条决策**：

- `src/commands.cpp` 新增 `view.dualpane.toggle` / `view.pane.next` / `view.pane.split`（`SetPaneSplit`，20–80%）
- `src/app.h` 新增 `struct Pane`（`panes[2]` + `activePane`）、`PaneAt` / `ActivePane` / `SetDualPane` / `NextPane` / `SetPaneSplit` / `FindPane`
- `src/app_layout.cpp` 已按 `s.dualPane` + `s.paneSplit` 布局左右两个 `RECT`，右窗格按需创建

所以**双栏不再是"不抄"项，而是进行中项**。本文据此把双栏从 §4 移出，不再列为缺口。
`DOPUS-NOTES.md` 的对应两行待改（属另一份文档的订正，不在本文范围）。

---

## 5. 本项目自己的缺口（比 Opus 对标更要紧）

### 5.1 AI 面板没有实现

`win.ai.toggle` / `win.ai.stop` 两条命令的 `implemented` 都是 `false`
（`src/commands.cpp` 的 `AiTogglePanel` / `AiStop`），`src/` 里**没有任何 AI 面板 UI 代码**。

`E:\Dopus\AgentChat` 已把这块做完，且是本项目最好的参照物。它的 v2 迁移动机值得抄：

| v1（Opus 脚本对话框）的限制 | v2 的解法 |
|---|---|
| 控件只有 Edit / Static / Button / List 几种 | 换 WebView2 + 独立面板 |
| `Dialog.NewHTTPReq` 只能拿完整响应 | 独立服务做**逐字流式** |
| 给 `.value` 赋值会重置滚动位置 | 自绘 UI |
| 没有 JSON 对象、没有 DirectUI 自绘 | Node 服务 + 浏览器渲染 |
| 对话框在消息循环里**阻塞** | 独立进程，人机互不阻塞 |

> 但本项目 `docs/DECISIONS.md` **D3** 已判「chrome 用 GDI 自绘，暂不上 WebView2」——
> 与 AgentChat v2 的技术路线**直接冲突**。这是一条需要显式决策的分叉，见 §6。

### 5.2 MCP 端点已实现，但没接上生态

`src/mcp_stdio.cpp` 已实现，`--mcp-stdio` 入口可用，`initialize` 返回
`protocolVersion = "2024-11-05"`，支持 `tools/list` / `tools/call` / `events/subscribe` /
`events/since` / `exbar/state` / `exbar/capabilities` —— **协议层是齐的**。

但：

| 事实 | 证据 |
|---|---|
| `docs/COMPETITIVE-GAP-2026-11-21.md` §M1 仍把"只有命名管道，没有标准 MCP 端点"列为**最高优先缺口** | 该结论**已被 `src/mcp_stdio.cpp` 推翻**，文档过期 |
| `~/.workbuddy/mcp.json` 里**没有 ExBarBrowser 条目** | 只有 `dopus-mcp`（且 `disabled: true`）、`rea`（`disabled`）、`yimu-bookkeeping`、`craft-anything` |

**即：M1 的"实现"已完成，只差"注册"这一步没做。**
`D:\dopus-mcp\README.md` 第 5 步专门警告过这个坑：改完 `mcp.json` 不**完全重启**客户端，
条目永远加载不上（主进程只在启动时读一次配置）。

### 5.3 路径白名单缺失

见 §3.4。当前 AI 通道（管道 + stdio MCP）的路径边界**只受"命令危不危险"约束**，
不受"路径该不该碰"约束。`AgentChat` 的 `allowedRoots` 提供了现成范式。

### 5.4 运行实例是旧构建

`agentctl.py status` 报 `stale=true`：exe 建于 2026-11-22 01:20，源码最新改动在 09:55 之后。
管道 `tools/list` 返回 155 条，源码口径 165 条 —— 差 10 条。
**任何基于管道返回的清单都是旧构建的**；本文一律用源码口径。

---

## 6. 优先级建议

| 优先级 | 事项 | 依据 | 成本 |
|---|---|---|---|
| **P0** | 注册进 `~/.workbuddy/mcp.json`（并重启客户端） | §5.2，协议层已完备，只差接线 | 极低 |
| **P0** | 搜索打穿 | §2.1，AI 侧完全空白 | **高（见 §7）** |
| **P1** | 属性写入（`SetAttr` 等价） | §2.2，看得到改不了 | 中 |
| **P1** | 重命名规则（正则 / 自动编号 / 按类型） | §2.3，Opus 31 参数对应的高频需求 | 中 |
| **P1** | `command_help` 运行期自省工具 | §3.1，数据已在 `commands.cpp` | 低 |
| **P1** | 路径白名单（`allowedRoots` 等价） | §3.4 / §5.3，现存安全缺口 | 低 |
| **P2** | 配置写入快照 + 回滚 | §3.2，AI 改配置无保险 | 中 |
| **P2** | `fsutil` 路径语义工具 | §3.3 | 低 |
| **P2** | AI 面板（参照 AgentChat v2） | §5.1，**但与 D3 冲突，见 §7** | 高 |
| **P3** | 压缩/解压、文件类型关联、redo、smart favorites、Image/Print/Split/Join | §2.6，低频 | — |

---

## 7. 两处需要拍板的分叉

### 7.1 搜索的实现路线（成本差一个数量级）

| 路线 | 代价 | 说明 |
|---|---|---|
| **Everything IPC** | 中 | Opus 的 `Find` 就是这条路（`everythingglobal` / `everything` 引擎）；需装 Everything 并接其 IPC |
| **自建索引** | 高 | 独立索引，与用户看到的视图可能不一致（`src/app_nav.cpp` 的 `search_url.h` 头注释已论证过这个矛盾） |
| **换宿主** | **极高** | 换掉 `IExplorerBrowser` —— **会推翻整个架构前提**，不现实 |

前两条是"加一个模块"，第三条是"重写"。**建议前两条之一。**
（`D:\Dopus` 侧有 `Find` 的 `EVERYTHING` 参数可作参照，但需确认目标机是否装有 Everything。）

### 7.2 AI 面板的技术路线（D3 冲突）

`docs/DECISIONS.md` **D3**「chrome 用 GDI 自绘，暂不上 WebView2」——
`AgentChat` v2 恰恰是因为脚本对话框做不出现代 Agent 界面才换成 **WebView2 + 独立面板**。

三条出路：

1. **沿用 D3**：AI 面板用 GDI 自绘，不做流式气泡。成本低，但放弃 AgentChat 的核心体验。
2. **只为 AI 面板开 WebView2 例外**：chrome 保持 GDI，面板单独走 WebView2 + 本地服务。
   与 D3 冲突但可写成"D3 补充例外"。
3. **整体 Web 化**：`docs/ROADMAP.md` §5 已写「明确不做：把整个窗口 Web 化」
   （一个空白 WebView2 就 260 MB）。

**本文不替这条分叉做决定**，但要指出：**AI 面板是 P4 的头号差异化项，而它的实现路线
与项目已接受的一条架构决策直接冲突**，越早定越省事。

---

## 8. 数据来源

### 本仓（实跑）
- `tools/check_commands.py` → 命令表 210 / 已实现 201 / 处理器 201 / keymap 105
- `tools/check_ai_meta.py` → AI 暴露 165；210 条全部合法
- `tools/ebbctl.py list` → 管道 `tools/list` 返回 155（旧构建）
- `tools/agentctl.py status` → `stale=true`，exe 01:20 vs 源码 09:55
- `src/commands.cpp` / `src/commands.h` / `src/app_commands_fs.cpp` / `src/file_rename_plan.h` /
  `src/file_undo.h` / `src/app.h` / `src/app_layout.cpp` / `src/mcp_stdio.cpp` / `src/ai_server.cpp`

### Opus 侧（提取件）
- `D:\dopus-mcp\data\opus-internal-commands.json` → 31 命令 / 参数集
- `D:\dopus-mcp\data\opus-api-inventory.json` → 162 对象 / 468 方法 / 1041 属性 / 49 事件 / 178 关键字
- `D:\dopus-mcp\data\opus-scripts.json` → 133 脚本 / 131 命令 / 63 列
- `D:\dopus-mcp\data\opus-script-modules.json` → 9 模块 / 1824 字段
- `D:\dopus-mcp\data\opus-vfs-plugins.json` → 16 VFS 插件
- `D:\dopus-mcp\data\opus-default-hotkeys.json` → 118 动作 / 136 键位
- `D:\dopus-mcp\data\opus-com-typelib.json` → 10 类型 / 122 方法
- `D:\dopus-mcp\README.md` §三 → 三层工具分层法 / 32 工具
- `E:\Dopus\AgentChat\README.md` + `service/tools.js` → 侧边栏 v2 动机 / 15 工具 / `allowedRoots`
- `E:\Dopus\VFS\` → 针对 `dopus.exe` 的逆向调研脚本（**调研产物，非移植清单**）
- `docs/DOPUS-NOTES.md` → Opus 公开契约调研（对象模型 / 抄什么不抄什么）

### 官方文档（经 `DOPUS-NOTES.md` §6 转引）
- https://docs.dopus.com/doku.php?id=reference:scripting_reference:scripting_objects:{dopus|lister|tab}
- https://docs.dopus.com/doku.php?id=reference:dopusrt_reference
- https://www.gpsoft.com.au/help/opus12/Documents/Scripting/scriptingobjects.htm
