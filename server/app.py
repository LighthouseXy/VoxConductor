from fastapi import FastAPI, Header, HTTPException, Request, Response
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field
from pathlib import Path
from weather_client import get_current_weather
import wave
import asyncio
import time

from deepseek_client import ask_deepseek
from asr_client import transcribe_audio
from tts_client import (
    TTS_OUTPUT_PATH,
    stream_speech,
    synthesize_speech,
)

from uuid import uuid4

from conversation_store import (
    clear_session,
    init_database,
    load_recent_messages,
    load_turn,
    save_turn,
)

app = FastAPI(title="Ambient Desk Voice Service")

# 当前ESP32使用固定会话；以后改为从请求头读取session_id
DEFAULT_SESSION_ID = "ambient-desk-01"

init_database()

AUDIO_SAMPLE_RATE = 16000
AUDIO_CHANNELS = 1
AUDIO_SAMPLE_WIDTH_BYTES = 2
AUDIO_MIN_SECONDS = 1
AUDIO_MAX_SECONDS = 20

AUDIO_MIN_BYTES = (
    AUDIO_SAMPLE_RATE
    * AUDIO_CHANNELS
    * AUDIO_SAMPLE_WIDTH_BYTES
    * AUDIO_MIN_SECONDS
)

AUDIO_MAX_BYTES = (
    AUDIO_SAMPLE_RATE
    * AUDIO_CHANNELS
    * AUDIO_SAMPLE_WIDTH_BYTES
    * AUDIO_MAX_SECONDS
)

RECORDINGS_DIR = Path(__file__).parent / "recordings"
# ponytail: 当前MVP只有一台设备；多设备并发时改用session_id/turn_id
latest_turn = {
    "transcript": "",
    "answer": "",
}

class ChatRequest(BaseModel):
    text: str = Field(min_length=1, max_length=2000)

@app.get("/health")
def health() -> dict[str, str]:
    # 只验证本地服务，不调用DeepSeek
    return {"status": "ok"}

@app.get("/weather/current")
async def current_weather() -> dict:
    try:
        return await asyncio.to_thread(get_current_weather)
    except RuntimeError as error:
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

@app.get("/conversation/history")
def get_conversation_history() -> dict:
    return {
        "session_id": DEFAULT_SESSION_ID,
        "messages": load_recent_messages(
            DEFAULT_SESSION_ID
        ),
    }


@app.post("/conversation/reset")
def reset_conversation() -> dict[str, str]:
    clear_session(DEFAULT_SESSION_ID)

    latest_turn["transcript"] = ""
    latest_turn["answer"] = ""

    return {
        "status": "ok",
        "session_id": DEFAULT_SESSION_ID,
    }

@app.get("/audio/latest")
def get_latest_audio_turn() -> dict[str, str]:
    if not latest_turn["transcript"]:
        raise HTTPException(
            status_code=404,
            detail="还没有完成的语音对话",
        )

    return latest_turn.copy()

TTS_TEST_TEXT = "你好，我是Ambient Desk，很高兴认识你。"

@app.get("/audio/turn/{turn_id}")
def get_audio_turn_by_id(
    turn_id: str,
    x_session_id: str = Header(
        ...,
        alias="X-Session-ID",
        min_length=1,
        max_length=64,
    ),
) -> dict[str, str]:
    # 必须同时匹配session_id和turn_id，避免取到其他轮次
    result = load_turn(x_session_id, turn_id)

    if result is None:
        raise HTTPException(
            status_code=404,
            detail="没有找到对应语音轮次",
        )

    return {
        "session_id": x_session_id,
        "turn_id": turn_id,
        **result,
    }

