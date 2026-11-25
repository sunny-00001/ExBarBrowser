#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时候选画廊：12 个风格各异的图标，拼成一张大预览图供挑选。
与 gen_icon.py 完全独立，不写 ico、不碰 exe。"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "resources" / "icon_gallery.png"

SS = 3
CELL = 240
W = CELL * SS
P = lambda v: round(v * W)

# 克制、高级的调色板
INK = (14, 22, 40, 255)
INK2 = (26, 38, 64, 255)
INDIGO = (99, 102, 241, 255)
VIOLET = (139, 92, 246, 255)
CYAN = (34, 211, 238, 255)
SKY = (56, 189, 248, 255)
WHITE = (248, 250, 252, 255)
CORAL = (251, 113, 133, 255)
AMBER = (251, 191, 36, 255)
MINT = (52, 211, 153, 255)
SILVER = (148, 163, 184, 255)


def blank():
    return Image.new("RGBA", (W, W), (0, 0, 0, 0))


def dark_bg(im, c1=INK, c2=INK2):
    """统一的深色圆角底，亮色元素跳出来。"""
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle((P(0.06), P(0.06), P(0.94), P(0.94)),
                                           radius=P(0.22), fill=255)
    grad = Image.new("RGBA", (W, W), c1)
    d = ImageDraw.Draw(grad)
    for y in range(0, W, 4):
        t = y / W
        col = tuple(int(c1[i] * (1 - t) + c2[i] * t) for i in range(3)) + (255,)
        d.rectangle((0, y, W, y + 4), fill=col)
    im.paste(grad, (0, 0), mask)
    return im


def rr(d, box, r, **kw):
    d.rounded_rectangle(tuple(P(v) for v in box), radius=P(r), **kw)


def star(d, cx, cy, outer, inner, fill):
    pts = [(cx, cy - outer), (cx + inner, cy - inner), (cx + outer, cy),
           (cx + inner, cy + inner), (cx, cy + outer), (cx - inner, cy + inner),
           (cx - outer, cy), (cx - inner, cy - inner)]
    d.polygon([(P(x), P(y)) for x, y in pts], fill=fill)


def folder_poly(d, pts, fill):
    d.polygon([(P(x), P(y)) for x, y in pts], fill=fill)


def finish(im):
    return im.resize((CELL, CELL), Image.Resampling.LANCZOS)


# 1 立体等距文件夹
def g1():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    folder_poly(d, [(0.20, 0.50), (0.50, 0.36), (0.82, 0.50), (0.52, 0.65)], SKY)      # 顶
    folder_poly(d, [(0.20, 0.50), (0.52, 0.65), (0.52, 0.79), (0.20, 0.64)], INDIGO)   # 左前
    folder_poly(d, [(0.52, 0.65), (0.82, 0.50), (0.82, 0.64), (0.52, 0.79)], VIOLET)   # 右前
    folder_poly(d, [(0.20, 0.50), (0.50, 0.36), (0.50, 0.43), (0.20, 0.57)], CYAN)     # 顶棱
    star(d, 0.66, 0.30, 0.085, 0.022, WHITE)
    return finish(im)


# 2 文件夹 + AI 对话气泡
def g2():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    folder_poly(d, [(0.18, 0.44), (0.18, 0.62), (0.82, 0.62), (0.82, 0.44),
                    (0.52, 0.44), (0.46, 0.37), (0.24, 0.37)], (203, 213, 225, 255))
    rr(d, (0.18, 0.48, 0.82, 0.72), 0.05, fill=WHITE)
    rr(d, (0.48, 0.22, 0.84, 0.46), 0.06, fill=VIOLET)
    d.polygon([(P(0.56), P(0.44)), (P(0.62), P(0.44)), (P(0.56), P(0.52))], fill=VIOLET)
    star(d, 0.66, 0.34, 0.075, 0.02, WHITE)
    return finish(im)


