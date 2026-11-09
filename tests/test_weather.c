#include "app_weather.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *fixture =
    "{\"current_units\":{\"temperature_2m\":\"°C\",\"apparent_temperature\":"
    "\"°C\"},"
    "\"current\":{\"time\":\"2026-09-19T19:30\",\"temperature_2m\":28.2,"
    "\"apparent_temperature\":31.6,\"weather_code\":0}}";

static void forecast_checks(void) {
  cJSON *root = cJSON_Parse(fixture);
  cJSON *hourly = cJSON_AddObjectToObject(root, "hourly");
  cJSON *units = cJSON_AddObjectToObject(root, "hourly_units");
  assert(cJSON_AddStringToObject(units, "temperature_2m", "°C"));
  cJSON *times = cJSON_AddArrayToObject(hourly, "time");
  cJSON *temps = cJSON_AddArrayToObject(hourly, "temperature_2m");
  cJSON *codes = cJSON_AddArrayToObject(hourly, "weather_code");
  cJSON *rain = cJSON_AddArrayToObject(hourly, "precipitation_probability");
  cJSON *uv = cJSON_AddArrayToObject(hourly, "uv_index");
  for (unsigned i = 0; i < 48; ++i) {
    char time[32];
    snprintf(time, sizeof(time), "2026-09-%02uT%02u:00", 19 + i / 24, i % 24);
    assert(cJSON_AddItemToArray(times, cJSON_CreateString(time)));
    assert(cJSON_AddItemToArray(temps, cJSON_CreateNumber(24 + i % 5)));
    assert(cJSON_AddItemToArray(codes, cJSON_CreateNumber(2)));
    assert(cJSON_AddItemToArray(rain, cJSON_CreateNumber(20)));
    unsigned hour = i % 24;
    // Exactly 3 is excluded. Two separated high-UV intervals must survive.
    double value = hour == 9                 ? 3.0
                   : hour >= 10 && hour < 12 ? 4.1
                   : hour >= 14 && hour < 16 ? 7.2
                                             : 0;
    assert(cJSON_AddItemToArray(uv, cJSON_CreateNumber(value)));
  }
  cJSON *daily = cJSON_AddObjectToObject(root, "daily");
  cJSON *days = cJSON_AddArrayToObject(daily, "time");
  cJSON *maxima = cJSON_AddArrayToObject(daily, "uv_index_max");
  assert(cJSON_AddItemToArray(days, cJSON_CreateString("2026-09-19")));
  assert(cJSON_AddItemToArray(days, cJSON_CreateString("2026-09-20")));
  assert(cJSON_AddItemToArray(maxima, cJSON_CreateNumber(7.2)));
  assert(cJSON_AddItemToArray(maxima, cJSON_CreateNumber(7.2)));
  char *json = cJSON_PrintUnformatted(root);
  app_weather_t weather = {0};
  assert(strlen(json) < 8192 && app_weather_parse(json, &weather));
  cJSON_free(json);
  assert(weather.hour_count == 48 && weather.uv_available);
  assert(!strcmp(weather.uv_day, "2026-09-19"));
  unsigned indices[24];
  assert(app_weather_future_hours(&weather, "2026-09-19T19:30", indices) == 24);
  assert(!strcmp(weather.hours[indices[0]].time, "2026-09-19T20:00"));
  assert(!strcmp(weather.hours[indices[23]].time, "2026-09-20T19:00"));
  assert(app_weather_future_hours(&weather, "2026-09-19T00:00", indices) == 24);
  assert(indices[0] == 0 && indices[23] == 23);
  assert(!app_weather_future_hours(&weather, "2026-02-30T00:00", indices));
  char periods[192];
  app_weather_uv_periods(&weather, "2026-09-19", periods, sizeof(periods));
  assert(!strcmp(periods, "10:00-12:00\n14:00-16:00"));
  app_weather_uv_periods(&weather, "2026-09-21", periods, sizeof(periods));
  assert(!strcmp(periods, "逐时数据不完整"));
  assert(!strcmp(app_weather_uv_level(3), "中等"));
  assert(!strcmp(app_weather_uv_level(6), "高"));
  assert(!strcmp(app_weather_uv_level(8), "很高"));
  assert(!strcmp(app_weather_uv_level(11), "极高"));
  app_weather_t saved = weather;
  cJSON_DeleteItemFromArray(rain, 0);
  json = cJSON_PrintUnformatted(root);
  assert(!app_weather_parse(json, &weather));
  assert(!memcmp(&weather, &saved, sizeof(weather)));
  cJSON_free(json);
  cJSON_Delete(root);
}

