"""Source/schema checks with optional real-model routing evaluation.

Default scope: offline source and schema checks. --live calls the configured
DeepSeek API with synthetic state/questions, incurs normal API usage and inspects
proposed calls. Firmware execution, SD writes and microphone/UI acceptance
require separate tests. Native builds validate C schema construction."""
import argparse
import concurrent.futures
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
STRINGS = re.compile(r'"(?:\\.|[^"\\])*"')
PARSES = re.compile(r'cJSON_Parse\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', re.S)
WRITE = {"prepare_memory", "prepare_task", "prepare_task_change", "prepare_device",
         "set_local_setting", "control_media", "control_desktop", "manage_anniversary"}


def literals(text):
    return "".join(json.loads(m[0]) for m in STRINGS.finditer(text))


def schemas(agent, control):
    values = [json.loads(literals(m[1])) for m in PARSES.finditer(agent + control)]
    tools = {}
    for value in values:
        for tool in value if isinstance(value, list) else [value]:
            if isinstance(tool, dict) and tool.get("type") == "function":
                tools[tool["function"]["name"]] = tool
    assert len(tools) == 14
    replacements = {"weather_function": "get_weather", "history_function": "search_history",
                    "change_function": "prepare_task_change", "function": "prepare_task"}
    for variable, name in replacements.items():
        match = re.search(r'cJSON_ReplaceItemInObjectCaseSensitive\(' + variable +
                          r',\s*"description",\s*cJSON_CreateString\(\s*'
                          r'((?:"(?:\\.|[^"\\])*"\s*)+)\)\)', agent)
        assert match, name
        tools[name]["function"]["description"] = literals(match[1])
    props = lambda name: tools[name]["function"]["parameters"]["properties"]
    for variable, name, field in [("detail_schema", "get_weather", "detail"),
                                  ("cursor_schema", "search_history", "before_offset"),
                                  ("id_schema", "search_history", "record_id"),
                                  ("clock", "prepare_task", "clock"),
                                  ("repeat", "prepare_task", "repeat"),
                                  ("request", "prepare_task", "request")]:
        match = re.search(r'cJSON \*' + variable + r' = cJSON_Parse\(\s*'
                          r'((?:"(?:\\.|[^"\\])*"\s*)+)\)', agent)
        assert match, variable
        props(name)[field] = json.loads(literals(match[1]))
    assert 'cJSON_CreateString("note")' in agent
    props("prepare_task")["kind"]["enum"].append("note")
    props("prepare_task_change")["clock"] = props("prepare_task")["clock"]
    props("search_history").update({
        "from_utc": {"type": "integer", "minimum": 0, "maximum": 4102444800},
        "to_utc": {"type": "integer", "minimum": 0, "maximum": 4102444800},
        "turn_id": {"type": "string", "maxLength": 63},
        "role": {"type": "string", "enum": ["user", "assistant"]},
    })
    for name in WRITE:
        params = tools[name]["function"]["parameters"]
        params["properties"]["request"] = props("prepare_task")["request"]
        params["required"].append("request")
    return list(tools.values())


def validate(value, schema):
    kind = schema["type"]
    assert {"object": isinstance(value, dict), "string": isinstance(value, str),
            "integer": isinstance(value, int) and not isinstance(value, bool),
            "boolean": isinstance(value, bool)}[kind], (value, kind)
    if "enum" in schema:
        assert value in schema["enum"]
    if kind == "object":
        assert set(schema.get("required", [])) <= value.keys()
        assert schema["additionalProperties"] is False
        assert value.keys() <= schema["properties"].keys()
        for key, item in value.items():
            validate(item, schema["properties"][key])
    if kind == "string":
        assert schema.get("minLength", 0) <= len(value) <= schema.get("maxLength", 100000)
    if kind == "integer":
        assert schema.get("minimum", -10**12) <= value <= schema.get("maximum", 10**12)


