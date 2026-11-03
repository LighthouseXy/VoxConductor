#!/usr/bin/env python3
"""Prepare SD-ready licensed music, synthesized ambience and generated wallpapers.

No firmware compilation, API key, serial access or device/SD writes. Outputs
only to assets/media-pack; downloads only when --download-music is requested.
Requires the bundled Pillow/NumPy runtime, and macOS afconvert for MP3.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import urllib.parse
import urllib.request
import wave
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "assets/media-pack"
RATE = 24000


def write_wav(path, samples, rms=0.09, peak=0.65):
    samples = np.asarray(samples, dtype=np.float64)
    samples -= samples.mean()
    level = float(np.sqrt(np.mean(samples * samples)))
    maximum = float(np.max(np.abs(samples)))
    if not level or not maximum:
        raise ValueError("Silent audio")
    gain = min(rms / level, peak / maximum, 4.0)
    pcm = np.rint(samples * gain * 32767).astype("<i2")
    with wave.open(str(path), "wb") as out:
        out.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        out.writeframes(pcm.tobytes())


def read_wav(path):
    with wave.open(str(path), "rb") as src:
        if (src.getnchannels(), src.getsampwidth(), src.getframerate(), src.getcomptype()) != (1, 2, RATE, "NONE"):
            raise ValueError(f"Unexpected WAV format: {path}")
        if not 1 <= src.getnframes() <= (32 * 1024 * 1024 - 44) // 2:
            raise ValueError(f"Unsupported WAV length: {path}")
        return np.frombuffer(src.readframes(src.getnframes()), dtype="<i2").astype(np.float64) / 32768


def periodic_noise(rng, length, exponent, low=45, high=10000):
    frequencies = np.fft.rfftfreq(length, 1 / RATE)
    spectrum = np.fft.rfft(rng.standard_normal(length))
    weights = np.maximum(frequencies, low) ** (-exponent / 2)
    weights *= (frequencies >= low) & (frequencies <= high)
    weights[0] = 0
    result = np.fft.irfft(spectrum * weights, n=length)
    return result / np.sqrt(np.mean(result * result))


def ambient(kind, seed):
    length = RATE * 60
    rng = np.random.default_rng(seed)
    time = np.arange(length) / RATE
    if kind in ("white", "pink", "brown"):
        return periodic_noise(rng, length, {"white": 0, "pink": 1, "brown": 2}[kind])
    if kind == "rain":
        bed = periodic_noise(rng, length, 0.4, 250, 10000)
        drops = np.zeros(length)
        at = rng.integers(0, length, 9000)
        np.add.at(drops, at, rng.uniform(0.1, 0.5, len(at)))
        kernel = np.exp(-np.arange(480) / 80) * np.sin(np.arange(480) * 0.65)
        patter = np.fft.irfft(np.fft.rfft(drops) * np.fft.rfft(kernel, length), n=length)
        return bed * 0.7 + patter * 0.7
    if kind == "ocean":
        wash = periodic_noise(rng, length, 1.2, 70, 8000)
        tide = 0.15 + 0.85 * ((np.sin(2 * np.pi * time / 10) + 1) / 2) ** 2
        return wash * tide
    if kind == "wind":
        gust = 0.6 + 0.22 * np.sin(2 * np.pi * time / 15) + 0.16 * np.sin(2 * np.pi * time / 6)
        return periodic_noise(rng, length, 1.8, 55, 5000) * gust
    if kind == "stream":
        bed = periodic_noise(rng, length, 0.8, 200, 9500)
        return bed * (0.8 + 0.12 * np.sin(2 * np.pi * time / 3)) + 0.08 * np.sin(2 * np.pi * 570 * time + 3 * np.sin(2 * np.pi * time / 2))
    if kind == "fire":
        bed = periodic_noise(rng, length, 1.6, 55, 8000) * 0.55
        impulses = np.zeros(length)
        at = rng.integers(0, length, 450)
        np.add.at(impulses, at, rng.uniform(-1, 1, len(at)))
        kernel = np.exp(-np.arange(900) / 130) * np.sin(np.arange(900) * 0.83)
        return bed + np.fft.irfft(np.fft.rfft(impulses) * np.fft.rfft(kernel, length), n=length) * 1.2
    raise ValueError(kind)


def download_music(item, target, cache):
    url = "https://incompetech.com/music/royalty-free/mp3-royaltyfree/" + urllib.parse.quote(item["source_file"])
    mp3 = cache / item["source_file"]
    if not mp3.exists():
        request = urllib.request.Request(url, headers={"User-Agent": "VoxConductor-Media-Pack/1.0"})
        with urllib.request.urlopen(request, timeout=45) as src, mp3.open("wb") as out:
            total = 0
            while chunk := src.read(64 * 1024):
                total += len(chunk)
                if total > 32 * 1024 * 1024:
                    raise ValueError("Source download exceeds 32 MiB")
                out.write(chunk)
    converted = cache / (target.stem + ".wav")
    subprocess.run(["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@24000", "-c", "1", str(mp3), str(converted)], check=True)
    samples = read_wav(converted)
    # Keep full-length pieces and dynamics, only normalize overall gain.
    ramp = min(RATE // 50, len(samples) // 2)
    samples[:ramp] *= np.linspace(0, 1, ramp)
    samples[-ramp:] *= np.linspace(1, 0, ramp)
    write_wav(target, samples, rms=0.105, peak=0.72)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download-music", action="store_true")
    parser.add_argument("--generated-images", type=Path, required=True)
    args = parser.parse_args()
    catalog = json.loads((PACK / "catalog.json").read_text())
    audio = PACK / "sdcard/media/audio"
    images = PACK / "sdcard/media/images"
    previews = PACK / "previews"
    for directory in (audio, images, previews):
        directory.mkdir(parents=True, exist_ok=True)
    for item in catalog["wallpapers"]:
        source = args.generated_images / item["source_image"]
        shutil.copy2(source, previews / (Path(item["file"]).stem + ".png"))
        # RGB565 has no alpha: empty pixels must be true black for the prism.
        rgba = Image.open(source).convert("RGBA")
        black = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        image = Image.alpha_composite(black, rgba).convert("RGB")
        image = ImageOps.fit(image, (240, 240), method=Image.Resampling.LANCZOS)
        pixels = np.array(image, dtype=np.uint16)
        pixels[np.max(pixels, axis=2) < 32] = 0
        rgb = ((pixels[:, :, 0] >> 3) << 11) | ((pixels[:, :, 1] >> 2) << 5) | (pixels[:, :, 2] >> 3)
        (images / item["file"]).write_bytes(rgb.astype("<u2").tobytes())
    for i, item in enumerate(catalog["ambient"]):
        target = audio / item["file"]
        if not target.exists():
            write_wav(target, ambient(item["kind"], 700 + i), rms=0.085, peak=0.55)
        print(f"Ambient ready: {item['file']}", flush=True)
    if args.download_music:
        cache = Path(tempfile.mkdtemp(prefix="vox-music-"))
        try:
            for item in catalog["music"]:
                target = audio / item["file"]
                if target.exists():
                    read_wav(target)
                else:
                    download_music(item, target, cache)
                print(f"Music ready: {item['title']}", flush=True)
        finally:
            shutil.rmtree(cache)
    credits = ["VoxConductor SD Media Pack", "", "Music attribution (retain this file when redistributing):"]
    for item in catalog["music"]:
        credits += [f'"{item["title"]}" Kevin MacLeod (incompetech.com)', "Licensed under Creative Commons: By Attribution 4.0 License", "https://creativecommons.org/licenses/by/4.0/", f'Source: https://incompetech.com/music/royalty-free/index.html?isrc={item["isrc"]}', "Changes: full-length MP3 converted to 24 kHz mono 16-bit PCM WAV; overall gain normalized; short edge fades.", ""]
    credits += ["Environment sounds: original procedural synthesis.", "Wallpapers: generated for this project using built-in imagegen. Prompts: catalog.json.", "", "INSTALL: copy the contents of sdcard/ to your SD root, preserving existing unrelated files. Do not format the SD. Power down the device before removing the card. The result must be /media/audio and /media/images on the card.", "Files are loaded from SD; preserve existing project audio, image, voice and task files."]
    (PACK / "ATTRIBUTION.txt").write_text("\n".join(credits) + "\n")
    records = []
    for path in sorted(list(audio.glob("*")) + list(images.glob("*"))):
        if path.is_file():
            records.append({"file": str(path.relative_to(PACK)), "bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    (PACK / "checksums.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"Ready: {len(records)} media files, {sum(x['bytes'] for x in records) / 1048576:.1f} MiB; {PACK}")


if __name__ == "__main__":
    main()
