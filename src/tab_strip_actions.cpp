// 标签栏的弹层动作：标签右键菜单、颜色二级菜单、溢出 » 菜单。
//
// 菜单行只回传动作号（WM_APP_TAB_MENU）；颜色档位直接走 tab.color 命令
// （color + index）。真正的动作由 App 的命令管线执行 —— 本文件不改标签状态。
// 行构建统一走 ui::Menu：这里只描述"有哪些动作、何时禁用"，不再手写 PopupRow。
#include "tab_strip.h"

#include "commands.h"   // CmdId / Args / Actor / CommandBus
#include "json.h"       // 构造命令参数 json::Value
#include "menu.h"

void TabStrip::ShowTabMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const size_t i = static_cast<size_t>(index);
    const int n = static_cast<int>(items_.size());

    // 边界要灰掉而不是"点了没反应"（P4：可执行性在菜单里就可见）。
    // 拖拽排序已经把左右移做出来了，但菜单路径的价值在键盘/触控板用户。
    const bool canLeft = index > 0;
    const bool canRight = index + 1 < n;
    // 重复判定已在 SetTabs 里算好（口径唯一，见 core::PathRepeated）。
    const bool hasDup = items_[i].hasDuplicate;
    // 锁定标签不可关；批量关闭还要看"目标集合里有没有非锁定的"。
    bool hasClosableOther = false, hasClosableLeft = false, hasClosableRight = false;
    for (int j = 0; j < n; ++j) {
        // 固定与锁定都不参与批量关闭：固定是"常用目录，别顺手关掉"，
        // 锁定是"明确不许关"。两者都不该让"关闭其他"变成部分成功的盲盒。
        if (items_[static_cast<size_t>(j)].locked || items_[static_cast<size_t>(j)].pinned) continue;
        if (j != index) hasClosableOther = true;
        if (j < index) hasClosableLeft = true;
        if (j > index) hasClosableRight = true;
    }

    ui::Menu menu;
    // 每行 `.Cmd(CmdId)` = 声明"这一行执行哪条命令"，快捷键列由键位表**自动**推出
    // （见 menu.h 的说明）。新增/改键后这些行自动跟随，不需要回来改这里的代码。
    //
    // 【哪些行没有 .Cmd】都不是漏写：
    //   · 目录里确实没有对应命令 —— 例如「复制路径」复制的是**该标签**的路径，
    //     而 path.copy / path.copy.dir 作用于当前活动标签，右键非活动标签时
    //     两者不是一回事；目录里只有 path.copy.tab（复制**所有**标签路径）。
    //     挂上去就会显示一个"按下去复制的是别的东西"的键，宁可不显示。
    //   · 这一行只打开二级菜单（颜色 →）。8 档在 ShowTabColorMenu 里各自挂
    //     CmdId::TabSetColor + 档位参数，键随档位自动显示。
    //   · 锁定/固定按当前状态切换，目录里未绑键 —— 挂上也是空，没必要。
    menu.Add(L"新建标签页", kTabMenuNew, ico::IconId::Add).Cmd(CmdId::TabNew)
        .Add(L"标签切换器...", kTabMenuSwitcher, ico::IconId::Search)
        .Cmd(CmdId::TabSwitcher)
        .Add(L"关闭", kTabMenuClose, ico::IconId::Close, items_[i].locked)
        .Cmd(CmdId::TabClose)
        .Add(L"关闭其他", kTabMenuCloseOther, ico::IconId::Close, !hasClosableOther)
        .Cmd(CmdId::TabCloseOther)
        .Add(L"关闭左侧", kTabMenuCloseLeft, ico::IconId::ChevronLeft, !hasClosableLeft)
        .Cmd(CmdId::TabCloseLeft)
        .Add(L"关闭右侧", kTabMenuCloseRight, ico::IconId::ChevronRight, !hasClosableRight)
        .Cmd(CmdId::TabCloseRight)
        .Add(L"关闭重复标签页", kTabMenuDedup, ico::IconId::Close, !hasDup)
        .Cmd(CmdId::TabCloseDuplicates)
        .Separator()
        .Add(L"重命名标签页...", kTabMenuRename, ico::IconId::Edit).Cmd(CmdId::TabRename)
        // 颜色走二级菜单：8 档色板需要"当前色打勾"，塞进主菜单会撑到 20 行之外。
        // 这一行只负责打开色板，不是 tab.color 本身（同"排序方式 →"），不挂 Cmd。
        .Add(L"设置标签页颜色", kTabMenuColor, ico::IconId::Tag)
        .Separator()
        .Add(L"复制标签", kTabMenuClone, ico::IconId::Copy).Cmd(CmdId::TabClone)
        .Add(items_[i].locked ? L"解锁标签" : L"锁定标签", kTabMenuToggleLock,
             ico::IconId::Lock)
        .Cmd(CmdId::TabLock)
        // 固定与锁定并列但语义不同：固定管位置与误触，锁定管"关不掉"。
        .Add(items_[i].pinned ? L"取消固定" : L"固定标签", kTabMenuTogglePin,
             ico::IconId::Pin)
        .Cmd(CmdId::TabPin)
        // 「复制路径」不挂 .Cmd：见上面文件内的说明（目录里没有"复制某个标签路径"）。
        .Add(L"复制路径", kTabMenuCopyPath, ico::IconId::Copy)
        // 改名之后"复制标签名"才有意义；单独一条而不是让"复制路径"二义。
        .Add(L"复制标签名", kTabMenuCopyTitle, ico::IconId::Copy).Cmd(CmdId::TabCopyTitle)
        .Separator()
        .Add(L"向左移动", kTabMenuMoveLeft, ico::IconId::ChevronLeft, !canLeft)
        .Cmd(CmdId::TabMoveLeft)
        .Add(L"向右移动", kTabMenuMoveRight, ico::IconId::ChevronRight, !canRight)
        .Cmd(CmdId::TabMoveRight)
        .Add(L"移动到开头", kTabMenuMoveFirst, ico::IconId::Back, !canLeft)
        .Cmd(CmdId::TabMoveFirst)
        .Add(L"移动到末尾", kTabMenuMoveLast, ico::IconId::Forward, !canRight)
        .Cmd(CmdId::TabMoveLast)
        .Add(L"移到新窗口（不迁移历史）", kTabMenuMoveNewWindow, ico::IconId::NewWindow,
             items_[i].locked || items_[i].pinned || items_[i].path.empty())
        .Cmd(CmdId::TabMoveNewWindow)
        .Separator()
        .Add(L"重新打开关闭的标签", kTabMenuReopen, ico::IconId::Refresh, !canReopen_)
        .Cmd(CmdId::TabReopenClosed);

    // 最近关闭：栈深 20，菜单只列前 10 条（再多会压过主菜单，也会把
    // 用户真正要点的“关闭/锁定”挤到滚动区）。标题行是禁用态，
    // 只为分组；真正的动作号是 kTabMenuReopenBase + 序号。
    if (!closed_.empty()) {
        menu.Separator().Add(L"最近关闭", -1, ico::IconId::Refresh, true);
        const int shown = std::min<int>(10, static_cast<int>(closed_.size()));
        for (int k = 0; k < shown; ++k) {
            const std::wstring& p = closed_[static_cast<size_t>(k)];
            std::wstring label = nav::Leaf(p);
            if (label.empty()) label = p;
            menu.AddPath(label, p, kTabMenuReopenBase + k, false);
        }
    }

    // 锚点键带标签下标：不同标签的右键菜单是不同锚点。
    const std::wstring toggleKey = L"tab.menu." + std::to_wstring(index);
    ui::MenuOptions opt;
    opt.maxRows = 36;   // 原 21 行 + 最近关闭最多 12 行（标题+10 条+分隔）
    opt.toggleKey = toggleKey.c_str();
    const int action = menu.Show(hwnd_, *theme_, screen, opt);
    if (action < 0) return;

    // 颜色是"选了一个值"而不是"选了一个动作"：再弹一层，选中值走独立消息。
    if (action == kTabMenuColor) {
        // 锚点跟被右键的标签（与主菜单同一坐标），不跟光标：主菜单弹出后
        // 光标可能已被移走，二级菜单飘到别处会让人找不到"这是给哪个标签上色"。
        ShowTabColorMenu(index, screen);
        return;
    }

    // 【w 传标签 id，不传下标】菜单是模态的：Show 期间 AI 通道的定时器仍会派发
    // 命令（tab.close / tab.move），集合一变下标就指向别的标签 —— 用户点了
    // "关闭"，实际关的是旁边那个。lParam 的动作号与下标无关，照旧。
    const int tabId = (index >= 0 && index < static_cast<int>(items_.size()))
                          ? items_[static_cast<size_t>(index)].id
                          : -1;
    PostMessageW(parent_, WM_APP_TAB_MENU, static_cast<WPARAM>(tabId),
                 static_cast<LPARAM>(action));
}

