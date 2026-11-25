# G10_tabjob 代码审查报告

审查范围：`review/G10_tabjob.txt` 列的 31 个文件，合计 4845 行。**全部整文件读完（无抽样、无跳读）**，逐函数逐分支过。
只读 `src/`，未构建、未改源码。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|------|------|------|
| 1 | src/disk_scan.cpp | 252 | 基本合格。早停/取消/重解析点/长路径处理正确；进度只在第二层报，`items` 把目录自身算进去（低）。 |
| 2 | src/disk_scan.h | 69 | 合格。`complete` 把 `deniedDirs` 纳入判定——这一点比 file_tools 正确。 |
| 3 | src/file_hash.cpp | 225 | 合格。RAII 收住 BCrypt 三件资源，错误路径无泄漏；`HashBytes` 4GB 截断（低）。 |
| 4 | src/file_hash.h | 68 | 合格。两条硬约束（流式/可停）与实现一致。 |
| 5 | src/file_rename_plan.cpp | 212 | **有缺陷**：批内重名与撞名检测都是 O(n²)（中）；仅改大小写被当成"没变化"跳过（中）；设备名未挡（低）。校验项（空名/非法字符/尾空格点/超长）齐全。 |
| 6 | src/file_rename_plan.h | 41 | 合格。占位符语义与实现一致；`ExpandPattern` 恒返回 true 属签名冗余（低）。 |
| 7 | src/file_tools.cpp | 349 | **有缺陷**：两处 `complete` 不含 `deniedDirs`（严重）；`Content::Unknown` 被并进 `Differ`，自打枚举注释（中）。遍历/取消/重解析点/并行预算正确。 |
| 8 | src/file_tools.h | 113 | 契约写得很好，但 `complete` 的契约（第 10-11 行）实现没兑现——正是本组最该修的一处。`CompareEntry::isFolder` 实际从未赋值（低）。 |
| 9 | src/file_undo.cpp | 53 | 合格。容量裁剪、"失败不弹栈"、机器不穿透人工三条规则正确。 |
| 10 | src/file_undo.h | 91 | 合格。`Applier` 非 const 契约与 app_file_undo.cpp 实现吻合。 |
| 11 | src/job.cpp | 225 | **有缺陷**：取消的作业同时被标 failed（严重，与自身注释相反）；异步作业线程永不 join、`jobs_` 无淘汰（中）；退出 join 无兜底（低）。 |
| 12 | src/job.h | 112 | 文档与实现的分歧即上面第一条；其余线程/快照约定与实现一致。 |
| 13 | src/job_ui.cpp | 268 | **有缺陷**：二次关闭窗口后上层仍会再等 5s（中）；`isHuman` 死字段；`WaitQuiet` 的"被取消"返回分支不可达。 |
| 14 | src/job_ui.h | 36 | `WaitQuiet` 返回值说明与实现不符（低-中）。 |
| 15 | src/scan_ctl.h | 61 | 合格。0=不限时的翻译责任划分清楚，是这组里最好的一处约定。 |
| 16 | src/search_folder.cpp | 222 | **有缺陷**：`SetScope`/作用域解析失败只打日志，仍返回成功 → 搜索静默降级到全索引（严重）。PIDL 指纹按 NUL 截止，与注释不符（低，仅日志）。 |
| 17 | src/search_folder.h | 48 | 合格（含实测记录，价值高）。引用已不存在的单体文件行号（低）。 |
| 18 | src/search_scope.cpp | 93 | 合格。`RangeSupported` 不校验上界（低）。 |
| 19 | src/search_scope.h | 71 | 合格；`Target::scopeDir`"空 = 整台电脑"与后端拒绝空作用域矛盾（低）。 |
| 20 | src/search_url.cpp | 36 | 合格。转义写法脆弱（`q.insert(i++, ...)`），`query` 里的 `&` 未转义且未记录（低）。 |
| 21 | src/search_url.h | 32 | 合格。已知不转义只记录了 scope，漏了 query（低）。 |
| 22 | src/tab_set.cpp | 224 | 合格。MRU 快照循环、关闭历史裁剪正确；`SetPinned` 与头注释语义相反（中）。 |
| 23 | src/tab_set.h | 134 | 见上；`PathRepeated`/`DisplayTitle` 的"唯一实现"约束执行得好。 |
| 24 | src/tab_strip.cpp | 172 | 基本合格。图标缓存与裁剪正确；空匿名命名空间（低）。 |
| 25 | src/tab_strip.h | 235 | 合格。纯函数（命中/落点/新建按钮 x）与注释质量是本组最高的一档。 |
| 26 | src/tab_strip_actions.cpp | 185 | 基本合格。禁用态齐全；`POINT at = screen;` 是死赋值，颜色菜单跟光标而非跟被右键标签（低）。 |
| 27 | src/tab_strip_input.cpp | 402 | **有缺陷**：`uia::kMsgItem` 分支访问 `tabRects_[i]` 无边界检查（严重，最小化时可越界）；命中测试自指依赖 hover 状态（中）；`(void)0;` 残留（低）。 |
| 28 | src/tab_strip_layout.cpp | 210 | 基本合格。夹取 `scrollX_` 后不重算 rects（中-低）；`EnsureVisible` 吃掉 margin（低）。 |
| 29 | src/tab_strip_paint.cpp | 111 | 合格。全部走 `ui::` 原语，无 GDI 句柄自管。 |
| 30 | src/tab_uia.cpp | 457 | **有缺陷**：`Root()` 返裸指针并在锁外 `AddRef`（严重，UAF 窗口）；`HasKeyboardFocus` 返回 selected（低）；`ElementProviderFromPoint` 不做裁切（低）。 |
| 31 | src/tab_uia.h | 38 | 合格。跨线程契约（同进程 SendMessage + 栈上 ItemInfo）成立。 |

