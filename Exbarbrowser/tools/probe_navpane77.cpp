// 探针 77：按 **PID** 精确锁定某一实例的导航树，逐帧量"行是否已画出内容"。
//
// 【为什么要重做】探针 73/66 都用 FindWindowW(类名) 找窗口 —— 只拿第一个匹配。
// 当机器上还有用户自己的 ExBarBrowser 时，采样会打到**那个**窗口上，
// 于是"我的测试实例首帧是否空白"根本无法判定（实测踩过：量到的是旧实例的
// 176 宽 / 3 行，而新实例是 220 宽 / 19 行，两边结论完全相反，白查半小时）。
//
// 本探针：命令行给 PID，只认这个进程的窗口。判据是"该行在图标列区间是否有墨"。
//
// 合规：只读 + PrintWindow，不发任何合成输入、不动光标、不抢焦点（R21）。
// 用法：probe77.exe <pid> [采样秒数]
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

DWORD gWantPid = 0;

BOOL CALLBACK FindMainProc(HWND h, LPARAM lp) {
    wchar_t cls[128] = L"";
    GetClassNameW(h, cls, 128);
    if (_wcsicmp(cls, L"ExBarBrowser_Main") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != gWantPid) return TRUE;
    *reinterpret_cast<HWND*>(lp) = h;
    return FALSE;
}

HWND FindTreeOf(HWND root) {
    struct Ctx {
        HWND found = nullptr;
    } ctx;
    EnumChildWindows(
        root,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[128] = L"";
            GetClassNameW(h, cls, 128);
            if (_wcsicmp(cls, L"SysTreeView32") == 0) {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool GrabClient(HWND hwnd, std::vector<unsigned char>* out, int* pw, int* ph) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mem, old);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    out->assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    GetDIBits(mem, bmp, 0, h, out->data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    *pw = w;
    *ph = h;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        printf("usage: probe77 <pid> [seconds]\n");
        return 2;
    }
    gWantPid = static_cast<DWORD>(strtoul(argv[1], nullptr, 10));
    const double secs = argc > 2 ? atof(argv[2]) : 3.0;

    printf("pid=%lu\n", static_cast<unsigned long>(gWantPid));
    printf("t(ms)  treeW treeH indent itemH nodes vis rowsNoInk\n");

    const DWORD t0 = GetTickCount();
    for (;;) {
        const DWORD el = GetTickCount() - t0;
        if (el > static_cast<DWORD>(secs * 1000.0)) break;

        HWND main_ = nullptr;
        EnumWindows(FindMainProc, reinterpret_cast<LPARAM>(&main_));
        if (!main_) {
            Sleep(5);
            continue;
        }
        HWND tv = FindTreeOf(main_);
        if (!tv) {
            Sleep(5);
            continue;
        }

        const int nodes = static_cast<int>(SendMessageW(tv, TVM_GETCOUNT, 0, 0));
        const int vis = static_cast<int>(SendMessageW(tv, TVM_GETVISIBLECOUNT, 0, 0));
        const int indent = static_cast<int>(SendMessageW(tv, TVM_GETINDENT, 0, 0));
        const int ih = static_cast<int>(SendMessageW(tv, TVM_GETITEMHEIGHT, 0, 0));

        RECT cr{};
        GetClientRect(tv, &cr);
        std::vector<unsigned char> px;
        int w = 0, h = 0;
        if (!GrabClient(tv, &px, &w, &h) || ih <= 0 || indent <= 0) {
            Sleep(5);
            continue;
        }

        auto at = [&](int x, int y) -> const unsigned char* {
            return &px[(static_cast<size_t>(y) * static_cast<size_t>(w) +
                        static_cast<size_t>(x)) * 4];
        };

        // 逐行判"有没有墨"。**只统计真实存在的行**（r < nodes）：
        // vis 是"能放下几行"，树矮时它会把节点数放大到占用行数
        // （实测 nodes=3 而 vis=19，于是 16 行空白区被误判成"图标没画"）。
        // 区间取 [indent, 3*indent)（图标列 + 文字起始）。
        // 背景 = 该行最右 6 列的众数色。
        int rowsNoInk = 0;
        int rowsChecked = 0;
        const int limit = (nodes < vis) ? nodes : vis;
        for (int r = 0; r < limit && r * ih < h; ++r) {
            const int y0 = r * ih;
            const int y1 = (y0 + ih < h) ? (y0 + ih) : h;
            const int ym = (y0 + y1) / 2;
            unsigned char bg[3] = {0, 0, 0};
            int best = -1;
            for (int x = w - 6; x < w; ++x) {
                const unsigned char* c = at(x, ym);
                int same = 0;
                for (int x2 = w - 6; x2 < w; ++x2) {
                    const unsigned char* d = at(x2, ym);
                    if (abs(c[0] - d[0]) + abs(c[1] - d[1]) + abs(c[2] - d[2]) < 6) ++same;
                }
                if (same > best) {
                    best = same;
                    bg[0] = c[0];
                    bg[1] = c[1];
                    bg[2] = c[2];
                }
            }
            bool ink = false;
            const int xa = indent;
            const int xb = (indent * 3 < w) ? indent * 3 : w;
            for (int y = y0; y < y1 && !ink; ++y) {
                for (int x = xa; x < xb; ++x) {
                    const unsigned char* c = at(x, y);
                    if (abs(c[0] - bg[0]) + abs(c[1] - bg[1]) + abs(c[2] - bg[2]) > 30) {
                        ink = true;
                        break;
                    }
                }
            }
            if (!ink) ++rowsNoInk;
            ++rowsChecked;
        }

        printf("%5lu %6ld %5ld %6d %5d %5d %3d %5d/%5d\n", static_cast<unsigned long>(el),
               static_cast<long>(cr.right), static_cast<long>(cr.bottom), indent, ih, nodes, vis,
               rowsNoInk, rowsChecked);
        Sleep(16);
    }
    return 0;
}