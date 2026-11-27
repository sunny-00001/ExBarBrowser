// 快捷键管理器：模型（哪些行可见 / 冲突对方是谁 / 搜索匹配）。
//
// 【为什么单独一个文件】"屏幕上该出现哪些行"与"这些行画在哪"是两件事。
// 混在一起的典型后果是：搜索改了一次 view_ 却没重排 rowLayout_，于是
// 命中判定按新视图、绘制按旧几何 —— 表现为"点这一行却改了上一行"。
// 这条线把"内容"与"几何"分开，几何只在 layout 里算。
#include "hotkey_manager.h"

#include <algorithm>
#include <map>

#include "chord.h"
#include "commands.h"
#include "key_binding.h"
#include "str.h"

namespace {

// 范围名的展示顺序。
//
// 只在**排序**时用它：真正有哪些范围由数据决定（见 RebuildGroups）。
// 以前这张表同时也是"有哪些范围"的唯一定义，于是新增一个 scopeUi 就会
// 悄悄漏出界面（那一类命令只能在"全部"里看到，没有任何检查会报）。
const wchar_t* const kScopeOrder[] = {L"标签栏", L"导航栏", L"书签栏", L"文件视图",
                                      L"文件操作", L"系统对话框", L"窗口", L"全局"};

int ScopeRank(const std::wstring& name) {
    for (int i = 0; i < static_cast<int>(std::size(kScopeOrder)); ++i)
        if (name == kScopeOrder[i]) return i;
    return static_cast<int>(std::size(kScopeOrder));   // 未知的排在已知之后
}

ico::IconId ScopeIcon(const std::wstring& name) {
    if (name == L"标签栏") return ico::IconId::TabBar;
    if (name == L"导航栏") return ico::IconId::NavBar;
    if (name == L"书签栏") return ico::IconId::Bookmark;
    if (name == L"文件视图") return ico::IconId::Folder;
    if (name == L"文件操作") return ico::IconId::Edit;
    if (name == L"系统对话框") return ico::IconId::Settings;
    if (name == L"窗口") return ico::IconId::Window;
    return ico::IconId::Bolt;
}

// 大小写不敏感的子串匹配。数据量是百级，逐行 Lower+find 足够；
// 不预先 Lower 整张表是为了让"改了 rows_ 之后忘了重建索引"这类错不可能发生。
bool Contains(const std::wstring& hay, const std::wstring& needleLower) {
    if (needleLower.empty()) return true;
    return str::Lower(hay).find(needleLower) != std::wstring::npos;
}

}  // namespace

void HotkeyManager::RebuildRows() {
    const std::vector<BindingRow>& src = router_->Rows();
    rows_.clear();
    rows_.reserve(src.size());

    for (const BindingRow& r : src) {
        Row row;
        row.src = r;
        row.sequence = r.isSequence;
        row.custom = r.custom;
        row.unbound = r.unbound;
        if (r.isSequence) {
            // 序列键不是"一个组合键"，它是一串字符。显示成 "G G"（与菜单同口径），
            // 状态列另标「序列」，让用户一眼知道这一条不能在这儿改。
            row.keyText = keybinding::SequenceText(r.key);
        } else if (r.unbound) {
            row.keyText = L"未绑定";
        } else {
            row.keyText = r.key;
        }
        if (!r.implemented) {
            row.status = kUnimplemented;
        } else if (!r.isSequence && r.reserved) {
            row.status = kReserved;
        } else if (!r.isSequence && r.conflictHard) {
            // 只把"真会互斥"的算冲突。键位相同但可共存的不进这一档 ——
            // 否则"有冲突"这一组里一半是误报，用户会开始忽略它。
            row.status = kConflict;
        } else {
            row.status = kOk;
        }
        rows_.push_back(std::move(row));
    }

    // 冲突对方：同一组合键还被谁绑了。
    //
    // 【为什么要给出名字而不是一个 bool】"冲突"这一列只说明"有问题"，
    // 用户接下来必然要问"跟谁冲"。让他自己去 281 行里找是这一列最没用的形态；
    // 右栏直接写"还与「关闭标签」共用"，才叫把信息给全。
    //
    // 【为什么分两级】"键位相同"不等于"真的会互相吃掉"：派发路径里唯一的作用域
    // 过滤是"标签栏作用域的裸键只在焦点位于标签栏时生效"。所以同键但恰有一条
    // 是标签栏裸键时，两者**可以共存**——把它标成红字"冲突"会让用户去修一个
    // 根本不需要修的东西，而真正该修的（两条都带 Ctrl）淹没在一片红里。
    std::map<std::wstring, std::vector<int>> byChord;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const Row& row = rows_[static_cast<size_t>(i)];
        if (row.sequence || row.unbound) continue;
        KeyChord c;
        if (!chord::Parse(row.src.key, &c)) continue;
        byChord[chord::Format(c)].push_back(i);
    }
    for (const auto& kv : byChord) {
        if (kv.second.size() < 2) continue;
        for (int i : kv.second) {
            Row& row = rows_[static_cast<size_t>(i)];
            for (int j : kv.second) {
                if (j == i) continue;
                const Row& peer = rows_[static_cast<size_t>(j)];
                // 只把"真会互斥"的对方写进 peers。可共存的（标签栏裸键 vs 其它）
                // 单独用 softPeers 说明，颜色也不同。
                if (row.src.conflictHard && peer.src.conflictHard) {
                    if (!row.peers.empty()) row.peers += L" · ";
                    row.peers += peer.src.title;
                } else if (!row.softPeers.empty()) {
                    row.softPeers += L" · ";
                    row.softPeers += peer.src.title;
                } else {
                    row.softPeers += peer.src.title;
                }
            }
        }
    }

    totalAll_ = static_cast<int>(rows_.size());
    totalCustom_ = 0;
    totalConflict_ = 0;
    for (const Row& r : rows_) {
        if (r.custom || r.unbound) ++totalCustom_;
        if (r.status == kConflict) ++totalConflict_;
    }
}

