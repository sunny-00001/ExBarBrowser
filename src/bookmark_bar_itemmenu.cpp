// 书签栏的项级菜单：项右键菜单（打开/编辑/删除/移到分类…全家）与
// 「移到分类…」的二层菜单。
//
// 【为什么从 bookmark_bar_popup.cpp 拆出来】那个文件装着"四套弹出层"，
// 项菜单（含移到分类的递归目标收集）独占近半行数；拆开后 popup 只剩
// 空白区菜单/溢出菜单/子目录树三类"栏级"弹层，行数回到棘轮内余量。
#include "bookmark_bar.h"

#include "commands.h"
#include "icon_resolve.h"
#include "menu.h"
#include "platform_win.h"
#include "prompt.h"
#include "str.h"
#include "text_file.h"
#include "ui_draw.h"


namespace {

struct MoveTarget {
    std::wstring id;
    std::wstring name;
    int depth = 0;
};

// 递归收集可选的目标分类：{id, 显示名, 深度}。
// excludeId = 被移动的项；它自己是分类时，它的整棵子树都不能当目标
// （BmkMove 会拒绝"移到自己子孙"，但菜单里就该先置灰/不列，别让用户点了才失败）。
void CollectMoveTargets(const BookmarkList& list, const std::wstring& excludeId, int depth,
                        std::vector<MoveTarget>* out) {
    // 深度上限：与 bookmark_category_popup 的下钻守卫同口径（32 层）。
    // 书签树正常不会这么深，但数据被外部改坏成自引用环时，没有这道闸就是
    // 递归到栈溢出（而崩点在"点右键 → 移到分类"这条路径上，很难联想到数据）。
    if (depth > 32) return;
    for (const BookmarkNode& n : list) {
        if (!n.isFolder) continue;
        if (n.id != excludeId) {
            out->push_back({n.id, n.name, depth});
        }
        // 被排除节点的子树整体不收（移到自己子孙 = 非法）。
        if (n.id != excludeId) CollectMoveTargets(n.children, excludeId, depth + 1, out);
    }
}

}  // namespace

// 一个分类下**可打开**的叶子数（递归，跳过纯分类与空 path）。
// 「打开全部（N 项）」的数字必须与实际会开的标签数一致 —— 菜单里写 N、
// 实际开 M 个，是比不写数字更糟的反馈（用户会以为漏了）。
int BookmarkBar::CountOpenableChildren(const std::wstring& folderId) const {
    const BookmarkNode* node = BookmarkStore::Get().Find(folderId);
    if (!node || !node->isFolder) return 0;
    struct Walk {
        static int Run(const BookmarkList& list, int depth) {
            if (depth > 32) return 0;   // 与 CollectMoveTargets / ShowFolderContents 同口径
            int n = 0;
            for (const BookmarkNode& e : list) {
                if (e.isFolder)
                    n += Run(e.children, depth + 1);
                else if (!e.path.empty())
                    ++n;
            }
            return n;
        }
    };
    return Walk::Run(node->children, 0);
}

