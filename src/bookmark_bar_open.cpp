// 书签栏的「打开」语义：中键 / Ctrl / Shift / 「固定在新标签打开」开关。
//
// 【为什么单独成文件】打开语义是一族**策略**（修饰键覆盖 + 一个配置开关 +
// 分类整组打开），与栏的绘制、命中、拖拽是不同职责。放在 bookmark_bar.cpp
// 的 WM_LBUTTONUP 分支里，那段 switch 会同时承担"拖拽提交/图标格/分类菜单/
// 导航"四件事，再加三种修饰键组合就没人读得动。
//
// 【业界依据（2026-11-27 调研，Chrome / Edge / Firefox 三家一致）】
//   中键             → 新标签打开（后台，不打断当前视图）
//   Ctrl + 左键      → 新标签打开
//   Shift + 左键     → 新窗口打开
//   固定新标签开关   → Edge 110+ 的「Always open favorites in new tab」原生设置
// 这四条是浏览器书签栏的肌肉记忆。本项目此前**一条都没有**：点哪都只进当前
// 标签，想"顺手开一个新标签"只能先 Ctrl+T 再点 —— 用户会以为功能坏了。
//
// 【修饰键必须能反向覆盖开关】开了「固定新标签」之后，Ctrl+左键 反而回当前
// 标签。理由是：开关是给"我从来都要新标签"的人设的默认值，而修饰键是**这一次**
// 的显式意图。只让开关单向生效的话，开了它就再也无法原地打开，用户只能去
// 设置里来回改 —— 那不是默认值，是死锁。
#include "bookmark_bar.h"

#include "commands.h"
#include "config.h"

namespace {

// 修饰键位（与 mouse_rules::CurrentMods 同口径：1=Ctrl 2=Shift 4=Alt）。
constexpr int kModCtrl = 1;
constexpr int kModShift = 2;

// 一次"打开一个路径"的派发：三条路径（当前标签 / 新标签 / 新窗口）各对应一条
// 既有命令，参数键统一是 path（新标签多一个 background）。不自己开标签/窗口 ——
// 那会把审计、标签位置策略、窗格归属全都复制一遍。
bool InvokeOpen(CmdId id, const std::wstring& path, bool background = false) {
    if (path.empty()) return false;
    json::Value a = json::Value::MakeObject();
    a.Set(L"path", json::Value::MakeStr(path));
    // background 只有 fs.open.tab 认；其余命令的 schema 是 additionalProperties:false，
    // 多塞一个键会被拒，所以按命令分别构造。
    if (id == CmdId::FsOpenInNewTab && background)
        a.Set(L"background", json::Value::MakeBool(true));
    return CommandBus::Get().Invoke(id, Args::FromJson(a), Actor::Ui());
}

}  // namespace

// 叶子书签的修饰键覆盖。返回 true = 已按语义打开（调用方直接返回）。
//
// 判据顺序（先修饰键、后开关）是刻意的：Ctrl/Shift 是**这一次**的显式意图，
// 必须压过配置里的默认值。反过来（先看开关再看修饰键）会让"开着开关的人
// 永远无法原地打开"，见文件头的死锁说明。
bool BookmarkBar::ConsumeLeafClickMods(const Item& it, int mods) {
    if (it.path.empty()) return false;   // 纯分类没有可打开的路径
    const bool fixedNewTab = Config::Get().s().bookmarkOpenInNewTab;
    // Shift 优先于 Ctrl：两家同时按下时用户多半是想"新窗口"（更强的那一个），
    // 而 Ctrl+Shift+左键 在浏览器里也确实是新窗口。
    if (mods & kModShift) {
        if (InvokeOpen(CmdId::FsOpenInNewWindow, it.path) && refocus_) refocus_();
        return true;
    }
    // Ctrl 与「固定新标签」互为反向：一个强制新标签，一个强制当前标签。
    const bool wantNewTab = (mods & kModCtrl) ? !fixedNewTab : fixedNewTab;
    if (!wantNewTab) return false;   // 走默认（当前标签导航）
    // 修饰键/开关触发的新标签一律**前台**：用户明确表达了"要开"，
    // 开完停在后台会让人以为没生效（Edge 的开关也是前台）。
    if (InvokeOpen(CmdId::FsOpenInNewTab, it.path) && refocus_) refocus_();
    return true;
}

// 中键：书签 → 后台新标签；分类 → 整组子项各开一个后台标签。
// 后台是刻意的（Chrome/Firefox 中键语义）：中键的价值就是"不打断当前视图"，
// 批量打开一整个分类时更是如此 —— 前台连开十个标签会把用户当前工作冲掉。
bool BookmarkBar::OnMiddleDown(int x, int y) {
    const int idx = HitItem(x, y);
    if (idx < 0) return false;
    const Item it = items_[static_cast<size_t>(idx)];   // 值拷贝：下面会进命令总线
    if (it.folder) {
        OpenFolderChildrenInTabs(it.id, /*background=*/true);
        return true;
    }
    InvokeOpen(CmdId::FsOpenInNewTab, it.path, /*background=*/true);
    return true;
}

