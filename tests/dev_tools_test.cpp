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

    std::printf("  通过 %d / 失败 %d\n", pass, fail);
    return fail ? 1 : 0;
}
