// 双栏编排（App 的编排分片）：窗格访问、按需创建、活动窗格切换、比例与回调反查。
//
// 【为什么单独成文件】这里的生命周期是"窗格"而不是"标签"：一个标签有两个窗格，
// 第二窗格按需创建且可以隐身保留状态。把它塞进 app_tabs.cpp 会混淆两种生命周期。
//
// 标签、地址栏、模式指示都跟着活动窗格走；锁定/固定/别名/会话按固定槽位持久化。
// 命令、状态栏、导航、文件操作一律走活动窗格，
// 避免用户在第二窗格操作时命令打到主窗格。
#include "app.h"

#include <algorithm>

#include "app_commands.h"
#include "commands.h"   // CommandBus：拖动落盘转投 config.set
#include "config.h"   // historyDepth：新窗格的历史栈深度来自设置
#include "pane_ui.h"  // 双栏分割条的就地菜单（paneui::ShowBarMenu）

namespace {

// 拖动落盘走命令总线：config.set 是这些键的**唯一**落盘入口（设置面板改同一项
// 也走它）。两点好处：
//   · 写盘失败会被如实告警（直调 Config::Save() 把失败吞掉了）；
//   · 这一步进审计与事件，与设置面板改同一项同源。
// 拖动**过程中**只改内存供实时重排（每一帧都落盘会刷屏），松手才提交。
bool CommitSetting(const wchar_t* key, int value) {
    json::Value a = json::Value::MakeObject();
    a.Set(L"key", json::Value::MakeStr(key));
    a.Set(L"value", json::Value::MakeInt(value));
    return CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(a), Actor::Ui());
}

}  // namespace

App::Pane* App::PaneAt(Tab* tab, int index) {
    if (!tab || index < 0 || index >= 2) return nullptr;
    if (!tab->panes[static_cast<size_t>(index)]) {
        auto pane = std::make_unique<Pane>();
        // 每个新窗格用当前配置的深度装填历史栈；构造注入，不让它反过来读 Config。
        pane->nav.SetMaxEntries(
            static_cast<size_t>(Config::Get().s().historyDepth));
        tab->panes[static_cast<size_t>(index)] = std::move(pane);
    }
    return tab->panes[static_cast<size_t>(index)].get();
}

// 把"焦点在哪个视图"换算成窗格下标。返回 -1 = 焦点不在本标签的任何视图里。
//
// 【为什么这是唯一真源】焦点可以来自键盘（Tab/F6）、程序化 SetFocus、shell 内部
// 焦点迁移、鼠标点击 —— 按"谁触发了"分流必然漏掉某一路。只有问系统"焦点现在在
// 哪个视图"，才是与触发方式无关的真相（与 DOpus/XYplorer 的 source panel 同义）。
int App::FocusedPaneIndex(Tab* tab) const {
    if (!tab) return -1;
    HWND focus = GetFocus();
    if (!focus) return -1;
    for (int i = 0; i < 2; ++i) {
        const Pane* p = tab->panes[static_cast<size_t>(i)].get();
        if (!p || !p->view || !p->view->hwnd()) continue;
        if (focus == p->view->hwnd() || IsChild(p->view->hwnd(), focus)) return i;
    }
    return -1;
}

App::Pane* App::ActivePane() {
    Tab* t = Active();
    if (!t) return nullptr;
    // 活动窗格 = 焦点所在视图。activePane 只是缓存：焦点明确落在某个视图里时，
    // 以焦点为准并顺手校正缓存（键盘 Tab/F6、程序化 SetFocus、shell 内部迁移都覆盖）。
    // 焦点不在本标签的视图里（导航栏/书签栏/标签栏/弹层）时，保留上一次的缓存值 ——
    // 这样"点地址栏输入路径"之后回车，导航仍打到刚才在用的那个视图。
    if (Config::Get().s().dualPane) {
        const int focused = FocusedPaneIndex(t);
        if (focused >= 0) t->activePane = focused;
    }
    return PaneAt(t, t->activePane);
}

ShellView* App::ActiveShellViewHost() {
    Pane* p = ActivePane();
    return p && p->view ? p->view.get() : nullptr;
}

void App::ApplyPaneChrome() {
    // 导航窗格是全局一棵（navPane_），不属于任何窗格：这里**不再**按活动窗格
    // 分配树的显隐（旧逻辑是切到哪栏哪栏才有树，导致另一栏留一条空白）。
    // 窗格的显隐由 SetDualPane / NextPane 各自负责，本函数只负责全局树。
    ApplyNavPane();
}

