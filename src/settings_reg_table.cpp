// settings_reg 的**表本体**：有哪些设置项、各项的形状与归属分组。
//
// 接口与校验在 settings_reg.cpp（"这一项能不能读写"），写入构造器在
// settings_reg_valid.cpp（"一个值能不能进这个字段"）。这里是第三件事：
// **有哪些项**。三件事分开，是因为它们的改动节奏不同 ——
// 加一项要动这里；改校验口径只动 valid；改接口只动 .cpp。
//
// 【聚合初始化的字段顺序】下面每条 Entry 都按 settings_reg.h 里 Entry 的
// 声明顺序给值：key / kind / get / set / doc / group / options / min / max。
// 顺序错位不会编译报错，只会让 group 变成空串、范围变成 0 —— 那种错很贵，
// 所以顺序由表头的这条注释钉住；新增项照抄同组的一条即可。
//
// 【读值 lambda 收 Settings 引用】get 不去读全局配置，而是收一份 Settings
// 引用：同一份映射既能读运行值，也能读**默认值**（一份默认构造的 Settings），
// 于是"恢复默认值"不需要第二张表 —— 两张表迟早分叉，而分叉点只有用户能发现。
#include "settings_reg.h"

#include "config.h"
#include "path_util.h"            // pathutil::ToShellPath（自定义搜索范围的路径归一）
#include "search_scope.h"         // searchscope::RangeLabel / kRangeCount（范围档位唯一来源）
#include "settings_bounds.h"      // 整型设置的数值域（与 config_io 的读盘夹取同一份）
#include "settings_reg_table.h"
#include "settings_reg_table_readonly.h"   // detail::AppendReadonlyEntries（只读三组）
#include "settings_reg_valid.h"   // BoolSet / IntSet / StrEnumSet / *Value
#include "status_segs.h"          // Seg_Count：statusSegs 的取值上界（纯枚举，不拖 UI 依赖）
#include "theme.h"                // thememode::kSystem / kDark / kLight（档位标识唯一来源）
#include "view_modes.h"           // viewmode::kModes：视图档位标签的唯一来源

// 【编译期钉住】settings_bounds.h 里的档位上下界是裸数字（它不能包含
// view_modes.h —— 那会把 shobjidl.h 拖进每个包含者）。裸数字与真实档数之间
// 一旦漂开，漂移是**静默**的：多一档视图时 viewMode 最多只能设到 7，第 8 档
// 永远设不到，而没有任何检查会报。static_assert 把这条关系搬到编译期。
static_assert(kViewModeMax == viewmode::kCount - 1,
              "settings_bounds::kViewModeMax 必须等于 viewmode::kCount - 1");
static_assert(kSortModeMax == viewmode::kSortGroupCount - 1,
              "settings_bounds::kSortModeMax 必须等于 viewmode::kSortGroupCount - 1");
static_assert(kGroupByMax == viewmode::kSortGroupCount - 1,
              "settings_bounds::kGroupByMax 必须等于 viewmode::kSortGroupCount - 1");

