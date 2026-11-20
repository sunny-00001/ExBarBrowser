#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""组件唯一性静态校验（架构规则 R6/R7/R11/R12/R13）。

## 解决什么问题

人工审查能发现"两个函数长得一样"，但无法阻止第三次复制 —— 下一个改动者
不会先去翻 18,000 行找有没有现成的。所以把"某概念恰有一份实现"写成检查项，
违例时 CI 直接失败，并且报错信息里带上**应该调用的那个入口**。

这比"文本相似度查重"有效：成熟副本往往已被改名、加参数、换了循环写法，
查重看不见；但"CreatePen 出现在 eb_ui.cpp 之外"是确定的。

## 两类检查

  UNIQUE   某符号/调用点**全仓只允许 N 处**（N 通常为 1），并声明正确宿主。
  FORBID   某些 API 只允许出现在指定文件（白名单式）。

## 用法

    python tools/check_components.py           # 全量
    python tools/check_components.py --list    # 只列出当前计数
退出码：0 = 通过；1 = 有违例。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# ---------------------------------------------------------------------------
# UNIQUE：某概念必须恰好一处实现
#
#   name      人读名字
#   pat       匹配"实现定义"的正则（跑在去注释的源码上）
#   allow     允许出现的文件（相对 src/），通常就是宿主
#   why       为什么必须唯一 + 该调什么
# ---------------------------------------------------------------------------
UNIQUE = [
    dict(name="双缓冲绘制", pat=r"\bCreateCompatibleDC\s*\(",
         allow={"eb_ui.cpp"},
         why="统一用 eb::PaintBuffered(hwnd, bg, draw)，不要自己建离屏 DC"),

    dict(name="分隔线", pat=r"\bMoveToEx\s*\(",
         allow={"eb_ui.cpp", "tab_strip_paint.cpp"},
         why="统一用 eb::DrawSeparator；tab_strip 的 ✕/＋ 是唯一的折线例外"),

    dict(name="实心画刷", pat=r"\bCreateSolidBrush\s*\(",
         allow={"theme.cpp", "eb_ui.cpp"},
         why="画刷由 Theme 持有（brHover/brListSel/...），临时填充用 eb::FillRectColor"),

    dict(name="钢笔", pat=r"\bCreatePen\s*\(",
         allow={"eb_ui.cpp", "tab_strip_paint.cpp"},
         why="统一用 eb::DrawSeparator / eb::DrawChevron"),

    dict(name="窗口类注册", pat=r"\bRegisterClassW\s*\(",
         allow={"platform_win.cpp"},
         why="统一用 win::RegisterClassOnce(name, WndProc, style, icon)"),

    dict(name="工具提示", pat=r"\bTOOLTIPS_CLASSW\b",
         allow={"tooltip_host.cpp"},
         why="统一用 eb::TooltipHost::Attach + SetTools"),

    dict(name="修饰键读取", pat=r"^\s*int\s+CurrentMods\s*\(\s*\)\s*\{",
         allow={"eb_ui.cpp", "input_router.cpp"},
         why="统一用 eb::CurrentMods()；input_router 的带 AltGr 特判，保留自身一份"),

    dict(name="诊断日志落盘", pat=r"ExBarBrowser\.log",
         allow={"log.cpp"},
         why="统一用 LogDbg / LogLine（log.cpp）"),

    dict(name="剪贴板文本", pat=r"\bSetClipboardData\s*\(",
         allow={"clipboard.cpp"},
         why="统一用 SetClipboardText(owner, text)"),

    dict(name="UTF-8 转换", pat=r"\bMultiByteToWideChar\s*\(|\bWideCharToMultiByte\s*\(",
         allow={"str.cpp"},
         why="统一用 str::ToUtf8 / str::FromUtf8"),

    dict(name="图标居中绘制", pat=r"\bDrawIconEx\s*\(",
         allow={"eb_ui.cpp"},
         why="统一用 eb::DrawIconCentered / DrawIconFit / DrawIconVCentered"),

    dict(name="模态消息泵", pat=r"GetMessageW\s*\(\s*&msg\s*,\s*nullptr\s*,\s*0\s*,\s*0\s*\)",
         allow={"app.cpp", "popup_list.cpp", "prompt.cpp"},
         why="对话框/弹层各自一份是过渡态；B5 收成 container::RunModal"),

    # 注意：shell_ops.cpp 的 GlobalAlloc(GMEM_MOVEABLE) 是 OLE STGMEDIUM 的
    # DROPEFFECT 载体（CFSTR_PREFERREDDROPEFFECT），与剪贴板文本无关，白名单放行。
    dict(name="剪贴板复制样板", pat=r"\bGlobalAlloc\s*\(\s*GMEM_MOVEABLE",
         allow={"clipboard.cpp", "shell_ops.cpp"},
         why="剪贴板**文本**统一用 SetClipboardText；OLE STGMEDIUM 不受此限"),
]

