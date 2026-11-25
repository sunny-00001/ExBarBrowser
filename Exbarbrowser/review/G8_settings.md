# G8_settings 组代码审查报告

审查范围：`review/G8_settings.txt` 列出的 17 个文件（settings_ / config 层），穷举式逐文件读完，
逐函数逐分支核对。只读 `src/`，未构建、未改源码。

> **行数口径差异（须知）**：清单写"共 3980 行"，本次实测 **4030 行**。
> `settings_dialog.h` 271→277、`settings_dialog_model.cpp` 417→442、`settings_dialog_actions.cpp` 436→455
> （`editKey_` / `badKey_` 那一批改动），本报告按**当前磁盘内容**行号给出定位。
> 结论不受影响；但清单的行数快照已过期，建议重算。

---

## 一、文件覆盖清单（17/17，全部读完）

| # | 文件 | 行数 | 结论 |
|---|---|---|---|
| 1 | src/config.cpp | 238 | **不合格**：整个匿名命名空间（7 个辅助函数）是 `config_io.cpp` 的逐字副本且本 TU 零引用 = 死代码（S1）；另有未用 include、冗余 API、多余空行 |
| 2 | src/config.h | 257 | 基本合格：注释与哨兵 `kHotkeyDisabled` 自相矛盾（M1）；默认 `statusSegs` 硬编码 0..6 未与 `Seg_Count` 关联（L1） |
| 3 | src/config_apply.cpp | 56 | 基本合格：35 个可写键全覆盖、与表无分叉；`tabMinWidth/tabMaxWidth` 归入"需要动作"却无落地类别（M3） |
| 4 | src/config_apply.h | 44 | 合格：判据分类口径清晰，无死接口（`NeedsAnyApply`/`IsPureData` 均被 tests 使用） |
| 5 | src/config_io.cpp | 398 | **不合格**：解析失败护栏做对了，但 `Save()` 全量重建 root → 未知键被抹掉（S2）；解析失败后命令层仍报成功=假成功（S3）；`searchRange`/`hotkeys`/`namedSessions` 三处读盘口径与写入侧不一致（M6/L10/M1） |
| 6 | src/settings_dialog.cpp | 248 | 基本合格：生命周期/消息路由正确，`WM_DESTROY` 收口完整；`SetFocus(h)` 先于 `OnClick` 造成编辑态切换丢输入（M7 的触发点） |
| 7 | src/settings_dialog.h | 277 | 合格（结构/不变式注释质量高）：`isOpen()` 命名不合规且与 `Close()` 同为死接口（L5）；分组清单注释过期（L7） |
| 8 | src/settings_dialog_actions.cpp | 455 | 基本合格：所有写入都走 `config.set`（人机同源落实到位）、失败原因原样上报；编辑态的"换行不提交"丢输入 + 多发一条 config.set（M7）；帮助按钮只写一行状态文案（L14） |
| 9 | src/settings_dialog_draw.cpp | 301 | **不合格**：全窗口零裁剪，滚动时行会画到中栏头部、左栏分类行会画到滚轮提示/页脚上（S6）；文件头注释把布局指到 `settings_dialog.cpp`（L6） |
| 10 | src/settings_dialog_draw_help.cpp | 126 | **不合格**：右栏命中状态 `right_.empty` 在 `PaintRight` 里写（S4）；只读行"该项由程序维护"文字区域可点（M10）；状态栏行高显示未缩放（L9） |
| 11 | src/settings_dialog_model.cpp | 442 | 基本合格：几何单一出处（`RowLayout`/`RightLayout` 被绘制与命中共用）落实到位、命中未用行号；`LayoutRight()` 无条件 `empty=true`（S4）；左栏分类无滚动（M8）；搜索词 128 字符上限（M9） |
| 12 | src/settings_reg.cpp | 243 | 合格：写后回读、默认值同源、`SplitDoc` 括号规则正确；`Set` 失败时状态已被改动这一点未回滚（见 S5 的连带影响） |
| 13 | src/settings_reg.h | 152 | 合格：契约（key 永不改名、doc 格式、唯一实现）表达清晰，无实现泄漏 |
| 14 | src/settings_reg_table.cpp | 469 | 基本合格：**已 include `status_bar.h` 取 `Seg_Count`（第 22 行），且 `IntArraySet` 的上界是排他的 → `0..Seg_Count` 正好等价 `0..8`，无越界**；`tabNewPos` 的 "right=最右" 与实现不符（M2）；顺序注释缺三组（L7） |
| 15 | src/settings_reg_table.h | 23 | 合格（纯入口声明），仅"合成一个文件会到 400 行上下"已过期（实 469，L12） |
| 16 | src/settings_reg_valid.cpp | 238 | 不合格：`StrEnumSet` 大小写不敏感校验却存**原串**，落盘后重启被 `config_io` 归一化（S5）；`IntArraySet` 不排序去重与 Load 口径不一致（M4）；"18 个可写项"实为 35（M11） |
| 17 | src/settings_reg_valid.h | 63 | 合格（纯声明），但文件头把"键表本体"指到 `settings_reg.cpp`（L8） |

