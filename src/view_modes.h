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

}  // namespace viewmode
