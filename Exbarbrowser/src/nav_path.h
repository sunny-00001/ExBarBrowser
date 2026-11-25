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
#include <vector>

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

// 用户输入的文本 → 可交给 shell 的路径。地址栏、命令行、剪贴板粘贴共用这一份。
//
// 【为什么必须收口】"资源管理器 → 复制为路径"给出的是**带引号**的
// `"C:\Program Files"`，而用户从聊天窗口复制的路径常带首尾空格、换行。
// 这些文本直接丢给 SHCreateItemFromParsingName 会返回 E_INVALIDARG，
// 表现是"粘贴路径回车没反应"，且失败原因（引号）与用户看到的东西完全无关。
//
// 只做**不改语义**的清理：
//   · 去首尾空白（含全角空格、制表、换行）—— 粘贴带进来的
//   · 剥掉一层配对的引号（' 或 "）—— "复制为路径"带进来的
//   · 正斜杠转反斜杠、去多余尾斜杠 —— Normalize 的既有规则
// 不做的：不展开环境变量（那是 Win32，留在调用方）、不做大小写变换（会改显示名）、
// 不猜相对路径（相对路径的基准是 CWD，属于调用方的语境）。
inline std::wstring FromUserText(std::wstring s) {
    auto isPad = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\u3000';
    };
    size_t b = 0;
    while (b < s.size() && isPad(s[b])) ++b;
    size_t e = s.size();
    while (e > b && isPad(s[e - 1])) --e;
    if (b > 0 || e < s.size()) s = s.substr(b, e - b);

    // 剥一层配对引号。只剥一层：`""C:\x""` 这类多半是用户抄错了，剥完再失败比
    // 继续剥更诚实（继续剥会把真正的怪路径改成"看起来能走"的样子）。
    if (s.size() >= 2 && (s.front() == L'"' || s.front() == L'\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);

    return Normalize(s);
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

// 路径 → 由浅到深的**完整前缀**列表（面包屑分段用）。最后一个元素就是路径本身。
//
// 【为什么必须在这里】以前这段只存在于 NavModel::Rebuild，且只认 `X:\` 一种形状：
//   · UNC 路径 `\\server\share\dir` 落进 else 分支，整个路径变成**单段** ——
//     面包屑只剩一格，用户没法点回 `\\server\share`；
//   · 命名空间子项 `::{GUID}\子项` 同样塌成一段。
// 而"上一级"（ParentOf）早就支持 UNC 了，于是同一个路径"按钮能上、面包屑点不了"。
//
// 规则：
//   · 命名空间（`::{GUID}` / `shell:`）→ 单段，原样（没有可靠的分段语义）
//   · 盘符 `C:\A\B` → [C:\, C:\A, C:\A\B]（首段是盘根，不带尾段名）
//   · UNC `\\srv\share\A\B` → [\\srv\share, \\srv\share\A, ...]（首段是共享根）
//   · 其他（相对路径等）→ 单段，原样 —— 不猜基准目录
// 每段都是**完整前缀**，不是裸段名：分段点击直接拿去导航，不需要再拼。
inline std::vector<std::wstring> Prefixes(const std::wstring& path) {
    std::vector<std::wstring> out;
    if (path.empty()) return out;
    if (IsNamespace(path)) {
        out.push_back(path);
        return out;
    }
    const std::wstring s = Normalize(path);

    size_t start = 0;   // 第一个可遍历的段起点
    std::wstring root;  // 首段（盘根或 UNC 共享根）
    if (s.size() >= 3 && s[1] == L':' && s[2] == L'\\') {
        root = s.substr(0, 3);   // "C:\"
        start = 3;
    } else if (s.rfind(L"\\\\", 0) == 0) {
        // UNC：\\server\share 是可达的最小根（\\server 本身不是有效目标）
        size_t first = s.find(L'\\', 2);
        size_t second = (first == std::wstring::npos) ? std::wstring::npos : s.find(L'\\', first + 1);
        if (second == std::wstring::npos) {
            // \\server\share 自身（或残缺的 \\server）：整串就是唯一一段
            out.push_back(s);
            return out;
        }
        root = s.substr(0, second);
        start = second + 1;
    } else {
        out.push_back(s);
        return out;
    }

    out.push_back(root);
    size_t i = start;
    while (i < s.size()) {
        size_t j = s.find(L'\\', i);
        size_t end = (j == std::wstring::npos) ? s.size() : j;
        if (end > i) out.push_back(s.substr(0, end));   // 跳过 `\\` 造成的空段
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    return out;
}

// 分段标签：分段点击用的显示文本。
// 单独一个函数是因为"取哪一段文本"和"切哪几段路径"是两个问题：
// 盘根要显示 "C:"（不是 "C:\"），命名空间要原样，其余才是 Leaf。
//
// 【回归】盘根判定必须用 IsDriveRoot，不能只看 s[1]==':'：
// 命名空间路径 "::{GUID}" 的第二个字符也是 ':'，旧写法会把它当盘根，
// substr(0,2) 只剩 "::" —— 导航到「此电脑」「回收站」时地址栏面包屑
// 就显示成 ">::>"（左折叠箭头 + "::" + 分段箭头），既看不到名字也点不动。
inline std::wstring CrumbLabel(const std::wstring& prefixPath, size_t index) {
    if (index == 0 && IsDriveRoot(prefixPath))
        return prefixPath.substr(0, 2);   // "C:\" -> "C:"
    return Leaf(prefixPath);
}

// 地址栏补全用：把"用户正在敲的文本"切成 [要枚举的目录] + [已敲的名字前缀]。
//
// 【为什么不能用 ParentOf + Leaf】那样切在两种输入上直接失效：
//   · 盘根 `D:\` —— ParentOf 返回空（盘根没有上级），补全逻辑于是回退到
//     "当前目录 + 整串当前缀"，一个候选都匹配不上，Tab 变成死键；
//   · UNC 半截 `\\server\sh` —— ParentOf 认不出共享根，同样返回空。
// 这两处正是用户最需要 Tab 的地方（敲网络路径时没人愿意手打完整共享名）。
//
// 规则：在**最后一个反斜杠之后**切。没有反斜杠 = 整个文本都是前缀，目录部分为空
// （调用方按"当前目录"处理）。命名空间不参与补全（返回 empty dir，调用方据此跳过）。
struct CompleteSplit {
    std::wstring dir;     // 可枚举的目录部分（带尾反斜杠）；空 = 无目录部分
    std::wstring prefix;  // 已敲的名字前缀
    bool completable = true;
};

inline CompleteSplit SplitForComplete(const std::wstring& text) {
    CompleteSplit r;
    if (text.empty() || IsNamespace(text)) {
        r.completable = false;
        return r;
    }
    const std::wstring s = Normalize(text);
    const size_t cut = s.find_last_of(L'\\');
    if (cut == std::wstring::npos) {
        r.prefix = s;   // 只敲了名字：去当前目录里找
        return r;
    }
    r.dir = s.substr(0, cut + 1);
    r.prefix = s.substr(cut + 1);
    return r;
}

}  // namespace nav
