// 长路径支持自测：tf::ToExtended 的转换规则 + 真实深路径读写。
//
// 守的问题：本工程导航层早就支持超长路径，但执行层（CreateFileW /
// FindFirstFileW / GetFileAttributesExW）拿到 >260 字符会直接失败，
// 而调用方常把失败当成"文件不存在"或"权限不足"—— 深目录静默读不到、
// 体积统计算成 0 且仍标记 complete=true。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/longpath_test.cpp src/text_file.cpp src/str.cpp \
//     -o /tmp/longpath_test.exe -lole32 -luuid -lshell32
#include <windows.h>

#include <cstdio>
#include <string>

#include "text_file.h"

static int fails = 0;
#define CHECK(cond, msg)                  \
    do {                                  \
        if (cond) {                       \
            printf("ok   %s\n", msg);     \
        } else {                          \
            printf("FAIL %s\n", msg);     \
            ++fails;                      \
        }                                 \
    } while (0)

static void CheckEq(const std::wstring& got, const std::wstring& want, const char* what) {
    if (got == want) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n     got  = [%ls]\n     want = [%ls]\n", what, got.c_str(),
               want.c_str());
        ++fails;
    }
}

int main() {
    printf("转换规则\n");
    {
        CheckEq(tf::ToExtended(L"D:\\a\\b.txt"), L"\\\\?\\D:\\a\\b.txt", "盘符路径加前缀");
        CheckEq(tf::ToExtended(L"D:\\a"), L"\\\\?\\D:\\a", "盘符根的子项");
        CheckEq(tf::ToExtended(L"D:"), L"\\\\?\\D:", "裸盘符（size==2 边界）");
        // 幂等
        CheckEq(tf::ToExtended(L"\\\\?\\D:\\a"), L"\\\\?\\D:\\a", "已有 \\\\?\\ 前缀不再加");
        CheckEq(tf::ToExtended(L"\\\\.\\C:\\a"), L"\\\\.\\C:\\a", "设备前缀原样");
        // UNC
        CheckEq(tf::ToExtended(L"\\\\server\\share\\x"), L"\\\\?\\UNC\\server\\share\\x",
                "UNC 转 \\\\?\\UNC\\");
        // 正斜杠必须转反斜杠（扩展路径不做规范化）
        CheckEq(tf::ToExtended(L"D:/a/b"), L"\\\\?\\D:\\a\\b", "正斜杠转反斜杠");
        // 命名空间不能加前缀
        CheckEq(tf::ToExtended(L"shell:RecycleBinFolder"), L"shell:RecycleBinFolder",
                "shell: 命名空间原样");
        CheckEq(tf::ToExtended(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"),
                L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", "::{GUID} 原样");
        CheckEq(tf::ToExtended(L"search:query=x"), L"search:query=x", "search: 原样");
        // 相对路径
        CheckEq(tf::ToExtended(L"a\\b"), L"a\\b", "相对路径原样（扩展前缀要求绝对路径）");
        CheckEq(tf::ToExtended(L""), L"", "空串原样");
        CheckEq(tf::ToExtended(L"x"), L"x", "单字符原样");
    }

    printf("\n真实超长路径读写（> 260 字符）\n");
    {
        wchar_t tmp[MAX_PATH] = L"";
        GetTempPathW(MAX_PATH, tmp);
        std::wstring base = std::wstring(tmp) + L"ebb_longpath_test";

        // 拼一条总长 > 500 的路径：20 层 × 25 字符 + 文件名
        std::wstring deep = base;
        for (int i = 0; i < 20; ++i) {
            deep += L"\\";
            deep += L"dir";                 // 3
            deep += std::to_wstring(i);     // 1-2
            deep += L"_";
            deep += std::wstring(18, L'x'); // 补到约 25
        }
        const std::wstring file = deep + L"\\deep_file.txt";
        printf("     路径长度 = %zu\n", file.size());

        // 逐层创建（CreateDirectoryW 也需要扩展前缀）
        std::wstring cur = base;
        CreateDirectoryW(tf::ToExtended(cur).c_str(), nullptr);
        size_t pos = base.size();
        while (pos < deep.size()) {
            const size_t next = deep.find(L'\\', pos + 1);
            cur = (next == std::wstring::npos) ? deep : deep.substr(0, next);
            CreateDirectoryW(tf::ToExtended(cur).c_str(), nullptr);
            if (next == std::wstring::npos) break;
            pos = next;
        }

        // 用统一的 WriteText/ReadText 读写 —— 它们内部走 ToExtended
        const bool wrote = tf::WriteText(file, L"深路径内容 OK");
        CHECK(wrote, "WriteText 写入超长路径成功");
        if (wrote) {
            std::wstring got;
            CHECK(tf::ReadText(file, &got), "ReadText 读回超长路径成功");
            CHECK(got == L"深路径内容 OK", "内容正确");
            CHECK(tf::Exists(file), "Exists 认得超长路径");
        } else {
            printf("     注意：写入失败可能是本机未启用长路径支持（见文件头说明）\n");
        }

        // 清理（自底向上删）
        DeleteFileW(tf::ToExtended(file).c_str());
        std::wstring del = deep;
        while (del.size() >= base.size()) {
            RemoveDirectoryW(tf::ToExtended(del).c_str());
            if (del == base) break;
            const size_t slash = del.find_last_of(L'\\');
            if (slash == std::wstring::npos || slash < base.size()) break;
            del = del.substr(0, slash);
        }
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
