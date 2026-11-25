// 统一菜单构建层：所有自绘菜单的“内容”只在这里组装。
//
// 【为什么要有这一层】
// PopupList 只解决“怎么画一个弹层”，不解决“菜单长什么样”。以前每个调用点
// 都手写 `PopupRow{label, path, icon, folder, separator, disabled, data, glyph}`
// 八字段，于是同一类信息散在十几处：
//   · 勾选前缀 "✓ " 各处自己拼；
//   · 分隔行自己塞一个 data=-1 的空行；
//   · 禁用态靠调用方记得填对位置；
//   · 路径图标有的用 ico::Resolve 临时 HICON、有的用缓存 HICON，生命周期不一致；
//   · 初态行号靠 call site 数行数，增删一行就错位。
//
// 本组件把“菜单内容”收口成四个动作：Add / AddChecked / AddPath / Separator。
// Show() 再把构建结果交给唯一渲染器 ShowPopupList，返回业务 action（不是行号）。
// 调用方不再持有 PopupRow 向量，也不再关心图标资格与行号布局。
//
// 【Shell 自己跟踪菜单的场合】少数宿主必须交出 IContextMenu。那种场合用
// menu_context.h 的 ShellContextMenu：内容仍由本组件提供，桥接层只负责
// WM_MEASUREITEM/DRAWITEM 和动作回传。图标、命令身份、快捷键解析仍然只有这一份。
// 导航窗格空白区不在此列：它把显示推迟到本组件的 Show()，否则原生菜单窗口
// 自带一套边框/留白，无法与书签栏菜单逐像素一致（见 nav_pane_shell.cpp）。
//
// 【勾选态】勾选是 PopupRow 的**独立字段**（`checked`），不是拼在 label 里的
// "✓ "。弹层把勾号画在左侧**独立的一列**（系统字形，所有行上下对齐）——
// 与 Win32 / Windows 11 Fluent / ARIA menuitemcheckbox 同一口径。
// 见 popup_list.h 的 PopupRow::checked。
// 【快捷键列：由命令身份自动推出，不手写】
//
// 菜单行只声明"我执行的是哪条命令"（`CmdId`），键位文案在 Show 时由键位表现推。
// 于是：
//   · 给某条命令新增/改键（data/keymap.json 或快捷键管理器）后，**所有**声明了
//     该命令的菜单自动带上新键 —— 不需要回到每个调用点改一遍；
//   · 同一命令出现在多处（标签菜单的"关闭"、快捷键、命令面板），三处永远同一个键；
//   · 指错命令查不出来这件事从"运行期静默显示错的键"变成"代码里一眼看出"。
//
// 这就是 Win32 / Explorer / Win11 Fluent 的口径：菜单项挂的是命令 id，
// 快捷键来自独立的 accelerator 表，而不是把键名抄进菜单资源。
//
// `.Cmd()` 与 `.Key()` 的分工：
//   · Cmd(id[, arg])  —— 有对应命令的行，**必须**用它（绝大多数）；
//   · Key(spec)       —— 仅当这一行确实没有 CommandDef 对应时（纯 UI 局部动作，
//                        如"完成"、颜色档位、搜索历史条目）。给这类行写键名属于
//                        显式例外，评审时要能说出为什么。
//
// 【哪些行**不该**挂 Cmd（判断清单，来自实际踩过的错）】
//   · 这一行做的事**作用域不同**于同名命令：标签菜单「复制路径」复制的是**该标签**
//     的路径，而 path.copy/path.copy.dir 作用于当前活动标签/选中项 —— 右键非活动
//     标签时两者不是一回事（目录里只有 path.copy.tab = 复制**所有**标签路径）。
//     书签菜单「复制路径」同理（复制该书签自己的 path）。
//   · 这一行做的事**不是那条命令**：视图菜单「排序方式 →」只是打开二级层，而
//     ViewSort 按下去是直接切档；二级层「改为降序」是"同列反向"，也不是 ViewSort
//     的"切到某档"。挂上去都会显示一个"按下去做别的事"的键。
//   · 颜色色板的 8 个档位现在也挂 `.Cmd(CmdId::TabSetColor, 档位)`：它们确实
//     执行 tab.color，键位表按 `tab.color:0..7` 绑了哪档就自动显示哪档；
//     只有右键目标 ≠ 活动标签时不挂（键只会作用于活动标签，显示即谎）。
//   · 需要**非标量参数**的行（如"选择详细信息"的每列可见性要传列 id 数组）：
//     键位表里没有按列绑的键，挂了永远解析为空。
//
// 【带必填参数的命令也照挂】不给它开例外：键位表按空参执行做不出这一行的效果时，
// key_binding 会在解析层自动判空（HasRequiredArgs）。这样"哪天给它绑了键"就自动
// 生效，不需要回来补一行 .Cmd —— 判空由唯一实现负责，不是靠调用点记性。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "commands_ids.h"   // CmdId（命令身份：菜单自动键位的锚点）
#include "icon_resolve.h"
#include "popup_list.h"

