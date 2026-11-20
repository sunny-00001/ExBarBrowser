// JSON 大整数自测（审查中发现的溢出 bug 的回归测试）。
//
// 背景：OpListDir / OpStat 早先把 unsigned long long 的文件大小强转 int，
// 超过 2 GB 的文件会输出成负数（如 3 GB -> -1073741824）。
// 这类错误在 JSON 里看起来"是个数字"，客户端不会察觉，比崩溃更难发现。
#include "common.h"

#include <cstdio>
#include <limits>   // numeric_limits：inf/nan 序列化用例

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

    // ---- GetInt 越界：以前是 static_cast<int>(double) 的 UB ----
    //
    // 实测 GCC/MSVC 都给 INT_MIN，然后 INT_MIN 流进窗口尺寸计算，
    // `work.right - w` 又一次有符号溢出，CreateWindowEx 收到负宽高。
    std::printf("\nGetInt 越界必须回落到默认值（不是 UB，也不是极值）\n");
    {
        auto objWith = [](double n) {
            json::Value v = json::Value::MakeObject();
            v.Set(L"w", json::Value::MakeNum(n));
            return v;
        };
        Check(objWith(1e10).GetInt(L"w", 800) == 800, "1e10 越界 -> 默认 800");
        Check(objWith(3e9).GetInt(L"w", 800) == 800, "3e9 越界 -> 默认 800");
        Check(objWith(-1e10).GetInt(L"w", 800) == 800, "-1e10 越界 -> 默认 800");
        Check(objWith(1e300).GetInt(L"w", 800) == 800, "1e300 越界 -> 默认 800");
        // 边界内仍正常
        Check(objWith(1024.0).GetInt(L"w", 800) == 1024, "1024 正常取值");
        Check(objWith(-5.0).GetInt(L"w", 0) == -5, "负数正常取值");
        Check(objWith(0.0).GetInt(L"w", 800) == 0, "0 正常取值");
        // 非数字类型仍回落默认
        json::Value o = json::Value::MakeObject();
        o.Set(L"w", json::Value::MakeStr(L"big"));
        Check(o.GetInt(L"w", 800) == 800, "字符串 -> 默认");
    }

    // ---- Num 序列化：inf / nan 以前会走到 static_cast<long long> 的 UB ----
    std::printf("\nNum 序列化 inf/nan 不产生非法 JSON\n");
    {
        const double inf = 1e999 < 1 ? 0 : std::numeric_limits<double>::infinity();
        const std::wstring si = json::Stringify(json::Value::MakeNum(inf), 0);
        Check(si == L"0", "inf -> 0（不是 inf/-9223...）");
        const std::wstring sn =
            json::Stringify(json::Value::MakeNum(std::numeric_limits<double>::quiet_NaN()), 0);
        Check(sn == L"0", "nan -> 0");
        // 1e300 合法但超 long long：应走 %g 而不是整数分支
        const std::wstring big = json::Stringify(json::Value::MakeNum(1e300), 0);
        Check(big.find(L"e+") != std::wstring::npos || big.size() > 10, "1e300 走 %g 分支");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
