# -*- coding: utf-8 -*-
"""开发期截图：找到 ExBarBrowser 主窗口，PrintWindow 抓完整内容（不受遮挡影响）。

用法：python scripts/shot.py [out.png]
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import zlib

u = ctypes.windll.user32
g = ctypes.windll.gdi32
try:
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        u.SetProcessDPIAware()

CLASS = 'ExBarBrowser_Main'


def find():
    return u.FindWindowW(CLASS, None)


def capture(hwnd, out_png):
    rect = wt.RECT()
    u.GetWindowRect(wt.HWND(hwnd), ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        raise SystemExit('bad rect %s' % ((w, h),))

    hdc_win = u.GetWindowDC(wt.HWND(hwnd))
    hdc_mem = g.CreateCompatibleDC(wt.HDC(hdc_win))
    hbm = g.CreateCompatibleBitmap(wt.HDC(hdc_win), w, h)
    g.SelectObject(wt.HDC(hdc_mem), wt.HBITMAP(hbm))
    ok = u.PrintWindow(wt.HWND(hwnd), wt.HDC(hdc_mem), 2)  # PW_RENDERFULLCONTENT
    print('PrintWindow ->', ok, 'size', w, h)
    if not ok:
        return False

    class BMIH(ctypes.Structure):
        _pack_ = 1
        _fields_ = [('sz', wt.DWORD), ('w', wt.LONG), ('h', wt.LONG),
                    ('planes', wt.WORD), ('bits', wt.WORD), ('comp', wt.DWORD),
                    ('imgsz', wt.DWORD), ('xppm', wt.LONG), ('yppm', wt.LONG),
                    ('used', wt.DWORD), ('imp', wt.DWORD)]

    bi = BMIH(ctypes.sizeof(BMIH), w, -h, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(wt.HDC(hdc_mem), wt.HBITMAP(hbm), 0, h, buf, ctypes.byref(bi), 0)

    raw = bytearray()
    for y in range(h):
        line = buf.raw[y * w * 4:(y + 1) * w * 4]
        out = bytearray(w * 4)
        out[0::4] = line[2::4]
        out[1::4] = line[1::4]
        out[2::4] = line[0::4]
        out[3::4] = b'\xff' * w
        raw.append(0)
        raw += out

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(raw), 6)) +
           chunk(b'IEND', b''))
    open(out_png, 'wb').write(png)
    print('saved', out_png)

    g.DeleteObject(wt.HBITMAP(hbm))
    g.DeleteDC(wt.HDC(hdc_mem))
    u.ReleaseDC(wt.HWND(hwnd), wt.HDC(hdc_win))
    return True


if __name__ == '__main__':
    hwnd = find()
    if not hwnd:
        raise SystemExit('window not found: ' + CLASS)
    out = sys.argv[1] if len(sys.argv) > 1 else 'shot.png'
    capture(hwnd, out)
