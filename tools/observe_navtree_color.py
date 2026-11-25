# 只读观察：读运行中 ExBarBrowser 导航树的实际背景色。
# 不发合成输入，只读；不动光标、不抢焦点。
# TVM_GETBKCOLOR 不带指针、只返回标量，跨进程安全。
import ctypes
import ctypes.wintypes as wt
import sys

u = ctypes.windll.user32
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u.EnumChildWindows.argtypes = [wt.HWND, WNDENUMPROC, wt.LPARAM]
u.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, ctypes.c_longlong]
u.SendMessageW.restype = ctypes.c_longlong

TVM_FIRST = 0x1100
TVM_GETBKCOLOR = TVM_FIRST + 31
TVM_GETTEXTCOLOR = TVM_FIRST + 35
CLR_NONE = 0xFFFFFFFF


def find_main():
    return u.FindWindowW('ExBarBrowser_Main', None)


def children(h):
    out = []

    def cb(c, _):
        out.append(c)
        return True

    u.EnumChildWindows(h, WNDENUMPROC(cb), 0)
    return out


def cls(h):
    b = ctypes.create_unicode_buffer(128)
    u.GetClassNameW(h, b, 128)
    return b.value


def main():
    m = find_main()
    if not m:
        print("没找到主窗口（应用没在跑？）")
        return 2
    print(f"主窗口 = {m}")

    trees = [c for c in children(m) if cls(c) == 'SysTreeView32']
    if not trees:
        print("主窗口下没有 SysTreeView32（导航树可能未创建/已隐藏）")
        return 3

    for i, tv in enumerate(trees):
        bk = u.SendMessageW(tv, TVM_GETBKCOLOR, 0, 0) & 0xFFFFFFFF
        tx = u.SendMessageW(tv, TVM_GETTEXTCOLOR, 0, 0) & 0xFFFFFFFF
        if bk == CLR_NONE:
            bk_s = "CLR_NONE(用主题色)"
        else:
            bk_s = f"RGB({bk & 0xFF},{(bk >> 8) & 0xFF},{(bk >> 16) & 0xFF})"
        tx_s = "CLR_NONE" if tx == CLR_NONE else f"RGB({tx & 0xFF},{(tx >> 8) & 0xFF},{(tx >> 16) & 0xFF})"
        print(f"  树#{i} hwnd={tv}  BK={bk_s}  TEXT={tx_s}")
    return 0


if __name__ == '__main__':
    sys.exit(main())