// 按**路径**打开，带修饰键语义。这是打开动作的**唯一收口点**。
//
// 【为什么必须收口】第二轮把修饰键语义写成 ConsumeLeafClickMods，但只接在
// WM_LBUTTONUP 一处。于是同一个书签出现了三种行为：
//   · 栏上点（带 Ctrl）→ 新标签
//   · » 溢出菜单里点 → Navigate(idx)，Ctrl 无效
//   · 分类弹层里点 → onNavigate_，Ctrl 无效且绕过命令总线（无审计）
// 这不是"漏改一行"，而是"一个语义有三个触发点，只改了一个"。收口之后，
// 三个入口都调这一个函数，将来加第四个入口也不会再分叉。
//
// 返回 true = 已按某种语义打开（调用方不要再导航）。
bool BookmarkBar::OpenPathWithMods(const std::wstring& path, int mods) {
    if (path.empty()) return false;
    // 修饰键判据与 ConsumeLeafClickMods 共用同一份逻辑：这里构造一个只带
    // path 的临时 Item 复用，而不是把判据抄第二遍（抄一遍就多一个分叉点）。
    Item probe;
    probe.path = path;
    return ConsumeLeafClickMods(probe, mods);
}

// UIA 的"激活"：真的打开这一项。
//
// 【为什么不能复用 kMsgSelect】UIA 规范里 SelectionItem.Select 是"选中"
// （焦点/选中态移过去），Invoke.Invoke 是"激活"（执行默认动作）。provider
// 原先两个方法都发 kMsgSelect，于是读屏用户"激活一个书签"只会看到焦点跳过去、
// 书签没打开 —— 无障碍入口的打开动作是坏的（E23）。
//
// 【为什么放在这里】它就是"打开一个书签"的又一个触发点（第七个），
// 必须与鼠标左键走同一条派发，否则又是一处行为分叉。
void BookmarkBar::UiaInvoke(int index) {
    const int n = static_cast<int>(items_.size());
    if (index < 0 || index >= n) return;
    const Item it = items_[static_cast<size_t>(index)];   // 值拷贝：下面会进命令总线
    // 先让键盘游标对齐（读屏软件激活后紧接着按方向键，应该从这一项继续走）。
    kbdIdx_ = index;
    EnsureVisible(index);
    if (it.folder) {
        // 分类的"默认动作"是展开它的内容，与左键点分类一致。
        POINT at{it.rc.left, it.rc.bottom};
        if (hwnd_) ClientToScreen(hwnd_, &at);
        ShowFolderMenu(index, at);
        return;
    }
    if (!OpenPathWithMods(it.path, /*mods=*/0)) Navigate(index);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// 分类的「打开全部」：把该分类下所有**可打开的叶子**各开一个标签。
//
// 【为什么按递归收集、而不是只取直接子项】浏览器的 "Open all" 打开的是整个
// 子树（Firefox 的 "Open All in Tabs" 会连嵌套书签夹一起开）。只取直接子项
// 会让"分类里再分类"的人点完发现少了一半。
//
// 【跳过什么】纯分类（没有 path）、空 path 的叶子。分类本身不是一个可打开的
// 位置，硬开只会失败并弹一串错误。
//
// 【为什么不用 store.Flatten()】那是全库拍平（含所有分类），拿不到"某一棵
// 子树"的边界。这里自己走一遍，语义就在调用点上，不用去猜 Flatten 的范围。
int BookmarkBar::OpenFolderChildrenInTabs(const std::wstring& folderId, bool background) {
    const BookmarkNode* node = BookmarkStore::Get().Find(folderId);
    if (!node || !node->isFolder) return 0;
    // 先收集再派发：命令总线会在 Invoke 内部刷新书签栏（Reload → BuildItems），
    // 边遍历 node->children 边派发就是在被释放的 vector 上走（UAF）。
    std::vector<std::wstring> paths;
    // 深度上限与 bookmark_category_popup / CollectMoveTargets 同口径（32 层）：
    // 数据被外部改坏成自引用时，没有这道闸就是栈溢出。
    struct Walk {
        static void Run(const BookmarkList& list, int depth, std::vector<std::wstring>* out) {
            if (depth > 32) return;
            for (const BookmarkNode& n : list) {
                if (n.isFolder) {
                    Run(n.children, depth + 1, out);
                } else if (!n.path.empty()) {
                    out->push_back(n.path);
                }
            }
        }
    };
    Walk::Run(node->children, 0, &paths);
    if (paths.empty()) {
        CommandBus::Get().NoteFailure(L"这个分类里没有可打开的书签");
        return 0;
    }
    int opened = 0;
    for (const std::wstring& p : paths) {
        if (InvokeOpen(CmdId::FsOpenInNewTab, p, background)) ++opened;
    }
    LogDbg(L"[bmk] 打开全部：分类 id=%ls 共 %zu 项，成功 %d", folderId.c_str(), paths.size(),
           opened);
    // 批量打开后把焦点还给视图：焦点留在书签栏上会让新开的标签收不到键盘。
    if (opened > 0 && refocus_) refocus_();
    return opened;
}
