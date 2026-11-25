// 搜索域的命令（导航栏搜索框的范围 / 历史 / 回填）。
//
// 这几条补的是"只有鼠标能做"的缺口：范围菜单与历史下拉过去只有界面一条路，
// AI 既看不见当前用的是哪一档范围，也读不出历史里有哪些词 —— 于是它连
// "这台机器上搜过什么"都答不上来。
//
// 范围与历史的真相在 Config::Settings（App 不持有副本，R19 同源），
// 与界面菜单读的是同一份 —— 不会出现"菜单显示 A、命令改的是 B"。
#include "app.h"

#include "app_commands.h"
#include "config.h"
#include "search_scope.h"
#include "search_url.h"
#include "str.h"

void App::RegisterSearchCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

    // ---------------- 执行搜索 ----------------
    //
    // 搜索框与这条命令共用 App::RunSearch（唯一实现）：两处各写一遍，表现迟早
    // 不同（"搜索框能搜、命令搜不到"这种最典型的假等价）。
    // 2026-11-22 起搜索真的能执行了 —— 走原生搜索文件夹（ISearchFolderItemFactory
    // 造的 PIDL），不再走那条被宿主拒绝的 search-ms: URL。
    regR(CmdId::OpSearch, [this](const Args& args, json::Value* result) {
        const std::wstring q = args.Str(L"query");
        std::wstring title;
        if (!RunSearch(q, &title)) return false;
        if (result) {
            result->Set(L"query", json::Value::MakeStr(searchurl::Trim(q)));
            result->Set(L"scope", json::Value::MakeStr(searchscope::RangeId(SearchRange())));
            result->Set(L"title", json::Value::MakeStr(title));
        }
        return true;
    });

    // 列出范围档位与当前值。纯读：只回答"现在配的是什么"，不改任何东西。
    regR(CmdId::SearchScopeList, [this](const Args&, json::Value* out) {
        if (!out) return false;
        const Settings& cfg = Config::Get().s();
        out->Set(L"scope", json::Value::MakeStr(searchscope::RangeId(cfg.searchRange)));
        out->Set(L"label", json::Value::MakeStr(searchscope::RangeLabel(cfg.searchRange)));
        // 自定义档的路径（其它档为空）。
        out->Set(L"path", json::Value::MakeStr(cfg.searchRangePath));
        json::Value arr = json::Value::MakeArray();
        for (int i = 0; i < searchscope::kRangeCount; ++i) {
            json::Value o = json::Value::MakeObject();
            o.Set(L"id", json::Value::MakeStr(searchscope::RangeId(i)));
            o.Set(L"label", json::Value::MakeStr(searchscope::RangeLabel(i)));
            o.Set(L"current", json::Value::MakeBool(i == cfg.searchRange));
            // 这一档能不能真的搜出来（"此电脑"在本宿主做不到）——
            // 不给这个字段，AI 会照着菜单里那一行去选，选完才发现失败。
            o.Set(L"supported", json::Value::MakeBool(searchscope::RangeSupported(i)));
            arr.arr.push_back(std::move(o));
        }
        out->Set(L"ranges", arr);
        return true;
    });

    // 设范围。scope 必填；path 只对"自定义路径"有意义（省略 = 沿用已设的）。
    reg(CmdId::SearchScopeSet, [this](const Args& args) {
        const std::wstring id = args.Str(L"scope");
        const int range = searchscope::RangeFromId(id);
        if (range < 0) {
            // 认不出的 id 明确拒绝，不静默退回默认档 ——
            // 退回的话用户以为换成了"此电脑"，实际搜的还是当前目录。
            CommandBus::Get().NoteFailure(L"未知的搜索范围：" + id);
            return false;
        }
        std::wstring path = args.Str(L"path");
        if (range == searchscope::kCustom && path.empty())
            path = Config::Get().s().searchRangePath;
        if (range == searchscope::kCustom && path.empty()) {
            CommandBus::Get().NoteFailure(L"自定义范围需要 path 参数（要搜索的目录）");
            return false;
        }
        // 搜不出来的一档直接拒绝，不让它进设置 —— 设置里躺着一个永远搜不动的
        // 档位，比命令当场报错更坑（用户以为设上了，之后每次搜索都失败）。
        if (!searchscope::RangeSupported(range)) {
            CommandBus::Get().NoteFailure(
                L"「此电脑」这一档在本宿主暂不可用：整机搜索要 Explorer 进程内的 search-ms 宿主，本宿主只能以单个索引目录建搜索文件夹");
            return false;
        }
        SetSearchRange(range, path);
        return true;
    });

    // 搜索历史（最近的在前）。只给内容，不给"第几条对应哪个按钮" ——
    // 那是界面顺序，不是数据。
    regR(CmdId::SearchHistoryList, [this](const Args&, json::Value* out) {
        if (!out) return false;
        const std::vector<std::wstring>& hist = Config::Get().s().searchHistory;
        json::Value arr = json::Value::MakeArray();
        for (const auto& q : hist) arr.arr.push_back(json::Value::MakeStr(q));
        out->Set(L"queries", arr);
        out->Set(L"count", json::Value::MakeInt(static_cast<int>(hist.size())));
        return true;
    });

    // 清空搜索历史。破坏性但可恢复不了 —— 所以标 destructive。
    // 回执要给"清掉了几条"：本来就是空的时候也是成功，但调用方得知道
    // 什么都没清（否则它会以为自己的清空动作生效了）。
    regR(CmdId::SearchHistoryClear, [this](const Args&, json::Value* out) {
        if (!out) return false;
        std::vector<std::wstring>& hist = Config::Get().mutableSettings().searchHistory;
        const int cleared = static_cast<int>(hist.size());
        hist.clear();
        Config::Get().Save();
        out->Set(L"cleared", json::Value::MakeInt(cleared));
        return true;
    });

    // 回填搜索词（**不执行搜索**）：执行是回车的事，替用户按下回车等于
    // 替他决定了要不要真去搜。历史菜单选中某条走的就是这条命令。
    reg(CmdId::SearchQuerySet, [this](const Args& args) {
        const std::wstring q = str::Trim(args.Str(L"query"));
        navBar_.SetSearchText(q);
        return true;
    });
}