行数约束：17 个文件**全部 ≤500 行**（最大 `settings_reg_table.cpp` 469），也都不在 `data/file-length-baseline.json` 的欠账清单里 —— 无违规。
配色/品牌串：本组 `src/` 文件**零 `RGB(`**、零 `L"ExBar…"` 字面量（仅注释提到 ExBar mod 作为历史背景），符合 §1.3。

---

## 二、严重问题（数据丢失 / 静默假成功 / 功能不可达）

### S1 `src/config.cpp:9-87` —— 整份读盘辅助函数是死代码，且与 `config_io.cpp` 逐字重复
`ClampInt`(13)、`ReadStringArray`(15)、`ReadStringArrayKeepEmpty`(31)、`MakeStringArray`(40)、
`MakeIntArray`(46)、`MakeTabMeta`(55)、`ReadTabMeta`(69) 在本 TU **一个都没被调用**（`Load`/`Save` 早已搬到
`config_io.cpp`，那里有一位一份的同名实现）。后果：
- 每次编译这 7 个 `static` 函数都会进 `-Wunused-function`（工程基线是"零警告"）；
- 最贵的是**两份夹取/保位逻辑并存**：谁按文件顺序先看到 `config.cpp` 那份去改（比如修 `sessionPane2Paths`
  的空串保位），改动不生效，也不会报错——只有等到"右窗格路径挂错标签"才会被发现。

修复：删除 `config.cpp:9-87` 整段匿名命名空间，同时删掉第 4 行已无用的 `#include "search_scope.h"`、
第 7 行 `#include <algorithm>` 按是否仍用 `std::remove` 决定（`AddRecent`/`AddSearchHistory`/`ClearHotkeyOverride`
仍在用，保留）。若担心回归，用 `tools/check_components.py` 加一条"同名辅助函数只允许一处定义"的规则。

### S2 `src/config_io.cpp:302-398` —— `Save()` 全量重建 JSON，未知键被静默抹掉
`Save()` 从头 `MakeObject()` 再逐键 `Set`，**没有**以读到的原文为基底。于是：
- 新版本写过、旧版本不认识的键（`Load` 里"认不出就忽略"的那类），只要旧版本跑一次 `Save` 就永久消失；
- 用户手写在 `settings.json` 里的自定义键（脚本/AI 备注、外部工具写入的字段）同样被清掉；
- `version` 恒写 1，降级/升级之间没有任何迁移锚点，只能靠这行注释理解。

这与本文件头（`config_io.cpp:11-13`）承诺的"向前兼容"只完成了"不报错"一半，"不丢数据"那半没做。

修复：`Config` 增加 `json::Value rawRoot_`，`Load` 成功解析后整份保存；`Save` 以 `rawRoot_` 为基底
（先 `root = rawRoot_`，再逐键覆盖已知项，最后整体写回），并同样在 `Save` 成功后回写 `rawRoot_ = root`。
单测：写一份含未知键的文件 → `Load()` → `Save()` → 读回仍含该键。

### S3 `src/config_io.cpp:305-308` + `src/app_commands_misc.cpp:170` —— 解析失败护栏只挡写盘，命令层仍"报成功"
`parseFailed_` 的护栏本身是正确的（`Load` 读不出/解析失败都置位，`Save` 早退，用户的 `recentPaths` /
`sessionPaths` / `lockedPaths` / `closedTabs` / `hotkeyOverrides` 不会被默认值覆盖）——这条做对了。
缺口在**语义**：`config.set` 先改内存、再 `Save()`（静默返回），`CommandBus` 照样 `ok=true`，
设置面板照样弹"已执行 config.set tab = false"。用户/脚本以为改成功了，重启后全部回退，
唯一痕迹是 `%TEMP%` 日志里一行。这恰恰是 `config_apply.h` 开头点名要消灭的那类"假成功"。