# (current utterance, allowed first-stage tools, expected argument subset, prior dialogue)
CASES = [
    ("紫外线是什么？", set(), {}, []),
    ("闹钟怎么设置？", set(), {}, []),
    ("音乐能不能暂停？", set(), {}, []),
    ("如果关闭语音唤醒会怎样？", set(), {}, []),
    ("不要把音量改到30。", set(), {}, []),
    ("别删我的闹钟。", set(), {}, []),
    ("我不想开启网页管理。", set(), {}, []),
    ("声音太大了。", set(), {}, []),
    ("可以调节屏幕亮度吗？", set(), {}, []),
    ("他说让我把音量改到30，你觉得呢？", set(), {}, []),
    ("你好，给我讲个笑话。", set(), {}, []),
    ("假设我说一分钟后提醒喝水，你会怎么做？", set(), {}, []),
    ("杭州现在紫外线多少？", {"get_weather"}, {"detail": "uv", "city": "杭州"}, []),
    ("现在杭州的空气质量怎么样？", {"get_weather"}, {"detail": "air"}, []),
    ("接下来12小时会不会下雨？", {"get_weather"}, {"detail": "hourly"}, []),
    ("今天风有多大？", {"get_weather"}, {"detail": "wind"}, []),
    ("能不能帮我把音量调到30？", {"set_local_setting"}, {"setting": "volume", "value": 30}, []),
    ("不要太亮，把亮度调到30。", {"set_local_setting"}, {"setting": "brightness", "value": 30}, []),
    ("把扬声器调低些。", {"set_local_setting"}, {"setting": "volume", "value": 50}, []),
    ("再小一点。", {"set_local_setting"}, {"setting": "volume", "value": 50},
     [("把音量调到60", "音量已设置为60。")]),
    ("把它改成30。", set(), {}, []),
    ("半小时后叫我喝水。", {"prepare_task"}, {"kind": "alarm", "delay_seconds": 1800}, []),
    ("明天早上七点半叫醒我。", {"prepare_task"}, {"kind": "alarm", "delay_seconds": 0}, []),
    ("帮我记下，滤芯型号是A15。", {"prepare_task"}, {"kind": "note", "title": "滤芯型号是A15"}, []),
    ("添加备忘录，今天不要喝咖啡。", {"prepare_task"}, {"kind": "note", "title": "今天不要喝咖啡"}, []),
    ("把刚才的闹钟改成每天。", {"get_tasks"}, {},
     [("明天早上七点叫醒我", "已设置明天早上七点的起床闹钟。")]),
    ("帮我查一下以前关于咖啡的原话。", {"search_history"}, {}, []),
    ("播放雨声。", {"get_local_state"}, {"scope": "media"}, []),
    ("暂停正在播放的歌。", {"control_media"}, {"action": "pause"}, []),
    ("把主页换成星空壁纸加数字时间。", {"set_local_setting"}, {}, []),
    ("主页恢复成钟表。", {"set_local_setting"}, {"setting": "home_style", "value": 0}, []),
    ("开启扫码管理。", {"control_desktop"}, {"action": "web_open"}, []),
    ("关掉客厅灯。", set(), {}, []),  # No configured peers in this synthetic snapshot.
    ("删除所有闹钟。", {"get_tasks"}, {}, []),
    ("一分钟后提醒喝水，再把音量调到30。", {"prepare_task", "set_local_setting"}, {}, []),
    ("校准姿态传感器。", {"control_desktop"}, {"action": "calibrate"}, []),
]


def offline():
    agent = (MAIN / "app_agent.c").read_text()
    control = (MAIN / "app_control.c").read_text()
    cloud = (MAIN / "app_cloud.c").read_text()
    conversation = (MAIN / "app_conversation.c").read_text()
    assert "explicit_action" not in control and "WORDS(" not in control
    assert "app_agent_is_reminder_request" not in agent + cloud
    assert "task_request" not in cloud and '"tool_choice", "auto"' in cloud
    assert '"tool_choice", "none"' in cloud and "round < 2 && tool_count < 4" in cloud
    assert "app_agent_tool_mask(void)" in agent and "app_agent_tool_mask()" in cloud
    assert "current_turn_request_required_not_executed" in agent
    assert 'strstr(current_user, request->valuestring)' in agent and "field->next" in agent
    assert "request->valuestring) > 240" in agent and "characters > 80" in agent
    assert "s_change_expected.due_utc" in agent and "if (!unchanged)" in agent
    assert '"%.48s-%08x", turn->id' in agent
    assert "removable_context -= remove" in cloud and "length > 32 * 1024" in cloud
    assert "free(body)" in cloud and "app_agent_runtime_context()" in cloud
    assert "app_device_ids()" in agent and "no SD catalogue, HTTP" in control
    peer = (MAIN / "app_devices.c").read_text().split("cJSON *app_device_ids(void)", 1)[1].split("char *app_device_request", 1)[0]
    assert "d->url" not in peer and "d->token" not in peer and "app_device_configured(d->id)" in peer
    assert "本轮最多4次工具调用" in conversation and "组合请求逐项报告结果" in conversation
    tools = schemas(agent, control)
    for tool in tools:
        fn = tool["function"]
        params = fn["parameters"]
        assert params["additionalProperties"] is False
        assert ("request" in params.get("required", [])) == (fn["name"] in WRITE)
        assert "authorized" not in params["properties"] and "confirmed" not in params["properties"]
    fn = {tool["function"]["name"]: tool["function"] for tool in tools}
    validate({"setting": "volume", "value": 30, "request": "音量调到30"}, fn["set_local_setting"]["parameters"])
    for bad in [{"setting": "volume", "value": 30},
                {"setting": "volume", "value": 30, "request": "", "confirmed": True},
                {"setting": "volume", "value": -1, "request": "调低"}]:
        try:
            validate(bad, fn["set_local_setting"]["parameters"])
        except AssertionError:
            pass
        else:
            raise AssertionError("invalid write schema accepted")
    assert len(CASES) == 36
    print('PASS: semantic routing source guards, 14 reconstructed schemas, provenance/confirmation/budget bounds; scope=offline source/schema checks; 36 model cases await live evaluation')
    return tools, conversation


