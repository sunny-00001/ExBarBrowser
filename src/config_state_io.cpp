// state\*.json 的读写：会话 / 历史 / 标签 / 窗口四份状态文件。
//
// 【为什么从 settings.json 拆出来】这四份是程序接管、随浏览高频写的状态；
// settings.json 是低频改、人工可读的偏好。以前混在一个文件里，改一个栏显隐
// 就会把整份会话历史重写一遍，而且任何一处解析失败都要禁止整份写盘
// （连带 recentPaths / sessionPaths / hotkeyOverrides 全丢）。
// 分开后：坏一份状态文件只影响它自己，偏好照常读写。
//
// 【读的口径】文件不存在 = 新装，保留内存默认值；文件存在但解析失败 =
// 记日志并保留内存值（不致命）；都不设置 parseFailed_（那只属于 settings.json）。
// 键名口径与旧的 settings.json 逐字相同，所以迁移只是"把同一批键搬到新文件"。
#include "config.h"

#include <cmath>

#include "config_io_internal.h"
#include "common.h"          // LogDbg
#include "layout_limits.h"
#include "search_scope.h"    // searchscope::kRangeCount / kDefaultRange / RangeSupported（唯一真源）
#include "settings_bounds.h"
#include "status_segs.h"     // Seg_Count：statusSegs 的元素上界（与写入端同一口径）
#include "str.h"
#include "text_file.h"

namespace {

// 读一份状态文件：不存在返回 false（正常）；存在但坏掉也返回 false，
// 同时记日志。调用方据此决定"保留内存值"。
bool ReadOne(const std::wstring& path, const wchar_t* label, json::Value* out) {
    std::wstring text;
    if (!tf::ReadText(path, &text)) return false;
    std::wstring err;
    if (!json::Parse(text, *out, &err) || !out->IsObject()) {
        LogDbg(L"[config] %ls 解析失败（保留内存值）：%ls", label, err.c_str());
        return false;
    }
    return true;
}

bool WriteOne(const std::wstring& path, const json::Value& root) {
    return tf::WriteText(path, json::Stringify(root, 2));
}

json::Value NamedSessionsJson(const std::vector<NamedSession>& list) {
    json::Value a = json::Value::MakeArray();
    for (const auto& sess : list) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"name", json::Value::MakeStr(sess.name));
        o.Set(L"paths", configio::MakeStringArray(sess.paths));
        a.arr.push_back(std::move(o));
    }
    return a;
}

void ReadNamedSessions(const json::Value& root, std::vector<NamedSession>* out) {
    out->clear();
    const json::Value* ns = root.Find(L"namedSessions");
    if (!ns || !ns->IsArray()) return;
    for (const auto& e : ns->arr) {
        if (!e.IsObject()) continue;
        NamedSession sess;
        sess.name = e.GetStr(L"name");
        if (sess.name.empty()) continue;
        if (const json::Value* ps = e.Find(L"paths"); ps && ps->IsArray())
            for (const auto& p : ps->arr)
                if (p.IsString()) sess.paths.push_back(p.str);
        out->push_back(std::move(sess));
    }
}

}  // namespace