// 全局导航窗格的显隐/根集落地。窗格的**宽度**由 Layout() 消费 navPane_.width()，
// 所以这里只要把"该不该显示"和"根有哪些"定下来，再触发一次布局即可。
void App::ApplyNavPane() {
    if (!navPane_.ok()) return;
    const Settings& s = Config::Get().s();

    // 根集（可配置）：配置为空则用内置默认（主页/图库/此电脑/网络）。
    // 解析失败的行会被跳过并记日志，不会让整棵树变空 —— 一行写错不至于把导航废掉。
    std::vector<navpane::RootSpec> specs;
    for (const auto& line : s.navPaneRoots) {
        navpane::RootSpec spec;
        if (navpane::RootSpec::Parse(line, &spec))
            specs.push_back(spec);
        else
            LogDbg(L"[navpane] 配置里的根无法解析，已忽略：%s", line.c_str());
    }
    navPane_.SetRoots(specs);

    // 【只在显隐真的变了才重排】ApplyPaneChrome() 会在切窗格、焦点变化、
    // 设置落地时被调用 —— 每次都 Layout() 就会把"没变化"也变成一次全量重排，
    // 而重排里的 SetWindowPos 会让 shell 视图整块重绘（用户看到的就是闪烁）。
    if (navPane_.visible() == s.showNavPane) return;
    navPane_.SetVisible(s.showNavPane);
    Layout();   // 宽度变了（0 <-> N），窗格区必须重排
}

// 根集发现在工作线程上算完了：把完整清单换上去（WM_APP_NAV_ROOTS_READY 走这里）。
//
// 【为什么不能直接调 ApplyNavPane()】它会先去读 s.navPaneRoots；配置为空时
// 又回到 Defaults() —— 那时缓存已就绪、拿到的是同一份，等于白跑。
// 这里直接把 PrefetchTake() 的结果交给 SetRoots，语义清楚也少一次分支。
//
// 【幂等】没算完 / 配置里用户自己指定了根 —— 两种情况都什么都不做。
// 前者等下次消息；后者是用户的显式选择，不该被"发现"覆盖。
void App::ApplyDiscoveredRootsIfReady() {
    if (!navPane_.ok()) return;
    // 用户在配置里写了根就尊重配置，别用发现结果顶掉它。
    if (!Config::Get().s().navPaneRoots.empty()) return;

    std::vector<navpane::RootSpec> roots;
    if (!navpane::PrefetchTake(&roots)) return;   // 没算完 / 结果为空
    navPane_.SetRoots(roots);
}

void App::WirePaneCallbacks(Tab* tab, int paneIndex, ShellView* view) {
    if (!tab || !view) return;
    // ShellView* 是回调的身份标记：FindPane 会以它反查标签和左右下标。
    // 先接回调再 Create —— 首个导航可能在 Create() 内部就完成。
    ShellView* raw = view;
    // 点击本视图即激活本视图：shell 的焦点常落在深层子窗口，容器未必收到
    // WM_SETFOCUS，所以"点击即激活"必须走鼠标通道（与 DOpus/XYplorer 一致）。
    // 仅左键按下触发：右键/中键/滚轮/拖动不改变活动窗格，避免误切。
    view->SetMouseSink([this, raw](int target, UINT msg, int button, int mods, POINT pt) {
        if (button == 0 && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ||
                            msg == WM_APP_MOUSE_DOWN_NOUP))
            OnPaneActivated(raw);
        return OnChromeMouse(target, msg, button, mods, pt);
    });
    view->SetNavFn([this, raw](const std::wstring& p, const std::wstring& d, unsigned n) {
        OnTabNav(raw, p, d, n);
    });
    view->SetFailFn([this, raw] { OnTabNavFailed(raw); });
    // 焦点进入本视图时，把活动窗格改成本视图。这是"激活哪个视图就哪个响应"
    // 的触发点：点击/Tab 进入右视图即切换，命令与导航随之打到右视图。
    view->SetActivateFn([this, raw] { OnPaneActivated(raw); });
    // 窗格显隐偏好：App 层转发到 Config（ShellView 只认回调，不碰 Config —— R16）。
    ShellView::PanePrefs prefs;
    prefs.getShowNav = [] { return Config::Get().s().showNavPane; };
    prefs.getShowPreview = [] { return Config::Get().s().showPreviewPane; };
    prefs.setNavPane = [](bool on) {
        Settings& s = Config::Get().mutableSettings();
        if (s.showNavPane == on) return;
        s.showNavPane = on;
        Config::Get().Save();
    };
    prefs.setPreviewPane = [](bool on) {
        Settings& s = Config::Get().mutableSettings();
        if (s.showPreviewPane == on) return;
        s.showPreviewPane = on;
        Config::Get().Save();
    };
    view->SetPanePrefs(std::move(prefs));
    (void)paneIndex;
}

