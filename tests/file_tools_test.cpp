// 文件集工具自测（tool.hash / tool.dupfinder / tool.compare 的数据源）。
//
// 这三条命令的结果会被用来**删文件**，所以数据层必须能脱离窗口测：
//   1. 摘要要对得上标准向量（MD5/SHA-1/SHA-256 各一条空串、一条 "abc"）——
//      自己写的十六进制转换或长度处理错一位，得到的就是"看起来像哈希"的垃圾；
//   2. 查重按"大小 + 内容"判，**不按文件名**：改名副本必须算重复；
//      同大小不同内容**不能**算重复（否则用户会删掉唯一那份）；
//   3. 比较的 byContent 开关必须真的改变结论，而不是个装饰参数。
#include "common.h"

#include <cstdio>
#include <string>

#include "file_hash.h"
#include "file_tools.h"
#include "file_rename_plan.h"
#include "text_file.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void CheckW(bool ok, const wchar_t* what) {
    std::printf("  %s  %ls\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void WriteFile(const std::wstring& p, const std::string& bytes) {
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"wb");
    if (!f) return;
    if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

// 夹具：a/ 与 b/ 两个目录，故意构造四种关系
//   dup1.bin（两侧同名同内容）          → 相同
//   dup2-renamed.bin（只有名字不同）    → 与 dup1 **内容相同**，查重必须归为一组
//   samelen.bin（同大小、内容不同）      → 比较时必须判 differ
//   only-left.bin / only-right.bin      → 各缺一侧
std::wstring MakeFixture() {
    wchar_t buf[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, buf);
    const std::wstring root = std::wstring(buf) + L"ExBarToolsTest";

    std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\"";
    _wsystem(cmd.c_str());

    tf::EnsureDir(root);
    tf::EnsureDir(root + L"\\a");
    tf::EnsureDir(root + L"\\b");

    // 200 KB 的可重复内容（不要用全 0：全 0 会让"长度对但内容错"的实现也碰巧通过）
    std::string big;
    big.reserve(200 * 1024);
    for (int i = 0; i < 200 * 1024; ++i) big.push_back(static_cast<char>((i * 7 + 13) & 0xFF));

    WriteFile(root + L"\\a\\dup1.bin", big);
    WriteFile(root + L"\\a\\dup2-renamed.bin", big);
    WriteFile(root + L"\\b\\dup1.bin", big);
    WriteFile(root + L"\\a\\samelen.bin", std::string(1024, 'A'));
    WriteFile(root + L"\\b\\samelen.bin", std::string(1024, 'B'));
    WriteFile(root + L"\\a\\only-left.bin", std::string(4096, 'L'));
    WriteFile(root + L"\\b\\only-right.bin", std::string(4096, 'R'));
    return root;
}

void Cleanup(const std::wstring& root) {
    std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\"";
    _wsystem(cmd.c_str());
}

}  // namespace

