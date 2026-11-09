#include "app_focus.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  app_focus_t f = {.work_minutes = 25, .break_minutes = 5};
  app_focus_start(&f, 1, false, 1000000);
  assert(app_focus_remaining(&f, 1000000) == 60);
  app_focus_toggle(&f, 11000000);
  assert(!f.running && f.remaining == 50);
  assert(app_focus_remaining(&f, 900000000) == 50);
  app_focus_toggle(&f, 900000000);
  assert(!app_focus_tick(&f, 949000000));
  assert(app_focus_tick(&f, 950000000));
  assert(!app_focus_tick(&f, 960000000));
  assert(!f.running && f.remaining == 0);
  app_focus_start(&f, 25, true, 1000000);
  assert(app_focus_tick(&f, 1501000000));
  assert(f.resting && !f.running && f.remaining == 300);
  app_focus_toggle(&f, 1501000000);
  assert(app_focus_tick(&f, 1801000000));
  assert(!f.resting && !f.running && f.remaining == 1500);

  puts("PASS: focus timing, pause and work/rest transitions");
}
