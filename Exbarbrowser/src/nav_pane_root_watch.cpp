// 导航树根集的变更监听：explorer 侧边栏配置变化 -> RefreshDefaultsAsync。
//
// 【为什么单独成文件】这里全是 OS 通知源（注册表 + 文件系统）和线程/句柄管理；
// nav_pane_root.cpp 是"配置 <-> RootSpec <-> IShellItem + 异步发现"的语义层。
// 两者改动理由不同，硬塞在一起会让原本就不小的发现实现继续膨胀。
#include "nav_pane_root.h"

#include "log.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <mutex>
#include <thread>
#include <vector>

namespace navpane {

namespace {

constexpr wchar_t kExplorerDesktopKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop";
// "显示所有文件夹 / 显示库 / 在网络中显示…" 这类 Folder Options 也改这里的值。
constexpr wchar_t kExplorerAdvancedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

std::thread g_watchThread;
HANDLE g_watchStop = nullptr;
std::mutex g_watchMu;
std::function<void()> g_watchHook;

// 快速访问（主页）里"固定到快速访问"的项不在注册表里，而在当前用户的
// AutomaticDestinations 文件中。只监听 HKCU\Desktop\NameSpace 能看见第三方
// 命名空间项的增删，却看不见主页固定项的 Pin/Unpin —— 那正是"我在 explorer
// 里移除了，程序没跟着移除"的常见来源。监听整个目录而不是单个文件：不同
// Windows 版本的文件名有变体，短暂误报合并到 150ms 防抖里比漏掉变化划算。
std::wstring QuickAccessWatchDir() {
    PWSTR roaming = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr,
                                       &roaming)) &&
        roaming) {
        wchar_t buf[MAX_PATH] = L"";
        if (PathCombineW(buf, roaming, L"Microsoft\\Windows\\Recent\\AutomaticDestinations")) {
            dir = buf;
        }
        CoTaskMemFree(roaming);
    }
    return dir;
}

}  // namespace

bool StartRootChangeWatch(std::function<void()> onChanged) {
    {
        std::lock_guard<std::mutex> lock(g_watchMu);
        g_watchHook = std::move(onChanged);
        if (g_watchThread.joinable()) return false;
    }

    // 三类来源都要盯：
    //   1) Desktop 子树：NameSpace 下的第三方扩展项增删/改名；
    //   2) Advanced 的值：Folder Options 里"显示所有文件夹/库/网络"等开关；
    //   3) AutomaticDestinations 目录：主页固定项的 Pin/Unpin。
    // 三者共用一条监听线程和一份 g_watchStop，退出时只需停一次、join 一次。
    HKEY nsKey = nullptr;
    HKEY advKey = nullptr;
    bool haveNs = RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerDesktopKey, 0, KEY_NOTIFY,
                                &nsKey) == ERROR_SUCCESS;
    bool haveAdv = RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedKey, 0, KEY_NOTIFY,
                                 &advKey) == ERROR_SUCCESS;
    HANDLE nsChange = haveNs ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    HANDLE advChange = haveAdv ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (haveNs && !nsChange) haveNs = false;
    if (haveAdv && !advChange) haveAdv = false;

    const std::wstring quickDir = QuickAccessWatchDir();
    HANDLE dirChange = INVALID_HANDLE_VALUE;
    bool haveDir = false;
    if (!quickDir.empty()) {
        dirChange = FindFirstChangeNotificationW(
            quickDir.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE);
        haveDir = dirChange != INVALID_HANDLE_VALUE;
        if (!haveDir) {
            LogDbg(L"[navpane] 无法监听快速访问目录（FindFirstChangeNotification）");
        }
    }

    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) {
        if (nsChange) CloseHandle(nsChange);
        if (advChange) CloseHandle(advChange);
        if (nsKey) RegCloseKey(nsKey);
        if (advKey) RegCloseKey(advKey);
        if (haveDir) FindCloseChangeNotification(dirChange);
        return false;
    }
    g_watchStop = stop;

    // 注册要在建线程之前完成：否则监听线程刚 Wait 时事件还没 arm，
    // 这期间的改动就永远收不到了。
    if (haveNs &&
        RegNotifyChangeKeyValue(nsKey, TRUE,
                                REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                nsChange, TRUE) != ERROR_SUCCESS) {
        haveNs = false;
    }
    if (haveAdv &&
        RegNotifyChangeKeyValue(advKey, FALSE,
                                REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                advChange, TRUE) != ERROR_SUCCESS) {
        haveAdv = false;
    }

    g_watchThread = std::thread([nsKey, advKey, nsChange, advChange, dirChange, stop, haveNs,
                                 haveAdv, haveDir] {
        std::vector<HANDLE> waits;
        waits.push_back(stop);                 // 索引 0 必须是停止信号
        if (haveNs) waits.push_back(nsChange);
        if (haveAdv) waits.push_back(advChange);
        if (haveDir) waits.push_back(dirChange);

        for (;;) {
            const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(waits.size()),
                                                   waits.data(), FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) break;     // 退出信号
            if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + waits.size()) {
                LogDbg(L"[navpane] 根集监听 Wait 失败 %lu", static_cast<unsigned long>(w));
                Sleep(300);
                continue;
            }

            const HANDLE signaled = waits[w - WAIT_OBJECT_0];
            bool changed = false;
            if (signaled == nsChange && haveNs) {
                // NameSpace 子键可能被反复改写，先重新 arm 再做发现。
                RegNotifyChangeKeyValue(nsKey, TRUE,
                                        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                        nsChange, TRUE);
                changed = true;
            } else if (signaled == advChange && haveAdv) {
                RegNotifyChangeKeyValue(advKey, FALSE,
                                        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                        advChange, TRUE);
                changed = true;
            } else if (signaled == dirChange && haveDir) {
                // Filesystem watch 也是"一次性"的，取走通知后必须续上。
                FindNextChangeNotification(dirChange);
                changed = true;
            }
            if (!changed) continue;

            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lock(g_watchMu);
                hook = g_watchHook;
            }
            if (hook) hook();
            Sleep(150);   // explorer 一次改配置常连发多条，合并一下
        }

        if (nsKey) RegCloseKey(nsKey);
        if (advKey) RegCloseKey(advKey);
        if (nsChange) CloseHandle(nsChange);
        if (advChange) CloseHandle(advChange);
        if (haveDir) FindCloseChangeNotification(dirChange);
    });
    return true;
}

void StopRootChangeWatch() {
    HANDLE stop = g_watchStop;
    if (stop) SetEvent(stop);
    if (g_watchThread.joinable()) g_watchThread.join();
    if (stop) {
        CloseHandle(stop);
        g_watchStop = nullptr;
    }
}

}  // namespace navpane
