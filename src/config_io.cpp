// settings.json 的**读写**：偏好字段的 JSON 映射、读盘夹取。
//
// 【为什么单独成文件】config.cpp 里混着两件事，改动节奏完全不同：
//   · 路径与写入口（Dir / SettingsPath / AddRecent / HotkeyOverride…）—— 被
//     "用户做了什么动作"驱动，改的是数据模型；
//   · settings.json 的读写（Load / Save）—— 被"文件格式"驱动，改的是**存盘契约**。
// 加一个设置项要动这里（两侧各一行）+ 表本体一处；而改路径规则只动 config.cpp。
//
// 【为什么这里只剩偏好】状态（会话/历史/标签/窗口）已拆到 state\*.json，
// 由 config_state_io.cpp 负责。settings.json 只留低频改、人工可读的偏好；
// 高频写的状态不再因为改一个栏显隐就被整份重写。
//
// 【键名口径】这里的键名与 settings.json 里的键名**逐字相同**，也与
// settings_reg 的键名相同：三处一致才能做到"手改文件 / 命令 / 界面"是同一套名字。
// 认不出的键静默忽略（向前兼容：新版本写的项，旧版本读进来只是不认识，不该报错）。
//
// 【读盘一律夹取】settings.json 是用户可改的文本，也可能是别的工具写坏的。
// 越界值不报错（那样会打不开程序），但必须夹到物理合理范围 —— 否则一个
// "w": 10000000000 会让窗口落到看不见的地方，且没有任何报错。
#include "config.h"

#include "config_io_internal.h"
#include "common.h"         // LogDbg
#include "layout_limits.h"  // 两条侧栏宽度的上下限（与界面拖动、设置项范围同一份）
#include "settings_bounds.h"  // 整型设置的数值域（与 settings_reg 的写入范围同一份）
#include "str.h"            // str::Lower（与写入端同一套大小写归一）
#include "text_file.h"

