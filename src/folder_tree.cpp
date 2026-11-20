#include "folder_tree.h"

#include "icon_resolve.h"
#include "nav_model.h"

namespace folder_tree {

std::vector<TreeNode> Children(const std::wstring& folder) {
    std::vector<TreeNode> out;
    if (folder.empty()) return out;

    // 枚举走 NavModel::ChildFolders —— shell 命名空间枚举的唯一实现
    // （已排除隐藏项、按名称自然排序、支持 ::{GUID} 等虚拟路径）。
    std::vector<std::pair<std::wstring, std::wstring>> kids;
    if (!NavModel::ChildFolders(folder, kids)) return out;

    out.reserve(kids.size());
    for (const auto& kv : kids) {
        // hasChildren 先给 true：真正有没有下一级由弹层首次展开时懒加载判定
        // （枚举一次目录在慢盘/网络共享上要几十毫秒，不能在建树时全量递归）。
        out.push_back({kv.first, kv.second, ico::Resolve(kv.second, true), 0, -1, {}, true, false,
                       false});
    }
    return out;
}

std::wstring Show(HWND owner, const Theme& theme, POINT at, std::vector<TreeNode> top,
                  int minWidthPx) {
    if (top.empty()) return std::wstring();
    // 懒加载回调 = Children 本身：点 chevron 就地展开，与地址栏分隔符同一行为。
    return ShowFolderTree(owner, theme, at, std::move(top), Children, minWidthPx);
}

}  // namespace folder_tree
