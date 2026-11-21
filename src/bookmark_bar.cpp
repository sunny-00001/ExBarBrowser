#include "bookmark_bar.h"
#include "str.h"

#include "platform_win.h"

#include "bookmark_drop.h"
#include "bookmark_io.h"
#include "clipboard.h"
#include "commands.h"
#include "config.h"
#include "folder_tree.h"
#include "menu.h"
#include "ui_draw.h"
#include "icon_resolve.h"
#include "mouse_rules.h"
#include "prompt.h"
#include "text_file.h"

#include <shellapi.h>

namespace {

// 类名常量由 identity.h 统一给出（规则 B1：品牌串只允许出现在那里）
using id::kBookmarkBarClass;

}  // namespace

void BookmarkBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kBookmarkBarClass, BookmarkBar::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = win::CreateChild(kBookmarkBarClass, parent, this, theme.S(26), nullptr,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP);
    if (!hwnd_) return;
    DragAcceptFiles(hwnd_, TRUE);
    BmkDropAttach(hwnd_, this);  // OLE 拖放（虚拟项/命名空间/drop-into-folder）
    Reload();
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
            // 只在**可见项**之间走：被溢出折叠的项在栏上没有位置，跳上去会让
            // 焦点框停在空处。要看被折叠的项请用 » （或 End 跳到 »）。
            int cur = kbdIdx_ >= 0 ? kbdIdx_ : (w == VK_RIGHT ? -1 : n);
            const int step = (w == VK_RIGHT) ? 1 : -1;
            for (int i = cur + step; i >= 0 && i < n; i += step) {
                if (items_[static_cast<size_t>(i)].rc.right > items_[static_cast<size_t>(i)].rc.left) {
                    kbdIdx_ = i;
                    break;
                }
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_HOME || w == VK_END) {
            const int step = (w == VK_HOME) ? 1 : -1;
            const int start = (w == VK_HOME) ? 0 : n - 1;
            for (int i = start; i >= 0 && i < n; i += step) {
                if (items_[static_cast<size_t>(i)].rc.right > items_[static_cast<size_t>(i)].rc.left) {
                    kbdIdx_ = i;
                    break;
                }
            }
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
    case WM_CAPTURECHANGED:
        // 捕获被系统抢走（弹出层/切窗口）：拖动状态必须复位，否则残留到下一次点击。
        dragArm_ = dragging_ = false;
        dragIdx_ = dropIdx_ = dropInto_ = -1;
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
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        // 走 bmk.add.path 而不是直接 store.AddPath：
        //   ① 拖放是"人"的一条真实路径，命令必须走同一条（R20 人机同源）；
        //   ② 去重 / 移入分类的语义只写在命令里一份，两边各写一遍必然分叉
        //      （比如"已存在就移进目标分类"只在命令侧实现了）。
        for (UINT i = 0; i < n; ++i) {
            // 先问长度再分配：固定 MAX_PATH*2 会把深路径截断，导致书签存到错路径。
            const UINT need = DragQueryFileW(drop, i, nullptr, 0);
            if (!need) continue;
            std::wstring path(static_cast<size_t>(need) + 1, L'\0');
            if (!DragQueryFileW(drop, i, path.data(), need + 1)) continue;
            path.resize(need);
            DWORD attr = GetFileAttributesW(tf::ToExtended(path).c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
            json::Value a = json::Value::MakeObject();
            a.Set(L"path", json::Value::MakeStr(path));
            CommandBus::Get().Invoke(CmdId::BmkAdd, Args::FromJson(a), Actor::Drop());
        }
        ::DragFinish(drop);  // 成员同名，必须显式全局作用域
        Reload();
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void BookmarkBar::Layout() {
    if (!hwnd_ || !theme_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原），不再各自 SelectObject
    HDC dc = GetDC(hwnd_);

    const int margin = theme_->S(6);
    const int padX = theme_->S(2);
    // 图标格宽必须与 Paint 里画图标的槽完全一致，否则文字会整体错位一格 ——
    // 两处都取同一个令牌，不再各写一个 S(16)。
    const int iconW = theme_->iconSmall();
    const int gap = theme_->S(6);
    const int ovfW = theme_->S(24);
    const int avail = rc.right - margin * 2;

    int total = 0;
    std::vector<int> widths(items_.size(), 0);
    for (size_t i = 0; i < items_.size(); ++i) {
        const std::wstring& t = items_[i].label;
        int textW = ui::MeasureText(dc, theme_->font, t);
        int w = padX * 2 + iconW + gap + textW + (items_[i].folder ? theme_->S(14) : 0);
        widths[i] = w;
        total += w;
        // 只有相邻项之间才有间距；末项之后不计，否则 total 偏大 1px，临界宽度会误判溢出。
        if (i + 1 < items_.size()) total += theme_->S(1);
    }

    overflow_ = total > avail;
    const int viewRight = overflow_ ? (rc.right - margin - ovfW - theme_->S(4)) : (rc.right - margin);
    int x = margin;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (x + widths[i] > viewRight) {
            items_[i].rc = RECT{0, 0, 0, 0};
            items_[i].iconRc = RECT{0, 0, 0, 0};
            continue;
        }
        items_[i].rc = RECT{x, theme_->S(2), x + widths[i], rc.bottom - theme_->S(2)};
        // 图标命中区与 Paint 里画图标的槽完全重合（左侧 padX 起、iconW 宽）。
        // 只有这一格内是"列子目录"，右侧文字区仍是打开/分类菜单。
        items_[i].iconRc = RECT{items_[i].rc.left + padX, items_[i].rc.top,
                                items_[i].rc.left + padX + iconW, items_[i].rc.bottom};
        x += widths[i] + theme_->S(1);
    }
    ovfRc_ = overflow_ ? RECT{rc.right - margin - ovfW, theme_->S(2), rc.right - margin,
                              rc.bottom - theme_->S(2)}
                       : RECT{};

    ReleaseDC(hwnd_, dc);
    RebuildTooltip(hwnd_);
}

void BookmarkBar::RebuildTooltip(HWND h) {
    // 工具提示走唯一实现（TooltipHost）：文本复制与 id 管理都在里面，
    // 不再需要"两趟稳定文本"的手工做法。
    tooltip_.Attach(h, theme_->S(600));
    std::vector<ui::TooltipHost::Item> tips;
    tips.reserve(items_.size());
    for (const auto& it : items_) {
        if (it.rc.right <= it.rc.left) continue;
        tips.push_back({it.rc, it.folder ? (it.label + L"（分类）") : it.path});
    }
    tooltip_.SetTools(tips);
}

void BookmarkBar::Paint(HWND h) {
    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        HGDIOBJ oldFont = SelectObject(mem, theme_->font);

        const int padX = theme_->S(2);
        // 与 Layout 的 iconW 同一令牌（见那里的注释）
        const int iconPx = theme_->iconSmall();

        // 空状态提示：一条书签都没有时，栏是一条什么都没有的灰带。
        // 用户（尤其新用户）看不出"这里是书签栏、可以往里放东西"，
        // 而右键菜单里"添加当前目录"这条唯一能自救的入口完全不可见。
        // 与资源管理器/Opus 的收藏栏一致：给一行浅色提示文本。
        if (items_.empty()) {
            RECT tr{theme_->S(10), 0, rc.right - theme_->S(10), rc.bottom};
            ui::DrawTextRow(mem, theme_->font,
                            L"书签栏（Ctrl+D 收藏当前目录，Ctrl+B 聚焦本栏）", tr, theme_->disabled,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            if (it.rc.right <= it.rc.left) continue;
            const bool draggingThis = dragging_ && dragIdx_ == static_cast<int>(i);
            if (hover_ == static_cast<int>(i) && !draggingThis) FillRect(mem, &it.rc, theme_->brHover);
            // 图标格单独高亮：它是"列子目录"的按钮（等价面包屑分隔符），
            // 与右侧文字区（导航/分类菜单）是两个动作，需让用户看出边界。
            if (hoverIcon_ == static_cast<int>(i) && !draggingThis) {
                RECT ir = it.iconRc;
                InflateRect(&ir, theme_->S(1), 0);
                ui::FillRound(mem, ir, theme_->pressed, theme_->radiusBtn());
            }
            int x = it.rc.left + padX;
            if (it.icon) {
                RECT ir{x, it.rc.top, x + iconPx, it.rc.bottom};
                ui::DrawIconCentered(mem, it.icon, ir, iconPx);
            }
            x += iconPx + theme_->S(6);
            RECT tr{x, it.rc.top, it.rc.right - padX - (it.folder ? theme_->S(14) : 0), it.rc.bottom};
            ui::DrawTextRow(mem, nullptr, it.label, tr, draggingThis ? theme_->sub : theme_->text,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (it.folder) {
                // 与导航栏/标签栏同一个箭头原语：用 chevron 字形 + 主题色，
                // 而不是自绘折线（DPI 下尺寸写死、深色下也不跟随文字色）。
                RECT cr{it.rc.right - padX - theme_->S(14), it.rc.top,
                        it.rc.right - padX - theme_->S(2), it.rc.bottom};
                ui::DrawChevron(mem, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2,
                                theme_->iconChevron(), theme_->sub, 1);
            }
        }

        if (overflow_) {
            if (hotOvf_) FillRect(mem, &ovfRc_, theme_->brHover);
            ui::DrawGlyph(mem, ico::IconId::More, ovfRc_, theme_->iconGlyph(), theme_->sub);  // More：与标签栏的下箭头区分
        }

        if (dragging_ && dropInto_ >= 0 && dropInto_ < static_cast<int>(items_.size())) {            // 拖进分类：整项描边（而不是画插入线）。两种提示必须一眼可分 ——
            // 插入线表示"放到它旁边"，描边表示"放进它里面"。
            RECT r = items_[static_cast<size_t>(dropInto_)].rc;
            if (r.right > r.left) {
                ui::FrameRectColor(mem, r, theme_->accent);
                RECT in = r;
                InflateRect(&in, -1, -1);
                ui::FrameRectColor(mem, in, theme_->accent);
            }
        } else if (dragging_ && dropIdx_ >= 0 &&
                   dropIdx_ <= static_cast<int>(items_.size())) {
            // 插入线：画在 dropIdx_ 位置（项间隙中线或末项右缘），压住分隔线但别盖下边框。
            int gx;
            if (dropIdx_ >= static_cast<int>(items_.size())) {
                gx = items_.empty() ? theme_->S(6) : items_.back().rc.right + theme_->S(1) / 2;
            } else if (dropIdx_ == 0) {
                gx = items_[0].rc.left - theme_->S(1) / 2;
            } else {
                const RECT& a = items_[static_cast<size_t>(dropIdx_ - 1)].rc;
                const RECT& b = items_[static_cast<size_t>(dropIdx_)].rc;
                gx = b.left - theme_->S(1) / 2;
                if (gx < a.right) gx = (a.right + b.left) / 2;  // 间隙被裁剪时取中线
            }
            RECT bar{gx, theme_->S(3), gx + theme_->S(2), rc.bottom - 2};
            ui::FillRectColor(mem, bar, theme_->accent);
        }

        RECT hr{0, rc.bottom - 1, rc.right, rc.bottom - 1};
        ui::DrawSeparator(mem, hr, false, theme_->line);

        // 键盘焦点框：整栏有焦点且落到了某一项。用系统 focus 点线框而不是自绘
        // 矩形，是为了在高对比度/不同 DPI 下都保持一致（这是唯一一处故意
        // 交给系统画的地方 —— 焦点指示是平台约定，不该自创样式）。
        // 只在有焦点时画：否则会和"悬停"看起来一模一样，用户分不清
        // "鼠标在这"和"键盘在这"。
        PaintFocusRing(mem);

        SelectObject(mem, oldFont);
    });
}

void BookmarkBar::PaintFocusRing(HDC dc) const {
    if (!hwnd_ || GetFocus() != hwnd_) return;
    if (kbdIdx_ < 0 || kbdIdx_ >= static_cast<int>(items_.size())) return;
    const RECT& r = items_[static_cast<size_t>(kbdIdx_)].rc;
    if (r.right <= r.left) return;
    RECT fr = r;
    InflateRect(&fr, -theme_->S(1), -theme_->S(1));
    DrawFocusRect(dc, &fr);
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

int BookmarkBar::HitItem(int x, int y) const {
    POINT pt{x, y};
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].rc.right <= items_[i].rc.left) continue;
        if (PtInRect(&items_[i].rc, pt)) return static_cast<int>(i);
    }
    return -1;
}

