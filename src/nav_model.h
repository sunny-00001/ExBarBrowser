// 导航模型：路径 → 面包屑分段 / 父级 / 直接子文件夹枚举 / 地址文本
//
// 这部分逻辑对应 ExBar mod 的 ebnav::CrumbPartPath / CrumbPartLabel /
// EnumChildFoldersAt（见 src/parts/003-olns.cpp），去掉与 IShellBrowser、DUI、
// Everything 搜索的耦合，只保留可复用的纯导航语义。
#pragma once
#include "common.h"

struct CrumbPart {
    std::wstring label;
    std::wstring path;
    bool ghost = false;  // 向上导航后留下的"被放弃的更深分支"（Directory Opus 风格）
};

class NavModel {
public:
    // path 用解析名（文件系统 C:\... 或命名空间 ::{GUID}）；display 是友好名
    void SetPath(const std::wstring& path, const std::wstring& display);
    void Clear();

    const std::wstring& path() const { return path_; }
    const std::wstring& display() const { return display_; }
    const std::vector<CrumbPart>& parts() const { return parts_; }
    bool IsRoot() const;

    // 地址框显示文本：命名空间用友好名，文件系统用真实路径
    std::wstring AddressText() const;

    static std::wstring ParentOf(const std::wstring& path);
    static std::wstring LeafOfPath(const std::wstring& path);
    // 列出 folder 的直接子文件夹（名称, 解析路径），按名称自然排序
    static bool ChildFolders(const std::wstring& folder,
                             std::vector<std::pair<std::wstring, std::wstring>>& out);

private:
    void Rebuild();

    std::wstring path_;
    std::wstring display_;
    std::wstring ghostPath_;
    std::vector<CrumbPart> parts_;
};
