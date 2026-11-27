// settings_reg —— 软件自身可调参数的**键表**与读写接口。
//
// 【为什么需要这一层】
// Settings 是编译期的 struct：机器侧（脚本 / AI / 自动化测试）既不知道"有哪些
// 设置项"，也没有稳定名字定位某一项。于是"打开设置、定位某项、改它、读回来确认"
// 这条路在结构上走不通，只能靠人眼看界面。
//
// 这一层给每个设置项一个**稳定身份**：
//     key（settings.json 里的键名）+ 类型 + 读写访问器 + 中文名/说明 + 取值域
//     + 所属分组 + 可选值清单
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
// 【分组】分组不是"分类学"，是**界面结构**：设置对话框左栏的标签页、中栏的
// 段落标题、右栏的归属说明都靠它。分组信息写在表里（Entry::group）而不是
// 由界面猜 —— 界面猜就意味着"界面里有一份分类规则"，加一项时得记得同步两处，
// 而漏同步的表现是"新设置项出现在错误的段落里"，没有任何检查会报。
//
// 【只读项】窗口几何、会话、标签个性化、键位覆盖这些"改了不会立即生效"或
// "运行时另有一份活状态"的项只提供读取：set 为空即只读，doc 里写清该用哪个
// 入口。与其造一个半成品写入口，不如明确说"这只读"。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "config.h"     // Settings：默认值与当前值的同一份映射
#include "icon_glyph.h" // ico::IconId：分组图标
#include "json.h"

