// 书签分类内容弹层：列出分类的**直接子项**，支持拖动排序与行内右键菜单。
//
// 【为什么分类内容要能拖动】书签栏顶层那条自绘横条早就支持拖动重排，
// 而分类里的条目只能点、不能组织 —— 同一个界面里两套规则，
// 用户第一次在分类里拖不动时会以为功能坏了。
//
// 【弹层只给"第几行 → 插到第几行之前"，业务自己折算】弹层是通用组件，
// 它不知道什么是书签。真正的移动走 bmk.move —— 与顶层拖动同一命令、
// 同一撤销栈、同一审计，于是"人能做的"与"AI 能做的"是同一件事（R20）。
//
// 【为什么行号必须与容器下标一一对应】落点坐标是**行号**，
// 而 bmk.move 的 toIndex 是**目标容器里子项的下标**。两者相等的前提是
// "弹层显示了容器的全部子项、且顺序一致" —— 所以这里**不跳过**任何子项
// （早先的实现会跳过"没有路径的叶子"，那会让行号与下标错位一位，
//  拖动就会把条目放到隔壁去）。
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

// 分类内容的一行数据：菜单行 + 与行号对齐的 id / 形态 / 路径。
struct FolderRows {
    ui::Menu menu;
    std::vector<std::wstring> ids;
    std::vector<bool> folders;
    // 能否拖到书签栏：纯分类节点 + 指向目录/命名空间的书签。
    // 不能只看 isFolder —— 用户口中的“分类里的文件夹”多数是普通书签，
    // 只是 HREF 指向一个目录；旧实现只认 H3，于是这些行始终拖不动。
    std::vector<bool> dragOutable;
    std::vector<std::wstring> paths;
};

bool IsDragOutable(const BookmarkNode& n) {
    if (n.isFolder) return true;
    if (n.path.empty()) return false;
    if (nav::IsNamespace(n.path)) return true;
    return tf::IsDirectory(n.path);
}

// 从 store 读某个分类的直接子项，组装成菜单。
//
// 顺序 = store 里的顺序（用户手工排过的顺序就是这里看到的顺序）。
FolderRows BuildFolderRows(const std::wstring& folderId) {
    FolderRows d;
    const BookmarkNode* node = BookmarkStore::Get().Find(folderId);
    if (!node || !node->isFolder) return d;
    for (const auto& n : node->children) {
        const int action = static_cast<int>(d.ids.size());
        d.menu.AddPath(n.name, n.path, action, n.isFolder);
        d.ids.push_back(n.id);
        d.folders.push_back(n.isFolder);
        d.dragOutable.push_back(IsDragOutable(n));
        d.paths.push_back(n.path);
    }
    return d;
}

}  // namespace

void BookmarkBar::ShowFolderMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ShowFolderContents(items_[static_cast<size_t>(index)].id, screen);
}

