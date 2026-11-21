// settings_reg —— 软件自身可调参数的**键表**与读写接口。
//
// 【为什么需要这一层】
// Settings 是编译期的 struct：机器侧（脚本 / AI / 自动化测试）既不知道"有哪些
// 设置项"，也没有稳定名字定位某一项。于是"打开设置、定位某项、改它、读回来确认"
// 这条路在结构上走不通，只能靠人眼看界面。
//
// 这一层给每个设置项一个**稳定身份**：
//     key（settings.json 里的键名）+ 类型 + 读写访问器 + 中文名/说明 + 取值域
// 于是能枚举（有哪些项）、能按名字读写（不靠坐标/行号/中文文案）、能校验
// （类型/范围不符当场拒绝，而不是静默截断）。
//
// 【键名口径】用 settings.json 里的键（"tab" / "viewMode" / "seqTimeoutMs"…），
// 不是结构体字段名。理由：config.list 暴露的名字要与用户手改 settings.json 时
// 看到的名字**完全一致** —— 否则会出现"命令里叫 ui.tab、文件里叫 tab"这种分裂，
// 而两套名字之间的对应关系只能靠文档维系。
//
// 【唯一实现】每条 Entry 的 get/set 都直接读写 Config::mutableSettings()，
// 不另存副本。两份副本必然分叉，而分叉只在"命令改了 A、界面显示 B"时才暴露。
//
// 【只读项】窗口几何、会话、标签个性化、键位覆盖这些"改了不会立即生效"或
// "运行时另有一份活状态"的项只提供读取：set 为空即只读，doc 里写清该用哪个
// 入口。与其造一个半成品写入口，不如明确说"这只读"。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "json.h"

namespace settings_reg {

// 值类型。决定命令行参数怎么校验、JSON 怎么序列化、界面用什么交互。
enum class Kind {
    Bool,      // 开关
    Int,       // 整数（可带范围）
    Str,       // 字符串（枚举型在 set 里校验）
    StrArray,  // 字符串数组
    IntArray,  // 整数数组
    Object,    // 复合结构（tabMeta / hotkeys）—— 只读
};

const char* KindName(Kind k);

struct Entry {
    std::wstring key;    // settings.json 里的键名。**永不改名**（对外契约）
    Kind kind;
    std::function<json::Value()> get;                              // 读当前值
    std::function<bool(const json::Value&, std::wstring*)> set;    // 写；空 = 只读
    std::wstring doc;    // 中文名 + 说明（界面标题，也是 AI 的决策依据）
    // Int 型的取值域。min == max == 0 表示未声明（不做范围校验、界面也不提示）。
    // 声明了就必须在 set 里真的查 —— 否则"文档说 200..5000、代码接受 1"。
    int minValue = 0;
    int maxValue = 0;
};

// 全部项（声明顺序即文档顺序，也是界面顺序）。新设置项必须在这里登记，
// 否则命令与界面都看不见它 —— 那是"能力缺失"，不是"没做美化"。
const std::vector<Entry>& Entries();

// 按 key 找。找不到返回 nullptr。
const Entry* Find(const std::wstring& key);

// 读一项。false = key 不存在（调用方据此如实报错，而不是返回空值 ——
// 那会与"值本身为空"混淆）。
bool Get(const std::wstring& key, json::Value* out);

// 写一项。失败时 err 给出人可读原因（不存在 / 只读 / 类型不符 / 超范围）。
//
// 【为什么错误必须带原因】"set 失败"对调用方没有信息量 —— 名字打错和值超范围
// 该做的下一步完全不同，AI 要靠原因决定是改正名字、改值，还是放弃。
//
// 【写后回读】成功返回前会读回来比对：赋值语句跑过 ≠ 状态真的变了。
bool Set(const std::wstring& key, const json::Value& value, std::wstring* err);

// 全部项导出为 JSON 数组：key / kind / doc / value / writable（+ Int 的 min/max）。
// 这是"能枚举"的落地形态，直接可给 AI 当决策依据。
json::Value DumpAll();

// 表自检：每项都要读得出来；可写项要能用当前值写回（证明 set 真的接线了）。
// 返回不通过的项（空 = 全通过）。给单测用 —— 这条最容易腐化：加了一行却忘了
// 在 set 里加分支，表现是"设置改了没反应"，从现象极难反推。
std::vector<std::wstring> SelfCheck();

}  // namespace settings_reg