void HotkeyManager::RebuildGroups() {
    groups_.clear();
    // 筛选项跟着**当前表**长，不再有"组合键 / 序列键"这两项 —— 切表是工具栏上
    // 的两颗开关（SetTableType），左栏只负责"在当前表里筛"。
    //
    // 【为什么筛选项也要分表】"已自定义 / 有冲突"是覆盖表的概念，序列键整表只读、
    // 没有覆盖项也没有冲突分级。以前这两项在序列表里恒为 0，点进去是一张空表 ——
    // 那是把"这张表没有这个维度"错说成"这一类一条都没有"。
    const bool seq = (tableType_ == kTypeSeq);
    int tableTotal = 0;
    for (const Row& r : rows_)
        if (r.sequence == seq) ++tableTotal;
    groups_.push_back({L"all", L"全部", ico::IconId::ListView, tableTotal, 0});
    if (!seq) {
        groups_.push_back({L"custom", L"已自定义", ico::IconId::Edit, totalCustom_, 0});
        groups_.push_back({L"conflict", L"有冲突", ico::IconId::Lock, totalConflict_, 0});
    }

    // 范围：按**当前表**实际出现的值收集，再按固定顺序排。
    std::vector<std::wstring> names;
    for (const Row& r : rows_) {
        if (r.sequence != seq) continue;
        if (std::find(names.begin(), names.end(), r.src.scope) == names.end())
            names.push_back(r.src.scope);
    }
    std::stable_sort(names.begin(), names.end(),
                     [](const std::wstring& a, const std::wstring& b) {
                         return ScopeRank(a) < ScopeRank(b);
                     });
    for (const std::wstring& n : names) {
        int total = 0;
        for (const Row& r : rows_)
            if (r.sequence == seq && r.src.scope == n) ++total;
        if (total == 0) continue;   // 空组不进左栏（点进去什么都没有）
        groups_.push_back({L"scope:" + n, n, ScopeIcon(n), total, 0});
    }
}

bool HotkeyManager::Matches(const Row& r) const {
    if (query_.empty()) return true;
    const std::wstring q = str::Lower(query_);
    // 键位也参与匹配：VS Code 的招牌用法就是直接敲 "ctrl+shift" 看这一组绑了谁。
    // 少了这一条，用户面对 281 行只能靠记命令名。
    return Contains(r.src.title, q) || Contains(r.src.command, q) || Contains(r.src.scope, q) ||
           Contains(r.keyText, q) || Contains(r.src.group, q);
}

