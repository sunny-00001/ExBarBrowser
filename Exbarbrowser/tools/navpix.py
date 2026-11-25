#!/usr/bin/env python3
# tools/navpix.py —— 把导航树 BMP 拆成"像素事实"，用来和 explorer 的基线逐项对齐。
#
# 输出每一条**文字行**的：
#   * 行区间（y 起止）
#   * 图标起止 x
#   * 文字起止 x
#   * 行高
# 以及**分隔线**：横贯整幅、颜色与背景不同的细行。
#
# 纯 stdlib（自己解 BMP），不依赖 PIL。
import struct
import sys
from collections import Counter


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        raise SystemExit('not a BMP: ' + path)
    off = struct.unpack_from('<I', data, 10)[0]
    hdr_size = struct.unpack_from('<I', data, 14)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    planes, bpp = struct.unpack_from('<HH', data, 26)
    comp = struct.unpack_from('<I', data, 30)[0]
    if comp != 0:
        raise SystemExit('unsupported compression %d' % comp)
    if bpp not in (24, 32):
        raise SystemExit('unsupported bpp %d' % bpp)
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp // 8) + 3) // 4 * 4
    rows = []
    for y in range(h):
        src = off + (y if top_down else (h - 1 - y)) * stride
        row = []
        for x in range(w):
            p = src + x * (bpp // 8)
            b, g, r = data[p], data[p + 1], data[p + 2]
            row.append((r, g, b))
        rows.append(row)
    return w, h, rows


def bg_color(rows, w, h):
    """背景色 = 右上角附近那一片的颜色（导航树背景是纯色）。"""
    cnt = Counter()
    for y in range(0, min(h, 8)):
        for x in range(w - 12, w):
            cnt[rows[y][x]] += 1
    return cnt.most_common(1)[0][0]


def analyze(path):
    w, h, rows = read_bmp(path)
    bg = bg_color(rows, w, h)
    print('== %s  %dx%d  bg=%s ==' % (path, w, h, bg))

    # 每行：非背景像素的 x 区间
    def runs(y):
        out = []
        start = None
        for x in range(w):
            p = rows[y][x]
            diff = (abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2])) > 24
            if diff and start is None:
                start = x
            elif not diff and start is not None:
                out.append((start, x - 1))
                start = None
        if start is not None:
            out.append((start, w - 1))
        return out

    # 行带 = 连续有内容的 y 段
    bands = []
    cur = None
    for y in range(h):
        if runs(y):
            if cur is None:
                cur = [y, y]
            else:
                cur[1] = y
        else:
            if cur is not None:
                bands.append(tuple(cur))
                cur = None
    if cur is not None:
        bands.append(tuple(cur))

    prev_end = None
    for (y0, y1) in bands:
        gap = '' if prev_end is None else '  gap=%d' % (y0 - prev_end - 1)
        print('  y=%4d..%4d h=%2d%s' % (y0, y1, y1 - y0 + 1, gap))
        for (a, b) in runs((y0 + y1) // 2):
            print('        x=%3d..%3d  w=%3d' % (a, b, b - a + 1))
        prev_end = y1
    return w, h, bands


if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)
        print()
