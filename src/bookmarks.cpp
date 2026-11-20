#include "bookmarks.h"

#include "common.h"
#include "config.h"
#include "text_file.h"

#include <algorithm>
#include <cwctype>

namespace {

// Trim / Lower 走唯一实现（str.cpp）：以前 bookmarks / bookmark_io / chord
// 各有一份同样的实现，口径漂移过一次（iswspace vs iswblank）。
using str::Lower;
using str::Trim;

std::wstring HtmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

std::wstring HtmlUnescape(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') {
            out.push_back(s[i]);
            continue;
        }
        struct Ent {
            const wchar_t* name;
            wchar_t ch;
        };
        static const Ent kEnts[] = {{L"&amp;", L'&'}, {L"&lt;", L'<'}, {L"&gt;", L'>'},
                                    {L"&quot;", L'"'}, {L"&apos;", L'\''}, {L"&nbsp;", L' '}};
        bool done = false;
        for (const auto& e : kEnts) {
            size_t n = wcslen(e.name);
            if (s.compare(i, n, e.name) == 0) {
                out.push_back(e.ch);
                i += n - 1;
                done = true;
                break;
            }
        }
        if (!done) out.push_back(s[i]);
    }
    return out;
}

std::wstring Attr(const std::wstring& tag, const wchar_t* key) {
    std::wstring lower = Lower(tag);
    std::wstring k = Lower(key);
    size_t p = 0;
    while ((p = lower.find(k, p)) != std::wstring::npos) {
        bool leftOk = p == 0 || !iswalnum(lower[p - 1]);
        size_t after = p + k.size();
        if (!leftOk || after >= lower.size() || lower[after] != L'=') {
            p = after;
            continue;
        }
        size_t v = after + 1;
        if (v < tag.size() && (tag[v] == L'"' || tag[v] == L'\'')) {
            wchar_t q = tag[v++];
            size_t end = tag.find(q, v);
            if (end == std::wstring::npos) end = tag.size();
            return tag.substr(v, end - v);
        }
        size_t end = v;
        while (end < tag.size() && !iswspace(tag[end]) && tag[end] != L'>') ++end;
        return tag.substr(v, end - v);
    }
    return std::wstring();
}

struct Token {
    bool tag = false;
    std::wstring text;
};

std::vector<Token> Tokenize(const std::wstring& html) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == L'<') {
            size_t end = html.find(L'>', i);
            if (end == std::wstring::npos) break;
            Token t;
            t.tag = true;
            t.text = html.substr(i + 1, end - i - 1);
            out.push_back(std::move(t));
            i = end + 1;
        } else {
            size_t next = html.find(L'<', i);
            if (next == std::wstring::npos) next = html.size();
            Token t;
            t.tag = false;
            t.text = html.substr(i, next - i);
            if (!Trim(t.text).empty()) out.push_back(std::move(t));
            i = next;
        }
    }
    return out;
}

std::wstring TagName(const std::wstring& tag) {
    std::wstring t = Trim(tag);
    if (!t.empty() && t[0] == L'/') t = t.substr(1);
    size_t end = 0;
    while (end < t.size() && (iswalnum(t[end]) || t[end] == L'!' || t[end] == L'-')) ++end;
    return Lower(t.substr(0, end));
}

bool IsClosing(const std::wstring& tag) {
    std::wstring t = Trim(tag);
    return !t.empty() && t[0] == L'/';
}

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
                node.name = HtmlUnescape(Trim(toks[pos + 1].text));
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
            node.path = BookmarkUrlToPath(HtmlUnescape(Attr(inner, L"HREF")));
            node.id = Attr(inner, L"EXBAR_ID");
            node.icon = Attr(inner, L"EXBAR_ICON");
            node.flags = _wtoi(Attr(inner, L"EXBAR_FLAGS").c_str());
            if (node.name.empty() && pos + 1 < toks.size() && !toks[pos + 1].tag)
                node.name = HtmlUnescape(Trim(toks[pos + 1].text));
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
            out += pad + L"<DT><H3 EXBAR_ID=\"" + HtmlEscape(n.id) + L"\" EXBAR_FLAGS=\"" +
                   std::to_wstring(n.flags) + L"\">" + HtmlEscape(n.name) + L"</H3>\r\n";
            out += pad + L"<DL><p>\r\n";
            out += Serialize(n.children, indent + 1);
            out += pad + L"</DL><p>\r\n";
        } else if (!n.path.empty()) {
            out += pad + L"<DT><A HREF=\"" + HtmlEscape(BookmarkPathToUrl(n.path)) + L"\" EXBAR_ID=\"" +
                   HtmlEscape(n.id) + L"\"";
            if (!n.icon.empty()) out += L" EXBAR_ICON=\"" + HtmlEscape(n.icon) + L"\"";
            out += L" EXBAR_FLAGS=\"" + std::to_wstring(n.flags) + L"\">" + HtmlEscape(n.name) +
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
        if (!n.isFolder && _wcsicmp(n.path.c_str(), path.c_str()) == 0) return true;
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

std::wstring BookmarkPathToUrl(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.rfind(L"::", 0) == 0 || path.rfind(L"shell:", 0) == 0) return path;
    std::wstring s = path;
    for (auto& c : s)
        if (c == L'\\') c = L'/';
    std::wstring out = L"file:///";
    // 最小编码：只转义会破坏往返的字符。中文等非 ASCII **原样保留**（文件本身是
    // UTF-8，mod 与浏览器都这么处理；按 UTF-16 码元做 %02X 会把中文永久损坏）。
    // 需要转义的：空格(HTML 属性分隔歧义)、% 与解码器自身冲突的保留字、# ? (fragment/
    // query 歧义)、ASCII 控制符。字母数字和 / : . _ - ~ 原样。
    auto needsEscape = [](wchar_t c) {
        if (c < 0x21 || c == 0x7F) return true;               // 控制符 + 空格
        return c == L'%' || c == L'#' || c == L'?' || c == L'"' || c == L'<' ||
               c == L'>';
    };
    for (wchar_t c : s) {
        if (!needsEscape(c)) {
            out.push_back(c);
        } else {
            wchar_t buf[8];
            swprintf(buf, 8, L"%%%02X", static_cast<unsigned>(c) & 0xFF);
            out += buf;
        }
    }
    return out;
}

