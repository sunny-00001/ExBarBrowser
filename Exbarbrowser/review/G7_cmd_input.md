# G7_cmd_input 组审查报告

审查对象：ExBarBrowser `src/` 下命令总线 + 输入栈，清单 `review/G7_cmd_input.txt` 共 **41 个文件**。
方法：41/41 全文逐行读完（无抽样），配合只读脚本做交叉核对（命令表 × 别名表 × keymap.gen.h × scopeUi 分布 × 基线条目）。
边界：未 build、未运行、未改源码；本文件是唯一写入。

行数口径提示：`wc -l` 合计 **6959**（`key_binding.h/.cpp` 无末尾换行，计行总数 6961）。清单里记的 6933 与实测差 26 行，说明清单生成后文件有改动；不影响覆盖（41/41 全读）。

---

## 一、文件覆盖清单

| # | 文件 | 行数 | 结论 |
|---|---|---|---|
| 1 | src/chord.cpp | 280 | 解析/格式化/保留键实现完整；`ShouldYieldToEdit` 死 API（见 M7）；`IsReserved` 承诺"Win 组合一律交系统"在钩子路径不成立（见 H3） |
| 2 | src/chord.h | 58 | 契约清楚；`kWin = 8` 已定义，但生产端（kb_hook）不产出该位 |
| 3 | src/cmd_naming.cpp | 333 | 202 对别名双向查表、大小写无关（`str::IEquals`）正确；`GroupDisplayName` 缺 3 组（M1）；`:25` 提到不存在的 `Derive()`（M2） |
| 4 | src/cmd_naming.h | 41 | 设计意图写得很清楚，正因为清楚才暴露 `:13` 的 Derive 承诺是空头（M2） |
| 5 | src/cmd_req.cpp | 24 | 与头文件一一对应，无问题 |
| 6 | src/cmd_req.h | 63 | `Requirements` 定长 4 + `ReqReason` 齐备，够用 |
| 7 | src/commands.cpp | 311 | `Dispatch` 收口正确（前置 :197 / 确认 :220 / 结果 handler 优先 :231）；`:219` 注释"本文件早已超 500 行"与 311 行不符（L4） |
| 8 | src/commands.h | 315 | `Invoke / InvokeByName / InvokeLegacy / InvokeByNameLegacy` 四层命名不混淆，`fromMachine` / `IsHumanCall` 语义明确 |
| 9 | src/commands_actor.h | 72 | Human/Ai 两类主体 + 9 种 UI 来源 + `FailKind`，无问题 |
| 10 | src/commands_args.cpp | 84 | `ArgsFromLegacyArg` 按 schema 反推单键，`HasRequiredArgs` 守卫到参即拒，正确 |
| 11 | src/commands_args.h | 107 | Risk/Effects/Scope 三件套清晰，`Effects` 默认值安全（非破坏） |
| 12 | src/commands_audit.cpp | 74 | 161 字截断用于展示、全量 JSON 用于 hash，口径正确（`:27` / `:49`） |
| 13 | src/commands_confirm.cpp | 84 | fail-closed 正确（`:68` 未注入回调即拒 = `CanceledByUser`）；10 条白名单逐条核对均为"可撤销/可重放" |
| 14 | src/commands_ids.h | 211 | append-only 纪律文档化；218 条命令全部有 CmdId，无重复值 |
| 15 | src/commands_state.h | 33 | StateProbe 单点注入，`CanExecute` 与总线分离，无问题 |
| 16 | src/commands_table.cpp | 304 | user 段命令行完整；`FindCommand` 三路（`:271-297`）+ CmdId 版（`:299`）实现与注释一致 |
| 17 | src/commands_table.h | 111 | 豁免原因常量 + 共享 Effects 常量化，避免每条命令手写 true/false，好评 |
| 18 | src/commands_table_tools.cpp | 187 | tool 段命名与 user 段同构，无风格漂移 |
| 19 | src/hotkey_manager.cpp | 340 | 改键写回错设置（H2）、作用域下拉漏 19 条命令（L3）、lParam 存元素指针（L2） |
| 20 | src/hotkey_manager.h | 11 | 仅两个导出，边界干净 |
| 21 | src/input_router.cpp | 505 | 状态机顺序正确（overlay → 模式前缀 → seq 收尾/取消 → chord → seq 启动）；恰好 505 = 行数基线，零余量（L4） |
| 22 | src/input_router.h | 174 | `HandleKey` 不给 `keyInfo` 默认值（漏传即编译错）是很对的取舍（`:54-55`） |
| 23 | src/input_router_overlay.cpp | 166 | 弹层按键分流完整；`ShowSequenceHint` 已切 overlay，`hint_` 只剩余 Hide（M5） |
| 24 | src/kb_hook.cpp | 239 | 吞键成对、修饰键自维护的思路正确，但三处硬伤：Ctrl/Alt 早退跳过 up 记账（H1）、不跟踪 Win（H3）、计数跨 Start/Stop 不重置（H1） |
| 25 | src/kb_hook.h | 50 | 契约写得好（含 LowLevelHooksTimeout 的坑说明）；缺 `kWin` 位说明（H3） |
| 26 | src/key_binding.cpp | 152 | `Effective` 先到先得 + 跳过 reserved/未实现/需参，是"哪些键真会触发"的单一口径，正确 |
| 27 | src/key_binding.h | 113 | `Binding{chord,spec,setting,scope}` 字段够用；`spec` 可带内联参数的注释很关键 |
| 28 | src/keymap.cpp | 117 | `LoadKeymap` 解析失败回退 `DefaultKeymap()`，`SystemKeyFor` 表驱动，正确 |
| 29 | src/keymap.gen.h | 301 | 105 热键 + 169 序列，全部可解析、无重复 chord/seq/setting；生成物有 `--check` 守卫 |
| 30 | src/keymap.h | 37 | 结构体三件套，无问题 |
| 31 | src/mouse_rules.cpp | 254 | 默认表覆盖 13 个目标；`MouseButtonName` 死代码（L5） |
| 32 | src/mouse_rules.h | 107 | `MT_UNKNOWN = -1` + `MT_COUNT` 哨兵显式，正确 |
| 33 | src/sequence.cpp | 138 | `Feed` 三分支（无匹配/完整匹配/唯一候选）逻辑正确；唯一自动执行被两个开关 AND 住（M4） |
| 34 | src/sequence.h | 50 | 头注释 `:5` 的语义描述与 `:120` 实现口径不一致（M4） |
| 35 | src/sequence_hint.cpp | 121 | 整份死代码（已被 Overlay 取代，仅剩 Create/Hide）（M5） |
| 36 | src/sequence_hint.h | 32 | 同上 |
| 37 | src/sequence_overlay.cpp | 329 | 弹层主体健壮（先关后派发、钉住例外）；`AfterExecute` 死 API（M6） |
| 38 | src/sequence_overlay.h | 208 | `Row` 12 字段承载 5 列/模式/目标三种布局，设计到位；`AfterExecute` 声明死 |
| 39 | src/sequence_overlay_input.cpp | 291 | Esc/翻页/字符/键模式分流清楚，repeat 抑制正确，WM_INPUT 走 DefWindowProc 释放 raw buffer |
| 40 | src/sequence_overlay_layout.h | 221 | 常量表可逐项核对（这点做得好）；`:59` 用 `MulDiv` 却自称"不碰 Win32"（L1） |
| 41 | src/sequence_overlay_paint.cpp | 284 | 三种布局 + 空态 + 光标闪烁 + 滚动条，实现完整，无问题 |

