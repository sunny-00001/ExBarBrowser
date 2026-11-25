// 导航树"展开整棵树"（Ctrl+Shift+E 的落地）。
//
// 【为什么单独成文件】这是唯一一处**绕过 NSTC 接口、直接驱动内部
// SysTreeView32** 的代码：NSTC 的 ExpandAll 只对根有效，子层没物化前
// 拿不到任何句柄，只能逐个 TVM_EXPAND。它与控件生命周期（建/销/缩放/主题）
// 没有关系，改动理由也不同 —— 混在 nav_pane.cpp 里会让人在找"树怎么建的"
// 时翻到一段纯遍历逻辑。
#include "nav_pane.h"

#include "log.h"

#include <commctrl.h>
#include <shlobj.h>

namespace navpane {

namespace {
// 展开时的节点上限（护栏：网盘/搜索类节点可能有极深或循环结构）。
constexpr int kMaxNodes = 10000;
}  // namespace

bool NavPane::ExpandAll() {
    if (!tree_) return false;
    // NSTC 没有"逐层全部展开"的接口（ExpandAll 只对根有效，子层没物化前拿不到）。
    // 因此走内部 SysTreeView32 逐节点 TVM_EXPAND。
    HWND tv = FindSysTree();
    if (!tv) return false;

    struct Walk {
        int visited = 0;
        int expanded = 0;
    } walk;
    // 迭代式展开，避免递归爆栈（网盘节点可能有几千项）。
    std::vector<HTREEITEM> stack;
    for (HTREEITEM r = reinterpret_cast<HTREEITEM>(
             SendMessageW(tv, TVM_GETNEXTITEM, TVGN_ROOT, 0));
         r; r = reinterpret_cast<HTREEITEM>(
                SendMessageW(tv, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(r)))) {
        stack.push_back(r);
    }
    while (!stack.empty() && walk.visited < kMaxNodes) {
        HTREEITEM cur = stack.back();
        stack.pop_back();
        ++walk.visited;
        if (SendMessageW(tv, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(cur)))
            ++walk.expanded;
        // 展开后才会有孩子
        for (HTREEITEM c = reinterpret_cast<HTREEITEM>(
                 SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(cur)));
             c; c = reinterpret_cast<HTREEITEM>(SendMessageW(
                    tv, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(c)))) {
            if (walk.visited + static_cast<int>(stack.size()) >= kMaxNodes) break;
            stack.push_back(c);
        }
    }
    if (walk.visited == 0) return false;
    RedrawWindow(tv, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    // 展开后行数变了，组间分隔线的位置也得跟着重算（线是画在树外面的覆盖层上）。
    UpdateSeparatorLines();
    LogDbg(L"[navpane] 已展开导航树：访问 %d 项，展开 %d 项", walk.visited, walk.expanded);
    return true;
}

}  // namespace navpane