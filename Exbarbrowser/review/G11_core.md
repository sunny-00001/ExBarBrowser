# G11_core 审查报告

审查范围：`review/G11_core.txt` 列的 40 个文件（7451 行，逐文件全量读完，无抽样）＋ 3 个跨组交叉核对文件（`view_stats.cpp` / `view_modes.h` / `app_wnd.cpp` 的 WM_COPYDATA 分支，用于验证第 7 维度与跨文件契约）。
只读审查：未编译、未杀进程、未改源码。唯一写入是本文件。
**实测验证**：运行了只读校验器 `python tools/check_components.py`，结论见 H7。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|------|------|------|
| 1 | src/app.cpp | 871 | **不合格**：超 500 行棘轮基线（H7）；品牌串硬编码（M29）；会话索引错位（M30）；类注册失败静默（L1） |
| 2 | src/app.h | 499 | 基本合格：紧贴 500 上限，成员/接口注释质量高；建议下次抽 `app_types.h`（M28） |
| 3 | src/assert_check.cpp | 283 | 有条件合格：`$changed` 与 `=` 的比较口径不一致（M23）、`.length` 口径未定义（L2） |
| 4 | src/assert_check.h | 84 | 合格：接口与理由清晰；缺"比较口径"说明（随 M23 补） |
| 5 | src/audit_log.cpp | 195 | 有条件合格：`thread_local` 上下文是隐式契约（M13）、双文件写无串行化（M14） |
| 6 | src/audit_log.h | 170 | 合格：字段设计有据；`errorCode` 未 OrDash 造成 JSONL 口径不一（L3） |
| 7 | src/clipboard.cpp | 58 | 合格：分配-再-清空的顺序修得对；SetClipboardData 失败仍已丢旧内容（L4） |
| 8 | src/clipboard.h | 12 | 合格 |
| 9 | src/common.h | 278 | **不合格（轻度）**：`CountText` 把 UI 文案放进"只放类型/常量"的头（M10）；`CopyDataPayload.path[1024]` 误导（M11）；`DpiOf` 无回退（M8） |
| 10 | src/dev_archive.cpp | 381 | **不合格**：轨迹哈希与落盘字节不一致（H3）；复制失败静默跳过（M20）；tmp 残留（M20b）；本地重复 `ParentOf/Normalize`（M31） |
| 11 | src/dev_archive.h | 185 | 合格（H3 的承诺需要实现来兑现） |
| 12 | src/dev_archive_eval.cpp | 366 | 有条件合格：JSON 入口无法表达 invalid（M21）；BOM 未剥（M22）；O(n²)（L5） |
| 13 | src/dev_diff.cpp | 312 | **不合格**：4 处 git 调用丢弃返回码 → 静默假绿（H2）；预算常量矛盾（L6） |
| 14 | src/dev_diff.h | 99 | 合格：问题诊断写得比实现更严（H2 的判据正来自这里） |
| 15 | src/dev_diff_parse.cpp | 336 | **不合格**：hunk 正文 `---`/`+++` 误判为文件头（H5）；未知状态字母悄悄归 modified（L7） |
| 16 | src/dev_diff_parse.h | 58 | 合格（"封闭词表"承诺随 L7 修） |
| 17 | src/dev_proc.cpp | 304 | **不合格**：`Drain` 只留头与文档矛盾（M15）；overflow 记账成超时（M16）；未传 `lpApplicationName`（M17） |
| 18 | src/dev_proc.h | 78 | 有条件合格："头+尾"承诺未被实现（M15） |
| 19 | src/dev_snapshot.cpp | 399 | **不合格**：非 `-z` 解析丢路径/取错重命名路径（H4）；git 失败仍报 ok（H4b）；同秒 id 覆盖（M18）；回滚哈希不同源（M19）；manifest 路径无校验（M19b） |
| 20 | src/dev_snapshot.h | 80 | 合格 |
| 21 | src/dev_tools.cpp | 275 | **不合格**：detach 线程无人 join（H6）；运行中作业被淘汰即丢失（M26）；文件级全局（M26） |
| 22 | src/dev_tools.h | 90 | 有条件合格：`Diff()` 重载只声明不定义（M25） |
| 23 | src/eval_surface.cpp | 390 | 有条件合格：canonical 形式依赖文档顺序（M23b）；集合比对只看一级目录（L8）；checkedFiles 重复计数（L9） |
| 24 | src/eval_surface.h | 75 | 合格：fail-closed 立场贯彻得好 |
| 25 | src/identity.h | 106 | **合格（标杆）**：品牌串唯一来源、每项取舍有据 |
| 26 | src/json.cpp | 449 | 有条件合格：`\uXXXX` 无代理对/无 NUL 处理（M2）；`//` 注释与头文件相反（M3）；重复键 first-wins（M4）；depth 早退漏减（M1） |
| 27 | src/json.h | 51 | 有条件合格：与 M3 矛盾；缺"重复键"语义说明 |
| 28 | src/layout_key.h | 60 | 合格：指纹字段的"为什么必须进指纹"逐条有据，是本仓最好的注释之一 |
| 29 | src/layout_limits.h | 24 | 合格（内容），但常量落在全局命名空间（L10） |
| 30 | src/log.cpp | 67 | 有条件合格：`_vsnwprintf` 靠零初始化兜底（L11）；行截断无标记（L12）；与 M14 同源的轮转竞态 |
| 31 | src/log.h | 17 | 合格 |
| 32 | src/main.cpp | 229 | **不合格**：空路径转发必然失败 → 开第二个窗口（H1）；`SetForegroundWindow` 未判返回值（M7）；`/select` 丢选中目标（M32）；转发不校验目标 exe（L13） |
| 33 | src/path_util.cpp | 68 | **不合格**：`\\?\C:\` 被削成 `\\?\C:`（M33）；`Shell:` 大小写（L14） |
| 34 | src/path_util.h | 87 | 合格：取舍说明非常完整 |
| 35 | src/platform_win.cpp | 73 | **不合格**：`name==nullptr` 返回成功（M12）；注册表无锁（M12） |
| 36 | src/platform_win.h | 45 | 合格 |
| 37 | src/report.cpp | 162 | 有条件合格：owner 为空时窗口跑到屏外（M27） |
| 38 | src/report.h | 18 | 合格 |
| 39 | src/str.cpp | 81 | **不合格（轻度）**：`FromUtf8(const char*)` 二次写入长度用 n 而非 n-1（M5）；`ToUtf8` 静默替换未配对代理项、无 strict 写出版本（M9）；头文件"零 Win32 依赖"不成立（M6） |
| 40 | src/str.h | 51 | 有条件合格：M6；`FromUtf8Strict` 注释提到不存在的 `ok` 变量（L15） |
| +1 | src/view_stats.cpp（跨组核对） | 379 | 有条件合格：`g_disk[26]` 文件级可变全局（M24） |
| +2 | src/view_modes.h（跨组核对） | 94 | 合格：下标即契约的表格设计正确，`ModeIndexFor` 用最近档位是对的 |
| +3 | src/app_wnd.cpp:172-190（跨组核对） | — | 有条件合格：`cbData`/magic 校验到位，但空路径返回 FALSE 引发 H1；`SetForegroundWindow` 未判返回值（M7） |

---

## 二、严重（High）—— 会产生"看起来成功但事实不符"或直接功能失效

### H1 无参数启动时"转发并激活已有窗口"永远失败，导致开第二个窗口
- `src/main.cpp:129-160` + `src/app_wnd.cpp:172-190`
- 链条：`InitialPath()`（main.cpp:83-113）在无参数时返回**空串**（注释明确"没给路径时返回空"）→ `ForwardToExisting(L"", 400)` 仍然组装并在 main.cpp:152 发出 WM_COPYDATA → 接收端 app_wnd.cpp:183-184 读到 `n == 0` 直接 `return FALSE` → main.cpp:155 `result != TRUE` → 返回 false → main.cpp:218 判断失败 → 继续 `App::Get().Run()` **新开一个进程**。
- 后果：用户双击 exe / 点任务栏快捷方式（最常见的启动形态）在已有实例时不再"激活它"，而是每次多开一个窗口。而 main.cpp 头注释第 6-7 行把"默认启动：已有窗口就…激活它"列为契约。
- 修复（二选一，需两边同改）：
  1. 发送端：`path.empty()` 时不发 WM_COPYDATA，改用一条明确"只激活"的消息（或 `cd.dwData` 带子类型 `kActivateOnly`），接收端据此 `SetForegroundWindow` 并 `return TRUE`；
  2. 接收端：`if (!n)` 时仍然激活并 `return TRUE`（但不调用 `OpenExternal`），并让 `ForwardToExisting` 以此作为成功判据。
- 建议同时补一条回归测试/探针：起一个实例 → 无参数再启动一次 → 断言窗口数仍为 1。

### H2 dev.diff 的四条 git 调用丢弃返回码，失败时静默退化成"没有改动/没有行级差异"
- `src/dev_diff.cpp:154-155`（`ls-files`）、`:167`（`--numstat`）、`:172-174`（patch）——只有 `:131` 的 `rev-parse` 检查了 `return && code == 0`。
- 失败路径的后果：
  - `untrackedText` 为空 → `entries` 里没有未跟踪文件，而 `rep.ok` 仍为 true → "agent 写的新文件一个字都看不到"正是 dev_diff.h:10-12 列的旧失明第 1 条。
  - `numText` 为空 → `added/deleted` 全 0；`sections` 为空 → `:271-283` 命中 `fd.added == 0 && fd.deleted == 0` 分支 → `fd.truncated` 保持 **false**，`reason = L"没有行级差异（纯重命名或只改了文件模式）"`。于是一次 git 故障被报告成"这个文件确实没改内容"。
- 修复：每个 `GitIn` 都判 `bool && code == 0`；任一失败 → `rep.ok = false; rep.error = L"git <子命令> 失败"`（而不是给一份看起来完整的空报告）。这与 dev_diff.h:95-96 自己写的"**不编造数据**：git 不可用就报错"完全一致，只是没落实。

### H3 archive 的原始轨迹被"解码→再编码"落盘，与 manifest 里的 sha256 不是同一份字节
- `src/dev_archive.cpp:228-236`（写盘 + 算哈希）、`:321-333`（读回）、`:309` 的 manifest 声明
- 事实：`tf::WriteText(tmp + "\\trajectory\\raw.txt", tf::FromUtf8(opt.rawTrajectory))` 先把 `std::string` 用**宽松** UTF-8 解码成 `wstring`（非法字节→U+FFFD），再 `str::ToUtf8` 编码回字节写盘；而 `rawHash = Sha256Of(opt.rawTrajectory)`（dev_archive.cpp:233）算的是**原始**字节。
- 后果：只要轨迹里有任何非 UTF-8 字节（中文 Windows 上编译器/测试脚本的 GBK 输出，非常常见），`trajectory/raw.txt` 的内容就与 `manifest.json` 里记的 sha256 不符。而这套设计的全部价值是 dev_archive.cpp:5-6 写的"任何'看起来像原始但被改过'的痕迹都能靠哈希对出来"——现在正常写入就会对不上，等于这条判据失效。
- 修复：新增字节级写盘（`tf::WriteAllBytes`，直接 `CreateFileW` + `WriteFile` + `FlushFileBuffers`），写盘与哈希都用 `opt.rawTrajectory`；`ReadTrajectory` 改用 `tf::ReadAllBytes` 返回原始字节。顺带修 `dev_archive_eval.cpp:50-73`（GBK 轨迹必然"缺少汇总行"）。修完应加一条单测：写入含 `\xD6\xD0` 的轨迹 → 重新算出文件哈希 == manifest 记录值。

### H4 快照取改动文件用非 `-z` 的 porcelain：带空格路径被丢、重命名取到旧路径
- `src/dev_snapshot.cpp:118-142`
- 两个独立缺陷：
  1. `:136` `if (!path.empty() && path.front() != L'"') out.push_back(path);` —— `git status --porcelain`（非 `-z`）对含空格/引号/控制字符的路径**加双引号并转义**，这里直接把这类文件**丢掉**。而 `core.quotepath=false` 只解决非 ASCII，不解决空格。
  2. `:132-134` 注释写"重命名行是 `new -> old`，取新的那个"，实现 `path = path.substr(0, arrow)` 取的是**旧**路径（porcelain 是 `XY PATH1 -> PATH2`，PATH1 为旧）。旧路径在磁盘上通常已不存在 → 快照记成 `bytes=0` + "文件已不存在"，而真正的新文件完全不在快照里。
- 后果：快照"成功了"，回滚时这些文件不会被动过；而且被丢掉的文件连 `saved=false` 都不会留一行，事后无从发现少了什么。
- 修复：与 `dev_diff.cpp` 一致改用 `-z` + `parse NUL fields`（`git status --porcelain -z` 里重命名是 `XY 新路径\0旧路径\0`，方向与非 -z 相反，注释里已记过这个坑）。可直接复用 `devdiff::ParseNameStatusZ` 同族的解析，避免第三份路径解析。

### H4b 快照在 git 不可用/失败时仍然报 ok=true、零文件
- `src/dev_snapshot.cpp:236-307`：`ChangedFiles()` 里的 `GitArgs` 丢弃 `code`（`:74-79`），git 起不来时返回空串 → `changed` 为空 → 循环不执行 → `manifest.json` 写出、`s.ok = true`。
- 这正是 dev_archive.h:13 与 devtool 全族反复强调的"没有证据 ≠ 证据支持"。
- 修复：`Create()` 开头先 `git rev-parse --show-toplevel` 校验（code==0），失败即 `s.error`；另外 `changed.empty() && tracked.empty()` 时也要报错（两种情况必须能分开：真干净 vs 读不到）。

### H5 hunk 正文行以 `--- ` / `+++ ` 开头时被误判成文件头
- `src/dev_diff_parse.cpp:279-286` 在 `:318` 的 `if (inHunk)` **之前**，对每一行无条件匹配 `--- `/`+++ `。
- 触发条件很普通：被删除的行内容以 `-- ` 开头（SQL/Ada/Lua 注释 `--`、Markdown 分隔线 `--- `），或新增行以 `++ ` 开头（`++i;` 写法前带 `+`？实际是新增行内容以 `++` 开头，如 `++counter;`）。
- 后果：该行既没进 `hunk.text`（内容丢失），又把 `oldPath`/`newPath` 覆盖成垃圾（`StripPrefix` 后的半截行），`flushSection` 于是给这个段落一个**错误路径**，`Compute` 里 `sections.find(e.path)` 落空 → 该文件报"git 没有给出这一项的 diff 段落"或错配到别的文件。
- 修复：`--- `/`+++ ` 只在 `!inHunk` 时识别；更稳的做法是按 hunk 头里的 `oldLines/newLines` 计数消费正文，计满才退出 hunk。修完补单测：删除一行 `-- a comment` 的 cpp，断言该行出现在 hunk.text 且段落路径不变。

### H6 dev 作业线程 detach 后无人跟踪，退出期可能用到已销毁对象
- `src/dev_tools.cpp:129-166`：`std::thread([...]{ ... LogDbg(...); ai::events::Publish(L"dev.job.done", data); }).detach();`
- 线程里用到 `tf::ExeDir()`、`LogDbg`（文件 IO）、`ai::events::Publish`（订阅者表在 App 撤销时清理）、`g_jobs`。`App::OnDestroy` 的 `ShutdownJobs()`（app.h:107-108 说明"取消全部作业并 join 工作线程"）**join 不到**这些 detach 线程。
- 后果：关窗口时若还有 build/test 在跑，进程可能在 `Publish` 阶段踩到已析构的订阅者（闪退），或留下孤儿进程树（作业对象的 KILL_ON_JOB_CLOSE 在 dev_proc 里，随函数返回已关，靠 timeout 才会杀树）。
- 修复：把 `std::thread` 存进 `g_jobs[id].thread`（`Job` 加 `std::thread`，或另一张 `map<id, thread>`），`ShutdownJobs()` 里先置 cancel 再 `join`；中间态的 `Job` 拷贝需拆分"线程句柄"与"只读快照"两个结构。

### H7 app.cpp 超出文件行数棘轮基线（已实测）
- `src/app.cpp` = **871** 行，`data/file-length-baseline.json` 记账 856 行；`python tools/check_components.py` 实测输出：
  ```
  ✗ 单文件 ≤500 行
      aichat_panel.cpp  662 行 / aichat_webview.cpp  673 行 / app.cpp  871 行
      nav_bar.cpp  552 行 / settings_dialog_model.cpp  560 行
  ```
- 我这组只认领 `app.cpp`（本组内其余 39 个文件均未超 500）。这是硬违反（AGENTS.md §3 / common.h:5 都写了 ≤500）。
- 修复：把 `App::OnCreate`（app.cpp:113-405，约 293 行）整段搬到 `app_create.cpp`，只留 `CreateMainWindow` / `Run` / 生命周期。搬完顺带去棘轮（`--update-length-baseline`）。注：同批还有 4 个外组文件在涨，说明棘轮报警没被当成门。

---

## 三、中（Medium）—— 语义/契约不一致，或可被现实输入触发

### M1 json：`depth` 在早退路径上不减，深度预算可被吃掉
- `src/json.cpp:232-236` `if (++depth > 64) { Fail(...); return false; }` 没有 `--depth`；`:239-242`（意外结尾）同样直接 return。
- 当前"解析失败即整体放弃"，所以只是潜在缺陷；但 `Parser` 是值对象，将来若做"容错续解"或单测复用同一 Parser 就会误判嵌套过深。
- 修复：把 depth 做成 RAII（进入 `ValueAt` 构造 `DepthGuard`，析构 `--depth`）。

### M2 json：`\uXXXX` 不做代理对配对/校验，NUL 也照收
- `src/json.cpp:174-193`：`out.push_back(static_cast<wchar_t>(code))`，`code ∈ [0, 0xFFFF]`。
- 后果：
  - 合法代理对（`\uD83D\uDE00`）在 Windows 上恰好拼成一对 `wchar_t` —— **能工作**；
  - 孤立代理（`\ud800`）成为非法 UTF-16 单元 → `str::ToUtf8` 编码时被替换成 `?`/U+FFFD → 写回文件时**内容已变**，round-trip 断；
  - `\u0000` 生成含 NUL 的 `std::wstring`，任何下游 `.c_str()` 都会截断（settings 里一个值被截半）。
- 修复：高代理后必须紧跟 `\uDC00-\uDFFF`（否则 `Fail(L"孤立的高代理")`）；低代理前面必须是高代理；`\u0000` 明确 `Fail`（JSON 里合法但本工程没有承载它的字符串 API，收下就是埋雷）。补两条单测。

### M3 json：`Ws()` 支持 `//` 注释，与 json.h 首行"刻意不做注释"直接相反
- `src/json.cpp:134-138` vs `src/json.h:3`。
- 这是"放宽了但没记账"的典型：读到注释支持的人会以为写文档的人忘了；读到文档的人会以为注释会报错。两者都错。
- 修复：保留实现（对用户手改 settings 有利）→ 改 json.h 第 3 行为"刻意不做：流式、数字精度保持、NaN（`//` 行注释是刻意的宽容）"，并补单测固定该行为。

