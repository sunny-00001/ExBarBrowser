// 前置条件的原因文案（唯一实现，见 cmd_req.h）。
#include "cmd_req.h"

const wchar_t* ReqReason(Req r) {
    switch (r) {
    case Req::None:              return L"";
    case Req::HasHistoryBack:    return L"已经是第一条历史";
    case Req::HasHistoryForward: return L"已经是最后一条历史";
    case Req::HasSelection:      return L"未选中任何项目";
    case Req::HasTabs:           return L"没有打开的标签";
    case Req::HasClipboard:      return L"剪贴板没有可粘贴的内容";
    // 三种"顶层"共用这一句：盘符根（去此电脑）、命名空间根（控制面板/回收站）、
    // UNC 根。以前只写"此电脑或网络根"，站在控制面板上按向上会得到一句与现场
    // 不符的解释 —— 用户会以为是程序判断错了，而不是"这里确实没有上级"。
    case Req::CanGoUp:           return L"已经是顶层（此电脑、命名空间或网络根）";
    case Req::StateUnavailable:  return L"界面尚未就绪";
    case Req::CanMoveTabLeft:    return L"当前标签已在最左";
    case Req::CanMoveTabRight:   return L"当前标签已在最右";
    case Req::HasClosedTabs:     return L"没有可重新打开的标签";
    case Req::HasMultipleTabs:   return L"只有一个标签";
    case Req::HasSelectionSingle: return L"需要恰好选中一个项目";
    }
    return L"";
}
