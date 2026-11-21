// 目录体积统计自测（OpTopN / OpDiskUsage 的数据源）。
//
// 这层能脱离窗口测，所以必须测 —— 它有三个容易出错的地方：
//   1. 大文件（>4 GB）的高低位拼接
//   2. 不跟随重解析点（跟随会无限递归）
//   3. 超时时给出"部分结果 + timedOut 标记"，而不是空结果或卡死
#include "common.h"

#include <cstdio>
#include <string>

#include "disk_scan.h"
#include "text_file.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

std::wstring MakeTempTree() {
    wchar_t buf[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, buf);
    const std::wstring root = std::wstring(buf) + L"ExBarScanTest";
    // 清掉上一次的残留
    std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\"";
    _wsystem(cmd.c_str());

    tf::EnsureDir(root);
    tf::EnsureDir(root + L"\\sub1");
    tf::EnsureDir(root + L"\\sub2");
    tf::EnsureDir(root + L"\\sub1\\deep");

    // 各写已知大小的文件
    auto write = [](const std::wstring& p, size_t n) {
        FILE* f = nullptr;
        _wfopen_s(&f, p.c_str(), L"wb");
        if (!f) return;
        std::string chunk(n, 'x');
        fwrite(chunk.data(), 1, n, f);
        fclose(f);
    };
    write(root + L"\\big.bin", 30000);
    write(root + L"\\sub1\\a.bin", 20000);
    write(root + L"\\sub1\\deep\\b.bin", 5000);
    write(root + L"\\sub2\\c.bin", 1000);
    return root;
}

void Cleanup(const std::wstring& root) {
    std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\"";
    _wsystem(cmd.c_str());
}

}  // namespace

int main() {
    const std::wstring root = MakeTempTree();

    std::printf("基本统计\n");
    {
        diskscan::Options opt;
        opt.budgetMs = 10000;
        opt.limit = 10;
        diskscan::Result r;
        std::wstring err;
        const bool ok = diskscan::Scan(root, opt, &r, &err);
        Check(ok, "扫描成功");
        if (!ok) std::printf("        err=%ls\n", err.c_str());

        // 总量 = 30000 + 20000 + 5000 + 1000 = 56000
        Check(r.totalBytes == 56000, "总量 56000 字节");
        if (r.totalBytes != 56000)
            std::printf("        实际: %llu\n", r.totalBytes);

        Check(!r.timedOut, "未超时");
        Check(r.deniedDirs == 0, "没有无法访问的目录");
        Check(r.complete, "整次统计完整");
        Check(!r.truncated, "未截断（3 个子项 < limit 10）");
        Check(r.entries.size() == 3, "3 个直接子项");
    }

    std::printf("\n按大小降序\n");
    {
        diskscan::Options opt;
        opt.budgetMs = 10000;
        diskscan::Result r;
        std::wstring err;
        diskscan::Scan(root, opt, &r, &err);
        // 预期顺序：big.bin(30000) > sub1(20000+5000=25000) > sub2(1000)
        Check(r.entries.size() == 3, "共 3 项");
        if (r.entries.size() == 3) {
            Check(r.entries[0].name == L"big.bin", "第一是 big.bin（30000）");
            Check(r.entries[0].bytes == 30000, "big.bin = 30000");
            Check(r.entries[1].name == L"sub1", "第二是 sub1（25000）");
            Check(r.entries[1].bytes == 25000, "sub1 = 20000 + 5000 = 25000");
            Check(r.entries[2].name == L"sub2", "第三是 sub2（1000）");

            // 降序是硬要求：任何相邻两项都不能逆序
            bool desc = true;
            for (size_t k = 1; k < r.entries.size(); ++k)
                if (r.entries[k - 1].bytes < r.entries[k].bytes) desc = false;
            Check(desc, "整体保持降序");
        } else {
            std::printf("        实际顺序：");
            for (const auto& e : r.entries)
                std::printf("%ls(%llu) ", e.name.c_str(), e.bytes);
            std::printf("\n");
        }
    }

    std::printf("\nlimit 截断\n");
    {
        diskscan::Options opt;
        opt.budgetMs = 10000;
        opt.limit = 1;
        diskscan::Result r;
        std::wstring err;
        diskscan::Scan(root, opt, &r, &err);
        Check(r.entries.size() == 1, "只返回 1 项");
        Check(r.truncated, "标记了截断");
        // 关键：totalBytes 要含全部，不能只算返回的那一项
        Check(r.totalBytes == 56000, "总量仍是全部 56000（不被 limit 影响）");
    }

    std::printf("\n目录条目带 items 计数\n");
    {
        diskscan::Options opt;
        opt.budgetMs = 10000;
        diskscan::Result r;
        std::wstring err;
        diskscan::Scan(root, opt, &r, &err);
        for (const auto& e : r.entries) {
            if (e.name == L"sub1") {
                // sub1 下：a.bin + deep 目录 + deep/b.bin = 1 文件 + 1 目录 + 1 文件
                Check(e.isFolder, "sub1 是目录");
                Check(e.items >= 3, "sub1 至少有 3 个条目（2 文件 + 1 目录）");
                Check(e.complete, "sub1 统计完整");
            }
        }
    }

    std::printf("\n不存在的路径\n");
    {
        diskscan::Options opt;
        diskscan::Result r;
        std::wstring err;
        const bool ok = diskscan::Scan(root + L"\\__不存在__", opt, &r, &err);
        Check(!ok, "不存在的路径 -> 失败");
        Check(!err.empty(), "给出可读原因");
        std::printf("        err=%ls\n", err.c_str());
    }

    std::printf("\n不是目录\n");
    {
        diskscan::Options opt;
        diskscan::Result r;
        std::wstring err;
        const bool ok = diskscan::Scan(root + L"\\big.bin", opt, &r, &err);
        Check(!ok, "文件 -> 失败");
        Check(err == L"不是目录", "原因准确");
    }

    std::printf("\n时间预算极小 -> 部分结果 + timedOut\n");
    {
        diskscan::Options opt;
        opt.budgetMs = 0;   // 立刻超时
        opt.limit = 10;
        diskscan::Result r;
        std::wstring err;
        const bool ok = diskscan::Scan(root, opt, &r, &err);
        Check(ok, "仍返回成功（数据部分可用）");
        Check(r.timedOut, "标记了 timedOut");
        Check(!r.complete, "标记了不完整（不能把部分结果当真实值）");
        // 关键：不能什么都不给。至少路径和目录结构该有
        Check(!r.path.empty(), "仍给出路径");
        std::printf("        timedOut=%d entries=%zu elapsed=%llums\n", (int)r.timedOut,
                    r.entries.size(), r.elapsedMs);
    }

    Cleanup(root);
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
