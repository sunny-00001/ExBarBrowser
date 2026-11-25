// 导航栏：生命周期与对外状态入口。
//
// 职责切分（按"做什么"，不按行数）：
//   nav_bar.cpp             生命周期 / 对外状态入口
//   nav_bar_input.cpp       窗口过程 / 地址框子类 / 鼠标捕获与键盘输入状态机
//   nav_bar_layout.cpp      几何（按钮 / 地址框 / 搜索框 / 面包屑段）
//   nav_bar_hit.cpp         命中（点落在哪块矩形）+ 鼠标规则目标翻译
//   nav_bar_actions.cpp     按钮与菜单动作
//   nav_bar_draw.cpp        自绘
//   nav_bar_search.cpp      搜索框控件与历史/范围入口
//   nav_bar_view_menu.cpp   视图菜单
//
// 【为什么输入单独成文件】它保存的是"按下→抬起→提交"的时序状态；与生命周期
// 混在一起时，改 Create 很容易顺手改到捕获顺序，而那类错位没有编译错误。
#include "nav_bar.h"

#include "platform_win.h"
#include "icon_glyph.h"
#include "icon_resolve.h"

NavBar::~NavBar() {
    for (auto& b : buttons_)
        if (b.icon) DestroyIcon(b.icon);
    // leadIcon_ 来自 ico::Cached()，句柄归 icon_resolve 进程级缓存所有。
    // 这里 DestroyIcon 会连带毁掉别的栏正在用的同一枚图标。
    leadIcon_ = nullptr;
}

void NavBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kNavBarClass, NavBar::WndProc);
    parent_ = parent;
    theme_ = &theme;

    hwnd_ = win::CreateChild(kNavBarClass, parent, this, theme.barNav());
    if (!hwnd_) return;

    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10,
                            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (edit_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
        editFontGen_ = theme.fontGeneration;
        win::ThemeControl(edit_, theme.dark, L"CFD");
        InstallEditSubclass(edit_);
    }

    BuildButtons();
    SearchCreate();
    Layout();
    // Layout 在"路径为空/正处于编辑态"时会提前返回，那时按钮提示还没建起来。
    // 这里显式补一次：冷启动第一帧就应当有提示，而不是等第一次导航。
    RebuildTooltip();
}

void NavBar::BuildButtons() {
    // Win11 资源管理器同一套 MDL2 字形：系统 stock 图标是深色位图，深色主题下几乎
    // 不可见（实测），所以统一走字形并按主题色着色；back/forward 带分裂箭头。
    // 字形一律取 ico::Glyph(id)，不写码位
    buttons_.push_back({kBack, nullptr, ico::Glyph(ico::IconId::Back), {}, {}, true, true});
    buttons_.push_back({kForward, nullptr, ico::Glyph(ico::IconId::Forward), {}, {}, true, true});
    buttons_.push_back({kUp, nullptr, ico::Glyph(ico::IconId::Up), {}, {}, false, true});
    buttons_.push_back({kRefresh, nullptr, ico::Glyph(ico::IconId::Refresh), {}, {}, false, true});
    buttons_.push_back({kView, nullptr, ico::Glyph(ico::IconId::ViewMenu), {}, {}, false, true});
}

void NavBar::SetPath(const std::wstring& path, const std::wstring& display) {
    model_.SetPath(path, display);
    UpdateLeadIcon();
    // "向上"由路径唯一推导（nav::CanNavigateUp，盘根 -> 此电脑）。
    // 后退/前进不在这里碰：它们由 SetNavState 提供，详见该函数注释。
    state_.canUp = nav::CanNavigateUp(model_.path());
    for (auto& b : buttons_) {
        if (b.id == kUp) b.enabled = state_.canUp;
    }
    Layout();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NavBar::SetNavState(const NavState& st) {
    // canUp 以路径推导为准：外部传错不会让按钮与实际去向分叉
    // （"能不能上"只有一个判据，见 nav::UpTarget）。
    state_.canBack = st.canBack;
    state_.canForward = st.canForward;
    state_.canUp = nav::CanNavigateUp(model_.path());
    bool changed = false;
    for (auto& b : buttons_) {
        bool want = b.enabled;
        if (b.id == kBack) want = state_.canBack;
        else if (b.id == kForward) want = state_.canForward;
        else if (b.id == kUp) want = state_.canUp;
        if (want != b.enabled) {
            b.enabled = want;
            changed = true;
        }
    }
    // 按下态如果对应按钮刚好被禁用（例如后退后历史用尽），保留会让它一直画成
    // pressed —— 抬起事件可能已经落在别的地方了。
    if (changed && downBtn_) {
        for (const auto& b : buttons_)
            if (b.id == downBtn_ && !b.enabled) { downBtn_ = 0; break; }
    }
    if (changed && hwnd_) {
        // 提示文案带可用态（"（当前不可用）"），状态一变就要重建。
        RebuildTooltip();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void NavBar::UpdateLeadIcon() {
    if (leadIconPath_ == model_.path()) return;
    if (leadIcon_) {
        // 共享缓存里的句柄不能由调用方销毁；直接丢弃引用即可。
        leadIcon_ = nullptr;
    }
    leadIconPath_ = model_.path();
    if (leadIconPath_.empty()) return;
    // 统一图标识别：特殊命名空间（此电脑/回收站/侧边栏/云存储）取准确图标。
    // 导航/切标签每次都会走到这里；Resolve 会重复 SHParseDisplayName +
    // SHGetFileInfoW，Cached 让同一路径全进程只解析一次。
    leadIcon_ = ico::Cached(leadIconPath_, true);
}

void NavBar::ReloadTheme() {
    if (!hwnd_) return;
    if (edit_) {
        // 字体代次没变（切主题 DPI 不变时 Theme 会保留字体）就不重设。EDIT 挂着
        // SHAutoComplete，每次 WM_SETFONT 都重建补全列表，实测 110~170ms。
        // 比的是代次而不是 HFONT 值 —— GDI 句柄会复用，句柄相同≠字体没重建。
        if (editFontGen_ != theme_->fontGeneration) {
            SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_->font), TRUE);
            editFontGen_ = theme_->fontGeneration;
        }
        win::ThemeControl(edit_, theme_->dark, L"CFD");
    }
    // 提示窗宽度按 DPI 缩放 → 重建（TooltipHost 析构会销毁旧窗）
    tooltip_ = ui::TooltipHost();
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void NavBar::RebuildTooltip() {
    if (!hwnd_) return;
    // 走唯一实现（TooltipHost）；宽度按 DPI 缩放，翻页/换盘符的长提示才不会被截断。
    tooltip_.Attach(hwnd_, theme_->S(420));
    struct Tip {
        int id;
        const wchar_t* label;
    };
    static const Tip kTips[] = {
        {kBack, L"后退 (Alt+←)"},
        {kForward, L"前进 (Alt+→)"},
        {kUp, L"向上一级 (Alt+↑)"},
        {kRefresh, L"刷新 (F5)"},
        {kView, L"视图与排序"},
    };
    std::vector<ui::TooltipHost::Item> items;
    for (const auto& t : kTips) {
        for (const auto& b : buttons_) {
            if (b.id != t.id) continue;
            // 灰掉的按钮也要提示 —— 但要说清"为什么点不动"。
            // 只提示动作名会让用户以为程序卡了（按钮画着禁用色却写着"后退"）。
            std::wstring text = t.label;
            if (!b.enabled) text += L"（当前不可用）";
            items.push_back({b.rc, text});
            break;
        }
    }
    tooltip_.SetTools(items);
}

void NavBar::FocusAddress() { BeginEdit(); }


