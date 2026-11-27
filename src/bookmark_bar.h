// 自绘书签栏：图标 + 名称 + 分类下拉 + 溢出 » + 右键菜单 + 拖入文件夹添加。
//
// 数据来自 BookmarkStore（便携模式：<exe>\data\bookmarks.html，不再与 mod 共用）；
// 本类只读展示 + 发导航/命令，增删改由 App 的 Bmk* 命令完成。
#pragma once
#include <string>
#include <vector>

#include "bookmarks.h"
#include "commands_ids.h"   // CmdId：SetShortcutFn 的入参（头文件自足，不靠 PCH 间接给）
#include "common.h"
#include "tab_uia.h"
#include "tooltip_host.h"
#include "theme.h"

#include "edge_autoscroll.h"

// 书签栏的 UIA Traits 与 WM_GETOBJECT 转发（provider 本体与标签栏共用，
// 见 tab_uia.h）。消息分支 kMsgCount/kMsgItem/kMsgSelect 在 bookmark_bar.cpp。
class BookmarkBar;
namespace bmuia {
uia::Traits TraitsFor(BookmarkBar* b);
LRESULT HandleGetObject(BookmarkBar* b, HWND hwnd, WPARAM w, LPARAM l);
}  // namespace bmuia

// 一次落点的完整语义：顶层插入线 / 拖入分类，两者互斥。
// valid=false 表示落在栏外（或没有可用栏窗口），调用方按"取消"处理。
struct BookmarkDropSpot {
    bool valid = false;
    int index = -1;         // 顶层插入线（items_ 下标，0..size）
    int folderIndex = -1;   // 悬停分类的可见项下标（用于画描边）
    std::wstring folderId;  // 非空 = 拖进该分类
};

// ---------------------------------------------------------------------------
// 书签栏横向滚动的几何判据（纯函数，便于单测）
//
// 【为什么要有滚动】书签一多就放不下。以前放不下的项被"折叠"（rect 置空），
// 栏本身横向挪不动 —— 用户只能从 » 菜单里点。要求是"鼠标停在书签栏上滚滚轮，
// 后面的书签就一个个滑进来"。
//
// 模型与标签栏同构（见 tab_strip.h）：内容整体左移 scrollX_，可视窗口右缘是
// 裁切线 viewRight_（» 按钮画在它右边，不属于任何书签）。
//
// 可见性判据集中在这里，**唯一来源**：绘制 / 命中 / 提示 / 溢出菜单 / 键盘导航
// 全都问同一组函数。否则就会出现"画出来了却点不到"或"已经滚进视野的项还挂在
// » 菜单里"这类分叉（折叠语义时代判据散在 8 处，每换一次语义就得改 8 遍）。
// ---------------------------------------------------------------------------

// 项矩形 r 与可视窗口 [0, viewRight) 求交：有交集 = 至少露出来一部分。
inline bool BmkInView(const RECT& r, int viewRight) {
    return r.right > 0 && r.left < viewRight;
}

// 完全落在窗口内（左右都没被切掉）。
inline bool BmkFullyInView(const RECT& r, int viewRight) {
    return r.left >= 0 && r.right <= viewRight;
}

// 命中判据：点必须落在可视窗口内。裁切线右侧是 » 按钮，不属于任何书签 ——
// 不按"点"判而只按"项自己的矩形"判的话，跨过裁切线的那一项会盖在 » 下面，
// 于是点 » 变成点它，溢出菜单永远打不开（标签栏踩过同一个坑，见 TabHitInView）。
inline bool BmkHitInView(int x, int viewRight) {
    return x >= 0 && x < viewRight;
}

// 滚动量夹紧到 [0, maxScroll]。
inline int BmkClampScroll(int scroll, int delta, int maxScroll) {
    int next = scroll + delta;
    if (next < 0) next = 0;
    if (next > maxScroll) next = maxScroll;
    return next;
}

// 让这一项露出来需要的位移：露在左边就往回滚（正数 = scrollX 减小），
// 露在右边就往前滚（负数 = scrollX 增大）。0 = 已经在视野内。
inline int BmkScrollToShow(const RECT& r, int viewRight) {
    if (r.left < 0) return r.left;
    if (r.right > viewRight) return r.right - viewRight;
    return 0;
}

// 滚轮方向 → 滚动方向。
// 垂直轮：向上滚（delta>0）= 看更早的书签（scrollX 减小），与标签栏同一口径
// （tab_strip_input.cpp：滚轮向上先看左边的内容）。
// 横向轮：向右倾（delta>0）= 看更晚的书签（scrollX 增大），Windows 文档口径。
inline int BmkWheelScroll(int delta, int step, bool horizontal) {
    if (horizontal) return delta > 0 ? step : -step;
    return delta > 0 ? -step : step;
}