# 3 负形字母 E
def g3():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    e = [(0.30, 0.24), (0.72, 0.24), (0.72, 0.34), (0.44, 0.34),
         (0.44, 0.45), (0.65, 0.45), (0.65, 0.55), (0.44, 0.55),
         (0.44, 0.64), (0.72, 0.64), (0.72, 0.74), (0.30, 0.74)]
    folder_poly(d, e, WHITE)
    star(d, 0.72, 0.29, 0.055, 0.015, CYAN)
    return finish(im)


# 4 打开的文件夹飞出星火
def g4():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    rr(d, (0.16, 0.50, 0.84, 0.76), 0.05, fill=INDIGO)
    folder_poly(d, [(0.16, 0.52), (0.16, 0.44), (0.42, 0.44), (0.48, 0.51)], SKY)
    folder_poly(d, [(0.20, 0.48), (0.76, 0.30), (0.80, 0.38), (0.24, 0.56)], VIOLET)  # 掀开的盖
    star(d, 0.60, 0.30, 0.09, 0.025, WHITE)
    star(d, 0.78, 0.20, 0.035, 0.010, CYAN)
    star(d, 0.42, 0.22, 0.025, 0.008, AMBER)
    return finish(im)


# 5 晶体棱镜
def g5():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    hexp = [(0.50, 0.18), (0.80, 0.35), (0.80, 0.65), (0.50, 0.82), (0.20, 0.65), (0.20, 0.35)]
    folder_poly(d, hexp, INDIGO)
    folder_poly(d, [(0.50, 0.18), (0.80, 0.35), (0.50, 0.50), (0.20, 0.35)], SKY)
    folder_poly(d, [(0.20, 0.35), (0.50, 0.50), (0.50, 0.82), (0.20, 0.65)], VIOLET)
    folder_poly(d, [(0.50, 0.50), (0.80, 0.35), (0.80, 0.65), (0.50, 0.82)], (79, 70, 229, 255))
    star(d, 0.50, 0.50, 0.11, 0.03, WHITE)
    return finish(im)


# 6 文档堆叠 + 星
def g6():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    def page(box, fill, fold=False):
        rr(d, box, 0.04, fill=fill)
    rr(d, (0.28, 0.22, 0.70, 0.68), 0.04, fill=(71, 85, 105, 255))
    rr(d, (0.24, 0.27, 0.66, 0.73), 0.04, fill=(100, 116, 139, 255))
    rr(d, (0.20, 0.32, 0.62, 0.78), 0.04, fill=WHITE)
    d.polygon([(P(0.50), P(0.32)), (P(0.62), P(0.32)), (P(0.62), P(0.44))], fill=(203, 213, 225, 255))
    for y in (0.48, 0.56, 0.64):
        rr(d, (0.28, y, 0.54, y + 0.025), 0.01, fill=(203, 213, 225, 255))
    star(d, 0.74, 0.26, 0.085, 0.022, AMBER)
    return finish(im)


# 7 对话气泡组
def g7():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    rr(d, (0.14, 0.26, 0.68, 0.62), 0.09, fill=WHITE)
    d.polygon([(P(0.26), P(0.58)), (P(0.38), P(0.58)), (P(0.26), P(0.70))], fill=WHITE)
    rr(d, (0.46, 0.46, 0.88, 0.76), 0.09, fill=VIOLET)
    d.polygon([(P(0.78), P(0.72)), (P(0.88), P(0.72)), (P(0.88), P(0.62))], fill=VIOLET)
    for x in (0.28, 0.38, 0.48):
        d.ellipse((P(x - 0.025), P(0.40), P(x + 0.025), P(0.45)), fill=SILVER)
    star(d, 0.67, 0.61, 0.075, 0.02, WHITE)
    return finish(im)


# 8 圆形徽章
def g8():
    im = blank(); d = ImageDraw.Draw(im)
    d.ellipse((P(0.08), P(0.08), P(0.92), P(0.92)), fill=INDIGO)
    d.ellipse((P(0.13), P(0.13), P(0.87), P(0.87)), fill=(79, 70, 229, 255))
    folder_poly(d, [(0.26, 0.60), (0.26, 0.40), (0.46, 0.40), (0.51, 0.46),
                    (0.74, 0.46), (0.74, 0.60)], (199, 210, 254, 255))
    rr(d, (0.24, 0.52, 0.76, 0.70), 0.03, fill=WHITE)
    star(d, 0.50, 0.34, 0.075, 0.02, CYAN)
    return finish(im)


