"""Applied-home source guards and local policy models for failed-speech outcomes. Native/device acceptance runs separately."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
control = (ROOT / "firmware/main/app_control.c").read_text()
main = (ROOT / "firmware/main/main.c").read_text()
ui = (ROOT / "firmware/main/app_ui.c").read_text()
settings = control.split("static cJSON *set_setting(", 1)[1].split("static cJSON *control_media", 1)[0]
cancel = control.split("void app_control_voice_cancel(void) {", 1)[1].split("bool app_control_voice_home_applied", 1)[0]
finish = control.split("bool app_control_voice_finish(bool success) {", 1)[1]
failed = finish.split("if (!success) {", 1)[1].split("if (!finish)", 1)[0]
assert settings.index("bool applied = readback") < settings.index("s_after.home_applied = true")
assert 'if (applied && (!strcmp(s, "home_style") || !strcmp(s, "wallpaper_mode")))' in settings
assert cancel.index("bool home_applied = s_after.home_applied") < cancel.index("memset(&s_after")
assert "s_after.home_applied = home_applied" in cancel  # readback receipt; authorization remains separately managed
assert "s_after.home_applied && !app_cloud_is_cancelled()" in failed
assert failed.index("memset(&s_after") < failed.index('"home"')
assert "app_desktop_game_control" not in failed and "app_media_" not in failed
assert "if (!show_applied_home) return false" in failed
assert finish.index("lvgl_port_lock(300)") < finish.index("app_ui_hide_voice_state()") < finish.index("app_ui_open_destination")
assert finish.index("app_ui_open_destination") < finish.rindex("memset(&s_after")
assert "app_control_voice_home_applied() ? \"主页已更改，语音回答未完成\"" in main
assert "s_home_preview_requested && s_active_voice_state == NO_VOICE_STATE" in ui
turn = main.split("static esp_err_t run_voice_turn(", 1)[1].split("static void voice_conversation_task(void *argument) {", 1)[0]
assert turn.index("if (result == ESP_OK) {\n    app_turn_emit(&turn, TURN_PLAY_END") < turn.index("app_cloud_commit_turn")
assert "app_agent_cancel_pending()" in turn  # failed replies still revoke pending authority

class Outcome:
    def __init__(self):
        self.style = self.saved_style = 1
        self.home_applied = False
        self.action = ""
        self.page = "assistant"
        self.history_commits = 0
        self.page_opened = False
    def setting(self, value, applied=True, saved=True):
        if applied:
            self.style = value
            if saved:
                self.saved_style = value
            self.home_applied = True
            self.action = "home"
    def cancel_pending(self):
        self.action = ""  # retain only evidence of the actual setting
    def finish(self, reply_ok, cancelled=False):
        action, applied = self.action, self.home_applied
        self.action, self.home_applied = "", False
        if reply_ok and not cancelled:
            self.history_commits += 1
        else:
            action = "home" if applied and not cancelled else ""
        if action == "home":
            self.page = "home"
        if action == "game":
            self.page_opened = True
        return bool(action)

# Persisted/readback success and ASR/LLM/TTS success are different outcomes.
o = Outcome()
o.setting(0)
o.cancel_pending()  # real failure cleanup used to erase the home handoff
assert o.finish(False) and o.page == "home" and o.style == o.saved_style == 0
assert not o.history_commits and not o.home_applied
assert not o.finish(False)  # the receipt cannot leak into the next turn

for cancelled in (False, True):
    o = Outcome()
    o.action = "game"
    o.cancel_pending()
    assert not o.finish(False, cancelled) and not o.page_opened
o = Outcome()
o.setting(0)
o.cancel_pending()
assert not o.finish(False, cancelled=True)
assert o.page == "assistant" and o.style == o.saved_style == 0  # no fake rollback
o = Outcome()
o.setting(0, applied=False)
o.cancel_pending()
assert not o.finish(False) and o.style == o.saved_style == 1
o = Outcome()
o.setting(0, saved=False)
o.cancel_pending()
assert o.finish(False) and o.page == "home" and o.style == 0 and o.saved_style == 1
o = Outcome()
o.setting(0)
assert o.finish(True) and o.page == "home" and o.history_commits == 1

print('PASS: applied-home vs reply result, cancel/unapplied/deferred-action boundaries; scope=offline checks; pending=native/device validation')
