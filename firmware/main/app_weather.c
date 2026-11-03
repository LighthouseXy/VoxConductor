/*
 * 天气解析/时间窗口：区分实况、预报、UV和US AQI。
 * 可用性、数值和预报来源分别记录；缺项标记无效。
 */
#include "app_weather.h"

#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const char *app_weather_condition(int code) {
  switch (code) {
  case 0:
    return "晴";
  case 1:
  case 2:
    return "多云";
  case 3:
    return "阴";
  case 45:
  case 48:
    return "雾";
  case 51:
  case 53:
  case 55:
  case 56:
  case 57:
  case 61:
  case 66:
  case 80:
    return "小雨";
  case 63:
  case 67:
  case 81:
    return "中雨";
  case 65:
  case 82:
    return "大雨";
  case 71:
  case 77:
  case 85:
    return "小雪";
  case 73:
  case 86:
    return "中雪";
  case 75:
    return "大雪";
  case 95:
  case 96:
  case 99:
    return "雷雨";
  default:
    return "暂无数据";
  }
}

static bool valid_temperature(const cJSON *value) {
  return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
         value->valuedouble >= -150 && value->valuedouble <= 100;
}

static bool valid_time(const char *text) {
  if (!text || strlen(text) != 16)
    return false;
  for (int i = 0; i < 16; ++i) {
    char separator = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 ? ':' : 0;
    if (separator ? text[i] != separator : !isdigit((unsigned char)text[i]))
      return false;
  }
  int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
             (text[2] - '0') * 10 + text[3] - '0';
  int month = (text[5] - '0') * 10 + text[6] - '0';
  int day = (text[8] - '0') * 10 + text[9] - '0';
  int hour = (text[11] - '0') * 10 + text[12] - '0';
  int minute = (text[14] - '0') * 10 + text[15] - '0';
  if (year < 1970 || month < 1 || month > 12)
    return false;
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int limit = days[month - 1] + (month == 2 && year % 4 == 0 &&
                                 (year % 100 != 0 || year % 400 == 0));
  return day >= 1 && day <= limit && hour < 24 && minute < 60;
}

static bool in_range(const cJSON *value, double min, double max) {
  return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
         value->valuedouble >= min && value->valuedouble <= max;
}
static bool unit_is(const cJSON *units, const char *key, const char *expected) {
  const cJSON *unit = cJSON_GetObjectItemCaseSensitive(units, key);
  return cJSON_IsString(unit) && !strcmp(unit->valuestring, expected);
}

