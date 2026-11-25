#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图标候选组 B（9–16 号）。

B 组比 A 组更大胆：换底色、换笔触、换符号系统，用来拉开可选范围。
同样守一条底线：文件夹/文件列表/窗口这三类「资源管理器信号」必须占主导，
AI 符号只做点缀，不能喧宾夺主。
"""
from PIL import Image, ImageDraw
from icon_parts import (Canvas, tile_bg, shadow_under, spark, bolt, folder,
                        file_rows, tree_items, chip, brain_nodes, robot,
                        pencil_spark, gear_spark, magnifier_spark,
                        rounded_mask, lin_gradient, iso_folder, drawer_cabinet)

WHITE = (255, 255, 255, 255)
GOLD = (0xFF, 0xD1, 0x66, 255)
LIME = (0x9B, 0xF6, 0xA0, 255)


def _09_bold_folder(size):
    """9 粗描边文件夹 + 星火：厚重单色描边风，像现代编辑器图标集。"""
    cv = Canvas(size)
    # 小尺寸下细描边会断，底色提亮一档保住轮廓
    bg = (0x2A, 0x33, 0x42) if size <= 24 else (0x1E, 0x24, 0x2E)
    cv.paste(tile_bg(cv, bg, (0x0D, 0x11, 0x18)))
    lw = 0.055 if size <= 24 else 0.045
    ink = (0x8C, 0xD4, 0xFF, 255) if size <= 24 else (0x6C, 0xC2, 0xFF, 255)
    folder(cv, 0.16, 0.30, 0.84, 0.78, (0, 0, 0, 0),
           (0, 0, 0, 0), (0, 0, 0, 0), r=0.04)
    # 重画：只描边不填充
    cv.d = ImageDraw.Draw(cv.im)
    tab = [(0.16, 0.30), (0.16 + 0.68 * 0.42, 0.30),
           (0.16 + 0.68 * 0.48, 0.38), (0.84, 0.38)]
    cv.d.line([(cv.p(x), cv.p(y)) for x, y in tab], width=cv.p(lw),
              fill=ink, joint="curve")
    cv.d.line([(cv.p(x), cv.p(y)) for x, y in
               [(0.84, 0.38), (0.84, 0.78), (0.16, 0.78), (0.16, 0.30)]],
              width=cv.p(lw), fill=ink, joint="curve")
    spark(cv, 0.80, 0.26, 0.135, GOLD, inner=0.26)
    return cv.out()


def _10_cube_folder(size):
    """10 文件夹 + 云 + 星火：AI 云同步/云端文件，语义具体且轮廓独特。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x5A, 0x3A, 0xC4), (0x24, 0x18, 0x74)))
    folder(cv, 0.11, 0.40, 0.89, 0.83, (0xF2, 0xEE, 0xFF, 255),
           (0xD9, 0xCF, 0xFA, 255), (0xC2, 0xB4, 0xF6, 255), r=0.035)
    # 云：三个圆 + 一个底座，云端在文件夹上方
    cx, cy = 0.52, 0.300
    for dx, dy, rr in ((-0.115, 0.020, 0.082), (0.0, -0.042, 0.108),
                       (0.115, 0.020, 0.085)):
        cv.ellipse((cx + dx - rr, cy + dy - rr, cx + dx + rr, cy + dy + rr),
                   fill=WHITE)
    cv.rrect((cx - 0.205, cy - 0.014, cx + 0.205, cy + 0.085), 0.049, fill=WHITE)
    spark(cv, 0.855, 0.185, 0.115, GOLD, inner=0.26)
    return cv.out()