namespace settings_reg {
namespace {

// doc 里引用数值域时用这个拼，而不是手写"（1..200）"。
//
// 【为什么值得一个函数】doc 是**给 AI 的决策依据**（tools/list 的说明），
// 手写的范围注释在有人改过 limits 之后不会跟着变 —— 于是出现"说明说 20..80、
// 写入其实接受 20..100"，AI 据此选了 90，报错原因与说明互相矛盾，它只能靠猜。
// 让这段文字由同一对常量生成，就不可能对不上。
//
// 只产出 "lo..hi" 这半截，圆括号与前后文由调用方写：不同项的 doc 句式不同
// （「（像素，120..600）：」/「（1..100）：」），把括号也定死反而逼着各处去
// 拆字符串。
std::wstring RangeText(int lo, int hi) {
    return std::to_wstring(lo) + L".." + std::to_wstring(hi);
}

// 整型枚举项：值 = 下标（与配置里存的数字一致）。
// 标签来自 view_modes.h —— 那是视图维度表的唯一来源。
std::vector<Option> IntOptions(const viewmode::ViewMode* modes, int count) {
    std::vector<Option> out;
    for (int i = 0; i < count; ++i) {
        Option o;
        o.label = modes[i].label;
        o.value = json::Value::MakeInt(i);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<Option> IntOptions(const wchar_t* const* labels, int count) {
    std::vector<Option> out;
    for (int i = 0; i < count; ++i) {
        Option o;
        o.label = labels[i];
        o.value = json::Value::MakeInt(i);
        out.push_back(std::move(o));
    }
    return out;
}

// 布尔枚举项：界面用下拉表达、落盘仍是 true/false。
//
// 【为什么不把 dualPane 改成 Int】settings.json 的 "dualPane": true 是既有契约
// （跨版本、跨 AI、脚本都在读它），改成 0/1 会让旧配置与旧客户端读到错的东西。
// 界面表达（下拉）与数据形状（bool）本来就是两件事 —— 靠 options 把它们解耦，
// 不需要为"换个控件"动落盘格式。
std::vector<Option> MakeBoolOptions() {
    std::vector<Option> out;
    {
        Option o;
        o.label = L"单栏";
        o.value = json::Value::MakeBool(false);
        out.push_back(std::move(o));
    }
    {
        Option o;
        o.label = L"双栏（左右/上下两个窗格）";
        o.value = json::Value::MakeBool(true);
        out.push_back(std::move(o));
    }
    return out;
}

// 字符串枚举项：值 = 稳定标识（落盘/命令用），label = 中文显示名。
std::vector<Option> MakeStrOptions(std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> pairs) {
    std::vector<Option> out;
    for (const auto& p : pairs) {
        Option o;
        o.label = p.second;
        o.value = json::Value::MakeStr(p.first);
        out.push_back(std::move(o));
    }
    return out;
}

// 枚举项里的取值清单（给 StrEnumSet 的合法值集合用）。
//
// 【为什么从同一张表取】合法值与显示名分开写两份时，改了一处忘了另一处，
// 表现是"下拉里有的选项写进去就被拒"。这里让它们同源。
std::vector<std::wstring> OptionValues(const std::vector<Option>& options) {
    std::vector<std::wstring> out;
    for (const auto& o : options)
        if (o.value.IsString()) out.push_back(o.value.str);
    return out;
}

}  // namespace

namespace {

// ---------------------------------------------------------------- 表本体
// 排序即文档顺序即界面顺序：**与左栏分组表逐段对齐**（外观 → 四条栏 → 视图 →
// 窗格 → 搜索 → 按键 → 弹层 → AI → 运维 → 设置窗 → 只读三组），组内按"最可能
// 被调的在前"。四条自绘栏（标签栏 / 导航栏 / 书签栏 / 状态栏）各自成段，顺序
// 按它们在窗口里自上而下的位置 —— 以前四栏的显隐与尺寸全塞在"栏显隐"一组里，
// 改一条栏的设置得先在四栏之间猜它在哪儿。
const std::vector<Entry>& Table() {
    static const std::vector<Entry> t = [] {
        std::vector<Entry> v;
        Config& cfg = Config::Get();
        Settings& m = cfg.mutableSettings();

        // ---- 外观 ----
        // 深浅色档位：以前恒跟随系统（theme.cpp 直接读注册表），于是"晚上想用
        // 深色"必须把整个 Windows 一起改。强制档是文件管理器的标配。
        // 取值与显示名同表（OptionValues 从同一份 options 取），改一处不会分叉。
        const std::vector<Option> themeModeOptions =
            MakeStrOptions({{thememode::kSystem, L"跟随系统"}, {thememode::kDark, L"深色"},
                            {thememode::kLight, L"浅色"}});
        // 大小写不是语义的一部分：存 "DARK" 会让读盘那侧（只认小写的三个标识）
        // 把它当成非法值回落到 system —— 规范形（小写）由 norm 一次定义。
        const settings_reg::StrSpec themeModeSpec{
            L"themeMode", OptionValues(themeModeOptions),
            [](std::wstring s) { return str::Lower(std::move(s)); }};
        v.push_back({L"themeMode", Kind::Str,
                     [](const Settings& s) { return json::Value::MakeStr(s.themeMode); },
                     themeModeSpec.SetterFor(&m.themeMode),
                     L"深浅色：system 跟随系统 / dark 强制深色 / light 强制浅色（立即生效）",
                     L"appearance", themeModeOptions, 0, 0, themeModeSpec.CanonFor()});

        // ---- 标签栏 ----
        v.push_back({L"tab", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.showTab); },
                     BoolSet(&m.showTab, L"tab"),
                     L"显示标签栏：关闭后整条隐藏，仍可用快捷键切换标签", L"bar_tab"});
        // 单标签时隐藏：只在"标签数恰好为 1"时生效，与上面的 tab 开关独立
        // （tab=false 是永久隐藏，这项是"没必要时不占地方"）。
        v.push_back({L"hideTabBarWhenSingle", Kind::Bool,
                     [](const Settings& s) {
                         return json::Value::MakeBool(s.hideTabBarWhenSingle);
                     },
                     BoolSet(&m.hideTabBarWhenSingle, L"hideTabBarWhenSingle"),
                     L"只有一个标签时隐藏标签栏：多开第二个标签立刻出现"
                     L"（立即重排整窗）",
                     L"bar_tab"});
        // 这些值以前散在 Theme 的 static constexpr、nav_metrics.h 与各栏的
        // S(600)/S(520) 里；现在都走注册表：可写、可夹、可脚本读、界面可见。
        v.push_back({L"tabBarHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.tabBarHeight); },
                     IntSet(&m.tabBarHeight, L"tabBarHeight", kTabBarHeightMin,
                            kTabBarHeightMax),
                     L"标签栏高度（逻辑像素，" + RangeText(kTabBarHeightMin, kTabBarHeightMax) +
                         L"）：立即重排整窗",
                     L"bar_tab", {}, kTabBarHeightMin, kTabBarHeightMax});
        v.push_back({L"tabMinWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.tabMinWidth); },
                     IntCheckedSet(
                         &m.tabMinWidth, L"tabMinWidth", kTabMinWidthMin, kTabMinWidthMax,
                         // 拍下**另一个字段的地址**而不是值：判据必须在
                         // 调用的这一刻读当前下限，否则改完 min 再改 max
                         // 会拿着一份旧的上下限去比。
                         [other = &m.tabMaxWidth](int v) {
                             return TabWidthsConsistent(v, *other);
                         },
                         L"不得高于 tabMaxWidth（先把 tabMaxWidth 调大）"),
                     L"标签最小宽度（逻辑像素，" + RangeText(kTabMinWidthMin, kTabMinWidthMax) +
                         L"）：紧凑可设 80",
                     L"bar_tab", {}, kTabMinWidthMin, kTabMinWidthMax});
        v.push_back({L"tabMaxWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.tabMaxWidth); },
                     IntCheckedSet(
                         &m.tabMaxWidth, L"tabMaxWidth", kTabMaxWidthMin, kTabMaxWidthMax,
                         [other = &m.tabMinWidth](int v) {
                             return TabWidthsConsistent(*other, v);
                         },
                         L"不得低于 tabMinWidth（先把 tabMinWidth 调小）"),
                     L"标签最大宽度（逻辑像素，" + RangeText(kTabMaxWidthMin, kTabMaxWidthMax) +
                         L"）：长标题在上限处省略；不得低于 tabMinWidth",
                     L"bar_tab", {}, kTabMaxWidthMin, kTabMaxWidthMax});
        // 新标签位置的取值与显示名是**同一张表**：落盘用 "after_current"（跨软件
        // 契约），界面显示"当前右侧"。两张表分叉时，界面显示的下拉项会写不进配置。
        //
        // 【为什么只有两项】早先有三个：right「最右」/ after_current / end「末尾」。
        // 而 right 与 after_current 的实现是同一行代码（ActiveIndex()+1），
        // 也就是说"最右"从来没生效过 —— 标签在撒谎。现在一个取值只对应一种行为：
        //   after_current = 插到当前标签右边；end = 追加到最右。
        // 老配置里的 "right" 由 Load 与 StrSpec 一起归一成 end（标签本来就说
        // 它是"最右"），于是老用户得到的是标签一直承诺的那个行为。
        //
        // 【规范形只写一次】StrSpec 同时产出 setter 与 canon：两处各写一份，
        // "存的"与"回读期望的"就会各有定义，而分叉的表现恰好是
        // "config.set 每次都被判成写入后回读不一致"。
        const settings_reg::StrSpec tabNewPosSpec{
            L"tabNewPos",
            {L"after_current", L"end"},
            [](std::wstring s) { return str::IEquals(s.c_str(), L"right") ? std::wstring(L"end") : s; }};
        const std::vector<Option> tabNewPosOptions =
            MakeStrOptions({{L"after_current", L"当前右侧"}, {L"end", L"最右"}});
        v.push_back({L"tabNewPos", Kind::Str,
                     [](const Settings& s) { return json::Value::MakeStr(s.tabNewPos); },
                     tabNewPosSpec.SetterFor(&m.tabNewPos),
                     L"新标签位置：after_current 当前右侧 / end 最右（旧值 right 等价于 end）",
                     L"bar_tab", tabNewPosOptions, 0, 0, tabNewPosSpec.CanonFor()});
        v.push_back({L"confirmCloseTabs", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.confirmCloseTabs); },
                     BoolSet(&m.confirmCloseTabs, L"confirmCloseTabs"),
                     L"关闭窗口且多于一个标签时先确认：最后一个标签关闭（= 退出程序）不弹", L"bar_tab"});
        v.push_back({L"tabDragScrollMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.tabDragScrollMs); },
                     IntSet(&m.tabDragScrollMs, L"tabDragScrollMs", kTabDragScrollMsMin,
                            kTabDragScrollMsMax),
                     L"标签拖拽滚动周期（ms，" + RangeText(kTabDragScrollMsMin,
                                                      kTabDragScrollMsMax) +
                         L"）：拖到条边缘时自动滚动的步进间隔",
                     L"bar_tab", {}, kTabDragScrollMsMin, kTabDragScrollMsMax});
        v.push_back({L"closedTabLimit", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.closedTabLimit); },
                     IntSet(&m.closedTabLimit, L"closedTabLimit", kClosedTabLimitMin,
                            kClosedTabLimitMax),
                     L"关闭标签历史上限（" + RangeText(kClosedTabLimitMin, kClosedTabLimitMax) +
                         L"）：Ctrl+Shift+T 能连续回退几步",
                     L"bar_tab", {}, kClosedTabLimitMin, kClosedTabLimitMax});
        // ---- 标签栏动效与拖出成窗（P0 交互做成设置：偏好而非正确性，须可关）----
        v.push_back({L"tabAnimMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.tabAnimMs); },
                     IntSet(&m.tabAnimMs, L"tabAnimMs", kTabAnimMsMin, kTabAnimMsMax),
                     L"让位动画时长（ms，" + RangeText(kTabAnimMsMin, kTabAnimMsMax) +
                         L"）：0 = 关闭动画；下一次拖拽生效",
                     L"bar_tab", {}, kTabAnimMsMin, kTabAnimMsMax});
        v.push_back({L"tabDragGhost", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.tabDragGhost); },
                     BoolSet(&m.tabDragGhost, L"tabDragGhost"),
                     L"拖动标签时画跟随光标的浮层（原位置留空）：关掉则只剩插入线",
                     L"bar_tab"});
        v.push_back({L"tabTearOffDy", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.tabTearOffDy); },
                     IntSet(&m.tabTearOffDy, L"tabTearOffDy", kTabTearOffDyMin,
                            kTabTearOffDyMax),
                     L"拖出成窗磁吸阈值（逻辑像素，" +
                         RangeText(kTabTearOffDyMin, kTabTearOffDyMax) +
                         L"）：0 = 关闭拖出成窗（右键菜单仍可）",
                     L"bar_tab", {}, kTabTearOffDyMin, kTabTearOffDyMax});

        // ---- 导航栏 ----
        v.push_back({L"nav", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.showNav); },
                     BoolSet(&m.showNav, L"nav"),
                     L"显示导航栏：含地址框、后退/前进、搜索框", L"bar_nav"});
        v.push_back({L"navBarHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.navBarHeight); },
                     IntSet(&m.navBarHeight, L"navBarHeight", kNavBarHeightMin, kNavBarHeightMax),
                     L"导航栏高度（逻辑像素，" + RangeText(kNavBarHeightMin, kNavBarHeightMax) +
                         L"）：立即重排整窗",
                     L"bar_nav", {}, kNavBarHeightMin, kNavBarHeightMax});
        v.push_back({L"navBarSearchWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.navBarSearchWidth); },
                     IntSet(&m.navBarSearchWidth, L"navBarSearchWidth", kNavBarSearchWidthMin,
                            kNavBarSearchWidthMax),
                     L"导航栏搜索框宽度（逻辑像素，" +
                         RangeText(kNavBarSearchWidthMin, kNavBarSearchWidthMax) +
                         L"）：窗口太窄时仍按地址框最小宽整体收起",
                     L"bar_nav", {}, kNavBarSearchWidthMin, kNavBarSearchWidthMax});
        v.push_back({L"navBarAddressMinWidth", Kind::Int,
                     [](const Settings& s) {
                         return json::Value::MakeInt(s.navBarAddressMinWidth);
                     },
                     IntSet(&m.navBarAddressMinWidth, L"navBarAddressMinWidth",
                            kNavBarAddressMinWidthMin, kNavBarAddressMinWidthMax),
                     L"导航栏地址框最小宽（逻辑像素，" +
                         RangeText(kNavBarAddressMinWidthMin, kNavBarAddressMinWidthMax) +
                         L"）：低于此宽就收起搜索框",
                     L"bar_nav", {}, kNavBarAddressMinWidthMin, kNavBarAddressMinWidthMax});
        // 最近访问上限：以前是代码里的裸数字（config.cpp 的 30），它直接决定
        // "下拉里能看到多少条"，写死在代码里就只能改代码重编译。同类的另外两个
        // 上限各有归属：搜索历史在"搜索"段、已关标签在"标签栏"段。
        v.push_back({L"recentLimit", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.recentLimit); },
                     IntSet(&m.recentLimit, L"recentLimit", kRecentLimitMin, kRecentLimitMax),
                     L"最近访问上限（" + RangeText(kRecentLimitMin, kRecentLimitMax) +
                         L"）：地址栏下拉里保留多少条最近目录",
                     L"bar_nav", {}, kRecentLimitMin, kRecentLimitMax});
        // 导航栈深度：决定"后退"最多退几步。以前写死在 nav_history.h 的
        // kDefaultMaxEntries=512，与 closedTabLimit 是两回事（一个是撤几步、一个是重开几个已关标签）。
        // 立即生效：ApplyHistoryLimits 会把深度推给每个窗格的 History 并裁剪。
        v.push_back({L"historyDepth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.historyDepth); },
                     IntSet(&m.historyDepth, L"historyDepth", kHistoryDepthMin, kHistoryDepthMax),
                     L"导航历史深度（" + RangeText(kHistoryDepthMin, kHistoryDepthMax) +
                         L"）：后退/前进最多几步；立即裁剪已有栈",
                     L"bar_nav", {}, kHistoryDepthMin, kHistoryDepthMax});

        // ---- 书签栏 ----
        v.push_back({L"bookmark", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.showBookmark); },
                     BoolSet(&m.showBookmark, L"bookmark"),
                     L"显示书签栏：与 bmk.show / bmk.hide 命令是同一条状态", L"bar_bookmark"});
        v.push_back({L"bookmarkBarHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.bookmarkBarHeight); },
                     IntSet(&m.bookmarkBarHeight, L"bookmarkBarHeight", kBookmarkBarHeightMin,
                            kBookmarkBarHeightMax),
                     L"书签栏高度（逻辑像素，" +
                         RangeText(kBookmarkBarHeightMin, kBookmarkBarHeightMax) +
                         L"）：立即重排整窗",
                     L"bar_bookmark", {}, kBookmarkBarHeightMin, kBookmarkBarHeightMax});
        v.push_back({L"bookmarkBarTooltipWidth", Kind::Int,
                     [](const Settings& s) {
                         return json::Value::MakeInt(s.bookmarkBarTooltipWidth);
                     },
                     IntSet(&m.bookmarkBarTooltipWidth, L"bookmarkBarTooltipWidth",
                            kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax),
                     L"书签提示窗最大宽（逻辑像素，" +
                         RangeText(kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax) +
                         L"）：书签标题被截断时的悬浮提示最大宽度",
                     L"bar_bookmark", {}, kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax});
        v.push_back({L"bookmarkWheelStepDp", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.bookmarkWheelStepDp); },
                     IntSet(&m.bookmarkWheelStepDp, L"bookmarkWheelStepDp",
                            kBmkWheelStepDpMin, kBmkWheelStepDpMax),
                     L"书签栏滚轮步进（逻辑像素，" + RangeText(kBmkWheelStepDpMin,
                                                        kBmkWheelStepDpMax) +
                         L"）：滚一格横向挪多远；48 约三个字宽（立即生效）",
                     L"bar_bookmark", {}, kBmkWheelStepDpMin, kBmkWheelStepDpMax});

        // ---- 状态栏 ----
        v.push_back({L"statusbar", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.showStatus); },
                     BoolSet(&m.showStatus, L"statusbar"), L"显示状态栏：底部信息条", L"bar_status"});
        v.push_back({L"statusBarHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.statusBarHeight); },
                     IntSet(&m.statusBarHeight, L"statusBarHeight", kStatusBarHeightMin,
                            kStatusBarHeightMax),
                     L"状态栏高度（逻辑像素，" + RangeText(kStatusBarHeightMin, kStatusBarHeightMax) +
                         L"）：立即重排整窗",
                     L"bar_status", {}, kStatusBarHeightMin, kStatusBarHeightMax});
        v.push_back({L"statusBarTooltipWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.statusBarTooltipWidth); },
                     IntSet(&m.statusBarTooltipWidth, L"statusBarTooltipWidth",
                            kStatusBarTooltipWidthMin, kStatusBarTooltipWidthMax),
                     L"状态栏提示窗最大宽（逻辑像素，" +
                         RangeText(kStatusBarTooltipWidthMin, kStatusBarTooltipWidthMax) +
                         L"）：状态栏按钮与计数提示的最大宽度",
                     L"bar_status", {}, kStatusBarTooltipWidthMin, kStatusBarTooltipWidthMax});
        // 段显隐的正确入口是 config.set statusSegs（状态栏右键菜单转投它）。
        // 过去这里只读，理由是"直接写配置不会重建状态栏"——那个理由已经被
        // config.set 的刷新钩子解决（见 app_commands_misc.cpp 的 ConfigSet）。
        // IntArraySet 逐元素校验段号上界，与右键菜单的段号来源同一口径。
        // 段集合的规范形（升序去重）由 config.h 的 NormalizeStatusSegs 提供，
        // setter 与 canon 共用它 —— 只归一不声明 canon 的话，"写进去 [6,0,6]"
        // 会被判成功，而下次启动读盘归一时变成 [0,6]（写进去、重启就变）。
        const settings_reg::IntArraySpec statusSegsSpec{
            L"statusSegs", 0, Seg_Count - 1,
            [](std::vector<int> v) {
                NormalizeStatusSegs(v);
                return v;
            }};
        v.push_back({L"statusSegs", Kind::IntArray,
                     [](const Settings& s) { return IntArrayValue(s.statusSegs); },
                     statusSegsSpec.SetterFor(&m.statusSegs),
                     L"状态栏显示哪些段：取值 0.." + std::to_wstring(Seg_Count - 1) +
                         L"，数组按规范形（升序去重）保存；右键状态栏可勾选",
                     L"bar_status", {}, 0, 0, statusSegsSpec.CanonFor()});

        // ---- 视图 ----
        // 视图档位标签取自 viewmode::kModes（唯一来源）：菜单里那 8 个中文名与这里
        // 必须是同一份 —— 界面显示"详细信息"而命令文档说"5=详细信息"就无从对证。
        v.push_back({L"viewMode", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.viewMode); },
                     IntSet(&m.viewMode, L"viewMode", kViewModeMin, kViewModeMax),
                     L"视图模式：0 超大图标 1 大 2 中 3 小 4 列表 5 详细信息 6 平铺 7 内容",
                     L"view", IntOptions(viewmode::kModes, viewmode::kCount), kViewModeMin,
                     kViewModeMax});
        v.push_back({L"sortMode", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.sortMode); },
                     IntSet(&m.sortMode, L"sortMode", kSortModeMin, kSortModeMax),
                     L"排序方式：0 名称 1 日期 2 类型 3 大小", L"view",
                     IntOptions(viewmode::kSortLabels, viewmode::kSortGroupCount), kSortModeMin,
                     kSortModeMax});
        v.push_back({L"sortAscending", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.sortAscending); },
                     BoolSet(&m.sortAscending, L"sortAscending"), L"升序：false = 降序", L"view"});
        v.push_back({L"groupBy", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.groupBy); },
                     IntSet(&m.groupBy, L"groupBy", kGroupByMin, kGroupByMax),
                     L"分组方式：0 不分组 1 类型 2 日期 3 大小", L"view",
                     IntOptions(viewmode::kGroupLabels, viewmode::kSortGroupCount), kGroupByMin,
                     kGroupByMax});

        // ---- 窗格与面板 ----
        // 窗格是"新标签套用"：ShellView 在创建时按配置初始化窗格宿主，
        // 显示中的标签没有改这一项的接口。doc 必须说清，否则调完看不到变化。
        v.push_back({L"showNavPane", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.showNavPane); },
                     BoolSet(&m.showNavPane, L"showNavPane"),
                     L"导航窗格：仅新标签生效（显示中的标签不重建）", L"pane"});
        v.push_back({L"showPreviewPane", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.showPreviewPane); },
                     BoolSet(&m.showPreviewPane, L"showPreviewPane"),
                     L"预览窗格：仅新标签生效（显示中的标签不重建）", L"pane"});
        v.push_back({L"dualPane", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.dualPane); },
                     BoolSet(&m.dualPane, L"dualPane"),
                     L"双栏布局：单栏 / 双栏两个窗格；立即生效，右窗格按需创建，"
                     L"左右各有独立目录与历史",
                     L"pane", MakeBoolOptions()});
        v.push_back({L"paneSplit", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.paneSplit); },
                     IntSet(&m.paneSplit, L"paneSplit", kPaneSplitMin, kPaneSplitMax),
                     L"双栏主窗格占比百分比（" + RangeText(kPaneSplitMin, kPaneSplitMax) +
                         L"）：立即生效；左右=左栏宽度，上下=上栏高度",
                     L"pane", {}, kPaneSplitMin, kPaneSplitMax});
        v.push_back({L"paneVertical", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.paneVertical); },
                     BoolSet(&m.paneVertical, L"paneVertical"),
                     L"双栏方向：false 左右并排 / true 上下堆叠（垂直双栏），立即生效", L"pane"});
        // 分割条两条强调线的粗细：活动侧指示条 + 悬停/拖动加粗线。以前写死 S(2)，
        // 高 DPI 下太细、低分辨率下偏粗 —— 纯手感值，该能调。
        v.push_back({L"splitMarkThick", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.splitMarkThick); },
                     IntSet(&m.splitMarkThick, L"splitMarkThick",
                            kSplitMarkThickMin, kSplitMarkThickMax),
                     L"分割条强调线粗细（逻辑像素，" + RangeText(kSplitMarkThickMin,
                                                              kSplitMarkThickMax) +
                         L"）：活动侧指示条与悬停线同档（立即生效）",
                     L"pane", {}, kSplitMarkThickMin, kSplitMarkThickMax});
        // 导航窗格宽度：由拖动侧栏边界写入（唯一写入点是拖拽结束），这里给命令侧
        // 开一个可写入口 —— 否则"把树调宽一点"只能靠拖，AI/脚本完全够不着。
        // 范围取 layout_limits.h：与界面能拖到的区间**同一份**。写死在两处的话，
        // 拖到最宽再松手会被读盘夹回一个更小的值（"宽度没保存住"）。
        v.push_back({L"navPaneWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.navPaneWidth); },
                     IntSet(&m.navPaneWidth, L"navPaneWidth", kNavPaneWidthMin, kNavPaneWidthMax),
                     L"导航窗格宽度（像素，" + RangeText(kNavPaneWidthMin, kNavPaneWidthMax) +
                         L"）：立即生效",
                     L"pane", {}, kNavPaneWidthMin, kNavPaneWidthMax});
        // 导航树行距与缩进：以前是 nav_pane.cpp 里的两个裸 30，按 explorer
        // 逐像素对比调出来的值，但"想要更密一点"只能改代码。TVM_SETINDENT /
        // TVM_SETITEMHEIGHT 本来就支持即时改，写进设置才不浪费这个能力。
        v.push_back({L"navPaneItemHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.navPaneItemHeight); },
                     IntSet(&m.navPaneItemHeight, L"navPaneItemHeight", kNavPaneItemHeightMin,
                            kNavPaneItemHeightMax),
                     L"导航树行高（逻辑像素，" +
                         RangeText(kNavPaneItemHeightMin, kNavPaneItemHeightMax) + L"）：立即生效",
                     L"pane", {}, kNavPaneItemHeightMin, kNavPaneItemHeightMax});
        v.push_back({L"navPaneIndent", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.navPaneIndent); },
                     IntSet(&m.navPaneIndent, L"navPaneIndent", kNavPaneIndentMin,
                            kNavPaneIndentMax),
                     L"导航树图标缩进（逻辑像素，" + RangeText(kNavPaneIndentMin, kNavPaneIndentMax) +
                         L"）：立即生效",
                     L"pane", {}, kNavPaneIndentMin, kNavPaneIndentMax});
        // 导航树的根集：一行一个，格式见 config.h。只读的理由与 tabMeta 同口径：
        // 改配置不会重建已经挂好的树，正确入口是写 settings.json 后重启，
        // 或由 App::ApplyNavPane 的调用方触发。给设备器一个可写入口会造出
        // "写进去了、树没变"的静默失败。
        v.push_back({L"navPaneRoots", Kind::StrArray,
                     [](const Settings& s) { return StrArrayValue(s.navPaneRoots); }, nullptr,
                     L"导航树的根（只读：一行一个，clsid:/known:/path: 前缀；改完重启生效）",
                     L"pane"});
        // AI 面板宽度：与 navPaneWidth 对称（一个占最左、一个占最右），同样由拖动写入。
        // 面板收着时改它只影响下次展开 —— 这一点必须写进 doc，否则调完看不到变化。
        v.push_back({L"aiPanelWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.aiPanelWidth); },
                     IntSet(&m.aiPanelWidth, L"aiPanelWidth", kAiPanelWidthMin, kAiPanelWidthMax),
                     L"AI 面板宽度（像素，" + RangeText(kAiPanelWidthMin, kAiPanelWidthMax) +
                         L"）：立即生效；收起时只影响下次展开",
                     L"pane", {}, kAiPanelWidthMin, kAiPanelWidthMax});

        // ---- 搜索 ----
        // 这三个字段此前只在 ai_api 的状态快照里露出，config.list 里完全没有 ——
        // 于是"搜索范围为什么是当前层"这种问题只能靠翻 settings.json 猜。
        // 档位取值来自 searchscope::Range（那里的枚举/文案是唯一真源）。
        const std::vector<Option> searchRangeOptions = [] {
            std::vector<Option> out;
            for (int i = 0; i < searchscope::kRangeCount; ++i) {
                Option o;
                o.label = searchscope::RangeLabel(i);
                o.value = json::Value::MakeInt(i);
                out.push_back(std::move(o));
            }
            return out;
        }();
        v.push_back({L"searchRange", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.searchRange); },
                     // 2（此电脑）在本宿主搜不出来：只查 0..3 会把"永远搜不动的一档"
                     // 写进配置，用户以为设上了、之后每次搜索都失败。判据与
                     // search.scope.set 共用 searchscope::RangeSupported —— 两处
                     // 各写一份就会出现"命令拒绝、设置接受"的分叉。
                     IntCheckedSet(&m.searchRange, L"searchRange", 0,
                                   searchscope::kRangeCount - 1,
                                   [](int n) { return searchscope::RangeSupported(n); },
                                   L"这一档在本宿主搜不出来（整机搜索要 Explorer 进程内的 "
                                   L"search-ms 宿主，本宿主只能以单个索引目录建搜索文件夹）"),
                     L"搜索范围档位：0 当前目录本层 1 含子目录 2 此电脑 3 自定义路径",
                     L"search", searchRangeOptions, 0, searchscope::kRangeCount - 1});
        // 存的必须是**规范形式**：这个值会被直接拿去当目录用（IsDirectory /
        // 索引作用域）。归一化收在 spec 里（写在唯一入口处），而不是让每个调用点
        // 各自记得转一遍 —— 否则 "命令传 /" "面板输入 \\" "AI 给双分隔符"
        // 会落成三种不同的盘上值。
        const settings_reg::StrSpec rangePathSpec{
            L"searchRangePath", {}, [](std::wstring s) { return pathutil::ToShellPath(s); }};
        v.push_back({L"searchRangePath", Kind::Str,
                     [](const Settings& s) { return json::Value::MakeStr(s.searchRangePath); },
                     rangePathSpec.SetterFor(&m.searchRangePath),
                     L"自定义搜索范围的路径：仅 searchRange=3 时有意义；反斜杠写法，"
                     L"正斜杠会在写入时归一",
                     L"search", {}, 0, 0, rangePathSpec.CanonFor()});
        v.push_back({L"searchLimit", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.searchLimit); },
                     IntSet(&m.searchLimit, L"searchLimit", kSearchLimitMin, kSearchLimitMax),
                     L"搜索历史上限（" + RangeText(kSearchLimitMin, kSearchLimitMax) +
                         L"）：搜索框右侧下拉保留多少条历史词",
                     L"search", {}, kSearchLimitMin, kSearchLimitMax});
        // 搜索历史下拉行数：与 searchLimit（存几条）刻意分开，一屏展示条数可调。
        v.push_back({L"searchHistoryMenuRows", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.searchHistoryMenuRows); },
                     IntSet(&m.searchHistoryMenuRows, L"searchHistoryMenuRows",
                            kSearchHistoryMenuRowsMin, kSearchHistoryMenuRowsMax),
                     L"搜索历史下拉行数（" + RangeText(kSearchHistoryMenuRowsMin,
                                                  kSearchHistoryMenuRowsMax) +
                         L"）：一屏列几条历史词（不影响保存上限 searchLimit）",
                     L"search", {}, kSearchHistoryMenuRowsMin, kSearchHistoryMenuRowsMax});
        v.push_back({L"searchHistory", Kind::StrArray,
                     [](const Settings& s) { return StrArrayValue(s.searchHistory); }, nullptr,
                     L"搜索历史（只读：最近的在前；上限见 searchLimit，用 search.history.clear 清空）",
                     L"search"});

        // ---- 按键与手势 ----
        v.push_back({L"seqEnabled", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.seqEnabled); },
                     BoolSet(&m.seqEnabled, L"seqEnabled"), L"启用序列键：关闭后只认普通快捷键",
                     L"sequence"});
        v.push_back({L"seqTimeoutMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.seqTimeoutMs); },
                     IntSet(&m.seqTimeoutMs, L"seqTimeoutMs", kSeqTimeoutMsMin, kSeqTimeoutMsMax),
                     L"序列键超时（ms，" + RangeText(kSeqTimeoutMsMin, kSeqTimeoutMsMax) +
                         L"）：两键之间超过它就从首键重新开始；太小难按、太大易误触",
                     L"sequence", {}, kSeqTimeoutMsMin, kSeqTimeoutMsMax});
        v.push_back({L"seqShowHints", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.seqShowHints); },
                     BoolSet(&m.seqShowHints, L"seqShowHints"), L"显示序列提示浮层：按下序列首键后在光标附近提示可选的第二键", L"sequence"});
        v.push_back({L"seqSingleInstant", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.seqSingleInstant); },
                     BoolSet(&m.seqSingleInstant, L"seqSingleInstant"),
                     L"单键即时：只有一个候选时立即执行，不等第二键", L"sequence"});
        v.push_back({L"seqUniqueAuto", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.seqUniqueAuto); },
                     BoolSet(&m.seqUniqueAuto, L"seqUniqueAuto"),
                     L"唯一候选自动：候选只剩一个时自动执行", L"sequence"});
        // 序列浮层手感：滚轮每格行数与翻页行数。都是纯偏好（没有技术最优值），
        // 以前是 sequence_overlay_input.cpp 里两个裸数字，改一次就得重编译。
        v.push_back({L"seqWheelRows", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.seqWheelRows); },
                     IntSet(&m.seqWheelRows, L"seqWheelRows", kSeqWheelRowsMin, kSeqWheelRowsMax),
                     L"浮层滚轮行数（" + RangeText(kSeqWheelRowsMin, kSeqWheelRowsMax) +
                         L"）：鼠标滚轮每格在候选列表里滚几行",
                     L"sequence", {}, kSeqWheelRowsMin, kSeqWheelRowsMax});
        v.push_back({L"seqPageRows", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.seqPageRows); },
                     IntSet(&m.seqPageRows, L"seqPageRows", kSeqPageRowsMin, kSeqPageRowsMax),
                     L"浮层翻页行数（" + RangeText(kSeqPageRowsMin, kSeqPageRowsMax) +
                         L"）：PgUp/PgDn 一次跳几行",
                     L"sequence", {}, kSeqPageRowsMin, kSeqPageRowsMax});
        v.push_back({L"holdDelayMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.holdDelayMs); },
                     IntSet(&m.holdDelayMs, L"holdDelayMs", kHoldDelayMsMin, kHoldDelayMsMax),
                     L"长按阈值（ms，" + RangeText(kHoldDelayMsMin, kHoldDelayMsMax) +
                         L"）：按住多久算长按；实际生效值不小于「双击间隔 + 50ms」",
                     L"sequence", {}, kHoldDelayMsMin, kHoldDelayMsMax});

        // ---- 弹层 ----
        // 通用弹层交互：type-ahead 缓冲窗口与可见行上限。弹层是菜单 / 面包屑
        // 共用的组件，改一次所有下拉一起变。
        v.push_back({L"popupTypeAheadMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.popupTypeAheadMs); },
                     IntSet(&m.popupTypeAheadMs, L"popupTypeAheadMs", kPopupTypeAheadMsMin,
                            kPopupTypeAheadMsMax),
                     L"弹层键入定位间隔（ms，" + RangeText(kPopupTypeAheadMsMin,
                                                       kPopupTypeAheadMsMax) +
                         L"）：连续敲字母超过它就算新前缀；慢打字者可调大",
                     L"popup", {}, kPopupTypeAheadMsMin, kPopupTypeAheadMsMax});
        v.push_back({L"popupMaxRows", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.popupMaxRows); },
                     IntSet(&m.popupMaxRows, L"popupMaxRows", kPopupMaxRowsMin, kPopupMaxRowsMax),
                     L"面包屑树弹层行数（" + RangeText(kPopupMaxRowsMin, kPopupMaxRowsMax) +
                         L"）：文件夹树下拉一屏最多几行（其它菜单行数由调用点定）",
                     L"popup", {}, kPopupMaxRowsMin, kPopupMaxRowsMax});
        v.push_back({L"popupDragThreshold", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.popupDragThreshold); },
                     IntSet(&m.popupDragThreshold, L"popupDragThreshold",
                            kPopupDragThresholdMin, kPopupDragThresholdMax),
                     L"弹层拖动阈值（逻辑像素，" + RangeText(kPopupDragThresholdMin,
                                                       kPopupDragThresholdMax) +
                         L"）：按住后移动超过它才算拖动；太小会把单击误判成拖动（立即生效）",
                     L"popup", {}, kPopupDragThresholdMin, kPopupDragThresholdMax});

        // ---- AI 通道 ----
        // aiEnabled 支持热插拔：config.set 之后命令层会调 App::SyncAiChannel
        // 立即启停管道服务（UI 线程、响应写回之后），无需重启。
        v.push_back({L"aiEnabled", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.aiEnabled); },
                     BoolSet(&m.aiEnabled, L"aiEnabled"),
                     L"启用 AI 通道（立即生效：开启后监听管道，关闭后停止监听）", L"ai"});
        // aiAllowDanger 每次危险命令都现读，改了立刻生效 —— 这是唯一允许
        // 运行中修改的"安全开关"。
        v.push_back({L"aiAllowDanger", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.aiAllowDanger); },
                     BoolSet(&m.aiAllowDanger, L"aiAllowDanger"),
                     L"允许危险命令：关闭时删除/覆盖类命令一律拒绝，即使调用方带 approve", L"ai"});
        // 调用超时：以前硬编码在 ai_server.cpp 的 kUiWaitMs=10000。扫描大目录的
        // 命令跑满 10 秒后客户端已收到 timeout，而命令还在跑 —— 两端对"成没成"
        // 的看法不一致。超时该由使用者按自己的命令耗时决定。
        v.push_back({L"aiTimeoutMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.aiTimeoutMs); },
                     IntSet(&m.aiTimeoutMs, L"aiTimeoutMs", kAiTimeoutMsMin, kAiTimeoutMsMax),
                     L"AI 调用超时（ms，" + RangeText(kAiTimeoutMsMin, kAiTimeoutMsMax) +
                         L"）：单次命令最多等多久，超时后客户端收到 timeout",
                     L"ai", {}, kAiTimeoutMsMin, kAiTimeoutMsMax});
        // 请求泵间隔：与 aiTimeoutMs 语义不同（一个是等命令，一个是查队列）。
        // 下次启用通道时重建定时器生效 —— doc 必须说清，否则调完以为卡了。
        v.push_back({L"aiPumpIntervalMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.aiPumpIntervalMs); },
                     IntSet(&m.aiPumpIntervalMs, L"aiPumpIntervalMs", kAiPumpIntervalMsMin,
                            kAiPumpIntervalMsMax),
                     L"AI 请求泵间隔（ms，" + RangeText(kAiPumpIntervalMsMin,
                                                   kAiPumpIntervalMsMax) +
                         L"）：多久检查一次请求队列；下次启用 AI 通道生效",
                     L"ai", {}, kAiPumpIntervalMsMin, kAiPumpIntervalMsMax});
        // 结果气泡自动弹出。关掉后 chip 仍显示未读计数与颜色，只是不再盖住视图。
        v.push_back({L"aiPeekEnabled", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.aiPeekEnabled); },
                     BoolSet(&m.aiPeekEnabled, L"aiPeekEnabled"),
                     L"AI 跑完时自动弹出结果气泡：关闭后不再盖住视图，"
                     L"状态栏 chip 仍显示未读数（立即生效）",
                     L"ai"});

        // ---- 审计与作业 ----
        // 总开关：放在 Write 内部收口，所有写入方自动生效。
        v.push_back({L"auditEnabled", Kind::Bool,
                     [](const Settings& s) { return json::Value::MakeBool(s.auditEnabled); },
                     BoolSet(&m.auditEnabled, L"auditEnabled"),
                     L"记录审计日志：关闭后命令不再留痕（立即生效；"
                     L"已写入的日志不删除）",
                     L"ops"});
        v.push_back({L"auditRotateKB", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.auditRotateKB); },
                     IntSet(&m.auditRotateKB, L"auditRotateKB", kAuditRotateKBMin,
                            kAuditRotateKBMax),
                     L"审计日志轮转大小（KB，" + RangeText(kAuditRotateKBMin,
                                                     kAuditRotateKBMax) +
                         L"）：写满就轮转；下次写审计生效",
                     L"ops", {}, kAuditRotateKBMin, kAuditRotateKBMax});
        v.push_back({L"auditArgsTextLimit", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.auditArgsTextLimit); },
                     IntSet(&m.auditArgsTextLimit, L"auditArgsTextLimit", kAuditArgsTextLimitMin,
                            kAuditArgsTextLimitMax),
                     L"审计参数明文上限（字符，" + RangeText(kAuditArgsTextLimitMin,
                                                       kAuditArgsTextLimitMax) +
                         L"）：超过截断；下次写审计生效",
                     L"ops", {}, kAuditArgsTextLimitMin, kAuditArgsTextLimitMax});
        v.push_back({L"jobMachineTimeoutMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.jobMachineTimeoutMs); },
                     IntSet(&m.jobMachineTimeoutMs, L"jobMachineTimeoutMs", kJobMachineTimeoutMsMin,
                            kJobMachineTimeoutMsMax),
                     L"机器作业超时（ms，" + RangeText(kJobMachineTimeoutMsMin,
                                                  kJobMachineTimeoutMsMax) +
                         L"）：无人值守任务的兜底等待；下次作业生效",
                     L"ops", {}, kJobMachineTimeoutMsMin, kJobMachineTimeoutMsMax});
        // 会话去抖：决定"崩溃前最多丢多久的状态"。它是可写策略（不是窗口几何那种
        // 活状态），但语义贴近会话持久化，放进本组。下次状态变更生效。
        v.push_back({L"sessionSaveDelayMs", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.sessionSaveDelayMs); },
                     IntSet(&m.sessionSaveDelayMs, L"sessionSaveDelayMs", kSessionSaveDelayMsMin,
                            kSessionSaveDelayMsMax),
                     L"会话保存去抖（ms，" + RangeText(kSessionSaveDelayMsMin,
                                                  kSessionSaveDelayMsMax) +
                         L"）：变更后多久落盘；下次变更生效",
                     L"ops", {}, kSessionSaveDelayMsMin, kSessionSaveDelayMsMax});

        // ---- 设置窗口 ----
        // 之前输入框长短不一的直接原因就是"下拉/字符串 192、数字 104"两套宽度；
        // 现在三栏与值控件宽度都进表，改完设置面板自己立刻重排。
        v.push_back({L"settingsGroupWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.settingsGroupWidth); },
                     IntSet(&m.settingsGroupWidth, L"settingsGroupWidth", kSettingsGroupWidthMin,
                            kSettingsGroupWidthMax),
                     L"设置窗口分类栏宽（逻辑像素，" +
                         RangeText(kSettingsGroupWidthMin, kSettingsGroupWidthMax) +
                         L"）：左侧分类栏宽度，改完立即重排",
                     L"dialog", {}, kSettingsGroupWidthMin, kSettingsGroupWidthMax});
        v.push_back({L"settingsDetailWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.settingsDetailWidth); },
                     IntSet(&m.settingsDetailWidth, L"settingsDetailWidth", kSettingsDetailWidthMin,
                            kSettingsDetailWidthMax),
                     L"设置窗口说明栏宽（逻辑像素，" +
                         RangeText(kSettingsDetailWidthMin, kSettingsDetailWidthMax) +
                         L"）：右侧说明栏宽度，改完立即重排",
                     L"dialog", {}, kSettingsDetailWidthMin, kSettingsDetailWidthMax});
        v.push_back({L"settingsControlWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.settingsControlWidth); },
                     IntSet(&m.settingsControlWidth, L"settingsControlWidth",
                            kSettingsControlWidthMin, kSettingsControlWidthMax),
                     L"设置值控件统一宽度（逻辑像素，" +
                         RangeText(kSettingsControlWidthMin, kSettingsControlWidthMax) +
                         L"）：数字框/下拉/字符串框同宽",
                     L"dialog", {}, kSettingsControlWidthMin, kSettingsControlWidthMax});

        // ---- 只读观测面（键位 / 窗口几何 / 会话状态）----
        // 这三组的 set 一律为空（config.set 拒绝）。与可写偏好分开，是因为
        // "改了不生效"必须一眼看得出来；内容见 settings_reg_table_readonly.cpp。
        detail::AppendReadonlyEntries(v);
        return v;
    }();
    return t;
}

