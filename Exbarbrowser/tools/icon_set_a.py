#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图标候选组 A（1–8 号）。

每个候选是一个函数 draw(size) -> PIL RGBA Image。
所有候选共用一条硬约束：**必须一眼看出是文件管理器**，同时带上明确的 AI 信号。
差别在于「AI 用什么符号表达」和「整体风格」，方便横向比选。
"""
from PIL import Image
from icon_parts import (Canvas, tile_bg, shadow_under, spark, bolt, folder,
                        file_rows, tree_items, chip, brain_nodes, robot,
                        pencil_spark, gear_spark, magnifier_spark)

BLUE_TOP = (0x6C, 0xC2, 0xFF)
BLUE_BOT = (0x1B, 0x5C, 0xBE)
AI_TOP = (0xA7, 0x8B, 0xFA)
AI_BOT = (0x6D, 0x28, 0xD9)
WHITE = (255, 255, 255, 255)
PAPER = (0xEC, 0xF3, 0xFC, 255)
GOLD = (0xFF, 0xD1, 0x66, 255)


def _01_folder_spark(size):
    """1 文件夹 + 星火：最直白的组合，文件夹占主体，右上角一枚金色星火。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, BLUE_TOP, BLUE_BOT))
    folder(cv, 0.14, 0.30, 0.86, 0.79, WHITE, (0xD3, 0xE4, 0xFA, 255),
           (0xB9, 0xD2, 0xF4, 255), r=0.035)
    spark(cv, 0.755, 0.285, 0.155, GOLD, inner=0.26)
    spark(cv, 0.885, 0.50, 0.075, (0xFF, 0xE9, 0xB0, 255), inner=0.26)
    return cv.out()


def _02_panes_ai(size):
    """2 三栏窗口 + 紫色 AI 面板：现役方案（app.ico 的形态），作对照组。"""
    cv = Canvas(size)
    fine = size >= 32
    L, T, R, B = 0.075, 0.085, 0.925, 0.915
    rad, title_b = 0.105, 0.265
    nav_r, ai_l = 0.295, 0.705
    from icon_parts import rounded_mask
    wm = rounded_mask(cv.w, rad, 0.0, cv.ss)
    base = Image.new("RGBA", (cv.w, cv.w), (255, 255, 255, 0))
    base.putalpha(wm)
    cv.paste(base)
    cv.rect((L, title_b, nav_r, B), fill=PAPER)
    cv.rect((nav_r, title_b, ai_l, B), fill=WHITE)
    from icon_parts import lin_gradient
    ai = lin_gradient(cv.w, AI_TOP, AI_BOT)
    aim = Image.new("L", (cv.w, cv.w), 0)
    from PIL import ImageDraw
    ImageDraw.Draw(aim).rectangle(cv.box((ai_l, title_b, R, B)), fill=255)
    layer = Image.new("RGBA", (cv.w, cv.w), (0, 0, 0, 0))
    layer.paste(ai, (0, 0), aim)
    cv.paste(layer)
    if fine:
        tree_items(cv, L + 0.055, 0.35, nav_r - L - 0.10, 4, row_h=0.036, gap=0.028)
        file_rows(cv, nav_r + 0.045, 0.35, ai_l - 0.045, 5, row_h=0.042, gap=0.022,
                  icon_col=(0x8F, 0xBC, 0xEF, 255))
    from icon_parts import lin_gradient as _lg
    spark(cv, (ai_l + R) / 2, (title_b + B) / 2 - 0.012, 0.10, WHITE, inner=0.30)
    tg = _lg(cv.w, BLUE_TOP, BLUE_BOT)
    tm = Image.new("L", (cv.w, cv.w), 0)
    ImageDraw.Draw(tm).rectangle(cv.box((0, 0, 1, title_b)), fill=255)
    tm = Image.composite(tm, Image.new("L", (cv.w, cv.w), 0), wm)
    tl = Image.new("RGBA", (cv.w, cv.w), (0, 0, 0, 0))
    tl.paste(tg, (0, 0), tm)
    cv.paste(tl)
    from icon_parts import rounded_mask as _rm
    bd = _rm(cv.w, rad, 0.0, cv.ss)
    ring = Image.new("RGBA", (cv.w, cv.w), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rd.rounded_rectangle(cv.box((L, T, R, B)), radius=cv.p(rad),
                         outline=(0x2B, 0x6F, 0xC4, 255), width=max(1, cv.p(0.008)))
    cv.paste(ring)
    return cv.out()


def _03_folder_chip(size):
    """3 文件夹 + 芯片：AI 算力内嵌在文件夹里，科技硬核。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x3A, 0x7B, 0xE0), (0x14, 0x3C, 0x8F), diag=False))
    folder(cv, 0.11, 0.26, 0.89, 0.84, (0xF4, 0xF8, 0xFF, 255),
           (0xCF, 0xDF, 0xF6, 255), (0xB4, 0xCB, 0xEC, 255), r=0.035)
    # 芯片压在文件夹前板上：本体用深蓝、引脚用更深的蓝、核心亮青，
    # 三层在浅色文件夹上有足够对比，16px 下也还看得出是个方芯。
    chip(cv, 0.50, 0.545, 0.225, (0x1B, 0x5C, 0xBE, 255), (0x14, 0x3C, 0x8F, 255),
         (0x7C, 0xE9, 0xFF, 255))
    spark(cv, 0.82, 0.235, 0.115, GOLD, inner=0.26)
    return cv.out()


def _04_folder_bolt(size):
    """4 文件夹 + 闪电：AI 提速语义，斜切构图有速度感。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x2E, 0x4A, 0x7A), (0x11, 0x1F, 0x38)))
    folder(cv, 0.12, 0.32, 0.80, 0.80, (0xF6, 0xF9, 0xFF, 255),
           (0xD2, 0xE0, 0xF6, 255), (0xB6, 0xCC, 0xEC, 255), r=0.035)
    bolt(cv, 0.735, 0.545, 0.235, GOLD, rot=0.12)
    return cv.out()


