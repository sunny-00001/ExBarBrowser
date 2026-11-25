// 设置对话框的模型：哪些行可见、当前值是什么、右栏说明是什么。
//
// 【为什么模型与排布分开】"当前显示哪些行"和"这些行画在哪"是两件事，
// 混在一起就会出现"点到的地方和画出来的地方不是同一处" —— 那种错在界面上
// 表现为"点在复选框上却切换了别的项"，极难查。
//
// 【排布与命中在 settings_dialog_layout.cpp】那份文件拥有唯一的几何出处：
// 排布产出的每一份矩形（RowLayout / RightLayout）被绘制与命中测试共用。
// 两处各算一遍，差一个像素就是"点复制却复制了恢复默认"。
#include "settings_dialog.h"

#include "config.h"
#include "str.h"

namespace {

// 大小写不敏感的包含。搜索按"名称 / 键名 / 说明 / 分组名"匹配，
// 键名是英文（viewMode），用户多半按其中一段打字 —— 大小写必须无关。
bool ContainsCI(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return str::Lower(hay).find(str::Lower(needle)) != std::wstring::npos;
}

}  // namespace

// ---------------------------------------------------------------- 模型

std::wstring SettingsDialog::DisplayOf(const settings_reg::Entry& e, const json::Value& v) const {
    // 【枚举优先于类型】Bool 项也可以带 options（如 dualPane 用"单栏/双栏"下拉而不是
    // 复选框）：以后台布尔字段为落盘口径、界面用下拉表达，是常见需求。
    // 判据只看"有没有 options"，不看 kind —— 于是"界面怎么表达"与"数据是什么类型"
    // 解耦，加一个下拉布尔项不需要新 Kind。
    if (!e.options.empty()) {
        for (const auto& o : e.options)
            if (settings_reg::ValueEquals(o.value, v)) return o.label;
        // 值不在清单里（配置被手改成 0/1 之外的数）：照实显示原值，不假装匹配上了。
    }
    switch (e.kind) {
    case settings_reg::Kind::Bool:
        return v.b ? L"开" : L"关";
    case settings_reg::Kind::Int:
    case settings_reg::Kind::Str: {
        // 枚举项显示中文名。取值与显示名是同一张表（settings_reg 的 Option），
        // 这里不再自己翻译一遍 —— 翻译一份就多一处可能对不上的地方。
        for (const auto& o : e.options)
            if (settings_reg::ValueEquals(o.value, v)) return o.label;
        if (e.kind == settings_reg::Kind::Int)
            return std::to_wstring(static_cast<long long>(v.num));
        return v.str.empty() ? L"（空）" : v.str;
    }
    case settings_reg::Kind::StrArray:
    case settings_reg::Kind::IntArray:
    case settings_reg::Kind::ObjArray:
        return std::to_wstring(v.arr.size()) + L" 项";
    case settings_reg::Kind::Object:
        return L"（复合结构）";
    }
    return std::wstring();
}

bool SettingsDialog::Matches(const settings_reg::Entry& e) const {
    if (query_.empty()) return true;
    std::wstring name, desc;
    settings_reg::SplitDoc(e.doc, &name, &desc);
    const settings_reg::Group* g = settings_reg::FindGroup(e.group);
    const std::wstring gname = g ? g->name : e.group;
    return ContainsCI(name, query_) || ContainsCI(e.key, query_) || ContainsCI(desc, query_) ||
           ContainsCI(gname, query_);
}

const SettingsDialog::Row* SettingsDialog::RowByKey(const std::wstring& key) const {
    for (const auto& r : rows_)
        if (r.entry->key == key) return &r;
    return nullptr;
}

int SettingsDialog::RowIndexByKey(const std::wstring& key) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].entry->key == key) return static_cast<int>(i);
    return -1;
}

// 右栏"说明"块的文本（空说明给一句占位）。
// 量块高与画文本读的是**同一个函数**：两处各拼一份字符串，块高就会按 A 算、
// 画的是 B —— 表现是"说明压住键名"或"块里留一截空白"。
std::wstring SettingsDialog::RightDescText() const {
    const Row* row = RowByKey(selKey_);
    if (!row) return std::wstring();
    return row->desc.empty() ? L"（该项没有额外说明）" : row->desc;
}

int SettingsDialog::GroupIndex(const std::wstring& key) const {
    for (size_t i = 0; i < groups_.size(); ++i)
        if (groups_[i].group->key == key) return static_cast<int>(i);
    return -1;
}

// 搜索框的原始文字。
std::wstring SettingsDialog::SearchRawText(HWND edit) {
    if (!edit) return std::wstring();
    wchar_t buf[128] = L"";
    GetWindowTextW(edit, buf, 128);
    return std::wstring(buf);
}

