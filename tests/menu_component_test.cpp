// 统一菜单组件自测：验证行构建、勾选前缀、分隔行、追加文案、路径图标兜底
// 全部由 ui::Menu 负责，调用方不再手写 PopupRow 八字段。
//
// 编译（复用主构建的 obj，排除 main/resource/check/test）：
//   CLANG="/c/Program Files/Windhawk/Compiler/bin/clang++.exe"
//   "$CLANG" -std=c++20 -O1 -g -target x86_64-w64-mingw32 \
//     -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
//     -D_WIN32_IE=0x0A00 -Isrc -include-pch obj/common.h.gch \
//     -c tests/menu_component_test.cpp -o obj/menu_component_test.obj
//   "$CLANG" -target x86_64-w64-mingw32 -o obj/menu_component_test.exe \
//     obj/menu_component_test.obj $(ls obj/*.obj | grep -v -E \
//     '(main|app\.res|menu_component_test|.*\.check)\.obj$') <主构建链接库>
#include <cstdio>

#include "menu.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("统一菜单组件\n");

    ui::Menu menu;
    Check(menu.Empty(), "新菜单为空");

    menu.Add(L"新建", 1, ico::IconId::Add);
    menu.Separator();
    const int checkedRow = menu.AddChecked(L"重命名", true, 2, ico::IconId::Edit);
    const int plainRow = menu.AddChecked(L"关闭", false, 3, ico::IconId::Close, true);
    const int pathRow = menu.AddPathChecked(L"当前标签", true, L"", 4);

    Check(menu.Count() == 5, "Add / Separator / AddChecked / AddPathChecked 都计入行数");
    Check(menu.rows()[0].data == 1 && menu.rows()[0].glyph == ico::IconId::Add,
          "普通行动作与字形一致");
    Check(menu.rows()[1].separator && menu.rows()[1].data == -1,
          "分隔行统一无 data、不可选");
    Check(menu.rows()[static_cast<size_t>(checkedRow)].label == L"✓ 重命名",
          "勾选前缀由组件统一拼");
    Check(menu.rows()[static_cast<size_t>(plainRow)].label == L"   关闭" &&
              menu.rows()[static_cast<size_t>(plainRow)].disabled,
          "未勾选行保留对齐空格，禁用态由组件落位");
    Check(menu.rows()[static_cast<size_t>(pathRow)].glyph == ico::IconId::Folder,
          "空路径行退回 Folder 字形，不留空图标槽");

    menu.AppendLabel(checkedRow, L"（已命名）");
    Check(menu.rows()[static_cast<size_t>(checkedRow)].label == L"✓ 重命名（已命名）",
          "追加说明只改文案，不动动作");
    menu.AppendLabel(999, L"忽略");
    Check(menu.rows()[static_cast<size_t>(checkedRow)].label == L"✓ 重命名（已命名）",
          "越界行号被忽略");

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