### M4 json：重复键 first-wins，且解析产物保留重复键
- `src/json.cpp:265` 用 `emplace_back` 无条件追加；`Value::Find`（`:30-35`）返回**第一个**匹配；`Value::Set`（`:56-65`）替换第一个。
- 后果：手改 settings.json 时写了两个同名键，生效的是**先写的那个**（业界普遍是后写覆盖）；再 `Stringify` 会把两个都写回去，形成"改不掉的僵尸键"。
- 修复：`Parse` 的对象分支里先 `Find(key)`，命中则替换并计一条警告（`err` 里累加）；或者在 json.h/J1 文档里显式写死 first-wins 并加单测。二者选一，别默认。

### M5 str：`FromUtf8(const char*)` 第二次调用把含 NUL 的长度传给只有 n-1 容量的缓冲
- `src/str.cpp:55-58`：`n` 是**含**终止符的宽字符数；`std::wstring out(n - 1)`；第二次调用仍传 `n` 作为 `cchWideChar`。
- 实际不越界：写入落在索引 `0..n-1`，而 `n-1 == out.size()` 正是 C++11 保证可写 `'\0'` 的终止符槽，MSVC 实现也总有 `size()+1` 的存储。但这是"靠标准的一个脚注活着"的写法，静态检查器会报，改一行就能消除：
- 修复：`MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], static_cast<int>(out.size()))`。

