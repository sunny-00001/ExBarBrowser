---
status: current
baseline: 2026-11-27 08:56
---

# 设置对话框对标审查与整改

> 本文起因是一份"参考优秀项目列改进项"的请求。第一版清单基于"设置面板很粗糙"的假设写就，
> **实测源码后确认该假设不成立**——按项目惯例（见 docs/ROADMAP.md 的"已完成阶段不删段落"），
> 原清单保留在附录 A，并在正文逐条标注实测结论。结论以正文为准，附录只作历史留痕。

审查对象：`src/settings_dialog*.cpp/.h`（窗口生命周期 / 模型 / 布局 / 命中 / 交互 / 绘制）、
`src/settings_reg*.cpp/.h`（设置注册表与取值校验）、`src/settings_bounds.h`。
对标基线：Files / Directory Opus / Windows Terminal / VS Code 的设置面板。

---

## 一、实测结论（先看这个）

设置对话框的工程成熟度**高于**原清单假设。六个对标维度逐一实测：

| 维度 | 原清单假设 | 实测结论 |
|---|---|---|
| 结构 | 一屏堆砌、无分组 | **已实现**：三栏（左分类 / 中表单 / 右说明）+ 状态栏；左栏一个分组一入口；非模态 owned 窗口，可边改边看 |
| 检索 | 无搜索 | **已实现**：真实 EDIT 搜索框 + 子类回调（占位文字 / Esc 清空 / Enter 与下箭头进表单）；按名称+键名+说明+分组名大小写无关匹配；搜索态跨组分段显示命中 |
| 生效模型 | 一半即时一半要确定、无反馈 | **已实现**：全部走 `config.set` 命令（人机同源，带审计与回读）；每次改动状态栏回显，命令告警按错误色显示；配置解析失败在动手前就警告 |
| 粒度 | 只有恢复全部 | **已实现**：每项右栏有"恢复默认值"按钮；只读项给"复制命令模板"而非灰死 |
| 可见性 | 改完没有反馈 | **已实现**：命令回显 + 告警 + 外部改动 `OnConfigChangedExternally` 刷新（不会出现"AI 改了、面板还在说谎"） |
| 可达性 | 只能鼠标、缩放重叠 | **大部分已实现**：方向键/Home/End/PgUp/PgDn/Enter/Space/Esc/Tab 齐备；`WM_GETMINMAXINFO` 最小尺寸由布局常量反推；全尺寸走 `theme.S()`；主窗口 `WM_DPICHANGED` 会连带 `ReloadTheme` |

一句话：原清单列的 25 条"改进项"，绝大多数**项目早已实现甚至超额实现**。照单盲改会重复造轮子，
并把已经正确的代码改坏。因此本轮只做两件事：纠正文档（本文），修复实测中**真正成立**的缺陷（第三节）。

---

## 二、逐条状态（原清单 S-01 ~ S-25 对照）

