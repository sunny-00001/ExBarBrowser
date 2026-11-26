// 书签栏顶层的拖动重排：算落点 + 提交移动。
//
// 【为什么单独成文件】拖动状态机是"输入 → 落点 → 提交"三段式，
// 和栏的绘制/命中是两种职责；而提交那一段必须与**分类内拖动**保持同一套
// 命令口径（bmk.move），两处各写一份必然分叉。拆开才读得动。
//
// 【边缘自动滚动】拖到栏左/右边缘带内按拍滚动（edge::Update 共享实现，
// 周期共用 tabDragScrollMs 设置）；滚动经 ScrollBy 收口，落点由 ScrollBy
// 里的统一重算刷新 —— 本文件不单独改 dropIdx_，避免两条路各写一份重算。
#include "bookmark_bar.h"

#include "commands.h"
#include "config.h"

// 悬停分类自动弹出的等待时长。资源管理器/浏览器的惯例在 500–1000ms 之间：
// 太短会"路过分类就被展开"，太长则等得着急。
constexpr UINT kFolderOpenHoverMs = 600;

// 边缘自动滚动的起停判定（唯一入口）：内部重排 DragUpdate 与 OLE 外部拖入
// PreviewDropAt 都调它。触发带以可视窗口为准（» 按钮不算落点）；滚动拍在
// WM_TIMER 里经 ScrollBy（落点/hover 的重算收口在那里），这里只起停。
// 非溢出时没有可滚内容，直接停表 —— 与标签栏 WM_MOUSEMOVE 的分支同构。
void BookmarkBar::EdgeAutoScroll(int clientX, int clientY) {
    dragMouseX_ = clientX;
    dragMouseY_ = clientY;
    if (!overflow_) {
        edge::Stop(&dragScrollStep_, hwnd_, kDragScrollTimer);
        return;
    }
    const int edge = theme_->S(28);
    const int lo = theme_->S(6) + edge;
    const int hi = viewRight_ - edge;
    edge::Update(&dragScrollStep_, hwnd_, kDragScrollTimer, clientX, lo, hi,
                 theme_->S(24), static_cast<UINT>(Config::Get().s().tabDragScrollMs));
}

void BookmarkBar::DragUpdate(HWND h, int x, int y) {
    // 落点算法与 OLE 外部拖放共用同一份 DropSpotAtClient，避免两条路各写一套
    // "分类中间 50% / 其余插插入线 / 栏外取消"的口径。
    (void)h;  // 命中数据来自 items_ / ovfRc_，窗口句柄由 hwnd_ 持有。
    const BookmarkDropSpot spot = DropSpotAtClient(x, y);
    const int newDrop = spot.valid ? spot.index : -1;
    const int newInto = spot.folderIndex;
    if (newDrop != dropIdx_ || newInto != dropInto_ || !dropPreview_) {
        dropIdx_ = newDrop;
        dropInto_ = newInto;
        dropPreview_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    // 边缘自动滚动：把落点推进到视野外。触发带以可视窗口为准（» 按钮不算落点）；
    // 滚动本身经 ScrollBy（里面统一重算落点），这里只管起停。
    EdgeAutoScroll(x, y);
    // 悬停分类自动弹出：拖进分类"内部"并停住 → 定时器到点移交弹层（WM_TIMER）。
    // 换了分类重挂定时器，拖出分类摘掉；拖自己不触发（那是顶层重排，不是移入）。
    const bool overFolder = newInto >= 0 && dragIdx_ >= 0 && newInto != dragIdx_;
    if (overFolder) {
        if (folderHoverIdx_ != newInto) {
            folderHoverIdx_ = newInto;
            SetTimer(hwnd_, kDragFolderTimer, kFolderOpenHoverMs, nullptr);
        }
    } else if (folderHoverIdx_ != -1) {
        folderHoverIdx_ = -1;
        KillTimer(hwnd_, kDragFolderTimer);
    }
}

void BookmarkBar::DragFinish(HWND h, bool commit) {
    int from = dragIdx_;
    int to = dropIdx_;
    int into = dropInto_;
    dragArm_ = dragging_ = false;
    dragIdx_ = dropIdx_ = dropInto_ = -1;
    dropPreview_ = false;
    edge::Stop(&dragScrollStep_, hwnd_, kDragScrollTimer);   // 滚着松手：先停表再提交
    KillTimer(h, kDragFolderTimer);
    folderHoverIdx_ = -1;
    // 下标必须**在这里重新校验**：from/into 是拖拽开始时记下的行号，而 items_
    // 可能在拖拽期间被外部改短（AI/命令删了书签 → 栏重建 → 行数变少）。
    // 只判 >= 0 就索引是越界读（Release 下读到垃圾 id，Debug 下直接断言）。
    const int n = static_cast<int>(items_.size());
    if (from >= n) from = -1;
    if (into >= n) into = -1;
    if (to > n) to = n;
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

// 悬停分类到点：把进行中的拖拽移交给分类内容弹层。
//
// 【移交而不是换手】弹层一开就 SetCapture(list)，书签栏的捕获被接走，
// 栏会同步收到 WM_CAPTURECHANGED 并复位拖拽状态 —— 所以需要的值必须在
// 放手之前取走。移交后落点跟弹层走：压行画插入线，松手 bmk.move 按行落位；
// 拖回书签栏松手 = 按栏落点提交（onDragOut）；拖出程序 = 取消。
void BookmarkBar::HandoffDragToFolder(HWND h) {
    const int folderIdx = dropInto_;
    const int dragIdx = dragIdx_;
    // 两个下标都要判**上界**：悬停计时器到点时才走到这里，而 items_ 可能已经
    // 被 Reload() 重建变短（拖拽期间 AI/命令改了书签）。只判 >= 0 就索引
    // items_[dragIdx] 是越界读 —— 与 DragFinish 用同一套判据。
    const int n = static_cast<int>(items_.size());
    if (dragIdx < 0 || dragIdx >= n || folderIdx < 0 || folderIdx >= n ||
        folderIdx == dragIdx)
        return;
    const Item& folder = items_[static_cast<size_t>(folderIdx)];
    if (!folder.folder) return;
    KillTimer(h, kDragFolderTimer);
    folderHoverIdx_ = -1;
    edge::Stop(&dragScrollStep_, hwnd_, kDragScrollTimer);   // 移交后滚动也停（弹层接管落点）

    // 空分类弹不出内容（ShowFolderContents 见空菜单直接返回）：退回旧语义，
    // 直接按"加入末尾"提交，不能让这次拖放静默消失。
    const BookmarkNode* node = BookmarkStore::Get().Find(folder.id);
    if (!node || !node->isFolder || node->children.empty()) {
        DragFinish(h, true);
        return;
    }

    const std::wstring dragId = items_[static_cast<size_t>(dragIdx)].id;
    const RECT rc = folder.rc;
    POINT anchor{rc.left, rc.bottom + 2};
    ClientToScreen(h, &anchor);
    // 先清自己的拖拽态再放手：ReleaseCapture 会同步派发 WM_CAPTURECHANGED，
    // 那边的复位是兜底，这里的清理才是移交本身。
    dragArm_ = dragging_ = false;
    dragIdx_ = dropIdx_ = dropInto_ = -1;
    dropPreview_ = false;
    InvalidateRect(h, nullptr, FALSE);
    ReleaseCapture();
    ShowFolderContents(folder.id, anchor, dragId);
}
