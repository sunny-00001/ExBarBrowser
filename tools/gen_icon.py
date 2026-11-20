#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ExBarBrowser 应用图标生成器
==========================
用 Pillow 手绘一个「标签文件夹」图标（品牌蓝 #4C9FFF），直接按目标尺寸
矢量式渲染（每个尺寸独立绘制 + LANCZOS 降采样），生成多尺寸 .ico 与预览图。
无外部素材依赖；改配色或几何只动本文件，重跑即可。

用法：
    python tools/gen_icon.py
产物：
    resources/app.ico          多尺寸图标（16/20/24/32/48/64/128/256）
    resources/icon_preview.png 各尺寸放大预览（方便肉眼检查）
"""
from pathlib import Path
import struct
from PIL import Image, ImageDraw, ImageOps

ROOT = Path(__file__).resolve().parent.parent
RES_DIR = ROOT / "resources"
ICO_PATH = RES_DIR / "app.ico"
PREVIEW_PATH = RES_DIR / "icon_preview.png"

# 多尺寸：小尺寸任务栏/标题栏清晰，大尺寸资源管理器/Alt-Tab 清晰
SIZES = [16, 20, 24, 32, 48, 64, 128, 256]

# 品牌配色（与 theme.cpp / eb_theme.h 的强调色对齐）
C_TOP = (0x6E, 0xBA, 0xFF)        # 渐变亮端（左上）
C_BOTTOM = (0x2B, 0x7B, 0xD6)     # 渐变暗端（右下）
C_FOLDER_BACK = (0xDD, 0xEC, 0xFF, 0xFF)  # 文件夹后板（淡蓝白）
C_WHITE = (0xFF, 0xFF, 0xFF, 0xFF)        # 前板 / 激活 tab
C_TAB_IDLE = (0xC9, 0xE0, 0xFF, 0xFF)     # 非激活 tab


def _rr(draw, box, r, fill):
    draw.rounded_rectangle(box, radius=max(1, round(r)), fill=fill)


def render(size: int, ss: int = 4) -> Image.Image:
    """按目标尺寸独立渲染一张 RGBA 图标。"""
    W = size * ss

    # 1) 圆角底板 + 对角蓝色渐变（左上亮 -> 右下暗）
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    m = 0.055
    box = (round(W * m), round(W * m), round(W * (1 - m)), round(W * (1 - m)))
    radius = round(W * 0.22)

    lum = Image.new("L", (W, W))
    lum.putdata([
        max(0, min(255, int(255 * (1 - (x + y) / (2 * W)))))
        for y in range(W) for x in range(W)
    ])
    grad = ImageOps.colorize(lum, black=C_BOTTOM, white=C_TOP).convert("RGBA")
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)

    # 顶部高光条：单独图层做 alpha 合成，避免半透明白直接写进 RGBA 变成暗条
    hl = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    _rr(ImageDraw.Draw(hl), (
        box[0] + round(W * 0.03), box[1] + round(W * 0.02),
        box[2] - round(W * 0.03), box[1] + round(W * 0.055),
    ), W * 0.02, (255, 255, 255, 46))
    img = Image.alpha_composite(img, hl)

    d = ImageDraw.Draw(img)

    # 2) 白色文件夹剪影：后板（带左上 tab 凸起）+ 前板
    def P(p):
        return (round(p[0] * W), round(p[1] * W))

    back = [
        (0.19, 0.44), (0.19, 0.37), (0.37, 0.37),
        (0.44, 0.45), (0.80, 0.45), (0.80, 0.70), (0.19, 0.70),
    ]
    d.polygon([P(p) for p in back], fill=C_FOLDER_BACK)
    _rr(d, (round(0.15 * W), round(0.49 * W), round(0.85 * W), round(0.77 * W)),
        W * 0.045, C_WHITE)

    # 3) 顶部两枚 tab，暗示「带标签栏的文件浏览器」（本程序的核心差异）
    _rr(d, (round(0.30 * W), round(0.31 * W), round(0.55 * W), round(0.50 * W)),
        W * 0.028, C_WHITE)
    _rr(d, (round(0.55 * W), round(0.32 * W), round(0.78 * W), round(0.49 * W)),
        W * 0.028, C_TAB_IDLE)

    return img.resize((size, size), Image.Resampling.LANCZOS)


def build_ico(images: dict) -> bytes:
    """手写 ICO 容器：每条以 PNG 存放，Win10/11 全场景支持且小尺寸无损。"""
    count = len(images)
    out = bytearray(struct.pack("<HHH", 0, 1, count))
    offset = 6 + 16 * count
    entries = []
    blobs = []
    for s in images:
        png = images[s]
        entries.append(struct.pack(
            "<BBBBHHII",
            0 if s == 256 else s, 0 if s == 256 else s,
            0, 0, 1, 32, len(png), offset,
        ))
        blobs.append(png)
        offset += len(png)
    out.extend(b"".join(entries))
    out.extend(b"".join(blobs))
    return bytes(out)


def main():
    RES_DIR.mkdir(parents=True, exist_ok=True)

    rendered = {s: render(s) for s in SIZES}

    # 每尺寸单独编码 PNG 再打包成 ICO（保证小尺寸是独立渲染的清晰版本）
    pngs = {}
    for s, im in rendered.items():
        from io import BytesIO
        buf = BytesIO()
        im.save(buf, format="PNG")
        pngs[s] = buf.getvalue()
    ICO_PATH.write_bytes(build_ico(pngs))
    print(f"[ico] {ICO_PATH}  ({ICO_PATH.stat().st_size} bytes, sizes={SIZES})")

    # 预览：各尺寸按 3x 放大排一行，深色底便于观察 alpha 边缘
    scale = 3
    show = [16, 24, 32, 48, 64, 128, 256]
    thumbs = [(s, rendered[s].resize((s * scale, s * scale), Image.Resampling.NEAREST))
              for s in show]
    pad = 14
    total_w = sum(t.width for _, t in thumbs) + pad * (len(thumbs) + 1)
    max_h = max(t.height for _, t in thumbs) + pad * 2 + 22
    prev = Image.new("RGBA", (total_w, max_h), (26, 26, 26, 255))
    d = ImageDraw.Draw(prev)
    x = pad
    for s, t in thumbs:
        prev.paste(t, (x, pad), t)
        d.text((x + 2, pad + t.height + 4), f"{s}px", fill=(224, 224, 224, 255))
        x += t.width + pad
    prev.convert("RGB").save(PREVIEW_PATH)
    print(f"[preview] {PREVIEW_PATH}")


if __name__ == "__main__":
    main()