合计 4845 行，与 `wc -l` 一致。

---

## 二、严重（会导致错误结果 / UB / 契约被静默违反）

### S1 `src/job.cpp:122` — 被取消的作业同时被标成 failed
```cpp
rec->cancelled = rec->cancel.load(std::memory_order_relaxed);
rec->failed = !ok;
```
body 被取消时通常返回 false（`file_hash.cpp:131-135` 取消就 `return false`；`file_tools` / `disk_scan` 的取消路径同理），于是 `cancelled=true` **且** `failed=true`。
而紧邻的注释（`job.cpp:119-121`）明确写"混成 failed 会让调用方以为出了错而重试"——实现正犯了它自己禁的那件事。
影响面：`app_commands_jobs.cpp:100`（`st.failed` → 报 error）、`:121-122`（`status` 把两个都导出）、`:223`（`tool.job.list` 同样）。AI 读 `tool.job.get/list` 会看到 `cancelled:true, failed:true`，语义自相矛盾。
**修复**：`rec->failed = !ok && !rec->cancelled;`，并在 cancelled 为真时把 `error` 清空（或统一成"已取消"文案）。

### S2 `src/file_tools.cpp:227` 与 `:343` — `complete` 不含 `deniedDirs`，与头文件契约正面冲突
```cpp
out->complete = !timedOut && !cancelled;          // FindDuplicates
out->complete = !timedOut && !cancelled;          // CompareFolders
```
`file_tools.h:10-11` 写明："必须报告完整性：timedOut / deniedDirs / complete 三个字段缺一不可，否则'没有重复'与'没扫完'无法区分 —— 用户会据此删错东西。"
实测语义：某个子树因权限打不开 → `deniedDirs=1`，但 `complete` 仍是 **true**，调用方据此宣称"扫完了，没有重复"。这正是头文件警告的那个删除事故场景。
对照 `disk_scan.cpp:235` 是正确写法：`... && out->deniedDirs == 0`。
**修复**：`FindDuplicates` → `out->complete = !timedOut && !cancelled && denied == 0;`；`CompareFolders` → `... && deniedL == 0 && deniedR == 0;`。

### S3 `src/tab_strip_input.cpp:309` — UIA 取项时对 `tabRects_` 无边界检查
```cpp
case uia::kMsgItem: {
    const int i = static_cast<int>(w);
    if (!info || i < 0 || i >= static_cast<int>(items_.size())) return FALSE;
    ...
    info->rect = tabRects_[static_cast<size_t>(i)];   // ← 只校验了 items_
```
`Layout()` 在窗口最小化（`GetClientRect` 得 `rc.right<=0`）或 `!hwnd_` 时会先 `tabRects_.clear()` 再 return（`tab_strip_layout.cpp:31-36`），此时 `items_` 仍非空。
读屏软件/检查器在最小化窗口上查询（UIA 客户端可从任意线程 SendMessage 进来）→ `i` 通过 `items_` 校验，但 `tabRects_` 为空 → 越界读 `std::vector`（UB）。
**修复**：`if (i >= static_cast<int>(tabRects_.size())) return FALSE;`（`items_` 与 `tabRects_` 两个长度都要校验）。