### M6 str.h 的"零 Win32 依赖 / 可放进单测"承诺不成立
- `src/str.h:8` vs `src/str.cpp:4`（`#include <windows.h>`、`WideCharToMultiByte`、`MultiByteToWideChar`）。
- 影响：这套"纯函数"只能跑 Windows 单测；且 `Trim`/`Lower` 用 `iswspace`/`towlower`（受 CRT locale 影响）—— 与注释"towlower 口径，不做 locale 之外的处理"其实有歧义（"C" locale 下非 ASCII 不变）。
- 修复：改文案为"无 MSVC 之外的依赖；UTF-8 转换经 Win32（可单测，仅 Windows）"，并把 `FromUtf8Strict` 的失败码（1113）写进注释。

### M7 main/app_wnd：`SetForegroundWindow` 的返回值从不检查，而 Win32 明确允许它失败
- 三处调用都是"当它一定成功"的写法：`src/main.cpp:157-159`（转发端激活目标）、`src/app_wnd.cpp:186-190`（接收端激活自己）、`src/hotkey_manager.cpp:255`（热键唤起主窗）。
- `SetForegroundWindow` 受前台锁（foreground lock）约束：进程不是前台、没收到最近一次输入、也不是被前台进程启动时，调用**直接返回 FALSE 且不报错**。`main.cpp:158` 恰恰是最容易命中的场景 —— 由后台进程/计划任务/带 `/select` 的第三方启动器拉起的新实例，想去激活那个老窗口。全仓没有任何 `AllowSetForegroundWindow` / `AttachThreadInput` 兜底（已 grep 确认）。
- 后果：`ForwardToExisting` 返回 true → 新进程 `return 0` 退出 → 老窗口没有前置。用户看到的是"双击图标，任务栏闪了一下，什么也没发生"——比 H1 的"多开一个窗口"更难排查，因为它连新窗口都不给。
- 修复：判返回值；失败时退到 `FlashWindowEx(FLASHW_ALL)` 让用户至少知道窗口在哪，并 `LogDbg` 记一条。跨进程这条路径还可以在 `WinMain` 里先 `AllowSetForegroundWindow(pid)`（`GetWindowThreadProcessId` 取 pid）再发送，这是微软文档给出的正解。
- 顺带（可并入本条）：`ApplyDarkTitleBar`（common.h:274-278）两次 `DwmSetWindowAttribute` 的返回值都丢弃 —— Win7/8 上属性号 19/20 都不存在，深色标题栏静默不生效、无日志。降级为 Low 处理即可（改 `if (FAILED(a) && FAILED(b)) LogDbg`）。

### M8 common.h `DpiOf`：没有系统 DPI 回退，Win7/8 永远 96
- `src/common.h:207-212`。`GetDpiForWindow` 是 1607+；老系统上 `fn == nullptr` → 常量 96 → 125%/150% 缩放下所有 `Theme::S()` 都按 96 算（窗口偏小、字号偏小），且**没有任何日志**说明为什么。
- 另：`GetProcAddress(GetModuleHandleW(L"user32.dll"), ...)` 未判空句柄（user32 必然已加载，属理论风险）。
- 修复：`fn ? fn(h) : GetDpiForSystemFallback()`，回退顺序 `GetDpiForSystem`（同样动态取）→ `GetDeviceCaps(LOGPIXELSX)`（按窗口所在显示器 HDC）。

