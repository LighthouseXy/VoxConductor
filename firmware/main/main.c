/*
 * 应用协调入口：初始化外设，组织UI输入，管理单一语音会话。
 * 语音按录音/ASR、语义工具/回答、TTS顺序执行；播完才提交成功上下文。
 * 后台控制与语音互斥，设置生效和播报完成分别处理，取消不等于回滚。
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_acoustic.h"
#include "app_agent.h"
#include "app_audio.h"
#include "app_button.h"
#include "app_cloud.h"
#include "app_desktop.h"
#include "app_games.h"
#include "app_control.h"
#include "app_log.h"
#include "app_display.h"
#include "app_media.h"
#include "app_media_web.h"
#include "app_motion.h"
#include "app_network.h"
#include "app_storage.h"
#include "app_ui.h"
#include "app_wake.h"
#include "app_wake_diagnostic.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "voxconductor";

static _Atomic bool s_voice_turn_active = false;
// 协调器互斥门和用户可取消的音频会话分开，后台存盘不吞掉本地按键。
static _Atomic bool s_audio_active = false;
static _Atomic bool s_voice_ready = false;
static _Atomic bool s_media_audio_active = false;
static _Atomic unsigned s_deferred_media_releases;
static bool s_menu_long_press;
static bool s_retry_after_release;
static _Atomic int s_pending_motion;
#define WAKE_HANDOFF_MS 2000
// Zero means empty. A deadline coalesces duplicates without renewing a stale
// request; signed subtraction below also handles the uptime-millisecond wrap.
static _Atomic uint32_t s_pending_wake_until;

static void voice_conversation_task(void *argument);
static _Atomic int64_t s_voice_accepted_us;
static void restore_wake(void) {
  app_wake_allow(!atomic_load(&s_media_audio_active) &&
                 !atomic_load(&s_audio_active));
}
static void media_audio_activity(bool active) {
  // Music cannot wake the microphone; voice/reminder owners control it while
  // busy.
  atomic_store(&s_media_audio_active, active);
  restore_wake();
}
static void release_media_audio(void) {
  // Reverse handoff also needs a clean DMA ring. A failed clear keeps music
  // suspended until the coordinator's background retry succeeds.
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (app_audio_abort_playback() == ESP_OK) {
      app_media_resume();
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  atomic_fetch_add(&s_deferred_media_releases, 1);
  ESP_LOGW(TAG, "扬声器清理未完成，音乐保持暂停并等待重试");
}
static void retry_media_release(void) {
  if (!atomic_load(&s_deferred_media_releases) ||
      app_audio_abort_playback() != ESP_OK)
    return;
  unsigned count = atomic_exchange(&s_deferred_media_releases, 0);
  while (count--)
    app_media_resume();
}
static bool start_voice(bool automatic) {
  if (!atomic_load(&s_voice_ready) ||
      atomic_exchange(&s_voice_turn_active, true))
    return false;
  atomic_store(&s_voice_accepted_us, esp_timer_get_time());
  if (!app_media_suspend(1000)) {
    app_media_resume();
    atomic_store(&s_voice_turn_active, false);
    ESP_LOGW(TAG, "音乐尚未释放扬声器，语音未启动");
    return false;
  }
  atomic_store(&s_audio_active, true);
  atomic_store(&s_pending_wake_until, 0);
  app_wake_allow(false);
  app_motion_set_enabled(false);
  app_cloud_summary_interrupt();
  app_cloud_reset_cancel();
  if (automatic)
    app_audio_prepare_auto_recording();
  else
    app_audio_prepare_recording();
  if (xTaskCreate(voice_conversation_task, "voice_turn", 12288,
                  (void *)(uintptr_t)automatic, 6,
                  NULL) != pdPASS) {
    app_motion_set_enabled(true);
    atomic_store(&s_audio_active, false);
    restore_wake();
    atomic_store(&s_voice_turn_active, false);
    app_media_resume();
    return false;
  }
  return true;
}
static void handle_wake(void) {
  if (app_wake_diagnostic_active()) return;
  if (!app_network_is_connected() || time(NULL) < 1704067200) {
    ESP_LOGW(TAG, "Wake ignored: network/time not ready");
    return;
  }
  if (!atomic_load(&s_voice_ready) || !app_wake_is_enabled() ||
      atomic_load(&s_audio_active) || atomic_load(&s_media_audio_active)) {
    ESP_LOGW(TAG, "Wake ignored: audio owner busy/not ready");
    return;
  }
  // The recognizer must keep consuming microphone frames, not wait for LVGL,
  // SD maintenance or the speaker. The existing audio worker dispatches this.
  uint32_t until = (uint32_t)(esp_timer_get_time() / 1000) + WAKE_HANDOFF_MS;
  if (!until) until = 1;
  uint32_t empty = 0;
  if (atomic_compare_exchange_strong(&s_pending_wake_until, &empty, until))
    APP_LOG_DETAIL(TAG, "Wake queued: awaiting UI/voice coordinator");
}
static bool dispatch_wake(void) {
  uint32_t until = atomic_load(&s_pending_wake_until);
  if (!until) return false;
  // A detection queued just before manual calibration must not start ASR.
  if (app_wake_diagnostic_active()) {
    atomic_store(&s_pending_wake_until, 0);
    return true;
  }
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if ((int32_t)(now - until) >= 0) {
    atomic_store(&s_pending_wake_until, 0);
    ESP_LOGW(TAG, "Wake expired: not started within %ums; internal=%u B, largest=%u B",
        (unsigned)WAKE_HANDOFF_MS,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
  }
  if (!app_wake_is_enabled() || !atomic_load(&s_voice_ready) ||
      !app_network_is_connected() || time(NULL) < 1704067200 ||
      atomic_load(&s_audio_active) || atomic_load(&s_media_audio_active)) {
    atomic_store(&s_pending_wake_until, 0);
    ESP_LOGI(TAG, "Pending wake discarded: enabled=%d ready=%d voice=%d network=%d audio=%d media=%d",
             (int)app_wake_is_enabled(), (int)app_wake_available(),
             (int)atomic_load(&s_voice_ready), (int)app_network_is_connected(),
             (int)atomic_load(&s_audio_active), (int)atomic_load(&s_media_audio_active));
    return true;
  }
  // Detection requests a model rebuild. Its brief ready=false is not a user
  // disable: keep the accepted event, within the original bounded deadline.
  if (!app_wake_available())
    return true;
  // Background indexing may briefly own the coordinator without owning audio.
  // Retain this wake, but do not interrupt an actual voice/reminder/music owner.
  if (atomic_load(&s_voice_turn_active) || !lvgl_port_lock(20))
    return true;
  // A button/setting/audio owner can change while this worker waits for LVGL.
  if (atomic_load(&s_pending_wake_until) != until || !app_wake_is_enabled() ||
      !app_wake_available() ||
      atomic_load(&s_voice_turn_active) || atomic_load(&s_audio_active) ||
      atomic_load(&s_media_audio_active) ||
      (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - until) >= 0) {
    lvgl_port_unlock();
    return true;
  }
  bool allowed = app_ui_wake_voice();
  lvgl_port_unlock();
  if (!allowed) {
    atomic_store(&s_pending_wake_until, 0);
    ESP_LOGW(TAG, "Wake ignored: voice overlay active");
    return true;
  }
  // This remains outside the LVGL lock and outside the WakeNet worker. A
  // short coordinator race/allocation failure can retry until the same deadline.
  if (atomic_load(&s_pending_wake_until) == until && app_wake_is_enabled() &&
      app_wake_available() &&
      !atomic_load(&s_audio_active) && !atomic_load(&s_media_audio_active) &&
      start_voice(true)) {
    ESP_LOGI(TAG, "Wake accepted: automatic recording started");
    APP_LOG_DETAIL(TAG, "Wake handoff_stack=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  }
  return true;
}

// Wake handoff runs outside the recognizer and LVGL callback.
static void wake_dispatch_task(void *argument) {
  (void)argument;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(25));
    (void)dispatch_wake();
  }
}

static void service_background_boundary(bool in_session) {
    retry_media_release();
    app_control_service();
    app_desktop_service();
    app_task_notice_t notice = {0};
    bool task_due = app_agent_due_notice(&notice);
    bool focus_due = !task_due && app_desktop_take_focus_notice(notice.title);
    if (task_due || focus_due) {
      if (!in_session && !app_media_suspend(1000)) {
        app_media_resume();
        if (task_due)
          (void)app_agent_task_result(&notice, "failed");
        ESP_LOGW(TAG, "提醒播放未启动：音乐未释放扬声器");
        return;
      }
      atomic_store(&s_audio_active, true);
      app_wake_allow(false);
      app_cloud_summary_interrupt();
      const char *title = notice.title;
      ESP_LOGI(TAG, "本机提醒到期：%s", title);
      app_ui_show_voice_state(APP_UI_VOICE_REMINDER, title);
      if (!app_cloud_is_cancelled()) {
        // 提醒不加入聊天上下文，也不触发LLM；复用同一TTS播放链路。
        app_turn_t turn;
        app_turn_begin(&turn);
        char text[256];
        snprintf(text, sizeof(text), "时间到了，提醒你：%s。", title);
        app_turn_emit(&turn, TURN_TTS_START, NULL, ESP_OK);
        bool cloud = atomic_load(&s_voice_ready) && app_network_is_connected();
        esp_err_t result = cloud ? app_cloud_speak_reminder(text, &turn)
                                 : app_audio_local_reminder();
        if (result != ESP_OK && cloud && !app_cloud_is_cancelled()) {
          (void)app_audio_abort_playback();
          result = app_audio_local_reminder();
        }
        if (app_cloud_is_cancelled()) {
          result = ESP_ERR_INVALID_STATE;
          (void)app_audio_abort_playback();
        }
        if (result == ESP_OK) {
          app_turn_emit(&turn, TURN_PLAY_END, NULL, ESP_OK);
          app_turn_emit(&turn, TURN_TTS_END, NULL, ESP_OK);
        }
        // 这是独立播报，没有录音/ASR/LLM阶段，不能伪造完整对话TURN_DONE。
        if (result != ESP_OK)
          app_turn_emit(&turn,
                        app_cloud_is_cancelled() ? TURN_CANCEL : TURN_ERROR,
                        NULL, result);
        ESP_LOGI(
            TAG,
            "提醒语音：%s，PCM提交=%u B，缓冲等待=%u次/%.2fs（不代表声学确认）",
            esp_err_to_name(result), (unsigned)turn.pcm_submitted_bytes,
            turn.tts_starvations, (double)turn.tts_wait_us / 1000000.0);
        if (result != ESP_OK && !app_cloud_is_cancelled())
          app_ui_show_voice_state(APP_UI_VOICE_REMINDER,
                                  "语音播放失败，请查看提醒");
        if (task_due && !app_agent_task_result(
                            &notice, app_cloud_is_cancelled() ? "cancelled"
                                     : result == ESP_OK       ? "played"
                                                              : "failed"))
          ESP_LOGW(TAG, "提醒结果保存失败，保留已提交状态");
      } else if (task_due)
        (void)app_agent_task_result(&notice, "cancelled");
      for (unsigned i = 0; i < 30 && !app_cloud_is_cancelled(); ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
      app_ui_hide_voice_state();
      atomic_store(&s_audio_active, false);
      if (!in_session) release_media_audio();
      else atomic_store(&s_audio_active, true);
    }
}

static void reminder_task(void *argument) {
  (void)argument;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_voice_turn_active, &expected, true))
      continue;
    app_cloud_reset_cancel();
    service_background_boundary(false);
    // Large SD indexing writes wait for a quiet speaker; due-task checks above
    // still run each second, so a long song does not postpone reminders.
    if (!atomic_load(&s_media_audio_active)) {
      app_cloud_memory_tick();
      app_storage_index_tick();
    }
    restore_wake();
    atomic_store(&s_voice_turn_active, false);
  }
}

static void handle_motion(int direction) {
  // Sampling must never wait for a slow LCD redraw; the LVGL timer navigates.
  // ponytail: one-slot mailbox keeps the newest gesture, not a history of
  // unseen pages; use a FIFO only if every rapid gesture must be retained.
  if (app_button_click_pending() ||
      atomic_load_explicit(&s_audio_active, memory_order_acquire))
    return;
  atomic_store_explicit(&s_pending_motion, direction, memory_order_release);
}
static void dispatch_motion(lv_timer_t *timer) {
  (void)timer;
  int direction = atomic_exchange_explicit(&s_pending_motion, 0, memory_order_acquire);
  if (!direction || app_button_click_pending() ||
      atomic_load_explicit(&s_audio_active, memory_order_acquire))
    return;
  if (app_ui_note_activity())
    return;
  if (!app_ui_detail_motion(direction) && !app_desktop_motion(direction)) {
    if (direction < 0)
      app_ui_show_previous_page();
    else
      app_ui_show_next_page();
  }
}

static void update_mic_level(uint32_t level, void *user_data) {
  (void)user_data;
  app_ui_set_mic_level(level);
}

static bool stop_active_audio(void) {
  if (!atomic_load_explicit(&s_audio_active, memory_order_acquire))
    return false;
  app_cloud_cancel();
  app_acoustic_discard();
  app_audio_request_record_stop();
  ESP_LOGI(
      TAG,
      "按键终止当前对话：等待播放和网络清理");
  return true;
}

static bool handle_button_event(app_button_event_t event, void *user_data) {
  (void)user_data;

  if (app_games_active()) {
    if (event == APP_BUTTON_EVENT_PRESS_DOWN && stop_active_audio())
      return true;
    return app_games_button(event);
  }

  if (event == APP_BUTTON_EVENT_PRESS_DOWN) {
    if (atomic_exchange(&s_pending_wake_until, 0))
      ESP_LOGI(TAG, "Pending wake cancelled by button");
    if (stop_active_audio())
      return true;
    if (lvgl_port_lock(100)) {
      bool woke = app_ui_note_activity();
      lvgl_port_unlock();
      if (woke)
        return true;
    }
    return false;
  }

  if (event == APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE) {
    // Menu navigation waits for single/double-click arbitration. Only an
    // actual physical release may cancel audio started during that window.
    return stop_active_audio();
  }

  if (event == APP_BUTTON_EVENT_SHORT_PRESS) {
    // A reminder may have started during the double-click window. An old menu
    // click must not cancel it; only a physical press/release requests a stop.
    if (atomic_load_explicit(&s_audio_active, memory_order_acquire))
      return true;
    atomic_store(&s_pending_motion, 0);
    // A confirmed click is not a lossy sensor sample: wait for the current
    // frame rather than discarding it after 100 ms. Audio stops remain above
    // this lock, and all speaker/network work stays outside it.
    if (!lvgl_port_lock(0))
      return false;
    if (atomic_load_explicit(&s_audio_active, memory_order_acquire)) {
      lvgl_port_unlock();
      return true;
    }
    if (app_ui_note_activity()) {
      lvgl_port_unlock();
      return true;
    }
    if (!app_desktop_button(false))
      app_ui_enter_page();
    bool retry = app_ui_take_retry();
    lvgl_port_unlock();
    if (retry && !start_voice(true))
      app_ui_show_voice_state(APP_UI_VOICE_ERROR, "系统正忙，请重试");
    return true;
  }

  if (event == APP_BUTTON_EVENT_DOUBLE_PRESS) {
    if (atomic_load_explicit(&s_audio_active, memory_order_acquire))
      return true;
    atomic_store(&s_pending_motion, 0);
    if (!lvgl_port_lock(0))
      return false;
    if (atomic_load_explicit(&s_audio_active, memory_order_acquire)) {
      lvgl_port_unlock();
      return true;
    }
    if (app_ui_note_activity()) {
      lvgl_port_unlock();
      return true;
    }
    int action = app_desktop_back();
    if (action == 0 || action == 3)
      app_ui_back_page();
    lvgl_port_unlock();
    return true;
  }

  if (event == APP_BUTTON_EVENT_LONG_PRESS_END) {
    if (s_menu_long_press) {
      s_menu_long_press = false;
      app_desktop_button_released();
      if (s_retry_after_release) {
        s_retry_after_release = false;
        if (!start_voice(true))
          app_ui_show_voice_state(APP_UI_VOICE_ERROR, "系统正忙，请重试");
      }
      return true;
    }
    // 松开事件可能早于录音任务真正开始，结束请求不能丢失。
    app_audio_request_record_stop();
    APP_LOG_DETAIL(TAG, "长按已松开，请求结束录音");
    return true;
  }

  if (event != APP_BUTTON_EVENT_LONG_PRESS_START)
    return false;
  s_menu_long_press = false;
  s_retry_after_release = false;
  if (stop_active_audio())
    return true;
  atomic_store(&s_pending_motion, 0);
  if (!lvgl_port_lock(0))
    return false;
  if (app_ui_note_activity()) {
    s_menu_long_press = true;
    lvgl_port_unlock();
    return true;
  }
  int action = app_desktop_button(true);
  bool handled = action == 1 || app_ui_handle_long_press();
  bool retry = app_ui_take_retry();
  atomic_store(&s_pending_motion, 0);
  lvgl_port_unlock();
  if (handled) {
    s_menu_long_press = true;
    s_retry_after_release = retry;
    return true;
  }
  if (!atomic_load_explicit(&s_voice_ready, memory_order_acquire)) {
    ESP_LOGW(TAG, "语音系统仍在初始化，请稍后再试");
    return false;
  }

  return start_voice(false);
}

static esp_err_t run_voice_turn(bool automatic, bool followup, bool interrupted) {
  app_acoustic_listen(false);
  app_turn_t turn;
  app_turn_begin(&turn);
  int64_t accepted = atomic_exchange(&s_voice_accepted_us, 0);
  if (accepted > 0) {
    turn.accepted_us = accepted;
    turn.deadline_us = accepted + APP_TURN_BUDGET_US;
  }
  app_cloud_begin_turn(&turn);
  int16_t *recording = NULL;
  char *transcript = NULL;
  char *answer = NULL;
  size_t recorded_samples = 0;
  const char *stage = "网络检查";
  bool end_session = false;
  bool reply_played = false;
  bool context_committed = false;
  esp_err_t result = ESP_OK;
  if (!app_network_is_connected()) {
    result = ESP_ERR_INVALID_STATE;
    goto finish;
  }
  stage = "录音缓冲";
  recording = heap_caps_malloc(APP_AUDIO_RECORD_MAX_BYTE_COUNT,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!recording) {
    result = ESP_ERR_NO_MEM;
    goto finish;
  }
  app_turn_resources(&turn, "录音缓冲已分配");
  // 聆听界面的五根短柱只消费真实麦克风幅度。
  app_ui_show_voice_state(APP_UI_VOICE_LISTENING,
                          automatic ? "正在聆听" : "松开发送");
  stage = "录音/ASR";
  if (app_turn_expired(&turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; goto finish; }
  result = app_cloud_listen(recording, &recorded_samples, &transcript, &turn,
                            followup || interrupted);
  // ASR结束即释放20秒录音缓冲，让后续HTTPS/WSS与播放有更多余量。
  heap_caps_free(recording);
  recording = NULL;
  if ((followup || interrupted) && result == ESP_ERR_NOT_FOUND && !app_cloud_is_cancelled()) {
    // The waiting window is not a failed dialogue or a saved empty turn.
    ESP_LOGI(TAG, "[%s] 结束: 继续聆听超时，无新语音", app_log_id(turn.id));
    app_ui_hide_voice_state();
    app_cloud_begin_turn(NULL);
    return result;
  }

  if (result == ESP_OK) {
    if (interrupted && app_control_strip_interrupt(transcript)) {
      // Stop-only is a control utterance, not an empty LLM request/history turn.
      ESP_LOGI(TAG, "Interrupt phrase accepted; waiting for the next question");
      app_ui_show_voice_state(APP_UI_VOICE_LISTENING, "正在聆听");
      app_turn_emit(&turn, TURN_CANCEL, NULL, ESP_OK);
      free(transcript);
      app_turn_report(&turn);
      app_cloud_begin_turn(NULL);
      return ESP_OK;
    }
    if (app_control_end_session(transcript)) {
      end_session = true;
      atomic_store(&s_pending_wake_until, 0);
      ESP_LOGI(TAG, "[%s] 结束对话指令已接受", app_log_id(turn.id));
      app_cloud_cancel();
      app_acoustic_discard();
      app_control_voice_cancel();
      app_audio_request_record_stop();
      goto finish;
    }
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
    // Reserve DSP while memory is available, but do not classify processing
    // noises as a new utterance. TTS arms it only when playback starts.
    if (app_wake_is_enabled() && !app_acoustic_prepare())
      ESP_LOGW(TAG, "本轮语音打断未就绪，请使用短按中断");
#endif
    app_turn_emit(&turn, TURN_USER_TEXT, transcript, ESP_OK);
    stage = "回答生成";
    app_turn_emit(&turn, TURN_GENERATE_START, NULL, ESP_OK);
    if (app_turn_expired(&turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; goto finish; }
    answer = app_desktop_voice_command(transcript);
    if (!answer)
      result = app_cloud_ask(transcript, &answer, &turn);
  }
  if (result == ESP_OK) {
    app_turn_emit(&turn, TURN_ASSISTANT_TEXT, answer, ESP_OK);
    stage = "TTS";
    if (app_turn_expired(&turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; goto finish; }
    if (turn.times[TURN_TTS_START] < 0) {
      app_turn_emit(&turn, TURN_TTS_START, NULL, ESP_OK);
      result = app_cloud_speak(answer, &turn);
    }
  }
  if (app_cloud_is_cancelled())
    result = ESP_ERR_INVALID_STATE;
  if (result == ESP_OK) {
    app_turn_emit(&turn, TURN_PLAY_END, NULL, ESP_OK);
    app_turn_emit(&turn, TURN_TTS_END, NULL, ESP_OK);
    // 一次只处理一轮；TTS完整结束后再把问答加入上下文。
    reply_played = true;
    if (app_turn_expired(&turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; stage = "会话保存"; goto finish; }
    const esp_err_t context_result = app_cloud_commit_turn(transcript, answer);
    context_committed = context_result == ESP_OK;
    if (context_result != ESP_OK) { result = context_result; stage = "会话保存"; }
    else if (app_turn_expired(&turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; stage = "会话收尾"; }
    if (context_result != ESP_OK)
      ESP_LOGW(TAG, "[%s] 本轮未加入上下文：%s", app_log_id(turn.id),
               esp_err_to_name(context_result));
  }
finish:
  app_acoustic_listen(false);
  if (app_cloud_is_cancelled())
    result = ESP_ERR_INVALID_STATE;
  if (result != ESP_OK) {
    (void)app_audio_abort_playback();
    app_agent_cancel_pending();
    if (!end_session && !reply_played && !app_turn_expired(&turn, esp_timer_get_time()))
      app_cloud_remember_interrupted(transcript, answer, turn.played_text_bytes);
  }
  heap_caps_free(recording);
  app_turn_emit(&turn,
                app_cloud_is_cancelled() ? TURN_CANCEL
                : result == ESP_OK       ? TURN_DONE
                                         : TURN_ERROR,
                NULL, result);
  free(transcript);
  free(answer);
  app_cloud_begin_turn(NULL);
  app_turn_report(&turn);

  if (result != ESP_OK && !app_cloud_is_cancelled()) {
    ESP_LOGE(TAG, "[%s] 阶段=%s，错误=%s", app_log_id(turn.id), stage,
             esp_err_to_name(result));
    app_ui_show_voice_state(APP_UI_VOICE_ERROR,
        context_committed ? "对话已保存，但本轮收尾超时" :
        reply_played ? "回答已播放，但对话保存未确认" :
        app_control_voice_home_applied() ? "主页已更改，语音回答未完成" :
        turn.stages[TURN_GENERATE] == TURN_SUCCEEDED ?
            "回答已生成，语音播报失败" :
        app_network_is_connected() ? "语音请求失败" : "网络未连接");
    // Keep Retry/Return visible, but release audio ownership immediately.
  } else {
    if (app_cloud_is_cancelled() && !app_acoustic_pending()) {
      app_ui_show_voice_state(APP_UI_VOICE_CANCELLED, "对话已结束");
      vTaskDelay(pdMS_TO_TICKS(650));
    }
    if ((!automatic || result != ESP_OK) && !app_acoustic_pending())
      app_ui_hide_voice_state();
  }
  if (app_cloud_is_cancelled())
    APP_LOG_DETAIL(TAG, "[%s] 当前回答已终止，旧TTS已释放", app_log_id(turn.id));
  return result;
}

static void voice_conversation_task(void *argument) {
  bool automatic = (uintptr_t)argument != 0;
  bool continuous = automatic;
  bool followup = false;
  bool finished_control = false;
  bool interrupted = false;
  esp_err_t result;
  app_network_voice_priority(true);

  do {
    result = run_voice_turn(automatic, followup, interrupted);
    finished_control = app_control_voice_finish(result == ESP_OK &&
        !app_cloud_is_cancelled() && !app_acoustic_pending());
    if (finished_control) break;
    if (app_acoustic_pending() && app_cloud_resume_barge_in()) {
      app_audio_prepare_recording();
      automatic = true;
      followup = false;
      continuous = true;
      interrupted = true;
      continue;
    }
    if (!continuous || result != ESP_OK || app_cloud_is_cancelled()) break;
    service_background_boundary(true);
    if (app_cloud_is_cancelled()) break;
    // Same clean speaker handoff as leaving a turn: do not hear our own tail.
    if (app_audio_abort_playback() != ESP_OK) {
      ESP_LOGW(TAG, "Follow-up not started: speaker cleanup failed");
      break;
    }
    ESP_LOGI(TAG, "Follow-up ready: speak directly within %ds",
             APP_AUDIO_FOLLOWUP_WAIT_SECONDS);
    // Keep this session's cancel latch; a press during handoff must still stop.
    atomic_store(&s_voice_accepted_us, esp_timer_get_time());
    app_audio_prepare_auto_recording();
    automatic = true;
    followup = true;
    interrupted = false;
  } while (true);

  if (result == ESP_OK && !finished_control) app_ui_hide_voice_state();
  app_acoustic_discard();
  app_network_voice_priority(false);
  atomic_store(&s_audio_active, false);
  app_motion_set_enabled(true);
  restore_wake();
  release_media_audio();
  atomic_store_explicit(&s_voice_turn_active, false, memory_order_release);
  vTaskDelete(NULL);
}

void app_main(void) {
#if !CONFIG_VOX_LOG_VERBOSE
  // Filter vendor INFO chatter only; never suppress warnings, TLS failures or panics.
  const char *quiet_tags[] = {"wifi", "wifi_init", "esp_netif_handlers",
      "esp-x509-crt-bundle", "websocket_client", "phy_init", "gpio", "LVGL"};
  for (size_t i = 0; i < sizeof(quiet_tags) / sizeof(quiet_tags[0]); ++i)
    esp_log_level_set(quiet_tags[i], ESP_LOG_WARN);
#endif
  ESP_LOGI(TAG, "串口日志: %s模式；每轮从[编号]开始到结束",
           CONFIG_VOX_LOG_VERBOSE ? "详细" : "精简");
  if(!app_control_init()) {ESP_LOGE(TAG,"网页配置队列初始化失败");return;}
  app_desktop_init();
  ESP_ERROR_CHECK(app_display_init());
  ESP_ERROR_CHECK(app_ui_init());
  // One small UI timer, not another FreeRTOS task or a blocking sensor callback.
  if (!lvgl_port_lock(0))
    ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
  lv_timer_t *motion_ui_timer = lv_timer_create(dispatch_motion, 33, NULL);
  lvgl_port_unlock();
  ESP_ERROR_CHECK(motion_ui_timer ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(app_button_init(handle_button_event, NULL));

  const esp_err_t motion_result = app_motion_init(handle_motion);
  if (motion_result != ESP_OK)
    ESP_LOGW(TAG, "摇动翻页未启用，按键仍可使用：%s",
             esp_err_to_name(motion_result));

  const esp_err_t storage_result = app_storage_init();
  const bool storage_ready = storage_result == ESP_OK;
  if (!storage_ready) {
    ESP_LOGW(TAG, "microSD挂载失败，继续启动其他功能：%s",
             esp_err_to_name(storage_result));
  }

  ESP_ERROR_CHECK(app_audio_init());
#if !CONFIG_VOX_ACOUSTIC_BARGE_IN
  ESP_LOGI(TAG, "稳定语音模式：自动声学打断已停用；按键终止，唤醒对话回答结束后继续聆听；小冰小冰仅空闲唤醒");
#else
  ESP_LOGI(TAG, "语音模式：小冰小冰空闲唤醒及回答中打断；按键终止会话");
#endif
  const esp_err_t media_result =
      app_media_init(storage_ready, media_audio_activity);
  if (media_result != ESP_OK)
    ESP_LOGW(TAG, "本地媒体未启动：%s", esp_err_to_name(media_result));
  app_media_web_init();

  const esp_err_t monitor_result =
      app_audio_start_level_monitor(update_mic_level, NULL);
  if (monitor_result != ESP_OK) {
    ESP_LOGE(TAG, "麦克风采集任务创建失败：%s",
             esp_err_to_name(monitor_result));
    return;
  }

  const esp_err_t cloud_result = app_cloud_init();
  ESP_LOGI(TAG, "本地硬件已就绪，正在连接Wi-Fi并校时");

  const esp_err_t network_result = app_network_init();
  if (network_result != ESP_OK) {
    ESP_LOGW(TAG, "网络初始化失败，本地功能继续运行：%s",
             esp_err_to_name(network_result));
  }
  ESP_LOGI(TAG, "启动内存：内部空闲=%u B，最大连续块=%u B，PSRAM空闲=%u B",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // 连接、校时和HTTPS天气由唯一后台owner维护，不阻塞本地提醒/桌面启动。
  if (app_network_start_weather_updates() != ESP_OK)
    ESP_LOGW(TAG, "网络自动维护任务启动失败");
  // Reserve the bounded UI/wake dispatcher before loading the optional model.
  bool wake_dispatch_ready =
      xTaskCreate(wake_dispatch_task, "wake_dispatch", 4096, NULL, 3, NULL) == pdPASS;
  if (wake_dispatch_ready)
    app_wake_init(handle_wake);
  else
    ESP_LOGW(TAG, "音频交接任务创建失败，唤醒不可用，按键语音继续可用");
  atomic_store_explicit(&s_voice_ready, cloud_result == ESP_OK,
                        memory_order_release);
  // Initialize idle detection at boot; existing audio owners still suppress it.
  restore_wake();
  if (!app_cloud_local_ready())
    ESP_LOGW(TAG, "任务上下文不可用，专注仍保留本地服务");
  if (xTaskCreate(reminder_task, "reminder", 8192, NULL, 3, NULL) != pdPASS) {
    ESP_LOGW(TAG, "本地提醒任务创建失败：stack=8192 B，内部空闲=%u B，最大连续块=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  } else {
    APP_LOG_DETAIL(TAG, "本地提醒任务已启动");
  }
  if (cloud_result == ESP_OK)
    ESP_LOGI(TAG,
             "云端直连语音已启用：长按500ms说话，松开发送；无需电脑语音服务");
}
