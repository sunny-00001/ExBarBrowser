#include "file_attr.h"

#include "log.h"
#include "text_file.h"  // tf::ToExtended

namespace fileattr {

bool ParseDelta(const json::Value& attrs, Delta* out) {
    if (!out || !attrs.IsObject() || attrs.obj.empty()) return false;
    static const struct {
        const wchar_t* key;
        DWORD bit;
    } kFields[] = {
        {L"readonly", FILE_ATTRIBUTE_READONLY},
        {L"hidden", FILE_ATTRIBUTE_HIDDEN},
        {L"system", FILE_ATTRIBUTE_SYSTEM},
        {L"archive", FILE_ATTRIBUTE_ARCHIVE},
    };
    Delta d;
    for (const auto& f : kFields) {
        const json::Value* v = attrs.Find(f.key);
        if (!v) continue;
        if (!v->IsBool()) return false;
        if (v->b) d.set |= f.bit;
        else d.clear |= f.bit;
    }
    if (d.Empty()) return false;
    *out = d;
    return true;
}

bool Apply(const std::vector<std::wstring>& paths, const Delta& d,
           std::vector<DWORD>* out) {
    if (paths.empty() || d.Empty()) return false;

    // 阶段 1：全部预检。先读后写，保证"要么全改，要么一个都不动"。
    std::vector<DWORD> before;
    before.reserve(paths.size());
    for (const auto& p : paths) {
        const DWORD a = GetFileAttributesW(tf::ToExtended(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            LogDbg(L"[attr] 预检失败：%s", p.c_str());
            return false;
        }
        before.push_back(a);
    }

    // 阶段 2：写入 + 回读。回读不是形式主义：写隐藏/系统位时 shell 与
    // 杀软可能改回去，不校验就会出现"命令说成功、属性没变"。
    std::vector<DWORD> after;
    after.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        const DWORD want = (before[i] | d.set) & ~d.clear;
        const DWORD wantWritable = want & kWritableBits;
        if (!SetFileAttributesW(tf::ToExtended(paths[i]).c_str(), want)) {
            LogDbg(L"[attr] 写入失败：%s（0x%08X）", paths[i].c_str(), GetLastError());
            return false;
        }
        const DWORD got = GetFileAttributesW(tf::ToExtended(paths[i]).c_str());
        if (got == INVALID_FILE_ATTRIBUTES || (got & kWritableBits) != wantWritable) {
            LogDbg(L"[attr] 回读不一致：%s want=0x%08X got=0x%08X",
                   paths[i].c_str(), wantWritable, got);
            return false;
        }
        after.push_back(got);
    }
    if (out) *out = std::move(after);
    return true;
}

}  // namespace fileattr
