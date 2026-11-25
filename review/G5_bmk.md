# G5_bmk 代码审查报告

审查范围：`review/G5_bmk.txt` 列的 12 个 bookmark / bmk / folder_tree 文件，合计 **3320 行**。
**全部整文件读完（无抽样、无跳读）**，逐函数逐分支过。只读 `src/`，未构建、未杀进程、未改源码
（唯一写入是本文件）。为核对契约另读了 `commands_table.cpp` / `app_commands_misc_bookmarks.cpp` /
`app_commands_bookmark_io.cpp` / `text_file.cpp` / `popup_list.h` / `tooltip_host.h` / `str.cpp` /
`tests/bmk_html_test.cpp`（只读，不改）。

> **并发修改说明**：`git status` 显示工作树里还有别的会话正在改 `src/`（`bookmarks.cpp`、`bookmark_bar.cpp`、
> `bookmark_drop.cpp` 等均在 modified 列表里，且 `bookmark_bar.cpp` 在我读完它之后又被改掉 2 行：
> 1073 → 1071）。本报告引用的**所有行号已逐条对照当前文件复核，全部仍然准确**；
> 行数一列按"我实际读到的版本"记账，与当时的 `wc -l` 一致。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|------|------|------|
| 1 | src/bmk_html.cpp | 358 | 基本合格。UTF-8 百分号解码、UNC 双向、数字实体、`Attr` 左边界都正确且被单测钉住。**但**孤立代理两条路径口径不一致（低）；`PathToUrl` 把非 `://` 协议（`mailto:` / `about:`）包成 `file:///`（中，见 M6）。 |
| 2 | src/bmk_html.h | 71 | 合格。词法层为什么抽、为什么不合并 `ParseLevel` 的论证准确，与实现一致；契约（纯字符串、可单测）成立。 |
| 3 | src/bookmark_bar.cpp | 1073（复审时 1071） | **有缺陷**：`ShowFolderMenu` 导航后不回焦（中）；插入线会用被溢出折叠的项坐标（中）；叶子清空路径导致节点从栏与文件里消失（合并入 S4）；`rc.bottom - 2` 硬编码像素、`FolderItemAt` 死变量、静态 `folderIcon`（低）。命中区几何、溢出布局、拖放插入线、键盘路径整体是认真做的。 |
| 4 | src/bookmark_bar.h | 106 | **有缺陷**：第 3 行给的存储位置（`%APPDATA%\ExBar\bookmarks.html`）与 `bookmarks.h:3`（`<exe>\data`）互斥；第 95 行"（或上次的位置）"未实现（`WM_KILLFOCUS` 一律清成 -1）。其余契约（拖放回调、refocus）与实现吻合。 |
| 5 | src/bookmark_drop.cpp | 220 | **有缺陷**：`CFSTR_SHELLIDLIST` 解析无长度/偏移校验，畸形 `IDataObject` 可致越界读（中，M2）；`Drop` 无条件回写 `DROPEFFECT_COPY` 与 `DragEnter` 保留源 effect 位的做法矛盾（中，M5）；`hoverFolder_` 只写不读、`g_drop` 文件级状态、`owner_` 未判空（低）。 |
| 6 | src/bookmark_drop.h | 12 | 合格。为什么必须 OLE（虚拟项）而不是 `WM_DROPFILES` 说清楚了；`Detach` 幂等成立。 |
| 7 | src/bookmark_io.cpp | 286 | **有缺陷**：`BmkExportHtmlTo` 绕开唯一实现 `tf::WriteText`，不判 `WriteFile`/无 `FlushFileBuffers`/无 temp+replace，失败仍报成功且先截断了目标文件（中，M1）；`BmkImportHtmlFromFile` 忽略 `Save()` 结果 → 假成功（中，M3）；UI 版与后端双层 `FileChanged` 提示（中，M7）；死 `using Lower` / 无用 `prompt.h`（低）。 |
| 8 | src/bookmark_io.h | 28 | 合格。`-1/-2/-3` 返回码与实现一致；但 UI 版额外产出 `-3` 而头注释只写到 `-1`（低）。 |
| 9 | src/bookmarks.cpp | 893 | **有缺陷（本组最重）**：同容器非 root 移动缺 `index` 上界 → `insert(begin()+to)` UB（严重，S1）；`Rename`/`SetNameAndPath` 不入撤销栈，而 `bmk.edit` 声明 `undoable=1`（严重，S2）；分类的 `path`/`icon` 从不序列化（严重，S3）；`path:""` 的"改成纯分类"契约未实现 → 僵尸节点（严重，S4）；`SortRecursive` 无条件入栈标脏（中，M4）；H3 无 DL 时会吞掉兄弟节点（低）。Load/Save 两条数据丢失链的护栏（`loadFailed_`、外部变更 + `.conflict.html`）写得很好。 |
| 10 | src/bookmarks.h | 207 | 合格偏优。每个护栏都写了"为什么不是另一种做法"，`PushUndo` 的机器/人工边界、`CountLeaves/Folders/Nodes` 三口径的理由都成立。唯一问题是它所描述的 `EXBAR_ICON`（分类）在实现里根本不落盘（S3）。 |
| 11 | src/folder_tree.cpp | 34 | 合格。10 字段位置序聚合初始化**已逐字段核对，顺序正确**；`kids.first` 作 label、`second` 作 path 也对。 |
| 12 | src/folder_tree.h | 32 | 合格。为什么单独成模块、为什么 `hasChildren` 先给 true（懒加载）都写清了；`minWidthPx=0` 透传正确。 |