---

## 二、严重（必须修）

### H1 — 钩子在 Ctrl/Alt 早退时跳过 key-up 记账，会把该键永久锁成 "repeat"
- **文件**：`src/kb_hook.cpp:136`（早退）、`:141`（up 记账）、`:154`（repeat 判定）
- **问题**：`g_mods & (kCtrl|kAlt)` 的早退**在所有 up 记账之前**。若某字符键的 key-down 被吞过（`g_downCount[vk] = 1`、`g_swallowUp[vk] = true`），而它的 key-up 落在"Ctrl 或 Alt 正按着"的窗口里，则 up 直接 `CallNextHookEx` 走掉：
  1. key-up 没被吞 → 窗口收到"从未按下过的抬起"（正是 `:18-19` 注释说要避免的不一致）；
  2. `g_downCount[vk]` 永久停在 1。
- **触发**：按住某字母键 → 再按下 Ctrl 或 Alt → 松开字母键（Alt 菜单键、AltGr 变体布局下更容易撞上）。
- **后果（连锁，且不可自愈）**：此后该键每次按下 `repeat == true`（`kb_hook.cpp:154`）→ `app.cpp:594` 把 bit30 写进合成 lParam → `input_router.cpp:254`（序列进行中遇 repeat 直接 `Cancel()`）与 `input_router.cpp:267`（`mods == 0 && !repeat` 才启动序列）双双拒绝 → **该字母的序列键与裸键热键在本次进程内彻底失效**。`Stop()`（`:187` 附近）也不重置 `g_downCount`/`g_swallowUp`/`g_mods`，重启钩子救不回来。
- **修复**：把 up 分支的记账提到 mods 早退之前（up 分支本就不依赖 mods）：
  ```cpp
  if (up) { const bool s = g_swallowUp[vk]; g_swallowUp[vk] = false;
            if (g_downCount[vk] > 0) --g_downCount[vk];
            return s ? 1 : CallNextHookEx(...); }   // 放在 :136 之前
  ```
  并在 `Start()` 里 `ZeroMemory(g_downCount/g_swallowUp) + g_mods = 0`，`Stop()` 里同样清零（避免"钩子重启时按键正按着"导致修饰位粘死）。

