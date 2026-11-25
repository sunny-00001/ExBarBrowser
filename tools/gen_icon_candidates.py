#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ExBarBrowser 图标候选集生成器。

把 A/B 两组共 16 个候选渲染出来，输出：
  · resources/icon_candidates_grid.png  —— 4x4 编号对比大图（给人挑）
  · resources/icon_candidates_small.png —— 每个候选的 16/32/48/256 小图对照
  · resources/candidates/<编号>_<名字>.ico —— 每个候选的多尺寸 ico（可直接换上）

刻意做成「一批出图」而不是改一行重跑一版：图标是主观题，
必须并排看得见差异才好挑。

用法:
    python tools/gen_icon_candidates.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

from icon_set_a import CANDIDATES_A  # noqa: E402
from icon_set_b import CANDIDATES_B  # noqa: E402
from icon_set_c import CANDIDATES_C  # noqa: E402
from icon_parts import lin_gradient  # noqa: E402

RES = ROOT / "resources"
OUT_DIR = RES / "candidates"
GRID_PATH = RES / "icon_candidates_grid.png"
SMALL_PATH = RES / "icon_candidates_small.png"

ICO_SIZES = [16, 20, 24, 32, 48, 64, 128, 256]
ALL = CANDIDATES_A + CANDIDATES_B + CANDIDATES_C

FONT_PATH = "C:/Windows/Fonts/msyh.ttc"


def font(sz):
    try:
        return ImageFont.truetype(FONT_PATH, sz)
    except Exception:
        return ImageFont.load_default()


def build_ico(images: dict) -> bytes:
    """手写 ICO 容器：每条以 PNG 存放（与 gen_icon.py 同一套写法）。"""
    import struct
    count = len(images)
    out = bytearray(struct.pack("<HHH", 0, 1, count))
    offset = 6 + 16 * count
    entries, blobs = [], []
    for s in sorted(images):
        png = images[s]
        entries.append(struct.pack(
            "<BBBBHHII",
            0 if s == 256 else s, 0 if s == 256 else s,
            0, 0, 1, 32, len(png), offset))
        blobs.append(png)
        offset += len(png)
    out.extend(b"".join(entries))
    out.extend(b"".join(blobs))
    return bytes(out)


def slug(name: str) -> str:
    """'01 文件夹 + 星火' -> '01_文件夹_星火'（保留编号，便于按序挑选）。"""
    import re
    body = re.sub(r"[^\w\u4e00-\u9fff]+", "_", name).strip("_")
    return body


def render_all():
    """渲染每个候选的全部尺寸，返回 {name: {size: Image}}。"""
    out = {}
    for name, _desc, fn in ALL:
        out[name] = {s: fn(s) for s in ICO_SIZES}
        print(f"  rendered {name}")
    return out


def save_icos(rendered):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    from io import BytesIO
    paths = []
    for name, imgs in rendered.items():
        pngs = {}
        for s, im in imgs.items():
            buf = BytesIO()
            im.save(buf, format="PNG")
            pngs[s] = buf.getvalue()
        p = OUT_DIR / f"{slug(name)}.ico"
        p.write_bytes(build_ico(pngs))
        paths.append(p)
        print(f"  [ico] {p.relative_to(ROOT)}")
    return paths


def make_grid(rendered):
    """编号对比大图：每格 = 256 图 + 编号 + 名称 + 一句话说明（按需多列）。"""
    cols = 4
    rows = (len(ALL) + cols - 1) // cols
    cell = 300
    pad = 18
    label_h = 62
    head = 46
    W = pad + cols * (cell + pad)
    H = head + rows * (cell + label_h + pad) + pad
    out = Image.new("RGB", (W, H), (24, 25, 28))
    d = ImageDraw.Draw(out)
    f_title = font(20)
    f_name = font(16)
    f_desc = font(13)
    d.text((pad, 12), f"ExBarBrowser 图标候选 · {len(ALL)} 选 1", font=f_title,
           fill=(240, 240, 245))
    for i, (name, desc, _fn) in enumerate(ALL):
        r, c = divmod(i, cols)
        x = pad + c * (cell + pad)
        y = head + r * (cell + label_h + pad)
        d.rounded_rectangle((x - 4, y - 4, x + cell + 4, y + cell + label_h + 4),
                            radius=10, fill=(38, 40, 45), outline=(58, 61, 68))
        im = rendered[name][256].resize((cell, cell), Image.Resampling.LANCZOS)
        out.paste(im, (x, y), im)
        num = name.split(" ", 1)[0]
        d.text((x + 2, y + cell + 4), f"{num}  {name.split(' ', 1)[1]}",
               font=f_name, fill=(235, 238, 245))
        d.text((x + 2, y + cell + 26), desc, font=f_desc, fill=(150, 156, 168))
    out.save(GRID_PATH)
    print(f"[grid] {GRID_PATH}")


def make_small(rendered):
    """小尺寸对照：每个候选 16/32/48/256 缩略横排，检验「小图标还认不认得出」。

    256 那一列不放大（否则一行放不下），只缩到 96 见其形；真正的
    尺寸测试交给 16/32/48 三列。
    """
    sizes = [16, 32, 48]
    big = 96
    cols = sizes + [big]
    scale = 2
    row_h = big + 42
    W = 30 + 210 + sum(int(s * scale) + 22 for s in sizes) + big + 30
    H = 34 + len(ALL) * row_h
    out = Image.new("RGB", (W, H), (32, 34, 38))
    d = ImageDraw.Draw(out)
    f = font(14)
    f_head = font(13)
    y = 20
    x0 = 24
    for s in sizes:
        d.text((x0 + 210 + sum(int(k * scale) + 22 for k in sizes[:sizes.index(s)]) + 2,
                4), f"{s}px", font=f_head, fill=(120, 126, 138))
    d.text((W - big - 24, 4), "256", font=f_head, fill=(120, 126, 138))
    for name, desc, _fn in ALL:
        d.text((x0, y + big // 2 - 8), name, font=f, fill=(228, 231, 238))
        x = x0 + 210
        for s in sizes:
            im = rendered[name][s]
            sc = im.resize((s * scale, s * scale),
                           Image.Resampling.NEAREST if s <= 16
                           else Image.Resampling.LANCZOS)
            out.paste(sc, (x, y + big // 2 - sc.height // 2), sc)
            x += sc.width + 22
        im = rendered[name][256].resize((big, big), Image.Resampling.LANCZOS)
        out.paste(im, (W - big - 24, y), im)
        y += row_h
    out.save(SMALL_PATH)
    print(f"[small] {SMALL_PATH}")


def main():
    print("[1/4] render")
    rendered = render_all()
    print("[2/4] ico")
    save_icos(rendered)
    print("[3/4] grid")
    make_grid(rendered)
    print("[4/4] small")
    make_small(rendered)
    print("done.")


if __name__ == "__main__":
    main()
