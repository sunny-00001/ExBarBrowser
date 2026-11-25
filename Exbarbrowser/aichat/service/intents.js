// intents.js - 高频文件管理意图的本地直调表。
//
// 为什么存在：
//   用户说「上一级」「打开下载」「按大小排序」这类话时，意图和结构化命令是一一
//   对应的。走模型虽然也能做，但要多一次网络往返 + 一段推理，用户看到的就是
//   「思考半天」。这里先做一层确定性匹配：命中就直接调用 ExBarBrowser 的结构化
//   命令，不命中再交给模型。只收**无歧义、无破坏性**的规则；任何可能改文件的
//   意图都不进这张表，仍由模型 + 审批门处理。
"use strict";

function normalise(text) {
  return String(text == null ? "" : text)
    .trim()
    .replace(/^[,，。!！?？\s]+/, "")
    .replace(/[,，。!！?？\s]+$/, "")
    .replace(/^(?:请|帮我|麻烦|能不能|可以)(?:你)?\s*/, "");
}

function queryFrom(text) {
  return String(text || "")
    .trim()
    .replace(/^(?:一下|下)/, "")
    .replace(/[。！!，,?？]+$/, "")
    .slice(0, 120);
}

const RULES = [
  // ---- 导航 ----
  { re: /^(?:回到|返回|去)?上(?:一级|层)(?:目录|文件夹)?$/, name: "nav.up", args: {}, reply: "已回到上一级。" },
  { re: /^上级(?:目录|文件夹)?$/, name: "nav.up", args: {}, reply: "已回到上一级。" },
  { re: /^(?:后退|返回上一页|返回上一个位置)$/, name: "nav.back", args: {}, reply: "已后退。" },
  { re: /^前进$/, name: "nav.forward", args: {}, reply: "已前进。" },
  { re: /^刷新(?:当前)?(?:文件夹|目录|视图)?$/, name: "nav.refresh", args: {}, reply: "已刷新当前文件夹。" },

  { re: /^(?:打开|回到|去)?(?:主目录|用户目录|home|主页)$/i, name: "go.home", args: {}, reply: "已打开主目录。" },
  { re: /^(?:打开|回到|去)?下载(?:文件夹|目录)?$/, name: "go.downloads", args: {}, reply: "已打开下载文件夹。" },
  { re: /^(?:打开|回到|去)?文档(?:文件夹|目录)?$/, name: "go.documents", args: {}, reply: "已打开文档文件夹。" },
  { re: /^(?:打开|回到|去)?桌面$/, name: "go.desktop", args: {}, reply: "已打开桌面。" },
  { re: /^(?:打开|回到|去)?此电脑$/, name: "go.thispc", args: {}, reply: "已打开此电脑。" },

  // ---- 视图 / 排序（按命令表真实 schema：preset / by）----
  { re: /^(?:切换为?|使用|显示)?详细信息(?:视图|模式)?$/, name: "view.mode", args: { preset: 5 }, reply: "已切换到详细信息视图。" },
  { re: /^(?:切换为?|使用|显示)?大图标(?:视图|模式)?$/, name: "view.mode", args: { preset: 1 }, reply: "已切换到大图标视图。" },
  { re: /^(?:切换为?|使用|显示)?列表(?:视图|模式)$/, name: "view.mode", args: { preset: 4 }, reply: "已切换到列表视图。" },
  { re: /^(?:按|以)?名称(?:升序)?(?:排序|排列)$/, name: "view.sort", args: { by: 0 }, reply: "已按名称排序。" },
  { re: /^(?:按|以)?日期(?:升序)?(?:排序|排列)$/, name: "view.sort", args: { by: 1 }, reply: "已按日期排序。" },
  { re: /^(?:按|以)?类型(?:升序)?(?:排序|排列)$/, name: "view.sort", args: { by: 2 }, reply: "已按类型排序。" },
  { re: /^(?:按|以)?大小(?:升序)?(?:排序|排列)$/, name: "view.sort", args: { by: 3 }, reply: "已按大小排序。" },

  // ---- 选择 / 标签 ----
  { re: /^全选$/, name: "file.select.all", args: {}, reply: "已全选。" },
  { re: /^选中?所有文件$/, name: "file.select.files", args: {}, reply: "已选中所有文件。" },
  { re: /^选中?所有文件夹$/, name: "file.select.folders", args: {}, reply: "已选中所有文件夹。" },
  { re: /^选中?(?:所有)?(?:图片|照片)$/, name: "file.select.pattern",
    args: { pattern: "*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.webp" }, reply: "已选中图片。" },
  { re: /^新建标签(?:页)?$/, name: "tab.new", args: {}, reply: "已新建标签。" },
  { re: /^关闭当前标签(?:页)?$/, name: "tab.close", args: {}, reply: "已关闭当前标签。" }
];

// 搜索单独处理：要把用户给的关键词原样带进 query，不能用固定规则。
function matchSearch(t) {
  const m = t.match(/^(?:搜索|查找|搜一下|查一下)\s*(.+)$/);
  if (!m) return null;
  const query = queryFrom(m[1]);
  if (!query) return null;
  return {
    name: "search.find",
    args: { query },
    reply: `已在视图里搜索「${query}」。`
  };
}

// 命中返回 {name,args,reply}；不命中返回 null，调用方照常走模型。
function matchFastIntent(text) {
  const t = normalise(text);
  if (!t) return null;
  const search = matchSearch(t);
  if (search) return search;
  for (const rule of RULES) {
    if (rule.re.test(t)) return { name: rule.name, args: { ...rule.args }, reply: rule.reply };
  }
  return null;
}

module.exports = { matchFastIntent, RULES };