### H2 — 热键管理器对"一命令多设置"的命令改错设置项（清除也清不掉）
- **文件**：`src/hotkey_manager.cpp:117`（`captureCommand = row->command`）、`:228`（写回）、`:189`（清除）；`src/input_router.cpp:484-500`（`SetKeyForCommand` 命中第一条即 return）
- **问题**：`BindingRow` 有 `setting` 字段，但捕获/清除只记了 `command`；`SetKeyForCommand` 遍历 `rows_`，**只改第一条匹配命令的行**。而默认键表里 6 条命令各有 2~3 个设置（实测 13 行）：
  `tab.next`(hotkeyTabNext `-` / hotkeyTabNextPage `Ctrl+PageDown`)、`tab.prev`(2)、`tab.move.left`(2)、`tab.move.right`(2)、`win.focus.address`(hotkeyFocusAddress `F4` / …Alt `Alt+D` / …CtrlL `Ctrl+L`)、`win.focus.search`(hotkeyFocusSearch `Ctrl+F` / …Alt `Ctrl+E`)。
- **触发**：管理器里选中"Ctrl+PageDown"这一行 → 按新键。写回落到 `hotkeyTabNext`，`RefreshRows` 后**第一行变了、被选中的那行没变**；用户以为改的是 PageDown 那个键，实际生效的是另一条设置。
- **清除同理**：点"清除"只给第一条设置写 `kHotkeyDisabled`，第二条仍绑着 → 界面显示"（未设置）"但按键照样触发，用户判定为"清不掉"。
- **修复**：`State` 增 `captureSetting`，捕获与清除都按 **setting** 写；`InputRouter` 增 `SetKeyForSetting(const std::wstring& setting, const std::wstring& key)`（现有 `SetKeyForCommand` 保留给脚本/兼容）。若不愿扩接口，至少让 `SetKeyForCommand` 在多命中时返回 `false` 并提示"该命令有多个绑定，请用 …"。

### H3 — 钩子不跟踪 Win 修饰：Win+字母被吞并当作裸键
- **文件**：`src/kb_hook.cpp:129`（`TrackMods` 只认 Ctrl/Shift/Alt）、`:136`（早退条件不含 Win）；`src/kb_hook.h:20-22`（只镜像了 3 个位，缺 `kWin`）
- **问题**：`chord::IsReserved`（`chord.cpp:201`）写明"**Win 开头的组合一律交系统（应用内拿不到）**"，但这条承诺只在"mods 来自 `GetKeyState`"的路径成立。钩子路径下 `g_mods` 永远没有 `kWin` 位（VK_LWIN/VK_RWIN 不是字符键 → 直接 `CallNextHookEx`，也不进 `TrackMods` 的状态机），于是 **Win+字母的字母部分被钩子吞掉**：`:156` 置 `g_swallowUp = true`、`:159` 投递 `Event{vk, mods=0}`、`return 1`。
- **触发**：焦点在本进程窗口（非 EDIT / 非 textRoot）时按 Win+E / Win+R / Win+S / Win+D / Win+L / Win+A / Win+X 等。
- **后果**：系统快捷键被吞（Win+L 锁屏、Win+D 显示桌面这一类直接失效），且该字母以 `mods == 0` 进入输入路由 → `input_router.cpp:267` 启动序列前缀（在文件视图里还会被回投成 type-ahead 字母）。是否实际命中取决于"Start 菜单是否已抢到前台"（`FocusIsOurs` 的 ① 判定）：Win 单独按下通常要等 key-up 才开 Start，字母消息多半先到，所以**多数情况下会命中**。
- **修复**：`kb_hook.h` 补 `constexpr int kWin = 8;`（与 `chord::kWin` 同值），`TrackMods` 增加 `VK_LWIN/VK_RWIN`（**注意：不要**把 Win 塞进现有的 `kCtrl|kAlt` 早退，那会叠加 H1 的 up 记账漏洞；正确顺序是先修 H1，再把 Win 加进早退条件），并在早退条件改成 `g_mods & (kCtrl | kAlt | kWin)`。

