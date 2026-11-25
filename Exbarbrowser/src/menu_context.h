// 统一菜单组件 → Shell IContextMenu 的桥接层。
//
// 【为什么不直接 ui::Menu::Show()】Show() 自己创建弹层并跑模态消息循环。
// NSTC 的空白处右键要求宿主交出 IContextMenu，菜单必须由 NSTC 自己跟踪；
// 在事件回调里再 Show() 会重进 shell 的消息状态，之前实测会卡死。
//
// 【为什么还要单独一层】菜单内容不能因此在调用点退化成 AppendMenuW：
// 那样图标、快捷键列、命令身份又散回各处，用户改键位后菜单不会自动跟着变。
// 这里把 ui::Menu 的结果解析成 owner-draw 的原生菜单项，并接住
// IContextMenu2/3 的 WM_MEASUREITEM/WM_DRAWITEM，让内容仍由统一组件负责。
//
// 业务侧只需要：
//   ui::Menu menu;
//   menu.Add(L"刷新", action, ico::IconId::Refresh).Cmd(CmdId::NavRefresh);
//   new ui::ShellContextMenu(menu, theme, invokeFn);
//
// 【视觉口径】自绘行不再自己选字体/图标格/高亮色，而是直接调用书签菜单同一份
// PopupDrawItem/PopupMeasureWidth。于是图标大小、图标到文字的间距、选中/悬停
// 底色、正文与快捷键颜色、分隔线都只有一个来源（Theme + 弹层绘制器）。
#pragma once
#include "menu.h"

#include <functional>

namespace ui {

class ShellContextMenu final : public IContextMenu3 {
public:
    // 参数是 ui::Menu::Add 传入的 action（不是行号）；返回 false 表示该动作没执行成功。
    using InvokeFn = std::function<bool(int action)>;

    // theme 必须是活到菜单跟踪结束的对象（App::theme_ 整个进程都活着）。
    // 不拷贝 Theme：Reload 会原地重建画刷/字体，拷贝一份会立刻过期。
    ShellContextMenu(const Menu& menu, const Theme& theme, InvokeFn invoke);
    ShellContextMenu(const ShellContextMenu&) = delete;
    ShellContextMenu& operator=(const ShellContextMenu&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst,
                                               UINT idCmdLast, UINT uFlags) override;
    HRESULT STDMETHODCALLTYPE InvokeCommand(LPCMINVOKECOMMANDINFO info) override;
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR idCmd, UINT uFlags, UINT* reserved,
                                               LPSTR name, UINT cchMax) override;
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT msg, WPARAM wp, LPARAM lp) override;
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT msg, WPARAM wp, LPARAM lp,
                                             LRESULT* result) override;

private:
    ~ShellContextMenu();

    bool OnMeasureItem(MEASUREITEMSTRUCT* item) const;
    bool OnDrawItem(DRAWITEMSTRUCT* item) const;

    LONG ref_ = 1;
    const Theme* theme_ = nullptr;
    Menu menu_;                     // 内容源：构建期不解析快捷键
    InvokeFn invoke_;
    // QueryContextMenu 时从 menu_ 拷出并 FillShortcuts；菜单跟踪期间保持有效。
    std::vector<PopupRow> rows_;
    // 与 rows_ 一一对应，只把助记符 & 去掉；绘制/量宽交给 PopupDrawItem。
    std::vector<PopupRow> drawRows_;
};

}  // namespace ui
