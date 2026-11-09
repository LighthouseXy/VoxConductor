#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { GAME_REACTION, GAME_PADDLE, GAME_SPACE };
typedef enum { GAME_INTRO, GAME_PLAY, GAME_PAUSED, GAME_RESULT } game_phase_t;
typedef enum { REACTION_WAIT, REACTION_DRAW, REACTION_GO, REACTION_SCORE } reaction_phase_t;
#define GAME_SHOTS 8
#define GAME_HAZARDS 12
#define GAME_ENEMIES 4
typedef struct { float x, y, vx, vy; bool active; } game_body_t;
typedef struct {
  unsigned kind, score, lives, level, hits;
  game_phase_t phase;
  uint32_t rng, elapsed, accumulator, shot_at, spawn_at, volley_at, invincible;
  bool firing, sensor_fault, slow_frame;
  float x, y;
  game_body_t ball, shots[GAME_SHOTS], hazards[GAME_HAZARDS], enemies[GAME_ENEMIES];
  reaction_phase_t reaction;
  uint32_t deadline, cue_at, results[5];
  unsigned rounds, false_starts, best, median;
  bool early, missed;
} app_game_t;
void app_game_init(app_game_t *g, unsigned kind, uint32_t seed);
void app_game_start(app_game_t *g, uint32_t now);
void app_game_step(app_game_t *g, unsigned elapsed_ms, int x_mg, int y_mg, bool sensor);
void app_game_reaction_tick(app_game_t *g, uint32_t now);
void app_game_cue_submitted(app_game_t *g, uint32_t now);
void app_game_press(app_game_t *g, uint32_t edge_ms);
// 去死区后的连续速度，约18度倾斜达到最大速度，无菜单手势冷却。
float app_game_axis(int mg);