void TabStrip::ShowTabColorMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const int cur = items_[static_cast<size_t>(index)].colorIdx;

    ui::Menu menu;
    int initial = -1;
    // 【只在"右键的正是活动标签"时挂 Cmd】色板作用于**被右键的那个**标签（带 index），
    // 而 tab.color 键作用于**活动标签** —— 两者只在右键的恰好是活动标签时等价。
    // 其余情况挂上去会显示"按下去给另一个标签上色"的键，那是不能容忍的谎。
    const bool sameAsActive = index == active_;
    for (int c = 0; c < Theme::kTabColorCount; ++c) {
        const int row = menu.AddChecked(Theme::TabColorName(c), c == cur, c, ico::IconId::Tag);
        // 档位参数让键位表按 tab.color:0..7 各自命中；未绑的档自动留空。
        if (sameAsActive) menu.Cmd(row, CmdId::TabSetColor, std::to_wstring(c));
        if (c == cur) initial = row;
    }
    const std::wstring toggleKey = L"tab.color." + std::to_wstring(index);
    ui::MenuOptions opt;
    opt.toggleKey = toggleKey.c_str();
    opt.initialRow = initial;
    const int color = menu.Show(hwnd_, *theme_, screen, opt);
    if (color < 0) return;
    // 派发走命令总线（与人机等价）：带 index，于是"给被右键的标签上色"这件事
    // 也进了结构化入口 —— 不再走从菜单直接改标签状态的私有消息。
    json::Value args = json::Value::MakeObject();
    args.Set(L"color", json::Value::MakeInt(color));
    args.Set(L"index", json::Value::MakeInt(index));
    CommandBus::Get().Invoke(CmdId::TabSetColor, Args::FromJson(args), Actor::Ui());
}

