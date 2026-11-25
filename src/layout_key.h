// 布局输入指纹：Layout() 用它判断"这次重排是不是白做"。
//
// 【为什么单独成文件】它是**纯数据 + 纯比较**，没有任何 App 状态依赖，
// 但内联在 app.h 里占了 20 行 —— app.h 是有 500 行上限的共享头（AGENTS §3），
// 而 app.h 已经因为各会话持续加功能而贴近上限。按"一文件一职责"抽出来：
// 读它的人只需要知道"哪些输入决定几何"，不必翻 App 的成员区。
//
// 【为什么需要这个指纹】ShowWindow 同步发 WM_SIZE -> Layout()，而 Run 在
// ShowWindow **之前**已经排过一次版，几何完全一样。实测启动期 Layout 被调
// 3 次（+547 / +578 / +828），前两次共 16 ms，第三次 250 ms —— 全花在
// 重复的 MoveWindow(..., TRUE) 重绘上，用户看不到任何变化。
//
// 【字段必须覆盖所有影响几何的输入】漏一项 = 漏掉一次真实重排（表算错位）：
//   客户区尺寸 / 四条栏显隐 / 四栏实际像素高 / 导航宽 / AI 面板宽 /
//   双栏 / 分栏比例 / 分栏方向。
//
// barPx 为什么必须单独存在（不能只记 showBits）：主题或 DPI 切换时
// 显隐不变、但每栏高度会变（theme_.barTab() 等都是 S() 缩放过的）。
// 只比 showBits 会把"每栏都高了 2px"误判成没变，整窗就不重排了。
#pragma once

namespace layout {

struct LayoutKey {
    long w = -1;
    long h = -1;
    int showBits = 0;   // tab|nav|bookmark|status 各占一位
    int barPx = -1;     // tabH + navH + bmkH + stH 的实际像素和
    int navW = -1;
    int aiW = -1;
    // 面板**期望**宽度（未夹取前）。为什么必须与 aiW 一起进指纹：
    // 拖动右侧边界时，先把值写进配置、再重排 —— 而当"窗口装不下"时求解会把
    // aiW 夹小甚至归零，于是 aiW 这一段不再变化，指纹判"没变"，边界条就停在
    // 原地不跟手（用户看到的是"拖到这里就不动了"）。期望值一定跟着鼠标变。
    int aiWant = -1;
    int split = -1;
    bool dual = false;
    bool vertical = false;
    // 双栏下**第二窗格的 view 是否已建出来**。
    //
    // 【为什么它必须进指纹】第二窗格是**按需创建**的（切标签 / 开双栏 / 首帧后补建），
    // 而"view 刚建出来"这件事本身不改变客户区尺寸与任何配置 —— 指纹若不含它，
    // 建完右窗格之后那一次 Layout() 会被整段跳过，右窗格就停在创建时的初始尺寸
    // （实测 8x8，右半区整块空白：分割条在位、右侧空空如也）。
    // 切标签建右窗格那条路径同样踩这个坑，所以它得进指纹而不是靠某个调用点记得。
    bool pane1Ready = false;
    // 活动标签 id：每个标签各有自己的一组窗格窗口，切标签 / 新建标签会**换掉整组**，
    // 而新建出来的那组一开始是 8x8 的初始尺寸 —— 不重排它就停在 8x8
    // （实测：新建标签后旧标签的窗格已隐藏、新标签的窗格是 8x8，整个窗格区空白）。
    // 所以"当前是哪个标签"同样是影响几何的输入。
    long activeTab = -1;

    bool operator==(const LayoutKey& o) const {
        return w == o.w && h == o.h && showBits == o.showBits && barPx == o.barPx &&
               navW == o.navW && aiW == o.aiW && aiWant == o.aiWant && split == o.split &&
               dual == o.dual && vertical == o.vertical && pane1Ready == o.pane1Ready &&
               activeTab == o.activeTab;
    }
};

}  // namespace layout