### S4 `src/tab_uia.cpp:383-398` — `Root()` 在锁外使用裸指针，存在 UAF 窗口
```cpp
TabRootProvider* TabItemProvider::Root() {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_roots.find(hwnd_);
    return it == g_roots.end() ? nullptr : it->second;   // 裸指针，未 AddRef
}
...
TabRootProvider* root = Root();
if (root) { *pRetVal = ...; root->AddRef(); }            // 解锁后才 AddRef
```
若同一时刻 UI 线程执行 `~TabStrip` → `uia::Release` → `Release()` 把 refcount 打到 0 并 `delete`，`root->AddRef()` 就作用在已释放对象上（读已回收内存并写 refcount）。
**修复**：`Root()` 内就地 `AddRef()` 后返回（改名为 `RootRef()` 表明所有权移交给调用方），或在调用点用 `ComPtr`/`Microsoft::WRL` 风格持有。

### S5 `src/search_folder.cpp:166-189` — 作用域失败被静默忽略，搜索降级为全索引
```cpp
hr = SHParseDisplayName(scope.c_str(), nullptr, &abs, 0, nullptr);
if (FAILED(hr) || !abs) { LogDbg(...); }        // 只打日志
else { ... hr = sff->SetScope(sia); if (FAILED(hr)) LogDbg(...); ... }  // 也只打日志
...
hr = sff->GetIDList(&pidl);
if (SUCCEEDED(hr) && pidl) { *outPidl = pidl; ok = true; }               // 仍然成功返回
```
`SetScope` 失败/作用域路径不存在时，返回的是一个**没有作用域**的搜索文件夹 = 索引全盘搜索。UI 与调用方都以为搜的是"自定义路径"。
这直接违反 `search_scope.h:21`（"这一档是'明确报错'，不是'悄悄搜成别的范围'"）与本源 `search_folder.cpp:129-134` 自己立的规矩（"空 = 调用方该报'整机范围不可用'，不是悄悄降级"）。
放大器：`search_scope.cpp:83-86`（kCustom 只判空，不判目录是否存在）。
**修复**：`SHParseDisplayName` 或 `SetScope` 失败即 `ok = false`（并置一个 `scopeFailed` 让调用方能区分"没有 Windows Search"与"作用域不可用"），同时在 `Resolve` 里对 kCustom 做一次目录存在性校验。

### S6 `src/job.cpp:134` + `:178-199`（配合 `app_commands_jobs.cpp:41`）— 异步作业线程永不 join，作业表无淘汰
- `spec.async` 时 `RunTask` 直接 `return id`（`app_commands_jobs.cpp:41`），`WaitFinished` 从不被调用 → `Record::worker` 一直 joinable 且从未 join（线程句柄泄漏）。
- `jobs_` 只有 `push_back`（`job.cpp:134`），无删除/淘汰 API → 每个作业的 `Record`（含 `result` 整个 json）永久常驻；`List()` 随时间线性变长，而 `SnapshotLocked`（`job.cpp:66`）每次 `Get`/`List` 都**拷贝整个 result**。
- 附带隐患：将来若给 `jobs_` 加淘汰，未 join 的 `std::thread` 析构会直接 `std::terminate`。
**修复**：增加 `Prune(bool join=true)` / `Remove(id)`（只回收 `done && joined` 的记录），或在 `Start` 的工作线程收尾处显式 `detach`/join 并把 join 责任写进 `job.h` 的契约。

---

## 三、中（可用性 / 性能 / 语义偏差）

