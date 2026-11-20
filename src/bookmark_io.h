// 书签导入导出（Netscape HTML 格式，与 BookmarkStore 的存储文件同格式互通）。
// 导出：IFileSaveDialog 选路径，UTF-8 BOM 写出。
// 导入：IFileOpenDialog 选文件，解析后由用户选追加/覆盖。
#pragma once
#include <string>

#include "common.h"
#include "theme.h"

// 打开保存对话框导出当前全部书签。返回导出条数，-1=用户取消，-2=写文件失败。
int BmkExportHtml(HWND owner, const Theme& theme);

// 打开打开对话框导入书签，弹出"追加/覆盖"选择。
// 返回导入条数（覆盖模式=覆盖后总数），-1=用户取消/文件无效。
int BmkImportHtml(HWND owner, const Theme& theme);
