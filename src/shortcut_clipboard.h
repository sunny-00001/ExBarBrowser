// path.copy.link 的落点：把路径做成 .lnk **虚拟文件**放进剪贴板。
//
// 为什么不落盘：path.copy* 全家族只动剪贴板，不改用户目录。写 .lnk 到磁盘是
// 另一条命令（Opus 的 "send to" 语义），会覆盖同名文件，不该挂在 copy 名下。
// 虚拟文件用 CFSTR_FILEDESCRIPTOR + CFSTR_FILECONTENTS 表达，粘贴到资源管理器
// 得到的是真正的快捷方式，而不是一行文本路径。
#pragma once
#include <string>
#include <vector>

namespace shortcut_clipboard {

// paths 为文件系统路径（虚拟命名空间项会被跳过并记日志）。
// outNames 回报本次写入剪贴板的 .lnk 文件名，顺序与 paths 一致。
// 返回 false 表示一条都没成功（不改动剪贴板）。
bool CopyAsShortcut(const std::vector<std::wstring>& paths,
                    std::vector<std::wstring>* outNames);

}  // namespace shortcut_clipboard
