// 外部拖入路径的落点预览与命令提交。
// 职责边界：本文件只把 BookmarkDropSpot 翻译成 bmk.add.path 参数；
// 真正改树仍在命令 handler -> BookmarkStore，拖放侧不直接写 store。
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
