# -*- coding: utf-8 -*-
"""读 screenshots/*.png（RGB 3B/px，无滤波）并给出边界处的像素剖面（只读）。

用途：判断"分隔条到底画在哪、有几条线"。不发输入、不动光标。
用法：
    python tools/png_profile.py tmp_pid.png cols   # 竖向分界：逐列平均亮度
    python tools/png_profile.py tmp_pid.png rows   # 横向分界：逐行平均亮度
    python tools/png_profile.py tmp_pid.png px X Y  # 单点 RGB
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    w = h = ct = None
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        ch = data[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', ch[:10])
        elif typ == b'IDAT':
            idat += ch
        pos += 12 + ln
    assert ct == 2, 'only RGB(2) supported, got %r' % ct
    raw = zlib.decompress(idat)
    stride = w * 3
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def col_profile(w, h, px, y0, y1):
    prof = []
    for x in range(w):
        tot = n = 0
        for y in range(y0, y1, 3):
            i = (y * w + x) * 3
            tot += px[i] + px[i + 1] + px[i + 2]
            n += 3
        prof.append(tot // max(1, n))
    return prof


def row_profile(w, h, px, x0, x1):
    prof = []
    for y in range(h):
        tot = n = 0
        for x in range(x0, x1, 3):
            i = (y * w + x) * 3
            tot += px[i] + px[i + 1] + px[i + 2]
            n += 3
        prof.append(tot // max(1, n))
    return prof


def main():
    path = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else 'cols'
    w, h, px = read_png(path)
    print('size %dx%d' % (w, h))
    if mode == 'px':
        x, y = int(sys.argv[3]), int(sys.argv[4])
        i = (y * w + x) * 3
        print('(%d,%d) = %d,%d,%d' % (x, y, px[i], px[i + 1], px[i + 2]))
        return
    if mode == 'cols':
        prof = col_profile(w, h, px, 60, h - 40)
        for x in range(2, w - 2):
            if prof[x] + 20 < prof[x - 2] and prof[x] + 20 < prof[x + 2]:
                print('dark col x=%d v=%d (nbrs %d,%d)' % (x, prof[x], prof[x - 2], prof[x + 2]))
        return
    prof = row_profile(w, h, px, 120, w - 120)
    for y in range(2, h - 2):
        if prof[y] + 20 < prof[y - 2] and prof[y] + 20 < prof[y + 2]:
            print('dark row y=%d v=%d (nbrs %d,%d)' % (y, prof[y], prof[y - 2], prof[y + 2]))


if __name__ == '__main__':
    main()