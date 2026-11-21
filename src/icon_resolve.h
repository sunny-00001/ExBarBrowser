// 统一图标识别（对齐 Exbar.cpp 的 GetPathIcon，唯一入口）
//
// 此前地址栏/书签栏/标签栏各自裸调 SHGetFileInfoW(path)，对特殊命名空间
// （此电脑、回收站、侧边栏、云存储/NSE 扩展）拿不到准确图标，只会退成通用
// 文件夹图标。本模块把 exbar.cpp 那套分层解析搬过来，所有 UI 图标都走这里：
//   1. 友好显示名（"此电脑"、"网络"…）→ 桌面命名空间映射回 "::{GUID}" 解析串
//   2. 虚拟命名空间 "::{GUID}[\子项]" → SHParseDisplayName 转 PIDL 取真实图标
//      （子项直接给系统文件夹图标，避开串解析不稳）
//   3. 真实文件系统路径 → 先 PIDL 解析（让云盘等 NSE 扩展返回真实图标），
//      再退 SHGetFileInfoW
//   4. 兜底：CLSID 注册表 DefaultIcon → 系统 stock 文件夹图标
//
// 返回的 HICON 归调用方 DestroyIcon（缓存由调用方按路径管理）。
//
// 菜单/弹层不要各自 Resolve + Destroy：那会让同一路径在不同菜单里反复创建句柄，
// 而且一漏 Destroy 就是常驻 GDI 泄漏。菜单栏统一走 Cached —— 进程内按
// (路径, 尺寸) 共享，句柄由本模块持有到进程结束，调用方只读借用。

#pragma once
#include "common.h"

namespace ico {

// path 可以是：文件系统路径 / "::{GUID}[\子项]" / 友好显示名。
// small=true 取 16px 槽，false 取 32px 槽。
HICON Resolve(const std::wstring& path, bool small);
HICON Cached(const std::wstring& path, bool small);
void ClearCache();

// 友好显示名 → shell 解析串（"此电脑" → "::{20D04FE0-...}"）。
//
// 【为什么要暴露它，而不是留在 Resolve 内部】地址栏显示的是**友好名**
// （命名空间没有文件系统路径，只能显示 display），用户按回车时交回来的也是友好名。
// 把"此电脑"直接丢给 SHCreateItemFromParsingName 必失败 —— 症状是
// "在地址栏里回车没反应"，而屏幕上的文字看起来完全正常。
//
// 对已经是路径/解析串的输入原样返回（幂等）；认不出来的名字也原样返回，
// 让调用方的错误路径照常生效（不要在这里吞掉失败原因）。
std::wstring ParsingPath(const std::wstring& pathOrName);

// 路径 / 解析串 → 面向用户的显示名（全项目唯一实现）。
//
// 【为什么必须收口】"::{GUID}" 是 shell 的解析串，不是给人看的文字。历史栈
// （nav::History）与最近访问（Config::recentPaths）只存解析串，没有 display 字段；
// 导航栏下拉、书签兜底等显示点过去各自把 path 直接当 label，于是 "此电脑"
// 的历史项显示成 "::{20D04FE0-...}"。给每处加 if(IsNamespace) 特判正是"打补丁" ——
// 显示名的推导只在这里做一次，所有显示点复用。
//
// 数据源按优先级：
//   1. RememberDisplay 登记的导航期权威名（来自 shell 回调，含第三方 NSE）
//   2. 桌面枚举已经缓存的 display → parsing 反查（第三方 NSE）
//   3. HKCR\CLSID\{GUID}\LocalizedString → SHLoadIndirectString（系统本地化名，
//      不依赖"本次是否访问过"，跨会话恢复的历史也能显示正确名字）
//   4. HKCR\CLSID\{GUID}\ 默认值（英文名兜底）
// 文件系统路径原样返回（历史下拉显示完整路径，与资源管理器一致）。
//
// 命名空间带子项（::{GUID}\...）时保留子项后缀，只替换根的名字。
std::wstring DisplayName(const std::wstring& pathOrParsing);

// 带 Shell 回调值的版本：display 非空且不是解析串时优先（它是导航期权威名，
// 第三方 NSE 的名字只能从这里拿到）；否则回落到上面的统一推导。
std::wstring DisplayName(const std::wstring& pathOrParsing, const std::wstring& display);

// 登记导航期拿到的权威显示名（App::OnTabNav 调用）。仅命名空间有意义；
// 普通路径的显示名是 shell 给的 leaf 名，不进这张表。
void RememberDisplay(const std::wstring& path, const std::wstring& display);

}  // namespace ico