### M9 str：`ToUtf8` 会把未配对代理项静默换成 U+FFFD，且没有 strict 写出版本
- `src/str.cpp:33-42`：`WideCharToMultiByte(CP_UTF8, 0, ...)` 不带 `WC_ERR_INVALID_CHARS`。NTFS 文件名是 UTF-16 码元序列，**允许**出现未配对的代理项（`CreateFileW`/`FindFirstFileW` 能建能读，只是编码不成 UTF-8）；这种名字经这里出去会变成 U+FFFD，而函数**返回成功**。
- 读方向有 `FromUtf8Strict`（`:62-79`，带 `MB_ERR_INVALID_CHARS`），写方向没有对应物 —— 单向严格。而写方向的出口恰好都是"要留证据"的地方：`audit_log.cpp:155/160`（`audit.jsonl` / `audit.log` 的全部路径与参数）、`dev_archive.cpp:331`（归档 manifest 里的路径）。落盘后变成 `\uFFFD`，**再读回来就是另一个路径**，而审计日志的全部价值就是"事后能凭它复原当时操作了什么"。
- 与 M2 同源：`json.cpp` 的 `\uXXXX` 不做配对，于是"写出去是替代符、读回来是两个 `\uFFFD`"这条链两头都不报错。
- 修复：加 `bool ToUtf8Strict(const std::wstring&, std::string*)`（`WC_ERR_INVALID_CHARS`），审计与归档两处改用它，失败时把该字段写成 `"<不可编码路径>"` 并置 `ok=false`；`str.h` 里"宽松/严格"两组各写一行口径。**注意别顺手把所有调用点都改成严格**：`ai_http.cpp:115`、`mcp_stdio.cpp:215` 这类协议出口被严格化会因为一个坏字符整条消息发不出去，反而是退步。

### M10 common.h `CountText` 把 UI 文案放进"只放类型/常量"的共享头
- `src/common.h:240-242`：`std::to_wstring(n) + L" 个项目"`。
- 该文件第 1-5 行自己规定"只做语言级共享，不含业务"，而这是**展示文案**：不该由它决定"个"量词、不该在被 3 个模块共享时无法改措辞。现状只有 `app_stats.cpp:96` 一处调用（比预期少）。
- 修复：把 `CountText` 移到 `status_bar` 的文案层（或 `ui_text.h`），并顺手支持千分位（10000 项可读性）与"0 个项目"的空态。若坚持留在 common.h，至少把第 1 行纪律改成"类型/常量/纯内联工具/统一文案"。

### M11 common.h `CopyDataPayload.path[1024]` 是误导性残留
- `src/common.h:152-155`。main.cpp:140-148 早已改成"头 + 实际字符数"动态缓冲，接收端 app_wnd.cpp:181-185 也按 `cbData` 读 —— `path[1024]` 现在**只起 `offsetof` 锚点作用**。下一个人看到 1024 会以为有长度上限（这个 bug 曾经真实存在，注释里还留着）。
- 另有一个真实缺口：`offsetof(CopyDataPayload, path)` 在 x86 是 4、x64 是 8。若同机同时存在 32/64 位构建，发送端 cbData 与接收端的 offset 不一致 → 从错位处读路径（可能读到半截或越界前的垃圾），`cbData >= kPathOff + sizeof(wchar_t)` 挡不住。当前只发 x64，所以是潜在问题。
- 修复：把 `path[1024]` 改成 `wchar_t path[1];`（或去掉结构体、直接"magic 4/8 字节 + 路径"），并在 `cd.dwData` 里再塞一个 `kCopyDataVersion`（版本 + 位宽），接收端不匹配就拒绝。这样 H1 的"激活"子类型也顺便有了位置。

### M12 platform_win：`name == nullptr` 被当成注册成功；注册表无锁
- `src/platform_win.cpp:21` `if (!name || Registered().count(name)) return true;` —— 空类名返回 **true**，调用方（app.cpp:52/115、report.cpp:94 等）随后 `CreateWindowExW` 必然返回 NULL，且没有任何日志 → 表现为"窗口没出来"而无原因。
- `:13-16` `Registered()` 是无锁 `static std::unordered_set`。若工作线程注册窗口类（dev 进度窗、作业 UI 属于这类场景），并发 insert/count 是数据竞争（可以直接崩在哈希表里）。头文件 platform_win.h:10-11 只说了"≤3 个类"的目标，没说"只许 UI 线程调用"。
- 修复：`if (!name) return false;`；`Registered()` 外面加 `static std::mutex`（或断言 `GetCurrentThreadId()==main`）。

### M13 audit_log：`thread_local` 上下文是隐式契约，跨线程即静默丢字段
- `src/audit_log.cpp:34` `thread_local Context g_ctx;` + `:64-68` 由 `MakeRecord` 取值。
- 现状**正确**：AI 请求经 `ai::PumpRequests()` 在 UI 线程泵（app_wnd.cpp:82，WM_TIMER 内）执行，`AiScope` 与 `CommandBus::Invoke` 同线程，我已核对调用链。但这是"靠调用位置"成立，代码里没有任何断言或兜底。
- 后果（一旦有人把泵挪到工作线程）：`sessionId`/`traceId` 变成 `-`、`approval` 变成 `not_required` —— 审计里"谁批的"消失，而且不会报错。
- 修复：`MakeRecord` 里若 `ctx.sessionId.empty() && c.actorKind == L"ai"` 就 `LogDbg` 一条告警；或在 `AiScope` 构造时记下创建线程 id，`MakeRecord` 比对不一致即标 `contextLost=true`。

### M14 audit/log：两条审计写入无串行化，且与调试日志共用同一个非原子轮转计数器
- `src/audit_log.cpp:149-169`：每条记录做**两次** `tf::AppendLine`（各自 `CreateFile`/`WriteFile`/`Close`）；两次都可能触发轮转。
- `src/log.cpp:34` 与审计共用 `tf::AppendLine`，而它的 `static int sinceCheck`（text_file.cpp:171）是**跨 path、跨线程共享的非原子全局**：AI 线程（dev 作业）、UI 线程、可能的 mcp 线程同时追加时，① 计数器竞争（丢计数，无正确性问题）；② 一方正在写、另一方 `MoveFileEx` 改名（轮转）→ 写失败或整行丢失。
- 顺带：`checkEveryN` 的语义也被这个共享计数器破坏 —— 调用方分别传 500（log.cpp:34）与 200（audit_log.cpp:157），但计数只有一份，谁先到阈值就 `sinceCheck = 0`，于是另一方永远等不到自己的 500。头文件 text_file.h:44 的描述（"每 N 次调用才查一次大小"）对每个 path 单独看并不成立。无害（只是 stat 频率变了），但属同一类"文档说的和代码做的不同"。
- 修复：`AppendLine` 内加一把 `static std::mutex`（最简单），并把 `sinceCheck` 改成 `std::atomic<int>`；更彻底的是审计单独一把文件锁、单写队列。

### M15 dev_proc `Drain` 只保留头部，与头文件"头 + 尾都要"相反
- `src/dev_proc.cpp:20-34`：超限即 `append` 到上限后 `return false`；**尾部从不追加**。
- `src/dev_proc.h:73-76` 明确写"截断时保留头部与尾部：编译错误既可能出现在前面…也可能出现在后面（汇总），只留尾巴会把第一处报错丢掉" —— 结论是"两头都要"，实现只做了一半，而 `dev_tools::TrimOutput`（dev_tools.cpp:44-56）又做了一次头+尾裁剪，所以最终用户看到的是"头 1400 字 + 尾 2552 字"，但**中间被 256KB 上限截掉的部分并不在尾部区间里**（尾部也一起被丢），于是"总损失了多少"无法计数。
- 修复：`Drain` 改成分段：前 `kMax/4` 直接 append，之后环形保留后 `3kMax/4`，结束时拼 `head + 标记 + tail`；`overflow` 仍置位。

### M16 dev_proc：输出超限导致的强杀被记成"超时"
- `src/dev_proc.cpp:240-256`：`overflowAt` 宽限 10s 到点也走 `killedByTimeout = true; *timedOut = true;`。
- 于是 `dev_tools.cpp:145-148` 报 `L"作业超时（超过 N 分钟…）"`，而 N = `timeoutMs / 60000`（可能是 0）。用户/agent 拿到的是**错误的归因**（"编译太慢"而不是"输出太多"），dev_tools.h:38 承诺的"与跑失败是两回事"实际变成了三种混两种。
- 修复：`Job` 加 `bool outputOverflowKilled`，`RunCaptureTimeout` 增加出参或在 `err` 里区分；reason 分别给"超时被杀"与"输出超过 256KB 且子进程未退出，已杀整棵树"。

