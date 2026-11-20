// 极简文本输入对话框（书签重命名 / 新建分类 / 标签组命名共用）。
// 用标准 EDIT + 自绘边框，模态但不依赖资源文件。
#pragma once
#include <string>

#include "common.h"
#include "theme.h"

// 返回 true = 用户确认，结果写入 out
bool PromptText(HWND owner, const Theme& theme, const std::wstring& title,
                const std::wstring& label, const std::wstring& initial, std::wstring* out);

// 双字段版：两个标签+两个输入框，确认后分别写入 out1/out2（书签编辑=名称+路径共用）。
bool PromptText2(HWND owner, const Theme& theme, const std::wstring& title,
                 const std::wstring& label1, const std::wstring& initial1, std::wstring* out1,
                 const std::wstring& label2, const std::wstring& initial2, std::wstring* out2);
