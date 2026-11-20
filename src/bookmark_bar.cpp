#include "bookmark_bar.h"

#include "platform_win.h"

#include "bookmark_drop.h"
#include "bookmark_io.h"
#include "clipboard.h"
#include "commands.h"
#include "config.h"
#include "eb_ui.h"
#include "icon_resolve.h"
#include "mouse_rules.h"
#include "popup_list.h"
#include "prompt.h"

#include <shellapi.h>

namespace {

// 类名常量（规则 R10：外部契约要能 grep 到，不能散在 CreateWindow 里）
constexpr wchar_t kBookmarkBarClass[] = L"ExBarBrowser_BookmarkBar";

}  // namespace

void BookmarkBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kBookmarkBarClass, BookmarkBar::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = CreateWindowExW(0, kBookmarkBarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100,
                            theme.S(26), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return;
    SetThis(hwnd_, this);
    DragAcceptFiles(hwnd_, TRUE);
    BmkDropAttach(hwnd_, this);  // OLE 拖放（虚拟项/命名空间/drop-into-folder）
    Reload();
}

BookmarkBar::~BookmarkBar() {
    BmkDropDetach(hwnd_);  // 先注销 OLE drop target（引用回调和本对象）
    for (auto& kv : icons_)
        if (kv.second) DestroyIcon(kv.second);
    icons_.clear();
}

void BookmarkBar::ReloadFromDisk() {
    BookmarkStore::Get().Load(Config::Get().BookmarksPath());
    Reload();
}

void BookmarkBar::Reload() {
    for (auto& kv : icons_)
        if (kv.second) DestroyIcon(kv.second);
    icons_.clear();
    BuildItems();
    if (hwnd_) {
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void BookmarkBar::ReloadTheme() {
    if (!hwnd_) return;
    // 提示窗宽度依赖 DPI/主题 → 重建（TooltipHost 析构会销毁旧窗）
    tooltip_ = eb::TooltipHost();
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

HICON BookmarkBar::IconFor(const BookmarkNode& n) {
    // 分类也可能带路径（绑定某个目录的分类）。只用 "<folder>" 当 key 会让所有
    // 分类共用第一次解析出来的图标。
    std::wstring key = n.isFolder ? (L"<folder>:" + n.path) : n.path;
    auto it = icons_.find(key);
    if (it != icons_.end()) return it->second;
    HICON icon = nullptr;
    if (n.isFolder && n.path.empty()) {
        // 书签"分类"节点无路径：给系统文件夹图标（与 mod 一致）。
        icon = eb::StockIcon(SIID_FOLDER, true);
    } else {
        // 统一图标识别：此电脑/回收站/侧边栏/云存储等命名空间取准确图标。
        icon = ico::Resolve(n.path, true);
    }
    icons_[key] = icon;
    return icon;
}

void BookmarkBar::BuildItems() {
    items_.clear();
    for (const auto& n : BookmarkStore::Get().roots()) {
        if (!n.isFolder && n.path.empty()) continue;
        Item it;
        it.id = n.id;
        it.label = n.name;
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
        if (hover_ >= 0 || hotOvf_) {
            hover_ = -1;
            hotOvf_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
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
        if (sink_ && sink_(target, m, button, eb::CurrentMods(), pt)) return 0;
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
        if (sink_ && sink_(target, m, MBTN_LEFT, eb::CurrentMods(), pt)) {
            dragArm_ = false;
            return 0;
        }
        // 鼠标规则未接管：左键按下可见项 → 准备拖动重排（超阈值才算拖）
        dragArm_ = idx >= 0;
        dragIdx_ = idx;
        dragStart_ = {x, y};
        dropIdx_ = idx;
        dragging_ = false;
        if (idx >= 0) SetCapture(h);
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
        dragIdx_ = dropIdx_ = -1;
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
            dragIdx_ = dropIdx_ = -1;
            ReleaseCapture();
        }
        int idx = HitItem(x, y);
        int target = idx >= 0 ? (items_[static_cast<size_t>(idx)].folder ? MT_BOOKMARK_FOLDER
                                                                        : MT_BOOKMARK)
                              : (HitOverflow(x, y) ? MT_OVERFLOW : MT_BOOKMARK_EMPTY);
        // UP 必须尊重 sink 的返回值：鼠标规则接管（"none"/命令）之后不能再执行默认导航。
        // 原来只调不判，规则命中时同一次点击会既跑命令又导航（双重触发）。
        if (sink_ && sink_(target, m, MBTN_LEFT, eb::CurrentMods(), pt)) return 0;
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
            sink_(idx >= 0 ? MT_BOOKMARK : MT_BOOKMARK_EMPTY, m, MBTN_RIGHT, eb::CurrentMods(), pt))
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
        int added = 0;
        BookmarkStore& store = BookmarkStore::Get();
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 2] = L"";
            if (!DragQueryFileW(drop, i, path, _countof(path))) continue;
            DWORD attr = GetFileAttributesW(path);
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (store.ContainsPath(path)) continue;  // 与 BmkAddCurrent 一致：不重复添加同一目录
            store.AddPath(LeafOf(path), path);
            ++added;
        }
        ::DragFinish(drop);  // 成员同名，必须显式全局作用域
        if (added) {
            store.Save();
            Reload();
        }
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

    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, theme_->font);

    const int margin = theme_->S(6);
    const int padX = theme_->S(2);
    const int iconW = theme_->S(16);
    const int gap = theme_->S(6);
    const int ovfW = theme_->S(24);
    const int avail = rc.right - margin * 2;

    int total = 0;
    std::vector<int> widths(items_.size(), 0);
    for (size_t i = 0; i < items_.size(); ++i) {
        SIZE sz{};
        const std::wstring& t = items_[i].label;
        if (!t.empty()) GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
        int w = padX * 2 + iconW + gap + sz.cx + (items_[i].folder ? theme_->S(14) : 0);
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
            continue;
        }
        items_[i].rc = RECT{x, theme_->S(2), x + widths[i], rc.bottom - theme_->S(2)};
        x += widths[i] + theme_->S(1);
    }
    ovfRc_ = overflow_ ? RECT{rc.right - margin - ovfW, theme_->S(2), rc.right - margin,
                              rc.bottom - theme_->S(2)}
                       : RECT{};

    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    RebuildTooltip(hwnd_);
}

