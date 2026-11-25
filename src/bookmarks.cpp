// 书签树模型：查找/增删改/移动/排序/去重/撤销/统计。
// Netscape HTML 解析、序列化与磁盘读写已搬到 bookmarks_io.cpp；
// 外部导入/导出对话框在 bookmark_io.cpp。本文件不碰文件、不解释 HTML token。
#include "bookmarks.h"

#include "bookmarks_internal.h"
#include "commands.h"   // PushUndo 标 actor：读 CommandBus::fromMachine（撤销边界用）
#include "common.h"
#include "str.h"

#include <algorithm>
#include <functional>   // std::function：Merge 的递归 lambda
#include <unordered_set>

namespace {

BookmarkNode* FindIn(BookmarkList& list, const std::wstring& id) {
    for (auto& n : list) {
        if (n.id == id) return &n;
        if (BookmarkNode* r = FindIn(n.children, id)) return r;
    }
    return nullptr;
}

bool RemoveIn(BookmarkList& list, const std::wstring& id) {
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->id == id) {
            list.erase(it);
            return true;
        }
        if (RemoveIn(it->children, id)) return true;
    }
    return false;
}

bool ContainsIn(const BookmarkList& list, const std::wstring& path) {
    for (const auto& n : list) {
        if (!n.isFolder && str::IEquals(n.path, path)) return true;
        if (ContainsIn(n.children, path)) return true;
    }
    return false;
}

void FlattenIn(const BookmarkList& list, std::vector<std::pair<std::wstring, std::wstring>>& out) {
    for (const auto& n : list) {
        if (!n.isFolder && !n.path.empty()) out.emplace_back(n.name, n.path);
        FlattenIn(n.children, out);
    }
}

// 只数不分配：与 FlattenIn 同口径（只数有 path 的叶子，不数分类）。
size_t CountLeavesIn(const BookmarkList& list) {
    size_t n = 0;
    for (const auto& e : list) {
        if (!e.isFolder && !e.path.empty()) ++n;
        n += CountLeavesIn(e.children);
    }
    return n;
}

// 只数不分配：分类数（isFolder 的节点），含嵌套分类。
size_t CountFoldersIn(const BookmarkList& list) {
    size_t n = 0;
    for (const auto& e : list) {
        if (e.isFolder) ++n;
        n += CountFoldersIn(e.children);
    }
    return n;
}

// 只数不分配：节点总数（叶子 + 分类）。
size_t CountNodesIn(const BookmarkList& list) {
    size_t n = 0;
    for (const auto& e : list) {
        ++n;
        n += CountNodesIn(e.children);
    }
    return n;
}

}  // namespace

namespace bmkstore::detail {

void AssignIds(BookmarkList& list, int& seq) {
    for (auto& n : list) {
        if (n.id.empty()) {
            wchar_t buf[32];
            swprintf(buf, 32, n.isFolder ? L"f-%d" : L"b-%d", seq++);
            n.id = buf;
        }
        AssignIds(n.children, seq);
    }
}

}  // namespace bmkstore::detail

BookmarkStore& BookmarkStore::Get() {
    static BookmarkStore s;
    return s;
}

const BookmarkNode* BookmarkStore::Find(const std::wstring& id) const {
    return FindIn(const_cast<BookmarkList&>(roots_), id);
}

BookmarkNode* BookmarkStore::Find(const std::wstring& id) { return FindIn(roots_, id); }

bool BookmarkStore::ContainsPath(const std::wstring& path) const {
    return ContainsIn(roots_, path);
}

const BookmarkNode* BookmarkStore::FindByPath(const std::wstring& path) const {
    if (path.empty()) return nullptr;
    const BookmarkNode* found = nullptr;
    std::function<void(const BookmarkList&)> walk = [&](const BookmarkList& list) {
        for (const auto& n : list) {
            if (found) return;
            if (!n.isFolder && str::IEquals(n.path, path)) {
                found = &n;
                return;
            }
            walk(n.children);
        }
    };
    walk(roots_);
    return found;
}

