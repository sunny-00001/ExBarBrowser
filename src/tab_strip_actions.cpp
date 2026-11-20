// 标签栏的弹层动作：标签右键菜单、溢出 » 菜单。
//
// 菜单项只回传动作号（WM_APP_TAB_MENU）或选中索引，真正的动作由 App 的命令管线执行。
#include "tab_strip.h"

#include "popup_list.h"

void TabStrip::ShowTabMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    std::vector<PopupRow> rows;
    rows.push_back({L"新建标签页", L"", nullptr, false, false, false, 1});
    rows.push_back({L"关闭", L"", nullptr, false, false, false, 2});
    rows.push_back({L"关闭其他", L"", nullptr, false, false, false, 3});
    rows.push_back({L"关闭左侧", L"", nullptr, false, false, false, 4});
    rows.push_back({L"关闭右侧", L"", nullptr, false, false, false, 5});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"复制标签", L"", nullptr, false, false, false, 6});
    rows.push_back({items_[static_cast<size_t>(index)].locked ? L"解锁标签" : L"锁定标签", L"",
                    nullptr, false, false, false, 7});
    rows.push_back({L"复制路径", L"", nullptr, false, false, false, 8});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"重新打开关闭的标签", L"", nullptr, false, false, false, 9});

    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0) return;
    int action = rows[static_cast<size_t>(pick)].data;
    PostMessageW(parent_, WM_APP_TAB_MENU, static_cast<WPARAM>(index),
                 static_cast<LPARAM>(action));
}

void TabStrip::ShowOverflowMenu(POINT screen) {
    std::vector<PopupRow> rows;
    for (size_t i = 0; i < items_.size(); ++i)
        rows.push_back({items_[i].title, items_[i].path, items_[i].icon, false, false, false,
                        static_cast<int>(i)});
    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick >= 0 && pick < static_cast<int>(rows.size()))
        NotifySelect(rows[static_cast<size_t>(pick)].data);
}
