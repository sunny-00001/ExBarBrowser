#include "bookmark_bar.h"
#include "str.h"

#include "platform_win.h"

#include "bookmark_drop.h"
#include "commands.h"
#include "config.h"
#include "ui_draw.h"
#include "icon_resolve.h"
#include "mouse_rules.h"
#include "settings_bounds.h"
#include "text_file.h"

#include <algorithm>
#include <shellapi.h>

namespace {

// 类名常量由 identity.h 统一给出（规则 B1：品牌串只允许出现在那里）
using id::kBookmarkBarClass;

}  // namespace

void BookmarkBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kBookmarkBarClass, BookmarkBar::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = win::CreateChild(kBookmarkBarClass, parent, this, theme.barBookmark(), nullptr,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP);
    if (!hwnd_) return;
    DragAcceptFiles(hwnd_, TRUE);
    BmkDropAttach(hwnd_, this);  // OLE 拖放（虚拟项/命名空间/drop-into-folder）
    // 这里不 Reload：Create 发生在 BookmarkStore::Load 之前，BuildItems 只会
    // 拿到空表白跑一遍；App::OnCreate 读盘后显式 Reload 一次即可。
}

BookmarkBar::~BookmarkBar() {
    BmkDropDetach(hwnd_);  // 先注销 OLE drop target（引用回调和本对象）
    // 不再销毁图标：句柄由 ico::Cached 持有到进程结束（见 IconFor 的说明）。
    // 在这里 DestroyIcon 会连带毁掉别的栏正在用的同一枚图标。
}

void BookmarkBar::ReloadFromDisk() {
    BookmarkStore::Get().Load(Config::Get().BookmarksPath());
    Reload();
}