int BookmarkBar::HitIcon(int x, int y) const {
    POINT pt{x, y};
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].iconRc.right <= items_[i].iconRc.left) continue;
        if (PtInRect(&items_[i].iconRc, pt)) return static_cast<int>(i);
    }
    return -1;
}

bool BookmarkBar::HitOverflow(int x, int y) const {
    if (!overflow_) return false;
    POINT pt{x, y};
    return PtInRect(&ovfRc_, pt) != FALSE;
}

void BookmarkBar::DragUpdate(HWND h, int x, int y) {
    // 依据光标相对可见项的位置决定落点，三种情形：
    //   · 落在某个**分类**的中间区（两侧各留 25% 给"插到它前面/后面"）
    //     → 拖进该分类（与 Opus / TC 的"拖到文件夹上 = 放进文件夹"同语义）；
    //   · 其余位置 → 顶层插入线，超过一项的水平中点就插到它后面；
    //   · 拖出内容区 → 取消（dropIdx_ = dropInto_ = -1）。
    //
    // 只有顶层插入线是不够的：分类本身是个容器，用户把书签拖到分类名上时
    // 预期是"装进去"，而不是"挪到它旁边"。原来没有这条路，分类里的书签
    // 只能靠右键新建或在文件里手改 —— 分类事实上只能读不能组织。
    RECT rc;
    GetClientRect(h, &rc);
    const int margin = theme_->S(6);
    const int viewRight = overflow_ ? (rc.right - margin - ovfRc_.right + ovfRc_.left - theme_->S(4))
                                    : rc.right;
    int newDrop = -1;
    int newInto = -1;
    if (x >= margin && x <= viewRight && y >= 0 && y < rc.bottom) {
        newDrop = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            const RECT& r = items_[i].rc;
            if (r.right <= r.left) continue;
            const int w = r.right - r.left;
            const int side = w / 4;
            if (items_[i].folder && x > r.left + side && x < r.right - side) {
                newInto = static_cast<int>(i);   // 落在分类中间：装进去
                newDrop = -1;
                break;
            }
            if (x > (r.left + r.right) / 2) newDrop = static_cast<int>(i) + 1;
        }
    }
    if (newDrop != dropIdx_ || newInto != dropInto_) {
        dropIdx_ = newDrop;
        dropInto_ = newInto;
        InvalidateRect(h, nullptr, FALSE);
    }
}