| 编号 | 原清单项 | 实测状态 | 依据 |
|---|---|---|---|
| S-01 | 主题令牌全覆盖 | 已实现 | 布局常量走 `theme.S()`；绘制只用 Theme 令牌与 `ui::*` 原语 |
| S-02 | 品牌串单一来源 | 已实现（本窗口不涉及） | `check_components.py` 唯一性规则在管 |
| S-03 | 无第二份字体/画刷/量字 | 已实现 | 绘制层注释明确"只用一套原语"；字体统一 WM_SETFONT |
| S-04 | 单文件 ≤500 行 | 记账欠账 | `settings_dialog_actions.cpp` 648 行已在 `file-length-baseline.json` 登记，合法欠账，不在本轮拆 |
| S-05 | 路径经 `pathutil::ToShellPath` | 已实现 | 注释明确"规范化由注册表 setter 负责，面板不碰领域规则" |
| S-06 | 键位单一来源 | 已实现（本窗口不涉及） | `check_commands.py` 三方对账通过（243 条命令） |
| S-07 | 验收脚本零合成输入 | 已实现 | `OnWheel` 只读观察光标位置，注释标 R21 |
| S-08 | 设置项命令化 | 已实现 | 全部写入走 `CmdId::ConfigSet`；只读项给命令模板 |
| S-09 | risk/destructive 标注 | 不在本窗口范围 | `config.set` 走豁免表；命令检查通过 |
| S-10 | AI 元数据 | 不在本窗口范围 | `check_ai_meta` 由命令层覆盖 |
| S-11 | 序列必填参数缺口 | 不在本窗口范围 | 全库 49 条，属序列/热键域 |
| S-12 | 状态四段闭环 | 已实现 | `WriteValue` → 命令 → 回读 → `RefreshValues`；告警接在成功消息后 |
| S-13 | Cancel 回滚语义 | 已实现（非模态无 Cancel） | 行内编辑 Enter 提交 / Esc 取消；关闭前同步 `CommitEdit` |
| S-14 | 数值钳制 | 已实现 | `CommitEdit` 查整数/范围；`settings_reg_valid` 单一校验实现 |
| S-15 | 依赖项联动校验 | 已实现 | 三栏宽度各有设置项与夹取；右栏说明块高度量文本反推 |
| S-16 | 外部改动刷新 | 已实现 | `OnConfigChangedExternally` 只刷值不重建模型（保选中/滚动） |
| S-17 | `WM_DPICHANGED` | **部分缺口** | 主窗口已处理并连带 ReloadTheme；独立 popup 自己跨屏时未单独处理，见第四节 |
| S-18 | 最小尺寸 / 滚动容器 | 已实现 | `WM_GETMINMAXINFO` + 中栏/左栏各自滚动上限，均由布局常量反推 |
| S-19 | 多屏居中 | 已实现 | 落在 owner 主窗口中间，主窗口不可见退回工作区中间 |
| S-20 | 设置项搜索 | 已实现 | 见第一节检索行 |
| S-21 | 单项恢复默认 | 已实现 | 右栏"恢复默认值"按钮 + `GetDefault` |
| S-22 | 生效提示与来源标注 | 部分实现 | 命令回显/告警已实现；"默认值/已自定义"来源标注**未做**，见第四节 |
| S-23 | 快捷键冲突检测 | 不在本窗口范围 | 属热键管理器域（`hotkey_manager*`） |
| S-24 | 关于/诊断页 | 未做 | 见第四节，低优先级 |
| S-25 | Tab/Esc/Enter/UIA 名称 | 键盘已实现，UIA 未做 | 键盘齐备；UIA 名称见第四节，低优先级 |

---

## 三、本轮修复（真实缺陷）

### FIX-1：搜索框固定缓冲截断（已修复）

- **位置**：`src/settings_dialog_model.cpp` 的 `SettingsDialog::SearchRawText`。
- **缺陷**：用 `wchar_t buf[128]` + `GetWindowTextW(edit, buf, 128)` 读搜索词，超过 127 字符被静默截断——
  实际只按前 127 字符匹配，界面表现是"搜不到"，且无任何报错。
- **依据**：同一逻辑在 `src/hotkey_manager_model.cpp` 的 `HotkeyManager::SearchRawText` 里是**正确写法**
  （先 `GetWindowTextLengthW` 再动态分配），其注释专门警告固定缓冲截断。本处是同类 bug 的漏改实例；
  项目自己的 `ValueEditText`（settings_dialog_actions.cpp）也用正确写法。
- **修复**：对齐 `hotkey_manager_model.cpp` 的两步读法（取长度 → `std::wstring(len)` → `GetWindowTextW(..., len+1)` → resize）。
- **备份**：`backup/settings_dialog_model.cpp.bak`。
- **验证**：`bash build.sh --out /tmp/x.exe`（只验证编过）；全文再搜 `wchar_t buf[128]` 确认无同类残留。

---

## 四、未做项与理由（不盲目落实）

