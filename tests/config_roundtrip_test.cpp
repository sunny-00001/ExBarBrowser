// 配置**回环**单测：config.set 接受的值，存盘再读盘之后必须还是它。
//
// 【为什么值得单独一个测试】"设置了、重启又变回去"是这一层最难查的错：
// 写入端（settings_reg）与读盘端（config_io）各自有一套边界与归一规则，
// 两边差一点点就会把用户刚设好的值悄悄改掉 —— 而**没有任何检查会报**，
// 存盘成功、读盘成功、日志干净，只有用户发现设置不听话。
//
// 实证过的两个例子（都在这里被钉住）：
//   · holdDelayMs：settings_reg 声明 200..2000，config_io 写的是"< 300 就当 800"。
//     `config.set holdDelayMs 250` 成功并写进 settings.json，下次启动变成 800。
//   · tabMaxWidth：以前只在读盘端夹"不得低于 tabMinWidth"，写入端不查，
//     于是能写进一个自相矛盾的对（min 230 / max 100），重启才被"修好"。
//
// 【判据为什么是"逐项边界值"而不是"随手一个值"】夹取类错误只在边界附近暴露：
// 800 在两边都合法，抄错上下限时它照样能过。真正的探针是最小值与最大值本身。
//
// 【R21】只读写 profile 目录里的文件，不碰窗口/输入。
#include <cstdio>
#include <string>

#include "common.h"
#include "config.h"
#include "json.h"
#include "settings_reg.h"
#include "text_file.h"

static int g_pass = 0, g_fail = 0;

static void Expect(bool cond, const wchar_t* key, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL  %s  [key=%ls]\n", what, key ? key : L"-");
    }
}

// 回环的三种结局。
//
// 【为什么必须分开】"写入被拒绝"和"写进去了、读回来变了"是**两类完全不同的错**，
// 混成一个 bool 的后果是后者被前者的话术掩盖：被业务判据拒绝的项本来就不该参与
// 边界断言，于是"读回来变了"也被当成"大概是被业务判据拒了吧"而放行。
// 实测过：这个洞会让 holdDelayMs（200 写进去、读回来 800）安然通过。
// 真正读一次盘（模拟"下次启动"）。
//
// 【为什么不能直接调 Load()】Config::Load() 是**幂等**的：loaded_ 置位后直接
// 返回。这是刻意的设计 —— 配置只在启动时读一次，运行中不会因为文件被改就
// 中途换一套值（半途配置突变会让界面与已经建好的窗口状态对不上）。
// 但这也意味着"再调一次 Load"根本不是回环，测试会变成空跑：
// 第一版就是这么写的，把 holdDelayMs 的读盘夹取改回旧规则它照样报绿。
// 所以这里靠"换目录再换回来"触发：SetProfileDir 会清空内存配置并复位 loaded_，
// 效果等价于重启进程（内存里只剩默认值 + 盘上的文件）。
static void ReloadFromDisk() {
    Config& cfg = Config::Get();
    const std::wstring keep = cfg.Dir();
    cfg.SetProfileDir(keep + L"-bounce");
    cfg.SetProfileDir(keep);
    cfg.Load();
}

enum class Rt {
    Ok,            // 写进去了，读回来还是它
    SetRejected,   // settings_reg 当场拒绝（原因看 err）
    Lost,          // 写进去了，但存盘/读盘之后不是它了 —— 这一层的真实故障
};

// 把一项设成 target，存盘，从盘上读回来，问"值还是不是存盘前的那个"。
//
// 【判据是 r1 == r2，不是 r2 == target】有些项会故意把请求规范化再存
// （themeMode 的大小写、statusSegs 的升序去重）。"请求 == 存下的"由
// settings_reg::Set 的写后回读负责（canon 就是为此声明的）；这里要问的是
// **另一件事**：存盘再读盘之后，值有没有被读盘那侧悄悄改掉。
// 混成一个判据的话，规范化过的项会被误报成"回环失败"，真正的问题反而没人看。
static Rt RoundTrip(const std::wstring& key, const json::Value& target, std::wstring* err) {
    Config& cfg = Config::Get();
    if (!settings_reg::Set(key, target, err)) return Rt::SetRejected;
    json::Value before;
    if (!settings_reg::Get(key, &before)) return Rt::Lost;   // 存盘前的运行值
    if (!cfg.Save()) {
        if (err) *err = cfg.lastSaveError();
        return Rt::Lost;   // 存不下去也是"值没保住"，不能算被拒绝
    }
    ReloadFromDisk();
    json::Value after;
    if (!settings_reg::Get(key, &after)) return Rt::Lost;
    return settings_reg::ValueEquals(after, before) ? Rt::Ok : Rt::Lost;
}

