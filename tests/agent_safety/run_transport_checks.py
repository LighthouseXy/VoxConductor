#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
main = root / 'firmware/main'
cjson = root / 'firmware/managed_components/espressif__cjson/cJSON'
flags = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-g']
include = ['-I' + str(root / 'tests/http_stubs'), '-I' + str(root / 'tests/tts_stubs'),
           '-I' + str(root / 'tests/speech_stubs'), '-I' + str(root / 'tests/stubs'),
           '-I' + str(main), '-I' + str(cjson)]
def run(args):
    subprocess.run([str(s) for s in args], check=True)

with tempfile.TemporaryDirectory(prefix='vox-transport-') as temp:
    out = Path(temp)
    text = (root / 'tests/test_cloud_http.c').read_text()
    marker = '  puts("PASS: HTTP reuse'
    start = text.index(marker)
    extra = '''  reset(); c = NULL; now_us = 1000;
  assert(app_cloud_http_open_until(URL, "test-key", 5, 2000, NULL, &c, now_us) == ESP_ERR_TIMEOUT);
  assert(!c && open_count == 0);
  assert(open_client(&c) == ESP_OK);
  assert(app_cloud_http_write_until(c, "hello", 5, NULL, now_us) == ESP_ERR_TIMEOUT && write_count == 0);
  assert(app_cloud_http_headers_until(c, NULL, &status, now_us) == ESP_ERR_TIMEOUT && header_count == 0);
  header_waits = 100;
  int64_t until = now_us + 1500000;
  assert(app_cloud_http_headers_until(c, NULL, &status, until) == ESP_ERR_TIMEOUT);
  assert(now_us <= until + 10000 && header_count <= 2);
  esp_http_client_cleanup(c);
  puts("PASS shared HTTP budget rejects expired opens/writes/headers and caps waiting");
'''
    (out / 'http.c').write_text(text[:start] + extra + text[start:])
    run(flags + include + [out / 'http.c', main / 'app_cloud_http.c', '-o', out / 'http'])
    run([out / 'http'])
    (out / 'device_registry.h').write_text('static const app_device_t APP_DEVICES[] = {{"light", "http://device.invalid", "synthetic-token-0123456789"}};\n')
    run(flags + ['-Wno-deprecated-declarations'] + include + ['-c', cjson / 'cJSON.c', '-o', out / 'json.o'])
    run(flags + ['-DAPP_DEVICE_HOST_TEST', '-DHTTP_METHOD_GET=0', '-I' + str(out)] + include +
        [here / 'test_device_transport.c', main / 'app_devices.c', out / 'json.o', '-lm', '-o', out / 'device'])
    run([out / 'device'])
    run(flags + ['-DAPP_TTS_HOST_TEST', '-pthread'] + include +
        [root / 'tests/test_tts.c', main / 'app_cloud_tts.c', main / 'app_turn.c', out / 'json.o', '-lm', '-o', out / 'tts'])
    run([out / 'tts'])
print('PASS targeted HTTP/device/TTS checks; temporary binaries cleaned')
