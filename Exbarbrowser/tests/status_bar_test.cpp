// 状态栏的纯函数自测：不碰窗口，只验证"图标口径"与"宽度口径"这两条不变量。
//
// 【为什么要有这个】状态栏的图标来源有三档（系统位图 → 系统位图 → 字形），
// 段与段的映射分散在两个函数里；任何一次"给某段换图标"的改动，
// 都有可能把某一档漏掉（该段没系统位图、字形又是 None → 空图标槽）。
// 这里把不变量钉死：每段要么有系统位图、要么有字形兜底，绝不两者皆空。
#include <cstdio>

#include "common.h"
#include "icon_glyph.h"
#include "status_bar.h"
#include "ui_draw.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

int main() {
    // ---- 1. 每段都必须有图标（系统位图或字形兜底），不能是空图标槽 ----
    for (int seg = 0; seg < Seg_Count; ++seg) {
        const ico::IconId glyph = StatusSegmentIcon(seg);
        const sysicon::Id sys = StatusSegmentSysId(seg);
        // 路径段是**例外**：它画的是"当前目录的真实系统图标"（由模型带进来），
        // 不在固定映射里 —— 所以它两档都该是 None，其余段则不许空。
        if (seg == Seg_Path) {
            Check(glyph == ico::IconId::None, "路径段没有固定字形（用当前目录的真实图标）");
            Check(sys == sysicon::Id::None, "路径段没有固定位图（同上）");
            continue;
        }
        Check(glyph != ico::IconId::None, "每段都有字形兜底");
        Check(ico::Glyph(glyph) != 0, "字形兜底在 ico 表里能查到码位");
        // 系统位图那一档是"可选增强"：有就用，没有就退字形 —— 两者都不许空。
        Check(sys != sysicon::Id::None || glyph != ico::IconId::None,
              "系统位图与字形至少有一档可用");
    }

    // ---- 2. 段位图的语义不能撞车 ----
    // 文件夹计数与选中容量都用"文件夹"的话，用户分不出"几个文件夹"和"占多大"。
    Check(StatusSegmentSysId(Seg_Fld) != StatusSegmentSysId(Seg_Size),
          "文件夹计数与容量段不用同一枚位图");
    Check(StatusSegmentSysId(Seg_Disk) != StatusSegmentSysId(Seg_Size),
          "容量段与磁盘段不用同一枚位图");

    // ---- 3. 已知段位的具体口径（换图标时这几行会红） ----
    Check(StatusSegmentIcon(Seg_Date) == ico::IconId::Calendar, "日期段 = 日历字形");
    Check(StatusSegmentIcon(Seg_Eye) == ico::IconId::Eye, "隐藏三态段 = 眼睛字形");
    Check(StatusSegmentIcon(Seg_Type) == ico::IconId::Tag, "类型段 = 标签字形");
    Check(StatusSegmentIcon(Seg_Ai) == ico::IconId::Channel, "AI chip = 通道信号波字形");
    Check(StatusSegmentSysId(Seg_Fld) == sysicon::Id::Folder, "文件夹计数 = 文件夹位图");
    Check(StatusSegmentSysId(Seg_File) == sysicon::Id::Document, "文件计数 = 文档位图");
    Check(StatusSegmentSysId(Seg_Chk) == sysicon::Id::Check, "已选段 = 对勾位图");
    Check(StatusSegmentSysId(Seg_Disk) == sysicon::Id::Disk, "磁盘段 = 硬盘位图");
    // 没有系统位图的段必须显式说 None —— 否则 sys_icon 会去查一张不存在的位图。
    Check(StatusSegmentSysId(Seg_Date) == sysicon::Id::None, "日期段无系统位图（退字形）");
    Check(StatusSegmentSysId(Seg_Eye) == sysicon::Id::None, "隐藏三态段无系统位图（退字形）");
    Check(StatusSegmentSysId(Seg_Type) == sysicon::Id::None, "类型段无系统位图（退字形）");
    Check(StatusSegmentSysId(Seg_Ai) == sysicon::Id::None, "AI chip 无系统位图（退字形）");

    // ---- 4. 宽度口径：位数相同则宽度相同（数值变化不抖） ----
    // 内存 DC + 自建字体：不碰屏幕、不抢焦点。
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-14, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 4, 0, L"Segoe UI");
    Check(dc != nullptr && font != nullptr, "内存 DC 与字体建得出来");
    if (dc && font) {
        HGDIOBJ old = SelectObject(dc, font);
        const int w1 = ui::MeasureStableText(dc, font, L"文件夹 0/83");
        const int w2 = ui::MeasureStableText(dc, font, L"文件夹 9/83");
        const int w3 = ui::MeasureStableText(dc, font, L"文件夹 0/832");
        Check(w1 > 0, "稳定宽量得出结果");
        Check(w1 == w2, "同位数不同数字：宽度不变（不抖）");
        Check(w3 > w1, "多一位：宽度变大（位数口径生效）");
        // 普通量法会随数字变（比例字体下尤其明显），两者不必相等，
        // 但稳定宽不能小于普通量法的最小值 —— 否则段宽会切字。
        Check(w1 >= ui::MeasureText(dc, font, L"文件夹 0/83"), "稳定宽不小于实际量宽");
        SelectObject(dc, old);
    }
    if (font) DeleteObject(font);
    if (dc) DeleteDC(dc);

    std::printf(g_fail ? "有失败（失败 %d 项）\n" : "全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
