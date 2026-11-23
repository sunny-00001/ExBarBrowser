// ShellView 的原生导航窗格辅助：查找 NamespaceTreeControl/SysTreeView32、
// 按记账值落地显隐、整棵树展开。
//
// 这里只操作 shell 自己画出来的窗格，不自己造树；显隐必须让 IExplorerBrowser
// 重排（SetRect），不能只 SW_HIDE —— 那样树占的那列宽度不会还给文件列表。
#include "shell_view.h"

#include "config.h"

namespace {

BOOL CALLBACK FindTreeProc(HWND h, LPARAM p) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (str::IEquals(cls, L"NamespaceTreeControl")) {
        *reinterpret_cast<HWND*>(p) = h;
        return FALSE;
    }
    return TRUE;
}

HWND FindNavTree(HWND container) {
    HWND found = nullptr;
    if (container) EnumChildWindows(container, FindTreeProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// 真正的 SysTreeView32 藏在 NamespaceTreeControl 里面；先在该控件内找，
// 找不到再退回整个容器（兼容不同 Win11 构建上 DirectUI 层级差异）。
BOOL CALLBACK FindSysTreeProc(HWND h, LPARAM p) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (str::IEquals(cls, L"SysTreeView32")) {
        *reinterpret_cast<HWND*>(p) = h;
        return FALSE;
    }
    return TRUE;
}

HWND FindNavSysTree(HWND container) {
    HWND found = nullptr;
    HWND host = FindNavTree(container);
    if (host) EnumChildWindows(host, FindSysTreeProc, reinterpret_cast<LPARAM>(&found));
    if (!found && container)
        EnumChildWindows(container, FindSysTreeProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

struct TreeWalk {
    int visited = 0;
    int expanded = 0;
};

// 逐节点展开，并递归进入刚展开出来的子节点。
// 上限是护栏：文件系统里有循环链接/超大树时不能把 UI 线程卡死。
constexpr int kMaxNavTreeNodes = 10000;
void ExpandTreeBranch(HWND tree, HTREEITEM item, TreeWalk* walk) {
    for (HTREEITEM cur = item;
         cur && walk->visited < kMaxNavTreeNodes;
         cur = reinterpret_cast<HTREEITEM>(
             SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(cur)))) {
        ++walk->visited;
        if (SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(cur)))
            ++walk->expanded;
        HTREEITEM child = reinterpret_cast<HTREEITEM>(
            SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
        if (child) ExpandTreeBranch(tree, child, walk);
    }
}

}  // namespace

void ShellView::ToggleNavPane() {
    if (!browser_) return;
    // EBO_SHOWFRAMES 只在下次导航被消费：导航后置位要再导航一次才出现，清零永不消失。
    // 所以真实显隐直接操作窗格子窗口，并自己记账。
    navPaneWanted_ = !navPaneWanted_;
    ApplyNavPaneWanted();
    // 用户显式切换 = 改变偏好：写回配置，下次启动/新标签沿用。
    Settings& s = Config::Get().mutableSettings();
    if (s.showNavPane != navPaneWanted_) {
        s.showNavPane = navPaneWanted_;
        Config::Get().Save();
    }
}

// 按"当前记账值"把导航窗格真实显隐落地（不写配置）。
void ShellView::ApplyNavPaneWanted() {
    if (!browser_) return;
    HWND tree = FindNavTree(container_);
    if (tree) ShowWindow(tree, navPaneWanted_ ? SW_SHOW : SW_HIDE);
    // 同步 IExplorerBrowser 的意图值，避免下次导航又把窗格画回来。
    EXPLORER_BROWSER_OPTIONS opts = EBO_NONE;
    if (SUCCEEDED(browser_->GetOptions(&opts))) {
        if (navPaneWanted_)
            opts = static_cast<EXPLORER_BROWSER_OPTIONS>(opts | EBO_SHOWFRAMES);
        else
            opts = static_cast<EXPLORER_BROWSER_OPTIONS>(opts & ~EBO_SHOWFRAMES);
        browser_->SetOptions(opts);
    }
    // 窗格占位变了，必须让 shell **重新算布局**，不是只重画：
    // 单纯 InvalidateRect 只重画像素，导航树隐藏后留下的空位还在（D4）。
    //
    // 【为什么同一尺寸的 SetRect 不够】IExplorerBrowser 内部按"矩形有没有变"决定
    // 要不要重排框架子窗口。传一模一样的 rc 等于没改，它直接忽略 —— 于是隐藏了
    // 导航树、宽度却还占着，双栏非活动窗格左侧就留一大块空白（实测 x 差 218px）。
    // 所以先抖 1px 再还原：第一次 SetRect 触发重排（此时树已 SW_HIDE，框架把宽度
    // 还给文件列表），第二次把尺寸还原成真实值。两次都在同一消息里完成，不闪。
    if (container_) {
        RECT rc{};
        if (GetClientRect(container_, &rc)) {
            InvalidateRect(container_, nullptr, TRUE);
            if (browser_ && rc.right > rc.left + 2) {
                RECT jitter = rc;
                jitter.right -= 1;                 // 造成"尺寸变了"的假象，逼框架重排
                browser_->SetRect(nullptr, jitter);
                browser_->SetRect(nullptr, rc);    // 立刻还原真实尺寸
            }
        }
    }
}

bool ShellView::NavPaneVisible() const {
    // 读我们自己的状态，不读 GetOptions：那是"意图"，关过一次后永远和真实状态相反。
    return navPaneWanted_;
}

bool ShellView::ExpandNavTree() {
    if (!browser_ || !navPaneWanted_) return false;
    HWND tree = FindNavSysTree(container_);
    if (!tree) return false;

    TreeWalk walk;
    for (HTREEITEM root = reinterpret_cast<HTREEITEM>(
             SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
         root && walk.visited < kMaxNavTreeNodes;
         root = reinterpret_cast<HTREEITEM>(
             SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(root)))) {
        ExpandTreeBranch(tree, root, &walk);
    }
    if (!walk.visited) return false;
    RedrawWindow(tree, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    LogDbg(L"[nav] 导航树已展开：访问 %d 项，展开 %d 项", walk.visited, walk.expanded);
    return true;
}