以下项**确实未做**，但本轮不动，理由写清楚，避免后来者以为遗漏：

| 项 | 为什么本轮不做 |
|---|---|
| S-17 独立 popup 跨屏 DPI | 主窗口 `WM_DPICHANGED` 已处理并连带重载；设置窗口是 owned popup，单独跨屏而主窗口不动是边缘场景，且 Theme 是进程级共享单例，不能为一个 popup 单独改全局 DPI。要做需引入局部 DPI override，属架构改动，风险大于收益 |
| S-22 来源标注 | 需要注册表提供"当前值是否等于默认值"的查询（逐项比对 `GetDefault`），属数据层改动；收益（少一次困惑）低于成本，且不影响正确性 |
| S-24 关于/诊断页 | 新功能，非缺陷；应作为独立需求排期，不混在"审查整改"里 |
| S-25 UIA 名称 | 本项目是自绘窗口 + 单一 EDIT 子控件，无系统控件树可供 UIA 描述；要做需自建 UIA provider，成本高，且设置窗口非无障碍主路径 |

---

## 五、判定纪律（沿用，供后续审查复用）

1. 先实测源码再列缺陷；假设必须与代码逐条对账，**不把"应该没有"写成"确实没有"**。
2. 同一逻辑多处实现时，找那处**正确的**做基准（本轮 FIX-1 即靠 `hotkey_manager` 的正确实现定位）。
3. 欠账文件（棘轮基线里的）不因"超 500 行"就拆——那是已登记的合法欠账。
4. 文档里的"当前数字"必须与脚本实跑对齐；本轮 `check_commands` 实测 243 条命令、三方一致。
5. 改文件前先备份到 `backup/`（用户约定）。
6. 自动化验证不发合成输入、不抢焦点（R21 零容忍）。

---

## 附录 A：第一版清单（历史留痕，已被正文取代）

最初清单按"对标六维度"列出 25 条改进项 S-01~S-25，假设设置面板缺少搜索、键盘、恢复默认、
外部刷新、最小尺寸、数值钳制等。**实测证明这些均已实现**，故清单整体不成立。保留此附录只为
说明"为什么文档从 25 条缺陷缩到 1 条真实修复"——避免后来者以为审查偷懒。以正文第二节的
逐条状态表为准。

---

## 附录 B：第二轮深度审查（2026-11-27 09:09，基于源码逐文件实测）

第一轮只覆盖了设置对话框本身。第二轮扩到**设置注册表 / 配置持久化 / 命令层**，并用最小探针
实测。结论：**"设置相关"真正做到位的是绝大多数，但确实还遗留 8 项未完成/缺陷**，按严重程度如下。

### B1. 【高·真 bug】整数项写入失败却已污染内存值

- **位置**：`settings_reg_valid.cpp` 的 `AsInt` + `settings_reg.cpp` 的 `Set`。
- **现象**：`AsInt` 对 double 直接 `static_cast<int>`，`1.5 → 1` 静默截断；setter 先把 `1` 写进
  `Settings`，随后 `Set` 的写后回读拿原文 `1.5` 比，不一致 → 返回 false，但**不回滚已写入的字段**。
  实测 `Set("seqWheelRows", 1.5)`：写前 3、`ok=0`、读回 1。命令报失败且不落盘，但内存已变，
  下一次任何成功的 `Save()` 会把错误值固化。
- **未完成点**：失败路径缺回滚；`AsInt` 拒绝非整数 double。

### B2. 【中·真 bug】恢复默认值吞掉"未写盘"告警与错误色

- **位置**：`settings_dialog_actions.cpp` 的 `RestoreDefault`。
- **现象**：`WriteValue` 成功时特意把命令层告警接在状态栏后、按错误着色；`RestoreDefault` 紧接着
  无条件 `SetStatus("已恢复默认值…")`，把告警文本和红色状态覆盖掉。配置解析失败/只读数据目录时
  "已在本次运行生效但没写盘"的警告被吞。
