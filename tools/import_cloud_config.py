"""从现有 server/.env 导入设备私有配置，不执行 shell、不输出凭据。"""
import json
import os
from pathlib import Path
import shlex

ROOT = Path(__file__).resolve().parents[1]


def main():
    values = {}
    for line in (ROOT / "server/.env").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:]
        key, sep, value = line.partition("=")
        if sep:
            parts = shlex.split(value, comments=True)
            values[key.strip()] = " ".join(parts)
    for key in values.keys() | {"DASHSCOPE_API_KEY", "DEEPSEEK_API_KEY"}:
        if key in os.environ:
            values[key] = os.environ[key]
    for key in ("DASHSCOPE_API_KEY", "DEEPSEEK_API_KEY"):
        if not values.get(key):
            raise SystemExit(f"缺少 {key}，未生成配置")
    base = values.get("DASHSCOPE_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1").rstrip("/")
    # TTS 使用现有 Python 实现的中国内地端点；其他地域需同时修改 URL 和密钥。
    config = {
        "CLOUD_DASHSCOPE_API_KEY": values["DASHSCOPE_API_KEY"],
        "CLOUD_DEEPSEEK_API_KEY": values["DEEPSEEK_API_KEY"],
        "CLOUD_ASR_URL": base + "/chat/completions",
        "CLOUD_ASR_MODEL": values.get("ASR_MODEL", "qwen3-asr-flash"),
        "CLOUD_ASR_REALTIME_URL": "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime",
        "CLOUD_DEEPSEEK_URL": "https://api.deepseek.com/chat/completions",
        "CLOUD_DEEPSEEK_MODEL": "deepseek-flash",
        "CLOUD_TTS_URL": "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime",
        "CLOUD_TTS_VOICE": values.get("TTS_VOICE", "Cherry"),
    }
    target = ROOT / "firmware/main/cloud_credentials.h"
    if target.exists():
        raise SystemExit("cloud_credentials.h 已存在，未覆盖；请直接编辑现有配置")
    with target.open("x") as out:
        os.chmod(target, 0o600)
        out.write("#pragma once\n// 本机私有云服务配置；禁止提交或公开固件。\n")
        for name, value in config.items():
            out.write(f"#define {name} {json.dumps(value, ensure_ascii=False)}\n")
    print("已导入私有云服务配置，未输出密钥。")


if __name__ == "__main__":
    main()