合计 3320 行，与 `wc -l` 一致。

---

## 二、严重（会导致错误结果 / UB / 数据丢失 / 契约被静默违反）

### S1 `src/bookmarks.cpp:687-699` — 同容器（非顶层）移动不校验 `index` 上界，`insert(begin()+to)` 越界

```cpp
if (src == dest) {
    if (src == &roots_) return Move(id, index < 0 ? (int)roots_.size() : index);   // 有校验
    const int from = (int)pos;
    int to = (index < 0) ? (int)dest->size() : index;
    if (to > from) --to;
    if (to == from) return false;
    PushUndo();
    BookmarkNode n = std::move((*src)[pos]);
    src->erase(src->begin() + (long long)pos);
    dest->insert(dest->begin() + to, std::move(n));    // ← to 未经夹取，且此时 size 已减 1
```

同类语义在本函数有三处分支，**只有这一处没有上界检查**：顶层走 `Move()`（`bookmarks.cpp:626` 校验
`toIndex <= size`），跨容器分支（`:716`）用 `index < 0 || index > dest->size()` 兜住，唯独"已在目标分类内部、
再在分类内部挪位置"这一支直接用 `to` 当下标。

**触发**：`bmk.move {"id":"b-1","toId":"f-2","toIndex":999}`，且 `b-1` 当前**已经**在 `f-2` 里。
`commands_table.cpp:244` 对该参数的 schema 只有 `"minimum":0`，没有上界；`Args::Int` 也不夹取。
设 `dest->size()==3`、`from==1`：`to = 999 → 998`（`to>from` 后又减 1），`erase` 后 size 变 2，
`insert(begin()+998, ...)` → **堆越界写**（std::vector 未定义行为，表现为崩溃或悄悄破坏相邻对象）。
拖放侧不会传这个组合（`DragFinish` 给 `toIndex` 时不给 `toId`），所以只有 AI / 脚本 / 命令面板能触发 ——
而这条命令正是给机器侧开的。

**修复**（最小改动，与 `:716` 同款）：

```cpp
int to = (index < 0) ? static_cast<int>(dest->size()) : index;
if (to > from) --to;
if (to < 0) to = 0;
if (to > static_cast<int>(dest->size()) - 1) to = static_cast<int>(dest->size()) - 1;  // erase 之后
if (to == from) return false;
```
或者干脆先 `erase` 再夹取，然后复用 `:716-719` 那段"越界即 push_back"的判断。

---

### S2 `src/bookmarks.cpp:471-477`、`:479-487`、`:489-497` + `src/commands_table.cpp:225` — `bmk.edit` 声明 `undoable=1`，改名族却从不入撤销栈

`Rename` / `Update` / `SetNameAndPath` 三个写操作都只 `Touch()`，**没有任何 `PushUndo()`**
（全文件 `PushUndo` 调用点：`:394 AddPath`、`:412 AddFolder`、`:447 Copy`、`:462 Remove`、`:500 ReplaceAll`、
`:614 Merge`、`:633 Move`、`:694/:702 MoveTo`、`:775 Sort`、`:790 Dedupe` —— 改名不在其中）。
而 `bmk.edit` 的 `Effects{0,0,0,0,1,Scope::App}` 第 5 位是 `undoable=true`（`commands_args.h:102`），
并且已登记在 `tools/check_ai_meta.py:265` 的 `UNDO_MECHANISM`。

