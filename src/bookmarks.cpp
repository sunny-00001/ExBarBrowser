#include "bookmarks.h"

#include "bmk_html.h"

#include "commands.h"   // PushUndo 标 actor：读 CommandBus::fromMachine（撤销边界用）
#include "common.h"
#include "str.h"
#include "config.h"
#include "text_file.h"

#include <algorithm>
#include <functional>   // std::function：Merge 的递归 lambda
#include <cwctype>
#include <unordered_set>

namespace {

// Trim / Lower 走唯一实现（str.cpp）：以前 bookmarks / bookmark_io / chord
// 各有一份同样的实现，口径漂移过一次（iswspace vs iswblank）。
using str::Lower;
using str::Trim;

// 词法层（Escape / Unescape / Attr / Tokenize / TagName / IsClosing）已抽到
// bmk_html.h —— 这里曾与 bookmark_io.cpp 各有一份逐字相同的副本。
using bmkhtml::Attr;
using bmkhtml::IsClosing;
using bmkhtml::TagName;
using bmkhtml::Token;
using bmkhtml::Tokenize;
using bmkhtml::Escape;
using bmkhtml::Unescape;


// 解析一层 DL；pos 指向 <DL> 之后的 token 下标
void ParseLevel(const std::vector<Token>& toks, size_t& pos, BookmarkList& out, int depth) {
    if (depth > 32) return;
    while (pos < toks.size()) {
        const Token& t = toks[pos];
        if (!t.tag) {
            ++pos;
            continue;
        }
        const std::wstring name = TagName(t.text);
        if (name == L"dl" && IsClosing(t.text)) {
            ++pos;
            return;
        }
        if (name != L"dt") {
            ++pos;
            continue;
        }
        ++pos;
        // 找到 DT 后面的第一个内容标签
        while (pos < toks.size() && !toks[pos].tag) ++pos;
        if (pos >= toks.size()) return;
        const std::wstring inner = toks[pos].text;
        const std::wstring iname = TagName(inner);
        if (iname == L"h3") {
            BookmarkNode node;
            node.isFolder = true;
            node.id = Attr(inner, L"EXBAR_ID");
            node.flags = _wtoi(Attr(inner, L"EXBAR_FLAGS").c_str());
            // H3 文本：标签内没有名字时取后续文本 token
            if (node.name.empty() && pos + 1 < toks.size() && !toks[pos + 1].tag)
                node.name = Unescape(Trim(toks[pos + 1].text));
            ++pos;
            while (pos < toks.size() && !(toks[pos].tag && TagName(toks[pos].text) == L"h3" &&
                                          IsClosing(toks[pos].text)))
                ++pos;
            ++pos;
            // 紧随其后的 DL
            while (pos < toks.size() &&
                   !(toks[pos].tag && TagName(toks[pos].text) == L"dl" && !IsClosing(toks[pos].text)))
                ++pos;
            if (pos < toks.size()) {
                ++pos;
                ParseLevel(toks, pos, node.children, depth + 1);
            }
            out.push_back(std::move(node));
        } else if (iname == L"a") {
            BookmarkNode node;
            // HREF 属性值是 HTML 转义后的（& → &amp;），必须先还原实体再解 URL，
            // 否则含 & 的路径每存一次就多一层 &amp;，往返累积损坏。
            node.path = BookmarkUrlToPath(Unescape(Attr(inner, L"HREF")));
            node.id = Attr(inner, L"EXBAR_ID");
            node.icon = Attr(inner, L"EXBAR_ICON");
            node.flags = _wtoi(Attr(inner, L"EXBAR_FLAGS").c_str());
            if (node.name.empty() && pos + 1 < toks.size() && !toks[pos + 1].tag)
                node.name = Unescape(Trim(toks[pos + 1].text));
            if (!node.path.empty()) out.push_back(std::move(node));
            ++pos;
            while (pos < toks.size() &&
                   !(toks[pos].tag && TagName(toks[pos].text) == L"a" && IsClosing(toks[pos].text)))
                ++pos;
            ++pos;
        } else {
            ++pos;
        }
    }
}

std::wstring Serialize(const BookmarkList& list, int indent) {
    std::wstring pad(static_cast<size_t>(indent * 4), L' ');
    std::wstring out;
    for (const auto& n : list) {
        if (n.isFolder) {
            out += pad + L"<DT><H3 EXBAR_ID=\"" + Escape(n.id) + L"\" EXBAR_FLAGS=\"" +
                   std::to_wstring(n.flags) + L"\">" + Escape(n.name) + L"</H3>\r\n";
            out += pad + L"<DL><p>\r\n";
            out += Serialize(n.children, indent + 1);
            out += pad + L"</DL><p>\r\n";
        } else if (!n.path.empty()) {
            out += pad + L"<DT><A HREF=\"" + Escape(BookmarkPathToUrl(n.path)) + L"\" EXBAR_ID=\"" +
                   Escape(n.id) + L"\"";
            if (!n.icon.empty()) out += L" EXBAR_ICON=\"" + Escape(n.icon) + L"\"";
            out += L" EXBAR_FLAGS=\"" + std::to_wstring(n.flags) + L"\">" + Escape(n.name) +
                   L"</A>\r\n";
        }
    }
    return out;
}

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

}  // namespace

