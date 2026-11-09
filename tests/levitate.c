/* Real levitation step/frame logic with a simulated physics record; no game process. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/ootgz/levitate.c"

static double physics[9];
static float cam[24];
static vec3f_t drawn_pos;
static uint32_t held;
static int32_t stick_x;
static bool mapped = true;
bool cheats[CHEAT_COUNT];
uint32_t input_hide_from_game;

uintptr_t link_actor(void) { return 0; }
uintptr_t camera(void) { return (uintptr_t)cam; }
bool link_drawn_pos(vec3f_t *p) { *p = drawn_pos; return true; }
bool link_health(int32_t **cur, int32_t **max) { (void)cur; (void)max; return false; }
bool sys_mapped(uintptr_t a, size_t n, uint32_t perm)
{
  (void)perm;
  return mapped && ((a >= (uintptr_t)physics && a + n <= (uintptr_t)(physics + 9)) ||
                    (a >= (uintptr_t)cam && a + n <= (uintptr_t)(cam + 24)));
}
uint32_t sys_query_memory(sys_meminfo_t *m, uint64_t a) { (void)m; (void)a; return 1; }
void sys_regions(uint32_t perm, void (*fn)(uintptr_t, size_t, void *), void *ctx)
{ (void)perm; (void)fn; (void)ctx; }
uint64_t sys_tick(void) { return 100; }
uint64_t sys_tick_freq(void) { return 1000; }
uint32_t input_held(void) { return held; }
uint32_t input_pressed(void) { return 0; }
void input_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry)
{ *lx = stick_x; *ly = *rx = *ry = 0; }
void log_printf(const char *fmt, ...) { (void)fmt; }
void gfx_notice(const char *s) { (void)s; }

static void sync_drawn(void)
{
  drawn_pos = (vec3f_t){ (float)physics[0], (float)(physics[1] - PHYS_Y), (float)physics[2] };
}

static void start(bool walls)
{
  memset(physics, 0, sizeof(physics));
  memset(cam, 0, sizeof(cam));
  physics[1] = 100;
  cam[0x44 / 4 + 2] = -1; /* camera forward -z, right +x */
  mapped = levitate_enabled = floating = true;
  levitate_through_walls = walls;
  levitate_speed = 4;
  levitate_move_speed = 6;
  held = BTN_RSTICK;
  stick_x = 0;
  record = (uintptr_t)physics;
  record_motion = false;
  state = L_READY;
  sync_drawn();
  float_start();
}

static void step(void)
{
  sync_drawn();
  float_step(1.0 / 30.0, false);
}

int main(void)
{
  start(true);
  levitate_speed = 0;
  held = 0; /* no R3: zero rise must hover by itself */
  for (int i = 0; i < 60; i++) {
    physics[1] -= 0.1; /* simulated gravity */
    step();
    assert(physics[1] == 100 && physics[0] == 0 && physics[2] == 0 && vy == 0);
  }
  stick_x = 32767;
  for (int i = 0; i < 30; i++) {
    physics[0] -= 0.2; /* wall push-back still bypassed at zero rise */
    physics[1] -= 0.1;
    step();
  }
  assert(physics[0] > 5 && physics[1] == 100);
  start(true);
  held = 0;
  step();
  assert(vy > 0);
  double rising_height = physics[1];
  levitate_speed = 0;
  step();
  assert(vy == 0 && physics[1] == rising_height);
  levitate_speed = 4;
  step();
  assert(vy > 0 && physics[1] > rising_height);
  puts("PASS: zero rise hovers without R3, allows horizontal wall passage, cancels ascent and resumes rising");

  for (int walls = 0; walls <= 1; walls++) {
    start(walls);
    physics[1] += 0.4; /* game lifts Link out of a floor */
    step();
    assert(fabs(physics[1] - 100.4) < 1e-9);
    physics[1] -= 0.6; /* gravity between mod frames must not sink the hover */
    step();
    assert(fabs(physics[1] - 100.4) < 1e-9);
    for (int i = 0; i < 30; i++) {
      physics[1] += 0.05; /* rising platform / repeated upward floor corrections */
      step();
    }
    assert(fabs(physics[1] - 101.9) < 1e-9);
  }

  start(true);
  stick_x = 32767;
  for (int i = 0; i < 30; i++) {
    physics[0] -= 0.2; /* horizontal wall pushes back */
    physics[1] = i % 2 ? py - 0.1 : py + 0.05;
    step();
  }
  assert(physics[0] > 5 && fabs(physics[1] - 100.75) < 1e-9);

  start(false);
  stick_x = 32767;
  clock = 1;
  steady_at = 2;
  vx = head_x = refs[0][0] = refs[1][0] = 6;
  physics[0] = -0.2;
  physics[1] = 100.4;
  step();
  assert(fabs(physics[0] + 0.2) < 1e-6 && fabs(physics[1] - 100.4) < 1e-9);

  start(true);
  held = 0; /* rise still works above corrected floor */
  physics[1] = 100.4;
  step();
  assert(physics[1] > 100.4);
  double height = physics[1];
  float_step(1.0 / 30.0, true);
  assert(physics[1] == height); /* menu stops intentional movement */
  drop();
  physics[1] -= 0.5;
  sync_drawn();
  levitate_frame(false);
  assert(physics[1] == height - 0.5); /* stopping levitation restores normal falling */

  start(true);
  mapped = false;
  levitate_frame(false);
  assert(state == L_IDLE && !floating && !writing && !record && physics[1] == 100);
  start(true); /* new record/scene starts from its own height, not an old floor */
  physics[1] = 20;
  sync_drawn();
  float_start();
  step();
  assert(physics[1] == 20);
  puts("PASS: levitation keeps upward floor corrections, rejects downward drift, passes walls only when enabled");
  puts("PASS: rising platforms, rise/hold/menu/drop, invalid record and fresh-scene height");
  return 0;
}
