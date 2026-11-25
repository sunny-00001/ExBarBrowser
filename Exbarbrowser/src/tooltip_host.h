// 工具提示宿主（唯一实现）。
//
// 收编理由：TabStrip / BookmarkBar / StatusBar 各写了一遍
//   CreateWindowEx(TOOLTIPS_CLASSW) + TTM_SETMAXTIPWIDTH
//   + 循环 TTM_DELTOOLW(id=1..N) + TTM_ADDTOOLW(rect, text)
// 三份的 id 上界还不同（64 / 128 / 32），且都要自己持有一份 std::wstring 副本
// （TTM_ADDTOOLW 只存指针，不复制字符串 —— 这个坑三份都踩过一次）。
//
// 这里把它收成一个对象：SetTools() 一次全量替换，内部管好 id 与字符串生命周期。
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace ui {

class TooltipHost {
public:
    TooltipHost() = default;
    // 曾因"只有析构、没有移动赋值"踩过坑：tooltip_ = TooltipHost() 走隐式拷贝赋值，
    // 旧 tooltip 窗口句柄被覆盖却没销毁，每次 DPI/主题变化都漏一个 HWND。
    TooltipHost(const TooltipHost&) = delete;
    TooltipHost& operator=(const TooltipHost&) = delete;
    TooltipHost(TooltipHost&& other) noexcept;
    TooltipHost& operator=(TooltipHost&& other) noexcept;
    ~TooltipHost();

    // 建/改宿主提示窗口。owner 是承载区的窗口（子窗口），maxWidthPx 是换行宽度。
    // 可重复调用（主题/DPI 变化后改宽度）。
    void Attach(HWND owner, int maxWidthPx);

    // 一条提示：矩形（owner 客户坐标）+ 文本。空文本表示该区域不要提示。
    struct Item {
        RECT rc{};
        std::wstring text;
    };

    // 全量替换所有提示。内部会先删旧工具再按新列表加。
    // 文本会被复制一份，调用方不必保证生命周期。
    void SetTools(const std::vector<Item>& items);

    // 只更新已有工具的矩形（顺序与 SetTools 的 items 一致），不删不重建。
    // 标签栏滚动时每次都走 SetTools 的话，N 个标签就是 2N 次跨进程消息，
    // 而滚动改变的只是热区位置 —— 文本和 id 都没变。
    void MoveTools(const std::vector<RECT>& rects);

    // 仅删除全部工具（窗口要销毁前调用；析构也会做）。
    void Clear();

    HWND hwnd() const { return tip_; }

private:
    void Reset();  // 销毁当前 tooltip 并清空所有状态（移动赋值/析构共用）

    HWND tip_ = nullptr;
    HWND owner_ = nullptr;
    int maxWidth_ = 0;
    // TTM_ADDTOOLW 只保存 lpszText 指针，所以文本必须由我们持有。
    std::vector<std::wstring> texts_;
    std::vector<UINT_PTR> ids_;
};

}  // namespace ui