---

## 三、中等

### M1 — `GroupDisplayName` 漏 Ai / Config / Dev 三组
- **文件**：`src/cmd_naming.cpp:319-332`（表 12 项），调用点 `src/app_sequence_overlay.cpp:67`、`:154`
- **问题**：命令表实际有 **15 个 group**：`Ai Bookmark Config Dev External File Go Nav Path Search Session Tab Tool View Window`。映射表只覆盖 12 个，缺 `Ai`/`Config`/`Dev`；未命中时 `:331` 原样返回英文 group id。
- **触发**：序列弹层/命令面板里出现 `dev.*`（9 行）、`config.*`（3 行）、`ai.*`（2 行）——"分组"列显示 `Dev`/`Config`/`Ai`，其它行显示中文，同列中英混排。
- **修复**：补三条，建议与 `scopeUi` 用词对齐（`开发` / `设置` / `AI 助手`，见 `settings`/`AI 助手` 已用于 scope）：
  ```cpp
  {L"Dev", L"开发"}, {L"Config", L"设置"}, {L"Ai", L"AI 助手"},
  ```
  更稳的做法：把 group→中文名做成单一数据源（从命令表 group 反查），漏组在启动时自检——而不是靠人记得补表。

### M2 — 注释承诺的 `Derive()` 不存在；28 条命令没有任何旧名别名
- **文件**：`src/cmd_naming.h:13`、`src/cmd_naming.cpp:25`（注释）；`src/cmd_naming.cpp:311`（`CanonicalName` 无推导）；`src/commands_table.cpp:271-297`（`FindCommand` 只有精确/AliasOf/LegacyOf 三路）
- **问题**：注释写"表里没列的名字自动退化成一个可推导的点分名（group 小写 + 原名小写），见 `Derive()`"，但全仓 `grep Derive` **零命中**——`Derive()` 从未实现。`CanonicalName` 对未知旧名原样返回，`FindCommand` 也不推导。
- **后果**：任何按注释推断的外部调用方（脚本 / MCP 客户端 / AI）把 `NavHistoryClear` 推成 `nav.history.clear` 之外的形式，或把 `ConfigGet` 推成 `config.get`，行为全靠猜；同时"新命令不必强制填表"这句承诺在代码里落空 → 实测 **28 条命令没有任何别名**，只能以点分名调用：
  `bmk.add.selection` `bmk.add.tabs` `bmk.copy`；`config.get` `config.list` `config.set`；`dev.archive.decide|list|record` `dev.build` `dev.diff` `dev.job` `dev.rollback` `dev.snapshot` `dev.test`；`nav.history.clear|jump|list` `nav.recent.list`；`search.history.clear|list` `search.query.set` `search.scope.list|set`；`view.dualpane.toggle` `view.pane.next|split|vertical`（共 28 条，占 218 条的 12.8%）。
- **修复**：二选一——(a) 删掉 `Derive()` 那两句注释，并在 `commands_ids.h`/`commands_table.h` 写明"新命令只需点分名，旧名别名仅对历史命令保留（C2 只覆盖 190/218）"；(b) 真实现 `Derive()` 并在 `FindCommand` 里作为第 4 路兜底（`group 小写 + '.' + 原名去掉 Cmd 前缀后的小写`）。倾向 (a)：推导规则和现有 202 对别名必然打架（如 `TabNew` → `tab.new` 靠表，推导会得 `tab.tabnew`），不确定性比收益大。

### M3 — C2「旧名保留」的实际覆盖面被高估
- 见 M2 后半：202 对别名 / 218 条命令。这不是缺陷本身，但**规则文本**（C2）与**实际覆盖**有 28 条缺口，评审会反复提问。建议在 `docs/` 的 C2 条目里加一句"覆盖 202/218，新增命令默认无别名（点分名为唯一名）"，把事实写死。

