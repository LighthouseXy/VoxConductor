"""Voice tool source/schema guards and Python policy fixtures.

Microphone behavior, durable SD writes and physical page changes require
separate native or device acceptance tests."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
sources = {name: (MAIN / (name + ".c")).read_text()
           for name in ("app_control", "app_agent", "app_desktop", "app_media", "app_ui", "main", "app_conversation")}
tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)


def masked(source):
    return tokens.sub(lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), source)


def body(source, name):
    clean = masked(source)
    match = re.search(r"(?m)^(?:static\s+)?[A-Za-z_][\w\s*]*\b" + re.escape(name) +
                      r"\s*\([^;{}]*\)\s*\{", clean)
    assert match, name
    start = clean.index("{", match.start())
    depth = 1
    for end in range(start + 1, len(clean)):
        depth += (clean[end] == "{") - (clean[end] == "}")
        if not depth:
            return source[start + 1:end]
    raise AssertionError(name)


for name, source in sources.items():
    stack = []
    for c in masked(source):
        if c in "({[":
            stack.append(c)
        elif c in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[c], name
    assert not stack, name

control, agent, desktop, media, ui, main, conversation = (sources[n] for n in sources)
schema_body = body(control, "app_control_voice_tools")
literal = re.search(r'cJSON_Parse\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', schema_body)
tools = json.loads("".join(json.loads(m[0]) for m in re.finditer(r'"(?:\\.|[^"\\])*"', literal[1])))
names = [tool["function"]["name"] for tool in tools]
assert names == ["get_local_state", "set_local_setting", "control_media", "control_desktop", "manage_anniversary"]
for tool in tools:
    params = tool["function"]["parameters"]
    assert params["additionalProperties"] is False
    assert set(params["required"]) <= set(params["properties"])

props = {tool["function"]["name"]: tool["function"]["parameters"]["properties"] for tool in tools}
assert len(props["set_local_setting"]["setting"]["enum"]) == 14
assert props["control_media"]["value"]["maximum"] == 240
assert props["manage_anniversary"]["index"]["maximum"] == 3
assert props["control_desktop"]["minutes"]["maximum"] == 120
assert not set(props["control_desktop"]["action"]["enum"]) & {"shoot", "move", "rotate", "steer"}
assert all("url" not in p and "token" not in p and "path" not in p for p in props.values())

execute = body(control, "voice_execute")
assert "strstr(allowed, key)" in execute and "|request|" in execute
assert "strlen(arguments) > 512" in execute and "true);" in execute
assert "explicit_action" not in control and "WORDS(" not in control
wrapper = body(control, "app_control_voice_execute")
assert "app_agent_request_matches(args, user_text)" in wrapper
assert "app_agent_has_pending()" in wrapper
settings = body(control, "set_setting")
assert "setting_result(" in settings and '"saved", ok' in settings
assert "app_desktop_configuration(&d, NULL)" in settings
assert "app_media_get_appearance(&a)" in settings
assert "value >= 10 && value <= 3600" in settings
web_configure = body(control, "configure")
assert "config_field(name)" in web_configure and "app_desktop_configuration_patch" in web_configure
assert "expected_revision" in web_configure and "APP_CONFIG_CONFLICT" in web_configure

pending = body(control, "prepare")
assert "app_agent_has_pending()" in pending and "120000000" in pending
assert '"executed", false' in pending
command = body(control, "app_control_voice_command")
assert command.index("app_control_voice_pending()") < command.index('phrase(text, "确认执行")')
assert "s_voice_deadline = 0" in command
anniversary = body(control, "anniversary")
assert "s_anniversary_expected" in anniversary and "confirmed" in anniversary
assert "app_media_anniversary_days" in anniversary
assert "app_media_anniversary_tracked" in anniversary

finish = body(control, "app_control_voice_finish")
assert finish.index("if (!success)") < finish.index("app_ui_open_destination")
assert finish.index("lvgl_port_lock(300)") < finish.index("app_ui_hide_voice_state()") < finish.index("app_ui_open_destination")
loop = body(main, "voice_conversation_task")
assert loop.index("run_voice_turn(automatic, followup, interrupted)") < loop.index("app_control_voice_finish(result == ESP_OK &&")
assert "!app_cloud_is_cancelled() && !app_acoustic_pending()" in loop
assert loop.index("if (finished_control) break") < loop.index("app_audio_prepare_auto_recording")
turn = body(main, "run_voice_turn")
assert turn.index("app_cloud_speak(answer, &turn)") < turn.index("app_cloud_commit_turn")
assert "app_control_voice_cancel()" in body(agent, "app_agent_cancel_pending")
assert "app_control_voice_pending_context()" in body(agent, "app_agent_pending_context")
assert "app_control_voice_execute(name, arguments, user_text)" in body(agent, "app_agent_execute")
assert "APP_TOOL_LOCAL_READ" in body(agent, "app_agent_tool_mask") and "HAS(" not in body(agent, "app_agent_tool_mask")
budget = body(agent, "app_agent_accept_call")
assert all('"' + key + '"' in budget for key in ("detail", "scope", "setting", "action", "index"))
assert "app_agent_write_tool(name)" in budget and "budget->count >= 4" in budget
assert "app_control_voice_command(text)" in body(desktop, "app_desktop_voice_command")

receipt = body(control, "media_result")
assert "app_media_job_status(id, &job)" in receipt and "job.done" in receipt
assert "job.applied && job.saved" in receipt and '"status", "pending"' in receipt
assert "1200000" in receipt and "app_cloud_is_cancelled()" in receipt
assert "if (delayed) break" in receipt
worker = body(media, "worker")
assert worker.index("REQUEST_AUDIO") < worker.index("if (atomic_load(&s_holds))")
assert "complete_job(request.job_id" in worker and "s_pending_image.job_id" in worker
assert "request.value.image_name" in worker and "find_name(s_view.audio" in worker
assert "s_jobs[id % 4].id == id" in body(media, "app_media_job_status")
assert "job->id && !job->done" in body(media, "enqueue_tracked")
assert "xTaskCreate" not in control and "app_control_submit(" not in body(control, "voice_execute")
assert "app_motion_set_enabled(true)" in body(control, "control_desktop")
assert "app_motion_set_enabled(false)" in body(control, "control_desktop")
assert "app_motion_available()" in body(control, "control_desktop")
assert "web.token" not in masked(body(control, "control_desktop"))
assert "if (!valid) return false" in body(ui, "app_ui_open_destination")

weather = body(agent, "app_agent_execute")
for flag in ("current_uv_available", "daily_uv_available", "air_available", "wind_available"):
    assert '"' + flag + '"' in weather
assert "US_AQI" in weather and "app_weather_uv_periods" in weather
assert "app_weather_future_hours" in weather and "if (count > 12) count = 12" in weather
assert "air_observed_at" in weather and "clock_not_synchronized" in weather
assert "pending_confirmation" in conversation and "saved=false" in conversation

# Policy models follow validated ranges; C execution is checked separately.
def setting_valid(name, value):
    if not isinstance(value, int) or isinstance(value, bool):
        return False
    limits = {
        "brightness": (0, 100), "volume": (0, 100), "night_brightness": (0, 100),
        "home_style": (0, 1), "wallpaper_mode": (0, 4), "wake_enabled": (0, 1),
        "night_enabled": (0, 1), "work_minutes": (1, 120), "break_minutes": (1, 120),
        "night_start_minute": (0, 1439), "night_end_minute": (0, 1439),
        "tts_voice_id": (0, 6),
    }
    if name == "idle_seconds":
        return value == 0 or 10 <= value <= 3600
    if name == "tts_rate_percent":
        return 50 <= value <= 200 and value % 5 == 0
    return name in limits and limits[name][0] <= value <= limits[name][1]

assert all(setting_valid("brightness", x) for x in (0, 50, 100))
assert not any(setting_valid("brightness", x) for x in (-1, 101, 50.5, True))
assert [setting_valid("idle_seconds", x) for x in (0, 1, 9, 10, 3600, 3601)] == [True, False, False, True, True, False]
assert not setting_valid("work_minutes", 0) and setting_valid("work_minutes", 120)
assert not setting_valid("night_start_minute", 1440)
assert not setting_valid("random_url", 1)
assert set(props["set_local_setting"]["setting"]["enum"]) == set([
    "brightness", "volume", "home_style", "wallpaper_mode", "wake_enabled", "work_minutes",
    "break_minutes", "night_enabled", "night_start_minute", "night_end_minute", "night_brightness", "idle_seconds", "tts_rate_percent", "tts_voice_id"])
assert all(setting_valid("tts_rate_percent", x) for x in (50, 100, 125, 200))
assert not any(setting_valid("tts_rate_percent", x) for x in (49, 101, 201, True, 100.5))

def confirmation(created, now, exact, changed=False):
    return created < now < created + 120_000_000 and exact and not changed

assert confirmation(1000, 1001, True)
assert not confirmation(1000, 120001000, True)
assert not confirmation(1000, 1001, False)
assert not confirmation(1000, 1001, True, changed=True)
# Pending != applied; a saved=false result must never be announced as persisted.
for done, applied, saved, expected in [
    (False, False, False, "pending"), (True, False, False, "failed"),
    (True, True, False, "partial"), (True, True, True, "applied"),
]:
    actual = "pending" if not done else "applied" if applied and saved else "partial" if applied else "failed"
    assert actual == expected
print("PASS: 5 voice tool schemas, authorization/confirmation, queue receipts, deferred UI, weather fields and policy fixtures; scope=source/policy fixtures; pending=C/hardware acceptance")
