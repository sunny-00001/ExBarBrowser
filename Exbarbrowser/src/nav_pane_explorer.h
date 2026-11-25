// 导航树根集的**运行时发现**：把 explorer 左栏的结构还原成一份可配置的根清单。
//
// 【为什么需要这一层】配置里的 navPaneRoots 是静态文本，但 explorer 左栏的
// 内容是**随机器变化**的：装了新的网盘就多一项、重命名就换名字。
// 硬编码一份 GUID 清单在别的机器上必然对不上 —— 探针 69 实测：本机 HKCU
// NameSpace 下有 17 个子键，其中 2 个（Everything 结果 / OpenList 裸）是
// **本项目自己的 Windhawk mod** 注入的，explorer 并不显示它们。
//
// 【数据源（两条，全部实测 —— 探针 61/69）】
//  1. Home（主页）命名空间**自身**的子项 —— explorer 左栏组2 的
//     桌面/Downloads/文档/图片/音乐/视频/**tools** 全部来自这里。
//     取法：SHGetIDListFromObject -> SHBindToParent -> parent->BindToObject(childLast)
//     得到「自身」的 IShellFolder，再 EnumObjects；显示名用**父 folder 的
//     GetDisplayNameOf(childPidl)**（用自身取名全是空串，探针 58/60/61 实测）。
//  2. 注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace
//     —— 每个子键是一个被固定到导航树的命名空间扩展。
//
// 【三组的构成（探针 69 全量实测 + 探针 45/53 基线截图）】
//  组0（3 项）主文件夹(Home 679F85CB) / 图库(E88865EA) / TAN - 个人(OneDrive 个人)
//  组1（7 项）桌面 / Downloads / 文档 / 图片 / 音乐 / 视频 / tools
//  组2（17 项）14 个第三方扩展（按显示名 Unicode 码位升序）+ 此电脑 / 库 / 网络
// 合计 27 项，与 explorer 左栏逐项对应。
//
// 【组0 第三项与组2 前 14 项的分工】HKCU NameSpace 里带 FolderValueFlags 的
// 恰好 15 个：其中 {018D5C66}(OneDrive 个人账号，显示名随账号变，故运行时取)
// 归组0，其余 14 个归组2。没有 FolderValueFlags 的 2 个是本项目注入的，排除。
//
// 【组2 前 14 项的排序】按**显示名的 Unicode 码位升序**（不是 std::wstring 的
// operator< —— 那是按内存字节比，UTF-16 小端下会把"天"排到"小"后面）。
#pragma once
#include "nav_pane_root.h"

#include <string>
#include <vector>

namespace navpane {

// 运行时还原 explorer 左栏的根清单。
// 返回的顺序、名字、分组都与 explorer 一致（实测）。
// 每项的 RootSpec::group 是 0/1/2 三组，组间要画分隔线。
std::vector<RootSpec> DiscoverExplorerRoots();

}  // namespace navpane
