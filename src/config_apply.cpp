// 见 config_apply.h：设置项 -> 落地动作的判据表（唯一实现）。
#include "config_apply.h"

namespace cfgapply {

bool AffectsLayout(const std::wstring& key) {
    // 四个栏显隐 + 导航窗格（显隐与宽度）+ 双栏与分栏比例。
    // 判据是"这一项会不会改变客户区的分配"。
    return key == L"tab" || key == L"nav" || key == L"bookmark" || key == L"statusbar" ||
           key == L"showNavPane" || key == L"navPaneWidth" || key == L"aiPanelWidth" ||
           key == L"dualPane" || key == L"paneSplit" || key == L"paneVertical" ||
           // 每条栏的高度与导航栏内宽：都改变客户区/控件几何，走同一次 Layout。
           key == L"tabBarHeight" || key == L"navBarHeight" || key == L"bookmarkBarHeight" ||
           key == L"statusBarHeight" || key == L"navBarSearchWidth" ||
           key == L"navBarAddressMinWidth" ||
           // 导航树行距/缩进也是几何（App 会额外刷新树控件，再统一 Layout）。
           key == L"navPaneItemHeight" || key == L"navPaneIndent";
}

bool AffectsActiveView(const std::wstring& key) {
    return key == L"viewMode" || key == L"sortMode" || key == L"sortAscending" ||
           key == L"groupBy";
}

bool AffectsStatusBar(const std::wstring& key) {
    return key == L"statusbar" || key == L"statusSegs";
}

bool AffectsTheme(const std::wstring& key) { return key == L"themeMode"; }

bool AffectsHistoryLimits(const std::wstring& key) {
    // 三个上限：recent / search 由 Config 自己持有；closedTab 由 TabSet 持有；
    // historyDepth 由每个窗格的 nav::History 持有（ApplyHistoryLimits 推过去并裁剪）。
    return key == L"recentLimit" || key == L"searchLimit" || key == L"closedTabLimit" ||
           key == L"historyDepth";
}

bool NeedsAnyApply(const std::wstring& key) {
    return AffectsLayout(key) || AffectsActiveView(key) || AffectsStatusBar(key) ||
           AffectsTheme(key) || AffectsHistoryLimits(key) ||
           // 这些键的落地在别处（各有一条专门的分支），仍然属于"需要动作"。
           key == L"aiEnabled" || key == L"tabMinWidth" || key == L"tabMaxWidth" ||
           key == L"bookmarkBarTooltipWidth" || key == L"statusBarTooltipWidth" ||
           // 设置窗口自己的三栏：落地在命令尾部的 OnConfigChangedExternally。
           key == L"settingsGroupWidth" || key == L"settingsDetailWidth" ||
           key == L"settingsControlWidth";
}

bool IsPureData(const std::wstring& key) {
    // 【逐个列名，不用"没分支就算纯数据"】后者会让"新加键忘了写落地"静默通过 ——
    // 那正是这一轮要修的那类错。只读项不在本函数管辖内（它们不会经 config.set 落地）。
    //
    // 这些项改完当场没有可见效果，只影响之后的某一次读取/某一次输入：
    //   seqTimeoutMs / holdDelayMs → 下一次按键序列 / 长按才读
    //   seqEnabled / seqShowHints / seqSingleInstant / seqUniqueAuto → 下一次按键
    //   tabNewPos → 下一次新建标签
    //   confirmCloseTabs → 下一次关窗口
    //   aiAllowDanger / aiTimeoutMs → 下一次危险命令 / 下一次调用
    //   searchRange / searchRangePath → 下一次搜索（范围菜单自己读配置渲染）
    //   showPreviewPane → 下一次建视图
    //   statusSegs → 由状态栏自己重排（AffectsStatusBar 已覆盖，不在此列）
    //   seqWheelRows / seqPageRows → 下一次打开序列浮层
    //   popupTypeAheadMs / popupMaxRows → 下一次打开任意弹层
    //   searchHistoryMenuRows → 下一次打开搜索历史下拉
    //   tabDragScrollMs → 下一次拖拽标签到边缘
    //   sessionSaveDelayMs → 下一次标签/路径变更
    //   aiPumpIntervalMs → 下一次启用 AI 通道（重建泵定时器）
    //   auditRotateKB / auditArgsTextLimit → 下一次写审计
    //   jobMachineTimeoutMs → 下一次启动机器作业
    return key == L"seqTimeoutMs" || key == L"holdDelayMs" || key == L"seqEnabled" ||
           key == L"seqShowHints" || key == L"seqSingleInstant" || key == L"seqUniqueAuto" ||
           key == L"tabNewPos" || key == L"confirmCloseTabs" || key == L"aiAllowDanger" ||
           key == L"aiTimeoutMs" || key == L"searchRange" || key == L"searchRangePath" ||
           key == L"showPreviewPane" ||
           // 本轮新增的"下次生效"策略项。
           key == L"seqWheelRows" || key == L"seqPageRows" ||
           key == L"popupTypeAheadMs" || key == L"popupMaxRows" ||
           key == L"searchHistoryMenuRows" || key == L"tabDragScrollMs" ||
           key == L"sessionSaveDelayMs" || key == L"aiPumpIntervalMs" ||
           key == L"auditRotateKB" || key == L"auditArgsTextLimit" ||
           key == L"jobMachineTimeoutMs";
}

}  // namespace cfgapply