# ---------------------------------------------------------------------------
# FORBID：某 API 只允许出现在白名单文件
#   pat / allow / why 同上；区别只是语义（这些是"结构性禁令"而非"唯一实现"）
# ---------------------------------------------------------------------------
FORBID = [
    dict(name="载体自绘（R13）", pat=r"\bCreateCompatibleBitmap\s*\(",
         allow={"eb_ui.cpp"},
         why="栏/面板只组合组件；绘制走 eb::PaintBuffered"),

    dict(name="绕过 eb::DrawChevron 自绘箭头",
         pat=r"MoveToEx[^;]*;\s*$",
         allow={"eb_ui.cpp", "tab_strip_paint.cpp"},
         why="箭头一律 eb::DrawChevron（字形），折线只在 tab_strip 的 ✕/＋"),
]

# 不参与扫描的文件（备份/生成物）
SKIP = {"common.h.gch", "keymap.gen.h"}


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释，避免注释里的 API 名误判。

    字符串字面量里的 // 会被误删，但本项目里没有"字符串含 // 且后面是 API 名"
    的情形，接受这个近似。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = []
    for line in text.splitlines():
        in_str = False
        quote = ""
        cut = len(line)
        i = 0
        while i < len(line) - 1:
            c = line[i]
            if in_str:
                if c == "\\":
                    i += 2
                    continue
                if c == quote:
                    in_str = False
            elif c in ('"', "'"):
                in_str = True
                quote = c
            elif c == "/" and line[i + 1] == "/":
                cut = i
                break
            i += 1
        out.append(line[:cut])
    return "\n".join(out)


def sources():
    for p in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.h")):
        if p.name in SKIP:
            continue
        yield p


def scan():
    files = {p.name: strip_comments(p.read_text(encoding="utf-8", errors="replace"))
             for p in sources()}
    fails = []

    for rule in UNIQUE + FORBID:
        hits = []
        rx = re.compile(rule["pat"], re.M)
        for name, text in files.items():
            n = len(rx.findall(text))
            if n:
                hits.append((name, n))
        stray = [(n, c) for n, c in hits if n not in rule["allow"]]
        if stray:
            fails.append((rule, stray))

    return fails, files


def main():
    list_only = "--list" in sys.argv
    fails, files = scan()

    if list_only:
        for rule in UNIQUE + FORBID:
            rx = re.compile(rule["pat"], re.M)
            row = [(n, len(rx.findall(t))) for n, t in files.items() if rx.search(t)]
            print(f"[{rule['name']}] " + (", ".join(f"{n}×{c}" for n, c in row) or "（无）"))
        return 0

    if not fails:
        print(f"OK：{len(UNIQUE)} 条唯一性规则、{len(FORBID)} 条禁令，全部通过。")
        return 0

    print(f"FAIL：{len(fails)} 条规则被违反\n")
    for rule, stray in fails:
        print(f"  ✗ {rule['name']}")
        for name, cnt in stray:
            print(f"      {name}  ×{cnt}")
        print(f"      → {rule['why']}")
        print(f"      允许：{', '.join(sorted(rule['allow']))}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
