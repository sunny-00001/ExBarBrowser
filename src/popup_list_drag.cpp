// 弹层行内拖拽重排：落点计算、拖出预览、提交。
// 与 popup_list_input.cpp 的分工：那边是输入状态机（按下/移动/松手的分流），
// 这里是"拖"本身的语义（落点在哪、松手怎么算、提交后怎么刷新）。
// 捕获与阈值约定见 popup_list_input.cpp 的 ListSubclass。
#include "popup_list_internal.h"

#include "config.h"
#include "popup_list_draw.h"

// 【为什么自己算而不是问 ListBox】LB_ITEMFROMPOINT 只给"压着哪一行"，
// 不给"这一行的上半还是下半" —— 而这正是"插到它前面还是后面"的区别。
static int ComputeDropIndex(PopupState* st, HWND h, int x, int y) {
    const int n = RowCount(st);
    if (n <= 0) return -1;
    int row = ItemFromPoint(h, x, y);
    bool lower = false;
    if (row < 0 || row >= n) {
        // 光标落在最后一行**之下**：允许插到末尾（否则"拖到最后"无处可放）。
        RECT rc;
        GetClientRect(h, &rc);
        POINT pt{x, y};
        if (!PtInRect(&rc, pt)) return -1;
        row = n - 1;
        lower = true;
    } else {
        RECT ir;
        if (SendMessageW(h, LB_GETITEMRECT, static_cast<WPARAM>(row),
                         reinterpret_cast<LPARAM>(&ir)) >= 0) {
            lower = y > (ir.top + ir.bottom) / 2;
        }
    }
    return PopupDropIndex(row, lower, n);
}

// 拖出列表时，弹层不再画自己的插入线；预览/提交都交给宿主的 onDragOut*。
// 收到 {-1,-1} 的约定见 popup_list.h 的 PopupReorder 注释。
void ClearDragOutPreview(PopupState* st) {
    if (st->dragOutside && st->reorder.onDragOutPreview)
        st->reorder.onDragOutPreview(st->dragFrom, POINT{-1, -1});
    st->dragOutside = false;
}

void UpdateDragPosition(PopupState* st, HWND h, int x, int y) {
    if (!st->dragging) return;
    RECT rc;
    GetClientRect(h, &rc);
    POINT cl{x, y};
    if (!PtInRect(&rc, cl)) {
        st->dragOutside = true;
        st->dropIndex = -1;
        if (st->reorder.onDragOutPreview) {
            POINT screen = cl;
            ClientToScreen(h, &screen);
            st->reorder.onDragOutPreview(st->dragFrom, screen);
        }
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (st->dragOutside) {
        st->dragOutside = false;
        if (st->reorder.onDragOutPreview)
            st->reorder.onDragOutPreview(st->dragFrom, POINT{-1, -1});
    }
    const int drop = ComputeDropIndex(st, h, x, y);
    if (drop != st->dropIndex) {
        st->dropIndex = drop;
        InvalidateRect(h, nullptr, FALSE);
    }
}

// 重建 ListBox 的行内容（平铺模式）。重排后行序变了，不重建的话用户看到的
// 还是旧顺序 —— 表现为"拖了没反应"。
static void RebuildListRows(PopupState* st) {
    const int top = static_cast<int>(SendMessageW(st->list, LB_GETTOPINDEX, 0, 0));
    SendMessageW(st->list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(st->list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < st->rows->size(); ++i) {
        const int id = static_cast<int>(SendMessageW(
            st->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>((*st->rows)[i].label.c_str())));
        if (id >= 0) {
            SendMessageW(st->list, LB_SETITEMDATA, static_cast<WPARAM>(id),
                         static_cast<LPARAM>(i));
            SendMessageW(st->list, LB_SETITEMHEIGHT, static_cast<WPARAM>(id),
                         RowHeight(st, static_cast<int>(i)));
        }
    }
    SendMessageW(st->list, WM_SETREDRAW, TRUE, 0);
    SendMessageW(st->list, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
    st->curSel = -1;
    st->hot = -1;
    InvalidateRect(st->list, nullptr, FALSE);
}

void PopupApplyReorder(PopupState* st, int from, int to) {
    if (!st || !st->reorder.onReorder) return;
    if (to < 0) return;
    // from = -1 是"外部移交"的拖拽（书签栏悬停分类自动弹出时的那一场），
    // 只有宿主显式开了 allowExternalFrom 才放行 —— 否则负数一律是状态错乱。
    if (from < 0 && !st->reorder.allowExternalFrom) return;
    st->reorder.onReorder(from, to);
    // 宿主重建行内容 → 弹层就地刷新（**不关窗口**：用户接着还能拖下一条）。
    if (!st->reorder.rebuildRows || !st->ownsRows) return;
    std::vector<PopupRow> fresh;
    if (!st->reorder.rebuildRows(&fresh)) return;
    st->rowsOwned = std::move(fresh);
    st->rows = &st->rowsOwned;
    RebuildListRows(st);
}
