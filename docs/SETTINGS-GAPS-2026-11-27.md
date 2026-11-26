---
status: current
baseline: 2026-11-27 06:45
scope: 已实现功能的设置项覆盖（行为写死 → 可配置）
method: 三方对照 —— Settings 字段 / settings_reg_table 键表 / 使用点的硬编码常量
related:
  - docs/COVERAGE-GAPS-2026-11-26.md
  - docs/FULL-COVERAGE.md
---

# 已实现功能的设置项缺口（2026-11-27）

> **这份文档要回答的不是「设置项够不够多」，而是「已经做出来的行为里，
> 哪些用户改不了、AI 也改不了」。**
>
> 设置项的骨架（字段 → 读写 → 生效 → 登记 → 分组）本身是完备的：
> 每个 `Settings` 字段都有键名，每个键都有 `IsPureData` / `AffectsLayout`
> 分类。真正的缺口在**行为侧** —— 功能实现了，但阈值、开关写死在代码里，
> 想调只能重编译。

---

## 1. 判定口径

一条行为算「缺设置」，要同时满足三条：

| 条件 | 反例（不算缺口） |
|---|---|
| **已实现且生效** | 只有计划、没有代码的功能（例如标签关闭撤销 toast、拖拽修饰键语义） |
| **值是策略而非契约** | 布局下限、DPI 换算基数、协议字段数 —— 改了就坏 |
| **不同人会有不同偏好** | 唯一正确解（例如 `confirmCloseTabs` 必须存在） |

第一条是这次最重要的边界：**给尚未实现的功能加一个「开关」是假的** ——
打开开关什么也不会发生。所以本轮把「toast 提示」「拖拽修饰键」从候选里剔除了。

---

## 2. 本轮补齐的六项

| 设置键 | 原状 | 现在 | 生效点 |
|---|---|---|---|
| `aiPeekEnabled` | AI 终态**无条件**自动弹气泡，长任务会反复盖住文件列表 | 可关；关掉后 chip 仍显示未读数与颜色，降级为「看得见但不打断」 | `app_ai_status.cpp` 终态分支 |
| `auditEnabled` | 审计日志**没有总开关**，只能靠删文件 | 可关；`Write()` 内收口，所有调用方自动生效 | `audit_log.cpp` `Write()` 入口 |
| `popupDragThreshold` | 弹层拖拽启动阈值写死 `6px` | 可调 2..20；太小把单击误判成拖动，太大拖不动 | `popup_list_input.cpp` 起拖判定 |
| `bookmarkWheelStepDp` | 书签栏滚轮步进写死 `48dp`（≈三个字宽） | 可调 16..200；条目宽窄差异大，一格跨多远因人而异 | `bookmark_bar_wheel.cpp` `ScrollBy` |
| `hideTabBarWhenSingle` | 单标签时标签栏仍占 26px 却零信息 | 可开；多开第二个标签立刻出现 | `TabBarVisible()` + `app_layout.cpp` 四处 |
| `splitMarkThick` | 双栏分割条两条强调线写死 `S(2)` | 可调 1..6；高 DPI 太细、低分辨率偏粗 | `split_bar.cpp` `Paint` |

设置项总数 65 → 71。

---

## 3. 两处值得记下来的工程细节

### 3.1 `hideTabBarWhenSingle` 必须进几何指纹

这个判断有**五个**使用点：`Layout` 的栏高、`LayoutKey` 的几何指纹、
`Layout` 里的 `ShowWindow`、`ChromeHeight` 的两处调用、启动期建树的高度。

各处各写一遍条件，只要漏一处，就会出现两种「看不见内容的空洞」：

- 栏藏起来了，但高度还留着 → 中间一条空白
- 高度收了，但窗口没藏 → 标签条画在文件列表上

所以语义收进 `config.h` 的 `TabBarVisible(s, tabCount)` **一个函数**，
调用点全部改成调它。改规则只改一处。

更关键的是 **`LayoutKey.showBits` 必须包含这个判断的结果**：从两个标签
关到一个标签时，标签栏要消失 —— 那是真实的几何变化。如果指纹只看
`s.showTab`，`Layout` 会认为「布局没变」而直接返回，栏就留在屏幕上。

`tabCount` 由调用方给（`Config` 不知道标签集）。传 `0` 视为「还没建标签」，
此时按 `showTab` 走 —— 启动期不该因为「暂时 0 个标签」把栏藏掉。

### 3.2 `auditEnabled` 收口在 `Write()` 内部而不是调用方

审计有两个写入点（`commands_audit.cpp` 的两处），但真正的写盘只有
`audit_log.cpp` 的 `Write()` 一处。开关放在 `Write()` 入口：

- 新增调用方不用记得判断开关
- 关掉审计时返回 `true` 而不是 `false` —— 调用方把返回值当「审计写成功了吗」
  用，而**关闭审计是用户明确要求的正常状态，不是失败**

---

## 4. 剔除的候选（附理由）

| 候选 | 为什么不做 |
|---|---|
| 标签关闭撤销 toast | 项目**没有 toast 组件**，要新建浮动窗口 + 主题 + DPI + 超时销毁 + 点击撤销。这是**新功能**，不是「给已实现功能补开关」 |
| 拖拽修饰键语义（Ctrl 复制标签 / Alt 拖出成窗） | 功能本身**未实现**（全仓无 `MK_CONTROL` / `tearOff` 相关分支）。加开关等于加一个空开关 |
| AI 服务自动启动 | 同上，`autoStart` 全仓不存在；`aiEnabled` 已覆盖「是否启用 AI」的语义 |
| 窗口几何三项（x/y/w/h） | 刻意只读：由窗口实际位置驱动，可写会与拖动打架 |
| 布局下限、DPI 基数 | 契约而非策略，改了就坏 |

---

## 5. 验证

| 项 | 结果 |
|---|---|
| 独立 objdir 全量构建（208 TU） | 0 错误，链接通过 |
| `check_commands.py` | 命令表 241 / 已实现 234 / keymap 243，三方一致 |
| `check_ai_meta.py` | 全部通过 |
| `config_apply_test` / `config_roundtrip_test` | PASS（往返写入与分类判据都覆盖新键） |
| `check_components.py` | 唯一红项是 `tests/key_binding_test.cpp` 被并行会话改动待重签，与本轮无关 |

**为什么用独立 objdir**：共享 `obj/` 里有并行会话的陈旧产物
（`shell_input_uia.obj` 缺 `DetectViaUia` 符号，源码里其实已经实现）。
独立 objdir 全量构建才能确认「我的改动干净」与「别人的半成品」分开。

---

## 6. 剩余可选方向

以下都属于「策略值仍写死，但需要先确认是否真有人要调」：

- `tabAnimMs` / `tabTearOffDy` / `tabDragGhost` —— 并行会话已实现并登记，无需重复
- 边缘自动滚动带宽度、标签溢出收缩下限 —— 纯视觉，调到极端会难看，收益低
- 搜索历史菜单行数已可配（`searchHistoryMenuRows`），同类项无需再开
