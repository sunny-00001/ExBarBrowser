// 只读文本报告窗口（tool.hash / tool.dupfinder / tool.compare 的人工输出口）。
//
// 【为什么需要它】这三条工具的结果是**多行表格**（一组重复文件、一份差异清单），
// 塞进 PromptText 的单行 EDIT 里没法读，也不能选中复制。而新建一套列表控件
// （列宽/排序/选择）对一个"看一眼然后去资源管理器处理"的场景是过度建设。
// 只读多行 EDIT 正好：能滚、能选、能 Ctrl+C，系统控件自带无障碍与输入法支持。
//
// 内容由调用方拼好（纯文本）；本模块只管显示。这样"结果怎么排版"可以单测，
// 而"窗口怎么画"只有一个实现。
#pragma once
#include <string>

#include "common.h"
#include "theme.h"

// 模态显示一段文本。返回时窗口已销毁；不返回任何结果（没有"确认"语义）。
void ShowReport(HWND owner, const Theme& theme, const std::wstring& title,
                const std::wstring& text);
