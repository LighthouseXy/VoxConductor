# 文档与展示素材逐文件清单

盘点日期：2026-10-11。以下是本次整理涉及的公开文档树，包含已存在的历史文件；列出文件不意味着每个文件都产生了新修改。实际提交范围以 Git 提交为准。

10 月 10 日批次仅发布 README.md 与 docs/ 的文档、图片、视频和素材元数据。10 月 11 日新增整理后的 hardware/ 及对应索引。小红书待发布稿单独交付，不放进公开仓库；固件、server、tests、tools、examples、assets、backups 与 .gitignore 的本机改动均不随此次发布。

## 入口与说明

- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/BOM.csv`
- `docs/DEMO_AND_VERIFICATION.md`
- `docs/DESKTOP_TOOLS_AND_GAMES.md`
- `docs/DEVICE_CLOUD_VOICE.md`
- `docs/FEATURE_ANALYSIS_AND_EXPANSION.md`
- `docs/FEATURES.md`
- `docs/GITHUB_FILE_MANIFEST.md`
- `docs/GITHUB_PUBLICATION_CHECKLIST.md`
- `docs/HARDWARE_V1.md`
- `docs/HARDWARE_DESIGN.md`
- `docs/TECHNICAL_ARCHITECTURE.md`
- `docs/LAN_DEVICE_PROTOCOL.md`
- `docs/MPU6050_GESTURES.md`
- `docs/README.md`
- `docs/ROADMAP.md`
- `docs/SHOWCASE.md`
- `docs/VOICE_DIALOGUE_AND_MEMORY_PLAN.md`
- `docs/WEB_MANAGEMENT_DESIGN.md`

## 开发日志

- `docs/logs/2026-08-04.md`
- `docs/logs/2026-08-05.md`
- `docs/logs/2026-08-07.md`
- `docs/logs/2026-08-08.md`
- `docs/logs/2026-08-09.md`
- `docs/logs/2026-08-17.md`
- `docs/logs/2026-08-18.md`
- `docs/logs/2026-08-19.md`
- `docs/logs/2026-08-20.md`
- `docs/logs/2026-08-26.md`
- `docs/logs/2026-08-27.md`
- `docs/logs/2026-09-25.md`
- `docs/logs/2026-09-26.md`
- `docs/logs/2026-09-27.md`
- `docs/logs/2026-09-30.md`
- `docs/logs/2026-10-01.md`
- `docs/logs/2026-10-02.md`
- `docs/logs/2026-10-03.md`
- `docs/logs/2026-10-04.md`
- `docs/logs/2026-10-05.md`
- `docs/logs/2026-10-06.md`
- `docs/logs/2026-10-07.md`
- `docs/logs/2026-10-08.md`
- `docs/logs/2026-10-09.md`
- `docs/logs/2026-10-10.md`
- `docs/logs/2026-10-11.md`
- `docs/logs/README.md`

## 原始发布图片和清单

- `docs/images/2026-10-10/alarm-edit-hour.jpg`
- `docs/images/2026-10-10/assembly-parts-overview.jpg`
- `docs/images/2026-10-10/demo-time-weather-cover.jpg`
- `docs/images/2026-10-10/demo-volume-cover.jpg`
- `docs/images/2026-10-10/desktop-menu.jpg`
- `docs/images/2026-10-10/device-clock-front.jpg`
- `docs/images/2026-10-10/device-rear.jpg`
- `docs/images/2026-10-10/device-side.jpg`
- `docs/images/2026-10-10/manifest.json`
- `docs/images/2026-10-10/pcb-assembled.jpg`
- `docs/images/2026-10-10/pcb-bare-detail.jpg`
- `docs/images/2026-10-10/settings-grid.jpg`
- `docs/images/2026-10-10/voice-listening.jpg`
- `docs/images/2026-10-10/voice-processing.jpg`
- `docs/images/2026-10-10/voice-speaking.jpg`
- `docs/images/2026-10-10/weather-current.jpg`
- `docs/images/2026-10-10/weather-hourly.jpg`
- `docs/images/2026-10-10/weather-metrics.jpg`

## 2026-10-11 展示版素材

本目录只保留正文使用或链接的素材，逐文件名称、来源和 SHA-256 见 [manifest.json](images/2026-10-11/manifest.json)：

| 文件组 | 内容与用途 |
| --- | --- |
| `device-*.jpg` | 圆形时钟与整机正面、侧面、后侧、另一侧开口 |
| `pcb-*.jpg` | 裸板批次、单板细节与贴片后实物 |
| `assembly-parts-*.jpg` | 两张部件全景的裁边展示版 |
| `part-*.jpg` | 八个全景局部图，加一张新增方形分光棱镜独立实拍 |
| 闹钟/天气/设置/语音/菜单图片 | 功能页面，与文字说明对应 |
| 原理图与 PCB SVG | 用于文档内放大查看；不是新增投板修订 |
| `web-*.jpg` | 真实前端运行的模拟数据截图，含语速/音色设置的电脑和手机布局，未连接设备 |

结构预览直接引用硬件目录，未重复提交；未使用的 Peripherals 导出和包装局部图不保留。桌面原件及此前公开历史照片不删除。

10 月 11 日后续更新另增加 `hardware/BOM.md`、`hardware/exports/board-bom-detailed.csv` 与视频 `audio-processing.json`：电子 BOM 覆盖 61 个位号，视频记录提问阶段变调、来源与画面一致性校验。完整视频的发布路径未变，校验值随音轨更新；桌面原件保留。

## 演示视频

- `docs/videos/2026-10-10/README.md`
- `docs/videos/2026-10-10/voice-volume-answer-clip.mp4`
- `docs/videos/2026-10-10/voice-volume-control.mp4`
- `docs/videos/2026-10-10/wake-time-weather-answer-clip.mp4`
- `docs/videos/2026-10-10/wake-time-weather.mp4`

## 保留的 V1 图片与索引

- `docs/images/README.md`
- `docs/images/context-dialog-turns-1-2.png`
- `docs/images/context-dialog-turns-3-4.png`
- `docs/images/home-page.jpg`
- `docs/images/voice-listening.jpg`
- `docs/images/voice-ready.jpg`
- `docs/images/voice-speaking.jpg`
- `docs/images/voice-thinking.jpg`
- `docs/images/voxconductor-prototype.jpg`
- `docs/images/weather-page.jpg`

## 下一批代码整理

我会以当前 CMake 和真实依赖重新盘点，不能继续沿用 10 月 7 日含游戏模块的旧清单：

- 固件入口、板级驱动、UI、网络、存储和分区/模型配置。
- 语音、声学、诊断、工具、轮次与记忆模块，以及相关辅助头文件。
- 桌面、媒体、网页、设置、字体和固定提醒资源。
- 占位符配置示例、测试与桩、资源制作/SD 备份工具。
- 可选局域网示例与历史服务端，各自说明边界。

10 月 11 日后续开发已在本机执行固件编译；本批仍不烧录或发布这些代码。正式发布前需在干净克隆中检查依赖、资源和测试入口；密钥头文件、个人 SD 数据与含凭据的构建产物不进入清单。详细规则见 [发布准备](GITHUB_PUBLICATION_CHECKLIST.md)。

## 2026-10-11 硬件资料

当前硬件逐文件 SHA-256 见 [hardware/manifest.json](../hardware/manifest.json)，目录用途见 [硬件入口](../hardware/README.md)。保留 KiCad 主工程和必要项目库、当前板载 BOM/原理图 PDF/PCB 预览、原制板输出及供应商尺寸依据；结构只保留 V3-SLA、必要冻结输入、STEP/STL/FreeCAD/PPT 和校验。

旧 A～H 概念、V1/V2 整包、历史候选/备份、未使用的 Peripherals 独立原理图与旧 ST7789 封装、一次性改板脚本、缓存和重复 ZIP 移出活动目录。完整清理前快照单独保存在本机工程外，不推送 GitHub。