void BookmarkBar::DragFinish(HWND h, bool commit) {
    int from = dragIdx_;
    int to = dropIdx_;
    int into = dropInto_;
    dragArm_ = dragging_ = false;
    dragIdx_ = dropIdx_ = dropInto_ = -1;
    if (commit && from >= 0) {
        const Item& src = items_[static_cast<size_t>(from)];
        const std::wstring id = src.id;
        // 走 bmk.move 而不是直接 store.Move/MoveTo：
        // 拖拽重排就是 bmk.move 的人类侧入口（R20），两处各写一份必然分叉。
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(id));
        if (into >= 0 && into != from) {
            a.Set(L"toId", json::Value::MakeStr(items_[static_cast<size_t>(into)].id));
        } else if (to >= 0) {
            // 顶层插入线：toId 省略（= 顶层），toIndex = 插入线位置。
            a.Set(L"toIndex", json::Value::MakeInt(to));
        } else {
            InvalidateRect(h, nullptr, FALSE);
            return;
        }
        if (!CommandBus::Get().Invoke(CmdId::BmkMove, Args::FromJson(a), Actor::Ui())) {
            // MoveTo 会拒绝"拖到自己 / 拖进自己的子孙"—— 那是正确行为，留痕即可。
            LogDbg(L"[bmk] 拒绝移动：%s -> %d (into=%d)", id.c_str(), to, into);
        }
    }
    InvalidateRect(h, nullptr, FALSE);
}

