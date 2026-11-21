#include "config.h"

#include "common.h"  // LogDbg
#include "text_file.h"

#include <algorithm>

namespace {

// 钳到 [lo, hi]。用于"用户可改的数值字段"——超出物理合理范围的值不是错误，
// 但直接采信会把界面画到看不见的地方。
int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

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

void Config::SetProfileDir(const std::wstring& dir) {
    if (dir.empty() || dir_ == dir) return;
    dir_ = dir;
    // 目录换了却留着旧的内存配置，会把 A profile 的会话/热键写进 B profile。
    s_ = Settings{};
    loaded_ = false;
    parseFailed_ = false;
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
    // 两个延时也是用户可改的：低于下限会让按键序列/长按失效，按默认值处理。
    if (s_.seqTimeoutMs < 200) s_.seqTimeoutMs = 3000;
    if (s_.holdDelayMs < 300) s_.holdDelayMs = 800;

    s_.tabNewPos = root.GetStr(L"tabNewPos", s_.tabNewPos);
    s_.confirmCloseTabs = root.GetBool(L"confirmCloseTabs", s_.confirmCloseTabs);
    s_.tabMinWidth = root.GetInt(L"tabMinWidth", s_.tabMinWidth);
    s_.tabMaxWidth = root.GetInt(L"tabMaxWidth", s_.tabMaxWidth);
    if (s_.tabMinWidth < 60) s_.tabMinWidth = 60;
    if (s_.tabMinWidth > 320) s_.tabMinWidth = 320;
    if (s_.tabMaxWidth < s_.tabMinWidth) s_.tabMaxWidth = s_.tabMinWidth;
    if (s_.tabMaxWidth > 420) s_.tabMaxWidth = 420;

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

    // AI 通道：默认开启（缺省即监听），配置里显式写 false 才关。
    // 运行中可经 config.set 热插拔，见 App::SyncAiChannel。
    s_.aiEnabled = root.GetBool(L"aiEnabled", s_.aiEnabled);
    s_.aiAllowDanger = root.GetBool(L"aiAllowDanger", s_.aiAllowDanger);

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
    s_.sessionHistory.clear();    if (const json::Value* hist = root.Find(L"sessionHistory"); hist && hist->IsArray()) {
        for (const auto& e : hist->arr) {
            if (!e.IsObject()) continue;
            TabHistory h;
            if (const json::Value* ps = e.Find(L"paths"); ps && ps->IsArray())
                for (const auto& p : ps->arr)
                    if (p.IsString()) h.paths.push_back(p.str);
            h.idx = e.GetInt(L"idx", 0);
            if (h.idx < 0 || h.idx >= static_cast<int>(h.paths.size())) h.idx = 0;
            s_.sessionHistory.push_back(std::move(h));
        }
    }
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
    root.Set(L"confirmCloseTabs", json::Value::MakeBool(s_.confirmCloseTabs));
    root.Set(L"tabMinWidth", json::Value::MakeInt(s_.tabMinWidth));
    root.Set(L"tabMaxWidth", json::Value::MakeInt(s_.tabMaxWidth));
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
    root.Set(L"sessionSelection", MakeStringArray(s_.sessionSelection));
    root.Set(L"lockedPaths", MakeStringArray(s_.lockedPaths));
    root.Set(L"pinnedPaths", MakeStringArray(s_.pinnedPaths));
    {
        json::Value hist = json::Value::MakeArray();
        for (const auto& h : s_.sessionHistory) {
            json::Value o = json::Value::MakeObject();
            o.Set(L"paths", MakeStringArray(h.paths));
            o.Set(L"idx", json::Value::MakeInt(h.idx));
            hist.arr.push_back(std::move(o));
        }
        root.Set(L"sessionHistory", hist);
    }
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

void Config::RememberSession(const std::vector<std::wstring>& paths, int active,
                             const std::vector<std::wstring>& selection,
                             const std::vector<TabHistory>& history) {
    s_.sessionPaths = paths;
    s_.sessionActive = active;
    // 选中项恢复是"尽力"语义：视图就绪后 SelectItem 可能因目录已变而部分失败，
    // 静默容忍（保存仍在，下次关闭会覆盖）。
    s_.sessionSelection = selection;
    s_.sessionHistory = history;
}

void Config::SetTabMeta(const std::wstring& path, const std::wstring& alias, int color) {
    if (path.empty()) return;
    if (color < 0 || color >= core::kTabColorCount) color = 0;
    auto& list = s_.tabMeta;
    auto it = std::find_if(list.begin(), list.end(),
                           [&path](const TabMeta& m) { return str::IEquals(m.path, path); });
    if (alias.empty() && color == 0) {
        if (it != list.end()) list.erase(it);   // 全部复位 = 不留空项
        return;
    }
    if (it == list.end()) {
        list.push_back(TabMeta{path, alias, color});
        return;
    }
    it->alias = alias;
    it->color = color;
}

void Config::GetTabMeta(const std::wstring& path, std::wstring* alias, int* color) const {
    if (alias) alias->clear();
    if (color) *color = 0;
    if (path.empty()) return;
    for (const auto& m : s_.tabMeta) {
        if (!str::IEquals(m.path, path)) continue;
        if (alias) *alias = m.alias;
        if (color) *color = m.color;
        return;
    }
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