std::wstring BookmarkPathToUrl(const std::wstring& path) { return bmkhtml::PathToUrl(path); }

std::wstring BookmarkUrlToPath(const std::wstring& url) { return bmkhtml::UrlToPath(url); }

std::wstring BookmarkStore::SerializeDocument() const {
    std::wstring html =
        L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n"
        L"<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\r\n"
        L"<TITLE>Bookmarks</TITLE>\r\n<H1>Bookmarks</H1>\r\n<DL><p>\r\n";
    html += Serialize(roots_, 1);
    html += L"</DL><p>\r\n";
    return html;
}

BookmarkStore& BookmarkStore::Get() {
    static BookmarkStore s;
    return s;
}

bool BookmarkStore::Load(const std::wstring& path) {
    // 【顺序很关键】先把内容读进内存树，成功后才替换现有状态。
    //
    // 以前是"一进函数就 roots_.clear() + undo_.clear()，然后任何一步失败就 return false"。
    // 调用方都没接返回值，于是"文件存在但读不懂"被无声地当成"没有书签"：书签栏空白 →
    // 用户随手加一个书签 → Save() 用只有表头的 HTML 覆盖原文件 → **原有书签全部永久
    // 丢失，且没有备份、Undo 栈也已被清空**。这是全模块唯一的数据丢失链。
    std::wstring html;
    const bool readOk = tf::ReadText(path, &html);

    if (!readOk) {
        // 读不到：可能是首次运行（文件不存在，正常），也可能是权限/占用（异常）。
        // 我们无法区分这两者，但**都不能清空内存树** —— 若文件其实存在只是读失败，
        // 清空就等于替用户删了书签。文件不存在时内存树本来就是空的，不清也无损。
        loadFailed_ = tf::Exists(path);
        // 【基线必须一起清掉】否则会留下上一次成功 Load 的时间戳：
        // 文件被删（首次运行/用户清掉）后再 Save，FileChanged() 看到"文件不见了"
        // 会保守判为"有变化"，于是**永远无法写出第一份书签**。
        // 实测就是这个失败：guard: first run can still save。
        fileStamp_ = FILETIME{};
        fileStampSize_ = 0;
        LogDbg(L"[bmk] 读取失败（%s）：%s", loadFailed_ ? L"文件存在但读不出" : L"文件不存在",
               path.c_str());
        return false;
    }

    size_t dl = Lower(html).find(L"<dl");
    if (dl == std::wstring::npos) {
        // 文件读到了但不像 Netscape 书签（空文件 / 格式不符 / 编码不对）。
        // 这是最危险的一种：文件确实存在，用户很可能有真实数据在里面。
        loadFailed_ = true;
        LogDbg(L"[bmk] 内容不是 Netscape 书签格式（%u 字节），已拒绝加载并禁止覆盖写",
               static_cast<unsigned>(html.size()));
        return false;
    }

    // 解析到临时树，成功后再整体换入 —— 中途失败不会留下半棵树。
    BookmarkList parsed;
    int parsedSeq = 1;
    std::vector<Token> toks = Tokenize(html);
    size_t pos = 0;
    while (pos < toks.size() &&
           !(toks[pos].tag && TagName(toks[pos].text) == L"dl" && !IsClosing(toks[pos].text)))
        ++pos;
    if (pos < toks.size()) ++pos;
    ParseLevel(toks, pos, parsed, 0);
    AssignIds(parsed, parsedSeq);

    file_ = path;
    roots_ = std::move(parsed);
    seq_ = parsedSeq;
    dirty_ = false;
    undo_.clear();
    loadFailed_ = false;
    fileStamp_ = LastWriteOf(path);  // 记录基线，供外部变更检测
    fileStampSize_ = SizeOf(path);
    return true;
}

