// 详情列 id 的纯函数自测：不碰 COM/窗口，只验证稳定文本形式可往返。
#include <cstdio>

#include "shell_columns.h"   // 先经 common.h 拿到 PROPERTYKEY/EXTERN_C
#include <propkey.h>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

int main() {
    const PROPERTYKEY in = PKEY_Size;
    const std::wstring id = shellcolumns::ColumnIdToString(in);
    Check(!id.empty(), "列 id 非空");
    Check(id.find(L'{') == 0 && id.rfind(L'}') != std::wstring::npos, "列 id 含完整 GUID");

    PROPERTYKEY out{};
    Check(shellcolumns::ColumnIdFromString(id, &out), "合法 id 可解析");
    Check(IsEqualPropertyKey(in, out), "往返后 PROPERTYKEY 不变");

    Check(!shellcolumns::ColumnIdFromString(L"garbage", &out), "垃圾串被拒绝");
    Check(!shellcolumns::ColumnIdFromString(L"{00000000-0000-0000-0000-000000000000}:abc", &out),
          "非数字 pid 被拒绝");

    std::printf(g_fail ? "有失败（失败 %d 项）\n" : "全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
