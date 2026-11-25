// 整型设置项的**数值域唯一来源**：写入端（settings_reg）与读盘端（config_io）读同一份。
//
// 【为什么必须是一份】这三个数以前各写一遍，而且已经分叉了：
//   · settings_reg_table.cpp 声明 holdDelayMs 可以取 200..2000；
//   · config_io.cpp 读盘时写的是「< 300 就当 800」。
// 于是 `config.set holdDelayMs 250` 会被接受并写进 settings.json，**下次启动
// 悄悄变成 800** —— 同一个值在"写"和"读"两条路上有不同定义，用户看到的是
// "设置自己变回去了"，而没有任何检查会报。这正是 layout_limits.h 开头记的那条
// 教训（"界面给 90%、落盘只收 80%"）在时间/百分比类字段上的重演。
//
// 【分层位置】放在 config.h 与 settings_reg 之间：两边都能包含它，而它谁也不依赖
// （唯一的依赖是 pane_split.h 的百分比上下限 —— 那两个数本来就是分割条的领域常量，
// 再抄一份就又分叉了）。config_io 因此不必反过来包含 settings_reg。
//
// 【默认值不在这里】默认值是可调的策略（"我建议 800"），范围是可接受的边界
// （"我不接受 100"）。默认值留在 config.h 的成员初始化器里：一处就够，因为
// "恢复默认值"读的是默认构造的 Settings，不需要第二张表。
//
// 【命名口径】k<键名首字母大写>Min / Max，与 settings.json 的键名逐字对应，
// 便于两边对照核查（键名是永不改名的对外契约）。
#pragma once

#include "pane_split.h"   // panesplit::kMinPercent / kMaxPercent

// ---- 历史上限 ----
// 上限不是性能问题（几十条字符串），是**用户习惯**问题：有人一天跑几十个目录、
// 有人几乎只用收藏路径。所以可调，但两头都要夹。
constexpr int kRecentLimitMin = 1, kRecentLimitMax = 200;
constexpr int kSearchLimitMin = 1, kSearchLimitMax = 100;
constexpr int kClosedTabLimitMin = 1, kClosedTabLimitMax = 100;

// ---- 四条栏与栏内尺寸（96 DPI 逻辑像素）----
// 下限保证文字/图标还画得下；上限防止一条栏把内容区吃光。
constexpr int kTabBarHeightMin = 18, kTabBarHeightMax = 48;
constexpr int kNavBarHeightMin = 20, kNavBarHeightMax = 56;
constexpr int kBookmarkBarHeightMin = 18, kBookmarkBarHeightMax = 48;
constexpr int kStatusBarHeightMin = 18, kStatusBarHeightMax = 48;
constexpr int kNavBarSearchWidthMin = 120, kNavBarSearchWidthMax = 400;
constexpr int kNavBarAddressMinWidthMin = 100, kNavBarAddressMinWidthMax = 320;
constexpr int kBookmarkBarTooltipWidthMin = 240, kBookmarkBarTooltipWidthMax = 1200;
constexpr int kStatusBarTooltipWidthMin = 240, kStatusBarTooltipWidthMax = 1200;
constexpr int kNavPaneItemHeightMin = 20, kNavPaneItemHeightMax = 48;
constexpr int kNavPaneIndentMin = 12, kNavPaneIndentMax = 64;

// ---- 设置窗口三栏 ----
constexpr int kSettingsGroupWidthMin = 160, kSettingsGroupWidthMax = 320;
constexpr int kSettingsDetailWidthMin = 160, kSettingsDetailWidthMax = 360;
constexpr int kSettingsControlWidthMin = 120, kSettingsControlWidthMax = 280;

// ---- 视图档位 ----
// 下标的合法区间必须与 viewmode::kModes / 排序 / 分组的实际档数一致；
// 越界值不会崩，但会让整块视图落到"未知档"分支上，表现是"视图全乱"。
constexpr int kViewModeMin = 0, kViewModeMax = 7;
constexpr int kSortModeMin = 0, kSortModeMax = 3;
constexpr int kGroupByMin = 0, kGroupByMax = 3;

