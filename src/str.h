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

// 宽松版：非法字节序列替换成 U+FFFD，不报错。
// 适用于"内容来自本进程/协议、错字节不值得让整个操作失败"的场景（JSON-RPC 行、
// 编译期字面量）。
std::wstring FromUtf8(const std::string& s);
// NUL 结尾的 C 串版（argsSchema 这类编译期字面量）。nullptr / 空串返回空。
std::wstring FromUtf8(const char* s);

// 严格版：遇到非法 UTF-8 字节序列**失败**（返回空 + ok=false），不静默替换。
//
// 【为什么必须有这个版本】宽松版把 GBK 的"中文"（D6 D0 CE C4）解成 4 个 U+FFFD —
// 中文名全变成 "????"，而调用方毫不知情。这在"读用户文件"的路径上是灾难：
// 老浏览器导出的 GBK 书签、记事本另存的 ANSI 文件，都会被静默损坏，
// 下一次 Save 就把损坏结果原子替换回原文件。
//
// 判据：读用户文件用严格版（失败就报错/拒绝写盘），解析协议用宽松版。
bool FromUtf8Strict(const std::string& s, std::wstring* out);

}  // namespace str