// 把矩形右缘裁到可视窗口（» 按钮在它右边）。左缘不裁：x<0 的部分栏窗口自己
// 就会裁掉，而且**不能**把左缘也裁到 0 再重画 —— 那样滚出左边的书签会从
// 第一个字重新显示，而不是显示它被切掉的那一段。
inline RECT BmkClipRight(const RECT& r, int viewRight) {
    RECT out = r;
    if (out.right > viewRight) out.right = viewRight;
    return out;
}

class BookmarkBar {
public:
    void Create(HWND parent, const Theme& theme);
    ~BookmarkBar();

    HWND hwnd() const { return hwnd_; }
    void SetNavigateFn(std::function<void(const std::wstring&)> fn) { onNavigate_ = std::move(fn); }
    void SetMouseSink(MouseSink fn) { sink_ = std::move(fn); }
    // 键盘退出本栏（Esc）时把焦点还给视图。与 nav_bar 的 SetRefocusFn 同一契约：
    // 焦点卡在栏上会让文件视图的 Delete / type-ahead 全部失效。
    void SetRefocusFn(std::function<void()> fn) { refocus_ = std::move(fn); }
    // 取某条命令**当前生效**的快捷键文案（"Ctrl+D"），空串 = 现在按不出来。
    // 空状态提示要显示"按什么键能收藏/能聚焦本栏"—— 键位是用户可改的，
    // 文案里写死 Ctrl+D / Ctrl+B 会在用户改键后变成假提示（按下去没反应）。
    // 注入而不是让本类自己读 keymap：覆盖表、保留键剔除、先到先得都在
    // InputRouter 里算过，这里拿到的必须是那一份推导的结果（唯一真相）。
    void SetShortcutFn(std::function<std::wstring(CmdId)> fn) { shortcutFor_ = std::move(fn); }
    void Reload();        // 用内存中的 store 重排
    void ReloadFromDisk();// 重新读文件
    void ReloadTheme();
    // 键盘进入本栏：把焦点给窗口并把 kbdIdx_ 落在第一项。
    // 本程序不跑 IsDialogMessage，Tab 不是焦点遍历键 —— 所以"到达书签栏"
    // 必须由一条命令/热键显式触发（与 F4 到地址栏同一模式）。
    void FocusBar();

