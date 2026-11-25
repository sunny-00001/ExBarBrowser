// 视图档位表（唯一来源）。
//
// 【为什么必须只有一份】这张表的**下标**是跨模块契约：
//   · `view.mode` 命令的 schema 声明 arg = 0..7，对应本表下标；
//   · 导航栏"视图"菜单按本表渲染，点击回传下标；
//   · 配置持久化的 viewMode 也存这个下标。
//
// 以前 app_commands.cpp 与 nav_bar_actions.cpp 各写一份 8 行表（逐字相同）。
// 任何一档增删/换序而另一处没改，就会让"菜单里点的档位"与"落盘的下标"错位 ——
// 表现是"下次启动视图模式对不上"，且没有编译期或运行期报错。这类纯数据重复
// 是最难发现的一种：它不像代码那样有相似结构可供查重。
//
// 【纪律】新增/调整档位只改这里。索引位置一旦发布就不再变动（与 CmdId 同理）：
// 用户配置里存的是下标，换序等于把所有人的视图设置打乱。
#pragma once

#include <shobjidl.h>   // FVM_*（FOLDERVIEWMODE）；不靠 PCH 间接提供

#include "icon_glyph.h"

namespace viewmode {

struct ViewMode {
    int fvm;              // FOLDERVIEWMODE
    int icon;             // 图标像素（96 DPI 基准，调用方按 DPI 缩放）
    const wchar_t* label; // 菜单显示文案
    ico::IconId glyph;    // 视图菜单行图标（与本档位同表，不另写一份映射）
};

// 8 个档位，顺序即 `view.mode` 命令的 arg 取值（0..7）。
inline constexpr ViewMode kModes[] = {
    {FVM_THUMBNAIL, 256, L"超大图标", ico::IconId::GridView},
    {FVM_THUMBNAIL, 96, L"大图标", ico::IconId::GridView},
    {FVM_THUMBNAIL, 48, L"中图标", ico::IconId::GridView},
    {FVM_ICON, 16, L"小图标", ico::IconId::ListView},
    {FVM_LIST, 16, L"列表", ico::IconId::ListView},
    {FVM_DETAILS, 16, L"详细信息", ico::IconId::DetailsView},
    {FVM_TILE, 48, L"平铺", ico::IconId::GridView},
    {FVM_CONTENT, 48, L"内容", ico::IconId::Page},
};

inline constexpr int kCount = static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));

// 排序 / 分组维度的标签表（4 档，下标与 Config::sortMode / groupBy 一一对应）。
//
// 【为什么也放在这里】视图档位表原本只有一处，但排序/分组的 8 个中文名
// 此前住在 nav_bar_actions.cpp 的文件私有静态数组里 —— 于是设置对话框
// （要把它们显示成下拉项）只能再抄一份。两份表迟早不同步，而不同步的表现
// 是"下拉里写'按日期排序'、写进去的值却是'按大小'"——没有任何检查会报。
//
// 维度表与档位表放一起的理由相同：它们都是"下标即契约"的维度表。
inline constexpr int kSortGroupCount = 4;
inline constexpr const wchar_t* kSortLabels[kSortGroupCount] = {L"按名称排序", L"按日期排序",
                                                                 L"按类型排序", L"按大小排序"};
inline constexpr const wchar_t* kGroupLabels[kSortGroupCount] = {L"不分组", L"按类型分组",
                                                                  L"按日期分组", L"按大小分组"};

// Ctrl+滚轮的图标尺寸步进。两侧都夹在 16..256，避免 shell 收到 0 或超大值后
// 直接忽略整次设置（表现为“按了没反应”，最难查的一种失败）。
inline constexpr int kMinIconPx = 16;
inline constexpr int kMaxIconPx = 256;
inline constexpr int kZoomStepPx = 16;

inline int ClampIconPx(int px) {
    if (px < kMinIconPx) return kMinIconPx;
    if (px > kMaxIconPx) return kMaxIconPx;
    return px;
}

inline int ZoomIconPx(int px, int steps) {
    if (px <= 0) px = kMinIconPx;
    return ClampIconPx(px + steps * kZoomStepPx);
}

// 从 shell 现读的 (FVM, iconPx) 反查档位下标（D2：视图菜单打勾跟真实状态同步）。
// FVM_THUMBNAIL 一档被 3 个下标共用，必须按 iconPx 精确匹配；返回的图标像素
// 会被 DPI 缩放，所以用"最接近的档位"而不是严格相等。找不到返回 -1（未知）。
inline int ModeIndexFor(int fvm, int iconPx) {
    int best = -1;
    int bestDelta = -1;
    for (int i = 0; i < kCount; ++i) {
        if (kModes[i].fvm != fvm) continue;
        const int delta = kModes[i].icon > iconPx ? kModes[i].icon - iconPx
                                                  : iconPx - kModes[i].icon;
        if (best < 0 || delta < bestDelta) {
            best = i;
            bestDelta = delta;
        }
    }
    return best;
}

}  // namespace viewmode