### M4 — 「唯一候选自动执行」被两个独立开关 AND 住
- **文件**：`src/sequence.cpp:120`（`matches == 1 && uniqueAuto_ && singleInstant_`）；`src/sequence.h:5`（注释）、`:24-25`（两个 setter）；`src/config.h:74-75`、`src/settings_reg_table.cpp:247-253`（两个独立设置项）
- **问题**：`singleInstant_` 在 `Feed` 里**只有这一处**使用，且以 AND 形式充当 `uniqueAuto_` 的隐式主开关。头注释 `:5` 把两者描述成互不相关的语义（"命中且唯一（或 exact 且 seqUniqueAuto）"），设置面板也把两项并列呈现。
- **触发**：用户关掉 `seqSingleInstant`（想"别一按就执行"）→ 顺带把"唯一候选自动执行"一起废掉，没有任何提示。当前两个默认都是 `true`（`config.h:74-75`），所以现网无感，但这是**明天就会咬人的耦合**。
- **修复**：`Feed` 只判 `uniqueAuto_`；若 `singleInstant_` 另有语义（看名字像"单 token 序列立即生效"）就实现它或删除该字段（现在它名不副实）。

### M5 — `sequence_hint.*` 整份是死代码（153 行 + 每实例一个隐形窗口）
- **文件**：`src/sequence_hint.h`(32) / `.cpp`(121)；使用点仅 `src/input_router.cpp:15`（Create）、`:29`/`:464`/`:474`（Hide）
- **问题**：`ShowSequenceHint`（`input_router_overlay.cpp` 尾部）已改为 `overlay_.OpenSeqKeys(prefix)`，注释自认"旧的 SequenceHint 小浮层只有 2 列、无输入区，与原版差得远 —— 不再用"。于是这个窗口从创建到销毁只被 Hide 过，从未 Show。
- **触发**：始终（每次启动都白建一个窗口）。
- **修复**：删 `sequence_hint.h/.cpp`，`InputRouter` 去掉 `hint_` 成员与 3 处 `Hide()`（`input_router.cpp:29/:464/:474`）；若想留作兜底，就在头注释写"保留但当前不走"并加编译期开关，避免下一个评审再问一遍。

### M6 — `SequenceOverlay::AfterExecute()` 死 API，且收尾规则被复制成两份
- **文件**：`src/sequence_overlay.cpp:203-210`（定义）、`src/sequence_overlay.h:117`（声明）；重复实现见 `:277-288`（`Commit` 的 `keepOpen/enterMode` 分支）与 `src/input_router_overlay.cpp:144`（注释 + 自己实现）
- **问题**：`grep AfterExecute` 只有声明、定义、两处注释，**零调用**。真正的收尾逻辑在 `Commit()` 里重写了一遍（`keepOpen = pinned_ || enterMode`），宿主 `SeqKeyFeed` 又按 `overlay_.pinned()` 实现第三份。
- **触发**：无（死代码）；风险是将来改收尾规则时改一处漏两处。
- **修复**：删 `AfterExecute`（推荐，逻辑已在 `Commit`），或让 `Commit` 调它，把规则收回一处。（注：`:282` 的 `@mode` 分支**不是**死代码——`app_sequence_overlay.cpp:83` 会构造 `command == L"@mode"` 的行，模式列表回车走这里。）

### M7 — `chord::ShouldYieldToEdit` 死 API，而同一判断散在三处
- **文件**：`src/chord.cpp:205`（定义）、`src/chord.h:38`（声明）；零调用
- **问题**：它本应是"裸字母该让给编辑框 / 该让给输入法"的单一判据，但实际判断分散在：`kb_hook.cpp:117`（EDIT 类）、`kb_hook.cpp:118-120`（textRoot 子树）、`input_router.cpp:189-193`（`inEdit` / `GlobalWhileEditing`）、`overlay HandleKey` 的字符分流。
- **后果**：现在是"能跑但规则不同步"——钩子放行 EDIT 的判据（类名/子树）与路由放行的判据（类名/子树/全局组合白名单）是两套，一处改了另一处不会跟。
- **修复**：要么删（承认判据不统一、各层自治），要么把它升级成"层间唯一的裸键让行契约"并在两侧调用。建议前者（成本低、不留假抽象）。

