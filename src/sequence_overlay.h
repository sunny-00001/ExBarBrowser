// 序列键总览弹窗：复刻 ExBar mod 的 ExBarSeqOverlay —— 一个自绘、无激活、置顶的
// 输入 + 列表面板。打字过滤、上下导航、回车派发、Esc 收起。
//
// 数据源不在这里：本类只负责 UI 与键盘模型，取数由注入的 provider 完成，
// 派发由注入的 executor 完成（统一回 CommandBus）。这样序列面板、命令面板、
// 未来任何"列表 + 过滤"的入口都能共用同一个弹层，不会各写一套。
#pragma once
#include <cwctype>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "sequence_overlay_layout.h"
#include "theme.h"

namespace seqov {

// 序列键 token：vk → 单字符（字母小写 / 数字原样 / 小键盘数字），其余 = 0。
//
// 【为什么按 vk 而不是按"字符"】序列状态机（SequenceMachine）吃的就是 vk：
// 键位表里的序列写成 "g g"、"g 1"，指的是**物理键**，与当前输入法、
// 键盘布局无关。弹窗里"再敲一个键"必须跟状态机同口径 —— 否则弹窗列出的
// 候选与真正会执行的命令可能不是同一条。
//
// 【为什么不能走 ToUnicodeEx】中文输入法在中文态下会把字母吃进拼音组合，
// ToUnicodeEx 直接返回 0（组合中），只有符号键才正常。实测：弹窗输入框里
// 敲 q/u/e/r/y 什么也不出现，敲 e 还会冒出 "5"（IME 组合态串了）。
// 序列键要的是"键"，所以一律按 vk 取。
inline wchar_t TokenChar(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<wchar_t>(towlower(static_cast<wint_t>(vk)));
    if (vk >= '0' && vk <= '9') return static_cast<wchar_t>(vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
    return 0;
}

}  // namespace seqov

class SequenceOverlay {
public:
    struct Row {
        std::wstring title;    // 主文本（描述：命令中文名）
        std::wstring sub;      // 副文本（组名 / 序列键 / 路径）
        std::wstring command;  // 回车时派发的命令名（可空 = 只导航）
        std::wstring arg;      // 旧式单参数（"3" / "ViewMode:3"）
        std::wstring target;   // 直接导航目标（命令行里由 provider 转成命令）
        // ---- 5 列布局（对齐原版「序号│序列键│描述│命令│分组」）----
        std::wstring key;      // 序列键 token 列（等宽强调色），空 = 不画该列
        std::wstring cmdId;    // 英文命令 id 列（等宽右对齐）
        std::wstring group;    // 中文分组列（最右）
        // ---- 目标列表布局（对齐原版「序号│图标│名称│副标题│分组│触发」）----
        std::wstring subtitle; // 副标题：跟在名称之后（点分名 / 父目录）
        std::wstring trigger;  // 触发键（最右列，如 "Ctrl+Shift+P"）
        std::wstring rightText;// 无触发键时的右列（路径 / 目标）
        // 图标来源路径（空 = 该行不画图标）。
        // 原版口径：命令行 / 帮助行的 target 是分组名/模式名，取图标会回落成
        // 通用文件夹图标（"每条命令挂一个文件夹"），所以这两类恒空；
        // 其余模式（书签/标签/历史/当前目录…）按路径取壳图标。
        std::wstring iconPath;
        // 命令行标记：原版靠 cmdId 非空判断"这是命令"，
        // 高亮且无绑定时在该行显示一次「未绑定」（整屏都写是噪音）。
        bool isCommand = false;
        int modeIndex = -1;    // help 模式：进入该模式
        int data = -1;         // 扩展槽（标签下标等）
    };
    using RowsFn = std::function<std::vector<Row>(const std::wstring& mode,
                                                   const std::wstring& filter)>;
    using ExecFn = std::function<void(const Row&)>;

    struct Mode {
        wchar_t prefix;
        const wchar_t* name;
        const wchar_t* hint;
    };
    static const std::vector<Mode>& Modes();

