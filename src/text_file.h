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

bool Exists(const std::wstring& path);
// 是不是目录。不存在的路径返回 false（调用方应先用 Exists 区分"不存在"与"是文件"）。
bool IsDirectory(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
bool CopyFileIfMissing(const std::wstring& from, const std::wstring& to);

std::wstring ExeDir();
std::wstring Join(const std::wstring& a, const std::wstring& b);
std::wstring FileName(const std::wstring& path);

}  // namespace tf
