## 改动说明

<!-- 一句话说明改了什么，以及为什么 -->

## 验证清单

- [ ] `bash build.sh` 或 `bash build.sh --out ...` 零警告
- [ ] `bash tests/run_all.sh` 全量通过（或说明为何不适用）
- [ ] `python tools/check_commands.py`
- [ ] `python tools/check_ai_meta.py`
- [ ] `python tools/check_components.py`
- [ ] `python tools/gen_eval_baseline.py --check`（如改到受保护文件，请先 `--human-confirm` 重签）
- [ ] 深/浅色与 125%/150% DPI 已检查（UI 改动）
- [ ] `CHANGELOG.md` 的 `[Unreleased]` 已补条目

## 约束确认

- [ ] 没有模拟鼠标/键盘、没有抢焦点（R21）
- [ ] 没有引入第三方运行期依赖（或已在此论证）
- [ ] 没有把真实密钥/用户配置写进仓库或发布包

## 相关 issue

<!-- Closes #123 -->