    // 序列键候选模式名：弹窗在"已敲了序列前缀、等待下一键"时用它取数，
    // 行按 5 列渲染（序号│序列键│描述│命令│分组），与原版一致。
    static const wchar_t* const kSeqKeyMode;

    // 用户唤起态（Ctrl+Shift+P / 命令面板热键）：**模式列表**。
    //
    // 【为什么不是直接列命令】原版 SeqUiUserOpenOnHookThread 唤起时把
    // g_seqMode 置 -1、**不点亮图标** —— 而"图标不在"正是"模式列表"的判据
    // （原版 SeqShowingModeList）。命令列表在原版里是 `!` 前缀进的模式，
    // 不是唤起态的默认页。唤起态敲第一个字符 → 图标点亮 → 序列键候选；
    // 敲模式前缀 → 直接进该模式；回车 → 进入高亮的那个模式。
    static const wchar_t* const kUserMode;

    // 行是否走 5 列布局（序列键候选专用）。
    // 【判据只认「序列键」与「命令 id」两列】分组列在目标列表里也存在，
    // 拿它当判据会把命令列表误判成 5 列（实测：命令面板只画出序号/描述/分组，
    // 副标题与触发键两列消失）。
    static bool IsWideRow(const Row& r) {
        return !r.key.empty() || !r.cmdId.empty();
    }

    void Init(HWND owner, const Theme& theme, RowsFn rows, ExecFn exec);
    // 序列键前缀回灌：返回 true 表示宿主已处理（唯一匹配 → 执行并关闭弹窗）。
    void SetSeqKeyFeed(std::function<bool(const std::wstring&)> fn) { seqKeyFeed_ = std::move(fn); }
    void ReloadTheme();
    void Open(const std::wstring& modeName = std::wstring());
    // 打开序列键候选模式：prefix = 已敲入的序列前缀（如 "g"），
    // 行取自 RowsFn(kSeqKeyMode, prefix)。与原版一致：敲 g 后弹窗列出 g* 全部序列。
    void OpenSeqKeys(const std::wstring& prefix);
    // 序列键模式下继续敲下一个键（累积前缀并刷新候选）。
    void FeedSeqKey(wchar_t ch);
    void Close();
    bool visible() const { return visible_; }
    // 钉住态：超时**既不关窗也不自动执行**（原版 HandleSequenceTimeout 分支）。
    bool pinned() const { return pinned_; }
    // 执行后的收尾（原版 SeqAfterExecute）：钉住态只清空输入框、弹层常驻；
    // 未钉住照旧关闭。
    void AfterExecute();
    // 清空输入区（回 S0）：清掉过滤串、选中与滚动。
    void ClearBuffer();
    // 当前是否处于序列键候选模式（供宿主决定键的归属）。
    bool SeqKeyMode() const { return mode_ == kSeqKeyMode; }
    // 当前是否处于用户唤起态（模式列表）：符号前缀（! / ? ~ …）在这里生效。
    bool UserMode() const { return mode_ == kUserMode; }

    // 返回 true = 该键已被弹窗消费（主窗口不应再处理）。
    //
    // 【键的两类归属】
    //   · 特殊键（Esc/方向/回车/退格）在这里处理；
    //   · **键模式**（用户唤起态 / 序列键候选）下的字母数字 = "下一个序列键"，
    //     按 vk 取（seqov::TokenChar，与序列状态机同口径）；
    //   · 其余键返回 false → 宿主按"输入框打字"处理（字符现译，不经输入法）。
    bool HandleKey(WPARAM vk, LPARAM keyInfo);
    // 字符入口：文本模式（命令/书签/搜索…）进过滤串；键模式只用来判模式前缀。
    //
    // 【两个来源，同一个函数】
    //   · 按键层：宿主把没被 HandleKey 吞掉的键现译成字符送来
    //     （chord::CharOfVk，US 布局确定性映射 —— 不经输入法，
    //      与原版 exbar.cpp「钩子吞键 + 现译字符」同款）；
    //   · WM_CHAR 层：输入法提交组合后的字符（中文等）也能进过滤串。
    // 键模式不吃文本字符 —— 序列键是"键"不是"字"（见 HandleKey 的说明）。
    bool HandleChar(WPARAM ch);