    // OLE 拖放支持（bookmark_drop 调用）：
    BookmarkDropSpot DropSpotAt(POINT screen) const;  // 屏幕点 → 插入线 / 目标分类
    void PreviewDrop(const BookmarkDropSpot& spot);   // 外部拖放预览（与内部拖动共用绘制）
    // OLE DragOver 的每次回调（屏幕点）：落点预览 + 边缘自动滚动起停。
    // 外部拖文件夹与内部拖书签必须获得同一种"拖到边缘自动滚过去"的能力，
    // 所以预览与滚动在这里一步做完（bookmark_drop.cpp 只调这一个）。
    void PreviewDropAt(POINT screen);
    void ClearDropPreview();
    // 把外部拖入的路径写成命令（审计/撤销/AI 同源）。返回成功条数。
    int ApplyDroppedPaths(const std::vector<std::wstring>& paths, const BookmarkDropSpot& spot);
    std::wstring FolderIdAt(int index) const;        // 项索引 → 文件夹书签 id（非文件夹=空）
    void ReloadExternal();                           // 拖放完成后的外部触发的刷新
    // 宿主按光标位置把滚轮转交给本栏。返回 true = 已处理。
    //
    // 【为什么需要这一条】滚轮消息只发给**焦点窗口**。光标停在书签栏上、
    // 焦点却在文件列表时，书签栏收不到 WM_MOUSEWHEEL —— 用户在栏上滚滚轮，
    // 滚的却是背后的文件列表。宿主在消息循环里（派发之前）按光标位置转交，
    // 与序列弹层用的是同一手法（见 input_router_overlay.cpp 的同类说明）。
    bool HandleWheelAt(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    struct Item {
        std::wstring id;
        std::wstring label;
        std::wstring path;  // 分类为空
        bool folder = false;
        HICON icon = nullptr;
        RECT rc{};
        RECT iconRc{};      // 左侧图标命中区：点击 → 子目录树（等同面包屑分隔符）
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    // 键盘导航一族（方向键 / Home / End / 回车 / 菜单键）。
    // 拆到 bookmark_bar_keys.cpp：Handle 里塞着绘制、焦点、UIA、拖拽、菜单、
    // 键盘六类消息，键盘这一族自成一体（"焦点项游标 + 保证可见"一个语义），
    // 不该让人在几百行 switch 里翻那几十行。
    LRESULT HandleKeyDown(HWND, WPARAM, LPARAM);
    void BuildItems();
    void Layout();
    void Paint(HWND);
    void UpdateHover(HWND, int x, int y);
    int HitItem(int x, int y) const;
    int HitIcon(int x, int y) const;
    bool HitOverflow(int x, int y) const;
    // 横向滚动（滚轮）：ScrollBy 改滚动量并重排，EnsureVisible 把某一项带进视野。
    // ScrollBy 是**唯一**的滚动入口：滚轮 / 键盘跟随 / 拖拽自动滚动都走它，
    // 滚动后的派生状态（拖拽落点、悬停高亮）也只在这里统一重算 ——
    // 不收口的话，"滚了但插入线/高亮还指旧项"这类错位每个滚动源各犯一遍。
    void ScrollBy(int dx);
    void EnsureVisible(int index);
    // 滚轮处理：先问鼠标规则（用户可能把"书签栏上滚滚轮"绑成别的动作），
    // 规则没接管才走默认的横向滚动。返回 true = 已消费。
    bool OnWheel(UINT msg, WPARAM w, LPARAM l);
    BookmarkDropSpot DropSpotAtClient(int x, int y) const;  // 内外拖动共用落点算法
    HICON IconFor(const BookmarkNode& n);
    void ShowItemMenu(int index, POINT screen);
    // "移到分类…"的二层菜单：列出所有分类 + 顶层，走 bmk.move 命令总线。
    void ShowMoveToMenu(const std::wstring& id, const std::wstring& label, POINT screen);
    void ShowEmptyMenu(POINT screen);
    void ShowFolderMenu(int index, POINT screen);
    // 分类内容弹层（可拖动排序 + 行内右键），实现见 bookmark_category_popup.cpp。
    // draggingId 非空 = "拖拽移交"：一场从书签栏拖进来、悬停分类自动弹出时
    // 还在进行的拖拽 —— 弹层进场即在拖拽态，from=-1 表示这个外部条目，
    // 松手按弹层落点提交（而不是只能在栏上"加入末尾"）。
    void ShowFolderContents(const std::wstring& folderId, POINT screen,
                            const std::wstring& draggingId = L"");
    void ShowCategoryItemMenu(const std::wstring& folderId, int row, POINT screen);
    void ShowChildTree(int index);   // 图标：列该路径的子目录，可就地逐级展开
    void ShowOverflowMenu(POINT screen);
    void Navigate(int index);
    // ---- 打开语义（bookmark_bar_open.cpp；Chrome/Edge/Firefox 书签栏通用标准）----
    // 中键按下：书签 → 新标签（后台）；分类 → 整组子项各开一个标签（浏览器
    // 「打开全部」的中键路径，与 Vivaldi/Chrome 的 "Open all" 同语义）。
    // 返回 true = 已消费（调用方不再走左键默认路径）。
    bool OnMiddleDown(int x, int y);
    // 叶子书签的修饰键覆盖（Ctrl / Shift / 「固定新标签」开关）。
    // 返回 true = 已按语义打开，调用方直接返回；false = 走默认（当前标签导航）。
    bool ConsumeLeafClickMods(const Item& it, int mods);
    // 按路径打开，带修饰键语义 —— **打开动作的唯一收口点**。
    // 栏上的点击、» 溢出菜单、分类弹层三个入口都调它（此前只接了第一个，
    // 于是另外两个入口的 Ctrl/Shift 无效、且绕过命令总线）。
    // 返回 true = 已打开（调用方不要再导航）；false = 无修饰覆盖，走默认。
    bool OpenPathWithMods(const std::wstring& path, int mods);
    // 把一个分类的全部可打开子项各开一个标签（「打开全部」）。
    // background = true 时不打断当前视图（中键/菜单默认）；返回成功条数。
    int OpenFolderChildrenInTabs(const std::wstring& folderId, bool background);
    // UIA 的"激活"（IInvokeProvider::Invoke）：真的打开这一项。
    // 与鼠标左键同一条语义（叶子走 OpenPathWithMods，分类弹分类菜单），
    // 只是触发源不同 —— 无障碍入口不该是第二套操作模型。
    // 【为什么不是 Select】UIA 里 Select = 选中（焦点过去），Invoke = 激活（执行）。
    // 两者原先发同一条消息，导致读屏软件"激活书签"只移焦点、不打开（E23）。
    void UiaInvoke(int index);
    // 分类下可打开的叶子数（递归）。菜单文案「打开全部（N 项）」用它，
    // 与 OpenFolderChildrenInTabs 必须同口径 —— 写的数字与实际开的标签数不符，
    // 比不写数字更糟。
    int CountOpenableChildren(const std::wstring& folderId) const;
    void RebuildTooltip(HWND);
    void DragUpdate(HWND h, int x, int y);   // 拖动中：算插入线 + 重绘
    void DragFinish(HWND h, bool commit);    // 松手提交 / 取消

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    ui::TooltipHost tooltip_;
    const Theme* theme_ = nullptr;
    std::vector<Item> items_;
    RECT ovfRc_{};
    bool overflow_ = false;
    // ---- 横向滚动 ----
    // scrollX_：内容整体左移的像素数（滚轮往回滚就减小）。
    // maxScroll_：滚动上限（内容末端顶到裁切线为止）。
    // viewRight_：可视窗口右缘（裁切线）。» 按钮画在它右边，不属于任何书签。
    int scrollX_ = 0;
    int maxScroll_ = 0;
    int viewRight_ = 0;
    int hover_ = -1;
    int hoverIcon_ = -1;   // 悬停在图标格上的项（高亮该格，提示"这里能点开子目录"）
    bool hotOvf_ = false;
    bool tracking_ = false;

    // 顶层拖动重排（仅文件系统样式的可见项；按下→移动超阈值→松手提交 Move）
    bool dragArm_ = false;     // 按下未超阈值
    bool dragging_ = false;    // 已超阈值，正在拖
    int dragIdx_ = -1;         // 起始项
    int dropIdx_ = -1;         // 插入线位置（绘制到 dropIdx_ 左缘）
    int dropInto_ = -1;        // 拖到某个分类"内部"时该分类的项索引（-1 = 无）
    bool dropPreview_ = false; // 落点预览可见（内部拖动 / 外部 OLE 拖放共用）
    POINT dragStart_{};
    // 悬停分类自动弹出：拖进分类"内部"后停 600ms，把进行中的拖拽移交给
    // 分类内容弹层（拖拽继续，落点跟着弹层行走）。folderHoverIdx_ 是定时器
    // 挂着的那个分类项索引（换分类 / 拖出 / 松手都要摘掉定时器）。
    static constexpr UINT kDragFolderTimer = 1;
    int folderHoverIdx_ = -1;
    void HandoffDragToFolder(HWND h);   // 定时器到点：移交拖拽给分类弹层

    // 拖拽边缘自动滚动（edge_autoscroll.h 的共享实现，与标签栏同一套）：
    // 拖到栏左/右边缘带内就按拍滚动，让视野外的书签也能成为落点。
    // 内部重排（DragUpdate）与 OLE 外部拖入（PreviewDropAt）两条链路都走
    // EdgeAutoScroll —— 起停判定只此一份，差别只在外部拖入的坐标是屏幕点。
    // 定时器 id 与分类悬停定时器区分（2 = 滚动拍）。
    static constexpr UINT kDragScrollTimer = 2;
    int dragScrollStep_ = 0;    // 0=停；>0 向右；<0 向左（edge::Update 维护）
    int dragMouseX_ = 0;        // 定时器拍没有鼠标坐标，缓存最后一次鼠标 x
    int dragMouseY_ = 0;        // 与 x 成对缓存（落点重算 DropSpotAtClient 需要整点）
    void EdgeAutoScroll(int clientX, int clientY);   // 起停判定 + 最后鼠标 x 缓存（两链路共用）

    // ---- 键盘访问 ----
    //
    // 书签栏原先**完全没有**键盘路径：只能鼠标点。而标签栏早就有焦点态与裸键处理，
    // 于是"用键盘切换标签可以、用键盘选书签不行" —— 这不是取舍，是缺口。
    // Opus / Total Commander / XYplorer 的收藏栏都能 Tab 进去、方向键走、Enter 打开、
    // Shift+F10 出菜单。这里照同一套做：
    //   · Tab / Shift+Tab 到达本栏 → kbdIdx_ 落在第一项（或上次的位置）
    //   · ←/→ 在项间移动，Home/End 到首/尾
    //   · Enter / 空格 → 打开（分类则展开菜单）
    //   · Shift+F10 / 应用键 → 该项的右键菜单（无障碍要求：图标功能都要有键盘路径）
    //   · Esc → 把焦点还给视图（不然焦点卡在栏上，Delete/type-ahead 全哑）
    int kbdIdx_ = -1;
    void PaintFocusRing(HDC dc) const;

    // 「移到分类…」的最近目标（MRU，最多 5 条）。只在本窗口会话内有效：
    // 它仅是菜单排序提示，不写配置、丢了不影响任何数据或撤销记录。
    std::vector<std::wstring> recentMoveTargets_;

    std::function<void(const std::wstring&)> onNavigate_;
    std::function<void()> refocus_;
    // 键位文案取数口（见 SetShortcutFn）。空 = 未注入，空状态提示退化成
    // 不带键名的版本 —— 宁可少说一句，也不显示一个可能按不出来的键。
    std::function<std::wstring(CmdId)> shortcutFor_;
    MouseSink sink_;
};
