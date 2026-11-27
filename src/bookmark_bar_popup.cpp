// 书签栏的弹出层：空白区菜单、溢出菜单、图标子目录树。
//
// 【职责边界】"针对某一项"的右键菜单在 bookmark_bar_itemmenu.cpp（拆出去的
// 那一半）；本文件只剩"针对整条栏"的三类弹层。分类内容弹层（可拖动排序）
// 在 bookmark_category_popup.cpp。
#include "bookmark_bar.h"

#include "commands.h"
#include "folder_tree.h"
#include "icon_resolve.h"
#include "menu.h"
#include "mouse_rules.h"   // CurrentMods：溢出菜单打开时按下的修饰键（与栏上同一口径）
#include "platform_win.h"
#include "prompt.h"
#include "str.h"
#include "text_file.h"
#include "ui_draw.h"

void BookmarkBar::ShowEmptyMenu(POINT screen) {
    BookmarkStore& store = BookmarkStore::Get();
    ui::Menu menu;
    menu.Add(L"添加当前目录为书签", 1, ico::IconId::Add).Cmd(CmdId::BmkAddCurrent)
        .Add(L"收藏所有标签页", 11, ico::IconId::Add).Cmd(CmdId::BmkAddTabs)
        .Add(L"新建分类", 2, ico::IconId::Add).Cmd(CmdId::BmkNewFolder)
        .Add(L"撤销删除", 8, ico::IconId::Undo, !store.CanUndo()).Cmd(CmdId::BmkUndo)
        .Separator()
        .Add(L"按名称排序（含分类内）", 3, ico::IconId::Sort).Cmd(CmdId::BmkSort)
        .Add(L"清理重复书签", 10, ico::IconId::Delete).Cmd(CmdId::BmkDedupe)
        .Add(L"重新载入书签文件", 4, ico::IconId::Refresh).Cmd(CmdId::BmkReload)
        .Add(L"导出书签...", 6, ico::IconId::Export).Cmd(CmdId::BmkExport)
        .Add(L"导入书签...", 7, ico::IconId::Import).Cmd(CmdId::BmkImport)
        .Separator()
        .Add(L"清除全部书签", 9, ico::IconId::Delete).Cmd(CmdId::BmkClearAll)
        .Add(L"隐藏书签栏", 5, ico::IconId::Eye).Cmd(CmdId::BmkHideBar);

    ui::MenuOptions opt;
    opt.toggleKey = L"bmk.empty";
    const int action = menu.Show(hwnd_, *theme_, screen, opt);
    if (action < 0) return;
    switch (action) {
    case 1:
        CommandBus::Get().InvokeLegacy(CmdId::BmkAddCurrent, L"", Actor::Ui());
        break;
    case 11: {  // 收藏所有打开标签页（与 bmk.add.tabs 同一命令）
        // 与 dedupe 同款反馈：批量动作必须报数，不能静默。
        json::Value result;
        const bool ok = CommandBus::Get().InvokeResult(CmdId::BmkAddTabs, Args::Empty(), &result, Actor::Ui());
        const int added = ok ? result.GetInt(L"added", 0) : -1;
        const int skipped = ok ? result.GetInt(L"skipped", 0) : 0;
        if (added > 0)
            MessageBoxW(hwnd_,
                        (L"已收藏 " + std::to_wstring(added) + L" 个标签页目录" +
                         (skipped > 0 ? (L"（跳过 " + std::to_wstring(skipped) +
                                         L" 个：已收藏或空标签）") : L"")).c_str(),
                        L"收藏所有标签页", MB_OK | MB_ICONINFORMATION);
        else if (added == 0 && skipped > 0)
            MessageBoxW(hwnd_,
                        (L"没有新增（" + std::to_wstring(skipped) +
                         L" 个标签页目录均已收藏或无目录）").c_str(),
                        L"收藏所有标签页", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case 2: {
        std::wstring name;
        if (PromptText(hwnd_, *theme_, L"新建分类", L"分类名称", L"新建分类", &name)) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"name", json::Value::MakeStr(name));
            CommandBus::Get().Invoke(CmdId::BmkNewFolder, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case 3:
        // 递归排序（含分类内部）。原实现只排顶层 —— 用户点"按名称排序"后
        // 分类里的顺序纹丝不动，那不是排序，只排了一半。排序入撤销栈（可能误触）。
        CommandBus::Get().Invoke(CmdId::BmkSort, Args::Empty(), Actor::Ui());
        break;
    case 10: {
        const size_t before = store.Flatten().size();
        CommandBus::Get().Invoke(CmdId::BmkDedupe, Args::Empty(), Actor::Ui());
        const size_t after = store.Flatten().size();
        if (after < before) {
            MessageBoxW(hwnd_,
                        (L"已删除 " + std::to_wstring(before - after) +
                         L" 条重复书签（可撤销）").c_str(),
                        L"清理重复书签", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd_, L"没有发现重复书签。", L"清理重复书签",
                        MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case 4:
        CommandBus::Get().Invoke(CmdId::BmkReload, Args::Empty(), Actor::Ui());
        break;
    case 8:
        CommandBus::Get().Invoke(CmdId::BmkUndo, Args::Empty(), Actor::Ui());
        break;
    case 9:
        // 【E24】这里问的具体（说明了可撤销），总线不再问第二遍。
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", id::kBrandShort,
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            CommandBus::Get().MarkConfirmedForNextCall();
            CommandBus::Get().Invoke(CmdId::BmkClearAll, Args::Empty(), Actor::Ui());
        }
        break;
    case 6: {
        json::Value r;
        const bool ok = CommandBus::Get().InvokeResult(CmdId::BmkExport, Args::Empty(), &r, Actor::Ui());
        if (ok)
            MessageBoxW(hwnd_, (L"已导出 " + std::to_wstring(r.GetInt(L"exported", 0)) + L" 个书签").c_str(), L"导出书签", MB_OK | MB_ICONINFORMATION);
        else if (CommandBus::Get().lastFailKind() != FailKind::CanceledByUser)
            MessageBoxW(hwnd_, CommandBus::Get().lastBlockReason().c_str(), L"导出书签", MB_OK | MB_ICONWARNING);
        break;
    }
    case 7: {
        json::Value r;
        const bool ok = CommandBus::Get().InvokeResult(CmdId::BmkImport, Args::Empty(), &r, Actor::Ui());
        if (ok && r.GetInt(L"imported", 0) > 0)
            MessageBoxW(hwnd_, (L"已导入 " + std::to_wstring(r.GetInt(L"imported", 0)) + L" 个书签").c_str(), L"导入书签", MB_OK | MB_ICONINFORMATION);
        else if (!ok && CommandBus::Get().lastFailKind() != FailKind::CanceledByUser)
            MessageBoxW(hwnd_, CommandBus::Get().lastBlockReason().c_str(), L"导入书签", MB_OK | MB_ICONWARNING);
        break;
    }
    case 5:
        CommandBus::Get().InvokeLegacy(CmdId::BmkToggleBar, L"", Actor::Ui());
        break;
    default:
        break;
    }
}

void BookmarkBar::ShowOverflowMenu(POINT screen) {
    // 【只列当前看不见的项】原实现把**全部**项都塞进溢出菜单，包括已经显示在栏上的
    // 那些。于是"点了 » 看到一个书签，再点一次栏上同一个书签"完全等价 ——
    // 溢出菜单没有存在的意义，还让用户以为自己数错了。
    // 溢出菜单的唯一职责是"把看不见的拿出来"，所以这里必须按可见性过滤。
    ui::Menu menu;
    std::vector<int> map;   // action -> items_ 下标
    for (size_t i = 0; i < items_.size(); ++i) {
        // 只列**当前看不全**的项：滚出视野的、以及被裁切线切掉一半的。
        // 判据用 bookmark_bar.h 里那组 inline 函数（与绘制/命中同一来源）——
        // 否则会出现"已经滚进视野的项还挂在 » 菜单里"。
        if (BmkFullyInView(items_[i].rc, viewRight_)) continue;
        menu.AddPath(items_[i].label, items_[i].path, static_cast<int>(map.size()),
                     items_[i].folder);
        map.push_back(static_cast<int>(i));
    }
    if (menu.Empty()) return;

    ui::MenuOptions opt;
    opt.maxRows = 24;
    opt.toggleKey = L"bmk.overflow";
    const int slot = menu.Show(hwnd_, *theme_, screen, opt);
    if (slot < 0 || slot >= static_cast<int>(map.size())) return;
    const int idx = map[static_cast<size_t>(slot)];
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return;

    if (items_[static_cast<size_t>(idx)].folder) {
        // 锚点取溢出按钮本身：分类的菜单从 » 下方展开，而不是从屏幕那个
        // 已经不在栏上的坐标展开（否则菜单会浮在半空，与鼠标位置无关）。
        ShowFolderMenu(idx, screen);
    } else {
        // 【E12 修复】这里原先直接 Navigate(idx)，于是"栏上 Ctrl+点开新标签、
        // » 菜单里 Ctrl+点还是当前标签"—— 同一个书签两种行为。
        // 修饰键在菜单 Show 返回后现读：ui::Menu 是纯 UI 组件，不该为了
        // "这一下按没按 Ctrl"去改它的返回契约。
        const std::wstring path = items_[static_cast<size_t>(idx)].path;
        if (!OpenPathWithMods(path, CurrentMods())) Navigate(idx);
    }
}

void BookmarkBar::ShowChildTree(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];

    // 锚点 = 图标格底边（不是鼠标点），与面包屑下拉"对齐到 chevron 区"同口径，
    // 否则菜单会随鼠标在图标格里的位置上下漂。
    // 滚动后这一项可能只露出右边一截，锚点要夹回栏内（否则弹层会开到栏外）。
    POINT at{std::max<LONG>(0, it.iconRc.left), it.rc.bottom};
    ClientToScreen(hwnd_, &at);

    // 纯分类节点没有路径（只装子书签）→ 没有文件系统子目录可列，
    // 退回原有的"列分类内容"下拉，避免点了图标什么都不发生。
    if (it.path.empty()) {
        ShowFolderMenu(index, at);
        return;
    }

    // 与地址栏分隔符同一份实现：顶层 = 该路径的直接子文件夹，子节点懒加载、就地展开。
    std::vector<TreeNode> top = folder_tree::Children(it.path);
    if (top.empty()) {
        // 没有子目录：空目录直接进去（与 mod 的 ShowFsMenuForPath 同口径 ——
        // 弹一个空菜单等于点了没反应）。文件类书签保持原行为，不动作。
        const DWORD attr = GetFileAttributesW(tf::ToExtended(it.path).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            // 【E26 修复】原先直接 onNavigate_，绕过命令总线（无审计、AI 侧看不到）
            // 与修饰键语义。与栏上左键、» 菜单、分类弹层、键盘 Enter 走同一个
            // 收口点 —— 这是"打开一个书签路径"的第五个触发点。
            if (!OpenPathWithMods(it.path, CurrentMods()) && onNavigate_) {
                onNavigate_(it.path);
                if (refocus_) refocus_();
            }
        }
        return;
    }
    // toggleKey 用书签 id（不是下标）：重排之后同一个书签的下标会变，用下标会让
    // "连点两下"在重排过的栏上判成两个不同锚点。带前缀与面包屑的键区分开。
    const std::wstring toggleKey = L"bmk-icon:" + it.id;
    std::wstring picked =
        folder_tree::Show(hwnd_, *theme_, at, std::move(top), 0, toggleKey.c_str());

    // 弹层是模态的，回来时"光标下高亮"已经不成立，清掉避免残留
    hoverIcon_ = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (!picked.empty()) {
        // 【E26 修复】同上面的空目录分支：树里选中的路径也是"打开一个书签路径"，
        // 必须过收口点（修饰键 + 命令总线），不能直连 onNavigate_。
        if (!OpenPathWithMods(picked, CurrentMods()) && onNavigate_) {
            onNavigate_(picked);
            if (refocus_) refocus_();   // 同 Navigate：打开目录后焦点必须回视图
        }
    }
}
