// 统一菜单构建层：所有自绘菜单的“内容”只在这里组装。
//
// 【为什么要有这一层】
// PopupList 只解决“怎么画一个弹层”，不解决“菜单长什么样”。以前每个调用点
// 都手写 `PopupRow{label, path, icon, folder, separator, disabled, data, glyph}`
// 八字段，于是同一类信息散在十几处：
//   · 勾选前缀 "✓ " 各处自己拼；
//   · 分隔行自己塞一个 data=-1 的空行；
//   · 禁用态靠调用方记得填对位置；
//   · 路径图标有的用 ico::Resolve 临时 HICON、有的用缓存 HICON，生命周期不一致；
//   · 初态行号靠 call site 数行数，增删一行就错位。
//
// 本组件把“菜单内容”收口成四个动作：Add / AddChecked / AddPath / Separator。
// Show() 再把构建结果交给唯一渲染器 ShowPopupList，返回业务 action（不是行号）。
// 调用方不再持有 PopupRow 向量，也不再关心图标资格与行号布局。
#pragma once
#include <string>
#include <vector>

#include "icon_resolve.h"
#include "popup_list.h"

namespace ui {

struct MenuOptions {
    int minWidthPx = 0;
    bool exactWidth = false;
    int maxRows = 20;
    const wchar_t* toggleKey = nullptr;
    int initialRow = -1;
    bool scrollTo = true;
};

class Menu {
public:
    // 普通动作行。glyph 必须是语义图标（ico::IconId），不是裸码位。
    Menu& Add(const std::wstring& label, int action, ico::IconId glyph,
              bool disabled = false);

    // 勾选行。勾选前缀只在这里拼一次，避免各处 "✓ " / "   " 的空格数不一致。
    // 返回行号，便于把当前项设为初态而不用手数行。
    int AddChecked(const std::wstring& label, bool checked, int action, ico::IconId glyph,
                   bool disabled = false);

    // 路径行（历史 / 书签 / 溢出菜单）：图标走 ico::Cached，进程内共享、菜单不销毁。
    // folder=true 时行尾画 chevron。path 为空时退回 Folder 字形，不留空槽。
    int AddPath(const std::wstring& label, const std::wstring& path, int action,
                bool folder = false, bool disabled = false);

    // 路径行的勾选版（标签溢出菜单要标当前标签）。勾选前缀仍只由组件拼。
    int AddPathChecked(const std::wstring& label, bool checked, const std::wstring& path,
                       int action, bool folder = false, bool disabled = false);

    // 分隔行统一无 data、无图标、不可选。
    Menu& Separator();

    // 给指定行追加上说明文字（视图菜单的“（升序）/（降序）”）。
    // 行号越界时什么也不做，不让“菜单文案补充”反过来影响动作派发。
    Menu& AppendLabel(int row, const std::wstring& suffix);

    // 只读视图：给纯逻辑单测用；业务代码不应靠它再手写行。
    const std::vector<PopupRow>& rows() const { return rows_; }

    bool Empty() const { return rows_.empty(); }
    int Count() const { return static_cast<int>(rows_.size()); }

    // 模态展示；返回所选行的 action。取消 / 分隔 / 无效 data 一律返回 -1。
    int Show(HWND owner, const Theme& theme, POINT screen, const MenuOptions& opt = {}) const;

private:
    std::vector<PopupRow> rows_;
};

}  // namespace ui
