#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# Source/asset checks and browser mocks are offline by default; never pass --live.
# Keep one entry point so configuration migrations and web regressions aren't skipped.
for check in "$root"/tests/test_*.py; do
  python3 "$check"
done
out=$(mktemp -d "${TMPDIR:-/tmp}/vox-cloud-tests.XXXXXX")
cjson="$root/firmware/managed_components/espressif__cjson/cJSON"
trap 'rm -rf "$out"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/tests/http_stubs" -I"$root/tests/tts_stubs" -I"$root/tests/speech_stubs" \
  -I"$root/tests/stubs" -I"$root/firmware/main" \
  "$root/tests/test_cloud_http.c" "$root/firmware/main/app_cloud_http.c" -o "$out/test_cloud_http"
"$out/test_cloud_http"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" "$root/tests/test_focus.c" \
  "$root/firmware/main/app_focus.c" -lm -o "$out/test_focus"
"$out/test_focus"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" "$root/tests/test_words.c" \
  "$root/firmware/main/app_words.c" -o "$out/test_words"
"$out/test_words"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" "$root/tests/test_games.c" \
  "$root/firmware/main/app_games_core.c" -lm -o "$out/test_games"
"$out/test_games"
cc -std=c11 -fsanitize=address,undefined -g -Wno-deprecated-declarations \
  -I"$cjson" -c "$cjson/cJSON.c" -o "$out/cJSON.o"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_cloud_protocol.c" \
  "$root/firmware/main/app_conversation.c" "$out/cJSON.o" \
  -o "$out/test_cloud_protocol"
"$out/test_cloud_protocol"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_weather.c" "$root/firmware/main/app_weather.c" \
  "$out/cJSON.o" -lm -o "$out/test_weather"
"$out/test_weather" "$@"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" "$root/tests/test_motion.c" -o "$out/test_motion"
"$out/test_motion"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_turn.c" "$root/firmware/main/app_turn.c" "$out/cJSON.o" \
  -lm -o "$out/test_turn"
"$out/test_turn"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -DAPP_HISTORY_HOST_TEST -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_history_store.c" "$root/firmware/main/app_history_store.c" \
  "$root/firmware/main/app_conversation.c" "$root/firmware/main/app_turn.c" \
  "$out/cJSON.o" -lm -o "$out/test_history_store"
"$out/test_history_store"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/firmware/main" -I"$cjson" "$root/tests/test_sse.c" \
  "$root/firmware/main/app_sse.c" "$out/cJSON.o" -lm -o "$out/test_sse"
"$out/test_sse"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread \
  -I"$root/tests/speech_stubs" -I"$root/tests/stubs" -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_speech.c" "$root/firmware/main/app_speech.c" \
  "$root/firmware/main/app_turn.c" "$out/cJSON.o" -lm -o "$out/test_speech"
"$out/test_speech"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -DAPP_TTS_HOST_TEST -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread \
  -I"$root/tests/tts_stubs" -I"$root/tests/speech_stubs" -I"$root/tests/stubs" -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_tts.c" "$root/firmware/main/app_cloud_tts.c" \
  "$root/firmware/main/app_turn.c" "$out/cJSON.o" -lm -o "$out/test_tts"
"$out/test_tts"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/tests/stubs" -I"$root/firmware/main" -I"$cjson" \
  "$root/tests/test_agent.c" "$root/firmware/main/app_agent.c" \
  "$root/firmware/main/app_weather.c" \
  "$root/firmware/main/app_conversation.c" "$out/cJSON.o" -lm -o "$out/test_agent"
"$out/test_agent"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$root/tests/button_stubs" -I"$root/firmware/main" \
  "$root/tests/test_button.c" -o "$out/test_button"
"$out/test_button"