// ---------------------------------------------------------------- 分组表
// 分组顺序即左栏标签页顺序。name 是中文名；note 写"什么时候生效"——
// 设置里最贵的一句误会就是"改了没反应"，而它多半是缺了这句。
const std::vector<Group>& GroupTable() {
    static const std::vector<Group> g = {
        {L"appearance", L"外观", L"立即生效", ico::IconId::Eye},
        {L"bar_tab", L"标签栏", L"立即生效", ico::IconId::TabBar},
        {L"bar_nav", L"导航栏", L"立即生效", ico::IconId::NavBar},
        {L"bar_bookmark", L"书签栏", L"立即生效", ico::IconId::Bookmark},
        {L"bar_status", L"状态栏", L"立即生效", ico::IconId::StatusBar},
        {L"view", L"视图", L"立即套用到当前标签", ico::IconId::GridView},
        {L"pane", L"窗格与面板", L"立即生效（窗格结构仅新标签生效）", ico::IconId::ListView},
        {L"search", L"搜索", L"立即生效", ico::IconId::Search},
        {L"sequence", L"按键与手势", L"立即生效（阈值类下次输入才读）", ico::IconId::Bolt},
        {L"popup", L"弹层", L"下次打开菜单/下拉生效", ico::IconId::Menu},
        {L"ai", L"AI 通道", L"立即生效", ico::IconId::Channel},
        {L"ops", L"审计与作业", L"下次写审计/起作业/落盘生效", ico::IconId::Page},
        {L"dialog", L"设置窗口", L"立即重排设置面板", ico::IconId::Settings},
        {L"keys", L"键位", L"只读：改 data\\keymap.json", ico::IconId::Keyboard},
        {L"geometry", L"窗口几何", L"只读：下次启动生效", ico::IconId::Window},
        {L"session", L"会话状态", L"只读：由程序维护", ico::IconId::History},
    };
    return g;
}

}  // namespace

const std::vector<Entry>& SettingsTable() { return Table(); }

const std::vector<Group>& SettingsGroupTable() { return GroupTable(); }

}  // namespace settings_reg