修复：给 `Config` 加 `bool WriteBlocked() const { return parseFailed_; }`；
`ConfigSet` 在 `Save()` 前判断，若被拦就 `NoteFailure(L"settings.json 解析失败，本次改动不会落盘：请修好或删除 data\\settings.json 后重启")`
并 `return false`（内存改动是否回滚可择一，但必须**不再报成功**）；设置面板的 `WriteValue` 已有"原样上报原因"的路径，无需改动。

### S4 `src/settings_dialog_draw_help.cpp:29` + `src/settings_dialog_model.cpp:406` —— 命中状态由绘制函数写入
`LayoutRight()` 无条件 `right_.empty = true;`，而把 `empty` 改成 `false` 的唯一位置是 `PaintRight()`
（`right_.empty = (row == nullptr);`）。`HitRightAction()`（`model.cpp:428-435`）第一行就 `if (right_.empty) return 0;`
——也就是说**右栏三个动作按钮可不可点，取决于"上一次重绘之后有没有再 Layout"**。
`WM_SIZE` → `Layout()`（重置为 `true`）→ `InvalidateRect`（异步）之间到达的点击，会静默失效；
同理 `OnConfigChangedExternally()`（`settings_dialog.cpp:100-110`，AI/命令改配置后必经）也是 `Layout()` + 异步重绘。
这是"命中与绘制共用一份几何"原则的常见破口：几何是共用的，但**有效性判定**被放进了绘制。

修复：把 `right_.empty` 的赋值挪到 `LayoutRight()` 里（`right_.empty = (RowByKey(selKey_) == nullptr);`），
`PaintRight` 只读不写。这样点击永远看到的是 Layout 之后的一致状态。

### S5 `src/settings_reg_valid.cpp:139-158` + `src/config_io.cpp:190-191` —— `StrEnumSet` 大小写不敏感校验、却存原串，落盘后重启丢值
`StrEnumSet` 用 `str::IEquals` 判合法（对），但通过后执行 `*field = s;`（`s` 是**用户输入的原始字符串**，
不是 `allowed` 里的规范值）。链路实测：
`config.set themeMode "Dark"` → 存 `"Dark"` → 写后回读 `"Dark"` == 输入 → **报成功** → `Save()` 落盘 `"themeMode": "Dark"`
→ 下次启动 `Load()` 在 `config_io.cpp:191` 归一化成 `"system"`（因为它只认全小写的 `dark`/`light`）。
用户看到的现象是"我明明设了深色，重启变回跟随系统"，而命令层从没说会这样。
同族问题：`tabNewPos` 存 `"RIGHT"`/`"End"` 时 `app_tabs.cpp:76-79` 只按 `IEquals("end")` 分支，
`"RIGHT"` 会落进"当前右侧"分支（侥幸没出错，但同样是归一化缺失）。

修复：`StrEnumSet` 命中时 `*field = a;`（写**规范值**）。副作用是 `Set()` 的写后回读可能报"不一致"
（用户写 `"Dark"`、读回 `"dark"`）——这恰恰是对的，同时把 `Set()` 的回读口径改成"与 `get` 的规范化结果比较"
或在 `Set` 里先做同样的规范化，避免把正确的归一化判成失败。

### S6 `src/settings_dialog_draw.cpp:160-170`、`:53-82` —— 全窗口零裁剪，越界绘制
`ui::PaintBuffered` 给的是**整窗**内存 DC（`ui_draw.cpp:283-306` 无裁剪），而设置面板没有任何
`IntersectClipRect` / `SaveDC`（本组 5 个 dialog 文件 grep 为零）。两处由此出问题：
1. **中栏**：`PaintMiddle` 先画头部（面包屑/密度/帮助，`:118-157`），再画行（`:160-170`）。行的裁剪条件只有
   "整行在 `form_` 之外才跳过"，**跨边界的那一行仍按 `rowLayout_` 原样绘制**。即 `scroll_` 稍大时，
   第一行的名称/说明会以垂直居中位置画到 `head_` 区域上，压住面包屑与两个按钮。
2. **左栏**：`PaintLeft` 按 `catList_.top + i*rh` 画全部 12 个分组（`:53-82`），没有裁到 `catList_`；
   窗口压扁时（最小高度 420）分类行会画进 `hint_`（滚轮提示）与 `foot_`（页脚计数）里，
   文字互相叠印。而命中侧 `HitGroup` 会因 `PtInRect(catList_)` 拒绝这些位置 —— **画得出来但点不动**。