    // 滚轮滚动列表：一次滚轮 3 行（原版 SeqApplyWheel 同款）。
    // 返回 true = 弹窗处理了这次滚动（调用方不要再把滚轮转给后面的视图）。
    //
    // 【为什么要单独一个入口】滚轮消息只会发给**焦点窗口**，而弹窗是
    // WS_EX_NOACTIVATE 的置顶浮窗 —— 它拿不到焦点，于是滚轮永远到不了它，
    // 滚的是弹窗**背后**的文件视图（实测：光标在弹窗上滚，列表纹丝不动）。
    // 现在有两条路都汇到这里：
    //   · 宿主把 WM_MOUSEWHEEL 转发过来（光标在弹窗上时）；
    //   · 弹窗自己收原始输入（WM_INPUT，RIDEV_INPUTSINK）—— 弹窗不激活
    //     也能收到（原版 exbar.cpp 的 WM_INPUT 兜底同款）。
    // 两条路会在同一次物理滚动里各来一次，靠 50ms 去重挡掉重复。
    bool Wheel(int delta);

    // 光标是否落在弹窗的**列表区**（屏幕坐标）——滚轮分流用。
    // 输入区（底部）不算：那儿滚轮不滚列表（原版 rcList 同款）。
    bool ListHitScreen(POINT screen) const;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    // 绘制在 sequence_overlay_paint.cpp：这里只管生命周期与状态。
    void Paint(HWND);
    void Refresh();
    void Move(int delta);
    void Commit();
    void SetMode(const std::wstring& name);
    void Resize();
    // 输入区（列表下方，Listary 风格）几何与命中。唯一来源，绘制/点击共用。
    RECT InputRect() const;
    RECT ClearBtnRect() const;
    RECT PinBtnRect() const;
    // 按钮命中：返回 0 = 清空 / 1 = 钉住 / -1 = 没落在按钮上。
    int HitBtn(POINT pt) const;
    void OnLButtonDown(POINT pt);
    void OnLButtonDblClick(POINT pt);
    void OnMouseMove(POINT pt);
    // 当前行的悬停态（-1 = 无）。
    int HoverRow(POINT pt) const;
    // 可见行数（= min(总行数, kMaxRows)）；总行数不足一行时保底一行（空态落点）。
    int ViewRows() const;
    // 序列键列宽按**全部**序列键里最长 token 算（与过滤结果无关，列宽不跳变）。
    void RecalcKeyChars();
    // 滚动后按**当前光标位置**重算悬停行：光标不动、行在动，不重算就会让
    // 高亮停在原索引上（原版 SeqRefreshHover 同款）。
    void RefreshHover();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    const Theme* theme_ = nullptr;
    RowsFn rowsFn_;
    ExecFn execFn_;
    std::function<bool(const std::wstring&)> seqKeyFeed_;
    std::vector<Row> all_;
    std::vector<Row> filtered_;
    std::wstring mode_;
    std::wstring filter_;
    size_t selected_ = 0;
    size_t scroll_ = 0;
    int hover_ = -1;          // 悬停行（-1 = 无）
    int keyChars_ = 1;        // 全部序列键里最长 token 的字数（序列键列宽）
    bool pinned_ = false;     // 钉住：弹层常驻，不随序列超时自动收起
    bool visible_ = false;
    int width_ = 0;
    int height_ = 0;
    DWORD lastWheelMs_ = 0;   // 滚轮去重（同一次物理滚动 WM_MOUSEWHEEL + WM_INPUT 双发）
    seqov::Metrics metrics_{};  // 按 DPI 缩放后的几何
};
