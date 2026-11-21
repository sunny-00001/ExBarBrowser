# -*- coding: utf-8 -*-
"""进程外验收：地址栏分隔符「再点即收」—— 观察式。
不发合成输入，只读；不动光标、不抢焦点。

## 这个脚本为什么不点

旧版是"坐标扫描"式的：横向每隔 6px 向主窗口投
`WM_MOUSEMOVE` / `WM_LBUTTONDOWN` / `WM_LBUTTONUP`，直到某个 x 打开弹层，
再在那个 x 上点第二次。它看起来"没动光标"，实际有两个真实副作用：

1. **它会连续向正在被使用的窗口投几十次点击。** 每命中一次就建一个模态弹层，
   弹层的 `ShowPopupList` 内部还会 `SetForegroundWindow` —— 前台从用户手上被拿走，
   用户正在打的字进了别人家的窗口。这就是"模拟鼠标循环点击抢占焦点"。
2. **它测的是脚本自己造出来的场景。** 前台归属决定 `WM_ACTIVATE(WA_INACTIVE)`
   的时序，脚本先把状态推到某个位置再断言，测到的不是用户会遇到的那条路径。

架构规则 R21 因此禁止一切合成输入（含 `PostMessage` 投鼠标消息 —— 它在本程序里
必然命中控件 → `ShowPopupList` → `SetForegroundWindow`，结果与 `SendInput` 相同）。
本脚本改成**观察者**：手不动，只数窗口。

## 它现在断言什么

```
人工：点一次 chevron（弹层应打开）
脚本：轮询到弹层出现 → 记为"开"
人工：再点一次同一个 chevron（弹层应收起，且不得重弹）
脚本：轮询到弹层消失 → 记为"关"
脚本：再盯 1.5s，若又冒出弹层 = 重弹缺陷（正是这条断言要抓的东西）
```

判据仍然是「弹层窗口数量」，不依赖像素、不依赖坐标、不需要知道 chevron 在哪 ——
**因此也不需要扫描**。旧版需要扫描只是因为脚本要自己点；人点就不需要了。

前置：让 ExBarBrowser 停在一个**多级路径**的目录（面包屑至少 2 段），窗口在前台。
用法：python tools/probe_toggle.py        退出码 0 = 通过 / 跳过；1 = 失败
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

u = ctypes.windll.user32

MAIN = 'ExBarBrowser_Main'
POPUP = 'ExBarBrowser_Popup'

# 观察窗口：人工操作给足时间，但脚本绝不代劳。
WAIT_OPEN_S = 30.0      # 等用户点开弹层
WAIT_CLOSE_S = 30.0     # 等用户再点一下收起
REAPPEAR_S = 1.5        # 收起后再盯多久判"重弹"


def windows_of(cls):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        buf = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(h, buf, 256)
        if buf.value == cls:
            out.append(h)
        return True

    u.EnumWindows(cb, 0)
    return out


def popup_count():
    return len(windows_of(POPUP))


def wait_until(pred, timeout):
    """轮询到 pred 成立为止，返回是否成立。只读，不发任何消息。"""
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


def main():
    mains = windows_of(MAIN)
    if not mains:
        print('SKIP: 没有运行中的 ExBarBrowser_Main（先起一个实例、停到多级目录再跑）')
        return 0

    # 不做 SetForegroundWindow：要点的窗口本来就该由用户自己置于前台。
    # 不是前台说明用户正在别处干活 —— 这时应该等，而不是把焦点抢过来。
    fg = u.GetForegroundWindow()
    own_tid = u.GetWindowThreadProcessId(mains[0], None)
    fg_tid = u.GetWindowThreadProcessId(fg, None) if fg else 0
    if fg_tid != own_tid:
        print('SKIP: ExBarBrowser 不在前台（用户正在别的窗口），不抢焦点，直接跳过')
        return 0

    if popup_count():
        print('SKIP: 已经有一个弹层开着（请先按 Esc 关掉再跑）')
        return 0

    print('请手动操作（脚本不会代点）：')
    print('  1) 点一次地址栏的面包屑分隔符（›），让子目录弹层打开')
    if not wait_until(lambda: popup_count() > 0, WAIT_OPEN_S):
        print(f'SKIP: {WAIT_OPEN_S:.0f}s 内没等到弹层打开')
        return 0
    print('  已看到弹层。')

    print('  2) 再点一次**同一个**分隔符（这一次应当收起，而不是重弹）')
    if not wait_until(lambda: popup_count() == 0, WAIT_CLOSE_S):
        print('FAIL: 第二次点击后弹层一直没收起')
        return 1
    print('  已收起。')

    # 关键断言：收起后不能"自己又冒出来"。旧缺陷的表现正是这里多出一个窗口。
    if wait_until(lambda: popup_count() > 0, REAPPEAR_S):
        print(f'FAIL: 收起后 {REAPPEAR_S}s 内又弹出（重弹缺陷）')
        return 1

    print('PASS: 人工两次点击 → 打开 / 收起 / 无重弹')
    return 0


if __name__ == '__main__':
    sys.exit(main())
