# 2026-11-03 同步分支验证

基于远端main `c0bf589`，保留最新中英文介绍与许可提交，叠加本地开发源码。

| 检查 | 结果 |
|---|---|
| `python3 tests/agent_safety/run_checks.py` | exit 0 |
| `python3 tests/agent_safety/run_task_reconcile.py` | exit 0 |
| `python3 tests/test_voice_memory_order.py` | exit 0 |
| `python3 tests/test_semantic_agent_source.py` | exit 0 |
| `python3 tests/test_followup_alarm_source.py` | exit 0 |

Agent gate含reliability/runtime/transport/semantic/storage五组，使用ASan/UBSan、虚拟时钟和合成SDK/设备输入。task-reconcile另覆盖真实文件快照、运行态恢复、取消、重复消息与新进程重载。实机表现待专项验收，36个模型案例待运行。

本次没有进行新固件构建、刷机、声学、断电、物理家居设备或付费模型测试。历史2026-10-25本地ESP32-S3构建成功是既往证据。当前公开源码缺少本地合法提醒PCM及用户凭据；构建前提详见PROGRESS.md。托管cJSON源码仅在本地复用，没有纳入提交。

原开发工作区未重置、暂存或提交；整合分支从远端最新main建立，未改写历史。媒体与设计资料、原始审计日志、凭据和缓存未纳入。