修复：`PaintMiddle` 在画 `items_` 之前 `SaveDC(dc); IntersectClipRect(dc, form_.left, form_.top, form_.right, form_.bottom);`
画完 `RestoreDC(dc, -1)`；`PaintLeft` 同样对 `catList_` 做一次。裁剪后 M8（分类不可滚动）的可见症状也会一起收敛。

---

## 三、中等问题（口径分叉 / 覆盖率假象 / 交互缺陷）

### M1 `src/config.h:179-180` 与 `:25-27` —— 同一个头文件里两套"解绑"说法
第 179 行写 `// 快捷键覆盖：setting -> "Ctrl+Shift+T"（空串 = 显式禁用）`，而第 27 行刚声明
`constexpr wchar_t kHotkeyDisabled[] = L"-";` 并解释"空字符串做不到这件事"。
实现侧两套都**生效**但效果微妙不同：`key_binding.cpp:65-67` 中空串覆盖使 `ResolveKey` 返回 `""`，
随后 `if (key.empty()) continue;` 直接丢掉该绑定（等价于解绑）；而 `input_router.cpp:76` 只把 `"-"` 显示成
"（未绑定）"，空串显示为**空白**。于是手写 `"hotkeys": {"nav.back": ""}` 会得到一个"看起来正常、按不出来"的键位行。

修复：把 179 行注释改成 `（"-" = 显式解绑，见 kHotkeyDisabled；空串非法）`，并在 `config_io.cpp:295-299`
读盘时把空串值归一成 `kHotkeyDisabled`（或直接丢弃该条），使"文件里只有一种解绑写法"。

### M2 `src/settings_reg_table.cpp:263-269` —— `tabNewPos` 文档承诺三档，实现只有两档
表里给出 `right=最右 / after_current=当前右侧 / end=末尾`，而唯一消费点 `app_tabs.cpp:76-79`：
```cpp
if (str::IEquals(pos.c_str(), L"end")) return tabSet_.Count();
return ActiveIndex() + 1;   // "right" 与 "after_current" 走同一条
```
即 **`right` 的实际行为是"当前标签右侧"，不是"最右"**，且与 `after_current` 完全等价。用户在设置面板
选"最右"、新标签插在当前标签旁边 —— 界面文案在说谎，属于本表自己反对的那类"界面显示与落盘语义对不上"。

修复：二选一 —— ①在 `NewTabIndex()` 里补 `right` 分支（`return tabSet_.Count();` 真落最右）；
②把 options 收敛为两项（`right=当前右侧` / `end=末尾`）并同步改 `config_io` 默认值语义与 doc。
考虑到 `settings.json` 键值已是跨软件契约，倾向①。

### M3 `src/config_apply.cpp:34` —— `tabMinWidth/tabMaxWidth` 被声明"需要动作"，但没有任何落地类别
`NeedsAnyApply()` 特判里有这两个键，`AffectsLayout()` 没有，`ConfigSet`（`app_commands_misc.cpp:152-225`）
也没有对应分支。`tests/config_apply_test.cpp:60-68` 的"全覆盖"因此**形式上通过、实质未验证**。
当前之所以看起来能用，是因为 `SyncChrome()` → `TabStrip::SetTabs()` → `Layout()`（`tab_strip.cpp:98`）
这条**副作用链**顺带重算了宽度（`tab_strip_layout.cpp:53-54` 每次 Layout 现读配置）。

风险：这类"靠别人的副作用落地"一旦被重构（`SetTabs` 不再调 `Layout`、或加了脏值短路），
就退化成"命令 ok=true、标签宽度没变"，且测试仍绿。

修复：把 `tabMinWidth`/`tabMaxWidth` 加进 `AffectsLayout()`（语义上它们确实改标签条的客户区分配），
或给 `ConfigSet` 加显式分支 `tabStrip_.Layout()`；并在 `tests/config_apply_test.cpp` 加一条断言钉住这一点。

### M4 `src/settings_reg_valid.cpp:174-192` 与 `src/config_io.cpp:285-293` —— `statusSegs` 的写/读口径不一致
`IntArraySet` 只逐元素查上界（`0..Seg_Count` 排他，正确），**不排序、不去重**；
而 `Load` 明确做了 `sort + unique`（注释：旧版 append 式切换会写出乱序/重复段号，状态栏右对齐依赖稳定升序）。
于是 `config.set statusSegs [5,5,3]` → 回读等于输入 → 报成功 → 落盘 `[5,5,3]` → 重启变 `[3,5]`；
中间这段时间 `StatusBar::Layout`（`status_bar.cpp:181-187`）会把段 5 排两次、量两次宽、画两次。

