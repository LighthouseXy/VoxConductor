/*
 * 三款游戏的纯状态逻辑；固定容量、不依赖显示和任务调度，便于主机测试。
 */
#include "app_games_core.h"
#include <math.h>
#include <string.h>

static float clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t random_next(app_game_t *g) {
  uint32_t x = g->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return g->rng = x;
}
float app_game_axis(int mg) {
  float v = (float)mg;
  if (fabsf(v) <= 25) return 0;
  return copysignf(clamp((fabsf(v) - 25) / 275, 0, 1), v);
}
static void serve(app_game_t *g) {
  bool right = random_next(g) & 1;
  float speed = 110 + fminf(g->score * 5, 130);
  g->ball = (game_body_t){right ? 229 : 11, 62, right ? -speed * .65f : speed * .65f,
                         speed * .76f, true};
  g->invincible = 650; // 侧边发球预告，给挡板留出就位时间。
}
void app_game_init(app_game_t *g, unsigned kind, uint32_t seed) {
  memset(g, 0, sizeof(*g));
  g->kind = kind <= GAME_SPACE ? kind : GAME_REACTION;
  g->rng = seed ? seed : 0x6a09e667;
  g->phase = GAME_INTRO; g->x = 120; g->y = 188; g->lives = 3; g->level = 1;
}
void app_game_start(app_game_t *g, uint32_t now) {
  if (g->kind == GAME_REACTION && g->phase == GAME_PLAY && g->reaction == REACTION_SCORE) {
    if (g->rounds == 5) { g->phase = GAME_RESULT; return; }
  } else {
    unsigned kind = g->kind; uint32_t seed = g->rng;
    app_game_init(g, kind, seed);
  }
  g->phase = GAME_PLAY;
  if (g->kind == GAME_REACTION) {
    g->reaction = REACTION_WAIT;
    g->early = g->missed = false;
    g->deadline = now + 1500 + random_next(g) % 3001;
  } else if (g->kind == GAME_PADDLE) serve(g);
}
void app_game_reaction_tick(app_game_t *g, uint32_t now) {
  if (g->phase != GAME_PLAY || g->kind != GAME_REACTION) return;
  if (g->reaction == REACTION_WAIT && (int32_t)(now - g->deadline) >= 0)
    g->reaction = REACTION_DRAW;
  if (g->reaction == REACTION_GO && now - g->cue_at > 3000) {
    g->missed = true; g->reaction = REACTION_SCORE;
  }
}
void app_game_cue_submitted(app_game_t *g, uint32_t now) {
  if (g->kind == GAME_REACTION && g->phase == GAME_PLAY && g->reaction == REACTION_DRAW) {
    g->cue_at = now; g->reaction = REACTION_GO;
  }
}
static void shoot(app_game_t *g) {
  if (g->elapsed < g->shot_at) return;
  for (unsigned i = 0; i < GAME_SHOTS; ++i) if (!g->shots[i].active) {
    g->shots[i] = (game_body_t){g->x, g->y - 12, 0, -270, true};
    break;
  }
  g->shot_at = g->elapsed + 180;
}
void app_game_press(app_game_t *g, uint32_t edge_ms) {
  if (g->phase != GAME_PLAY) return;
  // 即使一次按下/松开都落在同一显示帧内，也至少尝试发射一次。
  if (g->kind == GAME_SPACE) { g->firing = true; shoot(g); return; }
  if (g->kind != GAME_REACTION || g->reaction == REACTION_SCORE) return;
  int32_t delay = (int32_t)(edge_ms - g->cue_at);
  if (g->reaction != REACTION_GO || delay < 80) {
    g->early = true; ++g->false_starts;
  } else if (delay <= 3000 && g->rounds < 5) {
    g->results[g->rounds++] = (uint32_t)delay;
    uint32_t ordered[5]; memcpy(ordered, g->results, g->rounds * sizeof(*ordered));
    for (unsigned i = 1; i < g->rounds; ++i)
      for (unsigned j = i; j && ordered[j] < ordered[j-1]; --j) {
        uint32_t t = ordered[j]; ordered[j] = ordered[j-1]; ordered[j-1] = t;
      }
    g->best = ordered[0]; g->median = ordered[g->rounds / 2];
  } else g->missed = true;
  g->reaction = REACTION_SCORE;
}
static bool near(float x, float y, const game_body_t *b, float radius) {
  float dx = x - b->x, dy = y - b->y;
  return dx*dx + dy*dy < radius*radius;
}
static void damage(app_game_t *g) {
  if (!g->lives || g->invincible) return;
  if (!--g->lives) g->phase = GAME_RESULT;
  g->invincible = 1500;
}
static void paddle(app_game_t *g, float dt, float old_x) {
  if (g->invincible) return;
  game_body_t *b = &g->ball;
  float old_y = b->y, old_bx = b->x;
  b->x += b->vx * dt; b->y += b->vy * dt;
  if (b->x < 10) { b->x = 20 - b->x; b->vx = fabsf(b->vx); }
  if (b->x > 230) { b->x = 460 - b->x; b->vx = -fabsf(b->vx); }
  if (b->y < 38) { b->y = 76 - b->y; b->vy = fabsf(b->vy); }
  // 穿越板顶的连续碰撞：按交点插值球和板，避免高速穿板。
  if (b->vy > 0 && old_y <= 203 && b->y >= 203) {
    float t = (203 - old_y) / (b->y - old_y);
    float hit_x = old_bx + (b->x - old_bx) * t;
    float plate_x = old_x + (g->x - old_x) * t;
    if (fabsf(hit_x - plate_x) <= 31) {
      ++g->score;
      float speed = 110 + fminf(g->score * 5, 130);
      float side = clamp((hit_x - plate_x) / 27, -.9f, .9f);
      b->vx = side * speed;
      b->vy = -sqrtf(speed * speed - b->vx * b->vx);
      b->y = 203 - (b->y - 203);
    }
  }
  if (b->y > 239) {
    if (!--g->lives) g->phase = GAME_RESULT;
    else serve(g);
  }
}
static void space(app_game_t *g, float dt) {
  g->level = 1 + (g->elapsed >= 20000) + (g->elapsed >= 40000);
  if (g->elapsed >= 60000) { g->phase = GAME_RESULT; return; }
  if (g->firing) shoot(g);
  if (g->elapsed >= g->spawn_at) {
    for (unsigned i = 0; i < GAME_ENEMIES; ++i) if (!g->enemies[i].active) {
      g->enemies[i] = (game_body_t){22 + random_next(g) % 197, 38, 0, 19 + 6 * g->level, true};
      break;
    }
    g->spawn_at = g->elapsed + 1600 - 250 * g->level;
  }
  if (g->elapsed >= g->volley_at) {
    for (unsigned e = 0; e < GAME_ENEMIES; ++e) if (g->enemies[e].active) {
      unsigned rays = g->level == 3 ? 3 : 1;
      for (unsigned ray = 0; ray < rays; ++ray)
        for (unsigned i = 0; i < GAME_HAZARDS; ++i) if (!g->hazards[i].active) {
          game_body_t b = g->enemies[e];
          float dx = g->level > 1 ? g->x - b.x : 0;
          float dy = fmaxf(g->y - b.y, 35);
          if (rays == 3) dx += ((int)ray - 1) * dy * .35f;
          float length = sqrtf(dx*dx + dy*dy), speed = 70 + 15 * g->level;
          b.vx = dx / length * speed; b.vy = dy / length * speed;
          g->hazards[i] = b; break;
        }
    }
    g->volley_at = g->elapsed + 1500 - 240 * g->level;
  }
  for (unsigned i = 0; i < GAME_SHOTS; ++i) {
    game_body_t *b = &g->shots[i]; if (!b->active) continue;
    b->y += b->vy * dt;
    if (b->y < 31) b->active = false;
    for (unsigned e = 0; e < GAME_ENEMIES && b->active; ++e)
      if (g->enemies[e].active && near(b->x, b->y, &g->enemies[e], 13)) {
        b->active = g->enemies[e].active = false; g->score += 10; ++g->hits;
      }
  }
  for (unsigned i = 0; i < GAME_HAZARDS; ++i) {
    game_body_t *b = &g->hazards[i]; if (!b->active) continue;
    b->x += b->vx * dt; b->y += b->vy * dt;
    if (b->y > 217 || b->x < 0 || b->x > 240) b->active = false;
    if (b->active && near(g->x, g->y, b, 8)) { b->active = false; damage(g); }
  }
  for (unsigned i = 0; i < GAME_ENEMIES; ++i) {
    game_body_t *b = &g->enemies[i]; if (!b->active) continue;
    b->y += b->vy * dt;
    if (near(g->x, g->y, b, 14)) { b->active = false; damage(g); }
    if (b->y > 212) b->active = false;
  }
}
void app_game_step(app_game_t *g, unsigned ms, int x_mg, int y_mg, bool sensor) {
  if (g->phase != GAME_PLAY || g->kind == GAME_REACTION) return;
  if (!sensor || ms > 100) {
    g->sensor_fault = !sensor; g->slow_frame = ms > 100;
    g->phase = GAME_PAUSED; g->firing = false; return;
  }
  g->accumulator += ms;
  while (g->accumulator >= 10 && g->phase == GAME_PLAY) {
    g->accumulator -= 10; g->elapsed += 10;
    g->invincible = g->invincible > 10 ? g->invincible - 10 : 0;
    float old_x = g->x;
    g->x = clamp(g->x + app_game_axis(x_mg) * (g->kind == GAME_PADDLE ? 360 : 230) * .01f,
                 g->kind == GAME_PADDLE ? 33 : 14, g->kind == GAME_PADDLE ? 207 : 226);
    if (g->kind == GAME_SPACE) {
      g->y = clamp(g->y + app_game_axis(y_mg) * 230 * .01f, 52, 204);
      space(g, .01f);
    } else paddle(g, .01f, old_x);
  }
}
