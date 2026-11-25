// 序列弹层的**输入事件**处理（键盘 / 鼠标 / 滚轮）与窗口过程。
//
// 【为什么单独成文件】sequence_overlay.cpp 是弹层的**状态与数据**（模式、
// 过滤、行集、几何、生命周期）；这里是一整类**事件**：键按下、字符、鼠标
// 移动/点击、滚轮。两者的读法完全不同 —— 看前者是在问"弹层现在是什么"，
// 看后者是在问"用户刚才做了什么"。混在一个文件里，读的人要翻到文件末尾
// 才发现还有另一套语义。
//
// 【键盘归属的两层】弹窗自己吞掉"特殊键 + 键模式下的字母数字"，
// 其余键返回 false 交回宿主（宿主按"输入框打字"处理，字符现译、不经
// 输入法）。见 sequence_overlay.h 的 HandleKey 说明。
//
// 【滚轮归属】滚轮消息只发给**焦点窗口**，而弹窗是 WS_EX_NOACTIVATE 的
// 置顶浮窗 —— 拿不到焦点，于是光标在弹窗上滚时滚的是弹窗**背后**的文件
// 视图。三条路最终都汇到 SequenceOverlay::Wheel：
//   1. 宿主转发（InputRouter::HandleWheel，光标在列表区时）—— 正常态走这条；
//   2. 弹窗自己的 WM_MOUSEWHEEL（弹窗恰好有焦点时）；
//   3. 弹窗自己的 WM_INPUT 原始输入（本进程在后台时）。
// 三条路都用 50ms 去重挡掉同一次物理滚动的重复投递。
#include "sequence_overlay.h"

#include "config.h"

#include <algorithm>

#include "log.h"
#include "platform_win.h"

int SequenceOverlay::HoverRow(POINT pt) const {
    const int listH = metrics_.rowH * ViewRows();
    if (pt.y < 0 || pt.y >= listH) return -1;
    const int row = static_cast<int>(scroll_) + pt.y / metrics_.rowH;
    if (row < 0 || row >= static_cast<int>(filtered_.size())) return -1;
    return row;
}

bool SequenceOverlay::ListHitScreen(POINT screen) const {
    if (!visible_ || !hwnd_) return false;
    RECT rc{};
    if (!GetWindowRect(hwnd_, &rc)) return false;
    // 只认列表区：输入区（底部）滚轮不滚列表（原版 rcList 同款）。
    const int listH = metrics_.rowH * ViewRows();
    if (screen.y < rc.top || screen.y >= rc.top + listH) return false;
    return screen.x >= rc.left && screen.x < rc.right;
}