static void current_parameter_checks(void) {
  cJSON *root = cJSON_Parse(fixture);
  cJSON *current = cJSON_GetObjectItem(root, "current");
  cJSON *units = cJSON_GetObjectItem(root, "current_units");
  cJSON_AddNumberToObject(current, "uv_index", 5.0);
  cJSON_AddNumberToObject(current, "wind_speed_10m", 8.4);
  cJSON_AddNumberToObject(current, "wind_gusts_10m", 22.7);
  cJSON_AddNumberToObject(current, "wind_direction_10m", 341);
  cJSON_AddStringToObject(units, "wind_speed_10m", "km/h");
  cJSON_AddStringToObject(units, "wind_gusts_10m", "km/h");
  cJSON_AddStringToObject(units, "wind_direction_10m", "°");
  char *json = cJSON_PrintUnformatted(root);
  app_weather_t weather = {0};
  assert(app_weather_parse(json, &weather));
  assert(weather.current_uv_available && weather.current_uv == 5);
  assert(weather.wind_available && weather.wind_degrees == 341);
  assert(app_weather_wind_level(weather.wind_kmh) == 2);
  assert(!strcmp(app_weather_wind_direction(341), "北"));
  assert(!strcmp(app_weather_wind_direction(319), "西北"));
  cJSON_free(json);
  cJSON_SetNumberValue(cJSON_GetObjectItem(current, "uv_index"), 0);
  json = cJSON_PrintUnformatted(root);
  assert(app_weather_parse(json, &weather));
  assert(weather.current_uv_available && weather.current_uv == 0); // Night is not missing.
  cJSON_free(json);
  cJSON_ReplaceItemInObject(units, "wind_speed_10m", cJSON_CreateString("mph"));
  json = cJSON_PrintUnformatted(root);
  assert(app_weather_parse(json, &weather) && !weather.wind_available);
  cJSON_free(json);
  cJSON_Delete(root);
  const char *air_json = "{\"current\":{\"time\":\"2026-10-06T09:00\",\"us_aqi\":101,\"pm2_5\":30.4},"
                        "\"current_units\":{\"us_aqi\":\"USAQI\",\"pm2_5\":\"μg/m³\"}}";
  app_air_quality_t air = {0};
  assert(app_weather_parse_air(air_json, &air));
  assert(air.available && air.us_aqi == 101 && air.pm25 > 30 && air.pm25 < 31);
  app_air_quality_t saved = air;
  assert(!app_weather_parse_air("{}", &air));
  assert(!memcmp(&saved, &air, sizeof(air)));
  root = cJSON_Parse(air_json);
  current = cJSON_GetObjectItem(root, "current");
  cJSON_SetNumberValue(cJSON_GetObjectItem(current, "us_aqi"), 1.5);
  json = cJSON_PrintUnformatted(root);
  assert(!app_weather_parse_air(json, &air) && !memcmp(&saved, &air, sizeof(air)));
  cJSON_free(json);
  cJSON_Delete(root);
  assert(app_weather_wind_level(0) == 0 && app_weather_wind_level(118) == 12);
}

