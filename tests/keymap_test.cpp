// 运行时键位表自测：确认能读 data/keymap.json（当前 102 热键 + 38 序列）。
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
//     -D_WIN32_IE=0x0A00 -Isrc tests/keymap_test.cpp src/keymap.cpp src/json.cpp \
//     src/text_file.cpp src/str.cpp src/log.cpp src/platform_win.cpp \
//     -o /tmp/keymap_test.exe -static-libstdc++ -static-libgcc \
//     -lshell32 -lole32 -luxtheme -ladvapi32 -ldwmapi
// 运行：从 tests/ 里跑（默认读 ../data/keymap.json），或把路径作为第一个参数传进来。
//
// 依赖为什么是这些：`LoadKeymap` → `tf::ReadText`（text_file）→ `str::FromUtf8Strict`（str）
// → `LogDbg`（log）→ `win::ThemeControl`（platform_win，要 uxtheme）。早期文档只列了前三个，
// 照抄必然链接失败 —— 记录在这里，省得下一个人重新试一遍。
#include <cstdio>

#include "keymap.h"

// 键位条数会随功能增长（新增快捷键就要 +1）。这里钉的是"目录与内置兜底表同步"，
// 不是"冻结在某个数字"。改 data/keymap.json 后同步改这里，或者跑
// `python tools/gen_keymap.py` 让生成头跟上。
//
// 84 → 82（2026-11-21）：两条热键被移除（书签/标签右键动作补成命令后，
// 原来那两条重复绑定没有意义）。**不是随手改数字**：改完后
// `check_commands.py` 的"命令目录 / keymap / 处理器三方一致"仍是绿的，
// 说明没有命令因此失去入口。
//
// 82 → 84 → 85（2026-11-21）：新增 GoTab9 热键（Ctrl+9），原来的"最后一个标签"
// （TabLast）顺移到 Ctrl+0，让 Ctrl+1..Ctrl+9 与 Ctrl+0 连续对应
// 1..9 与"最后"。**不是随手改数字**：改完后 `check_commands.py` 的
// "命令目录 / keymap / 处理器三方一致"仍是绿的，说明没有命令因此失去入口。
//
// 85 → 98（2026-11-21，P2）：把 input_router.cpp 里隐藏的 kExtra 收编进
// data/keymap.json，并补齐 Alt+Enter / Ctrl+E / Alt+D / F2 / Ctrl+L /
// Ctrl+Shift+1..8 等 Explorer 常用键位。这些键从此可见、可配，并和所有
// 其他键位走同一套先到先得逻辑；不是随手改数字，`check_commands.py`
// 的"命令目录 / keymap / 处理器三方一致"仍是绿的。
//
// 98 → 100（2026-11-21，P2 尾）：补两个需要真实实现的 Explorer 键位：
// Shift+Delete（CmdDeletePermanent，永久删除）与 Ctrl+Shift+E
// （NavTreeExpandAll，展开导航树）。二者都有命令表条目与处理器。
//
// 【注意】这个数字跟**本机**的 data/keymap.json 走 —— 而 data/keymap.json
// 是 .gitignore 掉的个人配置（见 .gitignore "便携模式的用户数据"）。
// 换一台机器跑这个测试，数字可能不同；那不是代码坏了，是个人键位不同。
// 仓库里入库的 src/keymap.gen.h 才是"所有人共用的兜底表"。
// 102 → 100（原文 98 → 100）之后又补了双栏两条：Ctrl+F6（切换双栏）、F6（切换窗格）。
// 103 → 104（2026-11-23）：新增 Ctrl+J 开合右侧 AI 面板（ai.panel.toggle）。
// 104 → 105（2026-11-23）：新增 Ctrl+Alt+J 在系统浏览器打开面板（ai.panel.web）——
// 它是 WebView2 不可用时的退路，与 Ctrl+J 是一对（内嵌 / 外置）。
// 这两条都是**新增能力**而不是改数字：命令表里有条目、有 handler、有状态回读
// （aiPanelVisible），`check_commands.py` 的三方一致仍是绿的。
// 保持这里的数字与 data/keymap.json 同步；src/keymap.gen.h 是所有人的兜底表。
constexpr size_t kExpectedHotkeys = 105;
// 207 序列（38 默认 + 169 命令覆盖，见 data/keymap.json）：
// 前缀按命令组分配（b=书签 c=配置 d=开发 e=外部 f=文件 g=前往 n=导航 p=路径
// q=搜索 s=会话 t=标签 v=视图 w=窗口 x=关闭族 y=复制族 z=工具）。
//
// 【不是"每条命令都有序列"了 —— 序列只绑无参命令】
// 序列执行路径的 arg **恒为空**（InputRouter::FeedSequence →
// InvokeByNameLegacy(cmd, L"") → ArgsFromLegacyArg(def, "") → Args::Empty()），
// 所以**带必填参数的命令绑到序列上 = 每次按都注定失败**
// （实测：q s 报「未知的搜索范围：」、q q 把搜索框清空 —— 语义正好相反）。
// 因此这里的口径是：序列只覆盖**无必填参数**的命令；
// 带参命令的入口是命令面板（按 schema 追问参数）、热键内联参数（"view.mode:3"）
// 或界面菜单。
//
// 【209 → 207 的来历】（2026-11-22）删掉了两条**注定失败**的序列：
//   q s → search.scope.set（必填 scope）、q q → search.query.set（必填 query）。
//   它们的正确入口是命令面板与界面（范围菜单 / 历史下拉），不是序列。
//   同一个 q 前缀下保留的两条（q h / q l）都是**无参**查询命令，不会踩这个坑。
//
// 【207 → 169 的来历】（2026-11-23）同一类缺陷的**全量清算**：
//   上面那两条是"逐个发现、逐个删"的，剩下 38 条一直留着 —— 每按一次都注定失败。
//   其中三条不是失败，而是"静默做了别的事"（比失败更坏）：
//     t o → tab.color        空 color=0        → 标签被改成"无颜色"
//     w w → win.move         空 x,y=0          → 窗口挪到屏幕左上角
//     v r → view.pane.split  空 percent=50     → 分栏比例被重置成一半
//   另外 5 条（bmk.import/bmk.export/file.rename.batch/session.named.save/tool.compare）
//   在空参下会**弹出输入框**——序列按下去冒出一个模态框，同样是"按了没按预期的做"。
//
//   修法不是补内联参数（这些参数的合法值是运行时才知道的，没有可写死的确定值），
//   而是删掉绑定：带参命令的正确入口是命令面板（按 schema 追问）或界面菜单。
//   防复发已经机械化：`check_commands.py` 规则 C4 直接从 argsSchema 的 required
//   判定"序列绑了必填参数却没有内联参数"，命中即报错 —— 下次新增命令不用有人记得。
constexpr size_t kExpectedSequences = 169;

