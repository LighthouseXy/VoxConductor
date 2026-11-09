"""Follow-up/alarm source guards and boundary models; no firmware build.

Injected speech decisions do not validate the native VAD or physical sensor.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
audio = (MAIN / "app_audio.c").read_text()
header = (MAIN / "app_audio.h").read_text()
asr = (MAIN / "app_cloud_asr.c").read_text()
main = (MAIN / "main.c").read_text()
desktop = (MAIN / "app_desktop.c").read_text()
ui = (MAIN / "app_ui.c").read_text()

assert "#define APP_AUDIO_FOLLOWUP_WAIT_SECONDS 12" in header
assert "FOLLOWUP_PREROLL_SAMPLES (MIC_SAMPLE_RATE_HZ * 320 / 1000)" in audio
assert "bool waiting = wait_for_speech && !had_speech;" in audio
assert audio.index("memmove(recording, recording + recorded_samples - keep,") < audio.index("if (progress && !waiting)")
assert "if (!wait_for_speech && !had_speech && recorded_samples >=" in audio
assert "waiting_samples >=" in audio and "MIC_SAMPLE_RATE_HZ * APP_AUDIO_FOLLOWUP_WAIT_SECONDS" in audio
assert "Follow-up window closed: no speech, audio not published" in audio

gate = asr[asr.index("while (followup &&"):asr.index("char header[280]")]
assert "memory_order_acquire" in gate and "app_cloud_is_cancelled()" in gate
assert "result = ESP_ERR_NOT_FOUND" in gate
assert gate.index("CAPTURE_DONE") < gate.index("if (atomic_load_explicit(&s->available")
assert asr.index("while (followup &&") < asr.index("esp_websocket_client_init(&config)")
assert "client = result == ESP_OK ?" in asr
assert "s, s->followup);" in asr
assert "if (!followup) app_turn_emit(turn, TURN_RECORD_START" in asr
assert "Follow-up idle expiry: no cloud request sent" in asr

turn = main[main.index("static esp_err_t run_voice_turn("):main.index("static void voice_conversation_task(void *argument) {")]
idle = turn[turn.index("if ((followup || interrupted) && result == ESP_ERR_NOT_FOUND"):turn.index("if (result == ESP_OK)")]
assert "app_ui_hide_voice_state();" in idle and "return result;" in idle
assert "TURN_ERROR" not in idle and "app_cloud_remember_interrupted" not in idle
loop = main[main.index("static void voice_conversation_task(void *argument) {"):main.index("void app_main(void)")]
assert "bool continuous = automatic;" in loop
assert "if (!continuous || result != ESP_OK || app_cloud_is_cancelled()) break;" in loop
handoff = loop[loop.index("if (!continuous"):loop.index("} while (true);")]
assert handoff.index("app_audio_abort_playback()") < handoff.index("app_audio_prepare_auto_recording();")
assert "app_cloud_reset_cancel()" not in handoff  # button cancellation survives handoff
assert "followup = true;" in handoff
assert loop.index("} while (true);") < loop.index("restore_wake();")
assert "app_cloud_commit_turn(transcript, answer)" in turn

# Inject 20ms VAD decisions. No idle samples are published; speaking capacity
# starts anew after pre-roll compaction, even near the end of the 12s window.
def followup(states, cancel_at=None):
    waiting = True
    stored = waited = quiet = published = run = 0
    for index, speech in enumerate(states, 1):
        stored += 320
        if index == cancel_at:
            return "cancel", published, stored
        quiet = 0 if speech else quiet + 320
        run = run + 20 if speech else 0
        if waiting:
            waited += 320
            if run >= 200:
                stored = min(stored, 5120)
                waiting = False
        if not waiting:
            assert stored >= published
            published = stored
            if quiet >= 19200:
                return "silence", published, stored
        if waiting and not run and waited >= 192000:
            return "idle", published, stored
        if stored >= 320000:
            return "limit", published, stored
    return "waiting" if waiting else "speaking", published, stored

assert followup([False] * 600)[:2] == ("idle", 0)
assert followup([False] * 599 + [True] * 4 + [False] * 60)[:2] == ("idle", 0)
assert followup([False] * 599 + [True] * 50 + [False] * 60)[0] == "silence"
assert followup([False] * 590 + [True] * 1000) == ("limit", 320000, 320000)
assert followup([False] * 600, cancel_at=5)[:2] == ("cancel", 0)
raw = list(range(10000))
preroll = raw[-5120:]
assert preroll + [10000, 10001] == list(range(4880, 10002))

# Clock entry goes directly to Alarm; task management remains in Tools.
assert "MENU_CLOCK" not in desktop and "app_desktop_clock_text" not in desktop + ui
entry = desktop[desktop.index("void app_desktop_open_clock("):desktop.index("void app_desktop_visible(")]
assert "enter(MENU_ALARM);" in entry and "s_alarm_field = -1;" in entry
back = desktop[desktop.index("int app_desktop_back("):desktop.index("bool app_desktop_motion(")]
alarm_back = back.split("case MENU_ALARM:", 1)[1].split("case MENU_ROOT:", 1)[0]
assert "if (s_alarm_editing)" in alarm_back and "return 1;" in alarm_back
assert "return 3;" in alarm_back and "s_alarm_hour = s_edit_value" in alarm_back
assert "index == 2 ? MENU_TASKS" in desktop
assert ui.count("app_desktop_open_clock();") >= 2
service = desktop[desktop.index("void app_desktop_service("):desktop.index("unsigned app_desktop_home_style(")]
assert "app_agent_create_alarm(" in service and "enter(MENU_" not in service
tick = desktop[desktop.index("static void alarm_scroll_tick("):desktop.index("static void tick(")]
for guard in ("!valid", "app_button_click_pending()", "s_task_pending_id[0]", "s_alarm_field != s_selection"):
    assert guard in tick
assert "s_alarm_fraction = 0;" in tick and "s_alarm_scroll_armed = false;" in tick
assert "elapsed_ms > 100" in tick and "s_alarm_fraction %= 1000;" in tick
assert "s_selection == 2" in tick and "app_agent_create_alarm" not in tick

def constant(name):
    return int(re.search(rf"#define {name} (\d+)", desktop)[1])

deadzone, full = constant("ALARM_DEADZONE_MG"), constant("ALARM_FULL_TILT_MG")
def speed(tilt, maximum):
    magnitude = abs(tilt)
    if magnitude <= deadzone:
        return 0
    magnitude = min(magnitude, full)
    rate = 1 + (magnitude - deadzone) * (maximum - 1) // (full - deadzone)
    return -rate if tilt < 0 else rate

def adjust(start, tilt, maximum, modulus, frames):
    fraction = 0
    for elapsed in frames:
        fraction += speed(tilt, maximum) * min(elapsed, 100)
        steps = int(fraction / 1000)  # C integer division truncates toward zero
        fraction -= steps * 1000
        start = (start + modulus + steps) % modulus
    return start

for maximum, modulus in ((constant("ALARM_HOUR_MAX_SPEED"), 24), (constant("ALARM_MINUTE_MAX_SPEED"), 60)):
    rates = [speed(tilt, maximum) for tilt in range(1001)]
    assert rates == sorted(rates) and rates[-1] == maximum
    assert all(speed(-tilt, maximum) == -speed(tilt, maximum) for tilt in range(1001))
    assert adjust(0, 120, maximum, modulus, [33] * 100) == 0
    assert adjust(0, 121, maximum, modulus, [100] * 10) == 1
    assert adjust(0, -121, maximum, modulus, [100] * 10) == modulus - 1
    assert adjust(0, 600, maximum, modulus, [100] * 10) == maximum
    assert adjust(0, 600, maximum, modulus, [5000]) <= 2  # UI stall cannot fast-forward
print("PASS: follow-up quiet/late speech/cancel/pre-roll and alarm speed/wrap source models; C/device NOT run")
