"""Verify local SD assets and source routing. Firmware build and physical SD acceptance run separately."""
import array
import hashlib
import json
import math
import re
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "assets/media-pack"
CATALOG = json.loads((PACK / "catalog.json").read_text())
MAIN = ROOT / "firmware/main"
CORE = (MAIN / "app_media.c").read_text()
UI = (MAIN / "app_ui.c").read_text()
DESKTOP = (MAIN / "app_desktop.c").read_text()
HTML = (MAIN / "media_web.html").read_text()

assert len(CATALOG["music"]) == 10
assert len(CATALOG["ambient"]) == 8
assert len(CATALOG["wallpapers"]) == 6
assert CATALOG["music_license"] == "CC BY 4.0"
credits = (PACK / "ATTRIBUTION.txt").read_text()
names = set()
for category in ("music", "ambient", "wallpapers"):
    for item in CATALOG[category]:
        assert item["file"] not in names
        names.add(item["file"])
        assert len(item["file"]) <= 48 and re.fullmatch(r"[A-Za-z0-9_.-]+", item["file"])
        assert item["file"] in CORE and item["label"] in CORE
        if category == "wallpapers":
            raw = (PACK / "sdcard/media/images" / item["file"]).read_bytes()
            assert len(raw) == 115200 and len(set(raw)) > 20
            assert (PACK / "previews" / (Path(item["file"]).stem + ".png")).is_file()
            continue
        path = PACK / "sdcard/media/audio" / item["file"]
        assert path.stat().st_size <= 32 * 1024 * 1024
        with wave.open(str(path), "rb") as sound:
            assert (sound.getnchannels(), sound.getsampwidth(), sound.getframerate(), sound.getcomptype()) == (1, 2, 24000, "NONE")
            frames = sound.getnframes()
            assert frames > 24000
            if category == "ambient":
                assert frames == 60 * 24000 and item["file"].startswith("env_")
            else:
                assert 120 * 24000 < frames < 660 * 24000
                assert item["title"] in credits and item["isrc"] in credits
            # Check real PCM at the start, middle and end alongside the headers.
            samples = array.array("h")
            for offset in (0, frames // 2, frames - 24000):
                sound.setpos(offset)
                samples.frombytes(sound.readframes(24000))
            assert max(abs(x) for x in samples) <= 23600
            assert math.sqrt(sum(x * x for x in samples) / len(samples)) > 50

checksums = json.loads((PACK / "checksums.json").read_text())
assert len(checksums) == 24
for item in checksums:
    raw = (PACK / item["file"]).read_bytes()
    assert len(raw) == item["bytes"]
    assert hashlib.sha256(raw).hexdigest() == item["sha256"]

assert 'LV_SYMBOL_WIFI, &lv_font_montserrat_20' in UI
assert 'visible(s_home_network_slash, !network.connected)' in UI
assert 'lv_label_set_text(s_home_network, network.connected ? "已联网"' not in UI
assert '"相册", "音乐", "环境音", "壁纸", "网页管理"' in re.sub(r"\s+", " ", DESKTOP)
assert "audio_index((unsigned)s_selection)" in DESKTOP
assert "app_media_file_label" in UI and "app_media_file_label" in DESKTOP
assert "adjacent_track(false, false)" in CORE
assert "app_media_audio_is_ambient(s_view.audio[index]) == ambient" in CORE
assert "if (!seamless_loop && remaining <= 256)" in CORE
rewind = CORE.split("if (seamless_loop && !atomic_load(&s_holds)", 1)[1].split("close_track(true)", 1)[0]
assert "fseek(s_track, (long)s_wav.offset, SEEK_SET)" in rewind
assert "s_pcm_cursor = s_pcm_count = 0" in rewind and "continue;" in rewind
assert "app_audio_abort_playback" not in rewind
assert "s_preferences.appearance.wallpaper_enabled = true" in CORE
assert "load_image(selected, true, false)" in CORE
assert "load_image(index, s_pending_image.wallpaper, true)" in CORE
assert "webkitdirectory multiple" in HTML and "packCompleted.add(file.name)" in HTML
assert "await upload(kind,file.name,file)" in HTML
print("PASS: 10 real songs, 8 synthesized ambience loops, 6 wallpapers, attribution, hashes, PCM samples, Wi-Fi/menu/loop guards; scope=asset/source checks; pending=C/hardware acceptance")
