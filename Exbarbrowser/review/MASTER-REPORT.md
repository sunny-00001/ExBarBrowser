# ExBarBrowser 穷举式深度代码审查 · 主报告

- 生成时间：2026-11-23 21:00（快照）
- 覆盖范围：src/ 全部 278 个源文件（含审查期间从 shell_view.cpp 拆出的 5 个新文件），56,148+ 行
- 方法：11 个独立子代理，按互斥文件清单分组，逐文件完整通读、逐函数逐分支审查，**无抽样**
- 覆盖校验：11 份清单并集 = 273 个清单文件 + 5 个新拆分文件 = 278，`missing=[]`、`extra=[]`（机器验证）
- 分组明细：见同目录 G1..G11 `.md` 与 `.txt`

> 重要：审查期间 `src/` 被审查之外的进程并发改写（shell_view.cpp 1176→467 行并拆出 4 个文件、theme.cpp/ui_draw.cpp/nav_bar.cpp/settings_dialog* 等持续变动）。所有结论为**时间点快照**，行号以各分报告为准；改代码前请先复核目标文件当前内容。

---

## 0. 门禁现状（"绿"不等于"对"）

| 检查 | 结果 | 要点 |
|---|---|---|
| check_commands.py | ✅ 0 | 三方对账一致 |
| check_ai_meta.py | ✅ 0 | 28 项通过，但存在假绿（见 §3） |
| gen_keymap.py --check | ✅ 0 | 一致 |
| check_components.py | ❌ 1 | R21 判红 + 14 文件超 500 行欠账 + 评估面未重签 |
| tests/run_all.sh | ✅ 66/66 | 覆盖偏参数层/元数据层，核心失败路径缺失 |

---

## 1. 严重 / 高危（必须修，已逐条独立复核）

### A. 数据安全与内存 UB
1. **`bookmarks.cpp` MoveTo 越界写**（已核实）——同容器非 root 移动不夹取 `to` 上界，`insert(begin()+to)` UB。`bmk.move {"toIndex":999}` 可触发堆越界。修：erase 后按 `newSize=dest->size()-1` 夹紧，越界返回 false。
2. **`shell_ops.cpp:311-316/411-416` IFileOperation 排队阶段不判 HRESULT**（已核实）——`SetOperationFlags`/`SetOwnerWindow`/`DeleteItem`/`NewItem` 返回值全丢弃；全失败仍记成功；缺 `FOF_ALLOWUNDO` 的一条是**静默永久删除**路径。
3. **`bookmarks.cpp` 改名族从不 PushUndo**，但 `bmk.edit` 声明 `undoable=1` → undo 回滚到更早操作；分类 `path/icon` 不序列化，重启丢失。

### B. AI 子系统（最脆，用户点两下就撞到）
4. **`ai_queue` `interrupted_` 粘性无重置**（已核实）——设置里关→开 AI 通道后，整条通道**永久只回 timeout**。修：加 `Reset()` 并在 StartServer 调用。
5. **`ai_server StopServer` 先 join 后 Interrupt**（已核实）——关闭/退出可卡到 `aiTimeoutMs` 上限 10 分钟。修：`g_running=false` 后立即 `Interrupt()` 再 join。
6. **`events/subscribe` 鉴权旁路**（已核实，比原报更严重）——`processLine` 的 `IsSubscribeRequest` 只比对 method、不看令牌，无令牌连接被回 `bad_token` 但仍切入订阅态并推送全部事件。修：订阅切换前校验令牌。
7. **`aichat_webview Start()` 失败后重试假成功**；`g_pipe` 跨线程裸句柄；`CancelIoEx` 用在同步句柄；管道单实例被 mcp_stdio 独占致第二客户端 ERROR_PIPE_BUSY。

### C. 输入 / 钩子（R21 与硬约束）
8. **全局键盘钩子 `kb_hook.cpp` 装 WH_KEYBOARD_LL**，与 AGENTS.md「不装全局钩子」及 MIGRATION-PLAN C 类正面冲突；`check_components.py` 无对应守卫。
9. **钩子 Ctrl/Alt 早退早于 key-up 记账**（已核实第 136 行）——按住字母→按 Ctrl→松键，`g_downCount` 永久停在 1，该键此后恒判 repeat，序列键与裸键热键在进程内彻底失效。
10. **钩子不跟踪 Win 修饰**——Win+E/R/S/D/L 的字母被吞当裸键，系统快捷键失效 + 误触序列前缀。
11. **`app.cpp:599` PostMessage(WM_KEYDOWN) 回投**触发 R21 INPUT_GUARD 判红（钩子下游产物，与 #8 同源）。
12. **`shell_input Install` 丢弃 3 个 Subclass 结果**，全空仍 return true → 鼠标规则静默全失效。

### D. 启动与导航逻辑
13. **`main.cpp` 无参启动返回空串 → 转发失败 → 双击图标第二次必多开一个窗口**（已核实，修两分支即可）。
14. **`app_nav` 同路径导航提前 return 但 `History::pending_` 已写入不清理**（已核实 Request 在 89 行已写 pending）——随后双击子目录完成回调被当重定向，历史条目就地替换，后退跳层。
15. **`nav_pane_shell UnhookTreeScroll` 只清指针不摘子类** → refData 悬垂 UAF。