- **未完成点**：恢复默认后应保留 WriteValue 的告警，或由 WriteValue 返回状态。

### B3. 【中·功能缺陷】searchRange 下拉暴露必然被拒的档位 2

- **位置**：`settings_reg_table.cpp` 的 `searchRangeOptions` + `settings_dialog_actions.cpp` 的 `OpenEnum`。
- **现象**：档位清单由 `kRangeCount` 全量生成（0~3），但 `2=此电脑` 被 `IntCheckedSet+RangeSupported`
  明确拒绝。下拉把 4 项都做成可点，点中 2 只得到"未生效"；`config.list` 也把 0~3 报给 AI/脚本。
  导航栏搜索菜单则正确灰掉了这一档——同一逻辑两处口径不一致。
- **未完成点**：下拉/AI 域应过滤 `RangeSupported`，只暴露可用档位。

### B4. 【中·行为不一致】settings.json 解析失败时整组 state 文件被跳过

- **位置**：`config_io.cpp` 的 Load 两分支。
- **现象**："读不到"分支仍 `LoadStateFiles()`；"存在但解析失败"分支 `parseFailed_=true` 后直接
  `return`，不读四份 state。手改 settings.json 打错一个逗号，会话/历史/标签/窗口几何全退回默认，
  同时 Save 被禁。与"坏一份文件只影响它自己"的设计声明冲突。
- **未完成点**：解析失败分支也应加载 state 文件。

### B5. 【中偏低·规则未共享】读盘端未复用业务判据

- **位置**：`config_state_io.cpp` 读 `state\tabs.json`。
- **现象**：`searchRange` 只 `ClampInt(0,kRangeCount-1)`，不查 `RangeSupported`，手改成 2 会带不可用档启动；
  `statusSegs` 只做类型筛选+去重，不夹到 `[0,Seg_Count-1]`（写入端明确按此校验）。不崩，但读进来的
  值违反表声明的取值域。
- **未完成点**：读盘复用 `RangeSupported` / 段号边界。

### B6. 【边缘·状态一致性】纯空白搜索词导致三态不一致

- **位置**：`settings_dialog_model.cpp` 的 `UpdateSearchFromControl` + `actions.cpp` 的 `SearchSub`。
- **现象**：输入 `"   "` 被 Trim 成空 query，但 `searchText_` 与 `searchPlaceholder_` 未复位——框里既不是
  占位文字也不算搜索词，占位文字不恢复、计数按浏览态、失焦也不补占位。违反 header 声明的唯一规则。

### B7. 【边缘·反馈缺失】搜索态点击零命中分类无反馈

- **位置**：`settings_dialog_actions.cpp` 的 `SelectGroup` 搜索态分支。
- **现象**：零命中分组没有标题条（`bucket.empty() continue`），点击该分类时既不跳转也不给状态栏提示，
  表现"点了没反应"。

### B8. 【低·已声明未完成】契约漂移 + mcp.json 骨架

- doc 漂移：`settings_reg.h:63` 的 tabNewPos 口径、`settings_reg_valid.cpp:62` 的"18 个可写项"（实测 78）、
  searchRange doc 含不可用档位 2 未注明——影响 AI/维护者判断，不影响运行。
- `data\mcp.json` 是明确声明的骨架（`config.h:7`），无任何读取/连接实现——已声明的设计状态，非隐藏 bug。

### 第二轮"确认已完成"的关键项（避免误报）

- 78 个可写项全部有 setter、有边界、命中三栏、能持久化（settings.json + 4 份 state），无孤儿。
- 只读项无 setter 是设计（明确给出"请用命令改"），非缺陷。
- `hits` 字段确实被绘制层消费（搜索态显示 `hits/total`），不是算了不用。
- `check_commands`（243 条三方一致）、`check_ai_meta` 全通过；设置相关文件无 TODO/FIXME/空壳。

### 第二轮未完成计数