**触发**：右键某书签 → 编辑 → 改名 → 右键"撤销删除"。此时栈里若还有旧快照（比如刚才的删除），
`Undo()` 会**成功返回并把更早那次操作回滚掉** —— 用户看到"改的名字没撤掉，删掉的书签反而回来了"；
若栈空则报"没有可撤销的操作"。两种情况都是 AGENTS §4.1② 明令禁止的那类"对调用方说谎"。

**为什么静态检查抓不到**：`check_ai_meta.py:2227-2239` 只比对"声明了 `undoable` ⇒ 必须在登记表里"，
登记表是**手写的事实清单**，看不到实现。这条缺陷正好落在那个检查的盲区里。

**附带**：`:388-393` 的注释把 `PushUndo` 的覆盖面写成"Remove/Copy/MoveTo/Sort/Dedup/ReplaceAll/Merge
（新增路径与新增分类已补）"，读起来像已完整覆盖，实际漏了改名族 —— 注释与实现不符。

**修复**：三处进入时先 `PushUndo()`；`Find` 失败分支不需要（早退在 `PushUndo` 之前），
`SetNameAndPath` 若 name/path 都没变化则 `pop_back()`（与 `Remove`/`Merge` 的"无事发生不占额度"同口径）。

---

### S3 `src/bookmarks.cpp:106-111`（写）+ `:58-79`（读）— 分类的 `path` / `icon` 从不序列化，重启即丢

```cpp
if (n.isFolder) {
    out += pad + L"<DT><H3 EXBAR_ID=\"" + Escape(n.id) + L"\" EXBAR_FLAGS=\"" + ... + L"\">" + ... ;
    // 没有 HREF，也没有 EXBAR_ICON
```
`ParseLevel` 的 `h3` 分支同样只读 `EXBAR_ID` / `EXBAR_FLAGS`（`:61-62`），从不读 `HREF`。
而 `BookmarkNode` 有独立的 `path` 字段（`bookmarks.h:21` 只说"分类为空"，并未禁止分类带路径），
UI 侧是**主动支持**分类带路径的：`bookmark_bar.cpp:911-921`（编辑框给分类也开"路径"字段）、
`:722-730`（图标格按分类的 path 列子目录）、`:773`（分类行用自身 path 取图标）、
`bookmark_drop.cpp:121-135`（可把项拖进分类）。

**触发**：右键分类 → 编辑 → 填路径（如 `D:\项目`）→ 用完当天关程序 → 下次启动该分类的路径变空，
"列子目录"退化成"开分类菜单"，图标也退回通用文件夹。数据是**静默丢掉**的，日志、UI、审计都没有痕迹。

**修复**：`Serialize` 的分类分支补 `HREF="…"` 与（非空时）`EXBAR_ICON="…"`；`ParseLevel` 的 `h3`
分支补 `node.path = BookmarkUrlToPath(Unescape(Attr(inner, L"HREF")))`、`node.icon = Attr(inner, L"EXBAR_ICON")`。
`<H3 HREF=...>` 对浏览器/其它读端是无害的额外属性（它们只看 `<A HREF>`），不会破坏互通。

---

### S4 `src/bookmarks.cpp:489-497` + `src/commands_table.cpp:236` + `src/bookmark_bar.cpp:92`、`:920` — `path:""` 的"改成纯分类"契约没实现，结果是不可见、不落盘、撤不回的僵尸节点

三处口径互相矛盾，串起来是一条数据丢失链：

1. `commands_table.cpp:236` 的 schema 文案：`"path":{"description":"新路径；空串表示改成纯分类"}`；
2. `SetNameAndPath` 只做 `n->path = path`，**没有 `n->isFolder = true`**（`:494`）；
3. `bookmark_bar.cpp:92`：`if (!n.isFolder && n.path.empty()) continue;` —— 这种节点栏上不显示；
4. `bookmarks.cpp:112`：`else if (!n.path.empty())` —— 序列化时也不写；
5. `bookmark_bar.cpp:920`：`if (!it.folder || !path.empty()) a.Set(L"path", ...)` —— 分类那一支
   **挡住了**清空（作者知道危险），叶子那一支却无条件透传，于是编辑叶子时把路径清空就走到上面 3、4。

**触发**：右键一个叶子书签 → 编辑 → 把"路径"框清空 → 确定。结果：栏上少一条（看起来像正常删除），
`bookmarks.html` 下次保存后这条也没了（不是删除，是"不再被序列化"），而 `bmk.list` 里它还在
（`isFolder:false, path:""`）。因为 S2 那条，撤销栈里也没有它 —— 三条路都救不回来。

