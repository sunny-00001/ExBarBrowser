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
         allow={"ui_draw.cpp"},
         why="统一用 ui::PaintBuffered(hwnd, bg, draw)，不要自己建离屏 DC"),

    dict(name="分隔线", pat=r"\bMoveToEx\s*\(",
         allow={"ui_draw.cpp", "tab_strip_paint.cpp"},
         why="统一用 ui::DrawSeparator；tab_strip 的 ✕/＋ 是唯一的折线例外"),

    dict(name="实心画刷", pat=r"\bCreateSolidBrush\s*\(",
         allow={"theme.cpp", "ui_draw.cpp"},
         why="画刷由 Theme 持有（brHover/brListSel/...），临时填充用 ui::FillRectColor"),

    dict(name="钢笔", pat=r"\bCreatePen\s*\(",
         allow={"ui_draw.cpp", "tab_strip_paint.cpp"},
         why="统一用 ui::DrawSeparator / ui::DrawChevron"),

    dict(name="窗口类注册", pat=r"\bRegisterClassW\s*\(",
         allow={"platform_win.cpp"},
         why="统一用 win::RegisterClassOnce(name, WndProc, style, icon)"),

    dict(name="工具提示", pat=r"\bTOOLTIPS_CLASSW\b",
         allow={"tooltip_host.cpp"},
         why="统一用 eb::TooltipHost::Attach + SetTools"),

    # 修饰键读取：唯一实现在输入端（mouse_rules.cpp）—— 它读 GetKeyState，
    # 属输入状态而非绘制资源（R15）。input_router 的成员版本已改为转调它
    # （两个版本原本一字不差，"需按焦点抑制"的放行理由不成立）。
    dict(name="修饰键读取", pat=r"^\s*int\s+CurrentMods\s*\(\s*\)\s*\{",
         allow={"mouse_rules.cpp"},
         why="统一用 mouse_rules.h 的 CurrentMods()（含 AltGr 特判）；"
             "不要在别处再写一份 GetKeyState 三连"),

    dict(name="诊断日志落盘", pat=r"ExBarBrowser\.log",
         allow={"log.cpp", "identity.h"},
         why="统一用 LogDbg / LogLine（log.cpp）；identity.h 只是文件名声明，不落盘"),

    dict(name="剪贴板文本", pat=r"\bSetClipboardData\s*\(",
         allow={"clipboard.cpp"},
         why="统一用 SetClipboardText(owner, text)"),

    dict(name="UTF-8 转换", pat=r"\bMultiByteToWideChar\s*\(|\bWideCharToMultiByte\s*\(",
         allow={"str.cpp"},
         why="统一用 str::ToUtf8 / str::FromUtf8"),

    dict(name="图标居中绘制", pat=r"\bDrawIconEx\s*\(",
         allow={"ui_draw.cpp"},
         why="统一用 ui::DrawIconCentered / DrawIconFit / DrawIconVCentered"),

    dict(name="模态消息泵", pat=r"GetMessageW\s*\(\s*&msg\s*,\s*nullptr\s*,\s*0\s*,\s*0\s*\)",
         allow={"app.cpp", "popup_list.cpp", "prompt.cpp"},
         why="对话框/弹层各自一份是过渡态；B5 收成 container::RunModal"),

    # 注意：shell_ops.cpp 的 GlobalAlloc(GMEM_MOVEABLE) 是 OLE STGMEDIUM 的
    # DROPEFFECT 载体（CFSTR_PREFERREDDROPEFFECT），与剪贴板文本无关，白名单放行。
    dict(name="剪贴板复制样板", pat=r"\bGlobalAlloc\s*\(\s*GMEM_MOVEABLE",
         allow={"clipboard.cpp", "shell_ops.cpp"},
         why="剪贴板**文本**统一用 SetClipboardText；OLE STGMEDIUM 不受此限"),

    # ---- 品牌串唯一性（docs/NAMING.md B1）----
    # 独立前这些字面量散在 9 个文件里，改名要动 9 处。现在只有 identity.h 定义。
    dict(name="品牌串（正式名）", pat=r'L"ExBarBrowser"',
         allow={"identity.h"},
         why="品牌串唯一来源是 identity.h 的 id::kBrand；其他文件用常量"),

    dict(name="品牌串（显示名）", pat=r'L"ExBar"',
         allow={"identity.h"},
         why="显示名唯一来源是 identity.h 的 id::kBrandShort；其他文件用常量"),

    dict(name="窗口类名字面量", pat=r'L"ExBarBrowser_',
         allow={"identity.h"},
         why="窗口类名只能来自 identity.h（规则 B1）；新增类要先在 identity.h 登记"),

    # ---- 废弃的来源前缀（docs/NAMING.md N1 / N2）----
    # `eb` = ExBar，是独立前那个 Windhawk mod 的缩写。工程独立后它成了"用上一个
    # 项目的前缀写自己的代码"（Qt 社区对 Q 前缀的同一结论）。2026-11-20 已全部
    # 改为 ui / 领域文件名，这条规则防它回来。
    dict(name="废弃命名空间 eb", pat=r"namespace\s+eb\s*\{",
         allow=set(),
         why="`eb` 是上个项目的缩写；用领域命名空间（ui / ico / nav / win / input…）"),

    dict(name="废弃命名空间 eb 的引用", pat=r"\beb::",
         allow=set(),
         why="`eb` 命名空间已改名为 ui（ui_draw.h）；新代码不要再写 eb::"),

    dict(name="废弃文件名前缀 eb_", pat=r'#include\s+"eb_',
         allow=set(),
         why="eb_ui.* → ui_draw.*；eb_theme.h → ui_palette.h"),

    # ---- 模块入口必须被接线（防"建了入口当装饰"）----
    # 2026-11-20 审查发现 7 个入口零调用：MeasureText / CreateChild / CreatePopup /
    # ThemeControl / FontPx / Scale / IconSize / GlyphFace / IconGlyph / IconIdFromName。
    # 这类"声明了但没人用"的入口比重复实现更隐蔽 —— 查重抓不到，编译不报错。
    dict(name="文本测量", pat=r"\bGetTextExtentPoint32W\s*\(",
         allow={"ui_draw.cpp"},
         why="统一用 ui::MeasureText(hdc, font, text)；以前 8 处各写一遍 8 行样板"),

    dict(name="控件主题套用", pat=r"\bSetWindowTheme\s*\(",
         allow={"platform_win.cpp", "shell_view.cpp"},
         why="统一用 win::ThemeControl(h, dark)；shell_view 有 DirectUI 例外，见其注释"),
]

