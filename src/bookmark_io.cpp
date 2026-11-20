#include "bookmark_io.h"

#include "bmk_html.h"

#include "bookmarks.h"
#include "config.h"
#include "prompt.h"
#include "text_file.h"

#include <shobjidl.h>

#include <cwctype>

#include "str.h"

namespace {

// TrimWs / Lower 走唯一实现（str.cpp）；TrimWs 保留旧名以免改动 12 个调用点。
using str::Lower;
inline std::wstring TrimWs(const std::wstring& s) { return str::Trim(s); }

// 词法层已抽到 bmk_html.h（与 bookmarks.cpp 共用）。
//
// 【注意：ParseLevel 故意不共用】本文件导入外部书签时**不读** EXBAR_ID /
// EXBAR_ICON / EXBAR_FLAGS —— 否则导入的条目会与现有书签撞 id。
// "导入不撞 id"正是靠这个差异成立的，合并会让两者之一出错。
using bmkhtml::Attr;
using bmkhtml::IsClosing;
using bmkhtml::TagName;
using bmkhtml::Tokenize;
using Tok = bmkhtml::Token;
using bmkhtml::Escape;
using bmkhtml::Unescape;


void ParseLevel(const std::vector<Tok>& toks, size_t& pos, BookmarkList& out, int depth) {
    if (depth > 32) return;
    while (pos < toks.size()) {
        const Tok& t = toks[pos];
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
        while (pos < toks.size() && !toks[pos].tag) ++pos;
        if (pos >= toks.size()) return;
        const std::wstring inner = toks[pos].text;
        const std::wstring iname = TagName(inner);
        if (iname == L"h3") {
            BookmarkNode node;
            node.isFolder = true;
            if (pos + 1 < toks.size() && !toks[pos + 1].tag)
                node.name = Unescape(TrimWs(toks[pos + 1].text));
            ++pos;
            while (pos < toks.size() && !(toks[pos].tag && TagName(toks[pos].text) == L"h3" &&
                                          IsClosing(toks[pos].text)))
                ++pos;
            ++pos;
            while (pos < toks.size() && !(toks[pos].tag && TagName(toks[pos].text) == L"dl" &&
                                          !IsClosing(toks[pos].text)))
                ++pos;
            if (pos < toks.size()) {
                ++pos;
                ParseLevel(toks, pos, node.children, depth + 1);
            }
            out.push_back(std::move(node));
        } else if (iname == L"a") {
            // HREF 是 HTML 转义过的，先还原实体再解 URL（与 BookmarkStore 同口径）。
            std::wstring href = Unescape(Attr(inner, L"HREF"));
            BookmarkNode node;
            node.path = BookmarkUrlToPath(href);
            if (pos + 1 < toks.size() && !toks[pos + 1].tag)
                node.name = Unescape(TrimWs(toks[pos + 1].text));
            if (node.name.empty()) node.name = node.path;
            if (!node.path.empty()) out.push_back(std::move(node));
            ++pos;
            while (pos < toks.size() && !(toks[pos].tag && TagName(toks[pos].text) == L"a" &&
                                          IsClosing(toks[pos].text)))
                ++pos;
            ++pos;
        } else {
            ++pos;
        }
    }
}

void SerializeLevel(const BookmarkList& list, int indent, std::wstring& out) {
    std::wstring pad(static_cast<size_t>(indent * 4), L' ');
    for (const auto& n : list) {
        if (n.isFolder) {
            out += pad + L"<DT><H3>" + Escape(n.name) + L"</H3>\r\n";
            out += pad + L"<DL><p>\r\n";
            SerializeLevel(n.children, indent + 1, out);
            out += pad + L"</DL><p>\r\n";
        } else if (!n.path.empty()) {
            out += pad + L"<DT><A HREF=\"" + Escape(BookmarkPathToUrl(n.path)) +
                   L"\">" + Escape(n.name) + L"</A>\r\n";
        }
    }
}

// 统计树里实际书签条数（不含纯分类）。
size_t CountLeaves(const BookmarkList& list) {
    size_t n = 0;
    for (const auto& node : list) {
        if (!node.isFolder && !node.path.empty()) ++n;
        n += CountLeaves(node.children);
    }
    return n;
}

}  // namespace

