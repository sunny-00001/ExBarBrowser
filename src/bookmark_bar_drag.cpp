// 书签栏顶层的拖动重排：算落点 + 提交移动。
//
// 【为什么单独成文件】拖动状态机是"输入 → 落点 → 提交"三段式，
// 和栏的绘制/命中是两种职责；而提交那一段必须与**分类内拖动**保持同一套
// 命令口径（bmk.move），两处各写一份必然分叉。拆开才读得动。
#include "bookmark_bar.h"

#include "commands.h"

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
}

void BookmarkBar::DragFinish(HWND h, bool commit) {
    int from = dragIdx_;
    int to = dropIdx_;
    int into = dropInto_;
    dragArm_ = dragging_ = false;
    dragIdx_ = dropIdx_ = dropInto_ = -1;
    dropPreview_ = false;
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
