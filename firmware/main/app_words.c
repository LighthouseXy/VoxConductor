/*
 * 离线词卡：每组6词，未掌握优先；忘记的词在本组末尾再练一次。
 * mastered是用户自评，不代表考试能力；存储与界面由桌面模块负责。
 */
#include "app_words.h"
#include <stddef.h>
static const app_word_t words[APP_WORD_COUNT] = {
  {"focus", "专注；焦点", "Focus on one task."},
  {"habit", "习惯", "Build a good habit."},
  {"create", "创造", "Create something new."},
  {"simple", "简单的", "Keep it simple."},
  {"learn", "学习", "Learn every day."},
  {"improve", "改进；改善", "Practice to improve."},
  {"patient", "耐心的", "Be patient with me."},
  {"choose", "选择", "Choose your own path."},
  {"notice", "注意到", "Notice small changes."},
  {"balance", "平衡", "Balance work and rest."},
  {"explore", "探索", "Explore the world."},
  {"prepare", "准备", "Prepare for tomorrow."},
  {"enjoy", "享受；喜欢", "Enjoy your day."},
  {"quiet", "安静的", "Find a quiet place."},
  {"share", "分享", "Share your ideas."},
  {"reason", "原因；理由", "Tell me the reason."},
  {"discover", "发现", "Discover a new idea."},
  {"reduce", "减少", "Reduce screen time."},
  {"connect", "连接", "Connect the two parts."},
  {"design", "设计", "Design a small clock."},
  {"support", "支持", "Support your friends."},
  {"curious", "好奇的", "Stay curious."},
  {"progress", "进步；进展", "Make steady progress."},
  {"complete", "完成；完整的", "Complete this task."},
};
const app_word_t *app_words_current(const app_words_t *s) {
  return s && s->at < s->count && s->queue[s->at] < APP_WORD_COUNT
      ? &words[s->queue[s->at]] : NULL;
}
unsigned app_words_mastered(const app_words_t *s) {
  unsigned n = 0;
  for (unsigned i = 0; i < APP_WORD_COUNT; ++i) n += !!(s->mastered & (1u << i));
  return n;
}
void app_words_begin(app_words_t *s) {
  s->count = s->at = s->remembered = 0;
  s->revealed = false; s->repeated = 0;
  s->mastered &= (1u << APP_WORD_COUNT) - 1;
  unsigned start = s->cursor % APP_WORD_COUNT;
  for (unsigned pass = 0; pass < 2 && s->count < APP_WORD_BATCH; ++pass)
    for (unsigned i = 0; i < APP_WORD_COUNT && s->count < APP_WORD_BATCH; ++i) {
      unsigned at = (start + i) % APP_WORD_COUNT;
      if (!!(s->mastered & (1u << at)) == !!pass) s->queue[s->count++] = at;
    }
  s->cursor = (s->queue[s->count - 1] + 1) % APP_WORD_COUNT;
}
bool app_words_grade(app_words_t *s, bool remembered) {
  if (!app_words_current(s) || !s->revealed) return false;
  uint32_t bit = 1u << s->queue[s->at];
  if (remembered) { s->mastered |= bit; ++s->remembered; }
  else {
    s->mastered &= ~bit;
    if (!(s->repeated & bit) && s->count < APP_WORD_BATCH * 2) {
      s->queue[s->count++] = s->queue[s->at];
      s->repeated |= bit;
    }
  }
  ++s->at; s->revealed = false;
  return true;
}
