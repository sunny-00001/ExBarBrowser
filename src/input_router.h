// 输入端总路由：键盘组合键 / 序列键 / 鼠标规则。
//
// 一条纪律：本模块**只发命令**，不改任何业务状态（Tab/导航/书签都由命令处理者负责）。
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "chord.h"
#include "common.h"
#include "input_config.h"
#include "key_binding.h"
#include "keymap.h"
#include "mouse_rules.h"
#include "sequence.h"
#include "sequence_hint.h"
#include "sequence_overlay.h"
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
    // 「这一行的键是从哪来的」——快捷键管理器的"来源"列要它。
    // 只有 custom / unbound 两个派生位，不足以反推出"默认键"：默认值可能是
    // 键位表里的，也可能是 SystemKeyFor 兜底出来的，界面不该分这两者
    // （用户看到的都是"默认"），但**必须**能分出"我改过 / 我清过"。
    bool custom = false;   // 覆盖表里有这个 setting 且值不是解绑哨兵
    bool unbound = false;  // 覆盖表里是解绑哨兵（用户点了"清除"）
    // 系统原生兜底键（keymap::SystemKeyFor）。空 = 这条命令没有系统原生键。
    // 只用于右栏"默认：Ctrl+L"的说明，不参与"按得出来"的判定（那是 Effective 的事）。
    std::wstring nativeKey;
};

class InputRouter {
public:
    // cfg 由 App 注入（转发到 Config），输入端不再直接依赖 config 单例（R15）。
    void Init(HWND main, const Theme& theme, InputConfig& cfg);
    void Reload();  // 键位覆盖 / 鼠标规则变化后调用

    // 序列总览弹窗（Ctrl+Shift+P / 命令面板）：取数与派发由宿主注入，
    // 本模块只负责把键盘消息交给弹窗（保持"输入端只发命令"的纪律）。
    void InitSequenceOverlay(SequenceOverlay::RowsFn rows, SequenceOverlay::ExecFn exec);
    void ShowSequenceOverlay(const std::wstring& mode = std::wstring());
    // 序列键候选（5 列弹窗的数据源）：全部序列绑定（弹窗自己按前缀筛）。
    const std::vector<SequenceBinding>& Sequences() const { return keymap_.sequences; }
    // 模式前缀键（/ ! # @ …）在弹窗外也能唤起，回调由 App 注入（统一开弹窗入口）。
    void SetOverlayModeOpener(std::function<void(const std::wstring&)> fn) {
        modeOpener_ = std::move(fn);
    }

    // WebView2/AI 面板宿主：焦点落在它或它的子窗口里时按"文本输入区"处理。
    // 与 kbhook::SetTextEntryRoot 由 App 在同一个地方成对注册。
    void SetTextEntryRoot(HWND root) { textEntryRoot_ = root; }

    // 键盘：返回 true = 已消费。
    // keyInfo 是 WM_KEYDOWN 的 lParam（用 bit30 识别 auto-repeat：按住键不该连发序列）。
    // 故意不给默认值：漏传 lParam 会让 auto-repeat 守卫静默失效，宁可编译报错。
    //
    // 【modsOverride】修饰键来源。低层钩子路径（kbhook）传的是**事件流推导**的修饰键
    // （>= 0），比 GetKeyState 可靠 —— 按键消息是异步投递的，等 UI 线程处理时
    // 物理按键可能已经松开，GetKeyState 会读到"修饰键已放开"的错状态。
    // 传 -1 = 用 GetKeyState（普通 WM_KEYDOWN 路径）。
    bool HandleKey(WPARAM vk, HWND focus, LPARAM keyInfo, int modsOverride = -1);

    // 字符输入：返回 true = 已消费（主窗口不应再处理）。
    //
    // 【谁会送来字符】两条路，同一个入口：
    //   · 按键层：弹窗开着时，宿主把没被弹窗吞掉的键现译成字符送来
    //     （chord::CharOfVk，US 布局确定性映射，不经输入法）；
    //   · WM_CHAR：输入法提交组合后的字符（中文等）。
    //
    // 【为什么不用 ToUnicodeEx 翻字母】中文输入法在中文态下会把字母吃进
    // 拼音组合，ToUnicodeEx 直接返回 0（组合中）—— 实测弹窗过滤框里敲字母
    // 没反应、敲 e 还会冒出 "5"，还会把桌面级输入法状态卡死。
    bool HandleChar(wchar_t ch);

