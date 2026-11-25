#pragma once
#include "bookmarks.h"

#include "str.h"

namespace bmkstore::detail {

// 给空 id 发新号。内部 store 的 Netscape 解析与树模型新增都走这一处：
// 前者要把文件里已有的 EXBAR_ID 接住，后者要给新节点发号，口径分叉会让
// Find() 命中错节点，删除/改名作用到别人身上。
void AssignIds(BookmarkList& list, int& seq);

// 找到 id 所在的（父）列表与它在该列表里的下标；找不到返回 nullptr。
//
// 【为什么在头里而不是各写一份】bookmarks.cpp 的"复制"与 bookmarks_ops.cpp 的
// "解散"都要它。两处各抄一份的话，"找不到"这个返回值迟早被处理成不同答案 ——
// 而"找不到"正是"操作失败"与"操作成功但没东西可做"的分界点。
inline BookmarkList* ListContaining(BookmarkList& list, const std::wstring& id, size_t* idx) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == id) {
            *idx = i;
            return &list;
        }
        if (BookmarkList* sub = ListContaining(list[i].children, id, idx)) return sub;
    }
    return nullptr;
}

// 名称序比较：分类在前（foldersFirst）+ 名称不区分大小写；ascending 决定方向。
//
// 【为什么要共享】排序规则只有这一份。bookmarks.cpp 的整树排序与
// bookmarks_ops.cpp 的"只排某个分类"如果各写一个 cmp，迟早会在
// "大小写怎么处理"上分叉 —— 而那正是用户最先看见的差异。
inline bool BookmarkLess(const BookmarkNode& a, const BookmarkNode& b, bool foldersFirst,
                         bool ascending) {
    if (foldersFirst && a.isFolder != b.isFolder) return a.isFolder;
    const int c = str::ICmp(a.name, b.name);
    return ascending ? c < 0 : c > 0;
}

}  // namespace bmkstore::detail