// 与"打开分类"下钻一致的既有形态（见本文件 ShowFolderContents / 分类 popup）。
void BookmarkBar::ShowMoveToMenu(const std::wstring& id, const std::wstring& label, POINT screen) {
    BookmarkStore& store = BookmarkStore::Get();
    std::vector<MoveTarget> targets;
    CollectMoveTargets(store.roots(), id, 0, &targets);

    // 最近用过的目标排前面（顶层始终第一）。stable_sort 保证没进 MRU 的
    // 分类仍按树序，不会因为"最近"这一层把层级关系搅乱。
    auto mruRank = [this](const std::wstring& tid) -> int {
        auto it = std::find(recentMoveTargets_.begin(), recentMoveTargets_.end(), tid);
        return it == recentMoveTargets_.end() ? static_cast<int>(recentMoveTargets_.size())
                                              : static_cast<int>(it - recentMoveTargets_.begin());
    };
    std::stable_sort(targets.begin(), targets.end(),
                     [&](const MoveTarget& x, const MoveTarget& y) {
                         return mruRank(x.id) < mruRank(y.id);
                     });

    ui::Menu menu;
    // action 0 = 顶层；1..N = targets[i-1]。
    menu.Add(L"书签栏（顶层）", 0, ico::IconId::Bookmark);
    for (size_t i = 0; i < targets.size(); ++i) {
        const MoveTarget& t = targets[i];
        // 用缩进表达层级（与旧版目录树一致）：两层以内看得清，再深也不至于糊成一片。
        const std::wstring indent(t.depth > 2 ? 2 : t.depth, L' ');
        const std::wstring mark =
            mruRank(t.id) < static_cast<int>(recentMoveTargets_.size()) ? L"[最近] " : L"";
        menu.Add(indent + mark + t.name, static_cast<int>(i) + 1, ico::IconId::Folder);
    }
    if (targets.empty()) menu.Add(L"（暂无其它分类）", -1, ico::IconId::Folder, true);

    ui::MenuOptions opt;
    // toggleKey 必须活到 menu.Show 返回：写成 (L"..." + id).c_str() 会把指针
    // 指向一个语句结束就析构的临时串（-Wdangling-assignment-gsl 已报）。
    const std::wstring toggleKey = L"bmk-moveto:" + id;
    opt.toggleKey = toggleKey.c_str();
    const int pick = menu.Show(hwnd_, *theme_, screen, opt);
    if (pick < 0 || (pick > static_cast<int>(targets.size()))) return;

    json::Value a = json::Value::MakeObject();
    a.Set(L"id", json::Value::MakeStr(id));
    if (pick > 0) {
        // pick-1 = targets 下标；不传 toIndex = 追加到该分类末尾。
        a.Set(L"toId", json::Value::MakeStr(targets[static_cast<size_t>(pick - 1)].id));
    }
    const bool ok = CommandBus::Get().Invoke(CmdId::BmkMove, Args::FromJson(a), Actor::Ui());
    if (ok) {
        LogDbg(L"[bmk] 移到分类 %ls → %ls", label.c_str(),
               pick == 0 ? L"顶层" : targets[static_cast<size_t>(pick - 1)].name.c_str());
        if (pick > 0) {
            const std::wstring chosen = targets[static_cast<size_t>(pick - 1)].id;
            auto it = std::find(recentMoveTargets_.begin(), recentMoveTargets_.end(), chosen);
            if (it != recentMoveTargets_.end()) recentMoveTargets_.erase(it);
            recentMoveTargets_.insert(recentMoveTargets_.begin(), chosen);
            if (recentMoveTargets_.size() > 5) recentMoveTargets_.resize(5);
        }
    }
}