bool BookmarkStore::Save(bool overwriteExternal) {
    if (file_.empty()) file_ = Config::Get().BookmarksPath();

    // 每条返回路径都要给回执；调用方拿不到失败原因就只能静默丢数据。
    bool ok = false;
    auto notify = [&] {
        if (saveNotify_) saveNotify_(ok);
        return ok;
    };

    // 【护栏】加载失败过就别写盘。
    //
    // 这是 Load 那条数据丢失链的第二半：Load 失败后内存树可能是空的/不完整的，
    // 此时任何一次 Save（用户加个书签、关窗口时的自动保存）都会把用户的文件
    // 覆盖成"只有表头的空书签"。宁可不保存，也不能销毁用户数据。
    //
    // 用户可以恢复：改好那个文件（或删掉它）后重启，Load 成功即自动解除。
    if (loadFailed_) {
        LogDbg(L"[bmk] 拒绝写盘：上次加载失败，覆盖会销毁用户书签（%s）", file_.c_str());
        return notify();
    }

    // 【护栏二】文件被外部改过了就别整棵覆盖。
    //
    // 与 loadFailed_ 是两条不同的数据丢失链：
    //   · loadFailed_  = 我们没读懂那个文件（内存树不可信）；
    //   · 这一条        = 我们读懂了，但**之后**别人改了它（内存树过期了）。
    // 第二种以前完全没有守卫：本程序开着，用户在编辑器里改了 bookmarks.html
    // （或另一个实例/mod 写了它），此后本程序加一个书签就把外部修改整份冲掉。
    //
    // 默认不覆盖，但**不等于拒绝到底**：把内存树另存 `<file>.conflict.html`，
    // 两边数据都保住，再由 UI 询问用户想保留哪一份（覆盖走 Save(true)）。
    const bool haveBaseline = fileStamp_.dwHighDateTime || fileStamp_.dwLowDateTime;
    if (!overwriteExternal && haveBaseline && FileChanged()) {
        const std::wstring conflict = file_ + L".conflict.html";
        if (tf::WriteText(conflict, SerializeDocument())) {
            conflictSaved_ = true;
            LogDbg(L"[bmk] 文件已被外部修改，拒绝覆盖；内存书签已另存 %s", conflict.c_str());
        } else {
            LogDbg(L"[bmk] 文件已被外部修改，且另存失败（%s）", conflict.c_str());
        }
        return notify();
    }

    std::wstring html = SerializeDocument();
    ok = tf::WriteText(file_, html);
    if (ok) {
        dirty_ = false;
        fileStamp_ = LastWriteOf(file_);  // 原子替换后取新时间戳，别把自家写入当外部变更
        fileStampSize_ = SizeOf(file_);
    }
    return notify();
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
                                    const std::wstring& parentId) {
    BookmarkList* list = ParentList(parentId);
    if (!list || path.empty()) return std::wstring();
    BookmarkNode n;
    n.id = NewId(false);
    n.name = name.empty() ? path : name;
    n.path = path;
    n.icon = L"shell:folder";
    list->push_back(std::move(n));
    Touch();
    return list->back().id;
}

std::wstring BookmarkStore::AddFolder(const std::wstring& name, const std::wstring& parentId) {
    BookmarkList* list = ParentList(parentId);
    if (!list) return std::wstring();
    BookmarkNode n;
    n.id = NewId(true);
    n.name = name.empty() ? L"新建分类" : name;
    n.isFolder = true;
    list->push_back(std::move(n));
    Touch();
    return list->back().id;
}