void BookmarkBar::Reload() {
    // 图标已改为进程级共享缓存，重排只需重建条目（不再销毁/重建 HICON）。
    BuildItems();
    if (hwnd_) {
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void BookmarkBar::ReloadTheme() {
    if (!hwnd_) return;
    // 提示窗宽度依赖 DPI/主题 → 重建（TooltipHost 析构会销毁旧窗）
    tooltip_ = ui::TooltipHost();
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

HICON BookmarkBar::IconFor(const BookmarkNode& n) {
    // 走 ico::Cached（进程内按路径共享，句柄归 icon_resolve 持有到进程结束）。
    //
    // 【为什么不再自带一份 icons_ 缓存】本类原来自建 std::map<路径,HICON>，
    // 在 Reload/析构里逐个 DestroyIcon。而地址栏、标签栏、弹层早已统一走
    // ico::Cached —— 于是同一个目录的图标有两份句柄、两种所有权：
    //   · 本类销毁的是自己那份，看起来正确；
    //   · 但每 Reload 一次就重新 Resolve 一遍全部可见项（Reload 很频繁：
    //     加/删/排序/拖放都调），每次都真去问 shell 要图标 —— 大书签集下
    //     明显卡顿，且白白多一批 GDI 句柄。
    //   · 分类无路径时用的 StockIcon 是另一个坑：它每次返回新句柄，
    //     混在同一张表里靠"路径 key"去重，key 相同的两个无路径分类会共用。
    // 统一到 Cached 后：一份缓存、一份所有权、Reload 变成纯查表。
    if (n.isFolder && n.path.empty()) {
        // 纯分类没有路径：Cached 的 key 是路径，空串会被拒绝。
        // 给一个稳定的伪路径让缓存生效（同一伪 key 全进程共用一个句柄）。
        static HICON folderIcon = nullptr;
        if (!folderIcon) folderIcon = ui::StockIcon(SIID_FOLDER, true);
        return folderIcon;
    }
    return ico::Cached(n.path, true);
}

void BookmarkBar::BuildItems() {
    items_.clear();
    for (const auto& n : BookmarkStore::Get().roots()) {
        if (!n.isFolder && n.path.empty()) continue;
        Item it;
        it.id = n.id;
        // 历史数据里可能有旧版自动生成的裸 "::{GUID}" 名字：它不是用户改名，
        // 只是当时没有统一显示名入口。渲染时按当前系统语言补正；用户改名保留。
        std::wstring label = n.name;
        if (!n.path.empty() && nav::IsNamespace(n.path) &&
            (label.empty() || label == n.path || nav::IsNamespace(label)))
            label = ico::DisplayName(n.path);
        it.label = label;
        it.path = n.path;
        it.folder = n.isFolder;
        it.icon = IconFor(n);
        items_.push_back(std::move(it));
    }
}

LRESULT CALLBACK BookmarkBar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<BookmarkBar>(h);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT BookmarkBar::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ >= 0 || hotOvf_ || hoverIcon_ >= 0) {
            hover_ = -1;
            hotOvf_ = false;
            hoverIcon_ = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    // ---------------------------------------------------------------- 键盘
    //
    // 与标签栏同一套做法：本栏可获得焦点（WS_TABSTOP + Tab 键到达），
    // 焦点在栏上时用裸键移动 kbdIdx_，Enter 打开，Shift+F10 出菜单，Esc 退回视图。
    // 以前这里一个键盘消息都不处理 —— 只能用鼠标点书签，键盘用户完全被挡住。
    case WM_SETFOCUS:
        if (kbdIdx_ < 0 && !items_.empty()) kbdIdx_ = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        kbdIdx_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        // 声明要方向键/Enter/Esc：否则 Tab 之外的键会被外层当成对话框导航键吃掉。
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTALLKEYS;
    case WM_KEYDOWN: {
        const int n = static_cast<int>(items_.size());
        if (n == 0) {
            if (w == VK_ESCAPE && refocus_) refocus_();
            return 0;
        }
        if (w == VK_ESCAPE) {
            kbdIdx_ = -1;
            InvalidateRect(h, nullptr, FALSE);
            if (refocus_) refocus_();   // 焦点还给视图，Delete / type-ahead 才可用
            return 0;
        }
        if (w == VK_LEFT || w == VK_RIGHT) {
            // 走遍**全部**项。以前放不下的项被折叠、栏上没有位置，
            // 键盘只能停在折叠项之前，看它们得用 » 菜单 —— 键盘用户等于摸不到。
            // 现在项都有位置，方向键走过去时顺手把栏滚到那一项（EnsureVisible），
            // 焦点框不会停在看不见的地方。
            const int step = (w == VK_RIGHT) ? 1 : -1;
            const int cur = kbdIdx_ >= 0 ? kbdIdx_ : (step > 0 ? -1 : n);
            int next = cur + step;
            if (next < 0) next = 0;
            if (next >= n) next = n - 1;
            kbdIdx_ = next;
            EnsureVisible(next);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_HOME || w == VK_END) {
            kbdIdx_ = (w == VK_HOME) ? 0 : n - 1;
            EnsureVisible(kbdIdx_);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_RETURN || w == VK_SPACE) {
            if (kbdIdx_ >= 0 && kbdIdx_ < n) {
                POINT at{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                         items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
                ClientToScreen(h, &at);
                if (items_[static_cast<size_t>(kbdIdx_)].folder)
                    ShowFolderMenu(kbdIdx_, at);
                else
                    Navigate(kbdIdx_);
            }
            return 0;
        }
        if (w == VK_APPS || (w == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
            // 无障碍要求：图标/鼠标能做的，键盘也要能做到（等同右键）。
            if (kbdIdx_ >= 0 && kbdIdx_ < n) {
                POINT at{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                         items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
                ClientToScreen(h, &at);
                ShowItemMenu(kbdIdx_, at);
            }
            return 0;
        }
        break;
    }
    case WM_CONTEXTMENU: {
        // Shift+F10 也可能被系统转成 WM_CONTEXTMENU（lParam = -1,-1）。
        const int sx = GET_X_LPARAM(l), sy = GET_Y_LPARAM(l);
        POINT at{sx, sy};
        if (sx == -1 && sy == -1) {
            if (kbdIdx_ < 0 || kbdIdx_ >= static_cast<int>(items_.size())) return 0;
            at = POINT{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                       items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
            ClientToScreen(h, &at);
        }
        POINT cl = at;
        ScreenToClient(h, &cl);
        const int idx = HitItem(cl.x, cl.y);
        if (idx >= 0)
            ShowItemMenu(idx, at);
        else
            ShowEmptyMenu(at);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: {
        // 双击/右键按下维持原 sink 转发（拖动只走左键）。
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int idx = HitItem(x, y);
        int target = idx >= 0 ? (items_[static_cast<size_t>(idx)].folder ? MT_BOOKMARK_FOLDER
                                                                        : MT_BOOKMARK)
                              : (HitOverflow(x, y) ? MT_OVERFLOW : MT_BOOKMARK_EMPTY);
        int button = (m == WM_LBUTTONDBLCLK) ? MBTN_LEFT : MBTN_RIGHT;
        if (sink_ && sink_(target, m, button, CurrentMods(), pt)) return 0;
        return 0;
    }
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        // 书签栏侧键：按命中项算 target（书签/文件夹/空白/溢出），按钮编号
        // 从 wParam 解析（与 shell_input 同口径），交给规则引擎触发前进/后退。
        const int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        const int idx = HitItem(x, y);
        const int target = idx >= 0 ? (items_[static_cast<size_t>(idx)].folder
                                           ? MT_BOOKMARK_FOLDER
                                           : MT_BOOKMARK)
                                    : (HitOverflow(x, y) ? MT_OVERFLOW : MT_BOOKMARK_EMPTY);
        const int button = XButtonFromWParam(w);
        if (button < 0) return 0;
        if (sink_ && sink_(target, m, button, CurrentMods(), pt)) return 0;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int idx = HitItem(x, y);
        int target = idx >= 0 ? (items_[static_cast<size_t>(idx)].folder ? MT_BOOKMARK_FOLDER
                                                                        : MT_BOOKMARK)
                              : (HitOverflow(x, y) ? MT_OVERFLOW : MT_BOOKMARK_EMPTY);
        if (sink_ && sink_(target, m, MBTN_LEFT, CurrentMods(), pt)) {
            dragArm_ = false;
            return 0;
        }
        // 鼠标规则未接管：左键按下可见项 → 准备拖动重排（超阈值才算拖）。
        // 图标格例外：那里是"列子目录"的独立动作，按下不预备拖动 ——
        // 否则在图标上稍微一拖就变成重排书签，与点击语义打架。
        const bool onIcon = idx >= 0 && HitIcon(x, y) == idx;
        // 点击也把键盘焦点带到本栏并把 kbdIdx_ 对齐到点中的项：
        // 用户点完书签后按 ←/→ 能接着用键盘走，不会"焦点还在视图里"。
        SetFocus(h);
        kbdIdx_ = idx;
        dragArm_ = idx >= 0 && !onIcon;
        dragIdx_ = idx;
        dragStart_ = {x, y};
        dropIdx_ = idx;
        dropInto_ = -1;
        dropPreview_ = false;
        dragging_ = false;
        if (dragArm_) SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        if (dragArm_ && !dragging_ && dragIdx_ >= 0) {
            int dx = GET_X_LPARAM(l) - dragStart_.x;
            int dy = GET_Y_LPARAM(l) - dragStart_.y;
            if (dx * dx + dy * dy > 6 * 6) dragging_ = true;  // 6px 阈值，防手抖误重排
        }
        if (dragging_) {
            DragUpdate(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        }
        UpdateHover(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        // 滚轮：先问鼠标规则（用户可能把"书签栏上滚滚轮"绑成别的动作），
        // 规则没接管才走默认的横向滚动（把后面的书签带进视野）。
        if (OnWheel(m, w, l)) return 0;
        break;
    case WM_CAPTURECHANGED:
        // 捕获被系统抢走（弹出层/切窗口）：拖动状态必须复位，否则残留到下一次点击。
        dragArm_ = dragging_ = false;
        dragIdx_ = dropIdx_ = dropInto_ = -1;
        dropPreview_ = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        if (dragArm_ || dragging_) {
            // ReleaseCapture 会同步派发 WM_CAPTURECHANGED（把 dragIdx_/dropIdx_ 清掉），
            // 所以必须先提交拖动、再释放捕获，顺序不能反。
            if (dragging_) {
                DragFinish(h, true);
                ReleaseCapture();
                return 0;
            }
            dragArm_ = false;
            dragIdx_ = dropIdx_ = dropInto_ = -1;
            dropPreview_ = false;
            ReleaseCapture();
        }
        int idx = HitItem(x, y);
        int target = idx >= 0 ? (items_[static_cast<size_t>(idx)].folder ? MT_BOOKMARK_FOLDER
                                                                        : MT_BOOKMARK)
                              : (HitOverflow(x, y) ? MT_OVERFLOW : MT_BOOKMARK_EMPTY);
        // UP 必须尊重 sink 的返回值：鼠标规则接管（"none"/命令）之后不能再执行默认导航。
        // 原来只调不判，规则命中时同一次点击会既跑命令又导航（双重触发）。
        if (sink_ && sink_(target, m, MBTN_LEFT, CurrentMods(), pt)) return 0;
        if (idx >= 0 && HitIcon(x, y) == idx) {
            // 图标格：与地址栏分隔符同一个动作 —— 列这一级的子目录，可就地逐级展开。
            // 分类项同样适用（分类绑了路径就列那个路径的子目录，没路径退回分类菜单）。
            ShowChildTree(idx);
            return 0;
        }
        if (idx >= 0) {
            if (items_[static_cast<size_t>(idx)].folder)
                ShowFolderMenu(idx, pt);
            else
                Navigate(idx);
            return 0;
        }
        if (HitOverflow(x, y)) ShowOverflowMenu(pt);
        return 0;
    }
    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt{x, y};
        ClientToScreen(h, &pt);
        int idx = HitItem(x, y);
        if (sink_ &&
            sink_(idx >= 0 ? MT_BOOKMARK : MT_BOOKMARK_EMPTY, m, MBTN_RIGHT, CurrentMods(), pt))
            return 0;
        if (idx >= 0)
            ShowItemMenu(idx, pt);
        else
            ShowEmptyMenu(pt);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        // 老式 CF_HDROP 路径只在 Drop 时到达；OLE 路径在 bookmark_drop.cpp
        // 里有 DragOver 预览。两条路共用 DropSpotAt / ApplyDroppedPaths，
        // 保证"插到哪、放进哪个分类"的语义一致。
        POINT client{};
        DragQueryPoint(drop, &client);
        POINT screen = client;
        ClientToScreen(h, &screen);
        const BookmarkDropSpot spot = DropSpotAt(screen);
        PreviewDrop(spot);

        std::vector<std::wstring> paths;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            // 先问长度再分配：固定 MAX_PATH*2 会把深路径截断，导致书签存到错路径。
            const UINT need = DragQueryFileW(drop, i, nullptr, 0);
            if (!need) continue;
            std::wstring path(static_cast<size_t>(need) + 1, L'\0');
            if (!DragQueryFileW(drop, i, path.data(), need + 1)) continue;
            path.resize(need);
            const DWORD attr = GetFileAttributesW(tf::ToExtended(path).c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
            paths.push_back(std::move(path));
        }
        ApplyDroppedPaths(paths, spot);
        ClearDropPreview();
        ::DragFinish(drop);  // 成员同名，必须显式全局作用域
        Reload();
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void BookmarkBar::UpdateHover(HWND h, int x, int y) {
    int idx = HitItem(x, y);
    bool ovf = HitOverflow(x, y);
    int iconIdx = (idx >= 0 && HitIcon(x, y) == idx) ? idx : -1;
    if (idx != hover_ || ovf != hotOvf_ || iconIdx != hoverIcon_) {
        hover_ = idx;
        hotOvf_ = ovf;
        hoverIcon_ = iconIdx;
        InvalidateRect(h, nullptr, FALSE);
    }
}

void BookmarkBar::FocusBar() {
    if (!hwnd_) return;
    // 栏被隐藏时先让它可见：热键触发"聚焦书签栏"却什么也没发生是最糟的反馈。
    if (!IsWindowVisible(hwnd_)) {
        CommandBus::Get().InvokeLegacy(CmdId::BmkShowBar, L"", Actor::Ui());
        if (!IsWindowVisible(hwnd_)) return;
    }
    SetFocus(hwnd_);
    // 落在第一个**可见**项（滚出视野的项焦点框会停在看不见的地方）。
    kbdIdx_ = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (BmkInView(items_[i].rc, viewRight_)) {
            kbdIdx_ = static_cast<int>(i);
            break;
        }
    }
    if (kbdIdx_ < 0 && !items_.empty()) kbdIdx_ = 0;
    EnsureVisible(kbdIdx_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void BookmarkBar::Navigate(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const std::wstring& p = items_[static_cast<size_t>(index)].path;
    if (p.empty() || !onNavigate_) return;
    // 用键盘（或鼠标）在书签栏上打开一个目录后，焦点必须回到视图。
    // 否则焦点留在书签栏窗口上：Delete / F2 / type-ahead / 方向键全部落到栏上，
    // 用户看到的是"进了目录但键盘失灵"。导航栏结束编辑时也是同一条纪律。
    onNavigate_(p);
    if (refocus_) refocus_();
}

void BookmarkBar::ReloadExternal() { Reload(); }
