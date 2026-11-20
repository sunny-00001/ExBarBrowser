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
#pragma once
#include "common.h"

namespace ico {

// path 可以是：文件系统路径 / "::{GUID}[\子项]" / 友好显示名。
// small=true 取 16px 槽，false 取 32px 槽。
HICON Resolve(const std::wstring& path, bool small);

}  // namespace ico