namespace settings_reg {

// 值类型。决定命令行参数怎么校验、JSON 怎么序列化、界面用什么交互。
enum class Kind {
    Bool,      // 开关
    Int,       // 整数（可带范围）
    Str,       // 字符串（枚举型在 set 里校验）
    StrArray,  // 字符串数组
    IntArray,  // 整数数组
    // 对象数组（namedSessions / tabMeta / sessionHistory …）。
    //
    // 【为什么不能并入 Object】Kind 是**对外契约**：config.list 把 KindName 报给
    // AI 与脚本，界面按它决定"怎么显示这个值"。声明成 Object 而 get 返回数组时，
    // 契约是假的 —— 调用方按对象去取字段会拿到空，界面也只会说"（复合结构）"
    // 而不是"N 项"。形状与声明必须一一对应，这是唯一能让两边都不猜的写法。
    ObjArray,  // 对象数组（只读）
    Object,    // 复合结构（hotkeys，键 → 值）—— 只读
};

const char* KindName(Kind k);

// 一个可选值（枚举项）。
//
// 【为什么显示名与取值要分开】界面显示中文，落盘与命令用稳定标识。
// tabNewPos 显示"最右"，存的是 "end"（旧 "right" 读入时归一）；viewMode 显示"详细信息"，存的是 5。
// 合成一串（"right=最右"）会让取值本身变成中文，settings.json 的键名契约
// 就被界面文案绑住了 —— 换个说法就得改数据。
struct Option {
    std::wstring label;
    json::Value value;
};

// 分组（设置对话框左栏的一个标签页）。
//
// key 是内部标识（不改名不影响用户 —— 它不出现在 settings.json 里）；
// name 是中文名；note 是"改了什么时候生效"这类提示。
struct Group {
    std::wstring key;
    std::wstring name;
    std::wstring note;
    ico::IconId icon = ico::IconId::None;
};

struct Entry {
    std::wstring key;    // settings.json 里的键名。**永不改名**（对外契约）
    Kind kind;
    // 读当前值。参数是 Settings 的引用而不是"自己去读全局"：
    // 同一份映射既能读运行值，也能读**默认值**（一份默认构造的 Settings），
    // 于是"恢复默认值"不需要第二张表 —— 两张表迟早分叉，而分叉点只有
    // 用户能发现（按了"恢复默认值"，界面却停在另一个值）。
    std::function<json::Value(const Settings&)> get;
    std::function<bool(const json::Value&, std::wstring*)> set;    // 写；空 = 只读
    std::wstring doc;    // 中文名 + 说明（界面标题，也是 AI 的决策依据）
    // 所属分组（见 Groups）。必须是非空且在 Groups 里存在的 key。
    std::wstring group;
    // 可选值清单（空 = 自由取值）。界面据此画下拉而不是输入框。
    std::vector<Option> options;
    // Int 型的取值域。min == max == 0 表示未声明（不做范围校验、界面也不提示）。
    // 声明了就必须在 set 里真的查 —— 否则"文档说 200..5000、代码接受 1"。
    int minValue = 0;
    int maxValue = 0;
    // 可选的"规范形"：set 之后回读到的值必须等于 canon(请求值)。
    //
    // 【为什么需要】set 允许把请求规范化之后再存（tabNewPos 的旧别名 right → end、
    // 自定义搜索范围路径的分隔符归一）。而"写后回读"的判据是"回读 == 请求"，
    // 于是规范化过的项每次都被判成"写入后回读不一致"—— 一次正确的写入报成失败。
    // 声明 canon 之后判据变成"回读 == canon(请求)"：严格性一点没少（setter 完全
    // 不生效时回读仍是旧值，与 canon(请求) 不等，照样报错），但合法的规范化不再误报。
    //
    // 【必须在 set 与 canon 之间共用同一个规范化函数】两处各写一份就等于
    // "存的"与"期望的"各有定义，迟早分叉。
    std::function<json::Value(const json::Value&)> canon;
};

// 全部项（声明顺序即文档顺序，也是界面顺序）。新设置项必须在这里登记，
// 否则命令与界面都看不见它 —— 那是"能力缺失"，不是"没做美化"。
const std::vector<Entry>& Entries();

// 全部分组（顺序即左栏标签页顺序）。
const std::vector<Group>& Groups();

// 按 key 找分组。找不到返回 nullptr。
const Group* FindGroup(const std::wstring& key);

// 按 key 找。找不到返回 nullptr。
const Entry* Find(const std::wstring& key);

// 读一项。false = key 不存在（调用方据此如实报错，而不是返回空值 ——
// 那会与"值本身为空"混淆）。
bool Get(const std::wstring& key, json::Value* out);

// 读一项的**默认值**（默认构造的 Settings）。"恢复默认值"走这里。
bool GetDefault(const std::wstring& key, json::Value* out);

// 写一项。失败时 err 给出人可读原因（不存在 / 只读 / 类型不符 / 超范围）。
//
// 【为什么错误必须带原因】"set 失败"对调用方没有信息量 —— 名字打错和值超范围
// 该做的下一步完全不同，AI 要靠原因决定是改正名字、改值，还是放弃。
//
// 【写后回读】成功返回前会读回来比对：赋值语句跑过 ≠ 状态真的变了。
bool Set(const std::wstring& key, const json::Value& value, std::wstring* err);

// 两个值是否相等（按类型逐字段比；数组 / 对象递归比）。
// 单独做成公开接口：界面要判断"当前值是不是某个枚举项"，那正是同一个问题 ——
// 两处各写一份比较逻辑，迟早在"数字 5 与字符串 "5""这种边界上给出不同答案。
bool ValueEquals(const json::Value& a, const json::Value& b);

// 全部项导出为 JSON 数组：key / kind / doc / group / value / writable
// （+ Int 的 min/max + 枚举项的显示名）。这是"能枚举"的落地形态，
// 直接可给 AI 当决策依据。
json::Value DumpAll();

// 表自检：每项都要读得出来；可写项要能用当前值写回（证明 set 真的接线了）；
// 每项都要归在一个已登记的分组里。返回不通过的项（空 = 全通过）。给单测用 ——
// 这条最容易腐化：加了一行却忘了在 set 里加分支，表现是"设置改了没反应"，
// 从现象极难反推。
std::vector<std::wstring> SelfCheck();

// doc 的格式约定：「名称：说明」。界面把 doc 拆成两栏显示（名称列 + 说明列），
// 于是"名称"不必在表里写第二遍 —— 写两遍就得保持同步，而漏同步的表现是
// "界面标题和说明对不上"。
//
// 拆点固定是**第一个全角冒号**：没有冒号就整段当名称（说明为空）。
// 只有一个实现：界面、AI 说明、对话框右栏都走这里。
//
// 【例外：冒号落在括号里】只读项写成「名称（只读：细节）」时，按第一个冒号拆
// 会得到半个括号的名字（「名称（只读」）。所以冒号若落在圆括号内，改在括号前拆：
// 名字取括号外，说明取括号内（连「只读：」一起）。反过来「说明（括号）：细节」
// 里冒号在括号外，仍按老规矩拆。
void SplitDoc(const std::wstring& doc, std::wstring* name, std::wstring* desc);

}  // namespace settings_reg
