"""Menu wiring guards; actual tilt/LVGL rendering still requires the device."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = (root / "firmware/main/app_desktop.c").read_text()
def items(name):
    body = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", source, re.S).group(1)
    return re.findall(r'"([^"]+)"', body)

assert items("tool_items") == ["番茄钟", "背单词", "任务", "媒体", "测反应", "弹球", "太空飞机"]
assert items("settings_items") == ["显示亮度", "声音音量", "主页外观", "专注时长",
                                  "休息时长", "显示方向", "倾斜校准", "存储状态"]
assert "MENU_WAKE" not in source
assert "app_wake_set_enabled(s_preferences.wake_enabled)" in source
grid = source.split("else if (render_grid()) {", 1)[1].split("if (s_menu == MENU_FOCUS)", 1)[0]
for label in ("s_title", "s_position", "s_hint"):
    assert f"show({label}, false)" in grid
assert "menu_count(), dir * 2" in source
assert "app_menu_grid_move(s_selection, count, direction < 0 ? -1 : 1)" in source
assert 's_focus_confirm ? "确认结束"' in source
assert 'app_words_grade(&s_words, s_selection == 0)' in source
service = source.split("void app_desktop_service(", 1)[1].split("unsigned app_desktop_home_style", 1)[0]
assert service.index("lvgl_port_unlock();") < service.index('nvs_set_u32(h, "words_mask"')
assert "words_save_failed = result != ESP_OK" in service
assert "if (words_save_failed)" in service and "s_words_dirty = true" in service
print("PASS: four-cell menu order/hidden chrome/preserved wake, word persistence and focus confirmation wiring; LVGL/device NOT run")
