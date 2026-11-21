#include "status_bar.h"

#include "platform_win.h"

#include "ui_draw.h"
#include "mouse_rules.h"

// 段图标：以前这里有一张**私有**字形表（7 个码位），与 ui_draw 的命名表重复。
// 现在统一走 ico::IconId —— 改字形只动 icon_glyph.cpp。
// 抽到头文件声明：状态栏右键菜单的每一行也要用同一份映射，不再复制 switch。
ico::IconId StatusSegmentIcon(int seg) {
    switch (seg) {
    case Seg_Fld: return ico::IconId::Folder;
    case Seg_File: return ico::IconId::Page;
    case Seg_Chk: return ico::IconId::Check;
    case Seg_Date: return ico::IconId::Calendar;
    case Seg_Size: return ico::IconId::Folder;
    case Seg_Disk: return ico::IconId::HardDrive;
    case Seg_Eye: return ico::IconId::Eye;
    case Seg_Type: return ico::IconId::Tag;
    default: return ico::IconId::None;
    }
}

namespace {

wchar_t GlyphOf(int seg) { return ico::Glyph(StatusSegmentIcon(seg)); }

}  // namespace

void StatusBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kStatusBarClass, StatusBar::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = win::CreateChild(kStatusBarClass, parent, this, theme.S(24), nullptr,
                             WS_CHILD | WS_VISIBLE);
    if (!hwnd_) return;
    stableW_.assign(Seg_Count, 0);
}