### M1 `src/file_tools.cpp:303-309` — `Content::Unknown` 被并入 `Differ`
`file_tools.cpp:122-124` 的枚举注释写得很明确："第三态不能并入任何一边 —— 把'读不了'说成'相同'，用户会漏掉真正的差异；说成'不同'，用户会去核对一份好文件。"
但调用点：
```cpp
} else if (opt.byContent && it->second.bytes > 0) {
    const Content r = SameContent(...);
    if (r == Content::Same) { e.state = Diff::Same; ++out->sameCount; }
    else { e.state = Diff::Differ; ++out->differCount; }     // ← Unknown 走这里
}
```
文件被独占/刚被删 → 哈希失败 → 报成"内容不同"。且 `complete` 不同步失效。
**修复**：`else if (r == Content::Differ) {...} else { 记 unknown（新增计数）或把该项标为未知态 }`。

### M2 `src/job_ui.cpp:144-157` + `app_commands_jobs.cpp:52` — "第二次点关闭"之后用户仍被卡最多 5 秒
`WM_CLOSE` 第二次：`st->done = true; DestroyWindow(h);` → `modal::Run` 退出 → `WaitWithWindow` 返回 → `RunTask` 紧接着 `WaitFinished(id, kJoinGraceMs=5000)`，而作业还在跑 → 又阻塞 5 秒。
与同处注释（`job_ui.cpp:152-153`：二次关闭是"别再把用户困在窗口前"）相反。
**修复**：`WaitWithWindow` 区分"用户取消"与"用户强行放弃等待"，后者不再调 `WaitFinished`（或传 0 超时的非阻塞版本）。

### M3 `src/file_rename_plan.cpp:179-199` — 批内重名/撞名检测是 O(n²)，全在 UI 线程
```cpp
for (const auto& c : created) if (str::IEquals(c, newName)) dupInBatch = true;
for (const auto& e : existing) { ... if (str::IEquals(e, newName)) { clash = true; break; } }
```
1 万文件批量改名 → 约 1 亿次宽字符串大小写不敏感比较。批量改名天然是大批量场景，这里会成为秒级卡顿。
**修复**：改用 `std::set<std::wstring>`/`unordered_set`，键统一 `str::Lower(name)`（保留 `str::IEquals` 作为唯一比较原语的语义，只换容器）。

### M4 `src/file_rename_plan.cpp:173-176` — 仅改大小写的改名被当成"没变化"，且错误文案误导
`str::IEquals(newName, oldName)` 为真就 `skippedSame++` / `continue`，于是 `a.txt → A.txt` 永远做不到；若这是唯一意图，`plan->empty()` 会报"没有任何文件需要改名（模式算出的名字与原名相同）"（`:205-207`），用户看着明明不一样的两个名字收到这句话。
**修复**：`IEquals` 相等但原串不等 → 按"需要改名"处理（Windows 的 `MoveFile` 支持仅改大小写），或明确报"仅大小写变化需要额外确认"。

### M5 `src/tab_set.cpp:108-122` vs `src/tab_set.h:75-76` — 注释与实现语义相反
头文件：`void SetPinned(...)` "取消固定保持当前位置 —— 不把标签'弹'到别处，避免用户找不到。"
实现：`Move(id, pinned ? total - 1 : total);`，且 `:113-117` 注释写明是 Chrome 语义（移到第一个未固定位）。
两者只有一个对。按实现（Chrome 语义）修头注释；若按头注释，则 unpin 不应 `Move`。

### M6 `src/tab_strip_input.cpp:368-383` — 关闭钮命中测试自指依赖 hover 状态
`HitClose` → `CloseVisible` → 条件是 `index == active_ || (hoverKind_ == kTab && hoverIndex_ == index) || ...`，而 `hoverIndex_/hoverKind_` 只在 `WM_MOUSEMOVE`（`:324`）里更新。
后果：命中结果依赖"上一条 mousemove"，且 `SetTabs` 只在 `hoverIndex_ >= items_.size()` 时夹取（`tab_strip.cpp:96`），**不做下标重映射**——插入/关闭标签后 `hoverIndex_` 指向的可能是另一个标签，于是该标签被画出关闭钮并可被单击关闭。与 `tab_strip.h:139-141`"绘制与命中必须用同一判据"的初衷只差一步（判据一致，但状态陈旧）。
**修复**：`SetTabs` 里以 **id/path** 而非下标保存 hover 目标（或 `SetTabs` 时直接 `hoverKind_=kNone, hoverIndex_=-1`）。