int main() {
    const std::wstring root = MakeFixture();

    std::printf("算法名解析\n");
    {
        filehash::Algo a{};
        Check(filehash::ParseAlgo(L"sha256", &a) && a == filehash::Algo::Sha256, "sha256");
        Check(filehash::ParseAlgo(L"SHA-256", &a) && a == filehash::Algo::Sha256, "SHA-256（界面写法）");
        Check(filehash::ParseAlgo(L"md5", &a) && a == filehash::Algo::Md5, "md5");
        Check(filehash::ParseAlgo(L"SHA1", &a) && a == filehash::Algo::Sha1, "SHA1");
        // 认不出必须失败：静默退回 SHA-256 会让"我算的是 SHA-512"与实际结果无法区分
        Check(!filehash::ParseAlgo(L"sha512", &a), "sha512 被拒绝");
        Check(!filehash::ParseAlgo(L"", &a), "空串被拒绝");
        Check(filehash::AlgoName(filehash::Algo::Sha1) == std::wstring(L"SHA-1"), "规范名 SHA-1");
    }

    std::printf("\n标准向量（空串 / abc）\n");
    {
        const std::wstring empty = root + L"\\empty.bin";
        const std::wstring abc = root + L"\\abc.bin";
        WriteFile(empty, "");
        WriteFile(abc, "abc");

        struct Case {
            filehash::Algo algo;
            const std::wstring* path;
            const wchar_t* want;
            const wchar_t* what;
        };
        const Case cases[] = {
            {filehash::Algo::Md5, &empty, L"d41d8cd98f00b204e9800998ecf8427e", L"MD5(空)"},
            {filehash::Algo::Sha1, &empty, L"da39a3ee5e6b4b0d3255bfef95601890afd80709",
             L"SHA-1(空)"},
            {filehash::Algo::Sha256, &empty,
             L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", L"SHA-256(空)"},
            {filehash::Algo::Md5, &abc, L"900150983cd24fb0d6963f7d28e17f72", L"MD5(abc)"},
            {filehash::Algo::Sha1, &abc, L"a9993e364706816aba3e25717850c26c9cd0d89d",
             L"SHA-1(abc)"},
            {filehash::Algo::Sha256, &abc,
             L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256(abc)"},
        };
        for (const auto& c : cases) {
            filehash::Result r;
            const bool ok = filehash::HashFile(*c.path, c.algo, &r);
            Check(ok, "成功");
            CheckW(r.hex == c.want, c.what);
            if (r.hex != c.want) std::printf("        实际 %ls\n", r.hex.c_str());
        }
    }

    std::printf("\n拒绝非文件\n");
    {
        filehash::Result r;
        Check(!filehash::HashFile(root, filehash::Algo::Sha256, &r), "目录被拒绝");
        Check(!r.error.empty(), "有可读原因");
        Check(!filehash::HashFile(root + L"\\nope.bin", filehash::Algo::Sha256, &r),
              "不存在的文件被拒绝");

        // 取消标志：置位后必须失败，而不是硬算完
        std::atomic<bool> cancel{true};
        Check(!filehash::HashFile(root + L"\\a\\dup1.bin", filehash::Algo::Sha256, &r, &cancel),
              "取消后失败");
        Check(r.error == L"已取消", "错误里写明已取消");
    }

    std::printf("\n时间预算：大文件必须能被打断（tool.hash 跑在 UI 线程上）\n");
    {
        // 为什么要有这条：tool.hash 跑在 UI 线程上（AI 请求由定时器泵派发，
        // 人按快捷键也是 UI 线程）。没有预算的话，一个几十 MB 的文件就能把
        // 界面按死十几秒，用户连取消都按不了（UI 线程被占着，定时器都不转）。
        const std::wstring bigPath = root + L"\\big.bin";
        const size_t kBigSize = 64u * 1024u * 1024u;
        {
            FILE* f = nullptr;
            _wfopen_s(&f, bigPath.c_str(), L"wb");
            if (f) {
                const std::string chunk(1024 * 1024, 'x');
                for (size_t i = 0; i < kBigSize / chunk.size(); ++i)
                    fwrite(chunk.data(), 1, chunk.size(), f);
                fclose(f);
            }
        }

        // 1ms 预算：64 MB 无论从页缓存还是磁盘读都读不完 —— 必须被打断。
        filehash::Result r;
        const bool ok = filehash::HashFile(bigPath, filehash::Algo::Sha256, &r, nullptr, 1);
        Check(!ok, "预算用完 → 失败（而不是硬算完）");
        Check(r.timedOut, "标了 timedOut（调用方能区分「没算完」与「算失败」）");
        Check(r.hex.empty(), "没算完就没有摘要（绝不给半截哈希当结果）");
        Check(!r.error.empty(), "有可读原因");
        Check(r.bytes < kBigSize, "只读了一部分就停了");

        // 预算充足 → 必须算完，且是完整的 64 位十六进制。
        filehash::Result r2;
        Check(filehash::HashFile(bigPath, filehash::Algo::Sha256, &r2, nullptr, 120000),
              "预算充足时算完");
        Check(!r2.timedOut, "预算充足时没超时");
        Check(r2.hex.size() == 64, "SHA-256 是 64 个十六进制字符");
        Check(r2.bytes == kBigSize, "读满了整个文件");
    }

    std::printf("\n查重：按内容不按名字\n");
    {
        filetools::Options opt;
        opt.budgetMs = 20000;
        filetools::DupResult r;
        std::wstring err;
        const bool ok = filetools::FindDuplicates(root, opt, &r, &err);
        Check(ok, "扫描成功");
        if (!ok) std::printf("        err=%ls\n", err.c_str());

        Check(r.complete, "扫完了");
        Check(r.deniedDirs == 0, "没有被拒的目录");
        Check(r.groups.size() == 1, "恰好 1 组重复");
        if (r.groups.size() == 1) {
            const auto& g = r.groups[0];
            Check(g.files.size() == 3, "组内 3 份（含改名那份）");
            Check(g.bytes == 200 * 1024, "单份 200 KB");
            // 关键：改名副本必须在组里
            bool foundRenamed = false;
            for (const auto& f : g.files)
                if (f.path.find(L"dup2-renamed.bin") != std::wstring::npos) foundRenamed = true;
            Check(foundRenamed, "改名副本 dup2-renamed.bin 被认出");
            // 同大小不同内容不能被并进来
            bool leaked = false;
            for (const auto& f : g.files)
                if (f.path.find(L"samelen.bin") != std::wstring::npos) leaked = true;
            Check(!leaked, "同大小但内容不同的 samelen.bin 没被并进组");
        }
        Check(r.wastedBytes == 2ULL * 200 * 1024, "可回收 = 2 份 × 200 KB");
    }

    std::printf("\n查重：minBytes 过滤\n");
    {
        filetools::Options opt;
        opt.budgetMs = 20000;
        opt.minBytes = 300 * 1024;   // 比所有文件都大
        filetools::DupResult r;
        std::wstring err;
        Check(filetools::FindDuplicates(root, opt, &r, &err), "扫描成功");
        Check(r.groups.empty(), "没有组（都被 minBytes 过滤）");
        Check(r.wastedBytes == 0, "可回收 0");
    }

    std::printf("\n比较：byContent=true（默认）\n");
    {
        filetools::CompareOptions opt;
        opt.walk.budgetMs = 20000;
        filetools::CompareResult r;
        std::wstring err;
        const bool ok =
            filetools::CompareFolders(root + L"\\a", root + L"\\b", opt, &r, &err);
        Check(ok, "比较成功");
        Check(r.complete, "比完了");
        Check(r.sameCount == 1, "1 项相同（dup1.bin）");
        Check(r.differCount == 1, "1 项不同（samelen.bin 同大小不同内容）");
        Check(r.onlyLeftCount == 2, "2 项仅左（dup2-renamed / only-left）");
        Check(r.onlyRightCount == 1, "1 项仅右（only-right）");

        bool samelenIsDiffer = false;
        for (const auto& e : r.entries)
            if (e.rel == L"samelen.bin" && e.state == filetools::Diff::Differ) samelenIsDiffer = true;
        Check(samelenIsDiffer, "samelen.bin 判为 differ（不是同大小即相同）");
    }

    std::printf("\n比较：byContent=false（只比大小）\n");
    {
        filetools::CompareOptions opt;
        opt.walk.budgetMs = 20000;
        opt.byContent = false;
        filetools::CompareResult r;
        std::wstring err;
        Check(filetools::CompareFolders(root + L"\\a", root + L"\\b", opt, &r, &err), "比较成功");
        // 这一条证明开关不是装饰：同大小被当作相同，differ 归零、same 加一
        Check(r.sameCount == 2, "同大小被当作相同 → 2 项相同");
        Check(r.differCount == 0, "没有 differ");
    }

    std::printf("\n比较：路径不存在 / 不是目录\n");
    {
        filetools::CompareOptions opt;
        filetools::CompareResult r;
        std::wstring err;
        Check(!filetools::CompareFolders(root + L"\\nope", root + L"\\b", opt, &r, &err),
              "左不存在被拒绝");
        Check(!err.empty(), "有可读原因");
        Check(!filetools::CompareFolders(root + L"\\a\\dup1.bin", root + L"\\b", opt, &r, &err),
              "左侧是文件被拒绝");
    }

    std::printf("\n批量改名计划（纯函数）\n");
    {
        std::vector<filetools::RenamePair> plan;
        unsigned same = 0;
        std::wstring err;

        // 基本展开：{name}_{nn}，序号 1 起、补零到 2 位
        std::vector<std::wstring> old = {L"a.txt", L"b.jpg", L"c"};
        Check(filetools::PlanBatchRename(old, old, L"{name}_{nn}", &plan, &same, &err),
              "基本模式成功");
        Check(plan.size() == 3, "3 项");
        if (plan.size() == 3) {
            Check(plan[0].to == L"a_01.txt", "a.txt → a_01.txt（{ext} 自动带上）");
            Check(plan[1].to == L"b_02.jpg", "b.jpg → b_02.jpg");
            Check(plan[2].to == L"c_03", "无扩展名 → c_03");
        }

        // {old} 与 {ext} 的边界：多段扩展名按最后一个点拆
        Check(filetools::PlanBatchRename({L"a.tar.gz"}, {L"a.tar.gz"}, L"{name}{ext}_x", &plan,
                                         &same, &err),
              "多段扩展名成功");
        if (plan.size() == 1) Check(plan[0].to == L"a.tar.gz_x", "a.tar.gz → a.tar.gz_x");

        // 隐藏文件风格：开头的点不算扩展名分隔
        Check(filetools::PlanBatchRename({L".gitignore"}, {L".gitignore"}, L"{name}{ext}.bak", &plan,
                                         &same, &err),
              "点开头文件成功");
        if (plan.size() == 1) Check(plan[0].to == L".gitignore.bak", ".gitignore → .gitignore.bak");

        // 没有占位符 → 所有文件同名，必须早点拒绝
        Check(!filetools::PlanBatchRename(old, old, L"fixed", &plan, &same, &err),
              "无占位符被拒绝");
        Check(err.find(L"占位符") != std::wstring::npos, "原因指向占位符");

        // 批内重名（大小写不敏感）
        Check(!filetools::PlanBatchRename({L"a.txt", L"A.txt"}, {L"a.txt", L"A.txt"}, L"{name}_1",
                                          &plan, &same, &err),
              "批内同名被拒绝");

        // 撞到目录里现有、但不在本批的文件
        Check(!filetools::PlanBatchRename({L"a.txt"}, {L"a.txt", L"keep.txt"}, L"keep{ext}", &plan,
                                          &same, &err),
              "撞已有文件被拒绝");
        Check(err.find(L"已存在") != std::wstring::npos, "原因写明目标已存在");

        // 非法字符
        Check(!filetools::PlanBatchRename({L"a.txt"}, {L"a.txt"}, L"x:y{name}", &plan, &same, &err),
              "冒号被拒绝");
        // 末尾空格：只有"没有扩展名"的文件才可能真的把空格留在末尾
        Check(!filetools::PlanBatchRename({L"noext"}, {L"noext"}, L"{name} ", &plan, &same, &err),
              "末尾空格被拒绝");

        // 全部不变 → 不是成功（否则用户会以为改了）
        Check(!filetools::PlanBatchRename({L"a.txt"}, {L"a.txt"}, L"{old}", &plan, &same, &err),
              "全部不变被拒绝");
        if (same != 1) std::printf("        skippedSame=%u\n", same);
    }

    Cleanup(root);
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail;
}