namespace ui {

struct MenuOptions {
    int minWidthPx = 0;
    bool exactWidth = false;
    int maxRows = 20;
    const wchar_t* toggleKey = nullptr;
    int initialRow = -1;
    bool scrollTo = true;
    // 可选的行内拖拽重排通道（见 popup_list.h 的 PopupReorder）。
    // 默认 = 不启用拖拽（与从前完全一致）。
    //
    // 【什么时候该开】菜单里的行**代表一棵可重排的树**（书签分类里的条目）时。
    // 普通的动作菜单（打开/删除/…）不该开 —— 那种菜单的行不是可重排的列表。
    PopupReorder reorder;
};

class Menu {
public:
    // 普通动作行。glyph 必须是语义图标（ico::IconId），不是裸码位。
    Menu& Add(const std::wstring& label, int action, ico::IconId glyph,
              bool disabled = false);

    // 勾选行。勾选态落进 PopupRow::checked（独立字段），label 保持干净文案。
    // 返回行号，便于把当前项设为初态而不用手数行。
    int AddChecked(const std::wstring& label, bool checked, int action, ico::IconId glyph,
                   bool disabled = false);

    // 系统位图行：图标由调用方给（**借用**的 HICON，菜单不销毁）。
    // 状态栏的段勾选走它 —— 菜单里的段图标必须与栏上那一枚**长得一样**，
    // 否则同一个段在两处是两张脸。glyphFallback 是位图取不到时的字形兜底。
    int AddSystemIcon(const std::wstring& label, bool checked, int action, HICON icon,
                      ico::IconId glyphFallback = ico::IconId::None, bool disabled = false);

    // 路径行（历史 / 书签 / 溢出菜单）：图标走 ico::Cached，进程内共享、菜单不销毁。
    // folder=true 时行尾画 chevron。path 为空时退回 Folder 字形，不留空槽。
    int AddPath(const std::wstring& label, const std::wstring& path, int action,
                bool folder = false, bool disabled = false);

    // 路径行的勾选版（标签溢出菜单要标当前标签）。勾选态同样落进 checked 字段。
    int AddPathChecked(const std::wstring& label, bool checked, const std::wstring& path,
                       int action, bool folder = false, bool disabled = false);

    // 分隔行统一无 data、无图标、不可选。
    Menu& Separator();

    // 给指定行追加上说明文字（视图菜单的“（升序）/（降序）”）。
    // 行号越界时什么也不做，不让“菜单文案补充”反过来影响动作派发。
    Menu& AppendLabel(int row, const std::wstring& suffix);

    // 给指定行绑定**命令身份**：这一行执行的就是 CmdId 那条命令，快捷键自动取。
    //
    // arg 是内联参数（"3" = 第 4 档视图 / 第 2 种排序）。带参数的行必须写 arg，
    // 否则会匹配到同名命令**不带参**的那条键 —— 那是另一次操作。
    //
    // 行号越界 / 分隔行静默忽略（同 AppendLabel）；该命令当前没有可用键时
    // 该行右端留空（业界口径：有就画、没有就空，不画占位符、不画破折号）。
    Menu& Cmd(int row, CmdId id, const std::wstring& arg = std::wstring());

