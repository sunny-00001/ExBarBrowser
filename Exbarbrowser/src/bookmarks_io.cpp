// 书签内部 store 的 Netscape HTML 读写：解析/序列化、磁盘 Load/Save、外部变更检测。
// 与 bookmark_io.cpp 的"外部导入/导出对话框"不是一回事：这里读 EXBAR_ID，
// 那是跨软件稳定 id；bookmark_io.cpp::ParseLevel 故意不读，避免导入撞 id。
// 两套 ParseLevel 的语义差异是有意的，见 bookmark_io.cpp 顶部注释，不可合并。
#include "bookmarks.h"

#include "bmk_html.h"
#include "bookmarks_internal.h"
#include "common.h"
#include "config.h"
#include "str.h"
#include "text_file.h"

#include <cwctype>

namespace {

// Trim / Lower 走唯一实现（str.cpp）：以前 bookmarks / bookmark_io / chord
// 各有一份同样的实现，口径漂移过一次（iswspace vs iswblank）。
using str::Lower;
using str::Trim;

// 词法层（Escape / Unescape / Attr / Tokenize / TagName / IsClosing）已抽到
// bmk_html.h —— 这里曾与 bookmark_io.cpp 各有一份逐字相同的副本。
using bmkhtml::Attr;
using bmkhtml::Escape;
using bmkhtml::IsClosing;
using bmkhtml::TagName;
using bmkhtml::Token;
using bmkhtml::Tokenize;
using bmkhtml::Unescape;

// 解析一层 DL；pos 指向 <DL> 之后的 token 下标。
// 注意：这是**内部文件**解析器，会读 EXBAR_ID / EXBAR_ICON / EXBAR_FLAGS。
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

}  // namespace

std::wstring BookmarkPathToUrl(const std::wstring& path) { return bmkhtml::PathToUrl(path); }

std::wstring BookmarkUrlToPath(const std::wstring& url) { return bmkhtml::UrlToPath(url); }

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

std::wstring BookmarkStore::SerializeDocument() const {
    std::wstring html =
        L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n"
        L"<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\r\n"
        L"<TITLE>Bookmarks</TITLE>\r\n<H1>Bookmarks</H1>\r\n<DL><p>\r\n";
    html += Serialize(roots_, 1);
    html += L"</DL><p>\r\n";
    return html;
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
    bmkstore::detail::AssignIds(parsed, parsedSeq);

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
