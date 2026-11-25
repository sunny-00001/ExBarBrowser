// 见 search_scope.h。
#include "search_scope.h"

#include "path_util.h"

namespace searchscope {

const wchar_t* RangeId(int range) {
    switch (range) {
    case kCurrent: return L"current";
    case kSubtree: return L"subtree";
    case kThisPC:  return L"thispc";
    case kCustom:  return L"custom";
    }
    return L"";
}

int RangeFromId(const std::wstring& id) {
    for (int i = 0; i < kRangeCount; ++i)
        if (id == RangeId(i)) return i;
    return -1;
}

const wchar_t* RangeLabel(int range) {
    switch (range) {
    case kCurrent: return L"当前目录（仅本层）";
    case kSubtree: return L"当前目录（含子目录）";
    case kThisPC:  return L"此电脑";
    case kCustom:  return L"自定义路径";
    }
    return L"";
}

bool RangeSupported(int range) {
    // 整机范围在本宿主做不到（作用域约束见 src/search_folder.h）。
    return range != kThisPC;
}

const wchar_t* RangeShortLabel(int range) {
    switch (range) {
    case kCurrent: return L"仅本层";
    case kSubtree: return L"含子目录";
    case kThisPC:  return L"此电脑";
    case kCustom:  return L"自定义";
    }
    return L"";
}

Target Resolve(int range, const std::wstring& currentDir, const std::wstring& customPath) {
    Target t;
    // 作用域一律给**规范形式**：正斜杠→反斜杠、重复分隔符合并、尾分隔符去掉。
    //
    // 为什么必须在范围这一层收口：外部路径（AI/命令参数/弹窗输入/手改的
    // settings.json）里正斜杠与重复分隔符都是**合法输入**，但 IsDirectory 与
    // 索引作用域不认 —— path_util.h 记录了同一类输入在不同命令上表现不同的
    // 那次教训（fs.rename 收到正斜杠路径报 E_INVALIDARG，真因完全看不出来）。
    // 范围语义的唯一真源在这里，规范化也就只做一次。
    auto setScope = [&t](const std::wstring& dir) {
        t.scopeDir = pathutil::ToShellPath(dir);
        t.valid = true;
    };
    switch (range) {
    case kCurrent:
        // 仅本层：作用域仍是这个目录（把索引查询范围先收窄），真正圈住"本层"
        // 的是条件里的"父目录恰好等于它"（原生搜索的作用域天生递归）。
        if (currentDir.empty()) return t;
        setScope(currentDir);
        t.onlyThisLayer = true;
        break;
    case kSubtree:
        if (currentDir.empty()) return t;
        setScope(currentDir);
        break;
    case kThisPC:
        // 整机范围在本宿主**做不到**：原生搜索文件夹的作用域只能是一个普通
        // 文件系统目录（约束与实测见 src/search_folder.h）—— 不给作用域、
        // 命名空间项、多元素数组都绑不上；整机搜索要的是 Explorer 进程内的
        // search-ms 全局查询。
        //
        // 这里返回 valid=false：调用方据此**明确报错**，不悄悄退回别的档 ——
        // 退回的话用户以为搜了整台电脑，实际只搜了当前目录。
        return t;
    case kCustom:
        if (customPath.empty()) return t;
        setScope(customPath);
        break;
    default:
        return t;
    }
    return t;
}

}  // namespace searchscope
