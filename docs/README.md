# VoxConductor 文档索引

更新日期：2026-10-10。这里区分当前开发版、未来计划和旧版实测。本次只更新文档与展示素材，最新固件代码另行整理。

## 先看作品

- [项目总览](../README.md)：我为什么做它、现在的能力和边界。
- [实物与视频图集](SHOWCASE.md)：精选 15 张照片、两段完整演示、两段回答节选。
- [后续开发计划](ROADMAP.md)：先稳定语音，再完善控制、网页和复现。

## 当前设计与使用

- [设备直连云服务](DEVICE_CLOUD_VOICE.md)：配置、模型、操作、存储和资源限制。
- [语音与记忆](VOICE_DIALOGUE_AND_MEMORY_PLAN.md)：工具、上下文、连续接话、唤醒词打断、结束与回归。
- [桌面交互与工具](DESKTOP_TOOLS_AND_GAMES.md)：主页、闹钟、天气、媒体与设置；游戏已移除。
- [硬件与接线](HARDWARE_V1.md)、[历史原型 BOM](BOM.csv)、[姿态手势](MPU6050_GESTURES.md)。
- [局域网设备协议](LAN_DEVICE_PROTOCOL.md)：受限外部设备示例，不是通用远程控制平台。
- [网页管理改进设计](WEB_MANAGEMENT_DESIGN.md)：已有无配对码入口/诊断与后续布局计划。

## 开发与发布

- [按日期开发日志](logs/README.md)：八月原型和 9 月 25 日至 10 月 10 日阶段记录。
- [发布准备与范围](GITHUB_PUBLICATION_CHECKLIST.md)、[文件清单](GITHUB_FILE_MANIFEST.md)。
- [图片索引](images/README.md)、[视频说明](videos/2026-10-10/README.md)、[素材来源与校验](images/2026-10-10/manifest.json)。

## 历史参考

- [V1 服务端架构](ARCHITECTURE.md)、[V1 验证](DEMO_AND_VERIFICATION.md)：保留当时的 FastAPI/SQLite 路径与实测。
- [功能分析与历史方案](FEATURE_ANALYSIS_AND_EXPANSION.md)：旧方案按日期保留，当前取舍以最新说明为准。

主机检查、编译、烧录和实机验收分别记录；图片与单次视频不替代可靠性测试。

## 硬件设计文件

[硬件设计入口](../hardware/README.md)统一收录 KiCad、原理图 PDF、板载 BOM、制造资料和 V3-SLA 外壳；[装配 PPT](../hardware/mechanical/V3-SLA/presentation/V3-SLA_装配与修正说明.pptx)与打印清单在同一版本目录。
