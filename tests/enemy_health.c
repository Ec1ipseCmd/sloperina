/* Run the real actor traversal, health reads, projection and snapshot publisher
 * against bounded synthetic memory. No game process or game data is needed. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/ootgz/actors.c"
#include "../src/ootgz/world_view.c"
#include "../src/ootgz/enemy_health.c"

#define OBJECTS 260
static uint64_t objects[OBJECTS][1024], before[OBJECTS][1024];
#define A(i) ((uintptr_t)objects[i])
uintptr_t z64hd_main = 0x100000000ull;
static uintptr_t blocked, game_camera;
static bool paused;
static int32_t pause_requests;
static int visits;

bool sys_mapped(uintptr_t a, size_t n, uint32_t perm)
{
  assert(perm == SYS_PERM_R); /* The entire feature must remain read-only. */
  if (n > UINTPTR_MAX - a) return false;
  for (int i = 0; i < OBJECTS; i++)
    if (a >= A(i) && a + n <= A(i) + sizeof(objects[i]) && A(i) != blocked) return true;
  return false;
}
uintptr_t sys_ptr(uintptr_t a)
{
  if (a == MAIN(Z64HD_ACTOR_MGR_PTR)) return A(0);
  uintptr_t p = 0;
  if (sys_mapped(a, 8, SYS_PERM_R)) memcpy(&p, (void *)a, 8);
  return p;
}
uintptr_t camera(void) { return game_camera; }
bool gd_get_bool(uint32_t hash, bool *out) { assert(hash == GD_SEQUENCE_IS_PAUSE); *out = paused; return true; }
bool gd_get_int(uint32_t hash, int32_t *out) { assert(hash == GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT); *out = pause_requests; return true; }
static void ptr(uintptr_t a, uintptr_t p) { memcpy((void *)a, &p, 8); }
static void integer(uintptr_t a, int32_t v) { memcpy((void *)a, &v, 4); }
static void real(uintptr_t a, float v) { memcpy((void *)a, &v, 4); }
static void position(uintptr_t a, float x, float y, float z)
{
  vec3f_t p = { x, y, z }; memcpy((void *)a, &p, sizeof(p));
}
static uintptr_t actor(int i) { return A(3 + i * 5); }
static void setup(int count)
{
  memset(objects, 0, sizeof(objects));
  blocked = 0; paused = false; pause_requests = 0; visits = 0;
  ptr(A(0), A(1));
  uintptr_t sentinel = A(1) + MANAGER_LIST;
  ptr(sentinel, count ? actor(0) + ACTOR_LIST_NODE : sentinel);
  ptr(sentinel + 8, count ? actor(count - 1) + ACTOR_LIST_NODE : sentinel);
  for (int i = 0; i < count; i++) {
    uintptr_t a = actor(i), c = A(4 + i * 5), life = A(5 + i * 5), param = A(6 + i * 5);
    ptr(a, MAIN(Z64HD_VT_LINKOBJ));
    ptr(a + 0x20, a + 0x500);
    strcpy((char *)(a + 0x500), "Enemy_Test");
    ptr(a + ACTOR_LIST_NODE, i + 1 < count ? actor(i + 1) + ACTOR_LIST_NODE : sentinel);
    ptr(a + ACTOR_LIST_NODE + 8, i ? actor(i - 1) + ACTOR_LIST_NODE : sentinel);
    ptr(a + 0x270, c); ptr(c + 0x2B8, life); ptr(life + 0x1340, a);
    ptr(life + 0x1C10, param); ptr(life + 0x1C18, param + 0x20);
    ptr(param, MAIN(Z64HD_VT_PARAM)); ptr(param + 0x20, MAIN(Z64HD_VT_PARAM));
    integer(param + 8, i + 1); integer(param + 0x28, 50);
    position(a + 0x31C, 0, 0, 5 + i);
  }
  game_camera = A(2);
  position(game_camera + CAM_LOOK_AT, 0, 0, 1);
  real(game_camera + CAM_FOV_Y, 1.2f); real(game_camera + CAM_ASPECT, 16.0f / 9);
}
static void visit(uintptr_t a, const char *name, void *ctx) { assert(a && name && !ctx); visits++; }
static int frame(bool visible)
{
  memcpy(before, objects, sizeof(objects));
  enemy_health_frame(visible);
  assert(!memcmp(before, objects, sizeof(objects)));
  enemy_label_t labels[ENEMY_LABEL_MAX];
  return enemy_health_copy(labels);
}
int main(void)
{
  setup(1);
  int32_t cur, max;
  assert(actors_each(visit, NULL) && visits == 1);
  assert(actor_health(actor(0), &cur, &max) && cur == 1 && max == 50);
  assert(!actor_health(0, &cur, &max) && !actor_health(UINTPTR_MAX - 7, &cur, &max));
  assert(!actor_health(actor(0) + 1, &cur, &max));
  ptr(actor(0) + 0x270, UINTPTR_MAX - 7); assert(frame(true) == 0);
  ptr(actor(0) + 0x270, A(4));
  assert(frame(true) == 1);
  enemy_label_t labels[ENEMY_LABEL_MAX];
  assert(enemy_health_copy(labels) == 1 && !strcmp(labels[0].text, "1/50"));
  assert(labels[0].x >= 32766 && labels[0].x <= 32768 && labels[0].y < 32767);
  assert(frame(false) == 0);
  paused = true; assert(frame(true) == 0);
  paused = false; pause_requests = 1; assert(frame(true) == 0); pause_requests = 0;
  blocked = A(5); assert(frame(true) == 0); blocked = 0;
  ptr(A(5) + 0x1340, actor(1)); assert(frame(true) == 0);
  ptr(A(5) + 0x1340, actor(0));
  integer(A(6) + 8, 0); assert(frame(true) == 0);
  integer(A(6) + 8, -1); assert(frame(true) == 0);
  integer(A(6) + 8, 51); assert(frame(true) == 0);
  integer(A(6) + 8, 1); integer(A(6) + 0x28, 0); assert(frame(true) == 0);
  integer(A(6) + 0x28, 50);
  ptr(A(6), 0); assert(frame(true) == 0); ptr(A(6), MAIN(Z64HD_VT_PARAM));
  strcpy((char *)(actor(0) + 0x500), "NPC_Test"); assert(frame(true) == 0);
  strcpy((char *)(actor(0) + 0x500), "E"); assert(frame(true) == 0);
  strcpy((char *)(actor(0) + 0x500), "Enemy_Test");
  position(actor(0) + 0x31C, 0, 0, -5); assert(frame(true) == 0);
  position(actor(0) + 0x31C, 100, 0, 5); assert(frame(true) == 0);
  position(actor(0) + 0x31C, 0, 0, 65); assert(frame(true) == 0);
  position(actor(0) + 0x31C, NAN, 0, 5); assert(frame(true) == 0);
  position(actor(0) + 0x31C, 0, 0, 5);
  real(game_camera + CAM_FOV_Y, NAN); assert(frame(true) == 0);
  real(game_camera + CAM_FOV_Y, 1.2f);
  game_camera = 0; assert(frame(true) == 0); game_camera = A(2);

  vec3f_t head;
  assert(actor_label_position(actor(0), &head) && fabsf(head.y - 1.85f) < .001f);
  uintptr_t calc = A(7);
  ptr(A(4) + 0x128, calc); ptr(calc, MAIN(Z64HD_VT_POSITION_CALCULATOR)); ptr(calc + 0x18, actor(0));
  position(calc + 0x44, 0, 1, 5); position(calc + 0x5C, 0, 2, 5);
  position(calc + 0x74, 0, 3, 5); position(calc + 0x80, NAN, 10, 5);
  assert(actor_label_position(actor(0), &head) && fabsf(head.y - 3.35f) < .001f);
  ptr(calc + 0x18, actor(1));
  assert(actor_label_position(actor(0), &head) && fabsf(head.y - 1.85f) < .001f);

  /* Broken links, cycles and vanished manager clear a previously published frame. */
  assert(frame(true) == 1);
  ptr(actor(0) + ACTOR_LIST_NODE, actor(0) + ACTOR_LIST_NODE); assert(frame(true) == 0);
  setup(1); ptr(actor(0) + ACTOR_LIST_NODE + 8, 0); assert(frame(true) == 0);
  setup(1); ptr(A(0), 0); assert(frame(true) == 0);
  setup(0); assert(frame(true) == 0);
  setup(40); assert(frame(true) == ENEMY_LABEL_MAX);
  assert(enemy_health_copy(labels) == ENEMY_LABEL_MAX);
  assert(!strcmp(labels[0].text, "1/50") && !strcmp(labels[31].text, "32/50"));
  /* Spawn/list replacement is observed immediately; no cached actor addresses. */
  setup(2); integer(A(6) + 8, 12); assert(frame(true) == 2);
  assert(enemy_health_copy(labels) == 2 && !strcmp(labels[0].text, "12/50"));
  puts("enemy health: actor lifetime, invalid reads, projection, visibility, bounds and read-only checks passed");
  return 0;
}
