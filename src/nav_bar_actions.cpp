// 导航栏的交互动作：地址编辑 / Tab 补全 / 自绘下拉菜单
// （从 nav_bar.cpp 拆出，保持单文件 <=500 行）
#include "nav_bar.h"
#include "str.h"

#include "folder_tree.h"
#include "icon_resolve.h"
#include "menu.h"
#include "popup_toggle.h"
#include "text_file.h"
#include "view_modes.h"

namespace {

// "此电脑"的 CLSID 不再在这里重复定义：唯一来源是 nav_path.h 的 nav::kThisPc。

// 树节点路径图标走 ico::Cached：与菜单路径行同一份进程级缓存，
// 弹层只借不销毁；特殊命名空间（此电脑/回收站/云存储）仍由 ico 分层解析拿准确图标。
HICON IconFor(const std::wstring& path) {
    return ico::Cached(path, true);
}

// 历史栈/最近访问只存解析串；label 必须走统一显示名入口，不能把 path 直接给人看。
// 普通路径仍显示完整路径（与资源管理器一致），"::{GUID}" 显示此电脑/回收站等本地化名。
std::wstring LabelFor(const std::wstring& path) {
    return ico::DisplayName(path);
}

// 视图档位表在 view_modes.h（唯一来源）。这里原来有一份逐字相同的副本，
// 而两份表的**下标**同时被菜单回传与配置持久化使用 —— 改一处忘另一处就是
// "菜单显示的和实际切的不一致"，且无任何报错。
inline constexpr const viewmode::ViewMode* kViews = viewmode::kModes;
constexpr int kViewCount = viewmode::kCount;

}  // namespace