void TabStrip::ShowOverflowMenu(POINT screen) {
    // 只列出**当前看不见**的标签；滚到最右端时隐藏集合可能为空，退回全量列表。
    const int viewLeft = 0;
    const int viewRight = overflow_ ? tabViewRight_ : 32767;
    std::vector<int> hidden;
    hidden.reserve(items_.size());
    for (size_t i = 0; i < items_.size() && i < tabRects_.size(); ++i) {
        const RECT& r = tabRects_[i];
        const bool fullyVisible = r.left >= viewLeft && r.right <= viewRight;
        if (!fullyVisible) hidden.push_back(static_cast<int>(i));
    }
    if (hidden.empty()) {
        hidden.clear();
        for (size_t i = 0; i < items_.size(); ++i) hidden.push_back(static_cast<int>(i));
    }

    ui::Menu menu;
    int initial = -1;
    for (int idx : hidden) {
        const TabStripItem& it = items_[static_cast<size_t>(idx)];
        const int row = menu.AddPathChecked(it.title, idx == active_, it.path, idx);
        if (idx == active_) initial = row;
    }
    ui::MenuOptions opt;
    opt.toggleKey = L"tab.overflow";
    opt.initialRow = initial;
    const int picked = menu.Show(hwnd_, *theme_, screen, opt);
    if (picked >= 0) NotifySelect(picked);
}