void BookmarkBar::FocusBar() {
    if (!hwnd_) return;
    // 栏被隐藏时先让它可见：热键触发"聚焦书签栏"却什么也没发生是最糟的反馈。
    if (!IsWindowVisible(hwnd_)) {
        CommandBus::Get().InvokeLegacy(CmdId::BmkShowBar, L"", Actor::Ui());
        if (!IsWindowVisible(hwnd_)) return;
    }
    SetFocus(hwnd_);
    // 落在第一个**可见**项（被溢出折叠的项在栏上没有位置）。
    kbdIdx_ = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].rc.right > items_[i].rc.left) {
            kbdIdx_ = static_cast<int>(i);
            break;
        }
    }
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

void BookmarkBar::ShowChildTree(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];

    // 锚点 = 图标格底边（不是鼠标点），与面包屑下拉"对齐到 chevron 区"同口径，
    // 否则菜单会随鼠标在图标格里的位置上下漂。
    POINT at{it.iconRc.left, it.rc.bottom};
    ClientToScreen(hwnd_, &at);

    // 纯分类节点没有路径（只装子书签）→ 没有文件系统子目录可列，
    // 退回原有的"列分类内容"下拉，避免点了图标什么都不发生。
    if (it.path.empty()) {
        ShowFolderMenu(index, at);
        return;
    }

    // 与地址栏分隔符同一份实现：顶层 = 该路径的直接子文件夹，子节点懒加载、就地展开。
    std::vector<TreeNode> top = folder_tree::Children(it.path);
    if (top.empty()) {
        // 没有子目录：空目录直接进去（与 mod 的 ShowFsMenuForPath 同口径 ——
        // 弹一个空菜单等于点了没反应）。文件类书签保持原行为，不动作。
        const DWORD attr = GetFileAttributesW(tf::ToExtended(it.path).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && onNavigate_)
            onNavigate_(it.path);
        return;
    }
    // toggleKey 用书签 id（不是下标）：重排之后同一个书签的下标会变，用下标会让
    // "连点两下"在重排过的栏上判成两个不同锚点。带前缀与面包屑的键区分开。
    const std::wstring toggleKey = L"bmk-icon:" + it.id;
    std::wstring picked =
        folder_tree::Show(hwnd_, *theme_, at, std::move(top), 0, toggleKey.c_str());

    // 弹层是模态的，回来时"光标下高亮"已经不成立，清掉避免残留
    hoverIcon_ = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (!picked.empty() && onNavigate_) {
        onNavigate_(picked);
        if (refocus_) refocus_();   // 同 Navigate：打开目录后焦点必须回视图
    }
}

