"""Computer interruption source/policy models; no C build or hardware test.

Injected detections do not verify native WakeNet/AEC or physical echo timing.
"""
from itertools import permutations
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
acoustic = (MAIN / "app_acoustic.c").read_text()
header = (MAIN / "app_acoustic.h").read_text()
audio = (MAIN / "app_audio.c").read_text()
cloud = (MAIN / "app_cloud.c").read_text()
main = (MAIN / "main.c").read_text()
ui = (MAIN / "app_ui.c").read_text()
tts = (MAIN / "app_cloud_tts.c").read_text()
sdk = (ROOT / "firmware/sdkconfig").read_text()
config = (MAIN / "Kconfig.projbuild").read_text()

assert "CONFIG_VOX_ACOUSTIC_BARGE_IN=y" in sdk
for name, value in (("VOX_AEC_DELAY_MS", 40),):
    section = config.split("config " + name, 1)[1].split("\nconfig ", 1)[0]
    assert f"default {value}" in section
assert "depends on IDF_TARGET_ESP32S3 && SPIRAM" in config
assert "#define PRE_CAP RATE" in acoustic
assert "#define CAPTURE_CAP (20 * RATE)" in acoustic
assert "AEC_MODE_FD_LOW_COST" in acoustic and "heap_caps_aligned_alloc(16," in acoustic
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in acoustic
assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)" in acoustic
assert "heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 4096" in acoustic
assert "static int16_t reference[" not in acoustic and "pending_pcm[" in acoustic
assert "xTaskCreate" not in acoustic and "i2s_channel_read" not in acoustic
init = acoustic[acoustic.index("void app_acoustic_init"):acoustic.index("static bool create_dsp")]
assert "create_dsp()" not in init and "aec_create" not in init
arm = acoustic[acoustic.index("bool app_acoustic_listen"):acoustic.index("void app_acoustic_discard")]
assert arm.count("app_cloud_is_cancelled()") == 2  # cancellation during lazy creation
assert "if (!atomic_load(&pending)) atomic_fetch_add(&generation, 1)" in arm
assert "create_dsp()" not in arm
assert "atomic_store(&pending, false)" not in arm
discard = acoustic[acoustic.index("void app_acoustic_discard"):acoustic.index("void app_acoustic_playing")]
assert "atomic_store(&pending, false)" in discard and "atomic_fetch_add(&generation, 1)" in discard

feed = acoustic[acoustic.index("void app_acoustic_feed"):acoustic.index("bool app_acoustic_pending")]
assert "(!atomic_load(&listening) || !atomic_load(&playing)) && !atomic_load(&pending)" in feed
assert "if (echoing) aec_process(aec, mic, ref, clean);" in feed
assert "else memcpy(clean, mic, frame * sizeof(*clean));" in feed
assert "app_wake_feed_barge(clean, frame, seen_generation)" in feed
assert "if (!atomic_load(&pending))" in feed
assert "else if (!capture_finished && captured < CAPTURE_CAP)" in feed
assert "pending_silence >= SILENCE_SAMPLES" in feed
assert "captured == CAPTURE_CAP" in feed
accept = acoustic.split("static void accept_requested_wake", 1)[1].split("void app_acoustic_feed", 1)[0]
assert accept.index("unsigned epoch = atomic_exchange(&requested, 0)") < accept.index("pending_pcm[i] = preroll[")
assert accept.index("pending_pcm[i] = preroll[") < accept.index("portENTER_CRITICAL(&lock)")
assert "epoch == atomic_load(&generation)" in accept
assert "app_cloud_barge_in()" in accept and "app_cloud_cancel()" not in accept
assert "atomic_store_explicit(&pending, true, memory_order_release)" in accept
request = acoustic.split("bool app_acoustic_wake_detected", 1)[1].split("static void accept_requested_wake", 1)[0]
assert "atomic_compare_exchange_strong(&requested, &empty, epoch)" in request
assert "epoch == atomic_load(&generation)" in request
assert "atomic_load(&playing)" in request and "!atomic_load(&blocked)" in request
assert "app_cloud_barge_in()" not in request
for source in (acoustic, audio, main, tts, header):
    assert "app_acoustic_pause_requested" not in source
    assert "app_audio_note_voice_pause" not in source
    assert "Speech suspected" not in source