def _11_bifold_folder_ai(size):
    """11 前后双层文件夹 + 星火：层次感，暗示多标签/多根目录。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0xE8, 0xF2, 0xFF), (0xC6, 0xDC, 0xF8), diag=False))
    folder(cv, 0.10, 0.40, 0.66, 0.80, (0xA9, 0xCB, 0xF2, 255),
           (0x92, 0xBA, 0xEA, 255), (0x86, 0xB2, 0xE8, 255), r=0.03)
    folder(cv, 0.30, 0.20, 0.90, 0.66, WHITE, (0xDC, 0xE8, 0xFA, 255),
           (0xC6, 0xD9, 0xF4, 255), r=0.03)
    spark(cv, 0.845, 0.175, 0.135, (0x8B, 0x5C, 0xF6, 255), inner=0.26)
    return cv.out()


def _12_window_spark_badge(size):
    """12 窗口 + 右上角星火徽章：最像 Windows 正式产品图标的稳妥路线。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x3E, 0x8E, 0xE0), (0x14, 0x44, 0x8E), diag=False))
    cv.rrect((0.12, 0.20, 0.88, 0.80), 0.055, fill=WHITE)
    cv.rrect((0.12, 0.20, 0.88, 0.335), 0.055, fill=(0x2B, 0x7B, 0xD6, 255))
    cv.rect((0.12, 0.28, 0.88, 0.335), fill=(0x2B, 0x7B, 0xD6, 255))
    if size >= 32:
        file_rows(cv, 0.18, 0.395, 0.60, 4, row_h=0.046, gap=0.026,
                  icon_col=(0x8F, 0xBC, 0xEF, 255),
                  bar_col=(0xD9, 0xE7, 0xF7, 255))
    spark(cv, 0.815, 0.235, 0.145, GOLD, inner=0.26)
    return cv.out()


def _13_neuron_folder(size):
    """13 文件夹 + 神经网络节点：AI 语义最直白的一种（节点连线）。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x0F, 0x3B, 0x4A), (0x06, 0x1A, 0x24)))
    folder(cv, 0.12, 0.40, 0.88, 0.84, (0xE9, 0xF6, 0xFF, 255),
           (0xBF, 0xDA, 0xEA, 255), (0xA2, 0xC6, 0xDC, 255), r=0.035)
    # 节点画在文件夹「上方」的空白处，避免被浅色文件夹吃掉对比
    brain_nodes(cv, 0.50, 0.255, 0.140, LIME, (0x5E, 0xD2, 0xC0, 255))
    return cv.out()


def _14_purple_folder_spark(size):
    """14 紫金文件夹 + 星火：深色主题下的高饱和撞色，桌面上最跳。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x2A, 0x1B, 0x46), (0x12, 0x0C, 0x22)))
    folder(cv, 0.13, 0.30, 0.87, 0.79, (0xA7, 0x8B, 0xFA, 255),
           (0x8B, 0x5C, 0xF6, 255), (0x7C, 0x3A, 0xED, 255), r=0.035)
    spark(cv, 0.775, 0.275, 0.145, GOLD, inner=0.26)
    return cv.out()


def _15_gear_folder_ai(size):
    """15 文件夹 + 齿轮 + 星火：AI 自动化批量操作，工具感强。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x46, 0x8C, 0xC8), (0x1A, 0x40, 0x70)))
    folder(cv, 0.11, 0.31, 0.79, 0.80, (0xF2, 0xF8, 0xFF, 255),
           (0xCF, 0xE1, 0xF6, 255), (0xB2, 0xCC, 0xEC, 255), r=0.035)
    gear_spark(cv, 0.735, 0.545, 0.185, (0x8B, 0x5C, 0xF6, 255), GOLD)
    return cv.out()


def _16_monogram_ai(size):
    """16 文件柜 + 星火：抽屉柜剪影独特，'归档管理'的资源管理器语义。"""
    cv = Canvas(size)
    cv.paste(tile_bg(cv, (0x6C, 0xC2, 0xFF), (0x1B, 0x5C, 0xBE)))
    drawer_cabinet(cv, 0.47, 0.52, 0.62,
                   body=(0x1B, 0x5C, 0xBE, 255),
                   drawer=(0xF0, 0xF7, 0xFF, 255),
                   handle=(0x2B, 0x7B, 0xD6, 255),
                   spark_col=GOLD)
    return cv.out()


CANDIDATES_B = [
    ("09 描边文件夹 + 星火", "厚重描边风，现代编辑器图标集质感", _09_bold_folder),
    ("10 文件夹 + 云 + 星火", "AI 云同步，语义具体、轮廓独特", _10_cube_folder),
    ("11 双层文件夹 + 星火", "层次感，暗示多根目录", _11_bifold_folder_ai),
    ("12 窗口 + 星火徽章", "最像 Windows 正式产品图标", _12_window_spark_badge),
    ("13 文件夹 + 神经网络", "AI 语义最直白（节点连线）", _13_neuron_folder),
    ("14 紫金文件夹 + 星火", "深色桌面上最跳的高饱和撞色", _14_purple_folder_spark),
    ("15 文件夹 + 齿轮 + 星火", "AI 自动化批量操作，工具感", _15_gear_folder_ai),
    ("16 文件柜 + 星火", "抽屉柜剪影，归档管理的资源管理器语义", _16_monogram_ai),
]