**修复**（三处对齐，选一种并写进 schema 文案）：
- 推荐：`SetNameAndPath` 里 `if (!n->isFolder && path.empty()) n->isFolder = true;` —— 真正兑现
  "改成纯分类"，节点仍可见、仍序列化；
- 或：入口拒绝（`bmk.edit` 在 `!isFolder && path.empty()` 时报 `bad_args`），
  并把 `bookmark_bar.cpp:920` 改成 `if (!path.empty()) ...`；
- 兜底：`Serialize` 对"空 path 的叶子"按分类写，避免"文件里消失"。

---

## 三、中（可用性 / 数据完整性 / 语义偏差）

### M1 `src/bookmark_io.cpp:136-155` — 导出绕过唯一实现，写失败仍报成功，且先截断了目标文件
`CreateFileW(..., CREATE_ALWAYS, ...)` + 两次 `WriteFile` 全部不判返回值、不 `FlushFileBuffers`、
不 temp+replace。对比同一工程的 `tf::WriteText`（`src/text_file.cpp:90-127`，tmp 名带 pid → `FlushFileBuffers`
→ `MoveFileEx(MOVEFILE_WRITE_THROUGH)`，且判短写）。
**触发**：导出到磁盘满 / 网络盘断开 / 目标被占用 → `WriteFile` 失败但函数返回 `CountLeaves()`（正数），
UI 弹"已导出 N 个书签"，磁盘上是一个半截或 0 字节的文件，用户原来那个同名文件已经被 `CREATE_ALWAYS` 清空。
**修复**：直接 `tf::WriteText(file, html)`（BOM 可选：`ReadText` 会自动识别，导出加 BOM 只为兼容旧浏览器，
可以保留但要走同一套落盘三步）；或按 `text_file.cpp` 那三步自己写一遍并判 `written`。

### M2 `src/bookmark_drop.cpp:143-166` — `CFSTR_SHELLIDLIST` 解析无长度与偏移校验
`cida->cidl` 与 `aoffset[i]` 全部来自外部数据对象，代码直接按它们算指针并 `ILCombine`，
既没校验 `GlobalSize(sm.hGlobal) >= sizeof(CIDA) + (cidl+1)*sizeof(UINT)`，也没校验每个 offset 落在块内。
**触发**：第三方拖放源（或坏掉的 NSE）给出畸形 CIDA → 越界读 → `ILCombine` 拿到垃圾 PIDL → 崩溃或误解析成怪路径。
**修复**：`SIZE_T cb = GlobalSize(sm.hGlobal);` 后先校验头部与 `cidl` 上界（如 ≤ 64），
再逐个 `aoffset[i] < cb`；失败则整块跳过（计 `skipped`）。

### M3 `src/bookmark_io.cpp:201-216` — 导入忽略 `Save()` 结果 → "已导入 N 条"可能是假成功
`mode==1` 与 `mode==0` 都只 `store.Save();` 不看返回值。`Save()` 会在 `loadFailed_`
（拒绝覆盖读不懂的文件）或外部变更（改存 `.conflict.html`）时返回 false。
**触发**：书签文件被编辑器改坏 → 用户导入 200 条 → 内存树变了、栏上也有了 → 返回 200、UI 弹"已导入 200 个书签"
→ 但磁盘上什么都没写，关程序即全丢（`.conflict.html` 那条还会把内存树留在另一个文件里，更容易混淆）。
**修复**：`if (!store.Save()) return -4;`（并同步 `bookmark_io.h` 的返回码说明与两个 handler 的文案）。

### M4 `src/bookmarks.cpp:774-778` — `SortRecursive` 无条件入栈 + 无条件标脏
`PushUndo(); SortLevel(...); Touch();` —— 空表、单条、已经排好序时也各来一次。
后果：①"撤销删除"菜单项凭空变可用（`:870`、`:969` 用 `CanUndo()` 判启用），点下去"成功"但什么也没变；
②`dirty_` 置真，关窗口时白写一次盘（并可能触发一次外部变更守卫）。
这与 `Remove`（`:467 pop_back`）、`Merge`（`:619 pop_back`）、`RemoveDuplicatePaths`（`:787-788` 先探一遍）
确立的"无事发生不占撤销额度"口径不一致。

### M5 `src/bookmark_drop.cpp:88-97` — `Drop` 无条件回写 `DROPEFFECT_COPY`，与 `DragEnter/DragOver` 自相矛盾
`DragEnter`（`:72-74`）专门保留源支持的 `COPY|MOVE|LINK` 位，注释理由写得很清楚（"强制 COPY 会被系统对
LINK-only 项取消拖放"）；`Drop` 却直接 `*pdwEffect = DROPEFFECT_COPY`。
**触发**：只支持 LINK 的虚拟项（部分 NSE）拖进来 → OLE 认为目标接受了它不支持的效果 → 拖放被系统判定失败。
**修复**：`*pdwEffect = (*pdwEffect & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (*pdwEffect & DROPEFFECT_LINK);`
（我们本质是"复制一份书签"，优先级 COPY > LINK，都不支持则置 `DROPEFFECT_NONE`）。

