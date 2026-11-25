#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ExBarBrowser 应用图标生成器
==========================
手绘「金黄文件夹 + AI 魔法棒星火」图标（候选 5 定稿），按目标尺寸矢量式
独立渲染（每尺寸单独绘制 + LANCZOS 降采样），生成多尺寸透明底 .ico 与预览图。
无外部素材依赖；改配色或几何只动本文件，重跑即可。

注意：最终定稿已改为 `resources/candidates/12_窗口_星火徽章.ico`；本脚本保留作历史生成器，
默认拒绝覆盖已选定的 app.ico，确需重跑请加 `--force`。

设计要点：
  · 透明底（用户选定：背景对照图的 2/4 号）—— 浅色/深色桌面都不带固定色块。
  · 金黄渐变文件夹铺满画布（明显 tab + 顶部高光）—— 一眼是资源管理器。
  · 魔法棒和星火全部收进文件夹内部 —— 主体更大，AI 感不靠外部漂浮元素。
  · 小尺寸（<=24px）丢掉小星与高光，只留大色块 + 主星火，避免糊成噪点。
  · 每个尺寸独立渲染，非整图拉伸 —— 缩小后边缘干净。
  · 保留 with_bg=True 开关，仅供背景对照图复用，正式产物走透明底。

用法：
    python tools/gen_icon.py
产物：
    resources/app.ico          透明底多尺寸图标（16/20/24/32/48/64/128/256）
    resources/icon_preview.png 各尺寸放大预览（真实像素）
