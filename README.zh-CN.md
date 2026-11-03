# VoxConductor｜棱镜显示桌面语音助手

[简体中文](README.md) · [English](README.en.md) · [中文副本](README.zh-CN.md)

VoxConductor 是一台基于 ESP32-S3 的桌面语音助手，整合定制 PCB、屏幕与分光棱镜、麦克风、扬声器、按键和姿态交互。设备负责界面、音频、工具执行与本地记录；ASR、模型问答和 TTS 由设备直连云端服务完成。

棱镜将屏幕的二维画面反射为悬浮观感。

<p align="center">
  <img src="docs/images/2026-10-11/device-clock-front.jpg" alt="VoxConductor 整机与圆形时钟" width="340">
</p>

更新：2026-11-03。[最新进度](docs/PROGRESS.md) · [验证记录](docs/SYNC_VALIDATION.md) · [开发与维护记录](DEVELOPMENT_HISTORY.md)。

## 硬件

52 × 50 mm 四层 PCB，ESP32-S3-WROOM-1，240 × 240 ST7789 系列显示屏，INMP441 麦克风、MAX98357A I2S 功放及 MPU 姿态传感器。输入由实体按键和倾斜操作完成；固件基于 ESP-IDF / FreeRTOS 与 LVGL 9。

<p align="center">
  <img src="docs/images/2026-10-11/device-front.jpg" alt="整机正面" width="170">
  <img src="docs/images/2026-10-11/device-side.jpg" alt="整机侧面" width="170">
  <img src="docs/images/2026-10-11/device-rear.jpg" alt="整机后侧" width="170">
  <img src="docs/images/2026-10-11/pcb-assembled.jpg" alt="已装配元件的定制 PCB" width="170">
</p>

[硬件工程](hardware/README.md) 包含 KiCad、制造和 V3-SLA 结构资料；[板级 BOM](hardware/BOM.md) 收录 61 个器件位置。

## 软件功能

| 方向 | 当前代码能力 |
| --- | --- |
| 桌面与天气 | 时钟、壁纸、亮度与方向设置；当前天气、未来 12 小时预报、UV、US AQI / PM2.5、风与湿度 |
| 语音与工具 | 本地“小冰小冰”唤醒、按键说话、连续接话与取消；时间/天气查询、音量/主页设置及受限工具调用 |
| 日常与媒体 | 闹钟、提醒、待办、备忘录、倒计时、番茄钟；SD 音乐/环境音/壁纸与同网网页管理 |
| 记录与恢复 | 有界近期上下文、带来源和有效期的记忆、历史检索与删除；SD 文字记录和双槽校验快照 |

<p align="center">
  <img src="docs/images/2026-10-11/desktop-menu.jpg" alt="桌面工具菜单" width="170">
  <img src="docs/images/2026-10-11/weather-current.jpg" alt="当前天气" width="170">
  <img src="docs/images/2026-10-11/weather-hourly.jpg" alt="小时预报" width="170">
  <img src="docs/images/2026-10-11/weather-metrics.jpg" alt="空气质量、UV、风与湿度" width="170">
</p>

## 语音流程

1. 按键或 WakeNet 触发采集，16 kHz 实时 ASR 返回转写。
2. 本地精确命令直接处理；其他问题进入有限模型工具循环。示例配置使用 Qwen ASR/TTS 与 DeepSeek。
3. 设备校验参数与确认条件，执行后将真实结果交回模型。
4. 释放模型连接后建立 24 kHz TTS 会话，通过 I2S 播放；成功轮次结束后提交上下文，并在连续对话窗口等待下一句。

模型提出的提醒和备忘录先形成候选，须下一轮精确“确认执行”。“查看任务保存状态”读取磁盘快照；“重试任务保存”以稳定任务 ID 对账、恢复运行态，并对待保存候选重新请求确认。“取消执行”清除待确认与重试授权。

## 当前进度与待优化项

开发源码与宿主测试已同步；固件当前提供中文界面，语音按轮次协调采集与播放。

