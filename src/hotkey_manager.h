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

    // 排序键。点表头切换，再点同列反向。
    //
    // 【为什么"命令"是默认且必须存在】键位表原本的顺序是 keymap.json 的书写顺序，
    // 那对用户毫无意义（既不是字母序也不是分组序）。默认按标题排，至少能
    // "按名字找一条"；而按键位排是排查冲突时唯一有用的视角。
    enum SortKey { kSortTitle = 0, kSortKey, kSortScope, kSortStatus };

    // 行类型：组合键 / 序列键。
    //
    // 【为什么必须分开而不是混排】这两类东西的性质根本不同：
    //   · 组合键 —— 单个 chord，可在这里改键/清除/还原，落 settings.json 的覆盖表
    //   · 序列键 —— 多键状态机（前缀 + 超时 + 唯一匹配），**只读**，要改得动 keymap.json
    // 混在一张表里时，112 条可编辑的行被 169 条只读的行埋掉：用户滚半天点中一条
    // 才发现按钮是灰的。分开之后，"能改的东西"与"参考资料"各占一张表，
    // 工具栏的启用态也就不会整片发灰。
    enum RowType { kTypeChord = 0, kTypeSeq };

    // 工具栏按钮 id。0 保留为"没命中"，所以从 1 起。
    enum ToolId {
        kToolNone = 0,
        kToolEdit = 1,     // 改键（等同回车）
        kToolClear,        // 清除键位（写解绑哨兵）
        kToolReset,        // 还原此项默认（删覆盖项）
        kToolUndo,         // 撤销
        kToolRedo,         // 重做
        kToolCopyId,       // 复制命令 id
        kToolExport,       // 导出覆盖表到剪贴板
        kToolImport,       // 从剪贴板导入覆盖表
    };

    // 一次可撤销的改动。
    //
    // 【为什么记 setting + 旧值，而不是记"命令 + 键"】还原动作的旧值可能是
    // "没有覆盖项"（回落到默认），那与"覆盖成某个键"是两种状态，用一个字符串
    // 表达不了。oldKey 空 = 原本没有覆盖项（还原时应当删覆盖项）。
    struct UndoStep {
        std::wstring setting;
        std::wstring oldKey;   // 空 = 原本无覆盖项
        std::wstring newKey;   // 空 = 改动后无覆盖项
    };

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

    // 工具栏上的一颗按钮。
    //
    // 【为什么按钮表是数据而不是一串手写矩形】工具栏是这一版新增的骨架，按钮
    // 数量会随功能长（现在 6 颗，将来可能加"比对/预设"）。手写矩形意味着每加
    // 一颗都要同步改布局、命中、绘制、禁用判定四处；做成表之后，加按钮只是
    // 往 kToolbar 里加一行，四处自动跟上。
    struct ToolButton {
        int id;                // ToolId
        ico::IconId icon;
        const wchar_t* text;   // 按钮文字（窄窗口下省略，只留图标）
        const wchar_t* tip;    // 状态栏悬停提示
    };
    struct ToolRect {
        int id = 0;
        RECT rc{};
    };

    // 表头列（排序 + 命中共用一份定义）。
    //
    // 【为什么要抽出来】表头绘制、排序命中、列矩形三处都要知道"有哪些列、
    // 各占哪一段"。以前绘制一处按行矩形反推、命中一处再反推一遍 ——
    // 两份反推迟早分叉，表现是"点'键位'却按'范围'排了"。
    struct Column {
        int sortKey;           // SortKey
        const wchar_t* title;
        RECT rc{};
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
    // 排序：设键 + 方向（同列再点反向）。改完要 Refilter（view_ 的顺序就是显示顺序）。
    void SetSort(SortKey key);
    // 切换筛选项/表。group_ 与 tableType_ 必须一致，所以只允许经过这里改。
    void SetGroup(const std::wstring& id);
    bool IsSelectedSetting(const std::wstring& setting) const;
    void SelectAllVisible();
    void ClearMultiSelection();
    // 多选时受影响的 setting 集合：多选非空则用它，否则用当前单选行。
    std::vector<std::wstring> TargetSettings() const;

    // ---- 撤销（hotkey_manager_actions.cpp）----
    void PushUndo(const UndoStep& step);
    void UndoLast();
    void RedoLast();
    void ApplyUndoStep(const UndoStep& step, bool forward);

    // ---- 排布与命中（hotkey_manager_layout.cpp）----
    static void MinClientSize(const Theme& theme, int* w, int* h);
    // 工具栏按钮表（唯一来源，定义在 layout.cpp）。布局/命中/绘制/启用判定共用。
    static const ToolButton* ToolbarItems(int* count);
    void Layout();
    void LayoutToolbar();
    void LayoutRows();
    void LayoutRight();
    int RowHeight() const;
    int HitGroup(int x, int y) const;      // -1 = 没命中
    int HitRow(int x, int y) const;        // view_ 下标；-1 = 没命中
    int HitDetail(int x, int y) const;     // 0 无 / 1 编辑 / 2 清除 / 3 还原此项 / 4 还原全部
    int HitTool(int x, int y) const;       // ToolId；0 = 没命中
    int HitSortHeader(int x, int y) const; // SortKey；-1 = 没命中
    void ScrollBy(int delta);
    void EnsureVisible(int viewIndex);
    bool ToolEnabled(int id) const;

    // ---- 绘制（hotkey_manager_draw.cpp）----
    void Paint(HWND hwnd);
    void PaintRail(HDC dc);
    void PaintToolbar(HDC dc);
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
    void OnRightClick(int x, int y);
    void OnKeyDown(UINT vk);
    void OnTimer(UINT_PTR id);
    void OnTool(int id);
    void ToggleRecording();
    void StopRecording();
    void BeginCapture();
    void CancelCapture();
    // 捕获到一个完整组合键。可能先要过"这键已经被别人占了"这一关。
    void CommitCapture(const KeyChord& c);
    void ClearSelected();
    void ResetSelected();
    void ResetAll();
    // 复制选中行的命令 id（可粘到 hotkey.set 里）。多选时逐行。
    void CopySelectedIds();
    void ExportToClipboard();
    void ImportFromClipboard();
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

    // 分表：组合键 / 序列键。左栏点这两组就切表（见 Refilter）。
    //
    // 【为什么用"当前表"而不是"两张表各滚各的"】同一时刻只有一个滚动位置、
    // 一个选中项、一个详情面板。两张表各自独立滚动听起来更"完整"，但用户
    // 切回来会发现"刚才看的那条不知道在哪了" —— 而这两张表的规模（112 / 169）
    // 都不需要各自的滚动记忆。
    RowType tableType_ = kTypeChord;

    std::wstring group_ = L"all";
    std::wstring selSetting_;    // 选中行的 setting（跨重排稳定）
    std::wstring query_;
    std::wstring searchText_;
    bool searchPlaceholder_ = true;

    // ---- 排序 ----
    SortKey sortKey_ = kSortTitle;
    bool sortAsc_ = true;

    // ---- 多选（Ctrl/Shift 点击、Ctrl+A）。存 setting 而不是下标：重排/筛选后下标全变。----
    std::vector<std::wstring> multiSel_;
    int anchorView_ = -1;        // Shift 连选的锚点（view_ 下标）

    // ---- 撤销 / 重做 ----
    //
    // 【为什么要撤销】改键是直接落盘的（settings.json）。一次误点就把用户
    // 用了半年的键位覆盖掉了 —— 而"还原此项默认"只对**有覆盖项**的行有效，
    // 覆盖掉一个原本是默认的键之后，用户想回到"刚才那样"没有任何路径。
    // 撤销栈补的就是这条路。
    std::vector<UndoStep> undo_;
    std::vector<UndoStep> redo_;
    static constexpr size_t kUndoLimit = 100;

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
    int hoverTool_ = 0;          // ToolId；0 = 没悬停在按钮上
    int hoverSort_ = -1;         // SortKey；-1 = 没悬停在表头上
    bool tracking_ = false;

    int scroll_ = 0;
    int scrollMax_ = 0;

    int totalAll_ = 0;
    int totalCustom_ = 0;
    int totalConflict_ = 0;

    // 几何（物理像素）
    RECT toolbar_{};
    std::vector<ToolRect> toolRects_;
    RECT colA_{};
    RECT colB_{};
    RECT colC_{};
    RECT statusBar_{};
    RECT searchBox_{};
    RECT searchField_{};     // 工具栏上的搜索框（EDIT 落在里面）
    RECT railList_{};
    RECT railFoot_{};
    RECT recordBtn_{};
    RECT head_{};
    RECT tableHead_{};
    std::vector<Column> columns_;   // 表头列（排序命中与绘制共用）
    RECT body_{};
    RECT detailBox_{};
    DetailLayout detail_{};

    static constexpr UINT_PTR kTimerReset = 0x5301;
    static constexpr UINT_PTR kTimerStatus = 0x5302;
};