bool SequenceOverlay::Wheel(int delta) {
    if (!visible_ || delta == 0) return false;
    // 同一次物理滚动在 <50ms 内只会命中一次：WM_MOUSEWHEEL 与原始输入双发，
    // 挡掉重复（原版 SeqApplyWheel 同款）。
    const DWORD now = GetTickCount();
    if (now - lastWheelMs_ < 50) return true;
    lastWheelMs_ = now;
    if (filtered_.empty()) return true;
    // 一次滚轮 N 行（WHEEL_DELTA = 120；高精度滚轮的小增量按比例换算）。
    // N 来自设置 seqWheelRows：滚轮速度是纯手感偏好，写死就只能改代码重编译。
    //
    // 【符号口径】delta 用 **WM_MOUSEWHEEL 约定**：正值 = 滚轮向前转
    // （内容向上、看到更靠前的行）→ 偏移变小。所以这里取负号。
    // （原始输入的 usButtonData 符号与 WM_MOUSEWHEEL 相反，转换在调用处做。）
    const int perNotch = Config::Get().s().seqWheelRows;
    int rows = -(delta * perNotch) / WHEEL_DELTA;
    if (rows == 0) rows = delta > 0 ? -1 : 1;
    const int viewRows = ViewRows();
    const int maxScroll =
        static_cast<int>(filtered_.size()) > viewRows
            ? static_cast<int>(filtered_.size()) - viewRows
            : 0;
    int s = static_cast<int>(scroll_) + rows;
    s = std::max(0, std::min(s, maxScroll));
    if (s == static_cast<int>(scroll_)) return true;
    scroll_ = static_cast<size_t>(s);
    LogDbg(L"[sequi] wheel delta=%d scroll=%zu/%d", delta, scroll_, maxScroll);
    // 光标不动、行在动 → 重算悬停行，否则高亮会停在原索引上。
    RefreshHover();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void SequenceOverlay::OnMouseMove(POINT pt) {
    const int row = HoverRow(pt);
    if (row == hover_) return;
    hover_ = row;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceOverlay::RefreshHover() {
    if (!hwnd_) return;
    POINT pt{};
    if (!GetCursorPos(&pt)) return;
    if (!ScreenToClient(hwnd_, &pt)) return;
    OnMouseMove(pt);
}

void SequenceOverlay::OnLButtonDown(POINT pt) {
    const int btn = HitBtn(pt);
    if (btn == 1) {          // 钉住：弹层常驻，不随序列超时收起
        pinned_ = !pinned_;
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (btn == 0) {          // 清空
        if (filter_.empty()) return;
        if (mode_ == kSeqKeyMode) {
            // 序列键模式：清掉**最后一个序列键**（紧凑格式无空格，直接退一个字符）。
            filter_.pop_back();
        } else {
            filter_.clear();
        }
        selected_ = 0;
        scroll_ = 0;
        Refresh();
        return;
    }
    // 点空白处：把该行选中（回车即执行它）。
    const int row = HoverRow(pt);
    if (row >= 0) {
        selected_ = static_cast<size_t>(row);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void SequenceOverlay::OnLButtonDblClick(POINT pt) {
    // 双击 = 触发该行（原版 SeqRowActivate）；单击只选中。
    const int row = HoverRow(pt);
    if (row < 0) return;
    selected_ = static_cast<size_t>(row);
    Commit();
}

bool SequenceOverlay::HandleChar(WPARAM ch) {
    if (!visible_) return false;
    const wchar_t c = static_cast<wchar_t>(ch);
    if (c == VK_BACK) {
        // 退格：删输入区最后一个字符（序列键模式删的是最后一个序列键）。
        // **永不退出弹层**（Esc 才是唯一关闭键，原版同款）。
        if (mode_ == kSeqKeyMode) {
            FeedSeqKey(VK_BACK);
            return true;
        }
        if (!filter_.empty()) {
            filter_.pop_back();
            selected_ = 0;
            Refresh();
        }
        return true;
    }
    // 控制字符（Tab / Ctrl+某键）不进过滤串，但要吞掉 —— 别漏给下面的视图。
    if (c < 0x20) return true;
    // 模式前缀：只在**未选模式**时生效 —— 原版判据是 g_seqMode < 0。
    //   · 唤起态（模式列表）：命中前缀 → 切模式（原版同款）。
    //   · 序列键候选态：原版此时 g_seqMode 仍是 -1（图标亮着、但还没选模式），
    //     所以前缀照样能切模式（原版 prefix(active) 分支）。
    // 已进某模式后再敲 '/' 不会换模式（原版同款）。
    if (mode_ == kUserMode || mode_ == kSeqKeyMode) {
        for (const auto& m : Modes()) {
            if (c == m.prefix) {
                SetMode(m.name);
                return true;
            }
        }
        // 唤起态：其它字符不落进过滤串（图标一亮就切序列列表）。
        // 序列键候选态：其它字符只吞不落（序列键按 vk 走，见 HandleKey）。
        return true;
    }
    // 文本模式（命令 / 书签 / 搜索…）：字符进过滤串。
    filter_.push_back(c);
    selected_ = 0;
    scroll_ = 0;
    Refresh();
    return true;
}

bool SequenceOverlay::HandleKey(WPARAM vk, LPARAM keyInfo) {
    if (!visible_) return false;
    const bool repeat = (keyInfo & (1LL << 30)) != 0;
    // 翻页跨度来自设置 seqPageRows：列表长时一次跳几行是手感偏好，写死就改不了。
    const int pageRows = Config::Get().s().seqPageRows;
    switch (vk) {
    case VK_ESCAPE: Close(); return true;
    case VK_UP: if (!repeat) Move(-1); return true;
    case VK_DOWN: if (!repeat) Move(1); return true;
    case VK_PRIOR: if (!repeat) Move(-pageRows); return true;
    case VK_NEXT: if (!repeat) Move(pageRows); return true;
    case VK_HOME: if (!repeat) { selected_ = 0; scroll_ = 0; InvalidateRect(hwnd_, nullptr, TRUE); } return true;
    case VK_END: if (!repeat) { selected_ = filtered_.empty() ? 0 : filtered_.size() - 1; InvalidateRect(hwnd_, nullptr, TRUE); } return true;
    case VK_RETURN: Commit(); return true;
    case VK_BACK:
        if (mode_ == kSeqKeyMode) {
            FeedSeqKey(VK_BACK);
        } else if (!filter_.empty()) {
            filter_.pop_back();
            selected_ = 0;
            Refresh();
        }
        return true;
    default: break;
    }
    // 键模式（用户唤起态 / 序列键候选）：字母数字 = "下一个序列键"，**按 vk 取**。
    //
    // 【为什么不走 WM_CHAR】中文输入法在中文态下会把字母吃进拼音组合，
    // WM_CHAR 要等用户提交组合才来；序列键要的是"键"，而且必须与序列状态机
    // 同口径（状态机吃的是 vk，见 seqov::TokenChar），否则弹窗列的候选
    // 和真正会执行的命令可能不是同一条。
    if (mode_ == kSeqKeyMode || mode_ == kUserMode) {
        const wchar_t t = seqov::TokenChar(vk);
        if (!t) return false;   // 符号键 → 交给宿主（模式前缀在 HandleChar 里判）
        // 按住不放不重复推进序列：连发会把 "g" 变成 "gg"（直接命中第二条序列）。
        // 键模式下的"再敲一个键"必须是**一次物理按下**对应一个 token。
        if (repeat) return true;
        if (mode_ == kSeqKeyMode) {
            FeedSeqKey(t);
        } else {
            // 用户唤起态敲第一个字母 → 点亮"图标" → 序列键候选（原版同款）。
            OpenSeqKeys(std::wstring(1, t));
        }
        return true;
    }
    return false;  // 文本模式：交给宿主（HandleOverlayKey → HandleChar）
}

LRESULT CALLBACK SequenceOverlay::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<SequenceOverlay>(h);
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: self->Paint(h); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER: {
        // 输入区光标闪烁：只重绘输入区，不碰列表行。
        RECT ir = self->InputRect();
        InvalidateRect(h, &ir, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        self->OnMouseMove(pt);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        self->OnLButtonDown(pt);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        self->OnLButtonDblClick(pt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        // 滚轮翻行。弹窗是 WS_EX_NOACTIVATE，拿不到焦点，正常态滚轮走的是
        // 宿主转发（InputRouter::HandleWheel）；这里覆盖"弹窗恰好有焦点"的情况。
        // 按光标位置过滤：输入区（底部）不滚列表（原版 rcList 同款）。
        const POINT sp{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (self->ListHitScreen(sp)) self->Wheel(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    case WM_INPUT: {
        // 原始输入滚轮兜底：弹窗以 RIDEV_INPUTSINK 注册，不激活也能收到。
        //
        // 【为什么这条几乎不触发】本项目的文件视图（SHELLDLL_DefView /
        // DirectUIHWND）就在**我们进程、同线程**里，滚轮消息（WM_MOUSEWHEEL）
        // 被主循环截住转给弹窗 —— 正常态根本走不到这里。原始输入只在本进程
        // **不在前台**时才有意义（此时 WM_MOUSEWHEEL 进了别的程序）。
        //
        // 【必须判光标位置】原始输入是**全局**的：本进程在后台、用户在别的
        // 程序上滚滚轮，这里也会收到 —— 不判位置就会把一个看不见的弹窗滚起来。
        // 所以按"光标在列表区"过滤（同 ListHitScreen 口径）。
        UINT sz = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, nullptr, &sz,
                            sizeof(RAWINPUTHEADER)) && sz > 0 && sz <= 128) {
            BYTE buf[128];
            UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, buf, &sz,
                                       sizeof(RAWINPUTHEADER));
            if (got == sz) {
                const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf);
                if (raw->header.dwType == RIM_TYPEMOUSE &&
                    (raw->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
                    POINT cur{};
                    if (GetCursorPos(&cur) && self->ListHitScreen(cur)) {
                        // 原始输入的滚轮增量符号与 WM_MOUSEWHEEL **相反**
                        //（同一个物理滚动：WM_MOUSEWHEEL 给 -120，原始输入给
                        // +120）—— 不取负会让两条路互相抵消（列表只动一半，
                        // 再滚回去）。转换统一在 Wheel() 的入口口径上做。
                        self->Wheel(-static_cast<short>(raw->data.mouse.usButtonData));
                    }
                }
            }
        }
        // 必须走 DefWindowProc：WM_INPUT 的原始输入缓冲由它回收，
        // 不调用会漏内存（每个 WM_INPUT 一次）。
        return DefWindowProcW(h, m, w, l);
    }
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}
