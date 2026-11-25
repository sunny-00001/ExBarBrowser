#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""组件唯一性静态校验（架构规则 R6/R7/R11/R12/R13/R21）。

## 解决什么问题

人工审查能发现"两个函数长得一样"，但无法阻止第三次复制 —— 下一个改动者
不会先去翻 18,000 行找有没有现成的。所以把"某概念恰有一份实现"写成检查项，
违例时 CI 直接失败，并且报错信息里带上**应该调用的那个入口**。

这比"文本相似度查重"有效：成熟副本往往已被改名、加参数、换了循环写法，
查重看不见；但"CreatePen 出现在 eb_ui.cpp 之外"是确定的。

## 三类检查

  UNIQUE       某符号/调用点**全仓只允许 N 处**（N 通常为 1），并声明正确宿主。
  FORBID       某些 API 只允许出现在指定文件（白名单式）。
  INPUT_GUARD  输入纪律（R21）：**全仓**（含 tools/scripts/tests）禁止模拟输入与自动化抢焦点。
               与上面两类的区别是扫描范围 —— 出事的多半是验收脚本，而它在 src/ 之外。

## 用法

    python tools/check_components.py           # 全量
    python tools/check_components.py --list    # 只列出当前计数
退出码：0 = 通过；1 = 有违例。
"""
import json
import os
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
         allow={"ui_draw.cpp"},
         why="统一用 ui::DrawSeparator；✕/＋ 也是字形（ico::Close / ico::Add）"),

    dict(name="实心画刷", pat=r"\bCreateSolidBrush\s*\(",
         allow={"theme.cpp", "ui_draw.cpp"},
         why="画刷由 Theme 持有（brHover/brListSel/...），临时填充用 ui::FillRectColor"),

    dict(name="钢笔", pat=r"\bCreatePen\s*\(",
         allow={"ui_draw.cpp"},
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

    # 字体选入/还原（2026-11-23 收编）：以前 5 个自绘栏各写一遍
    # HGDIOBJ old = SelectObject(dc, theme_->font); ... SelectObject(dc, old);
    # 早退漏还原则后续串体。现在统一 ui::ScopedFont（作用域保证还原），
    # 单行文本走 DrawTextRow（内部已自带）。
    dict(name="裸字体选入", pat=r"\bSelectObject\s*\(\s*\w+\s*,\s*\w*(?:theme_|font)",
         allow={"ui_draw.cpp", "ui_draw.h"},
         why="整段共用一个字体用 ui::ScopedFont(hdc, font)（其唯一 SelectObject 在 ui_draw.h）；"
             "单行文本用 ui::DrawTextRow。裸 SelectObject 在早退路径上会漏还原，让后续绘制串体"),

    dict(name="模态消息泵", pat=r"GetMessageW\s*\(\s*&msg\s*,\s*nullptr\s*,\s*0\s*,\s*0\s*\)",
         allow={"app.cpp", "popup_list.cpp", "modal.cpp", "kb_hook.cpp"},
         why="对话框走 modal::Run（src/modal.h）；prompt/report 已收编，不要再各写一份。"
             "kb_hook.cpp 是钩子线程的专属消息循环（不是对话框模态泵），见 HookThread"),

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

    # ---- 日志入口唯一（R7）----
    # 曾同时存在 LogDbg（54 处）与 LogLine（31 处），实现逐字相同。两个名字、零语义
    # 差异，新代码不知道该用哪个。判据放在**头文件**：入口的唯一性就是"对外声明了几个"，
    # log.cpp 内部的 LogV 是私有辅助，不算入口。
    dict(name="日志入口唯一", pat=r"^void\s+(?!LogDbg\b)Log\w+\s*\(.*\);",
         allow=set(), headers_only=True,
         why="日志唯一入口是 LogDbg（log.h）；头文件里不要再声明同义的第二个名字"),

    # ---- 模块入口必须被接线（防"建了入口当装饰"）----
    # 2026-11-20 审查发现 7 个入口零调用：MeasureText / CreateChild / CreatePopup /
    # ThemeControl / FontPx / Scale / IconSize / GlyphFace / IconGlyph / IconIdFromName。
    # 这类"声明了但没人用"的入口比重复实现更隐蔽 —— 查重抓不到，编译不报错。
    dict(name="文本测量", pat=r"\bGetTextExtentPoint32W\s*\(",
         allow={"ui_draw.cpp"},
         why="统一用 ui::MeasureText(hdc, font, text)；以前 8 处各写一遍 8 行样板"),

    dict(name="控件主题套用", pat=r"\bSetWindowTheme\s*\(",
         allow={"platform_win.cpp", "shell_view.cpp", "shell_view_theme.cpp"},
         why="统一用 win::ThemeControl(h, dark)；shell_view 与 shell_view_theme 是同一个 DirectUI 例外，见其注释"),

    # ---- 大小写不敏感比较（2026-11-20 收编）----
    # str.h 从写下那天就有 ICmp / IEquals / IStartsWith，但**零外部调用** ——
    # 而同一时期 33 处直接写 _wcsicmp/_wcsnicmp，13 个文件各自为政。
    # 这是"入口建好没人用"的最典型样本：同一个头文件里 Trim/Lower 收编成功了，
    # 比较函数却从没被用过一次。
    #
    # 两条豁免：
    #   · str.cpp —— 那是唯一实现本体。
    #   · shell_ops.cpp —— 路径比较必须用 CompareStringOrdinal（ordinal 语义），
    #     不能用 locale 相关的 _wcsicmp（土耳其语 i/I 会判错）。它已改用 ordinal，
    #     但注释里提到 _wcsicmp 供后人对照，strip_comments 后不会命中，这里仍留着
    #     白名单以防将来有人在此文件里误用。
    dict(name="大小写不敏感比较", pat=r"\b_wcs(?:i|ni)cmp\s*\(",
         allow={"str.cpp", "shell_ops.cpp"},
         why="统一用 str::ICmp / str::IEquals / str::IStartsWith（str.h）；"
             "路径语义要 ordinal 比较时用 CompareStringOrdinal"),

    # ---- 视图档位表（2026-11-20 合并）----
    # app_commands.cpp 与 nav_bar_actions.cpp 曾各有一份逐字相同的 8 档表。
    # 这张表的**下标**同时是 view.mode 命令的 arg、菜单回传值、配置持久化值 ——
    # 任一处增删/换序而另一处没改，就是"菜单显示的和实际切的不一致"，无任何报错。
    # 纯数据重复查重抓不到，只能靠"某个特征值只允许出现在一个文件里"来守。
    dict(name="视图档位表", pat=r"\{FVM_THUMBNAIL,\s*256,",
         allow={"view_modes.h"},
         why="唯一来源是 view_modes.h 的 viewmode::kModes；下标是命令 arg 契约，不能有第二份"),

    # ---- 「再点即收」状态机（2026-11-21）----
    # 弹层是模态的：点第二下时旧弹层被 WA_INACTIVE 关掉（PostMessage(WM_CLOSE)），
    # 紧接着同一次点击的 WM_LBUTTONDOWN 又调一次 Show* —— 而 WM_CLOSE 还在队列里，
    # 于是重入建出第二个菜单。用户看到"点了两下，菜单一直在，收不掉"。
    #
    # 修法是一份全局单槽记录（popup_toggle.cpp）。这条规则守的是"不要另写一份"：
    # 各栏自己记一个 lastOpenTick_ 是很容易想到的本地方案，但那样每个栏一套窗口期、
    # 一套清记录时机，"有的地方能收起、有的地方收不掉"就是这么来的。
    dict(name="再点即收的状态机", pat=r"\bWasJustOpened\s*\(|\bNoteOpened\s*\(",
         allow={"popup_toggle.cpp", "popup_toggle.h", "popup_list.cpp",
                "nav_bar_actions.cpp"},
         why="唯一实现在 popup_toggle.cpp；各栏不要自己记 lastOpenTick_，"
             "经由 Show* 的 toggleKey 参数接入即可"),

    # ---- 取视图接口（2026-11-20 收编）----
    # `IShellView*` → `IFolderView2` 的 QI 模板以前只在 shell_ops.cpp 的匿名
    # 命名空间里，外部拿不到，于是 shell_view.cpp / view_stats.cpp 各自手写。
    # 手写版本有个坑：`reinterpret_cast<void**>(&fv2)` **不会**走 ComPtr 的
    # operator&（那个会先 Reset），fv2 非空时直接泄漏。
    dict(name="取 IFolderView2", pat=r"QueryInterface\(IID_IFolderView2",
         allow={"shell_ops.cpp"},
         why="统一用 shellops::FolderView2(view) / ShellFolder(fv2)；"
             "手写 QI 容易漏掉 ComPtr 的 Reset 语义而造成引用泄漏"),

    # ---- 画一行文本（2026-11-20 收编）----
    # MeasureText 只收了"量"，没收"画"，于是 7 个文件 11 处各写一遍
    # SelectObject + SetTextColor + DrawTextW + 还原。每处都是一个"忘记还原就
    # 让后续绘制串体"的机会。规则只守**直接调 DrawTextW** 的写法；
    # DrawGlyph 内部那一次（画单个字形码位）是 ui_draw.cpp 自己，白名单覆盖。
    dict(name="画一行文本", pat=r"\bDrawTextW\s*\(",
         allow={"ui_draw.cpp"},
         why="统一用 ui::DrawTextRow(hdc, font, text, rc, color, fmt)；"
             "它保证字体一定被还原，不会让后续绘制串体"),

    # ---- 日志/审计的"追加 + 按大小轮转"（2026-11-20 收编）----
    # log.cpp（4MB/500 行，UTF-16 LE）与 CommandBus::Audit（2MB/200 行，UTF-8）
    # 的轮转段逐字相同：GetFileAttributesExW + 判 hi/lo + MoveFileEx(.1)。
    # 编码不同是需求（调试日志 UTF-16 便于 Get-Content 直读，审计 UTF-8 便于 grep），
    # 现已收进 tf::AppendLine。规则守的是轮转那一段，不是编码。
    dict(name="日志轮转", pat=r"MoveFileExW?\([^;]*\.1",
         allow={"text_file.cpp"},
         why="统一用 tf::AppendLine(path, bytes, rotateAtBytes, checkEveryN)；"
             "编码由调用方给，轮转逻辑只一份"),

    # ---- actor 身份类型（2026-11-21 收编）----
    #
    # 以前 actor 是 `const wchar_t*`，"是不是机器"只认 `actor != L"ui"`。
    # 于是「系统菜单点的」「快捷键触发的」「拖进来的」全被判成**机器** ——
    # 而 fromMachine() 是撤销边界、审计归类、审批强度、人工改写率的共同前提。
    # 后果是用户按 Ctrl+Z 撤书签时被"只撤到机器那一步"的规则拦住（撤销看起来坏了）。
    #
    # 现在 actor 是 `Actor`（kind + 来源标签），判定只看 `ActorKind`。
    # 下面两条规则守的是"别退回去"：
    #   ① 调用点的 actor 必须是 Actor::Xxx()，不能是字符串字面量；
    #   ② fromMachine() 的定义只能有一处（不能在别处按字符串重新判一次）。
    dict(name="actor 身份判定（唯一定义）",
         pat=r"\bbool\s+fromMachine\s*\(\s*\)\s*const",
         allow={"commands.h"},
         why="统一用 CommandBus::fromMachine()（判 ActorKind，不看字符串）；"
             "任何地方重新按字符串判\"是不是机器\"，都会把键盘/系统菜单/拖拽误判成机器"),

    dict(name="actor 传字符串字面量",
         pat=r"\bInvoke(?:Legacy|ByName|ByNameLegacy|ByNameResult|Result)?\s*\("
             r"[^;]*?,\s*L\"[^\"]*\"\s*\)",
         allow=set(),
         why="actor 必须是 Actor::Ui()/Actor::Key()/Actor::Ai()… —— "
             "传字符串字面体会让\"是不是机器\"的判定退化成字符串比较（此前就是这么错的）"),

    # ---- 长路径（2026-11-21 本轮新增）----
    # >260 字符的路径在 CreateFileW / FindFirstFileW / GetFileAttributesExW 上
    # 会直接失败，而调用方常把失败当成"文件不存在"或"权限不足" ——
    # 深目录静默读不到、体积统计算成 0 且仍标记完整。
    # 唯一入口是 tf::ToExtended(path)（加 \\?\ 前缀，UNC 转 \\?\UNC\，幂等，
    # 命名空间原样跳过）。
    #
    # ai_server.cpp 的 CreateFileW 开的是命名管道（\\.\pipe\...），
    # 不是文件系统路径，加扩展前缀会彻底打不开 —— 白名单放行。
    # mcp_stdio.cpp 同理：它连的是 MCP stdio 的命名管道
    # （ai::CurrentPipeName() 返回的已是 "\\\\.\\pipe\\ExBarBrowser_ai_*"）。
    # 除命名管道外，所有文件系统 API 都必须过 tf::ToExtended。
    # ai_server.cpp 的 CreateFileW 开的是 \\.\pipe\...，加扩展前缀会彻底打不开，
    # 只对这一处放行；同文件的 DeleteFileW（令牌/发现文件）已走 ToExtended。
    # L"NUL" 同理：它是**设备名**不是文件系统路径（\\?\NUL 打不开），
    # dev_proc.cpp 用它给子进程的 stdin 一个空设备 —— 只放行这个字面量。
    dict(name="长路径前缀", pat=r"\bCreateFileW\s*\(\s*(?!tf::ToExtended|ToExtended|L\"NUL\")",
         allow={"ai_server.cpp", "mcp_stdio.cpp"},
         why="文件 API 一律传 tf::ToExtended(path)；命名管道与 NUL 设备是唯一例外"
             "（它们不是文件系统路径）"),
    dict(name="长路径前缀（其余文件 API）",
         pat=r"\b(CreateDirectoryW|CopyFileW|DeleteFileW|MoveFileExW|FindFirstFileW|"
             r"GetFileAttributesW|GetFileAttributesExW|SetFileAttributesW)\s*\(\s*"
             r"(?!tf::ToExtended|ToExtended)",
         allow=set(),
         why="CreateDirectoryW / CopyFileW / GetFileAttributesW 等同样受 MAX_PATH 限制；"
             "只守 CreateFileW 会漏掉 EnsureDir、复制、存在性判断和体积扫描"),
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
         allow={"ui_draw.cpp"},
         why="箭头一律 ui::DrawChevron（字形）；本仓已无折线图标例外"),

    # 图标尺寸必须是令牌，不能是"某个栏自己想画多大"。
    # 背景：字形表（icon_glyph）管码位、DrawGlyph 管画法，唯独"画多大"没有归属，
    # 于是 S(9)/S(11)/S(12)/S(14)/S(16) 散在 6 个文件里，同一行三个图标来自三个数字。
    # 现在统一走 Theme::iconChevron/iconGlyph/iconCompact/iconSmall。
    # 判据只抓"直接把 S(数字) 交给图标/字形原语"，不动普通布局尺寸（栏高、留白）。
    dict(name="图标尺寸绕过尺寸令牌",
         pat=r"(?:DrawGlyph|DrawChevron|DrawIconCentered|DrawIconVCentered|DrawIconFit)"
             r"\s*\([^;)]*?\bS\s*\(\s*\d+\s*\)",
         allow=set(),
         why="图标尺寸一律用 Theme::iconChevron/iconGlyph/iconCompact/iconSmall，"
             "不要在栏里写 S(裸数字) —— 同一行三个图标各写一个数就是'大小不协调'的成因"),

    # 显示名收口（2026-11-21）：历史/最近访问只存 "::{GUID}" 这类解析串，
    # 菜单行若把同一个变量同时当 label 和 path，用户就会看到裸 CLSID。
    # 统一入口是 ico::DisplayName(path)，path 仍原样作为导航目标。
    dict(name="路径行 label 直接复用 path",
         pat=r"\bAddPath\s*\(\s*([A-Za-z_]\w*)\s*,\s*\1\s*,",
         allow=set(),
         why="统一用 ico::DisplayName(path) 生成 label；path 只能作为导航目标，"
             "不能同时当给人看的文字"),

    # 菜单/弹层组件化（本轮新增）：业务代码只描述"有什么动作"，
    # PopupRow 八字段与 ShowPopupList 只剩 menu.cpp 这一层组件实现。
    # 旧写法每加一个菜单都要重新记一遍图标槽、分隔行 data、初态行号，
    # 这正是"同一个菜单在两处表现不同"的来源；机械守住宿主，新增菜单只能走 Menu。
    dict(name="业务侧直接建菜单行",
         pat=r"\bShowPopupList\s*\(|\bPopupRow\s*\{",
         allow={"menu.cpp", "popup_list.cpp", "popup_list.h"},
         why="自绘平铺菜单统一用 ui::Menu 的 Add/AddChecked/AddPath/Separator/Show；"
             "只有 menu.cpp 可以组装 PopupRow 并调用唯一渲染器 ShowPopupList"),

    # 双入口判据（2026-11-21）：无参数命令从 AI 过来也是 `{}`，用 args.IsEmpty()
    # 当"人侧信号"，handler 就会弹模态窗口，而 AI 那侧在等一个永远不会来的应答 ——
    # 实测 session.list 从管道调用返回 {"ok":false,"error":"timeout"}，
    # tool.dupfinder 传 {} 同理。统一用 IsHumanCall(args)（空参数 **且** 不是机器调用）。
    # 判据只抓两种会变成"人机判定"的写法；DescribeArgs 里那种纯粹问"空不空"的
    # `if (args.IsEmpty()) return ...` 不拦。
    dict(name="双入口只看空参数判人机",
         pat=r"human\s*=\s*args\.IsEmpty\s*\(\s*\)"
             r"|if\s*\(\s*args\.IsEmpty\s*\(\s*\)\s*\)\s*\{",
         allow=set(),
         why="用 IsHumanCall(args)：只看空参数会把 AI 传来的 {} 当成人在操作，"
             "走人侧分支弹模态窗口，管道调用卡到超时"),

    # 路径图标同样收口：菜单与文件夹树用 ico::Cached（进程内共享，弹层不销毁）。
    # 仍直接 Resolve 的只有长生命周期 chrome 图标：标签栏/书签栏/导航栏各管自己的缓存，
    # DPI 变化时要整批重取，生命周期语义与菜单不同，不能混成 Cached。
    dict(name="菜单/树路径图标绕过缓存",
         pat=r"\bico::Resolve\s*\(",
         allow={"icon_resolve.cpp", "tab_strip.cpp", "bookmark_bar.cpp", "nav_bar.cpp"},
         why="菜单和文件夹树统一走 ui::Menu::AddPath / ico::Cached（或 TreeNode + Cached）；"
             "弹层只借 HICON、不 Destroy，避免重复创建与二次释放"),
]

# ---------------------------------------------------------------------------
# INPUT_GUARD：输入纪律（R21）—— 全仓禁止模拟输入 / 抢焦点
#
#   这条和上面几类的区别是**扫描范围**：不只是 src/，还包括 tools/ scripts/ tests/
#   —— 出事的多半是"验收脚本"，而它们在 src/ 之外。
#
#   为什么必须机械守：模拟输入的代价不出现在编译期，也不出现在脚本自己的日志里，
#   它出现在**用户的下一次点击**上（光标被挪走、焦点被抢到别的窗口）。
#   一旦有人为了"跑一遍快捷键冒烟"写下 SendInput，这段代码会长期留在 tools/ 里，
#   下一个人跑它就再抢一次焦点 —— 而且它每次都"验证通过"，看不出问题。
#
#   两类：
#     injection  注入系统输入流或移动物理光标 —— 零容忍，allow 为空。
#     focus      改变前台/焦点。只在**被测窗口不是用户正在用的那个**时才可能成立，
#                因此 allow 里必须写清是哪个脚本、为什么它自己起的实例可以。
# ---------------------------------------------------------------------------
INPUT_GUARD = [
    dict(name="模拟输入注入（R21）",
         # 作用域 all：产品代码与脚本都不许。src/ 当前 0 处，将来也不该出现。
         scope="all",
         pat=r"\b(SendInput|SendInputEx|mouse_event|keybd_event|SetCursorPos|BlockInput|"
             r"AttachThreadInput)\s*\(",
         allow=set(),
         why="禁止模拟鼠标/键盘（会抢走用户光标与焦点）。"
             "要触发动作走 CommandBus::Invoke；进程外走 AI 命名管道 tools/call；"
             "只是要看界面用 scripts/shot.py（PrintWindow 只读）"),

    # 【2026-11-21 收紧】合成输入消息：这是 R21 最容易漏的口子。
    #
    #   它看起来"只是 PostMessage、不动光标"，但本程序的点击分支必然走到
    #   ShowPopupList，而那里对弹层调 SetForegroundWindow（弹层要拿键盘输入）——
    #   于是"投一条 WM_LBUTTONDOWN"和"SendInput 一下"的结果完全相同：前台被拿走。
    #   配合"扫描坐标"的用法，它还会向正在被使用的窗口连续投几十次点击、反复建模态弹层。
    #
    #   正则要同时抓两种写法：
    #     PostMessageW(h, 0x0201, ...)            消息号直接写字面量
    #     PostMessageW(h, WM_LBUTTONDOWN, ...)    消息号用宏
    #   WM_CLOSE / WM_THEMECHANGED / WM_APP_* 属于非输入消息，不在本规则管辖内
    #   （所以字面量只认 0x0100–0x0109 键盘区与 0x0200–0x020E 鼠标区，
    #    不能写成"0x0 开头全部" —— 那会把 WM_CLOSE=0x0010 一起判红）。
    dict(name="合成输入消息（R21）",
         scope="all",
         pat=r"\b(?:PostMessageW?|SendMessageW?|SendMessageTimeoutW?)\s*\(\s*[^,]+,\s*"
             r"(WM_MOUSEMOVE|WM_LBUTTONDOWN|WM_LBUTTONUP|WM_LBUTTONDBLCLK|"
             r"WM_RBUTTONDOWN|WM_RBUTTONUP|WM_RBUTTONDBLCLK|WM_MBUTTONDOWN|WM_MBUTTONUP|"
             r"WM_MOUSEWHEEL|WM_MOUSEHWHEEL|WM_KEYDOWN|WM_KEYUP|WM_CHAR|WM_SYSKEYDOWN|"
             r"WM_SYSKEYUP|WM_SYSCHAR|WM_XBUTTONDOWN|WM_XBUTTONUP|"
             r"0x010[0-9A-Fa-f]|0x020[0-9A-Ea-e])\b",
         allow={"src/app.cpp"},  # 见下方 R21 例外说明：钩子把真人按键还给焦点窗口，不是脚本合成输入
         why="不得向窗口投合成鼠标/键盘消息：它会真的走一遍'用户点了它'的路径，"
             "而点击分支在本程序里必然走到 ShowPopupList → SetForegroundWindow，等于抢前台。"
             "要验证点击流程 → 人工点、脚本只观察窗口数量；要验证命中判定 → 抽纯函数跑单测。"
             "src/app.cpp 的唯一命中是低层钩子把真人按键还给焦点窗口（保住 shell type-ahead），"
             "触发源是人不是脚本，与 AGENTS.md §1.1 对 SetForegroundWindow 的豁免同源"),

    # 第三方自动化库：一旦引入，一行就绕过上面所有判据（它们内部就是 SendInput）。
    #
    #   `SendKeys` 要带括号才算 —— `CmdId::SendKeys` 是命令枚举（ext.sendkeys，
    #   未实现），不是调用；把它判红会挡住正当的命令名。
    dict(name="第三方自动化库（R21）",
         scope="all",
         pat=r"\b(?:import|from)\s+(pyautogui|pynput|pywinauto|keyboard|mouse|AutoIt)\b"
             r"|\b(pyautogui|AutoItX)\.\w+\s*\("
             r"|\b(mouse|keyboard)\.(click|press|type|move|send|release)\s*\("
             r"|\bSendKeys\s*\(",
         allow=set(),
         why="不引入会模拟输入的自动化库（它们底层就是 SendInput/钩子）；"
             "机器入口是命令总线与 AI 管道，不是'帮用户按一下'"),

    dict(name="自动化抢前台（R21）",
         # 作用域 automation：只管脚本。src/ 里的 SetForegroundWindow 是**产品行为**
         # （路径转发激活已有窗口、弹层取得键盘输入），触发源是用户不是脚本，
         # 不在 R21 管辖范围内 —— 删掉它反而让"第二次启动"看起来没反应。
         scope="automation",
         pat=r"\b(SetForegroundWindow|SetActiveWindow|BringWindowToTop|"
             r"SwitchToThisWindow)\s*\(",
         # 不放行任何脚本。曾经为此开过 probe_reopen.py 的口子（它要让自己起的
         # 实例变前台才能测弹层），后来脚本改成"前台不是它就 SKIP" —— 脚本不该
         # 为了让测试成立而改变系统状态，那测到的是脚本造出来的场景。
         allow=set(),
         why="自动化脚本不得改变用户的前台窗口。需要前台才能测的场景，"
             "改成'没前台就跳过'（probe_toggle.py 的做法），而不是自己抢过来"),

    # HOOK_GUARD：全局钩子。项目明文禁令"不装全局钩子"（AGENTS.md / CONTRIBUTING.md /
    # ARCHITECTURE-RULES.md）的机械守门。R21 的四条判据不管 SetWindowsHookEx（它拦截
    # 真人按键、不是合成输入），所以这条单独守"别再装第二个钩子"。
    #
    #   唯一豁免 src/kb_hook.cpp：中文输入法把字母吃成 VK_PROCESSKEY，不装 WH_KEYBOARD_LL
    #   取不到真实 vk。这是已签发的功能豁免（CONTRIBUTING.md 禁项表 + review/G7 H2），
    #   不是漏洞 —— 任何其它文件再装钩子就把"不碰 explorer/不装钩子"的价值主张推翻。
    dict(name="全局钩子 HOOK_GUARD（唯一豁免 kb_hook.cpp）",
         scope="all",
         pat=r"\bSetWindowsHookEx(?:W|A)?\s*\(",
         allow={"src/kb_hook.cpp"},
         why="项目不装全局钩子（不碰 explorer 的延伸）。唯一豁免 src/kb_hook.cpp 的 "
             "WH_KEYBOARD_LL：中文输入法把字母吃成 VK_PROCESSKEY，必须取真实 vk"
             "（见 CONTRIBUTING.md 禁项表豁免行、review/G7_cmd_input.md H2）。"
             "其它文件不得再装钩子 —— 否则照抄先例就会装出第二个"),
]

# 扫描 R21 时覆盖的目录与后缀（相对仓库根）
INPUT_GUARD_DIRS = ["src", "tools", "scripts", "tests"]
INPUT_GUARD_EXTS = {".cpp", ".h", ".hpp", ".py", ".ps1"}
# automation 作用域只看这些目录（脚本/测试），不看产品代码
INPUT_GUARD_AUTOMATION_DIRS = ["tools", "scripts", "tests"]


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

# ---------------------------------------------------------------------------
# 令牌消费检查：色值令牌表里的字段必须真的被 Theme 取用
#
#   "定义了但没人取"的令牌比死代码更糟：它看起来像"这个颜色已经考虑了"，
#   实际改它不会有任何效果 —— 下一个改主题的人会以为自己在改，其实什么也没发生。
#   2026-11-20 的审查发现 UiPalette 有 6/20 个字段零消费（editFocus /
#   editPlaceholder / listText / listInertText / chevronHot / panelBorder）。
#
#   判据：字段名在 theme.cpp 里出现过（`T.xxx` 或 `T.xxx` 的赋值目标）。
#   故意不检查"是否被画到"—— 那需要运行时信息；这里只保证"有唯一的下游"。
# ---------------------------------------------------------------------------
TOKEN_TABLE = {
    "file": "ui_palette.h",       # 字段声明所在
    "struct": "UiPalette",
    "consumer": "theme.cpp",      # 唯一消费方
}

# ---------------------------------------------------------------------------
# 尺寸令牌检查（R6 的延伸）：Theme 的图标尺寸字段必须被 accessor 取用，
# 且 accessor 必须在业务代码里真的被调用。
#
#   为什么单独查这一类：图标尺寸直到 2026-11-20 才第一次被收成令牌，而它此前正是
#   "7 个裸数字散在 6 个文件"（S(9)/S(11)/S(12)/S(14)/S(16)）的重灾区。令牌化若只
#   停留在"定义了字段"，下一个人仍旧会写 S(12)。必须同时保证两件事：
#     ① 每个 icon*96 字段都被 theme.h 里的某个 accessor 读（否则改它不生效）
#     ② 每个 accessor 在 src/*.cpp 里至少有一处调用（否则是装饰）
#   判据是文本级的，不追求语义精确 —— 它要挡住的是"加了令牌却没人用"，
#   而不是"用法不合我意"。真正的分档口径写在 theme.h 那段注释里。
# ---------------------------------------------------------------------------
ICON_TOKEN = {
    "file": "theme.h",
    "prefix": "icon",      # 字段 iconXxx96 + accessor iconXxx()
    "suffix": "96",
}

# 不参与扫描的文件（备份/生成物）
SKIP = {"common.h.gch", "keymap.gen.h"}

# ---- 单文件行数上限（AGENTS.md §3「单文件 ≤500 行」）----
#
# 【为什么单独立一条】上面的唯一性/禁令规则管的是"**同一件事只能有一处实现**"，
# 它们看不出"一个文件塞了太多职责"。而超长文件正是职责不分的**结果**：
# shell_ops.cpp 一千多行里既有删除/移动，又有哈希/比较，还有撤销 ——
# 三件事要一起改、一起测、一起 review，改任何一件都要读全文。
#
# 【口径】只查 src/ 下的 .cpp/.h（与 sources() 的集合一致）：
# 规则约束的是**产品源码**的职责划分，不是脚本与测试的组织方式。
#
# 【为什么带"历史欠账基线"】2026-11-21 盘点时已有 18 个文件超限。
# 一次性把它们全劈开是**独立的一轮重构**（每个都要重新划清边界 + 补测试），
# 混在别的改动里做，等于在重构的半路上叠加新功能 —— 出事时没人分得清是谁的锅。
# 所以这里采取**棘轮**：
#   · 历史超限文件按**记账时的行数**放行，不是"上限"——
#     涨过记账行数就报错，降下来就自动收紧（欠账只能变少，不能变多）；
#   · 不在账里的文件超过 500 行直接报错（新文件从一开始就要达标）。
#
# 【基线为什么放在 data/ 而不是脚本里】基线是**账**，账要能被 diff 看见。
# 写进脚本里，改账和改规则混在同一个文件里，review 时看不出哪次是"改规则"、
# 哪次是"销账"；放进 data/ 的独立文件（与 data/commands-baseline.json 同一套路），
# 每次销账就是一次独立提交，git diff 里一目了然。
#
# 【销账怎么做】确认某文件真的拆干净了，跑：
#   python tools/check_components.py --update-length-baseline
FILE_LENGTH_MAX = 500
LENGTH_BASELINE = ROOT / "data" / "file-length-baseline.json"


def load_length_baseline():
    """读历史欠账基线；没有就返回空账（全部文件按 500 行判）。"""
    if not LENGTH_BASELINE.exists():
        return {}
    try:
        data = json.loads(LENGTH_BASELINE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    files = data.get("files", {})
    return files if isinstance(files, dict) else {}


def current_lengths():
    """src/ 下每个源文件的当前行数。"""
    out = {}
    for p in sources():
        out[p.name] = sum(
            1 for _ in p.read_text(encoding="utf-8", errors="replace").splitlines())
    return out


# 非致命提示（"应该私有化"这类），不影响退出码
stale_warn: list[str] = []
# 单文件行数的历史欠账（见 FILE_LENGTH_GRANDFATHERED 的说明）：
# 已记账但仍超 500 行的文件，每次跑检查都列出来 —— 欠账要一直看得见，
# 否则它就是一条「改了就没人记得」的静默债。
length_debt: list[str] = []


def strip_comments(text: str, hash_comments: bool = False) -> str:
    """去掉注释，避免注释里的 API 名误判。

    · C/C++ 风格 `//` 与 `/* */` 始终剥（src/ 用）。
    · `hash_comments=True` 时同时剥 `#` 到行尾（Python/PowerShell 用）。

    **为什么必须区分**：R21 的扫描集包含 `tools/*.py`。如果不剥 `#`，
    检查器自己那段"正则要同时抓 `PostMessageW(h, 0x0201)` 这种写法"的注释
    就会被自己判成违规 —— **文档里写"不要这样做"反而触发失败**，规则立刻变成
    不可维护的东西（下一个人只会选择删掉这条规则，而不是删掉注释）。
    这类"规则把说明文字当真"的假阳性，比漏报危险得多。

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
            elif hash_comments and c == "#":
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


