// 弹层行高规则自测：分隔线只占 2px，普通条目占整行高，总高 = 各行之��。
//
// 这条断言来自一次真实返工：原来 ListBox 用 LBS_OWNERDRAWFIXED，所有行强制同一高度，
// 分隔线也占满 24px（120 DPI 下 30px），一个分组界线吃掉一整行，视觉上像多了个空条目。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -Isrc tests/popup_layout_test.cpp -o /tmp/popup_layout_test.exe \
//     -static-libstdc++ -static-libgcc -lole32 -luuid -lshell32 -ladvapi32
// 首次编译要带 PCH 或让 common.h 自行引入 windows.h（见其他测试的说明）。
#include <cstdio>
#include <vector>

#include "popup_list.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

PopupRow Sep() {
    PopupRow r;
    r.separator = true;
    return r;
}

PopupRow Item(const wchar_t* label) {
    PopupRow r;
    r.label = label;
    return r;
}

}  // namespace

int main() {
    const int itemH = 24;   // 96 DPI 下的条目高
    const int sepPx = 2;    // 分隔线行高

    // 1. 单行：分隔线 vs 普通条目
    std::printf("单行高度\n");
    Check(PopupRowHeight(Sep(), itemH, sepPx) == sepPx, "分隔线 = 2px（不是整行高）");
    Check(PopupRowHeight(Item(L"a"), itemH, sepPx) == itemH, "普通条目 = itemH");

    // 2. 典型下拉：3 条路径 + 分隔线 + 1 条动作（地址框 ⌄ 的形状）
    std::printf("\n典型地址框下拉（3 路径 + 分隔 + 1 动作）\n");
    std::vector<PopupRow> rows{Item(L"C:\\a"), Item(L"C:\\b"), Item(L"C:\\c"),
                               Sep(), Item(L"清除历史记录")};
    const int h = PopupTotalHeight(rows, static_cast<int>(rows.size()), itemH, sepPx);
    Check(h == 4 * itemH + sepPx, "总高 = 4*itemH + 2px");
    Check(h == 98, "总高数值 = 98");
    // 旧算法（每行都按 itemH）会得到 5*24=120，多出 22px
    Check(h < 5 * itemH, "比以前省下一整行（< 5*itemH）");

    // 3. 两个分隔线（back / fwd / recent 三段都有时）
    std::printf("\n三段式（2 个分隔线）\n");
    std::vector<PopupRow> rows2{Item(L"back1"), Sep(), Item(L"fwd1"), Sep(), Item(L"recent1")};
    Check(PopupTotalHeight(rows2, 5, itemH, sepPx) == 3 * itemH + 2 * sepPx,
          "总高 = 3*itemH + 2*sepPx");

    // 4. n 越界钳制：不能读越界，也不能多算
    std::printf("\n越界钳制\n");
    Check(PopupTotalHeight(rows, 999, itemH, sepPx) == h, "n > 行数时按行数算");
    Check(PopupTotalHeight(rows, 0, itemH, sepPx) == 0, "n = 0 时为 0");
    Check(PopupTotalHeight(rows, -1, itemH, sepPx) == 0, "n < 0 时为 0");

    // 5. 空列表
    std::printf("\n空列表\n");
    Check(PopupTotalHeight({}, 0, itemH, sepPx) == 0, "空列表总高 0");

    // 6. 高 DPI：sepPx 也应随 DPI 缩放，且不为 0
    std::printf("\n高 DPI\n");
    const int dpiItem = 30, dpiSep = 3;   // 120 DPI
    std::vector<PopupRow> rows3{Item(L"a"), Sep(), Item(L"b")};
    Check(PopupTotalHeight(rows3, 3, dpiItem, dpiSep) == 2 * dpiItem + dpiSep,
          "120 DPI 下总高 = 2*30 + 3");

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