# 9 魔法棒
def g9():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    d.line([(P(0.28), P(0.76)), (P(0.64), P(0.40))], fill=WHITE, width=P(0.05))
    d.line([(P(0.28), P(0.76)), (P(0.64), P(0.40))], fill=SILVER, width=P(0.018))
    star(d, 0.68, 0.34, 0.13, 0.035, AMBER)
    star(d, 0.48, 0.20, 0.04, 0.011, CYAN)
    star(d, 0.84, 0.52, 0.05, 0.014, VIOLET)
    star(d, 0.34, 0.32, 0.03, 0.009, WHITE)
    return finish(im)


# 10 定位 pin + 文件夹
def g10():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    d.ellipse((P(0.20), P(0.16), P(0.80), P(0.70)), fill=CORAL)
    d.polygon([(P(0.36), P(0.58)), (P(0.64), P(0.58)), (P(0.50), P(0.86))], fill=CORAL)
    d.ellipse((P(0.30), P(0.26), P(0.70), P(0.62)), fill=WHITE)
    folder_poly(d, [(0.36, 0.50), (0.36, 0.38), (0.50, 0.38), (0.54, 0.43),
                    (0.64, 0.43), (0.64, 0.50)], CORAL)
    rr(d, (0.35, 0.46, 0.65, 0.56), 0.015, fill=CORAL)
    return finish(im)


# 11 层叠卡片 + 星
def g11():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    rr(d, (0.20, 0.24, 0.72, 0.60), 0.05, fill=(79, 70, 229, 255))
    rr(d, (0.24, 0.32, 0.76, 0.68), 0.05, fill=INDIGO)
    rr(d, (0.28, 0.40, 0.80, 0.76), 0.05, fill=WHITE)
    star(d, 0.54, 0.58, 0.085, 0.023, VIOLET)
    return finish(im)


# 12 拱门 + 星
def g12():
    im = blank(); dark_bg(im); d = ImageDraw.Draw(im)
    d.pieslice((P(0.20), P(0.18), P(0.80), P(0.78)), 180, 360, fill=VIOLET)
    d.rectangle((P(0.20), P(0.48), P(0.80), P(0.76)), fill=VIOLET)
    d.pieslice((P(0.28), P(0.26), P(0.72), P(0.70)), 180, 360, fill=WHITE)
    d.rectangle((P(0.28), P(0.48), P(0.72), P(0.70)), fill=WHITE)
    star(d, 0.50, 0.50, 0.10, 0.028, INDIGO)
    rr(d, (0.16, 0.76, 0.84, 0.82), 0.02, fill=CYAN)
    return finish(im)


ITEMS = [
    ("1 立体文件夹", g1), ("2 文件夹+气泡", g2), ("3 负形E", g3),
    ("4 开盒星火", g4), ("5 晶体棱镜", g5), ("6 文档堆+星", g6),
    ("7 对话气泡", g7), ("8 圆形徽章", g8), ("9 魔法棒", g9),
    ("10 定位pin", g10), ("11 层叠卡片", g11), ("12 拱门", g12),
]


def main():
    cols, rows = 4, 3
    pad, label_h, title_h = 16, 30, 40
    gw = pad * (cols + 1) + CELL * cols
    gh = title_h + pad + rows * CELL + (rows - 1) * pad + rows * label_h + pad
    out = Image.new("RGB", (gw, gh), (18, 18, 22))
    d = ImageDraw.Draw(out)
    d.text((pad, 12), "ExBarBrowser icon gallery - 12 candidates", fill=(240, 240, 240))
    y0 = title_h + pad
    for idx, (name, fn) in enumerate(ITEMS):
        r, c = divmod(idx, cols)
        x = pad + c * (CELL + pad)
        y = y0 + r * (CELL + label_h + pad)
        out.paste(fn(), (x, y))
        d.text((x + 4, y + CELL + 6), name, fill=(220, 220, 220))
    out.save(OUT)
    print(OUT, out.size)


if __name__ == "__main__":
    main()
