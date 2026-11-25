// 书签栏的弹出层：项右键菜单、空白区菜单、溢出菜单、图标子目录树。
//
// 【为什么从 bookmark_bar.cpp 拆出来】那一文件原本同时装着"栏本身的输入状态机"
// 与"四套弹出层"，冲到 864 行；而弹出层与栏的绘制/命中是**两种职责**：
// 前者每次都模态地接管整个系统（消息循环、鼠标捕获、焦点），
// 后者只是自绘一条横条。混在一起时，改弹层要在几千行里找，而单文件行数
// 也已经顶到欠账上限（AGENTS.md §3）。
//
// 分类内容弹层（可拖动排序的那个）在 bookmark_category_popup.cpp ——
// 它比这四套都复杂（多一套拖拽状态机），单独成文件才读得动。
#include "bookmark_bar.h"

#include "commands.h"
#include "folder_tree.h"
#include "icon_resolve.h"
#include "menu.h"
#include "platform_win.h"
#include "prompt.h"
#include "str.h"
#include "text_file.h"
#include "ui_draw.h"

void BookmarkBar::ShowItemMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];
    BookmarkStore& store = BookmarkStore::Get();
    ui::Menu menu;   // 每行 .Cmd 自动出快捷键（判断清单见 menu.h）
    menu.Add(L"打开", 1, it.folder ? ico::IconId::Folder : ico::IconId::Page).Cmd(CmdId::FsOpen)
        .Add(L"复制路径", 2, ico::IconId::Copy, it.folder)
        .Separator()
        .Add(L"编辑...", 3, ico::IconId::Edit).Cmd(CmdId::BmkEdit)
        .Add(L"复制书签", 9, ico::IconId::Copy).Cmd(CmdId::BmkCopy)
        .Add(L"删除", 4, ico::IconId::Delete).Cmd(CmdId::BmkRemove)
        .Separator()
        .Add(L"新建分类", 5, ico::IconId::Add).Cmd(CmdId::BmkNewFolder)
        .Add(L"添加当前目录", 6, ico::IconId::Folder).Cmd(CmdId::BmkAddCurrent)
        .Separator();

    // 分类专属的两项：**解散分类**（子项提到上一层）与**只排这个分类**。
    //
    // 为什么放在"新建分类/添加当前目录"之后而不是菜单最上面：
    // 解散是**结构重组**（一次能挪动很多条书签），而打开/编辑/删除是
    // "针对这一项"的动作。分组上把重组放在最后，用户不会误点。
    //
    // 叶子没有这两项 —— 叶子不是分类，没有壳可解散。
    if (it.folder) {
        menu.Add(L"解散分类", 12, ico::IconId::Export).Cmd(CmdId::BmkDissolve)
            .Add(L"排序本分类", 13, ico::IconId::Sort).Cmd(CmdId::BmkSort)
            .Separator();
    }

    menu.Add(L"撤销删除", 7, ico::IconId::Undo, !store.CanUndo()).Cmd(CmdId::BmkUndo)
        .Add(L"清除全部书签", 8, ico::IconId::Delete).Cmd(CmdId::BmkClearAll);

    // 右键菜单属于某一项：键带上 id，不同项的右键菜单互不当成连点。
    const std::wstring toggleKey = L"bmk-menu:" + it.id;
    ui::MenuOptions opt;
    opt.toggleKey = toggleKey.c_str();
    const int action = menu.Show(hwnd_, *theme_, screen, opt);
    if (action < 0) return;
    switch (action) {
    case 1: {
        // 分类开子菜单；叶子走 fs.open，与文件视图双击/AI 带参调用同源。
        if (it.folder) ShowFolderMenu(index, screen);
        else if (!it.path.empty()) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"path", json::Value::MakeStr(it.path));
            if (CommandBus::Get().Invoke(CmdId::FsOpen, Args::FromJson(a), Actor::Ui()) && refocus_) refocus_();
        }
        break;
    }
    case 2: {
        // 复制路径 = clipboard.copy；与面包屑右键共用命令与审计。
        if (!it.path.empty()) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"text", json::Value::MakeStr(it.path));
            CommandBus::Get().Invoke(CmdId::ClipboardCopy, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case 3: {
        // 编辑：名称 + 路径双字段（纯分类节点只有名称一项可改）。
        //
        // 空路径的语义：Update(id, name, L"") 里 L"" 表示"这一项不改"，于是
        // 用户把路径清空想改成纯分类时会被静默保留。所以这里走命令 bmk.edit
        // 并**显式**带 path —— 命令侧用 SetNameAndPath（空串就是清空）。
        std::wstring name = it.label;
        std::wstring path = it.path;
        bool confirmed;
        if (it.folder && it.path.empty()) {
            confirmed = PromptText(hwnd_, *theme_, L"编辑", L"名称", it.label, &name);
        } else {
            confirmed = PromptText2(hwnd_, *theme_, L"编辑", L"名称", it.label, &name, L"路径",
                                    it.path, &path);
        }
        if (!confirmed) break;
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(it.id));
        a.Set(L"name", json::Value::MakeStr(name));
        // 分类无路径时不要传 path（否则会把"没有路径"改成"路径是空串"，
        // 对 BookmarkStore 来说两者不同：后者会显式清掉 path）。
        if (!it.folder || !path.empty()) a.Set(L"path", json::Value::MakeStr(path));
        CommandBus::Get().Invoke(CmdId::BmkEdit, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 4:
        if (MessageBoxW(hwnd_, (L"删除书签“" + it.label + L"”？").c_str(), id::kBrandShort,
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(it.id));
            CommandBus::Get().Invoke(CmdId::BmkRemove, Args::FromJson(a), Actor::Ui());
        }
        break;
    case 5: {
        std::wstring name;
        if (PromptText(hwnd_, *theme_, L"新建分类", L"分类名称", L"新建分类", &name)) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"name", json::Value::MakeStr(name));
            CommandBus::Get().Invoke(CmdId::BmkNewFolder, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case 6:
        CommandBus::Get().InvokeLegacy(CmdId::BmkAddCurrent, L"", Actor::Ui());
        break;
    case 7:  // 撤销：恢复上一次删除/重排/清空前的快照（store 内部维护，≤20 步）
        CommandBus::Get().Invoke(CmdId::BmkUndo, Args::Empty(), Actor::Ui());
        break;
    case 8:  // 清除全部书签：可撤销（ReplaceAll 已入快照栈）
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", id::kBrandShort,
                        MB_YESNO | MB_ICONWARNING) == IDYES)
            CommandBus::Get().Invoke(CmdId::BmkClearAll, Args::Empty(), Actor::Ui());
        break;
    case 9: {  // 复制书签（分类连子树）：与 bmk.copy 同一命令，AI/面板可达同一动作
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(it.id));
        CommandBus::Get().Invoke(CmdId::BmkCopy, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 12: {  // 解散分类：子项提到上一层（顶层分类 → 书签栏），分类本身消失
        // 确认一下是有必要的：它一次能挪动很多条书签，用户点错一行就整片搬家。
        if (MessageBoxW(hwnd_,
                        (L"解散分类“" + it.label + L"”？\n\n"
                         L"分类里的条目会移到上一层（顶层分类则回到书签栏），分类本身消失。")
                            .c_str(),
                        id::kBrandShort, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(it.id));
            CommandBus::Get().Invoke(CmdId::BmkDissolve, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case 13: {  // 只排这个分类（不动别的层）
        json::Value a = json::Value::MakeObject();
        a.Set(L"parentId", json::Value::MakeStr(it.id));
        CommandBus::Get().Invoke(CmdId::BmkSort, Args::FromJson(a), Actor::Ui());
        break;
    }
    default:
        break;
    }
}

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
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", id::kBrandShort,
                        MB_YESNO | MB_ICONWARNING) == IDYES)
            CommandBus::Get().Invoke(CmdId::BmkClearAll, Args::Empty(), Actor::Ui());
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
    // 【只列被折叠的项】原实现把**全部**项都塞进溢出菜单，包括已经显示在栏上的
    // 那些。于是"点了 » 看到一个书签，再点一次栏上同一个书签"完全等价 ——
    // 溢出菜单没有存在的意义，还让用户以为自己数错了。
    // 溢出菜单的唯一职责是"把看不见的拿出来"，所以这里必须按可见性过滤。
    ui::Menu menu;
    std::vector<int> map;   // action -> items_ 下标
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].rc.right > items_[i].rc.left) continue;   // 已在栏上可见
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
        Navigate(idx);
    }
}

void BookmarkBar::ShowChildTree(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];

    // 锚点 = 图标格底边（不是鼠标点），与面包屑下拉"对齐到 chevron 区"同口径，
    // 否则菜单会随鼠标在图标格里的位置上下漂。
    POINT at{it.iconRc.left, it.rc.bottom};
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
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && onNavigate_)
            onNavigate_(it.path);
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
    if (!picked.empty() && onNavigate_) {
        onNavigate_(picked);
        if (refocus_) refocus_();   // 同 Navigate：打开目录后焦点必须回视图
    }
}
