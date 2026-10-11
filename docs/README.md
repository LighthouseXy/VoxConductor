# VoxConductor 文档导航

更新：2026-10-11。我将作品介绍、工程参考、验证记录分开组织。先了解设备，再查看实现与资料；历史文档保留当时的结果，不与当前开发版混用。

## 1. 了解项目

| 文档 | 阅读内容 |
| --- | --- |
| [项目总览](../README.md) | 开发期许、当前能力、技术路线与主要边界 |
| [硬件设计与装配](HARDWARE_DESIGN.md) | 三个实拍方向、电路、PCB、贴片前后、装配部件和结构 |
| [技术架构与路线](TECHNICAL_ARCHITECTURE.md) | 外设、音频、模型工具、轮次协调、资源和持久化 |
| [功能与页面](FEATURES.md) | 完整功能总览，再按设备页面介绍实际能力 |
| [实物与演示](SHOWCASE.md) | 精选图片、完整视频与注明来源的连续节选 |
| [后续计划](ROADMAP.md) | 稳定性优先级、待完善功能与暂缓范围 |

## 2. 查阅工程与操作

| 文档 | 阅读内容 |
| --- | --- |
| [硬件工程入口](../hardware/README.md) | KiCad、制造资料、BOM、供应商依据与校验；不重复保存 ZIP |
| [定制 PCB 电子 BOM](../hardware/BOM.md) | 电阻、电容、IC、连接器与逐位号明细；不与原型模块清单混用 |
| [V3 结构与打印](../hardware/mechanical/V3-SLA/README.md) | STEP/STL、试件、装配 PPT、重建输入与待试装边界 |
| [硬件与接线](HARDWARE_V1.md) | GPIO、器件配置、电气限制与通电检查 |
| [设备直连云服务](DEVICE_CLOUD_VOICE.md) | 服务配置、模型、分区、操作和资源限制 |
| [语音与记忆](VOICE_DIALOGUE_AND_MEMORY_PLAN.md) | 工具、上下文、连续接话、打断、结束与回归 |
| [桌面交互与工具](DESKTOP_TOOLS_AND_GAMES.md) | 主页、闹钟、天气、媒体、设置；游戏已移除 |
| [姿态手势](MPU6050_GESTURES.md) | 翻页、轴向、校准与调试 |
| [网页管理](WEB_MANAGEMENT_DESIGN.md) | 六区布局、实际接口、已实现改版和设备验收 |
| [局域网设备协议](LAN_DEVICE_PROTOCOL.md) | 受限外部设备示例，不是通用远程控制平台 |

## 3. 查看验证与发布

- [按日期开发日志](logs/README.md)：实现、问题、判断、修改与遗留事项。
- [发布准备与范围](GITHUB_PUBLICATION_CHECKLIST.md)：本批发布和后续代码整理分开记录。
- [文件清单](GITHUB_FILE_MANIFEST.md)：文档、素材、硬件的用途与范围。
- [图片索引](images/README.md)、[视频说明](videos/2026-10-10/README.md)：原图、裁切图、演示和预览的区别。
- [原始素材清单](images/2026-10-10/manifest.json)、[裁切与预览清单](images/2026-10-11/manifest.json)、[硬件文件校验](../hardware/manifest.json)。

主机检查、编译、烧录和实机验收分别记录。网页模拟预览不是设备运行截图，CAD 校验不是打印试装；图片与单次演示也不替代可靠性统计。

## 4. 历史参考

- [V1 服务端架构](ARCHITECTURE.md)、[V1 验证](DEMO_AND_VERIFICATION.md)：原 FastAPI/SQLite 阶段的真实实现与结果。
- [原型 BOM](BOM.csv)：旧模块阶段物料，不是定制 PCB 的采购表。
- [功能分析与历史方案](FEATURE_ANALYSIS_AND_EXPANSION.md)：旧方案按日期保留，当前取舍以最新说明为准。

项目文档独立维护，不采用社交博文的连载格式。小红书稿暂时不在本轮整理范围内。