# ---------------------------------------------------------------------------
# FORBID：某 API 只允许出现在白名单文件
#   pat / allow / why 同上；区别只是语义（这些是"结构性禁令"而非"唯一实现"）
# ---------------------------------------------------------------------------
FORBID = [
    dict(name="载体自绘（R13）", pat=r"\bCreateCompatibleBitmap\s*\(",
         allow={"ui_draw.cpp"},
         why="栏/面板只组合组件；绘制走 ui::PaintBuffered"),

    dict(name="绕过 ui::DrawChevron 自绘箭头",
         pat=r"MoveToEx[^;]*;\s*$",
         allow={"ui_draw.cpp", "tab_strip_paint.cpp"},
         why="箭头一律 ui::DrawChevron（字形），折线只在 tab_strip 的 ✕/＋"),
]

# ---------------------------------------------------------------------------
# STALE：声明了公开入口，但没有任何调用点
#
#   这一类比"重复实现"更隐蔽：查重抓不到（只有一个实现），编译不报错，
#   代码评审也容易漏（头文件看着挺完整）。2026-11-20 的审查一次就找出 7 个：
#   MeasureText / CreateChild / CreatePopup / ThemeControl / FontPx / Scale /
#   IconSize / GlyphFace / IconGlyph —— 全部"写好了、但没人用"。
#
#   判据：符号在 src/*.cpp 里的**调用**次数 ≤ 1（那 1 次通常是它自己的定义行）。
#   若确实要给将来用，就把 entry 删掉并在代码里写注释说明；不要留着当装饰。
#
#   name     人读名字
#   symbol   要统计的标识符（正则片段）
#   home     定义所在的文件（统计时排除自身，只看外部调用）
#   why      该调哪里 / 为什么留着没用
# ---------------------------------------------------------------------------
STALE = [
    # GetUiFont / GlyphFace 已移入 ui::detail（2026-11-20）—— "应私有化"的建议已执行，
    # 不再列入本表：它们本就不该被外部调用，出现在这里只会产生永久提示。
    #
    # IconIdFromName 是**预留的公共接口**：数据驱动处（keymap 里写图标名）需要它。
    # 当前 keymap.json 不含图标名，所以零调用。这类"为将来的数据形态预留"的接口
    # 与"建了忘了接"不同 —— 保留在表里是为了在它真被接线时留下记录；
    # 若三个月后仍零调用，应当删除而不是继续挂账。
    dict(name="图标按名查表（预留）", symbol=r"\bIconIdFromName\s*\(", home="icon_glyph.cpp",
         why="给 keymap 的图标名用；当前数据形态不含图标名 → 若长期不用应删"),
]

# 不参与扫描的文件（备份/生成物）
SKIP = {"common.h.gch", "keymap.gen.h"}

# 非致命提示（"应该私有化"这类），不影响退出码
stale_warn: list[str] = []


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
    stale_warn.clear()

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

    # STALE：区分三态
    #  外部调用 > 0        → 已接线，通过
    #  仅 home 内自用      → 公开声明是多余的，应收回 anonymous/detail（警告，不失败）
    #  home 内也无人调用  → 真正的死代码（失败）
    for rule in STALE:
        rx = re.compile(rule["symbol"], re.M)
        # home 是 .cpp；同名的 .h 只是声明，也要排除，否则"声明 + 无调用"会被算成已接线
        own = {rule["home"], rule["home"].replace(".cpp", ".h")}
        external = sum(len(rx.findall(t)) for n, t in files.items() if n not in own)
        internal = sum(len(rx.findall(files.get(f, ""))) for f in own)
        if external == 0 and internal <= 1:
            # internal == 1 就是定义行本身（含参数类型提及则可能为 2，见下）
            fails.append((dict(rule, allow={rule["home"]},
                               name=f"死入口（{rule['name']}）"),
                          [(rule["home"], 0)]))
        elif external == 0:
            stale_warn.append(f"{rule['name']}：只在 {rule['home']} 内自用"
                              f"（{internal} 处），公开声明多余 —— {rule['why']}")

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
        print(f"OK：{len(UNIQUE)} 条唯一性规则、{len(FORBID)} 条禁令、"
              f"{len(STALE)} 条接线检查，全部通过。")
        for w in stale_warn:
            print(f"  提示  {w}")
        return 0

    print(f"FAIL：{len(fails)} 条规则被违反\n")
    for rule, stray in fails:
        print(f"  ✗ {rule['name']}")
        for name, cnt in stray:
            if cnt == 0:
                print(f"      {name}  外部调用 0 次")
            else:
                print(f"      {name}  ×{cnt}")
        print(f"      → {rule['why']}")
        if rule.get("allow"):
            print(f"      允许：{', '.join(sorted(rule['allow']))}")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
