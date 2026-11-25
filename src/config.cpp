#include "config.h"

#include "common.h"  // LogDbg
#include "search_scope.h"  // searchscope::kRangeCount（范围档位上界，唯一真源）
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



void Config::AddRecent(const std::wstring& path) {
    if (path.empty()) return;
    auto& v = s_.recentPaths;
    // 已经在最前面就什么都不做：避免"原地导航"也把条目挪动一遍。
    if (!v.empty() && v.front() == path) return;
    v.erase(std::remove(v.begin(), v.end(), path), v.end());
    v.insert(v.begin(), path);
    // 上限可调（recentLimit）：夹在 [1,200]，配置被手改成 0 也不能把历史清空。
    int limit = s_.recentLimit;
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;
    if (v.size() > static_cast<size_t>(limit)) v.resize(static_cast<size_t>(limit));
    // 只改内存，不写盘：最近访问在每次导航时都会变，逐次落盘等于每次浏览都写文件。
    // 落盘时机由 App 的退出流程统一负责（OnDestroy 里 cfg.Save()）。
}

void Config::AddSearchHistory(const std::wstring& query) {
    if (query.empty()) return;
    auto& v = s_.searchHistory;
    // 已经在最前面就什么都不做：同一个词连搜两次不该把列表重排一遍。
    if (!v.empty() && v.front() == query) return;
    v.erase(std::remove(v.begin(), v.end(), query), v.end());
    v.insert(v.begin(), query);
    // 上限可调（searchLimit）：与 AddRecent 同一口径。
    int limit = s_.searchLimit;
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;
    if (v.size() > static_cast<size_t>(limit)) v.resize(static_cast<size_t>(limit));
    // 只改内存，不写盘：搜索历史每次提交都变，逐次落盘等于每次搜索都写文件。
    // 落盘时机与其它会话状态一致（App 退出流程统一 Save）。
}

void Config::RememberSession(const std::vector<std::wstring>& paths, int active,
                             const std::vector<std::wstring>& selection,
                             const std::vector<TabHistory>& history,
                             const std::vector<std::wstring>& pane2Paths,
                             const std::vector<TabHistory>& pane2History) {
    s_.sessionPaths = paths;
    s_.sessionActive = active;
    // 选中项恢复是"尽力"语义：视图就绪后 SelectItem 可能因目录已变而部分失败，
    // 静默容忍（保存仍在，下次关闭会覆盖）。
    s_.sessionSelection = selection;
    s_.sessionHistory = history;
    s_.sessionPane2Paths = pane2Paths;
    s_.sessionPane2History = pane2History;
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