def guarded_sources(scope: str):
    """R21 的扫描集：跨目录（src/tools/scripts/tests），文件名用相对仓库根的 posix 路径。

    scope="all"        → INPUT_GUARD_DIRS 全部
    scope="automation" → 只 tools/scripts/tests（产品代码里的前台调用是用户行为）
    """
    dirs = INPUT_GUARD_DIRS if scope == "all" else INPUT_GUARD_AUTOMATION_DIRS
    for d in dirs:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix.lower() not in INPUT_GUARD_EXTS:
                continue
            # 备份/生成物与依赖目录不进（obj/ 在根下，不在这些 dirs 里）
            if any(part in {"obj", "backup", ".git", "node_modules"} for part in p.parts):
                continue
            yield p.relative_to(ROOT).as_posix(), p


def scan():
    files = {p.name: strip_comments(p.read_text(encoding="utf-8", errors="replace"))
             for p in sources()}
    fails = []
    stale_warn.clear()
    length_debt.clear()

    for rule in UNIQUE + FORBID:
        hits = []
        rx = re.compile(rule["pat"], re.M)
        for name, text in files.items():
            # headers_only：只在头文件里判（入口 = 对外声明）
            if rule.get("headers_only") and not name.endswith(".h"):
                continue
            n = len(rx.findall(text))
            if n:
                hits.append((name, n))
        stray = [(n, c) for n, c in hits if n not in rule["allow"]]
        if stray:
            fails.append((rule, stray))

    # ---- 单文件行数上限（AGENTS.md §3）----
    # 走棘轮：历史超限文件按基线里的记账行数放行（涨过就报错，降下来自动收紧），
    # 不在账里的文件一律以 FILE_LENGTH_MAX 为准。
    length_hits = []
    baseline = load_length_baseline()
    for name, n in sorted(current_lengths().items()):
        allowed = baseline.get(name)
        limit = allowed if isinstance(allowed, int) else FILE_LENGTH_MAX
        if n > limit:
            length_hits.append((name, n))
        elif isinstance(allowed, int) and n > FILE_LENGTH_MAX:
            # 还在欠账里、但没涨过记账行数 —— 列出来，让欠账保持可见。
            length_debt.append(f"{name}  {n} 行（记账 {allowed}）")
    if length_hits:
        fails.append((
            dict(name=f"单文件 ≤{FILE_LENGTH_MAX} 行",
                 unit="行",
                 why=f"一个文件只做一件事（AGENTS.md §3）。"
                     f"历史超限文件按记账行数棘轮收紧，涨过记账行数即报错"),
            length_hits))

    # R21：输入纪律。扫描集与上面不同（跨目录），因此单独一趟。
    # 注释先剥掉 —— 文档里写"禁止 SendInput"不能自己触发失败。
    # py/ps1 还要剥 `#`：检查器自己会写"PostMessageW(h, 0x0201)"当反例，
    # 不剥就会自己把自己判红（见 strip_comments 的说明）。
    guarded: dict[str, str] = {}
    for rel, path in guarded_sources("all"):
        hash_style = path.suffix.lower() in {".py", ".ps1"}
        guarded[rel] = strip_comments(
            path.read_text(encoding="utf-8", errors="replace"), hash_comments=hash_style)
    automation_only = {rel for rel, _ in guarded_sources("automation")}

    for rule in INPUT_GUARD:
        rx = re.compile(rule["pat"], re.M)
        allow = rule["allow"]
        pool = automation_only if rule["scope"] == "automation" else set(guarded)
        stray = []
        for rel in sorted(pool):
            n = len(rx.findall(guarded.get(rel, "")))
            if n and rel not in allow:
                stray.append((rel, n))
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

    # 令牌表：每个字段都必须被 consumer 取用
    tbl = files.get(TOKEN_TABLE["file"], "")
    body = re.search(r"struct\s+" + TOKEN_TABLE["struct"] + r"\s*\{(.*?)\n\};", tbl, re.S)
    if body:
        consumer = files.get(TOKEN_TABLE["consumer"], "")
        unused = []
        for field in re.findall(r"\b([a-zA-Z_]\w*)\s*[,;]", body.group(1)):
            if not re.search(r"\bT\." + field + r"\b", consumer):
                unused.append(field)
        if unused:
            fails.append((dict(name="令牌未被消费", allow={TOKEN_TABLE["file"]},
                               why="色值令牌定义了却没被 Theme 取用 —— 改它不会有任何效果，" +
                                   "下一个改主题的人会误以为在改"),
                          [(TOKEN_TABLE["file"], len(unused))], unused))

    # 尺寸令牌：字段 ↔ accessor ↔ 调用点，三段都要通
    th = files.get(ICON_TOKEN["file"], "")
    if th:
        pre, suf = ICON_TOKEN["prefix"], ICON_TOKEN["suffix"]
        # ① 字段 iconXxx96 必须在 theme.h 里被某个 `S(iconXxx96)` 读到
        for field in re.findall(r"\b(" + pre + r"[A-Za-z]\w*" + suf + r")\s*=", th):
            if not re.search(r"S\s*\(\s*" + field + r"\s*\)", th):
                fails.append((dict(name=f"图标尺寸字段未被 accessor 读取（{field}）",
                                   allow={ICON_TOKEN["file"]},
                                   why="字段加了但没有 accessor 读它 —— 改这个数字不会影响任何绘制"),
                              [(ICON_TOKEN["file"], 1)]))
        # ② accessor iconXxx() 必须被业务代码调用（theme.h 内的声明不算）
        accs = set(re.findall(r"int\s+(" + pre + r"[A-Za-z]\w*)\s*\(\s*\)\s*const", th))
        for acc in sorted(accs):
            callers = [n for n, t in files.items()
                       if n != ICON_TOKEN["file"] and re.search(r"\b" + acc + r"\s*\(\s*\)", t)]
            if not callers:
                fails.append((dict(name=f"图标尺寸 accessor 零调用（{acc}）",
                                   allow=set(),
                                   why="accessor 建了但没人用 —— 那种图标尺寸还是散在栏里的裸数字"),
                              [(ICON_TOKEN["file"], 0)]))

    return fails, files