wake = (MAIN / "app_wake.c").read_text()
producer = wake.split("static void feed_pcm", 1)[1].split("void app_wake_feed(", 1)[0]
assert producer.index("!(playback ? atomic_load(&barge) : atomic_load(&idle))") < producer.index("used = 0")
assert "input->acoustic_generation = epoch" in producer and "input->playback = playback" in producer
worker = wake.split("static void worker", 1)[1].split("void app_wake_init", 1)[0]
assert worker.index("atomic_exchange(&reset, false)") < worker.index("if (received != pdTRUE)")
route = worker.split("if (work->playback) {", 1)[1].split("if (wakeup)", 1)[0]
assert "app_acoustic_wake_detected(work->acoustic_generation)" in route
assert "continue;" in route
assert "work->playback ? 0 : app_wake_diagnostic_raw" in worker
assert "work->generation != atomic_load(&generation)" in worker
assert "->clean(" not in wake
assert "echo_tail_until = esp_timer_get_time() + 200000" in acoustic
assert "ref_write - ref_read + count > REF_CAP" in acoustic
assert "CONFIG_VOX_AEC_DELAY_MS * (RATE / 1000)" in acoustic
assert "app_cloud_resume_barge_in" not in acoustic  # only the coordinator resumes

# Changing only the reference rate must not alter TTS/music pitch or duration.
assert "#define SPEAKER_SAMPLE_RATE_HZ 24000" in audio
assert ".clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE_HZ)" in audio
assert "stereo_samples[i * 2] = (int16_t)scaled;" in audio
assert "write_bytes = frame_count * 2 * sizeof(int16_t)" in audio
assert "app_acoustic_reference(reference, output_frames)" in audio
capture = audio[audio.index("esp_err_t app_audio_capture_progress("):audio.index("static void microphone_level_task(")]
assert capture.index("xSemaphoreTake(s_mic_read_mutex") < capture.index("app_acoustic_take(")
assert "&silence_samples" in capture and "!stopped_by_silence" in capture
assert capture.index("bool stopped_by_silence = had_speech") < capture.index("while (result == ESP_OK")
assert "size_t *silence_samples" in header

turn = main[main.index("static esp_err_t run_voice_turn("):main.index("static void voice_conversation_task(void *argument) {")]
assert turn.index("app_cloud_listen(") < turn.index("app_acoustic_prepare()") < turn.index("app_cloud_ask(")
assert "if (app_wake_is_enabled() && !app_acoustic_prepare())" in turn
assert "app_acoustic_listen(true)" not in turn
assert tts.index("playing = true;") < tts.index("app_acoustic_listen(true)") < tts.index("app_acoustic_playing(true)")
assert "bool can_interrupt = !reminder && app_acoustic_listen(true);" in tts
assert "finish:\n  app_acoustic_listen(false);" in turn
assert "app_cloud_commit_turn(transcript, answer)" in turn and "app_cloud_remember_interrupted" in turn
loop = main[main.index("static void voice_conversation_task(void *argument) {"):main.index("void app_main(void)")]
handoff = loop[loop.index("if (app_acoustic_pending()"):loop.index("if (!continuous")]
assert "app_cloud_resume_barge_in()" in handoff and "app_cloud_reset_cancel" not in handoff
assert "automatic = true;" in handoff and "continuous = true;" in handoff and "followup = false;" in handoff
assert "app_acoustic_discard();" in loop
stop = main[main.index("static bool stop_active_audio("):main.index("static bool handle_button_event(")]
assert stop.index("app_cloud_cancel()") < stop.index("app_acoustic_discard()")
assert "int expected = CANCEL_NONE" in cloud and "int expected = CANCEL_BARGE_IN" in cloud
assert "atomic_store(&s_cancelled, CANCEL_USER)" in cloud
assert cloud.count("atomic_compare_exchange_strong(&s_cancelled, &expected,") == 2
mic_ui = ui[ui.index("static void update_mic_ui("):ui.index("static void weather_shape(")]
assert "app_acoustic_pending()" in mic_ui and 'show_voice_state(APP_UI_VOICE_LISTENING, "正在聆听")' in mic_ui
cleanup = tts[tts.index("cleanup:\n  if (output_boosted)"):]
assert cleanup.index("app_audio_abort_playback()") < cleanup.index("while (!atomic_load(&stream->input_done))")
assert "app_acoustic_playing(false)" in cleanup

