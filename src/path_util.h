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
#include <vector>

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

// 把**命名空间解析名**归一化成 shell 能接受的形式：裸 `::{GUID}` 补 `shell:` 前缀。
//
// 【为什么单独一个函数、而不是并进 ToShellPath】
// 两者作用域不同：
//   · ToShellPath 管**文件系统**路径的分隔符（正斜杠、连续分隔符、尾分隔符）；
//   · 本函数管**命名空间**解析串的前缀。文件系统路径**绝不能**被套上 `shell:`
//     （`shell:C:\Windows` 全线失败），命名空间字符串也不需要动分隔符。
// 混成一个函数，将来任何一个方向的新规则都会顺手改坏另一个方向。
//
// 【为什么必须有】SIGDN_DESKTOPABSOLUTEPARSING 对命名空间项返回的是**裸**
// `::{GUID}`，而裸形式对"主页"（679F85CB-…，Win11 的快速访问/主页）返回
// E_INVALIDARG(0x80070057)。实测（探针 49）：`shell:::{GUID}` 五种命名空间
// 全部可解析，裸形式只有主页失败。不按 GUID 特判 —— 那样每加一个命名空间
// 就要再赌一次哪种写法能用。
//
// 【为什么它必须在 pathutil 而不是 nav_pane_root】
// 原实现在 navpane 命名空间里，只有导航窗格那一侧用。但**同一个 bug 存在于
// 所有"外部字符串 -> shell"的入口**：会话恢复把 sessionPane2Paths 里的
// 裸 `::{679F85CB-…}` 直接喂给 ShellView::DoNavigate，于是右窗格
// `[nav] parse hr=0x80070057` 一次都没导航成功 —— 用户看到的是"右栏空白"。
// 收口到 pathutil 之后，导航、树、ItemFromPath 三处共用同一份判据。
//
// 幂等：已带 `shell:` 前缀的原样返回（否则会变成 `shell::shell:::{…}`）。
std::wstring ToParsingName(const std::wstring& raw);

// 一组路径的 ToShellPath（逐项归一化，顺序与大小写都不变）。
//
// 为什么需要批量版本：fs.* 那组命令的 targets 参数本来就是数组，
// 批量版让"整批一起归一化"成为**一次**调用 —— 逐项在调用点写循环，
// 迟早有人漏掉其中一项（漏掉的那一项就是那个报 E_INVALIDARG 的）。
std::vector<std::wstring> ToShellPaths(const std::vector<std::wstring>& paths);

// child 是否是 parent 的**真子路径**（parent 本身不算）。纯函数，大小写不敏感。
//
// 判据的坑在分隔符：`C:\A` 是 `C:\AB` 的前缀，但不是它的父目录。
// 所以比较完前缀还必须要求下一字符是分隔符（或 parent 自己以分隔符结尾）。
// 尾部反斜杠先统一去掉再比 —— `C:\A\` 与 `C:\A` 是同一个目录。
bool IsUnder(const std::wstring& child, const std::wstring& parent);

}  // namespace pathutil

#endif  // PATH_UTIL_H_