BookmarkList* BookmarkStore::ParentList(const std::wstring& parentId) {
    if (parentId.empty()) return &roots_;
    BookmarkNode* n = Find(parentId);
    if (!n || !n->isFolder) return nullptr;
    return &n->children;
}

std::wstring BookmarkStore::NewId(bool folder) {
    // 冲突检测不能只看"形态"：用户手工编辑 bookmarks.html、或从 mod 导入的
    // 文件里可能已经有 f-3 / b-9，而 seq_ 是从 1 重新数起的。撞 id 的后果不是
    // "多一条重复"，而是 Find() 命中前一条、删除/改名作用到别人身上。
    for (int guard = 0; guard < 10000; ++guard) {
        wchar_t buf[32];
        swprintf(buf, 32, folder ? L"f-%d" : L"b-%d", seq_++);
        if (!Find(buf)) return buf;
    }
    // 兜底：给一个几乎不可能撞的 id，而不是返回空串。
    // 返回空串会让 AddPath/AddFolder 交回空 id，调用方（拖放、导入）把它当成
    // "添加失败"却没有任何提示 —— 静默丢一条用户操作。
    wchar_t buf[64];
    swprintf(buf, 64, folder ? L"f-x%llx" : L"b-x%llx",
             static_cast<unsigned long long>(GetTickCount64()));
    return buf;
}

std::wstring BookmarkStore::AddPath(const std::wstring& name, const std::wstring& path,
                                    const std::wstring& parentId, int index) {
    BookmarkList* list = ParentList(parentId);
    if (!list || path.empty()) return std::wstring();
    // 【必须先压撤销快照，再改树】bmk.add.path / bmk.add.current / bmk.add.selection /
    // bmk.add.tabs / 拖放 全走这里，而它们全部声明 undoable=1。
    // 2026-11-23 实测：这一族**一条都撤不掉** —— PushUndo 只写在 Remove/Copy/MoveTo/
    // Sort/Dedup/ReplaceAll/Merge 里，新增路径与新增分类两条路漏了。
    // 声明了 undoable 却没有栈，等于对调用方（尤其 AI）说谎：它据此敢试错，
    // 试完发现退不回来。快照必须在修改**之前**拍（PushUndo 存的是当前 roots_）。
    PushUndo();
    BookmarkNode n;
    n.id = NewId(false);
    n.name = name.empty() ? path : name;
    n.path = path;
    n.icon = L"shell:folder";
    // 拖入书签栏要尊重插入线，不能永远 push_back。越界按"追加"处理：
    // 拖放期间书签可能被别处改过，越界时静默丢一条比落到末尾更糟。
    auto at = list->end();
    if (index >= 0 && index <= static_cast<int>(list->size()))
        at = list->begin() + index;
    at = list->insert(at, std::move(n));
    Touch();
    return at->id;
}

std::wstring BookmarkStore::AddFolder(const std::wstring& name, const std::wstring& parentId) {
    BookmarkList* list = ParentList(parentId);
    if (!list) return std::wstring();
    // 同上：bmk.newfolder 声明 undoable=1，新增分类也必须可撤销。
    // 与 AddPath 的差别只在 isFolder；两条路各写一次 PushUndo 而不是抽公共函数，
    // 是因为 NewId/ParentList 的早退分支不同（一个查 path 空、一个查 list 空），
    // 抽出来反而要把"哪一步失败不占撤销额度"的规则再讲一遍。
    PushUndo();
    BookmarkNode n;
    n.id = NewId(true);
    n.name = name.empty() ? L"新建分类" : name;
    n.isFolder = true;
    list->push_back(std::move(n));
    Touch();
    return list->back().id;
}

namespace {

// 子树全部清 id（AssignIds 只给空 id 发新号，不清就会与原件撞 id）。
void ClearIds(BookmarkList& list) {
    for (auto& n : list) {
        n.id.clear();
        ClearIds(n.children);
    }
}

}  // namespace