void Config::LoadStateFiles() {
    // 每份独立读：一份坏掉不影响其它三份，也不影响 settings.json。
    if (json::Value r; ReadOne(SessionPath(), L"session.json", &r)) {
        s_.sessionPaths = configio::ReadStringArray(r, L"sessionPaths");
        s_.sessionActive = r.GetInt(L"sessionActive", s_.sessionActive);
        s_.sessionSelection = configio::ReadStringArray(r, L"sessionSelection");
        s_.sessionHistory = configio::ReadTabHistories(r, L"sessionHistory");
        s_.sessionPane2Paths = configio::ReadStringArrayKeepEmpty(r, L"sessionPane2Paths");
        s_.sessionPane2History = configio::ReadTabHistories(r, L"sessionPane2History");
        ReadNamedSessions(r, &s_.namedSessions);
    }
    if (json::Value r; ReadOne(HistoryPath(), L"history.json", &r)) {
        s_.recentPaths = configio::ReadStringArray(r, L"recentPaths");
        s_.closedTabs = configio::ReadStringArray(r, L"closedTabs");
        s_.searchHistory = configio::ReadStringArray(r, L"searchHistory");
        if (s_.searchHistory.size() > static_cast<size_t>(s_.searchLimit))
            s_.searchHistory.resize(static_cast<size_t>(s_.searchLimit));
    }
    if (json::Value r; ReadOne(TabsPath(), L"tabs.json", &r)) {
        s_.lockedPaths = configio::ReadStringArray(r, L"lockedPaths");
        s_.pinnedPaths = configio::ReadStringArray(r, L"pinnedPaths");
        s_.tabMeta = configio::ReadTabMeta(r);
        if (const json::Value* segs = r.Find(L"statusSegs"); segs && segs->IsArray()) {
            s_.statusSegs.clear();
            for (const auto& v : segs->arr) {
                // 手改的状态文件可能带越界段号：写入端按 [0, Seg_Count-1] 校验，
                // 读盘端却只做类型筛选 —— 同一批数据在两个方向上口径不同。
                // 越界元素直接丢弃（状态栏排版会自己滤，但读进来的值不该违反表声明）。
                if (v.type != json::Value::kNumber) continue;
                // 先按 double 判边界（避免浮点与 enum 直接比较的告警），再存整数。
                // 小数直接丢弃而不是静默截断：写入端 AsInt 用 std::trunc 拒收
                // 1.5 这类值，读盘端若把 1.5 截成 1，同一份 json 写进去读出来
                // 就变了值 —— 两处口径必须一致。
                if (v.num >= 0.0 && v.num < static_cast<double>(Seg_Count) &&
                    std::trunc(v.num) == v.num)
                    s_.statusSegs.push_back(static_cast<int>(v.num));
            }
            NormalizeStatusSegs(s_.statusSegs);
        }
        {
            const int r96 = configio::ClampInt(r.GetInt(L"searchRange", s_.searchRange), 0,
                                               searchscope::kRangeCount - 1);
            // 手改成"此电脑"(2) 时读进来会显示已选、搜索却永远不可用。
            // 与写入端共用 RangeSupported：不可用档回退默认值，而不是带病启动。
            s_.searchRange = searchscope::RangeSupported(r96) ? r96 : searchscope::kDefaultRange;
        }
        s_.searchRangePath = r.GetStr(L"searchRangePath");
        if (const json::Value* hk = r.Find(L"hotkeys"); hk && hk->IsObject()) {
            s_.hotkeyOverrides.clear();
            for (const auto& kv : hk->obj)
                if (kv.second.IsString()) s_.hotkeyOverrides.emplace_back(kv.first, kv.second.str);
        }
    }
    if (json::Value r; ReadOne(WindowPath(), L"window.json", &r)) {
        // 与旧 settings.json 同一套钳制：配置可能被手改或写坏，荒谬值会
        // 让 CreateWindowEx 收到负宽高、窗口落到屏幕外且没有任何报错。
        s_.hasGeometry = r.GetBool(L"valid", false);
        s_.winW = configio::ClampInt(r.GetInt(L"w", s_.winW), 240, 20000);
        s_.winH = configio::ClampInt(r.GetInt(L"h", s_.winH), 160, 20000);
        s_.winX = configio::ClampInt(r.GetInt(L"x", s_.winX), -32768, 32767);
        s_.winY = configio::ClampInt(r.GetInt(L"y", s_.winY), -32768, 32767);
        s_.maximized = r.GetBool(L"max", false);
    }
}

bool Config::SaveStateFiles() const {
    tf::EnsureDir(StateDir());

    json::Value session = json::Value::MakeObject();
    session.Set(L"sessionPaths", configio::MakeStringArray(s_.sessionPaths));
    session.Set(L"sessionActive", json::Value::MakeInt(s_.sessionActive));
    session.Set(L"sessionSelection", configio::MakeStringArray(s_.sessionSelection));
    session.Set(L"sessionHistory", configio::MakeTabHistories(s_.sessionHistory));
    session.Set(L"sessionPane2Paths", configio::MakeStringArray(s_.sessionPane2Paths));
    session.Set(L"sessionPane2History", configio::MakeTabHistories(s_.sessionPane2History));
    session.Set(L"namedSessions", NamedSessionsJson(s_.namedSessions));

    json::Value history = json::Value::MakeObject();
    history.Set(L"recentPaths", configio::MakeStringArray(s_.recentPaths));
    history.Set(L"closedTabs", configio::MakeStringArray(s_.closedTabs));
    history.Set(L"searchHistory", configio::MakeStringArray(s_.searchHistory));

    json::Value tabs = json::Value::MakeObject();
    tabs.Set(L"lockedPaths", configio::MakeStringArray(s_.lockedPaths));
    tabs.Set(L"pinnedPaths", configio::MakeStringArray(s_.pinnedPaths));
    tabs.Set(L"tabMeta", configio::MakeTabMeta(s_.tabMeta));
    tabs.Set(L"statusSegs", configio::MakeIntArray(s_.statusSegs));
    tabs.Set(L"searchRange", json::Value::MakeInt(s_.searchRange));
    tabs.Set(L"searchRangePath", json::Value::MakeStr(s_.searchRangePath));
    json::Value hk = json::Value::MakeObject();
    for (const auto& kv : s_.hotkeyOverrides) hk.Set(kv.first, json::Value::MakeStr(kv.second));
    tabs.Set(L"hotkeys", hk);

    json::Value window = json::Value::MakeObject();
    window.Set(L"valid", json::Value::MakeBool(s_.hasGeometry));
    window.Set(L"x", json::Value::MakeInt(s_.winX));
    window.Set(L"y", json::Value::MakeInt(s_.winY));
    window.Set(L"w", json::Value::MakeInt(s_.winW));
    window.Set(L"h", json::Value::MakeInt(s_.winH));
    window.Set(L"max", json::Value::MakeBool(s_.maximized));

    bool ok = true;
    ok = WriteOne(SessionPath(), session) && ok;
    ok = WriteOne(HistoryPath(), history) && ok;
    ok = WriteOne(TabsPath(), tabs) && ok;
    ok = WriteOne(WindowPath(), window) && ok;
    return ok;
}
