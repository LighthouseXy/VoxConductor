"""Wake handoff source/logic models; no firmware compile or real recognition.

Injected detections cannot establish WakeNet accuracy or real UI lock latency.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
main = (MAIN / "main.c").read_text()
wake = (MAIN / "app_wake.c").read_text()
ui = (MAIN / "app_ui.c").read_text()
sdk = (ROOT / "firmware/sdkconfig").read_text()
callback = main.split("static void handle_wake(void) {", 1)[1].split("static bool dispatch_wake", 1)[0]
dispatch = main.split("static bool dispatch_wake(void) {", 1)[1].split("static void wake_dispatch_task", 1)[0]
dispatch_worker = main.split("static void wake_dispatch_task(void *argument) {", 1)[1].split("static void reminder_task", 1)[0]
worker = wake.split("static void worker(", 1)[1].split("void app_wake_init", 1)[0]
boot = main.split("void app_main(void) {", 1)[1]
ttl = int(re.search(r"#define WAKE_HANDOFF_MS (\d+)", main)[1])
debounce_us = int(re.search(r"#define WAKE_DEBOUNCE_US (\d+)", wake)[1])
assert ttl == 2000 and debounce_us == 800000

# The native-model worker callback just deposits a bounded/coalesced request.
assert "atomic_compare_exchange_strong(&s_pending_wake_until, &empty, until)" in callback
assert not re.search(r"\b(?:lvgl_port_lock|start_voice|app_media_suspend|vTaskDelay|xTaskCreate)\s*\(", callback)
assert "Wake ignored: UI lock timeout" not in main
assert "(int32_t)(now - until) >= 0" in dispatch
assert "atomic_load(&s_voice_turn_active) || !lvgl_port_lock(20)" in dispatch
assert dispatch.index("lvgl_port_unlock();", dispatch.index("bool allowed =")) < dispatch.index("start_voice(true)")
assert "atomic_load(&s_pending_wake_until) != until" in dispatch  # changes during lock acquisition
assert "atomic_load(&s_pending_wake_until) == until" in dispatch  # button cancellation remains authoritative
assert "!app_network_is_connected()" in dispatch and "!app_wake_is_enabled()" in dispatch
assert "s_audio_active" in dispatch and "s_media_audio_active" in dispatch
logical_switch = wake.split("bool app_wake_is_enabled(void) {", 1)[1].split("}", 1)[0]
assert "return atomic_load(&enabled)" in logical_switch and "ready" not in logical_switch
assert "if (!app_wake_available())" in dispatch
assert dispatch.index("if (!app_wake_available())") < dispatch.index("lvgl_port_lock(20)")
feed = wake.split("static void feed_pcm(", 1)[1].split("void app_wake_feed(", 1)[0]
assert "!app_wake_available() || !app_wake_is_enabled()" in feed
assert "!app_wake_available() || !app_wake_is_enabled()" in worker
assert "dispatch_wake()" in dispatch_worker
assert "app_desktop_" not in dispatch_worker and "app_audio_" not in dispatch_worker
assert main.count('xTaskCreate(wake_dispatch_task, "wake_dispatch"') == 1
assert 'xTaskCreate(wake_dispatch_task, "wake_dispatch", 4096' in main
assert boot.index('xTaskCreate(wake_dispatch_task, "wake_dispatch"') < boot.index("app_wake_init(handle_wake)")
assert "if (wake_dispatch_ready)" in boot
button = main.split("if (event == APP_BUTTON_EVENT_PRESS_DOWN)", 1)[1].split("APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE", 1)[0]
assert button.index("atomic_exchange(&s_pending_wake_until, 0)") < button.index("stop_active_audio()")

# Cooldown must suppress only duplicate positive notifications, never skip
# three seconds of input before the classifier or make volume changes.
assert "3000000" not in worker
detect = worker.index("wn->detect(model, work->pcm)")
cooldown_guard = worker.index("if (now < cooldown)")
assert detect < cooldown_guard
assert "esp_timer_get_time() < cooldown" not in worker[:detect]
assert "cooldown = now + WAKE_DEBOUNCE_US" in worker
assert "++hits" in worker and "++suppressed" in worker
assert "hit=%u debounce=%u" in worker
assert "CONFIG_VOX_WAKE_THRESHOLD_PERCENT=60" in sdk.splitlines()
assert "CONFIG_VOX_WAKE_GAIN=1" in sdk.splitlines()
assert "->clean(" not in wake  # preserve safe vendor destroy/create reset
ui_wake = ui.split("bool app_ui_wake_voice(void) {", 1)[1].split("bool app_ui_take_retry", 1)[0]
assert "s_active_voice_state != APP_UI_VOICE_ERROR" in ui_wake
assert "s_active_voice_state != APP_UI_VOICE_CANCELLED" in ui_wake
assert "hide_overlay_locked();" in ui_wake and "s_retry_requested = false" in ui_wake

MASK = 2**32 - 1
def expired(now, deadline):
    delta = (now - deadline) & MASK
    return delta < 2**31

class Handoff:
    def __init__(self):
        self.until = 0
        self.starts = 0
        self.button_cancelled = False
        self.enabled = self.ready = True
    def detected(self, now, eligible=True):
        if eligible and self.enabled and self.ready and not self.until:
            self.until = ((now + ttl) & MASK) or 1
    def button(self):
        self.until = 0
        self.button_cancelled = True
    def dispatch(self, now, ui_lock=True, coordinator_busy=False,
                 valid=True, owner=False, overlay="idle", start_ok=True,
                 during_lock=None):
        until = self.until
        if not until:
            return "empty"
        if expired(now, until):
            self.until = 0
            return "expired"
        if not self.enabled or not valid or owner:
            self.until = 0
            return "discarded"
        if not self.ready:
            return "retained"
        if coordinator_busy or not ui_lock:
            return "retained"
        if during_lock:
            during_lock()
        if self.until != until:
            return "cancelled"
        if not self.enabled or not self.ready:
            return "retained"
        if overlay not in ("idle", "error", "cancelled"):
            self.until = 0
            return "blocked"
        if not start_ok:
            return "retained"
        self.until = 0
        self.starts += 1
        return "started"

# The screenshot's first detection survives a >100 ms lock hold; only one
# conversation starts. Duplicates do not extend the two-second deadline.
h = Handoff()
h.detected(25109)
initial = h.until
for now in range(25109, 25809, 25):
    assert h.dispatch(now, ui_lock=False) == "retained"
h.detected(25709)
assert h.until == initial
assert h.dispatch(25809) == "started"
assert h.starts == 1 and h.dispatch(25834) == "empty"
h = Handoff()
h.detected(100)
assert h.dispatch(110, coordinator_busy=True) == "retained"
assert h.dispatch(350, start_ok=False) == "retained"
assert h.dispatch(450) == "started"

# Screenshot regression: the detector's own rebuild takes ready low before
# the UI coordinator runs. This must not be interpreted as switch-off.
for detected_at, dispatched_at in ((48409, 48449), (52489, 52569)):
    h = Handoff()
    h.detected(detected_at)
    deadline = h.until
    h.ready = False
    assert h.enabled and not (h.enabled and h.ready)  # old is_enabled bug
    assert h.dispatch(dispatched_at) == "retained" and h.until == deadline
    h.ready = True
    assert h.dispatch(dispatched_at + 100) == "started" and h.starts == 1
h = Handoff()
h.detected(100)
h.ready = False
h.enabled = False  # actual disable or failed model rebuild
assert h.dispatch(150) == "discarded" and h.starts == 0
h = Handoff()
h.detected(100)
h.ready = False
assert h.dispatch(2100) == "expired" and h.starts == 0
h = Handoff()
h.detected(100)
assert h.dispatch(150, during_lock=lambda: setattr(h, "ready", False)) == "retained"
h.ready = True
assert h.dispatch(200) == "started" and h.starts == 1

for invalid in (dict(valid=False), dict(owner=True)):
    h = Handoff()
    h.detected(100)
    assert h.dispatch(120, **invalid) == "discarded" and h.starts == 0
h = Handoff()
h.detected(100)
assert h.dispatch(120, during_lock=h.button) == "cancelled" and h.starts == 0
h = Handoff()
h.detected(100)
h.detected(2099)
assert h.dispatch(2100) == "expired" and h.starts == 0
for overlay in ("idle", "error", "cancelled", "listening", "thinking", "speaking", "reminder"):
    h = Handoff()
    h.detected(100)
    expected = "started" if overlay in ("idle", "error", "cancelled") else "blocked"
    assert h.dispatch(150, overlay=overlay) == expected

# Uptime rollover must not turn a recent wake into an expired event, and a
# zero-valued deadline gets a one-millisecond sentinel adjustment, not lost.
for now in (MASK - 1000, MASK - ttl + 1, MASK - 10, 0, 100):
    h = Handoff()
    h.detected(now)
    assert h.until != 0
    assert h.dispatch((now + 500) & MASK, ui_lock=False) == "retained"
    assert h.dispatch((now + ttl + 1) & MASK) == "expired"

# Inject positive classifier outputs, not audio: all eligible frames are
# inferred during duplicate debounce; a repeat after 800 ms can notify.
processed = notified = suppressed = 0
cooldown = 0
for now in range(0, 1601, 32):
    processed += 1
    positive = now in (0, 32, 320, 800, 832, 1600)
    if positive:
        if now < cooldown:
            suppressed += 1
        else:
            notified += 1
            cooldown = now + debounce_us // 1000
assert processed == 51 and notified == 3 and suppressed == 3

print("PASS: queued wake/UI retry/owner/button/expiry/wrap/debounce models; native accuracy/device NOT run")
