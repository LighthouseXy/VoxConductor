/*
 * 本机统一控制：Web只入队，由协调任务执行；语音在独占会话内直接执行。
 * 字段范围集中定义，桌面层在同一UI锁内核对版本、合并增量。
 * 待确认动作仅存有界RAM；已应用回执与尚待执行的页面动作分开。
 */
#include "app_memory_alloc.h"
#include "app_control.h"
#include "app_agent.h"
#include "app_audio.h"
#include "app_cloud.h"
#include "app_desktop.h"
#include "app_media.h"
#include "app_media_web.h"
#include "app_motion.h"
#include "app_network.h"
#include "app_storage.h"
#include "app_ui.h"
#include "app_wake.h"
#include "app_tts_options.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
static SemaphoreHandle_t s_lock;
static char s_request[768];
static char *s_reply;
static uint32_t s_id;
// Web回调只入队/查询；后台协调任务串行执行慢I/O，不持队列锁执行动作。
typedef enum { CONTROL_IDLE, CONTROL_PENDING, CONTROL_RUNNING, CONTROL_DONE } control_phase_t;
static control_phase_t s_phase;
bool app_control_init(void) {
  s_lock = xSemaphoreCreateMutex();
  return s_lock != NULL;
}
uint32_t app_control_submit(const char *json) {
  if (!s_lock || !json || strlen(json) >= sizeof(s_request) ||
      xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE)
    return 0;
  uint32_t id = 0;
  if (s_phase != CONTROL_PENDING && s_phase != CONTROL_RUNNING) {
    free(s_reply);
    s_reply = NULL;
    strcpy(s_request, json);
    id = esp_random();
    if (!id)
      id = 1;
    s_id = id;
    s_phase = CONTROL_PENDING;
  }
  xSemaphoreGive(s_lock);
  return id;
}
cJSON *app_control_status(uint32_t id) {
  if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE)
    return NULL;
  cJSON *out = NULL;
  if (id == s_id && s_phase) {
    out = cJSON_CreateObject();
    if (out) {
      cJSON_AddNumberToObject(out, "job_id", id);
      cJSON_AddStringToObject(out, "status",
                              s_phase == CONTROL_DONE   ? "done"
                              : s_phase == CONTROL_RUNNING ? "running"
                                             : "pending");
      if (s_phase == CONTROL_DONE) {
        cJSON *reply = s_reply ? cJSON_Parse(s_reply) : NULL;
        if (reply)
          cJSON_AddItemToObject(out, "result", reply);
        else
          cJSON_AddStringToObject(out, "error", "result_unavailable");
      }
    }
  }
  xSemaphoreGive(s_lock);
  return out;
}
static const char *str(cJSON *o, const char *name) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static bool number(cJSON *o, const char *name, double min, double max,
                   unsigned *out) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < min ||
      v->valuedouble > max || floor(v->valuedouble) != v->valuedouble)
    return false;
  *out = (unsigned)v->valuedouble;
  return true;
}
static cJSON *result(bool ok, const char *message) {
  cJSON *r = cJSON_CreateObject();
  if (r) {
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddStringToObject(r, "message", message);
  }
  return r;
}
// Web与语音共用字段名、范围和位掩码。表中只存unsigned成员，bool单独处理。
typedef struct {
  const char *name;
  unsigned min, max, mask;
  size_t offset;
} config_field_t;
#define CONFIG_FIELD(name, min, max, mask) {#name, min, max, mask, offsetof(app_desktop_config_t, name)}
static const config_field_t s_config_fields[] = {
  CONFIG_FIELD(home_style, 0, 1, APP_CONFIG_HOME),
  CONFIG_FIELD(wallpaper_mode, 0, 4, APP_CONFIG_WALLPAPER),
  CONFIG_FIELD(volume, 0, 100, APP_CONFIG_VOLUME),
  CONFIG_FIELD(brightness, 0, 100, APP_CONFIG_BRIGHTNESS),
  CONFIG_FIELD(work_minutes, 1, 120, APP_CONFIG_WORK),
  CONFIG_FIELD(break_minutes, 1, 120, APP_CONFIG_BREAK),
  CONFIG_FIELD(tts_rate_percent, 50, 200, APP_CONFIG_TTS_RATE),
  CONFIG_FIELD(tts_voice_id, 0, APP_TTS_VOICE_COUNT - 1, APP_CONFIG_TTS_VOICE)
};
#undef CONFIG_FIELD
static const config_field_t *config_field(const char *name) {
  for (unsigned i = 0; i < sizeof(s_config_fields) / sizeof(s_config_fields[0]); ++i)
    if (!strcmp(name, s_config_fields[i].name)) return &s_config_fields[i];
  return NULL;
}
static unsigned *config_value(app_desktop_config_t *config, const config_field_t *field) {
  return (unsigned *)((char *)config + field->offset);
}
static void add_voice_catalog(cJSON *r) {
  cJSON *voices = cJSON_AddArrayToObject(r, "tts_voices");
  for (unsigned i = 0; voices && i < APP_TTS_VOICE_COUNT; ++i) {
    cJSON *voice = cJSON_CreateObject();
    if (!voice) break;
    cJSON_AddNumberToObject(voice, "id", i);
    cJSON_AddStringToObject(voice, "label", app_tts_voices[i].label);
    cJSON_AddStringToObject(voice, "voice", app_cloud_tts_voice(i));
    cJSON_AddItemToArray(voices, voice);
  }
}
cJSON *app_control_snapshot(void) {
  app_desktop_config_t d;
  if (!app_desktop_configuration(&d, NULL))
    return result(false, "设备界面忙，请重试");
  app_network_status_t n;
  app_network_status_snapshot(&n);
  cJSON *r = result(true, "设备状态已刷新");
  if (!r)
    return NULL;
  cJSON_AddBoolToObject(r, "wifi_connected", n.connected);
  cJSON_AddBoolToObject(r, "time_valid", n.time_valid);
  cJSON_AddBoolToObject(r, "time_syncing", n.time_syncing);
  cJSON_AddBoolToObject(r, "weather_updating", n.weather_updating);
  cJSON_AddNumberToObject(r, "time_retry_seconds", n.time_retry_seconds);
  cJSON_AddNumberToObject(r, "weather_retry_seconds", n.weather_retry_seconds);
  cJSON_AddNumberToObject(r, "utc_seconds", (double)time(NULL));
  cJSON_AddNumberToObject(r, "uptime_seconds",
                          (double)(esp_timer_get_time() / 1000000));
  cJSON_AddNumberToObject(r, "free_heap",
                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(r, "largest_internal_block",
                          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(r, "free_psram",
                          heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cJSON *cfg = cJSON_AddObjectToObject(r, "config");
  cJSON_AddNumberToObject(r, "config_revision", d.revision);
  if (cfg) {
    cJSON_AddNumberToObject(cfg, "home_style", d.home_style);
    cJSON_AddNumberToObject(cfg, "wallpaper_mode", d.wallpaper_mode);
    cJSON_AddNumberToObject(cfg, "volume", d.volume);
    cJSON_AddNumberToObject(cfg, "brightness", d.brightness);
    cJSON_AddBoolToObject(cfg, "wake_enabled", d.wake_enabled);
    cJSON_AddNumberToObject(cfg, "tts_rate_percent", d.tts_rate_percent);
    cJSON_AddNumberToObject(cfg, "tts_voice_id", d.tts_voice_id);
  }
  add_voice_catalog(r);
  return r;
}
static cJSON *state(void) {
  cJSON *r = app_control_snapshot();
  if (!r || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok")))
    return r;
  cJSON *tasks = cJSON_AddArrayToObject(r, "tasks");
  app_task_view_t list[8];
  unsigned count = app_agent_task_snapshot(list);
  for (unsigned i = 0; tasks && i < count; i++) {
    app_task_view_t *t = &list[i];
    cJSON *v = cJSON_CreateObject();
    if (!v)
      break;
    cJSON_AddStringToObject(v, "id", t->id);
    cJSON_AddStringToObject(v, "title", t->title);
    cJSON_AddStringToObject(v, "kind", t->kind);
    cJSON_AddStringToObject(v, "status", t->status);
    cJSON_AddStringToObject(v, "repeat", t->repeat);
    cJSON_AddStringToObject(v, "last_result", t->last_result);
    cJSON_AddNumberToObject(v, "due_utc", (double)t->due_utc);
    cJSON_AddItemToArray(tasks, v);
  }
  unsigned bytes = 0, archives = 0;
  cJSON_AddBoolToObject(r, "storage_ready",
                        app_storage_capacity(&bytes, &archives));
  cJSON_AddNumberToObject(r, "history_bytes", bytes);
  cJSON_AddNumberToObject(r, "history_archives", archives);
  cJSON_AddNumberToObject(r, "task_limit", 8);
  return r;
}
static cJSON *configure(cJSON *request) {
  app_desktop_config_t patch = {0}, actual = {0};
  unsigned mask = 0, expected = 0;
  bool versioned = cJSON_GetObjectItemCaseSensitive(request, "expected_revision") != NULL;
  if (versioned && !number(request, "expected_revision", 1, UINT32_MAX, &expected))
    return result(false, "配置版本无效，请刷新设备状态");
  const cJSON *item;
  cJSON_ArrayForEach(item, request) {
    const char *name = item->string ? item->string : "";
    if (!strcmp(name, "action") || !strcmp(name, "expected_revision")) continue;
    if (!strcmp(name, "wake_enabled")) {
      if (!cJSON_IsBool(item) || (mask & APP_CONFIG_WAKE))
        return result(false, "唤醒设置无效或字段重复");
      patch.wake_enabled = cJSON_IsTrue(item);
      mask |= APP_CONFIG_WAKE;
      continue;
    }
    const config_field_t *field = config_field(name);
    if (!field || (mask & field->mask) ||
        !number(request, name, field->min, field->max, config_value(&patch, field)))
      return result(false, "配置字段未知、重复或超出范围");
    mask |= field->mask;
  }
  // 旧网页仍可提交五项完整设置；新网页必须带版本，只提交改动项。
  unsigned legacy = APP_CONFIG_HOME | APP_CONFIG_WALLPAPER | APP_CONFIG_VOLUME |
                    APP_CONFIG_BRIGHTNESS | APP_CONFIG_WAKE;
  if (!mask || (!versioned && (mask & legacy) != legacy))
    return result(false, "请提交改动项与配置版本");
  app_config_result_t status = app_desktop_configuration_patch(&actual, &patch, mask, expected);
  const char *message = status == APP_CONFIG_SAVED ? "设备设置已保存" :
      status == APP_CONFIG_CONFLICT ? "设备设置已被其他操作更改，请刷新后核对草稿再保存" :
      status == APP_CONFIG_SAVE_FAILED ? "设置已暂时生效，但保存失败，请回读设备状态" :
      status == APP_CONFIG_BUSY ? "设备界面或硬件忙，请重试" : "配置无效或当前设置只读";
  cJSON *r = result(status == APP_CONFIG_SAVED, message);
  if (r) {
    cJSON_AddStringToObject(r, "status", status == APP_CONFIG_SAVED ? "saved" :
        status == APP_CONFIG_CONFLICT ? "conflict" :
        status == APP_CONFIG_SAVE_FAILED ? "partial" : "rejected");
    cJSON_AddNumberToObject(r, "config_revision", actual.revision);
  }
  return r;
}
static cJSON *execute(cJSON *request) {
  const char *action = str(request, "action");
  if (!strcmp(action, "state"))
    return state();
  if (!strcmp(action, "configure"))
    return configure(request);
  if (!strcmp(action, "task_create")) {
    unsigned due = 0;
    bool valid = number(request, "due_utc", 0, 4102444800.0, &due);
    bool ok = valid && app_agent_create_local_task(str(request, "kind"),
                                                   str(request, "title"), due,
                                                   str(request, "repeat"));
    return result(ok, ok ? "已保存到SD卡"
                         : "保存失败：检查SD、校时、时间范围或8条任务上限");
  }
  if (!strcmp(action, "task_change")) {
    const char *op = str(request, "operation"), *id = str(request, "id");
    unsigned seconds = 0;
    bool allowed = !strcmp(op, "complete") || !strcmp(op, "reopen") ||
                   !strcmp(op, "cancel") || !strcmp(op, "delete") ||
                   !strcmp(op, "edit");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(request, "seconds");
    bool valid = allowed && strlen(id) < 64 && *id &&
                 (!v || number(request, "seconds", 0, 2592000, &seconds));
    cJSON *due_field = cJSON_GetObjectItemCaseSensitive(request, "due_utc");
    if (due_field) {
      unsigned due = 0;
      long long now = time(NULL);
      valid = valid && !strcmp(op, "edit") &&
              number(request, "due_utc", 0, 4102444800.0, &due) &&
              now >= 1704067200 && due > now && due - now <= 2592000;
      if (valid)
        seconds = (unsigned)(due - now);
    }
    const char *title = str(request, "title"), *repeat = str(request, "repeat");
    bool ok = valid && strlen(title) < 128 && strlen(repeat) < 16 &&
              app_agent_task_change(id, op, (int)seconds, *title ? title : NULL,
                                    *repeat ? repeat : NULL);
    return result(ok, ok ? "任务已保存" : "任务修改或SD写入失败");
  }
  if (!strcmp(action, "history")) {
    unsigned before = 0;
    if (!number(request, "before", 0, 8 * 1024 * 1024, &before))
      return result(false, "记录分页位置无效");
    const char *key = str(request, "keyword"), *id = str(request, "record_id");
    if (strlen(key) > 48 || strlen(id) > 19)
      return result(false, "检索参数过长");
    cJSON *r = *id    ? app_storage_history_query("", before, id)
               : *key ? app_storage_history_query(key, before, NULL)
                      : app_storage_recent_history(before);
    if (!r)
      return result(false, "记录不可用，检查SD卡或关键词（至少2字节）");
    cJSON_AddBoolToObject(r, "ok",
                          cJSON_GetObjectItemCaseSensitive(r, "error") == NULL);
    return r;
  }
  return result(false, "不支持的操作");
}
void app_control_service(void) {
  if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE)
    return;
  if (s_phase != CONTROL_PENDING) {
    xSemaphoreGive(s_lock);
    return;
  }
  char request[768];
  strcpy(request, s_request);
  s_phase = CONTROL_RUNNING;
  xSemaphoreGive(s_lock);
  cJSON *in = cJSON_Parse(request),
        *out = cJSON_IsObject(in) ? execute(in) : result(false, "请求无效");
  char *reply = out ? cJSON_PrintUnformatted(out) : NULL;
  cJSON_Delete(in);
  cJSON_Delete(out);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_reply = reply;
  s_phase = CONTROL_DONE;
  xSemaphoreGive(s_lock);
}
// One voice coordinator owns this bounded RAM state. Never restore authorization.
static char s_voice_name[32], s_voice_args[513], s_voice_summary[192];
static int64_t s_voice_deadline;
static app_media_anniversary_t s_anniversary_expected;
static unsigned s_anniversary_slot;
static struct {
  char action[24], target[24];
  bool end_session, home_applied;
} s_after;

static char *print_result(cJSON *r) {
  char *json = r ? cJSON_PrintUnformatted(r) : NULL;
  cJSON_Delete(r);
  return json;
}
bool app_control_voice_tool(const char *name) {
  return name && (!strcmp(name, "get_local_state") || !strcmp(name, "set_local_setting") ||
      !strcmp(name, "control_media") || !strcmp(name, "control_desktop") ||
      !strcmp(name, "manage_anniversary"));
}
cJSON *app_control_voice_tools(void) {
  return cJSON_Parse(
      "["
      "{\"type\":\"function\",\"function\":{\"name\":\"get_local_state\",\"description\":\"读取本机设置、媒体目录、专注倒计时、纪念日或网络/SD状态。先查目录再选择文件，缺项不得猜测。\",\"parameters\":{\"type\":\"object\",\"properties\":{\"scope\":{\"type\":\"string\",\"enum\":[\"settings\",\"media\",\"focus\",\"anniversaries\",\"network\"]}},\"required\":[\"scope\"],\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"set_local_setting\",\"description\":\"修改本轮明确要求的本机设置，value为绝对值，相对值从本轮快照计算。亮度/音量/夜间亮度0..100；wake_enabled0/1；主页0钟表1壁纸时间；壁纸0SD图1星空2轨道3水母4花园；午夜起分钟0..1439；息屏秒0关闭或10..3600；专注/休息1..120；tts_rate_percent50..200，步长5，100正常；tts_voice_id只选本轮settings.tts_voices返回的ID，0恢复默认，找不到或音色不明确时澄清。\",\"parameters\":{\"type\":\"object\",\"properties\":{\"setting\":{\"type\":\"string\",\"enum\":[\"brightness\",\"volume\",\"home_style\",\"wallpaper_mode\",\"wake_enabled\",\"work_minutes\",\"break_minutes\",\"night_enabled\",\"night_start_minute\",\"night_end_minute\",\"night_brightness\",\"idle_seconds\",\"tts_rate_percent\",\"tts_voice_id\"]},\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3600}},\"required\":[\"setting\",\"value\"],\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"control_media\",\"description\":\"管理本机音乐/环境音及SD图片。play/image/wallpaper必须先查询目录，file填写返回的精确文件名；不得猜文件名。音乐操作响应成功仅表示播放设置应用，回答结束后才播放。repeat value0/1，timer value0关闭或1..240分钟。\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"play\",\"pause\",\"resume\",\"stop\",\"next\",\"previous\",\"repeat\",\"timer\",\"image\",\"wallpaper\"]},\"file\":{\"type\":\"string\",\"maxLength\":48},\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":240}},\"required\":[\"action\"],\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"control_desktop\",\"description\":\"管理本机页面、专注计时、姿态校准和网页管理。页面在本轮播报结束后执行，scheduled不是已打开。focus_start minutes0使用默认或1..120，target=pomodoro为番茄钟否则倒计时。停止计时/校准需下一轮确认执行。web_open显示扫码二维码，不读出配对密钥。\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"focus_start\",\"focus_pause\",\"focus_resume\",\"focus_stop\",\"calibrate\",\"web_open\",\"web_close\"]},\"target\":{\"type\":\"string\",\"enum\":[\"home\",\"weather\",\"forecast\",\"environment\",\"assistant\",\"tools\",\"settings\",\"alarm\",\"tasks\",\"focus\",\"music\",\"ambient\",\"media\",\"wallpapers\",\"anniversaries\",\"album\",\"web\",\"pomodoro\",\"countdown\"]},\"minutes\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":120}},\"required\":[\"action\"],\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"manage_anniversary\",\"description\":\"本轮明确要求时新增/修改/删除纪念日，先查4个槽位索引0..3；删除或覆盖已有项需下一轮确认执行。新增/修改必须完整日期与label、annual；annual代表每年，不能猜年份。\",\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"upsert\",\"delete\"]},\"index\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3},\"label\":{\"type\":\"string\",\"maxLength\":47},\"year\":{\"type\":\"integer\",\"minimum\":1970,\"maximum\":2199},\"month\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":12},\"day\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":31},\"annual\":{\"type\":\"boolean\"}},\"required\":[\"action\",\"index\"],\"additionalProperties\":false}}}"
      "]");
}
bool app_control_voice_pending(void) {
  return s_voice_deadline > esp_timer_get_time();
}
void app_control_voice_cancel(void) {
  s_voice_deadline = 0;
  // Cancel pending operations and retain receipts for applied settings.
  // The voice coordinator consumes each receipt once at turn end.
  bool home_applied = s_after.home_applied;
  memset(&s_after, 0, sizeof(s_after));
  s_after.home_applied = home_applied;
}
bool app_control_voice_home_applied(void) { return s_after.home_applied; }
char *app_control_voice_pending_context(void) {
  if (!app_control_voice_pending()) return NULL;
  cJSON *r = result(true, s_voice_summary);
  cJSON_AddStringToObject(r, "pending_action_id", "local_control");
  cJSON_AddStringToObject(r, "kind", s_voice_name);
  cJSON_AddBoolToObject(r, "executed", false);
  cJSON_AddBoolToObject(r, "requires_confirmation", true);
  return print_result(r);
}
static cJSON *prepare(const char *name, const char *args, const char *summary) {
  if (app_agent_has_pending()) return result(false, "已有待确认操作，请先确认或取消");
  snprintf(s_voice_name, sizeof(s_voice_name), "%s", name);
  snprintf(s_voice_args, sizeof(s_voice_args), "%s", args);
  snprintf(s_voice_summary, sizeof(s_voice_summary), "%s", summary);
  s_voice_deadline = esp_timer_get_time() + 120000000;
  cJSON *r = result(true, summary);
  cJSON_AddStringToObject(r, "status", "pending_confirmation");
  cJSON_AddBoolToObject(r, "executed", false);
  cJSON_AddStringToObject(r, "confirmation", "请下一轮说确认执行，或取消执行");
  return r;
}
static cJSON *media_result(uint32_t id, bool delayed, bool end_session) {
  if (!id) return result(false, "请求未入队：参数无效、文件不存在或队列忙");
  app_media_job_t job = {0};
  int64_t deadline = esp_timer_get_time() + 1200000;
  do {
    if (app_cloud_is_cancelled()) return result(false, "语音已中断；入队操作可能已生效，请查询当前状态");
    if (app_media_job_status(id, &job) && job.done) {
      cJSON *r = result(job.applied && job.saved, job.applied && job.saved ?
          (end_session ? "音频控制已应用，回答结束后返回媒体" : "操作已应用并保存") : job.message);
      cJSON_AddBoolToObject(r, "applied", job.applied);
      cJSON_AddBoolToObject(r, "saved", job.saved);
      cJSON_AddStringToObject(r, "status", job.applied && job.saved ? "applied" : job.applied ? "partial" : "failed");
      if (job.applied && end_session) s_after.end_session = true;
      return r;
    }
    if (delayed) break; // Image SD reads wait until voice releases its media lease.
    vTaskDelay(pdMS_TO_TICKS(20));
  } while (esp_timer_get_time() < deadline);
  cJSON *r = result(true, delayed ? "图片已排队，回答结束后加载；尚未验证显示" : "操作仍在处理，尚未确认成功");
  cJSON_AddStringToObject(r, "status", "pending");
  cJSON_AddNumberToObject(r, "job_id", id);
  cJSON_AddBoolToObject(r, "executed", false);
  if (delayed || end_session) s_after.end_session = true;
  return r;
}
static cJSON *local_state(const char *scope) {
  if (!strcmp(scope, "network")) return state();
  cJSON *r = result(true, "本机状态");
  if (!r) return NULL;
  if (!strcmp(scope, "settings")) {
    app_desktop_config_t d;
    if (!app_desktop_configuration(&d, NULL)) { cJSON_Delete(r); return result(false, "界面忙"); }
    cJSON_AddNumberToObject(r, "brightness", d.brightness);
    cJSON_AddNumberToObject(r, "volume", app_audio_get_volume());
    cJSON_AddNumberToObject(r, "saved_volume", d.volume);
    cJSON_AddNumberToObject(r, "tts_rate_percent", d.tts_rate_percent);
    cJSON_AddNumberToObject(r, "tts_voice_id", d.tts_voice_id);
    cJSON_AddStringToObject(r, "tts_voice", app_cloud_tts_voice(d.tts_voice_id));
    add_voice_catalog(r);
    cJSON_AddNumberToObject(r, "home_style", d.home_style);
    cJSON_AddNumberToObject(r, "wallpaper_mode", d.wallpaper_mode);
    cJSON_AddBoolToObject(r, "wake_enabled", d.wake_enabled);
    cJSON_AddBoolToObject(r, "wake_model_available", app_wake_available());
    cJSON_AddBoolToObject(r, "wake_active", app_wake_available() && app_wake_is_enabled());
    cJSON_AddNumberToObject(r, "work_minutes", d.work_minutes);
    cJSON_AddNumberToObject(r, "break_minutes", d.break_minutes);
    app_media_appearance_t a; app_media_get_appearance(&a);
    cJSON_AddBoolToObject(r, "night_enabled", a.night_enabled);
    cJSON_AddNumberToObject(r, "night_start_minute", a.night_start_minute);
    cJSON_AddNumberToObject(r, "night_end_minute", a.night_end_minute);
    cJSON_AddNumberToObject(r, "night_brightness", a.night_brightness);
    cJSON_AddNumberToObject(r, "idle_seconds", a.idle_seconds);
  } else if (!strcmp(scope, "media")) {
    app_media_snapshot_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) { cJSON_Delete(r); return result(false, "媒体目录内存不足"); }
    app_media_snapshot(m);
    cJSON_AddBoolToObject(r, "ready", m->ready);
    cJSON_AddBoolToObject(r, "playing", m->playing);
    cJSON_AddBoolToObject(r, "paused", m->paused);
    cJSON_AddBoolToObject(r, "repeat", m->repeat);
    cJSON_AddNumberToObject(r, "audio_index", m->audio_index);
    cJSON_AddNumberToObject(r, "stop_remaining_seconds", m->stop_remaining_seconds);
    cJSON_AddStringToObject(r, "message_detail", m->message);
    const char *fields[] = {"audio", "images"};
    for (unsigned type = 0; type < 2; ++type) {
      cJSON *list = cJSON_AddArrayToObject(r, fields[type]);
      unsigned count = type ? m->image_count : m->audio_count;
      for (unsigned i = 0; list && i < count && i < APP_MEDIA_MAX_FILES; ++i) {
        const char *file = type ? m->images[i] : m->audio[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "file", file);
        const char *label = app_media_file_label(file);
        if (strcmp(label, file)) cJSON_AddStringToObject(item, "label", label);
        if (!type) cJSON_AddStringToObject(item, "category", app_media_audio_is_ambient(file) ? "ambient" : "music");
        cJSON_AddItemToArray(list, item);
      }
    }
    heap_caps_free(m);
  } else if (!strcmp(scope, "anniversaries")) {
    app_media_anniversary_t items[APP_MEDIA_ANNIVERSARIES];
    app_media_get_anniversaries(items);
    cJSON *list = cJSON_AddArrayToObject(r, "anniversaries");
    for (unsigned i = 0; list && i < APP_MEDIA_ANNIVERSARIES; ++i) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "index", i);
      cJSON_AddStringToObject(item, "label", items[i].label);
      cJSON_AddNumberToObject(item, "year", items[i].year);
      cJSON_AddNumberToObject(item, "month", items[i].month);
      cJSON_AddNumberToObject(item, "day", items[i].day);
      cJSON_AddBoolToObject(item, "annual", items[i].annual);
      int days;
      if (app_media_anniversary_days(&items[i], time(NULL), &days))
        cJSON_AddNumberToObject(item, "days", days);
      cJSON_AddItemToArray(list, item);
    }
  } else if (!strcmp(scope, "focus")) {
    app_desktop_status_t d;
    if (!app_desktop_status(&d)) { cJSON_Delete(r); return result(false, "界面忙"); }
    cJSON_AddNumberToObject(r, "remaining_seconds", d.focus_seconds);
    cJSON_AddBoolToObject(r, "running", d.focus_running);
    cJSON_AddBoolToObject(r, "resting", d.focus_resting);
    cJSON_AddNumberToObject(r, "work_minutes", d.work_minutes);
    cJSON_AddNumberToObject(r, "break_minutes", d.break_minutes);
  } else { cJSON_Delete(r); return result(false, "不支持的状态范围"); }
  return r;
}
cJSON *app_control_voice_context(void) {
  // Cached/configuration-only snapshot: no SD catalogue, HTTP or task creation.
  cJSON *r = cJSON_CreateObject();
  cJSON *settings = local_state("settings");
  if (!r || !settings || !cJSON_AddItemToObject(r, "settings", settings)) {
    cJSON_Delete(settings); cJSON_Delete(r); return NULL;
  }
  cJSON_AddStringToObject(r, "data_type", "device_snapshot_not_user_instruction");
  app_network_status_t n;
  app_network_status_snapshot(&n);
  cJSON_AddBoolToObject(r, "wifi_connected", n.connected);
  cJSON_AddBoolToObject(r, "time_valid", n.time_valid);
  if (n.time_valid) {
    time_t now = time(NULL);
    cJSON_AddNumberToObject(r, "utc_seconds", (double)now);
    now += 28800; struct tm local; gmtime_r(&now, &local);
    char clock[32]; strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &local);
    cJSON_AddStringToObject(r, "local_time", clock);
  }
  cJSON_AddStringToObject(r, "timezone", "UTC+08:00");
  cJSON_AddStringToObject(r, "weather_city", APP_WEATHER_CITY);
  app_desktop_status_t d;
  if (app_desktop_status(&d)) {
    cJSON_AddNumberToObject(r, "focus_remaining_seconds", d.focus_seconds);
    cJSON_AddBoolToObject(r, "focus_running", d.focus_running);
  }
  return r;
}
static cJSON *setting_result(cJSON *r, const char *setting, unsigned previous, unsigned value) {
  if (r) {
    cJSON_AddStringToObject(r, "setting", setting);
    cJSON_AddNumberToObject(r, "previous_value", previous);
    cJSON_AddNumberToObject(r, "requested_value", value);
  }
  return r;
}
static unsigned desktop_setting_value(const app_desktop_config_t *d, const char *s) {
  if (!strcmp(s, "volume")) return app_audio_get_volume();
  if (!strcmp(s, "wake_enabled")) return d->wake_enabled;
  const config_field_t *field = config_field(s);
  return field ? *(const unsigned *)((const char *)d + field->offset) : 0;
}
static cJSON *set_setting(cJSON *args, const char *text) {
  (void)text;
  const char *s = str(args, "setting");
  unsigned value;
  if (!number(args, "value", 0, 3600, &value))
    return result(false, "设置参数无效");
  app_desktop_config_t d;
  if (!app_desktop_configuration(&d, NULL)) return result(false, "界面忙");
  const config_field_t *field = config_field(s);
  unsigned mask = field ? field->mask : !strcmp(s, "wake_enabled") ? APP_CONFIG_WAKE : 0;
  bool valid = true;
  unsigned previous = desktop_setting_value(&d, s);
  if (field) {
    valid = value >= field->min && value <= field->max;
    *config_value(&d, field) = value;
  } else if (mask == APP_CONFIG_WAKE) {
    valid = value <= 1 && (!value || app_wake_available());
    d.wake_enabled = value != 0;
  }
  if (mask) {
    if (!valid) return result(false, "设置值超出范围或该功能不可用");
    // 语音也使用版本核对；冲突时不自动重试写入，更不能假称已经完成。
    app_config_result_t status = app_desktop_configuration_patch(NULL, &d, mask, d.revision);
    if (status == APP_CONFIG_CONFLICT) return result(false, "设置已发生变化，请重新查询后调整");
    if (status == APP_CONFIG_INVALID || status == APP_CONFIG_BUSY)
      return result(false, "设置无效或设备忙，未应用本次调整");
    bool ok = status == APP_CONFIG_SAVED;
    app_desktop_config_t actual;
    bool readback = app_desktop_configuration(&actual, NULL);
    bool applied = readback && desktop_setting_value(&actual, s) == value;
    char message[160];
    if (!applied) snprintf(message, sizeof(message), "%s", "设置生效未确认，请查询当前状态");
    else if (!ok) snprintf(message, sizeof(message), "%s", "设置已暂时生效，但保存失败");
    else if (!strcmp(s, "volume")) snprintf(message, sizeof(message), "音量已设为%u%%并保存", desktop_setting_value(&actual, s));
    else if (!strcmp(s, "tts_rate_percent")) snprintf(message, sizeof(message), "语速已设为%u%%并保存", actual.tts_rate_percent);
    else if (!strcmp(s, "tts_voice_id")) snprintf(message, sizeof(message), "音色已设为%s并保存", app_tts_voices[actual.tts_voice_id].label);
    else if (!strcmp(s, "home_style")) snprintf(message, sizeof(message), "主页已保存为%s，播报结束后显示", value ? "壁纸加数字时间" : "钟表样式");
    else snprintf(message, sizeof(message), "%s", "本机设置已应用并保存");
    cJSON *r = setting_result(result(ok && applied, message), s, previous, value);
    if (r) {
      cJSON_AddBoolToObject(r, "saved", ok);
      cJSON_AddBoolToObject(r, "applied", applied);
      cJSON_AddStringToObject(r, "status", ok && applied ? "applied" : applied ? "partial" : "unconfirmed");
      if (readback) cJSON_AddNumberToObject(r, "value", desktop_setting_value(&actual, s));
    }
    ESP_LOGI("control", "Setting %s: previous=%u requested=%u actual=%d saved=%d applied=%d",
             s, previous, value, readback ? (int)desktop_setting_value(&actual, s) : -1, ok, applied);
    if (applied && (!strcmp(s, "home_style") || !strcmp(s, "wallpaper_mode")))
      s_after.home_applied = true;
    if (applied && (!strcmp(s, "home_style") || !strcmp(s, "wallpaper_mode")) &&
        (!s_after.action[0] || !strcmp(s_after.target, "home"))) {
      snprintf(s_after.action, sizeof(s_after.action), "%s", "open");
      snprintf(s_after.target, sizeof(s_after.target), "%s", "home");
      s_after.end_session = true;
    }
    return r;
  }
  app_media_appearance_t a; app_media_get_appearance(&a);
  if (!strcmp(s, "night_enabled")) { previous = a.night_enabled; valid = value <= 1; a.night_enabled = value != 0; }
  else if (!strcmp(s, "night_start_minute")) { previous = a.night_start_minute; valid = value < 1440; a.night_start_minute = value; }
  else if (!strcmp(s, "night_end_minute")) { previous = a.night_end_minute; valid = value < 1440; a.night_end_minute = value; }
  else if (!strcmp(s, "night_brightness")) { previous = a.night_brightness; valid = value <= 100; a.night_brightness = value; }
  else if (!strcmp(s, "idle_seconds")) { previous = a.idle_seconds; valid = !value || (value >= 10 && value <= 3600); a.idle_seconds = value; }
  else valid = false;
  return valid ? setting_result(media_result(app_media_appearance_tracked(&a), false, false), s, previous, value) : result(false, "设置值超出范围");
}
static cJSON *control_media(cJSON *args, const char *text) {
  (void)text;
  const char *action = str(args, "action"), *file = str(args, "file");
  bool image = !strcmp(action, "image") || !strcmp(action, "wallpaper");
  bool select = image || !strcmp(action, "play");
  unsigned value = 0;
  cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "value");
  if (v && !number(args, "value", 0, 240, &value)) return result(false, "媒体值无效");
  if (select) {
    if (!*file || strlen(file) >= APP_MEDIA_NAME_BYTES || !app_media_valid_name(file, image))
      return result(false, "需要目录中的精确文件名");
    app_media_snapshot_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return result(false, "媒体目录内存不足");
    app_media_snapshot(m);
    unsigned count = image ? m->image_count : m->audio_count, found = count;
    for (unsigned i = 0; i < count && i < APP_MEDIA_MAX_FILES; ++i)
      if (!strcmp(file, image ? m->images[i] : m->audio[i])) { found = i; break; }
    heap_caps_free(m);
    if (found == count) return result(false, "文件不在当前目录中，请先查询媒体目录");
    if (image && s_after.action[0]) return result(false, "本轮已有页面操作，图片未提交");
    uint32_t id = image ? app_media_image_tracked(found, !strcmp(action, "wallpaper")) :
                          app_media_command_tracked(APP_MEDIA_PLAY, found);
    if (id && image) {
      snprintf(s_after.action, sizeof(s_after.action), "%s", "open");
      snprintf(s_after.target, sizeof(s_after.target), "%s", !strcmp(action, "wallpaper") ? "home" : "album");
      if (!strcmp(action, "wallpaper")) {
        app_desktop_config_t d;
        if (!app_desktop_configuration(&d, NULL)) {
          s_after.end_session = true;
          return result(false, "图片已排队，但界面忙，SD壁纸模式未切换");
        }
        d.wallpaper_mode = 0;
        if (app_desktop_configuration_patch(NULL, &d, APP_CONFIG_WALLPAPER, d.revision) != APP_CONFIG_SAVED) {
          s_after.end_session = true;
          return result(false, "图片已排队，但SD壁纸模式未保存，可能暂时生效");
        }
      }
    }
    return media_result(id, image, true);
  }
  app_media_command_t command;
  if (!strcmp(action, "pause")) command = APP_MEDIA_PAUSE;
  else if (!strcmp(action, "resume")) command = APP_MEDIA_RESUME;
  else if (!strcmp(action, "stop")) command = APP_MEDIA_STOP;
  else if (!strcmp(action, "next")) command = APP_MEDIA_NEXT;
  else if (!strcmp(action, "previous")) command = APP_MEDIA_PREVIOUS;
  else if (!strcmp(action, "repeat") && v && value <= 1) command = APP_MEDIA_REPEAT;
  else if (!strcmp(action, "timer") && v) command = APP_MEDIA_AMBIENT_TIMER;
  else return result(false, "不支持的媒体操作或参数不足");
  bool play = command == APP_MEDIA_RESUME || command == APP_MEDIA_NEXT || command == APP_MEDIA_PREVIOUS;
  return media_result(app_media_command_tracked(command, value), false, play);
}
static bool destination(const char *target) {
  const char *values[] = {"home", "weather", "forecast", "environment", "assistant", "tools",
    "settings", "alarm", "tasks", "focus", "music", "ambient", "media",
    "wallpapers", "anniversaries", "album", "web"};
  for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i)
    if (!strcmp(target, values[i])) return true;
  return false;
}
static cJSON *control_desktop(cJSON *args, const char *text, bool confirmed, const char *raw) {
  (void)text;
  const char *action = str(args, "action"), *target = str(args, "target");
  unsigned minutes = 0;
  cJSON *m = cJSON_GetObjectItemCaseSensitive(args, "minutes");
  if (m && !number(args, "minutes", 0, 120, &minutes)) return result(false, "操作参数无效");
  if (!strcmp(action, "open") && !strcmp(s_after.action, "open") &&
      !strcmp(target, s_after.target)) {
    cJSON *r = result(true, "该页面已安排在播报结束后显示，无需重复打开");
    cJSON_AddStringToObject(r, "status", "scheduled");
    cJSON_AddBoolToObject(r, "executed", false);
    return r;
  }
  if ((!strcmp(action, "open") || !strcmp(action, "web_open")) && s_after.action[0])
    return result(false, "本轮已安排页面操作，请下一轮再切换");
  if ((!strcmp(action, "focus_stop") || !strcmp(action, "calibrate")) && !confirmed) {
    if (!strcmp(action, "calibrate") && !app_motion_available()) return result(false, "姿态传感器不可用");
    return prepare("control_desktop", raw, !strcmp(action, "calibrate") ?
      "将重新校准姿态中位，请放稳设备" : "将停止并清空当前专注倒计时");
  }
  if (!strncmp(action, "focus_", 6)) {
    if (*target && strcmp(target, "pomodoro") && strcmp(target, "countdown"))
      return result(false, "计时类型无效");
    app_desktop_status_t d;
    if (!app_desktop_status(&d)) return result(false, "界面忙");
    if (!confirmed && !strcmp(action, "focus_start") && d.focus_seconds) {
      char summary[96];
      snprintf(summary, sizeof(summary), "替换当前计时为%u分钟%s", minutes ? minutes : d.work_minutes,
          !strcmp(target, "pomodoro") ? "番茄钟" : "倒计时");
      return prepare("control_desktop", raw, summary);
    }
    if (!lvgl_port_lock(100)) return result(false, "界面忙");
    bool ok = app_desktop_focus_control(action, minutes, !strcmp(target, "pomodoro"));
    lvgl_port_unlock();
    return result(ok, ok ? "专注计时状态已应用" : "计时未开始或参数无效");
  }
  if (!strcmp(action, "calibrate")) {
    if (!app_motion_request_calibration()) return result(false, "姿态传感器不可用或校准请求失败");
    // Voice blocks gesture dispatch, but calibration itself must sample stillness.
    app_motion_set_enabled(true);
    int64_t deadline = esp_timer_get_time() + 4000000;
    while (app_motion_calibrating() && esp_timer_get_time() < deadline && !app_cloud_is_cancelled())
      vTaskDelay(pdMS_TO_TICKS(40));
    bool ok = !app_motion_calibrating() && app_motion_available() && !app_cloud_is_cancelled();
    app_motion_set_enabled(false);
    return result(ok, ok ? "姿态校准已完成" : "尚未确认校准完成，请放稳设备后查看设置");
  }
  if (!strcmp(action, "web_open") || !strcmp(action, "web_close")) {
    bool enabled = !strcmp(action, "web_open");
    app_media_web_request(enabled);
    app_media_web_status_t web = {0};
    int64_t deadline = esp_timer_get_time() + 2500000;
    do {
      app_media_web_status(&web);
      if (web.enabled == enabled || app_cloud_is_cancelled()) break;
      vTaskDelay(pdMS_TO_TICKS(40));
    } while (esp_timer_get_time() < deadline);
    cJSON *r = result(web.enabled == enabled, web.enabled == enabled ?
      (enabled ? "网页管理已开启，播报结束后显示扫码二维码" : "网页管理已关闭") : "网页管理切换尚未完成，请查看设备");
    if (enabled && web.enabled) {
      snprintf(s_after.action, sizeof(s_after.action), "%s", "open");
      snprintf(s_after.target, sizeof(s_after.target), "%s", "web");
      s_after.end_session = true;
    }
    return r; // Keep web.token/url inside the local management interface.
  }
  if (strcmp(action, "open") || !destination(target)) return result(false, "不支持的页面");
  if (s_after.action[0]) return result(false, "本轮已安排页面操作，请下一轮再切换");
  snprintf(s_after.action, sizeof(s_after.action), "%s", action);
  snprintf(s_after.target, sizeof(s_after.target), "%s", target);
  s_after.end_session = true;
  cJSON *r = result(true, "已安排在本轮播报结束后执行");
  cJSON_AddStringToObject(r, "status", "scheduled");
  cJSON_AddBoolToObject(r, "executed", false);
  return r;
}
static cJSON *anniversary(cJSON *args, bool confirmed, const char *raw) {
  unsigned index;
  const char *action = str(args, "action");
  if (!number(args, "index", 0, 3, &index) ||
      (strcmp(action, "upsert") && strcmp(action, "delete"))) return result(false, "纪念日操作或索引无效");
  app_media_anniversary_t items[APP_MEDIA_ANNIVERSARIES], value = {0};
  app_media_get_anniversaries(items);
  if (!strcmp(action, "upsert")) {
    const char *label = str(args, "label");
    cJSON *annual = cJSON_GetObjectItemCaseSensitive(args, "annual");
    if (!*label || strlen(label) >= sizeof(value.label) || !cJSON_IsBool(annual) ||
        !number(args, "year", 1970, 2199, &value.year) ||
        !number(args, "month", 1, 12, &value.month) || !number(args, "day", 1, 31, &value.day))
      return result(false, "需要完整有效的日期、名称及是否每年重复");
    snprintf(value.label, sizeof(value.label), "%s", label);
    value.annual = cJSON_IsTrue(annual);
    int days;
    if (!app_media_anniversary_days(&value, 1704067200, &days)) return result(false, "纪念日日期无效");
  }
  if (!confirmed && (items[index].label[0] || !strcmp(action, "delete"))) {
    char summary[192];
    if (!strcmp(action, "delete"))
      snprintf(summary, sizeof(summary), "删除第%u项纪念日：%s", index + 1, items[index].label[0] ? items[index].label : "空项");
    else
      snprintf(summary, sizeof(summary), "覆盖%s为%s，%04u-%02u-%02u，%s", items[index].label,
        value.label, value.year, value.month, value.day, value.annual ? "每年" : "单次");
    cJSON *r = prepare("manage_anniversary", raw, summary);
    if (app_control_voice_pending() && !strcmp(s_voice_name, "manage_anniversary") &&
        !strcmp(s_voice_args, raw)) {
      s_anniversary_expected = items[index];
      s_anniversary_slot = index;
    }
    return r;
  }
  if (confirmed && (s_anniversary_slot != index ||
      strcmp(items[index].label, s_anniversary_expected.label) ||
      items[index].year != s_anniversary_expected.year || items[index].month != s_anniversary_expected.month ||
      items[index].day != s_anniversary_expected.day || items[index].annual != s_anniversary_expected.annual))
    return result(false, "纪念日已被其他操作改动，请重新查询并确认");
  return media_result(app_media_anniversary_tracked(index, &value), false, false);
}
static char *voice_execute(const char *name, const char *arguments, const char *text, bool confirmed) {
  if (!name || !arguments || strlen(arguments) > 512 || strstr(arguments, "\\u0000"))
    return print_result(result(false, "参数无效"));
  cJSON *args = cJSON_ParseWithLengthOpts(arguments, strlen(arguments) + 1, NULL, true);
  if (!cJSON_IsObject(args)) { cJSON_Delete(args); return print_result(result(false, "参数无效")); }
  const char *allowed = !strcmp(name, "get_local_state") ? "|scope|" :
     !strcmp(name, "set_local_setting") ? "|setting|value|request|" :
     !strcmp(name, "control_media") ? "|action|file|value|request|" :
     !strcmp(name, "control_desktop") ? "|action|target|minutes|request|" : "|action|index|label|year|month|day|annual|request|";
  bool valid = app_control_voice_tool(name);
  cJSON *field;
  cJSON_ArrayForEach(field, args) {
    char key[40]; int n = snprintf(key, sizeof(key), "|%s|", field->string ? field->string : "");
    if (n < 0 || (size_t)n >= sizeof(key) || !strstr(allowed, key)) valid = false;
  }
  cJSON *r = NULL;
  if (!valid) r = result(false, "不支持的参数");
  else if (!strcmp(name, "get_local_state")) r = local_state(str(args, "scope"));
  else if (!strcmp(name, "set_local_setting")) r = set_setting(args, text);
  else if (!strcmp(name, "control_media")) r = control_media(args, text);
  else if (!strcmp(name, "control_desktop")) r = control_desktop(args, text, confirmed, arguments);
  else r = anniversary(args, confirmed, arguments);
  cJSON_Delete(args);
  return print_result(r);
}
char *app_control_voice_execute(const char *name, const char *arguments, const char *user_text) {
  if (app_agent_write_tool(name)) {
    cJSON *args = arguments && strlen(arguments) <= 512 && !strstr(arguments, "\\u0000") ?
        cJSON_ParseWithLengthOpts(arguments, strlen(arguments) + 1, NULL, true) : NULL;
    bool valid = app_agent_request_matches(args, user_text);
    cJSON_Delete(args);
    if (!valid) return print_result(result(false, "缺少匹配本轮原话的操作请求，未执行"));
    if (app_agent_has_pending()) return print_result(result(false, "已有待确认操作，请先确认或取消"));
  }
  return voice_execute(name, arguments, user_text, false);
}
static char *voice_answer(char *json) {
  cJSON *r = json ? cJSON_Parse(json) : NULL;
  const char *message = r ? str(r, "message") : "操作结果不可用";
  const char *confirmation = r ? str(r, "confirmation") : "";
  char *answer = app_psram_malloc(strlen(message) + strlen(confirmation) + 4);
  if (answer) sprintf(answer, "%s。%s", message, confirmation);
  cJSON_Delete(r); free(json);
  return answer;
}
static bool phrase(const char *text, const char *value) {
  while (*text == ' ' || *text == '\t') ++text;
  size_t n = strlen(value);
  if (strncmp(text, value, n)) return false;
  text += n;
  while (*text) {
    if (strchr(" .!?\t\r\n", *text)) ++text;
    else if (!strncmp(text, "。", 3) || !strncmp(text, "！", 3) || !strncmp(text, "？", 3)) text += 3;
    else return false;
  }
  return true;
}
bool app_control_strip_interrupt(char *text) {
  if (!text) return false;
  const char *p = text;
  while (*p == ' ' || *p == '\t') ++p;
  // Only strip a leading wake phrase in a wake-confirmed interruption.
  // Chinese ASR may attach the following question without punctuation.
  const size_t wake_len = strlen(APP_WAKE_PHRASE);
  if (!strncmp(p, APP_WAKE_PHRASE, wake_len)) {
    p += wake_len;
    while (*p) {
      if (strchr(" ,.!?:;\t\r\n", *p)) ++p;
      else if (!strncmp(p, "，", 3) || !strncmp(p, "。", 3) ||
               !strncmp(p, "！", 3) || !strncmp(p, "？", 3) ||
               !strncmp(p, "：", 3)) p += 3;
      else break;
    }
    memmove(text, p, strlen(p) + 1);
    p = text;
    if (!*p) return true;
  }
  // Short ambiguous phrases are stop-only controls only as whole utterances.
  // Pass the complete "等一下会不会下雨" utterance to semantic handling.
  const char *stops[] = {"停一下", "等一下", "先别说了"};
  for (unsigned i = 0; i < sizeof(stops) / sizeof(*stops); ++i)
    if (phrase(p, stops[i])) { text[0] = '\0'; return true; }
  const char *words[] = {"打断一下", "停止回答", "先停一下", "暂停回答"};
  for (unsigned i = 0; i < sizeof(words) / sizeof(*words); ++i) {
    size_t n = strlen(words[i]);
    if (strncmp(p, words[i], n)) continue;
    p += n;
    while (*p) {
      if (strchr(" ,.!?\t\r\n", *p)) ++p;
      else if (!strncmp(p, "，", 3) || !strncmp(p, "。", 3) ||
               !strncmp(p, "！", 3) || !strncmp(p, "？", 3)) p += 3;
      else break;
    }
    memmove(text, p, strlen(p) + 1);
    return !*text;
  }
  return false;
}
static char *setting_answer(const char *setting, unsigned value, const char *text) {
  cJSON *args = cJSON_CreateObject();
  bool ok = args && cJSON_AddStringToObject(args, "setting", setting) &&
      cJSON_AddNumberToObject(args, "value", value) && cJSON_AddStringToObject(args, "request", text);
  char *arguments = ok ? cJSON_PrintUnformatted(args) : NULL;
  cJSON_Delete(args);
  char *reply = arguments ? app_control_voice_execute("set_local_setting", arguments, text) : NULL;
  free(arguments);
  return voice_answer(reply);
}
static const char *command_start(const char *text) {
  while (*text == ' ' || *text == '\t') ++text;
  const char *polite[] = {"请帮我", "帮我", "请"};
  for (unsigned i = 0; i < sizeof(polite) / sizeof(*polite); ++i) {
    size_t n = strlen(polite[i]);
    if (!strncmp(text, polite[i], n)) { text += n; break; }
  }
  while (*text == ' ' || *text == '\t') ++text;
  return text;
}
bool app_control_end_session(const char *text) {
  if (!text) return false;
  while (*text == ' ' || *text == '\t') ++text;
  const size_t wake_len = strlen(APP_WAKE_PHRASE);
  if (!strncmp(text, APP_WAKE_PHRASE, wake_len)) {
    text += wake_len;
    while (*text) {
      if (strchr(" ,.!?:;\t\r\n", *text)) ++text;
      else if (!strncmp(text, "，", 3) || !strncmp(text, "。", 3) ||
               !strncmp(text, "：", 3) || !strncmp(text, "！", 3) ||
               !strncmp(text, "？", 3)) text += 3;
      else break;
    }
  }
  text = command_start(text);
  const char *ends[] = {"结束对话", "结束语音对话", "退出语音", "退出对话",
                        "不聊了", "先不聊了", "退下吧"};
  for (size_t i = 0; i < sizeof(ends) / sizeof(*ends); ++i)
    if (phrase(text, ends[i])) return true;
  return false;
}
static bool volume_number(const char *text, unsigned *out) {
  // Strict templates only: questions/negations/extra clauses go to the model.
  if (!strncmp(text, "百分之", strlen("百分之"))) text += strlen("百分之");
  const char *p = text;
  unsigned value = 0;
  if (*p >= '0' && *p <= '9') {
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (unsigned)(*p++ - '0');
      if (value > 100) return false;
    }
  } else {
    const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (!strncmp(p, "一百", 6)) { value = 100; p += 6; }
    else {
      unsigned first = 0;
      bool found = false;
      for (unsigned i = 0; i < 10; ++i) if (!strncmp(p, digits[i], 3)) {
        first = i; found = true; p += 3; break;
      }
      if (!strncmp(p, "十", 3)) {
        value = (found ? first : 1) * 10; p += 3;
        for (unsigned i = 1; i < 10; ++i) if (!strncmp(p, digits[i], 3)) {
          value += i; p += 3; break;
        }
      } else if (found) value = first;
      else return false;
    }
  }
  if (*p == '%') ++p;
  if (!phrase(p, "") || value > 100) return false;
  *out = value;
  return true;
}
char *app_control_voice_command(const char *text) {
  if (!text) return NULL;
  // Exact next-turn confirmation; model arguments can never grant authority.
  if (app_control_voice_pending()) {
    if (phrase(text, "取消执行")) {
      app_control_voice_cancel(); return app_psram_strdup("已取消待确认的本机操作。");
    }
    if (phrase(text, "确认执行")) {
      s_voice_deadline = 0;
      return voice_answer(voice_execute(s_voice_name, s_voice_args, text, true));
    }
  }
  // Common unambiguous controls have a verified local receipt in one hop.
  // Other requests use semantic tool selection.
  const char *direct = command_start(text);
  const char *clock[] = {"恢复钟表主页", "恢复成钟表样式", "恢复钟表样式", "恢复时钟主页",
      "切回钟表", "切回时钟", "切回钟表样式", "恢复原来的钟表", "恢复原来的钟表主页",
      "把主页恢复成钟表样式", "将主页恢复成钟表样式", "主页恢复成钟表样式", "把主页切回钟表", "主页切回钟表"};
  for (unsigned i = 0; i < sizeof(clock) / sizeof(*clock); ++i)
    if (phrase(direct, clock[i])) return setting_answer("home_style", 0, text);
  const char *volume[] = {"音量调到", "把音量调到", "将音量调到", "音量设置为", "把音量设置为", "音量设为"};
  for (unsigned i = 0; i < sizeof(volume) / sizeof(*volume); ++i) {
    size_t n = strlen(volume[i]); unsigned value;
    if (!strncmp(direct, volume[i], n) && volume_number(direct + n, &value))
      return setting_answer("volume", value, text);
  }
  if (phrase(direct, "调节音量") || phrase(direct, "调整音量"))
    return app_psram_strdup("你想把音量调到百分之多少？");
  int step = phrase(direct, "音量调大一点") || phrase(direct, "调大音量") || phrase(direct, "提高音量") ? 10 :
      phrase(direct, "音量调小一点") || phrase(direct, "调小音量") || phrase(direct, "降低音量") ? -10 : 0;
  if (step) {
    app_desktop_config_t d;
    if (!app_desktop_configuration(&d, NULL)) return app_psram_strdup("界面忙，音量未调整。");
    int value = (int)app_audio_get_volume() + step;
    return setting_answer("volume", (unsigned)(value < 0 ? 0 : value > 100 ? 100 : value), text);
  }
  static const struct { const char *phrase, *action; unsigned value; } commands[] = {
    {"播放音乐", "play", 0}, {"播放本地音乐", "play", 0}, {"开始播放音乐", "play", 0},
    {"暂停音乐", "pause", 0}, {"继续音乐", "resume", 0}, {"继续播放音乐", "resume", 0},
    {"停止音乐", "stop", 0}, {"上一首", "previous", 0}, {"下一首", "next", 0},
    {"打开单曲循环", "repeat", 1}, {"关闭单曲循环", "repeat", 0}};
  for (unsigned i = 0; i < sizeof(commands) / sizeof(*commands); ++i) {
    if (!phrase(text, commands[i].phrase)) continue;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "action", commands[i].action);
    cJSON_AddNumberToObject(args, "value", commands[i].value);
    if (!strcmp(commands[i].action, "play")) {
      app_media_snapshot_t *view = heap_caps_malloc(sizeof(*view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!view) { cJSON_Delete(args); return app_psram_strdup("媒体目录内存不足。"); }
      app_media_snapshot(view);
      unsigned selected = view->audio_count;
      if (view->audio_index < view->audio_count && !app_media_audio_is_ambient(view->audio[view->audio_index])) selected = view->audio_index;
      else for (unsigned n = 0; n < view->audio_count; ++n)
        if (!app_media_audio_is_ambient(view->audio[n])) { selected = n; break; }
      if (selected < view->audio_count) cJSON_AddStringToObject(args, "file", view->audio[selected]);
      heap_caps_free(view);
    }
    char *arguments = cJSON_PrintUnformatted(args); cJSON_Delete(args);
    char *json = arguments ? voice_execute("control_media", arguments, text, false) : NULL;
    free(arguments); return voice_answer(json);
  }
  const char *action = phrase(text, "开始专注") || phrase(text, "开始番茄钟") ? "focus_start" :
      phrase(text, "暂停专注") ? "focus_pause" : phrase(text, "继续专注") ? "focus_resume" :
      phrase(text, "停止专注") ? "focus_stop" : NULL;
  if (!action) return NULL;
  char args[96]; snprintf(args, sizeof(args), "{\"action\":\"%s\",\"target\":\"pomodoro\",\"minutes\":0}", action);
  return voice_answer(voice_execute("control_desktop", args, text, false));
}
bool app_control_voice_finish(bool success) {
  bool finish = s_after.end_session;
  if (!success) {
    bool show_applied_home = s_after.home_applied && !app_cloud_is_cancelled();
    memset(&s_after, 0, sizeof(s_after));
    if (!show_applied_home) return false;
    // Only reveal the setting already verified by readback. Never execute a
    // queued page/media command after a failed or cancelled response.
    snprintf(s_after.action, sizeof(s_after.action), "%s", "open");
    snprintf(s_after.target, sizeof(s_after.target), "%s", "home");
    finish = true;
    ESP_LOGW("control", "Home setting already applied (style=%u); voice reply failed, showing actual home",
             app_desktop_home_style());
  }
  if (!finish) return false;
  bool ok = true;
  if (s_after.action[0]) {
    if (!lvgl_port_lock(300)) ok = false;
    else {
      // LVGL's existing lock is recursive. Clear the overlay and switch the
      // page together within this bounded lock acquisition.
      app_ui_hide_voice_state();
      ok = app_ui_open_destination(s_after.target);
      lvgl_port_unlock();
    }
  } else app_ui_hide_voice_state();
  memset(&s_after, 0, sizeof(s_after));
  if (!ok) app_ui_show_voice_state(APP_UI_VOICE_ERROR, "页面操作未完成，请重试");
  return true;
}