void BookmarkBar::RebuildTooltip(HWND h) {
    // 工具提示走唯一实现（TooltipHost）：文本复制与 id 管理都在里面，
    // 不再需要"两趟稳定文本"的手工做法。
    tooltip_.Attach(h, theme_->S(600));
    std::vector<eb::TooltipHost::Item> tips;
    tips.reserve(items_.size());
    for (const auto& it : items_) {
        if (it.rc.right <= it.rc.left) continue;
        tips.push_back({it.rc, it.folder ? (it.label + L"（分类）") : it.path});
    }
    tooltip_.SetTools(tips);
}

void BookmarkBar::Paint(HWND h) {
    eb::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        HGDIOBJ oldFont = SelectObject(mem, theme_->font);

        const int padX = theme_->S(2);
        const int iconPx = theme_->S(16);
        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            if (it.rc.right <= it.rc.left) continue;
            const bool draggingThis = dragging_ && dragIdx_ == static_cast<int>(i);
            if (hover_ == static_cast<int>(i) && !draggingThis) FillRect(mem, &it.rc, theme_->brHover);
            int x = it.rc.left + padX;
            if (it.icon) {
                RECT ir{x, it.rc.top, x + iconPx, it.rc.bottom};
                eb::DrawIconCentered(mem, it.icon, ir, iconPx);
            }
            x += iconPx + theme_->S(6);
            RECT tr{x, it.rc.top, it.rc.right - padX - (it.folder ? theme_->S(14) : 0), it.rc.bottom};
            SetTextColor(mem, draggingThis ? theme_->sub : theme_->text);
            DrawTextW(mem, it.label.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (it.folder) {
                // 与导航栏/标签栏同一个箭头原语：用 chevron 字形 + 主题色，
                // 而不是自绘折线（DPI 下尺寸写死、深色下也不跟随文字色）。
                RECT cr{it.rc.right - padX - theme_->S(14), it.rc.top,
                        it.rc.right - padX - theme_->S(2), it.rc.bottom};
                eb::DrawChevron(mem, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2, theme_->S(9),
                                theme_->sub, 1);
            }
        }

        if (overflow_) {
            if (hotOvf_) FillRect(mem, &ovfRc_, theme_->brHover);
            eb::DrawGlyph(mem, ico::IconId::More, ovfRc_, theme_->S(11), theme_->sub);  // More：与标签栏的下箭头区分
        }

        if (dragging_ && dropIdx_ >= 0 && dropIdx_ <= static_cast<int>(items_.size())) {
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
            eb::FillRectColor(mem, bar, theme_->accent);
        }

        RECT hr{0, rc.bottom - 1, rc.right, rc.bottom - 1};
        eb::DrawSeparator(mem, hr, false, theme_->line);

        SelectObject(mem, oldFont);
    });
}