修复：`IntArraySet` 落字段前 `sort + unique`（归一化放在**唯一实现**里），或让 `Set()` 的比较走"归一化后比较"；
两者取其一即可，但不要两处各写一套。

### M5 `src/settings_reg_table.cpp:255-258` 与 `src/config_io.cpp:176` —— `holdDelayMs` 的合法区间两处不同
注册表：`IntSet(&m.holdDelayMs, ..., 200, 2000)`（面板 `minValue/maxValue` 也是 200..2000，行内编辑按它校验）；
读盘：`if (s_.holdDelayMs < 300) s_.holdDelayMs = 800;`。
于是 `config.set holdDelayMs 250` **成功**、落盘 250、重启变成 800 —— "设了不生效"的经典形态。
（对照：`seqTimeoutMs` 两处都是 200，没有这个问题。）

修复：统一为 200（改 `config_io.cpp:176` 的阈值，或把表的区间抬到 300）；推荐前者并顺手把
`config_io.cpp:175-176` 换成用注册表口径的常量（`kSeqTimeoutMin` / `kHoldDelayMin`），避免第三处硬编码。

### M6 `src/config_io.cpp:276-277` —— `searchRange` 读盘允许 `RangeSupported()==false` 的档位
`Load` 只做 `ClampInt(..., 0, kRangeCount - 1)`，因此手改 `"searchRange": 2`（此电脑）能进内存；
而 `config.set searchRange 2` 被 `IntCheckedSet` + `searchscope::RangeSupported` 明确拒绝 ——
"设置接受、命令拒绝"的分叉，正是 `settings_reg_table.cpp:378-386` 注释里要消灭的那种。
下游 `searchscope::Resolve` 会返回 `valid=false` 并报错（不会悄悄搜错范围），所以不是数据事故，但是口径不唯一。

修复：`Load` 改用 `if (!searchscope::RangeSupported(s_.searchRange)) s_.searchRange = searchscope::kDefaultRange;`
（`search_scope.h` 已 include 在本文件）。

### M7 `src/settings_dialog_actions.cpp:321-334` + `src/settings_dialog.cpp:220-224` —— 编辑数字后直接点另一行的数字框：旧输入静默丢弃，还多发一条 config.set
`WM_LBUTTONDOWN` 先 `SetFocus(h)`（`settings_dialog.cpp:221`）→ 触发 `valueEdit_` 的 `WM_KILLFOCUS` →
`ValueSub` `PostMessageW(kMsgCommitEdit, 1)`（`actions.cpp:446-450`，异步）；紧接着同一次 `OnClick` 里
`BeginEdit(新行)` 已把 `editKey_` 换成新键、`SetWindowTextW` 换成新值。等那条 post 消息到达时，
`CommitEdit(true)` 提交的是**新键 + 新值**：旧输入（比如 999）被丢掉，且凭空产生一条
"已执行 config.set seqTimeoutMs = 2000" 的状态与审计（用户没改这个值）。

修复：`BeginEdit` 开头先收口：`if (!editKey_.empty() && editKey_ != row.entry->key) CommitEdit(true);`
（`CommitEdit` 内部已经按 `editKey_` 取旧键与旧文本，顺序必须在此之后再 `SetWindowTextW`）。

### M8 `src/settings_dialog_model.cpp:411-419` + `src/settings_dialog_draw.cpp:53-82` —— 左栏 12 个分组没有滚动，最小窗口下 3 组点不到
12 组 × 行高（32/28/24）最大 384px；最小窗口高 420 时 `catList_` 只有约 288px（`Layout()` 里
`searchBox_.bottom+8` → `hint_.top`）→ 底行高下只能显示 9 行。`HitGroup` 用 `PtInRect(catList_)`
做前置门，超出部分返回 -1，因此"状态栏段 / 键位 / AI 通道"三组在最小高度下**点了没反应**（滚轮还能切，但那是另一个手势）。

修复：给 `catList_` 加滚轮/滚动偏移（或在组数超出时按可用高度压缩行高，最小到 `kRowHeights[2]`）；
至少要在 `HitGroup` 里对 idx 做 `idx*rh < catList_.height` 的显式判定并配合 S6 的裁剪，
做到"画得出来的才点得到"。