int BmkExportHtml(HWND owner, const Theme& theme) {
    (void)theme;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_IFileSaveDialog,
                                reinterpret_cast<void**>(&dlg))) ||
        !dlg)
        return -1;
    const COMDLG_FILTERSPEC filter[] = {{L"HTML 书签文件 (*.html)", L"*.html"},
                                        {L"全部文件 (*.*)", L"*.*"}};
    dlg->SetFileTypes(2, const_cast<COMDLG_FILTERSPEC*>(filter));
    dlg->SetDefaultExtension(L"html");
    dlg->SetFileName(L"bookmarks.html");

    int result = -1;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                std::wstring file = path;
                CoTaskMemFree(path);

                BookmarkStore& store = BookmarkStore::Get();
                std::wstring html =
                    L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n"
                    L"<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\r\n"
                    L"<TITLE>Bookmarks</TITLE>\r\n<H1>Bookmarks</H1>\r\n<DL><p>\r\n";
                SerializeLevel(store.roots(), 1, html);
                html += L"</DL><p>\r\n";

                std::string u8 = tf::ToUtf8(html);
                HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    const BYTE bom[] = {0xEF, 0xBB, 0xBF};
                    WriteFile(h, bom, 3, &written, nullptr);
                    WriteFile(h, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
                    CloseHandle(h);
                    result = static_cast<int>(CountLeaves(store.roots()));
                } else {
                    result = -2;
                }
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

int BmkImportHtml(HWND owner, const Theme& theme) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dlg))) ||
        !dlg)
        return -1;
    const COMDLG_FILTERSPEC filter[] = {{L"HTML 书签文件 (*.html;*.htm)", L"*.html;*.htm"},
                                        {L"全部文件 (*.*)", L"*.*"}};
    dlg->SetFileTypes(2, const_cast<COMDLG_FILTERSPEC*>(filter));

    int result = -1;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                std::wstring file = path;
                CoTaskMemFree(path);

                std::wstring html;
                if (tf::ReadText(file, &html)) {
                    std::vector<Tok> toks = Tokenize(html);
                    BookmarkList imported;
                    size_t pos = 0;
                    while (pos < toks.size() && !(toks[pos].tag &&
                                                  TagName(toks[pos].text) == L"dl" &&
                                                  !IsClosing(toks[pos].text)))
                        ++pos;
                    if (pos < toks.size()) {
                        ++pos;
                        ParseLevel(toks, pos, imported, 0);
                    }
                    if (!imported.empty()) {
                        // 模式选择：0=追加 1=覆盖 -1=取消。复用 PromptText 风格，
                        // 用 MessageBox 保持简单可靠（mod 也提供 MessageBox 回退）。
                        int mode = -1;
                        int r = MessageBoxW(owner, L"请选择导入模式（是=追加，否=覆盖）",
                                            L"导入书签", MB_YESNOCANCEL | MB_ICONQUESTION);
                        if (r == IDYES)
                            mode = 0;
                        else if (r == IDNO)
                            mode = 1;
                        if (mode >= 0) {
                            BookmarkStore& store = BookmarkStore::Get();
                            // 守卫：文件被外部改过就拒绝写入，防止覆盖外部修改（mod/编辑器）。
                            if (store.FileChanged()) {
                                MessageBoxW(owner,
                                            L"书签文件已被其它程序修改，请先右键“重新载入书签文件”再导入。",
                                            L"导入书签", MB_OK | MB_ICONWARNING);
                            } else if (mode == 1) {
                                store.ReplaceAll(std::move(imported));
                                result = static_cast<int>(CountLeaves(store.roots()));
                                store.Save();
                            } else {
                                size_t added = store.Merge(std::move(imported));
                                result = static_cast<int>(added);
                                if (added) store.Save();
                            }
                        }
                    }
                }
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}