void HotkeyManager::Refilter() {
    view_.clear();
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const Row& r = rows_[static_cast<size_t>(i)];
        // 第一道筛：当前表类型。组合键与序列键是两张表，永不混排。
        //
        // 【为什么不做成"一张表 + 沉底"】沉底只是排序，两类东西仍共用同一个
        // 滚动区与同一个表头 —— 用户滚到一半会突然发现"下面这半截全是灰的、
        // 点了没反应"。分表之后，每张表的每一行都可编辑（序列表则整表只读且
        // 明说），工具栏的启用态也就不会整片发灰。
        const bool isSeq = r.sequence;
        if ((tableType_ == kTypeSeq) != isSeq) continue;

        bool inGroup = false;
        if (group_ == L"all") {
            inGroup = true;
        } else if (group_ == L"custom") {
            inGroup = r.custom || r.unbound;
        } else if (group_ == L"conflict") {
            inGroup = r.status == kConflict;
        } else if (str::IStartsWith(group_, L"scope:")) {
            inGroup = r.src.scope == group_.substr(6);
        }
        if (inGroup && Matches(r)) view_.push_back(i);
    }

    // ---- 排序 ----
    //
    // 【为什么在这里排而不是在绘制时排】view_ 的顺序就是显示顺序，而命中判定
    // （HitRow）是按 view_ 下标算的。若绘制时另排一份，点到的行与画出的行
    // 就不是同一行 —— 那是本项目最怕的一类缺陷（看不出规律，只能数像素）。
    const bool asc = sortAsc_;
    const SortKey key = sortKey_;
    std::stable_sort(view_.begin(), view_.end(), [this, key, asc](int a, int b) {
        const Row& ra = rows_[static_cast<size_t>(a)];
        const Row& rb = rows_[static_cast<size_t>(b)];
        int cmp = 0;
        switch (key) {
        case kSortKey:
            // 按键位排时把"未绑定"沉底：它是"没有键"，排在最上面没意义。
            {
                const bool na = ra.unbound;
                const bool nb = rb.unbound;
                if (na != nb) cmp = na ? 1 : -1;
                else cmp = str::Lower(ra.keyText).compare(str::Lower(rb.keyText));
            }
            break;
        case kSortScope: cmp = str::Lower(ra.src.scope).compare(str::Lower(rb.src.scope)); break;
        case kSortStatus: cmp = ra.status - rb.status; break;
        case kSortTitle:
        default: cmp = str::Lower(ra.src.title).compare(str::Lower(rb.src.title)); break;
        }
        if (cmp == 0) cmp = str::Lower(ra.src.title).compare(str::Lower(rb.src.title));
        return asc ? (cmp < 0) : (cmp > 0);
    });

    // 各组命中数（左栏的 "12/34" 用它）。搜索时显示"命中/总数"，
    // 用户才知道"是这一组没有，还是我搜错了"。
    for (Group& g : groups_) {
        int hits = 0;
        for (const Row& r : rows_) {
            // 表过滤必须在最前：否则"全部"的命中数会把另一张表的行也算进来，
            // 界面显示"命中 12"而列表只有 5 条 —— 用户会以为搜索漏了行。
            if (r.sequence != (tableType_ == kTypeSeq)) continue;
            if (!Matches(r)) continue;
            bool inGroup = false;
            if (g.id == L"all") {
                inGroup = true;
            } else if (g.id == L"custom") {
                inGroup = r.custom || r.unbound;
            } else if (g.id == L"conflict") {
                inGroup = r.status == kConflict;
            } else if (str::IStartsWith(g.id, L"scope:")) {
                inGroup = r.src.scope == g.id.substr(6);
            }
            if (inGroup) ++hits;
        }
        g.hits = hits;
    }

    // 选中项被筛掉了：跟到第一条可见行（VS Code 同款）。
    // 不跟的话，右栏继续显示一条"已经不在列表里"的详情 —— 用户会以为
    // 列表和详情不是同一份数据。
    if (SelectedViewIndex() < 0) {
        if (!view_.empty()) {
            selSetting_ = rows_[static_cast<size_t>(view_[0])].src.setting;
        } else {
            selSetting_.clear();
        }
    }
    if (rowLayout_.size() != view_.size()) rowLayout_.assign(view_.size(), RowLayout{});
    scroll_ = 0;
}

const HotkeyManager::Row* HotkeyManager::Selected() const {
    if (selSetting_.empty()) return nullptr;
    for (const Row& r : rows_)
        if (r.src.setting == selSetting_) return &r;
    return nullptr;
}

int HotkeyManager::SelectedViewIndex() const {
    if (selSetting_.empty()) return -1;
    for (int vi = 0; vi < static_cast<int>(view_.size()); ++vi) {
        if (rows_[static_cast<size_t>(view_[static_cast<size_t>(vi)])].src.setting == selSetting_)
            return vi;
    }
    return -1;
}