void BookmarkBar::UpdateHover(HWND h, int x, int y) {
    int idx = HitItem(x, y);
    bool ovf = HitOverflow(x, y);
    if (idx != hover_ || ovf != hotOvf_) {
        hover_ = idx;
        hotOvf_ = ovf;
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

bool BookmarkBar::HitOverflow(int x, int y) const {
    if (!overflow_) return false;
    POINT pt{x, y};
    return PtInRect(&ovfRc_, pt) != FALSE;
}

void BookmarkBar::DragUpdate(HWND h, int x, int y) {
    // 依据光标相对可见项的中心位置决定插入线：超过一项的水平中点 → 插到它后面。
    // 隐藏项（rc 为空）与溢出按钮不参与命中；拖出内容区 → 取消（dropIdx_=-1）。
    RECT rc;
    GetClientRect(h, &rc);
    const int margin = theme_->S(6);
    const int viewRight = overflow_ ? (rc.right - margin - ovfRc_.right + ovfRc_.left - theme_->S(4))
                                    : rc.right;
    int newDrop = -1;
    if (x >= margin && x <= viewRight && y >= 0 && y < rc.bottom) {
        newDrop = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            const RECT& r = items_[i].rc;
            if (r.right <= r.left) continue;
            if (x > (r.left + r.right) / 2) newDrop = static_cast<int>(i) + 1;
        }
    }
    if (newDrop != dropIdx_) {
        dropIdx_ = newDrop;
        InvalidateRect(h, nullptr, FALSE);
    }
}

void BookmarkBar::DragFinish(HWND h, bool commit) {
    int from = dragIdx_;
    int to = dropIdx_;
    dragArm_ = dragging_ = false;
    dragIdx_ = dropIdx_ = -1;
    if (commit && from >= 0 && to >= 0) {
        BookmarkStore& store = BookmarkStore::Get();
        const std::wstring id = items_[static_cast<size_t>(from)].id;
        if (store.Move(id, to)) {  // Move 内部含 to==from 原位判断与撤销快照
            store.Save();
            Reload();
        }
    }
    InvalidateRect(h, nullptr, FALSE);
}

void BookmarkBar::Navigate(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const std::wstring& p = items_[static_cast<size_t>(index)].path;
    if (!p.empty() && onNavigate_) onNavigate_(p);
}

void BookmarkBar::ShowFolderMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const BookmarkNode* node = BookmarkStore::Get().Find(items_[static_cast<size_t>(index)].id);
    if (!node) return;

    std::vector<PopupRow> rows;
    std::function<void(const BookmarkList&, int)> walk = [&](const BookmarkList& list, int depth) {
        for (const auto& n : list) {
            if (n.isFolder) {
                std::wstring label = std::wstring(static_cast<size_t>(depth * 2), L' ') + n.name;
                rows.push_back({label, L"", IconFor(n), true, false, true, -1});
                walk(n.children, depth + 1);
            } else if (!n.path.empty()) {
                std::wstring label = std::wstring(static_cast<size_t>(depth * 2), L' ') + n.name;
                rows.push_back({label, n.path, IconFor(n), false, false, false,
                                static_cast<int>(rows.size())});
            }
        }
    };
    walk(node->children, 0);
    if (rows.empty()) return;

    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0 || pick >= static_cast<int>(rows.size())) return;
    const PopupRow& r = rows[static_cast<size_t>(pick)];
    if (!r.path.empty() && onNavigate_) onNavigate_(r.path);
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
    std::vector<PopupRow> rows;
    for (size_t i = 0; i < items_.size(); ++i) {
        rows.push_back({items_[i].label, items_[i].path, items_[i].icon, items_[i].folder, false,
                        false, static_cast<int>(i)});
    }
    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0) return;
    int idx = rows[static_cast<size_t>(pick)].data;
    if (idx < 0) return;
    if (items_[static_cast<size_t>(idx)].folder)
        ShowFolderMenu(idx, screen);
    else
        Navigate(idx);
}

