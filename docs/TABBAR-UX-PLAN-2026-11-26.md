---
status: current
baseline: 2026-11-26
method: 源码通读（tab_strip* / tab_set / app_tabs / app_commands_tabs / tab_uia）+ 全网调研（Chrome/Edge/Firefox/Arc/VS Code/Windows Terminal/Files/Directory Opus/Windows 11 Explorer）+ 本仓既有复审（REVIEW-TABBAR-RECHECK-2026-11-26）
---

# 标签栏交互与视觉改进计划（TABBAR-UX-PLAN-2026-11-26）

> 本文回答两件事：**标签栏现在差在哪**、**按什么顺序补**。
> 只写可执行项，每条给出落地符号/文件与验收方式；行号会漂移，一律用符号名定位。
> 与既有文档的边界：崩溃/错位级缺陷见 `REVIEW-TABBAR-RECHECK-2026-11-26.md`（S1/S2/M1–M3），
> 本文只管**交互手感与视觉**，不重复代码审查结论。

---

## 0. 结论摘要

1. **「标签不能移动」不是没有实现**：拖拽重排的完整链路在 `tab_strip_input.cpp`
   （`WM_LBUTTONDOWN` 记 `dragIdx_` → `WM_MOUSEMOVE` 过 4dp 死区进 `dragging_` →
   `DropTargetAt` 算落点 → `WM_LBUTTONUP` 发 `WM_APP_TAB_MOVED` → `tab.move.to` →
   `TabSet::Move`）。问题在**反馈太弱**：只有一条 4dp 竖线，标签不跟手、不让位，松手才瞬间换位。
2. **真正的功能缺口**是「拖出成窗」只有菜单入口（`tab.move.window`），没有拖拽路径；
   以及拖拽无修饰键语义、关闭无撤销提示、标签切换器未落地。
3. **视觉上**是「能看但不精致」：激活标签是纯色块、分隔线不避让、溢出硬切、无动效体系。
   这些不需要换渲染架构，在现有 `ui::PaintBuffered` + `theme_` 令牌上就能补齐。

---

## 1. 现状盘点

### 1.1 已具备（不再重复做）

| 能力 | 落点 |
|---|---|
| 拖拽重排 + 插入竖线 + 边缘自动滚动 + Esc 取消 + 捕获丢失复位 | `tab_strip_input.cpp` / `edge_autoscroll.h` |
| 固定 Pin（40dp 图标化、无关闭钮、不参与批量关闭） | `tab_strip_layout.cpp` / `tab_strip_paint.cpp` |
| 锁定（防关） | `tab_set.cpp` |
| 标签颜色（8 档）、改名、关闭重复、重开历史（栈深 20） | `tab_set.*` / `tab_strip_actions.cpp` |
| MRU 循环（Ctrl+Tab） | `tab_set.cpp` `CycleMru` |
| 溢出：等比收缩 + `»` 菜单 + rapid closure | `tab_strip_layout.cpp` |
| 关闭钮渐进显示（激活/悬停） | `CloseVisible` |
| UIA provider（含键盘路径） | `tab_uia.*` |

### 1.2 半有 / 缺失

| 项 | 现状 | 差距 |
|---|---|---|
| 拖拽让位 | 有插入线，无跟手/动画 | P0 已补：浮层跟手 + 松手让位动画（见 §7） |
| 拖出成窗 | 仅菜单 `tab.move.window` | P0 已补：垂直磁吸拖拽触发（见 §7） |
| 拖拽修饰键 | 无 | Ctrl=复制、Alt=新窗口、Shift=插入 |
| 关闭撤销 | 有命令、无提示 | 缺 toast「已关闭 xxx · 撤销」 |
| 标签切换器 | 命令位已登记 | 未实现缩略图网格 |
| 锁定语义 | 只防关 | Opus 的「锁路径→子目录开新标签」等档位缺失 |
| 悬停预览 | 仅 tooltip 路径 | 无缩略图卡片 |
| 标签组/空间 | 无 | 大特性，暂缓 |
| 单标签隐藏标签栏 | 无 | Opus 有「仅多标签时显示」 |

---

## 2. 「不能移动」的定位与排查

代码链路是通的；若实测完全拖不动，按顺序查：

