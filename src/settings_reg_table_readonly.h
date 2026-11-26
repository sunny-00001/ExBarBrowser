// 设置键表里的**只读观测面**（键位 / 窗口几何 / 会话状态）所在 TU 的入口。
//
// 【为什么单独一个 TU】这三组在语义上是同一类东西，与"可写的偏好设置"分得很开：
//   · 键位      —— 真正的写入入口是 hotkey.set / clear / reset，改文件不生效；
//   · 窗口几何  —— 由用户拖拽决定，下次启动才读，改了当前窗口不动；
//   · 会话状态  —— 运行时另有一份活状态，写配置文件不会改变已开着的标签/书签树。
// 它们的共同点是 `set == nullptr`（config.set 一律拒绝），doc 里都写着"只读：改哪个
// 命令"。把这 80 余行留在可写项中间，读表的人要一路翻过 600 行才发现"这一段根本
// 不能写"；拆出来之后 `settings_reg_table.cpp` 只剩"能改什么"。
//
// 对外仍只有 settings_reg.h 的 Entries()：这里是表本体的内部分段，
// 不是公开 API（同 settings_reg_table.h 的口径）。
#pragma once
#include <vector>

#include "settings_reg.h"

namespace settings_reg {
namespace detail {

// 把只读三组（keys / geometry / session）追加到 v 末尾。
// 调用方保证 v 里已经放好了可写项；本函数不读全局配置（get lambda 收 Settings 引用）。
void AppendReadonlyEntries(std::vector<Entry>& v);

}  // namespace detail
}  // namespace settings_reg
