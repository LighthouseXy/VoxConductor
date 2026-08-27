import json
import os
import urllib.error
import urllib.request
import wave
from pathlib import Path

import base64
from collections.abc import Iterator
from queue import Empty, Queue

import dashscope
from dashscope.audio.qwen_tts_realtime import (
    AudioFormat,
    QwenTtsRealtime,
    QwenTtsRealtimeCallback,
)


TTS_API_URL = (
    "https://dashscope.aliyuncs.com/api/v1/"
    "services/aigc/multimodal-generation/generation"
)

TTS_OUTPUT_PATH = (
    Path(__file__).parent / "tts" / "latest.wav"
)

class RealtimeTtsCallback(QwenTtsRealtimeCallback):
    def __init__(self) -> None:
        super().__init__()

        # SDK回调运行在其他线程，Queue负责安全地把PCM交给HTTP响应
        self.audio_queue: Queue[bytes | None] = Queue()
        self.error: RuntimeError | None = None
        self.finished = False

    def on_open(self) -> None:
        pass

    def on_close(self, close_status_code, close_msg) -> None:
        # 没有收到session.finished即关闭，应当按失败处理
        if not self.finished and self.error is None:
            self.error = RuntimeError(
                "实时TTS连接意外关闭："
                f"{close_status_code}, {close_msg}"
            )

        if not self.finished:
            self.audio_queue.put(None)

    def on_event(self, response: dict) -> None:
        event_type = response.get("type")

        if event_type == "response.audio.delta":
            audio_chunk = base64.b64decode(response["delta"])

            self.audio_queue.put(audio_chunk)

        elif event_type == "session.finished":
            # None表示流结束，不会被当成音频发送
            self.finished = True
            self.audio_queue.put(None)

        elif event_type == "error":
            self.error = RuntimeError(
                f"实时TTS返回错误：{response}"
            )
            self.audio_queue.put(None)


def stream_speech(text: str) -> Iterator[bytes]:
    api_key = os.environ.get("DASHSCOPE_API_KEY")
    voice = os.environ.get("TTS_VOICE", "Cherry")

    text = text.strip()

    if not api_key:
        raise RuntimeError("没有找到DASHSCOPE_API_KEY")

    if not text:
        raise RuntimeError("TTS文本不能为空")

    dashscope.api_key = api_key

    callback = RealtimeTtsCallback()

    client = QwenTtsRealtime(
        model="qwen3-tts-flash-realtime",
        callback=callback,
        url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
    )

    try:
        client.connect()

        client.update_session(
            voice=voice,
            response_format=AudioFormat.PCM_24000HZ_MONO_16BIT,
            language_type="Chinese",
            mode="server_commit",
        )

        client.append_text(text)
        client.finish()

        while True:
            try:
                audio_chunk = callback.audio_queue.get(
                    timeout=30
                )
            except Empty as error:
                raise TimeoutError(
                    "等待实时TTS音频块超时"
                ) from error

            if audio_chunk is None:
                break

            # StreamingResponse每次取得一个块就立即发送
            yield audio_chunk

        if callback.error:
            raise callback.error

    finally:
        client.close()

def synthesize_speech(
    text: str,
    output_path: Path = TTS_OUTPUT_PATH,
) -> Path:
    api_key = os.environ.get("DASHSCOPE_API_KEY")
    model = os.environ.get("TTS_MODEL", "qwen3-tts-flash")
    voice = os.environ.get("TTS_VOICE", "Cherry")

    text = text.strip()

    if not api_key:
        raise RuntimeError(
            "没有找到DASHSCOPE_API_KEY，请先加载server/.env"
        )

    if not text:
        raise RuntimeError("TTS文本不能为空")

    request_body = {
        "model": model,
        "input": {
            "text": text,
            "voice": voice,
            "language_type": "Chinese",
        },
    }

    request = urllib.request.Request(
        TTS_API_URL,
        data=json.dumps(
            request_body,
            ensure_ascii=False,
        ).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )

    print(f"正在调用TTS模型：{model}")
    print(f"使用音色：{voice}")

    try:
        with urllib.request.urlopen(
            request,
            timeout=90,
        ) as response:
            response_body = json.load(response)

    except urllib.error.HTTPError as error:
        error_body = error.read().decode(
            "utf-8",
            errors="replace",
        )

        raise RuntimeError(
            f"TTS请求失败：HTTP {error.code}\n{error_body}"
        ) from error

    except urllib.error.URLError as error:
        raise RuntimeError(
            f"无法连接阿里云TTS：{error.reason}"
        ) from error

    try:
        audio_url = response_body["output"]["audio"]["url"]
    except (KeyError, TypeError) as error:
        raise RuntimeError(
            "TTS返回格式与预期不符：\n"
            + json.dumps(
                response_body,
                ensure_ascii=False,
                indent=2,
            )
        ) from error

    if not isinstance(audio_url, str) or not audio_url:
        raise RuntimeError("TTS没有返回有效音频地址")

    try:
        with urllib.request.urlopen(
            audio_url,
            timeout=60,
        ) as response:
            wav_data = response.read()

    except urllib.error.URLError as error:
        raise RuntimeError(
            f"下载TTS音频失败：{error.reason}"
        ) from error

    # 先检查RIFF/WAVE头，避免把错误页面保存成音频
    if (
        len(wav_data) < 44
        or wav_data[:4] != b"RIFF"
        or wav_data[8:12] != b"WAVE"
    ):
        raise RuntimeError("下载结果不是有效的WAV文件")

    output_path.parent.mkdir(exist_ok=True)
    output_path.write_bytes(wav_data)

    return output_path


def main() -> None:
    output_path = synthesize_speech(
        "你好，我是VoxConductor桌面智能语音中枢，很高兴认识你。"
    )

    # 读取WAV参数，为后续ESP32播放确定采样格式
    with wave.open(str(output_path), "rb") as wav_file:
        channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        sample_rate = wav_file.getframerate()
        frame_count = wav_file.getnframes()
        duration = frame_count / sample_rate

    print(f"音频已保存：{output_path}")
    print(f"声道数：{channels}")
    print(f"采样位数：{sample_width * 8}")
    print(f"采样率：{sample_rate} Hz")
    print(f"时长：{duration:.2f} 秒")


if __name__ == "__main__":
    main()