### M7 `src/tab_strip_layout.cpp:144-145` — 夹取 `scrollX_` 后不重算 rects
```cpp
maxScroll_ = overflow_ ? std::max(0, contentEnd - tabViewRight_) : 0;
if (scrollX_ > maxScroll_) scrollX_ = maxScroll_;   // rects 已按旧 scrollX_ 算完
```
关闭标签 / 缩窗口导致内容变短时，本次的 `tabRects_`（已用于命中、tooltip 热区、绘制）比 `scrollX_` 落后一次，出现一次可见的错位，直到下一次 `Layout()`。
**修复**：夹取后若变化则重跑布局（把宽度计算抽成可重入的一次 pass，或用 `do { } while (clamped)`）。

### M8 `src/job_ui.h:32-34` + `src/job_ui.cpp:264-265` — `WaitQuiet` 的"被取消"分支不可达
`WaitQuiet` 返回 `st.cancelRequested`，但机器路径没有窗口、没有任何地方调用 `RequestCancel`（`job_ui.cpp:47`），该字段恒为 false。头文件却把"返回 true = 作业被取消"列为契约。
同时 `ProgState::isHuman`（`:40`，赋值于 `:203`/`:249`）从未被读取——死字段。
**修复**：删除 `isHuman`；`WaitQuiet` 返回值改成"是否正常等到结束"或直接 `void`，并同步头注释。

### M9 `src/search_folder.cpp:205-214` — PIDL 指纹按 NUL 字节截止，与"身份指纹"的说法不符
```cpp
for (BYTE* b = reinterpret_cast<BYTE*>(pidl); b && *b; ++b) hash = ...;
```
PIDL 是 packed 结构（`cb` 为 WORD、紧跟类型字节），内部普遍存在 0x00 字节——循环几乎立刻停在第一个 0 字节，得到的不是"这份 PIDL 的指纹"。目前只进 `LogDbg`，无功能后果，但注释（`:201-204`）把它当成可用的身份判据，容易误导后续维护者。
**修复**：按 `ILGetSize(pidl)` 取长度遍历，或降级注释为"仅用于日志相关性观察，不是可靠指纹"。

### M10 `src/file_hash.cpp:206` — `HashBytes` 把 `size_t` 静默截成 `ULONG`
```cpp
if (!NtOk(BCryptHashData(ctx.hash, const_cast<PUCHAR>(p), static_cast<ULONG>(len), 0)))
```
`len > 4GB` 时摘要只覆盖前 4GB 且**不报错**——"我算的是整个文件的摘要"变成假话。
**修复**：`if (len > MAXDWORD) return false;`，或按块循环喂 `BCryptHashData`。

### M11 `src/disk_scan.cpp:199` — 第一层枚举（可能几十万项）不报任何进度
第一层 `FindFirstFileW` 循环（`:150-176`）只在每 1024 项查 deadline/取消，但**不 Report 进度**；进度只在第二层逐个直接子项时报（`:199`）。超大目录时进度条长时间停在 `0 / N`，看起来像卡死。
**修复**：第一层也按 1024 项 `Report(L"正在枚举目录", outerCount, 0)`。

---

## 四、低（错误文案 / 死代码 / 注释不符 / 卫生）

