"""Source guards for media UI ownership/navigation; no ESP-IDF or hardware needed."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
UI = (ROOT / "firmware/main/app_ui.c").read_text()
DESKTOP = (ROOT / "firmware/main/app_desktop.c").read_text()
HEADER = (ROOT / "firmware/main/app_ui.h").read_text()
CONTROL = (ROOT / "firmware/main/app_control.c").read_text()
FLAT_UI = re.sub(r"\s+", " ", UI)
FLAT_DESKTOP = re.sub(r"\s+", " ", DESKTOP)


def function(source, name):
    start = source.index("{", source.index(name + "("))
    depth = 1
    cursor = start + 1
    while depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    return re.sub(r"\s+", " ", source[start:cursor])


# Four existing roots remain; all additional UI is below the tools page.
assert "#define PAGE_COUNT 4" in UI
assert '"番茄钟", "背单词", "任务", "媒体"' in FLAT_DESKTOP
assert "return sizeof(tool_items) / sizeof(*tool_items);" in DESKTOP
open_tool = function(DESKTOP, "app_desktop_open_tool")
assert "MENU_WORDS" in open_tool and "app_motion_available" not in open_tool
assert "index == 2 ? MENU_TASKS : MENU_MEDIA" in open_tool
back = function(DESKTOP, "app_desktop_back")
for child, parent in (("MENU_MEDIA", "MENU_ROOT"),
                      ("MENU_ANNIVERSARIES", "MENU_ROOT"),
                      ("MENU_WEB", "MENU_MEDIA"),
                      ("MENU_TRACKS", "MENU_MUSIC"),
                      ("MENU_MUSIC_TIMER", "MENU_MUSIC")):
    branch = back.split("case " + child + ":", 1)[1].split("break;", 1)[0]
    assert "enter(" + parent + ")" in branch, (child, parent)
assert "enter(s_wallpaper_from_media ? MENU_MEDIA : MENU_APPEARANCE)" in back

# Large catalogs/pixels are not task-stack or LVGL-pool allocations.
assert "static EXT_RAM_BSS_ATTR app_media_snapshot_t s_ui_media;" in UI
assert "static EXT_RAM_BSS_ATTR app_media_snapshot_t s_media;" in DESKTOP
assert UI.count("app_psram_zero(APP_MEDIA_IMAGE_BYTES)") == 2
assert UI.count(".cf = LV_COLOR_FORMAT_RGB565") == 2
assert FLAT_UI.count(".w = 240, .h = 240, .stride = 480") == 2
assert "s_wallpaper_dsc.data = s_wallpaper_pixels;" in UI
assert "s_album_dsc.data = s_album_pixels;" in UI
cleanup = function(UI, "discard_unpublished_images")
assert "free(s_wallpaper_pixels)" in cleanup and "free(s_album_pixels)" in cleanup
assert "s_wallpaper_pixels = s_album_pixels = NULL" in cleanup
init = function(UI, "app_ui_init")
allocation_failure = init.split("if (!s_wallpaper_pixels || !s_album_pixels)", 1)[1].split("s_wallpaper_dsc.data", 1)[0]
assert "discard_unpublished_images()" in allocation_failure
assert "return ESP_ERR_NO_MEM" not in allocation_failure
assert init.rindex("discard_unpublished_images()") < init.index("create_page(")
assert "if (s_album_dsc.data)" in init
assert "if (s_wallpaper_dsc.data)" in function(UI, "create_page")
assert UI.count("free(s_wallpaper_pixels)") == UI.count("free(s_album_pixels)") == 1
assert "图片缓存内存不足" in UI
assert not re.search(r"\b(?:fopen|fread|opendir|nvs_open|esp_http_client_init)\s*\(", UI)
assert "lv_image_set_src(s_album_image, &s_album_dsc)" in UI
refresh = function(UI, "refresh_media_ui") if "static void refresh_media_ui(void);" not in UI else UI.split("static void refresh_media_ui(void) {", 1)[1].split("static void update_home_time", 1)[0]
assert "app_media_copy_image(false" in refresh
assert "app_media_copy_image(true" in refresh
# The album remains manually browsable, but idle never selects its images.
assert "idle_album" not in refresh and "wallpaper_enabled" not in refresh
assert "app_media_select_image" not in refresh
assert "s_idle_image" not in UI and "s_album_advance_at" not in UI
idle_init = init.split("s_idle_screen = lv_obj_create(NULL);", 1)[1].split("s_last_activity =", 1)[0]
assert "holo_surface(s_idle_screen)" in idle_init
assert "create_clock(s_idle_screen, &s_clocks[1])" in idle_init
assert "lv_image_create" not in idle_init and "create_animated_wall" not in idle_init

# Manual album input does not advance on the first half of a double click.
enter = function(UI, "app_ui_enter_page")
album_branch = enter.split("if (s_album_open", 1)[1].split("return;", 1)[0]
assert "s_album_info_visible = !s_album_info_visible" in album_branch
assert "album_move" not in album_branch
assert "album_move(1)" in function(UI, "app_ui_show_next_page")
assert "album_move(-1)" in function(UI, "app_ui_show_previous_page")
album_back = function(UI, "app_ui_back_page").split("if (s_album_open)", 1)[1].split("return;", 1)[0]
assert "s_album_open = false" in album_back
assert "app_desktop_visible(true)" in album_back
assert "lv_screen_load(s_desktop_screen)" in album_back

# Wake restores the previous base only; voice wakes before adding its overlay.
assert "bool app_ui_note_activity(void);" in HEADER
activity = function(UI, "app_ui_note_activity")
assert "s_last_activity = esp_timer_get_time()" in activity
assert "app_desktop_base_brightness()" in activity
assert "lv_screen_load(s_test_screens[s_current_page_index])" in activity
assert "return woke;" in activity
voice = function(UI, "app_ui_show_voice_state")
assert voice.index("app_ui_note_activity()") < voice.index("show_voice_state(state, message)")
assert "!s_desktop_open && !s_detail_open && !s_album_open" in refresh
assert "s_active_voice_state == NO_VOICE_STATE" in refresh
assert "valid_time && s_ui_appearance.night_enabled && root" in re.sub(r"\s+", " ", refresh)
assert "monotonic - s_last_activity >= 15000000" in refresh
assert "effective = s_ui_appearance.night_brightness" in refresh
assert "s_preferences.brightness =" not in refresh and "nvs_" not in refresh
assert "network.time_valid && app_media_anniversary_days" in FLAT_DESKTOP
assert "game_begin" not in DESKTOP
assert "app_media_suspend(" not in UI and "app_media_suspend(" not in DESKTOP
assert "APP_MEDIA_RESUME" in DESKTOP
assert "music_minutes[] = {0, 15, 30, 60}" in DESKTOP
assert "app_media_web_request(!s_web.enabled)" in DESKTOP
media_voice = function(CONTROL, "app_control_voice_command")
assert all(phrase in media_voice for phrase in ("播放音乐", "暂停音乐", "继续播放音乐", "停止音乐", "上一首", "下一首"))
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in media_voice
assert 'voice_execute("control_media", arguments, text, false)' in media_voice
assert "app_cloud_" not in media_voice and "lvgl_port_lock" not in media_voice
assert "app_control_voice_command(text)" in function(DESKTOP, "app_desktop_voice_command")

# Boundary examples for the exact cross-midnight cap expression used above.
assert re.search(r"start\s*<\s*end\s*\?\s*minute\s*>=\s*start\s*&&\s*minute\s*<\s*end\s*:\s*minute\s*>=\s*start\s*\|\|\s*minute\s*<\s*end", refresh)
def effective_brightness(base, minute, start=1320, end=420, cap=20):
    night = start <= minute < end if start < end else minute >= start or minute < end
    return min(base, cap) if night else base

assert [effective_brightness(80, m) for m in (1319, 1320, 419, 420)] == [80, 20, 20, 80]
assert effective_brightness(10, 1380) == 10
assert [effective_brightness(80, m, 600, 900) for m in (599, 600, 899, 900)] == [80, 20, 20, 80]
print("media UI ownership, navigation, input wake and night-boundary guards: OK")
