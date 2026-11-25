// 设置对话框：三栏（左 分类 / 中 表单 / 右 说明）+ 底部状态栏。
//
// 【为什么是独立窗口，不是弹层菜单】旧的设置面板是一个弹出菜单：所有项平铺，
// 项数一多就只能靠滚动找，而"这项属于哪一类"完全没有表达。设置项本来就有
// 明确的领域分组（栏 / 视图 / 序列键 / 标签 / 窗口 / 会话 / 状态栏 / 键位 / AI），
// 分组就该出现在界面上 —— 左栏一个标签页一类，中栏按类排，右栏讲清这一项。
//
// 【为什么是非模态】相当一部分设置**立即生效**（栏显隐、排序分组、AI 通道）。
// 模态窗会把主窗口挡住，用户改完只能凭记忆猜"到底变了没有"；非模态可以一边
// 改一边看。它是主窗口的 owned 窗口：主窗口关掉时它跟着走，不需要额外清理。
//
// 【人机同源】面板不直接改 Settings 字段，而是调 config.set 命令 —— 与 AI /
// 脚本走同一条路。于是面板上的每次改动都有审计记录、都过注册表的类型校验，
// 不会出现"界面能改、命令改不动"或"命令改了、界面不知道"。
//
// 【表驱动】每一行都来自 settings_reg::Entries()，改动只发生在表里：
// 加一行就多一个可调项，界面 / 命令 / AI 三边同时看到它。手写控件的话，
// "表里有、界面没有"会变成静默的能力缺失 —— 而那种缺失没有任何检查会报。
//
// 【只读项】窗口几何、会话、键位覆盖这些项只提供读取：界面上灰显且点不动，
// 右栏改说"该项由程序维护"。比"干脆不列出来"诚实 —— 用户不会以为程序没有
// 这个设置。
#pragma once
#include <string>
#include <vector>

#include "common.h"
#include "icon_glyph.h"
#include "json.h"
#include "settings_reg.h"
#include "theme.h"

class SettingsDialog {
public:
    // 创建（owner = 主窗口；关掉主窗口时它会跟着走）。失败返回 false。
    bool Create(HWND owner, const Theme& theme);
    // 显示并前置（已开着就不重复创建 —— 两个设置窗口并排只会让人搞不清
    // 改的是哪一份）。
    void Show();
    // 主题切换后重绘（颜色与字体都换了）。
    void ReloadTheme();

    // 外部改了设置之后，把"当前值"重新读一遍并重绘。
    //
    // 【为什么需要这个公开入口】面板自己的写入（WriteValue）会自己调
    // RefreshValues()，但 AI / 命令面板 / 状态栏右键菜单走的是**同一条
    // config.set 命令**，它们改完之后面板并不知道 —— 于是出现"AI 把 themeMode
    // 改成 dark、面板上那个下拉还写着 system"。用户看到的是一块说谎的界面。
    //
    // 【只刷值，不重建模型】重建（RebuildModel）会丢选中的行、丢掉滚动位置，
    // 而外部改动从来不会改变"有哪些项/怎么分组"（那张表是编译期常量）。
    // 所以这里只需要重读值 + 重排（值的文本宽度可能变）+ 重绘。
    void OnConfigChangedExternally();

private:
    // 一行 = 表里的一项 + 它的当前值与显示文本。
    struct Row {
        const settings_reg::Entry* entry = nullptr;
        std::wstring name;      // 名称（doc 的冒号前）
        std::wstring desc;      // 说明（doc 的冒号后）
        json::Value value;      // 当前值
        std::wstring display;   // 当前值的显示文本
        bool writable = false;
    };

    // 左栏一个分组（含计数）。
    struct GroupItem {
        const settings_reg::Group* group = nullptr;
        int total = 0;   // 组内项数
        int hits = 0;    // 搜索命中数
    };

    // 右栏的排布结果。绘制与命中测试共用同一份矩形 ——
    // 两处各算一遍，差一个像素就是"点复制却复制了恢复默认"。
    //
    // 【这里只放几何，不放"有没有选中项"】那是派生状态（= selKey_ 找不到行），
    // 一旦存进几何结构就会出现"绘制时写、命中时读"的暗渠：布局时写入、
    // 绘制时覆盖、命中时读到哪一次全看调用顺序。派生状态永远现算。
    struct RightLayout {
        RECT head1;      // "说明"
        RECT name;
        RECT desc;       // 整段说明（自动换行）
        RECT chip;       // 键名芯片
        RECT type;       // 类型 / 取值域
        RECT head2;      // "相关操作"
        RECT restore;    // 恢复默认值
        RECT copyKey;    // 复制键名
        RECT copyCmd;    // 复制命令模板
        RECT head3;      // "帮助"
        RECT help;
    };