bool App::EnsurePaneView(Tab* tab, int paneIndex) {
    Pane* p = PaneAt(tab, paneIndex);
    if (!p) return false;
    if (p->view) return true;

    std::wstring path = p->path();
    if (path.empty()) {
        Pane* main = PaneAt(tab, 0);
        path = main && !main->path().empty() ? main->path() : HomePath();
    }
    auto view = std::make_unique<ShellView>(theme_);
    WirePaneCallbacks(tab, paneIndex, view.get());
    // 导航树是**全局一棵**（App 的 navTree_），不属于任何窗格 —— 所以每个窗格
    // 在创建时都声明"不要自己的导航树"。GetPaneState(EP_NavPane) 因此在首次
    // 导航时就回关，Shell 根本不建那条 187px 的列：窗格内容区从左边第一个像素
    // 就是文件列表，不会留空白。旧做法（建树再 SW_HIDE）宽度不回收，切窗格时
    // 被隐藏的一栏左侧会残留一块空白 —— 这正是"切左切右各空一次"的成因。
    view->PresetNoNavPane();
    if (!view->Create(viewArea_, path)) {
        LogDbg(L"[pane] 创建窗格失败：%s", path.c_str());
        return false;
    }
    // Create 可能同步完成首次导航并改写 nav；以视图回读的目录为准，
    // 否则第二窗格会显示 A、地址栏却说 B。
    const std::wstring settled = view->path().empty() ? path : view->path();
    if (p->nav.here().empty()) p->nav.Reset(settled);
    p->view = std::move(view);
    ApplyPersistedViewState(p->view.get());
    return true;
}

bool App::FindPane(ShellView* view, Tab** tabOut, int* paneIndexOut) const {
    if (!view) return false;
    for (const auto& kv : tabs_) {
        Tab* t = kv.second.get();
        for (int i = 0; i < 2; ++i) {
            const Pane* p = t->panes[static_cast<size_t>(i)].get();
            if (p && p->view.get() == view) {
                if (tabOut) *tabOut = t;
                if (paneIndexOut) *paneIndexOut = i;
                return true;
            }
        }
    }
    return false;
}

void App::CreateSplitBars() {
    // 三条边界条的父窗口**必须相同**：都是 viewArea_。
    //
    //   · 导航窗格、两个文件视图、AI 面板宿主都已经是 viewArea_ 的子窗口；
    //   · 三条边界条若放在主窗口 hwnd_ 上，就与这些内容不在同一条兄弟裁剪链里。
    //     实测后果是：panesBar_（与视图同父）能稳定画出 1px 中线，
    //     而 navEdge_/aiEdge_ 被 viewArea_ 的子窗口盖住，只剩下原生控件的边框
    //     —— 表现为"三处分隔线有粗有细、连续和断开混在一起"。
    //
    // 统一父窗口后，三条条都用 viewArea 内部坐标 [0,viewH]，也彻底消掉了
    // 主窗口坐标与内容区坐标混用导致的上下越界（旧代码靠 viewTop 补差）。
    // 所有边界条仍由 Layout() 按 HWND_TOP 压在内容之上，拖动/悬停不变。
    //
    // 三条都建好之后由 Layout() 决定几何与显隐：某条边界不存在时不该留一条
    // 拖不动的把手（单栏时没有窗格间边界，面板收起时没有右侧边界）。
    const struct {
        SplitBar* bar;
        HWND parent;
        const wchar_t* cls;
    } defs[] = {
        {&panesBar_, viewArea_, id::kPaneSplitterClass},  // 旧类名：双栏契约不变
        {&navEdge_, viewArea_, id::kSideSplitterClass},
        {&aiEdge_, viewArea_, id::kSideSplitterClass},
    };
    for (const auto& d : defs) {
        if (!d.bar->Create(d.parent, theme_, d.cls))
            LogDbg(L"[layout] 边界条创建失败：该边界将不可拖（布局仍按比例摆放）");
    }
    // 拖动回调统一落在 App：边界条自己不认识 Config，它只回答"鼠标现在在哪"，
    // 落盘时机（松手才写）属于编排层。
    panesBar_.SetDragFn([this](int percent, bool final) { OnSplitterDragged(percent, final); });
    navEdge_.SetDragFn([this](int centerX, bool final) { OnNavEdgeDragged(centerX, final); });
    aiEdge_.SetDragFn([this](int centerX, bool final) { OnAiEdgeDragged(centerX, final); });
    // 双栏分割条的"就地动作"入口（只有这条边界有：侧栏边界没有"两侧"语义）。
    // 右键 = 双栏族菜单；双击 = 比例归中（对标资源管理器/DOpus 双击分隔线）。
    // 两者都只是把意图转投命令总线 —— 边界条不认识 Config，也不实现动作。
    panesBar_.SetContextFn([this](POINT screen) { paneui::ShowBarMenu(hwnd_, theme_, screen); });
    panesBar_.SetDoubleClickFn([] {
        (void)CommandBus::Get().InvokeLegacy(CmdId::ViewPaneSplitSet, L"50", Actor::Ui());
    });
}