void BookmarkBar::ShowFolderContents(const std::wstring& folderId, POINT screen,
                                     const std::wstring& draggingId) {
    // 【逐级下钻，而不是把整棵子树拍平加缩进】
    //
    // 原实现把所有后代一次性列进同一个菜单，用 `depth * 2` 个空格表示层级。
    // 两个问题：
    //   · 一个有几层结构的书签集直接变成几十行的长菜单（弹出层上限 20 行，
    //     剩下的要靠滚动条翻 —— 在菜单里滚动找目录是很糟的体验）；
    //   · 两个空格的缩进在 24px 行高里几乎看不出来，"分类 A 里的 x" 和
    //     "顶层的 x" 长得一样。
    // 现在只列**直接子项**，点分类再开下一级 —— 与资源管理器/浏览器一致，
    // 每一级都是短菜单，层级靠"路径"本身表达而不是靠空格。
    //
    // 【draggingId = 拖拽移交】书签栏拖动悬停本分类到点，弹层被自动打开，
    // 那场拖拽还在进行（见 bookmark_bar_drag.cpp 的 HandoffDragToFolder）。
    // 弹层进场即在拖拽态（dragFrom=-1 表示外部条目），压行画插入线、
    // 松手按落点提交 —— "移到分类"从此能精确定位，而不是只能加入末尾。
    std::wstring current = folderId;
    for (int guard = 0; guard < 32; ++guard) {   // 深度上限，防数据自引用成死循环
        FolderRows data = BuildFolderRows(current);
        if (data.menu.Empty()) return;

        // 行号 ↔ id 的映射必须在重排后仍然对得上，所以拖放一提交就重建。
        std::vector<std::wstring> ids = data.ids;
        std::vector<bool> folders = data.folders;
        std::vector<bool> dragOutable = data.dragOutable;
        std::vector<std::wstring> paths = data.paths;
        int rightClicked = -1;

        ui::MenuOptions opt;
        // 锚点键带层级：下钻后的菜单是一个**新的**锚点，不能沿用上一级的 key
        // （否则"再点即收"会把下一级当成上一级的重开而直接吞掉）。
        // toggleKey 是裸指针，必须指向活到 menu.Show 返回的串 ——
        // 写成 (L"..." + current).c_str() 会把指针指向语句结束就析构的临时串
        //（-Wdangling-assignment-gsl 类问题；bookmark_bar_popup.cpp 同一处已修过一次）。
        const std::wstring folderKey = L"bmk-folder:" + current;
        opt.toggleKey = folderKey.c_str();

        // 拖动重排：落点是"插到第几行之前"，交给 bmk.move 去落到分类里的第几位。
        // toIndex 用的是**被拖行仍在列表里**的坐标 —— bmk.move 自己按
        // "移除被拖行"折算（to > from 时减一），两处都折算就会差一位。
        // from = -1 是外部移交的条目（draggingId），它不在本容器里，无需折算。
        opt.reorder.onReorder = [&](int from, int to) {
            std::wstring dragged;
            if (from >= 0 && from < static_cast<int>(ids.size()))
                dragged = ids[static_cast<size_t>(from)];
            else if (!draggingId.empty())
                dragged = draggingId;
            if (dragged.empty()) return;
            if (to < 0 || to > static_cast<int>(ids.size())) return;
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(dragged));
            a.Set(L"toId", json::Value::MakeStr(current));
            a.Set(L"toIndex", json::Value::MakeInt(to));
            CommandBus::Get().Invoke(CmdId::BmkMove, Args::FromJson(a), Actor::Ui());
        };
        // 落点之后**就地刷新**：行序变了，弹层若还拿着旧行，用户看到的是"没动"。
        opt.reorder.rebuildRows = [&](std::vector<PopupRow>* out) -> bool {
            FolderRows fresh = BuildFolderRows(current);
            if (fresh.menu.Empty()) return false;
            ids = fresh.ids;
            folders = fresh.folders;
            dragOutable = fresh.dragOutable;
            paths = fresh.paths;
            *out = fresh.menu.rows();
            fresh.menu.FillShortcuts(out);
            return true;
        };
        // 行内右键：弹层把命中的行号交回来，宿主在自己的循环里弹菜单
        //（右键菜单本身是模态的，不能在弹层的模态循环里再弹一层）。
        opt.reorder.onRowRightClick = [&](int row) { rightClicked = row; };

        // 拖出弹层：纯分类节点、指向目录的普通书签、命名空间项都能落到书签栏；
        // 普通文件书签仍走右键“移到书签栏”（dragOutable 为 false）。
        // 外部移交（draggingId）没有行号限制：从书签栏拖进来的就是可拖的。
        // 预览直接复用 BookmarkBar 的外部拖放落点/绘制，不在这里另画一套线。
        opt.reorder.onDragOutPreview = [&](int row, POINT screen) {
            bool allowed = !draggingId.empty();   // 外部移交项默认可落
            if (row >= 0 && row < static_cast<int>(dragOutable.size()))
                allowed = dragOutable[static_cast<size_t>(row)];
            if (screen.x < 0 || screen.y < 0 || !allowed) {
                ClearDropPreview();
                return;
            }
            PreviewDrop(DropSpotAt(screen));
        };
        opt.reorder.onDragOut = [&](int row, POINT screen) {
            ClearDropPreview();
            std::wstring dragged;
            if (row >= 0 && row < static_cast<int>(ids.size())) {
                if (!dragOutable[static_cast<size_t>(row)]) return;
                dragged = ids[static_cast<size_t>(row)];
            } else if (!draggingId.empty()) {
                dragged = draggingId;
            }
            if (dragged.empty()) return;
            const BookmarkDropSpot spot = DropSpotAt(screen);
            if (!spot.valid) return;   // 松手不在书签栏上 = 取消拖出
            LogDbg(L"[bmk] 分类拖出 id=%ls spot=1 idx=%d into=%d",
                   dragged.c_str(), spot.index, spot.folderIndex);
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(dragged));
            if (!spot.folderId.empty()) {
                a.Set(L"toId", json::Value::MakeStr(spot.folderId));
            } else {
                a.Set(L"toIndex", json::Value::MakeInt(spot.index));
            }
            CommandBus::Get().Invoke(CmdId::BmkMove, Args::FromJson(a), Actor::Ui());
        };

        // 外部移交时弹层进场即在拖拽态（from=-1 = draggingId 那一条）。
        opt.reorder.allowExternalFrom = !draggingId.empty();

        const int pick = data.menu.Show(hwnd_, *theme_, screen, opt);

        if (rightClicked >= 0 && rightClicked < static_cast<int>(ids.size())) {
            // 右键是一次**独立**操作：不接着下钻，也不打开。
            ShowCategoryItemMenu(current, rightClicked, screen);
            return;
        }
        if (pick < 0 || pick >= static_cast<int>(ids.size())) return;
        if (folders[static_cast<size_t>(pick)]) {
            // 分类：下钻一级，同一个锚点位置继续弹。模态循环已退出，可以安全再进。
            current = ids[static_cast<size_t>(pick)];
            continue;
        }
        if (!paths[static_cast<size_t>(pick)].empty() && onNavigate_) {
            onNavigate_(paths[static_cast<size_t>(pick)]);
            if (refocus_) refocus_();   // 同 Navigate：打开目录后焦点必须回视图
        }
        return;
    }
}

