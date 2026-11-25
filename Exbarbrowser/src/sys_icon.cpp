#include "sys_icon.h"

#include "ui_draw.h"

#include <vector>

namespace sysicon {
namespace {

struct Slot {
    HICON h = nullptr;
    bool resolved = false;
};

std::vector<Slot>& Cache() {
    static std::vector<Slot> cache;
    return cache;
}

// 从系统 DLL 的序号资源取图标。
//
// ★必须按 small 填小尺寸槽：ExtractIconExW 的大/小图标是两个**独立出参**。
// 无条件填大图标槽 → 需要 16px 的地方拿到 32px 再缩下去，发虚；高 DPI 下更糊。
// （Windhawk Exbar.cpp 的 IconFromLocation 记的就是这个坑。）
//
// 序号资源不是稳定 API（见 sys_icon.h 的说明），所以只在 SIID 没覆盖的形状上用，
// 且取不到就返回 nullptr 让调用方退字形。
HICON FromResourceIndex(const wchar_t* dll, int index) {
    HICON h = nullptr;
    if (ExtractIconExW(dll, index, nullptr, &h, 1) > 0 && h) return h;
    return nullptr;
}

// 语义 → 系统资源。★唯一映射：新增语义只在这里加一行。
//
// 序号资源的实测结果（Win11 26xx，脚本导出 PNG 肉眼确认过）：
//   imageres.dll 228 = 绿色圆圈对勾
//   imageres.dll 203 = 带蓝绿底座的立体文件夹（"文件夹 + 度量"）
//   imageres.dll  30 = 灰色硬盘（带绿色指示灯）
HICON Resolve(Id id) {
    switch (id) {
    case Id::Folder: return ui::StockIcon(SIID_FOLDER, true);
    case Id::Document: return ui::StockIcon(SIID_DOCNOASSOC, true);
    case Id::Check: return FromResourceIndex(L"imageres.dll", 228);
    case Id::Size: return FromResourceIndex(L"imageres.dll", 203);
    case Id::Disk: return FromResourceIndex(L"imageres.dll", 30);
    default: return nullptr;
    }
}

}  // namespace

HICON Get(Id id) {
    if (id == Id::None) return nullptr;
    auto& cache = Cache();
    const size_t k = static_cast<size_t>(id);
    if (cache.size() <= k) cache.resize(k + 1);
    if (!cache[k].resolved) {
        cache[k].resolved = true;
        cache[k].h = Resolve(id);
    }
    return cache[k].h;
}

void Invalidate() {
    auto& cache = Cache();
    for (auto& s : cache) {
        if (s.h) DestroyIcon(s.h);
        s.h = nullptr;
        s.resolved = false;
    }
}

}  // namespace sysicon
