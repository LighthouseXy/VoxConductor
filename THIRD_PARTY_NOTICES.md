# Third-party notices / 第三方边界

许可范围列明的项目原创软件与技术文档适用根 MIT。依赖、字体、素材、硬件、数据和云服务按各自条款使用。

## ESP-SR 2.5.4：具体条件与本项目目标

- 安装路径：`firmware/managed_components/espressif__esp-sr/LICENSE`；原文副本保留于 `licenses/ESP-SR-LICENSE.txt`。
- 版本依据：该组件的 idf_component.yml 为 2.5.4；项目 firmware/main/idf_component.yml 和 firmware/dependencies.lock 同样记录 2.5.4。组件来源提交为 `37f87981ddefdad5b50cc023acd4d90a411546ac`。
- 第 5 行关键条件：`Permission is hereby granted for use on all ESPRESSIF SYSTEMS products`（授权在所有 Espressif Systems 产品上使用）。该条件的产品范围为 Espressif 产品。
- 本项目 firmware/sdkconfig 的 `CONFIG_IDF_TARGET="esp32s3"`，属于这一产品范围。因此就此硬件条件而言，ESP32-S3 属于该许可所述产品范围；使用与再分发时仍须保留组件版权和许可通知。
- ESP-SR 按该组件自身的产品范围和许可条件使用；项目原创 app_*.c、工具与文档按其列明的 MIT 范围使用。未来移植到非 Espressif 产品时，须重新核对组件授权。
- 该版本的上游许可：[固定来源](https://github.com/espressif/esp-sr/blob/37f87981ddefdad5b50cc023acd4d90a411546ac/LICENSE)。组件内部若有更具体声明则继续保留；外部子依赖和模型不自动并入项目授权。

## 其他具体外部来源

| 路径/组件 | 实际依据 | 处理 |
| --- | --- | --- |
| firmware/main/app_ui_font_14.c、app_ui_font_16.c、app_ui_font_20.c、app_ui_font_weather_24.c | 生成头指向 SourceHanSansSC；原字体 LICENSE 为 OFL 1.1、Adobe 版权和保留名称 | 适用 OFL，保留原版权和保留名称条件 |
| firmware/main/app_ui_font_digits_56.c | 生成头指向 Montserrat-Medium.ttf；Montserrat Project Authors / OFL 1.1 | 保留字体版权与 OFL |
| firmware/managed_components/espressif__esp_websocket_client/** | 本地 LICENSE | Apache-2.0，保留声明/NOTICE |
| firmware/managed_components/espressif__esp_lvgl_port/** | 源文件 esp_lvgl_port.c 的 Espressif 版权、SPDX Apache-2.0 | 以各文件声明为依据使用，保留版权和许可通知 |
| firmware/managed_components/espressif__cjson/** | 本地 LICENSE，Dave Gamble/cJSON contributors | 按原 MIT 使用，保留原版权 |
| firmware/managed_components/lvgl__lvgl/** | 本地 LICENCE.txt，LVGL Kft | 主库 MIT；内部库与字体各自声明仍有效 |
| firmware/managed_components/espressif__esp-dl/** | 本地 LICENSE，Espressif Systems | 原 MIT；模型/内部第三方部分分别保留条款 |
| server 依赖 FastAPI、Pydantic、Uvicorn、DashScope | 已读取对应上游官方 LICENSE | 分别为 MIT、MIT、BSD-3-Clause、Apache-2.0；本项目原创 server/*.py 可独立采用 MIT |
| assets/media-pack/** 音乐 | 现有 ATTRIBUTION 列 Kevin MacLeod、CC BY 4.0、来源与转换说明 | 按原许可使用，保留逐曲归属与修改说明 |
| hardware/docs/** 及厂家附件 | 显示器、连接器、扬声器厂商原件 | 按厂商条款使用，重授权需取得权利人授权 |
| firmware/main/reminder.pcm | 现有生成工具指向 macOS Tingting 流程 | 保留原音源，来源与再分发权限待单独确认 |
| 实拍/截图/视频、生成壁纸、环境音、数据库及真实记录 | 媒体/数据边界 | 不包含在软件 MIT 范围，分别保留权利/隐私条件 |

上述材料受各自许可或资产权利约束。项目原创软件适用其 MIT 条件，第三方材料继续保留原版权和使用条件。

仍需资产级确认：厂商资料、照片视频的再分发范围，现有提醒音和媒体的具体权利；实际捆绑分发 SDK/模型及 Python extras 时，须核对并保留完整传递依赖通知。分发这些资产或依赖时，应一并保留所需版权及许可通知。

参考：[FastAPI](https://raw.githubusercontent.com/fastapi/fastapi/master/LICENSE)、[Pydantic](https://raw.githubusercontent.com/pydantic/pydantic/main/LICENSE)、[Uvicorn](https://raw.githubusercontent.com/encode/uvicorn/main/LICENSE.md)、[DashScope](https://raw.githubusercontent.com/dashscope/dashscope-sdk-python/main/LICENSE)、[标准 MIT](https://opensource.org/license/mit)。固定版本二进制包的完整许可与依赖仍需按实际分发内容核验。