// 从控件读一次搜索文字，存进缓存。
// 【为什么读控件这件事只做一次】读控件 = 给控件发 WM_GETTEXT，而控件身上
// 装着子类回调 —— 回调里再读一次，就是无限递归（爆栈崩溃）。
// 所以：文本在父窗口这边缓存一份，控件回调与绘制都只读缓存。
void SettingsDialog::UpdateSearchFromControl() {
    searchText_ = SearchRawText(searchEdit_);
    // 框里的文字等于占位文字 = 用户没在搜（规则见 kSearchPlaceholder）。
    searchPlaceholder_ = (searchText_ == kSearchPlaceholder);
    query_ = searchPlaceholder_ ? std::wstring() : str::Trim(searchText_);
}

void SettingsDialog::RebuildModel() {
    rows_.clear();
    items_.clear();
    groups_.clear();

    const std::vector<settings_reg::Entry>& entries = settings_reg::Entries();
    const bool searching = !query_.empty();

    // 全表统计：页脚要显示"共几项 / 几项可改 / 几项只读"。
    // 统计的是**整张表**，不是当前可见的行 —— 浏览态只看到一组，
    // 用可见行数算出来的是"这一组有几项"，与页脚承诺的口径不符。
    totalAll_ = 0;
    totalWritable_ = 0;
    for (const auto& e : entries) {
        ++totalAll_;
        if (e.set) ++totalWritable_;
    }

    // 分组计数：每组总项数 + 搜索命中数。左栏显示的是这两个数。
    for (const auto& g : settings_reg::Groups()) {
        GroupItem gi;
        gi.group = &g;
        for (const auto& e : entries) {
            if (e.group != g.key) continue;
            ++gi.total;
            if (searching && Matches(e)) ++gi.hits;
        }
        groups_.push_back(gi);
    }

    // 浏览态必须先有一个当前分组，否则中栏一行都没有（打开就是一片空白）。
    // 首次进入落在第一组 —— 这也是左栏列表的第一项，两边一致。
    if (!searching && group_.empty() && !groups_.empty())
        group_ = groups_.front().group->key;

    // 可见行：搜索态看全部（跨组），浏览态只看当前组。
    for (const auto& e : entries) {
        if (searching ? !Matches(e) : e.group != group_) continue;
        Row r;
        r.entry = &e;
        settings_reg::SplitDoc(e.doc, &r.name, &r.desc);
        r.value = e.get ? e.get(Config::Get().s()) : json::Value();
        r.display = DisplayOf(e, r.value);
        r.writable = static_cast<bool>(e.set);
        rows_.push_back(r);
    }

    // 中栏内容按分组表顺序分桶。标题条只在搜索态出现 ——
    // 浏览态左栏已经是页签，再顶一条标题是重复表达。
    for (const auto& gi : groups_) {
        std::vector<int> bucket;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].entry->group == gi.group->key) bucket.push_back(static_cast<int>(i));
        if (bucket.empty()) continue;
        if (searching) {
            Item head;
            head.header = true;
            head.title = gi.group->name;
            head.group = gi.group->key;
            head.icon = gi.group->icon;
            items_.push_back(head);
        }
        for (int i : bucket) {
            Item it;
            it.header = false;
            it.rowIndex = i;
            items_.push_back(it);
        }
    }

    // 选中项：原来选的那项还在就留着，不在（换组/换搜索词）就选第一条 ——
    // 右栏空着不给任何信息，比选第一条差。
    bool stillThere = false;
    for (const auto& r : rows_)
        if (r.entry->key == selKey_) stillThere = true;
    if (!stillThere) selKey_ = rows_.empty() ? std::wstring() : rows_.front().entry->key;

    // 【不变式】重建模型 = 旧的行集合已不存在。所有"指向某一行"的瞬态都必须
    // 在这里收口：编辑态、拒绝红框、悬停。
    // 正常路径（点击 / Esc / 关闭）都会先同步 CommitEdit，这里只兜底取消 ——
    // 但必须做，否则编辑框会停在新模型的同一行号上（那是另一项）。
    if (!editKey_.empty()) {
        editKey_.clear();
        if (valueEdit_) ShowWindow(valueEdit_, SW_HIDE);
    }
    badKey_.clear();
    hoverRow_ = -1;
    hoverGroup_ = -1;
}

void SettingsDialog::RefreshValues() {
    for (auto& r : rows_) {
        if (!r.entry->get) continue;
        r.value = r.entry->get(Config::Get().s());
        r.display = DisplayOf(*r.entry, r.value);
    }
}