"""
from pathlib import Path
import struct
import sys
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
RES_DIR = ROOT / "resources"
ICO_PATH = RES_DIR / "app.ico"
PREVIEW_PATH = RES_DIR / "icon_preview.png"
SELECTED_ICON = RES_DIR / "candidates" / "12_窗口_星火徽章.ico"

SIZES = [16, 20, 24, 32, 48, 64, 128, 256]

INK = (14, 22, 40, 255)
INK2 = (30, 41, 66, 255)
GOLD_TOP = (255, 217, 102, 255)
GOLD_BOT = (240, 168, 38, 255)
GOLD_BACK = (255, 236, 160, 255)
AMBER = (251, 191, 36, 255)
CYAN = (34, 211, 238, 255)
VIOLET = (167, 139, 250, 255)
WHITE = (248, 250, 252, 255)
SILVER = (148, 163, 184, 255)


def fit_subject(img: Image.Image, margin: float = 0.03) -> Image.Image:
    """透明底：按 alpha 实际范围裁掉多余留白，等比放大铺满画布。"""
    bbox = img.getchannel("A").getbbox()
    if not bbox:
        return img
    W, H = img.size
    pad = round(min(W, H) * 0.015)
    l, t, r, b = bbox
    l, t = max(0, l - pad), max(0, t - pad)
    r, b = min(W, r + pad), min(H, b + pad)
    crop = img.crop((l, t, r, b))
    cw, ch = crop.size
    target = min(W, H) * (1 - 2 * margin)
    scale = min(target / cw, target / ch)
    nw, nh = max(1, round(cw * scale)), max(1, round(ch * scale))
    crop = crop.resize((nw, nh), Image.Resampling.LANCZOS)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(crop, ((W - nw) // 2, (H - nh) // 2), crop)
    return out


def render(size: int, ss: int = 8, with_bg: bool = False) -> Image.Image:
    """按目标尺寸独立渲染一张 RGBA 图标。with_bg=True 仅供背景对照图复用。"""
    W = size * ss
    P = lambda v: round(v * W)
    fine = size >= 32

    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))

    # ---- 1) 深墨蓝圆角底（默认不绘制；with_bg=True 仅用于对照图）----
    if with_bg:
        mask = Image.new("L", (W, W), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (P(0.06), P(0.06), P(0.94), P(0.94)), radius=P(0.22), fill=255)
        bg = Image.new("RGBA", (W, W), INK)
        bd = ImageDraw.Draw(bg)
        for y in range(0, W, 4):
            t = y / W
            col = tuple(int(INK[i] * (1 - t) + INK2[i] * t) for i in range(3)) + (255,)
            bd.rectangle((0, y, W, y + 4), fill=col)
        img.paste(bg, (0, 0), mask)

    # ---- 2) 金黄渐变文件夹（铺满画布）----
    x0, y0, x1, y1 = P(0.05), P(0.40), P(0.95), P(0.88)
    d = ImageDraw.Draw(img)
    d.polygon([
        (x0, y0 + P(0.02)), (x0, y0 - P(0.16)),
        (x0 + P(0.30), y0 - P(0.16)), (x0 + P(0.42), y0 - P(0.04)),
        (x1, y0 - P(0.04)), (x1, y0 + P(0.02)),
    ], fill=GOLD_BACK)
    fmask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(fmask).rounded_rectangle(
        (x0, y0, x1, y1), radius=P(0.035), fill=255)
    gold = Image.new("RGBA", (W, W), GOLD_TOP)
    gd = ImageDraw.Draw(gold)
    for yy in range(y0, y1):
        t = (yy - y0) / max(1, y1 - y0)
        col = tuple(int(GOLD_TOP[i] * (1 - t) + GOLD_BOT[i] * t)
                    for i in range(3)) + (255,)
        gd.line([(0, yy), (W, yy)], fill=col)
    img.paste(gold, (0, 0), fmask)
    if fine:
        d = ImageDraw.Draw(img)
        d.line([(x0 + P(0.035), y0 + P(0.012)), (x1 - P(0.035), y0 + P(0.012))],
               fill=(255, 255, 255, 90), width=max(1, P(0.008)))

    # ---- 3) 魔法棒（收进文件夹内部）----
    d = ImageDraw.Draw(img)
    p0, p1 = (P(0.30), P(0.72)), (P(0.72), P(0.50))
    d.line([p0, p1], fill=INK, width=P(0.060))
    d.line([p0, p1], fill=WHITE, width=P(0.038))
    d.line([p0, p1], fill=SILVER, width=max(1, P(0.012)))

    # ---- 4) 星火（同样在文件夹内部）----
    def star(cx, cy, outer, inner, fill):
        pts = [
            (P(cx), P(cy - outer)), (P(cx + inner), P(cy - inner)),
            (P(cx + outer), P(cy)), (P(cx + inner), P(cy + inner)),
            (P(cx), P(cy + outer)), (P(cx - inner), P(cy + inner)),
            (P(cx - outer), P(cy)), (P(cx - inner), P(cy - inner)),
        ]
        d.polygon(pts, fill=fill)

    star(0.72, 0.50, 0.086, 0.023, CYAN)      # 主星火：青色，压在金底上更清楚
    if fine:
        star(0.44, 0.55, 0.032, 0.010, VIOLET)
        star(0.84, 0.67, 0.035, 0.011, AMBER)

    if not with_bg:
        img = fit_subject(img)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def build_ico(images: dict) -> bytes:
    """手写 ICO 容器：每条以 PNG 存放，Win10/11 全场景支持且小尺寸无损。"""
    count = len(images)
    out = bytearray(struct.pack("<HHH", 0, 1, count))
    offset = 6 + 16 * count
    entries, blobs = [], []
    for s in images:
        png = images[s]
        entries.append(struct.pack("<BBBBHHII",
            0 if s == 256 else s, 0 if s == 256 else s,
            0, 0, 1, 32, len(png), offset))
        blobs.append(png)
        offset += len(png)
    out.extend(b"".join(entries))
    out.extend(b"".join(blobs))
    return bytes(out)


def main():
    RES_DIR.mkdir(parents=True, exist_ok=True)
    if (SELECTED_ICON.exists() and ICO_PATH.exists()
            and ICO_PATH.read_bytes() == SELECTED_ICON.read_bytes()
            and "--force" not in sys.argv):
        print(f"[skip] app.ico 已是选定图标 {SELECTED_ICON.name}；加 --force 才重新生成")
        return
    rendered = {s: render(s, with_bg=False) for s in SIZES}

    pngs = {}
    for s, im in rendered.items():
        from io import BytesIO
        buf = BytesIO()
        im.save(buf, format="PNG")
        pngs[s] = buf.getvalue()
    ICO_PATH.write_bytes(build_ico(pngs))
    print(f"[ico] {ICO_PATH}  ({ICO_PATH.stat().st_size} bytes, sizes={SIZES})")

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
