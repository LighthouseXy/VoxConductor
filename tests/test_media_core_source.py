#!/usr/bin/env python3
"""Media contract/source guards plus Python boundary reference fixtures.

This deliberately does not compile firmware or claim C parser/device playback
verification. It protects the important source invariants while the user builds.
"""
from datetime import date, datetime, timezone, timedelta
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "firmware/main/app_media.c").read_text()


def body(name):
    start = SOURCE.index("{", SOURCE.index(name + "("))
    depth = 1
    for end in range(start + 1, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[start + 1:end]
    raise AssertionError(name)


def civil_days(year, month, day):
    year -= month <= 2
    era = year // 400
    yo = year - era * 400
    mp = month + (-3 if month > 2 else 9)
    doy = (153 * mp + 2) // 5 + day - 1
    return era * 146097 + yo * 365 + yo // 4 - yo // 100 + doy - 719468


def valid_name(name, image):
    suffix = ".rgb565" if image else ".wav"
    return (len(suffix) < len(name) < 49 and name.endswith(suffix)
            and not name.startswith(".") and ".." not in name
            and re.fullmatch(r"[A-Za-z0-9_.-]+", name) is not None)


def chunk(kind, value):
    return kind + struct.pack("<I", len(value)) + value + b"\0" * (len(value) & 1)


FORMAT = struct.pack("<HHIIHH", 1, 1, 24000, 48000, 2, 16)


def wav(*chunks):
    value = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(value)) + value


def wav_reference(value):
    if (len(value) < 44 or len(value) > 32 * 1024 * 1024
            or value[:4] != b"RIFF" or value[8:12] != b"WAVE"):
        return False
    end = struct.unpack_from("<I", value, 4)[0] + 8
    if end != len(value):
        return False
    fmt = data = False
    pos = 12
    for _ in range(256):
        if pos + 8 > end:
            return False
        kind, size = value[pos:pos + 4], struct.unpack_from("<I", value, pos + 4)[0]
        nxt = pos + 8 + size + (size & 1)
        if nxt > end:
            return False
        if kind == b"fmt ":
            if fmt or size < 16 or value[pos + 8:pos + 24] != FORMAT:
                return False
            fmt = True
        elif kind == b"data":
            if data or not size or size & 1:
                return False
            data = True
        pos = nxt
        if pos == end:
            return fmt and data
    return False


