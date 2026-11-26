// 快捷键管理器：自绘三栏面板（左 筛选 / 中 命令表 / 右 详情 + 底部状态栏）。
//
// 【为什么整窗自绘，而不是原来的 ListBox + ListView + BUTTON】
// 旧实现只有"底色 + 深色标题栏"跟着主题走：ListBox / ListView / BUTTON 全是系统
// 画的，深色主题下那块控件仍是浅色（表头、滚动条、按钮、选中行都在系统配色里），
// 于是窗口看起来像"深色底上贴了三块白板"。这不是调色能修的 —— 系统控件的配色
// 由 uxtheme 决定，SetWindowTheme(DarkMode_Explorer) 只能救回一部分，表头与
// 滚动条依旧浅色（Explorer++ 至今卡在这一步，见 docs/RESEARCH-OPENSOURCE-*）。
// 所以跟设置对话框一样：**所有像素自己画**，颜色只从 Theme 取。
//
// 【为什么是非模态】旧实现用 EnableWindow(owner, FALSE) 把主窗口锁死。可这个
// 面板的用途恰好是"改完立刻试"：用户改完键位想回主窗口按一下看生效没，模态就
// 必须关窗再开窗。设置对话框早就按非模态做了（见 settings_dialog.h 的说明），
// 这里对齐同一套心智。代价是"捕获键位期间要自己盯着焦点"——见 BeginCapture。
//
// 【分层与设置对话框一致】五个 .cpp 按"做什么"分，不按行数分：
//   hotkey_manager.cpp          窗口生命周期 / 消息路由
//   hotkey_manager_model.cpp    模型（哪些行可见 / 冲突是谁 / 搜索匹配）
//   hotkey_manager_layout.cpp   排布（画在哪）/ 命中（点到哪儿）—— 同一份矩形
//   hotkey_manager_draw.cpp     绘制
//   hotkey_manager_actions.cpp  交互（点击 / 键盘 / 改键 / 清除 / 还原）
#pragma once
#include <string>
#include <vector>

#include "common.h"
#include "icon_glyph.h"
#include "input_router.h"
#include "theme.h"

class HotkeyManager {
public:
    // 创建（owner = 主窗口；主窗口关掉时它跟着走）。已创建则直接返回 true。
    bool Create(HWND owner, const Theme& theme, InputRouter& router);
    // 显示并前置（不重复建窗口：两份管理器并排会让人搞不清改的是哪一份）。
    void Show();
    // 主题切换后重建字体/重排/重绘。App::ApplyTheme 调它（与 settingsDialog_ 同处）。
    void ReloadTheme();
    // 键位被**外部**改过（hotkey.set / hotkey.clear / hotkey.reset 命令、AI 通道）
    // 之后重读一遍。
    //
    // 【为什么需要】面板自己的改动会自己刷新，但命令总线上的改动它不知道 ——
    // 于是出现"AI 改了键、面板还显示旧键"。那是一块说谎的界面，比不显示更糟。
    // 只重读数据，不改筛选：外部改键不会改变"有哪些命令"。
    void OnKeymapChangedExternally();
    bool Visible() const;

private:
    // 一行的状态档。文案与颜色由 StatusText/StatusColor 一处给
    // （绘制与命中不各判一次 —— 两处各判一次迟早会分叉）。
    enum RowStatus { kOk = 0, kConflict, kReserved, kUnimplemented };

    // 一行 = 路由层的一行 + 派生态。
    //
    // 【为什么不直接拿 BindingRow 用】"冲突是同谁撞的"只有面板知道
    // （要给出**对方的名字**，而不是一个 bool）；"键位显示成什么"也要按
    // 序列/未绑定分别处理。这些是展示语义，不该塞回输入端。
    struct Row {
        BindingRow src;
        std::wstring keyText;   // 单元格文本："Ctrl+W" / "（未绑定）" / "g g"
        std::wstring peers;     // 同键的其它命令名（" · " 分隔；空 = 没冲突）
        int status = kOk;
        bool sequence = false;
        bool custom = false;    // 用户改过（键位芯片前画实心点）
        bool unbound = false;   // 显式解绑（画空心点）
    };

    // 左栏一个筛选项。
    //
    // 【为什么不写死一串中文】旧实现把 8 个范围名硬编码在数组里，再拿**字符串**
    // 去比对 r.scope —— 只要某个命令的 scopeUi 不在那 8 个里（新增一类、
    // 或拼写不同），它的行就永远只能在"全部"下看到，而且没有任何检查会报。
    // 现在筛选项从数据里长出来：范围名取实际出现的值，计数也是数出来的。
    struct Group {
        std::wstring id;      // "all" / "custom" / "conflict" / "sequence" / "scope:<名>"
        std::wstring name;
        ico::IconId icon = ico::IconId::None;
        int total = 0;        // 组内总项数
        int hits = 0;         // 当前搜索词下的命中数
    };

    // 一行在屏幕上的几何。绘制与命中**共用**这一份：两处各算一遍迟早错位，
    // 错位的表现是"点在键位芯片上却选中了下一行"。
    struct RowLayout {
        RECT row{};
        RECT title{};
        RECT scope{};
        RECT chip{};
        RECT status{};
    };

