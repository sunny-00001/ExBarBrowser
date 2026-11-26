// 复制/移动的撤销记录构造（src/transfer_undo.h）自测。
//
// 测的是**判据**（会真的建临时文件，因为 tf::Exists 是文件系统事实）：
//   · 目标目录原本已有同名文件 = 冲突 → 不记（否则撤销删错用户文件）
//   · 同目录复制/移动 → 不记（否则撤销删源）
//   · 落点确实存在且无冲突 → 记一条，before/after 正确
//   · 源文件名取不到 → 跳过
//
// 【为什么这条必须单测】判据错了不会报错，只会在用户按 Ctrl+Z 时删错文件。
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "transfer_undo.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 建一个空临时目录（带随机后缀，避免与别的会话/测试撞）。
std::wstring MakeTempDir() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    for (int i = 0; i < 100; ++i) {
        const std::wstring p = std::wstring(base) + L"ebb_transfer_undo_" +
                               std::to_wstring(GetCurrentProcessId()) + L"_" +
                               std::to_wstring(i);
        if (CreateDirectoryW(p.c_str(), nullptr)) return p;
    }
    return L"";
}

// 建一个空文件（内容无所谓，存在即可）。
bool Touch(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void RemoveTree(const std::wstring& dir) {
    // 目录里只有我们建的文件 + 一层子目录：不递归删，逐项清即可。
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring full = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(full.c_str());
            else
                DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
    std::printf("复制/移动的撤销记录构造（transfer_undo）\n");

    const std::wstring root = MakeTempDir();
    if (root.empty()) {
        std::printf("  无法创建临时目录，跳过\n");
        return 77;   // 77 = 无法运行（与 run_all.sh 的约定一致）
    }
    const std::wstring src = root + L"\\src";
    const std::wstring dst = root + L"\\dst";
    CreateDirectoryW(src.c_str(), nullptr);
    CreateDirectoryW(dst.c_str(), nullptr);

    const std::wstring a = src + L"\\a.txt";
    const std::wstring b = src + L"\\b.txt";
    Touch(a);
    Touch(b);
    // dst 里**预先**放一个 b.txt：模拟"目标目录本来就有同名文件"的冲突形态。
    Touch(dst + L"\\b.txt");

    const std::vector<std::wstring> sources{a, b};

    // ---- 冲突判定 ----
    std::printf("\n落点预探\n");
    {
        const std::vector<bool> pre = transferundo::PrecheckDest(sources, dst);
        Check(pre.size() == 2, "预探结果与源等长");
        Check(pre.size() == 2 && !pre[0], "a.txt 在目标目录不存在 → false");
        Check(pre.size() == 2 && pre[1], "b.txt 在目标目录已存在 → true（冲突）");
    }

    // ---- 只记无冲突的那条 ----
    std::printf("\n构造记录（冲突跳过）\n");
    {
        // 顺序就是真实调用顺序：**转移前**预探 → 转移 → 转移后构造记录。
        const std::vector<bool> pre = transferundo::PrecheckDest(sources, dst);
        Touch(dst + L"\\a.txt");   // 模拟 shell 把 a.txt 复制过去了
        const auto items = transferundo::BuildItems(sources, dst, pre);
        Check(items.size() == 1, "冲突项被跳过，只剩 1 条");
        if (items.size() == 1) {
            Check(items[0].beforePath == a, "beforePath = 源 a.txt");
            Check(items[0].afterPath == dst + L"\\a.txt", "afterPath = 目标目录里的 a.txt");
            Check(items[0].beforeName == L"a.txt", "beforeName = a.txt");
        }
    }

    // ---- 同目录：整条不记（否则撤销会删源）----
    std::printf("\n同目录不记\n");
    {
        const std::vector<bool> pre = transferundo::PrecheckDest({a}, src);
        Check(pre.size() == 1 && pre[0], "同目录预探：落点就是源本身 → true");
        const auto items = transferundo::BuildItems({a}, src, pre);
        Check(items.empty(), "同目录复制/移动不产生撤销记录");
    }

    // ---- 空目标目录 / 空叶子 ----
    std::printf("\n边界\n");
    {
        const auto items = transferundo::BuildItems({a}, L"", {});
        Check(items.empty(), "目标为空 → 不记");
        const auto items2 = transferundo::BuildItems({L""}, dst, {});
        Check(items2.empty(), "源为空串（取不到文件名）→ 跳过");
        // preExisted 比 sources 短：按"没探过"处理，仍要求落点存在。
        const std::vector<bool> empty;
        const auto items3 = transferundo::BuildItems({a}, dst, empty);
        Check(items3.size() == 1, "preExisted 缺省时：落点存在且无冲突仍记");
    }

    // ---- 落点不存在（并发改名/被删）----
    std::printf("\n落点不存在\n");
    {
        DeleteFileW((dst + L"\\a.txt").c_str());   // 上一步没真复制，本来就没有
        const auto items = transferundo::BuildItems({a}, dst, {false});
        Check(items.empty(), "落点不存在 → 不记（不承诺一个撤不动的记录）");
    }

    RemoveTree(src);
    RemoveTree(dst);
    RemoveDirectoryW(root.c_str());

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