def main():
    for name in ("lock", "unlock"):
        definition = SOURCE.index(f"static void {name}(void)")
        first_call = re.search(rf"\b{name}\s*\(\s*\)", SOURCE)
        assert first_call and definition < first_call.start(), f"{name} used before definition"
    begin = body("begin_io")
    assert begin.index("atomic_store(&s_busy, true)") < begin.index("atomic_load(&s_holds)")
    suspend = body("app_media_suspend")
    assert suspend.index("atomic_fetch_add(&s_holds, 1)") < suspend.index("atomic_load(&s_busy)")
    close = body("close_track")
    assert close.index("fclose(s_track)") < close.index("app_audio_abort_playback")
    assert close.index("app_audio_abort_playback") < close.index("s_audio_activity(false)")
    assert close.index("s_audio_activity(false)") < close.index("atomic_store(&s_busy, false)")
    assert "if (released != ESP_OK)" in close and "s_release_failed = true" in close
    failure = close.split("if (released != ESP_OK)")[1].split("s_release_failed = false")[0]
    assert "atomic_store(&s_busy, false)" not in failure
    assert "s_audio_activity(false)" not in failure
    assert "app_cloud_" not in SOURCE
    assert "app_audio_play_media_pcm" in SOURCE
    assert "app_psram_malloc(4096 * sizeof(*s_pcm_buffer))" in SOURCE
    assert "app_psram_malloc(APP_MEDIA_IMAGE_BYTES)" in SOURCE
    assert "media_preferences_t" in SOURCE and 'nvs_open("media"' in SOURCE
    parse = body("wav_read")
    for guard in ("APP_MEDIA_MAX_WAV_BYTES", "end != (uint64_t)size",
                  "next > end", "chunks++ < 256", "bytes & 1u", "WAV_RATE * 2",
                  "le16(header + 12) != 2", "le16(header + 14) != 16"):
        assert guard in parse, guard
    scan = body("scan_media")
    assert "track_name" in scan and "strcmp(track_name, s_view.audio[s_track_index])" in scan
    assert "s_want_play = s_user_paused = false" in scan
    directory = body("scan_directory")
    assert directory.index("app_media_valid_name(entry->d_name, image)") < directory.index("snprintf(path")
    assert '"%s/%.*s"' in directory and '"%.*s"' in directory
    assert directory.count("APP_MEDIA_NAME_BYTES - 1") == 2
    assert "path_length < 0 || (size_t)path_length >= sizeof(path)" in directory
    command = body("audio_request")
    pause = command.split("case APP_MEDIA_PAUSE:")[1].split("case APP_MEDIA_RESUME:")[0]
    assert "s_want_play = false" in pause and "!s_want_play" not in pause
    resume = command.split("case APP_MEDIA_RESUME:")[1].split("case APP_MEDIA_STOP:")[0]
    assert "s_want_play = true" in resume and "s_position = 0" not in resume
    worker = body("worker")
    assert "xQueueSendToFront" not in worker
    assert "s_pending_image = request" in worker
    assert "s_image_pending = true" in worker
    for request in ("REQUEST_APPEARANCE", "REQUEST_ANNIVERSARY"):
        section = worker.split("request.type == " + request)[1].split("save_preferences();")[0]
        assert "close_track(true)" in section
    assert "close_track(true)" in command.split("case APP_MEDIA_REPEAT:")[1].split("case APP_MEDIA_AMBIENT_TIMER:")[0]
    repeat = worker.split("if (repeat && count)")[1].split("else if")[0]
    assert "++s_track_index" not in repeat
    assert "memcpy(pcm, s_pcm_buffer + s_pcm_cursor" in worker
    assert "s_stop_at && esp_timer_get_time() >= s_stop_at" in worker
    assert "s_sd_ready = false" in body("app_media_init")
    assert "s_view.ready = false" in body("app_media_init")
    assert "s_worker_ready" in body("enqueue")

    epoch = date(1970, 1, 1)
    for day in (epoch, date(2000, 2, 29), date(2026, 1, 1), date(2026, 12, 31),
                date(2100, 3, 1), date(2199, 12, 31)):
        assert civil_days(day.year, day.month, day.day) == (day - epoch).days
    china = timezone(timedelta(hours=8))
    utc = datetime(2026, 10, 3, 16, 1, tzinfo=timezone.utc)
    assert utc.astimezone(china).date() == date(2026, 10, 4)
    today = date(2026, 10, 4)
    assert (date(2026, 10, 4) - today).days == 0
    assert (date(2027, 1, 1) - today).days == 89
    assert (date(2028, 2, 29) - today).days == 513  # next actual leap-day occurrence
    assert valid_name("01-rain.wav", False) and valid_name("photo_1.rgb565", True)
    for name in ("../rain.wav", ".hidden.wav", "a..wav", "rain.WAV", "水.wav",
                 "a/b.wav", "a\\b.wav", "a.wav?x=1", "a" * 45 + ".wav"):
        assert not valid_name(name, False), name
    assert valid_name("a" * 44 + ".wav", False)

    valid = wav(chunk(b"fmt ", FORMAT), chunk(b"data", b"\0\0" * 8))
    assert wav_reference(valid)
    assert wav_reference(wav(chunk(b"JUNK", b"123"), chunk(b"data", b"\0\0" * 8),
                             chunk(b"fmt ", FORMAT)))
    for invalid in (valid[:-1], valid + b"garbage", wav(chunk(b"data", b"123")),
                    wav(chunk(b"fmt ", FORMAT), chunk(b"data", b"123")),
                    wav(chunk(b"fmt ", FORMAT), chunk(b"data", b"\0\0"), chunk(b"data", b"\0\0")),
                    wav(chunk(b"fmt ", FORMAT), chunk(b"JUNK", b"12")),
                    wav(*(chunk(b"JUNK", b"") for _ in range(257)),
                        chunk(b"fmt ", FORMAT), chunk(b"data", b"\0\0"))):
        assert not wav_reference(invalid)
    print("media core source guards and Python boundary fixtures: OK (not C/device runtime)")


if __name__ == "__main__":
    main()