void Config::Load() {
    if (loaded_) return;
    loaded_ = true;
    // 六类目录一次性建好：它们是运行期所有落盘的父目录，晚建会让第一次写失败。
    tf::EnsureDir(Dir());
    tf::EnsureDir(StateDir());
    tf::EnsureDir(LogsDir());
    tf::EnsureDir(CacheDir());
    tf::EnsureDir(RuntimeDir());

    std::wstring text;
    if (!tf::ReadText(SettingsPath(), &text)) {
        // 读不到：文件不存在（首次运行，正常）或读不出（异常）。
        // 只有"存在但读不出"才需要禁止写盘。
        parseFailed_ = tf::Exists(SettingsPath());
        if (parseFailed_) LogDbg(L"[config] settings.json 存在但读不出，已禁止本次会话覆盖写");
        // 偏好文件缺失时状态文件仍可能存在（profile 被手工搬迁），照常读。
        LoadStateFiles();
        return;
    }
    json::Value root;
    std::wstring err;
    if (!json::Parse(text, root, &err) || !root.IsObject()) {
        // 【禁止后续 Save 覆盖】保留默认值继续运行（不能因为配置坏了就打不开程序），
        // 但绝不能把"只有默认值的 JSON"写回去 —— 那会抹掉用户的偏好。
        parseFailed_ = true;
        LogDbg(L"[config] settings.json 解析失败：%s（已禁止覆盖写，修好或删除该文件后重启）",
               err.c_str());
        return;
    }

    s_.showTab = root.GetBool(L"tab", s_.showTab);
    s_.showNav = root.GetBool(L"nav", s_.showNav);
    s_.showBookmark = root.GetBool(L"bookmark", s_.showBookmark);
    s_.showStatus = root.GetBool(L"statusbar", s_.showStatus);

    // 四条栏高度 + 各栏自己的尺寸项：与注册表共用同一对上下限，
    // 手改 settings.json 也不会把栏高写成 1px 或 1000px。
    s_.tabBarHeight = configio::ClampInt(root.GetInt(L"tabBarHeight", s_.tabBarHeight),
                                         kTabBarHeightMin, kTabBarHeightMax);
    s_.navBarHeight = configio::ClampInt(root.GetInt(L"navBarHeight", s_.navBarHeight),
                                         kNavBarHeightMin, kNavBarHeightMax);
    s_.bookmarkBarHeight = configio::ClampInt(root.GetInt(L"bookmarkBarHeight", s_.bookmarkBarHeight),
                                              kBookmarkBarHeightMin, kBookmarkBarHeightMax);
    s_.statusBarHeight = configio::ClampInt(root.GetInt(L"statusBarHeight", s_.statusBarHeight),
                                            kStatusBarHeightMin, kStatusBarHeightMax);
    s_.navBarSearchWidth = configio::ClampInt(root.GetInt(L"navBarSearchWidth", s_.navBarSearchWidth),
                                              kNavBarSearchWidthMin, kNavBarSearchWidthMax);
    s_.navBarAddressMinWidth =
        configio::ClampInt(root.GetInt(L"navBarAddressMinWidth", s_.navBarAddressMinWidth),
                           kNavBarAddressMinWidthMin, kNavBarAddressMinWidthMax);
    s_.bookmarkBarTooltipWidth =
        configio::ClampInt(root.GetInt(L"bookmarkBarTooltipWidth", s_.bookmarkBarTooltipWidth),
                           kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax);
    s_.bookmarkWheelStepDp =
        configio::ClampInt(root.GetInt(L"bookmarkWheelStepDp", s_.bookmarkWheelStepDp),
                           kBmkWheelStepDpMin, kBmkWheelStepDpMax);
    s_.statusBarTooltipWidth =
        configio::ClampInt(root.GetInt(L"statusBarTooltipWidth", s_.statusBarTooltipWidth),
                           kStatusBarTooltipWidthMin, kStatusBarTooltipWidthMax);
    s_.navPaneItemHeight = configio::ClampInt(root.GetInt(L"navPaneItemHeight", s_.navPaneItemHeight),
                                              kNavPaneItemHeightMin, kNavPaneItemHeightMax);
    s_.navPaneIndent = configio::ClampInt(root.GetInt(L"navPaneIndent", s_.navPaneIndent),
                                          kNavPaneIndentMin, kNavPaneIndentMax);
    s_.settingsGroupWidth = configio::ClampInt(root.GetInt(L"settingsGroupWidth", s_.settingsGroupWidth),
                                               kSettingsGroupWidthMin, kSettingsGroupWidthMax);
    s_.settingsDetailWidth = configio::ClampInt(root.GetInt(L"settingsDetailWidth", s_.settingsDetailWidth),
                                                kSettingsDetailWidthMin, kSettingsDetailWidthMax);
    s_.settingsControlWidth =
        configio::ClampInt(root.GetInt(L"settingsControlWidth", s_.settingsControlWidth),
                           kSettingsControlWidthMin, kSettingsControlWidthMax);

    s_.seqEnabled = root.GetBool(L"seqEnabled", s_.seqEnabled);
    s_.seqTimeoutMs = root.GetInt(L"seqTimeoutMs", s_.seqTimeoutMs);
    s_.seqShowHints = root.GetBool(L"seqShowHints", s_.seqShowHints);
    s_.seqSingleInstant = root.GetBool(L"seqSingleInstant", s_.seqSingleInstant);
    s_.seqUniqueAuto = root.GetBool(L"seqUniqueAuto", s_.seqUniqueAuto);
    s_.holdDelayMs = root.GetInt(L"holdDelayMs", s_.holdDelayMs);
    // 【为什么必须用同一份上下限】写/读两条路不能各自定义，否则
    // `config.set holdDelayMs 250` 会被接受、写进去，下次启动又悄悄变回去。
    s_.seqTimeoutMs = configio::ClampInt(s_.seqTimeoutMs, kSeqTimeoutMsMin, kSeqTimeoutMsMax);
    s_.holdDelayMs = configio::ClampInt(s_.holdDelayMs, kHoldDelayMsMin, kHoldDelayMsMax);
    s_.seqWheelRows = configio::ClampInt(root.GetInt(L"seqWheelRows", s_.seqWheelRows),
                                         kSeqWheelRowsMin, kSeqWheelRowsMax);
    s_.seqPageRows = configio::ClampInt(root.GetInt(L"seqPageRows", s_.seqPageRows),
                                        kSeqPageRowsMin, kSeqPageRowsMax);
    s_.popupTypeAheadMs = configio::ClampInt(root.GetInt(L"popupTypeAheadMs", s_.popupTypeAheadMs),
                                             kPopupTypeAheadMsMin, kPopupTypeAheadMsMax);
    s_.popupMaxRows = configio::ClampInt(root.GetInt(L"popupMaxRows", s_.popupMaxRows),
                                         kPopupMaxRowsMin, kPopupMaxRowsMax);
    s_.popupDragThreshold =
        configio::ClampInt(root.GetInt(L"popupDragThreshold", s_.popupDragThreshold),
                           kPopupDragThresholdMin, kPopupDragThresholdMax);

    s_.tabNewPos = root.GetStr(L"tabNewPos", s_.tabNewPos);
    // 旧值 "right" 归一成 "end"（界面一直写着"最右"，一个取值两种行为）。
    if (str::IEquals(s_.tabNewPos.c_str(), L"right")) s_.tabNewPos = L"end";
    s_.confirmCloseTabs = root.GetBool(L"confirmCloseTabs", s_.confirmCloseTabs);
    s_.hideTabBarWhenSingle =
        root.GetBool(L"hideTabBarWhenSingle", s_.hideTabBarWhenSingle);
    s_.tabMinWidth = configio::ClampInt(root.GetInt(L"tabMinWidth", s_.tabMinWidth),
                                        kTabMinWidthMin, kTabMinWidthMax);
    s_.tabMaxWidth = configio::ClampInt(root.GetInt(L"tabMaxWidth", s_.tabMaxWidth),
                                        kTabMaxWidthMin, kTabMaxWidthMax);
    if (!TabWidthsConsistent(s_.tabMinWidth, s_.tabMaxWidth)) s_.tabMaxWidth = s_.tabMinWidth;
    s_.tabDragScrollMs = configio::ClampInt(root.GetInt(L"tabDragScrollMs", s_.tabDragScrollMs),
                                            kTabDragScrollMsMin, kTabDragScrollMsMax);
    s_.tabAnimMs = configio::ClampInt(root.GetInt(L"tabAnimMs", s_.tabAnimMs), kTabAnimMsMin,
                                      kTabAnimMsMax);
    s_.tabDragGhost = root.GetBool(L"tabDragGhost", s_.tabDragGhost);
    s_.tabTearOffDy = configio::ClampInt(root.GetInt(L"tabTearOffDy", s_.tabTearOffDy),
                                         kTabTearOffDyMin, kTabTearOffDyMax);

    // 深浅色：先归一成规范形（小写）再判合法 —— 手抄 "Dark" 应该生效。
    s_.themeMode = str::Lower(root.GetStr(L"themeMode", s_.themeMode));
    if (s_.themeMode != L"dark" && s_.themeMode != L"light") s_.themeMode = L"system";

    // 历史上限：手改成 0/负数会让历史永远空着，过大则配置里堆几百条路径。两头都夹。
    s_.recentLimit = configio::ClampInt(root.GetInt(L"recentLimit", s_.recentLimit),
                                        kRecentLimitMin, kRecentLimitMax);
    s_.searchLimit = configio::ClampInt(root.GetInt(L"searchLimit", s_.searchLimit),
                                        kSearchLimitMin, kSearchLimitMax);
    s_.closedTabLimit = configio::ClampInt(root.GetInt(L"closedTabLimit", s_.closedTabLimit),
                                           kClosedTabLimitMin, kClosedTabLimitMax);
    s_.historyDepth = configio::ClampInt(root.GetInt(L"historyDepth", s_.historyDepth),
                                         kHistoryDepthMin, kHistoryDepthMax);
    s_.searchHistoryMenuRows =
        configio::ClampInt(root.GetInt(L"searchHistoryMenuRows", s_.searchHistoryMenuRows),
                           kSearchHistoryMenuRowsMin, kSearchHistoryMenuRowsMax);

    // 视图：越界值一律夹回合法范围。
    s_.viewMode = configio::ClampInt(root.GetInt(L"viewMode", s_.viewMode), kViewModeMin, kViewModeMax);
    s_.sortMode = configio::ClampInt(root.GetInt(L"sortMode", s_.sortMode), kSortModeMin, kSortModeMax);
    s_.sortAscending = root.GetBool(L"sortAscending", s_.sortAscending);
    s_.groupBy = configio::ClampInt(root.GetInt(L"groupBy", s_.groupBy), kGroupByMin, kGroupByMax);

    s_.showNavPane = root.GetBool(L"showNavPane", s_.showNavPane);
    s_.showPreviewPane = root.GetBool(L"showPreviewPane", s_.showPreviewPane);
    s_.navPaneWidth = configio::ClampInt(root.GetInt(L"navPaneWidth", s_.navPaneWidth),
                                         kNavPaneWidthMin, kNavPaneWidthMax);
    s_.aiPanelWidth = configio::ClampInt(root.GetInt(L"aiPanelWidth", s_.aiPanelWidth),
                                         kAiPanelWidthMin, kAiPanelWidthMax);
    s_.navPaneRoots = configio::ReadStringArray(root, L"navPaneRoots");
    s_.dualPane = root.GetBool(L"dualPane", s_.dualPane);
    s_.paneSplit = configio::ClampInt(root.GetInt(L"paneSplit", s_.paneSplit),
                                      kPaneSplitMin, kPaneSplitMax);
    s_.paneVertical = root.GetBool(L"paneVertical", s_.paneVertical);
    s_.splitMarkThick = configio::ClampInt(root.GetInt(L"splitMarkThick", s_.splitMarkThick),
                                          kSplitMarkThickMin, kSplitMarkThickMax);

    // AI 通道：默认开启（缺省即监听），配置里显式写 false 才关。
    s_.aiEnabled = root.GetBool(L"aiEnabled", s_.aiEnabled);
    s_.aiAllowDanger = root.GetBool(L"aiAllowDanger", s_.aiAllowDanger);
    s_.aiTimeoutMs = configio::ClampInt(root.GetInt(L"aiTimeoutMs", s_.aiTimeoutMs),
                                        kAiTimeoutMsMin, kAiTimeoutMsMax);
    s_.aiPumpIntervalMs = configio::ClampInt(root.GetInt(L"aiPumpIntervalMs", s_.aiPumpIntervalMs),
                                             kAiPumpIntervalMsMin, kAiPumpIntervalMsMax);
    s_.aiPeekEnabled = root.GetBool(L"aiPeekEnabled", s_.aiPeekEnabled);

    // 会话快照去抖 + 审计/作业策略。
    s_.auditEnabled = root.GetBool(L"auditEnabled", s_.auditEnabled);
    s_.sessionSaveDelayMs =
        configio::ClampInt(root.GetInt(L"sessionSaveDelayMs", s_.sessionSaveDelayMs),
                           kSessionSaveDelayMsMin, kSessionSaveDelayMsMax);
    s_.auditRotateKB = configio::ClampInt(root.GetInt(L"auditRotateKB", s_.auditRotateKB),
                                          kAuditRotateKBMin, kAuditRotateKBMax);
    s_.auditArgsTextLimit = configio::ClampInt(root.GetInt(L"auditArgsTextLimit", s_.auditArgsTextLimit),
                                               kAuditArgsTextLimitMin, kAuditArgsTextLimitMax);
    s_.jobMachineTimeoutMs =
        configio::ClampInt(root.GetInt(L"jobMachineTimeoutMs", s_.jobMachineTimeoutMs),
                           kJobMachineTimeoutMsMin, kJobMachineTimeoutMsMax);

    // ---- 状态：四份 state\*.json ----
    // 偏好从 settings.json 读，状态只从 state\ 读；两条互不覆盖。
    LoadStateFiles();
}

