// WebView2 的最小 C++ 绑定（只做 AI 面板需要的那几个调用）。
//
// 【为什么手写而不用 Microsoft 的 WebView2.h】
//   1. AGENTS.md §1.2 要求"新依赖必须先证明自己写 200 行解决不了"。
//      这里只需要 5 个方法（创建环境 / 创建控制器 / 取 CoreWebView2 /
//      导航 / 设 Bounds），手写 vtable 约 200 行就够，不必引入 SDK 头。
//   2. 官方 SDK 的 WebView2.h 是 2 万行的 IDL 产物，只用 5 个方法却要
//      把整份接口定义搬进仓库，可读性反而更差。
//   3. WebView2Loader.dll 是**运行时**加载（LoadLibrary），不进链接期依赖，
//      所以 exe 本身仍可零 WebView2 依赖启动 —— 没有运行时只是面板不可用。
//
// 【vtable 纪律】COM 靠方法**顺序**寻址，写错一个槽位就调到别的函数上，
//   表现为随机崩溃。下面每个 vtable 都按官方 IDL 的声明顺序完整列出到
//   最后一个被用到的方法为止；未用到的后续方法省略（不会调到）。
//   参考：learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/
//
// 【线程】所有调用必须在创建它的那个 UI 线程上。WebView2 是 STA，
//   跨线程调用会得到 RPC_E_WRONG_THREAD。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace aiui {

// COM 接口在这个绑定层里**不暴露给调用方** —— 外部只需要「启动/改尺寸/显示/关闭」。
// 内部实现用自己声明的 vtable（见 .cpp）。这里只留不透明句柄类型，
// 免得把 25 个槽位的接口定义塞进公共头。
class WebViewImpl;
struct AiWebStatus;

// 版本号探测：运行时没装返回 false（reason 给可读原因）。
bool RuntimeAvailable(std::wstring* version, std::wstring* reason);

// 面板里那个 WebView2。生命周期由调用方持有；所有方法只在 UI 线程调。
class WebView {
public:
    WebView();
    ~WebView();
    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;

    // 在 parent 里异步创建并导航到 url。返回 false = 立即失败（原因见 Reason()）。
    // 创建是异步的：成功后 WebView2 会自己作为子窗口挂到 parent 上。
    bool Start(HWND parent, const std::wstring& url, const std::wstring& userDataDir);

    // 子窗口矩形（相对 parent 客户区）。创建完成前调用是安全的（记下来，稍后落地）。
    void SetBounds(const RECT& rc);

    // 隐藏/显示（面板收起时用 hide 而不是 Stop —— Stop 会销毁浏览器进程，
    // 再展开要重新启动整个 WebView2，约 1 秒白屏）。
    void SetVisible(bool visible);

    // 主窗口移动/跨屏后通知 WebView2 刷新父窗口位置缓存（tooltip/无障碍坐标）。
    void NotifyParentMoved();

    // 彻底销毁。幂等。
    void Stop();

    // 深浅色主题：随时可设。控制器就绪前只记下来，就绪/页面加载后通过
    // WebMessage 推给页面；首次导航还会在 URL 上带 exbarTheme，避免消息比
    // 页面监听器更早到达导致首帧闪深色。
    void SetTheme(bool dark);

    // 控制器就绪（可以 put_Bounds/导航）时回调一次。面板用它把真实客户区
    // 尺寸推给 WebView2 —— 控件常在宿主 0×0 时发起创建，必须在就绪后补一次。
    void SetReadyCallback(std::function<void()> cb);

    // 异步失败也会走到这里：Start() 返回 true 只代表"已发起"，环境/控制器
    // 的真正失败在回调里。面板必须收到它才能把"创建中"改成具体错误。
    void SetErrorCallback(std::function<void(const std::wstring&)> cb);

    // 页面发来的窗口动作（已通过来源与白名单校验）。宿主负责决定怎么执行；
    // 面板是子窗口，具体动作映射放在 App 层，避免 WebView 绑定知道主窗口结构。
    void SetWindowActionCallback(std::function<void(const std::wstring&)> cb);

    // 页面发来的 AI 状态（已通过来源与状态白名单）。状态栏 chip / peek 用。
    void SetStatusCallback(std::function<void(const AiWebStatus&)> cb);

    bool Ready() const;
    const std::wstring& Reason() const;

    // 供实现内部使用（完成回调需要落地结果）。外部不要调。
    void OnEnvironmentReady(IUnknown* env, HRESULT hr);
    void OnControllerReady(IUnknown* ctrl, HRESULT hr);

private:
    void ApplyBounds();

    WebViewImpl* impl_ = nullptr;
};

}  // namespace aiui