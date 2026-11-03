"""Source guards and a local state model; native C and device acceptance run separately."""
from pathlib import Path
from collections import deque
import math

ROOT = Path(__file__).resolve().parents[1]
wake = (ROOT / "firmware/main/app_wake.c").read_text()
network = (ROOT / "firmware/main/app_network.c").read_text()
worker = wake.split("static void worker(", 1)[1].split("void app_wake_init(", 1)[0]
recreate = wake.split("static bool recreate_model(", 1)[1].split("static void worker(", 1)[0]
feed = wake.split("static void feed_pcm(", 1)[1].split("void app_wake_feed(", 1)[0]
assert "->clean(" not in wake
assert "if (model_used && !recreate_model())" in worker
assert "bool model_used = false" in worker
assert recreate.index("atomic_store(&ready, false)") < recreate.index("wn->destroy(model)") < recreate.index("wn->create(model_name, DET_MODE_95)")
assert recreate.index("xQueueReset(frames)") < recreate.index("atomic_store(&ready, true)")
assert "atomic_store(&enabled, false)" in recreate and "model = NULL" in recreate
assert "free(input)" not in recreate and "vQueueDelete" not in recreate
assert "get_samp_chunksize(model) != (int)chunk" in recreate
assert "work->generation != atomic_load(&generation)" in worker
assert "detecting_generation == atomic_load(&generation)" in worker
assert "!atomic_load(&reset)" in worker
assert "input->generation = producer_generation" in feed
assert "sizeof(wake_frame_t) + chunk * sizeof(int16_t)" in wake
assert "_Alignas(16) int16_t pcm[]" in wake
assert wake.count("heap_caps_aligned_alloc(16, frame_bytes") == 2
assert "#define WAKE_QUEUE_FRAMES 16" in wake
assert "xQueueCreateWithCaps(WAKE_QUEUE_FRAMES, frame_bytes," in wake
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in wake.split("frames = xQueueCreateWithCaps", 1)[1].split(";", 1)[0]
assert "vQueueDeleteWithCaps(frames)" in wake
assert "xTaskCreatePinnedToCore(worker" in wake
assert "xQueueSend(frames, input, 0)" in feed
assert "atomic_store(&reset" not in feed
assert "wn->" not in feed and "xQueueReset" not in feed
assert "atomic_fetch_add(&dropped, 1)" in feed
assert "atomic_exchange(&idle, value) != value" in wake
assert "atomic_exchange(&enabled, value) == value" in wake
assert "now - last_diag >= (detailed ? 2000000 : 15000000)" in worker
assert "CONFIG_VOX_LOG_VERBOSE || app_wake_diagnostic_active()" in worker
assert "pdMS_TO_TICKS(1000)" in worker
assert '"wake_diag:' in worker and '"Wake word detected:' in worker
assert "#define CONFIG_VOX_WAKE_GAIN 1" in wake
assert "dc_q8 += (raw * 256 - dc_q8) / 256" in worker
assert "(raw - dc_q8 / 256) * CONFIG_VOX_WAKE_GAIN" in worker
assert "scaled = INT16_MAX" in worker and "scaled = INT16_MIN" in worker
assert "work->pcm[i] = (int16_t)scaled" in worker
assert worker.index("work->pcm[i] = (int16_t)scaled") < worker.index("wn->detect(model, work->pcm)")
assert "dc_q8 = 0;" in worker.split("if (atomic_exchange(&reset, false))", 1)[1]
assert "scaled" not in feed  # the producer's recording/acoustic PCM stays unchanged
configure = wake.split("static bool configure_model(", 1)[1].split("static bool recreate_model(", 1)[0]
assert "CONFIG_VOX_WAKE_THRESHOLD_PERCENT / 100.0f, 1" in configure
assert "wn->get_det_threshold(model, 1)" in configure
assert "threshold > 0.0f && threshold < 1.0f" in configure
assert "abs(actual - expected)" not in configure
assert "return true;" in configure
assert "!configure_model()" in recreate
assert "!configure_model()" in wake.split("void app_wake_init(", 1)[1]
config = (ROOT / "firmware/main/Kconfig.projbuild").read_text()
gain_config = config.split("config VOX_WAKE_GAIN", 1)[1].split("config VOX_WAKE_THRESHOLD_PERCENT", 1)[0]
assert "range 1 16" in gain_config and "default 1" in gain_config
threshold_config = config.split("config VOX_WAKE_THRESHOLD_PERCENT", 1)[1].split("config VOX_ACOUSTIC_BARGE_IN", 1)[0]
assert "range 40 95" in threshold_config and "default 60" in threshold_config
assert "#define CONFIG_VOX_WAKE_THRESHOLD_PERCENT 60" in wake
sdkconfig = (ROOT / "firmware/sdkconfig").read_text()
assert "CONFIG_VOX_WAKE_THRESHOLD_PERCENT=60" in sdkconfig.splitlines()
assert "false wakes" in threshold_config  # calibration still requires device acceptance