### M17 dev_proc：`lpApplicationName = nullptr` + `L"git"` → 走含当前目录的搜索顺序
- `src/dev_proc.cpp:171` `CreateProcessW(nullptr, mutableCmd.data(), ...)`。
- 本项目的主场景正是"在开发机上跑 agent 写的代码 / 审 agent 改的仓库"。当父进程 CWD 是仓库（`tests/run_all.sh`、`build_fast.py` 都从仓库根启动）时，仓库里出现一个 `git.exe` 就会被执行。这与模块头"注入面直接消失"的宣称不完全一致——注入面消失了，**可执行文件替换面**还在。
- 修复：`devproc::RunCapture` 内部先 `SearchPathW`（或 `GetSystemDirectory` + PATH 遍历）解析成绝对路径再传 `lpApplicationName`；解不出就失败（fail-closed）。已有现成实现：`dev_tools.cpp:71-76 FindExe`，只是没被复用。

### M18 dev_snapshot：同秒内两次快照共用同一 id → 第一份被静默覆盖
- `src/dev_snapshot.cpp:241` `id = L"snap-" + NowStamp() + L"-" + pid`（秒级 + 进程号）。同进程同一秒内连做两次（脚本化流程很常见）→ 目录相同 → `EnsureDir` 返回 true（已存在）→ `CopyFileTo(..., FALSE)` 覆盖 → `manifest.json` 覆盖 → 两次都 `ok=true`。
- 对比 `dev_archive.cpp:187-190` 会报"候选已存在"，这里连报都没有。
- 修复：id 加单调序号（`g_seq`）或毫秒；或 `if (tf::Exists(s.dir)) { s.error = ...; return s; }`（fail-closed）。

### M19 dev_snapshot：回滚的 before/after 用 git blob SHA-1，而快照 sha256 用 SHA-256 —— 同一功能两套哈希
- `src/dev_snapshot.cpp:172-177 HashOf()` 走 `git hash-object`；`:153-157 Sha256Of()` 走 filehash。`Restore` 返回的 `before/after`（`:380-388`）与 manifest 里 `files[].sha256` 无法互相比对，调用方"用 before/after 核对 sha256"会永远不等。
- 另外 `HashOf` 每文件 **2 次进程启动**（before+after）；恢复 100 个文件 ≈ 200 次 `CreateProcessW`，比文件 IO 本身贵两个数量级。
- 修复：before/after 直接用 `filehash::HashFile(Sha256)`（一次本地读，复用已有结果）。

### M19b dev_snapshot：`AbsOf` 声称"拒绝越出仓库的路径"，实际不校验；manifest 可指向仓库外
- `src/dev_snapshot.cpp:88-94` 注释与实现不符（只做斜杠替换 + 拼接）。
- `Restore`（`:348-361`）用 manifest 里的 `f.path` 拼绝对路径 `CopyFileTo(源, abs)` → **写**文件。manifest 在 `<repo>/snapshots/` 下，属 agent 可写区域；一份手改的 manifest（`"path": "..\\..\\Users\\x\\Desktop\\a.txt"`）配合 `dev.rollback` 就能覆盖仓库外任意文件。`dev_archive.cpp:141-143` 有这套校验（`..`/`:`/前导分隔符），这里完全没有 —— 同一工程两套口径。
- 修复：把 `dev_tools::ValidRepoPath`（dev_tools.cpp:80-102）提成公共函数，`AbsOf` 里强制调用并返回空串表示拒绝；`Restore` 对空结果报错而不是静默跳过。

### M20 dev_archive：复制/哈希失败一律 `continue`，归档可能缺文件却看起来完整
- `src/dev_archive.cpp:146-149`（`EnsureDir`/`CopyFileIfMissing`/`HashFile` 失败都 `continue`），`:153` `a.bytes = f.bytes`（用的是 diff 报告里的字节数，不是实际复制结果）。
- 后果：候选归档少文件；`r.files` 变小；`RunFunnel` 的"200 文件 / 16MB"上限按缺文件后的清单算；评审看到的是"改动 = 这几个文件"。与 dev_archive.cpp:3-6"逐字保存、能对出痕迹"的口径冲突。
- 修复：任一复制/哈希失败 → `*err = L"归档失败：" + rel` 并让 `Record` 整体失败（已有 tmp 目录 + 改名机制，正好支撑 fail-closed）。

### M20b dev_archive：失败路径残留 tmp 目录（"不留半份记录"只做到了一半）
- `dev_archive.h:156` 承诺"任何一步失败都不留下半份记录（先建临时目录，成功后再改名）"；`dev_archive.cpp:210-218` 建 tmp 后，`:221-311` 的每个 `return r;` 都不删 tmp（只有 `:314` 的改名失败在文案里承认"临时目录保留"）。
- 后果：`.cand-xxx.tmp-<pid>` 会一直留着（`List` 因 `SafeId` 拒 `.` 而看不见它，等于隐形垃圾）；反复失败会累积。
- 修复：一段 `auto cleanup = [&]{ DeleteTree(tmp); }` + 每条失败路径调用（或 RAII guard，`r.ok` 时才 release）。

### M21 dev_archive_eval：JSON 版 `Decide` 无法表达"这一对不可用"，取值域也不校验
- `src/dev_archive_eval.cpp:150-167`：`Trial t; ... t.valid` 恒为默认 `true`；`candidate/baseline` 只要求 `IsNumber()`。
- 后果：这个模块最核心的性质——"有不可用配对就 fail-closed"（`:116-123`）——在 JSON 入口根本不可达：调用方把超时/对不上的试验**剔除后**再传数组，E 只会在好数据上增长。`100 vs 99` 也算一胜（只看符号）。
- 修复：JSON 支持 `{task, candidate, baseline, valid?:bool, reason?}`；`valid===false` 直接计入 `invalid`；`candidate/baseline` 限定 `{0,1}`（或加 `domain` 参数）。

### M22 dev_archive_eval：`ParseTrajectory` 不剥 UTF-8 BOM
- `src/dev_archive_eval.cpp:177-221`：逐行 `Trim` 只去空格/制表/CR。带 BOM 的轨迹（PowerShell `>` 重定向、记事本另存）首行是 `\xEF\xBB\xBFPASS xxx`，`compare(0,5,"PASS ")` 不匹配 → 该测试消失；若汇总行里的计数包含它，就会触发 `:238-242` 的"汇总与逐项对不上（可能被人改过）" —— 诊断信息把人指向"篡改"，真实原因是编码。
- 修复：函数入口跳过 3 字节 BOM；顺带按 `\r\n`/`\n` 双分隔处理（现在只按 `\n` 切，靠 `Trim` 去 CR，够用但脆）。

### M23 assert_check：`=` 用大小写不敏感比较，`$changed` 用大小写敏感比较
- `src/assert_check.cpp:166-171`（`actual->IsString()` → `str::IEquals`）vs `:252`（`r.actual != r.before`，逐字符）。
- 后果：把标签别名从 `foo` 改成 `Foo` —— 断言 `activeTabAlias=$changed` **通过**（文本不同），断言 `activeTabAlias=foo` **也通过**（大小写不敏感），断言 `activeTabAlias!=foo` **失败**。同一门语言对"变没变"给出两个答案，而这正是断言层要判定的东西。
- 修复：选定一个口径并写进 assert_check.h 的"运算符"段：路径类字段大小写不敏感是对的，但那应该由**字段元数据**决定（`pathFields` 白名单），不能由"值恰好是字符串"决定。

### M23b eval_surface：`CanonicalForm` 的注释说"键一律排序"，代码一处也没排
- `src/eval_surface.cpp:159-202`。注释（`:148-158`）承诺 `F`/`D` 行"键一律排序、分隔符是单个空格"，但两个循环都直接 `for (const auto& kv : files->obj)`（`:170`）/ `dirSets->obj`（`:181`）迭代 —— 而 `json::Value::obj` 是 `std::vector<std::pair<...>>`（json.h:17），**保持解析时的插入顺序**，不排序。`dirSets` 里 `files` 数组也是按原序 `,` 拼接（`:193`），没有排序。
- Python 端是排的：`tools/gen_eval_baseline.py:121/123/125` 用 `sorted(obj.get("files"))` / `sorted(obj.get("dirSets"))` / `",".join(sorted(entry["files"]))`，落盘还带 `sort_keys=True`（`:288`）。
- 目前两边一致**纯属巧合**：清单一直是 Python 生成的，所以磁盘上的顺序本来就是排好的。但"逐字节一致"是这道门的全部意义（注释里自己写了"C++ 与 Python 不可能逐字节一致，一旦不同这道门就永远拒绝构建"）—— 而 C++ 侧实际上并没有实现这个一致，只是没被挑战过。任何一次手改 `data/eval-surface.json`（改一个哈希值、加一行 `files`）而不重排顺序，应用内校验算出的 selfSeal 就与 `check_ai_meta.py` 算出的不同：一边拒、一边放。
- 修复：`CanonicalForm` 里对 `obj` 的 key 取出来 `std::sort` 后再迭代（`dirSets` 的 `files` 数组同样排序），并加一条单测：把清单文件的键顺序打乱后，selfSeal 必须不变。这条比 M4（重复键）更值得修，因为它守的是唯一的运行期信任根。