| 排查点 | 判据 / 位置 |
|---|---|
| 运行的是旧 exe | `agentctl status` 看 `stale`；当前工作区有未提交改动时 exe 可能落后 |
| 鼠标规则抢了 DOWN | `App::OnChromeMouse` → `InputRouter::OnMouseDown`；若 `mouse.json` 给 `tab` 绑了 single 规则会 `return true`，拖拽起不来 |
| 捕获被抢 | 拖到一半弹菜单/切窗口 → `WM_CAPTURECHANGED` → `CancelDrag` |
| 落点没变 | `dropIdx_ == dragIdx_` 视为原地，不提交移动 |

> 并发提醒：本轮开工时另一会话仍在改 `app_wnd.cpp` / `input_router.cpp`。
> 改标签栏代码时**避开**这两个热点文件（`WM_APP_TAB_MOVED` 的转投已存在，不需要动）。

---

## 3. 对标总表（交互）

| 能力 | Chrome/Edge | Firefox | VS Code | Win Terminal | Files | Directory Opus | 本项目 |
|---|---|---|---|---|---|---|---|
| 拖拽重排 | ✅ + 让位动画 | ✅ 插入线 | ✅ 竖线+分组 | ✅ | ✅ | ✅ | ✅ 有 + 让位动画（P0） |
| 拖出成窗 | ✅ 15dp 磁吸 | ✅ | ✅ 浮动窗 | ✅ 跨窗拖 | ❌ | ✅ | ✅ 拖拽磁吸（P0）；跨窗仍无 |
| 跨窗口拖标签 | ✅ | ✅ | ✅ | ✅ | ❌ | ✅ | ❌ |
| 拖拽修饰键 | Ctrl 复制/Shift 插入 | — | Ctrl 复制 | — | — | Alt+点击切锁 | ❌ |
| 关闭后可撤销 | ✅ toast | ✅ | ✅ | — | — | ✅ | ⚠️ 有命令无提示 |
| 标签切换器 | Ctrl+Tab 预览 | ✅ | Ctrl+Tab | ✅ | — | ✅ 缩略图 | ⚠️ 仅登记 |
| 固定 Pin | ✅ 图标化 | ✅ | ✅ | — | — | ✅ | ✅ |
| 锁定语义 | — | — | — | — | — | ✅ 四档 | ⚠️ 只防关 |
| 标签组/空间 | ✅ 组 / Edge 垂直 | — | ✅ 分组 | — | — | ✅ 标签组 | ❌ |
| 溢出策略 | 等比+下拉 | 滚动 | 滚动 | 下拉兜底 | 滚动 | 滚动/多行 | ✅ 等比+» |
| 悬停预览 | ✅ 卡片 | ✅ | ✅ | — | — | — | ❌ 仅 tooltip |
| 键盘可达 | ✅ | ✅ | ✅ roving | ✅ | ✅ | ✅ | ⚠️ 有裸键，无焦点框 |

---

## 4. 交互改进清单（按优先级）

状态列：`已完成` = 本轮落地；`待做` = 已登记未动；`暂缓` = 依赖其它改动。

| 优先级 | 项 | 做法（贴合本仓） | 状态 |
|---|---|---|---|
| **P0-1** | 拖拽让位动画 | 同规模重排时按稳定 id 记录旧/新 `left`，`WM_TIMER` 插值 ease-out（~140ms），`Paint` 按偏移绘制 | 已完成（代码，待重建验收） |
| **P0-2** | 被拖标签抬升 | 拖拽中把被拖标签画成跟随光标的浮层（`brBg` 底 + `accent` 描边），原槽留空 | 已完成（代码，待重建验收） |
| **P0-3** | 拖出成窗磁吸 | 垂直越界 > 阈值进 tear-off 态，抑制插入线并显示提示；松手走 `kTabMenuMoveNewWindow` | 已完成（代码，待重建验收） |
| **P1-1** | 关闭 toast 撤销 | 关标签后右下角浮层「已关闭 xxx · 撤销」，复用 `TabReopenClosed` | 待做 |
| **P1-2** | 标签切换器 | 长按 Ctrl+Tab 出缩略图网格（`tab.switcher` 命令位已有） | 待做 |
| **P1-3** | 锁定标签导航语义 | 补 Opus 三档：锁路径→子目录开新标签 / 允许改目录 / 切走复原 | 待做 |
| **P1-4** | 极窄窗口兜底 | `n*minW > avail` 时提供「全部标签下拉」；或加「仅多标签时显示标签栏」 | 待做 |
| **P2-1** | 拖拽修饰键 | Ctrl+拖 = `tab.clone`；Alt+拖 = 新窗口；Shift+拖 = 强制插入 | 待做 |
| **P2-2** | 双击空白新建 | 当前刻意不做（与 + 重复），保持 | 暂缓 |
| **P2-3** | 悬停缩略图 | 成本高，先做延迟富 tooltip（路径 + 修改时间） | 待做 |
| **P2-4** | 标签组/工作区 | 先做「按颜色分组 + 组折叠」 | 暂缓 |

