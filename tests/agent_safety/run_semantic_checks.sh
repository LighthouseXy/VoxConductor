#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=${1:-$(CDPATH= cd -- "$here/../.." && pwd)}
if [ "$#" -gt 0 ]; then shift; fi
out=$(mktemp -d "${TMPDIR:-/tmp}/ambient-device-contracts.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
cjson="$source_root/firmware/managed_components/espressif__cjson/cJSON"
cc -std=c11 -fsanitize=address,undefined -g -Wno-deprecated-declarations \
  -I"$cjson" -c "$cjson/cJSON.c" -o "$out/cJSON.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g \
  -DBASELINE_TEST_PATH="\"$source_root/tests/test_agent.c\"" \
  -I"$source_root/tests/stubs" -I"$source_root/firmware/main" -I"$cjson" \
  "$here/test_agent_semantics.c" "$source_root/firmware/main/app_agent.c" \
  "$source_root/firmware/main/app_weather.c" \
  "$source_root/firmware/main/app_conversation.c" "$out/cJSON.o" -lm \
  -o "$out/test_device_contracts"
"$out/test_device_contracts" "$@"
