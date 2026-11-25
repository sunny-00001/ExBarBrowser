// 输入路由的「序列键 + 弹层」作用域。
//
// 【为什么单独成文件】input_router.cpp 是**键鼠路由**（组合键 → 命令、鼠标规则）；
// 序列键前缀累积与弹层的键盘归属是另一种语义：它有自己的状态机（SequenceMachine）、
// 自己的候选数据源（provider）、自己的回灌协议（prefix → 唯一匹配 → 执行）。
// 两者混在一个文件里，读的人要翻到文件末尾才发现还有另一套语义。
//
// 【弹层开着时所有键归它】特殊键（方向/回车/Esc）直接处理；键模式
// （用户唤起态 / 序列键候选）下的字母数字按 **vk** 取序列 token；
// 其余可打印键**在按键层就吞掉**，字符由 chord::CharOfVk 现译（US 布局，
// 确定性、不经输入法）—— 与原版 exbar.cpp「低层钩子吞键 + 现译字符」同款。
//
// 【为什么不用 ToUnicodeEx 自己转字母】中文输入法在中文态下会把字母吃进
// 拼音组合：翻译不出（返回 0），还会把**桌面级**输入法状态卡死（之后所有
// 进程都翻译不出字母）。见 chord::CharOfVk 的说明。
//
// 【为什么不等 WM_CHAR】WM_CHAR 是输入法转好字之后的结果：中文态下字母
// 要等用户提交拼音组合才出现，英文态才有；而原版是"键一按下去就有字符"。
// 按键层吞掉还有个附带好处：输入法根本收不到这些键，不会出现"打 g 弹出
// 拼音候选框"的干扰。
#include <algorithm>

#include "commands.h"
#include "input_router.h"   // 配置经 InputConfig 注入（cfg_），不直接 include config.h（R15）
#include "str.h"

void InputRouter::InitSequenceOverlay(SequenceOverlay::RowsFn rows, SequenceOverlay::ExecFn exec) {
    if (theme_) overlay_.Init(main_, *theme_, std::move(rows), std::move(exec));
    // 弹窗把序列前缀回灌给状态机：唯一匹配时直接执行并让弹窗关闭。
    overlay_.SetSeqKeyFeed([this](const std::wstring& prefix) { return SeqKeyFeed(prefix); });
}

void InputRouter::ShowSequenceOverlay(const std::wstring& mode) {
    Cancel();
    overlay_.Open(mode);
}

bool InputRouter::HandleOverlayKey(WPARAM vk, LPARAM keyInfo, int mods) {
    // 特殊键（Esc/方向/回车/退格）+ 键模式下的字母数字：弹窗自己处理。
    if (overlay_.HandleKey(vk, keyInfo)) return true;
    // 弹窗没吞掉的键 → 当作「输入框打字」处理：字符现译（chord::CharOfVk，
    // US 布局确定性映射），不进输入法。
    //
    // 【为什么在这里就吞键，而不是等 WM_CHAR】原版 exbar.cpp 的做法是
    // 「低层键盘钩子吞键 + 钩子里现译字符」—— 输入法**根本看不到**这些键，
    // 所以不会有拼音组合、也不会有"打出来是汉字"。本项目对等实现：
    // 按键层吞掉 → 不调 TranslateMessage → 不产生 WM_CHAR → 输入法无从插手。
    // （走 WM_CHAR 的话，中文输入法在中文态下会把字母吃进拼音组合，
    //   字母要等用户提交组合才出现，甚至永远不出现。）
    //
    // 【模式前缀也在这一层】唤起态 / 序列键候选态下，符号前缀（! ? ~ …）
    // 由 SequenceOverlay::HandleChar 判前缀并切模式。
    const int m = (mods >= 0) ? mods : CurrentMods();
    // 带 Ctrl/Alt 的组合不是"输入字符"（复制/粘贴/Alt+F4 等），一律放行。
    if ((m & (chord::kCtrl | chord::kAlt)) != 0) return false;
    const wchar_t c = chord::CharOfVk(static_cast<unsigned>(vk), (m & chord::kShift) != 0);
    if (c == 0) return false;
    return overlay_.HandleChar(c);
}

bool InputRouter::HandleWheel(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 滚轮分流：弹窗开着且光标在弹窗列表上 → 滚弹窗，不给后面的视图。
    //
    // 【为什么要在宿主这一层拦】滚轮消息只发给**焦点窗口**（弹窗是
    // WS_EX_NOACTIVATE 的置顶浮窗，拿不到焦点）→ 以前它落到文件视图上，
    // 弹窗列表纹丝不动、背后的列表却在滚。宿主在消息循环里先截住
    // （派发之前），弹窗滚动与"背后别动"就都对了。
    if (msg != WM_MOUSEWHEEL) return false;   // 横向滚轮不参与
    if (!overlay_.visible()) return false;
    const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (!overlay_.ListHitScreen(screen)) return false;
    return overlay_.Wheel(GET_WHEEL_DELTA_WPARAM(wParam));
}

