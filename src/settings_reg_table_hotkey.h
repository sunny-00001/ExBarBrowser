// 设置键表里的**快捷键管理器面板尺寸**组所在 TU 的入口。
//
// 【为什么单独一个 TU】这一组有六项，全是"这个面板长什么样"的几何参数，
// 与其余可写偏好（栏高、延时、历史上限）不是同一类东西：它们只被一个窗口读，
// 改完只影响那一个窗口的重排。留在 settings_reg_table.cpp 里时，那 48 行会
// 夹在"双栏/标签/审计"中间，读表的人要猜"hk 是什么的缩写"。
//
// 【与只读组分开】这六项是**可写**的（走 config.set，有夹取、有落地分支），
// 所以不能并进 settings_reg_table_readonly.cpp —— 那个文件的契约是"set == nullptr"。
//
// 对外仍只有 settings_reg.h 的 Entries()：这里是表本体的内部分段，
// 不是公开 API（同 settings_reg_table.h 的口径）。
#pragma once
#include <vector>

#include "settings_reg.h"

namespace settings_reg {
namespace detail {

// 把快捷键管理器面板的六项追加到 v 末尾。
//
// 【为什么收 Settings& 而不是自己 Config::Get()】IntSet 要的是"字段地址"，
// 而地址必须取自**同一个** Settings 实例 —— 表里的 get lambda 也收同一个引用。
// 两处各自 Get 一次会拿到同一对象，但那样"这张表依赖全局单例"就藏进了
// 一个看起来纯函数的签名里；显式传参让依赖写在明面上。
void AppendHotkeyPanelEntries(std::vector<Entry>& v, Settings& m);

}  // namespace detail
}  // namespace settings_reg
