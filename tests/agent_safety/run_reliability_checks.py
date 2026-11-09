#!/usr/bin/env python3
"""Targeted sanitizer tests; no network or real device/storage access."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--commit-only', action='store_true')
parser.add_argument('--receipts-only', action='store_true')
args = parser.parse_args()
here = Path(__file__).resolve().parent
main = root / 'firmware/main'
cjson = root / 'firmware/managed_components/espressif__cjson/cJSON'
flags = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-g', '-I' + str(main), '-I' + str(cjson),
         '-I' + str(root / 'tests/stubs')]

def run(args):
    subprocess.run([str(s) for s in args], check=True)

with tempfile.TemporaryDirectory(prefix='vox-reliability-') as directory:
    out = Path(directory)
    run(flags + ['-Wno-deprecated-declarations', '-c', cjson / 'cJSON.c', '-o', out / 'json.o'])
    common = [main / 'app_conversation.c', main / 'app_turn.c', out / 'json.o', '-lm']
    def check(name, inputs, extra=()):
        run(flags + list(extra) + list(inputs) + common + ['-o', out / name])
        run([out / name])
    if not args.commit_only and not args.receipts_only:
        check('history-baseline', [root / 'tests/test_history_store.c', main / 'app_history_store.c'], ['-DAPP_HISTORY_HOST_TEST'])
        check('agent-baseline', [root / 'tests/test_agent.c', main / 'app_agent.c', main / 'app_weather.c'])
    if not args.commit_only:
        check('device-receipts', [here / 'test_agent_receipts.c', main / 'app_agent.c', main / 'app_weather.c'],
              ['-DBASELINE_TEST_PATH="' + str(root / 'tests/test_agent.c') + '"'])
    cloud = (main / 'app_cloud.c').read_text()
    start = cloud.index('esp_err_t app_cloud_commit_turn(')
    end = cloud.index('\n}\n', start) + 3
    interrupted_start = cloud.index('void app_cloud_remember_interrupted(')
    interrupted_end = cloud.index('\n}\n', interrupted_start) + 3
    (out / 'commit_function.inc').write_text(cloud[start:end] + cloud[interrupted_start:interrupted_end])
    if not args.receipts_only:
        check('commit-result', [here / 'test_commit_result.c'], ['-I' + str(out)])
print('PASS reliability checks; temporary binaries cleaned')
