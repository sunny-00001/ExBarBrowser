#include "status_bar.h"

#include "config.h"
#include "platform_win.h"
#include "settings_bounds.h"

#include "ui_draw.h"
#include "mouse_rules.h"

#include <algorithm>

// 段图标（字形口径）：以前这里有一张**私有**字形表（7 个码位），与 ui_draw 的命名表重复。
// 现在统一走 ico::IconId —— 改字形只动 icon_glyph.cpp。
// 抽到头文件声明：状态栏右键菜单的每一行也要用同一份映射，不再复制 switch。
ico::IconId StatusSegmentIcon(int seg) {
    switch (seg) {
    case Seg_Fld: return ico::IconId::Folder;
    case Seg_File: return ico::IconId::Page;
    case Seg_Chk: return ico::IconId::Check;
    case Seg_Date: return ico::IconId::Calendar;
    // 容量段：系统位图取不到时退"硬盘"字形（U+EDA2，扁平机身 + 指示灯）。
    // 不用 Folder 字形 —— 那跟 Seg_Fld 的文件夹字形一模一样，两段分不出。
    case Seg_Size: return ico::IconId::HardDrive;
    case Seg_Disk: return ico::IconId::HardDrive;
    case Seg_Eye: return ico::IconId::Eye;
    case Seg_Type: return ico::IconId::Tag;
    case Seg_Ai: return ico::IconId::Channel;   // AI 通道：信号波，和状态栏其它段不撞
    case Seg_Undo: return ico::IconId::Undo;    // 撤销历史：回退箭头，与"可撤 N 步"同义
    // 双栏协同 chip：必须与"复制/移动"本身同义，不能借用 Folder/Export 这类
    // 泛用字形（那会和 Seg_Fld 撞成一样）。图标本身在 icon_glyph.h。
    case Seg_CopyPane: return ico::IconId::CopyToPane;
    case Seg_MovePane: return ico::IconId::MoveToPane;
    default: return ico::IconId::None;
    }
}

// 段名（状态栏右键菜单用）。与图标表放在同一个文件：加段时两处一起改。
//
// 【为什么不用调用方的静态数组】以前那段数组写在 app_commands.cpp 里，
// 少写一项就是 nullptr（聚合初始化补零），而菜单接口收 const std::wstring&
// —— 从 nullptr 构造 std::wstring 直接崩。加 Seg_Ai 时就踩了这个坑，
// 表现是"状态栏右键菜单一点就崩"。下面的静态断言把这类漏项挡在编译期。
namespace {
// 【不要写死长度】以前写成 const wchar_t* const kSegNames[Seg_Count]，数组已
// 按 Seg_Count 定长，sizeof/sizeof 恒等于 Seg_Count —— 那条 static_assert 恒真，
// 挡不住它声称要挡的漏项（少写一个名字 → 聚合初始化补 nullptr → 从 nullptr
// 构造 std::wstring → 菜单一点就崩）。不写长度，用 std::size 比才有意义。
const wchar_t* const kSegNames[] = {
    L"文件夹计数", L"文件计数", L"已选/总数", L"修改日期", L"路径",
    L"选中容量",   L"容器/磁盘", L"隐藏文件三态", L"类型",
    L"AI 状态", L"撤销历史", L"复制到另一窗格", L"移动到另一窗格",
};
static_assert(std::size(kSegNames) == Seg_Count,
              "状态栏段名表必须与 Seg_Count 等长（少一项 = nullptr → 菜单崩溃）");
}  // namespace

const wchar_t* StatusSegmentName(int seg) {
    if (seg < 0 || seg >= Seg_Count) return L"";
    return kSegNames[seg];
}