std::wstring BookmarkUrlToPath(const std::wstring& url) {
    if (url.empty()) return url;
    std::wstring s = url;
    if (s.rfind(L"file:///", 0) == 0) {
        s = s.substr(8);
        for (auto& c : s)
            if (c == L'/') c = L'\\';
        // %XX 解码
        std::wstring out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
                wchar_t hex[3] = {s[i + 1], s[i + 2], 0};
                out.push_back(static_cast<wchar_t>(wcstol(hex, nullptr, 16)));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }
    return url;
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
    return true;
}

bool BookmarkStore::Save() {
    if (file_.empty()) file_ = Config::Get().BookmarksPath();

    // 【护栏】加载失败过就别写盘。
    //
    // 这是 Load 那条数据丢失链的第二半：Load 失败后内存树可能是空的/不完整的，
    // 此时任何一次 Save（用户加个书签、关窗口时的自动保存）都会把用户的文件
    // 覆盖成"只有表头的空书签"。宁可不保存，也不能销毁用户数据。
    //
    // 用户可以恢复：改好那个文件（或删掉它）后重启，Load 成功即自动解除。
    if (loadFailed_) {
        LogDbg(L"[bmk] 拒绝写盘：上次加载失败，覆盖会销毁用户书签（%s）", file_.c_str());
        return false;
    }

    std::wstring html =
        L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n"
        L"<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\r\n"
        L"<TITLE>Bookmarks</TITLE>\r\n<H1>Bookmarks</H1>\r\n<DL><p>\r\n";
    html += Serialize(roots_, 1);
    html += L"</DL><p>\r\n";
    bool ok = tf::WriteText(file_, html);
    if (ok) {
        dirty_ = false;
        fileStamp_ = LastWriteOf(file_);  // 原子替换后取新时间戳，别把自家写入当外部变更
    }
    return ok;
}

const BookmarkNode* BookmarkStore::Find(const std::wstring& id) const {
    return FindIn(const_cast<BookmarkList&>(roots_), id);
}

BookmarkNode* BookmarkStore::Find(const std::wstring& id) { return FindIn(roots_, id); }

bool BookmarkStore::ContainsPath(const std::wstring& path) const {
    return ContainsIn(roots_, path);
}

BookmarkList* BookmarkStore::ParentList(const std::wstring& parentId) {
    if (parentId.empty()) return &roots_;
    BookmarkNode* n = Find(parentId);
    if (!n || !n->isFolder) return nullptr;
    return &n->children;
}

std::wstring BookmarkStore::NewId(bool folder) {
    for (int guard = 0; guard < 10000; ++guard) {
        wchar_t buf[32];
        swprintf(buf, 32, folder ? L"f-%d" : L"b-%d", seq_++);
        if (!Find(buf)) return buf;
    }
    return std::wstring();
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

void BookmarkStore::ReplaceAll(BookmarkList&& replacement) {
    PushUndo();  // 清空/覆盖导入也走撤销栈，与删除/移动同口径
    roots_ = std::move(replacement);
    seq_ = 1;
    AssignIds(roots_, seq_);
    Touch();
}

size_t BookmarkStore::Merge(BookmarkList&& incoming) {
    size_t added = 0;
    // 顶层按路径去重（与 mod 追加导入同口径：_wcsicmp，仅对叶子书签比较）。
    for (auto& node : incoming) {
        if (!node.isFolder && !node.path.empty() && ContainsIn(roots_, node.path)) continue;
        if (node.isFolder) AssignIds(node.children, seq_);
        if (node.id.empty()) {
            wchar_t buf[32];
            swprintf(buf, 32, node.isFolder ? L"f-%d" : L"b-%d", seq_++);
            node.id = buf;
        }
        roots_.push_back(std::move(node));
        ++added;
    }
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

void BookmarkStore::PushUndo() {
    if (undo_.size() >= 20) undo_.erase(undo_.begin());
    undo_.push_back(roots_);
}

bool BookmarkStore::Undo() {
    if (undo_.empty()) return false;
    roots_ = std::move(undo_.back());
    undo_.pop_back();
    Touch();
    return true;
}

FILETIME BookmarkStore::LastWriteOf(const std::wstring& path) const {
    FILETIME ft{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ft;
    FILETIME written{};
    if (GetFileTime(h, nullptr, nullptr, &written)) ft = written;
    CloseHandle(h);
    return ft;
}

bool BookmarkStore::FileChanged() const {
    if (file_.empty()) return false;
    FILETIME cur = LastWriteOf(file_);
    if (cur.dwHighDateTime == 0 && cur.dwLowDateTime == 0) return false;
    if (fileStamp_.dwHighDateTime == 0 && fileStamp_.dwLowDateTime == 0) return true;
    return CompareFileTime(&cur, &fileStamp_) != 0;
}

std::vector<std::pair<std::wstring, std::wstring>> BookmarkStore::Flatten() const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    FlattenIn(roots_, out);
    return out;
}