### M6 `src/bmk_html.cpp:240-245` — 非 `://` 协议被包成 `file:///`，写出的文件对其它读端是坏路径
`PathToUrl` 只放过 `::` / `shell:` / 含 `://` 三种。`mailto:a@b`、`about:blank`、`chrome://…`（后者含 `://` 无碍）
里的 `mailto:` / `about:` 会被写成 `file:///mailto:a@b`。
**影响**：本工程自己读回来是稳定的（`UrlToPath` 剥前缀后原样还原），所以**不是往返 bug**；
但 `bookmarks.html` 与 mod / 浏览器互通（`bookmarks.h:1-10` 明说），对方会把它当本地路径打开失败。
**修复**：`PathToUrl` 增加"已带识别得到的 scheme（`^[A-Za-z][A-Za-z0-9+.-]*:` 且不是单字母盘符）"原样返回，
与 `UrlToPath` 的非 `file:` 分支对称（`bmk_html_test.cpp:98-100` 已经把"非 file: 原样返回"钉住了，缺的是写出一侧）。

### M7 `src/bookmark_io.cpp:266-278` — UI 版与后端各判一次 `FileChanged`，用户会看到两条提示
UI 版先判并 `MessageBoxW`（"请先重新载入"），随后仍然 `result = BmkImportHtmlFromFile(file, pick)`，
后端再判一次并返回 `-3`，handler（`app_commands_bookmark_io.cpp:39-41`）把它归成 `LockConflict` 并
`NoteFailure`，`bookmark_bar.cpp:1061-1062` 于是再弹一次 `lastBlockReason()`。
**修复**：UI 版判到 `FileChanged` 后直接 `return result;`（-1），不要继续调用后端。

### M8 `src/bookmark_bar.cpp:530-543` — 插入线会用"被溢出折叠的项"的坐标，线画在项中央
`DragUpdate`（`:635`）用可见项算出 `newDrop = i+1`，`i` 是最后一个可见项时 `newDrop` 指向**第一个被折叠的项**
（`Layout` 把它的 `rc`/`iconRc` 置成全 0，`bookmark_bar.cpp:434-435`）。`Paint` 里：
```cpp
const RECT& b = items_[dropIdx_].rc;   // = {0,0,0,0}
gx = b.left - theme_->S(1)/2;          // < 0
if (gx < a.right) gx = (a.right + b.left) / 2;   // → 被拉到最后一个可见项的中间
```
**触发**：书签多到溢出时，把某条往右拖过最后一个可见项 → 插入线闪到那一项正中间，松手却插到末尾（位置对、提示错）。
**修复**：`Paint` 里对 `items_[dropIdx_]` 先判 `rc.right > rc.left`，否则退回 `items_.back()` 的右缘；
或 `DragUpdate` 里把 `newDrop` 夹到"最后一个可见项 + 1"。

### M9 `src/bookmark_bar.cpp:746-804` — 从分类菜单打开目录后不回焦，键盘随即失效
`ShowFolderMenu` 在真正导航处（`:800-801`）只调 `onNavigate_`，没有 `if (refocus_) refocus_();`；
而 `Navigate`（`:702`）与 `ShowChildTree`（`:742`）都调了，且 `:698-703` 的注释把这条纪律写得很清楚
（"焦点留在书签栏窗口上：Delete / F2 / type-ahead / 方向键全部落到栏上"）。
**触发**：Ctrl+B 聚焦书签栏 → Enter 展开分类 → 点/回车选一条书签 → 目录打开了，但 Delete 键无效。
**修复**：`:800-802` 补 `if (refocus_) refocus_();`。

### M10 `src/bookmark_bar.cpp:369-391` — 拖文件到书签栏完全静默
`DragQueryFile` 后过滤 `!(attr & FILE_ATTRIBUTE_DIRECTORY)` 就 `continue`，一条都不加时也没有任何反馈。
**触发**：用户把 5 个文件拖到书签栏 → 光标显示可放置（`DragAcceptFiles(TRUE)` + OLE target）→ 松手什么都没有，
无提示、无日志。**修复**：至少 `MessageBeep` + 一条 `MessageBoxW("书签只接受文件夹")`，并 `LogDbg` 计数。