std::wstring BookmarkStore::Copy(const std::wstring& id) {
    size_t idx = 0;
    BookmarkList* list = bmkstore::detail::ListContaining(roots_, id, &idx);
    if (!list || idx >= list->size()) return std::wstring();
    PushUndo();
    const BookmarkNode src = (*list)[idx];   // 拷贝一份再插入，insert 后引用会失效
    BookmarkNode n = src;
    n.id.clear();
    ClearIds(n.children);
    n.name = src.name + L" 副本";
    bmkstore::detail::AssignIds(n.children, seq_);
    n.id = NewId(n.isFolder);
    list->insert(list->begin() + static_cast<long>(idx) + 1, std::move(n));
    Touch();
    return (*list)[idx + 1].id;
}

bool BookmarkStore::Remove(const std::wstring& id) {
    if (id.empty()) return false;
    PushUndo();
    bool ok = RemoveIn(roots_, id);
    if (ok)
        Touch();
    else
        undo_.pop_back();  // 没删成不占撤销额度
    return ok;
}

bool BookmarkStore::Rename(const std::wstring& id, const std::wstring& name) {
    BookmarkNode* n = Find(id);
    if (!n || name.empty()) return false;
    n->name = name;
    Touch();
    return true;
}

bool BookmarkStore::Update(const std::wstring& id, const std::wstring& name,
                           const std::wstring& path) {
    BookmarkNode* n = Find(id);
    if (!n) return false;
    if (!name.empty()) n->name = name;
    if (!path.empty()) n->path = path;
    Touch();
    return true;
}

bool BookmarkStore::SetNameAndPath(const std::wstring& id, const std::wstring& name,
                                   const std::wstring& path) {
    BookmarkNode* n = Find(id);
    if (!n) return false;
    if (!name.empty()) n->name = name;
    n->path = path;   // 空串 = 用户清空了路径，真的清
    Touch();
    return true;
}

void BookmarkStore::ReplaceAll(BookmarkList&& replacement) {
    PushUndo();  // 清空/覆盖导入也走撤销栈，与删除/移动同口径
    roots_ = std::move(replacement);
    seq_ = 1;
    bmkstore::detail::AssignIds(roots_, seq_);
    Touch();
}