    // 中栏内容的一段：一行设置，或一个分组标题（搜索态下可能有多个组）。
    //
    // 【为什么标题与行要分开建模】搜索结果会横跨多个分组，每组前面要有一条
    // 标题条；若把标题塞进"行"里，就得在每行上挂一个"我是不是组首"的标记，
    // 而漏标记的表现是"两组结果粘在一起、看不出分界"。
    struct Item {
        bool header = false;
        std::wstring title;      // 分组名（标题条）
        std::wstring group;      // 分组 key（标题条所属的组）
        ico::IconId icon = ico::IconId::None;
        int rowIndex = -1;       // 标题条之后的行（-1 = 标题条自身）
        RECT rc{};               // 排布结果
    };

    // 一行的几何。命中测试与绘制**共用**这一份：两处各算一遍迟早错位，
    // 而错位的表现是"点在复选框上却选中了别的行"。
    struct RowLayout {
        RECT row{};
        RECT name{};
        RECT desc{};
        RECT ctrl{};   // 控件列（整列，右对齐）
        RECT box{};    // 控件本体（复选框 / 下拉框 / 数字框）
        RECT edit{};   // 原生 EDIT 窗口位置（box 的横向 + line 的纵向）
        RECT line{};   // 这一行共用的文字行（上下留白相等）
        RECT unit{};   // 数字右侧的单位文字（可能为空）
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

    // 搜索框的占位文字。
    //
    // 【为什么不用系统的 cue banner】EM_SETCUEBANNER 要 comctl32 v6 清单才生效，
    // 而本工程没有清单（也就没有引入 v6 的理由）—— 发那条消息不报错，
    // 但框里一个字都不会出现。这种"调用成功却什么也没发生"最难查。
    //
    // 【那为什么不用自己画】占位文字必须画在**输入框控件内部**，而输入框是
    // 真控件（子窗口），它会盖住父窗口画的一切 —— 父窗口画不到那里。
    // 所以占位文字只能住在控件的**文本**里。
    //
    // 【规则只有一条】框里的文字等于占位文字时，它**不算搜索词**。
    // 于是"显示占位文字"与"搜索框是空的"是同一件事，不需要额外的状态位。
    //
    // 【占位文字不能自动选中】单行 EDIT 的选区底色只包住字形高度，而外层框用的是
    // 字体行盒；把四个占位字整段选中，视觉上就不是"文字在框里居中"，而是"一小块
    // 蓝色贴在上半部"——用户会看成选中态/未选中态文字位移。业界 cue banner /
    // placeholder 都不是可编辑内容，当然也不会被自动选中；没有 v6 清单时我们用
    // 真实文本模拟，但保留这条语义：占位文字只显示，绝不自动全选。
    static constexpr const wchar_t* kSearchPlaceholder = L"搜索设置";