### E. 其它严重
16. **`popup_list` 模态泵不过滤 WM_SETTINGCHANGE/WM_THEMECHANGED** → 弹层借用的 HICON/HFONT 被并发销毁，DrawIconEx 打悬空句柄。
17. **`job.cpp` 取消同时置 cancelled+failed**；异步作业线程永不 join、作业表无淘汰。
18. **`config_io Save()` 全量重建 JSON，未知键静默抹掉**；解析失败护栏只挡写盘、命令层仍报成功。
19. **`settings_reg_valid StrEnumSet` 大小写不敏感校验却存原串** → `themeMode "Dark"` 报成功、落盘、重启归 system。
20. **dev 观测面"假绿"**：dev.diff 四条 git 调用丢返回码、archive 落盘字节与 manifest sha256 不符、快照 git 失败仍报 ok、eval_surface CanonicalForm 注释说排序但没排（运行期信任根）。

---

## 2. 中等（择要，完整见分报告）

- 命令"部分成功"三套语义并存（BatchRename 全对 / fs.* 报 true / tool.hash 全败报 true）；`WithView` 失败无原因；`ShellExecuteW` 返回值多处忽略 → 恒 true。
- `file.select.pattern`(aiExposed) 空参弹模态框；`file.rename.batch`/`tool.hash` 机器侧隐式吃当前选中。
- `Derive()` 只在注释、无实现 → 28 条 CamelCase 旧名 FindCommand 直接 nullptr；`GroupDisplayName` 漏 Ai/Config/Dev 三组。
- `hotkey_manager` 一命令多设置时改错设置项、清除清不掉。
- 文件级可变全局状态 20+ 处超例外（nav_pane_root 线程、shell_input 上下文表、ai_events 总线、mcp_stdio 等）；`unhook`/`nav_bar` 单槽 WNDPROC 多实例断链。
- 跨层 include：`settings_reg_table.cpp→status_bar.h`、`folder_tree.h→popup_list.h`、`nav_pane_root.cpp→nav_pane_explorer.h`。
- `common.h` 含副作用实现（SystemIsDark/ApplyDarkTitleBar/DpiOf static 缓存/CountText 文案）。
- UI：`status_bar` 段丢弃链无兜底致重叠；`popup_list` 宽度不夹；`menu_context` 每行整表量宽 O(n²)；`modal.cpp` `gm` 未初始化即读；`aichat_panel` 0 处 theme_.S() + 11 个硬编码像素（高 DPI 风险）。
- `settings_dialog` 命中状态由绘制函数写入、Layout 后未重绘按钮失效；全窗零裁剪。
- 行数棘轮：`app.cpp` 超基线、`nav_bar.cpp` 552>547、`commands.h` 507 无记账。
- 测试缺口：shellopexec 失败矩阵、fs.* handler 错误路径、config_io 坏文件保护、ai_api 全 errorCode、ai_server 授权后协议、json 解析边界、input_router 状态机、bookmark_bar 命中——多数可纯函数离线补测。

---

## 3. 跨组共性根因（比单点更有价值）

1. **"注释说 A、代码做 B"系统性倾向**（G4/G5/G10/G11 均独立指出）：注释写的是判据与承诺，实现漏了那 5%。危害不是崩溃，而是耗尽读者对注释的信任。建议把关键注释改为可执行的断言/单测。
2. **"脚本全绿掩盖真实契约缺口"**：check_ai_meta 绿但 Derive 未实现、undoable 登记但没入栈、断言门槛只覆盖 exposed 子集。检查器需覆盖到 `implemented` 全集及推导回退。
3. **IFileOperation 单点判败对、但排队前置与调用方不判**：执行点 `shellopexec::Run()` 是干净的（值得保留），问题全在 setter/入队/批量汇总，且该点无单测。
4. **文件级可变全局状态 widespread**：导航窗格/输入/AI 面板/事件总线/mcp 桥均超三条例外，是搬迁到目标分层最大的阻力。

---

## 4. R21 合规（值得肯定）
全仓 `SendInput/mouse_event/keybd_event/SetCursorPos/BlockInput/AttachThreadInput` 零真实命中；脚本侧抢焦点零命中；`probe_toggle.py` 纯观察、`probe_highlight.cpp` 只投 WM_THEMECHANGED。R21 风险**集中于 #8/#11 这一条钩子因果链**。

---

## 5. 建议执行顺序
1. 修 `bookmarks.cpp` MoveTo 越界 + 改名 PushUndo（内存/数据安全）。
2. 修 AI 通道：Interrupt 顺序 + 队列 Reset + 订阅鉴权 + 管道所有权。
3. 定钩子策略：撤全局钩子回退应用内消息循环，或正式豁免 + 补 INPUT_GUARD 白名单；连带修 Ctrl/Alt 记账与 Win 修饰。
4. 修 `shell_ops` 排队阶段 HRESULT + 补 shellopexec 失败矩阵单测。
5. 修 `main.cpp` 无参转发、`app_nav` pending 清理、`UnhookTreeScroll` 摘子类。
6. 统一"部分成功"语义（ok=false + requested/hit/failed[]）。
7. 收编：全局状态对象化、跨层 include 下沉、common.h 净化、行数拆分、检查器补盲区（品牌串 UA、长路径）。
