// 鼠标规则内置默认 / 侧键解析自测。
//
// 断言来自本轮真实修掉的断线：
//   · 侧键无默认规则 → 前进/后退在文件视图与四栏全部失效；
//   · 四栏不解析 WM_XBUTTON 的 wParam → 规则引擎拿不到按钮编号；
//   · 覆盖 target 必须含空白/标签/导航/状态/预览等所有可达区域，
//     否则出现"在文件上能用、在标签栏没反应"的割裂。
//
// 编译：由 tests/run_all.sh 复用主构建 obj 自动完成。
#include <cstdio>

#include "mouse_rules.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("鼠标默认规则 / 侧键解析\n");

    // 内置默认必须含侧键：X1=后退、X2=前进，覆盖全部可达 target。
    static const int kTargets[] = {MT_EMPTY, MT_UNKNOWN, MT_VIEW,   MT_TAB,
                                   MT_NAV,   MT_ADDRESS, MT_TREE,   MT_STATUS,
                                   MT_STATUS_SEG, MT_PREVIEW, MT_BOOKMARK,
                                   MT_BOOKMARK_FOLDER, MT_BOOKMARK_EMPTY};
    MouseRules& rules = MouseRules::Get();
    rules.LoadDefaults();  // 不吃用户配置，纯验内置表

    for (int t : kTargets) {
        const MouseRule* back = rules.Find(t, MBTN_X1, TR_SINGLE, 0);
        const MouseRule* fwd = rules.Find(t, MBTN_X2, TR_SINGLE, 0);
        Check(back && back->action == L"command" && back->command == L"NavBack",
              "X1 默认后退存在（target 覆盖）");
        Check(fwd && fwd->action == L"command" && fwd->command == L"NavForward",
              "X2 默认前进存在（target 覆盖）");
    }

    // 方向不能反：这是修复里最容易写错的一处。
    const MouseRule* emptyBack = rules.Find(MT_EMPTY, MBTN_X1, TR_SINGLE, 0);
    const MouseRule* emptyFwd = rules.Find(MT_EMPTY, MBTN_X2, TR_SINGLE, 0);
    Check(emptyBack && emptyBack->command == L"NavBack", "空白区 X1 = 后退（方向校验）");
    Check(emptyFwd && emptyFwd->command == L"NavForward", "空白区 X2 = 前进（方向校验）");

    // 原有三条内置默认（双击空白上一级 / 书签栏 / 状态栏）不能丢。
    Check(rules.Find(MT_EMPTY, MBTN_LEFT, TR_DOUBLE, 0) != nullptr, "双击空白 -> NavUp 仍在");
    Check(rules.Find(MT_BOOKMARK_EMPTY, MBTN_LEFT, TR_DOUBLE, 0) != nullptr,
          "双击书签栏空白 -> BmkToggleBar 仍在");
    Check(rules.Find(MT_STATUS, MBTN_LEFT, TR_DOUBLE, 0) != nullptr, "双击状态栏 -> PathCopyDir 仍在");

    // 侧键默认不应吞掉文件/文件夹项（留给用户自配）：这两类没有内置侧键规则。
    Check(rules.Find(MT_FILE, MBTN_X1, TR_SINGLE, 0) == nullptr, "文件项 X1 无内置默认（留给用户）");
    Check(rules.Find(MT_FOLDER, MBTN_X2, TR_SINGLE, 0) == nullptr,
          "文件夹项 X2 无内置默认（留给用户）");

    // wParam HIWORD 直解析（WM_XBUTTONDOWN/UP 携带 XBUTTON1/2）。
    Check(XButtonFromWParam(MAKELONG(0, XBUTTON1)) == MBTN_X1, "wParam XBUTTON1 -> MBTN_X1");
    Check(XButtonFromWParam(MAKELONG(0, XBUTTON2)) == MBTN_X2, "wParam XBUTTON2 -> MBTN_X2");
    Check(XButtonFromWParam(0) == -1, "无编号且无物理按下 -> -1（放行）");

    std::printf("\n结果：%s（失败 %d）\n", g_fail == 0 ? "全通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
