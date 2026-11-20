// 视图统计：状态栏 8 段与"文件操作"共用的一份取数。
//
// 全部来自 shell 给的接口（IFolderView2 / IShellItem2），不自己递归扫盘：
//   · 计数：ItemCount + 逐项 SFGAO_FOLDER
//   · 选中：GetSelection(IShellItemArray) + PKEY_Size / PKEY_DateModified
//   · 容器：GetFolder(IShellItem2) 的 System.Size；盘符再取 GetDiskFreeSpaceEx
#pragma once
#include <string>

#include "common.h"

struct ViewStats {
    unsigned total = 0;
    unsigned folders = 0;
    unsigned files = 0;
    bool splitKnown = false;  // 分类是否完成（超上限时退化为只给 total）

    unsigned selCount = 0;
    bool selTruncated = false;  // 选中项太多，容量/日期只统计了前 500 项
    unsigned selFolders = 0;
    unsigned selFiles = 0;
    bool hasSelection = false;

    unsigned long long selBytes = 0;
    bool selBytesKnown = false;

    unsigned long long containerBytes = 0;
    bool containerBytesKnown = false;

    unsigned long long diskFree = 0;
    unsigned long long diskTotal = 0;
    bool hasDisk = false;

    bool hasModified = false;
    SYSTEMTIME modified{};
    bool modifiedFromSelection = false;

    int dateSort = 0;  // 0 未按日期排序；1 升序；-1 降序

    bool virtualRoot = false;
    std::wstring containerPath;
};

// 文件夹/文件分类的缓存：800ms 定时器每次都逐项 QueryInterface 会把 UI 线程压满
// （8000 项 ≈ 8000 次 COM 创建）。total 没变就复用上次结果，只在目录变化时重算。
struct SplitCache {
    unsigned total = 0;
    unsigned folders = 0;
    unsigned files = 0;
    bool valid = false;
};

// 单次取数；耗时与项目数成正比。传 cache 时分类结果按 total 复用（调用方持有）。
bool CollectViewStats(IShellView* view, ViewStats* out, SplitCache* cache = nullptr);

// 选中项的文件系统路径（最多 max 个）
std::vector<std::wstring> SelectedPaths(IShellView* view, size_t max = 64);

// 当前文件夹的文件系统路径（虚拟根返回空）
std::wstring ViewFolderPath(IShellView* view);

std::wstring FormatSize(unsigned long long bytes);
std::wstring FormatDate(const SYSTEMTIME& st);
