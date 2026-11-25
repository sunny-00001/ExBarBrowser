// 设置对话框的交互：点了做什么、写成什么。
//
// 【人机同源】所有写入都走 config.set 命令（CommandBus），不直接碰 Settings。
// 于是界面上的一次改动与 AI 的一次改动落在同一条路上：同一份校验、同一份
// 审计、同一个"写后回读"。手写控件若直接赋值字段，就会出现"界面能改、
// 命令改不动"，而且那种不一致没有任何检查会报。
//
// 【失败必须说清原因】"点了没反应"是设置里最贵的体验。写失败时把命令层给的
// 原因原样报在状态栏上 —— 名字写错、类型不符、超范围，要做的下一步完全不同。
#include "settings_dialog.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cwchar>

#include "clipboard.h"
#include "commands.h"
#include "config.h"
#include "menu.h"
#include "str.h"
#include "ui_draw.h"

namespace {

// 值的字面量写法（状态栏里报"已执行 config.set tab = true"）。
std::wstring ValueLiteral(const json::Value& v) {
    switch (v.type) {
    case json::Value::kBool: return v.b ? L"true" : L"false";
    case json::Value::kNumber: return std::to_wstring(static_cast<long long>(v.num));
    case json::Value::kString: return v.str;
    default: return L"（复合值）";
    }
}

// 左栏滚轮切分类的阈值：累计到这么多才切一次。
// 不设阈值的话，一格滚轮会连跳好几个分类（鼠标轮一格常常就是 3 行 delta）。
constexpr int kWheelStep = 40;

// 弹层锚点：客户区矩形的左下角 → 屏幕坐标。
//
// 【为什么必须换两个点】RECT 是 {left, top, right, bottom}，前两个字段才恰好构成
// 一个 POINT。以前把 RECT 强转成 POINT* 只映射 1 个点，于是 left/top 换了坐标系，
// bottom 还留在客户区 —— 弹层纵向飘到其他行。这里显式换两个角，坐标语义不再
// 依赖结构体内存布局，也不再出现 half-mapped 的RECT。
POINT PopupAnchor(HWND hwnd, const RECT& rc) {
    POINT topLeft{rc.left, rc.top};
    POINT bottomRight{rc.right, rc.bottom};
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);
    return {topLeft.x, bottomRight.y};
}

}  // namespace

// ---------------------------------------------------------------- 选中

