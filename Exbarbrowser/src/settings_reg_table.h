// 设置键表的**表本体**所在 TU 的入口。
//
// 【为什么单独一个头】表本体（"有哪些项"，settings_reg_table.cpp）与表接口
//（"这一项能不能读写"，settings_reg.cpp）是两件事，改动节奏不同：加一项只
// 动表本体，改校验口径只动 valid。合成一个文件会到 400 行上下，且每次读
// 都要翻过半张表才能找到接口 —— 分开后两边各自短平快。
//
// 对外仍只有 settings_reg.h 的 Entries() / Groups()：这两个入口是**表本体
// 与调用方之间**的缝，缝里的东西不该长成公开 API。
#pragma once
#include <vector>

#include "settings_reg.h"

namespace settings_reg {

// 全部项（表本体）。
const std::vector<Entry>& SettingsTable();

// 全部分组（表本体）。
const std::vector<Group>& SettingsGroupTable();

}  // namespace settings_reg
