// 统一菜单组件自测：验证行构建、勾选态字段、分隔行、追加文案、路径图标兜底
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
    Check(menu.rows()[static_cast<size_t>(checkedRow)].label == L"重命名" &&
              menu.rows()[static_cast<size_t>(checkedRow)].checked,
          "勾选态是独立字段，label 保持干净文案");
    Check(menu.rows()[static_cast<size_t>(plainRow)].label == L"关闭" &&
              !menu.rows()[static_cast<size_t>(plainRow)].checked &&
              menu.rows()[static_cast<size_t>(plainRow)].disabled,
          "未勾选行不带前缀（不用空格占位），禁用态由组件落位");
    Check(menu.rows()[static_cast<size_t>(pathRow)].glyph == ico::IconId::Folder,
          "空路径行退回 Folder 字形，不留空图标槽");
    Check(menu.rows()[static_cast<size_t>(pathRow)].checked,
          "路径勾选行的勾选态也落在 checked 字段");

    // 勾选列：只要有一行可勾选就留整列（未勾选行留空，不靠空格占位）。
    Check(HasCheckedRow(menu.rows()), "有勾选行 → 弹层留出勾选列");
    {
        ui::Menu plain;
        plain.Add(L"新建", 1, ico::IconId::Add);
        Check(!HasCheckedRow(plain.rows()), "没有勾选行 → 不留勾选列（不留空槽）");
    }

    menu.AppendLabel(checkedRow, L"（已命名）");
    Check(menu.rows()[static_cast<size_t>(checkedRow)].label == L"重命名（已命名）",
          "追加说明只改文案，不动动作");
    menu.AppendLabel(999, L"忽略");
    Check(menu.rows()[static_cast<size_t>(checkedRow)].label == L"重命名（已命名）",
          "越界行号被忽略");

    // ---- 快捷键列：由**命令身份**自动推出 ----
    //
    // 【为什么必须在菜单层再测一次】key_binding_test 验的是"身份/规格 → 文案"，
    // 而这里要验的是**接线**：`.Cmd(id)` 记到哪一行、参数有没有带上、
    // 越界/分隔行是否被挡住、解析器注入后真的把文案填进 PopupRow::shortcut。
    // 接错线的表现是"某几行显示了别人的键"，比推错一个键更难看、更难发现。
    {
        ui::Menu m2;
        m2.Add(L"新建标签页", 1, ico::IconId::Add).Cmd(CmdId::TabNew);
        m2.Separator();
        m2.Add(L"关闭", 2, ico::IconId::Close).Cmd(CmdId::TabClose);
        m2.Add(L"第 4 档视图", 3, ico::IconId::GridView).Cmd(CmdId::ViewMode, L"3");
        // 例外通道：没有 CommandDef 对应的纯 UI 行
        m2.Add(L"完成", 4, ico::IconId::Check).Key(L"ui.finish");

        Check(m2.Count() == 5, "Cmd/Key 不额外占用行（只给已有行挂锚点）");
        Check(m2.rows()[0].shortcut.empty(), "构建期不解析：shortcut 仍为空");
        Check(m2.rows()[2].data == 2, "Cmd 不改变动作号（只挂锚点）");

        // 分隔行 / 越界行都不得挂上锚点：挂上了就会画出一条对不上的提示。
        m2.Cmd(1, CmdId::TabClose);
        m2.Cmd(999, CmdId::TabClose);

        // 注入两个解析器，验证"身份 + 参数 → 该行文案"的对应关系。
        // 断言点：CmdId 那条路收到的**参数**必须原样传下去（第 4 档的 "3"）。
        ui::Menu::SetShortcutResolver(
            [](CmdId id, const std::wstring& arg) -> std::wstring {
                if (id == CmdId::TabNew) return L"Ctrl+T";
                if (id == CmdId::TabClose) return L"Ctrl+W";
                if (id == CmdId::ViewMode) return L"Ctrl+Shift+" + arg;   // 参数要原样
                return std::wstring();
            },
            [](const std::wstring& spec) -> std::wstring {
                return spec == L"ui.finish" ? L"Enter" : std::wstring();
            });
        std::vector<PopupRow> filled = m2.rows();
        m2.FillShortcuts(&filled);
        Check(filled[0].shortcut == L"Ctrl+T", "第 0 行按命令身份拿到自己的键");
        Check(filled[1].shortcut.empty(), "分隔行没有键位（锚点被挡住）");
        Check(filled[2].shortcut == L"Ctrl+W", "第 2 行按命令身份拿到自己的键");
        Check(filled[3].shortcut == L"Ctrl+Shift+3",
              "带参行把参数原样传给解析器（第 4 档 → :3）");
        Check(filled[4].shortcut == L"Enter", "例外通道（无 CommandDef 的行）走规格解析");

        // 未注入解析器时一律留空：纯逻辑单测与"没有这功能"时行为完全一致。
        ui::Menu::SetShortcutResolver(nullptr, nullptr);
        std::vector<PopupRow> none = m2.rows();
        m2.FillShortcuts(&none);
        Check(none[0].shortcut.empty(), "未注入解析器 → 不显示任何键位");

        // 解析器返回空 = 这条命令没有键 → 该行右端留空（业界口径：不画占位符）
        ui::Menu::SetShortcutResolver(
            [](CmdId, const std::wstring&) { return std::wstring(); },
            [](const std::wstring&) { return std::wstring(); });
        std::vector<PopupRow> blank = m2.rows();
        m2.FillShortcuts(&blank);
        Check(blank[0].shortcut.empty() && blank[4].shortcut.empty(),
              "解析器返回空 → 行右端留空（有就画、没有就空）");

        // 只注入 Cmd 解析器、不注入规格解析器：例外通道静默留空（不崩、不猜）
        ui::Menu::SetShortcutResolver(
            [](CmdId, const std::wstring&) { return L"X"; }, nullptr);
        std::vector<PopupRow> mixed = m2.rows();
        m2.FillShortcuts(&mixed);
        Check(mixed[0].shortcut == L"X" && mixed[4].shortcut.empty(),
              "只注入命令解析器时，例外通道留空而不是借用命令结果");

        ui::Menu::SetShortcutResolver(nullptr, nullptr);
    }

    // ---- 「自动」的核心断言：同一份菜单，键位表变了，右端文案跟着变 ----
    //
    // 这就是用户要的那件事：给某条命令**新增**一个快捷键之后，所有挂该命令的行
    // 自动带上它 —— 调用点一行代码都不用改。
    //
    // 这里用两个"键位表版本"注入同一个 Menu 对象（不重建菜单），验证：
    //   1. 第一次 Show 前解析 ⇒ 旧键；
    //   2. 换成新表 ⇒ 同一份菜单解析出新键（不需要重新 Add/Cmd）。
    // 之所以能在不建窗的前提下测，是因为解析与绘制分离（FillShortcuts 纯函数）。
    {
        ui::Menu m3;
        m3.Add(L"关闭", 1, ico::IconId::Close).Cmd(CmdId::TabClose);
        m3.Add(L"复制标签", 2, ico::IconId::Copy).Cmd(CmdId::TabClone);

        // 版本 A：tab.close 绑 Ctrl+W
        ui::Menu::SetShortcutResolver(
            [](CmdId id, const std::wstring&) -> std::wstring {
                if (id == CmdId::TabClose) return L"Ctrl+W";
                return std::wstring();   // tab.clone 还没键
            },
            nullptr);
        std::vector<PopupRow> a = m3.rows();
        m3.FillShortcuts(&a);
        Check(a[0].shortcut == L"Ctrl+W", "版本 A：关闭 = Ctrl+W");
        Check(a[1].shortcut.empty(), "版本 A：复制标签还没绑键 → 留空");

        // 版本 B：给 tab.clone **新增**键（模拟用户改键位表 / 管理器里加键）
        ui::Menu::SetShortcutResolver(
            [](CmdId id, const std::wstring&) -> std::wstring {
                if (id == CmdId::TabClose) return L"Ctrl+W";
                if (id == CmdId::TabClone) return L"Ctrl+K";
                return std::wstring();
            },
            nullptr);
        std::vector<PopupRow> b = m3.rows();
        m3.FillShortcuts(&b);
        Check(b[0].shortcut == L"Ctrl+W", "版本 B：关闭仍是 Ctrl+W");
        Check(b[1].shortcut == L"Ctrl+K",
              "新增键后同一份菜单自动带上（调用点未改）—— 这就是\"自动\"");

        ui::Menu::SetShortcutResolver(nullptr, nullptr);
    }

    // ---- 色板：8 档各自带档位参数，键位表新增哪一档就只有那一档亮 ----
    {
        ui::Menu palette;
        for (int c = 0; c < 8; ++c) {
            const int row = palette.AddChecked(std::to_wstring(c), false, c, ico::IconId::Tag);
            palette.Cmd(row, CmdId::TabSetColor, std::to_wstring(c));
        }
        ui::Menu::SetShortcutResolver(
            [](CmdId id, const std::wstring& arg) -> std::wstring {
                return id == CmdId::TabSetColor && arg == L"3" ? L"Ctrl+Alt+3" : std::wstring();
            },
            nullptr);
        std::vector<PopupRow> rows = palette.rows();
        palette.FillShortcuts(&rows);
        Check(rows[3].shortcut == L"Ctrl+Alt+3", "色板第 3 档按自己的参数自动带上键");
        Check(rows[0].shortcut.empty() && rows[7].shortcut.empty(), "未绑键的色档留空");
        ui::Menu::SetShortcutResolver(nullptr, nullptr);
    }

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
