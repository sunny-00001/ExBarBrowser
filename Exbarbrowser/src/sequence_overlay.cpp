#include "sequence_overlay.h"

#include <algorithm>

#include "identity.h"
#include "log.h"
#include "platform_win.h"

namespace {

using id::kSeqOverlayClass;

// 弹窗贴 owner 客户区右下角：右/底边零间距、向上弹出；弹窗过大时夹取到客户区左上边界。
// 取不到客户区时回退目标所在显示器工作区右下角（与参考 mod 的 SeqAnchorPos 一致）。
static POINT AnchorPos(HWND owner, int w, int h) {
    POINT pos{};
    bool anchored = false;
    RECT cr{};
    if (owner && GetClientRect(owner, &cr)) {
        POINT br{cr.right, cr.bottom};
        POINT tl{cr.left, cr.top};
        if (ClientToScreen(owner, &br) && ClientToScreen(owner, &tl)) {
            pos.x = br.x - w;   // 右边与客户区右边完全重合
            pos.y = br.y - h;   // 底边与客户区底边完全重合
            if (pos.x < tl.x) pos.x = tl.x;   // 弹窗过宽时不越过客户区左边界
            if (pos.y < tl.y) pos.y = tl.y;   // 弹窗过高时不越过客户区上边界
            anchored = true;
        }
    }
    if (!anchored) {
        HMONITOR hm = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hm, &mi);
        pos.x = mi.rcWork.right - w;
        pos.y = mi.rcWork.bottom - h;
    }
    return pos;
}

// 与参考 mod kSeqModes 对齐：前缀 + 名称 + 占位提示。默认态（空串）= 序列键总览。
const std::vector<SequenceOverlay::Mode>& AllModes() {
    static const std::vector<SequenceOverlay::Mode> kModes = {
        {L'/', L"快速跳转", L"书签·历史·标签·当前目录"},
        {L'~', L"当前目录", L"当前目录下的子项"},
        {L'#', L"书签", L"书签名"},
        {L'@', L"标签", L"标签名"},
        {L'$', L"搜索", L"搜索词"},
        {L'!', L"命令", L"命令名 / 点分名"},
        {L'&', L"历史", L"历史项"},
        {L'%', L"路径", L"路径·程序"},
        {L'^', L"最近文件", L"最近文件"},
        {L'*', L"筛选", L"通配 * ?"},
        {L'?', L"帮助", L"筛选模式"},
        {L'<', L"后退栈", L"后退历史"},
        {L'>', L"前进栈", L"前进历史"},
    };
    return kModes;
}

bool Match(const std::wstring& text, const std::wstring& filter) {
    if (filter.empty()) return true;
    std::wstring hay = text, needle = filter;
    for (auto& c : hay) c = towlower(c);
    for (auto& c : needle) c = towlower(c);
    // 顺序子序列匹配：输入 "bkm" 能命中 "bookmark"。
    size_t pos = 0;
    for (wchar_t c : needle) {
        pos = hay.find(c, pos);
        if (pos == std::wstring::npos) return false;
        ++pos;
    }
    return true;
}

