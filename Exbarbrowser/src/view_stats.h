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

    // 选中项的"类型"（扩展名）：给状态栏的类型段用。
    //   selExtKinds == 0 → 没有可判定的类型（无选中 / 全是文件夹）
    //   selExtKinds == 1 → selExt 是那一种类型（小写，含点，如 ".txt"）
    //   selExtKinds >= 2 → 跨多种类型，selExt 为空
    std::wstring selExt;
    int selExtKinds = 0;

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
// （8000 项 ≈ 8000 次 COM 创建）。目录没变就复用上次结果，只在目录变化时重算。
//
// 【键必须是路径，不能是 total —— 这是个真 bug】
// 旧实现只比 total："两个不同目录恰好项数相同"（例如 C:\A 与 D:\B 都是 100 项）
// 就会命中对方的缓存，状态栏显示**另一个目录的文件夹/文件数**。用户看到的
// 是"数不对"，但刷新一下又对（他导航到别处再回来），所以极难复现、极难归因。
// 探针 82 还实测了分类本身的价格：System32（5003 项）85.7 ms，而只数总数 8.2 ms
// —— 差 10 倍，所以"目录一变就重算"绝对不能发生在 UI 线程的同步路径上。
struct SplitCache {
    std::wstring path;   // 归属目录（缓存键）
    unsigned total = 0;
    unsigned folders = 0;
    unsigned files = 0;
    bool valid = false;
};

// 单次取数；耗时与项目数成正比。传 cache 时分类结果按 total 复用（调用方持有）。
// ownerPath = 这份 stats 描述的是哪个目录（缓存键）。传空 = 不启用分类缓存
// （每次强制重算）。调用方从 Pane::path() 取即可，两边口径一致。
//
// allowSplit = 是否允许"逐项分类"这一贵操作（探针 82：System32 5003 项要 85.7 ms，
// 而只数总数只要 8.2 ms）。false 时只填 total / 选中集 / 磁盘，
// folders/files 留 0 且 splitKnown=false —— 状态栏据此显示"N 个对象"而不是
// 编一个错的分类型。导航那一帧传 false，稳定后再传真。
//
// lightOnly = 只取状态栏"有没有变化"的判据：total + 选中数。800ms 定时器每拍都
// 先走这条轻量路径，只有判据变化（或调用方标脏）才做下面的完整取数。
// 这样空闲帧不再扫描 500 项属性、容器属性、排序列与磁盘。
bool CollectViewStats(IShellView* view, ViewStats* out, SplitCache* cache = nullptr,
                      const std::wstring& ownerPath = {}, bool allowSplit = true,
                      bool lightOnly = false);

// "多大以上就不值得在 UI 线程做逐项分类"。超过它 StateBar 只显示总数。
// 8000 是原来 kSplitCap 的上限（再大一次枚举要上百毫秒）；
// 3000 是节流阈值：超过就只在路径稳定且空闲时才分类。
constexpr unsigned kStatsSplitCap = 8000;

// 选中项的文件系统路径（最多 max 个）
std::vector<std::wstring> SelectedPaths(IShellView* view, size_t max = 64);

// 当前文件夹的文件系统路径（虚拟根返回空）
std::wstring ViewFolderPath(IShellView* view);

std::wstring FormatSize(unsigned long long bytes);
std::wstring FormatDate(const SYSTEMTIME& st);
