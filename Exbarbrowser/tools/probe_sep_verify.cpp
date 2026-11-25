// 探针 79：导航树分隔线的**像素验收**探针。
//
// 要回答（一次问完）：
//   1) NSTC 内部树（SysTreeView32）的客户区矩形、主窗口整窗矩形 ——
//      抓图换算要用"树客户区屏幕原点 − 主窗口**整窗**屏幕原点"（不是主窗口客户区原点）。
//   2) 直接把**树客户区**抓成 BMP（PrintWindow + PW_CLIENTONLY）：
//      这样 BMP 的 (0,0) 就是树客户区 (0,0)，像素分析不需要再换算偏移。
//   3) 枚举主窗口的子窗口（类名 + 标题 + 矩形），用来定位两个窗格视图
//      （左右/上下双栏的几何验收）。
//
// 只读：PrintWindow 到内存 DC + 几何查询，不发合成输入（R21 合规）。
//
// 用法：probe_sep_verify.exe <输出前缀> [pid]
//   例：probe_sep_verify.exe tools/verify
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

std::string RectStr(const RECT& r) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%ld,%ld,%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom,
                  r.right - r.left, r.bottom - r.top);
    return buf;
}

struct Ctx {
    int wantPid = 0;
    const wchar_t* wantImage = nullptr;   // 可执行文件路径子串（多实例时避免抓错）
    HWND main = nullptr;
    int mainPid = 0;
};

// 取某个 pid 的主模块路径（含 .exe）。多实例时靠它区分"是谁的窗口"。
std::wstring ImagePathOf(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD n = sizeof(buf) / sizeof(buf[0]) - 1;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

BOOL CALLBACK EnumMainProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<Ctx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (c->wantPid && static_cast<int>(pid) != c->wantPid) return TRUE;
    if (c->wantImage) {
        const std::wstring img = ImagePathOf(pid);
        if (img.find(c->wantImage) == std::wstring::npos) return TRUE;
    }
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    if (wcscmp(cls, L"ExBarBrowser_Main") == 0) {
        c->main = h;
        c->mainPid = static_cast<int>(pid);
        return FALSE;
    }
    return TRUE;
}

struct TreeCtx {
    HWND tree = nullptr;
};

BOOL CALLBACK EnumTreeProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<TreeCtx*>(lp);
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"SysTreeView32") == 0) {
        c->tree = h;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK EnumChildProc(HWND h, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<std::string>*>(lp);
    wchar_t cls[128] = L"";
    wchar_t txt[256] = L"";
    GetClassNameW(h, cls, 128);
    GetWindowTextW(h, txt, 256);
    RECT r{};
    GetWindowRect(h, &r);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "  child cls=%-22s txt=%-14s rect=%s", U8(cls).c_str(),
                  U8(txt).c_str(), RectStr(r).c_str());
    out->push_back(buf);
    return TRUE;
}

// 抓某个窗口的客户区到 BMP。PrintWindow(PW_CLIENTONLY) 对树走 WM_PRINTCLIENT ——
// 导航树的分隔线正是在 WM_PRINTCLIENT 里补画的，所以这条路径能抓到线。
bool SaveClientBmp(HWND hwnd, const char* path) {
    if (!hwnd) return false;
    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return false;
    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, PW_CLIENTONLY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 负高度 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 4);
    GetDIBits(mem, bmp, 0, h, buf.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + static_cast<uint32_t>(buf.size());
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(buf.data(), buf.size(), 1, f);
    fclose(f);
    std::printf("[bmp] 客户区 %dx%d -> %s\n", w, h, path);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: probe_sep_verify.exe <输出前缀> [pid|exe路径子串]\n");
        return 2;
    }
    const std::string prefix = argv[1];
    Ctx ctx{};
    bool byImage = false;
    if (argc >= 3) {
        const std::string arg = argv[2];
        const bool numeric = !arg.empty() && arg.find_first_not_of("0123456789") == std::string::npos;
        if (numeric) {
            ctx.wantPid = std::atoi(arg.c_str());
        } else {
            // 转成宽串：命令行给的是窄串（UTF-8/本地代码页），路径含中文时要按 ACP 解。
            int n = MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, nullptr, 0);
            if (n > 0) {
                std::wstring w(static_cast<size_t>(n - 1), L'\0');
                MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, w.data(), n);
                ctx.wantImage = w.c_str();
                byImage = true;
            }
        }
    }
    EnumWindows(EnumMainProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.main) {
        std::printf("NO_MAIN_WINDOW\n");
        return 1;
    }
    RECT wr{};
    GetWindowRect(ctx.main, &wr);
    std::printf("MAIN hwnd=%p pid=%d%s rect=%s\n", (void*)ctx.main, ctx.mainPid,
                byImage ? " (按 exe 路径匹配)" : "", RectStr(wr).c_str());

    TreeCtx tc{};
    EnumChildWindows(ctx.main, EnumTreeProc, reinterpret_cast<LPARAM>(&tc));
    if (!tc.tree) {
        std::printf("NO_TREE\n");
    } else {
        RECT twr{};
        GetWindowRect(tc.tree, &twr);
        RECT tcr{};
        GetClientRect(tc.tree, &tcr);
        POINT org{tcr.left, tcr.top};
        ClientToScreen(tc.tree, &org);
        std::printf("TREE hwnd=%p winRect=%s client=%dx%d clientOriginScreen=(%ld,%ld)\n",
                    (void*)tc.tree, RectStr(twr).c_str(), static_cast<int>(tcr.right - tcr.left),
                    static_cast<int>(tcr.bottom - tcr.top), org.x, org.y);
        // 树客户区在主窗口整窗 BMP 里的原点（= 屏幕原点 − 主窗口整窗原点）
        std::printf("TREE_ORIGIN_IN_MAINBMP=(%ld,%ld)\n", org.x - wr.left, org.y - wr.top);

        const std::string tbmp = prefix + "_tree.bmp";
        SaveClientBmp(tc.tree, tbmp.c_str());
    }

    std::vector<std::string> kids;
    EnumChildWindows(ctx.main, EnumChildProc, reinterpret_cast<LPARAM>(&kids));
    std::printf("CHILDREN (%d):\n", static_cast<int>(kids.size()));
    for (const auto& s : kids) std::printf("%s\n", s.c_str());
    return 0;
}