---

## 5. 视觉改进清单

| 方向 | 现状 | 建议 | 状态 |
|---|---|---|---|
| 激活标签形态 | 纯 `brBg` 色块，无圆角无描边 | 顶部两角圆角 + 与内容区同色 + 1px `line` 描边 + 极轻阴影 | 待做 |
| 相邻分隔线 | 非激活标签右缘固定 1px，不避让 | hover/active 两侧分隔线淡出 | 待做 |
| 关闭钮 | 只有字形，hover 换 `text` 色 | 圆形 hover 底（`brHover`），按下态再深一档 | 待做 |
| 固定区 | 与普通区无分界 | 固定区末尾加一条竖直短分隔线 | 待做 |
| 溢出边缘 | 标签硬切在裁切线 | 右缘加 16dp alpha 渐隐遮罩 | 待做 |
| 拖拽反馈 | 4dp 静态竖线 | 浮层 + 让位动画后，竖线退为辅助（仅外部拖入时显示） | 部分完成（浮层/动画已做，竖线仍在） |
| 颜色标签 | 26% 全底 + 底部色条 | 深色主题下偏弱；改「左侧 3dp 色条 + 8% 底」 | 待做 |
| 动效体系 | 全仓无 duration/easing 令牌 | 统一时长/缓动，别各处写死 | 部分完成（标签栏 `TabAnimDx` + 设置项 `tabAnimMs`；其它栏未统一） |
| 键盘焦点 | 无焦点框 | roving tabindex + 2px `accent` 外框；`WM_UPDATEUISTATE` 控制显隐 | 待做 |
| 像素对齐 | 1px 线走 `S(1)`，高 DPI 可能发虚 | 分隔线/描边按物理像素对齐 | 待做 |
| 单标签场景 | 恒显示标签栏 | 加「仅多标签时显示」选项 | 待做 |

---

## 6. 设置项（P0 三项已可配置）

三项动效/交互都做成了设置，而不是写死常量。理由：动效与拖出成窗是**偏好**而非正确性 ——
有人嫌动画是延迟，有人嫌拖出成窗容易误触；写死就只剩「改代码重编」一条路。

| 键 | 类型 | 范围 | 默认 | 语义 |
|---|---|---|---|---|
| `tabAnimMs` | int | 0..400 | 140 | 拖拽重排的让位动画时长。**0 = 关闭动画**（直接换位，即旧行为） |
| `tabDragGhost` | bool | — | true | 拖动时画跟随光标的浮层（原槽留空）。关掉则只剩插入线 |
| `tabTearOffDy` | int | 0..80 | 18 | 拖出成窗的垂直磁吸阈值（逻辑像素）。**0 = 关闭拖出成窗**，只留右键菜单 |

**生效时机**：三者都在拖拽的**当帧**读 `Config::Get().s()`，所以「改完下一次拖拽即生效」，
不需要重启，也不需要重建标签栏。归在 `cfgapply::IsPureData`（不触发整窗布局）。

**为什么分成三个键而不是一个开关**：
- 「拖动中」的反馈（浮层）与「松手后」的过渡（动画）是两件事，有人只想去掉其中一个。
- 磁吸阈值必须是数字：能关（0）也能调（18→30 让误触更少），一个 bool 表达不了「调多敏感」。

**落地位置**（五处，缺一处就会「设置里改了没反应」）：

| 环节 | 文件 |
|---|---|
| 范围唯一来源 | `settings_bounds.h`（`kTabAnimMsMin/Max`、`kTabTearOffDyMin/Max`） |
| 字段与默认值 | `config.h`（`Settings::tabAnimMs/tabDragGhost/tabTearOffDy`） |
| 注册（可写/可夹/可脚本读/界面可见） | `settings_reg_table.cpp` 的 `bar_tab` 组 |
| 读盘夹取 / 写盘 | `config_io.cpp` 的 `ClampInt` / `root.Set` |
| 落地分类 | `config_apply.cpp` 的 `IsPureData`（下次拖拽生效） |
| 消费 | `tab_strip_anim.cpp`（动画时长、浮层开关）、`tab_strip_input.cpp`（磁吸阈值、动画收尾） |
| 界面单位 | `settings_dialog_layout.cpp` 的 `UnitOf`（毫秒 / 像素） |

