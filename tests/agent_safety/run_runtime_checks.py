#!/usr/bin/env python3
"""Offline target contracts execute current production functions with mocks.

Side effects use synthetic local fixtures and a virtual clock.
Physical acceptance and RTOS/device behavior require separate validation."""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)


def extract(text, name):
    masked = TOKEN.sub(lambda m: " " * len(m.group()), text)
    match = re.search(r"(?:static\s+)?(?:void|esp_err_t|uint32_t|cJSON\s*\*)\s*" + name + r"\s*\([^;{}]*\)\s*\{", masked)
    assert match, name
    start = match.start()
    depth = 1
    end = match.end()
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return text[start:end]


def run(command):
    completed = subprocess.run([str(s) for s in command], text=True, capture_output=True)
    print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="")
    if completed.returncode:
        raise SystemExit(completed.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    root = args.source.resolve()
    main_dir = root / "firmware/main"
    cjson = root / "firmware/managed_components/espressif__cjson/cJSON"
    inputs = {name: (main_dir / name).read_text() for name in ("main.c", "app_control.c", "app_cloud.c")}
    template = (HERE / "runtime-harness.c.in").read_text()
    for name in ("service_background_boundary", "reminder_task", "voice_conversation_task"):
        template = template.replace("@" + name + "@", extract(inputs["main.c"], name))
    for name in ("app_control_submit", "app_control_status", "app_control_service"):
        template = template.replace("@" + name + "@", extract(inputs["app_control.c"], name))
    template = template.replace("@app_cloud_ask@", extract(inputs["app_cloud.c"], "app_cloud_ask"))
    fixture = (root / "tests/test_agent.c").read_text().split("static void contains(", 1)[0]
    template = template.replace("@FIXTURE_STUBS@", fixture)
    tracked = [main_dir / name for name in inputs]
    tracked += [main_dir / name for name in ("app_agent.c", "app_turn.c", "app_conversation.c", "app_weather.c")]
    tracked += [root / "tests/test_agent.c"]
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    for path, digest in before.items():
        print(f"SHA256 {digest} {path}", flush=True)
    with tempfile.TemporaryDirectory(prefix="ambient-scheduler-") as out:
        out = Path(out)
        generated = out / "harness.c"
        generated.write_text(template)
        flags = ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g"]
        run(flags + ["-Wno-deprecated-declarations", "-I" + str(cjson), "-c", cjson / "cJSON.c", "-o", out / "cJSON.o"])
        include = ["-I" + str(root / "tests/stubs"), "-I" + str(main_dir), "-I" + str(cjson)]
        sources = [main_dir / name for name in ("app_agent.c", "app_conversation.c", "app_weather.c", "app_turn.c")]
        run(flags + include + [generated] + sources + [out / "cJSON.o", "-lm", "-o", out / "scheduler"])
        run([out / "scheduler"])
    after = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    assert before == after, "source changed during execution; evidence must be rerun"
    print("PASS: tested production sources unchanged; temporary binaries cleaned")


if __name__ == "__main__":
    main()
