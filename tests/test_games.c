#include "app_games_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void reaction(void) {
  app_game_t g;
  app_game_init(&g, GAME_REACTION, 1);
  app_game_start(&g,100);
  assert(g.deadline>=1600 && g.deadline<=4600);
  app_game_press(&g,200);
  assert(g.early && !g.rounds && g.false_starts==1);
  const unsigned scores[]={270,190,310,230,250};
  for(unsigned i=0;i<5;++i) {
    app_game_start(&g,10000*i+1000);
    app_game_reaction_tick(&g,g.deadline);
    assert(g.reaction==REACTION_DRAW);
    app_game_cue_submitted(&g,g.deadline+25);
    app_game_press(&g,g.cue_at+scores[i]);
    assert(g.rounds==i+1 && !g.early);
  }
  app_game_start(&g,60000);
  assert(g.phase==GAME_RESULT && g.median==250 && g.best==190);
  app_game_start(&g,UINT32_MAX-100);
  app_game_reaction_tick(&g,g.deadline);
  app_game_cue_submitted(&g,UINT32_MAX-20);
  app_game_press(&g,179); // 单调毫秒计时跨32位回绕。
  assert(g.results[0]==200);
  app_game_start(&g,1000);
  app_game_reaction_tick(&g,g.deadline);
  app_game_cue_submitted(&g,g.deadline+20);
  app_game_press(&g,g.cue_at-10); // 消抖后才送达，但按下发生于变色前。
  assert(g.early && g.rounds==1);
  app_game_start(&g,9000);
  app_game_reaction_tick(&g,g.deadline);
  app_game_cue_submitted(&g,g.deadline);
  app_game_reaction_tick(&g,g.cue_at+3001);
  assert(g.missed && g.rounds==1);
}
static void paddle(void) {
  app_game_t g;
  app_game_init(&g,GAME_PADDLE,123); app_game_start(&g,0);
  g.invincible=0; g.ball=(game_body_t){120,202,0,240,true};
  app_game_step(&g,10,0,0,true);
  assert(g.score==1 && g.ball.vy<0 && g.ball.y<203);
  g.ball=(game_body_t){143,202,0,240,true};
  app_game_step(&g,10,0,0,true);
  assert(g.ball.vx>0 && g.score==2);
  g.ball=(game_body_t){10,238,0,240,true};
  app_game_step(&g,10,0,0,true);
  assert(g.lives==2 && g.invincible>0);
  app_game_step(&g,33,300,0,false);
  assert(g.phase==GAME_PAUSED && g.sensor_fault && g.lives==2);
  g.phase=GAME_PLAY;
  app_game_step(&g,101,300,0,true);
  assert(g.phase==GAME_PAUSED && g.slow_frame);
  assert(app_game_axis(25)==0 && app_game_axis(-25)==0);
  assert(app_game_axis(300)==1 && app_game_axis(-300)==-1);
}
static void space(void) {
  app_game_t g;
  app_game_init(&g,GAME_SPACE,42); app_game_start(&g,0);
  app_game_press(&g,1); g.firing=false;
  assert(g.shots[0].active); // 同帧快速单击不能丢失射击。
  g.enemies[0]=(game_body_t){120,100,0,0,true};
  g.shots[0]=(game_body_t){120,106,0,-270,true};
  app_game_step(&g,10,0,0,true);
  assert(g.hits==1 && g.score==10 && !g.shots[0].active);
  g.hazards[0]=(game_body_t){g.x,g.y,0,0,true};
  g.hazards[1]=g.hazards[0];
  app_game_step(&g,10,0,0,true);
  assert(g.lives==2 && g.invincible>0); // 同帧多弹只扣一次。
  g.elapsed=19990; app_game_step(&g,10,0,0,true); assert(g.level==2);
  g.elapsed=39990; app_game_step(&g,10,0,0,true); assert(g.level==3);
  g.elapsed=59990; app_game_step(&g,10,0,0,true); assert(g.phase==GAME_RESULT);
  // 固定时间步使不同显示帧率产生相同运动，而非每帧固定移动。
  app_game_t a,b;
  app_game_init(&a,GAME_SPACE,17); app_game_start(&a,0); b=a;
  for(int i=0;i<99;++i) app_game_step(&a,10,80,-40,true);
  for(int i=0;i<30;++i) app_game_step(&b,33,80,-40,true);
  assert(fabsf(a.x-b.x)<.001f && fabsf(a.y-b.y)<.001f && a.elapsed==b.elapsed);
  for(unsigned seed=1;seed<50;++seed) {
    app_game_init(&g,GAME_SPACE,seed); app_game_start(&g,0);
    for(unsigned t=0;t<3000 && g.phase==GAME_PLAY;++t) {
      g.firing=t%2;
      app_game_step(&g,33,(int)(t*79%1601)-800,(int)(t*31%1601)-800,true);
      assert(isfinite(g.x) && isfinite(g.y) && g.x>=14 && g.x<=226 && g.y>=52 && g.y<=220);
      assert(g.lives<=3 && g.level<=3);
    }
  }
}
int main(void) {
  assert(sizeof(app_game_t)<2048);
  reaction(); paddle(); space();
  printf("PASS: reaction edges/wrap/median, swept paddle, space stages/hits/invulnerability/frame-rate/bounds; state=%zu B\n",sizeof(app_game_t));
}
