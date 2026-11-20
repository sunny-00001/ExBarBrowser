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

    // ---- FileChanged 的两种漏判 ----
    printf("\n外部变更检测不得漏判（mtime 相同 / 文件打不开）\n");
    {
        // ① 只改大小、不动 mtime：FAT/网络共享的粒度可达 2 秒，
        //    CopyFile 保留时间戳，SetFileTime 可回拨 —— 这些场景 mtime 都不变。
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME keep{};
            GetFileTime(h, nullptr, nullptr, &keep);
            SetFilePointer(h, 0, nullptr, FILE_END);
            const char extra[] = "<DT><A HREF=\"file:///Z:/\">Z</A>\r\n";
            DWORD w = 0;
            WriteFile(h, extra, sizeof(extra) - 1, &w, nullptr);   // 变大，mtime 变了
            SetFileTime(h, nullptr, nullptr, &keep);               // 再把 mtime 改回去
            CloseHandle(h);
            CHECK(store.FileChanged(), "mtime 相同但大小变了 -> 检出");
        } else {
            printf("skip 无法打开文件做大小测试\n");
        }

        // ② 文件"看不到"时必须保守判为有变化。
        //
        //    旧实现 `if (cur 是零) return false` 把"读不到"当成"没变"。
        //    而读不到最常见的原因恰恰是**另一进程正独占着它**（编辑器保存中、
        //    同步工具写入中），那正是最该拒绝覆盖的时刻。
        //
        //    离线单测没法让另一个进程持续持句柄，但"文件消失"走的是同一条
        //    失败分支（LastWriteOf 返回零值），足以验证语义。
        store.Save();   // 基线刷干净
        CHECK(!store.FileChanged(), "基线干净时不误报");
        DeleteFileW(file.c_str());
        CHECK(store.FileChanged(), "文件不可见（已删除）-> 保守判为有变化");

        // 复原，后续用例继续用
        tf::WriteText(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/\" >D</A>\r\n</DL><p>\r\n");
        store.Load(file);
        CHECK(!store.FileChanged(), "重新加载后基线恢复正常");
    }

    // ---- 数据丢失护栏：读不懂的文件不许被覆盖 ----
    //
    // 这是全模块唯一的数据丢失链：Load 失败 → 书签栏空白 → 用户加一个书签 →
    // Save() 用只有表头的 HTML 覆盖原文件 → 原有书签永久丢失。
    printf("\n加载失败时必须拒绝写盘（防止清空用户书签）\n");
    {
        // 造一个"存在但读不懂"的文件：有内容，但不是 Netscape 书签格式。
        const std::wstring bad = L"\xD6\xD0\xCE\xC4 这不是书签文件";  // 含非 ASCII
        tf::WriteText(file, bad);

        // 先装一份可用书签，确认后续 Save 有东西可丢
        tf::WriteText(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/\" >D</A>\r\n</DL><p>\r\n");
        store.Load(file);
        CHECK(Names() == L"D|", "guard: seeded with D");

        // 现在把文件换成读不懂的内容，再 Load
        tf::WriteText(file, bad);
        CHECK(!store.Load(file), "guard: Load rejects non-bookmark content");
        CHECK(store.LoadFailed(), "guard: LoadFailed() reports it");
        CHECK(Names() == L"D|", "guard: in-memory tree NOT cleared by failed Load");

        // 关键断言：此时 Save 必须拒绝，且文件内容保持原样（不被空书签覆盖）
        CHECK(!store.Save(), "guard: Save refused after failed Load");
        std::wstring after;
        tf::ReadText(file, &after);
        CHECK(after == bad, "guard: file on disk untouched (no empty-bookmark overwrite)");

        // 文件不存在 ≠ 加载失败：首次运行应当允许写盘
        DeleteFileW(file.c_str());
        store.Load(file);
        CHECK(!store.LoadFailed(), "guard: missing file is NOT a load failure");
        CHECK(store.Save(), "guard: first run can still save");
    }

    // 恢复原文件
    if (!backup.empty()) {
        tf::WriteText(file, backup);
        store.Load(file);
    }
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
