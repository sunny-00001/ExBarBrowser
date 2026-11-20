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
bool EnsureDir(const std::wstring& path);
bool CopyFileIfMissing(const std::wstring& from, const std::wstring& to);

std::wstring ExeDir();
std::wstring Join(const std::wstring& a, const std::wstring& b);
std::wstring FileName(const std::wstring& path);

}  // namespace tf
