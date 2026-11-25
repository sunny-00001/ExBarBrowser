// 属性写入自测（file.attr.set / fs.attr.set 共用的纯逻辑层）。
//
// 锁住三条语义，防止"命令说成功但属性没变"：
//   1. 只改显式传了的位，没传的保持原值（bool 默认值不能当"要关掉"）；
//   2. 整批预检：任一条路径不存在就一条都不改；
//   3. 写后回读：目标位必须与期望一致。
//
// 本测试只操作临时目录里的自建文件，不碰用户文件、不发输入、不抢焦点。
#include "common.h"

#include <cstdio>
#include <string>

#include "file_attr.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

std::wstring TempDir() {
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf) + L"exbar_attr_test_" + std::to_wstring(GetCurrentProcessId());
}

void MakeFile(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f) {
        std::fputs("x", f);
        std::fclose(f);
    }
}

}  // namespace

int main() {
    // ---- ParseDelta ----
    {
        json::Value a = json::Value::MakeObject();
        a.Set(L"readonly", json::Value::MakeBool(true));
        a.Set(L"hidden", json::Value::MakeBool(false));
        fileattr::Delta d;
        Check(fileattr::ParseDelta(a, &d), "两个字段都能解析");
        Check(d.set == FILE_ATTRIBUTE_READONLY, "true → set 位");
        Check(d.clear == FILE_ATTRIBUTE_HIDDEN, "false → clear 位");
        Check((d.set & FILE_ATTRIBUTE_ARCHIVE) == 0, "未传的字段不出现在 set");
        Check((d.clear & FILE_ATTRIBUTE_ARCHIVE) == 0, "未传的字段不出现在 clear");
    }
    {
        json::Value empty = json::Value::MakeObject();
        fileattr::Delta d;
        Check(!fileattr::ParseDelta(empty, &d), "空对象被拒绝（没说要改什么）");
        json::Value bad = json::Value::MakeObject();
        bad.Set(L"readonly", json::Value::MakeInt(1));
        Check(!fileattr::ParseDelta(bad, &d), "非布尔值被拒绝（不猜意图）");
    }

    // ---- Apply ----
    const std::wstring dir = TempDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring a = dir + L"\\a.txt";
    const std::wstring b = dir + L"\\b.txt";
    MakeFile(a);
    MakeFile(b);

    json::Value attrs = json::Value::MakeObject();
    attrs.Set(L"readonly", json::Value::MakeBool(true));
    fileattr::Delta d;
    fileattr::ParseDelta(attrs, &d);

    std::vector<DWORD> bits;
    Check(fileattr::Apply({a, b}, d, &bits), "两条路径都写入成功");
    Check(bits.size() == 2, "回读结果数量一致");
    Check(bits.size() == 2 && (bits[0] & FILE_ATTRIBUTE_READONLY) != 0,
          "第一条只读位已置上");
    Check(bits.size() == 2 && (bits[1] & FILE_ATTRIBUTE_READONLY) != 0,
          "第二条只读位已置上");
    Check((GetFileAttributesW(a.c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
          "落盘后系统属性确实是只读");

    // 未传的位保持：先给 b 加隐藏位，再只改只读位，隐藏位不能丢。
    SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::vector<DWORD> bits2;
    Check(fileattr::Apply({b}, d, &bits2), "第二次写入成功");
    Check(!bits2.empty() && (bits2[0] & FILE_ATTRIBUTE_HIDDEN) != 0,
          "未传的隐藏位保持原值");
    Check(!bits2.empty() && (bits2[0] & FILE_ATTRIBUTE_READONLY) != 0,
          "只读位在隐藏文件上写成功");

    // 整批预检：一条不存在 → 已存在的那条也不能被改。
    SetFileAttributesW(a.c_str(), FILE_ATTRIBUTE_NORMAL);
    json::Value off = json::Value::MakeObject();
    off.Set(L"hidden", json::Value::MakeBool(true));
    fileattr::Delta dh;
    fileattr::ParseDelta(off, &dh);
    Check(!fileattr::Apply({a, dir + L"\\does_not_exist.txt"}, dh, nullptr),
          "批内有不存在的路径 → 整批失败");
    Check((GetFileAttributesW(a.c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0,
          "整批失败时已存在的那条没被部分修改");

    // 清理（只读位要先去掉，否则 DeleteFileW 会失败）。
    SetFileAttributesW(a.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(a.c_str());
    DeleteFileW(b.c_str());
    RemoveDirectoryW(dir.c_str());

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail;
}
