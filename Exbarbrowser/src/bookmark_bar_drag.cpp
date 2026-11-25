// 书签栏顶层的拖动重排：算落点 + 提交移动。
//
// 【为什么单独成文件】拖动状态机是"输入 → 落点 → 提交"三段式，
// 和栏的绘制/命中是两种职责；而提交那一段必须与**分类内拖动**保持同一套
// 命令口径（bmk.move），两处各写一份必然分叉。拆开才读得动。
#include "bookmark_bar.h"

#include "commands.h"

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