# Model all acceptance/resume/button orderings: a user press is never cleared
# by a voice handoff; without it, an accepted voice can resume exactly once.
def cancel_order(actions):
    reason, accepted, resumed = 0, False, False
    for action in actions:
        if action == "voice" and reason == 0:
            reason, accepted = 1, True
        elif action == "button":
            reason = 2
        elif action == "resume" and accepted and reason == 1:
            reason, resumed = 0, True
    return reason, resumed

for order in permutations(("voice", "button", "resume")):
    assert cancel_order(order)[0] == 2
assert cancel_order(("voice", "resume", "resume")) == (0, True)
assert cancel_order(("button", "voice", "resume")) == (2, False)

# The 3:2 reference converter must retain phase across arbitrary network blocks.
def reference_pcm(blocks):
    group, reference, speaker = [], [], []
    for block in blocks:
        for sample in block:
            speaker.extend((sample, sample))
            group.append(sample)
            if len(group) == 3:
                reference.extend((group[0], int((group[1] + group[2]) / 2)))
                group.clear()
    return reference, speaker, group

raw = [((n * 997) % 65536) - 32768 for n in range(24001)]
expected = reference_pcm([raw])
for size in (1, 2, 3, 256, 512):
    actual = reference_pcm([raw[i:i + size] for i in range(0, len(raw), size)])
    assert actual == expected
assert len(expected[0]) == 16000 and len(expected[1]) == 2 * len(raw)

# Circular pre-roll is chronological, including speech before confirmation.
ring = [0] * 16000
pos = count = 0
for sample in range(20000):
    ring[pos] = sample
    pos = (pos + 1) % len(ring)
    count = min(count + 1, len(ring))
saved = [ring[(pos + len(ring) - count + k) % len(ring)] for k in range(count)]
assert saved == list(range(4000, 20000))

# Only a Computer event for the current, armed playback can request handoff.
# Noise/VAD results are deliberately absent from this acceptance predicate.
def request(epoch, current=7, playing=True, enabled=True, blocked=False, pending=False, cancelled=False):
    return epoch == current and playing and enabled and not (blocked or pending or cancelled)

assert request(7)
for epoch in (0, 6, 8):
    assert not request(epoch)
for flag in ("blocked", "pending", "cancelled"):
    assert not request(7, **{flag: True})
assert not request(7, playing=False)
assert not request(7, enabled=False)

# A disabled raw feed does not erase a partly assembled playback frame.
used, queued = [], []
for part in ([1, 2, 3], [], [4, 5]):
    if part:
        used += part
        while len(used) >= 4:
            queued.append(used[:4])
            used = used[4:]
    # Raw producer, idle=False: return without touching used.
assert queued == [[1, 2, 3, 4]] and used == [5]

control = (MAIN / "app_control.c").read_text()
assert 'strncmp(p, APP_WAKE_PHRASE, wake_len)' in control
assert "TURN_CANCEL, NULL, ESP_OK" in turn
def strip_wake(text):
    return re.sub(r"^[ \t]*小冰小冰[\s,。.!?，！？:：;]*", "", text)
for text, expected in (("小冰小冰，明天天气呢？", "明天天气呢？"),
                       ("小冰小冰现在几点", "现在几点"),
                       ("小冰小冰。", ""), ("小冰", "小冰"),
                       ("解释小冰小冰是什么", "解释小冰小冰是什么"),
                       ("Computer，现在几点", "Computer，现在几点")):
    assert strip_wake(text) == expected

def pending_capture(states, prefix=16000):
    captured, quiet, finished = prefix, 0, False
    for speech in states:
        if not finished:
            captured = min(captured + 320, 320000)
            quiet = 0 if speech else quiet + 320
            finished = quiet >= 19200 or captured >= 320000
    return captured, quiet, finished

completed = pending_capture([True] * 20 + [False] * 60)
assert completed == pending_capture([True] * 20 + [False] * 60 + [True] * 500)
assert completed[1:] == (19200, True)  # immediately submit after slow old cleanup
assert pending_capture([True] * 2000)[0::2] == (320000, True)

token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for code in (acoustic, header, audio, cloud, main, ui):
    stack = []
    for char in token.sub("", code):
        if char in "({[":
            stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char]
    assert not stack
print("PASS: interruption ownership/cancel/pre-roll/reference/endpoint source models; C/native AEC/device NOT run")
