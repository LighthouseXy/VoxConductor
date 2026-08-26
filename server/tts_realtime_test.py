import base64
import os
import threading
import time
import wave
from pathlib import Path

import dashscope
from dashscope.audio.qwen_tts_realtime import (
    AudioFormat,
    QwenTtsRealtime,
    QwenTtsRealtimeCallback,
)


OUTPUT_PATH = Path(__file__).parent / "tts" / "realtime_test.wav"


class RealtimeCallback(QwenTtsRealtimeCallback):
    def __init__(self) -> None:
        super().__init__()

        self.done = threading.Event()
        self.started_at = 0.0
        self.first_audio_at = 0.0
        self.audio_bytes = 0

        OUTPUT_PATH.parent.mkdir(exist_ok=True)

        # 云端返回24kHz、16位、单声道PCM，
        # 直接写入WAV，方便在Mac上播放验证
        self.wav_file = wave.open(str(OUTPUT_PATH), "wb")
        self.wav_file.setnchannels(1)
        self.wav_file.setsampwidth(2)
        self.wav_file.setframerate(24000)

    def close_file(self) -> None:
        if self.wav_file is not None:
            self.wav_file.close()
            self.wav_file = None

    def on_open(self) -> None:
        print("实时TTS连接成功")

    def on_close(self, close_status_code, close_msg) -> None:
        self.close_file()
        self.done.set()
        print(
            f"实时TTS连接关闭："
            f"{close_status_code}, {close_msg}"
        )

    def on_event(self, response: dict) -> None:
        event_type = response.get("type")

        if event_type == "response.audio.delta":
            audio_chunk = base64.b64decode(response["delta"])

            if self.first_audio_at == 0.0:
                self.first_audio_at = time.perf_counter()
                print(
                    "首个音频块延迟："
                    f"{self.first_audio_at - self.started_at:.2f} 秒"
                )

            self.wav_file.writeframesraw(audio_chunk)
            self.audio_bytes += len(audio_chunk)

            print(f"收到音频块：{len(audio_chunk)} 字节")

        elif event_type == "session.finished":
            self.close_file()
            self.done.set()
            print("实时TTS生成完成")

        elif event_type == "error":
            print(f"实时TTS错误：{response}")
            self.done.set()


def main() -> None:
    api_key = os.environ.get("DASHSCOPE_API_KEY")
    voice = os.environ.get("TTS_VOICE", "Cherry")

    if not api_key:
        raise RuntimeError("没有找到DASHSCOPE_API_KEY")

    dashscope.api_key = api_key

    callback = RealtimeCallback()

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

        callback.started_at = time.perf_counter()

        client.append_text(
            "你好，我是VoxConductor桌面智能语音中枢，"
            "这是一段实时语音合成测试。"
        )

        # 通知云端文本已经发送完毕
        client.finish()

        if not callback.done.wait(timeout=30):
            raise TimeoutError("等待实时TTS完成超时")

    finally:
        client.close()
        callback.close_file()

    duration = callback.audio_bytes / 2 / 24000

    print(f"音频文件：{OUTPUT_PATH}")
    print(f"音频字节数：{callback.audio_bytes}")
    print(f"音频时长：{duration:.2f} 秒")


if __name__ == "__main__":
    main()