    // 链式便利版：给**上一行**绑定命令身份。绝大多数菜单是链式写的：
    //   menu.Add(L"关闭标签页", kTabMenuClose, ico::IconId::Close).Cmd(CmdId::TabClose)
    // 读起来就是"这一行执行 tab.close"，键位不在这行代码里出现（它属于键位表）。
    Menu& Cmd(CmdId id, const std::wstring& arg = std::wstring());

    // 显式键名（例外通道）。spec 是**命令规格**（`tab.close` / `view.mode:3`）。
    //
    // 【什么时候才用】这一行确实没有对应的 CommandDef（纯 UI 局部动作），
    // 但你仍然知道它对应哪个命令规格。能写 CmdId 就必须写 CmdId ——
    // 字符串不会在编译期被检查，写错只会静默留空、指错会显示别人的键。
    Menu& Shortcut(int row, const std::wstring& spec);
    Menu& Key(const std::wstring& spec);

    // 快捷键解析器：命令身份/规格 → 文案（"Ctrl+Shift+T"），空串 = 没有键。
    //
    // 【为什么是静态注入而不是构造参数】解析要读当前键位表与用户覆盖，
    // 那是 App/InputRouter 的状态；而 Menu 是纯 UI 组件，不能反向依赖输入端
    // （分层：ui 不 include input_router）。启动时注入一次即可，之后改键
    // Reload 后下一次 Show 自然读到新值 —— 这正是"自动"的机制所在。
    //
    // 未注入 = 不显示任何键位（纯逻辑单测因此不受影响，行为与从前完全一致）。
    using ShortcutResolver = std::function<std::wstring(CmdId, const std::wstring& arg)>;
    using SpecResolver = std::function<std::wstring(const std::wstring& spec)>;
    static void SetShortcutResolver(ShortcutResolver fn, SpecResolver specFn = nullptr);

    // 把已登记的命令身份/规格解析进 rows 的 shortcut 字段（Show 内部调它）。
    //
    // 【为什么公开】这是"身份 → 文案"的**接线**，而接线错了的表现是
    // "某几行显示了别人的键"——比推错一个键更难看，也更难在界面上发现。
    // 抽成不建窗就能调的纯函数，单测才能把它钉住（Show 需要真窗口 + 消息泵）。
    void FillShortcuts(std::vector<PopupRow>* rows) const;

    // 只读视图：给纯逻辑单测用；业务代码不应靠它再手写行。
    const std::vector<PopupRow>& rows() const { return rows_; }

    bool Empty() const { return rows_.empty(); }
    int Count() const { return static_cast<int>(rows_.size()); }

    // 模态展示；返回所选行的 action。取消 / 分隔 / 无效 data 一律返回 -1。
    int Show(HWND owner, const Theme& theme, POINT screen, const MenuOptions& opt = {}) const;

private:
    std::vector<PopupRow> rows_;
    // 每行的快捷键锚点（与 rows_ 一一对应）。二选一：
    //   · id != CmdId::None —— 该行执行这条命令，arg 是内联参数（"3"/空）；
    //   · spec 非空        —— 例外通道（这一行没有 CommandDef 对应）。
    // 【为什么不在 Add 时立刻解析成文案】解析要读当前键位表，而键位表可能在
    // 菜单构建与 Show 之间被 Reload 换掉（命令面板里改键就发生在菜单开着时）。
    // 存身份、Show 前统一解析，显示的永远是最新的生效键 —— 这就是"自动"。
    struct Anchor {
        CmdId id = CmdId::None;
        std::wstring arg;
        std::wstring spec;
    };
    std::vector<Anchor> anchors_;
};

}  // namespace ui
