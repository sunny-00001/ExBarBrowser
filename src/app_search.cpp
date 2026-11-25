// 搜索域的界面：搜索框左带的「搜索范围」菜单、右带的「搜索历史」下拉。
//
// 【为什么在 App 而不在 NavBar】范围的真相在 Config::Settings 里（App 不持有
// 副本，R19 同源）。导航栏只算锚点并把点击转过来 —— 与"设置…""选择详细信息…"
// 同一套路。放两处各存一份必然漂移：菜单写着"当前目录"、URL 拼的是整台电脑。
//
// 【菜单里的每一项都走命令】范围/历史/清空分别转投 search.scope.set、
// search.query.set、search.history.clear —— 鼠标路径与键盘/AI 路径同源。
// 直接在菜单里改配置就多了一套实现，改一处忘另一处的症状是
// "菜单能设、命令设不了"（或者反过来）。
#include "app.h"

#include "app_commands.h"
#include "config.h"
#include "icon_glyph.h"
#include "menu.h"
#include "path_util.h"
#include "prompt.h"
#include "search_scope.h"

namespace {

// 菜单动作号。菜单返回的是 action（不是行号），所以用固定号段。
//
// 四个范围动作号与 searchscope::Range 的取值**一一对应且同序**
// （1..4 ↔ 0..3），下面靠 pick - kScopeCurrent 换算 —— 改枚举时这里会错位，
// 所以那一行写了断言式的注释提醒。
enum : int {
    kScopeCurrent = 1,
    kScopeSubtree = 2,
    kScopeThisPC = 3,
    kScopeCustom = 4,
    kHistoryClear = 100,
};

// 历史菜单最多列几条（其余的仍在历史里，命令面板能列全）。
// 条数来自设置 searchHistoryMenuRows（默认 15）—— 重度用户可调大，一屏翻到更多。
// 与 searchLimit（Config 里保存多少条）刻意分开：两者是"显示"与"存储"两种口径。

}  // namespace

int App::SearchRange() { return Config::Get().s().searchRange; }

void App::SetSearchRange(int range, const std::wstring& path) {
    if (range < 0 || range >= searchscope::kRangeCount) return;
    // 【走 config.set，不直接改字段】范围有两个写入口：搜索框的范围菜单
    // （search.scope.set）与通用设置命令（config.set searchRange）。两条路径
    // 各自 mutableSettings()+Save() 就会分叉：字段夹取、落盘、设置面板回灌、
    // 审计只在前者里做过。收口到命令层之后，"改范围"只有一份实现。
    //
    // flag 用 Actor::Ui()：这条路径是人点了菜单才走到（AI 侧有 search.scope.set
    // 与 config.set 两个结构化入口，不该绕到这里）。
    json::Value a = json::Value::MakeObject();
    a.Set(L"key", json::Value::MakeStr(L"searchRange"));
    a.Set(L"value", json::Value::MakeInt(range));
    CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(a), Actor::Ui());
    // 自定义路径单独写一次：它是同一条命令的另一个键，值需要归一化。
    // 只对 custom 档有意义，其它档不该把一个旧路径留在配置里当"看起来生效"的假象 ——
    // 反过来也不清空：用户切回 custom 时上次那个目录还在，是更常见的使用方式。
    if (range == searchscope::kCustom && !path.empty()) {
        json::Value p = json::Value::MakeObject();
        p.Set(L"key", json::Value::MakeStr(L"searchRangePath"));
        // 存进去的必须是**规范形式**：这个路径来自外部（命令参数/弹窗输入/AI），
        // 正斜杠与重复分隔符都是合法写法，但它会被直接拿去当目录用
        // （IsDirectory、索引作用域）—— 存的与用的必须是同一个值。
        p.Set(L"value", json::Value::MakeStr(pathutil::ToShellPath(path)));
        CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(p), Actor::Ui());
    }
}

