#include "sequence_hint.h"

#include "platform_win.h"

#include "ui_draw.h"

namespace {

// 类名由 identity.h 统一给出（规则 B1）
using id::kSeqHintClass;

}  // namespace

void SequenceHint::Create(HWND owner, const Theme& theme) {
    win::RegisterClassOnce(kSeqHintClass, SequenceHint::WndProc);
    owner_ = owner;
    theme_ = &theme;
    hwnd_ = win::CreatePopup(kSeqHintClass, owner, this,
                             WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, WS_POPUP, 0, 0,
                             10, 10);
    if (!hwnd_) return;
}

void SequenceHint::ReloadTheme() {
    if (visible_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceHint::Show(const std::wstring& prefix,
                        const std::vector<std::pair<std::wstring, std::wstring>>& items) {
    if (!hwnd_ || !owner_ || items.empty()) {
        Hide();
        return;
    }
    prefix_ = prefix;
    items_ = items;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(hwnd_);
    int maxW = 0;
    for (const auto& kv : items_) {
        std::wstring line = kv.first + L"   " + kv.second;
        maxW = std::max(maxW, ui::MeasureText(dc, theme_->font, line));
    }
    std::wstring head = prefix_.empty() ? std::wstring(L"序列") : prefix_;
    maxW = std::max(maxW, ui::MeasureText(dc, theme_->font, head) + theme_->S(60));
    ReleaseDC(hwnd_, dc);

    const int pad = theme_->S(10);
    const int rowH = theme_->S(22);
    width_ = maxW + pad * 2;
    height_ = pad * 2 + rowH * (static_cast<int>(items_.size()) + 1);

    RECT rc{};
    GetWindowRect(owner_, &rc);
    int x = rc.right - width_ - theme_->S(16);
    int y = rc.bottom - height_ - theme_->S(40);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceHint::Hide() {
    if (!hwnd_) return;
    if (visible_) ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

LRESULT CALLBACK SequenceHint::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<SequenceHint>(h);
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        self->Paint(h);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void SequenceHint::Paint(HWND h) {
    ui::PaintBuffered(h, theme_->listBg, [&](HDC mem, const RECT& rc) {
        // 边框：整窗一圈 1px（用四条分隔线，避免再建 pen/brush 一套样板）
        RECT t{0, 0, rc.right, 0};
        ui::DrawSeparator(mem, t, false, theme_->editBorder);
        RECT b{0, rc.bottom - 1, rc.right, rc.bottom - 1};
        ui::DrawSeparator(mem, b, false, theme_->editBorder);
        RECT lft{0, 0, 0, rc.bottom};
        ui::DrawSeparator(mem, lft, true, theme_->editBorder);
        RECT rgt{rc.right - 1, 0, rc.right - 1, rc.bottom};
        ui::DrawSeparator(mem, rgt, true, theme_->editBorder);

        HGDIOBJ oldFont = SelectObject(mem, theme_->font);
        const int pad = theme_->S(10);
        const int rowH = theme_->S(22);
        int y = pad;

        // 前缀行：g
        RECT head{pad, y, rc.right - pad, y + rowH};
        SetTextColor(mem, theme_->accent);
        DrawTextW(mem, prefix_.empty() ? L"序列" : prefix_.c_str(), -1, &head,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        y += rowH;

        for (const auto& kv : items_) {
            RECT r{pad, y, rc.right - pad, y + rowH};
            RECT rt = r;
            rt.right = r.left + theme_->S(28);
            SetTextColor(mem, theme_->text);
            DrawTextW(mem, kv.first.c_str(), -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            rt = r;
            rt.left = r.left + theme_->S(28);
            SetTextColor(mem, theme_->sub);
            DrawTextW(mem, kv.second.c_str(), -1, &rt,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += rowH;
        }

        SelectObject(mem, oldFont);
    });
}