// 段图标（系统位图口径）：优先系统默认图标；系统没有对应资源的形状返回 None，
// 该段退 Segoe Fluent Icons 字形（系统里确实没有日历位图，也没有眼形位图）。
//
// ★与 StatusSegmentIcon 是**两份映射、一个语义**：这里决定"有没有系统位图"，
// 那里决定"没有时用哪个字形"。两份成对维护 —— 段少一个就是空图标。
sysicon::Id StatusSegmentSysId(int seg) {
    switch (seg) {
    case Seg_Fld: return sysicon::Id::Folder;
    case Seg_File: return sysicon::Id::Document;
    case Seg_Chk: return sysicon::Id::Check;
    case Seg_Size: return sysicon::Id::Size;      // 容量：带底座的立体文件夹（Windhawk 同口径）
    case Seg_Disk: return sysicon::Id::Disk;
    default: return sysicon::Id::None;
    }
}

void StatusBar::Create(HWND parent, const Theme& theme) {
    win::RegisterClassOnce(kStatusBarClass, StatusBar::WndProc, CS_DBLCLKS);
    parent_ = parent;
    theme_ = &theme;
    hwnd_ = win::CreateChild(kStatusBarClass, parent, this, theme.barStatus(), nullptr,
                             WS_CHILD | WS_VISIBLE);
}

