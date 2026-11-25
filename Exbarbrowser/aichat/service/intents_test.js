// intents_test.js - 高频意图直调表的纯函数自测。
// 不连模型、不连管道，只验证「哪些话会被本地直接翻译成哪条结构化命令」。
"use strict";

const assert = require("node:assert/strict");
const { matchFastIntent } = require("./intents");

let pass = 0;
function expect(text, name, args) {
  const got = matchFastIntent(text);
  assert.ok(got, `应命中：${text}`);
  assert.equal(got.name, name, `${text} -> 命令名`);
  assert.deepEqual(got.args, args, `${text} -> 参数`);
  pass++;
}
function none(text) {
  assert.equal(matchFastIntent(text), null, `不应命中：${text}`);
  pass++;
}

// 导航
expect("上一级", "nav.up", {});
expect("请回到上一级目录", "nav.up", {});
expect("后退", "nav.back", {});
expect("前进", "nav.forward", {});
expect("刷新", "nav.refresh", {});
expect("打开下载文件夹", "go.downloads", {});
expect("去文档", "go.documents", {});
expect("回到桌面", "go.desktop", {});
expect("打开此电脑", "go.thispc", {});

// 视图 / 排序：参数必须按命令表真实 schema（preset / by），不是旧 prompt 里的 mode/key
expect("详细信息", "view.mode", { preset: 5 });
expect("切换为列表视图", "view.mode", { preset: 4 });
expect("按大小排序", "view.sort", { by: 3 });
expect("按名称排列", "view.sort", { by: 0 });

// 选择 / 标签
expect("全选", "file.select.all", {});
expect("选中所有文件夹", "file.select.folders", {});
expect("选中所有图片", "file.select.pattern", { pattern: "*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.webp" });
expect("新建标签", "tab.new", {});
expect("关闭当前标签页", "tab.close", {});

// 搜索关键词原样透传
expect("搜索 发票", "search.find", { query: "发票" });
expect("查找 季度报告", "search.find", { query: "季度报告" });

// 复杂/有歧义/会改文件的意图必须留给模型 + 审批门
none("找上个月的发票");
none("按类型整理 Pictures 文件夹");
none("删除上个月的临时文件");
none("把选中的照片按拍摄日期重命名");
none("Pictures 文件夹里什么最占空间？");

console.log(`intents_test: ${pass} checks passed`);
