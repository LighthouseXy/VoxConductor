#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "esp_err.h"
#include "app_turn.h"
bool app_cloud_local_ready(void);

// 单个语音任务串行调用；返回的文字由调用者 free()。
esp_err_t app_cloud_init(void);
void app_cloud_cancel(void);
void app_cloud_reset_cancel(void);
bool app_cloud_is_cancelled(void);
// Set history provenance even when an exact local command bypasses the LLM.
void app_cloud_begin_turn(const app_turn_t *turn);
// Voice interruption may resume; explicit user cancellation always wins.
bool app_cloud_barge_in(void);
bool app_cloud_resume_barge_in(void);
// Follow-up idle waiting stays local; NOT_FOUND reports quiet-window expiry.
// pcm由调用者分配并持有；函数返回前已结束采集任务和WSS回调。
// 成功时text是独立分配的UTF-8字符串，调用者free；失败时不转移PCM所有权。
esp_err_t app_cloud_listen(int16_t *pcm, size_t *samples, char **text,
                           app_turn_t *turn, bool followup);
esp_err_t app_cloud_transcribe(const int16_t *pcm, size_t samples, char **text, app_turn_t *turn);
// 当前生产路径：完整接收答案/工具调用并释放LLM连接，再调用speak。
// answer成功时由调用者free；不能把首token时间当成已开始扬声器播报。
esp_err_t app_cloud_ask(const char *text, char **answer, app_turn_t *turn);
esp_err_t app_cloud_speak(const char *text, app_turn_t *turn);
// 非阻塞拉取下一段文本，0字节表示暂未到达；final表示此后不再有输入。
// 回调只由TTS输入任务调用，最多返回480字节，输出必须以NUL结尾。
typedef esp_err_t (*app_tts_feed_t)(void *context, char text[481], bool *final);
esp_err_t app_cloud_speak_stream(app_tts_feed_t feed, void *context,
                                 app_turn_t *turn, const _Atomic bool *abort);
esp_err_t app_cloud_speak_reminder(const char *text, app_turn_t *turn);
// 仅在完整播放成功后提交本轮，避免下一轮引用未播完的回答。
esp_err_t app_cloud_commit_turn(const char *text, const char *answer);
void app_cloud_remember_interrupted(const char *text, const char *answer, size_t played_bytes);
// Main coordinator calls tick under the same exclusive gate as dialogue/tools.
// interrupt只置取消标志，后台自行清理TLS；busy为false后前台才能联网。
void app_cloud_memory_tick(void);
void app_cloud_summary_interrupt(void);
bool app_cloud_summary_busy(void);

// Current coordinator-owned turn only; background operations use zero.
int64_t app_cloud_deadline_us(void);
