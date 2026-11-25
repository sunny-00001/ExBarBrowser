// 设置项落地判据的表驱动自测：**每个可写键都必须被某个落地类别覆盖**。
//
// 【为什么这个测试值得存在】2026-11-23 实测过一条真实的假成功：
// `config.set bookmark=false` 写盘成功、`config.get` 读回 false，而书签栏
// 仍然 visible=True、高 26px —— 因为 ConfigSet 里只有 statusbar/statusSegs
// 走了 Layout()。命令返回 ok=true 而界面没变，比报错更贵：调用方（尤其 AI）
// 据此认为状态已改，后面每一步都建在错的假设上。
//
// 这类漏项没有任何运行期检查会报，只能靠"对全部可写键逐个问一遍"的表驱动用例。
// 判据：可写键 → 要么属于某个落地类别，要么被**显式**列为纯数据项。
// 不允许"没有分支就算纯数据"—— 那正是漏项的温床。
#include <cstdio>
#include <string>
#include <vector>

#include "config_apply.h"
#include "settings_reg.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

// 逐条打印"哪个键漏了"，比一个笼统的 FAIL 有用得多。
static void CheckKey(const std::wstring& key) {
    if (cfgapply::NeedsAnyApply(key)) return;
    if (cfgapply::IsPureData(key)) return;
    std::printf("FAIL  可写键既没有落地动作、也没被列为纯数据：%ls\n", key.c_str());
    ++g_fail;
}

int main() {
    // ① 之前实测漏掉的那一条：栏显隐必须触发布局。
    for (const wchar_t* k : {L"tab", L"nav", L"bookmark", L"statusbar"}) {
        Check(cfgapply::AffectsLayout(k), "四个栏显隐都触发布局");
    }
    Check(cfgapply::AffectsLayout(L"navPaneWidth"), "导航窗格宽度触发布局");
    Check(cfgapply::AffectsLayout(L"aiPanelWidth"), "AI 面板宽度触发布局");
    Check(cfgapply::AffectsLayout(L"showNavPane"), "导航窗格显隐触发布局");
    Check(cfgapply::AffectsLayout(L"dualPane"), "双栏切换触发布局");
    Check(cfgapply::AffectsLayout(L"paneSplit"), "分栏比例触发布局");
    Check(cfgapply::AffectsLayout(L"paneVertical"), "分栏方向触发布局");

    // ② 不该触发布局的别误伤（否则每次改延时都整窗重排）。
    Check(!cfgapply::AffectsLayout(L"seqTimeoutMs"), "序列超时不触发整窗布局");
    Check(!cfgapply::AffectsLayout(L"themeMode"), "主题不靠布局落地（走 ApplyTheme）");
    Check(!cfgapply::AffectsLayout(L"recentLimit"), "历史上限不触发整窗布局");

    // ③ 主题 / 视图 / 状态栏 / 历史各自的归属。
    Check(cfgapply::AffectsTheme(L"themeMode"), "深浅色档位走重套主题");
    Check(cfgapply::AffectsActiveView(L"viewMode"), "视图档位套到当前视图");
    Check(cfgapply::AffectsActiveView(L"groupBy"), "分组套到当前视图");
    Check(cfgapply::AffectsStatusBar(L"statusSegs"), "状态栏段集要重新喂给栏");
    Check(cfgapply::AffectsHistoryLimits(L"recentLimit"), "最近访问上限要推给持有者");
    Check(cfgapply::AffectsHistoryLimits(L"closedTabLimit"), "关闭历史上限要推给 TabSet");

    // ④ **全覆盖**：表里每个可写键都必须有归属。
    // 这是本文件存在的理由 —— 新增一个键却忘了写落地，这一条会红。
    size_t writable = 0;
    for (const auto& e : settings_reg::Entries()) {
        if (!e.set) continue;   // 只读项不经 config.set 落地，不在管辖内
        ++writable;
        CheckKey(e.key);
    }
    Check(writable > 0, "键表里确实有可写项（否则这条测试等于空转）");

    // ⑤ 反向：纯数据项不能同时被声明为"需要落地"（两份判据打架会让行为随调用点变）。
    for (const auto& e : settings_reg::Entries()) {
        if (!e.set) continue;
        if (cfgapply::IsPureData(e.key))
            Check(!cfgapply::NeedsAnyApply(e.key), "纯数据项不应同时有落地动作");
    }

    // ⑥ 纯数据项必须真的在表里（防止改名后判据表留下孤儿）。
    for (const wchar_t* k :
         {L"seqTimeoutMs", L"holdDelayMs", L"tabNewPos", L"confirmCloseTabs", L"aiTimeoutMs"}) {
        Check(settings_reg::Find(k) != nullptr, "纯数据项仍在键表里（判据表没有孤儿）");
    }

    if (g_fail) {
        std::printf("config_apply_test: %d 失败\n", g_fail);
        return 1;
    }
    std::printf("全部通过（可写键 %zu 个，全部有落地归属）\n", writable);
    return 0;
}