### M9 `src/settings_dialog_model.cpp:108-113` —— 搜索词 128 字符硬上限
`wchar_t buf[128]` + `GetWindowTextW(edit, buf, 128)`：超过 127 字的搜索词被截断，且截断后
`UpdateSearchFromControl` 会把它当成"用户的搜索词"（与原输入不一致但界面不提示）。
设置项的 doc 都是短句，实际很难触发，属于隐患而非现网缺陷。

修复：`int n = GetWindowTextLengthW(edit); std::wstring s(n, L'\0'); GetWindowTextW(edit, s.data(), n+1);`
（或沿用项目里别的编辑框读法，保持唯一实现）。

### M10 `src/settings_dialog_draw_help.cpp:90-95` + `src/settings_dialog_model.cpp:428-435` —— 只读行的说明文字区域是"隐形按钮"
只读行在 `right_.restore` 位置画的是"该项由程序维护，设置面板只能查看。"（纯文本），
但 `HitRightAction` 只看矩形：点在 `restore` 上仍返回 1 → `RestoreDefault()` → 弹红色
"该项只读：xxx"。用户点了一句说明文字，得到的是一条错误提示。

修复：`HitRightAction` 增加"只读行不命中 restore"的判据（`RowByKey(selKey_)` 的 `writable`），
或让只读行把该区域画成灰字且返回 0。

### M11 `src/settings_reg_valid.cpp:62` —— 注释写"18 个可写项"，实际 35 个
表本体共 55 项，只读 20 项，可写 **35** 项（`tests/config_apply_test.cpp:87` 的末行打印会给出真实数）。
这类"数字注释"没有检查会报，但会误导下一个改 `Setter` 的人以为漏一两个无所谓。

修复：改成"每个可写项都从这里取构造器"，不写具体数字（数字必然漂移，符合"文档不写行号"的同一条纪律）。

---

## 四、低风险 / 一致性 / 注释不符

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| L1 | `src/config.h:168` | 默认 `statusSegs{0,1,2,3,4,5,6}` 硬编码 7 段，无注释解释第 7/8 段（`Seg_Eye`/`Seg_Type`）为何默认隐藏；新增段永远默认不显示，且注释说"段号见 status_bar.h"但本头不 include 它 | 加一行说明"默认只开前 7 段（眼睛/类型默认关）"，或改成 `{Seg_Fld..Seg_Disk}` 显式列名并 include `status_bar.h` |
| L2 | `src/config_io.cpp:245` | `sessionActive` 未夹取（可被手改成 999）。下游 `app.cpp:469-470`、`app.cpp:478-480`、`app_commands_misc.cpp:334` 都有边界检查，故无越界风险 | 就地 `ClampInt(..., 0, max(0, size-1))` 收口不变式（此处是"读盘一律夹取"的书面纪律，缺一条就是例外） |
| L3 | `src/config_io.cpp:310` | `version: 1` 只写不读，无迁移分支 | 要么删掉，要么在 `Load` 里读并留迁移挂点（否则将来改格式时没有锚点，只能靠"试探字段是否存在"） |
| L4 | `src/settings_reg_valid.cpp:66`（`using Setter` 与头文件重复）、`:161-171`（`StrArraySet` 全仓无调用点 —— 表里没有可写的 `StrArray` 项） | 重复别名合法但冗余；`StrArraySet` 是死代码，会让人以为"数组项可写" | 删 `.cpp` 里的重复 `using`；`StrArraySet` 或删除、或给 `navPaneRoots` 开写入口（表里已注明"改完重启生效"，属于可做但需配套） |
| L5 | `src/settings_dialog.h:42-41` | `isOpen()` 是全仓唯一 camelCase 成员函数（§4 要求函数 PascalCase），且 `isOpen()` 与 `Close()` 全仓无调用点（App 靠 owned window 生命周期收口） | 改名 `IsOpen()` 或直接删除两个未用接口；若删 `Close()`，在类注释里写明"关闭靠 owner 生命周期" |
| L6 | `src/settings_dialog_draw.cpp:8` | 注释"几何来自布局，行/列的矩形在 Layout 里算好（settings_dialog.cpp）" —— `Layout()` 实际在 `settings_dialog_model.cpp` | 改指对文件（或按"文档不写行号"只写符号名 `SettingsDialog::Layout()`） |
| L7 | `src/settings_reg_table.cpp:104-105`、`src/settings_dialog.h:5` | 两处"分组清单"都缺 `外观 / 历史上限 / 搜索`（表里实际 12 组，注释列 9 组） | 两处同步为 12 组，或直接删掉枚举、改成"见 GroupTable()" |
| L8 | `src/settings_reg_valid.h:1-2`、`src/settings_reg_valid.cpp:3` | "键表本体在 settings_reg.cpp" —— 实际在 `settings_reg_table.cpp`（`settings_reg.cpp` 自己的文件头是对的） | 改指 `settings_reg_table.cpp` |
| L9 | `src/settings_dialog_draw_help.cpp:120-122` | 状态栏活体读数用未缩放的 `kRowHeights[density_]`；实际行高是 `theme_->S(...)`（`model.cpp:226`）。150% DPI 下显示"行高 32"而真实 48，同时密度菜单标签（`actions.cpp:264`）也是逻辑值 | 显示 `theme_->S(kRowHeights[density_])`，或在文案里标"逻辑像素" |
| L10 | `src/config_io.cpp:264-266` | `namedSessions[].paths` 保留空串（`if (p.IsString())`），而同一文件其它路径数组用 `ReadStringArray` 丢空串。空路径会被 `session.named.restore` 拿去开标签 | 与 `sessionPaths` 同口径（丢空串），或明确写下"空串表示占位"的理由（若无理由就是口径漏了） |
| L11 | `src/config.cpp:4`（`search_scope.h` 未被使用）、`:130-132`（`MousePath()` 后三个连续空行）、`:112`（`DataDir()` 与 `Dir()` 完全等价） | 死 include / 排版 / 冗余 API | 一并清理；`DataDir()` 若无外部调用点则删（本组内无调用） |
| L12 | `src/settings_reg_table.h:5-6` | "合成一个文件会到 400 行上下" —— 本文件对应的表本体已 469 行 | 去掉数量断言，只留"两者改动节奏不同"的理由 |
| L13 | `src/ai_server.cpp:50`（跨组提示，G6 域，但根因在本组的配置读写模型） | `UiTimeoutMs()` 在**管道监听线程**读 `Config::Get().s().aiTimeoutMs`（普通 `int`），UI 线程可能正在 `config.set` 写同一字段 → 数据竞争（x86 上实际表现为读到旧值，形式上是 UB）。`aiTimeoutMs` 是本组唯一被非 UI 线程读取的字段 | `Settings::aiTimeoutMs` 改 `std::atomic<int>`（其余字段仍是 UI 线程独占，不必一起改）；或在 `Config` 上加 `std::atomic<int> aiTimeoutSnapshot_`，`Set` 时同步发布 |
| L14 | `src/settings_dialog_actions.cpp:285-287` | "帮助"按钮只往状态栏写一行用法，且这行与右栏"何时生效"段落职责重叠；按文档"右栏回答三个问题"，帮助按钮没有独立价值 | 或删按钮，或改成真正的帮助内容（搜索语法、键盘导航、`config.set` 用法），至少与右栏文案去重 |