// ---- 侧栏边界拖动：像素口径 ----
//
// 【为什么两条边界各一个入口】落点都是"边界中心的 x"，但换算成宽度的方向相反：
//   · 导航边界：宽度 = center（左边是导航，边界右移 = 导航变宽）；
//   · AI 边界：宽度 = 客户区宽 - center（右边是面板，边界左移 = 面板变宽）。
// 合成一个函数就要靠 bool 参数区分"哪一侧是栏"，调用点读起来是
// `OnEdgeDragged(x, final, true)` —— 那个 true 是什么意思只能回去查。
void App::OnNavEdgeDragged(int centerX, bool final) {
    Settings& s = Config::Get().mutableSettings();
    const int w = std::clamp(centerX, kNavPaneWidthMin, kNavPaneWidthMax);
    const bool changed = s.navPaneWidth != w;
    s.navPaneWidth = w;
    if (changed) Layout();   // 拖动中只重排：Layout 读的就是刚写进去的值
    if (!final) return;
    CommitSetting(L"navPaneWidth", w);
    if (changed) MarkSessionDirty();
    LogDbg(L"[layout] 导航边界拖动结束：%d px", w);
}

void App::OnAiEdgeDragged(int centerX, bool final) {
    RECT rc{};
    if (!hwnd_ || !GetClientRect(hwnd_, &rc)) return;
    const int w = std::clamp(static_cast<int>(rc.right) - centerX, kAiPanelWidthMin,
                              kAiPanelWidthMax);
    Settings& s = Config::Get().mutableSettings();
    const bool changed = s.aiPanelWidth != w;
    s.aiPanelWidth = w;
    if (changed) Layout();
    if (!final) return;
    CommitSetting(L"aiPanelWidth", w);
    if (changed) MarkSessionDirty();
    LogDbg(L"[layout] AI 边界拖动结束：%d px", w);
}

void App::SetDualPane(bool on) {
    Settings& s = Config::Get().mutableSettings();
    s.dualPane = on;
    Config::Get().Save();

    if (on) {
        // 只给活动标签建第二视图；其余标签切到时由 Activate 补建，避免启动时 N×2 个 ShellView。
        if (Tab* t = ById(activeId_)) (void)EnsurePaneView(t, 1);
    } else {
        // 回到单栏时活动窗格必须回主窗格：否则隐藏了第二窗格，命令却仍打到它。
        // 标签条同步回主窗格路径，避免"视图在主窗格、标签却写着第二窗格的路径"。
        for (auto& kv : tabs_) {
            kv.second->activePane = 0;
            SyncTabIdentity(kv.second.get());
        }
    }

    for (auto& kv : tabs_) {
        Tab* t = kv.second.get();
        const bool tabActive = kv.first == activeId_;
        Pane* main = PaneAt(t, 0);
        Pane* second = PaneAt(t, 1);
        if (main && main->view) main->view->SetVisible(tabActive);
        if (second && second->view) second->view->SetVisible(tabActive && s.dualPane);
    }
    viewMode_ = ActiveViewModeIndex();
    ApplyPaneChrome();
    Layout();
    SyncChrome();
    FocusActiveView();
    MarkSessionDirty();
}