void BookmarkBar::ShowItemMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];
    BookmarkStore& store = BookmarkStore::Get();
    std::vector<PopupRow> rows;
    rows.push_back({L"打开", L"", nullptr, false, false, false, 1});
    rows.push_back({L"复制路径", L"", nullptr, false, false, it.folder, 2});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"编辑...", L"", nullptr, false, false, false, 3});
    rows.push_back({L"删除", L"", nullptr, false, false, false, 4});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"新建分类", L"", nullptr, false, false, false, 5});
    rows.push_back({L"添加当前目录", L"", nullptr, false, false, false, 6});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"撤销删除", L"", nullptr, false, false, !store.CanUndo(), 7});
    rows.push_back({L"清除全部书签", L"", nullptr, false, false, false, 8});

    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0) return;
    int action = rows[static_cast<size_t>(pick)].data;
    switch (action) {
    case 1:
        if (it.folder)
            ShowFolderMenu(index, screen);
        else
            Navigate(index);
        break;
    case 2:
        // 剪贴板写入唯一实现（clipboard.cpp）
        if (!it.path.empty()) SetClipboardText(hwnd_, it.path);
        break;
    case 3: {
        // 编辑：名称 + 路径双字段（纯分类节点只有名称一项可改）。
        std::wstring name = it.label;
        std::wstring path = it.path;
        bool ok;
        if (it.folder && it.path.empty()) {
            ok = PromptText(hwnd_, *theme_, L"编辑", L"名称", it.label, &name) &&
                 store.Update(it.id, name, L"");
        } else {
            ok = PromptText2(hwnd_, *theme_, L"编辑", L"名称", it.label, &name, L"路径", it.path,
                             &path) &&
                 store.Update(it.id, name, path);
        }
        if (ok) {
            store.Save();
            Reload();
        }
        break;
    }
    case 4:
        if (MessageBoxW(hwnd_, (L"删除书签“" + it.label + L"”？").c_str(), L"ExBar",
                        MB_YESNO | MB_ICONQUESTION) == IDYES &&
            store.Remove(it.id)) {
            store.Save();
            Reload();
        }
        break;
    case 5: {
        std::wstring name;
        if (PromptText(hwnd_, *theme_, L"新建分类", L"分类名称", L"新建分类", &name)) {
            store.AddFolder(name);
            store.Save();
            Reload();
        }
        break;
    }
    case 6:
        CommandBus::Get().InvokeLegacy(CmdId::BmkAddCurrent, L"", L"ui");
        break;
    case 7:  // 撤销：恢复上一次删除/重排/清空前的快照（store 内部维护，≤20 步）
        if (store.Undo()) {
            store.Save();
            Reload();
        }
        break;
    case 8:  // 清除全部书签：可撤销（ReplaceAll 已入快照栈）
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", L"ExBar",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            store.ReplaceAll(BookmarkList{});
            store.Save();
            Reload();
        }
        break;
    default:
        break;
    }
}

void BookmarkBar::ShowEmptyMenu(POINT screen) {
    BookmarkStore& store = BookmarkStore::Get();
    std::vector<PopupRow> rows;
    rows.push_back({L"添加当前目录为书签", L"", nullptr, false, false, false, 1});
    rows.push_back({L"新建分类", L"", nullptr, false, false, false, 2});
    rows.push_back({L"撤销删除", L"", nullptr, false, false, !store.CanUndo(), 8});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"按名称排序", L"", nullptr, false, false, false, 3});
    rows.push_back({L"重新载入书签文件", L"", nullptr, false, false, false, 4});
    rows.push_back({L"导出书签...", L"", nullptr, false, false, false, 6});
    rows.push_back({L"导入书签...", L"", nullptr, false, false, false, 7});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"清除全部书签", L"", nullptr, false, false, false, 9});
    rows.push_back({L"隐藏书签栏", L"", nullptr, false, false, false, 5});

    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0) return;
    int action = rows[static_cast<size_t>(pick)].data;
    switch (action) {
    case 1:
        CommandBus::Get().InvokeLegacy(CmdId::BmkAddCurrent, L"", L"ui");
        break;
    case 2: {
        std::wstring name;
        if (PromptText(hwnd_, *theme_, L"新建分类", L"分类名称", L"新建分类", &name)) {
            store.AddFolder(name);
            store.Save();
            Reload();
        }
        break;
    }
    case 3:
        std::sort(store.roots().begin(), store.roots().end(),
                  [](const BookmarkNode& a, const BookmarkNode& b) {
                      if (a.isFolder != b.isFolder) return a.isFolder;
                      return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                  });
        store.MarkDirty();
        store.Save();
        Reload();
        break;
    case 4:
        ReloadFromDisk();
        break;
    case 8:
        if (store.Undo()) {
            store.Save();
            Reload();
        }
        break;
    case 9:
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", L"ExBar",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            store.ReplaceAll(BookmarkList{});
            store.Save();
            Reload();
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
        CommandBus::Get().InvokeLegacy(CmdId::BmkToggleBar, L"", L"ui");
        break;
    default:
        break;
    }
}