void NavBar::BeginEdit() {
    if (!edit_ || editing_) return;
    editing_ = true;
    SetWindowTextW(edit_, model_.AddressText().c_str());
    ShowWindow(edit_, SW_SHOW);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void NavBar::EndEdit(bool refocus) {
    if (!editing_) return;
    editing_ = false;
    tabMatches_.clear();
    tabIdx_ = -1;
    tabBase_.clear();
    if (refocus) {
        HWND main = hwnd_ ? GetAncestor(hwnd_, GA_ROOT) : nullptr;
        if (refocus_) refocus_();
        else if (main) SetFocus(main);  // 没接回调时至少别把焦点留在隐藏的 EDIT 上
    }
    if (edit_) ShowWindow(edit_, SW_HIDE);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring NavBar::AddressOfEdit() const {
    if (!edit_) return std::wstring();
    int n = GetWindowTextLengthW(edit_);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit_, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}

void NavBar::CommitEdit() {
    if (!editing_) return;
    // 用户输入 → 路径，两步都是**纯语义**，都在唯一入口里：
    //   1. nav::FromUserText —— 剥引号 / 去首尾空白 / 正斜杠归一。
    //      "资源管理器 → 复制为路径"给的是带引号的 `"C:\Program Files"`，
    //      直接丢给 shell 会 E_INVALIDARG（症状："粘贴路径回车没反应"）。
    //   2. ico::ParsingPath —— 友好名 → 解析串。命名空间（此电脑/回收站）在
    //      地址栏里显示的是**友好名**，用户回车交回来的也是它；
    //      而 SHCreateItemFromParsingName("此电脑") 必然失败。
    //      这条以前一直缺，所以地址栏里对命名空间回车是死的。
    std::wstring text = nav::FromUserText(AddressOfEdit());
    EndEdit(true);  // 先还焦点：后续导航/弹框都不该让键盘停在 EDIT 上
    if (text.empty() || text == model_.AddressText()) return;
    text = ico::ParsingPath(text);
    if (onNavigate_) onNavigate_(text);
}

bool NavBar::DoComplete() {
    if (!edit_) return false;
    // 补全也要先过一遍用户输入清理（带引号的粘贴文本补不出来是同一类问题）
    std::wstring text = nav::FromUserText(AddressOfEdit());
    if (text != tabBase_) {
        tabBase_ = text;
        tabMatches_.clear();
        tabIdx_ = -1;

        // 切分口径唯一实现在 nav::SplitForComplete：按**最后一个反斜杠**切。
        // 以前用 ParentOf + Leaf，盘根 `D:\` 与 UNC 半截 `\\server\sh` 上
        // ParentOf 都返回空 → 回退到"当前目录 + 整串前缀" → 一个候选都没有，
        // Tab 在用户最需要它的两处（换盘、敲共享名）变成死键。
        const nav::CompleteSplit cut = nav::SplitForComplete(text);
        if (!cut.completable) return false;
        const std::wstring dir = cut.dir.empty() ? model_.path() : cut.dir;
        const std::wstring& prefix = cut.prefix;

        std::vector<std::pair<std::wstring, std::wstring>> kids;
        NavModel::ChildFolders(dir, kids);
        for (const auto& kv : kids) {
            if (prefix.empty() || str::IStartsWith(kv.first, prefix))
                tabMatches_.push_back(kv.first);
        }
        if (tabMatches_.empty()) return false;
    }
    if (tabMatches_.empty()) return false;

    tabIdx_ = (tabIdx_ + 1) % static_cast<int>(tabMatches_.size());
    // 拼回完整路径：目录部分（可能为空 → 用当前目录） + 命中的名字。
    // 目录部分本身已带尾反斜杠（SplitForComplete 保证），不再补。
    std::wstring out = nav::SplitForComplete(tabBase_).dir;
    if (out.empty()) {
        out = model_.path();
        if (!out.empty() && out.back() != L'\\') out += L'\\';
    }
    out += tabMatches_[static_cast<size_t>(tabIdx_)];

    SetWindowTextW(edit_, out.c_str());
    int len = static_cast<int>(out.size());
    SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    return true;
}

void NavBar::ShowChildMenu(int partIndex, POINT screen) {
    (void)screen;
    const auto& parts = model_.parts();
    if (partIndex < 0 || partIndex >= static_cast<int>(parts.size())) return;

    // 分隔符归属于它**左边**那一级：列该级自己的直接子文件夹。
    // 例：D:\Windhawk 右侧的 › 列 Windhawk 的内容，而不是 D:\ 下的同级目录
    // （与 mod ShowCrumbMenu 的 EnumChildFoldersAt(crumbPidls[partIndex]) 同口径）。
    const std::wstring target = parts[static_cast<size_t>(partIndex)].path;
    if (target.empty()) return;

    // 顶层 = 该段的直接子文件夹；子节点由弹层按需懒加载、就地内联展开
    // （枚举与展开都走 folder_tree，与书签栏图标点击同一份实现）
    std::vector<TreeNode> top = folder_tree::Children(target);
    if (top.empty()) {
        // 没有子目录：**别弹空菜单，也别什么都不做**。
        // 以前这里直接 return —— 用户点了分隔符，箭头闪都不闪、列表不出、也不导航，
        // 看起来就是"这个三角是坏的"。与书签栏 ShowChildTree 同口径：
        // 确认是目录才直接进入（弹空菜单等于点了没反应）；命名空间项不动。
        if (onNavigate_ && tf::IsDirectory(target)) onNavigate_(target);
        return;
    }

    // 对齐到该段的 chevron 区（不是鼠标点），否则菜单会"飘"在鼠标位置
    int k = partIndex - firstShown_;
    RECT anchor =
        (k >= 0 && k < static_cast<int>(chevRects_.size())) ? chevRects_[static_cast<size_t>(k)]
                                                            : fieldRc_;
    POINT at{anchor.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);

    // 锚点键带段索引：不同级的 chevron 是不同锚点，不能互相干扰。
    const std::wstring toggleKey = L"nav.crumb." + std::to_wstring(partIndex);

    // 【先判再置位】弹层若还开着（= 用户点的是"收起"），立刻把箭头恢复 ›、不弹列表。
    //
    // 正常情况下这一支根本走不到 —— 关掉弹层的那一下点击已经被弹层自己吃掉了
    // （见 popup_list.cpp 的捕获模型），锚点压根收不到它。这里留着是为了
    // 键盘（Ctrl+L 后回车）/ 命令面板等**不经鼠标**的重入路径。
    // 判据是"同键弹层此刻是否开着"，不是时间窗。
    if (popup::IsOpen(toggleKey.c_str())) {
        openChev_ = -1;
        hotChev_ = -1;
        if (hwnd_) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
        return;
    }

    // 分隔符切到展开态「⌄」并立即重绘（模态菜单弹出前，用户能看到形态变化）
    openChev_ = partIndex;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
    // 点击 folder 行 chevron 内联展开子树（与 exbar.cpp treeMode 一致），
    // 节点 icon 借自 ico::Cached，弹层只借不销毁。
    std::wstring picked =
        folder_tree::Show(hwnd_, *theme_, at, std::move(top), 0, toggleKey.c_str());
    // 与 mod 28756-28758 一致：恢复 › 的同时清掉悬停态，避免光标下高亮残留
    openChev_ = -1;
    hotChev_ = -1;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
    if (!picked.empty() && onNavigate_) onNavigate_(picked);
}

void NavBar::ShowSegmentMenu(int partIndex, POINT screen) {
    (void)screen;
    const auto& parts = model_.parts();
    if (partIndex < 0 || partIndex >= static_cast<int>(parts.size())) return;
    const std::wstring target = parts[static_cast<size_t>(partIndex)].path;
    if (target.empty()) return;
    // 幽灵段（向上导航后被放弃的更深分支）是"历史痕迹"，不是可操作目标：
    // 它对应的目录可能已经被删了，右键菜单里的"属性/终端"会立刻失败。
    if (parts[static_cast<size_t>(partIndex)].ghost) return;

    // 菜单作用于**用户右键的那一段**，不是当前目录。
    // Opus / TC / XYplorer 的面包屑右键都是这个语义；如果这里用"当前目录"，
    // 用户右键 D:\ 却复制出 D:\Projects\foo 的路径，那是纯粹的错。
    // 菜单内容交给统一组件；这里只描述动作与分隔符。
    ui::Menu menu;
    menu.Add(L"在新标签页打开", kSegOpenNewTab, ico::IconId::Add)
        .Add(L"在新窗口打开", kSegOpenNewWindow, ico::IconId::NewWindow)
        .Separator()
        .Add(L"复制此路径", kSegCopyPath, ico::IconId::Copy)
        .Add(L"在此打开终端", kSegTerminal, ico::IconId::Terminal)
        .Separator()
        .Add(L"属性", kSegProperties, ico::IconId::Settings);

    // 锚点 = 该段的文本区（与 chevron 菜单同一套定位逻辑，菜单不跟着鼠标漂）
    int k = partIndex - firstShown_;
    RECT anchor = (k >= 0 && k < static_cast<int>(segRects_.size()))
                      ? segRects_[static_cast<size_t>(k)]
                      : fieldRc_;
    POINT at{anchor.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);

    const std::wstring toggleKey = L"nav.seg." + std::to_wstring(partIndex);
    ui::MenuOptions opt;
    opt.toggleKey = toggleKey.c_str();
    const int action = menu.Show(hwnd_, *theme_, at, opt);
    if (action > 0 && segmentCmd_) segmentCmd_(target, static_cast<SegCmd>(action));
}

void NavBar::ShowFoldMenu(POINT screen) {
    (void)screen;
    std::vector<TreeNode> top;

    if (firstShown_ > 0) {
        const auto& parts = model_.parts();
        for (int i = 0; i < firstShown_ && i < static_cast<int>(parts.size()); ++i)
            top.push_back({parts[static_cast<size_t>(i)].label, parts[static_cast<size_t>(i)].path,
                           IconFor(parts[static_cast<size_t>(i)].path),
                           0, -1, {}, true, false, false, ico::IconId::Folder});
    } else {
        // 没折叠时这个钮给"此电脑"的顶层（驱动器/用户文件夹），与 mod 一致
        top = folder_tree::Children(nav::kThisPc);
    }
    if (top.empty()) return;

    POINT at{moreRc_.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);
    // 折叠菜单同样走内联树：点 chevron 就地展开，节点 icon 借自 ico::Cached
    std::wstring picked = folder_tree::Show(hwnd_, *theme_, at, std::move(top), 0, L"nav.fold");
    if (!picked.empty() && onNavigate_) onNavigate_(picked);
}

void NavBar::ShowHistoryMenu(bool backward, POINT screen) {
    (void)screen;
    if (!historyList_) return;
    std::vector<std::wstring> items = historyList_(backward);
    if (items.empty()) return;

    ui::Menu menu;
    int n = std::min<int>(static_cast<int>(items.size()), 24);
    for (int i = 0; i < n; ++i) {
        const std::wstring& path = items[static_cast<size_t>(i)];
        menu.AddPath(LabelFor(path), path, i);
    }

    // 对齐到对应的分裂按钮（后退/前进），与资源管理器一致
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == (backward ? kBack : kForward)) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    // 后退/前进各自一个锚点：两个按钮的下拉不能共用键，否则"关掉后退下拉、
    // 马上点前进"会被误判成连点。
    ui::MenuOptions opt;
    opt.toggleKey = backward ? L"nav.hist.back" : L"nav.hist.forward";
    const int pick = menu.Show(hwnd_, *theme_, at, opt);
    if (pick >= 0 && historyJump_) historyJump_(pick, backward);
}

void NavBar::ShowDropMenu(POINT screen) {
    (void)screen;
    if (!historyList_) return;
    std::vector<std::wstring> back = historyList_(true);
    std::vector<std::wstring> fwd = historyList_(false);
    std::vector<std::wstring> recent;
    if (recentList_) recent = recentList_();
    if (back.empty() && fwd.empty() && recent.empty()) return;

    const std::wstring cur = model_.path();
    ui::Menu menu;
    std::vector<std::wstring> seen;
    std::vector<std::wstring> recentPaths;
    int initial = -1;
    auto addPath = [&](const std::wstring& p, int action) {
        const int row = menu.AddPath(LabelFor(p), p, action);
        seen.push_back(p);
        if (initial < 0 && str::IEquals(p, cur)) initial = row;
    };

    for (size_t i = 0; i < back.size() && i < 16; ++i)
        addPath(back[i], static_cast<int>(i));
    if (!back.empty() && !fwd.empty()) menu.Separator();
    for (size_t i = 0; i < fwd.size() && i < 16; ++i)
        addPath(fwd[i], 1000 + static_cast<int>(i));
    if (!recent.empty()) {
        if (!back.empty() || !fwd.empty()) menu.Separator();
        int n = 0;
        for (const auto& p : recent) {
            if (n >= 12) break;
            bool dup = false;
            for (const auto& s : seen)
                if (str::IEquals(s, p)) { dup = true; break; }
            if (!dup) {
                addPath(p, 2000 + n);
                recentPaths.push_back(p);
                ++n;
            }
        }
    }

    // 底部固定一条“清除历史记录”：它是作用于整个列表的动作，不与路径条目混排。
    menu.Separator().Add(L"清除历史记录", 3000, ico::IconId::Close);

    // 与地址栏左对齐、从字段底边展开，宽度不小于字段宽
    POINT at{fieldRc_.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);

    // 地址框右侧 ⌄：严格与地址框同宽（左右边缘齐平），长路径靠省略号收
    LogDbg(L"[drop] field=%d..%d w=%d anchor=%d,%d", fieldRc_.left, fieldRc_.right,
           fieldRc_.right - fieldRc_.left, at.x, at.y);
    ui::MenuOptions opt;
    opt.minWidthPx = fieldRc_.right - fieldRc_.left;
    opt.exactWidth = true;
    opt.toggleKey = L"nav.drop";
    opt.initialRow = initial;
    const int data = menu.Show(hwnd_, *theme_, at, opt);
    if (data >= 3000) {
        // 清空历史。不导航：用户只是整理了历史，当前目录不该跟着跳。
        if (clearHistory_) clearHistory_();
    } else if (data >= 2000) {
        const size_t k = static_cast<size_t>(data - 2000);
        if (k < recentPaths.size() && onNavigate_) onNavigate_(recentPaths[k]);
    } else if (data >= 1000) {
        if (historyJump_) historyJump_(data - 1000, false);
    } else if (data >= 0 && historyJump_) {
        historyJump_(data, true);
    }
}

// 「设置…」面板的锚点：视图按钮的左下角，与右键菜单弹在同一处。
//
// 抽成函数的原因：鼠标路径（右键菜单 900 项）与命令路径（win.preferences）
// 是两条独立入口，锚点各算一份就会在布局调整后指向不同按钮。
POINT NavBar::SettingsAnchor() const {
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == kView) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    return at;
}