void App::ShowSearchScopeMenu(POINT at, int widthPx) {
    const Settings& cfg = Config::Get().s();
    const int cur = cfg.searchRange;

    ui::Menu menu;
    // 勾选当前档：这类菜单以前完全不标当前值，用户点完不知道有没有生效。
    menu.AddChecked(L"当前目录（仅本层）", cur == searchscope::kCurrent, kScopeCurrent,
                    ico::IconId::Folder);
    menu.AddChecked(L"当前目录（含子目录）", cur == searchscope::kSubtree, kScopeSubtree,
                    ico::IconId::Folder);
    // 「此电脑」这一档在本宿主搜不出来（整机搜索要 Explorer 进程内的 search-ms
    // 宿主，见 search_folder.h）—— 摆成灰字说清楚，不给"点了就失败"的入口。
    const int thisPcRow =
        menu.AddChecked(L"此电脑", cur == searchscope::kThisPC, kScopeThisPC,
                        ico::IconId::HardDrive, !searchscope::RangeSupported(searchscope::kThisPC));
    if (!searchscope::RangeSupported(searchscope::kThisPC))
        menu.AppendLabel(thisPcRow, L"（本宿主暂不可用）");
    const int customRow =
        menu.AddChecked(L"自定义路径…", cur == searchscope::kCustom, kScopeCustom, ico::IconId::Folder);
    // 自定义档已设过路径时把路径补在文案后面（点进去才知道搜的是哪儿）。
    if (cur == searchscope::kCustom && !cfg.searchRangePath.empty())
        menu.AppendLabel(customRow, L"（" + cfg.searchRangePath + L"）");

    ui::MenuOptions opt;
    opt.toggleKey = L"nav.search.scope";
    // 弹层与搜索框同宽（左右边缘齐平）：宽度由导航栏随锚点一起给 ——
    // 弹层宽度在这边自己量的话，两处各量一次，DPI 一变就对不上。
    if (widthPx > 0) {
        opt.minWidthPx = widthPx;
        opt.exactWidth = true;
    }
    const int pick = menu.Show(hwnd_, theme_, at, opt);
    if (pick < kScopeCurrent || pick > kScopeCustom) return;
    const int range = pick - kScopeCurrent;   // 动作号 1..4 ↔ searchscope::Range 0..3

    if (range == searchscope::kCustom) {
        // 自定义路径要用户给路径：弹输入框（人侧这条路）。
        // 机器侧走同一条命令的 path 参数（search.scope.set {scope, path}）。
        std::wstring path;
        if (!PromptText(hwnd_, theme_, L"搜索范围", L"要搜索的目录：", cfg.searchRangePath, &path))
            return;
        if (path.empty()) return;
        json::Value a = json::Value::MakeObject();
        a.Set(L"scope", json::Value::MakeStr(searchscope::RangeId(searchscope::kCustom)));
        a.Set(L"path", json::Value::MakeStr(path));
        CommandBus::Get().Invoke(CmdId::SearchScopeSet, Args::FromJson(a), Actor::Ui());
        return;
    }

    json::Value a = json::Value::MakeObject();
    a.Set(L"scope", json::Value::MakeStr(searchscope::RangeId(range)));
    CommandBus::Get().Invoke(CmdId::SearchScopeSet, Args::FromJson(a), Actor::Ui());
}

void App::ShowSearchHistoryMenu(POINT at, int widthPx) {
    const std::vector<std::wstring> hist = Config::Get().s().searchHistory;

    const int maxRows = Config::Get().s().searchHistoryMenuRows;
    ui::Menu menu;
    int n = 0;
    for (const auto& q : hist) {
        if (n >= maxRows) break;
        menu.Add(q, n, ico::IconId::Search);
        ++n;
    }
    if (n == 0) {
        // 没有历史：给一行灰字说明，别弹一个空框让人以为程序坏了。
        menu.Add(L"（暂无搜索历史）", -1, ico::IconId::Search, true);
    } else {
        menu.Separator().Add(L"清除搜索历史", kHistoryClear, ico::IconId::Close)
            .Cmd(CmdId::SearchHistoryClear);
    }

    ui::MenuOptions opt;
    opt.toggleKey = L"nav.search.history";
    // 与范围菜单同一口径：弹层与搜索框同宽。
    if (widthPx > 0) {
        opt.minWidthPx = widthPx;
        opt.exactWidth = true;
    }
    const int pick = menu.Show(hwnd_, theme_, at, opt);
    if (pick == kHistoryClear) {
        CommandBus::Get().Invoke(CmdId::SearchHistoryClear, Args::Empty(), Actor::Ui());
        return;
    }
    if (pick < 0 || pick >= n) return;
    // 选中一条 = 回填搜索框（**不执行搜索**）：执行是回车的事，
    // 替用户按下回车等于替他决定了要不要真去搜。
    json::Value a = json::Value::MakeObject();
    a.Set(L"query", json::Value::MakeStr(hist[static_cast<size_t>(pick)]));
    if (CommandBus::Get().Invoke(CmdId::SearchQuerySet, Args::FromJson(a), Actor::Ui()))
        navBar_.FocusSearch();
}