### M24 view_stats：`g_disk[26]` 是文件级可变全局（且无锁）
- `src/view_stats.cpp:24`。app.h:476-479 刚把 `RefreshStats` 的函数内 static 搬成成员，理由正是"static 违反组件内不得有文件级可变状态"。这里同样的问题没搬。
- 修复：把 `DiskEntry g_disk[26]` 挂到 App（或 `StatsPump` 结构）里，与 `statsTick_/statsLastPath_` 同处。

### M25 dev_tools.h：`std::wstring Diff();` 只有声明没有定义
- `src/dev_tools.h:79` 声明"返回 JSON 文本"的 `Diff()`，`dev_tools.cpp` 只定义了 `bool Diff(const std::wstring&, json::Value*, std::wstring*)`（`:191`）。当前唯一调用点（app_commands_dev.cpp:140）用的是后者，所以链接得过；但只要有人按头文件调用，就是链接错误。注释里列的字段（`stagedPatch/patchTruncated/stat`）实现里也没有。
- 修复：删掉这段死声明与过时注释（"文档承诺了没实现的东西"比没文档更糟）。

### M26 dev_tools：运行中的作业会被淘汰，完成状态静默丢失
- `src/dev_tools.cpp:118-126`：`while (g_jobs.size() > kMaxJobs)` 找不到 done 就 `g_jobs.erase(g_jobs.begin())` —— 被删的很可能是**正在跑**的作业；其线程结束于 `:153-154` 找不到 id，于是 `done/output/exitCode` 与 `dev.job.done` 事件（`:165`）全部丢弃。订阅者只看到"作业消失了"，`GetJob` 永久返回 false。
- 另外 `g_mu/g_jobs/g_nextId`（`:26-29`）是文件级可变全局，与 M24 同一条纪律。
- 修复：淘汰时先 `it->second.evicted = true` 并发布一条 `dev.job.evicted` 事件（或直接拒绝启动新作业，报"队列已满"——fail-closed 更符合 dev_* 全族的立场）。

### M27 report：owner 为空时窗口落在屏幕外
- `src/report.cpp:105-110`：`RECT orc{}` 未初始化成功时全 0 → `x = (0 - w)/2 = -w/2`、`y = -h/2`。当前调用方大概都传 owner，但 `ShowReport(HWND owner, ...)` 的签名允许 nullptr，且没有兜底。
- 修复：`if (!owner || IsRectEmpty(&orc))` 用主显示器工作区居中（`SystemParametersInfoW(SPI_GETWORKAREA)`）。

### M28 app.h 499 行贴顶
- `src/app.h` = 499，上限 500。`layout_key.h:4-5` 已自陈"app.h 已贴近上限"，说明这是已知债。
- 修复：下次动 `App` 时先把 `Pane`/`Tab` 两个内嵌结构抽到 `app_types.h`（约 40 行），给后续会话留余量；否则下一次功能必然又撞 H7 那类棘轮。

### M29 app.cpp 品牌串硬编码，违反 identity.h 的 B1 规则
- `src/app.cpp:655` `MessageBoxW(..., L"退出 ExBarBrowser", ...)`。
- `identity.h:11` 明确"其他文件出现 `L"ExBar…"` 字面量即违规（见 docs/NAMING.md B1）"。全 `src/` 机检（`grep -rn 'L"[^"]*ExBar' src/ | grep -v identity.h`）得到 3 处真实字面量 + 1 处注释：本处、`ai_http.cpp:125`（WinHttpOpen 的 User-Agent，外组）、`aichat_webview.cpp:733`（"找不到 WebView2Loader.dll（应与 ExBarBrowser.exe 同目录）"，外组），以及 `shell_view.cpp:237` 被注释掉的 `SetPropertyBag`。前两处外组的一并报给对应组。
- 修复：`std::wstring(L"退出 ") + id::kBrand` 或新增 `id::kQuitTitle`；WebView2 那条建议改成 `id::kBrand + L".exe"`（顺便保证改名时提示不失真）。并建议在 `check_components.py` 里加一条机械规则"`L\"…ExBar…\"` 只许出现在 identity.h（注释除外）"——现在靠人眼，已经漏了 2 处。

### M30 app.cpp 会话恢复的下标错位（左窗格在第 0 条失效时错一格；右窗格历史另有一套游标）
- `src/app.cpp:434-467`：恢复时对失效路径 `continue`（`:440`），但 `sessionActive` 直接当标签下标用（`:469-470` `tabSet_.IdAt(idx)`）；`:478-481` 的 `pendingSelPath_` 又用 `s.sessionPaths[sessionActive]` 取路径。
- 后果一：会话里第 0 条路径已被删时，`sessionActive=0` 会激活"原本的第 1 个标签"，选中恢复也挂到错误的目录（或直接不恢复）。
- 后果二（同一函数的第二处，`:437/453-465`）：右窗格历史用一个**只在路径仍有效时前进**的游标 `pane2HistIndex`，而保存端（`:685-692`）是"p2 非空就 push 一份历史"——两边口径差一个条件。只要某个右窗格目录事后被删（`tf::Exists` 失败），`++pane2HistIndex`（`:464`）不执行，其后**所有**标签的右窗格"后退/前进"历史整体错位一格：用户在自己熟悉的目录里按 Alt+← 会跳到别的标签的历史里。
- 修复：恢复时维护 `vector<int> savedToTabIndex`（保存下标 → 实际 id），`sessionActive` 先映射再激活，取不到就退回"激活最后一个"；右窗格把 `pane2HistIndex` 的 `++` 移到 `if (!p2.empty())` 之后、存在性判断之外（即与保存端同一口径：非空就占一个位）。

### M31 dev_archive 自建 `ParentOf` / `Normalize`，与 nav_path.h / pathutil 重复
- `src/dev_archive.cpp:24-33`。`common.h:226-235` 的注释是"唯一的实现在 nav_path.h…不保留第二份实现：以前 common.h 与 NavModel 各有一套 ParentOf/LeafOf，规则不一致导致'上一级'和'地址栏补全'算出不同结果"。
- 这里第 24-27 行的 `ParentOf` 与 `nav::ParentOf` 就是两份实现（对 `\\server\share`、`C:\`、纯相对路径的行为未必一致）。
- 修复：直接 `#include "nav_path.h"` 用 `nav::ParentOf` / `nav::Leaf`；`Normalize` 用 `pathutil::ToShellPath`（若只需要反斜杠，就在 pathutil 里加一个显式的 `ToBackslashes`）。

### M32 main.cpp `/select` 丢掉"选中哪一项"
- `src/main.cpp:91-103`：`/select <file>` 与 `/select,<file>` 都只计算 `ParentOf(full)` 然后打开父目录，`full` 的末段（要选中的文件名）被丢弃。
- 资源管理器 `/select,` 的语义是"打开父目录并选中该项"；`App` 里其实已有"选中恢复"机制（app.h:283-286 `pendingSelPath_/pendingSelItems_`），完全可以复用。
- 后果：从"打开方式 → ExBarBrowser"或带 `/select` 的快捷方式进来，用户看到父目录但目标文件没被选中，且没有任何提示。
- 修复：把 `(parent, leaf)` 一起返回，填 `pendingSelPath_/pendingSelItems_`（或新加一对参数传给 `Run`）。

