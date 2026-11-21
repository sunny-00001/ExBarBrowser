// 路径分隔符折叠 —— 纯函数，无状态，可单测。
//
// 【为什么需要】SHCreateItemFromParsingName 对**连续**的路径分隔符返回
// E_INVALIDARG(0x80070057)。实测（2026-11-21）：
//   C:\Windows      → hr=0x00000000  正常
//   C:/Windows      → hr=0x00000000  正常（正斜杠先被换成反斜杠）
//   C://Windows     → hr=0x80070057  失败（归一化成两个真反斜杠）
//   C:\\Windows     → hr=0x80070057  失败
// 而 `C://Windows` 不是用户乱打：从 URL 粘的路径、从配置拼的路径、
// 别的程序传过来的路径都可能是这个形状。所以解析前必须折叠。
//
// 【UNC 例外】`\\server\share` 的前导双反斜杠**不是冗余，是 UNC 语法**，
// 折成一个会让它变成 `\server\share`（变成"当前盘根下的 server 目录"）。
// 所以 UNC 前缀整体保留，只折叠其余部分。
//
// 【边界】本函数只管分隔符，不判断路径是否存在、不解析 `..`、不处理
// 盘符/命名空间（`::`、`shell:` 由调用方先行排除）。
#ifndef PATH_UTIL_H_
#define PATH_UTIL_H_

#include <string>

namespace pathutil {

// 折叠路径里连续的分隔符。
//   `C:\\Windows`      → `C:\Windows`      （折叠中间/结尾的连续）
//   `\\server\share`  → `\\server\share`  （UNC 前缀保留）
//   `\\\\server\share` → `\\server\share`  （UNC 前缀之后的冗余折叠）
//   `C:\`              → `C:\`             （盘根单反斜杠不动）
//   `\\`               → `\\`              （纯 UNC 前缀不动）
// 空串返回空串；不含分隔符的串原样返回。
std::wstring CollapseSeparators(const std::wstring& path);

// 归一化成**shell 能解析的路径**（全项目"外部路径 → shell API"的唯一收口）：
//   · 正斜杠 → 反斜杠 —— SHCreateItemFromParsingName / SHParseDisplayName
//     只认反斜杠，收到正斜杠直接 E_INVALIDARG(0x80070057)。
//   · 折叠连续分隔符 —— 见 CollapseSeparators 的注释。
//   · 去掉多余的尾分隔符（盘根 `C:\` 除外）。
//   `::` / `shell:` 前缀的命名空间路径原样返回：它们没有盘符语义，
//   而且 `shell:` 协议后面可以带正斜杠（`shell:Downloads`）。
//
// 【为什么必须有这一个函数】外部路径（AI/脚本/命令行/配置文件）里
// 正斜杠是**完全合法**的写法（Win32 的 CreateFile 也接受），
// 而 shell 的解析 API 不接受。以前这件事散在各处各抄一份：
//   · shell_view.cpp（导航）抄了一份 → 导航能吃正斜杠；
//   · fs.* 这一组**根本没抄** → fs.rename / fs.copy 收到正斜杠路径时
//     报"无法定位源文件（HRESULT=0x80070057）"，而真正的原因
//     （分隔符）完全看不出来。
// 同一类输入在不同命令上表现不同，本身就是一种 bug。
std::wstring ToShellPath(const std::wstring& path);

}  // namespace pathutil

#endif  // PATH_UTIL_H_
