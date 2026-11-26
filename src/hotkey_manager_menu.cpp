// 快捷键管理器：右键菜单（行上的就地动作）。
//
// 【为什么单独一个文件】菜单是"把已有动作再暴露一遍"的**投影层**，本身不含
// 任何业务逻辑 —— 每一条都调同一个成员函数，与工具栏/键盘走同一条路径。
// 混在 actions.cpp 里时，它那 50 行"建菜单 + 分发"会夹在"改键状态机"中间，
// 读的人要跳过去才知道"菜单里那一条到底做了什么"。
//
// 【为什么用系统菜单而不是自绘弹层】本窗口的右键菜单只有 7 条、不需要
// 主题化之外的定制。项目里的自绘弹层（popup_list）是为"带图标、带搜索、
// 带多级"的复杂菜单写的，接进来要多维护一份"菜单项 → 动作"的映射。
// 系统菜单在深色下的观感由 uxtheme 负责（ApplyDarkTitleBar 已做过）。
#include "hotkey_manager.h"

#include "str.h"

void HotkeyManager::OnRightClick(int x, int y) {
    // 右键先选中命中的行（与资源管理器一致：右键总是作用于"你点的那一行"），
    // 再弹就地菜单。菜单是**自绘弹层**还是系统菜单？
    //
    // 这里用系统菜单：本窗口的右键菜单只有 5 条、不需要主题化之外的定制，
    // 而项目里已有一套自绘弹层的实现（popup_list），接进来要多维护一份
    // "菜单项 → 动作"的映射。系统菜单在深色下的观感由 uxtheme 负责，
    // 与主窗口的标题栏同一套（ApplyDarkTitleBar 已经做过）。
    const int vi = HitRow(x, y);
    if (vi < 0) return;
    if (!IsSelectedSetting(rows_[static_cast<size_t>(view_[static_cast<size_t>(vi)])].src.setting))
        multiSel_.clear();
    SelectViewIndex(vi);

    const Row* row = Selected();
    if (!row) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool editable = !row->sequence;
    AppendMenuW(menu, MF_STRING | (editable ? 0 : MF_GRAYED), 1, L"改键…\tEnter");
    AppendMenuW(menu, MF_STRING | (editable ? 0 : MF_GRAYED), 2, L"清除键位\tDelete");
    AppendMenuW(menu, MF_STRING | (editable ? 0 : MF_GRAYED), 3, L"还原此项默认\tF2");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"复制命令 id\tCtrl+C");
    AppendMenuW(menu, MF_STRING, 5, L"在命令面板中打开");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (undo_.empty() ? MF_GRAYED : 0), 6, L"撤销\tCtrl+Z");
    AppendMenuW(menu, MF_STRING | (redo_.empty() ? MF_GRAYED : 0), 7, L"重做\tCtrl+Shift+Z");

    POINT sp{x, y};
    ClientToScreen(hwnd_, &sp);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, hwnd_,
                                   nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case 1: BeginCapture(); break;
    case 2: ClearSelected(); break;
    case 3: ResetSelected(); break;
    case 4: CopySelectedIds(); break;
    case 5: {
        // 把命令 id 交给命令面板：用户常从这里跳到"看看这条命令干什么"。
        // 不直接 Invoke —— 那会执行命令，而用户想要的只是"打开面板看它"。
        if (owner_) PostMessageW(owner_, WM_COMMAND, 0xE120, 0);   // 命令面板入口
        SetStatus(L"命令 id：" + row->src.command + L"（已复制）");
        CopySelectedIds();
        break;
    }
    case 6: UndoLast(); break;
    case 7: RedoLast(); break;
    default: break;
    }
}