// ---- 双栏 ----
// 上下限取 panesplit.h：界面能拖到的区间、落盘接受的范围、命令 schema 里的
// minimum/maximum 必须是同一对数字。任何一个不同，"松手那一刻分割线往回跳"。
constexpr int kPaneSplitMin = panesplit::kMinPercent;
constexpr int kPaneSplitMax = panesplit::kMaxPercent;

// ---- 键盘时序（毫秒）----
// 下限的意义：小于它的值会让按键序列/长按在人的手速下失效（等于关掉了功能）。
// 上限的意义：超过它的值等于"永不触发"，同样是关掉功能但更隐蔽。
constexpr int kSeqTimeoutMsMin = 200, kSeqTimeoutMsMax = 5000;
constexpr int kHoldDelayMsMin = 200, kHoldDelayMsMax = 2000;

// ---- 标签宽度（逻辑像素）----
constexpr int kTabMinWidthMin = 60, kTabMinWidthMax = 320;
constexpr int kTabMaxWidthMin = 60, kTabMaxWidthMax = 420;

// 跨字段约束：上限不得低于下限（"每个标签最小 230、最多 100"没有意义）。
//
// 【为什么是一个谓词而不是各写一个 if】这条规则有两个执行点，它们的**策略**
// 不同（读盘是修复：把上限抬到下限；命令是拒绝：告诉调用方值不成立），
// 但**规则**必须同一份。各写一个 if 就是"手改文件能修好、命令却写进去一个
// 自相矛盾的对"，或者反过来。
constexpr bool TabWidthsConsistent(int minW, int maxW) { return maxW >= minW; }

// ---- AI 命令超时（毫秒）----
// 下限 1s：小于它会让所有命令都在"还没跑完"时被判超时，而那种失败看起来像
// "命令坏了"。上限 600s 足够扫完一个巨大的目录树。
constexpr int kAiTimeoutMsMin = 1000, kAiTimeoutMsMax = 600000;

// ---- 序列浮层手感 ----
// 滚轮每格行数：0 会让滚动失效，上限防止一格冲出可视区。
constexpr int kSeqWheelRowsMin = 1, kSeqWheelRowsMax = 10;
// 翻页行数：至少要能翻过一屏，太大则一次跳到几乎末尾。
constexpr int kSeqPageRowsMin = 2, kSeqPageRowsMax = 30;

// ---- 通用弹层 ----
// 键入定位缓冲：下限 200ms（低于此正常打字也会被拆成多个前缀），
// 上限 3000ms（超过此慢到几乎不会连续输入）。
constexpr int kPopupTypeAheadMsMin = 200, kPopupTypeAheadMsMax = 3000;
// 可见行上限：6 行是"还能不滚动地选"的下限，40 行已接近一屏极限。
constexpr int kPopupMaxRowsMin = 6, kPopupMaxRowsMax = 40;

// ---- 搜索历史下拉 ----
constexpr int kSearchHistoryMenuRowsMin = 5, kSearchHistoryMenuRowsMax = 50;

// ---- 导航历史栈深度 ----
// 下限 50（再小"后退"几乎没用），上限 2000（更大则 sessionHistory 膨胀明显）。
constexpr int kHistoryDepthMin = 50, kHistoryDepthMax = 2000;

// ---- 标签拖拽自动滚动 ----
constexpr int kTabDragScrollMsMin = 10, kTabDragScrollMsMax = 200;

// ---- 会话快照去抖 ----
// 下限 200ms（再短会频繁写盘），上限 10s（再长崩溃丢失过多状态）。
constexpr int kSessionSaveDelayMsMin = 200, kSessionSaveDelayMsMax = 10000;

// ---- AI 请求泵 ----
// 下限 20ms（再短空转吃 CPU），上限 500ms（再长每条命令Fixed延迟明显）。
constexpr int kAiPumpIntervalMsMin = 20, kAiPumpIntervalMsMax = 500;

// ---- 审计日志 ----
constexpr int kAuditRotateKBMin = 256, kAuditRotateKBMax = 65536;
constexpr int kAuditArgsTextLimitMin = 40, kAuditArgsTextLimitMax = 2000;

// ---- 机器作业超时 ----
constexpr int kJobMachineTimeoutMsMin = 1000, kJobMachineTimeoutMsMax = 600000;
