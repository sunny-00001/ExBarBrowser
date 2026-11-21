---
status: current
baseline: 2026-11-21 11:00
---

# 命名与标识规范

> 回答一个问题：**本工程独立后，名字该怎么起；旧名字哪些留、哪些改、哪些删。**
>
> 本文件由 2026-11-20 的业界比对产出（Blender / VS Code / Qt / Directory Opus /
> Total Commander / Far Manager / Chromium Views / Tauri / Emacs Lisp），比对结论见 §5。
> 规则（可机械检查的部分）见 §6；实施状态见 §7。

---

## 1. 为什么需要这份文档

工程 2026-11-20 从 `D:\Windhawk` 主仓独立（见 [DECISIONS.md](DECISIONS.md) D8）。
独立之前，大量名字是"为了和 ExBar mod 共存"才那样起的：文件前缀 `eb_`、命令的
`Cmd` 前缀、`browser-` 数据文件前缀、三种品牌串并存。这些**共存理由已经消失**，
但名字还在，于是出现三类问题：

| 问题 | 表现 |
|---|---|
| 归属错误 | `namespace eb`（= Ex**B**ar）是**上一个项目**的前缀，却写在独立工程的每个 UI 文件里（已于 2026-11-20 拆解，见 §3.2） |
| 一物多名 | 品牌串三套：`ExBar`（标题）/ `ExBarBrowser`（类名/管道/日志）/ `EXBAR`（书签属性） |
| 一义多名 | 点分名（`tab.close`）已在别名表里覆盖**全部命令**。2026-11-20 起**点分名已升为主名**（全量迁移完成），驼峰名降为 legacy 别名 |

---

## 2. 品牌与标识（一个工程一个名）

### 2.1 唯一来源

```cpp
// src/identity.h —— 品牌串唯一来源
constexpr wchar_t kBrand[] = L"ExBarBrowser";   // 正式名：窗口类/管道/日志/互斥体
constexpr wchar_t kBrandShort[] = L"ExBar";     // 显示名：窗口标题/对话框标题
```

**规则 B1**：品牌串只允许出现在 `identity.h`。其他文件出现 `L"ExBar..."` 字面量即违规。

### 2.2 什么必须改、什么必须留

| 项 | 现值 | 处置 | 理由 |
|---|---|---|---|
| 窗口标题 | `L"ExBar"` × 2 | 用 `kBrandShort` | 自用 |
| 对话框标题 | `L"ExBar"` × 4 | 用 `kBrandShort` | 自用 |
| 窗口类名 | `ExBarBrowser_*` × 11 | 用 `kBrand` 拼，**并减到 3 个** | 自用；R12 要求 ≤3 |
| 单实例互斥体 | `Local\ExBarBrowser.Instance` | 用 `kBrand` | 自用 |
| 命名管道 | `\\.\pipe\ExBarBrowser_ai_` | 用 `kBrand` | 自用 |
| AI 服务名 | `exbarbrowser` | 用 `kBrand` 小写 | 自用 |
| 环境变量 | `EXBARBROWSER_MULTI` | 用 `kBrand` 大写 | 自用 |
| 日志文件 | `ExBarBrowser.log` | 用 `kBrand` | 自用 |
| `WM_COPYDATA` 魔数 | `0xEBB1` | **保留** | 曾改为 `0x45584242` 后撤销：窗口类名未变，升级期新旧进程混跑会互相拒绝（见 D9） |
| 书签属性 | `EXBAR_ID` / `EXBAR_ICON` / `EXBAR_FLAGS` | **保留** | 唯一跨软件契约：Netscape 书签扩展格式，与浏览器/mod 互读（`bookmarks.h:3-6`） |
| `data/` 下文件名 | `keymap.json` / `mouse.json` / `bookmarks.html` | **保留** | 用户已有配置文件 |
| `settings.json` 字段名 | 现状 | **保留** | 同上 |

**规则 B2**：跨软件契约（书签属性、数据文件名、配置键名）**永不改名**，除非同批提供迁移代码。

---

## 3. 命名空间与文件（按领域，不按来源）

### 3.1 现状：`eb` 是唯一的例外

