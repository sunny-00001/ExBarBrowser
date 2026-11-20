// 离线驱动 BookmarkStore 的 Move/Undo/ReplaceAll/FileChanged，验证 P3 逻辑正确性。
// 独立编译运行（不走主构建），结束后把 store 恢复原状。
#include <windows.h>  // FILETIME（bookmarks.h 依赖主构建的 common.h 预包含，测试需自己带）

#include "bookmarks.h"
#include "config.h"
#include "text_file.h"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (cond) {                                          \
            printf("ok   %s\n", msg);                        \
        } else {                                             \
            printf("FAIL %s\n", msg);                       \
            ++fails;                                         \
        }                                                    \
    } while (0)

static std::wstring Names() {
    std::wstring s;
    for (const auto& n : BookmarkStore::Get().roots()) {
        s += n.isFolder ? L"[F]" : L"";
        s += n.name;
        s += L"|";
    }
    return s;
}

int main() {
    // 备份 → 用受控种子替换
    BookmarkStore& store = BookmarkStore::Get();
    std::wstring file = Config::Get().BookmarksPath();
    std::wstring backup;
    tf::ReadText(file, &backup);

    tf::WriteText(file,
                  L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                  L"<DT><A HREF=\"file:///D:/\" >D</A>\r\n"
                  L"<DT><A HREF=\"file:///C:/\" >C</A>\r\n"
                  L"<DT><A HREF=\"file:///E:/\" >E</A>\r\n"
                  L"</DL><p>\r\n");
    store.Load(file);
    CHECK(Names() == L"D|C|E|", "seed order D,C,E");

    // Move：D(0) → 插入线在 E 之后（toIndex=3）→ 视觉结果 C,E,D
    CHECK(store.Move(store.roots()[0].id, 3), "move D to 3");
    CHECK(Names() == L"C|E|D|", "insert-line semantics -> C,E,D");
    // Undo 回到 D,C,E
    CHECK(store.Undo(), "undo move");
    CHECK(Names() == L"D|C|E|", "undo restores D,C,E");
    // 原位移动无效
    CHECK(!store.Move(store.roots()[0].id, 0), "no-op move rejected");
    CHECK(store.Move(store.roots()[0].id, 2), "move D to 2 (same slot adj)");
    CHECK(Names() == L"C|D|E|", "move D between C,E -> C,D,E");
    store.Undo();

    // Undo 上限 20：种 30 项、连续删 30 次后只能回 20 步
    {
        std::wstring seed = L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n";
        for (int i = 0; i < 30; ++i)
            seed += L"<DT><A HREF=\"file:///D:/" + std::to_wstring(i) + L"\" >i" +
                    std::to_wstring(i) + L"</A>\r\n";
        seed += L"</DL><p>\r\n";
        tf::WriteText(file, seed);
        store.Load(file);
    }
    for (int i = 0; i < 30 && !store.roots().empty(); ++i)
        store.Remove(store.roots().front().id);
    CHECK(store.roots().empty(), "all removed");
    int steps = 0;
    while (store.Undo()) ++steps;
    CHECK(steps == 20, "undo capped at 20");
    // 重新种回 3 项文件（上面的上限测试已把文件改成 30 项种子）
    tf::WriteText(file,
                  L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                  L"<DT><A HREF=\"file:///D:/\" >D</A>\r\n"
                  L"<DT><A HREF=\"file:///C:/\" >C</A>\r\n"
                  L"<DT><A HREF=\"file:///E:/\" >E</A>\r\n"
                  L"</DL><p>\r\n");
    store.Load(file);

    // ReplaceAll 可撤销（清空书签的恢复路径）
    store.ReplaceAll(BookmarkList{});
    CHECK(store.roots().empty(), "replace-all cleared");
    CHECK(store.Undo() && Names() == L"D|C|E|", "undo after clear");

    // FileChanged：外部用 SetFileTime 推后 mtime 后应返回 true
    // （零字节打开+关闭不会更新 LastWriteTime，必须显式改时间戳）
    Sleep(60);
    HANDLE h = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULONGLONG t = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        t += 10000000;  // +1s
        FILETIME nt{static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
        SetFileTime(h, nullptr, nullptr, &nt);
        CloseHandle(h);
    }
    CHECK(store.FileChanged(), "FileChanged detects external touch");

    // Save 后基线刷新，不再误报
    store.Save();
    CHECK(!store.FileChanged(), "no false positive after Save");

    // 恢复原文件
    if (!backup.empty()) {
        tf::WriteText(file, backup);
        store.Load(file);
    }
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
