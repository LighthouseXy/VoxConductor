# VoxConductor | Prism-display desktop voice assistant

[简体中文](README.md) · [English](README.en.md) · [Chinese mirror](README.zh-CN.md)

VoxConductor is an ESP32-S3 desktop voice assistant with a custom PCB, a screen and beam-splitting prism, a microphone, a speaker, buttons, and motion input. The device handles its interface, audio, tool execution, and local records; ASR, model responses, and TTS use cloud services without a PC-hosted server.

The prism gives a floating appearance to a two-dimensional image. The firmware UI is Chinese-only; this English README does not imply an English device interface.

<p align="center">
  <img src="docs/images/2026-10-11/device-clock-front.jpg" alt="Assembled VoxConductor with the round clock" width="340">
</p>

Updated: 2026-11-03. Development sources and host tests are synchronized; photos and videos retain their original captured version. [Current progress](docs/PROGRESS.md) · [Validation](docs/SYNC_VALIDATION.md) · [Development history](DEVELOPMENT_HISTORY.en.md).

## Hardware

The 52 × 50 mm four-layer PCB uses an ESP32-S3-WROOM-1, a 240 × 240 ST7789-family display, an INMP441 microphone, a MAX98357A I2S amplifier, and an MPU motion sensor. Input uses physical buttons and tilt rather than a touchscreen; firmware uses ESP-IDF / FreeRTOS and LVGL 9.

<p align="center">
  <img src="docs/images/2026-10-11/device-front.jpg" alt="Device front" width="170">
  <img src="docs/images/2026-10-11/device-side.jpg" alt="Device side" width="170">
  <img src="docs/images/2026-10-11/device-rear.jpg" alt="Device rear" width="170">
  <img src="docs/images/2026-10-11/pcb-assembled.jpg" alt="Assembled custom PCB" width="170">
</p>

The [hardware project](hardware/README.md) contains KiCad, fabrication, and V3-SLA mechanical files. The [board BOM](hardware/BOM.md) covers 61 component positions, not the complete assembled device. The physical J2 repair is not yet reflected in the design, and V3-SLA still needs printed fit testing; revise and review the design before fabricating another board.

## Software features

| Area | Implemented in the current code |
| --- | --- |
| Desktop and weather | Clock, wallpapers, brightness and orientation; current weather, 12-hour forecast, UV, US AQI / PM2.5, wind, and humidity |
| Voice and tools | Local “小冰小冰” wake word, push-to-talk, follow-up turns, and cancellation; time/weather queries, volume/home settings, and bounded tool calls |
| Daily tools and media | Alarms, reminders, to-dos, notes, countdowns, and Pomodoro; SD music/ambient audio/wallpapers and a LAN management page |
| Records and recovery | Bounded recent context, memory with provenance and expiry, history search and deletion; SD text records and two checksummed snapshot slots |

<p align="center">
  <img src="docs/images/2026-10-11/desktop-menu.jpg" alt="Desktop tools menu" width="170">
  <img src="docs/images/2026-10-11/weather-current.jpg" alt="Current weather" width="170">
  <img src="docs/images/2026-10-11/weather-hourly.jpg" alt="Hourly forecast" width="170">
  <img src="docs/images/2026-10-11/weather-metrics.jpg" alt="Air quality, UV, wind, and humidity" width="170">
</p>

Weather comes from online services, not onboard environmental measurements; photo values reflect capture time only. The table describes code functionality; device reliability is distinguished by the validation levels below.

## Voice flow

1. A button or WakeNet starts capture; 16 kHz real-time ASR returns a transcript.
2. Exact local commands run directly; other questions enter a bounded model/tool loop. Example configuration uses Qwen ASR/TTS and DeepSeek, requiring internet access and your own service credentials.
3. The device validates arguments and confirmation rules, then returns actual execution results to the model; device actions with an unknown outcome are not automatically replayed.
4. After releasing the model connection, it opens a 24 kHz TTS session and plays audio through I2S. Successful turns then commit context and wait for another utterance within the follow-up window.

Model-proposed reminders and notes remain candidates until the next-turn exact command “确认执行” (confirm). “查看任务保存状态” reads disk snapshots; “重试任务保存” reconciles storage, restores runtime state for an existing ID without rewriting it, or requests confirmation again for an unsaved item. “取消执行” clears pending and retry authorization; cancellation or process reload does not automatically restore that authorization.

