// 序列弹层的横向几何（纯计算；不含绘制）。
//
// 【为什么单独成文件】这套数字就是"看起来对不对"的全部来源：宽度、列位、
// 按钮位置。原版 mod 把它散在绘制函数里，改一个数字要翻三百行；
// 这里把它收成一张可核对的表 —— 与参考 mod 的常量一一对应：
//
//     kSeqPad     = 8    列表右内边距
//     kSeqBufH    = 24   输入区高度（= 行高）
//     kSeqRowH    = 24   候选行高
//     kSeqMaxRows = 20   列表可视行数
//     kSeqModeWinW= 440  弹窗宽度（恒定，两种列表都不跳宽）
//     kSeqCmdW    = 150  命令列宽（快捷键管理器同口径）
//     kSeqGrpW    = 58   分组列宽
//
// 【口径】列位与原版逐项对齐：
//     序号(左 4px) │ 序列键 │ 描述 │ 命令(右对齐) │ 分组(右对齐)
// 序号列宽按**总行数**的位数算（不是按可见行）—— 否则 9→10 行时序号列会跳宽。
// 序列键列宽按**全部**序列键里最长 token 算，与过滤结果无关 —— 否则敲 g 时
// 按 "gg"(2 字) 算、删掉 g 变全量后按 "gfp"(3 字) 算，列宽跳一格，
// 每行序列与说明之间凭空多出一个空格（原版注释里记的同一个坑）。
#pragma once

