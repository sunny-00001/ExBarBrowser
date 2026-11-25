// 工具提示宿主实现（唯一）。见 tooltip_host.h。
#include "tooltip_host.h"

#include <commctrl.h>

#include <utility>

namespace ui {

TooltipHost::TooltipHost(TooltipHost&& other) noexcept
    : tip_(other.tip_), owner_(other.owner_), maxWidth_(other.maxWidth_),
      texts_(std::move(other.texts_)), ids_(std::move(other.ids_)) {
    other.tip_ = nullptr;
    other.owner_ = nullptr;
    other.maxWidth_ = 0;
}

TooltipHost& TooltipHost::operator=(TooltipHost&& other) noexcept {
    if (this != &other) {
        Reset();
        tip_ = other.tip_;
        owner_ = other.owner_;
        maxWidth_ = other.maxWidth_;
        texts_ = std::move(other.texts_);
        ids_ = std::move(other.ids_);
        other.tip_ = nullptr;
        other.owner_ = nullptr;
        other.maxWidth_ = 0;
    }
    return *this;
}

void TooltipHost::Reset() {
    if (tip_ && IsWindow(tip_)) DestroyWindow(tip_);
    tip_ = nullptr;
    owner_ = nullptr;
    maxWidth_ = 0;
    texts_.clear();
    ids_.clear();
}

TooltipHost::~TooltipHost() { Reset(); }

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

void TooltipHost::MoveTools(const std::vector<RECT>& rects) {
    if (!tip_) return;
    for (size_t i = 0; i < ids_.size() && i < rects.size(); ++i) {
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = owner_;
        ti.uId = ids_[i];
        ti.rect = rects[i];
        SendMessageW(tip_, TTM_NEWTOOLRECT, 0, reinterpret_cast<LPARAM>(&ti));
    }
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

    // 高频调用（状态栏 800ms 定时、标签滚动）大多只改矩形，文本和工具数量没变。
    // 先比对文本：完全一致就只搬热区，避免 2N 次 TTM_DELTOOLW/ADDTOOLW。
    std::vector<RECT> rects;
    rects.reserve(items.size());
    bool sameTexts = true;
    size_t k = 0;
    for (const auto& it : items) {
        if (it.text.empty()) continue;
        if (k >= texts_.size() || texts_[k] != it.text) sameTexts = false;
        rects.push_back(it.rc);
        ++k;
    }
    if (sameTexts && k == texts_.size()) {
        MoveTools(rects);
        return;
    }

    Clear();

    // 两趟：先把所有文本落定（vector 扩容会让 c_str() 失效），再一次性登记工具。
    texts_.reserve(items.size());
    for (const auto& it : items)
        if (!it.text.empty()) texts_.push_back(it.text);

    k = 0;  // 复用上面已声明的计数：第二趟按同一跳过条件重新从 0 索引 texts_。
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

}  // namespace ui
