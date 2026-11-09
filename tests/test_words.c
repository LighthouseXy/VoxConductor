#include "app_words.h"
#include "app_menu_grid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  app_words_t s = {0};
  assert(!app_words_current(&s));
  app_words_begin(&s);
  assert(s.count == 6 && s.cursor == 6);
  assert(!strcmp(app_words_current(&s)->word, "focus"));
  assert(!app_words_grade(&s, true)); // 不允许未翻卡就记入掌握。
  for (unsigned i = 0; i < 6; ++i) {
    s.revealed = true;
    assert(app_words_grade(&s, false));
  }
  assert(s.count == 12 && app_words_mastered(&s) == 0);
  for (unsigned i = 0; i < 6; ++i) {
    s.revealed = true;
    assert(app_words_grade(&s, true));
  }
  assert(!app_words_current(&s) && app_words_mastered(&s) == 6);
  assert(!app_words_grade(&s, true));
  app_words_begin(&s);
  assert(s.queue[0] == 6); // 下一组优先未掌握词。
  app_words_t restored = {.mastered=s.mastered, .cursor=s.cursor};
  app_words_begin(&restored);
  assert(app_words_mastered(&restored) == 6);
  s.mastered = 0xffffffff; s.cursor = 1000;
  app_words_begin(&s);
  assert(app_words_mastered(&s) == 24 && s.count == 6);
  for (unsigned i = 0; i < 12; ++i) {
    s.revealed = true;
    assert(app_words_grade(&s, false));
  }
  assert(!app_words_current(&s) && s.count == 12); // 错词不会无限追加。
  for (unsigned count = 1; count <= 38; ++count)
    for (int pos = 0; pos < (int)count; ++pos)
      for (int step = -2; step <= 2; ++step) {
        if (!step) continue;
        int next = app_menu_grid_move(pos, count, step);
        assert(next >= 0 && next < (int)count);
      }
  assert(app_menu_grid_move(3, 8, 1) == 4);
  assert(app_menu_grid_move(4, 8, -1) == 3);
  assert(app_menu_grid_move(2, 8, 2) == 4);
  assert(app_menu_grid_move(4, 8, -2) == 2);
  assert(app_menu_grid_move(0, 8, -2) == 6);
  puts("PASS: word reveal/grading/review bounds/progress restore and four-cell navigation");
}
