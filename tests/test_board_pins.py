"""Check PCB GPIOs; optional argument: KiCad XML netlist exported from schematic."""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
# Frozen from the user-confirmed VoxConductor schematic on 2026-09-25.
PINS = {
    "app_display.c": {
        "LCD_PIN_DC": ("LCD_DC", 11), "LCD_PIN_RST": ("LCD_RST", 12),
        "LCD_PIN_MOSI": ("LCD_MOSI", 10), "LCD_PIN_SCLK": ("LCD_SCLK", 13),
        "LCD_PIN_BL": ("LCD_BL", 9),
    },
    "app_audio.c": {
        "MIC_PIN_SCK": ("MIC_SCK", 2), "MIC_PIN_WS": ("MIC_WS", 1),
        "MIC_PIN_SD": ("MIC_SD", 42), "SPEAKER_PIN_BCLK": ("AMP_BCLK", 5),
        "SPEAKER_PIN_LRC": ("AMP_LRC", 6), "SPEAKER_PIN_DIN": ("AMP_DIN", 4),
    },
    "app_storage.c": {
        "SD_PIN_CS": ("SD_CS", 48), "SD_PIN_SCK": ("SD_SCK", 21),
        "SD_PIN_MOSI": ("SD_MOSI", 47), "SD_PIN_MISO": ("SD_MISO", 14),
    },
    "app_button.c": {"APP_BUTTON_PIN": ("KEY_USER1", 16)},
    "app_motion.h": {
        "APP_MOTION_SDA_GPIO": ("/MPU_SDA", 39),
        "APP_MOTION_SCL_GPIO": ("/MPU_SCL", 40),
    },
}

netlist = ET.parse(sys.argv[1]).getroot() if len(sys.argv) > 1 else None
used = {}
for filename, pins in PINS.items():
    source = (ROOT / "firmware/main" / filename).read_text()
    for macro, (net, expected) in pins.items():
        match = re.search(r"^#define\s+" + macro + r"\s+(?:GPIO_NUM_)?(\d+)\s*$", source, re.M)
        assert match, f"Missing GPIO definition: {macro}"
        actual = int(match[1])
        assert actual == expected, f"{macro}: expected {expected}, got {actual}"
        assert actual not in used, f"GPIO{actual} conflict: {macro} / {used.get(actual)}"
        assert actual not in (0, 19, 20, 35, 36, 37, 41), f"Reserved GPIO: {macro}"
        used[actual] = macro
        if netlist is not None:
            nodes = [p for n in netlist.findall("./nets/net") if n.get("name") == net
                     for p in n.findall("node") if p.get("ref") == "U1"]
            assert len(nodes) == 1, f"Missing/ambiguous MCU node: {net}"
            assert re.fullmatch(rf"IO{actual}_\d+", nodes[0].get("pinfunction", "")), net
print(f"PASS: {len(used)} PCB GPIOs match; no conflicts;" +
      (" KiCad netlist verified" if netlist is not None else " frozen mapping verified"))

# Native backlight control must not change pins, optical compensation or buffers.
display = (ROOT / "firmware/main/app_display.c").read_text()
assert '#include "driver/ledc.h"' in display
assert re.search(r"LCD_BL_PWM_HZ\s+5000\b", display)
assert "LEDC_TIMER_10_BIT" in display
for config in (".mirror_x = true", ".mirror_y = false", ".swap_xy = false",
               ".double_buffer = false", ".buff_dma = true"):
    assert config in display, f"Display contract changed: {config}"
assert "esp_lcd_panel_set_gap(panel_handle, 0, 0)" in display
assert "percent > 100" in display and "ESP_ERR_INVALID_ARG" in display
assert "atomic_load(&s_ready)" in display and "ESP_ERR_INVALID_STATE" in display
assert ".duty = 0" in display and "gpio_set_level(LCD_PIN_BL, 1)" not in display
assert display.index("lv_display_set_default(display)") < display.index(
    "result = app_display_set_brightness(100)")
assert "xSemaphoreCreateMutexStatic" in display
assert "ledc_set_duty(" in display and "ledc_update_duty(" in display
assert "ledc_fade_func_install" not in display
assert "esp_driver_ledc" in (ROOT / "firmware/main/CMakeLists.txt").read_text()
for source in (ROOT / "firmware/main").glob("*.c"):
    if source.name != "app_display.c":
        assert not re.search(r"\bledc_(?:timer_config|channel_config)\s*\(",
                             source.read_text()), f"LEDC resource collision: {source}"
duties = [((1 << 10) * percent + 50) // 100 for percent in range(101)]
assert duties[0] == 0 and duties[-1] == 1024
assert all(a <= b for a, b in zip(duties, duties[1:]))

motion = (ROOT / "firmware/main/app_motion.c").read_text()
header = (ROOT / "firmware/main/app_motion.h").read_text()
for api in ("app_motion_available", "app_motion_request_calibration",
            "app_motion_calibrating"):
    assert f"bool {api}(void)" in motion and f"bool {api}(void)" in header
assert re.search(r"age\s*<=\s*200000", motion)
assert re.search(r"if\s*\(!app_motion_available\(\)\)\s*return false;", motion)
assert "atomic_exchange(&s_calibration_requested, false)" in motion
assert "motion_input_reset_calibration(&input)" in motion
assert "vertical_calibration = (motion_recenter_t){0}" in motion
assert "input.calibrated && vertical_calibrated" in motion
assert "bool recovering = valid && !app_motion_available()" in motion
assert re.search(r"if\s*\(!valid\)\s*invalidate_sample\(\)", motion)
read_failure = motion[motion.index("if (result != ESP_OK)",
                                   motion.index("while (true)")):]
assert read_failure.index("invalidate_sample()") < read_failure.index("vTaskDelay(")
assert "direction && !calibrating && !atomic_load(&s_calibrating)" in motion
gesture = (ROOT / "firmware/main/app_motion_gesture.h").read_text()
reset = gesture[gesture.index("static inline void motion_input_reset_calibration"):]
reset = reset[:reset.index("static inline int motion_input_step")]
assert "require_stable_center = true" in reset and "s->neutral =" not in reset
assert "MOTION_RECENTER_MS 1000" in gesture
print("PASS: PWM range/startup/driver resources and fresh motion/manual stable calibration source guards; hardware/C not run")
