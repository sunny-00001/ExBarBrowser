// 书签栏的键盘导航：方向键 / Home / End / 回车 / 菜单键。
//
// 【为什么单独成文件】bookmark_bar.cpp 的 Handle 里塞着绘制、焦点、UIA、拖拽、
// 菜单、键盘六类消息；键盘这一族自成一体（一个"焦点项游标 + 保证可见"的语义），
// 拆出来读的人才不用在几百行 switch 里找那几十行。
//
// 【无障碍口径】图标能做的，键盘也要能做到 —— 所以菜单键（VK_APPS / Shift+F10）
// 与右键等价，方向键走遍**全部**项（不只可见项）。
#include "bookmark_bar.h"

#include "commands.h"   // CommandBus / CmdId：Delete 与菜单键都转投总线（审计一处）
#include "common.h"
#include "mouse_rules.h"   // CurrentMods：Enter 打开与鼠标左键同一套修饰键判据

// 返回 0 = 已处理；没认领的键交回 DefWindowProcW。
LRESULT BookmarkBar::HandleKeyDown(HWND h, WPARAM w, LPARAM l) {
    const int n = static_cast<int>(items_.size());
    if (n == 0) {
        if (w == VK_ESCAPE && refocus_) refocus_();
        return 0;
    }
    if (w == VK_ESCAPE) {
        kbdIdx_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        if (refocus_) refocus_();   // 焦点还给视图，Delete / type-ahead 才可用
        return 0;
    }
    if (w == VK_LEFT || w == VK_RIGHT) {
        // 走遍**全部**项。以前放不下的项被折叠、栏上没有位置，
        // 键盘只能停在折叠项之前，看它们得用 » 菜单 —— 键盘用户等于摸不到。
        // 现在项都有位置，方向键走过去时顺手把栏滚到那一项（EnsureVisible），
        // 焦点框不会停在看不见的地方。
        const int step = (w == VK_RIGHT) ? 1 : -1;
        const int cur = kbdIdx_ >= 0 ? kbdIdx_ : (step > 0 ? -1 : n);
        int next = cur + step;
        if (next < 0) next = 0;
        if (next >= n) next = n - 1;
        kbdIdx_ = next;
        EnsureVisible(next);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (w == VK_HOME || w == VK_END) {
        kbdIdx_ = (w == VK_HOME) ? 0 : n - 1;
        EnsureVisible(kbdIdx_);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (w == VK_RETURN || w == VK_SPACE) {
        if (kbdIdx_ >= 0 && kbdIdx_ < n) {
            EnsureVisible(kbdIdx_);   // 键盘触发必可见（锚点取 rc，同 WM_CONTEXTMENU）
            POINT at{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                     items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
            ClientToScreen(h, &at);
            if (items_[static_cast<size_t>(kbdIdx_)].folder) {
                ShowFolderMenu(kbdIdx_, at);
            } else {
                // 【E22 修复】这里原先直接 Navigate(kbdIdx_)，于是"栏上 Ctrl+点
                // 开新标签、键盘 Ctrl+Enter 还是当前标签" —— 打开语义的第三个
                // 触发点被漏掉了（E12/E13 修了溢出菜单与分类弹层两个）。
                // 收口点 OpenPathWithMods 就是为这件事存在的：无修饰覆盖时它
                // 返回 false，才走默认的当前标签导航。
                const Item& it = items_[static_cast<size_t>(kbdIdx_)];
                if (!OpenPathWithMods(it.path, CurrentMods())) Navigate(kbdIdx_);
            }
        }
        return 0;
    }
    if (w == VK_DELETE) {
        // Delete 删除焦点书签。无障碍要求：右键菜单能做的，键盘也要能做到 ——
        // 而"选中一项按 Delete"是列表类控件的通用约定（标签栏、资源管理器、
        // 浏览器书签管理器都这样）。此前本栏的 Delete 会穿透到文件视图，
        // 用户在书签上按 Delete 删掉的是背后的文件 —— 这是数据风险，不是手感问题。
        //
        // 删除不弹确认（用户明确要求"删除书签不要弹窗"）：与右键「删除」同一
        // 口径。bmk.remove 进了 kNoConfirmDestructive，误删可从「撤销删除」恢复；
        // 命令仍走总线（审计、撤销栈都在），不需要 MarkConfirmedForNextCall。
        if (kbdIdx_ >= 0 && kbdIdx_ < n) {
            const Item it = items_[static_cast<size_t>(kbdIdx_)];   // 值拷贝：删除会重建 items_
            EnsureVisible(kbdIdx_);
            // 不弹框、不标记"已确认"：白名单已让总线放行，多一层标记只会掩盖
            // 将来别人误把 BmkRemove 从白名单删掉时的回归。
            json::Value a = json::Value::MakeObject();
            a.Set(L"id", json::Value::MakeStr(it.id));
            CommandBus::Get().Invoke(CmdId::BmkRemove, Args::FromJson(a), Actor::Ui());
            // 删除后焦点项跟着往后挪一格（删的是最后一项则回到新末尾）。
            // 不挪的话 kbdIdx_ 会指向已不存在的下标，下一次按 Delete 就静默失效。
            const int after = static_cast<int>(items_.size());
            kbdIdx_ = after == 0 ? -1 : (kbdIdx_ < after ? kbdIdx_ : after - 1);
            EnsureVisible(kbdIdx_);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    if (w == VK_APPS || (w == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
        // 无障碍要求：图标/鼠标能做的，键盘也要能做到（等同右键）。
        if (kbdIdx_ >= 0 && kbdIdx_ < n) {
            EnsureVisible(kbdIdx_);
            POINT at{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                     items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
            ClientToScreen(h, &at);
            ShowItemMenu(kbdIdx_, at);
        }
        return 0;
    }
    return DefWindowProcW(h, WM_KEYDOWN, w, l);
}