### M8 — 全局 `WH_KEYBOARD_LL` 与项目明文规则冲突，且没有 lint 兜底
- **文件**：`src/kb_hook.cpp:169`（`SetWindowsHookExW(WH_KEYBOARD_LL, …)`）；规则处 `AGENTS.md`、`CONTRIBUTING.md:176`（"不注入 explorer、不装全局钩子"）、`docs/ARCHITECTURE-RULES.md`（同一句话）；检查器 `tools/check_components.py`（INPUT_GUARD 只查 R21 合成输入，**没有** `SetWindowsHookEx` 规则）
- **问题**：实现与文档正面对撞，而唯一的自动化守卫（`INPUT_GUARD`）不管这件事。也就是说这条规则现在**只靠人工评审守**，而它已经被违反了——任何一个新需求都可以照抄 `kb_hook` 的先例再装第二个钩子。
- **触发**：无（治理问题，非运行缺陷）。
- **修复**：把 `kb_hook` 明确写进规则作为**唯一豁免**并给出理由（"中文输入法把字母吃成 VK_PROCESSKEY，必须取真实 vk"——这个理由本身是站得住的），同时给 `check_components.py` 加一条：`SetWindowsHookEx` 只允许 `src/kb_hook.cpp` 出现，其它文件报错。文档与 lint 一起改，规则才不再自相矛盾。

---

## 四、轻微 / 观察

### L1 — `sequence_overlay_layout.h` 自称"不碰 Win32"却用 `MulDiv`，且头文件不自包含
- **文件**：`src/sequence_overlay_layout.h:57-59`（`// 按 DPI 缩放（纯算术；不碰 Win32…）` 紧跟 `MulDiv(v, dpi, 96)`）；该头文件**没有任何 `#include`**，只靠 `src/sequence_overlay.h:15` 先包含 `<windows.h>` 才能编译。
- **触发**：任何新 TU 先包含本头（或调整 `sequence_overlay.h` 的 include 顺序）→ 编译报 `MulDiv` 未定义。
- **修复**：保留 `MulDiv`（它是四舍五入，与截断不同，别随手改成 `v*dpi/96`）→ 显式 `#include <windows.h>` 并把注释改成"仅依赖 MulDiv 取整"；若真想要"纯算术"，就写 `(v * dpi + 48) / 96` 并说明取整口径。

### L2 — 热键管理器用 `LPARAM` 存 `st->rows` 元素指针（潜在悬垂）
- **文件**：`src/hotkey_manager.cpp:69`（`it.lParam = reinterpret_cast<LPARAM>(&r)`，`r` 是 `const auto& r : st->rows` 的引用）、`:97`（`SelectedRow` 转回 `BindingRow*`）
- **问题**：正确性依赖一个隐含约定——`RefreshRows`（`st->rows = router->Rows()` 后立刻 `FillList`）中间不能有消息泵，否则列表里躺着指向已释放 vector 的指针，点击即读野内存。
- **触发**：将来有人在 `rows=` 与 `FillList` 之间加一次 `SendMessage`/日志/`UpdateWindow`。
- **修复**：`lParam` 存**下标**（`LPARAM row`），`SelectedRow` 用 `st->rows[idx]` 反查；改造成本几乎为零，悬垂风险归零。

### L3 — 作用域下拉漏 5 类，19 条命令筛不出来
- **文件**：`src/hotkey_manager.cpp:311-312`（下拉 9 项：全部/标签栏/导航栏/书签栏/文件视图/文件操作/系统对话框/窗口/全局）；筛选按 `r.scope` 精确字符串匹配（`:56-62`）
- **问题**：实际 `scopeUi` 分布（218 行全量统计）为 文件视图 47 / 标签栏 44 / 导航栏 38 / 文件操作 26 / 书签栏 21 / 窗口 17 / **开发 9 / 会话 4 / 设置 3** / 系统对话框 3 / 全局 3 / **AI 助手 2 / 剪贴板 1**。后 5 类（合计 19 条）在下拉里没有对应项 → 只能在"全部"里翻。
- **修复**：下拉项改为**从 `rows` 里收集实际 scope 去重排序生成**（顺带自动消除"死选项"），不要在 UI 里硬编码一份与数据源脱钩的清单。

### L4 — `input_router.cpp` 恰好卡在行数基线上；且 `commands.cpp:219` 的注释已过期
- **文件**：`data/file-length-baseline.json` → `"input_router.cpp": 505`，实测正好 505 行（零余量）；`src/commands.cpp:219` 写"本文件早已超 500 行"，而该文件只有 311 行。
- **影响**：前者意味着下一次往 `input_router.cpp` 加任何输入逻辑都会让 CI 失败（设计上是对的棘轮，但需要知道：新逻辑必须像 `input_router_overlay.cpp` 那样开新 TU）；后者是纯粹的注释漂移，会误导下一个人"这里不能再加代码"。
- **修复**：`commands.cpp:219` 改成事实（"确认逻辑单独成文件，避免总线继续变胖"）；`input_router.cpp` 无需动，只需在头注释提醒"本文件已达基线，新逻辑请开 TU"。