void BookmarkBar::ShowItemMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    // 【值拷贝，不是引用】下面跨 menu.Show()（模态循环）与 PromptText() 使用
    // it 的字段。模态期间 WM_TIMER 照常派发，AI 通道的 bmk.* 命令会走到
    // BookmarkBar::Reload() → BuildItems()（clear + 重建）—— 引用当场悬空。
    // Item 本身很小（含 HICON 借用），拷一份的代价可以忽略。
    const Item it = items_[static_cast<size_t>(index)];
    BookmarkStore& store = BookmarkStore::Get();
    // 「打开」族的六条去处都走 CommandBus，参数 path 来自这一项本身。
    // disabled 判据用 it.path.empty() 而不是 it.folder：纯分类（只装子书签、无目录）
    // 没有可打开的路径；带路径的分类则与其它书签一样能开。
    const bool noPath = it.path.empty();
    ui::Menu menu;   // 每行 .Cmd 自动出快捷键（判断清单见 menu.h）
    menu.Add(L"打开", 1, it.folder ? ico::IconId::Folder : ico::IconId::Page).Cmd(CmdId::FsOpen)
        .Add(L"在新标签中打开", 14, ico::IconId::Page, noPath).Cmd(CmdId::FsOpenInNewTab)
        .Add(L"在后台新标签中打开", 15, ico::IconId::Page, noPath).Cmd(CmdId::FsOpenInNewTab)
        .Add(L"在新窗口中打开", 16, ico::IconId::NewWindow, noPath).Cmd(CmdId::FsOpenInNewWindow)
        .Add(L"在另一窗格中打开", 17, ico::IconId::Window, noPath).Cmd(CmdId::FsOpenInPane)
        .Add(L"在终端中打开", 18, ico::IconId::Terminal, noPath).Cmd(CmdId::ExtTerminalAt)
        .Add(L"属性", 19, ico::IconId::Settings, noPath).Cmd(CmdId::FsProperties)
        .Separator()
        .Add(L"复制路径", 2, ico::IconId::Copy, it.folder)
        .Separator()
        .Add(L"编辑...", 3, ico::IconId::Edit).Cmd(CmdId::BmkEdit)
        .Add(L"复制书签", 9, ico::IconId::Copy).Cmd(CmdId::BmkCopy)
        .Add(L"删除", 4, ico::IconId::Delete).Cmd(CmdId::BmkRemove)
        .Add(L"移到分类…", 20, ico::IconId::Export)  // 二层菜单：列出所有分类 + 顶层
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
        // 「打开全部」：浏览器书签夹的标配（Chrome/Edge 的 Open all、Firefox 的
        // Open All in Tabs、Vivaldi 的 Open in New Tab）。本项目的分类是"一组
        // 常用目录"，没有这一条时想开整组只能逐个点 —— 分类的意义就少了一半。
        //
        // 放在"打开"族**之后、重组族之前**：它属于打开语义（一次开很多个），
        // 不是结构改动。中键点分类是同一个动作（见 bookmark_bar_open.cpp）。
        const int childCount = CountOpenableChildren(it.id);
        menu.Add(L"打开全部（" + std::to_wstring(childCount) + L" 项）", 21, ico::IconId::Folder,
                 childCount == 0);
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

    // 「打开」族共用的派发：参数键由命令自己定（属性是 target，其余是 path）。
    // refocus 只对"焦点仍应留在我们窗口"的那些做：新窗口/终端/属性会把焦点交给
    // 外部窗口或系统对话框，抢回来反而打断用户。
    auto invokePath = [&](CmdId id, const wchar_t* key, bool refocus) {
        json::Value a = json::Value::MakeObject();
        a.Set(key, json::Value::MakeStr(it.path));
        const bool ok = CommandBus::Get().Invoke(id, Args::FromJson(a), Actor::Ui());
        if (ok && refocus && refocus_) refocus_();
        return ok;
    };

    switch (action) {
    case 14:  // 在新标签中打开
        invokePath(CmdId::FsOpenInNewTab, L"path", true);
        break;
    case 15: {  // 在后台新标签中打开：同一个命令，多带一个 background
        json::Value a = json::Value::MakeObject();
        a.Set(L"path", json::Value::MakeStr(it.path));
        a.Set(L"background", json::Value::MakeBool(true));
        CommandBus::Get().Invoke(CmdId::FsOpenInNewTab, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 16:  // 在新窗口中打开：外部窗口拿焦点，不抢回
        invokePath(CmdId::FsOpenInNewWindow, L"path", false);
        break;
    case 17:  // 在另一窗格中打开：活动窗格不变，焦点回当前视图
        invokePath(CmdId::FsOpenInPane, L"path", true);
        break;
    case 18:  // 在终端中打开
        invokePath(CmdId::ExtTerminalAt, L"path", false);
        break;
    case 19:  // 属性（系统对话框）：键是 target
        invokePath(CmdId::FsProperties, L"target", false);
        break;
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
    case 4: {
        // 删除单条书签不弹确认（用户明确要求）：高频操作，误删可从「撤销删除」
        // 恢复。bmk.remove 已在 kNoConfirmDestructive 白名单里，所以这里既不需要
        // MessageBoxW，也不需要 MarkConfirmedForNextCall；命令仍走总线，
        // 审计与撤销栈一步不少。清全部/解散分类不在白名单，仍保留各自确认框。
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(it.id));
        CommandBus::Get().Invoke(CmdId::BmkRemove, Args::FromJson(a), Actor::Ui());
        break;
    }
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
        // 同 case 4：这里问过了，总线不再问（E24）。
        if (MessageBoxW(hwnd_, L"确定清除全部书签？可在“撤销删除”中恢复。", id::kBrandShort,
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            CommandBus::Get().MarkConfirmedForNextCall();
            CommandBus::Get().Invoke(CmdId::BmkClearAll, Args::Empty(), Actor::Ui());
        }
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
            CommandBus::Get().MarkConfirmedForNextCall();
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
    case 20:  // 移到分类…：二层菜单列出目标，走 bmk.move（与拖拽/AI 同源）
        ShowMoveToMenu(it.id, it.label, screen);
        break;
    case 21:  // 打开全部：整组子项各开一个**前台**标签（用户显式点菜单要的，
              // 与中键的"后台"相反 —— 中键的价值是不打断，菜单是明确要求）
        OpenFolderChildrenInTabs(it.id, /*background=*/false);
        break;
    default:
        break;
    }
}
