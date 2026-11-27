// 设置键表里的**书签栏**组所在 TU 的入口。
//
// 【为什么单独一个 TU】这一组已经涨到七项，而且分两类语义：
//   · 几何（高度 / 提示窗宽）—— 改完重排整窗或重建 tooltip；
//   · 交互策略（滚轮步进 / 点击打开语义 / 拖拽自动滚动开关与周期）—— 改完
//     在下一次点击/拖拽时现读。
// 两类夹在"导航栏"与"状态栏"之间时，读表的人要在同一段里切换两种心智模型；
// 而且这一段是书签栏功能增长最快的一块（2026-11-26/27 两轮复审各加了几项），
// 留在总表里每加一项都把总表往棘轮上限推一次。
//
// 与 settings_reg_table_hotkey.cpp 同一手法：对外仍只有 settings_reg.h 的
// Entries()，这里是表本体的内部分段，不是公开 API。
#pragma once
#include <vector>

#include "settings_reg.h"

namespace settings_reg {
namespace detail {

// 把书签栏的七项追加到 v 末尾。
//
// 收 Settings& 而不是自己 Config::Get()：IntSet/BoolSet 要的是"字段地址"，
// 地址必须取自与 get lambda 同一个实例（理由同 AppendHotkeyPanelEntries）。
void AppendBookmarkBarEntries(std::vector<Entry>& v, Settings& m);

}  // namespace detail
}  // namespace settings_reg