int main(int argc, char **argv) {
  current_parameter_checks();
  app_weather_t weather = {0};
  assert(app_weather_parse(fixture, &weather));
  assert(weather.temperature_c == 28 && weather.apparent_c == 32);
  assert(weather.humidity_percent == -1);
  assert(strcmp(weather.condition, "晴") == 0);
  assert(strcmp(weather.updated_at, "2026-09-19T19:30") == 0);
  const char *bad[] = {"", "{", "{}", "{\"error\":true}", "[]"};
  app_weather_t saved = weather;
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    assert(!app_weather_parse(bad[i], &weather));
    assert(memcmp(&weather, &saved, sizeof(weather)) == 0);
  }
  const struct {
    const char *field;
    const char *value;
  } invalid[] = {
      {"temperature_2m", "null"},       {"temperature_2m", "\"28\""},
      {"temperature_2m", "1e309"},      {"apparent_temperature", "-200"},
      {"weather_code", "1.5"},          {"weather_code", "-1"},
      {"time", "\"2026-09-19T29:30\""}, {"time", "\"bad\""},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    cJSON *root = cJSON_Parse(fixture);
    cJSON *current = cJSON_GetObjectItem(root, "current");
    assert(cJSON_ReplaceItemInObject(current, invalid[i].field,
                                     cJSON_Parse(invalid[i].value)));
    char *json = cJSON_PrintUnformatted(root);
    assert(!app_weather_parse(json, &weather));
    assert(memcmp(&weather, &saved, sizeof(weather)) == 0);
    cJSON_free(json);
    cJSON_Delete(root);
  }
  cJSON *root = cJSON_Parse(fixture);
  cJSON *current = cJSON_GetObjectItem(root, "current");
  cJSON_SetNumberValue(cJSON_GetObjectItem(current, "temperature_2m"), -3.6);
  cJSON_SetNumberValue(cJSON_GetObjectItem(current, "weather_code"), 95);
  char *json = cJSON_PrintUnformatted(root);
  assert(app_weather_parse(json, &weather));
  assert(weather.temperature_c == -4 && strcmp(weather.condition, "雷雨") == 0);
  cJSON_free(json);
  cJSON *units = cJSON_GetObjectItem(root, "current_units");
  assert(cJSON_ReplaceItemInObject(units, "temperature_2m",
                                   cJSON_CreateString("°F")));
  json = cJSON_PrintUnformatted(root);
  assert(!app_weather_parse(json, &weather));
  cJSON_free(json);
  cJSON_Delete(root);
  root = cJSON_Parse(fixture);
  current = cJSON_GetObjectItem(root, "current");
  units = cJSON_GetObjectItem(root, "current_units");
  assert(cJSON_AddNumberToObject(current, "relative_humidity_2m", 63.6));
  assert(cJSON_AddStringToObject(units, "relative_humidity_2m", "%"));
  json = cJSON_PrintUnformatted(root);
  assert(app_weather_parse(json, &weather) && weather.humidity_percent == 64);
  cJSON_free(json);
  saved = weather;
  cJSON_SetNumberValue(cJSON_GetObjectItem(current, "relative_humidity_2m"),
                       101);
  json = cJSON_PrintUnformatted(root);
  assert(!app_weather_parse(json, &weather));
  assert(memcmp(&weather, &saved, sizeof(weather)) == 0);
  cJSON_free(json);
  cJSON_Delete(root);
  if (argc == 2) {
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    char body[8192];
    size_t n = fread(body, 1, sizeof(body) - 1, file);
    assert(!ferror(file) && fgetc(file) == EOF);
    fclose(file);
    body[n] = 0;
    assert(app_weather_parse(body, &weather));
    printf("Live API parsed: %s %d°C, apparent %d°C, %s, %s\n",
           APP_WEATHER_CITY, weather.temperature_c, weather.apparent_c,
           weather.condition, weather.updated_at);
  }
  forecast_checks();
  puts("PASS: weather fields, calendar, next-24h, UV periods and cache "
       "rollback");
}