| 状态 | 依据与后续事项 |
| --- | --- |
| 演示资料 | 2026-10-10 两段视频记录基础查询与音量控制；10 月 11 日整机和天气照片记录拍摄时刻的设备状态与网络服务数据 |
| 已做离线验证 | 2026-11-03 Agent 五组宿主测试、任务快照对账/重载及三项源码契约检查通过；包含模拟输入、虚拟时钟与故障注入 |
| 构建记录 | 2026-10-25 本地 ESP32-S3 构建通过；11 月 3 日同步分支的重新构建与刷机仍待开展 |
| 运行前提 | 云端问答需互联网和自有服务凭据；自行提供有使用权的 24 kHz、16-bit little-endian 单声道 `firmware/main/reminder.pcm`；Flash/PSRAM 配置须匹配实物 |
| 执行与隐私 | 结果未知的设备动作等待用户查询与对账；任务对账以稳定 ID 去重写入；取消或进程重载使确认及重试授权失效，再次执行须重新确认；SD 对话以明文保存，凭据、提醒音和私人记录保留本地 |
| 硬件修订 | KiCad 保留 J2 返修前设计，待回写实物修正并重新审图；V3-SLA 待打印试装；整机采购项需在板级 BOM 基础上补齐 |
| 待实机验收 | 按距离、发音和噪声环境验证唤醒与打断、回声控制、播报连续性、网页上传/保存与语音争用、SD 断电恢复、内存峰值和端到端延迟 |
| 后续测量与计划 | 识别率、唤醒成功率、延迟和长期稳定性待受控统计测试；多设备行动预览、能力注册、跨天任务续办与可控主动提醒待实现 |

180 秒软件预算用于约束轮次处理；SD 同步与驱动清理的尾延迟、硬实时能力和全双工稳定性仍待专项验证。使用云服务时需核对供应商的可用性、费用与数据条款。

## 演示视频

| 唤醒、时间与天气查询 · 约 26 秒 | 语音调节音量 · 约 29 秒 |
| --- | --- |
| [![唤醒与查询封面](docs/images/2026-10-10/demo-time-weather-cover.jpg)](docs/videos/2026-10-10/wake-time-weather.mp4?raw=true) | [![音量控制封面](docs/images/2026-10-10/demo-volume-cover.jpg)](docs/videos/2026-10-10/voice-volume-control.mp4?raw=true) |

提问阶段音轨经过变调，原画面顺序与时长保留。[视频处理记录](docs/videos/2026-10-10/README.md) · [展示图集](docs/SHOWCASE.md)。

## 文档与复现

目标为 ESP32-S3 / ESP-IDF 6.0.2，组件版本记录于 `firmware/dependencies.lock`。按[配置说明](docs/DEVICE_CLOUD_VOICE.md)，从 [Wi-Fi 示例](firmware/main/wifi_credentials.example.h)等模板创建本地配置，准备好提醒 PCM 后，在 `firmware` 目录运行 `idf.py build`。

| 入口 | 内容 |
| --- | --- |
| [文档导航](docs/README.md) | 完整资料入口 |
| [硬件设计与装配](docs/HARDWARE_DESIGN.md) / [接线](docs/HARDWARE_V1.md) | PCB、部件、GPIO 与结构边界 |
| [技术架构](docs/TECHNICAL_ARCHITECTURE.md) / [语音与记忆](docs/VOICE_DIALOGUE_AND_MEMORY_PLAN.md) | 链路、资源、执行和持久化 |
| [功能与页面](docs/FEATURES.md) / [桌面交互](docs/DESKTOP_TOOLS_AND_GAMES.md) | 页面和操作说明 |
| [网页管理](docs/WEB_MANAGEMENT_DESIGN.md) | 接口与验收方法 |
| [后续计划](docs/ROADMAP.md) / [发布清单](docs/GITHUB_PUBLICATION_CHECKLIST.md) | 优先级与公开范围 |
| [V1 历史架构](docs/ARCHITECTURE.md) / [旧验证记录](docs/DEMO_AND_VERIFICATION.md) | 早期 FastAPI 阶段的架构与验证记录 |

## 许可与归属

[许可范围](LICENSE_SCOPE.md)及其清单列明的项目原创软件与原创技术文档适用 [MIT License](LICENSE)，允许按许可商业使用、修改和再分发，须保留版权及许可声明。`hardware/**`（含机械脚本）、依赖、字体、图片、影音和数据按[第三方说明](THIRD_PARTY_NOTICES.md)及各自适用条款使用。

天气与空气质量来自 Open-Meteo，空气质量来源包含 CAMS，见[数据服务说明](https://open-meteo.com/en/docs/air-quality-api)。ESP-IDF、LVGL、ESP-SR、云服务及第三方媒体适用各自条款；音乐归属随媒体包保留。
