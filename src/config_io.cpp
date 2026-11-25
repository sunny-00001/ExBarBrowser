// settings.json 的**读写**：JSON 与 Settings 之间的单向映射，以及读盘时的夹取。
//
// 【为什么单独成文件】config.cpp 里混着两件事，改动节奏完全不同：
//   · 路径与写入口（Dir / SettingsPath / AddRecent / HotkeyOverride…）—— 被
//     "用户做了什么动作"驱动，改的是数据模型；
//   · settings.json 的读写（Load / Save）—— 被"文件格式"驱动，改的是**存盘契约**。
// 加一个设置项要动这里（两侧各一行）+ 表本体一处；而改路径规则只动 config.cpp。
// 混在一起时这个文件会涨过 500 行，读的人还得在"字段怎么夹取"和"热键怎么覆盖"
// 之间来回跳 —— 而两者的读者根本不同。
//
// 【键名口径】这里的键名与 settings.json 里的键名**逐字相同**，也与
// settings_reg 的键名相同：三处一致才能做到"手改文件 / 命令 / 界面"是同一套名字。
// 认不出的键静默忽略（向前兼容：新版本写的项，旧版本读进来只是不认识，不该报错）。
//
// 【读盘一律夹取】settings.json 是用户可改的文本，也可能是别的工具写坏的。
// 越界值不报错（那样会打不开程序），但必须夹到物理合理范围 —— 否则一个
// "w": 10000000000 会让窗口落到看不见的地方，且没有任何报错。
#include "config.h"

#include "common.h"         // LogDbg
#include "layout_limits.h"  // 两条侧栏宽度的上下限（与界面拖动、设置项范围同一份）
#include "search_scope.h"   // searchscope::kRangeCount（范围档位上界，唯一真源）
#include "settings_bounds.h"  // 整型设置的数值域（与 settings_reg 的写入范围同一份）
#include "str.h"            // str::Lower（与写入端同一套大小写归一）
#include "text_file.h"

#include <algorithm>

namespace {

// 钳到 [lo, hi]。用于"用户可改的数值字段"——超出物理合理范围的值不是错误，
// 但直接采信会把界面画到看不见的地方。
int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// 字符串数组：丢掉空串与异类项（这些项都是"一批路径/词"，空串没有语义）。
std::vector<std::wstring> ReadStringArray(const json::Value& root, const wchar_t* key) {
    std::vector<std::wstring> out;
    const json::Value* a = root.Find(key);
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr)
        if (v.IsString() && !v.str.empty()) out.push_back(v.str);
    return out;
}

// 保位读取：空串**保留**下来，只丢"不是字符串"的项。
//
// 【为什么需要两种读法】sessionPane2Paths 与 sessionPaths 是**按下标对齐**的：
// 空串表示"这个标签当时没有右窗格"，是占位符而不是"没有这条数据"。
// 用上面的 ReadStringArray 读它，空串会被丢掉 —— 于是后面的标签整体前移一格，
// 重启后右窗格路径挂到别的标签上（而这项写盘时是**保留**空串的，
// 也就是说存与读的口径本来就不一致，属于典型的"看起来能跑"的错位）。
std::vector<std::wstring> ReadStringArrayKeepEmpty(const json::Value& root, const wchar_t* key) {
    std::vector<std::wstring> out;
    const json::Value* a = root.Find(key);
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr)
        if (v.IsString()) out.push_back(v.str);
    return out;
}

json::Value MakeStringArray(const std::vector<std::wstring>& items) {
    json::Value a = json::Value::MakeArray();
    for (const auto& s : items) a.arr.push_back(json::Value::MakeStr(s));
    return a;
}

json::Value MakeIntArray(const std::vector<int>& items) {
    json::Value a = json::Value::MakeArray();
    for (int n : items) a.arr.push_back(json::Value::MakeInt(n));
    return a;
}

// 标签个性化：数组里每项是 {path, alias, color}。
// 只写"有内容"的项（alias 或 color 非空），不往配置里塞一堆空对象 ——
// 否则 settings.json 会随标签数线性膨胀，而绝大多数标签是默认态。
json::Value MakeTabMeta(const std::vector<TabMeta>& items) {
    json::Value a = json::Value::MakeArray();
    for (const auto& m : items) {
        if (m.path.empty()) continue;
        if (m.alias.empty() && m.color == 0) continue;
        json::Value o = json::Value::MakeObject();
        o.Set(L"path", json::Value::MakeStr(m.path));
        if (!m.alias.empty()) o.Set(L"alias", json::Value::MakeStr(m.alias));
        if (m.color != 0) o.Set(L"color", json::Value::MakeInt(m.color));
        a.arr.push_back(std::move(o));
    }
    return a;
}