def live(tools, conversation, credentials, limit):
    config = credentials.read_text()
    def macro(name):
        match = re.search(r'^#define\s+' + name + r'\s+("(?:\\.|[^"\\])*")', config, re.M)
        if not match:
            raise SystemExit("Required API configuration unavailable")
        return json.loads(match[1])
    url, key, model = (macro("CLOUD_DEEPSEEK_" + name) for name in ("URL", "API_KEY", "MODEL"))
    parsed = urllib.parse.urlparse(url)
    assert parsed.scheme == "https" and parsed.hostname == "api.deepseek.com", "Only the configured official DeepSeek endpoint is allowed"
    start = conversation.index("  const char *system =", conversation.index("cJSON *app_conversation_messages"))
    end = conversation.index(";", start)
    system = literals(conversation[start:end])
    state = {"data_type": "device_snapshot_not_user_instruction", "weather_city": "杭州",
             "settings": {"ok": True, "volume": 60, "brightness": 80, "home_style": 0, "wallpaper_mode": 1,
                          "wake_enabled": True, "wake_model_available": True},
             "time_valid": True, "local_time": "2026-10-07 09:00:00", "timezone": "UTC+08:00",
             "external_device_ids": [], "last_saved_task_id": "synthetic-alarm-1"}
    specs = {t["function"]["name"]: t["function"]["parameters"] for t in tools}
    def check(index_case):
        index, (question, allowed, expected, history) = index_case
        messages = [{"role": "system", "content": system}]
        for user, assistant in history:
            messages.extend([{"role": "user", "content": user}, {"role": "assistant", "content": assistant}])
        messages.extend([{"role": "user", "content": json.dumps(state, ensure_ascii=False)},
                         {"role": "user", "content": question}])
        body = json.dumps({"model": model, "messages": messages, "tools": tools,
                           "tool_choice": "auto", "thinking": {"type": "disabled"}, "max_tokens": 1536}).encode()
        request = urllib.request.Request(url, body, {"Authorization": "Bearer " + key, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=45) as response:
                message = json.load(response)["choices"][0]["message"]
        except (urllib.error.URLError, TimeoutError):
            return index, False, "API connection failed; credentials stay private"
        calls = message.get("tool_calls") or []
        try:
            assert bool(calls) == bool(allowed), "unexpected tool/no-tool decision"
            assert len(calls) <= 4
            for call in calls:
                name = call["function"]["name"]
                args = json.loads(call["function"]["arguments"])
                assert name in allowed, "unexpected tool " + name
                validate(args, specs[name])
                if name in WRITE:
                    assert args["request"] in question and args["request"].strip(), "current-turn provenance failed"
                    assert len(json.dumps(args, ensure_ascii=False, separators=(",", ":")).encode()) <= 512
                for field, value in expected.items():
                    assert args.get(field) == value, "argument mismatch " + field
            if index == 29:
                pairs = {(json.loads(c["function"]["arguments"]).get("setting"),
                          json.loads(c["function"]["arguments"]).get("value")) for c in calls}
                assert {("home_style", 1), ("wallpaper_mode", 1)} <= pairs, "home/wallpaper combination incomplete"
            return index, True, ",".join(c["function"]["name"] for c in calls) or "direct/clarify"
        except (AssertionError, KeyError, ValueError) as error:
            return index, False, str(error)
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(check, enumerate(CASES[:limit])))
    for index, ok, detail in results:
        print(f'{"PASS" if ok else "FAIL"}: case {index + 1}: {detail}')
    passed = sum(ok for _, ok, _ in results)
    print(f"Live model proposals: {passed}/{len(results)}; NO device actions executed; model={model}")
    if passed != len(results):
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--limit", type=int, default=36)
    parser.add_argument("--credentials", type=Path, default=MAIN / "cloud_credentials.h")
    args = parser.parse_args()
    tools, conversation = offline()
    if args.live:
        live(tools, conversation, args.credentials, args.limit)