namespace {

// 找到 id 所在的（父）列表与下标；找不到返回 nullptr。用于"插到原条目后面"。
BookmarkList* ListContaining(BookmarkList& list, const std::wstring& id, size_t* idx) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == id) { *idx = i; return &list; }
        if (BookmarkList* sub = ListContaining(list[i].children, id, idx)) return sub;
    }
    return nullptr;
}

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
    BookmarkList* list = ListContaining(roots_, id, &idx);
    if (!list || idx >= list->size()) return std::wstring();
    PushUndo();
    const BookmarkNode src = (*list)[idx];   // 拷贝一份再插入，insert 后引用会失效
    BookmarkNode n = src;
    n.id.clear();
    ClearIds(n.children);
    n.name = src.name + L" 副本";
    AssignIds(n.children, seq_);
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
    AssignIds(roots_, seq_);
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

    mergeInto(roots_, incoming);
    if (added) Touch();
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
        if (foldersFirst && a.isFolder != b.isFolder) return a.isFolder;
        const int c = str::ICmp(a.name, b.name);
        return ascending ? c < 0 : c > 0;
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

// shell 命名空间路径的纯格式校验：::{8-4-4-4-12}，共 40 字符。
// 只查形态（确定性、可单测）；"形态合法但 GUID 不存在"只能靠 shell 解析，
// 那是点击导航的事。文件系统路径（不以 ::{ 开头）不归这条规则管。
bool IsValidShellNamespacePath(const std::wstring& path) {
    if (path.compare(0, 3, L"::{") != 0) return true;
    if (path.size() != 40 || path.back() != L'}') return false;
    static constexpr int kSegLens[] = {8, 4, 4, 4, 12};
    size_t pos = 3;
    for (int i = 0; i < 5; ++i) {
        for (int k = 0; k < kSegLens[i]; ++k, ++pos)
            if (!std::iswxdigit(path[pos])) return false;
        if (i < 4) {
            if (path[pos] != L'-') return false;
            ++pos;
        }
    }
    return pos == 39;
}

FILETIME BookmarkStore::LastWriteOf(const std::wstring& path) const {
    FILETIME ft{};
    // ToExtended：书签文件可能放在深目录（用户自定义数据位置）。
    HANDLE h = CreateFileW(tf::ToExtended(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ft;
    FILETIME written{};
    if (GetFileTime(h, nullptr, nullptr, &written)) ft = written;
    CloseHandle(h);
    return ft;
}

unsigned long long BookmarkStore::SizeOf(const std::wstring& path) const {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(tf::ToExtended(path).c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

bool BookmarkStore::FileChanged() const {
    if (file_.empty()) return false;
    FILETIME cur = LastWriteOf(file_);

    // 【打不开 = 视野不明 = 当作已变更】
    //
    // 原实现是 `if (cur 是零) return false;` —— 即"读不到就算没变"。
    // 而"读不到"最常见的成因恰恰是**另一个进程正独占着这个文件**
    // （编辑器保存中、同步工具写入中），那正是最该拒绝覆盖的时刻。
    // 守卫在最需要的场景放行，等于没有守卫。
    if (cur.dwHighDateTime == 0 && cur.dwLowDateTime == 0) return true;

    // 基线未知（尚未 Load 成功过）：保守认为"有变化"。
    if (fileStamp_.dwHighDateTime == 0 && fileStamp_.dwLowDateTime == 0) return true;

    if (CompareFileTime(&cur, &fileStamp_) != 0) return true;

    // mtime 相同不代表内容相同：FAT/网络共享的时间戳粒度可达 2 秒，
    // CopyFile 会保留时间戳，SetFileTime 可以回拨。同一窗口内改两次、
    // 或工具"保留时间戳"复制覆盖，都只有大小会变。
    //
    // 代价：每次检查多一次 stat。FileChanged 只在导入/保存前的守卫里调用，
    // 不是热路径。
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(tf::ToExtended(file_).c_str(), GetFileExInfoStandard, &fad))
        return true;   // 同上的理由：看不到就算有变
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return size != fileStampSize_;
}

std::vector<std::pair<std::wstring, std::wstring>> BookmarkStore::Flatten() const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    FlattenIn(roots_, out);
    return out;
}
