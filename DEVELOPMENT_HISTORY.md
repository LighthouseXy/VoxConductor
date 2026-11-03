# VoxConductor 开发与维护记录

[简体中文](DEVELOPMENT_HISTORY.md) | [English](DEVELOPMENT_HISTORY.en.md) · [项目说明](README.md)

本记录按可访问的 Git 提交及其实际 diff 整理，日期采用提交作者时间的 UTC+8 日期。可核对的记录从 2026-08-26 的 V1 导入快照延续至 2026-10-21，跨度 56 天；首次开发日期和逐日活动需依据更早记录确认。

以下分别列出源码维护、硬件资料和文档许可变化。功能实现日期依据源码 diff，发布日期和实机验收结果分别依据发布与测试记录。

## 软件快照与维护

| 日期 | 类型 | diff 可核对的变化 | 提交 |
| --- | --- | --- | --- |
| 2026-08-26 | 初始导入快照 | 纳入当时名为 AmbientDesk 的固件、FastAPI 服务与文档。源码包含按键录音上传、云端 ASR/模型/TTS、流式音频返回、SQLite 会话和 SD 文本记录。它是已有实现的导入，更早开发日程需依据更早记录确认。 | [c2bb0e0](https://github.com/LighthouseXy/VoxConductor/commit/c2bb0e0745e420011c078f6c5a983c9f031a6dda) |
| 2026-08-26 | 改名与兼容维护 | 工程名和目录统一为 VoxConductor；`server/conversation_store.py` 增加旧数据库文件沿用及旧会话 ID 迁移逻辑，避免单纯改名丢失原有上下文入口。 | [c65367e](https://github.com/LighthouseXy/VoxConductor/commit/c65367ea2e0d3ced2b286247d0fab4520c277ca4) |
| 2026-08-27 | 服务端可观测性与错误处理 | `server/app.py` 统一阶段失败信息，并记录首音频块、音频字节和块数等过程指标；`server/tts_client.py` 区分正常 `session.finished` 与提前关闭，避免把异常流结束当作正常完成。延迟与准确率提升仍待专项测量。 | [e07bbb5](https://github.com/LighthouseXy/VoxConductor/commit/e07bbb5f4df8e3bc694e24ab32b2b5db5acbf4f8) |

这些条目描述各自提交中的代码变化。当前设备直连路线见 [设备配置](docs/DEVICE_CLOUD_VOICE.md)；V1 服务端记录保留早期架构的实现过程。

## 硬件工程与静态校验

| 日期 | 类型 | diff 可核对的变化 | 提交 |
| --- | --- | --- | --- |
| 2026-10-11 | 硬件资料纳入版本管理 | 加入 KiCad 原理图/PCB、项目库、既有制造资料、板级 BOM、V3-SLA 结构文件及重建/打包脚本。这次 diff 记录资料纳入版本管理的日期，制造日期依据原生产记录确认。 | [459a489](https://github.com/LighthouseXy/VoxConductor/commit/459a4894a30c6dbee7cd5b3852c938331d62f2e8) |
| 2026-10-11 | CAD 检查漏检修复 | `complete_assembly.py` 从按 FreeCAD 内部对象名改为按零件 Label 取六个结构件，要求三组参考包络与六件结构组成完整的 18 组检查，并加入 `--check-only`。该模式刷新校验报告，不保存或导出装配几何；不会把空报告当作通过。 | [cf8629e](https://github.com/LighthouseXy/VoxConductor/commit/cf8629e4ca9df97db9782ac3b10f217fb149fdf1) |
| 2026-10-11 | BOM 与装配说明 | 新增 `hardware/BOM.md` 和逐位号 BOM CSV，记录 61 个板载位置、59 个拟装配及 2 个 DNP；补充装配与技术说明。整机采购项需在板载 BOM 基础上补齐。 | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |

当前 KiCad 保留 J2 实物返修前的设计，待回写修正；V3 的树脂打印、线束弯折和螺纹强度仍待实物验证。参见 [硬件说明](hardware/README.md)。

## 文档、展示与许可

| 日期 | 类型 | diff 可核对的变化 | 提交 |
| --- | --- | --- | --- |
| 2026-08-26 | 历史许可 | 加入当时使用的 PolyForm Noncommercial 文本。该条记录历史状态，当前软件许可见下方 MIT 变更与根 LICENSE。 | [c59e8e9](https://github.com/LighthouseXy/VoxConductor/commit/c59e8e9608ce9f85f75948d442c5938802dba4e9) |
| 2026-08-26 / 2026-08-27 | 文档维护 | 调整验收文档入口，明确 V1 为设备与 FastAPI 协作的按键半双工路线，并区分功能链路验收与尚无统计结论的性能指标；另更新服务端配置示例说明，记录文档与配置维护。 | [4487e42](https://github.com/LighthouseXy/VoxConductor/commit/4487e42dff0f33f0b40053f9783d3aff8c86471b)、[ebc3d11](https://github.com/LighthouseXy/VoxConductor/commit/ebc3d11accf5ef8ae338b5c84c5f65d06284d8ef) |
| 2026-10-10 | 直连阶段说明与演示整理 | 新增设备直连配置、语音与记忆方案、展示图集及演示入口。这次提交的对应变化属于文档/素材，相关固件功能的首次提交日期需依据其源码记录确认。 | [ab63267](https://github.com/LighthouseXy/VoxConductor/commit/ab63267161b4ed16bc769a9328ca33c9b7d6d61e) |
| 2026-10-11 | 阅读结构与管理网页说明 | 重排 README 的硬件、功能与架构入口，整理网页管理的布局、接口与边界说明。 | [abbfed8](https://github.com/LighthouseXy/VoxConductor/commit/abbfed8c563733c2f535d854972c2323eaebcdc8) |
| 2026-10-11 | 技术说明与演示归属 | 新增技术架构说明，补充演示音轨变调处理记录及展示限制。唤醒率、延迟和连续运行统计仍待专项测量。 | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |
| 2026-10-11 | 语速/音色设置文档 | 更新功能表和网页管理文档中的播报语速、音色与持久化说明，加入桌面及移动端设置截图；此 diff 记录文档和截图变化，功能实现日期与听测结果需依据相应源码和测试记录确认。 | [f182f20](https://github.com/LighthouseXy/VoxConductor/commit/f182f200d775a78dfe27ec116d7aac062993cbb2) |
| 2026-10-21 | 英文 README 与导航 | 新增英文 README，在双语 README 顶部加入语言互链；英文明确说明设备 UI 主要为中文，界面语言保持中文。 | [ad5a675](https://github.com/LighthouseXy/VoxConductor/commit/ad5a67575b3ceac12e64bcd3b04ebc5117de6228) |
| 2026-10-21 | 软件许可与范围说明 | 原创软件及技术文档采用标准 MIT，新增路径/版本范围和第三方声明，保留硬件、字体、媒体与依赖各自条款；固件与音源沿用此前版本。 | [00684a0](https://github.com/LighthouseXy/VoxConductor/commit/00684a050d0d051534f9f48b29ee5039acd37da2) |

## 如何理解这些记录

- 第一条记录已有实现的导入快照，更早开发日期需依据更早记录确认。
- 日期与 commit 链接对应各提交实际包含的变化；其他工作按其源码和发布记录确定日期。
- 源码 diff、统计测试和实物验证各自提供证据，文档、照片和演示记录相应内容。
- 新 release、用户规模和社区贡献需依据各自记录确认；当前软件与第三方范围见 [许可说明](LICENSE_SCOPE.md)。
