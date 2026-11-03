/*
 * 固定对象池、30fps呈现/100Hz物理；无网络、音频、逐帧分配。
 */
#include "app_games.h"
#include "app_games_core.h"
#include "app_ui_theme.h"
#include "app_motion.h"
#include "app_ui.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct { app_button_event_t event; uint32_t at; } input_t;
static StaticQueue_t queue_control;
static uint8_t queue_data[12 * sizeof(input_t)];
static QueueHandle_t queue;
static _Atomic bool active, overflow;
static app_game_t game;
static lv_obj_t *root, *title, *body, *footer, *value, *cue, *player, *ball;
static lv_obj_t *shots[GAME_SHOTS], *hazards[GAME_HAZARDS], *enemies[GAME_ENEMIES];
static uint32_t last_tick;
static bool release_action, leave;
static bool flush_registered;
static const char *names[] = {"测反应", "弹球", "太空飞机"};
static char title_text[120], body_text[220], footer_text[120], value_text[40];
static void show(lv_obj_t *o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static void text(lv_obj_t *o, const char *v) {
  char *out = o == title ? title_text : o == body ? body_text : o == footer ? footer_text : value_text;
  size_t size = o == title ? sizeof(title_text) : o == body ? sizeof(body_text) :
                o == footer ? sizeof(footer_text) : sizeof(value_text);
  snprintf(out, size, "%s", v);
}
static void commit_text(lv_obj_t *o, const char *v) {
  if (strcmp(lv_label_get_text(o), v)) lv_label_set_text(o, v);
}
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void flush_started(lv_event_t *event) {
  const lv_area_t *area = lv_event_get_param(event);
  // 提示中心所在分块开始交给SPI时记时，不把下一帧等待算进成绩。
  // 这不是光学测量：LCD扫描/像素响应仍存在误差，结果只显示到10ms。
  if (active && area && area->y1 <= 125 && area->y2 >= 125 &&
      area->x1 <= 120 && area->x2 >= 120)
    app_game_cue_submitted(&game, now_ms());
}
bool app_games_active(void) { return atomic_load(&active); }
bool app_games_button(app_button_event_t event) {
  input_t input = {event, event == APP_BUTTON_EVENT_PRESS_DOWN ?
                          app_button_press_time_ms() : now_ms()};
  if (queue && xQueueSend(queue, &input, 0) != pdTRUE) atomic_store(&overflow, true);
  return event != APP_BUTTON_EVENT_PRESS_DOWN;
}
static void place(lv_obj_t *o, const game_body_t *b, int half) {
  show(o, b->active);
  if (b->active) lv_obj_set_pos(o, (int)b->x-half, (int)b->y-half);
}
static void render(void) {
  char line[120], detail[220];
  bool playing = game.phase == GAME_PLAY;
  bool reaction = game.kind == GAME_REACTION;
  show(cue, reaction && playing);
  show(player, !reaction && playing);
  show(ball, game.kind == GAME_PADDLE && playing);
  for (unsigned i=0; i<GAME_SHOTS; ++i) {
    place(shots[i], &game.shots[i], 2); if (!playing) show(shots[i], false);
  }
  for (unsigned i=0; i<GAME_HAZARDS; ++i) {
    place(hazards[i], &game.hazards[i], 3); if (!playing) show(hazards[i], false);
  }
  for (unsigned i=0; i<GAME_ENEMIES; ++i) {
    place(enemies[i], &game.enemies[i], 9); if (!playing) show(enemies[i], false);
  }
  text(title, names[game.kind]); text(body, ""); text(footer, ""); text(value, "");
  if (game.phase == GAME_INTRO) {
    const char *instructions[] = {
      "等绿色提示出现再按键\n抢跑不计入成绩\n五轮后显示中位数",
      "左右倾斜移动挡板\n接住侧边发出的球\n击球位置决定反弹方向",
      "上下左右倾斜移动\n单击发射，躲避红色子弹\n三段关卡，生存六十秒"};
    text(body, instructions[game.kind]);
    text(footer, "单击开始 · 长按返回");
  } else if (game.phase == GAME_PAUSED) {
    text(value, "II");
    text(body, game.sensor_fault ? "倾斜传感器不可用\n恢复后单击继续" :
               game.slow_frame ? "系统繁忙，已暂停\n单击继续" : "已暂停\n放平设备，再继续");
    text(footer, "单击继续 · 长按返回");
  } else if (game.phase == GAME_RESULT) {
    if (reaction) {
      snprintf(line, sizeof(line), "%u ms", (game.median + 5) / 10 * 10);
      snprintf(detail, sizeof(detail), "五轮中位数\n最快 %u ms · 抢跑 %u 次\n非专业测量，10ms分档",
               (game.best + 5) / 10 * 10, game.false_starts);
    } else {
      snprintf(line, sizeof(line), "%u", game.score);
      snprintf(detail, sizeof(detail), "%s\n%s", game.lives ? "任务完成" : "本局结束",
               game.kind == GAME_PADDLE ? "接球得分 · 挑战下一局" : "击落得分 · 再试一次");
    }
    text(value, line); text(body, detail); text(footer, "单击重玩 · 长按返回");
  } else if (reaction) {
    bool go = game.reaction == REACTION_DRAW || game.reaction == REACTION_GO;
    unsigned color = go ? HOLO_MINT : HOLO_CORAL;
    lv_obj_set_style_bg_color(cue, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(cue, go ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(cue, lv_color_hex(color), 0);
    snprintf(line, sizeof(line), "测反应  %u / 5", game.rounds < 5 ? game.rounds+1 : 5);
    text(title, line);
    if (game.reaction == REACTION_SCORE) {
      show(cue, false);
      if (game.early) text(value, "!");
      else if (game.missed) text(value, "--");
      else { snprintf(line,sizeof(line),"%u ms", (unsigned)((game.results[game.rounds-1]+5)/10*10)); text(value,line); }
      text(body, game.early ? "抢跑了，不计入成绩" : game.missed ? "超时，请再试一次" : "本轮反应时间");
      text(footer, game.rounds == 5 ? "单击查看汇总" : "单击下一轮 · 长按暂停");
    } else {
      text(footer, go ? "现在按键！" : "等待绿色 · 不要提前按");
    }
  } else {
    snprintf(line, sizeof(line), game.kind == GAME_PADDLE ? "接球 %u     机会 %u" : "得分 %u     生命 %u", game.score, game.lives);
    text(title, line);
    if (game.kind == GAME_PADDLE) {
      lv_obj_set_pos(player, (int)game.x-27, 207);
      place(ball, &game.ball, 4);
      if (game.invincible) text(footer, "侧边发球准备");
    } else {
      lv_obj_set_pos(player, (int)game.x-12, (int)game.y-12);
      show(player, !game.invincible || (game.elapsed/120)%2 == 0);
      snprintf(line,sizeof(line),"第%u关   剩余%us", game.level, (unsigned)((60000-game.elapsed+999)/1000));
      text(footer,line);
    }
  }
  // 一帧只提交最终文本，未变化的HUD不失效、不重复重绘。
  commit_text(title,title_text); commit_text(body,body_text);
  commit_text(footer,footer_text); commit_text(value,value_text);
}
void app_games_open(lv_obj_t *parent, unsigned kind) {
  app_games_close();
  if (!queue) queue = xQueueCreateStatic(12, sizeof(input_t), queue_data, &queue_control);
  xQueueReset(queue); atomic_store(&overflow, false);
  app_game_init(&game, kind, esp_random());
  root = holo_rect(parent, 0, 0, 240, 240, HOLO_INK, 0, true, 0);
  // 稀疏固定星点；不全屏重绘、不占用位图缓冲。
  for (unsigned i=0; i<8; ++i)
    holo_rect(root, 12+(i*47)%214, 45+(i*37)%154, 2, 2, 0x52645C, 0, true, 0);
  cue = holo_rect(root, 24, 65, 192, 120, HOLO_CORAL, 12, false, 4);
  player = holo_rect(root, 93, 207, kind == GAME_PADDLE ? 54 : 24,
                     kind == GAME_PADDLE ? 8 : 24, HOLO_APRICOT, 3, kind == GAME_PADDLE, 0);
  if (kind == GAME_SPACE) {
    holo_rect(player, 9, 0, 6, 22, HOLO_MINT, 2, true, 0);
    holo_rect(player, 1, 12, 22, 5, HOLO_MINT, 2, true, 0);
    holo_rect(player, 7, 20, 10, 3, HOLO_APRICOT, 1, true, 0);
    holo_rect(player, 11, 11, 3, 3, 0xFFFFFF, 1, true, 0);
  }
  ball = holo_rect(root, 0, 0, 8, 8, HOLO_CREAM, LV_RADIUS_CIRCLE, true, 0);
  for(unsigned i=0;i<GAME_SHOTS;++i) shots[i]=holo_rect(root,0,0,4,9,HOLO_MINT,2,true,0);
  for(unsigned i=0;i<GAME_HAZARDS;++i) hazards[i]=holo_rect(root,0,0,6,6,HOLO_CORAL,3,true,0);
  for(unsigned i=0;i<GAME_ENEMIES;++i) {
    enemies[i]=holo_rect(root,0,0,18,16,HOLO_CORAL,4,false,2);
    holo_rect(enemies[i],6,5,6,5,HOLO_CORAL,1,true,0);
  }
  title=holo_label(root,"",&app_ui_font_20,HOLO_CREAM,5,5,230,LV_TEXT_ALIGN_CENTER);
  value=holo_label(root,"",&lv_font_montserrat_32,HOLO_APRICOT,8,55,224,LV_TEXT_ALIGN_CENTER);
  body=holo_label(root,"",&app_ui_font_16,HOLO_CREAM,12,106,216,LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(body,LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_line_space(body,7,0);
  footer=holo_label(root,"",&app_ui_font_16,HOLO_CREAM,2,220,236,LV_TEXT_ALIGN_CENTER);
  lv_display_add_event_cb(lv_display_get_default(),flush_started,LV_EVENT_FLUSH_START,NULL);
  flush_registered=true;
  last_tick=now_ms(); release_action=leave=false;
  app_motion_set_enabled(false); // 保留20ms连续采样，冻结菜单手势和后台中位漂移。
  atomic_store(&active,true); render();
}
void app_games_close(void) {
  if (atomic_exchange(&active,false)) app_motion_set_enabled(true);
  if (flush_registered) {
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(),flush_started,NULL);
    flush_registered=false;
  }
  if (root) { lv_obj_delete(root); root=NULL; }
}
bool app_games_tick(void) {
  if (!app_games_active()) return false;
  uint32_t now=now_ms(), elapsed=now-last_tick; last_tick=now;
  input_t in;
  if (atomic_exchange(&overflow,false)) { game.phase=GAME_PAUSED; game.slow_frame=true; }
  while(xQueueReceive(queue,&in,0)==pdTRUE) {
    app_ui_note_activity();
    if (in.event==APP_BUTTON_EVENT_PRESS_DOWN) {
      release_action=game.phase!=GAME_PLAY || (game.kind==GAME_REACTION && game.reaction==REACTION_SCORE);
      if (!release_action) app_game_press(&game,in.at);
    } else if(in.event==APP_BUTTON_EVENT_LONG_PRESS_START) {
      release_action=false; game.firing=false;
      if(game.phase==GAME_PLAY) { game.phase=GAME_PAUSED; game.sensor_fault=game.slow_frame=false; }
      else leave=true;
    } else if(in.event==APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE) {
      game.firing=false;
      if(release_action) {
        if(game.phase==GAME_PAUSED) {
          if(game.kind==GAME_REACTION) { game.reaction=REACTION_SCORE; game.phase=GAME_PLAY; app_game_start(&game,now); }
          else if(app_motion_available()) { game.phase=GAME_PLAY; game.sensor_fault=game.slow_frame=false; }
        } else app_game_start(&game,now);
      }
      release_action=false;
    } else if(in.event==APP_BUTTON_EVENT_LONG_PRESS_END) game.firing=false;
  }
  int x=0,y=0;
  bool sensor=app_motion_sample(&x,&y);
  app_game_step(&game,elapsed,x,y,sensor);
  app_game_reaction_tick(&game,now);
  if (game.phase==GAME_PLAY) app_ui_note_activity();
  render();
  return leave;
}
