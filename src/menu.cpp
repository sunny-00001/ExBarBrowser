// 统一菜单构建层实现。见 menu.h。
#include "menu.h"

namespace ui {

namespace {

// 快捷键解析器（进程内唯一一份，见 menu.h::SetShortcutResolver）。
// 两个都未注入 = 菜单右端不显示任何键位（行为与"没有这功能"时一致）。
struct Resolvers {
    Menu::ShortcutResolver cmd;   // 首选：按命令身份
    Menu::SpecResolver spec;      // 例外通道：按命令规格字符串
};
Resolvers& R() {
    static Resolvers r;
    return r;
}

}  // namespace

void Menu::SetShortcutResolver(ShortcutResolver fn, SpecResolver specFn) {
    R().cmd = std::move(fn);
    R().spec = std::move(specFn);
}

Menu& Menu::Add(const std::wstring& label, int action, ico::IconId glyph, bool disabled) {
    rows_.push_back({label, L"", nullptr, false, false, disabled, action, glyph, false, L""});
    anchors_.emplace_back();
    return *this;
}

int Menu::AddChecked(const std::wstring& label, bool checked, int action, ico::IconId glyph,
                     bool disabled) {
    // 勾选态是**独立字段**，不再拼 "✓ " 前缀：勾号由弹层画在左侧独立的一列里
    // （系统字形，且所有行的勾上下对齐）。见 PopupRow::checked。
    rows_.push_back({label, L"", nullptr, false, false, disabled, action, glyph, checked, L""});
    anchors_.emplace_back();
    return Count() - 1;
}

int Menu::AddSystemIcon(const std::wstring& label, bool checked, int action, HICON icon,
                        ico::IconId glyphFallback, bool disabled) {
    // 系统位图行：icon 由调用方给（**借用**，菜单不销毁），glyphFallback 是
    // 位图取不到时的字形兜底（弹层渲染时 icon 优先、glyph 次之）。
    rows_.push_back({label, L"", icon, false, false, disabled, action,
                     icon ? ico::IconId::None : glyphFallback, checked, L""});
    anchors_.emplace_back();
    return Count() - 1;
}

int Menu::AddPath(const std::wstring& label, const std::wstring& path, int action, bool folder,
                  bool disabled) {
    // 缓存 HICON 的生命周期归 ico::Cached，菜单只借用；空路径给字形兜底，
    // 否则那一行会没有图标而显得像分隔行。
    const HICON icon = path.empty() ? nullptr : ico::Cached(path, true);
    rows_.push_back({label, path, icon, folder, false, disabled, action,
                     icon ? ico::IconId::None : ico::IconId::Folder, false, L""});
    anchors_.emplace_back();
    return Count() - 1;
}

int Menu::AddPathChecked(const std::wstring& label, bool checked, const std::wstring& path,
                         int action, bool folder, bool disabled) {
    AddPath(label, path, action, folder, disabled);
    const int row = Count() - 1;
    rows_[static_cast<size_t>(row)].checked = checked;
    return row;
}

Menu& Menu::Separator() {
    rows_.push_back({L"", L"", nullptr, false, true, false, -1, ico::IconId::None, false, L""});
    anchors_.emplace_back();
    return *this;
}

Menu& Menu::AppendLabel(int row, const std::wstring& suffix) {
    if (row < 0 || row >= Count()) return *this;
    rows_[static_cast<size_t>(row)].label += suffix;
    return *this;
}

Menu& Menu::Cmd(int row, CmdId id, const std::wstring& arg) {
    if (row < 0 || row >= Count()) return *this;
    // 分隔行不可能有动作，给它挂命令只会画出一条对不上的提示。
    if (rows_[static_cast<size_t>(row)].separator) return *this;
    if (id == CmdId::None) return *this;   // 显式不挂：保持留空
    Anchor& a = anchors_[static_cast<size_t>(row)];
    a.id = id;
    a.arg = arg;
    a.spec.clear();   // 两种锚点互斥
    return *this;
}

Menu& Menu::Cmd(CmdId id, const std::wstring& arg) { return Cmd(Count() - 1, id, arg); }

Menu& Menu::Shortcut(int row, const std::wstring& spec) {
    if (row < 0 || row >= Count()) return *this;
    if (rows_[static_cast<size_t>(row)].separator) return *this;
    Anchor& a = anchors_[static_cast<size_t>(row)];
    a.spec = spec;
    a.id = CmdId::None;   // 两种锚点互斥
    a.arg.clear();
    return *this;
}

Menu& Menu::Key(const std::wstring& spec) { return Shortcut(Count() - 1, spec); }

void Menu::FillShortcuts(std::vector<PopupRow>* rows) const {
    if (!rows || rows->empty()) return;
    const Resolvers& r = R();
    const size_t n = std::min(rows->size(), anchors_.size());
    for (size_t i = 0; i < n; ++i) {
        const Anchor& a = anchors_[i];
        if (a.id != CmdId::None) {
            // 首选：按命令身份。没有解析器就留空（不猜）。
            if (r.cmd) (*rows)[i].shortcut = r.cmd(a.id, a.arg);
        } else if (!a.spec.empty() && r.spec) {
            (*rows)[i].shortcut = r.spec(a.spec);
        }
    }
}

int Menu::Show(HWND owner, const Theme& theme, POINT screen, const MenuOptions& opt) const {
    if (rows_.empty()) return -1;

    // 解析键位：这里做（而不是 Add 时）是为了读到**最新**的键位表 ——
    // 这正是"新增快捷键后所有菜单自动带上"的机制：调用点只存命令身份，
    // 文案在每次 Show 时现取。菜单开着时改键，下一次 Show 就是新键。
    // rows_ 是 const（Show 不改菜单内容），所以拷一份再填 shortcut；
    // 行数在菜单生命周期内不变，拷贝就是一次 vector 复制。
    std::vector<PopupRow> rows = rows_;
    FillShortcuts(&rows);

    // 【重排后必须用"当前显示的那份"行做映射】
    //
    // 弹层支持就地重建行（分类内容弹层拖完一行，宿主重读数据再重建）。
    // 重建之后行序变了 —— 但动作号（data）是**构建时**定的，重建后的新行
    // 带着新动作号。如果这里还拿最初那份 rows_ 去查，用户拖完之后点的
    // 那一行会被映射成**重排之前**的动作号：点 A 打开 B、点 B 打开 C。
    //
    // 所以把宿主的 rebuildRows 包一层：它每次重建成功，就把这份 rows
    // 一起换成新的 —— "弹层里显示的是哪一份，映射就得用哪一份"。
    PopupReorder reorder = opt.reorder;
    if (reorder.rebuildRows) {
        const auto userRebuild = reorder.rebuildRows;
        reorder.rebuildRows = [&rows, userRebuild](std::vector<PopupRow>* out) -> bool {
            if (!userRebuild(out)) return false;
            rows = *out;
            return true;
        };
    }

    PopupInitial init;
    init.selectRow = opt.initialRow;
    init.scrollTo = opt.scrollTo;
    const int pick = ShowPopupList(owner, theme, screen, rows, opt.minWidthPx, opt.exactWidth,
                                   opt.maxRows, opt.toggleKey, init, reorder);
    // 动作号从**当前显示的那份**行取（拷贝出来的 rows 只是多填了 shortcut，
    // data 相同；但重排后这两份可能已经不同，见上面的说明）。
    return PopupActionFor(rows, pick);
}

}  // namespace ui