### L5 — `MouseButtonName` 零调用
- **文件**：`src/mouse_rules.cpp:57`、`src/mouse_rules.h:64`
- **说明**：同组的 `MouseTargetName`/`MouseTriggerName` 都在日志与配置里用到，只有它没有调用点（配置里按钮名走的是别的映射）。
- **修复**：删，或接进 `input_router.cpp` 的 `[mouse]` 日志（现在日志只打 target 不打 button，接上还能顺手补齐可观测性）。

### L6 — `g_textRoot` / `g_mainWnd` 是 UI 线程写、钩子线程读，无同步
- **文件**：`src/kb_hook.cpp:31-32`（`g_mainWnd`/`g_textRoot` 裸变量）、`:118-120`（钩子线程读）、`SetTextEntryRoot`（UI 线程写）
- **说明**：`g_mods` 的注释"仅钩子线程读写，无需加锁"只对 `g_mods` 成立，对这两个 HWND 不成立。实践上 HWND 写入是原子的、最坏情况是读到上一帧的值（`IsChild` 对已销毁窗口返回 false，无害），所以不是 bug；但**约束没写下来**，将来把这个文件改成多实例/热重载就会踩。
- **修复**：改 `std::atomic<HWND>`（或加一行注释明确"单写多读、允许短暂陈旧"）。

### L7 — `app.cpp:599` 回投计数 `reInjected` 可能泄漏一次
- **文件**：`src/app.cpp:598-600`（`reInjected[vk]++` 后 `PostMessage(focus, WM_KEYDOWN, …)`）、`:607-613`（消费处）
- **说明**：若目标窗口在消息被派发前销毁 / 消息被过滤，计数不清零 → 该 vk 的**下一次真实** `WM_KEYDOWN` 会在 `:611` 被当作"我们自己塞回去的"直接放行给 `DefWindowProc`，少走一次路由（丢一次热键）。
- **R21 判定**：这条 `PostMessage` 是 `INPUT_GUARD` 白名单里的**唯一**命中（`allow={"src/app.cpp"}`），语义是"把真实用户的按键还给焦点窗口"，不是伪造输入——**不违反 R21 的意图**，注释也把理由写清了。这条只记风险，不建议改。
- **修复（可选）**：给 `reInjected` 条目加时间戳，超过 ~200ms 未消费即丢弃；或改成投递到 `main_` 再用 `SendMessage` 转发。

### L8 — `vk >= kMaxTrackedVk` 是防御性死分支
- **文件**：`src/kb_hook.cpp:139`
- **说明**：`IsCharKey`（`:59-69`）只放行 `0x30-0x5A` 与 OEM `0xBA-0xDF`，全部 `< 256`，所以 `:139` 永远不会命中。留着无害（防将来扩 `IsCharKey`），但会让读者以为存在 `vk > 255` 的字符键路径。
- **修复**：加一句注释说明是给 `IsCharKey` 扩容用的护栏，或直接删。

---

## 五、七个维度的结论