@app.get("/tts/test")
async def tts_test() -> Response:
    try:
        # 没有现成语音时才调用云端TTS，避免每次测试都重复计费
        if not TTS_OUTPUT_PATH.is_file():
            await asyncio.to_thread(
                synthesize_speech,
                TTS_TEST_TEXT,
            )

        # ESP32只需要PCM数据，因此在服务器端去掉WAV文件头
        with wave.open(str(TTS_OUTPUT_PATH), "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            sample_rate = wav_file.getframerate()
            pcm_data = wav_file.readframes(wav_file.getnframes())

    except (RuntimeError, OSError, wave.Error) as error:
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

    # 第一版只支持单声道、16位、24kHz，保持ESP32播放代码简单
    if channels != 1 or sample_width != 2 or sample_rate != 24000:
        raise HTTPException(
            status_code=500,
            detail=(
                "TTS音频格式不符合要求："
                f"{channels}声道，"
                f"{sample_width * 8}位，"
                f"{sample_rate}Hz"
            ),
        )

    return Response(
        content=pcm_data,
        media_type="application/octet-stream",
        headers={
            "X-Audio-Sample-Rate": str(sample_rate),
            "X-Audio-Channels": str(channels),
            "X-Audio-Bits": str(sample_width * 8),
        },
    )
@app.post("/tts/stream")
def tts_stream(request: ChatRequest) -> StreamingResponse:
    # 返回24kHz、16位、单声道原始PCM，不缓存完整音频
    return StreamingResponse(
        stream_speech(request.text),
        media_type="application/octet-stream",
        headers={
            "X-Audio-Sample-Rate": "24000",
            "X-Audio-Channels": "1",
            "X-Audio-Bits": "16",
        },
    )
@app.post("/audio/turn")
async def process_audio_turn(
    request: Request,
    x_session_id: str = Header(
        ...,
        alias="X-Session-ID",
        min_length=1,
        max_length=64,
    ),
    x_turn_id: str = Header(
        ...,
        alias="X-Turn-ID",
        min_length=1,
        max_length=64,
    ),
) -> Response:
# 从开始接收录音起统计整轮服务端耗时
    turn_started = time.perf_counter()

    receive_started = time.perf_counter()
    pcm_data = await request.body()
    receive_elapsed = time.perf_counter() - receive_started

    audio_seconds = (
        len(pcm_data)
        / AUDIO_SAMPLE_RATE
        / AUDIO_CHANNELS
        / AUDIO_SAMPLE_WIDTH_BYTES
    )

    print(
        f"收到录音：{len(pcm_data)}字节，"
        f"{audio_seconds:.2f}秒，"
        f"上传接收耗时：{receive_elapsed:.2f}秒"
    )

    if (
        len(pcm_data) < AUDIO_MIN_BYTES
        or len(pcm_data) > AUDIO_MAX_BYTES
        or len(pcm_data) % AUDIO_SAMPLE_WIDTH_BYTES != 0
    ):
        raise HTTPException(
            status_code=400,
            detail=(
                f"录音长度错误：收到{len(pcm_data)}字节，"
                f"允许范围为{AUDIO_MIN_BYTES}～{AUDIO_MAX_BYTES}字节"
            ),
        )

    RECORDINGS_DIR.mkdir(exist_ok=True)

    wav_path = RECORDINGS_DIR / "latest.wav"

    # ESP32上传的是16 kHz、16位、单声道原始PCM
    with wave.open(str(wav_path), "wb") as wav_file:
        wav_file.setnchannels(AUDIO_CHANNELS)
        wav_file.setsampwidth(AUDIO_SAMPLE_WIDTH_BYTES)
        wav_file.setframerate(AUDIO_SAMPLE_RATE)
        wav_file.writeframes(pcm_data)

    asr_started = time.perf_counter()
    try:
        # ASR是同步网络请求，放入工作线程，避免阻塞FastAPI
        transcript = await asyncio.to_thread(
            transcribe_audio,
            wav_path,
        )
    except (RuntimeError, TimeoutError) as error:
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

    asr_elapsed = time.perf_counter() - asr_started
    print(f"ASR耗时：{asr_elapsed:.2f} 秒")
    print(f"ASR识别结果：{transcript}")

    # 每次调用模型前读取最近6轮对话
    conversation_history = load_recent_messages(
        x_session_id
    )

    deepseek_started = time.perf_counter()
    try:
        # 将语音识别文字交给DeepSeek生成回答
        answer = await asyncio.to_thread(
            ask_deepseek,
            transcript,
            conversation_history,
        )
    except (RuntimeError, TimeoutError) as error:
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

    deepseek_elapsed = time.perf_counter() - deepseek_started
    print(f"DeepSeek耗时：{deepseek_elapsed:.2f} 秒")
    print(f"DeepSeek回答：{answer}")
        # 只有模型成功回答后才保存，失败请求不会污染上下文
    save_turn(
        x_session_id,
        x_turn_id,
        transcript,
        answer,
    )

    print(
        f"已保存会话：session={x_session_id}，"
        f"turn={x_turn_id}"
    )
    # TTS播放前保存本轮文字，供ESP32随后写入SD卡
    latest_turn["transcript"] = transcript
    latest_turn["answer"] = answer
    def answer_audio_stream():
        # 直到所有音频块发送完成，才统计本轮最终耗时
        tts_started = time.perf_counter()

        try:
            yield from stream_speech(answer)

        except Exception as error:
            print(f"实时TTS流发送失败：{error}")
            raise

        finally:
            tts_elapsed = time.perf_counter() - tts_started
            total_elapsed = time.perf_counter() - turn_started

            print(
                "本轮流式服务端耗时："
                f"上传接收={receive_elapsed:.2f}s，"
                f"ASR={asr_elapsed:.2f}s，"
                f"DeepSeek={deepseek_elapsed:.2f}s，"
                f"TTS流={tts_elapsed:.2f}s，"
                f"总计={total_elapsed:.2f}s"
            )

    # 不再等待完整WAV，实时TTS每生成一块就发送一块
    return StreamingResponse(
        answer_audio_stream(),
        media_type="application/octet-stream",
        headers={
            "X-Audio-Sample-Rate": "24000",
            "X-Audio-Channels": "1",
            "X-Audio-Bits": "16",

            # 把当前轮次身份返回给ESP32验证
            "X-Session-ID": x_session_id,
            "X-Turn-ID": x_turn_id,
        },
    )

@app.post("/chat")
def chat(request: ChatRequest) -> dict[str, str]:
    try:
        turn_id = uuid4().hex

        history = load_recent_messages(
            DEFAULT_SESSION_ID
        )

        answer = ask_deepseek(
            request.text,
            history,
        )

        save_turn(
            DEFAULT_SESSION_ID,
            turn_id,
            request.text,
            answer,
        )
    except RuntimeError as error:
        # DeepSeek失败时返回错误，但不关闭服务
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

    return {"answer": answer}
