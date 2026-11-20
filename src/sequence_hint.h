// 序列键候选提示：无激活、置顶的小弹层，画在主窗口右下角。
// 不用系统 tooltip：需要多行 + 主题色 + 前缀高亮，且不能抢焦点。
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "theme.h"

class SequenceHint {
public:
    void Create(HWND owner, const Theme& theme);
    void ReloadTheme();
    void Show(const std::wstring& prefix,
              const std::vector<std::pair<std::wstring, std::wstring>>& items);  // token,title
    void Hide();
    bool visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    const Theme* theme_ = nullptr;
    std::wstring prefix_;
    std::vector<std::pair<std::wstring, std::wstring>> items_;
    bool visible_ = false;
    int width_ = 0;
    int height_ = 0;
};