void NavBar::ShowViewMenu(POINT screen) {
    (void)screen;
    // 视图菜单要回答"现在用的是哪一档、按什么排序、怎么分组"。
    // 以前完全不标当前值 —— 菜单看起来像"设置向导"，用户点完不知道有没有生效。
    // 资源管理器 / Opus / TC / XYplorer 的视图菜单都给当前项打点或打勾。
    //
    // 用"✓ "前缀打勾而不是高亮：高亮是"光标所在行"，会被误读成已选中
    // （与标签颜色菜单同一取舍，见 tab_strip_actions.cpp）。
    const ViewState vs = viewState_ ? viewState_() : ViewState{};

    ui::Menu menu;
    int initial = -1;
    for (int i = 0; i < static_cast<int>(kViewCount); ++i) {
        const int row = menu.AddChecked(kViews[i].label, vs.mode == i, i, kViews[i].glyph);
        if (i == vs.mode) initial = row;
    }
    menu.Separator();
    static const wchar_t* kSorts[] = {L"按名称排序", L"按日期排序", L"按类型排序", L"按大小排序"};
    static const wchar_t* kGroups[] = {L"不分组", L"按类型分组", L"按日期分组", L"按大小分组"};
    // 排序/分组按列语义取图标；不分组用关闭字形表达“无”，而不是空白。
    static const ico::IconId kSortGlyphs[] = {ico::IconId::Page, ico::IconId::Calendar,
                                               ico::IconId::Tag, ico::IconId::HardDrive};
    static const ico::IconId kGroupGlyphs[] = {ico::IconId::Close, ico::IconId::Tag,
                                                ico::IconId::Calendar, ico::IconId::HardDrive};
    int sortRows[4] = {-1, -1, -1, -1};
    for (int i = 0; i < 4; ++i)
        sortRows[i] = menu.AddChecked(kSorts[i], vs.sort == i, 100 + i, kSortGlyphs[i]);
    // 排序方向也是一条独立信息：同一列点两次会反向，但菜单上完全看不出来。
    if (vs.sort >= 0 && vs.sort < 4)
        menu.AppendLabel(sortRows[vs.sort], vs.ascending ? L"（升序）" : L"（降序）");
    menu.Separator();
    for (int i = 0; i < 4; ++i)
        menu.AddChecked(kGroups[i], vs.group == i, 200 + i, kGroupGlyphs[i]);

    // 「设置…」放在最下面：它是"其它可调项"的入口，与上面三组（视图/排序/分组）
    // 不是同一层级的即时动作。动作号 900 与 0..7 / 1xx / 2xx 分开，
    // 免得将来视图档位扩到 9 档时撞号。
    menu.Separator();
    menu.Add(L"设置…", 900, ico::IconId::Settings);

    const POINT at = SettingsAnchor();
    // 预选当前档位：键盘用户按 ↓ 从这里继续，鼠标用户一眼看到自己在哪一档。
    ui::MenuOptions opt;
    opt.toggleKey = L"nav.view";
    opt.initialRow = initial;
    const int data = menu.Show(hwnd_, *theme_, at, opt);
    if (data == 900) {
        // 面板自己会按同一锚点重新打开，这里只把位置交出去。
        if (onSettings_) onSettings_(at);
    } else if (data >= 200) {
        if (onSortGroup_) onSortGroup_(1, data - 200);
    } else if (data >= 100) {
        if (onSortGroup_) onSortGroup_(0, data - 100);
    } else if (data >= 0 && data < static_cast<int>(kViewCount) && onView_) {
        onView_(kViews[data].fvm, kViews[data].icon);
    }
}

void NavBar::ShowViewMenuNow() {
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == kView) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    ShowViewMenu(at);
}