void App::NextPane() {
    if (!Config::Get().s().dualPane) return;
    Tab* t = Active();
    if (!t || !EnsurePaneView(t, 1)) return;
    const int next = t->activePane == 0 ? 1 : 0;
    t->activePane = next;
    // 【必须先把焦点搬到目标视图，再同步外壳】
    //
    // ActivePane() 的口径是"活动窗格 = 焦点所在视图"：焦点明确落在某个视图里时
    // 它以焦点为准并**覆写** t->activePane 这个缓存。而 SyncTabIdentity /
    // ActiveViewModeIndex / SyncChrome 内部都会调 ActivePane() —— 如果此刻焦点
    // 还在旧视图里，它就把刚翻转的 activePane 覆写回去，随后 FocusActiveView()
    // 又把焦点送回旧视图：整条命令净效果为零（Alt+F6 按了没反应），
    // 但标签标题已经被 SyncTabIdentity 改成新窗格的路径 → 标题/地址栏/
    // 分割条活动侧三者互相矛盾。
    //
    // 先 SetFocus 让"焦点"这个唯一真源指到目标窗格，后面所有 ActivePane() 才会
    // 一致地读到新值。
    Pane* target = PaneAt(t, next);
    if (target && target->view && target->view->hwnd()) SetFocus(target->view->hwnd());
    // 标签、地址栏、模式指示都跟着活动窗格走；两个视图本身保持原样。
    SyncTabIdentity(t);
    viewMode_ = ActiveViewModeIndex();
    // 切换活动窗格只搬焦点，不改显隐：双栏下两个 ShellView 必须同时可见，
    // 否则第二窗格一激活主窗格就被隐藏，"双栏"会退化成单栏。
    Pane* main = PaneAt(t, 0);
    Pane* second = PaneAt(t, 1);
    if (main && main->view) main->view->SetVisible(true);
    if (second && second->view) second->view->SetVisible(true);
    ApplyPaneChrome();
    Layout();
    SyncChrome();
    FocusActiveView();
    MarkSessionDirty();
}

void App::OnFocusMaybeChanged() {
    // 单栏：活动窗格恒为主窗格，外壳无需跟随焦点变化。
    if (!Config::Get().s().dualPane) return;
    Tab* t = Active();
    if (!t) return;
    const int focused = FocusedPaneIndex(t);
    // 焦点不在本标签的任何一个视图里（导航栏/书签栏/标签栏/弹层）：
    // 保留当前活动窗格，不动外壳 —— 用户正在地址栏里打字，此时切栏是错的。
    if (focused < 0) return;
    if (focused != t->activePane) {
        t->activePane = focused;
    }
    // 无论是否真的换了窗格，都刷新一次：视图从"未激活"变"激活"时，
    // 选中高亮与模式指示也可能需要跟着变（幂等，代价只有一次 SetText）。
    SyncTabIdentity(t);
    viewMode_ = ActiveViewModeIndex();
    ApplyPaneChrome();
    SyncChrome();
}

void App::OnPaneActivated(ShellView* view) {
    if (!view) return;
    // 单栏没有"活动窗格"的概念，恒为主窗格，不必切换。
    if (!Config::Get().s().dualPane) return;

    Tab* t = nullptr;
    int paneIndex = -1;
    if (!FindPane(view, &t, &paneIndex) || !t) return;
    if (paneIndex < 0 || paneIndex > 1) return;
    // 后台新标签的视图在创建时会自报激活：它只是被 Shell 初始化了，用户并没有点它。
    // 放行会把当前标签/窗格切走，那就不是"后台"了。
    if (t == backgroundTab_) return;
    // 这个视图报告自己被激活了。但它的自报不等于真相 —— 焦点此刻可能已经在别处
    // （例如点击那一瞬间 shell 又把焦点搬走了）。以 GetFocus 的实际结果为准；
    // 只有当焦点确实落在某个视图里时才采纳，否则退回本视图的自报。
    const int focused = FocusedPaneIndex(t);
    const int effective = focused >= 0 ? focused : paneIndex;
    if (t->activePane != effective) t->activePane = effective;
    SyncTabIdentity(t);
    viewMode_ = ActiveViewModeIndex();
    ApplyPaneChrome();   // 共享侧栏跟随活动窗格
    SyncChrome();
    MarkSessionDirty();
}