bool app_weather_parse(const char *json, app_weather_t *out) {
  if (!json || !out)
    return false;
  cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
  const cJSON *temperature =
      cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
  const cJSON *apparent =
      cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature");
  const cJSON *humidity =
      cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
  const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
  const cJSON *time = cJSON_GetObjectItemCaseSensitive(current, "time");
  const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "current_units");
  const cJSON *current_uv = cJSON_GetObjectItemCaseSensitive(current, "uv_index");
  const cJSON *wind = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
  const cJSON *direction = cJSON_GetObjectItemCaseSensitive(current, "wind_direction_10m");
  const cJSON *gust = cJSON_GetObjectItemCaseSensitive(current, "wind_gusts_10m");
  bool wind_available = in_range(wind, 0, 500) && in_range(gust, 0, 500) &&
                        in_range(direction, 0, 360) &&
                        unit_is(units, "wind_speed_10m", "km/h") &&
                        unit_is(units, "wind_gusts_10m", "km/h") &&
                        unit_is(units, "wind_direction_10m", "°");
  const cJSON *temperature_unit =
      cJSON_GetObjectItemCaseSensitive(units, "temperature_2m");
  const cJSON *apparent_unit =
      cJSON_GetObjectItemCaseSensitive(units, "apparent_temperature");
  const cJSON *humidity_unit =
      cJSON_GetObjectItemCaseSensitive(units, "relative_humidity_2m");
  bool humidity_valid =
      !humidity ||
      (cJSON_IsNumber(humidity) && isfinite(humidity->valuedouble) &&
       humidity->valuedouble >= 0 && humidity->valuedouble <= 100 &&
       cJSON_IsString(humidity_unit) &&
       !strcmp(humidity_unit->valuestring, "%"));
  bool valid = humidity_valid && valid_temperature(temperature) &&
               valid_temperature(apparent) && cJSON_IsNumber(code) &&
               isfinite(code->valuedouble) && code->valuedouble >= 0 &&
               code->valuedouble <= 99 && code->valuedouble == code->valueint &&
               cJSON_IsString(time) && valid_time(time->valuestring) &&
               cJSON_IsString(temperature_unit) &&
               strcmp(temperature_unit->valuestring, "°C") == 0 &&
               cJSON_IsString(apparent_unit) &&
               strcmp(apparent_unit->valuestring, "°C") == 0;
  // Validate the full forecast before mutating the caller's last-good cache.
  const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
  const cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time");
  const cJSON *temps =
      cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
  const cJSON *codes = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
  const cJSON *rain =
      cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
  const cJSON *uv = cJSON_GetObjectItemCaseSensitive(hourly, "uv_index");
  int hours = cJSON_GetArraySize(times);
  if (hourly) {
    const cJSON *hu = cJSON_GetObjectItemCaseSensitive(root, "hourly_units");
    const cJSON *tu = cJSON_GetObjectItemCaseSensitive(hu, "temperature_2m");
    valid = valid && cJSON_IsObject(hourly) && cJSON_IsArray(times) &&
            hours > 0 && hours <= APP_WEATHER_HOURS && cJSON_IsArray(temps) &&
            cJSON_IsArray(codes) && cJSON_IsArray(rain) && cJSON_IsArray(uv) &&
            cJSON_GetArraySize(temps) == hours &&
            cJSON_GetArraySize(codes) == hours &&
            cJSON_GetArraySize(rain) == hours &&
            cJSON_GetArraySize(uv) == hours && cJSON_IsString(tu) &&
            !strcmp(tu->valuestring, "°C");
    for (int i = 0; valid && i < hours; ++i) {
      const cJSON *t = cJSON_GetArrayItem(times, i),
                  *c = cJSON_GetArrayItem(codes, i);
      const cJSON *r = cJSON_GetArrayItem(rain, i),
                  *u = cJSON_GetArrayItem(uv, i);
      valid = cJSON_IsString(t) && valid_time(t->valuestring) &&
              !strcmp(t->valuestring + 14, "00") &&
              (!i || strcmp(cJSON_GetArrayItem(times, i - 1)->valuestring,
                            t->valuestring) < 0) &&
              valid_temperature(cJSON_GetArrayItem(temps, i)) &&
              cJSON_IsNumber(c) && isfinite(c->valuedouble) &&
              c->valuedouble >= 0 && c->valuedouble <= 99 &&
              c->valuedouble == c->valueint && cJSON_IsNumber(r) &&
              isfinite(r->valuedouble) && r->valuedouble >= 0 &&
              r->valuedouble <= 100 && cJSON_IsNumber(u) &&
              isfinite(u->valuedouble) && u->valuedouble >= 0 &&
              u->valuedouble <= 50;
    }
  }
  const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
  const cJSON *days = cJSON_GetObjectItemCaseSensitive(daily, "time");
  const cJSON *maxima = cJSON_GetObjectItemCaseSensitive(daily, "uv_index_max");
  int uv_at = -1;
  if (daily) {
    int count = cJSON_GetArraySize(days);
    valid = valid && cJSON_IsArray(days) && cJSON_IsArray(maxima) &&
            count > 0 && count <= 2 && cJSON_GetArraySize(maxima) == count;
    for (int i = 0; valid && i < count; ++i) {
      const cJSON *d = cJSON_GetArrayItem(days, i),
                  *m = cJSON_GetArrayItem(maxima, i);
      char check[17];
      if (!cJSON_IsString(d) || strlen(d->valuestring) != 10) {
        valid = false;
        break;
      }
      snprintf(check, sizeof(check), "%.10sT00:00", d->valuestring);
      valid = valid_time(check) && cJSON_IsNumber(m) &&
              isfinite(m->valuedouble) && m->valuedouble >= 0 &&
              m->valuedouble <= 50;
      if (valid && !strncmp(d->valuestring, time->valuestring, 10))
        uv_at = i;
    }
  }
  if (valid) {
    out->temperature_c = (int)lround(temperature->valuedouble);
    out->apparent_c = (int)lround(apparent->valuedouble);
    out->humidity_percent = humidity ? (int)lround(humidity->valuedouble) : -1;
    out->code = code->valueint;
    out->condition = app_weather_condition(out->code);
    memcpy(out->updated_at, time->valuestring, sizeof(out->updated_at));
    out->hour_count = hourly ? (unsigned)hours : 0;
    memset(out->hours, 0, sizeof(out->hours));
    for (unsigned i = 0; i < out->hour_count; ++i) {
      app_weather_hour_t *h = &out->hours[i];
      memcpy(h->time, cJSON_GetArrayItem(times, i)->valuestring,
             sizeof(h->time));
      h->temperature_c = (int)lround(cJSON_GetArrayItem(temps, i)->valuedouble);
      h->code = cJSON_GetArrayItem(codes, i)->valueint;
      h->rain_percent = (int)lround(cJSON_GetArrayItem(rain, i)->valuedouble);
      h->uv_index = (float)cJSON_GetArrayItem(uv, i)->valuedouble;
    }
    out->uv_available = uv_at >= 0;
    out->current_uv_available = in_range(current_uv, 0, 50);
    out->current_uv = out->current_uv_available ? (float)current_uv->valuedouble : 0;
    out->wind_available = wind_available;
    out->wind_kmh = wind_available ? (float)wind->valuedouble : 0;
    out->gust_kmh = wind_available ? (float)gust->valuedouble : 0;
    out->wind_degrees = wind_available ? (int)lround(direction->valuedouble) % 360 : 0;
    out->air = (app_air_quality_t){0};
    out->air_cached = false;
    out->uv_day[0] = 0;
    out->uv_max = 0;
    if (uv_at >= 0) {
      memcpy(out->uv_day, cJSON_GetArrayItem(days, uv_at)->valuestring,
             sizeof(out->uv_day));
      out->uv_max = (float)cJSON_GetArrayItem(maxima, uv_at)->valuedouble;
    }
  }
  cJSON_Delete(root);
  return valid;
}