---

## 四、低（死代码 / 注释不符 / 边界卫生）

1. `src/bookmarks.cpp:64-65`、`:88-89`：`if (node.name.empty() && ...)` —— `node` 刚构造，`name` 必然为空，
   条件是恒真的死判据；注释"标签内没有名字时取后续文本 token"暗示有两条取名路径，实际只有一条。
2. `src/bookmarks.cpp:72-79`、`src/bookmark_io.cpp:74-81`（两份 `ParseLevel` 同病）：`<H3>` 之后若无 `<DL>`，
   内层 `while (... 找下一个开始 DL) ++pos` 会**扫过兄弟 `<DT>`**，把后面第一个 `DL` 当成这个分类的子表 →
   兄弟条目被吞进上一个分类。浏览器导出总会写 `<DL>`，手改文件/第三方工具则不一定。
   修：遇到下一个 `<DT>` 就停（视为空分类）。
3. `src/bookmarks.cpp:35-36`、`src/bookmark_io.cpp:39`：`depth > 32` 直接 `return`，`pos` 停在 DL 内容处 →
   第 33 层以下的子树被**摊平**挂到上一层（结构损坏而非丢弃）。
4. `src/bookmarks.cpp:712-715`：`if (!dest) { Touch(); return false; }` —— 此分支里节点已从 `src` 摘除、
   `n` 即将析构，却返回 false 并标脏：真走到就是"静默删一条 + 落盘"。当前不可达（注释也这么说），
   但应按"回插原处"收尾，否则下次重构会变成真数据丢失。
5. `src/bookmarks.cpp:817-831`：`IsValidShellNamespacePath` 末尾 `return pos == 39;` 在
   `path.size() == 40` 已保证时恒真 —— 死判据（当断言留着无害，但别当校验）。
6. `src/bmk_html.cpp:320-335`：孤立**高**代理被替换成 U+FFFD，孤立**低**代理走三分支编码后又解回同一个
   孤立代理（静默放行）—— 两条同类路径口径不一致。
7. `src/bmk_html.cpp:346` + `src/bookmarks.cpp:90`：坏字节 → 返回空串 → 整条书签**静默丢弃**，
   且 `bookmarks.cpp` 这一侧**没有日志**（`bookmark_io.cpp:95-97` 记了 `href`）。读自家文件丢条目时查不到痕迹。
8. `src/bmk_html.cpp:125-152`：`Tokenize` 丢弃纯空白文本 token，而 `ParseLevel` 用 `Trim` 取名字 →
   "名字全是空格"或"名字带首尾空格"的书签往返后被改写成空名/去空格名。
9. `src/bookmark_io.cpp:21-22`：`using str::Lower;` 在本文件**零使用**（死 using）；
   `#include "prompt.h"`（`:9`）也无用（本文件用的是 `MessageBoxW` 与 `ui::Menu`）。
10. `src/bookmark_io.cpp:187`：`static bool ParseHtmlFile(...)` 是文件级 static，同文件其它辅助函数都在
    匿名命名空间（`:18-134`）—— 两种收法混用。
11. `src/bookmark_io.h:14-17`：UI 版注释只写 `-1=用户取消/文件无效`，实现还会经后端返回 `-3`（见 M7）。
12. `src/bookmark_bar.cpp:806-812`：`FolderItemAt` 里 `POINT pt = screen;` 与 `POINT cl = pt;` 是死赋值/死变量。
13. `src/bookmark_bar.cpp:544`：`RECT bar{gx, theme_->S(3), gx + theme_->S(2), rc.bottom - 2}` —— `- 2` 是硬编码像素，
    违反"不写死像素，全走 `theme_->S()`"（同文件其余尺寸都走令牌）。
14. `src/bookmark_bar.cpp:521`：注释被塞在 `if (...) {` 行尾（"// 拖进分类：整项描边…"接在代码后），
    后续两行又是同段注释的续行 —— 排版事故，读起来很容易漏掉 `{` 后面的分号判断。
15. `src/bookmark_bar.cpp:847-849`：注释说"锚点取溢出按钮本身…而不是从屏幕那个已经不在栏上的坐标展开"，
    实现传的是 `screen`（`:849`，来源是 » 的点击点）—— 结论偶然正确，注释与代码不一致。
16. `src/bookmark_bar.h:3`：`%APPDATA%\ExBar\bookmarks.html` 与 `bookmarks.h:3` 的 `<exe>\data\bookmarks.html`
    互斥，两处注释必须改一处。
