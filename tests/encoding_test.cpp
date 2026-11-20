// 字符串与编码边界自测：str::FromUtf8Strict / tf::ReadText 的 BOM 分派。
//
// 这些断言守的是"静默数据损坏"这一类问题 —— 它比崩溃更难发现：
// 文件读进来没有报错，只是中文全变成 "????"，下一次保存就把损坏结果写回去。
// 2026-11-20 审查发现 lenient 解码正是这样把 GBK 书签解成 4 个 U+FFFD。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/encoding_test.cpp src/str.cpp src/text_file.cpp \
//     -o /tmp/encoding_test.exe -lole32 -luuid -lshell32
#include <windows.h>

#include <cstdio>
#include <string>

#include "str.h"
#include "text_file.h"

static int fails = 0;
#define CHECK(cond, msg)      \
    do {                      \
        if (cond) {           \
            printf("ok   %s\n", msg); \
        } else {              \
            printf("FAIL %s\n", msg); \
            ++fails;          \
        }                     \
    } while (0)

// 写原始字节（绕开 WriteText 的 UTF-8 编码，模拟"外部程序留下的文件"）
static void WriteRaw(const std::wstring& path, const char* data, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
}

int main() {
    printf("严格解码：非法 UTF-8 必须失败而非静默替换\n");
    {
        // GBK 的"中文" = D6 D0 CE C4，不是合法 UTF-8
        const char gbk[] = "\xD6\xD0\xCE\xC4";
        std::wstring out;
        const bool ok =
            str::FromUtf8Strict(std::string(gbk, sizeof(gbk) - 1), &out);
        CHECK(!ok, "GBK 字节 -> 严格解码失败（不是替换成 U+FFFD）");
        CHECK(out.empty(), "失败时输出保持为空");

        // 对照：宽松版会静默产出 4 个 U+FFFD —— 这正是被修掉的危害
        const std::wstring lenient = str::FromUtf8(gbk);
        bool allReplacement = !lenient.empty();
        for (wchar_t c : lenient)
            if (c != 0xFFFD) allReplacement = false;
        CHECK(allReplacement, "（对照）宽松版静默产出 U+FFFD，故读文件不能用它");
    }

    printf("\n严格解码：合法 UTF-8 正常通过\n");
    {
        std::wstring out;
        // "中文" 的 UTF-8 = E4 B8 AD E6 96 87
        CHECK(str::FromUtf8Strict("\xE4\xB8\xAD\xE6\x96\x87", &out), "合法 UTF-8 解码成功");
        CHECK(out == L"中文", "内容正确");
        CHECK(str::FromUtf8Strict("", &out) && out.empty(), "空串成功且为空");
        // ASCII
        CHECK(str::FromUtf8Strict("abc", &out) && out == L"abc", "ASCII 正常");
        // 4 字节（emoji 😀 = F0 9F 98 80），验证代理对不被破坏
        CHECK(str::FromUtf8Strict("\xF0\x9F\x98\x80", &out) && out.size() == 2,
              "emoji 解成 2 个 wchar_t（代理对）");
    }

    printf("\nReadText：BOM 分派与失败语义\n");
    {
        const std::wstring dir = L"\\\\.\\pipe\\";  // 占位，实际用临时文件
        wchar_t tmpPath[MAX_PATH] = L"";
        GetTempPathW(MAX_PATH, tmpPath);
        const std::wstring f = std::wstring(tmpPath) + L"ebb_enc_test.txt";
        (void)dir;

        std::wstring out;

        // 1. 合法 UTF-8（无 BOM）
        WriteRaw(f, "\xE4\xB8\xAD\xE6\x96\x87", 6);
        CHECK(tf::ReadText(f, &out) && out == L"中文", "UTF-8 无 BOM 读出 中文");

        // 2. UTF-8 带 BOM
        WriteRaw(f, "\xEF\xBB\xBF\xE4\xB8\xAD\xE6\x96\x87", 9);
        CHECK(tf::ReadText(f, &out) && out == L"中文", "UTF-8 BOM 被跳过");

        // 3. UTF-16LE 带 BOM："中" = 2D 4E
        WriteRaw(f, "\xFF\xFE\x2D\x4E\x87\x65", 6);
        CHECK(tf::ReadText(f, &out) && out == L"中文", "UTF-16LE BOM 正确");

        // 4. UTF-16BE 带 BOM：以前会掉进 UTF-8 分支变乱码
        WriteRaw(f, "\xFE\xFF\x4E\x2D\x65\x87", 6);
        CHECK(tf::ReadText(f, &out) && out == L"中文", "UTF-16BE BOM 正确（新增）");

        // 5. 非法 UTF-8：必须返回 false（配合 BookmarkStore 的护栏拒绝写盘）
        const char gbk[] = "\xD6\xD0\xCE\xC4";
        WriteRaw(f, gbk, sizeof(gbk) - 1);
        CHECK(!tf::ReadText(f, &out), "GBK 文件读失败而非静默损坏");
        CHECK(out.empty(), "失败时不交出半截数据");

        // 6. 不存在 → false
        DeleteFileW(f.c_str());
        CHECK(!tf::ReadText(f, &out), "文件不存在返回 false");
    }

    printf("\n往返：ToUtf8 -> FromUtf8Strict 不丢字符\n");
    {
        const std::wstring cases[] = {L"", L"abc", L"中文路径\\子目录",
                                      L"emoji 😀 与符号 ←→", L"a\tb\nc"};
        bool all = true;
        for (const auto& c : cases) {
            std::wstring back;
            if (!str::FromUtf8Strict(str::ToUtf8(c), &back) || back != c) all = false;
        }
        CHECK(all, "五种样本往返一致（含中文/emoji/控制字符）");
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
