"""No compiler or device access: structural guards, not C/runtime validation."""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = ["app_cloud_tts.c", "app_cloud.c", "app_conversation.c", "app_agent.c",
         "app_history_store.c", "app_speech.c", "app_storage.c", "app_sse.c"]
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)


def masked(text):
    return TOKEN.sub(lambda m: " " * len(m.group()), text)


def body(text, name):
    code = masked(text)
    match = re.search(r"\b" + name + r"\s*\([^;{}]*\)\s*\{", code)
    assert match, name
    start = match.end() - 1
    depth = 0
    for end in range(start, len(code)):
        if code[end] == "{":
            depth += 1
        elif code[end] == "}":
            depth -= 1
            if not depth:
                return text[start:end + 1]
    raise AssertionError(name)


def check_delimiters(path):
    stack = []
    for c in masked(path.read_text()):
        if c in "({[":
            stack.append(c)
        elif c in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[c], path.name
    assert not stack, path.name


for name in FILES:
    check_delimiters(ROOT / "firmware/main" / name)
for name in ["test_tts.c", "test_speech.c", "test_cloud_protocol.c", "test_history_store.c", "test_agent.c", "test_sse.c"]:
    check_delimiters(ROOT / "tests" / name)

tts = (ROOT / "firmware/main/app_cloud_tts.c").read_text()
sender, playback = body(tts, "tts_input_task"), body(tts, "speak")
assert "input_text_buffer.append" in sender and "session.finish" in sender
assert '"input_text_buffer.append"' not in playback and '"session.finish"' not in playback
assert playback.index("while (!atomic_load(&stream->input_done))") < playback.index("esp_websocket_client_stop(client)")
assert "#define TTS_SESSION_LIMIT_US 360000000" in tts and "last_audio_us > 30000000" in playback
assert "CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y" in (ROOT / "firmware/sdkconfig").read_text()
assert "capacity - WS_MESSAGE_LIMIT : capacity / 2" in playback
assert "available < prebuffer_bytes && !finished" in playback
assert "#define TTS_OUTPUT_PRIORITY 6" in tts
assert playback.index("vTaskPrioritySet(NULL, TTS_OUTPUT_PRIORITY)") < playback.index("app_audio_play_mono_pcm(block")
assert "if (output_boosted) vTaskPrioritySet(NULL, output_priority);" in playback.split("cleanup:", 1)[1]
assert "playback_diag: feed_gap_max=" in playback
assert "write_start - last_write_end" in playback
assert "available < min_pcm_available" in playback
reminder = body(tts, "app_cloud_speak_reminder")
assert reminder.index("vTaskPrioritySet(NULL, TTS_PLAY_PRIORITY)") < reminder.index("speak(text") < reminder.index("vTaskPrioritySet(NULL, priority)")

speech = (ROOT / "firmware/main/app_speech.c").read_text()
assert "char text[4096]" in speech and "_Atomic size_t published, consumed" in speech
assert "% sizeof(s->text)" in body(speech, "feed_text")
assert "APP_REPLY_TEXT_BYTES" in body(speech, "app_speech_append")

history = (ROOT / "firmware/main/app_history_store.c").read_text()
forget = body(history, "history_forget")
assert "erase.pending" in forget and "dialogue.rewrite" in forget
assert forget.index("app_history_save(directory, replacement)") < forget.index("close_synced(output, ok)") < forget.index("rename(temporary, path)")
assert "history_forget(directory, next, notes, true)" in body(history, "app_history_retry_forget")
assert "cJSON_Delete(record); ok = false; break;" in forget
assert "continue" not in forget[forget.index("if (!cJSON_IsString(source)"):forget.index("if (strcmp(current")]
assert "INDEX_SKIP_LINE" in body(history, "app_history_index_tick")
assert "text_total_bytes" in body(history, "history_event_chunk")

conversation = (ROOT / "firmware/main/app_conversation.c").read_text()
assert "new_goal && (comparison ||" in body(conversation, "update_discussion")
assert "pass == 0" in body(conversation, "app_conversation_messages")
assert "cJSON_GetArraySize(candidate) != 5" in body(conversation, "app_conversation_refine_discussion")
cloud = (ROOT / "firmware/main/app_cloud.c").read_text()
assert "strcmp(s_history->discussion, job->discussion)" in body(cloud, "app_cloud_memory_tick")
assert "app_conversation_refine_discussion" in body(cloud, "app_cloud_memory_tick")
ask = body(cloud, "app_cloud_ask")
assert "round < 3" in ask and "round < 2 && tool_count < 4" in ask
assert "tool_mask &= ~APP_TOOL_WEATHER" not in ask and "tool_mask &= ~APP_TOOL_DEVICE;" not in ask
agent = (ROOT / "firmware/main/app_agent.c").read_text()
assert "app_agent_request_matches(args, user_text)" in body(agent, "app_agent_execute")
assert "task_request" not in ask and '"tool_choice", "auto"' in ask
assert "from_utc" in body(agent, "app_agent_accept_call")

# Exercise the actual export tool only on generated temporary data.
with tempfile.TemporaryDirectory(prefix="vox-voice-export-") as folder:
    path = Path(folder) / "dialogue.jsonl"
    row = {"turn_id": "synthetic-1", "event": "助手文本", "text": "长回答测试", "text_offset": 2047, "text_total_bytes": 5000}
    path.write_text(json.dumps(row, ensure_ascii=False) + "\n{broken}\n", encoding="utf-8")
    result = subprocess.run([sys.executable, str(ROOT / "tools/export_dialogue.py"), str(path)], check=True, capture_output=True, text=True)
    assert json.loads(result.stdout) == row
print("PASS: voice structural guards and synthetic export; C tests/build/hardware NOT run")