| namespace | 依据 | 合规 |
|---|---|---|
| `win` | 平台 | ✅ 领域 |
| `ico` | 图标 | ✅ |
| `str` | 字符串 | ✅ |
| `nav` | 导航 | ✅ |
| `tf` | 文本文件 | ✅ |
| `json` | 格式 | ✅ |
| `chord` | 键组合 | ✅ |
| `shellops` / `shellinput` | shell 操作/输入 | ✅ |
| `cmdnaming` / `keymap_gen` / `core` / `diskscan` / `ai` / `id` | 领域 | ✅ |
| **`eb`** | **ExBar（来源项目）** | ❌ **唯一例外** |

**规则 N1**：命名空间按**领域**命名，不按来源项目。`eb` 必须拆解。

### 3.2 拆解结果（2026-11-20 已落地）

`eb` 已整体消失（`grep -r "namespace eb\b\|eb::" src/` 无命中），落到两个领域命名空间 + 一个输入端：

| 原 `eb::` 符号 | 现在归属 | 依据 |
|---|---|---|
| `DrawGlyph` / `DrawChevron` / `Fill*` / `FrameRectColor` / `DrawSeparator` / `PaintBuffered` / `MeasureText` / `StockIcon` / `DrawIcon*` / `GetUiFont` / `GlyphFace` | `ui::`（`ui_draw.{h,cpp}`） | Chromium `gfx::Canvas` / `ui::ResourceBundle` —— 本工程规模不需要拆成 Painter/Assets/Metrics 三个类，一个 `ui` 命名空间 + 扁平函数已足够，拆细只是多一层跳转 |
| 色值令牌（原 `eb_theme.h` 的 `ThemeColors`） | `UiPalette`（`ui_palette.h`） | 令牌与绘制原语是两件事：前者是数据，后者是动作 |
| `TooltipHost` | `ui::TooltipHost`（`tooltip_host.h`） | 同属 UI 原语 |
| **`CurrentMods`** | **`mouse_rules.h` 的全局函数** | 它读 `GetKeyState`，是**输入端**语义（R15）。`input_router` 的成员版本因需按焦点抑制，保留自身一份 |
| `FontPx` / `Scale` / `IconSize` / `IconGlyph` | **已删** | 前三个零调用且在造"第二个 DPI 换算公式"（唯一入口是 `Theme::S()`）；`IconGlyph` 被 `ico::Glyph` 取代 |

**规则 N2**：文件名用领域名，不用来源前缀。`eb_ui.*` / `eb_theme.h` 已作废并改名。

### 3.3 内部符号用 `<>::detail` 或 `--` 约定

Emacs Lisp 用双连字符 `--` 标记"不对外"（`pkg--internal`）。本工程用 C++ 惯例：
**不公开的符号放 `namespace <domain>::detail`**，或放匿名命名空间。

`ui_draw.h` 现状：**14 个公开符号 + 2 个 `detail::` 私有**。

- `ui::detail::GetUiFont` / `ui::detail::GlyphFace`：只给 `DrawGlyph` / `DrawChevron` 内部选字体用。
  以前是公开声明（零外部调用），公开等于开了"绕过 `DrawGlyph` 自己建字体"的口子，
  字体一旦分叉就是"两套观感"。收进 `detail` 比删掉好 —— 它们是真实需要的，只是不该外露。
- `ico::IconIdFromName`：**预留**的公共接口（数据驱动处按名字取图标）。当前 `keymap.json`
  不含图标名，所以零调用；`STALE` 段把它登记为"预留"，若长期不用应删。

`tools/check_components.py` 的 `STALE` 段区分三态，只有"真死代码"才判失败：

| 状态 | 判定 | 结果 |
|---|---|---|
| 外部有调用 | `external > 0` | 通过 |
| 只有 `home` 内自用 | `external == 0 && internal > 1` | **提示**（应私有化，不拦 CI） |
| `home` 内也无人调用 | `external == 0 && internal <= 1` | **失败**（真死代码） |

历史清账：7 个零调用入口（`FontPx` / `Scale` / `IconSize` / `GlyphFace` / `IconGlyph` /
`IconIdFromName` / `MeasureText`）→ 删 3 个（前三个 + `IconGlyph`）、接线 1 个（`MeasureText`
被 8 处调用）、私有化 2 个（`GetUiFont` / `GlyphFace`）、预留 1 个（`IconIdFromName`）。

