// 离线驱动 BookmarkStore 的 Move/MoveTo/Undo/ReplaceAll/FileChanged/Sort/Dedup。
//
// 【绝不碰 user 的真实书签】本测试原先读写 Config::Get().BookmarksPath()，
// 也就是 <exe>\data\bookmarks.html —— 跑一次测试就把用户的书签换成测试种子，
// 跑两次还可能把"备份"也变成种子。测试必须用**自己的临时文件**。
//
// 做法：在 %TEMP% 下建一个专属目录，bookmarks.html 放那里。
// 不改 Config（它没有 setter，也不该为测试开一个），直接构造路径传给 Load/Save。
#include <windows.h>  // FILETIME（bookmarks.h 依赖主构建的 common.h 预包含，测试需自己带）

#include "bookmarks.h"
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

// 临时目录：<TEMP>\exbarbrowser_store_test_<pid>
static std::wstring TempDir() {
    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"exbarbrowser_store_test_" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// 夹具写入：**必须成功**，否则后面的判据读的是上一段留下的文件。
//
// 【为什么单独包一层】tf::WriteText 是"写 .tmp → MoveFileEx 替换"的原子写，
// 改名这一下会被 AV / 搜索索引器瞬时占用挡住（ERROR_SHARING_VIOLATION），
// 而这里 12 处夹具调用原来都不看返回值 —— 写失败后 store.Load 读到的是
// **上一段的内容**，于是报出的是"假红"。这不是假设：全量跑时 test_store
// 曾有两条 `无重复 -> 删 0 条` / `无重复时不产生撤销快照` 失败，单独重跑
// 同一个二进制 3 次全过，正是这条路径。
//
// 所以：重试跨过瞬时占用；仍失败按"无法运行"(77) 退出，而不是继续跑出一串
// 没有意义的红 —— 与 clipboard_test 的 77 同一个口径。
#define FIXTURE(path, text)                                                          \
    do {                                                                             \
        bool wrote = false;                                                          \
        for (int i = 0; i < 20 && !wrote; ++i) {                                     \
            wrote = tf::WriteText((path), (text));                                   \
            if (!wrote) Sleep(25); /* 20 × 25ms ≈ 0.5s，够跨过一次扫描 */            \
        }                                                                            \
        if (!wrote) {                                                                \
            std::printf("无法运行：夹具写入失败（被占用/权限），判据无从成立\n");     \
            return 77;                                                               \
        }                                                                            \
    } while (0)

int main() {
    BookmarkStore& store = BookmarkStore::Get();
    const std::wstring dir = TempDir();
    const std::wstring file = dir + L"\\bookmarks.html";

    // 用户的书签文件一个字节都不动。下面全部在 temp 里完成。
    printf("测试数据目录：%ls\n\n", dir.c_str());
    FIXTURE(file,
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
        FIXTURE(file, seed);
        store.Load(file);
    }
    for (int i = 0; i < 30 && !store.roots().empty(); ++i)
        store.Remove(store.roots().front().id);
    CHECK(store.roots().empty(), "all removed");
    int steps = 0;
    while (store.Undo()) ++steps;
    CHECK(steps == 20, "undo capped at 20");
    // 重新种回 3 项文件（上面的上限测试已把文件改成 30 项种子）
    FIXTURE(file,
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

    // Save 后基线刷新，不再误报。
    // 这里必须用 Save(true)：上一行是测试自己把 mtime 推后（模拟"外部改过"），
    // 默认的 Save 会正确地拒绝覆盖并另存 conflict 副本 —— 那正是新加的护栏。
    // 想让基线刷新就要显式声明"我知道有外部改动、仍然覆盖"。
    CHECK(store.Save(true), "显式覆盖式 Save 成功");
    CHECK(!store.FileChanged(), "no false positive after Save");
    CHECK(!store.TakeConflictFlag(), "显式覆盖不产生冲突副本");

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
        CHECK(store.Save(true), "显式覆盖式 Save 成功（基线刷干净）");
        CHECK(!store.FileChanged(), "基线干净时不误报");
        DeleteFileW(file.c_str());
        CHECK(store.FileChanged(), "文件不可见（已删除）-> 保守判为有变化");

        // 复原，后续用例继续用
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
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
        FIXTURE(file, bad);

        // 先装一份可用书签，确认后续 Save 有东西可丢
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/\" >D</A>\r\n</DL><p>\r\n");
        store.Load(file);
        CHECK(Names() == L"D|", "guard: seeded with D");

        // 现在把文件换成读不懂的内容，再 Load
        FIXTURE(file, bad);
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

    // ---- Merge：重复导入不得翻倍 ----
    //
    // 原实现只对顶层叶子去重；遇到文件夹就整棵子树无条件 push_back。
    // 同一份含分类的导入文件连导两次 -> 分类里的书签 1 -> 2 -> 4 份。
    printf("\n重复导入同一份含分类的书签不得翻倍\n");
    {
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/\" >D</A>\r\n"
                            L"<DT><H3>分类A</H3>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///C:/x\" >X</A>\r\n"
                            L"<DT><A HREF=\"file:///C:/y\" >Y</A>\r\n"
                            L"</DL><p>\r\n</DL><p>\r\n");
        store.Load(file);
        const size_t leaves0 = store.Flatten().size();
        CHECK(leaves0 == 3, "初始 3 个叶子（D / X / Y）");

        // 用同一份内容做一次 Merge
        // 直接构造 BookmarkList 模拟导入内容（BookmarkStore 构造是私有的）。
        BookmarkList imported;
        {
            BookmarkNode folder;
            folder.isFolder = true;
            folder.name = L"分类A";
            BookmarkNode x;
            x.name = L"X";
            x.path = L"C:\\x";
            BookmarkNode y;
            y.name = L"Y";
            y.path = L"C:\\y";
            folder.children.push_back(x);
            folder.children.push_back(y);
            imported.push_back(folder);
            BookmarkNode d;
            d.name = L"D";
            d.path = L"D:\\";
            imported.push_back(d);
        }
        const size_t added1 = store.Merge(std::move(imported));
        const size_t leaves1 = store.Flatten().size();
        CHECK(leaves1 == 3, "第一次合并后仍是 3 个叶子（全部已存在）");
        CHECK(added1 == 0, "第一次合并新增 0 条（同名分类 + 同路径叶子）");

        // 再合一次，仍不应翻倍
        BookmarkList again;
        {
            BookmarkNode folder;
            folder.isFolder = true;
            folder.name = L"分类A";
            BookmarkNode x;
            x.name = L"X2";
            x.path = L"C:\\x";   // 同名不同 label，但路径相同 -> 应去重
            folder.children.push_back(x);
            again.push_back(folder);
        }
        store.Merge(std::move(again));
        CHECK(store.Flatten().size() == 3, "第二次合并仍不翻倍（核心断言）");

        // 新分类里的**嵌套**重复叶子也必须剔掉；只查直接子项会在这里漏掉。
        BookmarkList nested;
        {
            BookmarkNode top;
            top.isFolder = true;
            top.name = L"新分类";
            BookmarkNode sub;
            sub.isFolder = true;
            sub.name = L"子分类";
            BookmarkNode dup;
            dup.name = L"D-again";
            dup.path = L"D:\\";   // 顶层已有，任意深度都不该再进
            sub.children.push_back(dup);
            top.children.push_back(sub);
            nested.push_back(top);
        }
        CHECK(store.Merge(std::move(nested)) == 2, "嵌套重复只计两个新分类，叶子被剔除");
        CHECK(store.Flatten().size() == 3, "嵌套重复路径没有翻倍");

        // 真新项应当加进来
        BookmarkList fresh;
        {
            BookmarkNode n;
            n.name = L"NEW";
            n.path = L"C:\\brand-new";
            fresh.push_back(n);
        }
        CHECK(store.Merge(std::move(fresh)) == 1, "新路径被加入并计为 1");
        CHECK(store.Flatten().size() == 4, "叶子数 3 -> 4");

        // 导入节点自带与现存书签相同的 id：必须换新，否则 Find() 会串位。
        const std::wstring existingId = store.FindByPath(L"D:\\")->id;
        BookmarkList colliding;
        {
            BookmarkNode bad;
            bad.id = existingId;
            bad.name = L"撞ID";
            bad.path = L"C:\\collide";
            colliding.push_back(bad);
        }
        CHECK(store.Merge(std::move(colliding)) == 1, "撞 id 的导入项仍被加入");
        const BookmarkNode* collided = store.FindByPath(L"C:\\collide");
        CHECK(collided && collided->id != existingId, "冲突 id 已换新");
        const BookmarkNode* original = store.Find(existingId);
        CHECK(original && original->path == L"D:\\", "Find(旧 id) 仍指向原书签，没有串位");
    }

    // ---- MoveTo：拖进分类 ----
    //
    // 原实现只有 Move（顶层插入线），分类里的书签根本没法用拖放组织 ——
    // 分类事实上只能读。而"拖进自己的子孙"会把整棵子树从树上摘下，是一条
    // 数据消失路径，必须在 store 里拦住。
    printf("\nMoveTo：拖进分类 / 拒绝成环\n");
    {
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/x\" >X</A>\r\n"
                            L"<DT><H3>A</H3>\r\n<DL><p>\r\n"
                            L"<DT><H3>B</H3>\r\n<DL><p>\r\n"
                            L"</DL><p>\r\n"
                            L"</DL><p>\r\n"
                            L"<DT><H3>C</H3>\r\n<DL><p>\r\n"
                            L"</DL><p>\r\n</DL><p>\r\n");
        store.Load(file);
        const std::wstring idX = store.roots()[0].id;
        const std::wstring idA = store.roots()[1].id;
        const std::wstring idC = store.roots()[2].id;
        std::wstring idB;
        if (const BookmarkNode* a = store.Find(idA))
            if (!a->children.empty()) idB = a->children[0].id;

        CHECK(!idB.empty(), "B 存在（A 的子分类）");
        CHECK(store.MoveTo(idX, idC), "X 拖进空分类 C");
        CHECK(store.Find(idC) && store.Find(idC)->children.size() == 1,
              "C 里有 1 项");
        CHECK(store.roots().size() == 2, "顶层剩 A / C");

        CHECK(!store.MoveTo(idA, idB), "拒绝把 A 拖进自己的子孙 B（会成环/丢子树）");
        CHECK(!store.MoveTo(idA, idA), "拒绝拖到自己");
        CHECK(!store.MoveTo(idX, idX), "拒绝：X 现在是 C 的子项，不能塞进自己");
        CHECK(store.Find(idA) != nullptr, "拒绝之后 A 仍在树上");

        // 子孙判定用真实的树结构，不是名字比较
        CHECK(store.MoveTo(idX, idA), "C 里的 X 可以移到 A");
        CHECK(store.Find(idA) && store.Find(idA)->children.size() == 2,
              "A 里现在有 B 和 X");

        // 移回顶层
        CHECK(store.MoveTo(idX, L""), "X 移回顶层");
        CHECK(!store.roots().empty() && store.roots().back().id == idX, "X 在顶层末尾");

        CHECK(store.Undo(), "移动进撤销栈");
    }

    // ---- SortRecursive：分类内部也要排 ----
    printf("\nSortRecursive：递归排序（含分类内部）\n");
    {
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><H3>Z分类</H3>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/b\" >bee</A>\r\n"
                            L"<DT><A HREF=\"file:///D:/a\" >ant</A>\r\n"
                            L"</DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/c\" >cat</A>\r\n"
                            L"<DT><H3>A分类</H3>\r\n<DL><p>\r\n</DL><p>\r\n</DL><p>\r\n");
        store.Load(file);
        store.SortRecursive(true, true);
        CHECK(store.roots().size() == 3, "3 个顶层项");
        CHECK(store.roots()[0].isFolder && store.roots()[0].name == L"A分类",
              "文件夹在前、按名称升序");
        CHECK(store.roots()[1].name == L"Z分类", "第二个文件夹");
        CHECK(store.roots()[2].name == L"cat", "文件在文件夹之后");
        // 关键断言：分类内部也被排过（原实现只排顶层）
        const BookmarkNode& z = store.roots()[1];
        CHECK(z.children.size() == 2 && z.children[0].name == L"ant",
              "分类内部按名称排序（ant 在 bee 前）");
        CHECK(store.Undo(), "排序可撤销");
    }

    // ---- RemoveDuplicatePaths：跨层级去重 ----
    printf("\nRemoveDuplicatePaths：同一路径不重复占位\n");
    {
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/dup\" >dup-top</A>\r\n"
                            L"<DT><H3>分类</H3>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/dup\" >dup-in-folder</A>\r\n"
                            L"<DT><A HREF=\"file:///D:/DUP\" >dup-case</A>\r\n"
                            L"</DL><p>\r\n</DL><p>\r\n");
        store.Load(file);
        const size_t leaves = store.Flatten().size();
        CHECK(leaves == 3, "种子里 3 条（其中 2 条是重复路径，大小写不同也算）");
        const size_t removed = store.RemoveDuplicatePaths();
        CHECK(removed == 2, "删掉 2 条重复");
        CHECK(store.Flatten().size() == 1, "只剩 1 条");
        CHECK(store.Undo() && store.Flatten().size() == 3, "去重可撤销");

        // 没有重复时不该产生撤销记录（"清理"不该无端点亮"撤销删除"）
        FIXTURE(file, L"<!DOCTYPE NETSCAPE-Bookmark-file-1>\r\n<DL><p>\r\n"
                            L"<DT><A HREF=\"file:///D:/a\" >a</A>\r\n"
                            L"<DT><A HREF=\"file:///D:/b\" >b</A>\r\n</DL><p>\r\n");
        store.Load(file);
        while (store.Undo()) {}   // 清空撤销栈
        CHECK(store.RemoveDuplicatePaths() == 0, "无重复 -> 删 0 条");
        CHECK(!store.CanUndo(), "无重复时不产生撤销快照");
    }

    // 收尾：删掉临时目录（不碰用户书签）
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".conflict.html").c_str());
    RemoveDirectoryW(dir.c_str());
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