| 编号 | 位置 | 问题 | 修复 |
|------|------|------|------|
| L1 | `tab_strip.cpp:12-14` | 空匿名命名空间（死代码，`namespace { }` 里什么都没有） | 删掉 |
| L2 | `tab_strip_input.cpp:352` | `(void)0;` 残留语句 | 删掉 |
| L3 | `tab_strip_actions.cpp:117-118` | `POINT at = screen;` 立刻被 `GetCursorPos(&at)` 覆盖——死赋值；同时"颜色二级菜单"跟着光标弹，而 `screen` 是被右键标签的位置，两者可能不在一处 | 用传入的 `screen` 定位，去掉死赋值 |
| L4 | `tab_strip_layout.cpp:9-16` | `EnsureVisible` 用 `ScrollBy(r.left)`，代入 `r.left = margin - scrollX_` 得 `scrollX_ = margin`，标签左缘贴到 0 而不是 margin（少一档留白） | `ScrollBy(r.left - margin)` |
| L5 | `disk_scan.cpp:74`/`:208` | `++*dirs` 把子树根目录自身算进 `dirs`，于是 `e.items = f + d` 比 `disk_scan.h:27`"目录下的条目数"多 1 | 首层不计入，或改注释为"含自身" |
| L6 | `disk_scan.cpp:185` | `const unsigned long long total = found.size();` 类型与用途不符（`size_t` 的个数），且 `Report(idx, total)` 传 `size_t` 隐式转换 | 统一为 `size_t`/`unsigned long long` |
| L7 | `disk_scan.cpp:241` | `static_cast<int>(found.size()) > opt.limit` 对大值溢出（≥2^31 项时变成负数，不截断） | 用 `found.size() > static_cast<size_t>(opt.limit)` |
| L8 | `search_scope.cpp:34-37` | `RangeSupported(99)` 返回 true（只判 `!= kThisPC`），越界档位被当"支持" | 加 `range >= 0 && range < kRangeCount` |
| L9 | `search_scope.h:62` | `scopeDir` 注释"空 = 整台电脑的索引"，但 `search_folder.cpp:131-134` 明确拒绝空作用域（本宿主做不到） | 改注释为"空 = 不可用" |
| L10 | `search_url.cpp:21-26` | `q.insert(i++, 1, L'"')` 依赖"实参先取值再自增"才正确，可读性差、易被后续改动破坏 | 换成显式 `out` 逐字符拼接 |
| L11 | `search_url.h:25-29` | 只记录了 `scope` 不转义（`&` 会断参），同样的问题在 `query` 上也存在（`Build` 直接 `query=` 拼接）且未记录 | 补一句已知限制，或在 Trim 之后按需百分号编码 |
| L12 | `search_folder.cpp:14-16` | 注释引用 `exbar.cpp:25995`（单体文件，本工程已拆分，行号已无对应物） | 改为指向本仓模块或删除行号 |
| L13 | `tab_uia.cpp:151` | `UIA_HasKeyboardFocusPropertyId` 返回 `info.selected`（选中 ≠ 焦点） | 仅在 `IsKeyboardFocusable`/真实焦点查询时返回 TRUE，或另取焦点状态 |
| L14 | `tab_uia.cpp:296-312` | `ElementProviderFromPoint` 不按 `tabViewRight_` 裁切，落在 `»`/`+` 上的命中测试会返回被遮住的标签 | 复用 `TabHitInView` 判据 |
| L15 | `tab_uia.cpp:83-229` | `TabItemProvider` 用**下标**做身份，标签增删后旧 provider 指向别的标签（`QueryInfo` 越界时静默返回空 name/零 rect） | 身份改用 `TabRef::id`（`kMsgItem` 传 id 而非下标） |
| L16 | `job_ui.cpp:86-118` | `DrawProgress` 直接解引用 `st->theme`（`WaitQuiet` 路径为 nullptr），当前靠两处 `WM_PAINT`/`WM_ERASEBKGND` 的上游判空兜住 | 函数入口自己判空并直接返回 |
| L17 | `file_rename_plan.cpp:55-103` | `ExpandPattern` 恒返回 true，调用方全部忽略返回值（签名冗余，读者会以为有失败路径） | 改 `void` |
| L18 | `file_rename_plan.cpp:159-171` | 未挡 Windows 保留设备名（`CON`/`NUL`/`COM1`…），计划通过但执行改名必失败 | 校验 stem（去扩展名后）是否设备名 |
| L19 | `file_tools.h:81` | `CompareEntry::isFolder` 从未被赋值（永远是 false），目录只在单侧存在时也不会出现在差异里 | 删字段，或补齐"目录差异"语义 |
| L20 | `file_undo.h:67-68` | `kMax=20` 与 `bmk.*` 口径的一致性是**注释**保证的（跨模块常量各写一份），改一处不会编译报错 | 提到共享常量头 |
| L21 | `disk_scan.cpp:68`、`file_tools.cpp:54` | `cur + L"\\*"` 在卷根（`C:\`）上拼出 `C:\\*`，依赖 Win32 路径归一化才成立 | 用统一的路径拼接原语（工程已有 `pathutil`） |
| L22 | `tab_strip.cpp:66` | `iconCache_.size() >= 256` 才裁剪；若 256 个 path 全在 `refs` 里则一个也不清，缓存可继续增长 | 裁剪后若仍 ≥ 上限，再按 LRU 丢最旧的（或用使用计数） |

---

## 五、工程约束检查

- **品牌串**：本组 31 个文件未见产品宣传串或对外品牌承诺文案，只有 `search_folder.h:8` 与 `search_folder.cpp:16` 引用外部单体文件的路径/行号（见 L12），属失效注释而非品牌问题。
- **硬编码色**：`tab_strip_paint.cpp` 全部走 `theme_->*`（`brBg/brHover/line/text/sub/accent/iconGlyph`）与 `ui::FillRectColor/DrawSeparator/DrawGlyph`，未出现 `RGB(...)`。唯一的裸像素是 `:32-34` 的 `r.right - 1`（发丝分隔线）与 `:77` 的 `theme_->S(2)`，前者判定为**可接受**（1px 发丝线不应随 DPI 缩放）。
- **命名空间**：`diskscan` / `filehash` / `filetools` / `fileundo` / `job` / `jobui` / `searchscope` / `searchfolder` / `searchurl` / `core` / `uia` / `scanctl` 均闭合且无同名歧义；`tab_uia.cpp` 的本地实现放在匿名命名空间，仅导出 `uia::HandleGetObject/Release`，边界干净。
- **文件行数**：本组最大 `tab_uia.cpp` 457 行，其余 100–400 行，**无超限文件**；拆分方向（layout/input/paint/actions）与文件头声明的 N2 约定一致，且各文件确实只做一件事（`tab_strip_paint.cpp` 零成员写、`tab_strip_layout.cpp` 零绘制）。
- **依赖方向**：`scan_ctl.h` 只依赖标准库、`tab_set.h` 零 Win32、`file_undo.h` 不含 COM——三处声明与实现一致，未发现反向依赖。
- **线程模型**：`job`（工作线程 + 互斥 + 原子取消）、`tab_uia`（跨线程 SendMessage 取快照）、`jobui`（UI 线程模态泵）三者边界清楚；发现的实际问题集中在 S1/S4/S6，而非模型本身。

---

## 六、总体评价

**质量水位：中上。** 这组代码的注释密度与"为什么"的说明在本仓里属于最好的一档——`disk_scan.h` 的三条硬约束、`scan_ctl.h` 对"0 是有限还是无限"的责任切分、`tab_strip.h` 把命中/落点/新建按钮 x 抽成纯函数、`search_folder.h` 把实测结论（尾斜杠、卷根、命名空间项）逐条写进注释，都是能防止后来者重踩坑的资产。数据层的资源管理（BCrypt RAII、`FindClose` 每条路径都配平、`ReleaseIcons` 与 `items_.icon` 同步置空）经逐分支核对，没有发现泄漏或二次释放。

**但存在一类系统性问题：注释承诺与代码行为脱钩，且方向一致地"往好里写"。**
- `file_tools.h` 说三字段缺一不可 → 实现里 `complete` 不看 `deniedDirs`（S2）；
- `job.cpp` 说不能把取消混成失败 → 实现里两个标志同时置位（S1）；
- `search_folder.cpp` 说"不悄悄降级" → 作用域失败照样返回成功（S5）；
- `Content` 枚举说第三态不能并入两边 → 调用点并入了 `Differ`（M1）；
- `tab_set.h` 说 unpin 保持位置 → 实现移到第一个未固定位（M5）。

这类问题的危害不在崩溃，而在于**读代码的人会相信注释、于是不去查实现**——一旦有人基于注释做修改（比如给 `jobs_` 加淘汰），就会踩到注释没提的真实约束（S6 的 `std::terminate`）。

**修复优先级建议**：
1. 先修 S1/S2/S5（都是"把不确定说成确定"，直接影响 AI 侧与用户的删除/重试决策），三处各一到三行。
2. 再修 S3/S4（越界与 UAF，触发条件窄但后果是崩溃），各一到两行。
3. S6 与 M2 一起做（作业生命周期 + 等待策略本是同一件事）。
4. M3/M4（批量改名）是唯一有明确性能与功能天花板的模块，值得一次小重构。
5. 低优先级的死代码/注释不符建议在改相邻代码时顺带清理，不必单独排期。