---

## 4. 命令命名

### 4.1 目标形态

```cpp
struct CommandDef {
    CmdId id;                  // 稳定：枚举值分配后不改，废弃保留占位
    const wchar_t* name;       // 主名：点分小写（tab.close.others）
    const wchar_t* legacyName; // 旧名：驼峰（TabCloseOther），永不删
    const wchar_t* group;      // 第一段（tab / nav / fs / op / view / win / bmk / sess）
    const wchar_t* title;      // 中文显示名
    const wchar_t* scopeUi;    // 中文区域名（只用于展示）
};
```

### 4.2 为什么点分是主名（而不是别名）

迁移前的方向是反的：`name = "TabCloseOther"`，`cmd_naming.cpp` 的别名表把
`tab.close.others` 存成**别名**。于是 `keymap.json`、`tools/list`、`audit.jsonl`
里出现的全是驼峰名，点分名只在 `FindCommand` 查表那一瞬间存在。

2026-11-20 已完成全量翻转（当时 140 条主名全部为点分名，`legacyName` 由
`cmdnaming::LegacyOf` 反查），上面这段保留为「为什么当初要改」的依据。

业界没有"驼峰主名 + 点分别名"的先例（见 §5.1）：

| 项目 | 主名形态 |
|---|---|
| Blender | `object.mode_set`，**强制包含一个点**，否则注册失败 |
| VS Code | `workbench.action.files.save`，无别名 |
| Directory Opus | `Go` / `Copy`（短单词 + 参数） |
| Total Commander | `cm_Copy`（前缀 + 单词） |
| Far Manager | `edit:` 前缀做命令行路由 |

### 4.3 三条硬规则

**规则 C1（稳定 id）**：`CmdId` 枚举值一经分配不再改动。删除命令时保留枚举项并标记废弃，
不得重排（VS Code：`id` 是稳定标识，永不变）。

**规则 C2（旧名永不删）**：`legacyName` 一旦写入不再删除。多次改名则累加
（VS Code：`legacyToolReferenceFullNames` 只增不减）。

