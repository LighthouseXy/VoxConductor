#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_WEATHER_HOURS 48

// 沿用原服务端的杭州位置；更换城市时同时修改名称和经纬度。
#define APP_WEATHER_CITY "杭州"
#define APP_WEATHER_LATITUDE "30.2741"
#define APP_WEATHER_LONGITUDE "120.1551"
#define APP_WEATHER_URL                                                        \
  "https://api.open-meteo.com/v1/forecast?latitude=" APP_WEATHER_LATITUDE      \
  "&longitude=" APP_WEATHER_LONGITUDE                                          \
  "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_" \
  "code,wind_speed_10m,wind_direction_10m,wind_gusts_10m,uv_index"             \
  "&hourly=temperature_2m,weather_code,precipitation_probability,uv_index"     \
  "&daily=uv_index_max&forecast_days=2"                                        \
  "&wind_speed_unit=kmh&temperature_unit=celsius&timezone=Asia%2FShanghai"
#define APP_AIR_QUALITY_HOST "air-quality-api.open-meteo.com"
#define APP_AIR_QUALITY_URL                                                     \
  "https://" APP_AIR_QUALITY_HOST "/v1/air-quality?latitude="                 \
  APP_WEATHER_LATITUDE "&longitude=" APP_WEATHER_LONGITUDE                     \
  "&current=us_aqi,pm2_5&timezone=Asia%2FShanghai"

typedef struct {
  char time[17];
  int temperature_c, code, rain_percent;
  float uv_index;
} app_weather_hour_t;

typedef struct {
  bool available;
  int us_aqi; // Air quality uses the U.S. AQI standard.
  float pm25;
  char updated_at[17];
} app_air_quality_t;

typedef struct {
  int temperature_c;
  int apparent_c;
  int humidity_percent; // -1: absent in a legacy observation.
  int code;
  const char *condition;
  char updated_at[17]; // YYYY-MM-DDTHH:MM
  unsigned hour_count;
  app_weather_hour_t hours[APP_WEATHER_HOURS];
  char uv_day[11];
  float uv_max;
  bool uv_available;
  bool current_uv_available, wind_available;
  float current_uv, wind_kmh, gust_kmh;
  int wind_degrees;
  app_air_quality_t air;
  bool air_cached;
} app_weather_t;

// 仅在完整、有效响应时更新out；不依赖ESP-IDF，供主机测试复用。
bool app_weather_parse(const char *json, app_weather_t *out);
bool app_weather_parse_air(const char *json, app_air_quality_t *out);
unsigned app_weather_wind_level(float kmh);
const char *app_weather_wind_direction(int degrees);
const char *app_weather_condition(int code);
// now为北京时间ISO分钟；只返回未来24小时内的小时项，不重复循环旧数据。
unsigned app_weather_future_hours(const app_weather_t *weather, const char *now,
                                  unsigned indices[24]);
const char *app_weather_uv_level(float uv);
// 输出当天严格UV>3的逐时预报区间；等于3排除，分钟级实测需独立来源。
void app_weather_uv_periods(const app_weather_t *weather, const char *day,
                            char *out, size_t bytes);