size_t BookmarkStore::Merge(BookmarkList&& incoming) {
    // 【递归合并，不是"整棵子树塞进顶层"】
    //
    // 原实现只对**顶层叶子**做路径去重；遇到文件夹就 `AssignIds(children)` 后
    // 整棵子树无条件 push_back。后果：同一个含分类的导入文件连导两次，
    // 分类里的书签变成 1 -> 2 -> 4 份（每导一次翻一倍），且随 Save 落盘，
    // 只能逐条手删。
    //
    // 现在的语义：文件夹按**名字**对齐（同名视为同一个分类），对齐后递归合并
    // 其子项；叶子按**路径**去重（与 mod 追加导入同口径），去重集合覆盖整棵树
    // 而不只是新分类的直接子项。返回"实际新增的条目数"。
    size_t added = 0;

    std::unordered_set<std::wstring> seenPaths;
    std::unordered_set<std::wstring> usedIds;
    std::function<void(const BookmarkList&)> collect = [&](const BookmarkList& list) {
        for (const auto& n : list) {
            usedIds.insert(n.id);
            if (!n.isFolder && !n.path.empty()) seenPaths.insert(str::Lower(n.path));
            collect(n.children);
        }
    };
    collect(roots_);

    // Import 不读 EXBAR_ID，但调用方仍可能给带 id 的节点；撞 id 必须换新，
    // 否则 Find() 命中旧节点，改名/删除会作用到别人身上。
    auto freshId = [&](bool folder) {
        wchar_t buf[64];
        do {
            swprintf(buf, _countof(buf), folder ? L"f-%d" : L"b-%d", seq_++);
        } while (usedIds.count(buf));
        usedIds.insert(buf);
        return std::wstring(buf);
    };
    auto normalizeIds = [&](auto&& self, BookmarkList& list) -> void {
        for (auto& n : list) {
            if (n.id.empty() || usedIds.count(n.id)) n.id = freshId(n.isFolder);
            else usedIds.insert(n.id);
            self(self, n.children);
        }
    };
    auto claimPath = [&](const std::wstring& path) {
        if (path.empty()) return true;  // 空路径是"未定位"的书签，不去重
        return seenPaths.insert(str::Lower(path)).second;
    };
    auto pruneDuplicates = [&](auto&& self, BookmarkList& list) -> void {
        BookmarkList kept;
        for (auto& n : list) {
            if (n.isFolder) {
                self(self, n.children);
                kept.push_back(std::move(n));
            } else if (claimPath(n.path)) {
                kept.push_back(std::move(n));
            }
        }
        list = std::move(kept);
    };
    auto countNodes = [](auto&& self, const BookmarkList& list) -> size_t {
        size_t n = 0;
        for (const auto& x : list) {
            ++n;
            n += self(self, x.children);
        }
        return n;
    };

    std::function<void(BookmarkList&, BookmarkList&)> mergeInto =
        [&](BookmarkList& dest, BookmarkList& src) {
            for (auto& node : src) {
                if (!node.isFolder) {
                    if (!claimPath(node.path)) continue;
                    node.id = (node.id.empty() || usedIds.count(node.id)) ? freshId(false) : node.id;
                    usedIds.insert(node.id);
                    dest.push_back(std::move(node));
                    ++added;
                    continue;
                }

                // 文件夹：按名字找同层已有的同名分类，找到就合并进去。
                auto it = std::find_if(dest.begin(), dest.end(), [&](const BookmarkNode& d) {
                    return d.isFolder && str::IEquals(d.name, node.name);
                });
                if (it != dest.end()) {
                    mergeInto(it->children, node.children);
                    continue;
                }

                // 新分类：整棵子树进来，但要先剔掉全树中已存在的叶子（任意深度）。
                pruneDuplicates(pruneDuplicates, node.children);
                node.id = (node.id.empty() || usedIds.count(node.id)) ? freshId(true) : node.id;
                usedIds.insert(node.id);
                normalizeIds(normalizeIds, node.children);
                const size_t subtreeCount = 1 + countNodes(countNodes, node.children);
                dest.push_back(std::move(node));
                added += subtreeCount;
            }
        };
    // 【快照必须在合并之前拍】原实现是在 mergeInto 之后 push：
    // 那份快照存的已经是**合并后**的树，撤销它等于"撤到一个和现在一样的树" ——
    // 界面会给"已撤销"的成功回执，用户的导入却一条没退。这是最坏的一类假成功。
    //
    // 先拍、后判：一条都没加进去时把快照弹掉，不占 20 步额度、也不让
    // "撤销删除"凭空亮起来（与 Remove 失败时 pop_back 同口径）。
    //
    // 代价说清楚：栈已满 20 时 PushUndo 会淘汰最老一条，此时 pop_back 只弹掉刚拍的
    // 那份，被淘汰的那条回不来。权衡是有意的 —— "撤销按钮凭空可用"比"少一条历史"
    // 更坏，错的东西会让人按下去，缺一条只是退不回去（与交互式改名那条一致）。
    PushUndo();
    mergeInto(roots_, incoming);
    if (added) {
        Touch();
    } else {
        undo_.pop_back();   // 无事发生：不占撤销额度
    }
    return added;
}

