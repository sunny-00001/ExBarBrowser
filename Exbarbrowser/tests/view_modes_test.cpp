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

    // D2：从 shell 现读的 (FVM, iconPx) 反查档位 —— FVM_THUMBNAIL 一档三用，
    // 必须靠像素区分；DPI 缩放后用"最接近"匹配。
    Check(viewmode::ModeIndexFor(FVM_DETAILS, 16) == 5, "详细信息档反查");
    Check(viewmode::ModeIndexFor(FVM_THUMBNAIL, 256) == 0, "超大图标档反查");
    Check(viewmode::ModeIndexFor(FVM_THUMBNAIL, 96) == 1, "大图标档反查");
    Check(viewmode::ModeIndexFor(FVM_THUMBNAIL, 48) == 2, "中图标档反查");
    Check(viewmode::ModeIndexFor(FVM_THUMBNAIL, 50) == 2, "DPI 缩放取最接近档");
    Check(viewmode::ModeIndexFor(FVM_LIST, 16) == 4, "列表档反查");
    Check(viewmode::ModeIndexFor(FVM_CONTENT, 48) == 7, "内容档反查");
    Check(viewmode::ModeIndexFor(9999, 48) == -1, "未知 FVM 返回 -1");

    std::printf(g_fail ? "有失败（失败 %d 项）\n" : "全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
