// search-ms: URL 的协议形式（拼装规则 + 自测）。
//
// 【现状：程序执行搜索不走这里】搜索的执行改走**原生搜索文件夹**
// （src/search_folder.h，ISearchFolderItemFactory 造的 PIDL）—— 因为 URL 形式
// 在本宿主根本导航不了：ShellView::DoNavigate 明确拒绝 search-ms: 前缀
// （见 docs/EXPLORER-GAP-AUDIT.md §2.1 F1），BrowseToObject 只会返回 S_OK 却把
// 视图留在原地，"搜索成功"成了一句假话。
//
// 本模块保留的是**协议本身**的拼装规则：搜索词的规范化（Trim）与引号转义
// 仍是搜索链路上真实存在、且必须有测试兜住的部分（见 tests/search_url_test.cpp）。
// 谁要是把搜索改回 URL 导航，这里就是现成的契约。
#pragma once
#include <string>

namespace searchurl {

// 去掉首尾空白。搜索框提交时不做这件事，于是"只打了几个空格"会被当成搜索词，
// 搜出一屏 0 项 —— 用户以为搜索坏了。规范化放在这里（调用方唯一入口），
// 而不是散在导航栏和命令两处。
std::wstring Trim(const std::wstring& s);

// query 为空 → 返回空串，调用方据此"取消搜索回到原目录"
// （搜索视图没有上一级，只能靠记下来的来源目录退回去）。
//
// scope 是搜索范围（一般是当前目录）。原样拼进 crumb=location:，不做转义 ——
// 这是**现状行为**，不是理想行为：路径里含 `&` 时会被当成参数分隔符。
// 之所以不动它：本函数由资源管理器自己的 search-ms 协议解释，改拼接方式
// 可能让现在能用的路径搜不出东西，需要真机逐类验证。这里先把契约固定下来
// （见 tests/search_url_test.cpp），要改就是一次有测试兜底的改动。
std::wstring Build(const std::wstring& query, const std::wstring& scope);

}  // namespace searchurl
