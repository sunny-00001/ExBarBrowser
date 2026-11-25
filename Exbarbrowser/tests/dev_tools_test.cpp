// 开发面单测：只测**判定与形状**，不真的编译/回滚工作树。
// 真编译/测试走 E2E（dev.build/dev.test + dev.job.done 事件）。
#include <cstdio>
#include <string>

#include "dev_tools.h"
#include "json.h"

static int pass = 0, fail = 0;
static void Expect(bool c, const char* what) {
    if (c) ++pass;
    else { ++fail; std::printf("  FAIL  %s\n", what); }
}

int main() {
    std::printf("开发面工具\n");
    std::wstring err;
    Expect(devtools::ValidRepoPath(L"src/commands.cpp", &err), "普通相对路径合法");
    Expect(devtools::ValidRepoPath(L"tests\\profile_isolation_test.cpp", &err), "反斜杠也合法");
    Expect(!devtools::ValidRepoPath(L"", &err) && err.find(L"空") != std::wstring::npos,
           "空路径被拒且说明原因");
    Expect(!devtools::ValidRepoPath(L"C:/Windows/system32/x", &err), "绝对路径被拒");
    Expect(!devtools::ValidRepoPath(L"../secret", &err), ".. 被拒");
    Expect(!devtools::ValidRepoPath(L"src/a & del x", &err), "命令分隔符被拒");
    Expect(!devtools::ValidRepoPath(L"src/a\"b", &err), "引号被拒");

    devtools::Job job;
    Expect(!devtools::GetJob(L"job-does-not-exist", &job), "未知作业查不到，不编造结果");

    json::Value snap;
    json::Parse(devtools::Snapshot(), snap, &err);
    Expect(snap.IsObject(), "快照是合法 JSON 对象");
    Expect(snap.Find(L"head") && snap.Find(L"status") && snap.Find(L"profile"),
           "快照含 HEAD / 脏文件 / profile（闭环的起点）");

    // P0-9：改写可见性。dev.diff 是只读观测，必须返回可核对的结构化数据。
    //
    // 【这里测的是"不编造"】测试 exe 所在目录通常不是 git 仓库，
    // 所以更该断言的是：**失败时不返回一份看起来像"没有改动"的空 diff** ——
    // 那是最坏的一种假绿（审阅者会以为"确实没改动"）。
    // 内容级的正确性由 tests/dev_diff_test.cpp 在真仓库里测。
    json::Value diff;
    std::wstring derr;
    if (devtools::Diff(L"", &diff, &derr)) {
        Expect(diff.IsObject() && diff.Find(L"head"), "dev.diff 返回合法 JSON 且带 head");
        const json::Value* files = diff.Find(L"files");
        Expect(files && files->IsArray(), "dev.diff 含 files[]（改了哪些文件）");
        Expect(diff.Find(L"truncated"), "dev.diff 带整体截断标记（审阅视图不谎报完整）");
        if (files && files->IsArray()) {
            bool shapeOk = true;
            for (const auto& f : files->arr) {
                if (!f.Find(L"path") || !f.Find(L"status") || !f.Find(L"mtime") ||
                    !f.Find(L"sha256") || !f.Find(L"added") || !f.Find(L"deleted") ||
                    !f.Find(L"hunks")) {
                    shapeOk = false;
                    break;
                }
            }
            Expect(shapeOk,
                   "dev.diff 每个文件都带 path/status/mtime/sha256/added/deleted/hunks");
        }
    } else {
        Expect(!derr.empty(), "dev.diff 失败时给出原因（不编造空 diff）");
        Expect(!diff.Find(L"files"), "dev.diff 失败时不返回一份像\"没有改动\"的结果");
    }

    std::printf("  通过 %d / 失败 %d\n", pass, fail);
    return fail ? 1 : 0;
}
