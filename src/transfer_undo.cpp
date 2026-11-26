// 见 transfer_undo.h：复制/移动的撤销记录构造（纯逻辑，可单测）。
#include "transfer_undo.h"

#include "common.h"
#include "nav_path.h"
#include "str.h"
#include "text_file.h"

namespace transferundo {

std::vector<bool> PrecheckDest(const std::vector<std::wstring>& sources,
                               const std::wstring& to) {
    std::vector<bool> out;
    out.reserve(sources.size());
    for (const std::wstring& p : sources) {
        const std::wstring leaf = nav::Leaf(p);
        // 同目录：落点就是源本身，探"已存在"没有意义（BuildItems 会整条跳过）。
        out.push_back(!leaf.empty() && !to.empty() && tf::Exists(tf::Join(to, leaf)));
    }
    return out;
}

std::vector<fileundo::Item> BuildItems(const std::vector<std::wstring>& sources,
                                       const std::wstring& to,
                                       const std::vector<bool>& preExisted) {
    std::vector<fileundo::Item> items;
    if (to.empty()) return items;

    for (size_t i = 0; i < sources.size(); ++i) {
        const std::wstring& p = sources[i];
        const std::wstring leaf = nav::Leaf(p);
        if (leaf.empty()) continue;

        // 同目录：after 会等于 p 本身。撤销复制 = 删 p（删源文件）；
        // 撤销移动 = 把 p 搬回它自己所在目录（无操作却报成功）。两种都不该记。
        if (str::IEquals(ParentOf(p), to)) continue;

        // 转移前就存在同名项 = 冲突，shell 已改名，真实落点不可知。
        // 记下 after 会把"用户原有的文件"当成我们产生的副本。
        if (i < preExisted.size() && preExisted[i]) continue;

        const std::wstring after = tf::Join(to, leaf);
        // 没探过 preExisted 时（老调用方）仍以"落点存在"为准；但这条只有在
        // 目标目录原本没有同名项时才成立，所以只作兜底，不作主判据。
        if (!tf::Exists(after)) continue;

        fileundo::Item item;
        item.beforePath = p;
        item.afterPath = after;
        item.beforeName = leaf;
        items.push_back(std::move(item));
    }
    return items;
}

}  // namespace transferundo
