#include "bookmark_io.h"

#include "bmk_html.h"

#include "bookmarks.h"
#include "config.h"
#include "log.h"
#include "menu.h"
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
            // 没有内容的分类也要保留（名字兜底）—— 原实现让空分类名留空，
            // Merge 按名字对齐时它们会全部塌成同一个"空名分类"。浏览器允许
            // 建空文件夹，导出的文件里就会有一条只有 H3 没有 A 的分类。
            if (node.name.empty()) node.name = L"分类";
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
            // 路径解不出来（坏百分号 / 非法 UTF-8）时**丢弃这一条**，而不是把
            // 半截路径塞进书签 —— 导进去的坏路径只会在点击时失败，比不导入更难查。
            // 但要在日志里留痕，否则用户会以为"我明明导入了几百条怎么少了几条"。
            if (!node.path.empty()) {
                out.push_back(std::move(node));
            } else if (!href.empty()) {
                LogDbg(L"[bmk] 导入跳过无法解析的链接：%s", href.c_str());
            }
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

int BmkExportHtmlTo(const std::wstring& file) {
    BookmarkStore& store = BookmarkStore::Get();
    std::wstring html =
        L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n"
        L"<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\r\n"
        L"<TITLE>Bookmarks</TITLE>\r\n<H1>Bookmarks</H1>\r\n<DL><p>\r\n";
    SerializeLevel(store.roots(), 1, html);
    html += L"</DL><p>\r\n";

    std::string u8 = tf::ToUtf8(html);
    HANDLE h = CreateFileW(tf::ToExtended(file).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -2;
    DWORD written = 0;
    const BYTE bom[] = {0xEF, 0xBB, 0xBF};
    WriteFile(h, bom, 3, &written, nullptr);
    WriteFile(h, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
    CloseHandle(h);
    return static_cast<int>(CountLeaves(store.roots()));
}

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
                result = BmkExportHtmlTo(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// 把一份 Netscape HTML 解析成书签树。供 UI 版与确定式版共用。
static bool ParseHtmlFile(const std::wstring& file, BookmarkList* imported) {
    std::wstring html;
    if (!tf::ReadText(file, &html)) return false;
    std::vector<Tok> toks = Tokenize(html);
    size_t pos = 0;
    while (pos < toks.size() && !(toks[pos].tag && TagName(toks[pos].text) == L"dl" &&
                                  !IsClosing(toks[pos].text)))
        ++pos;
    if (pos >= toks.size()) return false;
    ++pos;
    ParseLevel(toks, pos, *imported, 0);
    return !imported->empty();
}

int BmkImportHtmlFromFile(const std::wstring& file, int mode) {
    BookmarkList imported;
    if (!ParseHtmlFile(file, &imported)) return -2;
    // 守卫：文件被外部改过就拒绝写入，防止覆盖外部修改（与 UI 版同一处口径）。
    BookmarkStore& store = BookmarkStore::Get();
    if (store.FileChanged()) return -3;
    if (mode == 1) {
        store.ReplaceAll(std::move(imported));
        const int n = static_cast<int>(CountLeaves(store.roots()));
        store.Save();
        return n;
    }
    const size_t added = store.Merge(std::move(imported));
    if (added) store.Save();
    return static_cast<int>(added);
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

                // 先确认文件可解析（空/无效文件不弹模式选择，避免误导用户）。
                BookmarkList probe;
                if (ParseHtmlFile(file, &probe)) {
                    // 【追加 / 覆盖的选择不能塞进 MessageBox 的是/否】
                    //
                    // 原来的写法：MessageBox("请选择导入模式（是=追加，否=覆盖）",
                    // MB_YESNOCANCEL)。三个按钮里两个都带"破坏性歧义"：
                    //   · 用户习惯性点是（"好的，导入吧"）→ 得到追加，符合预期；
                    //   · 但"否"在直觉里是"不要覆盖"，而它恰恰**就是**覆盖 ——
                    //     一把清空全部书签，且导入的文件可能是别的浏览器里的一小部分。
                    // 覆盖是全模块唯一的"一键清空"入口（除了清除全部书签），
                    // 不该藏在一个措辞反直觉的按钮后面。
                    //
                    // 现在用自绘弹层：两个动作用各自明确的文案，覆盖项标出来。
                    ui::Menu menu;
                    menu.Add(L"追加到现有书签（推荐）", 0, ico::IconId::Add)
                        .Separator()
                        .Add(L"覆盖全部书签（现有书签将被替换）", 1, ico::IconId::Delete);
                    POINT at{};
                    GetCursorPos(&at);
                    ui::MenuOptions opt;
                    opt.minWidthPx = theme.S(300);
                    opt.maxRows = 8;
                    opt.toggleKey = L"bmk.import.mode";
                    const int pick = menu.Show(owner, theme, at, opt);
                    // 取消（点空白/Esc）→ 什么都不做。
                    if (pick < 0) return result;

                    BookmarkStore& store = BookmarkStore::Get();
                    if (store.FileChanged()) {
                        MessageBoxW(owner,
                                    L"书签文件已被其它程序修改，请先右键“重新载入书签文件”再导入。",
                                    L"导入书签", MB_OK | MB_ICONWARNING);
                    } else if (pick == 1 &&
                               MessageBoxW(owner, L"确定用导入的文件替换全部现有书签？此操作可撤销。",
                                           L"覆盖全部书签",
                                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                        return result;
                    }
                    // 解析与合并共用确定式后端：UI 版与 AI 版不会分叉。
                    // 上面已做过 FileChanged 守卫与覆盖确认，后端不再弹框。
                    result = BmkImportHtmlFromFile(file, pick);
                }
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}
