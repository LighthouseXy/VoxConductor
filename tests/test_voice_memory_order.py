"""Heap/order/handshake policy checks; no C build, network request or device I/O.

These models do not execute FreeRTOS, TLS, the native VAD, or a microphone.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
tts = (MAIN / "app_cloud_tts.c").read_text()
cloud = (MAIN / "app_cloud.c").read_text()
audio = (MAIN / "app_audio.c").read_text()
asr = (MAIN / "app_cloud_asr.c").read_text()
sender = tts.split("static void tts_input_task(void *argument) {", 1)[1].split("static void report_first_pcm", 1)[0]
play = tts.split("static esp_err_t speak(", 1)[1].split("esp_err_t app_cloud_speak(", 1)[0]
assert tts.count('xTaskCreate(tts_input_task, "tts_input", 8192') == 1
assert play.index('xTaskCreate(tts_input_task, "tts_input", 8192') < play.index("esp_websocket_client_init(&config)") < play.index("esp_websocket_client_start(client)")
assert play.index("wait_event(stream, UPDATED_BIT)") < play.index("atomic_store(&stream->input_ready, true)")
assert sender.index("while (!atomic_load(&stream->input_ready)") < sender.index('"input_text_buffer.append"')
assert "!stopped(stream)" in sender and "atomic_load(&stream->error) == ESP_OK" in sender
cleanup = play.split("cleanup:", 1)[1]
assert cleanup.index("atomic_store(&stream->error, result)") < cleanup.index("while (!atomic_load(&stream->input_done))")
assert cleanup.index("while (!atomic_load(&stream->input_done))") < cleanup.index("esp_websocket_client_destroy(client)") < cleanup.index("free(stream->pcm_storage)")
assert "TTS failed: stage=%s" in cleanup and 'stage = "input_stack"' in play
assert ".task_stack = 8192" in play  # retain existing safe internal stack sizes
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in play  # PCM/JSON already external

read = cloud.split("static esp_err_t read_stream(", 1)[1].split("esp_err_t app_cloud_ask(", 1)[0]
assert "app_speech" not in cloud and "queue_ready" not in cloud
assert "esp_http_client_cleanup" not in read  # owner is the bounded tool loop
ask = cloud.split("esp_err_t app_cloud_ask(", 1)[1].split("esp_err_t app_cloud_commit_turn", 1)[0]
assert "app_cloud_http_reusable(client)" in ask
assert ask.rindex("esp_http_client_cleanup(client)") < ask.index("LLM连接已释放/TTS准备") < ask.rindex("return result;")
assert 's->tool_count' in read and '"stop"' in read and "app_cloud_is_cancelled()" in read
main = (MAIN / "main.c").read_text()
turn = main.split("static esp_err_t run_voice_turn(", 1)[1].split("static void voice_conversation_task(void *argument) {", 1)[0]
assert turn.index("app_cloud_ask(") < turn.index("TURN_ASSISTANT_TEXT") < turn.index("app_cloud_speak(")
assert '"app_speech.c"' not in (MAIN / "CMakeLists.txt").read_text()
assert asr.index('xTaskCreate(capture_task, "asr_capture"') < asr.index("esp_websocket_client_start(client)")  # ASR network setup does not delay recording
assert "if (speech_run_ms >= AUTO_START_CONFIRM_MS) had_speech = true" in audio
assert "if (waiting && !speech_run_ms && waiting_samples >=" in audio

# The screenshot has more free bytes in total than the requested 8 KiB,
# but no single block large enough. More PSRAM cannot satisfy this stack.
blocks = [7680, 6000, 2716]
assert sum(blocks) == 16396 and sum(blocks) > 8192 and max(blocks) < 8192

class Pipeline:
    def __init__(self):
        self.llm_open = True
        self.tts_started = False
        self.events = []
    def chunk(self, tools=False):
        assert self.llm_open and not self.tts_started
    def complete(self, tools=False, valid=True, cancelled=False):
        self.llm_open = False
        self.events.append("llm_tls_release")
        if valid and not tools and not cancelled and not self.tts_started:
            assert not self.llm_open
            self.tts_started = True
            self.events.append("buffered_tts_start")

for chunks in (1, 10, 1000):
    p = Pipeline()
    for _ in range(chunks):
        p.chunk()
    p.complete()
    assert p.events == ["llm_tls_release", "buffered_tts_start"]
for outcome in (dict(tools=True), dict(valid=False), dict(cancelled=True)):
    p = Pipeline()
    p.complete(**outcome)
    assert not p.tts_started and p.events == ["llm_tls_release"]

class SenderGate:
    def __init__(self):
        self.ready = self.error = self.cancelled = self.done = False
        self.sends = 0
    def step(self):
        if self.error or self.cancelled:
            self.done = True
        elif self.ready:
            self.sends += 1
    def free(self):
        assert self.done  # no use-after-free when a reserved sender was never released

g = SenderGate()
for _ in range(20):
    g.step()
assert not g.sends  # never send before session.updated
g.ready = True
g.step()
assert g.sends == 1
for error, cancelled in ((True, False), (False, True)):
    g = SenderGate()
    g.error, g.cancelled = error, cancelled
    g.step()
    g.free()
    assert not g.sends

print("PASS: TTS internal-stack reservation/gated sender/serial TLS ordering and pressure policies; C/runtime/VAD NOT run")