void BookmarkBar::ShowCategoryItemMenu(const std::wstring& folderId, int row, POINT screen) {
    BookmarkStore& store = BookmarkStore::Get();
    const BookmarkNode* node = store.Find(folderId);
    if (!node || !node->isFolder) return;
    // 行号就是子项下标（弹层显示了全部子项），所以这里直接取。
    if (row < 0 || row >= static_cast<int>(node->children.size())) return;
    const BookmarkNode item = node->children[static_cast<size_t>(row)];

    ui::Menu menu;
    menu.Add(L"打开", 1, item.isFolder ? ico::IconId::Folder : ico::IconId::Page)
        .Add(L"复制路径", 2, ico::IconId::Copy, item.path.empty())
        .Separator()
        .Add(L"编辑...", 3, ico::IconId::Edit).Cmd(CmdId::BmkEdit)
        .Add(L"复制书签", 4, ico::IconId::Copy).Cmd(CmdId::BmkCopy)
        .Separator()
        // 上移 / 下移：分类内最常用的两种整理动作（拖不动时也能用键盘路径）。
        .Add(L"上移", 5, ico::IconId::ChevronUp, row <= 0).Cmd(CmdId::BmkMove)
        .Add(L"下移", 6, ico::IconId::ChevronDown,
             row + 1 >= static_cast<int>(node->children.size()))
        .Cmd(CmdId::BmkMove)
        .Add(L"移到书签栏", 7, ico::IconId::Export).Cmd(CmdId::BmkMove)
        .Separator()
        .Add(L"删除", 8, ico::IconId::Delete).Cmd(CmdId::BmkRemove);
    // 分类才能"解散"（叶子没有壳可散）。
    if (item.isFolder)
        menu.Add(L"解散分类", 9, ico::IconId::Export).Cmd(CmdId::BmkDissolve);

    const std::wstring toggleKey = L"bmk-cat-item:" + item.id;
    ui::MenuOptions opt;
    opt.toggleKey = toggleKey.c_str();
    const int action = menu.Show(hwnd_, *theme_, screen, opt);
    if (action < 0) return;

    // 移动类动作：toIndex 用"被拖行仍在列表里"的坐标（bmk.move 内部再折算）。
    //
    // 【边界必须自己挡】菜单行虽然在首/末位会置灰，但置灰只是"这一下点不动"，
    // 不等于下标合法 —— 真走到这里时 row 仍可能越界（比如重排后行号变了）。
    // 越界的 toIndex 在 store 里会被拒（而不是越界写 vector），这里先挡住，
    // 免得用户看到"点了没反应"。
    const int childCount = static_cast<int>(node->children.size());
    auto moveTo = [&](int toIndex) {
        if (toIndex < 0 || toIndex > childCount) return;
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(item.id));
        a.Set(L"toId", json::Value::MakeStr(folderId));
        a.Set(L"toIndex", json::Value::MakeInt(toIndex));
        CommandBus::Get().Invoke(CmdId::BmkMove, Args::FromJson(a), Actor::Ui());
    };

    switch (action) {
    case 1:
        if (item.isFolder) {
            ShowFolderContents(item.id, screen);
        } else if (!item.path.empty()) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"path", json::Value::MakeStr(item.path));
            if (CommandBus::Get().Invoke(CmdId::FsOpen, Args::FromJson(a), Actor::Ui()) && refocus_)
                refocus_();
        }
        break;
    case 2:
        if (!item.path.empty()) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"text", json::Value::MakeStr(item.path));
            CommandBus::Get().Invoke(CmdId::ClipboardCopy, Args::FromJson(a), Actor::Ui());
        }
        break;
    case 3: {
        std::wstring name = item.name;
        std::wstring path = item.path;
        bool confirmed;
        if (item.isFolder && item.path.empty()) {
            confirmed = PromptText(hwnd_, *theme_, L"编辑", L"名称", item.name, &name);
        } else {
            confirmed = PromptText2(hwnd_, *theme_, L"编辑", L"名称", item.name, &name, L"路径",
                                    item.path, &path);
        }
        if (!confirmed) break;
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(item.id));
        a.Set(L"name", json::Value::MakeStr(name));
        if (!item.isFolder || !path.empty()) a.Set(L"path", json::Value::MakeStr(path));
        CommandBus::Get().Invoke(CmdId::BmkEdit, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 4: {
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(item.id));
        CommandBus::Get().Invoke(CmdId::BmkCopy, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 5:
        // 上移一位：插入点 = row - 1（被拖行还在列表里时的坐标）。
        moveTo(row - 1);
        break;
    case 6:
        // 下移一位：插入点 = row + 2（越过下一项）。
        moveTo(row + 2);
        break;
    case 7: {
        // 移到书签栏（顶层末尾）：不传 toId = 顶层；不传 toIndex = 末尾。
        json::Value a = json::Value::MakeObject();
        a.Set(L"id", json::Value::MakeStr(item.id));
        CommandBus::Get().Invoke(CmdId::BmkMove, Args::FromJson(a), Actor::Ui());
        break;
    }
    case 8:
        if (MessageBoxW(hwnd_, (L"删除书签“" + item.name + L"”？").c_str(), id::kBrandShort,
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(item.id));
            CommandBus::Get().Invoke(CmdId::BmkRemove, Args::FromJson(a), Actor::Ui());
        }
        break;
    case 9:
        if (MessageBoxW(hwnd_,
                        (L"解散分类“" + item.name + L"”？\n\n"
                         L"分类里的条目会移到上一层（顶层分类则回到书签栏），分类本身消失。")
                            .c_str(),
                        id::kBrandShort, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(item.id));
            CommandBus::Get().Invoke(CmdId::BmkDissolve, Args::FromJson(a), Actor::Ui());
        }
        break;
    default:
        break;
    }
}