**规则 C3（消费方全覆盖）**：所有读命令名的地方必须经 `FindCommand`（两个方向都认），
不得直接字符串比较。三个消费方：`keymap` 查找、`ai_api` 的 `tools/list`、命令面板。
> VS Code 踩过的坑：改名后漏改 `eligibleForAutoApproval` 的检查逻辑，用户设的限制静默失效
> （[vscode#278506](https://askill.sh/skills/gh/microsoft/vscode/@tool-rename-deprecation)）。

### 4.4 `Cmd` 前缀处置

现状 15 条（`CmdCopy` / `CmdCut` / `CmdPaste` / `CmdDelete` / `CmdRename` / `CmdProps` /
`CmdUndo` / `CmdSelectAll` / `CmdOpen` / `CmdOpenWith` / `CmdRefresh` / `CmdNewFolder` /
`CmdNewTextFile` / `CmdOpenInNewTab` / `CmdOpenInNewWindow`）。

`cmd_naming.h:4-5` 自己承认：

> `Cmd` 前缀是历史残留（当初用来区分"命令"与"导航"），语义上无意义

Total Commander 的 `cm_` 能成立，是因为**所有**内置命令都有 `cm_`，无例外。
本工程 15/137 有 `Cmd`，前缀反而成了噪音。

**处置**：点分名为主名后，`Cmd` 前缀无意义。`CmdCopy` → `fs.copy`，`CmdOpen` → `fs.open`，
`CmdDelete` → `fs.delete`（与已有 `FsDelete` 合并）。旧驼峰名进 `legacyName`。

**规则 C4**：命令主名第一段必须是 `group` 的小写形式，且 `group` 取值可枚举。

### 4.5 `QuitExplorer` 必须改名

```cpp
CmdId::QuitExplorer   // commands.h:63
```

实际行为是 `app_commands_misc.cpp:33` 的**退出本程序**。mod 期这个名字是"重启 explorer.exe"，
现在语义完全变了：命令名叫"退出资源管理器"，行为是"退出 ExBarBrowser"。

**处置**：`win.quit`（点分名已存在：`cmd_naming.cpp:141`），`legacyName = "QuitExplorer"`。

---

## 5. 业界比对（本文档的依据）

### 5.1 命令命名：四条路，同一结论

| 项目 | 做法 | 取 | 舍 |
|---|---|---|---|
| **Blender** | `bl_idname` 强制含一个点；C 里 `SOME_OT_operator` → Python `some.operator`；按领域分 `object`/`mesh`/`uv`/`wm` | 点分唯一名；领域分段；可发现性（自动补全） | C 宏形态 |
| **VS Code** | `<区域>.<类别>.<动作>`；`id` 稳定永不改；改名把旧名进 `legacy*` 数组永不删 | 稳定 id + legacy 数组 + 全消费方检查 | 完整贡献点体系 |
| **Directory Opus** | 29 条短单词（`Go`/`Copy`/`Select`），行为靠参数（`Go BACK`/`Copy MOVE`） | 命令名短、可记 | 一命令多模式（与 JSON Schema 冲突） |
| **Total Commander** | `cm_` 内置 / `em_` 用户自定义 | 前缀标记**稳定性** | 前缀做**分类**（本工程需更细分组） |
| **Far Manager** | `PluginInfo.CommandPrefix`，冒号分隔多前缀，命令行输入即路由 | 前缀 = 路由键 | 插件 ABI |

**为什么本工程不学 Opus 的粗粒度**：`commands.h:48-51` 已有论证 ——

> 不设 mode 参数是刻意的...带 mode 会让 schema 变成枚举，调用方要先查"有哪些 mode"。

本工程的对 AI 暴露命令（`aiExposed`）**必须**一条命令一个 schema（条数以 `check_ai_meta.py` 为准）。

### 5.2 命名空间：Qt 的警句直接命中 `eb`

Qt 惯例是所有框架类以 `Q` 开头（`QFile`/`QThread`），但对**用户自己的类**：

> 你不应该用 `Q` 作前缀。`Q` 是为 Qt 开发环境的一部分类保留的...最好选你自己的
> 1-2 字母前缀（**不以 Q 开头**）并坚持用它。
> 如果你只是为了 IDE 高亮才加 `Q` 前缀，那这是个坏主意。用命名空间是更好的方式 ——
> 它有作用域，而简单前缀没有。

> 出处：[Qt Class Naming Convention](https://stackoverflow.com/questions/14176514/qt-class-naming-convention)、
> [Should the name of my classes begin with 'Q'?](https://stackoverflow.com/questions/2921915/should-the-name-of-my-classes-begin-with-q-in-qt)

| Qt 的话 | 套到本工程 |
|---|---|
| `Q` 是框架保留的 | `eb` 是 **ExBar mod 保留的** |
| 自己的类不要用 `Q` | 工程已独立，`eb` 是"别人的前缀" |
| 用命名空间，前缀没有作用域 | 有 namespace 却造了个扁平杂物间 |
| 选自己的前缀并坚持 | 其余 9 个 namespace 都是领域名 |

### 5.3 UI 分层：Chromium 的三分

```
Widget（原生窗口桥）
└─ RootView
   └─ NonClientView
      ├─ NonClientFrameView  ← 边框/阴影/标题栏（可随主题替换）
      └─ ClientView          ← 内容（不受框架替换影响）
```

> 非客户区框架视图**可以随系统主题变化被替换，而不影响客户区视图**。
> 需要特殊背景或边框时，创建 `Background` / `Border` 子类并安装，而不是重写 `OnPaint*`。

> 出处：[Views Overview](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/ui/views/overview.md)、
> [views Windowing](https://www.chromium.org/developers/design-documents/views-windowing)

**本工程缺这一层**：每个栏的 `Paint()` 里"框架"（底色/分隔线）和"内容"（项/段/标签）混在
同一函数，于是换主题只能整栏 `ReloadTheme()` + 重建，而不是"只换框架"。

**规则 U1**：栏的 `Paint()` 只允许调 `Painter` 的声明式方法；底色与边框由载体统一处理。

### 5.4 窗口类名：唯一性 + Tauri 的公开教训

Windows 官方惯例：

> 类名是**当前进程本地**的名字，只需进程内唯一。但标准控件也有类名，必须避免冲突。
> 确保唯一性的**惯例方法**是把应用名包含进全局类名。

> 出处：[创建窗口 - Microsoft Learn](https://technet.microsoft.com/zh-cn/library/ff381397.aspx)

Tauri 的反面教训：早年把类名**硬编码为 `"Window Class"`**，社区 issue 抱怨
`GetClassName()` 返回的名字在 Win32 语境下毫无意义；2024-11 才加 `windowClassname` 配置项。

> 出处：[tauri#7498](https://github.com/tauri-apps/tauri/issues/7498)、
> [tauri#11469](https://github.com/tauri-apps/tauri/commit/e077eab3b9b8980159c57a73ec4f54ce7606513f)

**结论**：本工程窗口类名 `ExBarBrowser_*` 是**合规的**（带应用名、进程内唯一、不撞系统控件）。
问题不在命名而在**数量**（11 个 vs R12 要求 ≤3）。

### 5.5 Emacs Lisp：反面教材，但 `--` 约定可取

Emacs Lisp 没有包系统，所有全局符号共享一个命名空间，于是被迫：

> 所有全局符号应以包前缀开头，用连字符 `-` 分隔。**两个连字符**表示该符号
> 不打算给别的包用（`pkg--internal`）。

**取**：内部/公开的标记习惯。
**不取**：前缀方案本身 —— 那是"没有 namespace"的补偿，C++ 有 namespace 时用前缀是退化。

---

## 6. 可机械检查的规则

写进 `tools/check_naming.py`（待实现）：

| 编号 | 规则 | 判据 |
|---|---|---|
| B1 | 品牌串唯一来源 | `L"ExBar"` / `L"ExBarBrowser"` 只允许出现在 `identity.h` |
| B2 | 跨软件契约不改 | `EXBAR_ID` / `EXBAR_ICON` / `EXBAR_FLAGS` 必须仍存在（防止误删） |
| N1 | 命名空间按领域 | 禁止 `namespace eb` |
| N2 | 文件名不带来源前缀 | 禁止 `eb_*.h` / `eb_*.cpp` |
| C1 | 命令 id 稳定 | `CmdId` 枚举值与上次基线比对，只允许追加 |
| C2 | 旧名永不删 | `legacyName` 集合只增不减 |
| C4 | 主名第一段 = group | `name` 的点分第一段 == `group` 小写 |
| R12 | 窗口类 ≤3 | `RegisterClassOnce` 调用点数 ≤3 + 原生控件。**前置条件：B5 载体合并完成**（R12 原文如此）；当前 11 个类，本规则暂不启用 |

---

## 7. 实施状态

| 项 | 状态 | 说明 |
|---|---|---|
| 品牌串收编到 `identity.h` | ✅ 2026-11-20 | 新建 `src/identity.h`；21 处字面量全部改为常量引用（窗口类 11、标题 6、日志/互斥体/管道/服务名/环境变量各 1） |
| `kCopyDataMagic` 换值 | ↩️ 已撤销 | 曾改 `0x45584242`；因新旧版本窗口类名相同，升级期混跑会互相拒绝，恢复 `0xEBB1`（见 D9） |
| `MeasureText` 接线 | ✅ 2026-11-20 | 8 处裸调 → 1 处实现；顺带删掉 6 处冗余 `SelectObject/还原` 样板 |
| `ThemeControl` 接线 | ✅ 2026-11-20 | 3 处裸调 `SetWindowTheme` → 收编；签名改为 `subApp` 后缀，支持 `CFD`（编辑框）与 `Explorer`（列表） |
| `CreateChild` 接线 | ✅ 2026-11-20 | 4 个栏的 `CreateWindowExW` + `SetThis` 两行 → 一行 |
| `CreatePopup` 接线 | ✅ 2026-11-20 | 4 处（弹层/对话框/序列提示）；**修掉一个真实缺陷**：原来创建后才设 `GWLP_USERDATA`，晚于 `WM_NCCREATE`，现统一走 `lpCreateParams` |
| 删死入口 `FontPx`/`Scale`/`IconSize` | ✅ 2026-11-20 | 零调用且在造"第二个 DPI 换算公式"；`IconGlyph` 一并删 |
| `QuitExplorer` 主名 → `win.quit` | ✅ 2026-11-20 | 枚举名保留（规则 C1）；`legacyName` 仍可解析（规则 C2）；`keymap.json` / `keymap.gen.h` / 校验器白名单同步 |
| `FindCommand` 三路解析 | ✅ 2026-11-20 | 规则 C3 落地：见 §4.3 |
| 校验器：品牌串规则 | ✅ 2026-11-20 | `check_components.py` 新增 3 条（正式名/显示名/窗口类名） |
| 校验器：`STALE` 段 | ✅ 2026-11-20 | 新增"入口必须被接线"检查（3 条），防本轮发现的死代码复发 |
| 校验器：`MeasureText`/`ThemeControl` | ✅ 2026-11-20 | 两条 `UNIQUE` 规则，先报出 10 处违规再逐个消除 |
| 校验器：`check_commands.py` 认点分名 | ✅ 2026-11-20 | 正则 `\w+` → `[^"]+`；`implemented` 归一到枚举 id；读 `cmd_naming` 表认旧名 |
| 点分名升为主名（全量 137 条） | ✅ 2026-11-20 | `commands.cpp` 137 条主名全部改为点分名；`Cmd` 前缀自然消失（零残留）；`legacyName` 改由 `cmdnaming::LegacyOf` 反查（否则 `name` 与 `legacyName` 会变成同值） |
| `Cmd*` 与 `fs.*` 是否合并 | 🔁 已结论：**不合并** | `file.*` 是交互式（弹框/读选中），`fs.*` 是确定式（参数点名）。`commands.h:39-47` 已论证两套语义应当共存；合并会丢掉这个区分，且改写已暴露给 AI 的 `fs.*` 名 |
| `eb` 拆成领域命名空间 | ✅ 2026-11-20 | `eb_ui.*` → `ui_draw.*`（`namespace ui`）、`eb_theme.h` → `ui_palette.h`；`EbColors`/`EbPalette` → `UiPalette`/`PaletteFor`；21 文件约 77 处调用点；`shell_view.cpp` 里同名的局部 `HWND eb` 一并改掉 |
| `CurrentMods` 移入输入端 | ✅ 2026-11-20 | 从 `eb::` 移到 `mouse_rules.cpp` 的全局 `CurrentMods()`。**顺带修掉重复**：`input_router` 的成员版本与它一字不差（含 AltGr 特判），原先以"需按焦点抑制"为由放行 —— 读代码后确认无任何额外逻辑，改为转调；白名单收紧到 1 个文件 |
| 校验器：C1 命令 id 稳定 + C2 旧名只增不减 | ✅ 2026-11-20 | `check_commands.py` 新增基线比对（`data/commands-baseline.json`，`--update-baseline` 更新）：C1a 顺序不变、C1b 不删、C2 legacy 只增 |
| 校验器：`eb` 废弃规则 | ✅ 2026-11-20 | 3 条 `FORBID`（`namespace eb` / `eb::` / `#include "eb_`），防旧前缀回流 |
| `eb` 五类符号拆成具名类型（`Painter`/`Assets`/`Metrics`） | ⬜ 待办 | 命名空间已改 `ui`（"来源前缀"这个痛点已消除）；进一步拆成类型属组件化（B4），见 §3.2 |
| 窗口类 11 → 3 | ⛔ 依赖 B5，不是独立任务 | R12 原文即"**B5 之后**：窗口类 ≤3"。当前 11 个类各承载独立 WndProc + 独立绘制，减到 3 个等价于把栏/面板合并进统一载体（Lister/Popup/Dialog）—— 那是架构重构，改法与"改名"完全无关。**把它挂在命名清单里是分类错误**，已在 §6 标注依赖关系 |

### 7.1 本轮顺带修掉的真实缺陷

| 缺陷 | 表现 | 修法 |
|---|---|---|
| `CreatePopup` 传 this 的时机晚于 `WM_NCCREATE` | 若窗口过程在 `WM_NCCREATE` 里就要用 this，会拿到空指针 | 统一走 `CREATESTRUCTW::lpCreateParams`（`platform_win.cpp` 有注释说明为什么不能事后补设） |
| `log.cpp` 依赖 `common.h` 的间接包含取 `identity.h` | 单测独立编译 `log.cpp` 时编译失败 | 显式 `#include "identity.h"` |
| `check_commands.py` 正则不认点分名 | `win.quit` 那行被静默跳过，137 条变 136 条 | `L"(\w+)"` → `L"([^"]+)"` |
| `check_commands.py` 用主名比对处理器 | 改名后报"已实现但没处理器" | `implemented` 归一到 `CmdId` 枚举名比对 |
| `naming_test.cpp` 把点分主名当"缺别名" | 迁移完成的命令被报错 | 点分主名 + 旧名可解析 = 通过（迁移目标态） |
| `check_ai_meta.py` 第 11 项同一处正则 `L"(\w+)"` | 与 `check_commands.py` 同源缺陷：`win.quit` 被跳过，误报"别名 QuitExplorer -> win.quit：源命令不存在"，且覆盖率误报"1/137 条尚无显式别名" | 正则改 `L"([^"]+)"`；覆盖率按**两个方向**统计（左列旧名 ∪ 右列点分名），否则迁移完成的命令会被算成没覆盖 |
| `cmd_naming` 两个查表方向大小写不对称 | `AliasOf` 用 `find`（区分大小写）、`LegacyOf` 先 `Lower` —— 表现为"`nav.back` 认、`NavBack` 认、`navback` 不认"这种半吊子行为。命名测试断言 `FindCommand(L"navback") != nullptr` 一直失败 | 两个方向都改 `_wcsicmp` 遍历；局部 `Lower()`（第三个大小写归一实现）随之删除，同类工具统一在 `str::Lower` |
| `check_ai_meta.py` 硬编码旧名做键 | 点分名作主名后，`INDIRECT_ARGS` / `KNOWN_NO_TARGET` / `COVERS` / `UI_ONLY` 四张表的键全部失配，误报 19 条 | 全部改为点分名（`c['name']` 现在就是这个值） |
| `input_router::CurrentMods` 与 `eb::CurrentMods` 逐行重复 | 校验器以"需按焦点抑制"为由白名单放行，但两个版本一字不差，成员函数无任何额外逻辑 | 成员版改为 `return ::CurrentMods();`；白名单从 2 个文件收紧到 1 个 |

### 7.2 第二轮审查（2026-11-20 21:00）发现的问题

以"假设自己改错了"为前提重审，不重复上一轮结论。以下都是**上一轮没发现**的：

| 问题 | 性质 | 处置 |
|---|---|---|
| `FindCommand` 第三路是死路径 | 注释写"迁移期间三种组合"，但迁移已完成，`d.name` 全为点分名，第三路永不可能命中 | 删掉该分支；三路 137×3 次线性扫描改为 `unordered_map` 索引（`FindCommand` 在 `BuildBindings` 里被调 92 次） |
| **C1 判据漏报中间插入** | 原实现比"已有序项的相对顺序"，而 `[A,B,C]` vs `[A,X,B,C]` 的相对顺序**没变** → 最危险的情况（后续 id 全部位移）恰好漏报 | 改为**前缀判据** `new[:len(old)] == old`，并指出第一处不同的下标。实测注入中间项后能准确报出"枚举第 1 项起与基线不符" |
| **C1 读错了数据源** | 基线存的是 `commands.cpp` 的**表行顺序**，但 `CmdId` 数值来自 `commands.h` 的**枚举声明顺序**。两者当前确实不同（枚举 @69 = `CopyToFolder`，表 @69 = `FileFocusTop`）→ 既会误报（挪表行）又漏报（真改枚举） | 新增 `EnumOrder()` 从 `commands.h` 读取；重建基线 |
| `WM_COPYDATA` 魔数换值是错的 | 窗口类名未变，升级期新旧进程混跑时新进程发新魔数会被旧进程静默拒绝，用户看到"未响应"的误导提示 | **撤销换值**，恢复 `0xEBB1`，并在 `identity.h` / D9 说明"独立免除了与 mod 的兼容义务，但同一程序前后版本仍要兼容" |
| `UiPalette` 有 6 个令牌零消费 | `editFocus` / `editPlaceholder` / `listText` / `listInertText` / `chevronHot` / `panelBorder` 定义了但 `Theme` 从不取用 —— 改它们毫无效果，下一个改主题的人会以为在改 | 删掉 6 个；新增校验规则"令牌未被消费"防复发；位置式初始化改**指定初始化器**（23 个字段逐位核对，值零变化） |
| `CreateChild` 与 `CreatePopup` 约定不一致 | 前者传 `nullptr` 再事后设置，后者走 `lpCreateParams`。栏目前不用 `WM_NCCREATE` 所以侥幸无事，但下一个给栏加该处理的人会拿到空指针 | 统一走 `lpCreateParams` |
| R12（窗口类 ≤3）被错列为独立待办 | R12 原文写的是"**B5 之后**：窗口类 ≤3" —— 它依赖载体合并，不是命名清理能达成的 | 标注前置条件；当前 11 个类暂不启用该规则 |

### 7.3 第三轮审查（2026-11-20 22:00）：生产路径上的诊断代码

前两轮盯的是"名字"和"规则"，这一轮换了个角度查**性能**，发现的问题比命名问题严重：

| 问题 | 实测影响 | 处置 |
|---|---|---|
| **`ReapplyPreviewPane` 里塞着 ~160 行诊断** | 该函数由 **120ms 定时器**反复调用。诊断内容包括：在最多 40 列里逐个 `GetDetailsOf`+`StrRetToBufW` 找 Size 列、对前 4 项取 `PKEY_Size`（网络路径/大文件夹 = 真实 I/O）、用 `IColumnManager` 把 Size 列**移除再加回**（视图会闪）、以及一段 **UIA 全后代遍历**（`TreeScope_Descendants` + 逐元素取名字，跨进程调用）。用户每次把鼠标移过文件列表都要付一遍 | 抽出 `DiagnosePreviewPaneState()`，`-DEXBAR_DIAG_PREVIEW` 控制；`ReapplyPreviewPane` 从 168 行回到 13 行 |
| `SetViewMode` 里也有 93 行同类诊断 | 每次切换视图都执行一遍上述列枚举 + 逐项 I/O | 同上，`-DEXBAR_DIAG_COLUMNS` 控制 |
| 诊断代码虽在 `#if` 内，但**没有任何开关**（等于永久开启） | `strings ExBarBrowser.exe` 能查到 `[cols]`/`[uia]`/`[late]` 三个特征串 | 加开关后实测：产物中三个特征串**均 0 次**；运行 5 次视图切换，**新增日志 0 行** |
| `LogLine` 与 `LogDbg` 两个名字、实现逐字相同 | `shell_view.cpp` 用 `LogLine`（31 处），其余全用 `LogDbg`（54 处）。`log.h` 注释称 LogLine 是"兼容名"，但实际已是并列主名，新代码不知道用哪个 | 统一到 `LogDbg`，删除 `LogLine`；新增校验规则"日志入口唯一"（判据在**头文件**——入口的唯一性就是"对外声明了几个"，`log.cpp` 内的 `LogV` 是私有辅助不算） |
| `shell_view.cpp` 有一行注释掉的 `SetPropertyBag` 诊断 | 注释掉的代码会腐烂，且它带着已收编到 `identity.h` 的字符串 | 删除，改为说明"为什么不设持久 bag" |
| `common.h` 的日志注释说"两份轮转策略还不一致" | 该问题早已修复，注释是过时信息 | 更新为实际状态 |

**方法上的收获**：前两轮只查"是否违反规则"，规则之外的问题（性能、过时注释、注释掉的代码）全部漏掉。这类问题**静态检查抓不到**，只能靠"读生产路径上的每一行在运行时到底做了什么"。

---

## 8. 不做什么

- **不改** `EXBAR_*` 书签属性、`data/` 下文件名、`settings.json` 键名 —— 跨软件/用户契约。
- **不改** `CmdId` 枚举已有值 —— 稳定 id 原则（C1）。
- **不删** `legacyName` —— 旧名永不删（C2）。
- **不做** Opus 式粗粒度命令合并 —— 与 AI schema 需求冲突。
- **不引** 包前缀式命名（Emacs Lisp 模式）—— C++ 有 namespace。
