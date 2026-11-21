// 统一菜单构建层实现。见 menu.h。
#include "menu.h"

namespace ui {

Menu& Menu::Add(const std::wstring& label, int action, ico::IconId glyph, bool disabled) {
    rows_.push_back({label, L"", nullptr, false, false, disabled, action, glyph});
    return *this;
}

int Menu::AddChecked(const std::wstring& label, bool checked, int action, ico::IconId glyph,
                     bool disabled) {
    const std::wstring text = std::wstring(checked ? L"✓ " : L"   ") + label;
    Add(text, action, glyph, disabled);
    return Count() - 1;
}

int Menu::AddPath(const std::wstring& label, const std::wstring& path, int action, bool folder,
                  bool disabled) {
    // 缓存 HICON 的生命周期归 ico::Cached，菜单只借用；空路径给字形兜底，
    // 否则那一行会没有图标而显得像分隔行。
    const HICON icon = path.empty() ? nullptr : ico::Cached(path, true);
    rows_.push_back({label, path, icon, folder, false, disabled, action,
                     icon ? ico::IconId::None : ico::IconId::Folder});
    return Count() - 1;
}

int Menu::AddPathChecked(const std::wstring& label, bool checked, const std::wstring& path,
                         int action, bool folder, bool disabled) {
    const std::wstring text = std::wstring(checked ? L"✓ " : L"   ") + label;
    return AddPath(text, path, action, folder, disabled);
}

Menu& Menu::Separator() {
    rows_.push_back({L"", L"", nullptr, false, true, false, -1, ico::IconId::None});
    return *this;
}

Menu& Menu::AppendLabel(int row, const std::wstring& suffix) {
    if (row < 0 || row >= Count()) return *this;
    rows_[static_cast<size_t>(row)].label += suffix;
    return *this;
}

int Menu::Show(HWND owner, const Theme& theme, POINT screen, const MenuOptions& opt) const {
    if (rows_.empty()) return -1;
    PopupInitial init;
    init.selectRow = opt.initialRow;
    init.scrollTo = opt.scrollTo;
    const int pick = ShowPopupList(owner, theme, screen, rows_, opt.minWidthPx, opt.exactWidth,
                                   opt.maxRows, opt.toggleKey, init);
    if (pick < 0 || pick >= Count()) return -1;
    return rows_[static_cast<size_t>(pick)].data;
}

}  // namespace ui