## Progress and remaining work

| Status | Evidence and scope |
| --- | --- |
| Demonstrated | Videos below show basic queries and volume control, not every behavior of the latest source |
| Offline checks passed | On 2026-11-03, five Agent host-test groups, task snapshot reconciliation/reload, and three source-contract checks passed, using mocks, virtual clocks, and fault injection |
| Build record | A local ESP32-S3 build passed on 2026-10-25; the November 3 sync branch was not rebuilt or flashed |
| Device acceptance pending | Wake-word interruption, playback continuity, web upload/save and voice contention, SD power-loss recovery, memory peaks, and end-to-end latency |
| Planned | Multi-device action previews, capability registration, multi-day task continuation, and controllable proactive reminders |

Voice still needs validation across distance, pronunciation, and noise conditions; stable full-duplex operation is not claimed. The 180-second software budget is not a hard real-time bound on SD or driver cleanup. Cloud-service availability, costs, and data terms must be reviewed separately.

## Demonstration videos

| Wake word, time and weather queries · about 26 s | Voice volume control · about 29 s |
| --- | --- |
| [![Wake and query cover](docs/images/2026-10-10/demo-time-weather-cover.jpg)](docs/videos/2026-10-10/wake-time-weather.mp4?raw=true) | [![Volume control cover](docs/images/2026-10-10/demo-volume-cover.jpg)](docs/videos/2026-10-10/voice-volume-control.mp4?raw=true) |

The question audio is pitch-shifted; visual sequence and duration are retained. See [video notes](docs/videos/2026-10-10/README.md). Individual demonstrations do not establish recognition accuracy, latency, or long-term stability; see [Showcase](docs/SHOWCASE.md) for more photos.

## Documentation and reproduction

The target is ESP32-S3 / ESP-IDF 6.0.2, with component versions in `firmware/dependencies.lock`. Follow [setup](docs/DEVICE_CLOUD_VOICE.md), create local configuration from templates such as the [Wi-Fi example](firmware/main/wifi_credentials.example.h), and supply a legally usable 24 kHz, 16-bit little-endian mono `firmware/main/reminder.pcm` before running `idf.py build` in `firmware`. Reminder audio and real credentials are not in the repository; a clean clone requires these prerequisites. Flash/PSRAM settings must match the physical board. SD conversations are plain text; keep private records out of uploads.

| Entry | Contents |
| --- | --- |
| [Documentation index](docs/README.md) | Full documentation directory |
| [Hardware design and assembly](docs/HARDWARE_DESIGN.md) / [Wiring](docs/HARDWARE_V1.md) | PCB, components, GPIO, and mechanical boundaries |
| [Technical architecture](docs/TECHNICAL_ARCHITECTURE.md) / [Voice and memory](docs/VOICE_DIALOGUE_AND_MEMORY_PLAN.md) | Pipeline, resources, execution, and persistence |
| [Features and pages](docs/FEATURES.md) / [Desktop interaction](docs/DESKTOP_TOOLS_AND_GAMES.md) | Pages and controls |
| [Web management](docs/WEB_MANAGEMENT_DESIGN.md) | Interfaces and acceptance steps |
| [Roadmap](docs/ROADMAP.md) / [Publication checklist](docs/GITHUB_PUBLICATION_CHECKLIST.md) | Priorities and publication scope |
| [Historical V1 architecture](docs/ARCHITECTURE.md) / [Earlier validation](docs/DEMO_AND_VERIFICATION.md) | Earlier FastAPI stage, not a required service in the current production flow |

## License and attribution

Only the project-authored software and original technical documentation identified in [License scope](LICENSE_SCOPE.md) and its inventories are covered by the [MIT License](LICENSE), permitting commercial use, modification, and redistribution subject to retaining copyright and permission notices. `hardware/**` (including mechanical scripts), dependencies, fonts, images, audiovisual material, and data are excluded from this project MIT grant and retain their own rights; see [Third-party notices](THIRD_PARTY_NOTICES.md).

Weather and air quality use Open-Meteo; air-quality sources include CAMS. See the [data-service documentation](https://open-meteo.com/en/docs/air-quality-api). ESP-IDF, LVGL, ESP-SR, cloud services, and third-party media have their own terms; music attribution remains with the media pack.
