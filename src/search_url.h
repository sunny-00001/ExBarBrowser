// search-ms: URL 的唯一拼装点。
//
// 【为什么单独拿出来】这段拼装原来长在导航栏搜索框的回调里，于是：
//   · 命令总线（命令面板 / 键位 / agent）根本用不到它 —— `search.find` 一直
//     是 implemented=false，用户能看到搜索框、机器却"没有搜索这个能力"；
//   · 拼错了没法测（要真起一个 IShellView 才能看出结果不对）。
// 抽成纯函数后，引号转义这类细节可以被单测按住。
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
