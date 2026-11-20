// 工具提示宿主实现（唯一）。见 tooltip_host.h。
#include "tooltip_host.h"

#include <commctrl.h>

namespace eb {

TooltipHost::~TooltipHost() {
    if (tip_ && IsWindow(tip_)) DestroyWindow(tip_);
    tip_ = nullptr;
}

void TooltipHost::Attach(HWND owner, int maxWidthPx) {
    owner_ = owner;
    maxWidth_ = maxWidthPx;
    if (!tip_ && owner) {
        tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    }
    if (tip_ && maxWidthPx > 0) SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, maxWidthPx);
}

void TooltipHost::Clear() {
    if (!tip_) return;
    for (UINT_PTR id : ids_) {
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = owner_;
        ti.uId = id;
        SendMessageW(tip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    ids_.clear();
    texts_.clear();
}

void TooltipHost::SetTools(const std::vector<Item>& items) {
    if (!tip_) return;
    Clear();

    // 两趟：先把所有文本落定（vector 扩容会让 c_str() 失效），再一次性登记工具。
    texts_.reserve(items.size());
    for (const auto& it : items)
        if (!it.text.empty()) texts_.push_back(it.text);

    size_t k = 0;
    for (const auto& it : items) {
        if (it.text.empty()) continue;
        const UINT_PTR id = static_cast<UINT_PTR>(ids_.size() + 1);
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = owner_;
        ti.uId = id;
        ti.rect = it.rc;
        ti.lpszText = const_cast<wchar_t*>(texts_[k].c_str());
        SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        ids_.push_back(id);
        ++k;
    }
}

}  // namespace eb
