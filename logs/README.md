# logs：AI Coding 与历史运行记录

本目录已包含项目日志，不是待替换的空白模板。返回[仓库首页](../README.md)。

## 内容分类

[flash555588](flash555588/README.md)按日期保存真实 AI Coding JSONL，会话入口是
[manifest.json](flash555588/manifest.json)。日志清单描述已收录的会话，不保证本机所有
AI 对话都被自动采集，也不能用它推断当前对话已上传。

本目录根部的 `build-*.log`、`v1-*.log` 等是构建、串口或实板记录；
它们不是 AI 对话。其他功能证据在 [docs/evidence](../docs/evidence/README.md)与
[应用 evidence](../app/espdl-quickapp/evidence/README.md)。

## 提交与隐私

使用归集工具导出真实会话并更新清单，不手工伪造事件、时间或工具结果。
本机归集规则仅在被识别为 openvela 工作区时自动采集，不应为了“补日志”导出个人项目。
公开前检查长期令牌、密钥、Wi-Fi 凭据、个人信息和设备数据，清理结果需人工复核。

在仓库根目录运行 `python3 tools/check_submission.py` 检查日志字段、路径、清单与其他
提交门槛；通过校验不代表完成全部隐私或许可审查。日志应与代码一起经过正常 Git 审查，
不直接上传整个用户配置目录，也不把测试报告冒充 AI Coding 会话。
Device build, flash, boot, and runtime logs from 2026-09-19 are application evidence and are stored in [app/espdl-quickapp/evidence](../app/espdl-quickapp/evidence/README.md). The summary is [the work log](../docs/WORKLOG_2026-09-19.md); these records are deliberately kept separate from the flash555588 AI Coding session manifest.
