# Third-party notices / 第三方边界

根 MIT 只覆盖项目原创软件与技术文档。依赖、字体、素材、硬件、数据和云服务各保留自己的条款；以下条款不扩大到项目原创代码。

## ESP-SR 2.5.4：具体条件与本项目目标

- 安装路径：`firmware/managed_components/espressif__esp-sr/LICENSE`；原文副本保留于 `licenses/ESP-SR-LICENSE.txt`。
- 版本依据：该组件的 idf_component.yml 为 2.5.4；项目 firmware/main/idf_component.yml 和 firmware/dependencies.lock 同样记录 2.5.4。组件来源提交为 `37f87981ddefdad5b50cc023acd4d90a411546ac`。
- 第 5 行关键条件：`Permission is hereby granted for use on all ESPRESSIF SYSTEMS products`（授权在所有 Espressif Systems 产品上使用）。该文本没有将授权限定为 ESP32-S3 一颗芯片，而是覆盖 Espressif 产品。
- 本项目 firmware/sdkconfig 的 `CONFIG_IDF_TARGET="esp32s3"`，属于这一产品范围。因此就此硬件条件而言，ESP32-S3 属于该许可所述产品范围；使用与再分发时仍须保留组件版权和许可通知。
- 该条件属于 ESP-SR 组件自身；不能把它改成标准无限制 MIT，也不能因此断言项目原创 app_*.c、工具或文档不能采用标准 MIT。未来若将组件运行于非 Espressif 产品上，须重新核对其授权，不凭根 MIT 推导权利。
- 该版本的上游许可：[固定来源](https://github.com/espressif/esp-sr/blob/37f87981ddefdad5b50cc023acd4d90a411546ac/LICENSE)。组件内部若有更具体声明则继续保留；外部子依赖和模型不自动并入项目授权。

## 其他具体外部来源

| 路径/组件 | 实际依据 | 处理 |
| --- | --- | --- |
| firmware/main/app_ui_font_14.c、app_ui_font_16.c、app_ui_font_20.c、app_ui_font_weather_24.c | 生成头指向 SourceHanSansSC；原字体 LICENSE 为 OFL 1.1、Adobe 版权和保留名称 | 保留 OFL，不赋予项目 MIT |
| firmware/main/app_ui_font_digits_56.c | 生成头指向 Montserrat-Medium.ttf；Montserrat Project Authors / OFL 1.1 | 保留字体版权与 OFL |
| firmware/managed_components/espressif__esp_websocket_client/** | 本地 LICENSE | Apache-2.0，保留声明/NOTICE |
| firmware/managed_components/espressif__esp_lvgl_port/** | 源文件 esp_lvgl_port.c 的 Espressif 版权、SPDX Apache-2.0 | 保留；根目录缺独立 LICENSE 不等于无权使用，也不授权删除各文件声明 |
| firmware/managed_components/espressif__cjson/** | 本地 LICENSE，Dave Gamble/cJSON contributors | 原 MIT 及原版权，不能改成项目作者的版权 |
| firmware/managed_components/lvgl__lvgl/** | 本地 LICENCE.txt，LVGL Kft | 主库 MIT；内部库与字体各自声明仍有效 |
| firmware/managed_components/espressif__esp-dl/** | 本地 LICENSE，Espressif Systems | 原 MIT；模型/内部第三方部分分别保留条款 |
| server 依赖 FastAPI、Pydantic、Uvicorn、DashScope | 已读取对应上游官方 LICENSE | 分别为 MIT、MIT、BSD-3-Clause、Apache-2.0；本项目原创 server/*.py 可独立采用 MIT |
| assets/media-pack/** 音乐 | 现有 ATTRIBUTION 列 Kevin MacLeod、CC BY 4.0、来源与转换说明 | 保留逐曲归属与修改说明，音频不进入项目 MIT |
| hardware/docs/** 及厂家附件 | 显示器、连接器、扬声器厂商原件 | 不推定作者有重授权权利；独立于原创软件 MIT |
| firmware/main/reminder.pcm | 现有生成工具指向 macOS Tingting 流程 | 仅记录现状；不纳入 MIT、不替换音源，不声称已获得音频再分发权利 |
| 实拍/截图/视频、生成壁纸、环境音、数据库及真实记录 | 媒体/数据边界 | 不包含在软件 MIT 范围，分别保留权利/隐私条件 |

上述材料受各自许可或资产权利约束。项目原创软件采用 MIT 不会取消这些条件，也不会将第三方版权转移给项目作者。

仍需资产级确认：厂商资料、照片视频的再分发范围，现有提醒音和媒体的具体权利；SDK/模型及 Python extras 的完整传递依赖通知在实际捆绑分发时继续保留，不能由根 MIT 代替。分发这些资产或依赖时，应一并保留所需版权及许可通知。

参考：[FastAPI](https://raw.githubusercontent.com/fastapi/fastapi/master/LICENSE)、[Pydantic](https://raw.githubusercontent.com/pydantic/pydantic/main/LICENSE)、[Uvicorn](https://raw.githubusercontent.com/encode/uvicorn/main/LICENSE.md)、[DashScope](https://raw.githubusercontent.com/dashscope/dashscope-sdk-python/main/LICENSE)、[标准 MIT](https://opensource.org/license/mit)。当前上游文本不是对每个固定版本二进制包内容的穷尽核验。