void HotkeyManager::SelectViewIndex(int vi) {
    if (vi < 0 || vi >= static_cast<int>(view_.size())) return;
    selSetting_ = rows_[static_cast<size_t>(view_[static_cast<size_t>(vi)])].src.setting;
    anchorView_ = vi;
    EnsureVisible(vi);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::SetSort(SortKey key) {
    if (sortKey_ == key) {
        sortAsc_ = !sortAsc_;   // 同列再点 = 反向（所有列表控件的通用手势）
    } else {
        sortKey_ = key;
        // 换列时给一个"读起来自然"的默认方向：标题/键位/范围升序（A→Z），
        // 状态降序（有问题的先出来 —— 那才是用户点这一列的目的）。
        sortAsc_ = (key != kSortStatus);
    }
    // 排序改变的是 view_ 的顺序，不动 rows_。所以选中项按 setting 跟得住，
    // 但滚动位置要归零（否则会停在一个与内容无关的位置）。
    Refilter();
    scroll_ = 0;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::SetGroup(const std::wstring& id) {
    if (group_ == id) return;
    // 只改筛选，**不改表**。表由 SetTableType 管（工具栏那两颗开关）。
    //
    // 【为什么要拆】以前这里兼任"点序列键就切表"，于是 group_ 与 tableType_
    // 是同一次点击写下的两个状态，必须靠注释维持"两者一致"。现在切表只有一个
    // 写入点，"改 group_ 忘了改 tableType_"这种空表故障从结构上不可能发生。
    bool exists = false;
    for (const Group& g : groups_) {
        if (g.id == id) { exists = true; break; }
    }
    if (!exists) return;   // 不认识的组（换表后残留的旧 id）一律忽略，别把列表清空
    group_ = id;
    status_.clear();
    statusError_ = false;
    Refilter();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HotkeyManager::SetTableType(RowType t) {
    if (tableType_ == t) return;
    tableType_ = t;
    // 换表要清三样东西，少一样就会留下一块"指向旧表"的状态：
    //   · 筛选 —— 旧表的分组在新表里可能不存在（"已自定义"在序列表里没有），
    //     留着它 Refilter 会滤出一张空表；
    //   · 选中 —— 旧表的行不在新表里，右栏会显示一条"列表里找不到"的详情；
    //   · 多选 —— 同上，而且后面的批量动作会作用到看不见的行上。
    group_ = L"all";
    selSetting_.clear();
    multiSel_.clear();
    anchorView_ = -1;
    status_.clear();
    statusError_ = false;
    RebuildGroups();   // 筛选项的集合本身跟着表变
    Refilter();        // 内部会把选中项跟到第一条可见行
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool HotkeyManager::IsSelectedSetting(const std::wstring& setting) const {
    for (const std::wstring& s : multiSel_)
        if (s == setting) return true;
    return setting == selSetting_;
}

void HotkeyManager::SelectAllVisible() {
    multiSel_.clear();
    for (int idx : view_) multiSel_.push_back(rows_[static_cast<size_t>(idx)].src.setting);
    // 全选时单选光标保持不动（右栏仍显示一条），否则右栏会变成"没有选中"。
    if (selSetting_.empty() && !view_.empty())
        selSetting_ = rows_[static_cast<size_t>(view_[0])].src.setting;
    SetStatus(L"已全选 " + std::to_wstring(multiSel_.size()) + L" 条（Ctrl+点击可增减）");
}

void HotkeyManager::ClearMultiSelection() {
    if (multiSel_.empty()) return;
    multiSel_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<std::wstring> HotkeyManager::TargetSettings() const {
    if (!multiSel_.empty()) return multiSel_;
    std::vector<std::wstring> out;
    if (!selSetting_.empty()) out.push_back(selSetting_);
    return out;
}

std::wstring HotkeyManager::SearchRawText() const {
    if (!searchEdit_) return std::wstring();
    // 两步：先取长度再取文本。固定长度缓冲区会静默截断 —— 搜索词一长，
    // 实际参与匹配的只是前若干个字符，而界面上看起来是"搜不到"。
    const int len = GetWindowTextLengthW(searchEdit_);
    if (len <= 0) return std::wstring();
    std::wstring buf(static_cast<size_t>(len), L'\0');
    const int got = GetWindowTextW(searchEdit_, buf.data(), len + 1);
    buf.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return buf;
}

void HotkeyManager::SyncSearchBox() {
    if (!searchEdit_) return;
    if (query_.empty()) {
        SetWindowTextW(searchEdit_, kSearchPlaceholder);
        searchPlaceholder_ = true;
        searchText_ = kSearchPlaceholder;
    } else {
        SetWindowTextW(searchEdit_, query_.c_str());
        searchPlaceholder_ = false;
        searchText_ = query_;
    }
}
