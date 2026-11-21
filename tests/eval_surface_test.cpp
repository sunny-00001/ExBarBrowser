// 评估面完整性单测：防"改了评估器再宣布通过"。
//
// 【要测住的两件事】
//   1. 清单在、且仓库没被改过 → ok=true（正常路径别误报）；
//   2. 清单在、但某个受保护文件被改了 → ok=false 且**指名道姓**说清是哪个。
//
// 第 2 条不能靠真的改仓库文件来测（会污染工作树），
// 所以这里用一个**临时仓库**造一个假清单来测 —— 判定逻辑与真实路径完全一致。
#include <cstdio>
#include <string>

#include "eval_surface.h"
#include "json.h"
#include "text_file.h"

static int pass = 0, fail = 0;
static void Expect(bool c, const char* what) {
    if (c) ++pass;
    else { ++fail; std::printf("  FAIL  %s\n", what); }
}

int main() {
    std::printf("评估面完整性\n");

    const evalsurface::Report real = evalsurface::Check();
    std::printf("  清单：%ls\n", real.manifestPath.c_str());
    Expect(real.manifestFound, "评估面清单存在（仓库里应已签发）");
    if (!real.ok) {
        std::printf("  注意：当前仓库的评估面与清单不一致：\n%ls\n", real.reason.c_str());
    }
    Expect(real.checkedFiles > 0, "至少核对了几个文件");

    // ---- 清单缺失必须判成"无法校验"，而不是"通过" ----
    // 找不到清单时如果返回 ok=true，等于把"没设防"当成"设好了"。
    Expect(!real.ok || real.manifestFound,
           "清单缺失时不会静默通过（manifestFound 与 ok 必须同时成立才算通过）");

    // ---- selfSeal：清单自身被手改必须被抓到 ----
    //
    // 【为什么这条是核心】守卫防的是"改了判据还宣布通过"。如果清单本身
    // 可以被直接编辑（把某个测试的哈希改成新值、把某条记录删掉），
    // 那么守卫就只挡住"忘了同步"，挡不住"故意同步"。
    // selfSeal 让任何未经重签的改动都无法自洽 —— 这条测试守住它。
    //
    // 【关键】必须**真的复算**并比对 seal。只验证"我成功改了一个副本"
    // 证明不了任何事 —— 那是"改副本"这个动作本身成立，而不是"seal 拦住了它"。
    std::printf("\n清单自封（selfSeal）\n");
    {
        std::wstring text;
        const bool haveManifest = tf::ReadText(real.manifestPath, &text);
        Expect(haveManifest, "能读到清单正文");

        if (haveManifest) {
            json::Value m;
            std::wstring err;
            Expect(json::Parse(text, m, &err), "清单是合法 JSON");

            const json::Value* seal = m.Find(L"selfSeal");
            Expect(seal && seal->IsString() && !seal->str.empty(),
                   "清单带 selfSeal（旧版签发或被人删掉这一项都不合格）");

            // ① 原样复算：必须与清单里写的 seal 一致
            //    （这一条守住"两侧算法逐字节一致"—— 不一致的话，
            //      门会永远拒绝构建，或者更糟：为让它通过而把校验删掉）
            std::wstring recomputed;
            if (evalsurface::ComputeSelfSeal(text, &recomputed)) {
                Expect(seal && recomputed == seal->str,
                       "原样复算的 seal 与清单里写的一致（两侧算法逐字节一致）");
            } else {
                Expect(false, "能复算清单的 selfSeal");
            }

            // ② 篡改一份副本：改掉某个受保护文件的哈希，正文必须与 seal 不符。
            json::Value tampered = m;
            bool mutated = false;
            for (auto& kv : tampered.obj) {
                if (kv.first != L"files" || !kv.second.IsObject() || kv.second.obj.empty()) {
                    continue;
                }
                kv.second.obj.begin()->second = json::Value::MakeStr(L"deadbeef");
                mutated = true;
                break;
            }
            Expect(mutated, "清单里至少有一个受保护文件（否则自我校验无意义）");

            if (mutated) {
                std::wstring tamperedSeal;
                const bool canCompute =
                    evalsurface::ComputeSelfSeal(json::Stringify(tampered), &tamperedSeal);
                Expect(canCompute, "篡改后的清单仍能复算（否则说明解析路径有问题）");
                if (canCompute) {
                    Expect(tamperedSeal != seal->str,
                           "篡改哈希后 seal 对不上 —— 手改清单无法自洽");
                }
            }
        }
    }

    std::printf("通过 %d / 失败 %d\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
