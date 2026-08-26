import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

WEATHER_URL = "https://api.open-meteo.com/v1/forecast"
CACHE_SECONDS = 600

_cached_weather = None
_cached_at = 0.0


def weather_condition(code: int) -> str:
    if code == 0:
        return "晴"
    if code in (1, 2):
        return "多云"
    if code == 3:
        return "阴"
    if code in (45, 48):
        return "雾"
    if code in (51, 53, 55, 56, 57, 61, 66, 80):
        return "小雨"
    if code in (63, 67, 81):
        return "中雨"
    if code in (65, 82):
        return "大雨"
    if code in (71, 77, 85):
        return "小雪"
    if code in (73, 86):
        return "中雪"
    if code == 75:
        return "大雪"
    if code in (95, 96, 99):
        return "雷雨"
    return "暂无数据"


def get_current_weather() -> dict:
    global _cached_weather, _cached_at

    now = time.monotonic()
    if _cached_weather is not None and now - _cached_at < CACHE_SECONDS:
        return _cached_weather

    latitude = os.environ["WEATHER_LATITUDE"]
    longitude = os.environ["WEATHER_LONGITUDE"]

    query = urllib.parse.urlencode(
        {
            "latitude": latitude,
            "longitude": longitude,
            "current": (
                "temperature_2m,"
                "apparent_temperature,"
                "weather_code"
            ),
            "timezone": os.getenv(
                "WEATHER_TIMEZONE",
                "Asia/Shanghai",
            ),
        }
    )

    request = urllib.request.Request(
        f"{WEATHER_URL}?{query}",
        headers={"User-Agent": "VoxConductor/1.0"},
    )

    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            payload = json.load(response)

        current = payload["current"]
        code = int(current["weather_code"])

    except (
        urllib.error.URLError,
        TimeoutError,
        KeyError,
        TypeError,
        ValueError,
        json.JSONDecodeError,
    ) as error:
        raise RuntimeError(f"获取天气失败：{error}") from error

    _cached_weather = {
        "city": os.getenv("WEATHER_CITY", ""),
        "temperature_c": round(float(current["temperature_2m"])),
        "apparent_c": round(float(current["apparent_temperature"])),
        "condition": weather_condition(code),
        "weather_code": code,
        "updated_at": current.get("time", ""),
    }
    _cached_at = now

    return _cached_weather
