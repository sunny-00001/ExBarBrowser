// 系统搜索结果文件夹（Windows Search 索引）的唯一构造点。
//
// 【为什么不用 search-ms: URL】资源管理器自己的 search-ms: 是**协议处理器 URL**，
// 不是可浏览的文件夹：宿主 IExplorerBrowser 没有搜索带，ShellView::DoNavigate 对
// search-ms: 前缀明确拒绝（BrowseToObject 返回 S_OK 却把视图留在原地 —— 于是
// "搜索成功"变成一句假话）。原生搜索文件夹（ISearchFolderItemFactory 造的 PIDL）
// 是**真的文件夹**，shell 能绑定它，导航过去就是搜索结果视图。
// 同一套做法见 D:\Windhawk\exbar.cpp:26215 RunSystemSearchFolder。
//
// 【作用域的代价】ISearchFolderItemFactory::SetScope 给的是**递归**范围，
// 没有 .search-ms 那种 nonRecursive —— 所以"仅本层"不能靠作用域表达，
// 只能用条件里的"父目录恰好等于 scopeDir"来圈（见 onlyThisLayer）。
//
// 【作用域只能是一个普通目录 —— 全部实测，2026-11-22】能绑上并出结果的：
//   "C:\Windows\"、"C:\Program Files\"、"C:\ProgramData\"、
//   "C:\Users\X\Documents\"、"C:\Users\X\"、"E:\ExBarBrowser\" 等普通目录；
// 绑不上的（BrowseToObject 返回 S_OK，视图留在原地，随后 OnNavigationFailed）：
//   · 不带尾斜杠的已知 shell 位置（"C:\Users\X" → 被规范化成 CSIDL_PROFILE
//     那个命名空间项；"C:\Users" 同理）—— 所以作用域一律补尾斜杠；
//   · 卷根（"C:\"、"D:\"）：绑上了但查不到东西（卷根不是 crawl scope）；
//   · "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"（此电脑命名空间）；
//   · 多个目录组成的 IShellItemArray（哪怕每个单独都能用）；
//   · 不给作用域 —— "整台电脑"要靠它，但那是 Explorer 进程内的 search-ms
//     全局查询，本宿主没有那个宿主，于是绑不上。
// 所以"整台电脑"这一档在本宿主**做不到**（见 searchscope::kThisPC 的说明）：
// 要真正的整机搜索，得自己查 Windows Search 的查询 API（searchapi.h，本编译器
// 没有）或做混合虚拟文件夹。
#pragma once
#include "common.h"
#include <shlobj.h>

namespace searchfolder {

// 造一个"搜索结果"文件夹的 PIDL。
//   query        搜索词（按**文件名包含**匹配，与资源管理器搜索框同义）
//   scopeDir     限定目录（**必填**，见头文件的作用域约束）
//   onlyThisLayer true = 只搜 scopeDir **本层**（父目录恰好等于 scopeDir 的项）
//   displayName  结果文件夹的显示名（会出现在地址栏；必须随范围变，见 App::RunSearch）
// 成功返回 true，*outPidl 由调用者 CoTaskMemFree。
// 失败 = 本机没有可用的 Windows Search（精简版系统 / 索引服务缺失 / 条件建不出来）。
bool Create(const std::wstring& query, const std::wstring& scopeDir, bool onlyThisLayer,
            const std::wstring& displayName, PIDLIST_ABSOLUTE* outPidl);

// 本机有没有可用的系统搜索。探过 CoCreateInstance 才知道，不猜 ——
// "系统没装 Windows Search"和"条件建不出来"是两回事，报错文案不能混。
bool Available();

}  // namespace searchfolder
