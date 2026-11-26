// 布局与主题（App 的编排分片）：四条栏的高度/显隐、视图区回收、DPI 与深浅色切换。
//
// 改这里只有一个理由：客户区的空间怎么分。栏内部怎么画不在此处（各自 ReloadTheme）。
#include "app.h"

#include <algorithm>

#include "log.h"
#include "pane_split.h"

// ---------------------------------------------------------------- 布局
void App::Layout(bool force) {
    if (!hwnd_) return;
    // 状态栏 chip 的屏幕位置随窗口尺寸变化；锚点一变，旧气泡就不该挂着。
    if (aiPeek_.visible()) HideAiPeek();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const Settings& s = Config::Get().s();

    // 栏高只有一处出处（Theme::barTab/barNav/…）：以前四个数各写一份，
    // 改栏高要改五处（见 theme.h 的栏高令牌说明）。
    // 标签栏此刻是否显示：语义只在 TabBarVisible 定义一次（config.h），
    // 后面四处（栏高/指纹/ShowWindow/ChromeHeight）全用这个值。
    const bool showTabBar = TabBarVisible(s, tabSet_.Count());
    const int tabH = showTabBar ? theme_.barTab() : 0;
    const int navH = s.showNav ? theme_.barNav() : 0;
    const int bmkH = s.showBookmark ? theme_.barBookmark() : 0;
    const int stH = s.showStatus ? theme_.barStatus() : 0;

    // 【同样的输入就整段跳过】ShowWindow 会同步发 WM_SIZE -> Layout()，
    // 而 Run 在 ShowWindow 之前已经排过一次版，几何一模一样。实测启动期
    // Layout 跑 3 次（+547 / +578 / +828），第三次那 250 ms 纯属重复重绘。
    // 指纹见 app.h 的 LayoutKey；必须覆盖所有影响几何的输入，漏一项就会
    // 把一次真实重排误判成"没变"（栏高也要进 —— 主题/DPI 切换时它会变）。
    layout::LayoutKey key;
    key.w = rc.right;
    key.h = rc.bottom;
    // 标签数变化必须进指纹：切到单标签时标签栏要消失，那是真实的几何变化。
    key.showBits = (showTabBar ? 1 : 0) | (s.showNav ? 2 : 0) | (s.showBookmark ? 4 : 0) |
                   (s.showStatus ? 8 : 0);
    key.barPx = tabH + navH + bmkH + stH;
    // AI 面板宽、导航宽的求解**只有一处**（side_layout）：窗口太窄时谁先让位、
    // 让到多少、文件区至少留多少，都是同一条产品规则。这里只把结果搬给窗口。
    //
    // 【为什么"放不下就整块收起"落在 Layout 而不是面板自己】面板只知道自己的
    // 期望宽度，不知道导航占了多少、窗口还剩多少 —— 它自己算就必须有第二份
    // 侧栏宽度副本。收起这件事由持有全窗几何的一方决定。
    const int barW = theme_.edgeHit();
    const sidelayout::Layout side = sidelayout::Solve(
        rc.right, s.showNavPane ? s.navPaneWidth : 0, aiPanel_.WidthPx(), barW);
    key.navW = side.navW;
    key.aiW = side.aiW;
    // 面板"期望宽度"变了也要重排：拖动边界时它每帧都变，但 WidthPx 读的是配置，
    // 而 aiW 已经被夹过 —— 所以必须把**期望值**也放进指纹，否则拖到"被夹住"的
    // 那一段里，aiW 不再变，指纹判定"没变"，边界条就停在原地不跟手。
    key.aiWant = aiPanel_.WidthPx();
    key.split = s.paneSplit;
    key.dual = s.dualPane;
    key.vertical = s.paneVertical;
    // 当前是哪个标签（见 layout_key.h 的说明）：每个标签各有自己的一组窗格窗口，
    // 切/建标签会换掉整组，新那组一开始是 8x8 的初始尺寸 —— 不重排就停在 8x8。
    key.activeTab = activeId_;
    // 第二窗格是否已就绪（见 layout_key.h 的说明）：双栏下它决定右半区有没有内容，
    // 而"view 刚建出来"不改变任何配置输入 —— 不进指纹就会把这次真实重排误判成"没变"。
    if (s.dualPane) {
        Tab* act = Active();
        Pane* p1 = act ? PaneAt(act, 1) : nullptr;
        key.pane1Ready = (p1 && p1->view) ? true : false;
    }
    // 不加日志：WM_SIZE/WM_SHOWWINDOW 每次都进来，"跳过"这条会刷满日志。
    // 需要验证幂等时临时打开即可（判据是"几何没变却重排了"——那属于 bug）。
    //
    // 【窗格与栏都建好之后才认指纹】启动期 WM_SIZE 早于 OnCreate 建栏/建视图，
    // 那一次 Layout() 里 MoveWindow(tabStrip_.hwnd()==nullptr, ...) 是空操作，
    // 而指纹照样被记下（输入只在客户端尺寸/配置，与"控件建没建"无关）。
    // 于是 ShowWindow 之后那次**真正的** Layout() 输入完全相同 -> 整段跳过，
    // 四条栏与窗格就此停在创建时的 100x26 / 10x10 —— 表现是"栏窄了、右侧空一块"
    // （实测：ViewArea 已按客户区排好，TabStrip 仍是 100 宽、TabView 仍是 10x10）。
    // 判据用"控件是否都在"，而不是"第几次调用"：后者会被任意一次早到的 WM_SIZE 打乱。
    const bool widgetsReady = tabStrip_.hwnd() && navBar_.hwnd() && bookmarkBar_.hwnd() &&
                              statusBar_.hwnd() && viewArea_ &&
                              (!s.dualPane || (Active() && PaneAt(Active(), 0) &&
                                               PaneAt(Active(), 0)->view));
    if (!force && widgetsReady && key == lastLayout_) return;
    if (widgetsReady) lastLayout_ = key;

    int y = 0;
    if (showTabBar) {
        MoveWindow(tabStrip_.hwnd(), 0, y, rc.right, tabH, TRUE);
        ShowWindow(tabStrip_.hwnd(), SW_SHOW);
        y += tabH;
    } else {
        ShowWindow(tabStrip_.hwnd(), SW_HIDE);
    }
    if (s.showNav) {
        MoveWindow(navBar_.hwnd(), 0, y, rc.right, navH, TRUE);
        ShowWindow(navBar_.hwnd(), SW_SHOW);
        y += navH;
    } else {
        ShowWindow(navBar_.hwnd(), SW_HIDE);
    }
    if (s.showBookmark) {
        MoveWindow(bookmarkBar_.hwnd(), 0, y, rc.right, bmkH, TRUE);
        ShowWindow(bookmarkBar_.hwnd(), SW_SHOW);
        y += bmkH;
    } else {
        ShowWindow(bookmarkBar_.hwnd(), SW_HIDE);
    }

    // viewH 用 Theme::ChromeHeight 算，**与启动期建树的高度同源**。
    //
    // 【为什么必须同源】启动时导航树以 navW × viewH 建（App::OnCreate），若这里
    // 用另一套加法算出不同的 viewH，树就会被二次 Resize —— 而那次 Resize 之前
    // 树已经按错误高度画过一帧（实测：treeH=80 / 3 行全空白）。两处各算一遍就是
    // 那个"启动第一帧图标没渲染完"的成因，所以统一收到 ChromeHeight 一处。
    const int chrome =
        theme_.ChromeHeight(showTabBar, s.showNav, s.showBookmark, s.showStatus);
    int viewH = rc.bottom - chrome;
    if (viewH < 0) viewH = 0;
    MoveWindow(viewArea_, 0, y, rc.right, viewH, TRUE);

    if (s.showStatus) {
        // 图标大小滑块占状态栏行最右端，状态栏本体让出这段宽度。
        //
        // 【为什么是"让位"而不是叠在上面】叠上去会盖住状态栏最右边的段
        // （AI chip 就在那儿），而那段是有内容的。缩窄状态栏是唯一不打架的做法。
        //
        // 【窄窗口为什么整体收起】窗口不够宽时，让状态栏少一段信息比让它被切掉
        // 一半要好 —— 与状态栏自己"收窄时按优先级丢段"的处理是同一个思路。
        const int sliderW = StatusZoomSlider::WidthPx(theme_);
        const bool roomForSlider = rc.right >= theme_.S(360);
        const int statusW = roomForSlider ? rc.right - sliderW : rc.right;
        MoveWindow(statusBar_.hwnd(), 0, rc.bottom - stH, statusW, stH, TRUE);
        ShowWindow(statusBar_.hwnd(), SW_SHOW);
        if (roomForSlider) {
            MoveWindow(zoomSlider_.hwnd(), statusW, rc.bottom - stH, sliderW, stH, TRUE);
            ShowWindow(zoomSlider_.hwnd(), SW_SHOW);
        } else {
            ShowWindow(zoomSlider_.hwnd(), SW_HIDE);
        }
    } else {
        ShowWindow(statusBar_.hwnd(), SW_HIDE);
        ShowWindow(zoomSlider_.hwnd(), SW_HIDE);
    }

    // 全局导航窗格占内容区最左一块；窗格区（含左右/上下分栏）排在它右边。
    // 宽度由配置 navPaneWidth 决定（拖动左侧边界会改它），不再是 shell 内部的 150px。
    // 关掉后窗格区立即占满 —— 与旧版"某栏隐藏树但留空白"的区别就在这里：
    // 占位由我们的布局算，不由 shell 说了算。
    // 宽度已由 side_layout 求解（含"窗口太窄就别显示"），这里只落地。
    const int navW = side.navW;
    if (navW > 0) {
        navPane_.Resize(navW, viewH);
        navPane_.SetVisible(true);
    } else {
        // 收起时也收成零宽：只 SetVisible(false) 不够 —— 树的宿主仍占着上次的宽度，
        // 而它是 WS_CHILD 的宿主窗口，隐藏并不等于不参与布局（与第二窗格同一道理）。
        navPane_.Resize(0, viewH);
        navPane_.SetVisible(false);
    }

    // AI 面板占内容区最右一块（与 navW 对称）。展开时宽度从文件区里扣，
    // 收起时为 0 —— 所以"没开面板"与"以前没有面板"的几何完全一致，不会白占一条。
    //
    // 【为什么在这里扣而不是让面板自己浮在上面】浮层会盖住文件视图最后几列的
    // 内容（用户看不到、也无法右键到），而扣宽度让文件视图真按剩余空间重排。
    const int aiW = side.aiW;

    // 【坐标系】AI 面板、两条侧栏边界与两个文件视图都是 viewArea_ 的子窗口
    // （见 CreateSplitBars），所以这里一律用 viewArea 内部坐标：x 从 0 到 rc.right，
    // y 从 0 到 viewH。旧代码把 AI 面板/侧栏边界挂在主窗口上、在这里补 viewTop，
    // 虽然修掉了上下越界，但仍是把两种父窗口混在一起 —— 正是三处边界线观感
    // 不一致的根源。统一父窗口后不再需要坐标系转换。
    RECT inner{navW, 0, side.fileHi, viewH};
    if (aiW > 0) {
        aiPanel_.SetBounds(RECT{side.fileHi, 0, rc.right, viewH});
    } else {
        // 收起（或被窗口宽度挤掉）时给个零矩形：面板宿主是隐藏的，这只是让
        // WebView2 的 bounds 归零，免得它在隐藏期间仍按旧尺寸渲染（有布局开销）。
        aiPanel_.SetBounds(RECT{0, 0, 0, 0});
    }

    // ---- 两条侧栏边界条 ----
    //
    // 规矩与双栏那条一致：**有内容才有一条可拖的边界**。导航收着 / 面板没开时
    // 显示一条拖不动的灰条，只会让人以为"这里坏了"。
    //
    // 边界条压在交界线上（不额外占位），所以 navPaneWidth=220 依然等于
    // "文件区从 x=220 开始" —— 旧配置不需要迁移，拖完也不会"文件区莫名变窄"。
    const int edgeH = theme_.edgeHit();
    if (navEdge_.ok()) {
        if (side.navW > 0) {
            RECT r{side.nav.barLo, 0, side.nav.barLo + edgeH, viewH};
            // 区间由求解给出（kNavPaneWidthMin..Max 且留住文件区），
            // 与落盘口径同一份 —— 见 layout_limits.h。
            navEdge_.SetPixelGeometry(r, false, side.nav.center, kNavPaneWidthMin,
                                      std::max(kNavPaneWidthMin,
                                               static_cast<int>(rc.right) - aiW - kMinFileAreaPx));
            navEdge_.SetVisible(true);
        } else {
            navEdge_.SetVisible(false);
        }
    }
    if (aiEdge_.ok()) {
        if (side.aiW > 0) {
            RECT r{side.ai.barLo, 0, side.ai.barLo + edgeH, viewH};
            // 区间同样按"留住文件区"算：面板最宽只能到"文件区还剩 kMinFileAreaPx"。
            // 【为什么不给 kAiPanelWidthMax 当上界】900 是配置口径的绝对上限，
            // 而窗口窄时能拖到多远由剩余空间决定 —— 拖到中间就被卡住才是对的，
            // 拖过去再被夹回来是"松手跳一截"。
            const int minCenter = std::max(navW + kMinFileAreaPx, static_cast<int>(rc.right) - kAiPanelWidthMax);
            const int maxCenter = std::max(minCenter, static_cast<int>(rc.right) - kAiPanelWidthMin);
            aiEdge_.SetPixelGeometry(r, false, side.ai.center, minCenter, maxCenter);
            aiEdge_.SetVisible(true);
        } else {
            aiEdge_.SetVisible(false);
        }
    }

    // ---- 双栏几何：算一次，所有标签共用 ----
    //
    // 两栏的矩形与标签无关（所有标签按同一套比例摆），以前放在标签循环里逐个重算，
    // 于是"分割条几何"要再算第三份。现在只有这一次计算，分割条与窗格读的
    // 是同一份结果 —— 分割线画在哪、两栏就在哪分开。
    const int gap = theme_.splitter();
    panesplit::Geometry geo{};
    const bool dual = s.dualPane;
    // 主轴起点与长度：左右并排是 x，上下堆叠是 y。分割条与窗格读**同一份**，
    // 两处各算一遍的话，拖动时鼠标换算的"整段窗格区"会和实际摆法差一个原点。
    const int axisStart = s.paneVertical ? inner.top : inner.left;
    const int axisAvail = s.paneVertical ? (inner.bottom - inner.top) : (inner.right - inner.left);
    if (dual) {
        const int center = axisStart + panesplit::CenterFromPercent(axisAvail, gap, s.paneSplit);
        geo = panesplit::Compute({inner.left, inner.top, inner.right, inner.bottom}, gap, center,
                                 !s.paneVertical);
    }

    // 分割条：真窗口（实体条），自己吃鼠标。
    //
    // 【为什么是实体条而不是"缝里画线"】两栏 Shell 视图各带 1px 边框，那是系统画的、
    // 关不掉。只要它露在外面，我们再画什么都凑成两条。所以窗口比缝**两侧各宽 1px**，
    // 把那两条边框压在自己底下 —— 物理上不显示，也就不可能数出第二条。
    // 窗口内：bg 打底 + 中线 1px（绘制见 split_bar.cpp）。
    //
    // 单栏时藏起来 —— 留着一条可抓的条会让"单栏"看起来有个拖不动的把手。
    if (panesBar_.ok()) {
        if (dual) {
            const int pad = theme_.splitterHit() > theme_.splitter()
                                ? (theme_.splitterHit() - theme_.splitter()) / 2
                                : 0;   // 每侧压住视图边框的像素数（=1）
            RECT sr{geo.splitter.left, geo.splitter.top, geo.splitter.right, geo.splitter.bottom};
            // geo.splitter 是缝本身；向外各扩 pad，盖住两侧视图边框。
            if (s.paneVertical) {
                sr.top -= pad;
                sr.bottom += pad;
            } else {
                sr.left -= pad;
                sr.right += pad;
            }
            panesBar_.SetGeometry(sr, s.paneVertical, axisAvail, axisStart, geo.center);
            panesBar_.SetVisible(true);
        } else {
            panesBar_.SetVisible(false);
        }
    }

    for (auto& kv : tabs_) {
        Tab* t = kv.second.get();
        // 槽 0 / 槽 1。左右并排时是"左/右"，上下堆叠时是"上/下"——
        // 槽位模型不变，方向只决定这两个槽怎么摆（所以切方向不丢路径与历史）。
        Pane* pane0 = PaneAt(t, 0);
        Pane* pane1 = PaneAt(t, 1);
        // 【每个视图只 Resize 一次】以前先 Resize(inner) 再 Resize(leftRc) ——
        // 主视图被改两次尺寸：先撑满整个窗格区，再缩回一半（上下堆叠时是缩回上半）。
        // 中间那一帧用户看到内容整块重排（"切换到第二窗格时闪烁"的成因之一）。
        // 现在先算好最终矩形（上面已算好），再一次性落地。
        if (pane0 && pane0->view) {
            pane0->view->Resize(dual ? RECT{geo.pane0.left, geo.pane0.top, geo.pane0.right,
                                            geo.pane0.bottom}
                                     : inner);
        }
        if (!dual) {
            // 【单栏要把第二窗格收成零矩形】只 SetVisible(false) 不够：窗口还占着
            // 双栏时的那半区，而它是 WS_CHILD | WS_CLIPCHILDREN 的宿主 —— 隐藏窗口
            // 不参与绘制，但主窗格的 Z 序/裁剪链里仍留着它，回收不到真正的全宽。
            // 表现就是"单栏时右侧还有一块占位"。收成 0x0 才是真的不占地方。
            // 视图对象本身不销毁（路径/历史/滚动位置要留住，切回双栏即刻恢复）。
            if (pane1 && pane1->view) pane1->view->Resize(RECT{0, 0, 0, 0});
            continue;
        }
        if (pane1 && pane1->view) {
            pane1->view->Resize(RECT{geo.pane1.left, geo.pane1.top, geo.pane1.right,
                                     geo.pane1.bottom});
        }
    }

    // ---- 内容区上下边缘：只留一条线 ----
    //
    // Shell 视图（IExplorerBrowser 的 SHELLDLL_DefView）会在自己的顶行/底行画
    // 1px 的 0x646464 原生边框；书签栏底边与状态栏顶边又各画一条 theme.line，
    // 于是上下交界原本是相邻两条线（y=103 的 D0 + y=104 的 64）。
    // 这里用两个 1px 子窗口盖住内容侧那一条：它们复用 viewArea 的类背景刷
    // （theme.bg），不新增颜色令牌，也不需要自绘。
    //
    // 必须放在所有内容窗口 Resize 之后、边界条抬层级之前：Shell 视图与 AI
    // 面板都有 WS_CLIPSIBLINGS，压到最上层的覆盖带不会被它们再盖回去；
    // 随后把三条 SplitBar 抬到覆盖带之上，交叉点仍是那条连续的中线。
    const int coverH = viewH > 0 ? 1 : 0;
    for (HWND cover : {viewTopCover_, viewBottomCover_}) {
        if (!cover) continue;
        ShowWindow(cover, coverH ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (!coverH) continue;
        const int yCover = (cover == viewTopCover_) ? 0 : viewH - 1;
        SetWindowPos(cover, HWND_TOP, 0, yCover, rc.right, coverH,
                     SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }
    for (HWND bar : {panesBar_.hwnd(), navEdge_.hwnd(), aiEdge_.hwnd()}) {
        if (bar)
            SetWindowPos(bar, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void App::ApplyTheme(int dpiOverride) {
    if (!hwnd_) return;
    const ULONGLONG t0 = GetTickCount64();
    const bool darkBefore = theme_.dark;
    theme_.Reload(hwnd_, dpiOverride);
    const bool darkChanged = darkBefore != theme_.dark;

    // 关键顺序：先把所有 EBC/DirectUI 窗口拆掉，再切进程颜色策略，最后重建。
    // 旧实现是先 ApplyProcessDarkMode 再让各 ShellView 重建；切浅色时旧深色
    // DirectUI 仍活着，shell 用它缓存的颜色策略创建"新"视图，表现就是文件区
    // 粘在深色、必须重启。这里把"拆-切-建"明确分成三段。
    if (darkChanged) {
        for (auto& kv : tabs_) {
            for (const auto& pane : kv.second->panes)
                if (pane && pane->view) pane->view->SuspendForThemeSwitch();
        }
    }
    ApplyProcessDarkMode(theme_.dark);
    theme_.ApplyToViewArea(viewArea_);
    ApplyDarkTitleBar(hwnd_, theme_.dark);
    tabStrip_.ReloadTheme();
    navBar_.ReloadTheme();
    bookmarkBar_.ReloadTheme();
    statusBar_.ReloadTheme();
    zoomSlider_.ReloadTheme();
    // 设置对话框是独立窗口，不会跟着主窗口 InvalidateRect 一起刷新 ——
    // 不显式通知它，深色切换后它会保持旧配色直到用户碰它一下。
    settingsDialog_.ReloadTheme();
    // 快捷键管理器同样是独立顶层窗口，理由与上面那条一样：
    // 它不会跟着主窗口 InvalidateRect 一起刷新，不显式通知就停在旧配色。
    hotkeyManager_.ReloadTheme();
    navPane_.ApplyTheme(theme_);
    panesBar_.ReloadTheme();
    navEdge_.ReloadTheme();
    aiEdge_.ReloadTheme();
    aiPanel_.SyncTheme();
    aiPeek_.SyncTheme();
    for (auto& kv : tabs_) {
        for (const auto& pane : kv.second->panes)
            if (pane && pane->view) pane->view->ApplyTheme();
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
    Layout();
    LogDbg(L"[theme] ApplyTheme %llums changed=%d", GetTickCount64() - t0, darkChanged ? 1 : 0);
}
