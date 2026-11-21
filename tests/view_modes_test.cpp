// 图标缩放的纯函数自测：不碰窗口，只验证步进与边界。
#include <cstdio>

#include "view_modes.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

int main() {
    Check(viewmode::ZoomIconPx(48, 1) == 64, "放大一步 48 -> 64");
    Check(viewmode::ZoomIconPx(48, -1) == 32, "缩小一步 48 -> 32");
    Check(viewmode::ZoomIconPx(256, 1) == viewmode::kMaxIconPx, "上限夹紧");
    Check(viewmode::ZoomIconPx(16, -1) == viewmode::kMinIconPx, "下限夹紧");
    Check(viewmode::ZoomIconPx(0, 1) == viewmode::kMinIconPx + viewmode::kZoomStepPx,
          "读到 0 时从最小档起步");
    Check(viewmode::ClampIconPx(1000) == 256 && viewmode::ClampIconPx(-5) == 16,
          "ClampIconPx 双向夹紧");

    std::printf(g_fail ? "有失败（失败 %d 项）\n" : "全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
