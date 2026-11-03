"""Local exit and DMA policy regression checks; no firmware compilation."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
main = root / "firmware/main"
control = (main / "app_control.c").read_text()
runner = (main / "main.c").read_text()
audio = (main / "app_audio.c").read_text()
acoustic = (main / "app_acoustic.c").read_text()
tts = (main / "app_cloud_tts.c").read_text()
matcher = control.split("bool app_control_end_session", 1)[1].split("static bool volume_number", 1)[0]
ends = re.findall(r'"([^"]+)"', matcher.split("const char *ends[] =", 1)[1].split("};", 1)[0])
assert "phrase(text, ends[i])" in matcher and "strstr" not in matcher

def exits(text):
    if text is None:
        return False
    text = text.lstrip(" \t")
    if text.startswith("小冰小冰"):
        text = text[4:].lstrip(" ,.!?:;\t\r\n，。：！？")
    for polite in ("请帮我", "帮我", "请"):
        if text.startswith(polite):
            text = text[len(polite):]
            break
    text = text.lstrip(" \t")
    return text.rstrip(" .!?\t\r\n。！？") in ends

for text in ends + ["请结束对话。", "小冰小冰，退出语音！", "小冰小冰不聊了", "  请帮我结束语音对话。"]:
    assert exits(text), text
for text in [None, "", "谢谢", "不要结束对话", "结束对话是什么意思", "结束对话后再叫我", "不聊了吗", "播放结束对话这首歌"]:
    assert not exits(text), text
turn = runner.split("static esp_err_t run_voice_turn", 1)[1].split("static void voice_conversation_task", 1)[0]
branch = turn.split("if (app_control_end_session(transcript))", 1)[1].split("#if", 1)[0]
for action in ("app_cloud_cancel()", "app_acoustic_discard()", "app_control_voice_cancel()", "goto finish"):
    assert action in branch
assert turn.index("app_control_end_session(transcript)") < turn.index("app_cloud_ask(")
assert 'if (!end_session)' in turn
assert '对话已结束' in turn
assert 'APP_AUDIO_FOLLOWUP_WAIT_SECONDS 12' in (main / "app_audio.h").read_text()
assert 'app_cloud_is_cancelled()) break;' in runner

def constant(name):
    return int(re.search(rf"#define {name} (\d+)", audio)[1])
descs, frames, rate = [constant(x) for x in ("SPEAKER_DMA_DESC_COUNT", "SPEAKER_DMA_FRAME_COUNT", "SPEAKER_SAMPLE_RATE_HZ")]
assert descs == 8 and frames == 256 and rate == 24000
assert 80 <= descs * frames * 1000 / rate <= 100
assert descs * frames * 4 - 4 * 160 * 4 < 6144  # bounded extra DMA SRAM
assert 'TTS_PLAYBACK_FRAMES_PER_BLOCK SPEAKER_DMA_FRAME_COUNT' in audio
assert 'channel_config.intr_priority = 3' in audio
assert 'CONFIG_I2S_ISR_IRAM_SAFE=y' in (root / 'firmware/sdkconfig').read_text()
assert '.on_sent = speaker_sent, .on_send_q_ovf = speaker_overflow' in audio
for fn in ('speaker_sent', 'speaker_overflow'):
    callback = audio.split('static bool IRAM_ATTR ' + fn, 1)[1].split('\n}', 1)[0]
    assert 'ESP_LOG' not in callback and 'malloc' not in callback
assert 'ref_write += app_audio_reference_extra_samples();' in acoustic
assert 200 * 16 + (descs * frames - 640) * 16000 // rate < 8192
assert 'SPEAKER_DMA_DESC_COUNT * SPEAKER_DMA_FRAME_COUNT * 2' in audio
assert tts.index('app_audio_playback_diagnostics(false)') < tts.index('while (!atomic_load(&stream->input_done))')
print('PASS: strict local exit, cancellation path, bounded aligned DMA and ISR diagnostics; device NOT tested')