17. `src/bookmark_bar.h:95`：头注释说 `kbdIdx_` 落在"第一项（或上次的位置）"，实现里 `WM_KILLFOCUS`
    （`:144-147`）一律清成 -1，`WM_SETFOCUS`（`:141`）只会给 0 —— "上次的位置"不存在。
18. `src/bookmark_bar.cpp:137-138`：注释写"（WS_TABSTOP + Tab 键到达）"，与 `bookmark_bar.h:29-30`
    自己说明的"本程序不跑 `IsDialogMessage`，Tab 不是焦点遍历键"矛盾；`WS_TABSTOP` 实际不起作用。
19. `src/bookmark_bar.cpp:82-84`：`static HICON folderIcon` 是函数级可变状态（"栏与组件内部不得有
    文件级可变状态"的精神），且不随主题/DPI 重建 —— 换主题后纯分类图标不跟着换（`ico::Cached` 那份会）。
20. `src/bookmark_drop.cpp:197`：`hoverFolder_` 在 `DragEnter/DragOver/DragLeave/Drop` 里写，**从不读**
    （死字段）—— 拖到分类上没有"将放进此分类"的视觉反馈，与 `bookmark_bar` 的 `dropInto_` 描边能力没接上。
21. `src/bookmark_drop.cpp:182`：`owner_->ReloadExternal();` 未判空，而同一文件的 `DropOne`（`:122`）判了。
22. `src/bookmark_drop.cpp:200-220`：`g_drop` 是文件级可变状态，且只支持一个宿主 —— 第二个 `BookmarkBar`
    调 `BmkDropAttach` 会被 `if (g_drop) return;` 静默忽略（OLE 语义下难以避免，但应显式记一行日志）。
23. `src/bookmark_drop.cpp:39-40`：`SHGetPathFromIDListW` 用 `MAX_PATH * 2` 定长缓冲，超长路径会被判不合法；
    本工程其它地方统一走 `tf::ToExtended`。
24. `src/bookmarks.cpp:486`、`:496`：`Update` / `SetNameAndPath` 在写入内容与原来完全相同时也 `Touch()` → 白标脏。
25. `src/folder_tree.cpp:21`：10 字段**位置序**聚合初始化（AGENTS 明确要求先读完整 struct 定义）。已核对
    `TreeNode`（`popup_list.h:103-115`）顺序为 `label, path, icon, depth, parent, children, hasChildren,
    expanded, loaded, glyph`，**本处传值正确**；但建议改指定初始化器，避免 `TreeNode` 增字段时静默错位。

---

## 五、工程约束检查

| 项 | 结果 |
|---|---|
| 品牌串（只允许 `identity.h`） | **通过**。12 个文件里 `L"ExBar…"` 字面量 0 处；`bookmark_bar.cpp:925/948/1043` 走 `id::kBrandShort`，类名走 `id::kBookmarkBarClass`（`:22`）。`bookmark_bar.h:3`、`bookmarks.h:3` 的 `ExBar` 只在注释里，且两处互相矛盾（见低-16）。 |
| 硬编码色 / `RGB(` | **通过**。0 处 `RGB(`、0 处 6 位色值；颜色全部 `theme_->xxx`。唯一违规是像素常量 `rc.bottom - 2`（低-13）。 |
| 命名空间 | **通过**。无 `namespace eb`；`bmkhtml` / `str` / `ui` / `ico` / `nav` / `folder_tree` 均按领域。 |
| 单文件 ≤500 行 | **超限但在棘轮基线内**：`bookmark_bar.cpp` 1073、`bookmarks.cpp` 893（`data/file-length-baseline.json` 记账值同为 1073/893，未涨）。`bookmarks.cpp` 实际承担 4 件事（store 写操作 / 撤销栈 / HTML 解析序列化 / 去重排序），**不满足"一文件一职责"**，建议按作用域拆成 `bookmarks_write` + `bookmarks_html`（解析序列化）+ `bookmarks_ops`（排序去重）。 |
| 共用机制只有一份 | **部分违反**：①`bookmark_io.cpp:136-155` 绕开 `tf::WriteText` 自己写盘（M1）；②`ParseLevel` 两份，但 `bmk_html.h:8-13` 已论证"语义分叉是有意的"，且实际差异确实存在（读不读 `EXBAR_*`），**判定合格** —— 只是两份都带同一个 H3/DL 缺陷（低-2）。 |
| 头文件自足 | **通过**。`bookmarks.h` 显式 `<windows.h>` 拿 FILETIME 并注明理由；`bookmark_bar.h` 自带 `bookmarks.h` / `tooltip_host.h` / `theme.h`；`bmk_html.h` 零 Win32；`folder_tree.h` 自带 `popup_list.h`（`TreeNode`）。 |
| 无全局可变状态 | **两处**：`bookmark_drop.cpp:200 g_drop`（OLE 必需的宿主引用，但应记日志）、`bookmark_bar.cpp:82 static HICON folderIcon`（低-19）。 |
| 注释与实现一致 | **多处不符**：`bookmarks.cpp:388-393`（覆盖面漏改名族，S2）、`bookmark_bar.h:3`/`bookmarks.h:3`（存储位置互斥）、`bookmark_bar.h:95`（"上次的位置"）、`bookmark_bar.cpp:137`（Tab 到达）、`bookmark_bar.cpp:847-849`（锚点）、`commands_table.cpp:236`（"空串表示改成纯分类"未实现，S4）。 |
| R21 输入纪律 | **通过**。12 个文件无 `SendInput`/`PostMessage(WM_*)`/`SetCursorPos`/`SetForegroundWindow`；`SetFocus` 只在真实鼠标/命令路径（`bookmark_bar.cpp:281`、`:682`），属允许项。 |
| `undoable=1` 必有撤销机制 | **违反**（S2）。 |