    // 滚轮（WM_MOUSEWHEEL）：返回 true = 已消费（主窗口不应再处理）。
    //
    // 【为什么单独一个入口】滚轮消息只发给**焦点窗口**，弹窗是
    // WS_EX_NOACTIVATE 的置顶浮窗、拿不到焦点 —— 以前滚轮落到文件视图上，
    // 弹窗列表纹丝不动。宿主在消息循环里先截住，光标在弹窗上时滚弹窗，
    // 背后视图不再跟着滚。
    bool HandleWheel(UINT msg, WPARAM wParam, LPARAM lParam);

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
    // 把一条命令的键位**还原成默认**：删掉它的覆盖项，而不是写空串/哨兵。
    // 【为什么必须删而不是写空】写空串会被 ResolveKey 原样返回，BuildBindings 见到空键
    // 就跳过 —— 那条命令反而**按不出来**了，与"恢复默认"正好相反。写哨兵更糟：
    // 哨兵是"显式解绑"，是用户主动清掉的语义。只有删掉覆盖项才会回落到
    // keymap.json 的默认值 / 系统原生键。
    bool ResetKeyForCommand(const std::wstring& command);
    void ResetDefaults();
    std::wstring StatusText() const;

    // 菜单行右端的快捷键文案（唯一取数口，给 ui::Menu 的解析器用）。
    //
    // 【为什么从这里出】菜单要显示"按得出来的键"，而"哪些键真能触发"只有本模块
    // 知道（用户覆盖、保留键剔除、先到先得都在 Reload 里算过）。所以派生逻辑
    // 收在 keybinding::Effective 一处，这里只是把它接到当前已加载的键位表上 ——
    // 不在菜单侧另推一套，否则显示的键与实际生效的键迟早分叉。
    //
    // spec 是**命令规格**（"tab.close" / "view.mode:3"）。返回空串 = 没有快捷键，
    // 菜单那一行右端留空（业界口径：有就画、没有就空，不画占位符）。
    std::wstring ShortcutText(const std::wstring& spec) const;

    // 按**命令身份**取文案 —— 菜单的主入口（见 menu.h 的 CmdId 说明）。
    // 名字由 FindCommand(id) 反查，再走同一个 ShortcutText。两条入口共用一份
    // 推导（keybinding 唯一实现），不存在"按名字查"和"按身份查"两套结果。
    std::wstring ShortcutText(CmdId id, const std::wstring& arg = std::wstring()) const;

private:
    enum {
        kSingleTimer = 0x5101,
        kHoldTimer = 0x5102,
        kSeqTimer = 0x5103,
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
    // 焦点是否在宿主登记的文本输入区（当前 = AI 面板的 WebView2 子树）。
    bool InTextEntry(HWND focus) const;
    // 弹窗开着时的键盘归属（序列 + 弹层作用域，见 input_router_overlay.cpp）。
    bool HandleOverlayKey(WPARAM vk, LPARAM keyInfo, int mods);
    // 序列键：把虚拟键码转成序列 token（字母/数字/小键盘）。
    static std::wstring TokenOf(WPARAM vk);
    // 喂一个序列 token 给状态机（pending → 弹候选；matched → 执行）。
    bool FeedSequence(const std::wstring& token);
    // 打开序列候选弹窗（5 列 + 输入区）。
    void ShowSequenceHint();
    // 弹窗回灌的序列前缀（"g" / "gg"）：重跑状态机判定唯一匹配。
    // 返回 true = 已唯一匹配并执行（弹窗应关闭）。
    bool SeqKeyFeed(const std::wstring& prefix);

    HWND main_ = nullptr;
    HWND textEntryRoot_ = nullptr;
    const Theme* theme_ = nullptr;
    InputConfig* cfg_ = nullptr;  // App 注入的配置边界（R15）
    Keymap keymap_;
    // 生效组合键（来自 keybinding::Effective —— 唯一推导处）。
    std::vector<keybinding::Binding> chords_;
    SequenceMachine seq_;
    SequenceHint hint_;
    SequenceOverlay overlay_;
    std::function<void(const std::wstring&)> modeOpener_;
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
