// 模态层数守卫（modal::Depth / modal::Scope）自测。
//
// 【为什么值得单测】这个计数一旦失衡，后果是**静默**的：
//   · 永远减不回去 → AI 泵被永久关掉（AI 通道看着还开着，就是不响应）；
//   · 永远加不上去 → 重入防护形同虚设（模态框里照样派发 AI 命令）。
// 两种都不报错，只是功能不对 —— 所以必须有单测盯着。
#include "common.h"

#include <cstdio>

#include "modal.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    std::printf("模态层数\n");

    Check(modal::Depth() == 0, "初始深度是 0（没有模态框）");

    {
        modal::Scope s1;
        Check(modal::Depth() == 1, "进入一层后深度是 1");
        {
            modal::Scope s2;
            Check(modal::Depth() == 2, "嵌套两层后深度是 2");
        }
        Check(modal::Depth() == 1, "内层析构后回到 1");
    }
    Check(modal::Depth() == 0, "全部析构后回到 0");

    // 早退路径：作用域提前结束也要正确减计数。
    {
        modal::Scope s;
        Check(modal::Depth() == 1, "提前退出的作用域也计数");
    }
    Check(modal::Depth() == 0, "提前退出后回到 0");

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
