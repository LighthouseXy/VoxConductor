from fastapi import FastAPI, Header, HTTPException, Request, Response
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field
from pathlib import Path
from weather_client import get_current_weather
import wave
import asyncio
import time
import os

from deepseek_client import MODEL as DEEPSEEK_MODEL, ask_deepseek
from asr_client import transcribe_audio
from tts_client import (
    TTS_OUTPUT_PATH,
    stream_speech,
    synthesize_speech,
)

from uuid import uuid4

from conversation_store import (
    MAX_CONTEXT_MESSAGES,
    clear_session,
    init_database,
    load_recent_messages,
    load_turn,
    save_turn,
)

app = FastAPI(title="VoxConductor Voice Service")

# 当前ESP32使用固定会话；以后改为从请求头读取session_id
DEFAULT_SESSION_ID = "voxconductor-01"

init_database()
LOG_SEPARATOR = "─" * 44


def compact_text(text: str, limit: int = 100) -> str:
    """把多行文本整理成适合终端显示的一行。"""
    result = " ".join(text.split())

    if len(result) <= limit:
        return result

    return result[: limit - 1] + "…"


def print_service_banner() -> None:
    """显示服务端当前运行配置，不输出密钥。"""
    asr_model = os.environ.get("ASR_MODEL", "qwen3-asr-flash")
    tts_model = "qwen3-tts-flash-realtime"

    print(
        "\n"
        "VoxConductor Voice Service\n"
        f"{LOG_SEPARATOR}\n"
        "状态    已启动\n"
        "地址    http://0.0.0.0:8000\n"
        f"会话    {DEFAULT_SESSION_ID}\n"
        f"上下文  最近 {MAX_CONTEXT_MESSAGES // 2} 轮\n"
        f"ASR     {asr_model}\n"
        f"LLM     {DEEPSEEK_MODEL}\n"
        f"TTS     {tts_model}\n"
        f"{LOG_SEPARATOR}",
        flush=True,
    )


def print_turn_failure(
    turn_tag: str,
    stage: str,
    turn_started: float,
    error: Exception,
) -> None:
    """用统一格式显示本轮失败原因。"""
    print(
        "\n"
        f"TURN {turn_tag} · 执行失败\n"
        f"{LOG_SEPARATOR}\n"
        f"阶段    {stage}\n"
        f"耗时    {time.perf_counter() - turn_started:.2f} 秒\n"
        f"原因    {compact_text(str(error), 120)}\n"
        "结果    失败\n"
        f"{LOG_SEPARATOR}",
        flush=True,
    )

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

print_service_banner()

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

TTS_TEST_TEXT = "你好，我是VoxConductor桌面智能语音中枢，很高兴认识你。"

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
    # 短标识只用于终端显示，数据库仍保存完整turn_id
    turn_tag = x_turn_id[-8:]
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

    if (
        len(pcm_data) < AUDIO_MIN_BYTES
        or len(pcm_data) > AUDIO_MAX_BYTES
        or len(pcm_data) % AUDIO_SAMPLE_WIDTH_BYTES != 0
    ):
        error = ValueError(
            f"录音长度为{len(pcm_data)}字节，"
            f"允许范围为{AUDIO_MIN_BYTES}～{AUDIO_MAX_BYTES}字节"
        )
        print_turn_failure(
            turn_tag,
            "录音校验",
            turn_started,
            error,
        )
        raise HTTPException(
            status_code=400,
            detail=str(error),
        )

    RECORDINGS_DIR.mkdir(exist_ok=True)
    wav_path = RECORDINGS_DIR / "latest.wav"

    # ESP32上传的是16kHz、16位、单声道原始PCM
    with wave.open(str(wav_path), "wb") as wav_file:
        wav_file.setnchannels(AUDIO_CHANNELS)
        wav_file.setsampwidth(AUDIO_SAMPLE_WIDTH_BYTES)
        wav_file.setframerate(AUDIO_SAMPLE_RATE)
        wav_file.writeframes(pcm_data)

    asr_started = time.perf_counter()

    try:
        transcript = await asyncio.to_thread(
            transcribe_audio,
            wav_path,
        )
    except (RuntimeError, TimeoutError) as error:
        print_turn_failure(
            turn_tag,
            "ASR",
            turn_started,
            error,
        )
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

    asr_elapsed = time.perf_counter() - asr_started

    # 每两条消息代表一轮用户与助手对话
    conversation_history = load_recent_messages(x_session_id)
    context_turns = len(conversation_history) // 2

    deepseek_started = time.perf_counter()

    try:
        answer = await asyncio.to_thread(
            ask_deepseek,
            transcript,
            conversation_history,
        )
    except (RuntimeError, TimeoutError) as error:
        print_turn_failure(
            turn_tag,
            "DeepSeek",
            turn_started,
            error,
        )
        raise HTTPException(
            status_code=502,
            detail=str(error),
        ) from error

    deepseek_elapsed = time.perf_counter() - deepseek_started

    # 只有模型成功回答后才保存，失败请求不会污染上下文
    save_turn(
        x_session_id,
        x_turn_id,
        transcript,
        answer,
    )

    latest_turn["transcript"] = transcript
    latest_turn["answer"] = answer

    def answer_audio_stream():
        tts_started = time.perf_counter()
        first_audio_elapsed = None
        tts_first_chunk_elapsed = None
        audio_bytes = 0
        chunk_count = 0

        try:
            for audio_chunk in stream_speech(answer):
                if first_audio_elapsed is None:
                    first_audio_at = time.perf_counter()

                    # 用户感受到的响应时间：请求开始到首个语音块
                    first_audio_elapsed = (
                        first_audio_at - turn_started
                    )
                    tts_first_chunk_elapsed = (
                        first_audio_at - tts_started
                    )

                audio_bytes += len(audio_chunk)
                chunk_count += 1
                yield audio_chunk

            if first_audio_elapsed is None:
                raise RuntimeError("TTS没有返回任何音频数据")

        except Exception as error:
            print_turn_failure(
                turn_tag,
                "TTS",
                turn_started,
                error,
            )
            raise

        # 24kHz、16位、单声道，每秒为48000字节
        generated_audio_seconds = audio_bytes / 48000

        print(
            "\n"
            f"TURN {turn_tag} · 上下文 {context_turns} 轮\n"
            f"{LOG_SEPARATOR}\n"
            f"录音    {audio_seconds:.2f} 秒\n"
            f"用户    {compact_text(transcript)}\n"
            f"助手    {compact_text(answer)}\n"
            "\n"
            f"响应    {first_audio_elapsed:.2f} 秒（首个音频块）\n"
            "阶段    "
            f"接收 {receive_elapsed:.2f}｜"
            f"ASR {asr_elapsed:.2f}｜"
            f"模型 {deepseek_elapsed:.2f}｜"
            f"TTS首包 {tts_first_chunk_elapsed:.2f}\n"
            f"音频    {generated_audio_seconds:.2f} 秒｜"
            f"{chunk_count} 块\n"
            "结果    成功\n"
            f"{LOG_SEPARATOR}",
            flush=True,
        )

    return StreamingResponse(
        answer_audio_stream(),
        media_type="application/octet-stream",
        headers={
            "X-Audio-Sample-Rate": "24000",
            "X-Audio-Channels": "1",
            "X-Audio-Bits": "16",
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