---

## 五、逐项核对（审查维度里点名要做实、结论是"做对了"的部分）

| 维度 | 核对结果 |
|---|---|
| 解析失败禁止 Save 覆盖 | **做对**：`config_io.cpp:142-148`（读不出）与 `:151-161`（解析失败/非对象）都置 `parseFailed_`；`Save`（`:305`）早退；`SetProfileDir`（`config.cpp:103-110`）会重置该标志，不会跨 profile 串。缺的只是 S3 的"命令层别报成功" |
| 越界值夹取 | **做对且覆盖完整**：`seqTimeoutMs/holdDelayMs/tabMin/MaxWidth/viewMode/sortMode/groupBy/navPaneWidth/aiPanelWidth/paneSplit/recentLimit/searchLimit/closedTabLimit/aiTimeoutMs/window.*/searchRange` 全部夹取；`ReadTabMeta` 颜色越界回 0（`config_io.cpp:98`）；`ReadTabHistories` 的 `idx` 越界回 0（`:117`）。唯一口径分叉是 M5/M6 |
| `sessionPane2Paths` 空串占位 | **做对**：读用 `ReadStringArrayKeepEmpty`（`:49-56`，`Load:253`），写用 `MakeStringArray`（`Save:362`，不跳空串）；`ReadStringArray` 与它并存并在注释里写明为什么需要两种（`:42-48`）。这是本组质量最高的一处 |
| 未知键保留 | **没做**，见 S2 |
| 注册表模型 / 校验 | 55 项、12 组、无重复键、每项都有合法 group（`SelfCheck` 覆盖）；`minValue/maxValue` 与 `IntSet` 的实际区间逐条对得上（`holdDelayMs` 除外，M5）；`Doc` 全为「名称：说明」格式，`SplitDoc` 的括号例外规则与表里 `（只读：…）` 写法一致 |
| 跨层 include：`settings_reg_table.cpp` 取 `Seg_Count` | **做对**：`:22 #include "status_bar.h"`，`:406 IntArraySet(..., 0, Seg_Count)`；`IntArraySet` 的上界是**排他**的（`valid.cpp:182`），故等价 `0..8`，与 `Seg_Count==9` 恰好自洽，还顺带把错误信息打印成 `[0, 8]` |
| 热更新线程安全 | UI 侧全部走 `CommandBus`（单线程）；唯一跨线程读点是 L13。`aiEnabled` 的重启/停服走 `PostMessage(WM_APP_SYNC_AI)`（`app_commands_misc.cpp:213-216`）而不是同步 `StopServer`，这个取舍是对的 |
| 空指针 | 本组所有对外入口都有判空（`GetTabMeta`/`GetFrom`/`Set` 的 `out/err`、`SplitDoc` 的 `name/desc`、`SplitDoc` 的括号 `npos` 判定、`RowByKey` 返回值、`theme_`/`hwnd_`）；未发现可触发路径 |
| 字符串/编码 | 全部 `L""` 宽字面量；`LogDbg` 用 `%s` + `wchar_t*`（`config_io.cpp:158`、`:394`）符合工程口径；`tf::WriteText` 已是原子写（tmp+FlushFileBuffers+MoveFileEx），没有"写一半的 settings.json"风险 |
| 硬编码色 / 品牌串 | 零 `RGB(`、零 `L"ExBar…"`；尺寸全部过 `theme_->S()`（`settings_dialog_model.cpp` 的布局常量有 `S()` 包裹，`PaintStatus` 的 260 也是 `S(260)`） |
| 死代码 | S1（config.cpp 整段）、L4（`StrArraySet`、重复 `using`）、L5（`Close`/`isOpen`）、`Config::DataDir()` |

