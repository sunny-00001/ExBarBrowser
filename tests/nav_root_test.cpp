// 导航树根集自测：配置文本 <-> RootSpec 的解析与互逆。
//
// 钉死的行为（都是曾经会静默出错、错得还不明显的那几条）：
//   · "known:" 名字拼错必须判无效，不能静默退回某个默认目录；
//   · clsid 的花括号可有可无，但 Format 输出不带花括号（配置里不写）；
//   · Parse/Format 必须互逆 —— 否则"读进来再写回去"会把配置改坏；
//   · path: 带前导/尾随空白要去掉（手改配置最常见）。
//
// 本测试纯字符串，不需要窗口/消息循环，不碰 COM 的 IShellItem 路径。
#include <cstdio>
#include <string>

#include "nav_pane_root.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("nav_root_test\n");

    // 1) known: 合法名
    {
        navpane::RootSpec s;
        Check(navpane::RootSpec::Parse(L"known:ComputerFolder", &s), "known:ComputerFolder 可解析");
        Check(s.kind == navpane::RootSpec::Kind::Known, "kind 应为 Known");
        Check(s.value == L"ComputerFolder", "value 保留原名");
        Check(navpane::RootSpec::Format(s) == L"known:ComputerFolder", "Format 与 Parse 互逆");
    }

    // 2) known: 拼错必须判无效（不能静默变成"桌面"）
    {
        navpane::RootSpec s;
        Check(!navpane::RootSpec::Parse(L"known:ComputerFolderr", &s), "拼错的 known 名必须判无效");
        Check(!navpane::RootSpec::Parse(L"known:", &s), "known: 空值必须判无效");
    }

    // 3) clsid：带花括号与不带花括号都应解析成同一种写法
    {
        navpane::RootSpec a;
        navpane::RootSpec b;
        const bool okA = navpane::RootSpec::Parse(L"clsid:679F85CB-0220-4080-B29B-5540CC05AAB6", &a);
        const bool okB = navpane::RootSpec::Parse(L"clsid:{679F85CB-0220-4080-B29B-5540CC05AAB6}", &b);
        Check(okA && okB, "clsid 带/不带花括号都能解析");
        Check(a.value == b.value, "两种写法的 value 归一化成同一个");
        Check(a.value.find(L'{') == std::wstring::npos, "value 不带花括号（配置约定）");
        Check(navpane::RootSpec::Format(a) == L"clsid:679F85CB-0220-4080-B29B-5540CC05AAB6",
              "clsid 的 Format 输出无花括号");
    }

    // 4) clsid 非 GUID 必须判无效
    {
        navpane::RootSpec s;
        Check(!navpane::RootSpec::Parse(L"clsid:not-a-guid", &s), "非法 GUID 必须判无效");
    }

    // 5) path: 去两端空白
    {
        navpane::RootSpec s;
        Check(navpane::RootSpec::Parse(L"path:  C:\\Windows \t", &s), "path 带空白可解析");
        Check(s.kind == navpane::RootSpec::Kind::Path, "kind 应为 Path");
        Check(s.value == L"C:\\Windows", "两端空白已去掉");
        Check(navpane::RootSpec::Format(s) == L"path:C:\\Windows", "path 的 Format 互逆");
    }

    // 6) 无冒号 / 未知种类 必须判无效
    {
        navpane::RootSpec s;
        Check(!navpane::RootSpec::Parse(L"ComputerFolder", &s), "无冒号必须判无效");
        Check(!navpane::RootSpec::Parse(L"bogus:xyz", &s), "未知种类必须判无效");
        Check(!navpane::RootSpec::Parse(L"", &s), "空串必须判无效");
    }

    // 7) Defaults() 的不变式 —— **不**断言具体条数。
    //
    // 【为什么改】Defaults() 现在是两阶段的（见 nav_pane_root.h 的 Prefetch*）：
    //   · 发现还没算完 -> 内置兜底三项（主文件夹/图库/网络）
    //   · 发现算完了   -> 完整清单（本机 27 项，随机器变化）
    // 断言"恰好 4 个根"在两种状态下都会假报 —— 而它本来钉的也不是契约，
    // 只是当时那份硬编码清单的实现细节。真正该钉的是这三条：
    //   a. 永不为空（树不能因为发现失败整棵空掉）
    //   b. 每项的 kind/value 自洽（能过 Format -> Parse 往返）
    //   c. 主文件夹是 CLSID 形式（它是 shell 命名空间，不是已知文件夹）
    {
        const auto d = navpane::RootSpec::Defaults();
        Check(!d.empty(), "Defaults() 永不为空（发现失败也要有兜底）");

        bool allRoundTrip = true;
        for (const auto& s : d) {
            navpane::RootSpec back;
            if (!s.value.empty() && !navpane::RootSpec::Parse(navpane::RootSpec::Format(s), &back))
                allRoundTrip = false;
        }
        Check(allRoundTrip, "每一项都能 Format -> Parse 往返（配置可写回）");

        // 兜底三项与完整清单都以"主文件夹"开头（组0 第一项）。
        Check(d[0].kind == navpane::RootSpec::Kind::Clsid, "第 1 个是 主文件夹（CLSID）");
        Check(d[0].value == L"679F85CB-0220-4080-B29B-5540CC05AAB6",
              "第 1 个的 CLSID 是 主文件夹");
    }

    // 8) IsKnownFolderName 与 Parse 的判据一致（同一条实现的对外出口）
    {
        Check(navpane::IsKnownFolderName(L"Desktop"), "Desktop 是已知文件夹");
        Check(!navpane::IsKnownFolderName(L"Desktop2"), "Desktop2 不是已知文件夹");
    }

    // 9) NormalizeParsingName：补 shell: 前缀 —— 这是"主文件夹打不开"的修复。
    //    实机症状：点主页报 "无法打开 ::{679F85CB-...}"，
    //    日志 [nav] parse '::{679F85CB-...}' hr=0x80070057（E_INVALIDARG）。
    {
        const std::wstring home = L"::{679F85CB-0220-4080-B29B-5540CC05AAB6}";
        const std::wstring got = navpane::NormalizeParsingName(home);
        Check(got == L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",
              "裸 ::{GUID} 补上 shell: 前缀（主页只认这种写法）");

        // 文件系统路径绝不能被套前缀 —— 套了就变成"打开 shell:C:\..."，全线失败。
        Check(navpane::NormalizeParsingName(L"C:\\Windows") == L"C:\\Windows",
              "文件系统路径原样返回");
        Check(navpane::NormalizeParsingName(L"D:\\") == L"D:\\", "盘根原样返回");

        // 已带前缀的不能被重复加：否则会变成 shell::shell:::{...}
        const std::wstring already = L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
        Check(navpane::NormalizeParsingName(already) == already,
              "已带 shell: 前缀的不重复加");

        // 幂等：归一化两次结果不变（防止将来在上游再套一层）
        Check(navpane::NormalizeParsingName(got) == got, "归一化是幂等的");

        Check(navpane::NormalizeParsingName(L"").empty(), "空串原样返回空");
    }

    std::printf("%s\n", g_fail ? "有失败" : "全部通过");
    return g_fail ? 1 : 0;
}