**关闭后的行为边界**（写清楚，避免「关了但还留一半」）：
- `tabAnimMs=0`：重排直接落到最终位置；动画途中改成 0 会立刻收尾（`AnimDx` 走 t=1）。
- `tabDragGhost=false`：不画浮层，但**原槽仍然留空** —— 留空是「这一格被拿走了」的语义，
  不是装饰，去掉会让人以为标签还在原位。
- `tabTearOffDy=0`：不进入 tear-off 态，拖出标签栏只是普通重排；右键菜单的
  「移到新窗口」不受影响（那是另一条显式入口）。

---

## 7. 落地约束（本仓硬规则）

- 新交互一律走 `CommandBus`（`tab.move.to` / `tab.clone` / `tab.move.window` 已有底座），
  不新开私有消息旁路；`WM_APP_TAB_MOVED` 的转投已存在。
- 单文件 ≤500 行：动画/浮层放独立 `tab_strip_anim.cpp`；命中仍留 `tab_strip_input.cpp`。
- 不引第三方库、不写死像素/`RGB(`：尺寸走 `theme_->S()`，颜色走 `theme_` 令牌。
- 拖拽是脚本不可合成路径，验收只能「人工拖、脚本读状态/截图观察」，不得模拟输入。
- 与 `REVIEW-TABBAR-RECHECK` 的 S1/S2/M2/M3 分开提交，避免混在一起。
- **新增设置项必须同时改六处**（见 §6 表格）：漏掉 `config_apply` 会被
  `config_apply_test` 判红（「可写键既没有落地动作、也没被列为纯数据」）。

---

## 8. 实施记录

### 2026-11-26 23:35 — 第一批（P0-1 / P0-2 / P0-3）

**改动文件**

| 文件 | 内容 |
|---|---|
| `src/tab_strip.h` | 纯函数 `TabAnimDx`；`TabStripItem.id`（**稳定身份**，动画按 id 不按下标）；`animFrom_/animStart_/animating_/kAnimTimer`；`dragMouse_/dragGrabDx_/tearOff_/clientH_` |
| `src/tab_strip_anim.cpp`（新增 111 行） | `StartReorderAnim` / `StopReorderAnim` / `AnimDx` / `DrawDragGhost` |
| `src/tab_strip.cpp` | `SetTabs` 按 id 快照旧 `left`，`Layout`+`EnsureVisible` 后启动动画；`CancelDrag`/`ReloadTheme` 停动画 |
| `src/tab_strip_layout.cpp` | 缓存 `clientH_`；`ScrollBy` 停动画（滚动与让位是两种位移，不能叠加） |
| `src/tab_strip_paint.cpp` | 按 `AnimDx` 偏移绘制；拖拽中跳过原槽并画浮层；tear-off 时抑制插入线、整条显示「松开：移到新窗口」 |
| `src/tab_strip_input.cpp` | `WM_MOUSEMOVE` 更新 `dragMouse_` 并**每帧重绘**（浮层跟手）、判垂直越界；`WM_TIMER` 处理 `kAnimTimer`；`WM_LBUTTONUP` 走 tear-off 提交；`WM_SIZE` 停动画 |
| `tests/tab_strip_rules_test.cpp` | 新增 `TabAnimDx` 的 7 条断言（起点/终点/夹取/半程值/单调性） |

### 2026-11-26 23:55 — 第二批（P0 三项接入设置项）

**动机**：第一批把时长与阈值写成了头文件常量，用户只能「改代码重编」。它们都是偏好，
必须可关、可调 —— 于是接进设置系统（见 §6）。

**改动文件**

| 文件 | 内容 |
|---|---|
| `src/settings_bounds.h` | `kTabAnimMsMin/Max`（0..400）、`kTabTearOffDyMin/Max`（0..80） |
| `src/config.h` | `Settings::tabAnimMs=140` / `tabDragGhost=true` / `tabTearOffDy=18`（默认值唯一出处） |
| `src/settings_reg_table.cpp` | `bar_tab` 组新增三条注册（含中文说明与范围文本） |
| `src/config_io.cpp` | 读盘 `ClampInt` / `GetBool`；写盘 `root.Set` |
| `src/config_apply.cpp` | 三者归 `IsPureData`（下次拖拽生效，不触发整窗布局） |
| `src/settings_dialog_layout.cpp` | `UnitOf`：`tabAnimMs`→毫秒、`tabTearOffDy`→像素 |
| `src/tab_strip.h` | 删掉 `kTabAnimMs` / `kTabTearOffDy96` 两个常量（默认值改由 `Settings` 持有） |
| `src/tab_strip_anim.cpp` | 时长与浮层开关当帧读 `Config`；`tabAnimMs<=0` 不起动画、`AnimDx` 走 t=1 |
| `src/tab_strip_input.cpp` | 磁吸阈值当帧读 `Config`（0 = 关闭）；动画收尾也按当前时长判定 |