def check_eval_surface():
    """评估面清单是否与仓库现状一致，且清单自身可信、重签留痕。

    【为什么这是一条规则】构建器、测试源、静态校验器合起来就是"判据"。
    自我迭代的闭环是「改代码 → 编译 → 跑测试 → 看结果」，判据被改软之后，
    "测试通过"就不再说明代码是对的 —— Darwin Gödel Machine 论文把这叫
    objective hacking，并把"如何防止模型改 evaluator"列为公开问题。

    规则是「只许签发，不许自改」。**三道关，缺一不可**：

      1. selfSeal  —— 清单正文未被直接编辑（防"手改清单"）；
      2. diff      —— 受保护文件未被改动（防"改了判据"）；
      3. git 跟踪  —— 重签必然以"未提交改动"出现（防"改了并同步重签"）。

    第 3 道是本轮补的：前两道都在"清单说什么"，只有第 3 道保证
    "清单的变化有人看得见"。清单不入库时，重签在 git 里没有任何痕迹 ——
    那等于允许 agent 自己放宽判据。
    """
    try:
        import gen_eval_baseline as geb
    except Exception:
        return None
    if not os.path.isfile(geb.MANIFEST):
        return [(dict(name="评估面清单缺失",
                      why="构建器/测试/校验器是判据。清单缺失 = 没有设防。"
                          "由人跑：python tools/gen_eval_baseline.py --human-confirm"),
                 [("data/eval-surface.json", 0)])]
    try:
        with open(geb.MANIFEST, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except Exception as e:
        return [(dict(name="评估面清单解析失败", why=str(e)),
                 [("data/eval-surface.json", 0)])]

    # ---- 第 1 道：清单自身未被手改 ----
    seal = manifest.get("selfSeal", "")
    if not seal:
        return [(dict(name="评估面清单缺少 selfSeal",
                      why="旧版签发或被人删掉了这一项。清单自身不可验 = 前两道关都失效。"
                          "由人重签：python tools/gen_eval_baseline.py --human-confirm"),
                 [("data/eval-surface.json", 0)])]
    if geb.compute_seal(manifest) != seal:
        return [(dict(name="评估面清单被直接改过（selfSeal 不符）",
                      why="没走重签就改了清单正文 —— 这比\"评估面被改\"更可疑。"
                          "用 git diff data/eval-surface.json 看改了什么。"),
                 [("data/eval-surface.json", 0)])]

    # ---- 第 2 道：受保护文件未被改动 ----
    problems = geb.diff(None, manifest)
    if problems:
        return [(dict(name=f"评估面被改动（{len(problems)} 项，未重新签发）",
                      why="判据被改软时，\"测试通过\"不代表代码对。"
                          "确认改动是有意的后，由人重签："
                          "python tools/gen_eval_baseline.py --human-confirm"),
                 [(p, 1) for p in problems[:20]])]

    # ---- 第 3 道：重签痕迹必须被 git 记住 ----
    ok, why = geb.check_git_tracked()
    if not ok:
        return [(dict(name="评估面清单重签不留痕（git 未跟踪或有未提交改动）",
                      why=why), [("data/eval-surface.json", 0)])]
    return []


def main():
    list_only = "--list" in sys.argv

    # ---- 销账：重写单文件行数基线 ----
    #
    # 只在**确认某文件确实拆干净了**之后才跑。跑一次就把该文件从账上抹掉，
    # 之后它就回到 500 行的硬上限。基线是 git 跟踪的文件（data/ 下），
    # 所以销账是一次独立提交，review 时一眼看得见。
    if "--update-length-baseline" in sys.argv:
        cur = current_lengths()
        old = load_length_baseline()
        # 只记仍然超限的文件：已经降到 500 行以下的，自然从账上消失。
        files_out = {k: v for k, v in sorted(cur.items()) if v > FILE_LENGTH_MAX}
        payload = {
            "max": FILE_LENGTH_MAX,
            "note": "单文件行数历史欠账基线（棘轮）：涨过记账行数即报错，"
                    "降下来自动收紧。销账：python tools/check_components.py "
                    "--update-length-baseline",
            "updatedAt": __import__("datetime").datetime.now().astimezone().isoformat(
                timespec="seconds"),
            "files": files_out,
        }
        LENGTH_BASELINE.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        changed = [k for k in set(old) | set(files_out) if old.get(k) != files_out.get(k)]
        print(f"已写入基线 {LENGTH_BASELINE.relative_to(ROOT)}"
              f"（账上 {len(files_out)} 个文件）")
        if changed:
            print("  变化：" + ", ".join(
                f"{k} {old.get(k, '-')}→{files_out.get(k, '-')}" for k in sorted(changed)))
        else:
            print("  无变化")
        return 0

    fails, files = scan()
    eval_fails = check_eval_surface() or []

    if list_only:
        for rule in UNIQUE + FORBID:
            rx = re.compile(rule["pat"], re.M)
            row = [(n, len(rx.findall(t))) for n, t in files.items() if rx.search(t)]
            print(f"[{rule['name']}] " + (", ".join(f"{n}×{c}" for n, c in row) or "（无）"))
        # R21 的扫描集跨目录，单独列一遍（含被放行的例外，便于核对口径）。
        # 【口径必须与 scan() 完全一致】同一个 strip_comments（py/ps1 剥 #），
        # 否则 --list 会把检查器自己注释里的反例算成命中 —— 列表与结论互相矛盾。
        guarded = dict(guarded_sources("all"))
        auto = {rel for rel, _ in guarded_sources("automation")}
        for rule in INPUT_GUARD:
            rx = re.compile(rule["pat"], re.M)
            pool = auto if rule["scope"] == "automation" else set(guarded)
            row = []
            for rel in sorted(pool):
                path = guarded[rel]
                hash_style = path.suffix.lower() in {".py", ".ps1"}
                body = strip_comments(path.read_text(encoding="utf-8", errors="replace"),
                                      hash_comments=hash_style)
                n = len(rx.findall(body))
                if n:
                    row.append((rel, n))
            tag = "放行" if row and all(n in rule["allow"] for n, _ in row) else rule["scope"]
            print(f"[{rule['name']}/{tag}] " + (", ".join(f"{n}×{c}" for n, c in row) or "（无）"))
        # 非正则类的规则：单独列，让 --list 覆盖全部检查
        for rule in STALE:
            rx = re.compile(rule["symbol"], re.M)
            own = {rule["home"], rule["home"].replace(".cpp", ".h")}
            ext = sum(len(rx.findall(t)) for n, t in files.items() if n not in own)
            print(f"[接线/{rule['name']}] 外部调用 {ext} 次（home={rule['home']}）")
        if TOKEN_TABLE.get("file"):
            print(f"[令牌消费] {TOKEN_TABLE['struct']} 的字段必须被 {TOKEN_TABLE['consumer']} 取用")
        # 单文件行数：非正则类规则，单独列，让 --list 覆盖全部检查
        bl = load_length_baseline()
        over = [(n, v) for n, v in sorted(current_lengths().items()) if v > FILE_LENGTH_MAX]
        debt = [f"{n}×{v}" for n, v in over if isinstance(bl.get(n), int)]
        print(f"[单文件 ≤{FILE_LENGTH_MAX} 行] 账上 " +
              (", ".join(debt) or "（无）") +
              f"（共 {len(over)} 个超限；不在账里的超限 = 违规）")
        return 0

    if not fails and not eval_fails:
        # 令牌检查是单条规则（TOKEN_TABLE 有定义就启用），一并报出来
        token_n = 1 if TOKEN_TABLE.get("file") else 0
        print(f"OK：{len(UNIQUE)} 条唯一性规则、{len(FORBID)} 条禁令、"
              f"{len(STALE)} 条接线检查、{token_n} 条令牌消费、"
              f"{len(INPUT_GUARD)} 条输入纪律、1 条评估面完整性，全部通过。")
        for w in stale_warn:
            print(f"  提示  {w}")
        if length_debt:
            print(f"  欠账  {len(length_debt)} 个文件仍超 {FILE_LENGTH_MAX} 行"
                  f"（AGENTS.md §3，棘轮记账，涨过即报错）：")
            for d in sorted(length_debt):
                print(f"        {d}")
        return 0

    print(f"FAIL：{len(fails) + len(eval_fails)} 条规则被违反"
          + (f"（另有 {len(length_debt)} 个文件仍超 {FILE_LENGTH_MAX} 行，记在欠账里）"
             if length_debt else "") + "\n")
    for item in fails + eval_fails:
        rule, stray = item[0], item[1]
        detail = item[2] if len(item) > 2 else None
        print(f"  ✗ {rule['name']}")
        unit = rule.get("unit", "")
        for name, cnt in stray:
            if unit:
                print(f"      {name}  {cnt} {unit}")
            elif cnt == 0:
                print(f"      {name}  外部调用 0 次")
            else:
                print(f"      {name}  ×{cnt}")
        print(f"      → {rule['why']}")
        if detail:
            print(f"      未消费：{', '.join(detail)}")
        if rule.get("allow"):
            print(f"      允许：{', '.join(sorted(rule['allow']))}")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
