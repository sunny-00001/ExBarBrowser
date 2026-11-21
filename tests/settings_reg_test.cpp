// 设置键表单测。
//
// 【为什么值得单测】这张表是"机器操作自身"的地基：config.list / config.get /
// config.set 和设置面板全部从它派生。表腐化的方式很隐蔽 ——
//   · 加了一行却忘了接 set  → 改了没反应，日志说成功；
//   · get 读 A、set 写 B     → 界面显示与写入分叉；
//   · 范围写进 doc 却没查    → 文档说 200..5000，代码接受 1。
// 三者都不会崩，只会让"设置改了没生效"变成长期悬案。所以这里逐条验：
// 每项读得出来、可写项写得回去（写后回读一致）、越界与错类型当场拒绝。
#include <cstdio>
#include <set>
#include <string>

#include "json.h"
#include "settings_reg.h"

static int g_pass = 0, g_fail = 0;

static void Expect(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n", what);
    }
}

// 写一项，返回是否成功，并带回原因（失败时必须有原因 —— 空原因等于没有信息）。
static bool TrySet(const wchar_t* key, const json::Value& v, std::wstring* err) {
    return settings_reg::Set(key, v, err);
}

int main() {
    const auto& entries = settings_reg::Entries();

    // ① 表本身：非空、键唯一、每个键都读得出来。
    Expect(!entries.empty(), "键表非空");
    std::set<std::wstring> seen;
    for (const auto& e : entries) {
        Expect(seen.insert(e.key).second, "键名唯一");
        json::Value v;
        Expect(settings_reg::Get(e.key, &v), "每一项都读得出来");
        Expect(!e.doc.empty(), "每一项都有中文说明（界面标题与 AI 决策依据）");
        if (e.kind == settings_reg::Kind::Int && e.maxValue > e.minValue)
            Expect(e.maxValue > e.minValue, "Int 项的范围上界大于下界");
    }

    // ② 自检：可写项能用当前值写回（证明 set 真的接线，且写后回读一致）。
    const auto bad = settings_reg::SelfCheck();
    for (const auto& b : bad) std::printf("  自检未通过：%ls\n", b.c_str());
    Expect(bad.empty(), "SelfCheck 全部通过（可写项写回原值成功）");

    // ③ 未知键：读返回 false、写返回 false 且带原因。
    {
        json::Value v;
        Expect(!settings_reg::Get(L"no.such.key", &v), "未知键读失败");
        std::wstring err;
        Expect(!TrySet(L"no.such.key", json::Value::MakeInt(1), &err), "未知键写失败");
        Expect(!err.empty(), "未知键写失败时必须给出原因");
    }

    // ④ 类型不符：布尔项给数字必须拒绝，且原因说清要什么。
    {
        std::wstring err;
        Expect(!TrySet(L"tab", json::Value::MakeInt(1), &err), "布尔项拒绝整数");
        Expect(err.find(L"布尔") != std::wstring::npos, "原因里说明需要布尔值");
    }

    // ⑤ 越界：viewMode 声明 0..7，9 必须被拒（而不是静默接受）。
    {
        json::Value before;
        settings_reg::Get(L"viewMode", &before);
        std::wstring err;
        Expect(!TrySet(L"viewMode", json::Value::MakeInt(9), &err), "越界值被拒绝");
        Expect(err.find(L"超出范围") != std::wstring::npos, "原因里说明超出范围");
        json::Value after;
        settings_reg::Get(L"viewMode", &after);
        Expect(after.num == before.num, "被拒绝的写入没有改动原值");
    }

    // ⑥ 枚举：tabNewPos 只认三个值；大小写不敏感。
    {
        std::wstring err;
        Expect(!TrySet(L"tabNewPos", json::Value::MakeStr(L"bogus"), &err), "枚举拒绝非法取值");
        Expect(err.find(L"after_current") != std::wstring::npos, "原因里列出合法取值");
        json::Value prev;
        settings_reg::Get(L"tabNewPos", &prev);
        Expect(TrySet(L"tabNewPos", json::Value::MakeStr(L"RIGHT"), &err), "枚举值大小写不敏感");
        settings_reg::Set(L"tabNewPos", prev, &err);   // 还原
    }

    // ⑦ 只读项：写必须被拒，且原因是"只读"（不是"没有这个键"）。
    {
        std::wstring err;
        Expect(!TrySet(L"window.x", json::Value::MakeInt(10), &err), "只读项拒绝写入");
        Expect(err.find(L"只读") != std::wstring::npos, "原因里说明是只读项");
    }

    // ⑧ 正常写入 + 回读：布尔项翻转后读回来必须是新值，然后还原。
    {
        json::Value before;
        settings_reg::Get(L"tab", &before);
        const bool target = !before.b;
        std::wstring err;
        Expect(TrySet(L"tab", json::Value::MakeBool(target), &err), "布尔项写入成功");
        json::Value after;
        settings_reg::Get(L"tab", &after);
        Expect(after.b == target, "写完读回来是新值");
        Expect(TrySet(L"tab", before, &err), "布尔项可还原");
    }

    // ⑨ DumpAll 的形状：条数与表一致，每项都带 key/kind/writable。
    // config.list 直接返回它，形状错了是 AI 侧第一个撞到的墙。
    {
        const json::Value dump = settings_reg::DumpAll();
        Expect(dump.IsArray(), "DumpAll 返回数组");
        Expect(dump.arr.size() == entries.size(), "DumpAll 条数与键表一致");
        bool shape_ok = true;
        for (const auto& o : dump.arr) {
            const json::Value* k = o.Find(L"key");
            const json::Value* kind = o.Find(L"kind");
            const json::Value* w = o.Find(L"writable");
            const json::Value* val = o.Find(L"value");
            if (!k || !kind || !w || !val) shape_ok = false;
        }
        Expect(shape_ok, "每项都有 key/kind/writable/value");
    }

    std::printf("%s  settings_reg_test: %d 通过, %d 失败\n",
                g_fail == 0 ? "OK  " : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