bool app_weather_parse_air(const char *json, app_air_quality_t *out) {
  if (!json || !out) return false;
  cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
  const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "current_units");
  const cJSON *aqi = cJSON_GetObjectItemCaseSensitive(current, "us_aqi");
  const cJSON *pm = cJSON_GetObjectItemCaseSensitive(current, "pm2_5");
  const cJSON *time = cJSON_GetObjectItemCaseSensitive(current, "time");
  bool valid = in_range(aqi, 0, 1000) && aqi->valuedouble == aqi->valueint &&
               in_range(pm, 0, 10000) && cJSON_IsString(time) &&
               valid_time(time->valuestring) && unit_is(units, "us_aqi", "USAQI") &&
               unit_is(units, "pm2_5", "μg/m³");
  if (valid) {
    out->available = true;
    out->us_aqi = aqi->valueint;
    out->pm25 = (float)pm->valuedouble;
    memcpy(out->updated_at, time->valuestring, sizeof(out->updated_at));
  }
  cJSON_Delete(root);
  return valid;
}
unsigned app_weather_wind_level(float kmh) {
  static const float limits[] = {1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
  unsigned level = 0;
  while (level < sizeof(limits) / sizeof(*limits) && kmh >= limits[level]) ++level;
  return level;
}
const char *app_weather_wind_direction(int degrees) {
  static const char *names[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
  return degrees >= 0 && degrees < 360 ? names[((degrees + 22) / 45) % 8] : "--";
}
unsigned app_weather_future_hours(const app_weather_t *weather, const char *now,
                                  unsigned indices[24]) {
  if (!weather || !now || !indices || !valid_time(now))
    return 0;
  // The endpoint and device TZ both use Beijing time (no daylight saving).
  // Calendar conversion also handles month/year boundaries for the 24h bound.
  struct tm t = {0};
  if (sscanf(now, "%d-%d-%dT%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min) != 5)
    return 0;
  t.tm_year -= 1900;
  t.tm_mon--;
  t.tm_isdst = -1;
  time_t end = mktime(&t) + 86400;
  struct tm tomorrow;
  char limit[17];
  if (!localtime_r(&end, &tomorrow) ||
      !strftime(limit, sizeof(limit), "%Y-%m-%dT%H:%M", &tomorrow))
    return 0;
  unsigned count = 0;
  for (unsigned i = 0;
       i < weather->hour_count && i < APP_WEATHER_HOURS && count < 24; ++i)
    if (strcmp(weather->hours[i].time, now) >= 0 &&
        strcmp(weather->hours[i].time, limit) < 0)
      indices[count++] = i;
  return count;
}

const char *app_weather_uv_level(float uv) {
  return uv < 3    ? "低"
         : uv < 6  ? "中等"
         : uv < 8  ? "高"
         : uv < 11 ? "很高"
                   : "极高";
}

void app_weather_uv_periods(const app_weather_t *weather, const char *day,
                            char *out, size_t bytes) {
  if (!out || !bytes)
    return;
  out[0] = 0;
  if (!weather || !day || strlen(day) != 10)
    return;
  unsigned seen = 0;
  for (unsigned i = 0; i < weather->hour_count && i < APP_WEATHER_HOURS; ++i) {
    const app_weather_hour_t *h = &weather->hours[i];
    if (!strncmp(h->time, day, 10) && valid_time(h->time)) {
      unsigned hour = (unsigned)(h->time[11] - '0') * 10 + h->time[12] - '0';
      seen |= 1u << hour;
    }
  }
  if (seen != 0xFFFFFFu) {
    snprintf(out, bytes, "逐时数据不完整");
    return;
  }
  int start = -1, last = -1;
  for (unsigned i = 0; i <= weather->hour_count && i <= APP_WEATHER_HOURS;
       ++i) {
    const app_weather_hour_t *h =
        i < weather->hour_count && i < APP_WEATHER_HOURS ? &weather->hours[i]
                                                         : NULL;
    int hour = h && !strncmp(h->time, day, 10)
                   ? (h->time[11] - '0') * 10 + h->time[12] - '0'
                   : -1;
    bool high = hour >= 0 && h->uv_index > 3.0f;
    if (start >= 0 && (!high || hour != last + 1)) {
      size_t used = strlen(out);
      snprintf(out + used, bytes - used, "%s%02d:00-%02d:00", used ? "\n" : "",
               start, last + 1);
      start = -1;
    }
    if (high) {
      if (start < 0)
        start = hour;
      last = hour;
    }
  }
  if (!*out)
    snprintf(out, bytes, "%s",
             weather->hour_count ? "无大于3的时段" : "逐时数据不可用");
}
