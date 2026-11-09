#pragma once
// 四格分页的环形游标。末页不足四项时跳过空格，任何方向都不会卡在边缘。
static inline int app_menu_grid_move(int selected, unsigned count, int step) {
  if (!count) return 0;
  int next = (selected + step) % (int)count;
  return next < 0 ? next + (int)count : next;
}
