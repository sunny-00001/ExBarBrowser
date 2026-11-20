#include "config.h"

#include "common.h"  // LogDbg
#include "text_file.h"

#include <algorithm>

namespace {

std::vector<std::wstring> ReadStringArray(const json::Value& root, const wchar_t* key) {
    std::vector<std::wstring> out;
    const json::Value* a = root.Find(key);
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr)
        if (v.IsString() && !v.str.empty()) out.push_back(v.str);
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

}  // namespace

Config& Config::Get() {
    static Config c;
    return c;
}

// 便携模式：所有用户数据都在 exe\data 下，整个程序目录可整体拷走。
// 不再使用 %APPDATA%\ExBar —— 那是 ExBar mod 的目录，两边共写同一份
// bookmarks.html 会互相覆盖。
std::wstring Config::Dir() const {
    if (!dir_.empty()) return dir_;
    dir_ = tf::Join(tf::ExeDir(), L"data");
    return dir_;
}

std::wstring Config::DataDir() const { return Dir(); }
std::wstring Config::SettingsPath() const { return tf::Join(Dir(), L"settings.json"); }
std::wstring Config::BookmarksPath() const { return tf::Join(Dir(), L"bookmarks.html"); }

// 键位与鼠标规则各只有一个文件，直接就是用户文件。
//
// 早先这两个文件带 browser- 前缀（browser-keymap.json / browser-mouse.json），
// 原因是数据目录曾是 %APPDATA%\ExBar —— 与 ExBar mod 共用，而 keymap.json /
// mouse.json 是 mod 自己的文件名，不加前缀就会互相覆盖。现在数据固定放在
// <exe>\data，撞名风险消失，前缀只剩噪音，因此合并为单文件。
//
// 可恢复性：这两个文件缺失/解析失败时会回落到**编译期内置默认**
// （键位见 keymap_gen::kHotkeys，鼠标见 MouseRules 的内置表），
// 因此"删掉文件"等价于"恢复出厂设置"，不会变成无法输入的状态。
std::wstring Config::KeymapPath() const { return tf::Join(Dir(), L"keymap.json"); }

std::wstring Config::MousePath() const { return tf::Join(Dir(), L"mouse.json"); }

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

    s_.seqEnabled = root.GetBool(L"seqEnabled", s_.seqEnabled);
    s_.seqTimeoutMs = root.GetInt(L"seqTimeoutMs", s_.seqTimeoutMs);
    s_.seqShowHints = root.GetBool(L"seqShowHints", s_.seqShowHints);
    s_.seqSingleInstant = root.GetBool(L"seqSingleInstant", s_.seqSingleInstant);
    s_.seqUniqueAuto = root.GetBool(L"seqUniqueAuto", s_.seqUniqueAuto);
    s_.holdDelayMs = root.GetInt(L"holdDelayMs", s_.holdDelayMs);
    if (s_.seqTimeoutMs < 200) s_.seqTimeoutMs = 3000;
    if (s_.holdDelayMs < 300) s_.holdDelayMs = 800;

    s_.tabNewPos = root.GetStr(L"tabNewPos", s_.tabNewPos);

    // 视图：越界值一律夹回合法范围，避免配置被手改成 99 后整块视图异常。
    s_.viewMode = root.GetInt(L"viewMode", s_.viewMode);
    if (s_.viewMode < 0 || s_.viewMode > 7) s_.viewMode = 3;
    s_.sortMode = root.GetInt(L"sortMode", s_.sortMode);
    if (s_.sortMode < 0 || s_.sortMode > 3) s_.sortMode = 0;
    s_.sortAscending = root.GetBool(L"sortAscending", s_.sortAscending);
    s_.groupBy = root.GetInt(L"groupBy", s_.groupBy);
    if (s_.groupBy < 0 || s_.groupBy > 3) s_.groupBy = 0;

    s_.showNavPane = root.GetBool(L"showNavPane", s_.showNavPane);
    s_.showPreviewPane = root.GetBool(L"showPreviewPane", s_.showPreviewPane);

    // AI 通道：默认关闭，只有配置里明确写了 true 才开。
    s_.aiEnabled = root.GetBool(L"aiEnabled", s_.aiEnabled);
    s_.aiAllowDanger = root.GetBool(L"aiAllowDanger", s_.aiAllowDanger);

    const json::Value* geo = root.Find(L"window");
    if (geo && geo->IsObject()) {
        s_.hasGeometry = geo->GetBool(L"valid", false);
        s_.winX = geo->GetInt(L"x", s_.winX);
        s_.winY = geo->GetInt(L"y", s_.winY);
        s_.winW = geo->GetInt(L"w", s_.winW);
        s_.winH = geo->GetInt(L"h", s_.winH);
        s_.maximized = geo->GetBool(L"max", false);
    }

    s_.recentPaths = ReadStringArray(root, L"recentPaths");
    s_.sessionPaths = ReadStringArray(root, L"sessionPaths");
    s_.sessionActive = root.GetInt(L"sessionActive", 0);
    s_.lockedPaths = ReadStringArray(root, L"lockedPaths");
    s_.closedTabs = ReadStringArray(root, L"closedTabs");

    if (const json::Value* segs = root.Find(L"statusSegs"); segs && segs->IsArray()) {
        s_.statusSegs.clear();
        for (const auto& v : segs->arr)
            if (v.type == json::Value::kNumber) s_.statusSegs.push_back(static_cast<int>(v.num));
        // 旧版本 append 式切换会写出乱序/重复段号；状态栏右对齐依赖稳定升序，读取时归一化。
        std::sort(s_.statusSegs.begin(), s_.statusSegs.end());
        s_.statusSegs.erase(std::unique(s_.statusSegs.begin(), s_.statusSegs.end()),
                            s_.statusSegs.end());
    }

    if (const json::Value* hk = root.Find(L"hotkeys"); hk && hk->IsObject()) {
        s_.hotkeyOverrides.clear();
        for (const auto& kv : hk->obj)
            if (kv.second.IsString()) s_.hotkeyOverrides.emplace_back(kv.first, kv.second.str);
    }
}

