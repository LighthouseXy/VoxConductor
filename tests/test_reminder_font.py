"""Read generated font data to check glyph coverage. Compilation and display acceptance run separately."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
fonts = [MAIN / "app_ui_font_14.c",
         ROOT / "firmware/managed_components/lvgl__lvgl/src/font/lv_font_source_han_sans_sc_14_cjk.c"]
glyphs = {int(value, 16) for font in fonts
          for value in re.findall(r"/\* U\+([0-9A-Fa-f]+)", font.read_text())}
ui = (MAIN / "app_ui.c").read_text()
states = re.search(r"VOICE_STATES\[\]\s*=\s*\{(.*?)\n\};", ui, re.S)
assert states, "voice state label section missing"
texts = re.findall(r'"([^"\n]*)"', states.group(1))
texts += ["小冰小冰可打断", "时间到了，提醒你：喝水。", "语音播放失败，请查看提醒",
          "服药，休息，带伞，开会，吃饭。", "：；？！、（）【】“”…—"]
missing = sorted({char for text in texts for char in text
                  if not char.isspace() and ord(char) not in glyphs})
assert not missing, f"missing reminder/UI glyphs: {''.join(missing)}"
assert ".fallback = &lv_font_source_han_sans_sc_14_cjk" in fonts[0].read_text()
print('PASS: reminder/UI labels and common punctuation have glyphs; scope=offline checks; pending=display hardware validation')
