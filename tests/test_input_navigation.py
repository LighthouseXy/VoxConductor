"""Input/navigation source guards and local arbitration models. Native timing and hardware validation run separately."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
button = (MAIN / "app_button.c").read_text()
main = (MAIN / "main.c").read_text()
desktop = (MAIN / "app_desktop.c").read_text()
ui = (MAIN / "app_ui.c").read_text()
motion = (MAIN / "app_motion.c").read_text()
motion_header = (MAIN / "app_motion.h").read_text()
coordinates = (MAIN / "app_motion_gesture.h").read_text()

route = main[main.index("static bool handle_button_event("):main.index("static esp_err_t run_voice_turn(")]
assert "app_ui_show_next_page" not in route and "app_ui_show_previous_page" not in route
assert "app_ui_enter_page()" in route and "app_ui_back_page()" in route
assert "app_desktop_is_playing" not in route
candidate = route.split("APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE", 1)[1].split("APP_BUTTON_EVENT_SHORT_PRESS", 1)[0]
assert "return stop_active_audio();" in candidate and "app_desktop_button" not in candidate
for event in ("PRESS_DOWN", "SHORT_RELEASE_CANDIDATE", "SHORT_PRESS", "DOUBLE_PRESS", "LONG_PRESS_END"):
    assert f"event == APP_BUTTON_EVENT_{event}" in route
assert "event != APP_BUTTON_EVENT_LONG_PRESS_START" in route
for event, end in (("SHORT_PRESS", "DOUBLE_PRESS"), ("DOUBLE_PRESS", "LONG_PRESS_END")):
    delayed = route[route.index(f"event == APP_BUTTON_EVENT_{event}"):route.index(f"event == APP_BUTTON_EVENT_{end}")]
    assert "stop_active_audio()" not in delayed
    assert delayed.count("atomic_load_explicit(&s_audio_active") == 2
    assert "lvgl_port_lock(0)" in delayed and "lvgl_port_lock(100)" not in delayed
    assert "app_ui_note_activity()" in delayed
    assert "atomic_store(&s_pending_motion, 0)" in delayed
gesture = main[main.index("static void handle_motion("):main.index("static void update_mic_level(")]
assert gesture.index("app_button_click_pending()") < gesture.index("app_desktop_motion(direction)")
producer = gesture.split("static void dispatch_motion(", 1)[0]
assert "atomic_store_explicit(&s_pending_motion" in producer
assert "lvgl_port_lock" not in producer and "app_desktop_motion" not in producer
consumer = gesture.split("static void dispatch_motion(", 1)[1]
assert "atomic_exchange_explicit(&s_pending_motion, 0" in consumer
assert "lvgl_port_lock" not in consumer
assert "lv_timer_create(dispatch_motion, 33" in main
assert "界面忙" not in route and "等待界面锁超时" not in gesture
assert "static _Atomic bool s_click_pending" in button
assert "#define APP_BUTTON_DOUBLE_CLICK_MS 280" in button
assert "#define APP_BUTTON_LONG_PRESS_MS 500" in button
commit = button[button.index("static void finish_click("):button.index("static void poll_button(")]
assert commit.index("emit_event(APP_BUTTON_EVENT_SHORT_PRESS)") < commit.index("clear_click()")
assert "if (s_press_consumed)" in button and "if (consumed)" in button
assert "s_second_click && app_button_click_pending()" in button
assert "!s_second_click" in button and "raw_level == 1 && s_stable_level == 1" in button

activate = desktop[desktop.index("int app_desktop_button("):desktop.index("unsigned app_desktop_base_brightness(")]
back = desktop[desktop.index("int app_desktop_back("):desktop.index("bool app_desktop_motion(")]
assert "s_selection = (s_selection + 1)" not in activate
assert "xQueueSend" not in back and "save_later()" not in back
assert "app_audio_set_volume(s_preferences.volume)" in back
assert "s_zero_x" not in desktop and "s_zero_y" not in desktop
assert "app_motion_sample(&x, &y)" in desktop
assert "alarm_scroll_tick(x, y, valid" in desktop
assert all(f"case {menu}:" in back for menu in (
    "MENU_ROOT", "MENU_FOCUS", "MENU_SETTINGS", "MENU_TASKS",
    "MENU_TASK_ACTION", "MENU_CONFIRM", "MENU_TASK", "MENU_VOLUME", "MENU_STORAGE"))
assert ui.count("app_desktop_create(s_desktop_screen)") == 1
assert re.search(r"s_desktop_open\s*\|\|\s*s_detail_open\s*\|\|\s*s_album_open\s*\|\|\s*s_idle\s*\|\|\s*page_index", ui)
assert "lv_screen_load(s_test_screens[s_current_page_index])" in ui
assert "lv_screen_load_anim(" not in ui
layers = ui[ui.index("void app_ui_enter_page("):ui.index("static void create_voice_overlay(")]
assert "lv_screen_load_anim" not in layers
assert "lv_screen_load(s_desktop_screen)" in layers
assert "lv_screen_load(s_detail_screen)" in layers
assert "lv_screen_load(s_test_screens[s_current_page_index])" in layers

# Restore same-direction input through the shared mapping.
assert "#define APP_MOTION_DIRECTION -1" in motion_header
assert "#define APP_MOTION_VERTICAL_DIRECTION -1" in motion_header
assert "(accel[axis] - horizontal_neutral) * horizontal_direction" in coordinates
assert "(accel[1 - axis] - vertical_neutral) * vertical_direction" in coordinates
direction = re.search(r"int\s+direction\s*=\s*motion_input_step\(", motion)
assert direction and direction.start() < motion.index("s_x = x_mg")
assert "APP_MOTION_VERTICAL_DIRECTION" in motion and "!enabled, &adjusted_vertical" in motion

# Check edited C/header/test delimiters without invoking a compiler.
tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
paths = [MAIN / name for name in (
    "app_button.c", "app_button.h", "main.c", "app_desktop.c", "app_desktop.h",
    "app_ui.c", "app_ui.h", "app_motion.c", "app_motion.h", "app_motion_gesture.h")]
paths += [ROOT / "tests" / name for name in ("test_button.c", "test_motion.c", "test_focus.c")]
for path in paths:
    stack = []
    for char in tokens.sub("", path.read_text()):
        if char in "({[":
            stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char], path.name
    assert not stack, path.name
print('PASS: input dispatch, double-click arbitration, parent navigation and screen-coordinate source guards; scope=offline checks; pending=C/runtime/hardware validation')