void StatusBar::SetModel(const StatusModel& m) {
    // 换目录就清掉"历史最宽"，否则进过大目录后计数段永久偏宽（窄窗口必重叠）。
    if (m.text[Seg_Path] != model_.text[Seg_Path] && !stableW_.empty())
        std::fill(stableW_.begin(), stableW_.end(), 0);
    model_ = m;
    if (hwnd_) {
        Layout(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void StatusBar::SetEnabledSegments(const std::vector<int>& segs) {
    enabled_ = segs;
    if (hwnd_) {
        Layout(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void StatusBar::ReloadTheme() {
    if (!hwnd_) return;
    // 字体变了（DPI/主题）→ "历史最宽"是在旧字体下量的，必须清零重测。
    std::fill(stableW_.begin(), stableW_.end(), 0);
    tooltip_ = ui::TooltipHost();   // 重建（析构销毁旧窗），宽度随 DPI 变
    Layout(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK StatusBar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<StatusBar>(h);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT StatusBar::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        Layout(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_MOUSEMOVE:
        UpdateHover(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverSeg_ >= 0) {
            hoverSeg_ = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &pt);
        int button = (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK)
                         ? MBTN_LEFT
                         : (m == WM_RBUTTONDOWN ? MBTN_RIGHT : MBTN_MIDDLE);
        // 段上要报 MT_STATUS_SEG：mouse.json 里按"状态栏段"配的规则以前永远命中不到。
        const int target = HitSegment(GET_X_LPARAM(l), GET_Y_LPARAM(l)) >= 0 ? MT_STATUS_SEG
                                                                            : MT_STATUS;
        if (sink_ && sink_(target, m, button, CurrentMods(), pt)) return 0;
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        POINT screen = pt;
        ClientToScreen(h, &screen);
        int seg = HitSegment(pt.x, pt.y);
        if (sink_ && sink_(seg >= 0 ? MT_STATUS_SEG : MT_STATUS, m, MBTN_LEFT, CurrentMods(),
                           screen))
            return 0;
        if (seg >= 0 && onClick_) onClick_(seg);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &pt);
        if (sink_ && sink_(MT_STATUS, m, MBTN_RIGHT, CurrentMods(), pt)) return 0;
        if (onContext_) onContext_(pt);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void StatusBar::Layout(HWND h) {
    layout_.clear();
    if (!h || !theme_) return;
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const int pad = theme_->S(8);
    const int gap = theme_->S(12);
    const int glyphW = theme_->S(18);
    const int avail = rc.right - pad * 2;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(h);

    std::vector<int> vis;
    for (int seg : enabled_)
        if (seg >= 0 && seg < Seg_Count) vis.push_back(seg);

    auto textW = [&](int seg) {
        // seg 已由调用方保证在 [0, Seg_Count) 内（见上面 vis.push_back 的条件）。
        const auto si = static_cast<size_t>(seg);
        int w = ui::MeasureText(dc, theme_->font, model_.text[si]);
        if (seg <= Seg_Chk || seg == Seg_Size || seg == Seg_Disk) {
            if (w > stableW_[si]) stableW_[si] = w;
            w = stableW_[si];
        }
        if (w > 0 && GlyphOf(seg)) w += glyphW + theme_->S(4);  // 没文字就不占字形位
        return w;
    };

    auto has = [&vis](int seg) {
        return std::find(vis.begin(), vis.end(), seg) != vis.end();
    };
    auto drop = [&vis](int seg) {
        vis.erase(std::remove(vis.begin(), vis.end(), seg), vis.end());
    };

    // 测宽：左段（≤Seg_Date）+ 弹性路径 + 右段
    auto measure = [&](int* leftW, int* rightW, bool* hasPath, int* pathW) {
        *leftW = *rightW = *pathW = 0;
        *hasPath = false;
        for (int seg : vis) {
            int w = textW(seg);
            if (seg <= Seg_Date)
                *leftW += w + gap;
            else if (seg == Seg_Path) {
                *hasPath = true;
                *pathW = w;
            } else
                *rightW += w + gap;
        }
    };

    // 收窄时的丢弃顺序：先右段（类型→眼睛→磁盘→容量），再日期；
    // 以前是"一刀切删 Seg_Eye 及之后"，既没重算宽度、又留下日期被压成负宽，
    // 窄窗口下所有段叠在同一个矩形里。
    static const int kDropOrder[] = {Seg_Type, Seg_Eye, Seg_Disk, Seg_Size, Seg_Date};
    int leftW = 0, rightW = 0, pathW = 0;
    bool hasPath = false;
    for (int guard = 0; guard <= Seg_Count; ++guard) {
        measure(&leftW, &rightW, &hasPath, &pathW);
        const int need = leftW + rightW + (hasPath ? theme_->S(120) : 0);
        if (need <= avail) break;
        bool removed = false;
        for (int seg : kDropOrder) {
            if (has(seg)) {
                drop(seg);
                removed = true;
                break;
            }
        }
        if (!removed) break;
    }
    measure(&leftW, &rightW, &hasPath, &pathW);

    // 路径拿剩余空间；不够 120px 就整段不画（否则会压到右段）
    bool showPath = hasPath;
    if (showPath) {
        pathW = avail - leftW - rightW;
        if (pathW < theme_->S(120)) showPath = false;
    }

    // 左段从左排
    int x = pad;
    for (int seg : vis) {
        if (seg > Seg_Date) continue;
        int w = textW(seg);
        SegLayout l;
        l.seg = seg;
        l.glyph = GlyphOf(seg) != 0 && w > 0;
        l.glyphCh = GlyphOf(seg);
        l.rc = {x, 0, x + w, rc.bottom};
        l.textRc = l.rc;
        layout_.push_back(l);
        x += w + gap;
    }
    if (showPath) {
        SegLayout l;
        l.seg = Seg_Path;
        l.rc = {x, 0, x + pathW, rc.bottom};
        l.textRc = l.rc;
        layout_.push_back(l);
    }

    // 右段从右边界回填（右对齐），并用最后一条记录的真实宽度
    std::vector<SegLayout> rights;
    for (int seg : vis) {
        if (seg <= Seg_Date) continue;
        int w = textW(seg);
        SegLayout l;
        l.seg = seg;
        l.glyph = GlyphOf(seg) != 0 && w > 0;
        l.glyphCh = GlyphOf(seg);
        l.rc = {0, 0, w, rc.bottom};
        l.textRc = l.rc;
        rights.push_back(l);
    }
    int rx = rc.right - pad;
    for (auto it = rights.rbegin(); it != rights.rend(); ++it) {
        const int w = it->rc.right;  // 上面把宽度记在 right
        it->rc = {rx - w, 0, rx, rc.bottom};
        it->textRc = it->rc;
        rx -= w + gap;
    }
    for (const auto& l : rights) layout_.push_back(l);

    ReleaseDC(h, dc);
    RebuildTooltip(h);
}

void StatusBar::RebuildTooltip(HWND h) {
    // 工具提示走唯一实现（TooltipHost）：以前三处栏各写一遍 TTM_DELTOOLW/ADDTOOLW
    // 样板，id 上界分别是 32/64/128，且都要自己持一份字符串副本。
    tooltip_.Attach(h, theme_->S(520));
    std::vector<ui::TooltipHost::Item> items;
    items.reserve(layout_.size());
    for (const auto& l : layout_) {
        ui::TooltipHost::Item it;
        it.rc = l.rc;
        it.text = model_.tip[l.seg].empty() ? model_.text[l.seg] : model_.tip[l.seg];
        items.push_back(std::move(it));
    }
    tooltip_.SetTools(items);
}

void StatusBar::UpdateHover(HWND h, int x, int y) {
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
    }
    int seg = HitSegment(x, y);
    if (seg != hoverSeg_) {
        hoverSeg_ = seg;
        InvalidateRect(h, nullptr, FALSE);
    }
}

int StatusBar::HitSegment(int x, int y) const {
    POINT pt{x, y};
    for (const auto& l : layout_)
        if (PtInRect(&l.rc, pt)) return l.seg;
    return -1;
}

void StatusBar::Paint(HWND h) {
    ui::PaintBuffered(h, theme_->bar, [&](HDC mem, const RECT& rc) {
        const int glyphW = theme_->S(18);
        const int gap = theme_->S(4);
        for (const auto& l : layout_) {
            if (l.seg == hoverSeg_) FillRect(mem, &l.rc, theme_->brHover);
            int x = l.rc.left;
            if (l.glyph) {
                RECT gr{x, l.rc.top, x + glyphW, l.rc.bottom};
                ui::DrawGlyph(mem, l.glyphCh, gr, theme_->iconGlyph(), theme_->sub);
                x += glyphW + gap;
            }
            RECT tr = l.rc;
            tr.left = x;
            tr.right = l.rc.right;
            UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
            // 路径段用 PATH_ELLIPSIS（从中间省略，保住盘符与末级目录），
            // 其余段用 END_ELLIPSIS。
            fmt |= (l.seg == Seg_Path) ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS;
            ui::DrawTextRow(mem, theme_->font, model_.text[l.seg], tr,
                            l.seg == Seg_Path ? theme_->text : theme_->sub, fmt);
        }

        // 顶部 1px 分隔线
        RECT hr{0, 0, rc.right, 0};
        ui::DrawSeparator(mem, hr, false, theme_->line);
    });
}