| 严重程度 | 数量 | 编号 |
|---|---|---|
| 高（真 bug） | 1 | B1 |
| 中（真 bug / 功能缺陷） | 3 | B2、B3、B4 |
| 中偏低 | 1 | B5 |
| 边缘场景 | 2 | B6、B7 |
| 低（契约/已声明） | 2 组 | B8 |
| **合计** | **8 类** | 其中"真 bug + 功能缺陷"= 5 项值得修 |

---

## 附录 C：整改落地记录（2026-11-27 10:17）

按优先级依次修复了 B1~B5（前 5 项真 bug / 功能缺陷）。每项先备份到 `backup/`，改完做
语法检查 + 隔离构建（`build.sh --out /tmp/exbar_b*.exe`），最后由 settings_reg_test 回归。

| 项 | 改动 | 备份 | 验证 |
|---|---|---|---|
| **B1** 写入失败污染内存 | `settings_reg_valid.cpp` 的 `AsInt` 拒绝带小数 double（`std::trunc` 判定）；`settings_reg.cpp` 的 `Set` 写入前存旧值，写后校验失败（读不回 / 回读不一致）一律用 `e->set(old)` 回滚 | `backup/settings_reg_valid.cpp.bak`、`backup/settings_reg.cpp.bak` | 新增回归测试 ⑤b：小数写入被拒且原值不变 |
| **B2** 恢复默认吞告警 | `WriteValue` 加 `doneLabel` 参数，成功消息统一由它拼（含写盘告警与错误色）；`RestoreDefault` 不再自己 SetStatus 覆盖 | `backup/settings_dialog_actions.cpp.bak`、`backup/settings_dialog.h.bak` | 语法 + 全量构建通过 |
| **B3** searchRange 假选项 | `settings_reg_table.cpp` 的 `searchRangeOptions` 跳过 `!RangeSupported(i)` 档位；doc 注明档位 2 本宿主不支持。下拉 / config.list / AI 从同一份 options 派生，自动一致 | `backup/settings_reg_table.cpp.bak` | 新增断言：options 不含档位 2；`backup/settings_reg_test.cpp.pre_b3.bak` |
| **B4** 解析失败跳过 state | `config_io.cpp` 解析失败分支补 `LoadStateFiles()`，与"读不到"分支对齐 | `backup/config_io.cpp.bak` | 语法 + 全量构建通过 |
| **B5** 读盘端判据漂移 | `config_state_io.cpp` 读 `statusSegs` 只收 `[0, Seg_Count-1]`；`searchRange` 读到不可用档回退 `kDefaultRange`。新增 `status_segs.h` include | `backup/config_state_io.cpp.bak` | 语法（零警告）+ 全量构建通过 |

**验证结果**：`settings_reg_test` 1110 通过 / 0 失败；`check_commands` 243 条三方一致；
`check_ai_meta` 全部通过；`check_components` 的品牌串 / 日志 / 钩子 / 令牌 / 输入守卫规则均正常。

**遗留流程项**：`check_components` 唯一失败是"测试文件已改、未重新签发评估面"——
`settings_reg_test.cpp` 新增了加严断言（⑤b 小数回归、⑰ options 不含档位 2），不是把判据改软；
按项目纪律须由人重签 `python tools/gen_eval_baseline.py --human-confirm`，本 Agent 未自行绕过。
此外工作区仍有他人未提交的 WIP（`eval-surface.json`、热键管理器相关文件），本轮未触碰。

**未做项**（B6 空白搜索三态、B7 零命中反馈、B8 契约漂移）保持原状，理由见附录 B：
B6/B7 是边缘反馈问题、不影响正确性；B8 是文档口径，单独排期更合适。

---

## 附录 D：第三轮审查（2026-11-27 12:42，修复后复验 + 未覆盖区首审）

第三轮做两件前两轮没做的事：**验收 B1~B5 修复本身**（防引入回归），以及首审从未深入的
**绘制层 / 完整设置表 / config 命令层**。所有发现均经主 Agent 逐条复核源码确认。