void BookmarkBar::ShowFolderMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    BookmarkStore& store = BookmarkStore::Get();
    std::wstring folderId = items_[static_cast<size_t>(index)].id;

    // 【逐级下钻，而不是把整棵子树拍平加缩进】
    //
    // 原实现把所有后代一次性列进同一个菜单，用 `depth * 2` 个空格表示层级。
    // 两个问题：
    //   · 一个有几层结构的书签集直接变成几十行的长菜单（弹出层上限 20 行，
    //     剩下的要靠滚动条翻 —— 在菜单里滚动找目录是很糟的体验）；
    //   · 两个空格的缩进在 24px 行高里几乎看不出来，"分类 A 里的 x" 和
    //     "顶层的 x" 长得一样。
    // 现在只列**直接子项**，点分类再开下一级 —— 与资源管理器/浏览器一致，
    // 每一级都是短菜单，层级靠"路径"本身表达而不是靠空格。
    for (int guard = 0; guard < 32; ++guard) {   // 深度上限，防数据自引用成死循环
        const BookmarkNode* node = store.Find(folderId);
        if (!node) return;

        ui::Menu menu;
        std::vector<std::wstring> ids;      // action -> 节点 id（分类下钻用）
        std::vector<std::wstring> paths;    // action -> 导航路径
        std::vector<bool> folders;          // action -> 是否分类
        for (const auto& n : node->children) {
            const int action = static_cast<int>(ids.size());
            if (n.isFolder) {
                // 分类行用自身路径取图标；纯分类没有路径时由组件退回 Folder 字形。
                menu.AddPath(n.name, n.path, action, true);
                folders.push_back(true);
                paths.push_back(n.path);
            } else if (!n.path.empty()) {
                menu.AddPath(n.name, n.path, action);
                folders.push_back(false);
                paths.push_back(n.path);
            } else {
                continue;
            }
            ids.push_back(n.id);
        }
        if (menu.Empty()) return;

        // 锚点键带层级：下钻后的菜单是一个**新的**锚点，不能沿用上一级的 key
        // （否则"再点即收"会把下一级当成上一级的重开而直接吞掉）。
        const std::wstring toggleKey = L"bmk-folder:" + folderId;
        ui::MenuOptions opt;
        opt.toggleKey = toggleKey.c_str();
        const int pick = menu.Show(hwnd_, *theme_, screen, opt);
        if (pick < 0 || pick >= static_cast<int>(ids.size())) return;

        if (folders[static_cast<size_t>(pick)]) {
            // 分类：下钻一级，同一个锚点位置继续弹。模态循环已退出，可以安全再进。
            folderId = ids[static_cast<size_t>(pick)];
            continue;
        }
        if (!paths[static_cast<size_t>(pick)].empty() && onNavigate_)
            onNavigate_(paths[static_cast<size_t>(pick)]);
        return;
    }
}

