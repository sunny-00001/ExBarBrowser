// 标签栏的弹层动作：标签右键菜单、颜色二级菜单、溢出 » 菜单。
//
// 菜单项只回传动作号（WM_APP_TAB_MENU）或颜色值（WM_APP_TAB_COLOR），
// 真正的动作由 App 的命令管线执行 —— 本文件不改标签状态。
#include "tab_strip.h"

#include "str.h"

#include "popup_list.h"

void TabStrip::ShowTabMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const size_t i = static_cast<size_t>(index);
    const int n = static_cast<int>(items_.size());

    // 边界要灰掉而不是"点了没反应"（P4：可执行性在菜单里就可见）。
    // 拖拽排序已经把左右移做出来了，但菜单路径的价值在键盘/触控板用户。
    const bool canLeft = index > 0;
    const bool canRight = index + 1 < n;

    // 重复判定按路径（大小写不敏感），口径与 core::TabSet::DuplicatesOf 一致。
    bool hasDup = false;
    for (size_t k = 0; k < items_.size() && !hasDup; ++k) {
        if (k == i || items_[k].path.empty()) continue;
        if (str::IEquals(items_[k].path, items_[i].path)) hasDup = true;
    }

    std::vector<PopupRow> rows;
    rows.push_back({L"新建标签页", L"", nullptr, false, false, false, kTabMenuNew});
    rows.push_back({L"关闭", L"", nullptr, false, false, false, kTabMenuClose});
    rows.push_back({L"关闭其他", L"", nullptr, false, false, false, kTabMenuCloseOther});
    rows.push_back({L"关闭左侧", L"", nullptr, false, false, false, kTabMenuCloseLeft});
    rows.push_back({L"关闭右侧", L"", nullptr, false, false, false, kTabMenuCloseRight});
    // 没有重复项时灰掉：留着可点会让"关闭重复"看起来像生效了（其实一个也没关）。
    rows.push_back({L"关闭重复标签页", L"", nullptr, false, false, !hasDup, kTabMenuDedup});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});

    rows.push_back({L"重命名标签页...", L"", nullptr, false, false, false, kTabMenuRename});
    // 颜色走二级菜单：8 档色板需要"当前色打勾"，塞进主菜单会撑到 20 行上限之外。
    rows.push_back({L"设置标签页颜色", L"", nullptr, false, false, false, kTabMenuColor});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});

    rows.push_back({L"复制标签", L"", nullptr, false, false, false, kTabMenuClone});
    rows.push_back({items_[i].locked ? L"解锁标签" : L"锁定标签", L"", nullptr, false, false,
                    false, kTabMenuToggleLock});
    rows.push_back({L"复制路径", L"", nullptr, false, false, false, kTabMenuCopyPath});
    // 改名之后"复制标签名"才有意义（复制路径已经被 Shell 名覆盖了）；
    // 单独一条而不是让"复制路径"二义，调用方要哪个很清楚。
    rows.push_back({L"复制标签名", L"", nullptr, false, false, false, kTabMenuCopyTitle});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});

    rows.push_back({L"向左移动", L"", nullptr, false, false, !canLeft, kTabMenuMoveLeft});
    rows.push_back({L"向右移动", L"", nullptr, false, false, !canRight, kTabMenuMoveRight});
    rows.push_back({L"移动到开头", L"", nullptr, false, false, !canLeft, kTabMenuMoveFirst});
    rows.push_back({L"移动到末尾", L"", nullptr, false, false, !canRight, kTabMenuMoveLast});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"重新打开关闭的标签", L"", nullptr, false, false, false, kTabMenuReopen});

    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0) return;
    int action = rows[static_cast<size_t>(pick)].data;

    // 颜色是"选了一个值"而不是"选了一个动作"：再弹一层，选中值走独立消息。
    if (action == kTabMenuColor) {
        // 二级菜单落在鼠标当前位置（= 用户刚点的那一行），而不是原始右键点 ——
        // 在同一个锚点再弹一次会看起来像"菜单没关掉又刷了一遍"。
        POINT at = screen;
        GetCursorPos(&at);
        ShowTabColorMenu(index, at);
        return;
    }

    PostMessageW(parent_, WM_APP_TAB_MENU, static_cast<WPARAM>(index),
                 static_cast<LPARAM>(action));
}

void TabStrip::ShowTabColorMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const int cur = items_[static_cast<size_t>(index)].colorIdx;

    std::vector<PopupRow> rows;
    for (int c = 0; c < Theme::kTabColorCount; ++c) {
        // 打勾而不是高亮：高亮会被当成"光标所在行"，用户会以为已经选上了。
        std::wstring label = std::wstring(c == cur ? L"✓ " : L"   ") + Theme::TabColorName(c);
        rows.push_back({label, L"", nullptr, false, false, false, c});
    }
    int pick = ShowPopupList(hwnd_, *theme_, screen, rows, 0);
    if (pick < 0) return;
    const int color = rows[static_cast<size_t>(pick)].data;
    if (color < 0) return;
    PostMessageW(parent_, WM_APP_TAB_COLOR, static_cast<WPARAM>(index),
                 static_cast<LPARAM>(color));
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
