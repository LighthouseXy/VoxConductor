# VoxConductor 开发与维护记录

[简体中文](DEVELOPMENT_HISTORY.md) | [English](DEVELOPMENT_HISTORY.en.md) · [项目说明](README.md)

本记录按可访问的 Git 提交及其实际 diff 整理。日期采用提交作者时间的 UTC+8 日期，不以文件修改时间或资料内标注日期推算。可核对的记录从 2026-08-26 的 V1 导入快照延续至 2026-10-21，跨度 56 天；这不表示每天都有提交，也不推断首次开发日期。

以下分别列出源码维护、硬件资料和文档许可变化。资料新增不等于固件在同一天首次实现或发布，提交中的检查报告也不等于重新完成实机测试。

## 软件快照与维护

| 日期 | 类型 | diff 可核对的变化 | 提交 |
| --- | --- | --- | --- |
| 2026-08-26 | 初始导入快照 | 纳入当时名为 AmbientDesk 的固件、FastAPI 服务与文档。源码包含按键录音上传、云端 ASR/模型/TTS、流式音频返回、SQLite 会话和 SD 文本记录。它是已有实现的导入，不据此补造更早开发日程。 | [c2bb0e0](https://github.com/LighthouseXy/VoxConductor/commit/c2bb0e0745e420011c078f6c5a983c9f031a6dda) |
| 2026-08-26 | 改名与兼容维护 | 工程名和目录统一为 VoxConductor；`server/conversation_store.py` 增加旧数据库文件沿用及旧会话 ID 迁移逻辑，避免单纯改名丢失原有上下文入口。 | [c65367e](https://github.com/LighthouseXy/VoxConductor/commit/c65367ea2e0d3ced2b286247d0fab4520c277ca4) |
| 2026-08-27 | 服务端可观测性与错误处理 | `server/app.py` 统一阶段失败信息，并记录首音频块、音频字节和块数等过程指标；`server/tts_client.py` 区分正常 `session.finished` 与提前关闭，避免把异常流结束当作正常完成。这里没有据此宣称延迟或准确率提升。 | [e07bbb5](https://github.com/LighthouseXy/VoxConductor/commit/e07bbb5f4df8e3bc694e24ab32b2b5db5acbf4f8) |

这些条目描述各自提交中的代码变化。当前设备直连路线见 [设备配置](docs/DEVICE_CLOUD_VOICE.md)；V1 服务端记录保留作历史参考，不要求当前设备运行该旧服务。

## 硬件工程与静态校验

| 日期 | 类型 | diff 可核对的变化 | 提交 |
| --- | --- | --- | --- |
| 2026-10-11 | 硬件资料纳入版本管理 | 加入 KiCad 原理图/PCB、项目库、既有制造资料、板级 BOM、V3-SLA 结构文件及重建/打包脚本。制造目录和零件文件中的旧日期不等于这次提交的制造日期；这次 diff 主要证明资料已纳入版本管理。 | [459a489](https://github.com/LighthouseXy/VoxConductor/commit/459a4894a30c6dbee7cd5b3852c938331d62f2e8) |
| 2026-10-11 | CAD 检查漏检修复 | `complete_assembly.py` 从按 FreeCAD 内部对象名改为按零件 Label 取六个结构件，要求三组参考包络与六件结构组成完整的 18 组检查，并加入 `--check-only`。该模式刷新校验报告，不保存或导出装配几何；不会把空报告当作通过。 | [cf8629e](https://github.com/LighthouseXy/VoxConductor/commit/cf8629e4ca9df97db9782ac3b10f217fb149fdf1) |
| 2026-10-11 | BOM 与装配说明 | 新增 `hardware/BOM.md` 和逐位号 BOM CSV，记录 61 个板载位置、59 个拟装配及 2 个 DNP；补充装配与技术说明。板载 BOM 仍不是完整整机采购清单。 | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |

硬件边界保持明确：J2 实物返修尚未写回当前 KiCad；V3 的 CAD 校验记录不替代树脂打印、线束弯折和螺纹强度的实物验证。参见 [硬件说明](hardware/README.md)。

## 文档、展示与许可

| 日期 | 类型 | diff 可核对的变化 | 提交 |
| --- | --- | --- | --- |
| 2026-08-26 | 历史许可 | 加入当时使用的 PolyForm Noncommercial 文本。该条记录历史状态，当前软件许可见下方 MIT 变更与根 LICENSE。 | [c59e8e9](https://github.com/LighthouseXy/VoxConductor/commit/c59e8e9608ce9f85f75948d442c5938802dba4e9) |
| 2026-08-26 / 2026-08-27 | 文档维护 | 调整验收文档入口，明确 V1 为设备与 FastAPI 协作的按键半双工路线，并区分功能链路验收与尚无统计结论的性能指标；另更新服务端配置示例说明，不作为新功能实现。 | [4487e42](https://github.com/LighthouseXy/VoxConductor/commit/4487e42dff0f33f0b40053f9783d3aff8c86471b)、[ebc3d11](https://github.com/LighthouseXy/VoxConductor/commit/ebc3d11accf5ef8ae338b5c84c5f65d06284d8ef) |
| 2026-10-10 | 直连阶段说明与演示整理 | 新增设备直连配置、语音与记忆方案、展示图集及演示入口。这次提交的对应变化属于文档/素材，不能用来认定相关固件功能的首次提交日期。 | [ab63267](https://github.com/LighthouseXy/VoxConductor/commit/ab63267161b4ed16bc769a9328ca33c9b7d6d61e) |
| 2026-10-11 | 阅读结构与管理网页说明 | 重排 README 的硬件、功能与架构入口，整理网页管理的布局、接口与边界说明。 | [abbfed8](https://github.com/LighthouseXy/VoxConductor/commit/abbfed8c563733c2f535d854972c2323eaebcdc8) |
| 2026-10-11 | 技术说明与演示归属 | 新增技术架构说明，补充演示音轨变调处理记录及展示限制。演示处理记录不是唤醒率、延迟或连续运行统计。 | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |
| 2026-10-11 | 语速/音色设置文档 | 更新功能表和网页管理文档中的播报语速、音色与持久化说明，加入桌面及移动端设置截图；此 diff 未新增固件源码，不作为当天实现或听测通过的证据。 | [f182f20](https://github.com/LighthouseXy/VoxConductor/commit/f182f200d775a78dfe27ec116d7aac062993cbb2) |
| 2026-10-21 | 英文 README 与导航 | 新增英文 README，在双语 README 顶部加入语言互链；英文明确说明设备 UI 主要为中文，没有新增 UI 语言切换。 | [ad5a675](https://github.com/LighthouseXy/VoxConductor/commit/ad5a67575b3ceac12e64bcd3b04ebc5117de6228) |
| 2026-10-21 | 软件许可与范围说明 | 原创软件及技术文档采用标准 MIT，新增路径/版本范围和第三方声明，保留硬件、字体、媒体与依赖各自条款；该提交没有改变固件或音源。 | [00684a0](https://github.com/LighthouseXy/VoxConductor/commit/00684a050d0d051534f9f48b29ee5039acd37da2) |

## 如何理解这些记录

- 第一条是导入快照，不能从它推断更早的逐日开发过程。
- 日期与 commit 链接对应真实提交；未进入相应提交的修改不在该日期下计作已发布功能。
- 文档叙述、照片和单次演示分别说明其内容，不替代源码 diff、统计测试或实物验证。
- 本页不宣称新 release、用户规模或社区贡献；当前软件与第三方范围见 [许可说明](LICENSE_SCOPE.md)。