void App::SetPaneSplit(int percent) {
    int v = percent;
    if (v < 20) v = 20;
    if (v > 80) v = 80;
    Config::Get().mutableSettings().paneSplit = v;
    Config::Get().Save();
    Layout();
}

// 拖动分割条的唯一落点。与 SetPaneSplit 共用同一个字段（paneSplit），
// 区别只有"现在就落盘"与"松手才落盘"：
//   · 拖动中每移动一像素写一次 settings.json，既没必要（值随时会变），
//     又会把磁盘抖动叠加到一次本来就要重排两个 Shell 视图的操作上；
//   · 但**内存**必须立刻改 —— Layout() 读的是它，不改就等于拖了没反应。
// 松手（final）才 Save()，语义与命令路径完全一致：同一条路径产生同一份配置。
void App::OnSplitterDragged(int percent, bool final) {
    int v = percent;
    if (v < 20) v = 20;
    if (v > 80) v = 80;
    Settings& s = Config::Get().mutableSettings();
    const bool changed = s.paneSplit != v;
    s.paneSplit = v;
    if (changed) Layout();
    if (!final) return;
    CommitSetting(L"paneSplit", v);
    if (changed) MarkSessionDirty();
    LogDbg(L"[pane] 分割条拖动结束：%d%%", v);
}

void App::SetPaneVertical(bool vertical) {
    Settings& s = Config::Get().mutableSettings();
    // 方向没变就别动：SetPaneVertical 会触发 Layout()，而 Layout 里的
    // SetWindowPos 会让两个 ShellView 整块重绘（用户看到的就是闪一下）。
    if (s.paneVertical == vertical) return;
    s.paneVertical = vertical;
    Config::Get().Save();
    ApplyPaneVertical();   // 换方向只改摆法，不动槽位与状态
    LogDbg(L"[pane] 双栏方向改为 %s", vertical ? L"上下堆叠" : L"左右并排");
}

// 把**当前**方向落地（重排 + 刷外壳），不做"值有没有变"的判断。
//
// 【为什么必须与 SetPaneVertical 分开】config.set 这条路径是"先把新值写进
// Settings，再通知应用层生效"。值在调用前就已经是新值，于是 SetPaneVertical
// 里那句"同值就 return"必然命中 —— 方向改了却一次 Layout 都不发生，
// 用户看到的是"设置了上下堆叠，界面还是左右"。而那正是"方向要等下一次
// 改比例才顺带生效"的成因（Layout 恰好在别的路径上被调到）。
// 判重属于**命令/快捷键**入口（它们给的是"我要翻转"），不属于"按当前值重排"。
void App::ApplyPaneVertical() {
    Layout();
    SyncChrome();
    MarkSessionDirty();
}

// ---------------------------------------------------------------- 焦点观察
//
// 【为什么需要它】活动窗格的真源是"焦点现在在哪个视图"，而焦点可以来自键盘
// （Tab/F6）、程序化 SetFocus、shell 内部焦点迁移、鼠标点击……按触发方式分流
// 必然漏掉某一路。EVENT_OBJECT_FOCUS 是系统级"焦点变了"的唯一广播，与来源无关。
//
// 【为什么是只读的】SetWinEventHook 只观察事件，不改变焦点、不注入输入 ——
// 不违反 R21（禁的是 SendInput/合成输入/抢前台）。回调里只 PostMessage 一个
// 自定义消息回 UI 线程，重活（切窗格/刷外壳）在消息循环里做。
void App::StartFocusWatch() {
    if (focusWatch_ || !hwnd_) return;
    focusWatch_ = SetWinEventHook(
        EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr,
        [](HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
            // 只关心本进程的焦点事件：别的进程的焦点变化与本窗口无关。
            if (event != EVENT_OBJECT_FOCUS || !hwnd || !IsWindow(hwnd)) return;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId()) return;
            // 回投 UI 线程，不在钩子回调里切窗格（回调可能在别的线程）。
            PostMessageW(App::Get().hwnd(), WM_APP_FOCUS_CHANGED, 0, 0);
        },
        GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
}

void App::StopFocusWatch() {
    if (!focusWatch_) return;
    UnhookWinEvent(static_cast<HWINEVENTHOOK>(focusWatch_));
    focusWatch_ = nullptr;
}