---

## 六、总体评价

**质量水平：中上。** 这一组是整个工程里"文档即设计"风格贯彻得最彻底的模块之一 ——
`config_apply.h` 用影响面分类替代命令名枚举、`settings_reg` 把"有哪些项/能不能读写/能不能校验"三分、
`ConfigSet` 用判据表替代 `else if` 链、设置面板坚持"所有写入都走 `config.set`"，
这几个决定都是对的，而且注释把"为什么"写到了可以直接据此做 code review 的程度（`sessionPane2Paths`
的保位读、`RebuildModel → Layout` 的不变式、`Item::rc` 曾经漏赋值的那次事故）。核心风险点
（解析失败不覆盖、越界夹取、空串占位、写后回读、默认值与表同源）**都真的落地了**，不是纸面设计。

**但有四个结构性问题值得在下一轮一起清掉：**

1. **"声明"与"落地"之间仍有一层没有机械检查**。`config_apply_test` 的"全覆盖"只验证
   `NeedsAnyApply || IsPureData`，不验证 `ConfigSet` 真的有分支（M3 就是被这层掩护的）；
   建议让 `ConfigSet` 的每条落地都从判据表派生（或在测试里用桩 App 断言"调用了哪个落地入口"），
   否则这张表迟早退化成一份注释。
2. **保存侧的两个盲区**：未知键被抹（S2）、被护栏拦下时仍报成功（S3）。前者是"数据会丢"，
   后者是"说了假话"，都发生在用户看不见的地方。修起来都不大（一个 `rawRoot_` + 一个 `if`）。
3. **绘制与命中的边界**：几何共用做对了，但 `right_.empty` 被写进绘制里（S4）、
   并且全窗口没有裁剪（S6）—— 这两条合起来会让"画出来的东西"和"点得到的东西"在窗口尺寸/滚动/时序
   变化时错位，而这正是本组注释反复强调要避免的那一类 bug。
4. **同一语义的多份实现仍在蔓延**：`config.cpp` 里那 7 个函数（S1）、`holdDelayMs` 的两套区间（M5）、
   `statusSegs` 的两套归一化（M4）、`StrEnumSet` 的"校验规范但存原串"（S5）。工程纪律写的是
   "共用机制只有一份"，本组在**设计**上做到了，在**实现细节**上还有 4 处漏水，且每一处都是
   "改一处不生效"的典型形态。

**优先顺序建议**：S3 → S2 → S1 → S4 → S6 → S5/M2/M4/M5（口径统一类可一批做完，配 4 条单测）。
S3/S2 涉及用户数据与"命令是否可信"，其余是可见性与一致性问题。
