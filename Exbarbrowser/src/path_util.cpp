#include "path_util.h"

#include <vector>

namespace pathutil {

std::wstring CollapseSeparators(const std::wstring& path) {
    if (path.empty()) return path;

    // UNC 前缀判定：开头两个都是分隔符。
    // 注意 `\foo`（单反斜杠开头）是"当前盘根下的相对路径"，不是 UNC。
    const bool unc = path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';

    std::wstring out;
    out.reserve(path.size());

    if (unc) {
        // UNC 前缀原样保留（两个反斜杠），从第三个字符开始折叠。
        out.push_back(L'\\');
        out.push_back(L'\\');
        for (size_t i = 2; i < path.size(); ++i) {
            if (path[i] == L'\\' && !out.empty() && out.back() == L'\\') continue;
            out.push_back(path[i]);
        }
        return out;
    }

    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out.push_back(path[i]);
    }
    return out;
}

std::wstring ToShellPath(const std::wstring& path) {
    if (path.empty()) return path;

    // 命名空间路径原样返回：`::{GUID}` 是 PIDL 表达式，
    // `shell:xxx` 是协议串（后面可以带正斜杠），两者都不是文件系统路径。
    if (path.rfind(L"::", 0) == 0 || path.rfind(L"shell:", 0) == 0) return path;

    std::wstring p = path;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    // 去尾分隔符：盘根 `C:\` 必须留（去掉就变成 `C:`，语义从"根"变成"当前目录"）。
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return CollapseSeparators(p);
}

// 裸 ::{GUID} -> shell:::{GUID}。理由见头文件。
//
// 只补这一种形态：`::` 开头但**不是** `shell:` 前缀的解析名。
// `shell:` 开头说明调用方已经归一化过（例如 nav_pane::PathOfItem 的出口），
// 再补一次会变成 `shell::shell:::{…}` —— 那种串一样解析失败，而且更难看出原因。
std::wstring ToParsingName(const std::wstring& raw) {
    if (raw.empty()) return raw;
    if (raw.rfind(L"shell:", 0) == 0) return raw;   // 已归一化，幂等
    if (raw.rfind(L"::", 0) == 0) return L"shell:" + raw;
    return raw;
}

std::vector<std::wstring> ToShellPaths(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> out;
    out.reserve(paths.size());
    for (const auto& p : paths) out.push_back(ToShellPath(p));
    return out;
}

}  // namespace pathutil