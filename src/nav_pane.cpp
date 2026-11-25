// 原生导航窗格（独立 INameSpaceTreeControl）的实现。
// 取舍与实测依据见 nav_pane.h 顶部：为什么放弃 EP_NavPane 的裁剪法、
// 以及独立控件这条路每一条验证过的事实。
#include "nav_pane.h"

#include "config.h"
#include "log.h"
#include "nav_pane_sep.h"
#include "platform_win.h"
#include "settings_bounds.h"
#include "str.h"

#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>

namespace navpane {

namespace {

constexpr wchar_t kSysTreeClass[] = L"SysTreeView32";
// 展开整棵树的节点上限在 nav_pane_expand.cpp（唯一使用者）。

// 与 explorer 左栏对齐的控件样式：
//   HASEXPANDOS        展开箭头
//   ROOTHASEXPANDO     根行也占展开按钮列（**不是可选项**，见下）
//   FULLROWSELECT      整行选中（点空白也选中该行，和 explorer 一致）
//   SINGLECLICKEXPAND  单击展开
//   AUTOHSCROLL        长条目自动横向滚动
//   NOINFOTIP          不要那条"单击打开"的提示
//   TABSTOP            可 Tab 进入
//   BORDER             左侧细边（与文件列表区分）
//
// 【ROOTHASEXPANDO 为什么必须有 —— 探针 64/67 实测】
// 不开它时，根行**完全没有按钮列**：根行图标紧贴内容起点，子行则先占一列按钮。
// 于是根行比子行左移整整一个 indent（产品实测 indent=23），四个根（主页/图库/
// 此电脑/网络）看起来"另起一列、缩进不对齐"；更糟的是 主页/此电脑/网络 明明
// 有子项，却因为没有那一列而**画不出折叠三角**（三角落在客户区 x<0 被裁掉）。
// 探针 67 两种样式的 TVM_GETITEMRECT 文字左缘（indent=19）：
//   不开： 根 19 / 子 38   -> 根少一列
//   开：   根 38 / 子 57   -> 每层各占一列，图标与文字全部落进同一张网格
// 开了之后 图库（无子项）只是空着那一列，不会左右抖动 —— 这正是
// "折叠按钮独占一列、没有折叠按钮的项目也按同一列对齐"的语义。
constexpr DWORD kNstcStyle = NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO | NSTCS_FULLROWSELECT |
                             NSTCS_SINGLECLICKEXPAND | NSTCS_AUTOHSCROLL | NSTCS_NOINFOTIP |
                             NSTCS_TABSTOP | NSTCS_BORDER;

// ---- 行几何：对齐 explorer 左栏 ----
//
// 【实测依据 —— 像素级对比 explorer 截图（tools/explorer_navpane.bmp）与我们的
//   截图（tools/our_navpane3.png）得出】
//   explorer：图标列 x=30..49（indent=30）、行距 **30px**
//             （图标竖直中心 19.5 / 49.5 / 79.5，间距正好 30；
//               组间还多一个 30px 的空行，分隔线画在空行正中 y=110）
//   我们：  indent=23、行高 26 -> 图标列 x=23..42、图标中心 12.5 / 38.5 / 64.5
// 于是"看着不像 explorer"的两大来源就是行距与图标列位置。
//
// 【为什么能改】这两个都是内部 SysTreeView32 的标准消息（TVM_SETINDENT /
//   TVM_SETITEMHEIGHT），NSTC 不拦；改完立刻生效，不需要重建树。
constexpr int kTreeIndentDefault = 30;
constexpr int kTreeItemHeightDefault = 30;

// 【组间空行做不出来 —— 如实记录，别再重复试】
// explorer 两组之间是"多一个 30px 的空行"，而 NSTC 没有任何"分组/间隔行"的概念
// （全部 NSTCS_* 位里没有这一类，NSTCROOTSTYLE 也只有 VISIBLE/HIDDEN/EXPANDED）。
// 往内部树插"外来"空行项会**让 NSTC 崩溃**（它把树项 lParam 当自己的节点指针
// 解引用，见 nav_pane_sep.h 顶部的实测记录）。
// 所以我们只能画一条 1px 线标出组界；线画在**新组第一行的行边界**上，
// 位置与 explorer 的线一致（都是两组之间的那道界），只是 explorer 上下留白更多。

// 老 SDK 头（4.71 之前）没有 TVIR_BOUNDS，这里自己补一个。
#ifndef TVIR_BOUNDS
#define TVIR_BOUNDS 0
#endif

// 已知文件夹表、RootSpec 解析、路径->IShellItem 都在 nav_pane_root.cpp
// （纯配置语义，可单测）；这里只留控件语义。

}  // namespace

// 取 IShellItem 的路径。文件系统项直接给盘符路径；命名空间项补 `shell:` 前缀。
//
// 【为什么必须补 shell: 前缀 —— 实机日志与探针 49 双重证据】
// SIGDN_DESKTOPABSOLUTEPARSING 对命名空间项返回的是**裸** `::{GUID}`，而裸形式
// 对"主页"（679F85CB-…，Win11 的快速访问/主页）会返回 E_INVALIDARG(0x80070057)：
//   [navpane] click path=::{679F85CB-...}
//   [nav] parse '::{679F85CB-...}' hr=0x80070057   -> "无法打开 ::{679F85CB-...}"
// 探针 49 把五种命名空间各测两种写法，结论是确定的：
//   ::{GUID}       -> 主页失败，其余四个成功（此电脑/网络/回收站/图库）
//   shell:::{GUID} -> **五个全部成功**，且规范化结果与裸形式一致
// 所以统一补前缀，既不破坏原本能用的，又修好主页。不要按 GUID 特判 ——
// 那样每加一个命名空间根就要再赌一次哪种写法能用。
//
// 非匿名命名空间：事件适配器（nav_pane_events.cpp）也要用它。
std::wstring PathOfItem(IShellItem* item) {
    if (!item) return L"";
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p)) || !p) return L"";
    std::wstring s = p;
    CoTaskMemFree(p);
    // 归一化规则（补 shell: 前缀）的唯一实现在 nav_pane_root.cpp，可单测。
    return NormalizeParsingName(s);
}