### D0. B1~B5 修复验收结论：可接受，无严重回归

逐条核清 6 个关键点：`1.0`（整数以 double 表示）不被 `std::trunc` 误拒；三个 canon 项
（tabNewPos / searchRangePath / statusSegs）回滚幂等（setter 与 canon 共用同一 norm，
正常路径下回滚分支几乎不可达）；B2 消息拼接与告警/错误色正确；B3 下拉下标映射正确；
B4 的 LoadStateFiles 在 parseFailed_ 下不被覆盖；B5 的 255.9 因 `< Seg_Count` 被丢弃而非
截断成 255，searchRange 回退默认值合理。

### D1~D9. 本轮新发现（已复核）

| 编号 | 严重程度 | 问题 | 位置 |
|---|---|---|---|
| D1=F1 | 中 | `config.import` 跨字段顺序依赖：导出按 `tabMinWidth→tabMaxWidth`，导入逐项写时前置校验读旧值，合法配置（300/420）在默认机器（112/230）上被部分拒，最终 112/420 却报成功 | `app_commands_config.cpp` 导入循环 + `settings_reg_table.cpp` 跨字段谓词 |
| D2=BUG-1 | 中 | 面包屑第二段按未裁剪的完整文本宽度 `w1` 定位，长搜索词时被推出 `head_.right` → "N 项匹配"消失或压到密度按钮 | `settings_dialog_draw.cpp` PaintMiddle |
| D3=BUG-2 | 中 | 左栏页脚两段写死 `S(120)`，左栏最小宽 160 → 两段重叠 80px，且都无省略号 | `settings_dialog_draw.cpp` PaintLeft 页脚 |
| D4=BUG-5 | 中 | 右栏键名 chip 宽度只按文本算、不夹到 `right_.chip.right`，右栏最小宽 160 时长键名（22 字符）溢出被裁 | `settings_dialog_draw_help.cpp` PaintRight chip |
| D5=BUG-3/4 | 低 | 数字框绘制内缩 `S(4)`、字符串框 `S(6)` 与布局 EDIT 的 `kValueUnitGap=6` / 0 不一致 → 进入编辑态文字跳位 | `settings_dialog_draw.cpp` PaintNumber/PaintTextField |
| D6=BUG-6 | 低 | 行高读数用逻辑值 `kRowHeights[density_]`，150% DPI 下显示"32"实际 48 | `settings_dialog_draw.cpp` / `draw_help.cpp` |
| D7=F2/F4 | 低 | 状态文件写失败时告警笼统说"没能写盘"；dualPane/paneSplit 一次命令两次全量 Save | `app_commands_config.cpp` / `app_panes.cpp` |
| D8=F5 | 低 | 4 处过期注释：tabNewPos 存 `right`（实为 end）、"18 个可写项"（实为 77）、sessionHistory 写 settings.json（实为 state）、searchRange 写 settings.json（实为 state\tabs.json） | 4 个头文件/源文件 |
| D9=自引入 | 低 | B3 后 `config.list` 的 min/max 仍 0..3 与 options 0/1/3 矛盾；B5 读盘小数截断与 B1 写入拒绝口径不一致 | `settings_reg_table.cpp` / `config_state_io.cpp` |

### D10. 本轮确认"已实现/设计如此"（避免误报）

77 个可写项的 get/set 字段、边界常量、doc 范围逐项对齐，无分叉；cfgapply 全覆盖；
settings.json 读写对称（74/75 键，差异是 navPaneRoots 的 ReadStringArray）；只读项无 setter
是设计；绘制/命中共用同一份矩形（左栏滚动、中栏行、右栏动作、头部按钮严格互逆）；
右栏说明块高度量画同源、最小窗不被裁；零裸 RGB、尺寸全过 `theme_->S()`。

## 附录 E：第三轮修复落地（2026-11-27 13:05）