std::vector<TabMeta> ReadTabMeta(const json::Value& root) {
    std::vector<TabMeta> out;
    const json::Value* a = root.Find(L"tabMeta");
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr) {
        if (!v.IsObject()) continue;
        TabMeta m;
        m.path = v.GetStr(L"path");
        m.alias = v.GetStr(L"alias");
        m.color = v.GetInt(L"color", 0);
        // 越界颜色夹回 0：配置被手改成 99 时不该让绘制走空色板。
        if (m.color < 0 || m.color >= core::kTabColorCount) m.color = 0;
        if (m.path.empty()) continue;
        out.push_back(std::move(m));
    }
    return out;
}

// 每标签历史读盘：坏项跳过而不是整份丢弃（旧配置没有这一段也要能读）。
std::vector<TabHistory> ReadTabHistories(const json::Value& root, const wchar_t* key) {
    std::vector<TabHistory> out;
    const json::Value* hist = root.Find(key);
    if (!hist || !hist->IsArray()) return out;
    for (const auto& e : hist->arr) {
        if (!e.IsObject()) continue;
        TabHistory h;
        if (const json::Value* ps = e.Find(L"paths"); ps && ps->IsArray())
            for (const auto& p : ps->arr)
                if (p.IsString()) h.paths.push_back(p.str);
        h.idx = e.GetInt(L"idx", 0);
        if (h.idx < 0 || h.idx >= static_cast<int>(h.paths.size())) h.idx = 0;
        out.push_back(std::move(h));
    }
    return out;
}

json::Value MakeTabHistories(const std::vector<TabHistory>& list) {
    json::Value a = json::Value::MakeArray();
    for (const auto& h : list) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"paths", MakeStringArray(h.paths));
        o.Set(L"idx", json::Value::MakeInt(h.idx));
        a.arr.push_back(std::move(o));
    }
    return a;
}

}  // namespace

