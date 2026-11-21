// 导航模型显示自测：命名空间（此电脑/回收站）的面包屑与地址文本。
//
// 钉死的行为：SetPath("::{GUID}", "友好名") 后面包屑显示友好名，而且绝不能
// 退化成 "::" —— 那正是导航到「此电脑」「回收站」时地址栏显示 ">::>" 的根因。
//
// 编译（在 ExBarBrowser/ 下）：
//   "/c/Program Files/Windhawk/Compiler/bin/clang++.exe" -std=c++20 -O1 \
//     -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 \
//     -Isrc -static tests/nav_model_test.cpp src/nav_model.cpp \
//     -o /tmp/nav_model_test.exe -lole32 -luuid -lshell32 -lshlwapi
//   /tmp/nav_model_test.exe
#include <cstdio>
#include <string>

#include "nav_model.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void Eq(const std::wstring& got, const std::wstring& want, const char* what) {
    const bool ok = got == want;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) std::printf("        期望 \"%ls\"，得到 \"%ls\"\n", want.c_str(), got.c_str());
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    // 此电脑：友好名同时用于地址文本和面包屑标签
    {
        NavModel m;
        m.SetPath(nav::kThisPc, L"ThisPC");
        Check(m.parts().size() == 1, "此电脑只有一段面包屑");
        Eq(m.AddressText(), L"ThisPC", "地址文本显示友好名，不是裸 GUID");
        Eq(m.parts()[0].label, L"ThisPC", "面包屑标签显示友好名");
        Eq(m.parts()[0].path, nav::kThisPc, "分段路径仍是解析串（点击可导航）");
        Check(m.parts()[0].label != L"::", "绝不能退化成两个冒号");
    }

    // 回收站：同上，确认不是只对这一个 GUID 特判
    {
        const std::wstring bin = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
        NavModel m;
        m.SetPath(bin, L"RecycleBin");
        Eq(m.AddressText(), L"RecycleBin", "回收站地址文本是友好名");
        Eq(m.parts()[0].label, L"RecycleBin", "回收站面包屑标签是友好名");
        Eq(m.parts()[0].path, bin, "回收站分段路径是解析串");
    }

    // 友好名还没到（Shell 回调前）时退回完整解析串，至少不是空标签 / "::"
    {
        NavModel m;
        m.SetPath(nav::kThisPc, L"");
        Eq(m.parts()[0].label, nav::kThisPc, "无友好名时退回完整解析串");
        Eq(m.AddressText(), nav::kThisPc, "无友好名时地址文本退回解析串");
    }

    // 普通盘符路径不受影响：首段仍是 "C:"，地址文本仍是真实路径
    {
        NavModel m;
        m.SetPath(L"C:\\Windows\\System32", L"System32");
        Eq(m.parts()[0].label, L"C:", "盘符首段仍是 C:");
        Eq(m.parts()[1].label, L"Windows", "中间段是末级名");
        Eq(m.AddressText(), L"C:\\Windows\\System32", "文件系统地址文本是真实路径");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