int BookmarkBar::FolderItemAt(POINT screen) const {
    POINT pt = screen;
    if (!hwnd_) return -1;
    POINT cl = pt;
    ScreenToClient(hwnd_, &cl);
    return HitItem(cl.x, cl.y);
}

std::wstring BookmarkBar::FolderIdAt(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return std::wstring();
    const Item& it = items_[static_cast<size_t>(index)];
    if (!it.folder) return std::wstring();  // 只有文件夹书签能作为落点容器
    return it.id;
}

void BookmarkBar::ReloadExternal() { Reload(); }

void BookmarkBar::ShowOverflowMenu(POINT screen) {
    // 【只列被折叠的项】原实现把**全部**项都塞进溢出菜单，包括已经显示在栏上的
    // 那些。于是"点了 » 看到一个书签，再点一次栏上同一个书签"完全等价 ——
    // 溢出菜单没有存在的意义，还让用户以为自己数错了。
    // 溢出菜单的唯一职责是"把看不见的拿出来"，所以这里必须按可见性过滤。
    ui::Menu menu;
    std::vector<int> map;   // action -> items_ 下标
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].rc.right > items_[i].rc.left) continue;   // 已在栏上可见
        menu.AddPath(items_[i].label, items_[i].path, static_cast<int>(map.size()),
                     items_[i].folder);
        map.push_back(static_cast<int>(i));
    }
    if (menu.Empty()) return;

    ui::MenuOptions opt;
    opt.maxRows = 24;
    opt.toggleKey = L"bmk.overflow";
    const int slot = menu.Show(hwnd_, *theme_, screen, opt);
    if (slot < 0 || slot >= static_cast<int>(map.size())) return;
    const int idx = map[static_cast<size_t>(slot)];
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return;

    if (items_[static_cast<size_t>(idx)].folder) {
        // 锚点取溢出按钮本身：分类的菜单从 » 下方展开，而不是从屏幕那个
        // 已经不在栏上的坐标展开（否则菜单会浮在半空，与鼠标位置无关）。
        ShowFolderMenu(idx, screen);
    } else {
        Navigate(idx);
    }
}