bool Config::Save() {
    saveError_.clear();   // 每次写盘都重新判定：上一次的失败不该永远跟着
    // 加载时解析失败过就别写 —— 内存里只有默认值，写回去等于销毁用户偏好。
    if (parseFailed_) {
        saveError_ = L"settings.json 上次解析失败，为免覆盖已拒绝写盘（请先修好该文件）";
        LogDbg(L"[config] 拒绝写盘：settings.json 上次解析失败");
        return false;
    }
    json::Value root = json::Value::MakeObject();
    root.Set(L"tab", json::Value::MakeBool(s_.showTab));
    root.Set(L"nav", json::Value::MakeBool(s_.showNav));
    root.Set(L"bookmark", json::Value::MakeBool(s_.showBookmark));
    root.Set(L"statusbar", json::Value::MakeBool(s_.showStatus));
    root.Set(L"tabBarHeight", json::Value::MakeInt(s_.tabBarHeight));
    root.Set(L"navBarHeight", json::Value::MakeInt(s_.navBarHeight));
    root.Set(L"bookmarkBarHeight", json::Value::MakeInt(s_.bookmarkBarHeight));
    root.Set(L"statusBarHeight", json::Value::MakeInt(s_.statusBarHeight));
    root.Set(L"navBarSearchWidth", json::Value::MakeInt(s_.navBarSearchWidth));
    root.Set(L"navBarAddressMinWidth", json::Value::MakeInt(s_.navBarAddressMinWidth));
    root.Set(L"bookmarkBarTooltipWidth", json::Value::MakeInt(s_.bookmarkBarTooltipWidth));
    root.Set(L"bookmarkWheelStepDp", json::Value::MakeInt(s_.bookmarkWheelStepDp));
    root.Set(L"statusBarTooltipWidth", json::Value::MakeInt(s_.statusBarTooltipWidth));
    root.Set(L"navPaneItemHeight", json::Value::MakeInt(s_.navPaneItemHeight));
    root.Set(L"navPaneIndent", json::Value::MakeInt(s_.navPaneIndent));
    root.Set(L"settingsGroupWidth", json::Value::MakeInt(s_.settingsGroupWidth));
    root.Set(L"settingsDetailWidth", json::Value::MakeInt(s_.settingsDetailWidth));
    root.Set(L"settingsControlWidth", json::Value::MakeInt(s_.settingsControlWidth));

    root.Set(L"seqEnabled", json::Value::MakeBool(s_.seqEnabled));
    root.Set(L"seqTimeoutMs", json::Value::MakeInt(s_.seqTimeoutMs));
    root.Set(L"seqShowHints", json::Value::MakeBool(s_.seqShowHints));
    root.Set(L"seqSingleInstant", json::Value::MakeBool(s_.seqSingleInstant));
    root.Set(L"seqUniqueAuto", json::Value::MakeBool(s_.seqUniqueAuto));
    root.Set(L"holdDelayMs", json::Value::MakeInt(s_.holdDelayMs));
    root.Set(L"seqWheelRows", json::Value::MakeInt(s_.seqWheelRows));
    root.Set(L"seqPageRows", json::Value::MakeInt(s_.seqPageRows));
    root.Set(L"popupTypeAheadMs", json::Value::MakeInt(s_.popupTypeAheadMs));
    root.Set(L"popupMaxRows", json::Value::MakeInt(s_.popupMaxRows));
    root.Set(L"popupDragThreshold", json::Value::MakeInt(s_.popupDragThreshold));
    root.Set(L"tabNewPos", json::Value::MakeStr(s_.tabNewPos));
    root.Set(L"confirmCloseTabs", json::Value::MakeBool(s_.confirmCloseTabs));
    root.Set(L"hideTabBarWhenSingle", json::Value::MakeBool(s_.hideTabBarWhenSingle));
    root.Set(L"tabMinWidth", json::Value::MakeInt(s_.tabMinWidth));
    root.Set(L"tabMaxWidth", json::Value::MakeInt(s_.tabMaxWidth));
    root.Set(L"tabDragScrollMs", json::Value::MakeInt(s_.tabDragScrollMs));
    root.Set(L"tabAnimMs", json::Value::MakeInt(s_.tabAnimMs));
    root.Set(L"tabDragGhost", json::Value::MakeBool(s_.tabDragGhost));
    root.Set(L"tabTearOffDy", json::Value::MakeInt(s_.tabTearOffDy));
    root.Set(L"themeMode", json::Value::MakeStr(s_.themeMode));
    root.Set(L"recentLimit", json::Value::MakeInt(s_.recentLimit));
    root.Set(L"searchLimit", json::Value::MakeInt(s_.searchLimit));
    root.Set(L"closedTabLimit", json::Value::MakeInt(s_.closedTabLimit));
    root.Set(L"historyDepth", json::Value::MakeInt(s_.historyDepth));
    root.Set(L"searchHistoryMenuRows", json::Value::MakeInt(s_.searchHistoryMenuRows));
    root.Set(L"viewMode", json::Value::MakeInt(s_.viewMode));
    root.Set(L"sortMode", json::Value::MakeInt(s_.sortMode));
    root.Set(L"sortAscending", json::Value::MakeBool(s_.sortAscending));
    root.Set(L"groupBy", json::Value::MakeInt(s_.groupBy));
    root.Set(L"showNavPane", json::Value::MakeBool(s_.showNavPane));
    root.Set(L"showPreviewPane", json::Value::MakeBool(s_.showPreviewPane));
    root.Set(L"navPaneWidth", json::Value::MakeInt(s_.navPaneWidth));
    root.Set(L"aiPanelWidth", json::Value::MakeInt(s_.aiPanelWidth));
    root.Set(L"navPaneRoots", configio::MakeStringArray(s_.navPaneRoots));
    root.Set(L"dualPane", json::Value::MakeBool(s_.dualPane));
    root.Set(L"paneSplit", json::Value::MakeInt(s_.paneSplit));
    root.Set(L"paneVertical", json::Value::MakeBool(s_.paneVertical));
    root.Set(L"splitMarkThick", json::Value::MakeInt(s_.splitMarkThick));
    root.Set(L"aiEnabled", json::Value::MakeBool(s_.aiEnabled));
    root.Set(L"aiAllowDanger", json::Value::MakeBool(s_.aiAllowDanger));
    root.Set(L"aiTimeoutMs", json::Value::MakeInt(s_.aiTimeoutMs));
    root.Set(L"aiPumpIntervalMs", json::Value::MakeInt(s_.aiPumpIntervalMs));
    root.Set(L"aiPeekEnabled", json::Value::MakeBool(s_.aiPeekEnabled));
    root.Set(L"sessionSaveDelayMs", json::Value::MakeInt(s_.sessionSaveDelayMs));
    root.Set(L"auditEnabled", json::Value::MakeBool(s_.auditEnabled));
    root.Set(L"auditRotateKB", json::Value::MakeInt(s_.auditRotateKB));
    root.Set(L"auditArgsTextLimit", json::Value::MakeInt(s_.auditArgsTextLimit));
    root.Set(L"jobMachineTimeoutMs", json::Value::MakeInt(s_.jobMachineTimeoutMs));

    tf::EnsureDir(Dir());
    bool okCfg = true;
    if (!tf::WriteText(SettingsPath(), json::Stringify(root, 2))) {
        saveError_ = L"写入失败（数据目录可能只读）：" + SettingsPath();
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogDbg(L"[config] 写入失败（数据目录可能只读）：%s", SettingsPath().c_str());
        }
        okCfg = false;
    }
    // 状态四件套独立写：失败也如实回报（界面能提示"改了没存进去"）。
    if (!SaveStateFiles()) {
        if (saveError_.empty()) saveError_ = L"状态文件写入失败：" + StateDir();
        okCfg = false;
    }
    return okCfg;
}