int main(int argc, char** argv) {
    // 二进制没有宽字符命令行；用窄 argv 直接转（路径只含 ASCII）。
    std::wstring path = L"../data/keymap.json";
    if (argc > 1) path.assign(argv[1], argv[1] + strlen(argv[1]));
    Keymap km;
    bool ok = LoadKeymap(path, &km);
    std::printf("load=%d hotkeys=%zu sequences=%zu\n", ok ? 1 : 0, km.hotkeys.size(),
                km.sequences.size());

    int shown = 0;
    for (const auto& h : km.hotkeys) {
        if (h.key.empty()) continue;
        std::printf("  key  %-22ls %-16ls %ls\n", h.setting.c_str(), h.key.c_str(),
                    h.command.c_str());
        if (++shown >= 5) break;
    }
    shown = 0;
    for (const auto& s : km.sequences) {
        std::printf("  seq  %-8ls -> %ls\n", s.seq.c_str(), s.command.c_str());
        if (++shown >= 5) break;
    }

    if (!ok) return 2;
    // 不符时**把两个值都打出来**：只给退出码 3 的话，下一个人要先猜是"表坏了"还是
    // "常量过时了"。绝大多数情况是常量过时（新增键位没同步这里）。
    if (km.hotkeys.size() != kExpectedHotkeys || km.sequences.size() != kExpectedSequences) {
        std::printf("MISMATCH  期望 %zu/%zu，实际 %zu/%zu —— 若刚加了键位，改 kExpected* 常量\n",
                    kExpectedHotkeys, kExpectedSequences, km.hotkeys.size(),
                    km.sequences.size());
        return 3;
    }
    return 0;
}
