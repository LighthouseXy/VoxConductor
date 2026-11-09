"""Source guards and injected-VAD boundary model; no C build or hardware access.

The installed native classifier's speech accuracy still needs device testing.
"""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]
audio = (ROOT / "firmware/main/app_audio.c").read_text()
asr = (ROOT / "firmware/main/app_cloud_asr.c").read_text()
main = (ROOT / "firmware/main/main.c").read_text()
config = (ROOT / "firmware/main/Kconfig.projbuild").read_text()
sdk = (ROOT / "firmware/sdkconfig").read_text()

assert "CONFIG_VOX_AUTO_END_LEVEL" not in audio + sdk
assert "config VOX_AUTO_END_LEVEL" not in config
assert "CONFIG_VOX_AUTO_END_VAD_MODE=0" in sdk
assert "vad_create_with_param(" in audio and "vad_process_with_trigger(vad, vad_frame)" in audio
assert "AUTO_VAD_FRAME_MS, 80, AUTO_VAD_FRAME_MS) : NULL;" in audio
assert "#define AUTO_START_CONFIRM_MS 200" in audio
assert "if (speech_run_ms >= AUTO_START_CONFIRM_MS) had_speech = true" in audio
assert "speech_run_max=%ums" in audio
assert "heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 4096" in audio
assert "if (vad) vad_destroy(vad);" in audio
assert "if (vad_used != AUTO_VAD_FRAME_SAMPLES) continue;" in audio
assert "vad_dc_q8 += (raw * 256 - vad_dc_q8) / 256" in audio
assert "int16_t)(s_mic_samples[i] >> 16)" in audio  # original ASR PCM
assert "if ((automatic || recorded_samples >= MIC_SAMPLE_RATE_HZ)" in audio
assert "stopped_without_activity = true;" in audio and "no_activity_submit" in audio
assert "!had_speech && recorded_samples >= MIC_SAMPLE_RATE_HZ * 5" not in audio
assert audio.index("*recorded_sample_count = recorded_samples;") < audio.index('"Recording failed:')
assert "Automatic recording started:" in audio and "Automatic capture:" in audio
assert 'automatic ? "正在聆听" : "松开发送"' in main
assert 'stage = "ASR"' not in main

messages = []
for literal in re.findall(r'"(?:\\.|[^"\\])*"', asr):
    decoded = json.loads(literal)
    if decoded.startswith('{"event_id":') and "%" not in decoded:
        messages.append(json.loads(decoded))
assert [m["type"] for m in messages] == ["session.update", "input_audio_buffer.commit", "session.finish"]
assert messages[0]["session"]["turn_detection"] is None
assert len({m["event_id"] for m in messages}) == len(messages)
assert asr.index("free(encoded); free(json);") < asr.index('stage = "commit"') < asr.index('"ASR submitted:')
assert asr.index('stage = "commit"') < asr.index('stage = "recognition_result"')
assert "sent == atomic_load_explicit(&s->available, memory_order_acquire)" in asr
assert "ASR failed: stage=%s" in asr and '"capture" : stage' in asr
assert asr.index("xEventGroupWaitBits(s->events, CAPTURE_DONE") < asr.index("esp_websocket_client_destroy(client)")

# Model VAD frame assembly (256-sample DMA blocks -> exact 320-sample input).
used = calls = 0
for _ in range(5 * 256):
    used += 1
    if used == 320:
        used = 0
        calls += 1
assert (calls, used) == (4, 0)

# Inject native speech/silence outcomes to test capture termination policy.
silence_ms = int(re.search(r"CONFIG_VOX_AUTO_END_SILENCE_MS=(\d+)", sdk)[1])
assert silence_ms == 800
def endpoint(states, preroll=False, stop_at=None):
    heard, quiet, run = preroll, 0, 0
    for index, speech in enumerate(states, 1):
        run = run + 20 if speech else 0
        heard = heard or run >= 200
        quiet = 0 if speech else quiet + 20
        if heard and quiet >= silence_ms:
            return "vad_silence", index * 20
        if not heard and index * 20 >= 8000:
            return "no_activity_submit", index * 20
        if stop_at == index:
            return "stop_request", index * 20
        if index * 20 >= 20000:
            return "duration_limit", index * 20
    return "recording", len(states) * 20

assert endpoint([False] * 50 + [True] * 100 + [False] * 60) == ("vad_silence", 3800)
assert endpoint([True] * 50 + [False] * 30 + [True] * 50 + [False] * 60) == ("vad_silence", 3400)
assert endpoint([True] * 50 + [False] * 40) == ("vad_silence", 1800)
assert endpoint([False] * 400) == ("no_activity_submit", 8000)
assert endpoint([True] * 1000) == ("duration_limit", 20000)
assert endpoint([False] * 60, preroll=True) == ("vad_silence", 800)
assert endpoint([False] * 100, stop_at=5) == ("stop_request", 100)
assert endpoint([True] * 4 + [False] * 60) == ("recording", 1280)
assert endpoint([True] * 4 + [False] * 100 + [True] * 70 + [False] * 60) == ("vad_silence", 4280)
assert endpoint(([True] * 4 + [False] * 20) * 17) == ("no_activity_submit", 8000)

token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for code in (audio, asr, main):
    stack = []
    for char in token.sub("", code):
        if char in "({[": stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char]
    assert not stack
print("PASS: auto capture source/order/lifetime guards and injected-VAD boundary model; C/native VAD/device NOT run")