    // 右栏几何。同样是绘制与命中共用。
    struct DetailLayout {
        RECT title{};
        RECT cmd{};
        RECT keyLabel{};
        RECT keyChip{};
        RECT meta{};         // "来源 默认 · 范围 标签栏"
        RECT defaultKey{};   // "默认 Ctrl+L"
        RECT conflict{};     // 冲突说明（可能多行）
        RECT edit{};
        RECT clear{};
        RECT reset{};
        RECT resetAll{};
        RECT hint{};
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SearchSub(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // 搜索框占位文字。与设置对话框同一条规则：框里的字 == 占位文字时不算搜索词
    // （不引入"框是空的"这个额外状态位，两处各存一份必然会不同步）。
    // 不用 EM_SETCUEBANNER：那要 comctl32 v6 清单，本工程没有清单。
    static constexpr const wchar_t* kSearchPlaceholder = L"搜索命令、键位或 id";

    // ---- 模型（hotkey_manager_model.cpp）----
    void RebuildRows();     // 从 router 重读 + 派生（冲突对方 / 状态 / 键位文本）
    void RebuildGroups();   // 从数据长出筛选项（含计数）
    void Refilter();        // 按 (group_, query_) 重算 view_ 与各组 hits
    bool Matches(const Row& r) const;
    const Row* Selected() const;
    int SelectedViewIndex() const;              // -1 = 没选中 / 不在当前视图里
    void SelectViewIndex(int vi);

    // ---- 排布与命中（hotkey_manager_layout.cpp）----
    static void MinClientSize(const Theme& theme, int* w, int* h);
    void Layout();
    void LayoutRows();
    void LayoutRight();
    int RowHeight() const;
    int HitGroup(int x, int y) const;      // -1 = 没命中
    int HitRow(int x, int y) const;        // view_ 下标；-1 = 没命中
    int HitDetail(int x, int y) const;     // 0 无 / 1 编辑 / 2 清除 / 3 还原此项 / 4 还原全部
    void ScrollBy(int delta);
    void EnsureVisible(int viewIndex);

    // ---- 绘制（hotkey_manager_draw.cpp）----
    void Paint(HWND hwnd);
    void PaintRail(HDC dc);
    void PaintTable(HDC dc);
    void PaintRow(HDC dc, int viewIndex);
    void PaintDetail(HDC dc);
    void PaintStatus(HDC dc);
    // 键位芯片（键帽那个样子）。同一处画，中栏与右栏共用，
    // 于是"同一组合键在两处宽度不同"这种低级不一致不可能发生。
    // fg 由调用方给（未绑定用 inert、冲突用 danger），底色与边框只在这里定。
    void PaintChip(HDC dc, const RECT& rc, const std::wstring& text, bool emphasis, COLORREF fg);
    // 一颗小方块：自定义 = accent，已解绑 = inert。没这两者就不画。
    void PaintSourceDot(HDC dc, const RECT& rc, const Row& row);

    // ---- 交互（hotkey_manager_actions.cpp）----
    void OnClick(int x, int y);
    void OnDoubleClick(int x, int y);
    void OnKeyDown(UINT vk);
    void OnTimer(UINT_PTR id);
    void ToggleRecording();
    void StopRecording();
    void BeginCapture();
    void CancelCapture();
    // 捕获到一个完整组合键。可能先要过"这键已经被别人占了"这一关。
    void CommitCapture(const KeyChord& c);
    void ClearSelected();
    void ResetSelected();
    void ResetAll();
    void SetStatus(const std::wstring& msg, bool error = false);
    // 当前按住的修饰键文案（"Ctrl+Shift+"），用于捕获/录制期间的实时回显。
    std::wstring HeldModsText() const;
    void SyncSearchBox();
    std::wstring SearchRawText() const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND searchEdit_ = nullptr;
    const Theme* theme_ = nullptr;
    InputRouter* router_ = nullptr;

    std::vector<Row> rows_;      // 全量（与 router_->Rows() 同序）
    std::vector<Group> groups_;
    std::vector<int> view_;      // 可见行（rows_ 下标）
    std::vector<RowLayout> rowLayout_;   // 与 view_ 同序

    std::wstring group_ = L"all";
    std::wstring selSetting_;    // 选中行的 setting（跨重排稳定）
    std::wstring query_;
    std::wstring searchText_;
    bool searchPlaceholder_ = true;

    std::wstring status_;
    bool statusError_ = false;

    // ---- 捕获 ----
    bool capturing_ = false;
    std::wstring captureSetting_;
    // 冲突待确认：已按过的组合键 + 占用它的命令名。再按同一个键才真写下去。
    bool conflictPending_ = false;
    KeyChord pendingChord_;
    std::wstring pendingPeer_;

    // ---- 录制（按键盘来筛选，VS Code 的 Record Keys）----
    bool recording_ = false;
    int recordMods_ = 0;

    // ---- 还原全部默认的两步确认（不用 MessageBox：那是系统浅色框，深色下刺眼）----
    bool resetArmed_ = false;

    int hoverRow_ = -1;
    int hoverGroup_ = -1;
    bool hoverRecord_ = false;
    bool tracking_ = false;

    int scroll_ = 0;
    int scrollMax_ = 0;

    int totalAll_ = 0;
    int totalCustom_ = 0;
    int totalConflict_ = 0;

    // 几何（物理像素）
    RECT colA_{};
    RECT colB_{};
    RECT colC_{};
    RECT statusBar_{};
    RECT searchBox_{};
    RECT railList_{};
    RECT railFoot_{};
    RECT recordBtn_{};
    RECT head_{};
    RECT tableHead_{};
    RECT body_{};
    RECT detailBox_{};
    DetailLayout detail_{};

    static constexpr UINT_PTR kTimerReset = 0x5301;
};
