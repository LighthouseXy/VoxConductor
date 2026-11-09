#pragma once
#include <stdbool.h>
#include <stdint.h>
#define APP_WORD_COUNT 24
#define APP_WORD_BATCH 6
typedef struct { const char *word, *meaning, *example; } app_word_t;
typedef struct {
  uint32_t mastered, repeated;
  unsigned cursor, queue[APP_WORD_BATCH * 2], count, at, remembered;
  bool revealed;
} app_words_t;
const app_word_t *app_words_current(const app_words_t *s);
void app_words_begin(app_words_t *s);
bool app_words_grade(app_words_t *s, bool remembered);
unsigned app_words_mastered(const app_words_t *s);