void BookmarkBar::ShowItemMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];
    BookmarkStore& store = BookmarkStore::Get();
    ui::Menu menu;
    menu.Add(L"打开", 1, it.folder ? ico::IconId::Folder : ico::IconId::Page)
        .Add(L"复制路径", 2, ico::IconId::Copy, it.folder)
        .Separator()
        .Add(L"编辑...", 3, ico::IconId::Edit)
        .Add(L"复制书签", 9, ico::IconId::Copy)
        .Add(L"删除", 4, ico::IconId::Delete)
        .Separator()
        .Add(L"新建分类", 5, ico::IconId::Add)
        .Add(L"添加当前目录", 6, ico::IconId::Folder)
        .Separator()
        .Add(L"撤销删除", 7, ico::IconId::Undo, !store.CanUndo())
        .Add(L"清除全部书签", 8, ico::IconId::Delete);

    // 右键菜单属于某一项：键带上 id，不同项的右键菜单互不当成连点。
    const std::wstring toggleKey = L"bmk-menu:" + it.id;
    ui::MenuOptions opt;
    opt.toggleKey = toggleKey.c_str();
    const int action = menu.Show(hwnd_, *theme_, screen, opt);
    if (action < 0) return;
    switch (action) {
    case 1:
        // 只有分类才有"打开"这一步可选；普通书签点"打开"等同导航。
        if (it.folder)
            ShowFolderMenu(index, screen);
        else
            Navigate(index);
        break;
    case 2:
        // 剪贴板写入唯一实现（clipboard.cpp）。失败要留痕 ——
        // 否则用户"点了复制、粘贴出旧内容"时无从判断是复制没成功。
        if (!it.path.empty() && !SetClipboardText(hwnd_, it.path))
            LogDbg(L"[bmk] 复制路径失败（剪贴板被占用？）：%s", it.path.c_str());
        break;
    case 3: {
        // 编辑：名称 + 路径双字段（纯分类节点只有名称一项可改）。
        //
        // 空路径的语义：Update(id, name, L"") 里 L"" 表示"这一项不改"，于是
        // 用户把路径清空想改成纯分类时会被静默保留。所以这里走命令 bmk.edit
        // 并**显式**带 path —— 命令侧用 SetNameAndPath（空串就是清空）。
        std::wstring name = it.label;
        std::wstring path = it.path;
        bool confirmed;
        if (it.folder && it.path.empty()) {
            confirmed = PromptText(hwnd_, *theme_, L"编辑", L"名称", it.label, &name);
        } else {
            confirmed = PromptText2(hwnd_, *theme_, L"编辑", L"名称", it.label, &name, L"路径",
                                    it.path, &path);
        }
        if (!confirmed) break;
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(it.id));
        a.Set(L"name", json::Value::MakeStr(name));
        // 分类无路径时不要传 path（否则会把"没有路径"改成"路径是空串"，
        // 对 BookmarkStore 来说两者不同：后者会显式清掉 path）。
        if (!it.folder || !path.empty()) a.Set(L"path", json::Value::MakeStr(path));
        CommandBus::Get().Invoke(CmdId::BmkEdit, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 4:
        if (MessageBoxW(hwnd_, (L"删除书签“" + it.label + L"”？").c_str(), id::kBrandShort,
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(it.id));
            CommandBus::Get().Invoke(CmdId::BmkRemove, Args::FromJson(a), Actor::Ui());
        }
        break;
    case 5: {
        std::wstring name;
        if (PromptText(hwnd_, *theme_, L"新建分类", L"分类名称", L"新建分类", &name)) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"name", json::Value::MakeStr(name));
            CommandBus::Get().Invoke(CmdId::BmkNewFolder, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case 6:
        CommandBus::Get().InvokeLegacy(CmdId::BmkAddCurrent, L"", Actor::Ui());
        break;
    case 7:  // 撤销：恢复上一次删除/重排/清空前的快照（store 内部维护，≤20 步）
        CommandBus::Get().Invoke(CmdId::BmkUndo, Args::Empty(), Actor::Ui());
        break;
    case 8:  // 清除全部书签：可撤销（ReplaceAll 已入快照栈）
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", id::kBrandShort,
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            CommandBus::Get().Invoke(CmdId::BmkClearAll, Args::Empty(), Actor::Ui());
        }
        break;
    case 9: {  // 复制书签（分类连子树）：与 bmk.copy 同一命令，AI/面板可达同一动作
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(it.id));
        CommandBus::Get().Invoke(CmdId::BmkCopy, Args::FromJson(a), Actor::Ui());
        break;
    }
    default:
        break;
    }
}

