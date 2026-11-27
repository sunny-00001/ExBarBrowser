// 设置对话框的右栏（说明）与底部状态栏。
//
// 与 settings_dialog_draw.cpp 分开：右栏是"读"的（说明 + 键名 + 生效时机），
// 中栏是"改"的（表单控件）。两者的信息密度与交互方式都不同，混在一个文件里
// 读的人得在两件事之间来回跳。
//
// 【右栏回答三个问题】这一项叫什么、它是什么键、什么时候生效。
// 第三个最容易被忘："改了没反应"是设置里最贵的误会，而它多半只差一句
// "立即生效" / "只读：下次启动生效"。
#include "settings_dialog.h"

#include <algorithm>

#include "ui_draw.h"

namespace {

// 右栏的段落标题（"说明" / "相关操作" / "何时生效"）。
const wchar_t* kHeadExplain = L"说明";
const wchar_t* kHeadActions = L"相关操作";
const wchar_t* kHeadTiming = L"何时生效";

}  // namespace

void SettingsDialog::PaintRight(HDC dc, const RECT& rc) {
    ui::FillRectColor(dc, rc, theme_->panelBg);
    // 左边界一条分隔线（不画竖分隔线的理由见文件头：三栏靠底色差分界）。
    ui::DrawSeparator(dc, rc, true, theme_->line);

    // 【"有没有选中项"现算，不存进 right_】它是派生状态（selKey_ 能否找到行）。
    // 早先在这里写 right_.empty、在 HitRightAction 里读 —— 于是"命中测试读到的
    // 是上一次绘制留下的值"，而绘制与点击的先后顺序并不由谁保证。
    const Row* row = RowByKey(selKey_);

    // 段落标题 + 段落下方的分隔线。
    auto head = [&](const RECT& r, const wchar_t* text) {
        ui::DrawTextRow(dc, theme_->fontBold, text, r, theme_->text,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        RECT line{r.left, r.bottom + theme_->S(6), r.right, r.bottom + theme_->S(6)};
        ui::DrawSeparator(dc, line, false, theme_->line);
    };

    head(right_.head1, kHeadExplain);

    if (!row) {
        const std::wstring empty =
            L"在左侧选中一项设置，这里会显示它的说明、键名和相关操作。";
        ui::DrawTextRow(dc, theme_->font, empty, right_.desc, theme_->inert,
                        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        return;
    }

    // 名称（加粗）+ 说明（整段换行，不截断 —— 说明全文只在这里能看到）。
    // 说明块的高度就是按这段文本量出来的（见 LayoutRight），这里画同一份。
    ui::DrawTextRow(dc, theme_->fontBold, row->name, right_.name, theme_->text,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    ui::DrawTextRow(dc, theme_->font, RightDescText(), right_.desc, theme_->sub,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    // 键名芯片：等宽字体，让人能照着这个名字去写命令 / 跟 AI 说。
    // 【夹到布局给的右边界】宽度只按文本算时，长键名（22 字符）在右栏最小宽
    // 160 下会画到栏外被裁 —— 看起来像"芯片没有右边框"。夹到 right_.chip.right
    // 再用省略号把"还有内容"显示出来，而不是悄悄丢掉。
    const int chipW = ui::MeasureText(dc, theme_->fontMono, row->entry->key) + theme_->S(12);
    const int chipRight = std::min(right_.chip.right, right_.chip.left + chipW);
    RECT chip{right_.chip.left, right_.chip.top, chipRight, right_.chip.bottom};
    ui::FillRound(dc, chip, theme_->bg, theme_->S(3));
    ui::FrameRectColor(dc, chip, theme_->line);
    RECT chipText{chip.left + theme_->S(6), chip.top, chip.right - theme_->S(6), chip.bottom};
    ui::DrawTextRow(dc, theme_->fontMono, row->entry->key, chipText, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 类型与取值域。
    std::wstring type = L"类型 ";
    type += str::FromUtf8(settings_reg::KindName(row->entry->kind));
    // 枚举项优先显示"N 个可选值"：它有 options 给出的精确取值域，连续 min..max
    // 会暗示中间那档（searchRange 的档位 2）合法，而那档其实写不进去。
    if (!row->entry->options.empty())
        type += L" · " + std::to_wstring(row->entry->options.size()) + L" 个可选值";
    else if (row->entry->maxValue > row->entry->minValue)
        type += L" · 取值 " + std::to_wstring(row->entry->minValue) + L"–" +
                std::to_wstring(row->entry->maxValue);
    ui::DrawTextRow(dc, theme_->font, type, right_.type, theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    head(right_.head2, kHeadActions);
    if (row->writable) {
        // 恢复默认值：一个按钮样子的框。
        ui::FillRound(dc, right_.restore, theme_->bg, theme_->S(3));
        ui::FrameRectColor(dc, right_.restore, theme_->line);
        RECT rtext{right_.restore.left + theme_->S(10), right_.restore.top, right_.restore.right,
                   right_.restore.bottom};
        ui::DrawTextRow(dc, theme_->font, L"恢复默认值", rtext, theme_->text,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        // 两个链接：复制键名 / 复制命令模板。
        ui::DrawTextRow(dc, theme_->font, L"复制键名", right_.copyKey, theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ui::DrawTextRow(dc, theme_->font, L"复制命令模板", right_.copyCmd, theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        // 只读项：没有"恢复默认值"，那一段改成一句说明。
        // 矩形取 restore 的上沿到 copyKey 的下沿（三个按钮的高度之和）——
        // 只写进 restore 那一格（29px）是不够的：这句话折行要两行，
        // 而 DT_WORDBREAK 画出矩形外的部分会被直接丢弃（看不出来少了什么）。
        const std::wstring note = L"该项由程序维护，设置面板只能查看。";
        const RECT noteBox{right_.restore.left, right_.restore.top, right_.restore.right,
                           right_.copyKey.bottom};
        ui::DrawTextRow(dc, theme_->font, note, noteBox, theme_->inert,
                        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        ui::DrawTextRow(dc, theme_->font, L"复制命令模板", right_.copyCmd, theme_->accent,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    head(right_.head3, kHeadTiming);
    const settings_reg::Group* g = settings_reg::FindGroup(row->entry->group);
    const std::wstring timing = g ? g->note : L"立即生效";
    // 【单行 + 省略号，而不是 DT_WORDBREAK】这一块的高度是固定的一行（可变的
    // 说明块在上面已经吃饱了可用高度）。既然排不下，就让截断**看得见** ——
    // 省略号说明"还有，但没地方"，而 DT_WORDBREAK 会把第二行悄悄丢掉。
    ui::DrawTextRow(dc, theme_->font, timing, right_.help, theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void SettingsDialog::PaintStatus(HDC dc, const RECT& rc) {
    ui::FillRectColor(dc, rc, theme_->bg);
    ui::DrawSeparator(dc, rc, false, theme_->line);

    // 左边：最近一次动作的结果（成功 / 失败）。
    const std::wstring text = status_.empty() ? L"就绪" : status_;
    RECT tr{rc.left + theme_->S(12), rc.top, rc.right - theme_->S(260), rc.bottom};
    ui::DrawTextRow(dc, theme_->font, text, tr,
                    statusError_ ? theme_->danger : theme_->sub,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 右边：活体读数（行高 / 控件高 / 总项数）。密度是唯一真源：
    // 控件高由行高推导，所以这里显示的两个数必然自洽。
    int total = 0;
    for (const auto& gi : groups_) total += gi.total;
    // 行高读数和密度按钮/菜单同一口径：都是缩放后的实际像素（RowHeight/CtrlHeight）。
    const std::wstring metrics = L"行高 " + std::to_wstring(RowHeight()) + L" · 控件 " +
                                 std::to_wstring(CtrlHeight()) + L" · 共 " +
                                 std::to_wstring(total) + L" 项";
    RECT mr{rc.right - theme_->S(260), rc.top, rc.right - theme_->S(12), rc.bottom};
    ui::DrawTextRow(dc, theme_->font, metrics, mr, theme_->inert,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
}