### M33 path_util：`\\?\C:\` 被削成 `\\?\C:`（扩展长度路径的盘根被破坏）
- `src/path_util.cpp:46` `while (p.size() > 3 && p.back() == L'\\') p.pop_back();`
- 对 `\\?\C:\`（7 字符）会削成 `\\?\C:` —— 这在扩展长度语法里**无效**（`\\?\C:` 不是根，也不是盘符相对路径）。虽然 `\\?\` 形态一般不经 `ToShellPath`（`.cpp` 里只对 `::`/`shell:` 放行），但 `ToShellPath` 是"外部路径 → shell API 的唯一收口"，长路径前缀正是外部输入常见的形态。
- 修复：判断"根"时同时认 `\\?\X:\` 与 `\\?\UNC\server\share\`（用 `nav::IsDriveRoot` 同族逻辑，或先剥离 `\\?\` 前缀再判断再拼回）。

---

## 四、低（Low）—— 一致性与健壮性，建议顺手清掉

- **L1 app.cpp:52/115** `win::RegisterClassOnce(...)` 返回值未检查；失败 → `CreateWindowExW` 返回 NULL → `Run` 返回 1，无任何日志。建议 `if (!Register)... { LogDbg; return false; }`。
- **L2 assert_check.cpp:86-91** `.length` 按 UTF-16 code unit 计数（emoji 计 2、CJK 计 1），与"字符"直觉不符；assert_check.h 的断言语法段应写明口径。
- **L3 audit_log.cpp:95** `errorCode` 是唯一没有 `OrDash` 的字段（空串 vs 其余字段的 `-`），JSONL 消费方要特判。要么统一，要么在头文件注明"空串=无"。
- **L4 clipboard.cpp:49-55** 注释称"改成先分配后清空，避免两头空"，但 `EmptyClipboard()` 之后 `SetClipboardData` 失败仍会把旧内容丢掉（注释只对分配失败路径成立）。文案收窄即可。
- **L5 dev_archive_eval.cpp:286-297** 双循环求"候选缺测试"，O(n²)；用 `set` 或按已解析的 map 查即可。
- **L6 dev_diff.cpp:77-78 / :230-231** `maxTotalBytes = 2MB` 但 `devproc::kMaxOutputBytes = 256KB`，2MB 预算不可达；且单文件预算 `fd.hunks.size() > 0 && h.text.size() > maxBytesPerFile` 让**第一个** hunk 可以超过 128KB。两处都要么对齐常量，要么改成累计判断（`fdBytes + h.size() > maxBytesPerFile`）。
- **L7 dev_diff_parse.cpp:144** 未知状态字母（`U` 未合并、`X`、`B` 等）统一归 `L"modified"` —— 与 dev_diff.h:55-57"封闭词表，调用方按这几个词分支"冲突，等于悄悄扩大了词表。建议归 `L"unknown"` 并在 `FileDiff.reason` 里带原始字母。
- **L8 eval_surface.cpp:22-36 / :311-341** `ListExt` 只看一级目录，`tests/sub/x.cpp` 既不在 `files` 段（签发时没列）也不在 `dirSets` 集合比对里 → 逃过两道检查（好在 `run_all.sh` 也只编译一级 `tests/*.cpp`，所以没被执行的窗口；但这个"恰好一致"值得写进注释）。
- **L9 eval_surface.cpp:296/339** `checkedFiles` 把 `files` 段与 `dirSets` 段重复计数（同一文件算两次），字段名"实测了几个文件"虚高。
- **L10 layout_limits.h:14-25** 常量落在全局命名空间（`kNavPaneWidthDefault` …），与 `layout::` / `nav::` / `id::` / `str::` 的真命名空间惯例不一致，容易与别的同名常量撞。建议 `namespace layoutlimits {}` 或并入 `layout::`。
- **L11 log.cpp:51/55** `_vsnwprintf` / `_snwprintf` 在截断时**不写终止符**，这里靠 `wchar_t body[1024] = L""` 的零初始化兜住（正确但脆弱）。建议换 `_vsnwprintf_s`/`StringCchPrintfW`，让"截断"这件事在代码里可见。
- **L12 log.cpp:54-56** 单行超 1088 宽字符被截断且无标记 —— 一次 `LogDbg` 打了很长内容（如 diff 片段）时，日志看起来是完整的。建议截断时补 `…(截断)`。
- **L13 main.cpp:117-124** `FindWindowW(kMainClass, nullptr)` 不校验目标进程的 exe 路径：把路径转发给**另一份拷贝**（D 盘的旧版本、别的 profile）也算成功。建议 `GetWindowThreadProcessId` + `QueryFullProcessImageNameW` 比对自身路径（不一致就不转发，直接新开）。
- **L14 path_util.cpp:40/57** `rfind(L"shell:", 0)` 大小写敏感；`Shell:Downloads` 会被当文件系统路径去折叠/去尾斜杠。改用 `_wcsnicmp` 或 `str::IStartsWith`（注意循环依赖：pathutil 目前不依赖 str，可本地 `_wcsnicmp`）。
- **L15 str.h:41** 注释"返回空 + ok=false"，实际签名是 `bool FromUtf8Strict(std::string, std::wstring*)`，没有 `ok` 变量。改为"返回 false"。
- **L16 str.cpp:12-13 / 18** `iswspace`/`towlower` 走 CRT locale；若将来调用 `setlocale(LC_ALL, "")`（中文系统），`towlower` 对全角字符行为会变，书签/键位匹配口径随之漂移。建议注释标明"依赖 C locale，禁止在进程内 setlocale"。
- **L17 dev_snapshot.cpp:222** `f.bytes = static_cast<unsigned long long>(o.GetInt(L"bytes"))` —— `GetInt` 会夹到 int 范围（json.cpp:50-54），>2GB 的 pre-rollback 条目读回来会变成默认值 0。用 `Find`+`num` 读 double（或加 `GetNum`）。
- **L18 dev_snapshot.cpp:105** `TrackedFiles` 逐行 `pop_back()` 掉尾部空格 —— 文件名本身以空格结尾的（合法）会被破坏；另外 `ls-files` 非 `-z` 对含引号/换行的路径会转义，与 `ChangedFiles` 的口径不完全一致。统一走 `-z`。
- **L19 dev_snapshot.cpp:256-259** 每个改动文件都在 `tracked` 线性表里 `std::find`（大仓 10k×500 次字符串比较）；用 `unordered_set` 建一次。
- **L20 dev_archive.cpp:356-384** `List` 用 manifest 里的 `id` 排序而不是目录名（manifest 缺 id 时排序错位、`Entry.id` 为空），并且全量读 manifest 无数量上限。
- **L21 dev_tools.h:89-91 ValidRepoPath** 仍在拒绝 `& | < > ^` —— 那是 shell 拼接时代的残留（dev_proc 已经不经 shell），反而**误伤合法文件名**（`a&b.txt`），同时又不拒绝 `%`/换行。既然不再拼 shell，这段黑名单应删除或改成"只挡 `..`/绝对路径/盘符"。
- **L22 view_modes.h:78-91 ModeIndexFor** 对同一 FVM 取"最近图标尺寸"：`FVM_LIST` 只有 16px 一档，用户手动缩到 48 时仍返回 LIST 档（合理），但 `FVM_THUMBNAIL` 256/96/48 三档在 iconPx=64 时取 48（差 16 < 差 32）—— 行为正确，建议补一句"平票取先出现者"的说明（当前 `delta < bestDelta` 是严格小于，平票取前者 ✓）。
- **L23 view_stats.cpp:30-35** `now - e.tick` 用 `GetTickCount()`（49.7 天回绕）算 TTL；回绕时差值巨大 → 缓存失效（安全方向），但用 `GetTickCount64` 更省心。
- **L24 report.cpp:118-122** 多行 EDIT 用窗口标题传正文，超大文本（几十万行 dup 结果）会在 `CreateWindowExW` 里同步拷贝一次、再 `EM_SETSEL` 一次，打开有明显停顿；建议改 `WM_SETTEXT` 或限制正文长度并在末尾注明"已截断"。
- **L25 json.cpp:203-230 Number** 接受 `+1`、`.5`、`1.`、前导零（均非严格 JSON）；`wcstod` 走 CRT locale（若进程 `setlocale` 到逗号小数点的区域，`"1.5"` 会被读成 1）。宽容解析对配置文件可接受，但应在 json.h 记一句；`wtod` 侧建议加 `_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)` 或自己实现整数路径。
- **L26 json.cpp:142-147 Lit** 前缀匹配：`Lit(L"true")` 对 `truthy` 会先吃掉 `true` 再在根值后报"还有内容"（拒绝，方向安全）；但当它出现在数组里 `[truthy]` 时错误信息会指向"数组缺少 ]"。可在 `Lit` 后加"下一个字符必须是分隔符/结尾"的检查，让报错点更准。
- **L27 assert_check.cpp:177-188 SplitExpr** 找**第一个** `!=`：右值里含 `!=`（如 `reason=a!=b`）会被切错；`x=`（空右值）不判 malformed，只会在比较时失败。建议 `find` 后校验两侧非空，并把"右值不允许含 `!=`"写进头文件。
- **L28 assert_check.cpp:275** `if (!r.before.empty())` —— `before` 合法为空串时字段被省略，JSON 里无法区分"没有 before 状态"与"before 是空串"。建议 `$changed/$unchanged` 断言总是输出 `before`（哪怕是 `""`）。
- **L29 dev_archive_eval.cpp:35-45 ParseCount** 上限 1e9（=10 亿），单个数字串超限直接 false（拒绝整条轨迹）；对测试计数够用，但错误信息只说"缺少汇总行"，不区分"数字太大"。可选。
- **L30 audit_log.cpp:40-47 NowIso** 本地时间、无时区偏移 —— 严格说不是 ISO-8601（缺 `+08:00`），跨机器/跨时区对账会歧义。建议加偏移或改 UTC。

---

## 五、已核实无问题（避免下一位重复排查）

1. `json.cpp:22-26 ClampToInt` / `:349-369 Num`：`2^63` 边界注释与实现一致，`1e999→inf` 与 `NaN` 都有前置判定，`static_cast<long long>` 不会 UB。这一点比多数自研 JSON 写得稳。
2. `json.cpp:428-441 Parse` 的"根值之后还有内容"检查到位；`Stringify` 的 `Escape` 对 `<0x20` 全部转 `\u00XX`（含 NUL）正确。
3. `str.cpp:33-51` 两个带长度版本用 `s.size()` 双次传参，含内嵌 NUL 的字符串不会被截断（与 `FromUtf8(const char*)` 的取舍不同，是有意的）。
4. `platform_win.cpp:46-63 CreatePopup` 用 `lpCreateParams` 传 self、并在过程未处理 `WM_NCCREATE` 时兜底补设 —— 注释给出的理由（模态窗在 NCCREATE 里就要用 this）成立。
5. `clipboard.cpp:33-46` 先 `GlobalAlloc` + `memcpy` 再 `EmptyClipboard`，且 `SetClipboardData` 失败时 `GlobalFree` —— 内存泄漏那条真的修掉了。
6. `audit_log.cpp:106-147 ToTextLine` 逐字复刻旧格式（`ok=N` 在行尾、`rejected=` 独立记号、blocked 保留前导空格），且理由写在注释里 —— 兼容性处理得体。
7. `dev_proc.cpp:143-199` 的 `CREATE_SUSPENDED` + `AssignProcessToJobObject` + `ResumeThread` 顺序正确；`AssignProcessToJobObject` 失败时降级不卡死也是对的；`createErr` 在 `CloseHandle` 之前取，符合注释。
8. `dev_proc.cpp:297-300` `overflow` 一路透传到 `dev_diff` → `Report.truncated`，截断不会被静默吞掉（只有 M15 的"头尾"没做到）。
9. `dev_archive_eval.cpp:85-141 Decide` 的 e-process：λ=0.5、因子 `{0.5,1,1.5}`、阈值 `1/α=20` 与 Ville 不等式口径一致；`invalid>0` 先于阈值判定（fail-closed）；`trials` 为空给 `insufficient` 而不是 `accept` —— 判定骨架是对的。
10. `eval_surface.cpp:250-289` 的顺序（先 selfSeal → 再 git 信任 → 再逐文件哈希 → 最后合并报错）与"不提前返回"的注释一致，fail-closed 贯彻得很彻底。
11. `app.cpp:783`（`SyncAiChannel`）与 `:819`（`InitAiPanel`）两次 `aisvc::Start` 是**幂等**的（`aichat_service.cpp:130-133` 命中 `g_started && Current().running` 就直接返回旧端口，不重启 node），不构成重复拉起 —— 已核对，不算缺陷。
12. `layout_key.h` 的指纹字段与 `app.h:473-474` 的用法一致；`pane1Ready` / `activeTab` 进指纹的理由（新建出来的窗格是 8x8）代码注释与实测记录吻合。
13. `path_util.cpp:7-33 CollapseSeparators` 的 UNC 前缀保留、`\\` 幂等、单反斜杠不算 UNC，三个边界都对。
14. `assert_check.cpp:56-99 ResolveArgRef` 的 `.length`/`.trim` 后缀判定用 `key.size() > suffix.size()`（严格大于），不会把 `key=".trim"` 切成空键 —— 边界正确。
15. `dev_tools.cpp:44-56 TrimOutput` 的 `kKeep - kHead - 48` 不会下溢（4000-1400-48），marker 里的省略字符数算式正确。
16. 我这组 40 个文件里**没有硬编码颜色**：机检 `RGB(` / `0x00RRGGBB` / `COLORREF` / `GetSysColor` 全部零命中；`common.h:49-56` 的 `SSF_*` 是 shell 视图掩码不是颜色，`report.cpp:72` 的 `SetTextColor` 取的是 `theme->text`。颜色确实全部走 `Theme`。

---

## 六、总体评价

**结构质量高于平均水平，问题集中在"承诺与实现之间那 5% 的缝隙"。**

优点（值得保留的工程习惯）：
- 注释写的是**判据**而不是描述："探针 82 实测 System32 5003 项 8.2ms vs 85.7ms"、"探针 84 ShellView::Destroy 20~69ms"、"hr=0x80070057 的三组对照"。这类注释让后来者无法随手改坏（layout_key.h、dev_proc.h、dev_diff.h、view_stats.h 是最好的几份）。
- 收口意识强：`str::` / `pathutil::` / `win::` / `id::` / `json::` / `auditlog::` 每个都写了"以前散在 N 处、为什么必须一份"。`identity.h` 是品牌串治理的标杆。
- fail-closed 的立场明确且多数落到了代码：`Decide` 的 `invalid>0 → reject`、`eval_surface` 的 manifest 缺失即不通过、`GetJob` 删掉"现编超时"、`json::ClampToInt` 越界给默认值而不是钳极值。

真正需要动手的，按优先级：

1. **H1（转发激活失效）** —— 用户每天都会遇到的"多开一个窗口"，修复成本极小（两个分支），收益最大。
2. **H2/H4/H4b（dev 观测面的假绿）** —— 这三条的共同点是"失败被报告成'没有变化'"。这不是普通 bug：整个 dev.* 与 archive/funnel 体系的价值就是"给 agent 一个不可自欺的观测面"，一旦失败方向是"看起来干净"，防线就整体失效。修法一致：每个外部命令都要判 `code`，判不了就 `ok=false`。
3. **H3（轨迹哈希对不上）+ M9（`ToUtf8` 静默替换）+ M15/M16（截断与归因）** —— 同属"证据链完整性"。H3 尤其值得优先：它让"逐字保存、可哈希核对"这句承诺在中文 Windows 上默认不成立；M9 是同一个问题的另一半（写出去时就丢了字符，且返回成功）。
4. **H5/H7** —— H5 是解析器的真缺陷（普通 diff 就能触发）；H7 已被校验器判红，说明棘轮机制在工作，只是没人在意红灯。
5. **M23b（canonical 形式不排序）** —— 唯一一条"守门代码与它的注释不一致"的地方，且守的还是唯一的运行期信任根。它今天不发作，只因为清单恰好总是由 Python 生成；一旦有人手改一次，应用内校验与 CI 校验就会给出相反结论 —— 而那种时刻通常正是你最需要它们一致的时候。修一条 `std::sort` + 一个单测即可。
6. **M7（`SetForegroundWindow` 不判返回值）+ M5/M6/M10/M11/M12/M33** —— 一批"注释说 A、代码做 B"的小口子。M7 是其中唯一有用户可见后果的；其余的危害不是崩溃，而是**耗尽读者对注释的信任**；这个仓库的注释密度是它最大的资产，一旦读者开始怀疑注释，维护成本会跳一档。

一句话结论：**这批文件的"设计"部分可以打 8/10，"设计与实现的一致性"只有 6/10，"可验证的假绿防护"6.5/10。** 修掉 7 条 High（估计 1~2 天）之后，这一组可以作为其他组的模板；不修则会在"agent 说自己改好了、系统说没看到改动"这种最要命的场景里反复失分。