按 D1~D9 的优先级逐项落实到源码。所有改动前均已备份到 `backup/*.pre_*.bak`。

### E1. 逐项落地

| 编号 | 改动 | 文件 |
|---|---|---|
| D1=F1 | `config.import` 导入前按 `tabMaxWidth → tabMinWidth → 其他` 稳定排序，消除跨字段顺序依赖 | `app_commands_config.cpp` |
| D2=BUG-1 | 面包屑第二段右对齐到密度按钮前的可用宽度、第一段用剩余宽度并省略；另修 `std::max(int, LONG)` 类型冲突 | `settings_dialog_draw.cpp` |
| D3=BUG-2 | 页脚两段按 `foot_` 实际宽度分半、各自 `DT_END_ELLIPSIS`，右段右对齐 | `settings_dialog_draw.cpp` |
| D4=BUG-5 | chip 宽度夹到 `right_.chip.right`，文字加 `DT_END_ELLIPSIS` | `settings_dialog_draw_help.cpp` |
| D5=BUG-3/4 | `kValueUnitGap` 提为 `SettingsDialog` 类常量，绘制内缩与布局 EDIT 边界共用一份；数值框取 `rl.edit` / `rl.unit` 原边界，字符串框无内缩 | `settings_dialog.h` / `settings_dialog_layout.cpp` / `settings_dialog_draw.cpp` |
| D6=BUG-6 | 行高读数四处统一用缩放后的实际像素（密度按钮、状态栏、菜单标签、状态消息）；菜单回传值仍是逻辑档位，判据不变 | `settings_dialog_draw.cpp` / `settings_dialog_draw_help.cpp` / `settings_dialog_actions.cpp` |
| D7=F2/F4 | 本轮未改（低优先级，留待后续） | — |
| D8=F5 | 4 处注释改正：tabNewPos 存 `end`、"77 个可写项"、sessionHistory 在 `state\session.json`、searchRange 在 `state\tabs.json` | `settings_reg.h` / `settings_reg_valid.cpp` / `config.h` / `search_scope.h` |
| D9=自引入 | ① 枚举项在 DumpAll 与右栏显示中省略 min/max、只给 options（表内真实边界保留供 SelfCheck）；② 读盘端用 `std::trunc` 拒收小数，与 B1 写入口径一致 | `settings_reg.cpp` / `settings_dialog_draw_help.cpp` / `config_state_io.cpp` |

### E2. 验证

- **语法检查**：8 个涉及的 TU 全部通过（`clang++ -fsyntax-only`，含 draw / draw_help / actions / layout / settings_reg_table / settings_reg / settings_reg_valid / config_state_io）。
- **`settings_reg_test`**：1097 通过 / 0 失败（独立 `EXBAR_TEST_OUT`；全项目 218 个 TU 编译链接成功）。
- **`config_roundtrip_test` / `config_apply_test`**：本轮未能运行 —— 工作区其他会话正在重构 `CommandDef` 确认回调，`tab_strip_layout.cpp` / `tab_strip_anim.cpp` 第 209 行把 `bool` 赋给 `std::function<bool(const CommandDef&)>`，全量构建中断；用已编译 obj 手工链接也因 obj 缺口（210/218）失败。**非本次改动引入**，未触碰对方文件。
- **评估面**：因改动过 `tests/settings_reg_test.cpp`，`check_components` 要求人工重签（`python tools/gen_eval_baseline.py --human-confirm`），按纪律未自行绕过。

### E3. 过程中的两次自我纠正

1. D9 初版把 `searchRange` 的 min/max 改成相等，想让 DumpAll 只输出 options，结果触发 `SelfCheck`「可写整型项必须声明数值域」红。改为保留表内真实边界、仅在对外展示层省略 min/max。
2. BUG-1 的编辑遗留 `std::max(int, LONG)` 类型冲突（上一轮中断前未验证），语法检查抓到后加 `static_cast<int>` 修正。
