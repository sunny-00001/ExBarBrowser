// 单行 EDIT 垂直度量的纯测试。
//
// 实测（EM_POSFROMCHAR）：原生单行 EDIT 的文字顶部恒为 0，不随窗口高度居中；
// 窗口越高，文字越贴顶。所以 EDIT 窗口高度必须≈文字实际高度且偶数化，才能既
// 让自绘 DT_VCENTER 文字与 EDIT 文字落在同一像素，又能在偶数行高里上下对称。
// 这里把这两条钉成断言，防止再退回"拉高 EDIT 去凑留白"那种越修越歪的做法。
#include <cstdio>

#include "ui_draw.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("单行 EDIT 垂直度量\n");

    const int lineH = 18;
    const int pad = ui::kEditVPad96;
    Check(pad == 1, "上下留白令牌 = 1px");

    const RECT box{0, 0, 100, lineH + pad * 2};
    const RECT line = ui::CenteredLineRect(box, lineH);
    const int topGap = line.top - box.top;
    const int bottomGap = box.bottom - line.bottom;
    Check(topGap == pad && bottomGap == pad, "文字行上下留白相等且各 1px");
    Check(line.bottom - line.top == lineH, "文字行高 = 字体行高");

    // 框高与行高差奇数像素时，不能让下方多出一整行：两端最多差 1px。
    const RECT odd{0, 0, 100, lineH + 2 * pad + 1};
    const RECT oddLine = ui::CenteredLineRect(odd, lineH);
    Check((oddLine.top - odd.top) - (odd.bottom - oddLine.bottom) <= 1,
          "奇数剩余像素只影响 1px，不会让上下明显不等");

    // 行高超过框高时退化为框本身，不能算出负留白或负高度。
    const RECT tiny{0, 0, 100, 8};
    const RECT clamped = ui::CenteredLineRect(tiny, lineH);
    Check(clamped.top == tiny.top && clamped.bottom == tiny.bottom,
          "行高超过框高时退化为框本身");

    Check(ui::EditLineHeight(nullptr, nullptr, 18) == 18, "无 DC 时用调用方兜底值（18 已是偶数）");
    Check(ui::EditLineHeight(nullptr, nullptr, 17) == 18, "奇数兜底值也偶数化，保证上下对称");
    if (HDC dc = GetDC(nullptr)) {
        const int h = ui::EditLineHeight(dc, nullptr, 18);
        ReleaseDC(nullptr, dc);
        Check(h > 0 && (h % 2) == 0, "有 DC 时量出正且为偶数的行高");
        // 偶数行高在偶数行里居中：EDIT 文字贴顶 y=0，自绘 DT_VCENTER 也落在
        // top=(h-字高)/2=0，两者基线一致——这就是编辑态/非编辑态不跳字的判据。
        const RECT row{0, 0, 100, 28};
        const RECT line = ui::CenteredLineRect(row, h);
        Check(line.top - row.top == row.bottom - line.bottom,
              "偶数行高在 28px 行里上下留白相等");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