void StatusBar::SetModel(const StatusModel& m) {
    model_ = m;
    if (hwnd_) {
        Layout(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void StatusBar::SetEnabledSegments(const std::vector<int>& segs) {
    enabled_ = segs;
    // 常驻段：statusSegs 管的是"文件统计段显示哪些"，不该把后台 AI 状态
    // 与"还有几步可撤"一起藏掉。这里补一次再归一，用户手改配置里写了
    // 重复的段号也不会重复画。
    for (int seg : {Seg_Ai, Seg_Undo})
        if (std::find(enabled_.begin(), enabled_.end(), seg) == enabled_.end())
            enabled_.push_back(seg);
    std::sort(enabled_.begin(), enabled_.end());
    enabled_.erase(std::unique(enabled_.begin(), enabled_.end()), enabled_.end());
    if (hwnd_) {
        Layout(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void StatusBar::SetSegmentText(int seg, const std::wstring& text, const std::wstring& tip) {
    if (seg < 0 || seg >= Seg_Count) return;
    model_.text[seg] = text;
    model_.tip[seg] = tip;
    if (hwnd_) {
        Layout(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool StatusBar::SegmentScreenRect(int seg, RECT* out) const {
    if (!out || !hwnd_) return false;
    for (const auto& l : layout_) {
        if (l.seg != seg) continue;
        RECT r = l.rc;
        POINT lt{r.left, r.top};
        POINT rb{r.right, r.bottom};
        ClientToScreen(hwnd_, &lt);
        ClientToScreen(hwnd_, &rb);
        out->left = lt.x;
        out->top = lt.y;
        out->right = rb.x;
        out->bottom = rb.y;
        return true;
    }
    return false;
}

void StatusBar::ReloadTheme() {
    if (!hwnd_) return;
    // 系统库存图标随系统主题变化：主题切换后重新取（句柄由 sys_icon 统一销毁）。
    sysicon::Invalidate();
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
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &pt);
        int button = (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK)
                         ? MBTN_LEFT
                         : (m == WM_RBUTTONDOWN ? MBTN_RIGHT : MBTN_MIDDLE);
        if (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) {
            button = XButtonFromWParam(w);
            if (button < 0) return 0;
        }
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

// 段的图标解析：系统位图优先，系统没有对应资源时退字形。
// ★只有这一处 —— 排版（占多宽）与绘制（画哪个）都问它。
StatusBar::SegIcon StatusBar::ResolveSegIcon(int seg) const {
    SegIcon out;
    if (!theme_) return out;
    if (seg == Seg_Path) {
        // 路径段画"当前文件夹的真实图标"（ico::Cached 的进程级缓存，借用不销毁）。
        out.sys = model_.pathIcon;
    } else {
        out.sys = sysicon::Get(StatusSegmentSysId(seg));
    }
    if (out.sys) {
        out.slot = theme_->iconSmall();   // 系统小图标槽（16@96）
        return out;
    }
    out.glyph = ico::Glyph(StatusSegmentIcon(seg));
    if (out.glyph) out.slot = theme_->iconGlyph();
    return out;
}

void StatusBar::Layout(HWND h) {
    layout_.clear();
    if (!h || !theme_) return;
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const int pad = theme_->S(8);
    const int gap = theme_->S(12);
    const int avail = rc.right - pad * 2;

    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(h);

    std::vector<int> vis;
    for (int seg : enabled_)
        if (seg >= 0 && seg < Seg_Count) vis.push_back(seg);

    // 段图标先解析一次（系统位图优先，退字形）：排版与绘制共用同一份结果。
    std::vector<SegIcon> icons(Seg_Count);
    for (int seg : vis) icons[seg] = ResolveSegIcon(seg);

    auto textW = [&](int seg) {
        // seg 已由调用方保证在 [0, Seg_Count) 内（见上面 vis.push_back 的条件）。
        const auto si = static_cast<size_t>(seg);
        // 数值段用"位数控宽"：把数字换 '8' 再量，宽度只跟**位数**走 ——
        // 数值变化不抖，也不会像旧口径（历史最大宽）那样只增不减
        // （进过一次大目录后该段永久偏宽，窄窗口下白占地方还挤掉别的段）。
        const bool numeric = (seg <= Seg_Chk) || seg == Seg_Size || seg == Seg_Disk;
        int w = numeric ? ui::MeasureStableText(dc, theme_->font, model_.text[si])
                        : ui::MeasureText(dc, theme_->font, model_.text[si]);
        // 没文字就不占图标位（图标不脱离文本出现）。
        if (w > 0 && icons[seg].slot > 0) w += icons[seg].slot + theme_->S(4);
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
        l.icon = icons[seg];
        if (w <= 0) l.icon = SegIcon{};   // 无文本 → 无图标（不画光杆图标）
        l.rc = {x, 0, x + w, rc.bottom};
        l.textRc = l.rc;
        layout_.push_back(l);
        x += w + gap;
    }
    if (showPath) {
        SegLayout l;
        l.seg = Seg_Path;
        l.icon = icons[Seg_Path];
        if (pathW <= 0) l.icon = SegIcon{};
        l.rc = {x, 0, x + pathW, rc.bottom};
        l.textRc = l.rc;
        layout_.push_back(l);
    }

    // 右段从右边界回填（右对齐），并用最后一条记录的真实宽度
    std::vector<SegLayout> rights;
    for (int seg : vis) {
        // ★必须显式排除 Seg_Path：路径段归**中间弹性位**（左段与右段之间），
        // 不是右组的一员。以前这里只排了 "seg <= Seg_Date"，
        // 路径段编号 4 > 3 于是被右组又收了一份 —— 同一段画两遍，
        // 右组那个还得挤走右组其它段的宽度。
        // 放不下中间位时按上面注释的口径"整段不画"，右组也不补。
        if (seg <= Seg_Date || seg == Seg_Path) continue;
        int w = textW(seg);
        SegLayout l;
        l.seg = seg;
        l.icon = icons[seg];
        if (w <= 0) l.icon = SegIcon{};
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
    // 提示窗最大宽以前写死 520：路径/容量段的长文本会被截掉。
    const int maxW = std::clamp(Config::Get().s().statusBarTooltipWidth,
                                kStatusBarTooltipWidthMin, kStatusBarTooltipWidthMax);
    tooltip_.Attach(h, theme_->S(maxW));
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
        const int gap = theme_->S(4);
        for (const auto& l : layout_) {
            if (l.seg == hoverSeg_) FillRect(mem, &l.rc, theme_->brHover);
            int x = l.rc.left;
            if (l.icon.slot > 0) {
                RECT gr{x, l.rc.top, x + l.icon.slot, l.rc.bottom};
                // 系统位图优先；系统没有对应资源时画字形（同一段只会有一种）。
                if (l.icon.sys)
                    ui::DrawIconCentered(mem, l.icon.sys, gr, l.icon.slot);
                else if (l.icon.glyph)
                    ui::DrawGlyph(mem, l.icon.glyph, gr, l.icon.slot, theme_->sub);
                x += l.icon.slot + gap;
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
