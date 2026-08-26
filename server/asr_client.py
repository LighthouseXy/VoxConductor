import base64
import json
import os
import urllib.error
import urllib.request
from pathlib import Path


AUDIO_PATH = Path(__file__).parent / "recordings" / "latest.wav"


def transcribe_audio(audio_path: Path) -> str:
    api_key = os.environ.get("DASHSCOPE_API_KEY")
    base_url = os.environ.get("DASHSCOPE_BASE_URL", "").rstrip("/")
    model = os.environ.get("ASR_MODEL", "qwen3-asr-flash")

    # 先检查配置，避免把环境变量问题误判成ASR问题
    if not api_key:
        raise RuntimeError("没有找到DASHSCOPE_API_KEY，请先加载server/.env")

    if not base_url:
        raise RuntimeError("没有找到DASHSCOPE_BASE_URL，请检查server/.env")

    if not audio_path.is_file():
        raise RuntimeError(f"没有找到录音文件：{audio_path}")

    # 千问ASR通过Data URL接收Base64编码的WAV文件
    audio_base64 = base64.b64encode(audio_path.read_bytes()).decode("ascii")
    audio_data_url = f"data:audio/wav;base64,{audio_base64}"

    request_body = {
        "model": model,
        "messages": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "input_audio",
                        "input_audio": {
                            "data": audio_data_url,
                        },
                    }
                ],
            }
        ],
        "stream": False,
    }

    request = urllib.request.Request(
        f"{base_url}/chat/completions",
        data=json.dumps(request_body).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )

    print(f"正在识别：{audio_path.name}")
    print(f"使用模型：{model}")

    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            response_body = json.load(response)

    except urllib.error.HTTPError as error:
        error_body = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"千问ASR请求失败：HTTP {error.code}\n{error_body}"
        ) from error

    except urllib.error.URLError as error:
        raise RuntimeError(
            f"无法连接阿里云百炼：{error.reason}"
        ) from error

    try:
        transcript = response_body["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as error:
        raise RuntimeError(
            "千问ASR返回格式与预期不符：\n"
            + json.dumps(response_body, ensure_ascii=False, indent=2)
        ) from error

    if not isinstance(transcript, str) or not transcript.strip():
        raise RuntimeError("千问ASR没有返回有效文字")

    return transcript.strip()


def main() -> None:
    transcript = transcribe_audio(AUDIO_PATH)

    print("\n识别结果：")
    print(transcript)


if __name__ == "__main__":
    main()