**为什么「当帧读」而不是缓存到成员**：三项都只在拖拽路径上读，一次拖拽最多几次读，
开销可忽略；缓存就得再写一套「设置变更 → 推给标签栏」的同步，那是新的分叉点。

**验证**

- 9 个改动 TU `clang++ -fsyntax-only` 全绿。
- `config_apply_test`：**全部通过（可写键 67 个，全部有落地归属）** —— 新增 3 键都被归类。
- `settings_reg_test`：**973 通过 / 0 失败**（含新键的读写与夹取）。
- `config_roundtrip_test`：**通过 93 / 失败 0**（遍历全部键，写盘→读盘值不变）。
- **全量构建仍被并发会话阻塞**：`app_commands_misc.cpp`（`ShowHotkeyManager` / `hotkeyManager_`
  未定义）、`app_layout.cpp`、`app_pane_chips.cpp`（`NavBar::SetPaneCmdFn` 缺失）编译失败，
  这三个文件都是另一会话的未完成改动，与本批零交集。等它收口后再整窗构建。

**关键取舍**

- 动画只对「同规模重排」启动：关闭/新建/rapid close 各有既定路径，硬加滑动会打乱关闭钮落点。
- 浮层用 `brBg` 实底 + `accent` 描边，**不是 alpha**：GDI 每帧建 32bpp DIB 是纯 churn；
  等有统一合成层再换。原槽留空 + 插入线仍保留，落点不丢。
- tear-off 复用 `kTabMenuMoveNewWindow`（App 侧已是完整实现：新实例按路径打开 + 关源标签），
  不新开私有消息通道；锁定/固定/空路径标签不参与。

**验证**

- 5 个改动 TU `clang++ -fsyntax-only` 全绿。
- `tab_strip_rules_test` 编译运行**全部通过**（含新增 7 条）。
- `check_components.py`：本批文件最大 443 行，未触 500 红线。报告的两类失败均**不是本批代码缺陷**：
  - 500 行超限：`app.cpp` / `app.h` / `settings_reg_table.cpp` / `shell_input.cpp`（并发会话的改动）。
  - 评估面待重签：本批对 `tests/tab_strip_rules_test.cpp` **新增**了 7 条断言（加强判据，不是放宽），
    连同并发会话的 `check_ai_meta.py` / `transfer_undo_test.cpp`，需由人执行
    `python tools/gen_eval_baseline.py --human-confirm` 重签。**未自签**（这是人把关的门）。
- **未做全量构建/链接**：开工时另一会话仍在跑（`ExBarBrowser.new.exe` 在进程表里），
  避免抢 `obj/` 与 exe。重建后用 §8 的方式验收。

**残留**

- 需重建后**人工拖拽验收**：浮层跟手、松手让位动画、拖出成窗提示与结果。
- 拖拽过程中**其他标签尚未实时让位**（目前只有松手后的动画），是下一步；
  做的时候要与 `dropIdx_` 的落点口径统一，避免「看得见的空位」和「实际插入位」不一致。
- 浮层若拖到窗口外会被裁掉（窗口 DC 裁剪），tear-off 提示在条内，能接受。

---

## 9. 验收方法

| 要验证什么 | 方法 |
|---|---|
| 重排动画 | 人工拖 3 个以上标签，观察让位是否连续；脚本只截图 |
| 浮层跟手 | 人工拖拽，确认被拖标签跟随光标且原槽留空 |
| 拖出成窗 | 人工把标签拖离标签栏 > 阈值，确认提示出现、松手开新窗口且源标签关闭 |
| 不影响既有 | `bash tests/run_all.sh tab`；`python tools/check_components.py` |
| 视觉 | `python scripts\shot.py out.png`（只读 PrintWindow） |
| **设置项生效** | `config.set tabAnimMs 0` → 拖一次确认无动画；再设回 140 确认恢复 |
| **设置项落盘** | `config.get tabAnimMs` / `tabDragGhost` / `tabTearOffDy` 读回等于所设；重启后仍保持 |
| **关闭语义** | `tabTearOffDy=0` 时拖离标签栏只重排、不开新窗口；`tabDragGhost=false` 时无浮层但原槽留空 |
