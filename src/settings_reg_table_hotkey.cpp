// 设置键表的**快捷键管理器面板尺寸**组：五项可写几何参数。
//
// 【为什么这一组值得单独存在】它们以前是 hotkey_manager_layout.cpp 里的
// constexpr —— 用户看得见的界面却改不动，改一次要重编。而"行距太挤""左栏太窄"
// 恰恰是这类面板最常收到的两条反馈。
//
// 【行高只有一项】中栏命令行、左栏筛选项、表头、工具栏、状态栏共用一个值。
// 以前是 30 / 32 / 26 三个数各写一遍，同一屏里三种行距 —— 眼睛扫下来觉得列表
// 歪了，而代码里毫无异常。工具栏曾经有自己的高度（默认 40），结果顶栏比列表
// 每一行都高；现在它也吃行高，于是"工具栏比行高"从结构上不可能出现。
// 要让"行距不齐"从根上不可能发生，就得让它只有一个数可改。
//
// 分段理由见 settings_reg_table_hotkey.h。
#include "settings_reg_table_hotkey.h"

#include "config.h"
#include "settings_bounds.h"
#include "settings_reg_valid.h"   // IntSet

namespace settings_reg {
namespace detail {

namespace {

// doc 里引用数值域时用这个拼（与 settings_reg_table.cpp 的同名函数同口径）。
//
// 【为什么这里也有一份】那个是 Table() 所在 TU 的匿名命名空间函数，跨 TU
// 不可见。抽成公共头当然可以，但它只有一行、且两个 TU 都要用 —— 为一个
// 一行函数开一个头文件，换来的只是"改这一行要动两个文件"。
// 判据仍是同一对常量（settings_bounds.h），所以两份的**输出**不可能分叉。
std::wstring RangeText(int lo, int hi) {
    return std::to_wstring(lo) + L".." + std::to_wstring(hi);
}

}  // namespace

void AppendHotkeyPanelEntries(std::vector<Entry>& v, Settings& m) {
        // ---- 快捷键管理器面板尺寸 ----
        // 用户能看到的界面必须能调：这几项以前是 hotkey_manager_layout.cpp 里的
        // constexpr，改一次要重编。行高尤其重要 —— 它是"列表看着歪"这类反馈的
        // 直接解法，而中栏/左栏/表头共用这一个值（以前是 30/32/26 三个数）。
        v.push_back({L"hkRowHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.hkRowHeight); },
                     IntSet(&m.hkRowHeight, L"hkRowHeight", kHkRowHeightMin, kHkRowHeightMax),
                     L"快捷键管理器行高（逻辑像素，" + RangeText(kHkRowHeightMin, kHkRowHeightMax) +
                         L"）：中栏命令行、左栏筛选项、表头、工具栏、状态栏共用这一个值",
                     L"dialog", {}, kHkRowHeightMin, kHkRowHeightMax});
        v.push_back({L"hkRailWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.hkRailWidth); },
                     IntSet(&m.hkRailWidth, L"hkRailWidth", kHkRailWidthMin, kHkRailWidthMax),
                     L"快捷键管理器左栏宽（逻辑像素，" + RangeText(kHkRailWidthMin, kHkRailWidthMax) +
                         L"）：筛选项与计数",
                     L"dialog", {}, kHkRailWidthMin, kHkRailWidthMax});
        v.push_back({L"hkDetailWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.hkDetailWidth); },
                     IntSet(&m.hkDetailWidth, L"hkDetailWidth", kHkDetailWidthMin,
                            kHkDetailWidthMax),
                     L"快捷键管理器右栏宽（逻辑像素，" +
                         RangeText(kHkDetailWidthMin, kHkDetailWidthMax) +
                         L"）：详情与动作按钮",
                     L"dialog", {}, kHkDetailWidthMin, kHkDetailWidthMax});
        v.push_back({L"hkWindowWidth", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.hkWindowWidth); },
                     IntSet(&m.hkWindowWidth, L"hkWindowWidth", kHkWinWidthMin, kHkWinWidthMax),
                     L"快捷键管理器初始宽度（逻辑像素，" +
                         RangeText(kHkWinWidthMin, kHkWinWidthMax) +
                         L"）：只影响下次打开时的默认尺寸",
                     L"dialog", {}, kHkWinWidthMin, kHkWinWidthMax});
        v.push_back({L"hkWindowHeight", Kind::Int,
                     [](const Settings& s) { return json::Value::MakeInt(s.hkWindowHeight); },
                     IntSet(&m.hkWindowHeight, L"hkWindowHeight", kHkWinHeightMin,
                            kHkWinHeightMax),
                     L"快捷键管理器初始高度（逻辑像素，" +
                         RangeText(kHkWinHeightMin, kHkWinHeightMax) +
                         L"）：只影响下次打开时的默认尺寸",
                     L"dialog", {}, kHkWinHeightMin, kHkWinHeightMax});
}

}  // namespace detail
}  // namespace settings_reg