void SettingsDialog::SelectGroup(const std::wstring& key) {
    // 搜索态：左栏的分类不是"切换"，是"跳到那一段"——
    // 中栏此时按分组分段排布（有标题条），跳过去比过滤更直观：
    // 过滤会让"这一组有 3 项命中"变成"只剩 3 行"，看不出还有别的组也有命中。
    if (!query_.empty()) {
        const settings_reg::Group* g = settings_reg::FindGroup(key);
        if (!g) return;
        for (size_t i = 0; i < items_.size(); ++i) {
            // 【按 key 比，不按显示名】标题条身上本来就带着 group key；
            // 用中文名去比等于把"显示文案"当成主键，改一个字就断。
            if (items_[i].header && items_[i].group == key) {
                EnsureVisible(static_cast<int>(i));
                return;
            }
        }
        return;
    }
    if (group_ == key) return;
    group_ = key;
    RebuildModel();
    scroll_ = 0;
    // 切到的那一组必须在左栏里看得见 —— 分组多于可用高度时它可能正滚在
    // 可视区外，而"选中了却看不见选中的是哪个"就是这个窗口最容易迷路的地方。
    EnsureGroupVisible(GroupIndex(key));
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::SelectEntry(const std::wstring& key) {
    if (selKey_ == key) return;
    selKey_ = key;
    // 右栏的说明块高度随选中的项变（每项说明长短不同），所以选中也要重排右栏。
    // 只重绘的话，上一个项的长说明会盖住键名芯片 —— 那是几何问题，不是颜色问题。
    // 中栏的行几何与选中项无关，不必整体 Layout。
    LayoutRight();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::SetStatus(const std::wstring& msg, bool error) {
    status_ = msg;
    statusError_ = error;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 写入

bool SettingsDialog::WriteValue(const std::wstring& key, const json::Value& value) {
    json::Value args = json::Value::MakeObject();
    args.Set(L"key", json::Value::MakeStr(key));
    args.Set(L"value", value);
    if (CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(args), Actor::Settings())) {
        badKey_.clear();   // 写入成功即清掉上一次的拒绝红框
        // 【告警必须说出来】命令成功、值也生效了，但可能没写盘（数据目录只读 /
        // settings.json 上次解析失败）。这时若只报"已执行"，用户下次启动会发现
        // 设置自己变回去了 —— 而那次追溯不了。所以把命令层的告警接在成功消息后面，
        // 并按错误着色：它不是成功的样子。
        const std::wstring& warn = CommandBus::Get().lastWarning();
        const std::wstring done = L"已执行 config.set " + key + L" = " + ValueLiteral(value);
        SetStatus(warn.empty() ? done : (done + L"（" + warn + L"）"), !warn.empty());
        RefreshValues();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    std::wstring why = CommandBus::Get().lastBlockReason();
    if (why.empty()) why = L"（命令层没有给出原因）";
    SetStatus(L"未生效：" + why, true);
    return false;
}

void SettingsDialog::ToggleBool(const Row& row) {
    WriteValue(row.entry->key, json::Value::MakeBool(!row.value.b));
}

// ---------------------------------------------------------------- 下拉

void SettingsDialog::OpenEnum(const Row& row, const RECT& ctrl) {
    const settings_reg::Entry& e = *row.entry;
    if (e.options.empty()) return;

    // 菜单内容交给 ui::Menu 组装（业务侧不手写 PopupRow）：
    // 勾选列的画法、图标槽、当前项高亮都由组件一处决定 ——
    // 手写一份就会和状态栏 / 视图菜单的勾选长得不一样。
    ui::Menu menu;
    int current = -1;
    for (size_t i = 0; i < e.options.size(); ++i) {
        const bool checked = settings_reg::ValueEquals(e.options[i].value, row.value);
        const int r = menu.AddChecked(e.options[i].label, checked, static_cast<int>(i),
                                      ico::IconId::None);
        if (checked) current = r;   // 预选当前值：下拉一打开就知道自己选的是哪个
    }

    // 锚点：控件下沿（屏幕坐标）。弹层贴着控件出现，而不是飘到别处。
    const POINT at = PopupAnchor(hwnd_, ctrl);

    // 锚点键带设置项名：同一个控件连点第二下 = 收起（弹层的"再点即收"契约）。
    const std::wstring toggle = L"settings.enum." + e.key;

    ui::MenuOptions opt;
    // 宽度取**下拉控件本身**：exactWidth + 同一个物理宽度，弹层左右边缘才会
    // 与框严格齐平。固定 S(192) 只是“看起来差不多”，控件一变宽/变窄就错位。
    opt.minWidthPx = ctrl.right - ctrl.left;
    opt.exactWidth = true;
    opt.maxRows = 12;
    opt.toggleKey = toggle.c_str();
    opt.initialRow = current;
    opt.scrollTo = true;

    const int picked = menu.Show(hwnd_, *theme_, at, opt);
    if (picked < 0 || picked >= static_cast<int>(e.options.size())) return;
    WriteValue(e.key, e.options[static_cast<size_t>(picked)].value);
}

// ---------------------------------------------------------------- 右栏动作

void SettingsDialog::RestoreDefault() {
    const Row* row = RowByKey(selKey_);
    if (!row) return;
    if (!row->writable) {
        SetStatus(L"该项只读：" + row->entry->key, true);
        return;
    }
    json::Value def;
    if (!settings_reg::GetDefault(row->entry->key, &def)) {
        SetStatus(L"取不到默认值：" + row->entry->key, true);
        return;
    }
    if (WriteValue(row->entry->key, def))
        SetStatus(L"已恢复默认值：config.set " + row->entry->key + L" = " + ValueLiteral(def));
}

void SettingsDialog::CopyKeyName() {
    const Row* row = RowByKey(selKey_);
    if (!row) return;
    if (SetClipboardText(hwnd_, row->entry->key))
        SetStatus(L"已复制键名 " + row->entry->key + L" 到剪贴板");
    else
        SetStatus(L"剪贴板被占用，复制失败", true);
}

void SettingsDialog::CopyCommandTemplate() {
    const Row* row = RowByKey(selKey_);
    if (!row) return;
    // 模板给的是**能直接粘**的命令，而不是一个键名：
    // 只读项尤其需要 —— 它的正确入口就是命令。
    std::wstring tpl = L"config.set " + row->entry->key + L" = ";
    switch (row->entry->kind) {
    case settings_reg::Kind::Bool: tpl += L"true"; break;
    case settings_reg::Kind::Int:
        tpl += (row->entry->maxValue > row->entry->minValue)
                   ? std::to_wstring(row->entry->minValue)
                   : std::to_wstring(static_cast<long long>(row->value.num));
        break;
    case settings_reg::Kind::Str: tpl += L"\"…\""; break;
    default: tpl += L"…"; break;
    }
    if (SetClipboardText(hwnd_, tpl))
        SetStatus(L"已复制命令模板 " + tpl + L" 到剪贴板");
    else
        SetStatus(L"剪贴板被占用，复制失败", true);
}

// ---------------------------------------------------------------- 滚动

void SettingsDialog::ScrollBy(int delta) {
    if (scrollMax_ <= 0) return;
    const int next = std::max(0, std::min(scroll_ + delta, scrollMax_));
    if (next == scroll_) return;
    scroll_ = next;
    LayoutRows();
    // 行几何变了 → 行内编辑框必须跟着挪。少了这一步，滚动时输入框停在原地，
    // 看起来像是"浮在另一行上"（用户会以为改的是那一行）。
    PlaceValueEdit();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::EnsureVisible(int itemIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items_.size())) return;
    const RECT& rc = items_[static_cast<size_t>(itemIndex)].rc;
    if (rc.top < form_.top) {
        ScrollBy(rc.top - form_.top);
    } else if (rc.bottom > form_.bottom) {
        ScrollBy(rc.bottom - form_.bottom);
    }
}

void SettingsDialog::EnsureGroupVisible(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= static_cast<int>(groups_.size())) return;
    const int rh = RowHeight();
    if (rh <= 0) return;
    const int top = groupIndex * rh;                       // 该行在内容坐标里的上沿
    const int viewH = static_cast<int>(catList_.bottom - catList_.top);
    if (top < groupScroll_) {
        groupScroll_ = top;
    } else if (top + rh > groupScroll_ + viewH) {
        groupScroll_ = top + rh - viewH;
    }
    groupScroll_ = std::max(0, std::min(groupScroll_, groupScrollMax_));
}

void SettingsDialog::EnsureRowVisible(int rowIndex) {
    // 行下标 → items_ 下标：搜索态下两者不相等（中间夹着段标题条）。
    for (size_t i = 0; i < items_.size(); ++i)
        if (!items_[i].header && items_[i].rowIndex == rowIndex) {
            EnsureVisible(static_cast<int>(i));
            return;
        }
}

// ---------------------------------------------------------------- 点击

void SettingsDialog::ClearHover() {
    if (hoverRow_ == -1 && hoverGroup_ == -1) return;
    hoverRow_ = -1;
    hoverGroup_ = -1;
    // tracking_ 不动：它记的是父窗口自己的 WM_MOUSELEAVE 跟踪，
    // 与"哪一行高亮"无关。清掉它会让指针每次回到父窗口都重新登记一次。
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDialog::OnWheel(int delta) {
    if (!hwnd_) return;
    POINT pt{};
    GetCursorPos(&pt);   // 只读观察光标位置（R21：不发合成输入、不移动光标）
    ScreenToClient(hwnd_, &pt);

    // 【为什么按矩形判而不是只看 x】原来用 `pt.x < colA_.right` 二分左右：
    // 光标落在窗口外、中栏头部、右栏帮助、底部状态栏时都会被当成"中栏"滚表单，
    // 于是"在帮助文字上滚一下，中间的表单跑了"。命中判据必须与绘制/点击同一套
    // 几何（colA_ / form_），而不是一套临时的 x 不等式。
    if (PtInRect(&colA_, pt)) {
        if (!query_.empty()) {
            // 搜索态不切分类；但必须清掉上一态留下的半格累计量，否则退出搜索后
            // 第一次滚一下会毫无征兆地跳一个分类。
            wheelAcc_ = 0;
            return;
        }
        // 换向时先清零：先在 A 方向滚 30、再反向滚 -30，两下互相抵消，
        // 用户的感受是"反向滚的第一下没反应"。
        if ((wheelAcc_ > 0 && delta < 0) || (wheelAcc_ < 0 && delta > 0)) wheelAcc_ = 0;
        wheelAcc_ += delta;
        if (std::abs(wheelAcc_) < kWheelStep) return;
        // 方向与中栏一致：上滚 = 往列表上方（前一个分类），不是往下一个。
        const int dir = wheelAcc_ > 0 ? -1 : 1;
        wheelAcc_ = 0;
        int gi = GroupIndex(group_);
        if (gi < 0) gi = 0;
        gi += dir;
        if (gi < 0) gi = static_cast<int>(groups_.size()) - 1;
        if (gi >= static_cast<int>(groups_.size())) gi = 0;
        if (!groups_.empty()) SelectGroup(groups_[static_cast<size_t>(gi)].group->key);
        return;
    }
    if (PtInRect(&form_, pt)) {
        wheelAcc_ = 0;
        ScrollBy(-delta * 3);
        return;
    }
    // 头部 / 右栏 / 状态栏 / 窗口外：这里没有可滚动的内容，别让表单跟着跑。
    wheelAcc_ = 0;
}

void SettingsDialog::OnClick(int x, int y) {
    if (!hwnd_) return;
    const POINT pt{x, y};

    // 右栏：只有几个动作。
    if (x >= colC_.left) {
        switch (HitRightAction(x, y)) {
        case 1: RestoreDefault(); return;
        case 2: CopyKeyName(); return;
        case 3: CopyCommandTemplate(); return;
        default: return;
        }
    }

    // 左栏：分类。
    if (x < colA_.right) {
        const int gi = HitGroup(x, y);
        if (gi >= 0) SelectGroup(groups_[static_cast<size_t>(gi)].group->key);
        return;
    }

    // 中栏头部：密度 / 帮助。
    if (y < head_.bottom) {
        switch (HitHeadAction(x, y)) {
        case 1: {
            // 行高档位也是菜单（ui::Menu 组装）：与枚举下拉同一条路径，
            // 勾选列的画法自然一致。
            ui::Menu menu;
            for (int v : kRowHeights)
                menu.AddChecked(L"行高 " + std::to_wstring(v), v == kRowHeights[density_], v,
                                ico::IconId::None);

            const POINT at = PopupAnchor(hwnd_, rcDensity_);

            ui::MenuOptions opt;
            opt.minWidthPx = theme_->S(120);
            opt.maxRows = 4;
            opt.toggleKey = L"settings.density";
            const int picked = menu.Show(hwnd_, *theme_, at, opt);
            if (picked < 0) return;
            density_ = kDefaultDensity;
            for (int i = 0; i < 3; ++i)
                if (kRowHeights[i] == picked) density_ = i;
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            SetStatus(L"行高已切到 " + std::to_wstring(kRowHeights[density_]) + L" 像素");
            return;
        }
        case 2:
            SetStatus(L"用法：左栏选分类，中栏改值，右栏看说明；搜索框可按名称 / 键名 / 说明过滤");
            return;
        default:
            return;
        }
    }

    // 中栏表单。
    const int item = HitItem(x, y);
    if (item < 0) return;
    const Item& it = items_[static_cast<size_t>(item)];
    if (it.header) return;

    const Row& row = rows_[static_cast<size_t>(it.rowIndex)];
    const RowLayout& rl = rowLayout_[static_cast<size_t>(it.rowIndex)];
    SelectEntry(row.entry->key);

    if (!row.writable) {
        SetStatus(L"该项只读：" + row.entry->key + L"（由程序维护）", true);
        return;
    }
    // 点在控件上才改值；点在行上只是选中。
    if (!PtInRect(&rl.box, pt)) return;
    if (!ApplyRowControl(row))
        SetStatus(L"该项没有可编辑的界面：请用命令 config.set " + row.entry->key, true);
}

// ---------------------------------------------------------------- 键盘

void SettingsDialog::SelectRowAt(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) return;
    SelectEntry(rows_[static_cast<size_t>(rowIndex)].entry->key);
    // 选中项必须看得见 —— 键盘移动时"选中了哪一个"完全靠画面。
    EnsureRowVisible(rowIndex);
}

void SettingsDialog::MoveSelection(int delta) {
    if (rows_.empty()) return;
    int idx = RowIndexByKey(selKey_);
    // 没有选中项时（理论上不会，RebuildModel 总会选一条）向下从第一行开始，
    // 向上从最后一行开始 —— 不该出现"按了没反应"。
    if (idx < 0) idx = delta > 0 ? -1 : static_cast<int>(rows_.size());
    int next = idx + delta;
    next = std::max(0, std::min(next, static_cast<int>(rows_.size()) - 1));
    SelectRowAt(next);
}

void SettingsDialog::ActivateSelected() {
    const int idx = RowIndexByKey(selKey_);
    if (idx < 0) return;
    const Row& row = rows_[static_cast<size_t>(idx)];
    if (!row.writable) {
        SetStatus(L"该项只读：" + row.entry->key + L"（由程序维护）", true);
        return;
    }
    if (!ApplyRowControl(row))
        SetStatus(L"该项没有可编辑的界面：请用命令 config.set " + row.entry->key, true);
}

bool SettingsDialog::ApplyRowControl(const Row& row) {
    const int idx = RowIndexByKey(row.entry->key);
    if (idx < 0 || idx >= static_cast<int>(rowLayout_.size())) return false;
    // 【枚举优先】带 options 的项走下拉 —— 包括布尔型枚举（dualPane）。
    // 与绘制同一条判据（见 settings_dialog_model.cpp 的 DisplayOf）。
    if (!row.entry->options.empty()) {
        OpenEnum(row, rowLayout_[static_cast<size_t>(idx)].box);
        return true;
    }
    if (row.entry->kind == settings_reg::Kind::Bool) {
        ToggleBool(row);
        return true;
    }
    if (row.entry->kind == settings_reg::Kind::Int) {
        BeginEdit(idx);
        return true;
    }
    if (row.entry->kind == settings_reg::Kind::Str) {
        // 可写字符串（当前只有 searchRangePath）：就地编辑。
        // 枚举型的 Str 上面已经按 options 走进下拉了，到这里的都是自由文本。
        BeginEdit(idx);
        return true;
    }
    // 其余（只读的字符串 / 数组 / 对象）没有控件可编辑。
    return false;
}

// ---------------------------------------------------------------- 行内编辑

void SettingsDialog::BeginEdit(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) return;
    if (!valueEdit_) return;
    const Row& row = rows_[static_cast<size_t>(rowIndex)];
    const settings_reg::Kind k = row.entry->kind;
    // 能就地编辑的只有两种：Int（数字）与自由字符串（可写、无枚举）。
    // 判据与 ApplyRowControl 一致 —— 两处各写一份就会出现
    // "能点开输入框，但里面没有值"或反过来。
    if (!row.writable) return;
    if (k != settings_reg::Kind::Int && k != settings_reg::Kind::Str) return;
    if (k == settings_reg::Kind::Str && !row.entry->options.empty()) return;

    editKey_ = row.entry->key;   // 用键名记身份，不用下标
    badKey_.clear();
    // 先填文本，再定位：PlaceValueEdit 会把控件显示出来，顺序反过来会先闪一下
    // 上一次编辑留下的旧值（"刚点开就看见一个别的数字"）。
    SetWindowTextW(valueEdit_, k == settings_reg::Kind::Int
                                   ? std::to_wstring(static_cast<long long>(row.value.num)).c_str()
                                   : row.value.str.c_str());
    Layout();   // 定位（并显示）编辑框到那一格
    SetFocus(valueEdit_);
    // 全选便于整体替换；文字位置由 EditLineHeight（偶数行高）保证与自绘态一致，
    // 选中高亮只改背景、不改基线，不会再造成"编辑态文字跳位"。
    SendMessageW(valueEdit_, EM_SETSEL, 0, -1);
}

