// 文本文件读写与路径工具（唯一实现，禁止各模块自带 CreateFileW 读配置）
//
// 约定：
//   · 磁盘上的配置/资产一律 UTF-8（无 BOM 写出；读入时兼容 UTF-8 BOM 与 UTF-16 LE BOM）。
//   · 日志仍走 common.h 的 LogDbg（%TEMP%\ExBarBrowser.log）。
#pragma once
#include <string>

namespace tf {

std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

bool ReadAllBytes(const std::wstring& path, std::string* out);
bool ReadText(const std::wstring& path, std::wstring* out);   // 自动识别 BOM
bool WriteText(const std::wstring& path, const std::wstring& text);  // UTF-8 无 BOM

// 把路径转成 Win32 的"扩展长度"形式，绕过 MAX_PATH(260) 限制。
//
// 【为什么需要】本工程的导航层（nav_path.h）早就把路径语义从 MAX_PATH 里解放
// 出来了，但执行层没跟上：CreateFileW / FindFirstFileW / GetFileAttributesExW
// 拿到 >260 字符的路径会直接失败，而调用方往往把失败当成"文件不存在"或
// "权限不足" —— 表现为深目录静默读不到、体积统计把它计成 0。
//
// 规则（照 Win32 文档）：
//   · 已是 \\?\ 或 \\.\ 前缀 → 原样返回（幂等，可重复调用）
//   · UNC 路径 \\server\share\x → \\?\UNC\server\share\x
//   · 普通绝对路径 C:\x        → \\?\C:\x
//   · 相对路径、shell: / ::{GUID} 这类命名空间 → 原样返回
//     （扩展前缀只对文件系统路径有效，套在命名空间上会让它彻底打不开）
std::wstring ToExtended(const std::wstring& path);

// 追加一行（含换行）到文件，并按大小轮转（只保留一份 .1）。
//
// bytes 是**已编码好的字节**：调用方自己决定 UTF-8 还是 UTF-16。
// 这是刻意的 —— 调试日志用 UTF-16 LE（`log.cpp`，便于 Get-Content -Encoding
// Unicode 直接看），审计日志用 UTF-8（`commands.cpp`，便于脚本 grep）。
// 编码不同是需求，不该被"统一"掉。
//
// 真正需要共用的只有轮转这一段：`GetFileAttributesExW` + 判 hi/lo +
// `MoveFileEx(REPLACE_EXISTING)`。这段以前在 log.cpp 与 CommandBus::Audit
// 里各写一遍（阈值 4MB/500 行 vs 2MB/200 行，其余逐字相同）。
//
// checkEveryN：每 N 次调用才查一次大小（避免每行都 stat）。
// rotateAtBytes：超过它就改名成 `<path>.1`（旧的 .1 被覆盖）。
bool AppendLine(const std::wstring& path, const std::string& bytes, unsigned long long rotateAtBytes,
                int checkEveryN);

bool Exists(const std::wstring& path);
// 是不是目录。不存在的路径返回 false（调用方应先用 Exists 区分"不存在"与"是文件"）。
bool IsDirectory(const std::wstring& path);

// 文件最后写入时间：**十进制 Unix 秒**的字符串形态。取不到返回空串。
//
// 【为什么用字符串而不是整数】JSON 的 Value 只有 GetInt（32 位）——
// Unix 秒在 2038 年就溢出；而且"取不到"与"时间是 0"必须能分开，
// 空串正好表达"取不到"，0 表达"1970 年"（那也是真实值，不该被当成缺失）。
//
// 【为什么是 Unix 秒而不是本地格式串】调用方要的是**可比较**的时间戳：
// 本地格式串在不同时区/语言下不可比，机器没法拿它判断"哪一版更新"。
std::wstring FileMtimeUnix(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
bool CopyFileIfMissing(const std::wstring& from, const std::wstring& to);

std::wstring ExeDir();
std::wstring Join(const std::wstring& a, const std::wstring& b);
std::wstring FileName(const std::wstring& path);

}  // namespace tf
