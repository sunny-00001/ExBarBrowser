// 外部拖入路径的落点预览与命令提交。
// 职责边界：本文件只把 BookmarkDropSpot 翻译成 bmk.add.path 参数；
// 真正改树仍在命令 handler -> BookmarkStore，拖放侧不直接写 store。
//
// 【边缘自动滚动】外部拖文件夹与内部拖书签必须有同一种"拖到栏边缘自动滚过去"
// 的能力（Chrome/Firefox 对书签栏上的外部拖放同样是这么做的）。OLE 链路的
// DragOver 走 PreviewDropAt：落点预览与 edge 自动滚动的起停一步做完；
// 滚动拍（WM_TIMER）经 ScrollBy —— 落点预览的重算以 dropPreview_ 为守卫
// （外部拖放也写这个标志），所以滚出来的新落点会立刻反映到插入线/分类描边。
#include "bookmark_bar.h"

#include "commands.h"
#include "icon_resolve.h"
#include "json.h"

namespace {

std::wstring DroppedName(const std::wstring& path) {
    if (nav::IsNamespace(path)) return ico::DisplayName(path);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

}  // namespace

void BookmarkBar::PreviewDropAt(POINT screen) {
    // 落点预览（与内部拖动同一份 DropSpotAtClient 口径）+ 边缘滚动起停。
    // 坐标是屏幕点（OLE DragOver 的 pt 即屏幕坐标，见 bookmark_drop.cpp 的说明）。
    PreviewDrop(DropSpotAt(screen));
    POINT cl = screen;
    if (hwnd_) ScreenToClient(hwnd_, &cl);
    EdgeAutoScroll(cl.x, cl.y);
}

void BookmarkBar::PreviewDrop(const BookmarkDropSpot& spot) {
    const int index = spot.valid ? spot.index : -1;
    const int into = spot.valid ? spot.folderIndex : -1;
    if (dropPreview_ == spot.valid && dropIdx_ == index && dropInto_ == into) return;
    dropPreview_ = spot.valid;
    dropIdx_ = index;
    dropInto_ = into;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void BookmarkBar::ClearDropPreview() {
    if (!dropPreview_ && dropIdx_ < 0 && dropInto_ < 0) return;
    dropPreview_ = false;
    dropIdx_ = -1;
    dropInto_ = -1;
    // 预览消失 = 拖放对栏结束（Drop 提交 / DragLeave 拖出 / 拖进分类弹层接管），
    // 自动滚动随之停 —— 两条链路的停表都收口在这里，不留"还在滚的表"。
    edge::Stop(&dragScrollStep_, hwnd_, kDragScrollTimer);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int BookmarkBar::ApplyDroppedPaths(const std::vector<std::wstring>& paths,
                                   const BookmarkDropSpot& spot) {
    int added = 0;
    // 多条同时落下时按拖入顺序占位：每成功一条，插入线后移一位。
    // 失败项不占位置，否则后面的书签会被空位隔开。
    int nextIndex = spot.valid ? spot.index : -1;
    for (const std::wstring& path : paths) {
        if (path.empty()) continue;
        json::Value args = json::Value::MakeObject();
        // 命名空间项（此电脑/回收站…）的名字必须显式给，否则命令默认取 LeafOf
        // 会把裸 "::{GUID}" 显示到栏上。
        args.Set(L"name", json::Value::MakeStr(DroppedName(path)));
        args.Set(L"path", json::Value::MakeStr(path));
        if (!spot.folderId.empty()) {
            args.Set(L"parentId", json::Value::MakeStr(spot.folderId));
        } else if (nextIndex >= 0) {
            args.Set(L"toIndex", json::Value::MakeInt(nextIndex));
        }
        if (!CommandBus::Get().Invoke(CmdId::BmkAdd, Args::FromJson(args), Actor::Drop())) continue;
        ++added;
        if (spot.folderId.empty() && nextIndex >= 0) ++nextIndex;
    }
    if (added) Reload();  // handler 已 Reload 同一实例；这里保证拖放宿主立刻重排。
    return added;
}