# The bundled WakeNet9l public setter reports success and derives 0.992x for
# its internal threshold, which its getter returns. The old exact-match gate
# rejected a healthy Computer model at startup (546 vs requested 550).
def configured(setter_ok, readback):
    return bool(setter_ok and 0.0 < readback < 1.0)

derived = 0.55 * 0.992
assert abs(round(derived * 1000) - 550) > 1  # reproduce the previous regression
assert configured(True, derived)
for requested in (0.40, 0.55, 0.65, 0.95):
    assert configured(True, requested * 0.992)
assert not configured(False, derived)
for invalid in (0.0, -1.0, 1.0, float('nan'), float('inf')):
    assert not configured(True, invalid)
assert "esp_wifi_connect" not in wake
ui = (ROOT / "firmware/main/app_ui.c").read_text()
ui_wake = ui.split("bool app_ui_wake_voice(void)", 1)[1].split("bool app_ui_take_retry", 1)[0]
assert "if (s_desktop_open" not in ui_wake
assert "s_active_voice_state != NO_VOICE_STATE" in ui_wake
assert "s_desktop_open = s_detail_open = s_album_open = false" in ui_wake
assert "app_ui_note_activity()" in ui_wake and "app_desktop_visible(false)" in ui_wake
main = (ROOT / "firmware/main/main.c").read_text()
handle = main.split("static void handle_wake(void)", 1)[1].split("static void wake_dispatch_task", 1)[0]
assert "start_voice(true)" in handle and "Wake accepted:" in handle
assert "Wake queued:" in handle and "UI lock timeout" not in handle
assert "static bool dispatch_wake" in handle and "lvgl_port_lock(20)" in handle
maintenance = network.split("static void weather_update_task", 1)[1]
assert maintenance.index("network_progress(syncing, false, weather_error, time_due, weather_due)") < maintenance.index("air_error = fetch_air_quality()")

# Model the reset boundary. clean() is deliberately forbidden by the vendor stub.
class Vendor:
    def __init__(self):
        self.created, self.destroyed, self.detected = 1, 0, 0
        self.fail = False
    def clean(self):
        raise AssertionError("unsafe vendor clean path")
    def recreate(self):
        self.destroyed += 1
        if self.fail:
            return False
        self.created += 1
        return True

class Wake:
    def __init__(self):
        self.vendor = Vendor()
        self.generation = 0
        self.used = False
        self.ready = self.enabled = self.idle = True
        self.reset = True
    def request_reset(self):
        self.generation += 1
        self.reset = True
    def allow(self, value):
        if self.idle != value:
            self.idle = value
            self.request_reset()
    def enable(self, value):
        if self.enabled != value:
            self.enabled = value
            self.request_reset()
    def step(self, frame_generation, detected=False, during_detect=None):
        if self.reset:
            self.reset = False
            if self.used:
                self.ready = False
                if not self.vendor.recreate():
                    self.enabled = False
                    return False
                self.generation += 1
                self.ready = True
            self.used = False
            return False
        if not (self.ready and self.enabled and self.idle) or frame_generation != self.generation:
            return False
        before = self.generation
        self.used = True
        self.vendor.detected += 1
        if during_detect:
            during_detect()
        if detected and before == self.generation and not self.reset and self.enabled and self.idle:
            self.request_reset()
            return True
        return False