int main() {
    std::printf("配置回环（写盘 -> 读盘 值不变）\n");

    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = tf::Join(std::wstring(tmp), L"exbar-roundtrip-test");
    tf::EnsureDir(dir);
    // 测试必须可重复跑：上一轮的 settings.json 会让"原值"断言假失败。
    DeleteFileW(tf::ToExtended(tf::Join(dir, L"settings.json")).c_str());

    Config& cfg = Config::Get();
    cfg.SetProfileDir(dir);
    cfg.Load();

    const auto& entries = settings_reg::Entries();
    for (const auto& e : entries) {
        // 只测**可写**且**有明确数值域**的整型项：这正是夹取规则最容易分叉的形状。
        // 自由取值的项（路径、数组、复合结构）回环由 SelfCheck 的写回验证覆盖。
        if (!e.set || e.kind != settings_reg::Kind::Int) continue;
        if (e.maxValue <= e.minValue) continue;

        json::Value orig;
        settings_reg::Get(e.key, &orig);
        std::wstring err;

        // 边界值：上下限本身在两个方向上都必须是"存得进、读得回"的值。
        for (int v : {e.minValue, e.maxValue}) {
            err.clear();
            switch (RoundTrip(e.key, json::Value::MakeInt(v), &err)) {
                case Rt::Ok:
                    ++g_pass;
                    break;
                case Rt::SetRejected:
                    // 被业务判据拒绝（如跨字段约束）不是回环失败 —— 但若原因说的是
                    // "超出范围"，那就是声明的范围与 setter 的范围不一致，真错。
                    Expect(err.find(L"超出范围") == std::wstring::npos, e.key.c_str(),
                           "边界值不被判『超出范围』（声明范围 == 实现范围）");
                    break;
                case Rt::Lost:
                    Expect(false, e.key.c_str(), "边界值写盘再读盘后不变");
                    break;
            }
        }

        // 还原：这一节把每一项都改过，不还原会污染后面依赖当前值的检查，
        // 也会让"下次运行看到的是上次测试的配置"。
        settings_reg::Set(e.key, orig, &err);
        cfg.Save();
        ReloadFromDisk();
    }

    // 枚举字符串与整数数组：同样逐项走一遍。
    //
    // 【为什么这两类不能只靠 SelfCheck】SelfCheck 只验"写进去能不能读回来"，
    // 它看不见读盘那一侧。实测过的两个例子：themeMode 存 "DARK" 而读盘只认
    // 小写的三个标识（重启变成 system）；statusSegs 写入端原样存、读盘端排序
    // 去重（写 [6,0,6] 重启变 [0,6]）。
    for (const auto& e : entries) {
        if (!e.set) continue;
        std::wstring err;

        if (e.kind == settings_reg::Kind::Str && !e.options.empty()) {
            // 挑一个与当前值不同的选项：设成当前值等于什么都没验。
            json::Value cur;
            settings_reg::Get(e.key, &cur);
            const json::Value* target = nullptr;
            for (const auto& o : e.options)
                if (!settings_reg::ValueEquals(o.value, cur)) {
                    target = &o.value;
                    break;
                }
            if (!target) continue;
            Expect(RoundTrip(e.key, *target, &err) == Rt::Ok, e.key.c_str(), "枚举值回环不变");

            // 大小写不是语义的一部分：调用方传 "DARK" 应该被接受并归一成 "dark"。
            // 这条专抓"写入端原样存、读盘端只认小写"这一类（themeMode 就是这么坏的：
            // config.set themeMode DARK 成功，重启变成 system）。
            if (target->IsString()) {
                std::wstring up = target->str;
                for (wchar_t& c : up)
                    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
                if (up != target->str) {
                    err.clear();
                    const Rt rt = RoundTrip(e.key, json::Value::MakeStr(up), &err);
                    // 被拒（该值不合法）和回环丢失是两件事：只有后者是错的。
                    Expect(rt != Rt::Lost, e.key.c_str(), "大小写变体回环后不被改写");
                }
            }
            settings_reg::Set(e.key, cur, &err);
            Config::Get().Save();
            ReloadFromDisk();
        }

        if (e.kind == settings_reg::Kind::IntArray) {
            // 故意给一个**乱序且带重复**的数组：正是旧版 append 式切换写出来的形状。
            json::Value cur;
            settings_reg::Get(e.key, &cur);
            if (cur.arr.empty()) continue;
            json::Value weird = json::Value::MakeArray();
            for (size_t i = cur.arr.size(); i > 0; --i) weird.arr.push_back(cur.arr[i - 1]);
            weird.arr.push_back(cur.arr[0]);
            err.clear();
            const Rt rt = RoundTrip(e.key, weird, &err);
            Expect(rt != Rt::Lost, e.key.c_str(), "乱序带重复的数组回环后不被改写");
            settings_reg::Set(e.key, cur, &err);
            Config::Get().Save();
            ReloadFromDisk();
        }
    }

    // 恢复默认目录，避免同进程里的后续测试受影响（与 profile_isolation_test 同口径）。
    cfg.SetProfileDir(tf::Join(tf::ExeDir(), L"data"));
    cfg.Load();

    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