namespace {

BOOL CALLBACK FindSysTreeProc(HWND h, LPARAM p) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (str::IEquals(cls, kSysTreeClass)) {
        *reinterpret_cast<HWND*>(p) = h;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

// ---------------------------------------------------------------- NavPane

HRESULT STDMETHODCALLTYPE NavPane::QueryInterface(REFIID riid, void** ppv) {
    // NavPane 本身不是 COM 对象（它持有 NSTC，反过来 NSTC 持有 Events 适配器）。
    // 这个 QI 只为满足"回指对象也该长得像 COM 对象"的形式要求，没有调用方会真的
    // 拿它去 QI 别的接口。
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE NavPane::AddRef() { return (ULONG)InterlockedIncrement(&comRef_); }

HWND NavPane::FindSysTree() const {
    HWND found = nullptr;
    if (host_) EnumChildWindows(host_, FindSysTreeProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// 行几何对齐 explorer（只写不同的值，避免每次布局都让整棵树重排闪一下）。
void NavPane::ApplyTreeMetrics() {
    HWND tv = FindSysTree();
    if (!tv) return;
    const Settings& s = Config::Get().s();
    const int indent = s.navPaneIndent > 0
                           ? std::clamp(s.navPaneIndent, kNavPaneIndentMin, kNavPaneIndentMax)
                           : kTreeIndentDefault;
    const int rowH = s.navPaneItemHeight > 0
                         ? std::clamp(s.navPaneItemHeight, kNavPaneItemHeightMin,
                                      kNavPaneItemHeightMax)
                         : kTreeItemHeightDefault;
    if (static_cast<int>(SendMessageW(tv, TVM_GETINDENT, 0, 0)) != indent)
        SendMessageW(tv, TVM_SETINDENT, indent, 0);
    if (static_cast<int>(SendMessageW(tv, TVM_GETITEMHEIGHT, 0, 0)) != rowH)
        SendMessageW(tv, TVM_SETITEMHEIGHT, rowH, 0);
}

void NavPane::RefreshTreeMetrics() {
    if (!host_) return;
    ApplyTreeMetrics();
    UpdateSeparatorLines();
}

// 重算组间分隔线（**入口**）。
//
// 【现在它做什么】线的位置在**每次绘制时现算**（见 ComputeSeparatorYs），
// 这里只做两件事：让树重画一遍（重画 -> WM_PAINT -> 补画线）+ 把线位记进日志。
void NavPane::UpdateSeparatorLines() {
    if (!tree_ || !host_) return;
    HWND tv = FindSysTree();
    if (!tv) return;
    // 树可能被 NSTC 换掉了 —— 子类要跟着换，否则线就不画了。
    if (treeHooked_ != tv) HookTreeScroll();
    sepYs_ = ComputeSeparatorYs(tv);
    // 线位也打出来：组界数与线位数对不上时（组界滚出可视区/行矩形取不到），光看两个数字看不出是哪一种。
    wchar_t ys[96] = L"";
    for (size_t i = 0; i < sepYs_.size() && i < 8; ++i)
        swprintf(ys + wcslen(ys), 96 - wcslen(ys), L",%d", sepYs_[i]);
    LogDbg(L"[navpane] 分隔线：组界 %d 个，线位 %d 个（树客户区 y）：%s",
           static_cast<int>(sepItems_.size()), static_cast<int>(sepYs_.size()), ys);
    // 让树重画 -> WM_PAINT -> PaintSeparatorLines。
    InvalidateRect(tv, nullptr, FALSE);
}

// 算出组界线在**内部树客户区**里的 y 坐标。
//
// 【坐标系 —— 探针 75 实测，别再猜】
// INameSpaceTreeControl::GetItemRect 给的是**屏幕坐标**：实测三个根的矩形是
// (9,32,307,52) / (9,52,307,52+20) / (9,72,...)，而那个 NSTC 窗口的屏幕原点
// 正好是 (9,32) —— 也就是说矩形是相对**树窗口**的屏幕位置。
// 所以要换算成树客户区坐标，用 ScreenToClient(内部树)。
std::vector<int> NavPane::ComputeSeparatorYs(HWND tv) {
    std::vector<int> ys;
    if (!tree_ || !tv) return ys;
    RECT cr{};
    if (!GetClientRect(tv, &cr)) return ys;
    for (const SepItem& si : sepItems_) {
        // 用缓存的 item（见 sepItems_ 的说明），不在绘制路径上做 shell 调用。
        IShellItem* item = si.item;
        if (!item) continue;
        RECT r{};
        const HRESULT hr = tree_->GetItemRect(item, &r);
        if (FAILED(hr)) continue;
        POINT tl{r.left, r.top};
        ScreenToClient(tv, &tl);
        // 线的位置 = 新组第一行的**上边界**（= 上一行的下边界）。
        const int y = tl.y - 1;
        if (y < 0 || y >= cr.bottom) continue;   // 滚出可视区就不画
        ys.push_back(y);
    }
    return ys;
}

// 在树的 DC 上补画分隔线。
//
// 【为什么这里可以安全地回 NSTC 取行矩形】此刻树的 WM_PAINT 已经走完
// （DefSubclassProc 返回之后才走到这里），GetItemRect 是一次普通的跨窗口
// 消息，不会重入树的绘制状态。paintingSep_ 是第二道保险：万一 NSTC 内部
// 又触发了一次 WM_PAINT，也不许递归下去。
void NavPane::PaintSeparatorLines(HWND tv, HDC dc) {
    if (!tv) return;
    if (paintingSep_) return;
    paintingSep_ = true;

    RECT cr{};
    GetClientRect(tv, &cr);
    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;

    HDC target = dc;
    bool owned = false;
    if (!target) {
        target = GetDC(tv);
        owned = true;
    }
    if (target && w > 0 && h > 0) {
        std::vector<int> ys = ComputeSeparatorYs(tv);
        sepYs_ = ys;
        DrawSeparatorLines(target, ys, w, h, sepColor_);
    }
    if (owned && target) ReleaseDC(tv, target);

    paintingSep_ = false;
}

void NavPane::ApplyLayoutToControl(bool syncTreeState) {
    // 宽度真正落地要走三层（探针 31 实测）：
    //   1) 我们自己的宿主窗口
    //   2) NSTC 自己的窗口 + **补发 WM_SIZE** —— 不补这一条，控件内部不重排，
    //      内部树仍是 Initialize 时的宽度（探针 30 的 B 组踩的就是这个坑）
    //   3) 内部 SysTreeView32（双保险：NSTC 在某些构建上不转发尺寸）
    if (host_) {
        SetWindowPos(host_, nullptr, 0, 0, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (nstcWnd_) {
        SetWindowPos(nstcWnd_, nullptr, 0, 0, width_, height_,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(nstcWnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width_, height_));
    }
    if (HWND tv = FindSysTree()) {
        SetWindowPos(tv, nullptr, 0, 0, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // 行几何 + 滚动钩子 + 分隔线：宽度一变，行的位置就变了。
    //
    // 【syncTreeState=false 时跳过这一步】SetRoots 的调用链是
    //   SetRoots -> RebuildRoots（末尾已做一遍 Hook/Metrics/Sep）
    //            -> ApplyLayoutToControl（**又做一遍**），
    // 造树之后几何没变的话这一遍是纯浪费（每个组界一次跨窗口 GetItemRect
    // + 整棵 27 行树重绘），所以让 RebuildRoots 那条路径跳过。
    if (!syncTreeState) return;
    ApplyTreeMetrics();
    HookTreeScroll();
    UpdateSeparatorLines();
}

bool NavPane::Create(HWND parent, const Theme& theme, int w, int h) {
    if (!parent) return false;
    win::RegisterClassOnce(id::kNavPaneHostClass, &NavPane::HostWndProc);
    parent_ = parent;
    // 事件适配器在下面就要建；空白区菜单需要同一个 Theme 才能与书签菜单同脸。
    theme_ = &theme;
    width_ = w;
    height_ = h;

    // WS_CLIPSIBLINGS：导航树与边界条现在同为 viewArea 的子窗口。少了这一位，
    // NSTC 的原生树（含它自己画的边框）重绘时会盖住上层的 navEdge_，
    // 于是同一条边界在树区显示成 0x646464 的粗线、离开树区又变成 1px 中线。
    host_ = win::CreateChild(id::kNavPaneHostClass, parent, this, h, nullptr,
                             WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    if (!host_) {
        LogDbg(L"[navpane] 宿主窗口创建失败");
        return false;
    }
    // 【这里只套宿主，不能调 ApplyTheme()】ApplyTheme 会顺手把 darkTheme_ 记下来，
    // 而此时 NSTC 与内部 SysTreeView32 都还不存在；等树建好再调它时，
    // `if (darkTheme_ == name) return;` 会直接短路，树永远拿不到主题 ——
    // 实机症状就是"导航树不跟随深色模式"。所以：
    //   1) 建窗口后只给宿主套主题（AllowDark + SetWindowTheme 的一层）；
    //   2) 树与 NSTC 窗口建好、根也 Append 完之后，**再**调 ApplyTheme() 套全三层。
    darkTheme_.clear();
    AllowDarkForWindow(host_, theme.dark);
    win::ThemeControl(host_, theme.dark);

    HRESULT hr = CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_INameSpaceTreeControl, reinterpret_cast<void**>(&tree_));
    if (FAILED(hr) || !tree_) {
        LogDbg(L"[navpane] CoCreateInstance(NamespaceTreeControl) 失败 0x%08lX",
               static_cast<unsigned long>(hr));
        Destroy();
        return false;
    }

    // Initialize 的 RECT 是控件初始几何；之后靠 ApplyLayoutToControl()。
    RECT rc{0, 0, w, h};
    hr = tree_->Initialize(host_, &rc, static_cast<NSTCSTYLE>(kNstcStyle));
    if (FAILED(hr)) {
        LogDbg(L"[navpane] Initialize 失败 0x%08lX", static_cast<unsigned long>(hr));
        Destroy();
        return false;
    }

    // 空白区右键的标准开关（win7+）：打开后 NSTC 会在 psi==nullptr 时抛
    // OnBeforeContextMenu，由事件适配器交出我们的 IContextMenu。不要在内部树
    // 的 WM_CONTEXTMENU 里自己 TrackPopupMenuEx —— 那会重入 NSTC 的事件分发。
    if (INameSpaceTreeControl2* tree2 = nullptr;
        SUCCEEDED(tree_->QueryInterface(IID_INameSpaceTreeControl2,
                                        reinterpret_cast<void**>(&tree2))) &&
        tree2) {
        const HRESULT hr2 = tree2->SetControlStyle2(NSTCS2_SHOWNULLSPACEMENU,
                                                    NSTCS2_SHOWNULLSPACEMENU);
        LogDbg(L"[navpane] NSTCS2_SHOWNULLSPACEMENU hr=0x%08lX",
               static_cast<unsigned long>(hr2));
        tree2->Release();
    }

    // NSTC 自己的窗口：缩放要靠它（SetWindowPos + WM_SIZE）。
    if (IOleWindow* ow = nullptr;
        SUCCEEDED(tree_->QueryInterface(IID_IOleWindow, reinterpret_cast<void**>(&ow))) && ow) {
        ow->GetWindow(&nstcWnd_);
        ow->Release();
    }

    // 事件：树 -> 视图。必须在 AppendRoot 之前挂上（否则首次选中收不到）。
    events_ = NavPane::MakeEvents(this);
    if (FAILED(tree_->TreeAdvise(events_, &cookie_))) {
        LogDbg(L"[navpane] TreeAdvise 失败（树仍可用，但点选不会联动）");
        cookie_ = 0;
    }

    roots_ = RootSpec::Defaults();
    RebuildRoots();
    ApplyLayoutToControl();
    // 【树建好之后才套第二遍主题】内部 SysTreeView32 与 NSTC 窗口到这一步才存在，
    // 上面那次只覆盖了宿主。darkTheme_ 此时仍为空（没有提前设），所以这里
    // 不会短路，三层窗口都会被套上正确的 SubAppName。见 Create 里那段说明。
    ApplyTheme(theme);
    // 树建好之后才订阅 shell 变更：否则启动期那批 AppendRoot/SyncTo 也会
    // 被引成"目录变了"，平白多刷一次。
    RegisterShellChanges();
    LogDbg(L"[navpane] 原生导航树已创建（独立 INameSpaceTreeControl，%d 个根）",
           static_cast<int>(roots_.size()));
    return true;
}

void NavPane::Destroy() {
    UnregisterShellChanges();
    theme_ = nullptr;
    if (tree_) {
        if (cookie_) {
            tree_->TreeUnadvise(cookie_);
            cookie_ = 0;
        }
        tree_->Release();
        tree_ = nullptr;
    }
    // 适配器自己管引用（Release 到 0 时自删）；TreeUnadvise 已断开连接。
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
    nstcWnd_ = nullptr;
    UnhookTreeScroll();
    for (const SepItem& si : sepItems_) {
        if (si.item) si.item->Release();
    }
    sepItems_.clear();
    if (host_) {
        DestroyWindow(host_);
        host_ = nullptr;
    }
    parent_ = nullptr;
    roots_.clear();
}

void NavPane::RebuildRoots() {
    if (!tree_) return;
    // 【必须在同步窗口内抑制点击回调】实测：AppendRoot(NSTCRS_EXPANDED) 展开某项时，
    // NSTC 会**同步**抛 OnItemClick（"点开了这一项"），路径就是那个根。
    // 四次建树 = 四次把活动窗格拽走（用户看到"一启动就跑到网络"）。
    // 这是 AppendRoot 调用栈内部的同步回调，所以作用域标志能精确覆盖它 ——
    // 与"事件可能异步到达"的 OnSelectionChanged 不同。
    const bool prev = suppressClick_;
    suppressClick_ = true;
    tree_->RemoveAllRoots();

    // 【P0-1：不再按解析名重复绑定 shell】探针 79 实测解析一个 CLSID 就要绑定
    // 一次第三方 shell 扩展：27 个根 = 424 ms（冷）/ 138 ms（热），而 AppendRoot
    // 本身只要 13.5 ms。本轮诊断也测到 23 根的 resolve=78~94 ms。
    //
    // 现在发现层在拿显示名时顺手留下绝对 PIDL，这里从 PIDL 重建 IShellItem；
    // 只有配置来的根（pidl 为空）才回退到 ItemFromRootSpec。解析名这条慢路
    // 仍保留作为兜底，但启动首帧不再走它。
    // 另外组界项复用同一批 item，不再为分隔线二次解析。
    std::vector<IShellItem*> resolved;
    resolved.reserve(roots_.size());
    for (const auto& spec : roots_) {
        // P0-1：发现层已经解析过这些根并留下绝对 PIDL。从 PIDL 重建
        // IShellItem 不再走 shell 解析名那次第三方扩展绑定；只有配置来的根
        // （pidl 为空）才回退到 ItemFromRootSpec。
        IShellItem* item = spec.pidl.empty() ? nullptr : ItemFromPidlBytes(spec.pidl);
        if (!item) item = ItemFromRootSpec(spec);
        if (!item) {
            LogDbg(L"[navpane] 根解析失败，已跳过：%s", RootSpec::Format(spec).c_str());
            resolved.push_back(nullptr);
            continue;
        }
        // NSTCRS_VISIBLE：根自己显示出来（explorer 左栏那 27 项各占一行）。
        //
        // 【为什么**不**加 NSTCRS_EXPANDED —— 实测 explorer 的 27 项全是折叠的】
        // explorer 左栏的基线（tools/baseline_explorer_nav.txt，探针 45 MSAA 导出）
        // 里，除"主文件夹/图库"本身没有子项外，其余每一项都是 **COLLAPSED**：
        // TAN - 个人、123云盘、Links、…、此电脑、库、网络 全部折叠。
        // 探针 53 的截图（tools/explorer_navpane.bmp）也确认了：27 项平铺在同一
        // 缩进级、**没有任何展开箭头**。
        // 而我们一旦加了 NSTCRS_EXPANDED，NSTC 就会把每个根的子项**全铺开**：
        // 实机 MSAA 导出是 259 项（27 个根 + 全部子孙），什么 115网盘/Github/
        // PikPak/阿里云盘 全冒了出来 —— 与 explorer 完全不是一回事。
        tree_->AppendRoot(item, SHCONTF_FOLDERS, static_cast<NSTCROOTSTYLE>(NSTCRS_VISIBLE),
                          nullptr);
        resolved.push_back(item);
    }
    suppressClick_ = prev;
    // 重建后内部树可能换了窗口：滚动钩子要重挂，行几何要重设，分隔线要重算。
    HookTreeScroll();
    ApplyTreeMetrics();
    // 缓存"组界"那一项的 IShellItem（滚动路径上要用，但不能再去做 shell 调用）。
    for (const SepItem& si : sepItems_) {
        if (si.item) si.item->Release();
    }
    sepItems_.clear();
    for (size_t i = 1; i < roots_.size(); ++i) {
        if (roots_[i].group == roots_[i - 1].group) continue;
        // 组界项 = **新组的第一项**（线画在它的行上边界）。
        // 复用上面已解析的 item（AddRef 一份给 sepItems_，它要活到下次重建）。
        IShellItem* reuse = i < resolved.size() ? resolved[i] : nullptr;
        if (reuse) reuse->AddRef();
        sepItems_.push_back(SepItem{i, reuse});
    }
    UpdateSeparatorLines();
    // AppendRoot 会把 item 内部持有（AddRef），这里放掉我们自己的引用。
    for (IShellItem* item : resolved) {
        if (item) item->Release();
    }
}

void NavPane::SetRoots(const std::vector<RootSpec>& roots) {
    const std::vector<RootSpec> want = roots.empty() ? RootSpec::Defaults() : roots;

    // 【幂等】根集没变就不重建。App::ApplyNavPane() 会被 ApplyPaneChrome() 调，
    // 而 ApplyPaneChrome() 又在每次 Activate / 焦点变化 / 设置落地时跑 ——
    // 无条件重建的后果是：切一次标签就把整棵树 RemoveAllRoots + 重新 AppendRoot
    // 一遍（可见闪烁），而且每次 AppendRoot(NSTCRS_EXPANDED) 都会让 NSTC
    // 再抛一轮"合成点击"（见 OnItemClick 的说明）。
    // 比较必须连顺序一起比：根的先后就是用户看到的顺序。
    // 【group 也必须比】它决定组间分隔线的位置（见 nav_pane_sep.h）。
    // 只比 kind/value 的话，"同样的根但改了分组"会被判成"没变"，
    // 于是 Configure 里调过分组后分隔线迟迟不更新 —— 这类"看起来没生效"
    // 最难查。label 只用于日志，不参与比较（改个显示名不该重建整棵树）。
    if (want.size() == roots_.size()) {
        bool same = true;
        for (size_t i = 0; i < want.size(); ++i) {
            if (want[i].kind != roots_[i].kind || want[i].value != roots_[i].value ||
                want[i].group != roots_[i].group) {
                same = false;
                break;
            }
        }
        if (same) return;
    }

    roots_ = want;
    RebuildRoots();          // 内部树可能被重建，之前刷的底色会丢
    // 【syncTreeState=false】RebuildRoots 末尾已经做过 Hook/Metrics/Sep，
    // 这里只需要把几何落到控件上。重复一遍的代价是：组界项各一次 GetItemRect
    // 跨窗口调用 + 一次 InvalidateRect 整树重绘（日志实测单次启动里连打两遍）。
    ApplyLayoutToControl(false);
    RepaintTreeColorsFromCache();   // 用缓存色重刷，别让树回白
}

void NavPane::Resize(int w, int h) {
    if (!host_) return;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    // 几何没变就不动窗口：与 ShellView::Resize 同一个理由 —— 无条件 SetWindowPos
    // 会让控件整块重绘，切窗格时就是可见的闪烁。
    if (width_ == w && height_ == h) return;
    width_ = w;
    height_ = h;
    ApplyLayoutToControl();
}

void NavPane::SetVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (host_) ShowWindow(host_, visible ? SW_SHOW : SW_HIDE);
}

// 给内部 SysTreeView32 刷背景/文字色。
//
// 【为什么必须单独刷，而不是靠视觉样式】见 ApplyTheme 里那段探针 50 的说明：
// TreeView 的背景是它自己用 BK 色画的（实测恒为显式 RGB(255,255,255)），
// uxtheme 的 DarkMode_Explorer 只管边框/箭头/展开按钮那套视觉样式，不管底色。
//
// 【为什么要能被反复调用】SetRoots 会 RemoveAllRoots + 重新 AppendRoot，
// 内部树可能被重建，之前刷过的底色会丢。所以这里是幂等的"再刷一次"，
// 与 ApplyTheme 的 darkTheme_ 短路解耦 —— 那边判的是"模式变没变"，
// 这边判的是"树在不在、颜色对不对"。
void NavPane::PaintTreeColors(const Theme& theme) {
    treeBg_ = theme.bg;
    treeFg_ = theme.text;
    HWND tv = FindSysTree();
    if (!tv) return;
    // 短路只看"两个都对了没有"：只比 BK 会在"背景已设、文字还没设"时提前返回。
    const bool sameBk = SendMessageW(tv, TVM_GETBKCOLOR, 0, 0) == static_cast<LPARAM>(theme.bg);
    const bool sameFg =
        SendMessageW(tv, TVM_GETTEXTCOLOR, 0, 0) == static_cast<LPARAM>(theme.text);
    if (sameBk && sameFg) return;
    SendMessageW(tv, TVM_SETBKCOLOR, 0, static_cast<LPARAM>(theme.bg));
    SendMessageW(tv, TVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(theme.text));
    // 记一行：这组颜色到底是不是有效的。深色下若 text 仍是 0，会看到"深底黑字"。
    LogDbg(L"[navpane] PaintTreeColors bg=0x%06lX fg=0x%06lX same(bk=%d fg=%d)",
           static_cast<unsigned long>(theme.bg), static_cast<unsigned long>(theme.text),
           sameBk ? 1 : 0, sameFg ? 1 : 0);
    // 改完要整块重绘，否则已画出来的行还是旧底色。
    InvalidateRect(tv, nullptr, TRUE);
}

// 树刚被重建（SetRoots）后调用：没有 Theme 参数，用上次缓存下来的颜色重刷。
void NavPane::RepaintTreeColorsFromCache() {
    HWND tv = FindSysTree();
    if (!tv || treeBg_ == CLR_INVALID) return;
    SendMessageW(tv, TVM_SETBKCOLOR, 0, static_cast<LPARAM>(treeBg_));
    if (treeFg_ != CLR_INVALID) {
        SendMessageW(tv, TVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(treeFg_));
    }
    InvalidateRect(tv, nullptr, TRUE);
}

void NavPane::ApplyTheme(const Theme& theme) {
    if (!host_) return;
    theme_ = &theme;   // Reload 原地重建，事件侧下次弹菜单自然读到新配色。
    const wchar_t* name = theme.dark ? L"DarkMode_Explorer" : L"Explorer";
    // 分隔线颜色：走主题令牌 theme.navSep（深色 = explorer 逐像素实测 0x383838，
    // 浅色 = 0xD0D0D0，与栏分隔线同值）。以前写的是硬编码
    // `theme.dark ? RGB(56,56,56) : theme.line` —— 颜色得人肉记两档，违反"只有一份来源"。
    sepColor_ = theme.navSep;
    if (darkTheme_ == name) return;
    darkTheme_ = name;
    // 记一行：套主题时三个窗口各自在不在。这行是"导航树不跟随深色模式"的
    // 第一判据 —— 若 tree=null 说明调用点太早（时序问题），否则是 SetWindowTheme
    // 没被控件采纳（样式问题）。两者修法完全不同，不要混着猜。
    LogDbg(L"[navpane] ApplyTheme dark=%d host=%p nstc=%p tree=%p", theme.dark ? 1 : 0,
           (void*)host_, (void*)nstcWnd_, (void*)FindSysTree());
    // AllowDarkForWindow 不是 ThemeControl 的替代：前者是 uxtheme 的进程级深色豁免
    // （不调它 Shell 托管控件固定浅色），后者只设 SubAppName。两者都要，各有职责。
    AllowDarkForWindow(host_, theme.dark);
    win::ThemeControl(host_, theme.dark);
    // 内部树也要单独套一次：它由 NSTC 创建，不继承我们宿主的主题。
    if (HWND tv = FindSysTree()) {
        AllowDarkForWindow(tv, theme.dark);
        win::ThemeControl(tv, theme.dark);
    }
    // 背景/文字色单独刷：视觉样式管不到 TreeView 的底色（探针 50 实测）。
    // 放在这里（而不是上面那个 if 里）是为了让"树不存在"时也能安全跳过，
    // 且 SetRoots 之后可以复用同一个入口。
    PaintTreeColors(theme);
    UpdateSeparatorLines();
    if (nstcWnd_) {
        AllowDarkForWindow(nstcWnd_, theme.dark);
        win::ThemeControl(nstcWnd_, theme.dark);
    }
}

void NavPane::OnPicked(IShellItem* item) {
    if (!item) return;
    const std::wstring path = PathOfItem(item);
    if (path.empty()) return;
    OnPickedPath(path);
}

// 导航到某个路径（鼠标与键盘两条入口的唯一交汇点）。
// 路径为空或没有回调就静默返回：树只是导航的一个入口，绝不能让它失败。
void NavPane::OnPickedPath(const std::wstring& path) {
    if (!pickFn_ || path.empty()) return;
    pickFn_(path);
}

// 选中变化 = "树当前停在哪"，**不是**导航指令。
//
// 【为什么必须分成两件事】NSTC 在建树（AppendRoot）、首次布局、以及我们自己
// SetItemState(SyncTo) 时都会抛这个回调。若接成导航，启动就会被拽到最后一个根，
// 且 SyncTo -> OnSelectionChanged -> Navigate -> SyncTo 会无限自激。
// 因此这里只记录位置，导航一律由 OnItemClick（真实点击/回车）触发。
void NavPane::NoteSelection(IShellItem* item) {
    std::wstring path = PathOfItem(item);
    if (path.empty()) return;
    selection_ = path;
}

// 键盘回车：把当前高亮项交给导航。返回 false = 没有高亮项（按键照旧交给控件）。
bool NavPane::PickCurrent() {
    if (selection_.empty()) return false;
    OnPickedPath(selection_);
    return true;
}

void NavPane::SyncTo(const std::wstring& path) {
    // 【把"高亮"和"展开"分开 —— 这是"只能展开不能折叠"的修复】
    //
    // 旧实现每次导航后都调 EnsureItemVisible + SetItemState(NSTCIS_SELECTED)：
    // EnsureItemVisible 会**强制展开整条父链**（否则目标项不可见）；而 SyncChrome
    // 在切标签 / 焦点变化 / 移动窗口后都会调到这里，于是用户手动折叠的节点
    // 转眼又被下一次同步展开回去 —— 症状就是"只能展开、折不回去"。
    //
    // 正确语义：同步只负责"把高亮挪到当前目录"，**不负责布局**。
    // 用 NSTCIS_SELECTEDNOEXPAND：只高亮、绝不展开父链。目标项本来不可见时
    // （父链已折叠），高亮落在空处也无害 —— 用户展开到那里会看到高亮已就位，
    // 这比"擅自展开"更符合预期。
    if (!tree_ || path.empty()) return;
    // 【比较前必须归一化】selection_ 来自 PathOfItem（已补 shell: 前缀），
    // 而这里的 path 来自导航层（可能是裸 ::{GUID}，也可能是文件系统路径）。
    // 不归一化就永远不相等 -> 每次同步都白跑一次 SetItemState，
    // 而它又会触发 OnSelectionChanged，白添一轮回调。
    if (selection_ == NormalizeParsingName(path)) return;
    IShellItem* item = ItemFromPath(NormalizeParsingName(path));
    if (!item) return;
    tree_->SetItemState(item, NSTCIS_SELECTEDNOEXPAND, NSTCIS_SELECTEDNOEXPAND);
    item->Release();
}

}  // namespace navpane