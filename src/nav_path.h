// 导航路径语义：全项目唯一实现。
//
// 为什么独立成一个组件而不是散在 common.h / nav_model.cpp 里：
//   这些函数（父级、叶子名、盘符根、能不能向上）以前有两套 —— common.h 一套
//   （无长度限制），NavModel 一套（基于 PathRemoveFileSpecW + MAX_PATH）。两套
//   规则不一致：同一个路径，"上一级"和"地址栏补全"算出的父目录可能不同；超过
//   260 字符时 NavModel 那套直接返回空，功能静默失效。
//
// 边界：本文件只做**纯字符串**语义，不碰 Shell、不碰窗口、不碰 COM。
// 需要枚举 shell 命名空间（子文件夹、显示名）的留在 nav_model.cpp。
#pragma once
#include <string>

namespace nav {

// 桌面命名空间的根 = 资源管理器里的"此电脑"。盘符根的上一级就是它。
inline constexpr wchar_t kThisPc[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

// 命名空间路径（::{GUID} / shell:xxx）。这类路径没有"父目录"语义：
// 再往上就是桌面，属意外跳转，所以向上/父级一律返回空。
inline bool IsNamespace(const std::wstring& p) {
    return p.rfind(L"::", 0) == 0 || p.rfind(L"shell:", 0) == 0;
}

// 盘符根：接受 "C:" 与 "C:\" 两种写法（地址栏/命令行都有人这么给）。
inline bool IsDriveRoot(const std::wstring& p) {
    if (p.size() < 2 || p.size() > 3) return false;
    if (p[1] != L':') return false;
    if (p.size() == 3 && p[2] != L'\\' && p[2] != L'/') return false;
    const wchar_t c = p[0];
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

// 归一化：正斜杠转反斜杠（SHParseDisplayName / SHCreateItemFromParsingName 只认
// 反斜杠），去掉多余的尾斜杠。命名空间原样返回。
inline std::wstring Normalize(std::wstring p) {
    if (IsNamespace(p)) return p;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

// 上一级目录。命名空间、盘符根、UNC 根返回空串 —— "有没有去处"由 UpTarget 统一
// 判断，不要在这里替调用方决定"留在原地"。
//
// 不用 MAX_PATH 栈缓冲：超长路径（>260）与 UNC 根以前会静默返回空，
// 导致 /select 超长路径、盘根、网络共享的"上一级"全部失效。
inline std::wstring ParentOf(const std::wstring& path) {
    if (path.empty() || IsNamespace(path)) return std::wstring();
    std::wstring s = Normalize(path);

    // 盘根 "C:\" 与 UNC 根 "\\server\share" 没有更上级
    if (s.size() == 3 && s[1] == L':' && s[2] == L'\\') return std::wstring();
    const bool unc = s.rfind(L"\\\\", 0) == 0;
    if (unc) {
        size_t first = s.find(L'\\', 2);
        if (first == std::wstring::npos) return std::wstring();
        size_t second = s.find(L'\\', first + 1);
        size_t cut = s.find_last_of(L'\\');
        // cut == second 表示"父级正好是 UNC 根"（\\server\share\dir -> \\server\share），
        // 这是有效上一级。以前写成 cut <= second 把它一起排掉，
        // 结果网络共享下任何子目录的"上一级"都静默失效。
        if (second == std::wstring::npos || cut == std::wstring::npos || cut < second)
            return std::wstring();
        return s.substr(0, cut);
    }

    size_t cut = s.find_last_of(L'\\');
    if (cut == std::wstring::npos) return std::wstring();
    if (cut == 2 && s[1] == L':') return s.substr(0, 3);  // "C:\A" -> "C:\"
    return s.substr(0, cut);
}

// 末级名称（标签标题、面包屑标签用）。盘根返回 "C:\"，命名空间原样返回。
inline std::wstring Leaf(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    if (IsNamespace(path)) return path;
    // 盘根没有"文件名"，PathFindFileNameW("C:\") 返回整串，直接用更稳。
    if (IsDriveRoot(path)) {
        std::wstring s = Normalize(path);
        return s.size() >= 3 ? s : s + L'\\';
    }
    const std::wstring s = Normalize(path);
    const size_t cut = s.find_last_of(L'\\');
    return cut == std::wstring::npos ? s : s.substr(cut + 1);
}

// "按上一级该去哪"。空串 = 没有去处（命名空间 / UNC 根 / 空路径）。
//
// 这是导航语义里唯一允许回答"能不能向上"的地方：CanNavigateUp 由它派生，
// 命令前置检查、工具栏按钮灰显、GoUp 的执行三者都读这里。
// 以前它们各写一段 if，口径一分叉就会出现"按钮亮着但点了没反应"。
inline std::wstring UpTarget(const std::wstring& path) {
    if (path.empty() || IsNamespace(path)) return std::wstring();
    // 盘符根的上一级不是"没有"，而是"此电脑" —— 与资源管理器一致。
    if (IsDriveRoot(path)) return kThisPc;
    return ParentOf(path);
}

inline bool CanNavigateUp(const std::wstring& path) { return !UpTarget(path).empty(); }

}  // namespace nav