void BookmarkBar::ShowEmptyMenu(POINT screen) {
    BookmarkStore& store = BookmarkStore::Get();
    ui::Menu menu;
    menu.Add(L"添加当前目录为书签", 1, ico::IconId::Add)
        .Add(L"收藏所有标签页", 11, ico::IconId::Add)
        .Add(L"新建分类", 2, ico::IconId::Add)
        .Add(L"撤销删除", 8, ico::IconId::Undo, !store.CanUndo())
        .Separator()
        .Add(L"按名称排序（含分类内）", 3, ico::IconId::Sort)
        .Add(L"清理重复书签", 10, ico::IconId::Delete)
        .Add(L"重新载入书签文件", 4, ico::IconId::Refresh)
        .Add(L"导出书签...", 6, ico::IconId::Export)
        .Add(L"导入书签...", 7, ico::IconId::Import)
        .Separator()
        .Add(L"清除全部书签", 9, ico::IconId::Delete)
        .Add(L"隐藏书签栏", 5, ico::IconId::Eye);

    ui::MenuOptions opt;
    opt.toggleKey = L"bmk.empty";
    const int action = menu.Show(hwnd_, *theme_, screen, opt);
    if (action < 0) return;
    switch (action) {
    case 1:
        CommandBus::Get().InvokeLegacy(CmdId::BmkAddCurrent, L"", Actor::Ui());
        break;
    case 11: {  // 收藏所有打开标签页（与 bmk.add.tabs 同一命令）
        // 与 dedupe 同款反馈：批量动作必须报数，不能静默。
        json::Value result;
        const bool ok = CommandBus::Get().InvokeResult(CmdId::BmkAddTabs, Args::Empty(), &result, Actor::Ui());
        const int added = ok ? result.GetInt(L"added", 0) : -1;
        const int skipped = ok ? result.GetInt(L"skipped", 0) : 0;
        if (added > 0)
            MessageBoxW(hwnd_,
                        (L"已收藏 " + std::to_wstring(added) + L" 个标签页目录" +
                         (skipped > 0 ? (L"（跳过 " + std::to_wstring(skipped) +
                                         L" 个：已收藏或空标签）") : L"")).c_str(),
                        L"收藏所有标签页", MB_OK | MB_ICONINFORMATION);
        else if (added == 0 && skipped > 0)
            MessageBoxW(hwnd_,
                        (L"没有新增（" + std::to_wstring(skipped) +
                         L" 个标签页目录均已收藏或无目录）").c_str(),
                        L"收藏所有标签页", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case 2: {
        std::wstring name;
        if (PromptText(hwnd_, *theme_, L"新建分类", L"分类名称", L"新建分类", &name)) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"name", json::Value::MakeStr(name));
            CommandBus::Get().Invoke(CmdId::BmkNewFolder, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case 3:
        // 递归排序（含分类内部）。原实现只排顶层 —— 用户点"按名称排序"后
        // 分类里的顺序纹丝不动，那不是排序，只排了一半。排序入撤销栈（可能误触）。
        CommandBus::Get().Invoke(CmdId::BmkSort, Args::Empty(), Actor::Ui());
        break;
    case 10: {
        const size_t before = store.Flatten().size();
        CommandBus::Get().Invoke(CmdId::BmkDedupe, Args::Empty(), Actor::Ui());
        const size_t after = store.Flatten().size();
        if (after < before) {
            MessageBoxW(hwnd_,
                        (L"已删除 " + std::to_wstring(before - after) +
                         L" 条重复书签（可撤销）").c_str(),
                        L"清理重复书签", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd_, L"没有发现重复书签。", L"清理重复书签",
                        MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case 4:
        ReloadFromDisk();
        break;
    case 8:
        CommandBus::Get().Invoke(CmdId::BmkUndo, Args::Empty(), Actor::Ui());
        break;
    case 9:
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", id::kBrandShort,
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            CommandBus::Get().Invoke(CmdId::BmkClearAll, Args::Empty(), Actor::Ui());
        }
        break;
    case 6: {
        int n = BmkExportHtml(hwnd_, *theme_);
        if (n == -2)
            MessageBoxW(hwnd_, L"写入文件失败", L"导出书签", MB_OK | MB_ICONWARNING);
        else if (n >= 0)
            MessageBoxW(hwnd_, (L"已导出 " + std::to_wstring(n) + L" 个书签").c_str(), L"导出书签",
                        MB_OK | MB_ICONINFORMATION);
        break;
    }
    case 7: {
        int n = BmkImportHtml(hwnd_, *theme_);
        if (n > 0) {
            Reload();
            MessageBoxW(hwnd_, (L"已导入 " + std::to_wstring(n) + L" 个书签").c_str(), L"导入书签",
                        MB_OK | MB_ICONINFORMATION);
        } else if (n == -2) {
            MessageBoxW(hwnd_, L"文件无效或无法解析", L"导入书签", MB_OK | MB_ICONWARNING);
        }
        break;
    }
    case 5:
        CommandBus::Get().InvokeLegacy(CmdId::BmkToggleBar, L"", Actor::Ui());
        break;
    default:
        break;
    }
}