void Config::Load() {
    if (loaded_) return;
    loaded_ = true;
    tf::EnsureDir(Dir());

    std::wstring text;
    if (!tf::ReadText(SettingsPath(), &text)) {
        // 读不到：文件不存在（首次运行，正常）或读不出（异常）。
        // 只有"存在但读不出"才需要禁止写盘。
        parseFailed_ = tf::Exists(SettingsPath());
        if (parseFailed_) LogDbg(L"[config] settings.json 存在但读不出，已禁止本次会话覆盖写");
        return;
    }
    json::Value root;
    std::wstring err;
    if (!json::Parse(text, root, &err) || !root.IsObject()) {
        // 【禁止后续 Save 覆盖】保留默认值继续运行（不能因为配置坏了就打不开程序），
        // 但绝不能把"只有默认值的 JSON"写回去 —— 那会抹掉用户的
        // recentPaths / sessionPaths / lockedPaths / closedTabs / hotkeyOverrides。
        // 原来的实现只写一行日志就 return，于是任何一次 Save（13 处调用点，
        // 包括"点一下显示/隐藏书签栏"）都会销毁用户的配置文件。
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
    s_.tabBarHeight = ClampInt(root.GetInt(L"tabBarHeight", s_.tabBarHeight),
                               kTabBarHeightMin, kTabBarHeightMax);
    s_.navBarHeight = ClampInt(root.GetInt(L"navBarHeight", s_.navBarHeight),
                               kNavBarHeightMin, kNavBarHeightMax);
    s_.bookmarkBarHeight = ClampInt(root.GetInt(L"bookmarkBarHeight", s_.bookmarkBarHeight),
                                    kBookmarkBarHeightMin, kBookmarkBarHeightMax);
    s_.statusBarHeight = ClampInt(root.GetInt(L"statusBarHeight", s_.statusBarHeight),
                                  kStatusBarHeightMin, kStatusBarHeightMax);
    s_.navBarSearchWidth = ClampInt(root.GetInt(L"navBarSearchWidth", s_.navBarSearchWidth),
                                    kNavBarSearchWidthMin, kNavBarSearchWidthMax);
    s_.navBarAddressMinWidth =
        ClampInt(root.GetInt(L"navBarAddressMinWidth", s_.navBarAddressMinWidth),
                 kNavBarAddressMinWidthMin, kNavBarAddressMinWidthMax);
    s_.bookmarkBarTooltipWidth =
        ClampInt(root.GetInt(L"bookmarkBarTooltipWidth", s_.bookmarkBarTooltipWidth),
                 kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax);
    s_.statusBarTooltipWidth =
        ClampInt(root.GetInt(L"statusBarTooltipWidth", s_.statusBarTooltipWidth),
                 kStatusBarTooltipWidthMin, kStatusBarTooltipWidthMax);
    s_.navPaneItemHeight = ClampInt(root.GetInt(L"navPaneItemHeight", s_.navPaneItemHeight),
                                    kNavPaneItemHeightMin, kNavPaneItemHeightMax);
    s_.navPaneIndent = ClampInt(root.GetInt(L"navPaneIndent", s_.navPaneIndent),
                                kNavPaneIndentMin, kNavPaneIndentMax);
    s_.settingsGroupWidth = ClampInt(root.GetInt(L"settingsGroupWidth", s_.settingsGroupWidth),
                                     kSettingsGroupWidthMin, kSettingsGroupWidthMax);
    s_.settingsDetailWidth = ClampInt(root.GetInt(L"settingsDetailWidth", s_.settingsDetailWidth),
                                      kSettingsDetailWidthMin, kSettingsDetailWidthMax);
    s_.settingsControlWidth =
        ClampInt(root.GetInt(L"settingsControlWidth", s_.settingsControlWidth),
                 kSettingsControlWidthMin, kSettingsControlWidthMax);

    s_.seqEnabled = root.GetBool(L"seqEnabled", s_.seqEnabled);
    s_.seqTimeoutMs = root.GetInt(L"seqTimeoutMs", s_.seqTimeoutMs);
    s_.seqShowHints = root.GetBool(L"seqShowHints", s_.seqShowHints);
    s_.seqSingleInstant = root.GetBool(L"seqSingleInstant", s_.seqSingleInstant);
    s_.seqUniqueAuto = root.GetBool(L"seqUniqueAuto", s_.seqUniqueAuto);
    s_.holdDelayMs = root.GetInt(L"holdDelayMs", s_.holdDelayMs);
    // 两个延时也是用户可改的：夹到注册表声明的同一区间。
    //
    // 【为什么必须用同一份上下限】这里原来写的是"holdDelayMs < 300 就当 800"，
    // 而 settings_reg 声明的是 200..2000 —— 于是 `config.set holdDelayMs 250`
    // 会被接受、写进 settings.json，**下次启动又悄悄变回 800**。同一个值在
    // "写"和"读"两条路上有两个定义，用户看到的是"设置自己变回去了"，
    // 没有任何检查会报。真正的"低于下限会让长按失效"由使用处兜
    // （input_router 取 max(holdDelayMs, 双击间隔+50)），读盘不该另立一条规则。
    s_.seqTimeoutMs = ClampInt(s_.seqTimeoutMs, kSeqTimeoutMsMin, kSeqTimeoutMsMax);
    s_.holdDelayMs = ClampInt(s_.holdDelayMs, kHoldDelayMsMin, kHoldDelayMsMax);
    // 序列浮层手感：与注册表同一对上下限（写/读两条路不许各自定义）。
    s_.seqWheelRows = ClampInt(root.GetInt(L"seqWheelRows", s_.seqWheelRows),
                               kSeqWheelRowsMin, kSeqWheelRowsMax);
    s_.seqPageRows = ClampInt(root.GetInt(L"seqPageRows", s_.seqPageRows),
                              kSeqPageRowsMin, kSeqPageRowsMax);
    // 通用弹层交互。
    s_.popupTypeAheadMs = ClampInt(root.GetInt(L"popupTypeAheadMs", s_.popupTypeAheadMs),
                                   kPopupTypeAheadMsMin, kPopupTypeAheadMsMax);
    s_.popupMaxRows = ClampInt(root.GetInt(L"popupMaxRows", s_.popupMaxRows),
                               kPopupMaxRowsMin, kPopupMaxRowsMax);

    s_.tabNewPos = root.GetStr(L"tabNewPos", s_.tabNewPos);
    // 旧值 "right" 归一成 "end"：历史上它被实现为"当前标签右侧"，而界面标签
    // 一直写着"最右" —— 一个取值对应两种行为，谁也说不清哪个是对的。
    // 现在 right 只是 end 的别名（读入与 config.set 两处接受，中间状态里不存在）。
    if (str::IEquals(s_.tabNewPos.c_str(), L"right")) s_.tabNewPos = L"end";
    s_.confirmCloseTabs = root.GetBool(L"confirmCloseTabs", s_.confirmCloseTabs);
    s_.tabMinWidth = root.GetInt(L"tabMinWidth", s_.tabMinWidth);
    s_.tabMaxWidth = root.GetInt(L"tabMaxWidth", s_.tabMaxWidth);
    // 标签宽度：两头的范围与注册表同一份；"上限不得低于下限"用共享谓词表达
    // （写入端的选择是**拒绝**，读盘端的选择是**修复** —— 策略不同，规则同一份）。
    s_.tabMinWidth = ClampInt(s_.tabMinWidth, kTabMinWidthMin, kTabMinWidthMax);
    s_.tabMaxWidth = ClampInt(s_.tabMaxWidth, kTabMaxWidthMin, kTabMaxWidthMax);
    if (!TabWidthsConsistent(s_.tabMinWidth, s_.tabMaxWidth)) s_.tabMaxWidth = s_.tabMinWidth;
    // 标签拖拽自动滚动周期：与注册表同一区间。
    s_.tabDragScrollMs = ClampInt(root.GetInt(L"tabDragScrollMs", s_.tabDragScrollMs),
                                  kTabDragScrollMsMin, kTabDragScrollMsMax);

    // 深浅色档位：先归一成规范形（小写）再判合法 —— 手抄 "Dark" 应该生效，
    // 而不是静默回落成 system（"主题没变"极难反推到配置里的大小写上）。
    // 写入端（settings_reg 的 themeModeSpec）用的是同一个 str::Lower：
    // 两处各写一套大小写规则，"写进去的是 dark、读回来判成非法"就会重演。
    // 真正认不出的值（blue / 空串）才回落到 system。
    s_.themeMode = str::Lower(root.GetStr(L"themeMode", s_.themeMode));
    if (s_.themeMode != L"dark" && s_.themeMode != L"light") s_.themeMode = L"system";

    // 历史上限：手改成 0/负数会让历史永远空着（"删掉的标签再也回不来"），
    // 过大则 settings.json 里堆几百条路径。两头都夹。
    s_.recentLimit =
        ClampInt(root.GetInt(L"recentLimit", s_.recentLimit), kRecentLimitMin, kRecentLimitMax);
    s_.searchLimit =
        ClampInt(root.GetInt(L"searchLimit", s_.searchLimit), kSearchLimitMin, kSearchLimitMax);
    s_.closedTabLimit = ClampInt(root.GetInt(L"closedTabLimit", s_.closedTabLimit),
                                 kClosedTabLimitMin, kClosedTabLimitMax);
    // 导航历史栈深度 + 搜索历史下拉行数：与注册表同一口径。
    s_.historyDepth =
        ClampInt(root.GetInt(L"historyDepth", s_.historyDepth), kHistoryDepthMin, kHistoryDepthMax);
    s_.searchHistoryMenuRows = ClampInt(root.GetInt(L"searchHistoryMenuRows",
                                                    s_.searchHistoryMenuRows),
                                        kSearchHistoryMenuRowsMin, kSearchHistoryMenuRowsMax);

    // 视图：越界值一律夹回合法范围，避免配置被手改成 99 后整块视图异常。
    s_.viewMode = ClampInt(root.GetInt(L"viewMode", s_.viewMode), kViewModeMin, kViewModeMax);
    s_.sortMode = ClampInt(root.GetInt(L"sortMode", s_.sortMode), kSortModeMin, kSortModeMax);
    s_.sortAscending = root.GetBool(L"sortAscending", s_.sortAscending);
    s_.groupBy = ClampInt(root.GetInt(L"groupBy", s_.groupBy), kGroupByMin, kGroupByMax);

    s_.showNavPane = root.GetBool(L"showNavPane", s_.showNavPane);
    s_.showPreviewPane = root.GetBool(L"showPreviewPane", s_.showPreviewPane);
    // 两条侧栏的宽度：夹在合理范围，避免手改成 5 或 5000 后整块布局失衡。
    // 上下限取 layout_limits.h（与界面拖动范围、设置项的可写范围同一份）。
    s_.navPaneWidth =
        ClampInt(root.GetInt(L"navPaneWidth", s_.navPaneWidth), kNavPaneWidthMin, kNavPaneWidthMax);
    s_.aiPanelWidth =
        ClampInt(root.GetInt(L"aiPanelWidth", s_.aiPanelWidth), kAiPanelWidthMin, kAiPanelWidthMax);
    s_.navPaneRoots = ReadStringArray(root, L"navPaneRoots");
    s_.dualPane = root.GetBool(L"dualPane", s_.dualPane);
    s_.paneSplit =
        ClampInt(root.GetInt(L"paneSplit", s_.paneSplit), kPaneSplitMin, kPaneSplitMax);
    s_.paneVertical = root.GetBool(L"paneVertical", s_.paneVertical);

    // AI 通道：默认开启（缺省即监听），配置里显式写 false 才关。
    // 运行中可经 config.set 热插拔，见 App::SyncAiChannel。
    s_.aiEnabled = root.GetBool(L"aiEnabled", s_.aiEnabled);
    s_.aiAllowDanger = root.GetBool(L"aiAllowDanger", s_.aiAllowDanger);
    // 超时下限 1s：小于它的值会让所有命令都在"还没跑完"时被判超时，
    // 而那种失败看起来像"命令坏了"。上限 600s 足够扫完一个巨大的目录树。
    s_.aiTimeoutMs =
        ClampInt(root.GetInt(L"aiTimeoutMs", s_.aiTimeoutMs), kAiTimeoutMsMin, kAiTimeoutMsMax);
    s_.aiPumpIntervalMs = ClampInt(root.GetInt(L"aiPumpIntervalMs", s_.aiPumpIntervalMs),
                                   kAiPumpIntervalMsMin, kAiPumpIntervalMsMax);

    // 会话快照去抖 + 审计/作业策略：与注册表同一对上下限。
    s_.sessionSaveDelayMs = ClampInt(root.GetInt(L"sessionSaveDelayMs", s_.sessionSaveDelayMs),
                                     kSessionSaveDelayMsMin, kSessionSaveDelayMsMax);
    s_.auditRotateKB = ClampInt(root.GetInt(L"auditRotateKB", s_.auditRotateKB),
                                kAuditRotateKBMin, kAuditRotateKBMax);
    s_.auditArgsTextLimit = ClampInt(root.GetInt(L"auditArgsTextLimit", s_.auditArgsTextLimit),
                                     kAuditArgsTextLimitMin, kAuditArgsTextLimitMax);
    s_.jobMachineTimeoutMs =
        ClampInt(root.GetInt(L"jobMachineTimeoutMs", s_.jobMachineTimeoutMs),
                 kJobMachineTimeoutMsMin, kJobMachineTimeoutMsMax);

    const json::Value* geo = root.Find(L"window");
    if (geo && geo->IsObject()) {
        s_.hasGeometry = geo->GetBool(L"valid", false);
        // 【必须钳制】配置是用户可改的文本，也可能是别的工具写坏的。
        // 手写成 "w": 10000000000 时，app.cpp 的 `work.right - w` 会先有符号溢出，
        // CreateWindowEx 收到负宽高 → 窗口看不见且没有任何报错。
        // GetInt 已经挡住越界到 int 之外的值（返回默认），这里再挡"合法但荒谬"的范围。
        s_.winW = ClampInt(geo->GetInt(L"w", s_.winW), 240, 20000);
        s_.winH = ClampInt(geo->GetInt(L"h", s_.winH), 160, 20000);
        s_.winX = ClampInt(geo->GetInt(L"x", s_.winX), -32768, 32767);
        s_.winY = ClampInt(geo->GetInt(L"y", s_.winY), -32768, 32767);
        s_.maximized = geo->GetBool(L"max", false);
    }

    s_.recentPaths = ReadStringArray(root, L"recentPaths");
    s_.sessionPaths = ReadStringArray(root, L"sessionPaths");
    s_.sessionActive = root.GetInt(L"sessionActive", 0);
    s_.sessionSelection = ReadStringArray(root, L"sessionSelection");
    s_.lockedPaths = ReadStringArray(root, L"lockedPaths");
    s_.pinnedPaths = ReadStringArray(root, L"pinnedPaths");
    // 每标签历史：旧配置没有这一段（向后兼容），坏项跳过而不是整份丢弃。
    s_.sessionHistory = ReadTabHistories(root, L"sessionHistory");
    // 双栏右窗格路径（与 sessionPaths 对齐，空串 = 该标签没有右窗格）。
    // 【必须保位读】空串是占位符，用会丢空串的读法会让后面的标签整体前移一格。
    s_.sessionPane2Paths = ReadStringArrayKeepEmpty(root, L"sessionPane2Paths");
    // 右窗格历史只记录非空路径的标签，顺序与 sessionPane2Paths 中非空项一致。
    s_.sessionPane2History = ReadTabHistories(root, L"sessionPane2History");
    // 命名会话：同样向后兼容，坏项/无名项跳过。
    s_.namedSessions.clear();
    if (const json::Value* ns = root.Find(L"namedSessions"); ns && ns->IsArray()) {
        for (const auto& e : ns->arr) {
            if (!e.IsObject()) continue;
            NamedSession sess;
            sess.name = e.GetStr(L"name");
            if (sess.name.empty()) continue;
            if (const json::Value* ps = e.Find(L"paths"); ps && ps->IsArray())
                for (const auto& p : ps->arr)
                    if (p.IsString()) sess.paths.push_back(p.str);
            s_.namedSessions.push_back(std::move(sess));
        }
    }
    s_.tabMeta = ReadTabMeta(root);
    s_.closedTabs = ReadStringArray(root, L"closedTabs");

    // 搜索域：范围档位 + 自定义路径 + 历史。
    // 范围值必须钳制（配置是用户可改的文本）：认不出的档位回落到默认，
    // 而不是变成"搜不到东西"的怪状态。
    s_.searchRange = ClampInt(root.GetInt(L"searchRange", s_.searchRange), 0,
                              searchscope::kRangeCount - 1);
    s_.searchRangePath = root.GetStr(L"searchRangePath");
    s_.searchHistory = ReadStringArray(root, L"searchHistory");
    // 上限跟着 searchLimit 走（与 AddSearchHistory 一致）：配置里手改出 50 条
    // 而设置里只保留 20 条时，下拉里能看到的和实际存的就不一样了。
    if (s_.searchHistory.size() > static_cast<size_t>(s_.searchLimit))
        s_.searchHistory.resize(static_cast<size_t>(s_.searchLimit));

    if (const json::Value* segs = root.Find(L"statusSegs"); segs && segs->IsArray()) {
        s_.statusSegs.clear();
        for (const auto& v : segs->arr)
            if (v.type == json::Value::kNumber) s_.statusSegs.push_back(static_cast<int>(v.num));
        // 旧版本 append 式切换会写出乱序/重复段号；状态栏右对齐依赖稳定升序。
        // 归一规则只有一份（config.h 的 NormalizeStatusSegs），写入端存的就是
        // 规范形 —— 这里再夹一次是为了手改过的文件，不是另一条规则。
        NormalizeStatusSegs(s_.statusSegs);
    }

    if (const json::Value* hk = root.Find(L"hotkeys"); hk && hk->IsObject()) {
        s_.hotkeyOverrides.clear();
        for (const auto& kv : hk->obj)
            if (kv.second.IsString()) s_.hotkeyOverrides.emplace_back(kv.first, kv.second.str);
    }
}

bool Config::Save() {
    saveError_.clear();   // 每次写盘都重新判定：上一次的失败不该永远跟着
    // 加载时解析失败过就别写 —— 内存里只有默认值，写回去等于销毁用户配置。
    // 这条与 BookmarkStore::Save 的同名护栏是同一个道理：宁可不保存。
    if (parseFailed_) {
        saveError_ = L"settings.json 上次解析失败，为免覆盖已拒绝写盘（请先修好该文件）";
        LogDbg(L"[config] 拒绝写盘：settings.json 上次解析失败（覆盖会丢失最近访问/会话/键位）");
        return false;
    }
    json::Value root = json::Value::MakeObject();
    root.Set(L"version", json::Value::MakeInt(1));
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
    root.Set(L"tabNewPos", json::Value::MakeStr(s_.tabNewPos));
    root.Set(L"confirmCloseTabs", json::Value::MakeBool(s_.confirmCloseTabs));
    root.Set(L"tabMinWidth", json::Value::MakeInt(s_.tabMinWidth));
    root.Set(L"tabMaxWidth", json::Value::MakeInt(s_.tabMaxWidth));
    root.Set(L"tabDragScrollMs", json::Value::MakeInt(s_.tabDragScrollMs));
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
    root.Set(L"navPaneRoots", MakeStringArray(s_.navPaneRoots));
    root.Set(L"dualPane", json::Value::MakeBool(s_.dualPane));
    root.Set(L"paneSplit", json::Value::MakeInt(s_.paneSplit));
    root.Set(L"paneVertical", json::Value::MakeBool(s_.paneVertical));
    root.Set(L"aiEnabled", json::Value::MakeBool(s_.aiEnabled));
    root.Set(L"aiAllowDanger", json::Value::MakeBool(s_.aiAllowDanger));
    root.Set(L"aiTimeoutMs", json::Value::MakeInt(s_.aiTimeoutMs));
    root.Set(L"aiPumpIntervalMs", json::Value::MakeInt(s_.aiPumpIntervalMs));
    root.Set(L"sessionSaveDelayMs", json::Value::MakeInt(s_.sessionSaveDelayMs));
    root.Set(L"auditRotateKB", json::Value::MakeInt(s_.auditRotateKB));
    root.Set(L"auditArgsTextLimit", json::Value::MakeInt(s_.auditArgsTextLimit));
    root.Set(L"jobMachineTimeoutMs", json::Value::MakeInt(s_.jobMachineTimeoutMs));

    json::Value geo = json::Value::MakeObject();
    geo.Set(L"valid", json::Value::MakeBool(s_.hasGeometry));
    geo.Set(L"x", json::Value::MakeInt(s_.winX));
    geo.Set(L"y", json::Value::MakeInt(s_.winY));
    geo.Set(L"w", json::Value::MakeInt(s_.winW));
    geo.Set(L"h", json::Value::MakeInt(s_.winH));
    geo.Set(L"max", json::Value::MakeBool(s_.maximized));
    root.Set(L"window", geo);

    root.Set(L"recentPaths", MakeStringArray(s_.recentPaths));
    root.Set(L"sessionPaths", MakeStringArray(s_.sessionPaths));
    root.Set(L"sessionActive", json::Value::MakeInt(s_.sessionActive));
    root.Set(L"sessionSelection", MakeStringArray(s_.sessionSelection));
    root.Set(L"lockedPaths", MakeStringArray(s_.lockedPaths));
    root.Set(L"pinnedPaths", MakeStringArray(s_.pinnedPaths));
    root.Set(L"sessionHistory", MakeTabHistories(s_.sessionHistory));
    root.Set(L"sessionPane2Paths", MakeStringArray(s_.sessionPane2Paths));
    root.Set(L"sessionPane2History", MakeTabHistories(s_.sessionPane2History));
    {
        json::Value ns = json::Value::MakeArray();
        for (const auto& sess : s_.namedSessions) {
            json::Value o = json::Value::MakeObject();
            o.Set(L"name", json::Value::MakeStr(sess.name));
            o.Set(L"paths", MakeStringArray(sess.paths));
            ns.arr.push_back(std::move(o));
        }
        root.Set(L"namedSessions", ns);
    }
    root.Set(L"tabMeta", MakeTabMeta(s_.tabMeta));
    root.Set(L"closedTabs", MakeStringArray(s_.closedTabs));
    root.Set(L"statusSegs", MakeIntArray(s_.statusSegs));

    // 搜索域：范围档位 + 自定义路径 + 历史（最近的在前）。
    root.Set(L"searchRange", json::Value::MakeInt(s_.searchRange));
    root.Set(L"searchRangePath", json::Value::MakeStr(s_.searchRangePath));
    root.Set(L"searchHistory", MakeStringArray(s_.searchHistory));

    json::Value hk = json::Value::MakeObject();
    for (const auto& kv : s_.hotkeyOverrides)
        hk.Set(kv.first, json::Value::MakeStr(kv.second));
    root.Set(L"hotkeys", hk);

    tf::EnsureDir(Dir());
    if (!tf::WriteText(SettingsPath(), json::Stringify(root, 2))) {
        // 便携模式的代价：exe 放在只读目录时写不进去。
        // 原因存下来给界面读（见 Save 的说明）；日志只记一次，避免刷屏。
        saveError_ = L"写入失败（数据目录可能只读）：" + SettingsPath();
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogDbg(L"[config] 写入失败（数据目录可能只读）：%s", SettingsPath().c_str());
        }
        return false;
    }
    return true;
}