bool BookmarkStore::Move(const std::wstring& id, int toIndex) {
    // 插入线语义：合法区间 [0, size]（size = 插到末项之后）。
    if (toIndex < 0 || toIndex > static_cast<int>(roots_.size())) return false;
    for (auto it = roots_.begin(); it != roots_.end(); ++it) {
        if (it->id != id) continue;
        int from = static_cast<int>(it - roots_.begin());
        // from < to 时 erase 后目标前移一位；adj == from 说明插入线贴着自身，无真实移动。
        int adj = toIndex > from ? toIndex - 1 : toIndex;
        if (adj == from) return false;  // 不产生撤销快照、不标脏
        PushUndo();
        BookmarkNode n = std::move(*it);
        roots_.erase(it);
        roots_.insert(roots_.begin() + adj, std::move(n));
        Touch();
        return true;
    }
    return false;
}

namespace {

// id 是否落在以 root 为根的子树里（含自身）。
bool InSubtree(const BookmarkNode& root, const std::wstring& id) {
    if (root.id == id) return true;
    for (const auto& c : root.children)
        if (InSubtree(c, id)) return true;
    return false;
}

}  // namespace

bool BookmarkStore::MoveTo(const std::wstring& id, const std::wstring& targetFolderId,
                           int index) {
    if (id.empty()) return false;
    if (id == targetFolderId) return false;   // 拖到自己身上

    BookmarkList* dest = ParentList(targetFolderId);
    if (!dest) return false;                  // 目标不存在 / 不是文件夹

    // 目标是自己子孙 -> 拒绝。不拦的话 erase 会把整棵子树摘下，再插回自己的
    // 子节点列表 —— 结果那棵子树从顶层消失，且用户没有明显线索能把它找回。
    if (!targetFolderId.empty()) {
        const BookmarkNode* target = Find(targetFolderId);
        const BookmarkNode* self = Find(id);
        if (!target || !self) return false;
        if (InSubtree(*self, targetFolderId)) return false;
    }

    // 找到源容器与位置。顶层与任意子层都要支持，所以递归找。
    BookmarkList* src = nullptr;
    size_t pos = 0;
    std::function<bool(BookmarkList&)> locate = [&](BookmarkList& list) -> bool {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].id == id) {
                src = &list;
                pos = i;
                return true;
            }
            if (locate(list[i].children)) return true;
        }
        return false;
    };
    if (!locate(roots_)) return false;
    if (src == dest) {
        // 同容器内移动：交给 Move（只在顶层有意义），否则按插入线手算。
        if (src == &roots_) return Move(id, index < 0 ? static_cast<int>(roots_.size()) : index);
        const int from = static_cast<int>(pos);
        // 插入线语义：合法区间 [0, size]（size = 插到末项之后）。
        //
        // 【越界必须在这里拦】下面的 insert() 不会自己检查下标 ——
        // 传一个超出末位的值就是往 vector 之外写（未定义行为，实测会崩）。
        // 顶层那条路走 Move()，它有同样的检查；这条嵌套的路以前没有，
        // 于是"分类内移动"只要 toIndex 偏大就越界。命令侧不做夹取
        // （夹取会把"移到末尾"与"位置非法"混成同一个结果），所以在这里拒。
        if (index > static_cast<int>(dest->size())) return false;
        int to = (index < 0) ? static_cast<int>(dest->size()) : index;
        if (to > from) --to;
        if (to == from) return false;
        PushUndo();
        BookmarkNode n = std::move((*src)[pos]);
        src->erase(src->begin() + static_cast<long long>(pos));
        dest->insert(dest->begin() + to, std::move(n));
        Touch();
        return true;
    }

    PushUndo();
    BookmarkNode n = std::move((*src)[pos]);
    src->erase(src->begin() + static_cast<long long>(pos));

    // 【必须重新取 dest】上面这次 erase 来自 roots_ 或某节点的 children；
    // 若来自 roots_（vector<BookmarkNode>），后面的元素整体前移，`dest` 这个
    // 指向"某个 BookmarkNode 的 children 成员"的指针会随之失效 ——
    // 往失效的 vector 里 push_back 就是访问违例（实测崩溃）。
    // 两次 ParentList 之间没有任何其它修改，所以重新取是安全且必要的。
    dest = ParentList(targetFolderId);
    if (!dest) {   // 理论上不可能（上面刚校验过）—— 但绝不能拿空指针 push_back
        Touch();
        return false;
    }
    if (index < 0 || index > static_cast<int>(dest->size()))
        dest->push_back(std::move(n));
    else
        dest->insert(dest->begin() + index, std::move(n));
    Touch();
    return true;
}