std::wstring InputRouter::TokenOf(WPARAM vk) {
    // 唯一实现：seqov::TokenChar —— 弹窗里"再敲一个键"走同一份口径，
    // 否则弹窗列的候选与状态机真正会执行的命令可能不是同一条。
    const wchar_t c = seqov::TokenChar(vk);
    if (!c) return std::wstring();
    return std::wstring(1, c);
}

bool InputRouter::HandleChar(wchar_t ch) {
    // 弹窗开着时：字符全归弹窗（输入框打字过滤）。
    // 但焦点已经进 WebView2 聊天框时，残留的弹窗必须先收掉；否则中文输入法
    // 提交的字符会被弹窗吃掉，表现就是"聊天框打字没反应"。
    if (overlay_.visible()) {
        if (InTextEntry(GetFocus())) {
            Cancel();
            return false;
        }
        return overlay_.HandleChar(ch);
    }
    return false;
}

bool InputRouter::FeedSequence(const std::wstring& token) {
    if (!cfg_->SeqEnabled()) {  // 用户关掉序列后，字母键应原样交给视图（type-ahead）
        Cancel();
        return false;
    }
    std::wstring cmd;
    SequenceMachine::Result r = seq_.Feed(token, &cmd);
    LogDbg(L"[seq] token=%s prefix='%s' result=%d cmd=%s", token.c_str(), seq_.prefix().c_str(),
           static_cast<int>(r), cmd.c_str());
    switch (r) {
    case SequenceMachine::kMatched:
        hint_.Hide();
        overlay_.Close();
        KillTimer(main_, kSeqTimer);
        CommandBus::Get().InvokeByNameLegacy(cmd, L"", Actor::Seq());
        return true;
    case SequenceMachine::kPending:
        ShowSequenceHint();
        SetTimer(main_, kSeqTimer, static_cast<UINT>(cfg_->SeqTimeoutMs()), nullptr);
        return true;
    case SequenceMachine::kAborted:
        hint_.Hide();
        overlay_.Close();
        KillTimer(main_, kSeqTimer);
        return true;
    default:
        return false;
    }
}

bool InputRouter::SeqKeyFeed(const std::wstring& prefix) {
    // 弹窗是序列状态的唯一真相来源：重跑状态机（从空前缀），
    // 逐段喂入，最后一段命中即执行。
    seq_.Reset();
    std::wstring cmd;
    bool matched = false;
    std::wstring token;
    for (wchar_t c : prefix) {
        if (c == L' ') continue;
        token.assign(1, c);
        const SequenceMachine::Result r = seq_.Feed(token, &cmd);
        if (r == SequenceMachine::kMatched) { matched = true; break; }
        if (r != SequenceMachine::kPending) return false;  // 中途失配：交给弹窗继续显示候选
    }
    if (!matched) return false;
    KillTimer(main_, kSeqTimer);
    // 执行后按钉住态收尾（原版 SeqAfterExecute）：
    //   · 未钉住 → 先收起再派发（带参命令会弹输入框，弹层置顶会盖住对话框）
    //   · 钉住   → 弹层常驻，派发完只清输入框
    const bool keepOpen = overlay_.pinned();
    if (!keepOpen) overlay_.Close();
    CommandBus::Get().InvokeByNameLegacy(cmd, L"", Actor::Seq());
    // 命令自己开了新弹层（如 g p → 命令面板）时不要动它的输入框。
    if (keepOpen && overlay_.SeqKeyMode()) overlay_.ClearBuffer();
    return true;
}

void InputRouter::ShowSequenceHint() {
    if (!cfg_->SeqShowHints()) return;
    // 序列候选与命令面板**共用同一个弹窗**（对齐原版 ExBarSeqOverlay）：
    // 5 列（序号│序列键│描述│命令│分组）+ 底部输入区。
    // 旧的 SequenceHint 小浮层只有 2 列、无输入区，与原版差得远 —— 不再用。
    // 前缀存紧凑形式（"x l" → "xl"，原版 SeqCompact 口径）。
    std::wstring prefix;
    for (wchar_t c : seq_.prefix())
        if (c != L' ') prefix.push_back(c);
    overlay_.OpenSeqKeys(prefix);
    SetTimer(main_, kSeqTimer, static_cast<UINT>(cfg_->SeqTimeoutMs()), nullptr);
}