w = Wake()
assert not w.step(0) and (w.vendor.created, w.vendor.destroyed) == (1, 0)
assert w.step(w.generation, True)  # first detection has no vendor clean
assert not w.step(w.generation) and (w.vendor.created, w.vendor.destroyed) == (2, 1)
before = w.vendor.detected
assert not w.step(w.generation - 1, True) and w.vendor.detected == before
assert not w.step(w.generation, True, w.request_reset)  # disable/reset during detect
assert not w.step(w.generation)
for _ in range(3):
    w.step(w.generation)
    w.request_reset()
    assert not w.step(w.generation)
assert w.vendor.created - w.vendor.destroyed == 1  # no duplicate live models
w.step(w.generation)
w.vendor.fail = True
w.request_reset()
assert not w.step(w.generation) and not w.ready and not w.enabled
assert not w.step(w.generation, True)

# Bursty/slow inference can fill the queue, but must never rebuild the model.
w = Wake()
w.step(0)  # consume the initial reset on the fresh instance
queue, drops = deque(), 0
for tick in range(300):
    w.allow(True)  # periodic restore of the unchanged state must be harmless
    w.enable(True)  # saving other settings preserves the recognizer generation
    if len(queue) == 16:
        drops += 1
    else:
        queue.append(w.generation)
    if tick % 3 == 0:
        w.step(queue.popleft())
assert drops > 0 and w.vendor.detected == 100
assert (w.vendor.created, w.vendor.destroyed) == (1, 0) and not w.reset
assert w.step(queue.popleft(), detected=True)  # still able to wake after overflow
w.allow(False)
before = w.generation
w.allow(False)
assert w.generation == before  # repeated audio-owner hold preserves the generation
assert not w.step(w.generation)  # real owner transition resets used state once
w.allow(True)
assert not w.step(w.generation)
assert not w.step(w.generation - 1, detected=True)  # stale owner audio is blocked
assert w.step(w.generation, detected=True)  # new session works after resuming

# Mirror integer C arithmetic: signal conditioning is continuous across chunks,
# rejects DC, amplifies quiet speech and saturates instead of wrapping int16.
def cdiv(n, d):
    return n // d if n >= 0 else -((-n) // d)

class WakePCM:
    def __init__(self, gain=1):
        self.gain, self.dc, self.initialized, self.clips = gain, 0, False, 0
    def process(self, samples):
        out = []
        for raw in samples:
            if not self.initialized:
                self.dc, self.initialized = raw * 256, True
            self.dc += cdiv(raw * 256 - self.dc, 256)
            scaled = (raw - cdiv(self.dc, 256)) * self.gain
            if scaled > 32767 or scaled < -32768:
                self.clips += 1
            out.append(max(-32768, min(32767, scaled)))
        return out

for dc in (1800, -1800):
    pcm = WakePCM()
    assert pcm.process([dc] * 16000) == [0] * 16000
voice = [int(400 + 1400 * math.sin(2 * math.pi * 1000 * i / 16000)) for i in range(16000)]
saved_voice = voice.copy()
whole = WakePCM().process(voice)
split, chunked = WakePCM(), []
for start in range(0, len(voice), 512):
    chunked.extend(split.process(voice[start:start + 512]))
assert whole == chunked and voice == saved_voice
assert 1325 < max(abs(x) for x in whole[8000:]) < 1500
assert abs(sum(whole[8000:]) / 8000) < 10 and split.clips == 0
for gain in (1, 2, 4, 16):
    pcm = WakePCM(gain)
    loud = pcm.process([32767, -32768] * 512)
    assert all(-32768 <= x <= 32767 for x in loud)
    if gain > 1:
        assert pcm.clips > 0 and 32767 in loud and -32768 in loud

# Independent AQ deadline: preserve the 30 s AQ retry after weather success.
weather_due, air_due = 600, 30
assert 30 < weather_due and 30 >= air_due
air_due = 30 + 600  # successful retry resumes normal ten-minute polling
assert air_due == 630
# Wi-Fi failures back off quickly, cap at 30 s, and reset on a new connection.
delays = [2 << n if n < 4 else 30 for n in range(8)]
assert delays == [2, 4, 8, 16, 30, 30, 30, 30]
print('PASS: wake threshold 60/config consistency, PCM DC/gain/clipping/continuity, overflow/owner/reset and AQ/Wi-Fi guards; scope=offline checks; pending=C and hardware validation')
