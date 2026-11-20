// 字符串工具（唯一实现）。
//
// 收编理由：Trim + towlower 这一对以前在 bookmarks.cpp / bookmark_io.cpp /
// chord.cpp 各写一遍，UTF-8 转换在 text_file.cpp 之外还有 ai_api / ai_server /
// commands 三处内联样板。规则不一致时表现为"键位能匹配、书签匹配不上"这类
// 静默行为差异，很难查。
//
// 全部为纯函数、零 Win32 依赖（可以放进单测），不改变任何既有语义。
#pragma once
#include <string>

namespace str {

// 去首尾空白（iswspace 口径）
std::wstring Trim(const std::wstring& s);
// 转小写（towlower 口径，不做 locale 之外的处理）
std::wstring Lower(std::wstring s);
// 大小写不敏感比较（_wcsicmp 口径），返回 <0 / 0 / >0
int ICmp(const std::wstring& a, const std::wstring& b);
inline bool IEquals(const std::wstring& a, const std::wstring& b) { return ICmp(a, b) == 0; }

// 前缀判断（大小写不敏感）—— 路径/命令名匹配用
bool IStartsWith(const std::wstring& s, const std::wstring& prefix);

// UTF-8 转换（唯一的 WideCharToMultiByte / MultiByteToWideChar 落点）
std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);
// NUL 结尾的 C 串版（argsSchema 这类编译期字面量）。nullptr / 空串返回空。
std::wstring FromUtf8(const char* s);

}  // namespace str