// 行内编辑框里的文字（不做类型解释）。
std::wstring SettingsDialog::ValueEditText() const {
    if (!valueEdit_) return std::wstring();
    const int n = GetWindowTextLengthW(valueEdit_);
    if (n <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    const int got = GetWindowTextW(valueEdit_, s.data(), n + 1);
    s.resize(got > 0 ? static_cast<size_t>(got) : 0u);
    return s;
}

void SettingsDialog::CommitEdit(bool accept) {
    if (editKey_.empty()) return;
    // 先快照键名并立刻清空编辑态：ShowWindow(SW_HIDE) 会触发子控件的
    // WM_KILLFOCUS → 再 post 一条本消息；先清空使那条迟到消息成为空操作。
    const std::wstring key = editKey_;
    editKey_.clear();
    const std::wstring text = str::Trim(ValueEditText());   // 先取值，再隐藏
    if (valueEdit_) ShowWindow(valueEdit_, SW_HIDE);

    // 用键名重新定位，绝不沿用下标：RebuildModel 可能已把 rows_ 换成另一组，
    // 旧下标会指向完全不同的设置项（"编辑 A、却写进 B"的根因）。
    const int idx = RowIndexByKey(key);
    if (idx < 0) {
        SetStatus(L"编辑已取消：" + key + L" 不在当前列表", false);
        return;
    }
    if (!accept) {
        SetStatus(L"已取消修改 " + key);
        return;
    }

    const settings_reg::Entry& e = *rows_[static_cast<size_t>(idx)].entry;
    // 字符串项：原样交给 config.set（规范化由注册表那边的 setter 负责 ——
    // 面板不该知道"路径要转反斜杠"这类领域规则）。
    if (e.kind == settings_reg::Kind::Str) {
        WriteValue(key, json::Value::MakeStr(text));
        return;
    }

    wchar_t* end = nullptr;
    errno = 0;
    const long long v = std::wcstoll(text.c_str(), &end, 10);
    if (text.empty() || !end || *end != L'\0' || errno == ERANGE) {
        badKey_ = key;
        SetStatus(L"拒绝：" + e.key + L" 要一个整数（收到「" + text + L"」）", true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (v < INT_MIN || v > INT_MAX) {
        badKey_ = key;
        SetStatus(L"拒绝：" + e.key + L" 超出整数范围", true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const int iv = static_cast<int>(v);
    if (e.maxValue > e.minValue && (iv < e.minValue || iv > e.maxValue)) {
        badKey_ = key;
        SetStatus(L"拒绝：" + e.key + L" 取值须在 " + std::to_wstring(e.minValue) + L"–" +
                      std::to_wstring(e.maxValue) + L" 之间", true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    WriteValue(key, json::Value::MakeInt(iv));
}

// ---------------------------------------------------------------- 子类回调

LRESULT CALLBACK SettingsDialog::SearchSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id,
                                           DWORD_PTR data) {
    auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(GetParent(h), GWLP_USERDATA));
    // 【只读缓存】这里绝不能碰控件文本：读控件 = 发 WM_GETTEXT → 又回到本函数
    // → 再读一次 → 无限递归爆栈。缓存由父窗口在 EN_CHANGE 时更新。
    const bool placeholder = self && self->SearchShowsPlaceholder();
    // 占位文字只是显示层：首次真实输入先清掉，避免插进词中或产生整段蓝色选区。
    const auto ClearPlaceholder = [&]() {
        if (placeholder) { SetWindowTextW(h, L""); SendMessageW(h, EM_SETSEL, 0, 0); }
    };
    if (m == WM_MOUSEMOVE || m == WM_MOUSELEAVE) {
        if (self) self->ClearHover();
        return DefSubclassProc(h, m, w, l);
    }
    switch (m) {
    case WM_CHAR:
    case WM_IME_CHAR:
    case WM_PASTE:
        ClearPlaceholder();
        break;
    case WM_KILLFOCUS:
        // 失焦且框是空的 → 把占位文字放回去（框里空着看起来像"坏了"）。
        // 判空也读缓存（searchText_ 缓存的是控件文本）。
        if (self && self->searchText_.empty())
            SetWindowTextW(h, kSearchPlaceholder);
        break;
    case WM_KEYDOWN:
        if (placeholder && (w == VK_BACK || w == VK_DELETE)) {
            ClearPlaceholder();  // 删占位符 = 清空搜索，不逐字删
            return 0;
        }
        if (w == VK_ESCAPE) {
            // Esc = 清空搜索（不是关窗口）：搜索框里按 Esc 关掉整个设置窗口
            // 是很容易误触的。放回占位文字 = 视觉上"空了"，查询词也归零。
            SetWindowTextW(h, kSearchPlaceholder);   // 触发 EN_CHANGE → 重建模型
            return 0;
        }
        if (w == VK_RETURN) {
            // 回车 = "我看完了结果，去改"：焦点交给表单，并选中第一条结果。
            if (self) self->SelectRowAt(0);
            SetFocus(GetParent(h));
            return 0;
        }
        if (w == VK_DOWN) {
            // 下方向键 = 从搜索结果进入表单（与回车同一个去向，但由"想往下翻"
            // 这个直觉触发）。不处理的话它会在 EDIT 里毫无反应。
            if (self) self->SelectRowAt(0);
            SetFocus(GetParent(h));
            return 0;
        }
        if (w == VK_TAB) {
            // Tab 在单行 EDIT 里没有任何默认行为，也没人来接管 —— 显式切到表单。
            SetFocus(GetParent(h));
            return 0;
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK SettingsDialog::ValueSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id,
                                          DWORD_PTR data) {
    auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(GetParent(h), GWLP_USERDATA));
    // 指针进了行内编辑框，父窗口的 hover 高亮必须收掉：父窗口此时不再收到
    // WM_MOUSEMOVE，原先那一行会一直保持"悬停"高亮，看起来像"另一行被选中"。
    if (m == WM_MOUSEMOVE || m == WM_MOUSELEAVE) {
        if (self) self->ClearHover();
        return DefSubclassProc(h, m, w, l);
    }
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            PostMessageW(GetParent(h), kMsgCommitEdit, 1, 0);
            return 0;
        }
        if (w == VK_ESCAPE) {
            PostMessageW(GetParent(h), kMsgCommitEdit, 0, 0);
            return 0;
        }
        if (w == VK_TAB) {
            // Tab = 收下这个值并离开（"改完就走"）。提交照旧走 post：
            // 显示/隐藏控件不能发生在控件自己的回调里。
            PostMessageW(GetParent(h), kMsgCommitEdit, 1, 0);
            SetFocus(GetParent(h));
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        // 失焦 = 提交。用 post 而不是直接改：失焦发生在控件回调里，
        // 那时父窗口正在处理这次点击，直接改会撞上"控件正在被销毁"的时序。
        PostMessageW(GetParent(h), kMsgCommitEdit, 1, 0);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, m, w, l);
}
