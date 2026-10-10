# 局域网 ESP32 设备接入

本页描述开发版的可选接入方式；本次文档与素材提交不包含尚未发布的示例代码，也没有完成第二设备实机验收。请以对应代码发布版本为准。

主终端调用模型与工具，目标 ESP32 只提供固定开关接口。两者通过同一个局域网通信，不需要桌面服务；主终端的语音识别与回答仍需要互联网。

## 第二设备程序

`examples/lan_switch` 是独立 ESP-IDF 工程，只用于低压 LED 开关联调，不接门锁、加热器、电机或市电继电器。

在 VS Code 的另一窗口打开该目录，选择实际芯片目标，在 `menuconfig → VoxConductor LAN peer` 配置 Wi-Fi、16～128 字符随机共享令牌和安全可用的 LED GPIO。GPIO 默认 `-1`，不配置就拒绝写操作。选择 GPIO 前核对第二块开发板接线，不复用 Flash、USB 或启动相关引脚。

```bash
cd examples/lan_switch
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor
```

以上 target 仅适用于 ESP32-S3；其他型号选择对应目标。启动日志显示 IP，在路由器给设备保留固定地址。示例保存最后一次 SET 状态及命令 ID，复位会恢复该 LED 状态。

## 主终端白名单

复制 `firmware/main/device_registry.example.h` 为同目录的 `device_registry.h`，将空条目换成实际目标：

```c
static const app_device_t APP_DEVICES[] = {
    {"desk_light", "http://192.168.1.50", "与第二设备相同的随机令牌"},
};
```

这是格式示例，IP 和令牌必须替换。最多四个目标，URL 不带末尾斜杠。该文件已被 Git 忽略；示例的 `sdkconfig` 也被忽略，仍不得公开包含令牌的固件。主终端重新编译烧录后才应用白名单。

当前不会自动发现设备或将人类别名映射到 ID。测试时明确说出 `desk_light`；实际产品可将白名单 ID 改成容易识别的中文短名称，名称不超过 32 UTF-8 字节。模型只能提供注册 ID，不能提供 URL、端口、路径或 shell 命令。

## 协议

请求均携带 `Authorization: Bearer <token>`，响应为 JSON，不允许自动重定向。

`GET /state`：

```json
{"value": true}
```

`POST /command` 请求：

```json
{"command_id": "bootid-12", "value": true}
```

成功回执：

```json
{"command_id": "bootid-12", "value": true}
```

主终端只有收到 HTTP 200、相同命令 ID 和期望布尔状态才报告目标确认。响应上限 1024 字节、网络超时 3 秒；不自动重试。只支持 SET，不支持翻转状态的 toggle。

目标端将最后一次命令与状态作为一个 NVS blob 保存。重复最后一个 ID 且参数相同，返回原状态；参数不同返回 409。旧 ID 跨越多条后不是全历史去重，所以主终端不自动重放或重试。目标回执表示程序已提交 GPIO 状态，不表示负载有独立物理传感器回报。

用户说“打开 desk_light”，模型准备操作并复述；用户下一轮“确认执行”后，主终端先记录发送意图再发命令。超时后可能已执行也可能未执行，只能先查询状态，不能说“肯定没执行”。取消当前对话不会撤销已发送的命令。

## 网络边界

示例 HTTP 会在局域网暴露 bearer token，只用于可信网络实验，不开放端口到公网。HTTPS 目标可配置，但证书必须能被主终端信任；当前没有自动安装自签名证书的流程。量产设备应增加设备身份、加密、凭据轮换与更完整的状态协议。

冰箱食材列表、温湿度等不是布尔开关，应另加有大小上限的只读 schema 与对应工具，不通过字符串执行器或任意 URL 接口扩展。