---

## 六、总体评价

这组代码的**文档与防御性**明显高于本仓平均水平：`bookmarks.cpp` 的两条数据丢失链
（`loadFailed_` 拦"读不懂就别写"、外部变更改存 `.conflict.html` 而不是死拒）写法是教科书级的，
每处护栏都同时写了"为什么不是另一种做法"；`FileChanged` 把"打不开 = 视为已变更"、
"mtime 相同再看 size"这两个容易反的判据都摆正了；`bmk_html` 的 UTF-8 百分号解码修复
（旧实现按宽字符解 `%E4%B8%AD`，中文路径永久损坏）有单测钉住，`Attr` 的 `_` 左边界坑也留了记录。
拖放全部收敛到 `CommandBus`（不再绕审计/撤销）、`ParseLevel` 的两份**故意不合并**并写明理由，
都是对的方向。`bookmark_bar` 的键盘路径（←/→/Home/End/Enter/Shift+F10/Esc）、溢出只列被折叠项、
`dropInto_` 与插入线两种提示的区分，说明作者真的按 Opus/TC 的行为对过。

问题集中在**同一套语义的三处分支没有同时改完**（这是本组的共同病灶）：

- `MoveTo` 的三个分支里，顶层有校验、跨容器有校验，**同容器非 root 那一支漏了** → 可致堆越界（S1）；
- 写操作一族里 `Add*`/`Remove`/`Copy`/`Move*`/`Sort`/`Dedup`/`Merge`/`ReplaceAll` 都补了 `PushUndo`，
  **改名族漏了**，而命令表照样声明 `undoable=1`，静态检查因为只比对登记表而永远绿（S2）；
- `Serialize` 的叶子分支写 `HREF`、分类分支不写，而解析两侧也对称地漏了分类 → 分类路径/图标静默丢（S3）；
- `path:""` 在 schema 里承诺"改成纯分类"，`SetNameAndPath` 没置 `isFolder`，UI 一侧又只挡住了分类
  （叶子放行）→ 节点变得不可见、不落盘、撤不回（S4）。

四条严重里有三条是**同一个模板的第三次漏改**，说明"新增一个写路径时清点所有出口"这条机制目前靠人记。
建议除了修 S1–S4，再补两条机械守卫：①`BookmarkStore` 的每个 public 写函数入口强制 `PushUndo`
（用 RAII guard，让"忘记"不可能）；②`bmk.move` 的 `toIndex` 在 handler 层就按目标容器实际 size 夹取，
store 层再兜一次 —— 两层都夹，单层迟早再漏。

中的 10 条里，M1（导出写盘）与 M3（导入忽略 Save 结果）是同一类"返回值不看"，只是方向相反：
一个是写失败报成功，一个是没写盘报成功。两者都会让用户以为数据保住了。这两条建议与本组的
S3/S4 一起修 —— 它们共同构成了"书签到底有没有真的存下来"这条链上剩下的口子。

低级别的 25 条里，真正值得动手的是低-2（H3 无 DL 吞兄弟节点）与低-7（坏路径丢条目无日志）：
前者是第三方格式兼容的实质缺口，后者是"我明明有 300 条怎么只剩 298 条"这类无法排查的投诉来源。
其余多为注释漂移与卫生问题，可在拆分 `bookmarks.cpp` 时顺手清掉。