namespace {

void SortLevel(BookmarkList& list, bool foldersFirst, bool ascending, bool recursive) {
    std::sort(list.begin(), list.end(), [&](const BookmarkNode& a, const BookmarkNode& b) {
        return bmkstore::detail::BookmarkLess(a, b, foldersFirst, ascending);
    });
    if (!recursive) return;
    for (auto& n : list) SortLevel(n.children, foldersFirst, ascending, true);
}

// 跨层级去重（先序保留第一个）。返回删除条数。
size_t DedupLevel(BookmarkList& list, std::vector<std::wstring>& seen) {
    size_t removed = 0;
    for (auto it = list.begin(); it != list.end();) {
        if (!it->isFolder && !it->path.empty()) {
            bool dup = false;
            for (const auto& s : seen)
                if (str::IEquals(s, it->path)) {
                    dup = true;
                    break;
                }
            if (dup) {
                it = list.erase(it);
                ++removed;
                continue;
            }
            seen.push_back(it->path);
        }
        ++it;
    }
    for (auto& n : list) removed += DedupLevel(n.children, seen);
    return removed;
}

bool HasDuplicatePaths(const BookmarkList& list, std::vector<std::wstring>& seen) {
    for (const auto& n : list) {
        if (!n.isFolder && !n.path.empty()) {
            for (const auto& s : seen)
                if (str::IEquals(s, n.path)) return true;
            seen.push_back(n.path);
        }
        if (HasDuplicatePaths(n.children, seen)) return true;
    }
    return false;
}

}  // namespace

void BookmarkStore::SortRecursive(bool foldersFirst, bool ascending, bool recursive) {
    PushUndo();   // 排序改变用户手工顺序，必须可撤销
    SortLevel(roots_, foldersFirst, ascending, recursive);
    Touch();
}

size_t BookmarkStore::RemoveDuplicatePaths() {
    // 去重键：路径大小写不敏感（Windows 语义），且**跨层级**去重 ——
    // 同一个目录既在顶层又在分类里，本质仍是重复（XYplorer 的 "Remove duplicates"
    // 也是这个口径）。保留先序遍历里第一个出现的。
    //
    // 先探一遍再动手：没有重复时不该留下一条空的撤销记录（用户点"清理重复"
    // 却看到"撤销删除"亮了，会以为删掉了东西）。
    std::vector<std::wstring> probe;
    if (!HasDuplicatePaths(roots_, probe)) return 0;

    PushUndo();   // 必须在修改之前拍快照
    std::vector<std::wstring> seen;
    const size_t removed = DedupLevel(roots_, seen);
    if (removed) Touch();
    return removed;
}

void BookmarkStore::PushUndo() {
    if (undo_.size() >= 20) undo_.erase(undo_.begin());
    undo_.push_back({roots_, CommandBus::Get().fromMachine()});
}

bool BookmarkStore::TopUndoFromMachine() const {
    return !undo_.empty() && undo_.back().byMachine;
}

bool BookmarkStore::Undo() {
    if (undo_.empty()) return false;
    roots_ = std::move(undo_.back().roots);
    undo_.pop_back();
    Touch();
    return true;
}

std::vector<std::pair<std::wstring, std::wstring>> BookmarkStore::Flatten() const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    FlattenIn(roots_, out);
    return out;
}

size_t BookmarkStore::CountLeaves() const { return CountLeavesIn(roots_); }

size_t BookmarkStore::CountFolders() const { return CountFoldersIn(roots_); }

size_t BookmarkStore::CountNodes() const { return CountNodesIn(roots_); }
