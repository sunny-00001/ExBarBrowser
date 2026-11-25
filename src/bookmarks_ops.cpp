// 书签树的"层级重组"：解散分类、只排某一个分类。
//
// 【为什么与 bookmarks.cpp 分开】bookmarks.cpp 是"增删改查"的日常路径，
// 这里两条动的是**父子关系**（一次改动整段层级结构），失败口径、撤销口径
// 都与日常路径不同。混在一起时，读者要翻到文件末尾才发现还有另一套语义 ——
// 而"一个文件只做一件事"正是 AGENTS.md §3 的要求。
//
// 【撤销纪律】两条都改树，所以都必须在**改之前**拍快照（PushUndo）。
// 解散分类尤其需要：它一次动很多条，用户发现点错了必须能一步退回来。
#include "bookmarks.h"

#include "bookmarks_internal.h"

#include <algorithm>

std::vector<std::wstring> BookmarkStore::Dissolve(const std::wstring& id) {
    std::vector<std::wstring> promoted;
    if (id.empty()) return promoted;

    size_t idx = 0;
    BookmarkList* list = bmkstore::detail::ListContaining(roots_, id, &idx);
    if (!list || idx >= list->size()) return promoted;
    if (!(*list)[idx].isFolder) return promoted;   // 叶子没有"解散"可言

    // 【快照必须在改树之前】与 Remove/Copy/MoveTo 同纪律：声明了 undoable
    // 却没有栈，等于对调用方（尤其 AI）说谎。
    PushUndo();

    // 把分类整棵摘出来（含它的子树），再把它的**直接子项**插回它原来的位置。
    //
    // 为什么"整棵摘出来"而不是只摘分类本身：分类节点里带着 children，
    // 直接 erase 会连带把子树一起丢掉。摘出来之后，子项各自带着自己的子树
    // 回到上一层 —— 解散掉的是"这一层壳"，不是把整棵树摊平。
    //
    // 指针安全性：list 指向的是**父节点的 children**（或 roots_），
    // erase/insert 只动那个 vector 里的元素，不会让 list 本身失效；
    // 被摘走的分类节点是局部副本，与 list 无生命周期关系。
    BookmarkNode node = std::move((*list)[idx]);
    list->erase(list->begin() + static_cast<long>(idx));
    for (size_t i = 0; i < node.children.size(); ++i) {
        promoted.push_back(node.children[i].id);
        list->insert(list->begin() + static_cast<long>(idx + i), std::move(node.children[i]));
    }
    Touch();
    return promoted;
}

bool BookmarkStore::SortChildren(const std::wstring& folderId, bool foldersFirst,
                                 bool ascending) {
    if (folderId.empty()) return false;
    BookmarkNode* folder = Find(folderId);
    if (!folder || !folder->isFolder) return false;
    // 只有一个子项时无事可做：返回成功，但**不占撤销额度**
    // （与 RemoveDuplicatePaths 的"没有重复就不留快照"同一纪律 ——
    //  否则用户点一次"排序本分类"就凭空多出一个可撤销步骤）。
    if (folder->children.size() < 2) return true;

    PushUndo();
    std::sort(folder->children.begin(), folder->children.end(),
              [&](const BookmarkNode& a, const BookmarkNode& b) {
                  return bmkstore::detail::BookmarkLess(a, b, foldersFirst, ascending);
              });
    Touch();
    return true;
}
