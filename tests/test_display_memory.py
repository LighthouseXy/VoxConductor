"""Read-only LVGL pool capacity/rollback guards; no compiler or device access."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
display = (root / "firmware/main/app_display.c").read_text()
config = (root / "firmware/sdkconfig").read_text()
cmake = (root / "firmware/main/CMakeLists.txt").read_text()
linker = (root / "firmware/main/lvgl_psram.lf").read_text()
tlsf = (root / "firmware/managed_components/lvgl__lvgl/src/stdlib/builtin/lv_tlsf.c").read_text()

def setting(name):
    return int(re.search(r"^" + name + r"=(\d+)$", config, re.M).group(1))

pool_bytes = int(re.search(r"LCD_WIDGET_POOL_BYTES\s+\((\d+)U \* 1024U\)", display).group(1)) * 1024
pool_count = int(re.search(r"LCD_WIDGET_POOL_COUNT\s+(\d+)U", display).group(1))
configured_bytes = (setting("CONFIG_LV_MEM_SIZE_KILOBYTES") +
                    setting("CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES")) * 1024
assert "CONFIG_LV_USE_BUILTIN_MALLOC=y" in config
# The original 64 KiB pool must not compete with internal FreeRTOS stacks.
assert "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y" in config
assert "CONFIG_SPIRAM_USE_CAPS_ALLOC=y" in config
assert "LDFRAGMENTS \"lvgl_psram.lf\"" in cmake
assert "archive: liblvgl__lvgl.a" in linker
assert "if SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY = y:" in linker
assert "lv_mem_core_builtin (extram_bss)" in linker
assert "* (extram_bss)" not in linker  # Do not move every LVGL object.
assert "#define TLSF_MAX_POOL_SIZE (LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE)" in tlsf
assert "FL_INDEX_MAX = TLSF_LOG2_CEIL(TLSF_MAX_POOL_SIZE)" in tlsf
limit = 1 << (configured_bytes - 1).bit_length()
assert pool_bytes == 64 * 1024 and pool_count == 3
assert pool_bytes <= limit < pool_bytes * pool_count
assert pool_bytes * pool_count == 192 * 1024
assert pool_bytes % 4 == 0  # Each slice keeps the required ESP32 alignment.
assert "_Static_assert(LCD_WIDGET_POOL_BYTES <= LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE," in display
assert "app_psram_malloc(widget_bytes)" in display
assert "lv_mem_add_pool(widget_memory + added * LCD_WIDGET_POOL_BYTES," in display
rollback = display.split("if (added != LCD_WIDGET_POOL_COUNT) {", 1)[1].split("\n  }", 1)[0]
assert rollback.index("lv_mem_remove_pool(pools[--added])") < rollback.index("lvgl_port_unlock()")
assert rollback.index("lvgl_port_unlock()") < rollback.index("free(widget_memory)")
assert "return ESP_ERR_NO_MEM;" in rollback
for message in ("界面PSRAM申请失败", "等待LVGL锁超时", "LVGL内存池注册失败",
                "LVGL任务初始化失败", "LVGL显示器注册失败", "LVGL界面内存池就绪"):
    assert message in display, message
assert "heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)" in display
assert "heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)" in display
assert display.index("LVGL界面内存池就绪") < display.index("lvgl_port_add_disp")
assert display.index("lvgl_port_add_disp") < display.index("app_display_set_brightness(100)")
print("PASS: primary LVGL pool mapped to PSRAM; three extra 64 KiB pools, rollback and diagnostics source guards; C/device NOT run")