namespace seqov {

// 96 DPI 逻辑值（96 = 1 倍，144 = 150%）。所有几何都从这里出发。
struct Metrics {
    int pad = 8;        // 列表右内边距
    int rowH = 24;      // 候选行高
    int bufH = 24;      // 输入区高度（= 行高）
    int maxRows = 20;   // 列表可视行数
    int winW = 440;     // 弹窗宽度（恒定）
    int cmdW = 150;     // 命令列宽
    int grpW = 58;      // 分组列宽
    int numX = 4;       // 序号列左线（**不缩放**：原版是字面量 4px）
    int numGap = 4;     // 序号列右间隙（**不缩放**：原版是字面量 4px）
    int colGap = 8;     // 列间隙
    int scrollW = 8;    // 滚动条槽宽
    int scrollPadL = 2; // 滑块距槽左
    int scrollPadR = 1; // 滑块距槽右
    int thumbMin = 30;  // 滑块最小高
    int btnW = 18;      // 输入区按钮尺寸
    int btnGap = 2;     // 输入区按钮间隙
    int btnRight = 6;   // 输入区按钮距右边
    int btnTextGap = 4; // 按钮区左边界与输入文本右界之间的留白（原版 SX(4)）
    int caretGap = 3;   // 光标与输入文本之间的间隙（原版 SX(3)）
    int caretW = 2;     // 光标宽度（原版 SX(2)）
    int caretInset = 4; // 光标上下内缩（原版 SX(4)）
    int caretMaxGap = 6;// 光标右界与按钮区之间的留白（原版 SX(6)）
    int hintW = 146;    // 模式列表「说明」列宽（原版 SX(146)）
    int titleReserve = 150;  // 模式列表：名称列右界给说明列留的宽度（原版 SX(150)）
    int iconSz = 16;    // 目标列表图标槽尺寸（原版 SX(16)）
    int iconGap = 6;    // 图标槽与名称列之间的间隙（原版 SX(6)）
    int pathW = 210;    // 目标列表无触发键时的右列宽（原版 SX(210)）
    int grpGap = 12;    // 分组列与触发列之间的间隙（原版 SX(12)）
};

// 按 DPI 缩放（纯算术；不碰 Win32，便于核对与复用）。
inline Metrics Scaled(const Metrics& m, int dpi) {
    auto s = [dpi](int v) { return MulDiv(v, dpi, 96); };
    Metrics o;
    o.pad = s(m.pad);
    o.rowH = s(m.rowH);
    o.bufH = s(m.bufH);
    o.maxRows = m.maxRows;          // 行数不是像素，不缩放
    o.winW = s(m.winW);
    o.cmdW = s(m.cmdW);
    o.grpW = s(m.grpW);
    o.numX = m.numX;         // 字面量 4px（原版如此，不按 DPI 缩放）
    o.numGap = m.numGap;     // 同上
    o.colGap = s(m.colGap);
    o.scrollW = s(m.scrollW);
    o.scrollPadL = s(m.scrollPadL);
    o.scrollPadR = s(m.scrollPadR);
    o.thumbMin = s(m.thumbMin);
    o.btnW = s(m.btnW);
    o.btnGap = s(m.btnGap);
    o.btnRight = s(m.btnRight);
    o.btnTextGap = s(m.btnTextGap);
    o.caretGap = s(m.caretGap);
    o.caretW = s(m.caretW);
    o.caretInset = s(m.caretInset);
    o.caretMaxGap = s(m.caretMaxGap);
    o.hintW = s(m.hintW);
    o.titleReserve = s(m.titleReserve);
    o.iconSz = s(m.iconSz);
    o.iconGap = s(m.iconGap);
    o.pathW = s(m.pathW);
    o.grpGap = s(m.grpGap);
    return o;
}

// 横向列位。
struct Cols {
    int numW = 0;    // 序号列宽
    int keyX = 0;    // 序列键 / 名称列左界
    int keyW = 0;    // 序列键列宽
    int titleX = 0;  // 描述列左界
    int cmdX = 0;    // 命令列左界
    int cmdW = 0;    // 命令列宽
    int grpX = 0;    // 分组列左界
    int right = 0;   // 列表右界（= 宽 − 滚动条槽宽）
};

// 序号位数（按总行数，不是按可见行数）。
inline int Digits(int totalRows) {
    int digits = 1, tenPow = 10;
    while (totalRows >= tenPow && digits < 6) { tenPow *= 10; ++digits; }
    return digits;
}

// 五列几何。
//   monoCharW = 等宽字符宽（像素）；keyChars = 全部序列键里最长 token 的字数；
//   scroll    = 内容超出可视行数（右侧留滚动条槽）。
inline Cols ComputeCols(int totalRows, int monoCharW, int keyChars, int winW,
                        int pad, int colGap, int numX, int numGap, int cmdW, int grpW,
                        int scrollW, bool scroll) {
    Cols c;
    c.numW = Digits(totalRows) * monoCharW;
    c.keyX = numX + c.numW + numGap;
    c.keyW = keyChars * monoCharW;
    c.titleX = c.keyX + c.keyW + colGap;
    c.cmdW = cmdW;
    c.right = winW - (scroll ? scrollW : 0);
    c.cmdX = c.right - pad - grpW - colGap - cmdW;
    c.grpX = c.cmdX + cmdW + colGap;
    return c;
}

// 模式列表列位（对齐原版 SeqShowingModeList 分支）。
//
//   序号(左 4px) │ 名称(含前缀) │ 说明（右对齐，固定宽 146）
//
// 与 5 列的序列候选列表**不同构**：模式列表没有「序列键」列，名称直接占
// 序列键列的位置（原版 titleX = keyX），这样两种列表的视觉节奏一致、
// 切换时不跳版。列表里不画图标 —— 输入框的图标占的是序号列（x=4）。
struct ModeCols {
    int numW = 0;      // 序号列宽
    int titleX = 0;    // 名称列左界（= 原版 keyX）
    int titleRight = 0;// 名称列右界（给说明列让位）
    int hintX = 0;     // 说明列左界
    int hintRight = 0; // 说明列右界
    int right = 0;     // 列表右界（= 宽 − 滚动条槽宽）
};

inline ModeCols ComputeModeCols(int totalRows, int monoCharW, int winW, int pad,
                                 int numX, int numGap, int scrollW, bool scroll,
                                 int hintW, int titleReserve) {
    ModeCols c;
    c.numW = Digits(totalRows) * monoCharW;
    c.titleX = numX + c.numW + numGap;
    c.right = winW - (scroll ? scrollW : 0);
    c.hintRight = c.right - pad;
    c.hintX = c.hintRight - hintW;
    c.titleRight = c.hintRight - titleReserve;
    return c;
}

// 目标列表列位（对齐原版 SeqShowingTargetList 分支）。
//
//   序号(左 4px) │ 图标槽(16) │ 名称 │ 副标题 │ [分组][触发]
//
// 图标槽**恒预留**：原版「无图标就留白，下一帧数据到了自然就有了」——
// 名称列起点不随图标有无而跳，两种列表的视觉节奏才稳定。
struct TargetCols {
    int numX = 0;      // 序号列左线
    int numW = 0;      // 序号列宽
    int iconX = 0;     // 图标槽左界
    int iconSz = 0;    // 图标槽尺寸
    int nameX = 0;     // 名称列左界
    int right = 0;     // 列表右界（= 宽 − 滚动条槽宽）
    int pathW = 0;     // 无触发键时的右列宽
};

// 单行的右侧双列（分组 + 触发）宽度按**本行**文本量取 —— 原版同款：
// 触发列宽随触发键长短变，分组列让位到它左侧。
struct TargetRight {
    int trigW = 0;   // 触发列宽
    int grpW = 0;    // 分组列宽
    int trigX = 0;   // 触发列左界
    int grpX = 0;    // 分组列左界
    int nameRight = 0;  // 名称列右界
};

inline TargetCols ComputeTargetCols(int totalRows, int monoCharW, int winW, int pad,
                                     int numX, int numGap, int scrollW, bool scroll,
                                     int iconSz, int iconGap, int pathW) {
    TargetCols c;
    c.numX = numX;
    c.numW = Digits(totalRows) * monoCharW;
    c.iconX = numX + c.numW + numGap;
    c.iconSz = iconSz;
    c.nameX = c.iconX + iconSz + iconGap;
    c.right = winW - (scroll ? scrollW : 0);
    c.pathW = pathW;
    return c;
}

// 按本行的分组 / 触发文本量算右侧双列。
//   trigW > 0 → 双列：[分组][触发]；名称列右界 = 分组列左界 − 8
//   trigW = 0 → 单列（右列放路径）：名称列右界 = 右列左界 − 6
inline TargetRight ComputeTargetRight(const TargetCols& c, int pad, int grpGap,
                                       int trigW, int grpW) {
    TargetRight r;
    r.trigW = trigW;
    r.grpW = grpW;
    r.trigX = c.right - pad - trigW;
    r.grpX = r.trigX - grpGap - grpW;
    r.nameRight = (trigW > 0) ? (r.grpX - 8) : (c.right - pad - c.pathW - 6);
    return r;
}

// 输入区按钮：0 = 清空（最右），1 = 钉住（其左）。
// 绘制与命中必须走同一个函数，否则会"画在右边、点在左边"。
inline int BtnX(const Metrics& m, int idx) {
    return m.winW - m.btnRight - m.btnW - idx * (m.btnW + m.btnGap);
}

// 按钮区左边界（输入文本的右界要让开它，否则长文本会钻到按钮底下）。
inline int BtnsLeft(const Metrics& m) { return BtnX(m, 1) - m.btnTextGap; }

}  // namespace seqov
