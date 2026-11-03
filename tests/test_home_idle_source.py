"""Home-page selection and idle-policy source guards with local state models."""
from itertools import product
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
desktop = (MAIN / "app_desktop.c").read_text()
ui = (MAIN / "app_ui.c").read_text()
media = (MAIN / "app_media.h").read_text()
html = (MAIN / "media_web.html").read_text()


def function(source, name):
    match = re.search(r"\b" + name + r"\s*\([^;{}]*\)\s*\{", source)
    assert match, name
    start = match.end() - 1
    depth, at = 1, start + 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return re.sub(r"\s+", " ", source[start:at])


dynamic_first = int(re.search(r"WALLPAPER_DYNAMIC_FIRST = (\d+)", desktop)[1])
image_first = int(re.search(r"WALLPAPER_IMAGE_FIRST = (\d+)", desktop)[1])
assert dynamic_first == 2 and image_first - dynamic_first == 4
assert 'home_style_names[] = {"圆形时钟", "壁纸+数字时间"}' in desktop
items = re.search(r"appearance_items\[\] = \{(.*?)\};", desktop, re.S)[1]
assert re.findall(r'"([^\"]*)"', items) == [
    "圆形时钟", "壁纸+数字时间", "选择壁纸", "夜间调暗", "闲置待机"
]
assert "屏保壁纸" not in desktop and "待机画面" not in desktop
setter = function(desktop, "select_home_style")
assert "style > 1" in setter and "s_home_style = style" in setter
assert "save_later()" in setter and "app_ui_preview_home()" in setter
assert "s_wallpaper_mode =" not in setter and "app_media_select_image" not in setter
assert 'nvs_get_u32(h, "home_style", &home_style)' in desktop
assert 'nvs_set_u32(h, "home_style", home_style)' in desktop
assert 'nvs_set_u32(h, "wall_mode", wallpaper_mode)' in desktop
assert ".version = 5" in desktop
assert "loaded.version < 5" in desktop  # pre-TTS preferences retain migration
activate = function(desktop, "activate")
appearance = activate.split("case MENU_APPEARANCE:", 1)[1].split("case MENU_WALLPAPER:", 1)[0]
wallpaper = activate.split("case MENU_WALLPAPER:", 1)[1]
assert "s_selection < 2" in appearance
assert "select_home_style((unsigned)s_selection)" in appearance
assert "select_home_style((unsigned)s_selection)" in wallpaper
assert "open_wallpapers(true)" in activate and "open_wallpapers(false)" in appearance
assert "- WALLPAPER_DYNAMIC_FIRST + 1" in wallpaper
assert "- WALLPAPER_IMAGE_FIRST" in wallpaper
assert "s_media.image_count + WALLPAPER_IMAGE_FIRST" in function(desktop, "menu_count")
picker = function(desktop, "wallpaper_item")
assert "home_style_names[index]" in picker
assert "wallpaper_names[index - WALLPAPER_DYNAMIC_FIRST]" in picker
assert "image < s_media.image_count" in picker  # Catalog deletion is bounded.
open_picker = function(desktop, "open_wallpapers")
assert "s_selection = (int)s_home_style" in open_picker
assert "s_wallpaper_mode" not in open_picker and "wallpaper_index" not in open_picker

# Enumerate the exact index policy linked to the constants and source above.
# First two cells always exist, including a device without an SD card.
for count, style, mode in product((0, 1, 32), (0, 1), range(5)):
    selected = style
    assert 0 <= selected < count + image_first
    # The recycled grid starts at the first row, so both choices are visible.
    row = selected // 3
    grid_start = (row - 2 if row > 2 else 0) * 3
    assert grid_start == 0
    assert list(range(dynamic_first, image_first)) == [2, 3, 4, 5]
    assert [i - dynamic_first + 1 for i in range(dynamic_first, image_first)] == [1, 2, 3, 4]
    assert [i - image_first for i in range(image_first, count + image_first)] == list(range(count))

# No media object exists on the idle screen, so old NVS flags cannot add one.
assert "s_idle_image" not in ui and "s_album_advance_at" not in ui
assert "idle_album" not in ui and "wallpaper_enabled" not in ui
assert "static holo_wall_t s_animated;" in ui and "s_animated[" not in ui
init = function(ui, "app_ui_init")
idle = init.split("s_idle_screen = lv_obj_create(NULL);", 1)[1].split("s_last_activity =", 1)[0]
assert "holo_surface(s_idle_screen)" in idle
assert "create_clock(s_idle_screen, &s_clocks[1])" in idle
assert "lv_image_create" not in idle and "create_animated_wall" not in idle
tick = function(ui, "wallpaper_tick")
assert "!mode || s_idle ||" in tick
refresh = function(ui, "refresh_media_ui")
assert "lv_screen_load(s_idle_screen)" in refresh
assert "s_home_style =" not in refresh and "app_media_select_image" not in refresh
activity = function(ui, "app_ui_note_activity")
assert "lv_screen_load(s_test_screens[s_current_page_index])" in activity
assert "app_desktop_select_wallpaper_mode" not in activity
# Preserve stored struct/wire fields for old firmware/pages, but hide controls.
assert "wallpaper_enabled" in media and "idle_album" in media
assert 'id="wallpaperEnabled"' not in html and 'id="idleAlbum"' not in html
assert "wallpaper_enabled:true" in html and "idle_album:false" in html
assert '<option value="0">圆形时钟</option>' in html
assert "屏保始终为纯时钟" in html
print('PASS: explicit clock/digital picker, empty/full catalog bounds, preserved selection and clock-only idle; scope=offline checks; pending=C/device validation')