// 序列键前缀匹配：紧凑 token（"g g" → "gg"）以 prefix 开头即算命中。
// 段前缀与紧凑前缀等价 —— 每段都是单字符，两种口径切出的集合完全相同。
bool SeqPrefixMatch(const std::wstring& compactToken, const std::wstring& prefix) {
    if (prefix.empty()) return true;
    if (compactToken.size() < prefix.size()) return false;
    return compactToken.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

const std::vector<SequenceOverlay::Mode>& SequenceOverlay::Modes() { return AllModes(); }

const wchar_t* const SequenceOverlay::kSeqKeyMode = L"序列键";
const wchar_t* const SequenceOverlay::kUserMode = L"";

void SequenceOverlay::Init(HWND owner, const Theme& theme, RowsFn rows, ExecFn exec) {
    win::RegisterClassOnce(kSeqOverlayClass, SequenceOverlay::WndProc);
    owner_ = owner;
    theme_ = &theme;
    metrics_ = seqov::Scaled(seqov::Metrics{}, theme.dpi);
    rowsFn_ = std::move(rows);
    execFn_ = std::move(exec);
    hwnd_ = win::CreatePopup(kSeqOverlayClass, owner, this,
                             WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, WS_POPUP, 0, 0,
                             metrics_.winW, metrics_.rowH * metrics_.maxRows + metrics_.bufH);
    // 原始输入兜底：弹窗是 WS_EX_NOACTIVATE 的置顶浮窗，**拿不到焦点**，
    // 而滚轮消息只发给焦点窗口 —— 于是光标在弹窗上滚时，滚的是弹窗背后的
    // 文件视图，弹窗列表纹丝不动（实测）。
    // 注册 RIDEV_INPUTSINK 后，弹窗即使不激活也能收到鼠标原始输入
    // （原版 exbar.cpp 的 SeqUiProc WM_INPUT 分支同款）。
    if (hwnd_) {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;   // HID 用法页
        rid.usUsage = 0x02;       // HID 用法：鼠标
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = hwnd_;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            LogDbg(L"[sequi] 原始输入注册失败（滚轮兜底不可用）err=%lu", GetLastError());
    }
}

void SequenceOverlay::ReloadTheme() {
    if (theme_) metrics_ = seqov::Scaled(seqov::Metrics{}, theme_->dpi);
    if (visible_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceOverlay::SetMode(const std::wstring& name) {
    mode_ = name;
    filter_.clear();
    selected_ = 0;
    scroll_ = 0;
    Refresh();
}

void SequenceOverlay::Open(const std::wstring& modeName) {
    if (!hwnd_) return;
    SetMode(modeName);
    visible_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Resize();
    // 光标闪烁定时器：周期重绘输入区（对齐原版看门狗 tick 驱动的闪烁）。
    SetTimer(hwnd_, 1, 500, nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceOverlay::OpenSeqKeys(const std::wstring& prefix) {
    if (!hwnd_) return;
    mode_ = kSeqKeyMode;
    filter_ = prefix;   // filter 在此模式下即"已敲入的序列前缀"
    selected_ = 0;
    scroll_ = 0;
    hover_ = -1;
    visible_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Refresh();           // 先算行数，Resize 才知道多高
    Resize();
    SetTimer(hwnd_, 1, 500, nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceOverlay::FeedSeqKey(wchar_t ch) {
    if (!visible_ || mode_ != kSeqKeyMode) return;
    if (ch == VK_BACK) {
        if (!filter_.empty()) {
            filter_.pop_back();
            selected_ = 0;
            scroll_ = 0;
            Refresh();
        }
        return;
    }
    if (ch < 0x20) return;
    filter_.push_back(ch);
    selected_ = 0;
    scroll_ = 0;
    Refresh();
    // 回灌给宿主：宿主用状态机判定是否唯一匹配，并负责**执行 + 收尾**
    // （未钉住 → 关窗；钉住 → 常驻，只清输入框）。
    //
    // 【为什么这里不再无条件 Close】宿主执行命令时可能自己打开**另一个**弹层
    // （g p → CommandPalette 会开出「命令」面板）。此时弹层已经切成「命令」模式，
    // 这里再 Close 就会把刚开的面板关掉 —— 实测：面板闪一下就没了。
    // 收尾规则统一由宿主按钉住态决定（原版 SeqAfterExecute）。
    if (seqKeyFeed_) seqKeyFeed_(filter_);
}

void SequenceOverlay::Close() {
    if (!hwnd_) return;
    if (visible_) {
        KillTimer(hwnd_, 1);
        ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
    hover_ = -1;
    // 钉住只在本次弹层会话内有效：关窗即解钉（原版 SeqReset 末尾同款）。
    // 否则用户钉一次就永远留一个置顶浮窗（连重载模组都关不掉）。
    pinned_ = false;
}

void SequenceOverlay::ClearBuffer() {
    filter_.clear();
    selected_ = 0;
    scroll_ = 0;
    hover_ = -1;
    Refresh();
}

void SequenceOverlay::AfterExecute() {
    // 钉住态不关窗，只回 S0（可连续 Up/Down + Enter 导航、连续跑命令）；
    // 未钉住照旧关闭。这是「钉住」唯一真正改变行为的地方（原版 SeqAfterExecute）。
    if (!pinned_) { Close(); return; }
    ClearBuffer();
}

int SequenceOverlay::ViewRows() const {
    const int n = static_cast<int>(filtered_.size());
    const int cap = metrics_.maxRows > 0 ? metrics_.maxRows : 20;
    return std::max(1, std::min(cap, n));
}

void SequenceOverlay::RecalcKeyChars() {
    // 列宽按**全部**序列键里最长 token 算，与过滤结果无关 —— 否则过滤一变列宽就跳，
    // 行内凭空多出一个空格（原版注释里记的同一个坑）。
    size_t maxLen = 0;
    for (const auto& r : all_)
        if (r.key.size() > maxLen) maxLen = r.key.size();
    keyChars_ = static_cast<int>(std::max<size_t>(1, maxLen));
}

void SequenceOverlay::Refresh() {
    all_ = rowsFn_ ? rowsFn_(mode_, filter_) : std::vector<Row>{};
    filtered_.clear();
    if (mode_ == kSeqKeyMode) {
        // 序列键模式：provider 给的是**全部**序列（不按前缀筛），
        // 前缀匹配在这里做 —— 这样"列宽按全量最长 token 算"才有数据源。
        // 不用子序列匹配：否则 "g" 会把 token "g g" 之类当子串筛掉，
        // 且描述文本里没有 "g" 的行会被误杀。
        for (const auto& r : all_)
            if (SeqPrefixMatch(r.key, filter_)) filtered_.push_back(r);
        RecalcKeyChars();
    } else {
        for (const auto& r : all_)
            if (Match(r.title + L" " + r.sub + L" " + r.command, filter_)) filtered_.push_back(r);
    }
    if (selected_ >= filtered_.size()) selected_ = 0;
    if (scroll_ > selected_) scroll_ = selected_;
    // 滚动夹取：内容变少后偏移可能越界（否则列表会露出空白）。
    const size_t viewRows = static_cast<size_t>(ViewRows());
    const size_t maxScroll =
        filtered_.size() > viewRows ? filtered_.size() - viewRows : 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;
    // 可观测证据：面板每次重算都写一行（过滤/导航/派发排障的唯一入口，
    // 不写进 WM_PAINT，避免重绘风暴放大）。只列前 5 行标题，够核对又不会刷屏。
    {
        std::wstring sample;
        const size_t n = std::min<size_t>(filtered_.size(), 5);
        for (size_t i = 0; i < n; ++i) {
            if (i) sample += L" | ";
            sample += filtered_[i].title;
        }
        LogDbg(L"[sequi] mode='%s' filter='%s' rows=%zu sel=%zu top=[%s]",
               mode_.c_str(), filter_.c_str(), filtered_.size(), selected_, sample.c_str());
    }
    Resize();
    if (visible_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceOverlay::Move(int delta) {
    if (filtered_.empty()) return;
    int n = static_cast<int>(filtered_.size());
    int cur = static_cast<int>(selected_) + delta;
    if (cur < 0) cur = n - 1;
    if (cur >= n) cur = 0;
    selected_ = static_cast<size_t>(cur);
    const int kMaxRows = metrics_.maxRows > 0 ? metrics_.maxRows : 20;
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + static_cast<size_t>(kMaxRows))
        scroll_ = selected_ - static_cast<size_t>(kMaxRows) + 1;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SequenceOverlay::Commit() {
    if (selected_ >= filtered_.size()) return;
    const Row row = filtered_[selected_];
    // 进入模式（模式列表 / 帮助列表的行）**不关窗** —— 原版 SeqActivateTarget
    // 返回 true 时跳过 SeqAfterExecute，弹层留着继续用。
    const bool enterMode = (row.command == L"@mode");
    const bool keepOpen = pinned_ || enterMode;
    // 先收起再派发：带参命令会弹输入框，弹层置顶且不激活，留着会盖在对话框上。
    // 钉住态例外 —— 弹层常驻，派发完只清空输入框（原版 SeqAfterExecute）。
    if (!keepOpen) Close();
    if (execFn_) execFn_(row);
    if (keepOpen && !enterMode) ClearBuffer();
}

void SequenceOverlay::Resize() {
    if (!hwnd_ || !theme_) return;
    const int rows = ViewRows();
    const int w = metrics_.winW;
    const int h = metrics_.rowH * rows + metrics_.bufH;
    width_ = w;
    height_ = h;
    const POINT pos = AnchorPos(owner_, w, h);
    SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 输入区在列表正下方、贴窗口底边；高度与 Resize 的 bufH 同源。
RECT SequenceOverlay::InputRect() const {
    const int listH = metrics_.rowH * ViewRows();
    return RECT{0, listH, width_, listH + metrics_.bufH};
}

// 输入区按钮：0 = 清空（最右），1 = 钉住（其左）。绘制与命中同源。
RECT SequenceOverlay::ClearBtnRect() const {
    const RECT ir = InputRect();
    const int x = seqov::BtnX(metrics_, 0);
    const int y = ir.top + (ir.bottom - ir.top - metrics_.btnW) / 2;
    return RECT{x, y, x + metrics_.btnW, y + metrics_.btnW};
}

RECT SequenceOverlay::PinBtnRect() const {
    const RECT ir = InputRect();
    const int x = seqov::BtnX(metrics_, 1);
    const int y = ir.top + (ir.bottom - ir.top - metrics_.btnW) / 2;
    return RECT{x, y, x + metrics_.btnW, y + metrics_.btnW};
}

int SequenceOverlay::HitBtn(POINT pt) const {
    const RECT clr = ClearBtnRect();
    if (PtInRect(&clr, pt)) return 0;
    const RECT pin = PinBtnRect();
    if (PtInRect(&pin, pt)) return 1;
    return -1;
}