void Config::Save() {
    // 加载时解析失败过就别写 —— 内存里只有默认值，写回去等于销毁用户配置。
    // 这条与 BookmarkStore::Save 的同名护栏是同一个道理：宁可不保存。
    if (parseFailed_) {
        LogDbg(L"[config] 拒绝写盘：settings.json 上次解析失败（覆盖会丢失最近访问/会话/键位）");
        return;
    }
    json::Value root = json::Value::MakeObject();
    root.Set(L"version", json::Value::MakeInt(1));
    root.Set(L"tab", json::Value::MakeBool(s_.showTab));
    root.Set(L"nav", json::Value::MakeBool(s_.showNav));
    root.Set(L"bookmark", json::Value::MakeBool(s_.showBookmark));
    root.Set(L"statusbar", json::Value::MakeBool(s_.showStatus));

    root.Set(L"seqEnabled", json::Value::MakeBool(s_.seqEnabled));
    root.Set(L"seqTimeoutMs", json::Value::MakeInt(s_.seqTimeoutMs));
    root.Set(L"seqShowHints", json::Value::MakeBool(s_.seqShowHints));
    root.Set(L"seqSingleInstant", json::Value::MakeBool(s_.seqSingleInstant));
    root.Set(L"seqUniqueAuto", json::Value::MakeBool(s_.seqUniqueAuto));
    root.Set(L"holdDelayMs", json::Value::MakeInt(s_.holdDelayMs));
    root.Set(L"tabNewPos", json::Value::MakeStr(s_.tabNewPos));
    root.Set(L"viewMode", json::Value::MakeInt(s_.viewMode));
    root.Set(L"sortMode", json::Value::MakeInt(s_.sortMode));
    root.Set(L"sortAscending", json::Value::MakeBool(s_.sortAscending));
    root.Set(L"groupBy", json::Value::MakeInt(s_.groupBy));
    root.Set(L"showNavPane", json::Value::MakeBool(s_.showNavPane));
    root.Set(L"showPreviewPane", json::Value::MakeBool(s_.showPreviewPane));
    root.Set(L"aiEnabled", json::Value::MakeBool(s_.aiEnabled));
    root.Set(L"aiAllowDanger", json::Value::MakeBool(s_.aiAllowDanger));

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
    root.Set(L"lockedPaths", MakeStringArray(s_.lockedPaths));
    root.Set(L"closedTabs", MakeStringArray(s_.closedTabs));
    root.Set(L"statusSegs", MakeIntArray(s_.statusSegs));

    json::Value hk = json::Value::MakeObject();
    for (const auto& kv : s_.hotkeyOverrides)
        hk.Set(kv.first, json::Value::MakeStr(kv.second));
    root.Set(L"hotkeys", hk);

    tf::EnsureDir(Dir());
    if (!tf::WriteText(SettingsPath(), json::Stringify(root, 2))) {
        // 便携模式的代价：exe 放在只读目录时写不进去。只记一次，避免刷日志。
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogDbg(L"[config] 写入失败（数据目录可能只读）：%s", SettingsPath().c_str());
        }
        return;
    }
}


void Config::AddRecent(const std::wstring& path) {
    if (path.empty()) return;
    auto& v = s_.recentPaths;
    // 已经在最前面就什么都不做：避免"原地导航"也把条目挪动一遍。
    if (!v.empty() && v.front() == path) return;
    v.erase(std::remove(v.begin(), v.end(), path), v.end());
    v.insert(v.begin(), path);
    if (v.size() > 30) v.resize(30);
    // 只改内存，不写盘：最近访问在每次导航时都会变，逐次落盘等于每次浏览都写文件。
    // 落盘时机由 App 的退出流程统一负责（OnDestroy 里 cfg.Save()）。
}

void Config::RememberSession(const std::vector<std::wstring>& paths, int active) {
    s_.sessionPaths = paths;
    s_.sessionActive = active;
}

void Config::SetHotkeyOverride(const std::wstring& setting, const std::wstring& key) {
    for (auto& kv : s_.hotkeyOverrides) {
        if (kv.first == setting) {
            kv.second = key;
            return;
        }
    }
    s_.hotkeyOverrides.emplace_back(setting, key);
}

void Config::ClearHotkeyOverride(const std::wstring& setting) {
    s_.hotkeyOverrides.erase(
        std::remove_if(s_.hotkeyOverrides.begin(), s_.hotkeyOverrides.end(),
                       [&setting](const std::pair<std::wstring, std::wstring>& kv) {
                           return kv.first == setting;
                       }),
        s_.hotkeyOverrides.end());
}

std::wstring Config::HotkeyOverride(const std::wstring& setting) const {
    for (const auto& kv : s_.hotkeyOverrides)
        if (kv.first == setting) return kv.second;
    return std::wstring();
}

bool Config::HasHotkeyOverride(const std::wstring& setting) const {
    for (const auto& kv : s_.hotkeyOverrides)
        if (kv.first == setting) return true;
    return false;
}
