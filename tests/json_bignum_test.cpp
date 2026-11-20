// JSON 大整数自测（审查中发现的溢出 bug 的回归测试）。
//
// 背景：OpListDir / OpStat 早先把 unsigned long long 的文件大小强转 int，
// 超过 2 GB 的文件会输出成负数（如 3 GB -> -1073741824）。
// 这类错误在 JSON 里看起来"是个数字"，客户端不会察觉，比崩溃更难发现。
#include "common.h"

#include <cstdio>

#include "json.h"

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    std::printf("大整数序列化\n");
    {
        // 3 GB
        const double threeGB = 3.0 * 1024 * 1024 * 1024;
        json::Value v = json::Value::MakeNum(threeGB);
        const std::wstring s = json::Stringify(v, 0);
        // 必须输出完整整数，不能是 3.22123e+09 也不该是负数
        Check(s == L"3221225472", "3 GB 输出为完整整数");
        if (s != L"3221225472") std::printf("        实际：%hs\n", "");

        // 回到 double 也应精确（2^53 以内）
        json::Value back;
        std::wstring e;
        if (json::Parse(L"3221225472", back, &e))
            Check(back.num == threeGB, "解析回来数值一致");
    }

    std::printf("\n超过 2 GB 不是负数\n");
    {
        const double justOver = 2147483648.0;   // 2^31，int 的正上界 +1
        json::Value v = json::Value::MakeNum(justOver);
        const std::wstring s = json::Stringify(v, 0);
        Check(s == L"2147483648", "2^31 输出正确");
        Check(s.find(L'-') == std::wstring::npos, "没有负号");
    }

    std::printf("\n小数字仍正常（回归）\n");
    {
        Check(json::Stringify(json::Value::MakeInt(0), 0) == L"0", "0 正常");
        Check(json::Stringify(json::Value::MakeInt(42), 0) == L"42", "42 正常");
        Check(json::Stringify(json::Value::MakeInt(-7), 0) == L"-7", "负数正常");
        // 非整数仍用 %g
        const std::wstring half = json::Stringify(json::Value::MakeNum(0.5), 0);
        Check(half == L"0.5", "0.5 正常");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
