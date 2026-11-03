"""Source integration guards, not firmware compilation or hardware validation."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
main = (MAIN / "main.c").read_text()
audio = (MAIN / "app_audio.c").read_text()
cmake = (MAIN / "CMakeLists.txt").read_text()

def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start) + len(start))]

voice = section(main, "static bool start_voice(", "static void handle_wake(")
assert voice.index("app_media_suspend(1000)") < voice.index("app_audio_prepare_recording()")
assert voice.count("app_media_resume()") == 2  # timeout and task-creation failure
cleanup = section(main, "static void voice_conversation_task(void *argument) {", "void app_main(")
assert "release_media_audio()" in cleanup
wake = section(main, "static void wake_dispatch_task(", "static void reminder_task(")
assert "dispatch_wake()" in wake
assert "app_media_suspend" not in wake and "app_audio_" not in wake
reminder = section(main, "static void reminder_task(", "static void handle_motion(")
assert reminder.index("app_agent_due_notice(") < reminder.index("app_media_suspend(1000)")
assert reminder.index("app_media_suspend(1000)") < reminder.index("app_cloud_speak_reminder(")
assert reminder.count("app_media_resume()") == 1 and "release_media_audio()" in reminder
assert "retry_media_release()" in reminder
release = section(main, "static void release_media_audio(", "static void retry_media_release(")
assert release.index("app_audio_abort_playback()") < release.index("app_media_resume()")
assert "atomic_fetch_add(&s_deferred_media_releases, 1)" in release
assert "if (!atomic_load(&s_media_audio_active))" in reminder
assert "restore_wake()" in reminder

pcm = section(audio, "static esp_err_t play_pcm(", "esp_err_t app_audio_local_reminder(")
assert "wait_voice_pause" not in audio
assert pcm.count("if (cancellable && app_cloud_is_cancelled())") == 2
assert "return cancellable && app_cloud_is_cancelled() ? ESP_ERR_INVALID_STATE : ESP_OK" in pcm
assert "return play_pcm(samples, count, false)" in pcm
assert "return play_pcm(samples, count, true)" in pcm
assert "app_cloud_reset_cancel" not in pcm
assert "s_volume" in pcm and "stereo_samples" in pcm
assert ".max_files = 8" in (MAIN / "app_storage.c").read_text()
assert all(f'"{name}"' in cmake for name in ("app_media.c", "app_media_web.c", "media_web.html"))
assert "esp_http_server" in cmake
assert "app_media_init(storage_ready, media_audio_activity)" in main
assert main.index("ESP_ERROR_CHECK(app_audio_init())") < main.index("app_media_init(")
assert "app_ui_note_activity()" in main

tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for name in ("app_media.c", "app_media.h", "app_media_web.c", "app_media_web.h",
             "app_audio.c", "app_audio.h", "app_desktop.c", "app_ui.c", "main.c"):
    stack = []
    for char in tokens.sub("", (MAIN / name).read_text()):
        if char in "({[": stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char], name
    assert not stack, name
print("PASS: media integration, priority leases and independent cancel source guards; C/runtime/hardware NOT run")
