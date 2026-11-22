// 书签导入导出（Netscape HTML 格式，与 BookmarkStore 的存储文件同格式互通）。
// 导出：IFileSaveDialog 选路径，UTF-8 BOM 写出。
// 导入：IFileOpenDialog 选文件，解析后由用户选追加/覆盖。
#pragma once
#include <string>

#include "common.h"
#include "theme.h"

// 打开保存对话框导出当前全部书签。返回导出条数，-1=用户取消，-2=写文件失败。
// UI 版：内部选路径后转调 BmkExportHtmlTo，保证人与机器共用同一份序列化实现。
int BmkExportHtml(HWND owner, const Theme& theme);

// 打开打开对话框导入书签，弹出"追加/覆盖"选择。
// 返回导入条数（覆盖模式=覆盖后总数），-1=用户取消/文件无效。
// UI 版：内部选路径+模式后转调 BmkImportHtmlFromFile。
int BmkImportHtml(HWND owner, const Theme& theme);

// ---- 确定式后端（AI / 命令面板 / 脚本入口，绝不弹框）----
// 目标由参数点名，不读对话框、不读选中、不碰焦点。
// 返回导出条数；-2=写文件失败。
int BmkExportHtmlTo(const std::wstring& file);

// mode: 0=追加（Merge），1=覆盖（ReplaceAll，可撤销）。
// 需要覆盖确认时由调用方在进入本函数前完成（人侧弹框 / AI 侧 approve）。
// 返回导入条数（覆盖模式=覆盖后总数）；-2=文件无效/无条目；-3=书签文件被外部改动。
// 被改动时不写盘，避免覆盖外部修改（与 UI 版同一守卫）。
int BmkImportHtmlFromFile(const std::wstring& file, int mode);