1. **命令契约（强）**：218 条命令**全部**点分小写、无重名、无 `CmdId` 重复；`CmdId` append-only 有文档；`FindCommand` 双向解析（精确 → `AliasOf` → `LegacyOf`，`:271-297`）+ `CmdId` 版（`:299`）实现与注释一致；`Dispatch` 是唯一漏斗（前置 → 确认 → 结果 handler 优先 → 审计），`FailKind → errorCode` 映射单点。**缺口只有 M2/M3**（`Derive()` 不存在、28 条无旧名）。
2. **cmd_naming（良）**：202 对别名双向查表、大小写无关（`str::IEquals`，躲开了 `_wcsicmp` 与 locale 坑）；`CanonicalName` 语义正确。**`GroupDisplayName` 漏 3 组**（M1），且漏的正好是最近新增的 `Dev`/`Config`/`Ai`——典型的"表随功能增长但映射表没跟"。
3. **输入栈（良）**：`HandleKey` 的分流顺序是对的（弹层优先 → 模式前缀（排除 Ctrl/Alt/EDIT/repeat/序列中）→ 序列 Esc/取消 → chord 匹配 → 序列启动），三重守卫（`inEdit`、`repeat`、标签栏作用域）都在点上，`ctrl+Tab` 循环切换的例外也写明了理由。双击/长按（`kHoldTimer` + `GetDoubleClickTime`）、滚轮（50ms 去重、3 行/格）、计时器（`kSingleTimer/kHoldTimer/kSeqTimer`）状态机干净，`MT_UNKNOWN` 首击升级/沿用的处理考虑了 UIA 冷启动失败。**问题集中在"多设置命令改键"（H2）**。
4. **kb_hook（弱，本组风险最高）**：设计意图正确（钩子只做取真实 vk + 吞键 + 投递，零业务逻辑，躲 `LowLevelHooksTimeout`），吞键确实成对（`:146-149` 注释与实现一致）。但三处硬伤：**H1 早退跳过 up 记账**（可把某键永久锁成 repeat）、**H3 不跟踪 Win**（吞掉 Win+字母）、**L6 跨线程 HWND**。另外它与文档"不装全局钩子"正面对撞且无 lint（M8）。
5. **hotkey / chord（良-）**：`chord::Parse/Format/VkFromName/CharOfVk` 实现扎实，`IsReserved` 覆盖 Win 组合与 Alt+F4/Alt+Tab/Ctrl+Esc/Ctrl+Alt+Del；管理器捕获用 `chord::FromKey + Format`，能正确处理 `WM_SYSKEYDOWN`（Alt 组合）与修饰键单独按下。**H2 是唯一的功能性错误**，另有下拉漏项（L3）与指针别名（L2）。
6. **工程约束（达标）**：41 个文件内**无** `PostMessage/SendInput/keybd_event/mouse_event`（唯一相关的是 `hotkey_manager.cpp` 的 `SendMessage` 给 ListView，与 R21 无关）；R21 的唯一白名单命中在 `app.cpp:599`，语义正当、注释完整。品牌串：41 个文件里 `L"ExBar"` / `L"ExBarBrowser"` **零出现**（全部走 `identity.h`），合规。行数：`input_router.cpp` 505 = 基线（零余量，L4），其余新增文件（`sequence_overlay*` 5 个）都做了 TU 拆分，方向正确。命名空间：`chord`/`cmdnaming`/`kbhook`/`keybinding`/`seqov`/`keymap_gen` 边界清楚，无 `using namespace` 污染。
7. **死代码 / 注释与实现不符（偏多，6 处）**：`Derive()`（M2，注释有代码无）、`sequence_hint.*`（M5，整份 153 行）、`SequenceOverlay::AfterExecute`（M6）、`chord::ShouldYieldToEdit`（M7）、`MouseButtonName`（L5）、`commands.cpp:219`"本文件早已超 500 行"（L4，实际 311 行）；另外 `sequence_overlay_layout.h` 的"不碰 Win32"（L1）。这类问题的共同成因是**先把设计写进注释、实现后没回头改注释**——建议在提交前跑一次"注释里的函数名是否真实存在"的粗检查（`grep` 注释中的 `\w+\(\)` 逐个核对）。

---

## 六、整体评价

这是全项目**契约最规范、文档密度最高**的一组：命令表纯数据、`Dispatch` 单一漏斗、`FailKind` 单点映射、`key_binding::Effective` 作为"哪些键真会触发"的唯一口径、`keymap.gen.h` 由脚本生成并有 `--check` 守卫——这几条把"命令 / 键位 / 序列 / 鼠标"四套输入源的冲突面压到了很小，218 条命令 / 105 热键 / 169 序列的规模下没有一条重名、重复 chord 或无法解析的引用，这是实打实的工程质量。

风险全部集中在 **`kb_hook` 这一层**：它为了绕开中文输入法把字母吃成 `VK_PROCESSKEY` 而装了 `WH_KEYBOARD_LL`，这个决策是对的（否则序列键在中文环境下完全不可用），但"自己维护修饰键 + 自己判自动重复 + 自己吞键对"这三件手工活各自留了一个边界漏洞——H1（早退跳过 up 记账，可让某键永久失效）、H3（不跟踪 Win，吞掉系统快捷键）、L6（跨线程无同步）。这三个都属于"平时不显形、用户撞上就投诉'热键突然不灵了'"的类型，且 H1 不可自愈（要重启进程）。**建议第一批就修 H1 + H3 + H2**：H1/H3 各 5~10 行、H2 是一次 `setting` 贯穿的改造，都是低风险高收益。

第二批修 M1 + M8 + L3：都是"表/清单/规则与实际脱钩"，改动小、能立刻消掉同列中英混排与"规则写在文档里但代码违反"的尴尬。

第三批清理死代码（M5/M6/M7/L5）与注释漂移（M2/L1/L4）——不影响运行，但这一组的杀手级优势本来就是**可读性与契约清晰**，让读注释的人踩空（`Derive()` 根本不存在）比留 153 行死代码更伤。整体评级：**B+（工程质量高，钩子层需一次集中加固）**。
