// 设置键表的**书签栏**组：几何两项 + 交互策略五项。
//
// 分段理由见 settings_reg_table_bookmark.h。
#include "settings_reg_table_bookmark.h"

#include "config.h"
#include "settings_bounds.h"
#include "settings_reg_valid.h"   // BoolSet / IntSet

namespace settings_reg {
namespace detail {

namespace {

// doc 里引用数值域时用这个拼（与 settings_reg_table.cpp / _hotkey.cpp 同口径）。
// 三个 TU 各有一份一行实现，换来的只是"改这一行要动三个文件"；判据仍是同一对
// 常量（settings_bounds.h），所以三份的**输出**不可能分叉。
std::wstring RangeText(int lo, int hi) {
    return std::to_wstring(lo) + L".." + std::to_wstring(hi);
}

// 布尔枚举项：界面用下拉表达、落盘仍是 true/false。
// 与 settings_reg_table.cpp 的 MakeBoolOptions 同形 —— 那里的是 Table() 所在
// TU 的匿名命名空间函数，跨 TU 不可见（同上：一行实现不值得开公共头）。
std::vector<Option> BoolOptions(const wchar_t* offLabel, const wchar_t* onLabel) {
    std::vector<Option> out;
    {
        Option o;
        o.label = offLabel;
        o.value = json::Value::MakeBool(false);
        out.push_back(std::move(o));
    }
    {
        Option o;
        o.label = onLabel;
        o.value = json::Value::MakeBool(true);
        out.push_back(std::move(o));
    }
    return out;
}

}  // namespace

void AppendBookmarkBarEntries(std::vector<Entry>& v, Settings& m) {
    // ---- 显隐与几何 ----
    v.push_back({L"bookmark", Kind::Bool,
                 [](const Settings& s) { return json::Value::MakeBool(s.showBookmark); },
                 BoolSet(&m.showBookmark, L"bookmark"),
                 L"显示书签栏：与 bmk.show / bmk.hide 命令是同一条状态", L"bar_bookmark"});
    v.push_back({L"bookmarkBarHeight", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.bookmarkBarHeight); },
                 IntSet(&m.bookmarkBarHeight, L"bookmarkBarHeight", kBookmarkBarHeightMin,
                        kBookmarkBarHeightMax),
                 L"书签栏高度（逻辑像素，" +
                     RangeText(kBookmarkBarHeightMin, kBookmarkBarHeightMax) + L"）：立即重排整窗",
                 L"bar_bookmark", {}, kBookmarkBarHeightMin, kBookmarkBarHeightMax});
    v.push_back({L"bookmarkBarTooltipWidth", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.bookmarkBarTooltipWidth); },
                 IntSet(&m.bookmarkBarTooltipWidth, L"bookmarkBarTooltipWidth",
                        kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax),
                 L"书签提示窗最大宽（逻辑像素，" +
                     RangeText(kBookmarkBarTooltipWidthMin, kBookmarkBarTooltipWidthMax) +
                     L"）：书签标题被截断时的悬浮提示最大宽度",
                 L"bar_bookmark", {}, kBookmarkBarTooltipWidthMin,
                 kBookmarkBarTooltipWidthMax});

    // ---- 滚轮 ----
    v.push_back({L"bookmarkWheelStepDp", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.bookmarkWheelStepDp); },
                 IntSet(&m.bookmarkWheelStepDp, L"bookmarkWheelStepDp", kBmkWheelStepDpMin,
                        kBmkWheelStepDpMax),
                 L"书签栏滚轮步进（逻辑像素，" +
                     RangeText(kBmkWheelStepDpMin, kBmkWheelStepDpMax) +
                     L"）：滚一格横向挪多远；48 约三个字宽（立即生效）",
                 L"bar_bookmark", {}, kBmkWheelStepDpMin, kBmkWheelStepDpMax});

    // ---- 点击打开语义（Chrome / Edge / Firefox 书签栏的通用标准）----
    //
    // 【为什么要一个开关】中键与 Ctrl/Shift 是"这一次"的修饰，够用；但有人
    // 就是固定要新标签，每次都按 Ctrl 是持续的负担 —— Edge 110+ 因此加了
    // 「Always open favorites in new tab」这条原生设置。这里对齐它。
    // 修饰键**反向覆盖**（开着开关时 Ctrl 回当前标签），否则开关一开就再也
    // 无法原地打开，只能去设置里来回改。
    v.push_back({L"bookmarkOpenInNewTab", Kind::Bool,
                 [](const Settings& s) { return json::Value::MakeBool(s.bookmarkOpenInNewTab); },
                 BoolSet(&m.bookmarkOpenInNewTab, L"bookmarkOpenInNewTab"),
                 L"书签固定在新标签打开：开启后左键也进新标签（Edge「固定打开新标签页」"
                 L"同款）；Ctrl+左键 反向覆盖 —— 开着时 Ctrl 回当前标签。"
                 L"中键恒为新标签（后台），不受此项影响。下一次点击生效",
                 L"bar_bookmark", BoolOptions(L"当前标签", L"固定新标签")});

    // ---- 拖拽边缘自动滚动 ----
    //
    // 【开关与周期分开两项】有人只要滚轮、不要拖拽自动滚（拖到边缘就自己滑走，
    // 想停在某个位置很别扭）；也有人要它但嫌快/嫌慢。一个开关加一个周期，
    // 三种诉求都有解；只给开关则"嫌快"的人只能关掉。
    v.push_back({L"bookmarkDragAutoscroll", Kind::Bool,
                 [](const Settings& s) { return json::Value::MakeBool(s.bookmarkDragAutoscroll); },
                 BoolSet(&m.bookmarkDragAutoscroll, L"bookmarkDragAutoscroll"),
                 L"书签栏拖拽边缘自动滚动：拖着书签到栏左/右边缘时带视野滚过去"
                 L"（关闭 = 只想用滚轮滚；内部重排与从文件视图拖文件夹都受影响）",
                 L"bar_bookmark", BoolOptions(L"关闭", L"开启")});
    v.push_back({L"bookmarkDragScrollMs", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.bookmarkDragScrollMs); },
                 IntSet(&m.bookmarkDragScrollMs, L"bookmarkDragScrollMs", kBmkDragScrollMsMin,
                        kBmkDragScrollMsMax),
                 L"书签栏拖拽自动滚动周期（毫秒，" +
                     RangeText(kBmkDragScrollMsMin, kBmkDragScrollMsMax) +
                     L"）：数越小滚得越快；与标签栏的 tabDragScrollMs 各管各的",
                 L"bar_bookmark", {}, kBmkDragScrollMsMin, kBmkDragScrollMsMax});
}

}  // namespace detail
}  // namespace settings_reg