    // 子类回调：搜索框（Esc 清空 / Enter 跳到表单）与行内编辑框
    // （回车提交 / Esc 取消 / 失焦提交）。
    static LRESULT CALLBACK SearchSub(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK ValueSub(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    // 编辑框提交的窗口消息（从子类回调 post 回来，不在回调里直接改父窗口状态）。
    static constexpr UINT kMsgCommitEdit = WM_APP + 60;

    // ---- 模型（settings_dialog_model.cpp） ----
    // 【不变式】RebuildModel 之后必须 Layout()。
    // 重建模型会换掉可见行（数量与内容都变），而行/控件的矩形存在
    // rowLayout_ 里 —— 不重算就会拿上一组的矩形画新行，表现为
    // "下拉框画成了复选框的样子、点下去没反应"。三个调用点（创建 / 搜索 / 切组）
    // 都紧跟着 Layout()，漏一个就出这种错。
    void RebuildModel();
    void RefreshValues();
    std::wstring DisplayOf(const settings_reg::Entry& e, const json::Value& v) const;
    bool Matches(const settings_reg::Entry& e) const;
    const Row* RowByKey(const std::wstring& key) const;
    int GroupIndex(const std::wstring& key) const;
    // 按 key 找回行下标（-1 = 不在当前可见行里）。编辑态与"拒绝红框"都用
    // 键名而不是下标定位：RebuildModel 会整体替换 rows_，下标会指向别的设置项。
    int RowIndexByKey(const std::wstring& key) const;

    // 搜索框当前的文字（去掉占位文字、去掉首尾空白）。
    // 唯一读搜索框的地方 —— 谁读了原始文本，谁就可能把"搜索设置"当成搜索词。
    //
    // 【只能从 EN_CHANGE 调】读控件文本 = 给控件发 WM_GETTEXT，而控件身上
    // 装着子类回调 —— 回调里再读一次，就是无限递归（爆栈崩溃）。
    // 所以文本在父窗口这边缓存一份，控件回调只读缓存。
    void UpdateSearchFromControl();
    // 框里现在是不是占位文字（= 用户没在搜）。读的是**缓存**，不读控件。
    bool SearchShowsPlaceholder() const { return searchPlaceholder_; }
    // 读输入框的原始文本（只有父窗口侧能调：见上面的递归说明）。
    static std::wstring SearchRawText(HWND edit);

    // ---- 布局（settings_dialog_layout.cpp） ----
    // 客户区最小尺寸：由三栏常量与右栏内容反推（不手写手感值）。
    // WM_GETMINMAXINFO 用它 + AdjustWindowRectEx 换算出窗框下限。
    static void MinClientSize(const Theme& theme, int* w, int* h);
    void Layout();
    void LayoutRows();
    void LayoutRight();
    // 把行内编辑框挪到 editKey_ 那一格的数字框上（没有编辑态就收起）。
    //
    // 【为什么必须单独成函数】行/控件的矩形会因**不止一处**变化：Layout（换组）
    // 与 ScrollBy（滚动）。编辑框位置若只写在 Layout 里，滚动时它就停在原地 ——
    // 表现为"滚一下，输入框浮在别的行上"。位置的唯一出口在这里，谁动了
    // rowLayout_ 谁就调它。
    void PlaceValueEdit();
    int RowHeight() const;
    int CtrlHeight() const;
    // 单行文字行高：与导航栏地址框/搜索框共用 ui::EditLineHeight。
    // 自绘文字的矩形和原生 EDIT 的窗口都取这一把尺子，上下留白才会相等。
    int EditLineHeight() const;
    // 右栏"说明"块要显示的文本（该项没有说明时给一句占位）。
    //
    // 【为什么必须只有一处】LayoutRight 量它来决定块高、PaintRight 画它。
    // 两处各拼一份字符串的话，块高是按 A 算的、画的是 B —— 表现是"说明压住
    // 键名芯片"或者"块里留着一截空白"，而改文案的人不会想到去看布局。
    std::wstring RightDescText() const;

    // 数字右侧的单位文字（"毫秒" / "%" / "像素"）。没有单位返回 nullptr。
    static const wchar_t* UnitOf(const std::wstring& key);

    // ---- 绘制（settings_dialog_draw.cpp / settings_dialog_draw_help.cpp）----
    void Paint(HWND hwnd);
    void PaintLeft(HDC dc, const RECT& rc);
    void PaintMiddle(HDC dc, const RECT& rc);
    void PaintRight(HDC dc, const RECT& rc);
    void PaintStatus(HDC dc, const RECT& rc);
    void PaintSectionHeader(HDC dc, const Item& item);
    void PaintRow(HDC dc, int rowIndex);
    void PaintCheckbox(HDC dc, const RECT& rc, bool on, bool dim);
    void PaintSelect(HDC dc, const RECT& rc, const RECT& line, const std::wstring& text, bool dim);
    void PaintNumber(HDC dc, const RECT& rc, const RECT& line, const std::wstring& text,
                     const wchar_t* unit, bool dim, bool bad);
    // 可写自由字符串的输入框（与数字框同样的形状，路径省略在左端）。
    void PaintTextField(HDC dc, const RECT& rc, const RECT& line, const std::wstring& text,
                        bool dim);

    // ---- 命中（settings_dialog_layout.cpp） ----
    int HitGroup(int x, int y) const;        // -1 = 没命中
    int HitItem(int x, int y) const;         // 中栏：返回 items_ 下标
    int HitRightAction(int x, int y) const;  // 0 无 / 1 恢复默认 / 2 复制键名 / 3 复制命令
    int HitHeadAction(int x, int y) const;   // 0 无 / 1 密度 / 2 帮助

    // ---- 交互 ----
    void OnClick(int x, int y);
    void OnWheel(int delta);
    void SelectGroup(const std::wstring& key);
    void SelectEntry(const std::wstring& key);
    void SetStatus(const std::wstring& msg, bool error = false);
    bool WriteValue(const std::wstring& key, const json::Value& value);
    void ToggleBool(const Row& row);
    void OpenEnum(const Row& row, const RECT& ctrl);
    void RestoreDefault();
    void CopyKeyName();
    void CopyCommandTemplate();
    void ScrollBy(int delta);
    void EnsureVisible(int itemIndex);
    // 让左栏第 groupIndex 个分类滚进可视区（分组多于可用高度时用）。
    void EnsureGroupVisible(int groupIndex);
    // 让第 rowIndex **行**滚进可视区。
    // 【为什么不直接用 EnsureVisible】后者收的是 items_ 下标；搜索态下 items_
    // 里还夹着段标题条，行下标与 items_ 下标不相等 —— 拿行下标去索引它会滚到
    // 别的地方（表现为"选中下一行，画面跳到上一组"）。
    void EnsureRowVisible(int rowIndex);

    // ---- 键盘 ----
    // 这个窗口不是对话框（建立时没走 DialogBox 那一套，消息循环里也没有
    // IsDialogMessage），所以方向键 / Tab / 空格**没有任何人处理**。
    // 不自己实现的结果就是"只能用鼠标" —— 而且没有任何检查会报。
    // 指针进入子控件时收掉父窗口的行/分类高亮（同一时刻只能有一处高亮）。
    void ClearHover();
    void MoveSelection(int delta);       // 上下移动选中项
    void SelectRowAt(int rowIndex);      // 选中第 rowIndex 行（并滚进可视区）
    void ActivateSelected();             // 对选中项做"改值"动作（回车 / 空格）
    // 一行上的改值动作：下拉 / 切布尔 / 行内编辑。
    // 【为什么收成一个函数】点击控件与键盘激活是**同一个动作的两种入口**；
    // 各写一份的话，新增一种 Kind 时必然只改一处，另一处的表现是
    // "点得动、回车没反应"（或反过来）。
    bool ApplyRowControl(const Row& row);

    // 数字项 / 可写自由字符串项的行内编辑（就地改值，不用弹窗）。
    void BeginEdit(int rowIndex);
    void CommitEdit(bool accept);
    // 编辑框里的文字（原样取出，不做类型解释）。
    // 【为什么要单独一个函数】取长度再取文本必须两步，固定长度的缓冲区会
    // 静默截断：路径项一长，写进配置的就只剩前若干个字符，而且没有任何报错。
    std::wstring ValueEditText() const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND valueEdit_ = nullptr;
    const Theme* theme_ = nullptr;

    std::vector<Row> rows_;
    std::vector<GroupItem> groups_;
    std::vector<Item> items_;
    std::vector<RowLayout> rowLayout_;
    std::wstring group_;     // 当前分组（空 = 搜索态）
    std::wstring selKey_;    // 选中项的 key
    std::wstring query_;     // 搜索词
    std::wstring searchText_;      // 搜索框的原始文本（缓存，见 UpdateSearchFromControl）
    bool searchPlaceholder_ = true;  // 搜索框现在显示的是占位文字
    std::wstring status_;    // 状态栏消息
    bool statusError_ = false;

    // 几何（物理像素）。
    RECT colA_{};
    RECT colB_{};
    RECT colC_{};
    RECT head_{};       // 中栏头（面包屑 + 密度 + 帮助）
    RECT form_{};       // 中栏表单区（可滚动）
    RECT searchBox_{};  // 左栏搜索框
    RECT catList_{};    // 左栏分类列表
    RECT hint_{};       // 左栏滚轮提示（在列表与页脚之间）
    RECT foot_{};       // 左栏底部计数
    RECT statusBar_{};  // 底部状态栏
    RECT rcDensity_{};  // 头部的密度按钮
    RECT rcHelp_{};     // 头部的帮助按钮
    RightLayout right_{};   // 右栏各块的矩形

    int scroll_ = 0;        // 表单区滚动（像素）
    int scrollMax_ = 0;
    int groupScroll_ = 0;   // 左栏分类列表滚动（像素；分组多于可用高度时非零）
    int groupScrollMax_ = 0;
    // 行高档位（与原型的密度三档一致：32 / 28 / 24 逻辑像素）。
    // 默认值写成“像素”而不是裸下标：以后 insertion/换序不会再静默改变默认行高。
    static constexpr int kDefaultRowHeightPx = 28;
    static constexpr int kRowHeights[3] = {32, kDefaultRowHeightPx, 24};
    static constexpr int kDefaultDensity = 1;
    static_assert(kRowHeights[kDefaultDensity] == kDefaultRowHeightPx);

    int density_ = kDefaultDensity;       // kRowHeights 的下标
    // 编辑态与"刚被拒绝"都用**键名**定位，不用 rows_ 下标：RebuildModel 会
    // 换掉整份 rows_（换组/搜索），下标会指到另一个设置项 —— 那正是"编辑 A、
    // 却写进 B"的根因。Entry::key 跨重建稳定。
    std::wstring editKey_;  // 正在行内编辑的项（空 = 无）
    std::wstring badKey_;   // 刚被拒绝的项（值非法 → 画红框）
    int hoverRow_ = -1;
    int hoverGroup_ = -1;
    int wheelAcc_ = 0;      // 左栏滚轮累计量（切换分类的阈值）
    bool tracking_ = false; // 是否已登记鼠标离开跟踪

    // 左栏页脚的三个数：全部项数 / 可改项数 / 只读项数。
    // 【为什么存在模型里而不是现算】页脚要在**任何**分组下都显示全局统计
    // （浏览态只看到一组，用可见行数算就变成了"这一组有几项"）。
    // 统计的是注册表的全表，与命令 config.list 看到的是同一份口径。
    int totalAll_ = 0;
    int totalWritable_ = 0;
};
