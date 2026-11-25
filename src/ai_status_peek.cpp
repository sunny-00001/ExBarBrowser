#include "ai_status_peek.h"

#include "identity.h"
#include "platform_win.h"
#include "ui_draw.h"

namespace aiui {
namespace {

bool PointIn(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

int ClampInt(int v, int lo, int hi) {
    if (hi < lo) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

const wchar_t* AiStateLabel(const std::wstring& state) {
    if (state == L"starting") return L"准备中";
    if (state == L"busy") return L"运行中";
    if (state == L"approval") return L"待确认";
    if (state == L"done") return L"完成";
    if (state == L"error") return L"失败";
    return L"空闲";
}

bool IsAiTerminalState(const std::wstring& state) {
    return state == L"approval" || state == L"done" || state == L"error";
}

bool ShouldRaiseAiUnread(const std::wstring& state, bool panelVisible) {
    return !panelVisible && IsAiTerminalState(state);
}

std::wstring AiChipText(const std::wstring& state, bool unread) {
    std::wstring text = L"AI · ";
    text += AiStateLabel(state);
    if (unread) text += L" ●";
    return text;
}

AiNativeActivity ResolveAiNativeActivity(bool aiEnabled,
                                          const std::wstring& webViewState,
                                          const std::wstring& error) {
    AiNativeActivity out;
    if (!aiEnabled) {
        out.state = L"idle";
        out.summary = L"AI 助手未启用";
        out.detail = L"在设置里打开 AI 通道后再展开面板。";
        return out;
    }
    if (!error.empty() || webViewState == L"failed") {
        out.state = L"error";
        out.summary = L"AI 助手不可用";
        out.detail = error.empty() ? L"浏览器视图启动失败。" : error;
        return out;
    }
    if (webViewState == L"ready") {
        out.state = L"idle";
        out.summary = L"AI 助手已就绪";
        out.detail = L"收到第一条会话事件后，这里会显示真实进度。";
        return out;
    }
    if (webViewState == L"creating") {
        out.state = L"starting";
        out.summary = L"AI 助手正在创建浏览器视图…";
        return out;
    }
    out.state = L"starting";
    out.summary = L"AI 助手准备中…";
    return out;
}

AiPeekLayout ComputeAiPeekLayout(const RECT& anchorScreen, int width, int height,
                                 const RECT& workArea) {
    AiPeekLayout out;
    const int w = width > 40 ? width : 320;
    const int h = height > 60 ? height : 150;
    const int margin = 6;
    int workL = workArea.left + margin;
    int workT = workArea.top + margin;
    int workR = workArea.right - margin;
    int workB = workArea.bottom - margin;
    if (workR - workL < w) { workL = workArea.left; workR = workArea.right; }
    if (workB - workT < h) { workT = workArea.top; workB = workArea.bottom; }

    int x = anchorScreen.right - w;
    if (anchorScreen.right <= anchorScreen.left) x = workR - w;
    x = ClampInt(x, workL, workR - w);

    int y = anchorScreen.top - h - 8;
    if (anchorScreen.bottom <= anchorScreen.top) y = workB - h;
    if (y < workT) y = anchorScreen.bottom + 8;
    y = ClampInt(y, workT, workB - h);

    out.window = {x, y, x + w, y + h};

    const int pad = 10;
    const int btnH = 28;
    const int openW = 92;
    const int undoW = 76;
    const int btnY = y + h - pad - btnH;
    const int openL = ClampInt(x + w - pad - openW, x + pad, x + w - pad - openW);
    out.openButton = {openL, btnY, openL + openW, btnY + btnH};
    const int undoGap = 8;
    const int undoL = ClampInt(openL - undoGap - undoW, x + pad, openL - undoGap);
    out.undoButton = {undoL, btnY, undoL + undoW, btnY + btnH};
    out.closeButton = {x + w - pad - 24, y + pad, x + w - pad, y + pad + 24};
    return out;
}

void StatusPeek::Create(HWND owner, const Theme* theme) {
    if (hwnd_) return;
    if (!win::RegisterClassOnce(id::kAiStatusPeekClass, StatusPeek::WndProc)) return;
    owner_ = owner;
    theme_ = theme;
    hwnd_ = win::CreatePopup(id::kAiStatusPeekClass, owner_, this,
                             WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             WS_POPUP, 0, 0, 1, 1, nullptr);
}

void StatusPeek::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    owner_ = nullptr;
    theme_ = nullptr;
}

void StatusPeek::SyncTheme() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void StatusPeek::Show(const AiPeekView& view, const RECT& anchorScreen) {
    if (!hwnd_ || !theme_) return;
    view_ = view;

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};

    const int w = theme_->S(360);
    const int h = theme_->S(156);
    const AiPeekLayout screen = ComputeAiPeekLayout(anchorScreen, w, h, work);
    rects_ = screen;
    const RECT win = rects_.window;
    auto toClient = [&win](RECT& r) {
        r.left -= win.left;
        r.right -= win.left;
        r.top -= win.top;
        r.bottom -= win.top;
    };
    toClient(rects_.openButton);
    toClient(rects_.undoButton);
    toClient(rects_.closeButton);

    SetWindowPos(hwnd_, HWND_TOP, win.left, win.top, win.right - win.left,
                 win.bottom - win.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void StatusPeek::Hide() {
    if (hwnd_ && IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK StatusPeek::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        if (cs && cs->lpCreateParams)
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    auto* self = reinterpret_cast<StatusPeek*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT StatusPeek::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_SETCURSOR: {
        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(h, &pt)) {
            const bool hot = PointIn(rects_.openButton, pt.x, pt.y)
                || PointIn(rects_.closeButton, pt.x, pt.y)
                || (view_.canUndo && PointIn(rects_.undoButton, pt.x, pt.y));
            if (hot) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        if (PointIn(rects_.closeButton, x, y)) {
            Hide();
            if (onDismiss_) onDismiss_();
            return 0;
        }
        if (PointIn(rects_.openButton, x, y)) {
            Hide();
            if (onOpen_) onOpen_();
            return 0;
        }
        if (view_.canUndo && PointIn(rects_.undoButton, x, y)) {
            Hide();
            if (onUndo_) onUndo_(view_.undoDomain);
            return 0;
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void StatusPeek::Paint(HWND h) {
    if (!theme_) return;
    ui::PaintBuffered(h, theme_->barAlt, [&](HDC dc, const RECT&) {
        RECT rc{};
        GetClientRect(h, &rc);
        const COLORREF bg = ui::MixColor(theme_->bg, theme_->text, 6);
        const COLORREF fg = ui::EnsureContrast(theme_->text, bg, 4.5);
        const COLORREF dim = ui::MixColor(fg, bg, 58);
        ui::FillRectColor(dc, rc, bg);
        ui::FrameRectColor(dc, rc, theme_->panelBorder);

        COLORREF accent = theme_->accent;
        if (view_.tone == AiPeekTone::Error) accent = theme_->danger;
        else if (view_.tone == AiPeekTone::Approval) accent = theme_->accent;
        else if (view_.tone == AiPeekTone::Done) accent = theme_->sub;

        const int pad = 10;
        RECT title{rc.left + pad, rc.top + pad, rc.right - pad - 30, rc.top + pad + 22};
        ui::DrawTextRow(dc, theme_->font, view_.title.empty() ? L"AI 助手" : view_.title,
                        title, accent,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        RECT close = rects_.closeButton;
        ui::DrawTextRow(dc, theme_->font, L"×", close, dim,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT body{rc.left + pad, rc.top + pad + 30, rc.right - pad,
                  rects_.openButton.top - 8};
        if (body.bottom < body.top + 20) body.bottom = body.top + 20;
        UINT fmt = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;
        const int h1 = ui::MeasureTextHeight(dc, theme_->font, view_.summary, body, fmt);
        RECT sumRect = body;
        sumRect.bottom = sumRect.top + h1;
        ui::DrawTextRow(dc, theme_->font, view_.summary, sumRect, fg, fmt);
        if (!view_.detail.empty() && sumRect.bottom + 20 < body.bottom) {
            RECT detail{body.left, sumRect.bottom + 4, body.right, body.bottom};
            ui::DrawTextRow(dc, theme_->font, view_.detail, detail, dim, fmt);
        }

        ui::FrameRectColor(dc, rects_.openButton, theme_->accent);
        ui::DrawTextRow(dc, theme_->font, L"打开对话", rects_.openButton, fg,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (view_.canUndo) {
            ui::FrameRectColor(dc, rects_.undoButton, theme_->line);
            ui::DrawTextRow(dc, theme_->font, L"撤销", rects_.undoButton, fg,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    });
}

}  // namespace aiui
