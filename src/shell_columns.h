// 详细信息视图的“选择列”能力：IColumnManager 的唯一封装。
//
// 【为什么要独立文件】shell_ops.cpp 已经是视图作用域文件操作的大杂烩；
// 列管理是纯视图元数据（PROPERTYKEY ↔ 显示名 ↔ 可见性），既不读选中集也不落盘，
// 放进 shell_ops 会让那个文件再增加一种互不相干的职责。
#pragma once
#include <string>
#include <vector>

#include "common.h"

namespace shellcolumns {

struct ColumnDesc {
    PROPERTYKEY key{};
    std::wstring id;       // 稳定机器标识：{GUID}:pid
    std::wstring name;     // shell 给的本地化列名
    bool visible = false;
    bool canHide = true;   // shell 标成固定列（典型是名称列）时不可隐藏
    UINT width = 0;
};

// 列 id 的稳定文本形式。外部（命令参数 / 配置 / 日志）只传它，
// 不传 PROPERTYKEY 的二进制布局。
std::wstring ColumnIdToString(const PROPERTYKEY& key);
bool ColumnIdFromString(const std::wstring& id, PROPERTYKEY* key);

// 枚举当前详细信息视图可用列（含隐藏列）。返回 false = 当前视图不支持列管理。
bool List(IShellView* view, std::vector<ColumnDesc>* out);

// 把 visible 中列出的 id 设为可见，其余可隐藏列设为隐藏。
// 未知 id 一律拒绝，避免“传错一个 id 就把整个列布局清空”。
bool SetVisible(IShellView* view, const std::vector<std::wstring>& visible);

// 切换单列可见性。返回切换后的状态；列不存在时返回 false。
bool Toggle(IShellView* view, const std::wstring& id, bool* nowVisible);

struct KnownProperty {
    PROPERTYKEY key{};
    std::wstring id;     // {GUID}:pid，与 ColumnIdToString 同格式
    std::wstring title;  // 中文显示名
};
// 常用可排序/可分组属性键（不依赖当前视图，列表稳定）。
// view.props.list 用它给机器一份可抄的 id；view.sort.bykey / view.group.bykey 收它给的 id。
const std::vector<KnownProperty>& KnownProperties();

}  // namespace shellcolumns