def _05_list_magnet(size):
    """5 文件列表 + 磁吸星火：星火落在列表右端，像被「AI 接管」的那一列。"""
    cv = Canvas(size)
    fine = size >= 32
    # 小尺寸下浅底 + 白卡会糊成一片，改用蓝底让卡片边界留住
    if size <= 24:
        cv.paste(tile_bg(cv, (0x2E, 0x74, 0xD0), (0x14, 0x38, 0x78)))
        card = (0xF2, 0xF8, 0xFF, 255)
        head = (0x8F, 0xBC, 0xEF, 255)
    else:
        cv.paste(tile_bg(cv, (0xF2, 0xF7, 0xFF), (0xD3, 0xE4, 0xFA), diag=False))
        card = WHITE
        head = (0x2B, 0x7B, 0xD6, 255)
    cv.rrect((0.12, 0.16, 0.88, 0.84), 0.055, fill=card)
    cv.rrect((0.12, 0.16, 0.88, 0.30), 0.055, fill=head)
    cv.rect((0.12, 0.25, 0.88, 0.30), fill=head)
    if fine:
        file_rows(cv, 0.175, 0.355, 0.62, 5, row_h=0.045, gap=0.028,
                  icon_col=(0x8F, 0xBC, 0xEF, 255),
                  bar_col=(0xD9, 0xE7, 0xF7, 255))
    spark(cv, 0.745, 0.52, 0.145, (0x8B, 0x5C, 0xF6, 255), inner=0.26)
    spark(cv, 0.845, 0.68, 0.065, (0xC4, 0xB5, 0xFD, 255), inner=0.26)
    return cv.out()


def _06_tree_robot(size):
    """6 导航树 + 机器人：AI 作为右侧助手直接出现在窗格里。"""
    cv = Canvas(size)
    fine = size >= 32
    cv.paste(tile_bg(cv, (0x59, 0xA5, 0xF5), (0x1E, 0x4F, 0xA8), diag=False))
    cv.rrect((0.10, 0.16, 0.90, 0.84), 0.06, fill=WHITE)
    cv.rrect((0.10, 0.16, 0.90, 0.30), 0.06, fill=(0x24, 0x6F, 0xC0, 255))
    cv.rect((0.10, 0.25, 0.90, 0.30), fill=(0x24, 0x6F, 0xC0, 255))
    cv.rect((0.10, 0.30, 0.40, 0.84), fill=PAPER)
    if fine:
        tree_items(cv, 0.145, 0.355, 0.215, 4, row_h=0.036, gap=0.026)
    robot(cv, 0.655, 0.545, 0.155, (0x8B, 0x5C, 0xF6, 255), WHITE,
          (0x6D, 0x28, 0xD9, 255))
    return cv.out()


def _07_folder_search_ai(size):
    """7 文件夹 + 放大镜 + 星火：AI 智能搜索，最贴「找文件」的真实使用场景。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x4F, 0xA8, 0xF0), (0x1A, 0x4B, 0xA0)))
    folder(cv, 0.11, 0.30, 0.72, 0.78, WHITE, (0xD3, 0xE4, 0xFA, 255),
           (0xB6, 0xCC, 0xEC, 255), r=0.035)
    magnifier_spark(cv, 0.685, 0.475, 0.175, (0xFF, 0xFF, 0xFF, 255),
                    (0xCC, 0xE4, 0xFF, 255), GOLD)
    return cv.out()


def _08_folder_pencil_ai(size):
    """8 文件夹 + 铅笔 + 星火：AI 帮忙批量整理 / 重命名。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x7A, 0xB8, 0xF0), (0x2A, 0x5A, 0xA8)))
    folder(cv, 0.12, 0.31, 0.87, 0.79, (0xFF, 0xFF, 0xFF, 255),
           (0xDC, 0xE8, 0xFA, 255), (0xC0, 0xD4, 0xF0, 255), r=0.035)
    pencil_spark(cv, 0.545, 0.535, 0.215, (0xFF, 0xC9, 0x4D, 255),
                 (0xF5, 0xA6, 0x23, 255), GOLD)
    return cv.out()


CANDIDATES_A = [
    ("01 文件夹 + 星火", "最直白：文件夹主体 + 金色星火，16px 也能认", _01_folder_spark),
    ("02 三栏窗口 + AI 面板", "现役方案，作为对照组", _02_panes_ai),
    ("03 文件夹 + 芯片", "AI 算力内嵌，硬核科技感", _03_folder_chip),
    ("04 文件夹 + 闪电", "AI 提速，斜切构图有速度感", _04_folder_bolt),
    ("05 文件列表 + 星火", "列表右端被星火接管", _05_list_magnet),
    ("06 导航树 + 机器人", "AI 助手出现在窗格右侧", _06_tree_robot),
    ("07 文件夹 + 放大镜", "AI 智能搜索，最贴使用场景", _07_folder_search_ai),
    ("08 文件夹 + 铅笔", "AI 批量整理 / 重命名", _08_folder_pencil_ai),
]
