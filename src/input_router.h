// 输入端总路由：键盘组合键 / 序列键 / 鼠标规则。
//
// 一条纪律：本模块**只发命令**，不改任何业务状态（Tab/导航/书签都由命令处理者负责）。
#pragma once
#include <string>
#include <vector>

#include "chord.h"
#include "common.h"
#include "keymap.h"
#include "mouse_rules.h"
#include "sequence.h"
#include "sequence_hint.h"
#include "theme.h"

struct BindingRow {
    std::wstring setting;
    std::wstring command;
    std::wstring title;
    std::wstring group;
    std::wstring scope;
    std::wstring key;      // 当前生效键（含覆盖/系统原生参考）
    bool implemented = false;
    bool isSequence = false;
    bool conflict = false;
    bool reserved = false;
};

class InputRouter {
public:
    void Init(HWND main, const Theme& theme);
    void Reload();  // 键位覆盖 / 鼠标规则变化后调用

    // 键盘：返回 true = 已消费。
    // keyInfo 是 WM_KEYDOWN 的 lParam（用 bit30 识别 auto-repeat：按住键不该连发序列）。
    // 故意不给默认值：漏传 lParam 会让 auto-repeat 守卫静默失效，宁可编译报错。
    bool HandleKey(WPARAM vk, HWND focus, LPARAM keyInfo);

    // 鼠标：组件命中后调用；返回 true = 规则已接管，组件应跳过默认行为
    // expectUp=false 用于 Shell 的 WM_PARENTNOTIFY（只有 DOWN、没有 UP）：
    // 这类路径无法靠抬起清 claim，派发后由 InputRouter 自己即时释放。
    bool OnMouseDown(int target, int button, int mods, POINT screen, bool expectUp = true);
    bool OnMouseUp(int target, int button, int mods);
    bool OnWheel(int target, int mods, int delta, bool horizontal);
    void OnMouseMove(POINT screen);
    void OnTimer(UINT_PTR id);
    void Cancel();

    // 管理器
    const std::vector<BindingRow>& Rows() const { return rows_; }
    bool SetKeyForCommand(const std::wstring& command, const std::wstring& key);
    void ResetDefaults();
    std::wstring StatusText() const;

private:
    enum {
        kSingleTimer = 0x5101,
        kHoldTimer = 0x5102,
        kSeqTimer = 0x5103,
    };

    struct RuntimeBinding {
        KeyChord chord;
        std::wstring command;
        std::wstring setting;
        std::wstring scope;
    };

    struct PendingClick {
        bool active = false;
        int target = -1;
        int button = -1;
        int mods = 0;
        POINT pt{};
        ULONGLONG tick = 0;
        bool holdFired = false;
        bool down = false;
        bool expectUp = true;
    };

    void BuildBindings();
    void BuildRows();
    bool DispatchMouse(int target, int button, int trigger, int mods);
    bool HasRule(int target, int button, int trigger, int mods) const;
    int CurrentMods() const;
    bool FeedSequence(const std::wstring& token);
    void ShowSequenceHint();
    static std::wstring TokenOf(WPARAM vk);

    HWND main_ = nullptr;
    const Theme* theme_ = nullptr;
    Keymap keymap_;
    std::vector<RuntimeBinding> chords_;
    SequenceMachine seq_;
    SequenceHint hint_;
    std::vector<BindingRow> rows_;

    PendingClick pending_;
    POINT downPt_{};
    bool trackingMove_ = false;
    // 本次按下已经派发过规则（单击立即执行 / 双击）：抬起时不能再让组件跑默认行为，
    // 否则同一次点击会"既执行命令又导航/关闭"。
    bool clickClaimed_ = false;
    // WM_PARENTNOTIFY 没有配对的抬起消息：派发后要立即释放 claim，否则下一次
    // 标签栏点击会被上一次残留的 claim 吞掉默认行为。
    ULONGLONG lastDoubleTick_ = 0;  // 防三击：刚触发双击后的第三击不再算双击
};
