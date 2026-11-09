#include "voids.h"
#include "actors.h"
#include "levitate.h"
#include "sys.h"
#include "z64hd.h"
#include "ootgz_version.h"

bool voids_shown;
int voids_mode;
const char *const voids_mode_names[VOIDS_MODE_COUNT] = { "normal", "off while levitating", "off" };

#define MAX_VOID_BOXES 64
#define ABYSS_MESSAGE 0x613B0000u
#define SAND_MESSAGE 0x76910020u
#define FALL_INTO_ABYSS 8u
#define NO_ACTOR UINT32_MAX

static uint32_t protected_actor = NO_ACTOR;
static struct { vec3f_t corners[8]; bool active; } boxes[MAX_VOID_BOXES];
static int box_count, sensor_count, missing_bounds;
static bool list_ready;

static uintptr_t next(uintptr_t p, unsigned offset)
{
  return p && !(p & 7) && p <= UINTPTR_MAX - offset ? sys_ptr(p + offset) : 0;
}

static bool readable(uintptr_t p, size_t n)
{
  return p && !(p & 7) && p <= UINTPTR_MAX - n && sys_mapped(p, n, SYS_PERM_R);
}

static bool named(uintptr_t p, const char *expected)
{
  size_t n = strlen(expected) + 1;
  if (!p || !sys_mapped(p, n, SYS_PERM_R)) return false;
  for (size_t i = 0; i < n; i++)
    if (((const char *)p)[i] != expected[i]) return false;
  return true;
}

/* The same SensorHitInfo output slot must feed the abyss node. Merely sharing a
 * logic module is not sufficient: many modules also contain harmless Areas. */
static bool player_void(uintptr_t area_node, bool *active)
{
  uintptr_t context = next(area_node, 8), nodes = next(context, 0x68);
  uint32_t count;
  if (!readable(context, 0x70) || !sys_read32(context + 0x60, &count) || count > 1024 ||
      (count && !readable(nodes, count * 8))) return false;
  bool found = false;
  *active = false;
  for (uint32_t i = 0; i < count; i++) {
    uintptr_t node = sys_ptr(nodes + i * 8), vt = next(node, 0);
    if (vt != MAIN(Z64HD_VT_ABYSS_NODE) && vt != MAIN(Z64HD_VT_ABYSS_CONDITION_NODE) &&
        vt != MAIN(Z64HD_VT_SAND_ABYSS_NODE)) continue;
    if (next(node, 8) != context || (next(node, 0x28) & ~(uintptr_t)3) != area_node + 0x80) continue;
    bool enabled = true;
    if (vt == MAIN(Z64HD_VT_ABYSS_CONDITION_NODE)) {
      uintptr_t reason = next(node, 0x78) & ~(uintptr_t)3;
      uintptr_t target_cell = next(node, 0x68) & ~(uintptr_t)3;
      if (!readable(target_cell, 8)) continue;
      uintptr_t target = sys_ptr(target_cell);
      uintptr_t condition = next(node, 0x60);
      uint32_t value;
      if (!reason || !sys_read32(reason, &value) || value != FALL_INTO_ABYSS ||
          (target && !named(target, "Link")) || !condition ||
          !sys_mapped(condition, 1, SYS_PERM_R)) continue;
      enabled = *(const uint8_t *)condition == 1;
    }
    found = true;
    *active |= enabled;
  }
  return found;
}

static bool sensor_box(uintptr_t body, bool active)
{
  if (!readable(body, 0xB0) || sys_ptr(body) != MAIN(Z64HD_VT_SENSOR_BODY)) return false;
  uintptr_t shape = next(body, 0x28), vt = next(shape, 0);
  /* This bounds implementation uses cached local extents/centre and scale.
   * Other shape implementations (e.g. compound geometry) are not guessed. */
  if (!readable(shape, 0xDC) || !readable(vt, 0x80) ||
      next(vt, 0x78) != MAIN(Z64HD_SHAPE_CACHED_BOUNDS)) return false;
  float transform[12], extent[3], centre[3];
  memcpy(transform, (const void *)(body + 0x80), sizeof(transform));
  memcpy(extent, (const void *)(shape + 0xC0), sizeof(extent));
  memcpy(centre, (const void *)(shape + 0xD0), sizeof(centre));
  float scale = *(const float *)(shape + 0x10);
  if (!(scale > 0 && scale < 10000)) return false;
  for (int i = 0; i < 3; i++) {
    if (!(extent[i] > 0 && extent[i] < 10000 && centre[i] > -10000 && centre[i] < 10000)) return false;
    float norm = 0;
    for (int j = 0; j < 3; j++) norm += transform[i * 4 + j] * transform[i * 4 + j];
    if (!(norm > .9f && norm < 1.1f)) return false;
  }
  vec3f_t corners[8];
  for (int corner = 0; corner < 8; corner++) {
    float local[3], world[3];
    for (int i = 0; i < 3; i++) local[i] = (centre[i] + ((corner >> i & 1) ? extent[i] : -extent[i])) * scale;
    for (int i = 0; i < 3; i++) {
      world[i] = transform[i * 4 + 3];
      for (int j = 0; j < 3; j++) world[i] += transform[i * 4 + j] * local[j];
      if (!(world[i] > -100000 && world[i] < 100000)) return false;
    }
    corners[corner] = (vec3f_t){ world[0], world[1], world[2] };
  }
  if (next(body, 0x28) != shape || box_count == MAX_VOID_BOXES) return false;
  memcpy(boxes[box_count].corners, corners, sizeof(corners));
  boxes[box_count++].active = active;
  return true;
}

static void area(uintptr_t actor, const char *name, void *ctx)
{
  (void)ctx;
  if (name[0] != 'A' || name[1] != 'r' || name[2] != 'e' || name[3] != 'a' || name[4]) return;
  uintptr_t binder = next(next(actor, 0x3D8), 0x80);
  if (next(binder, 0) != MAIN(Z64HD_VT_AREA_BINDER)) return;
  uintptr_t node = next(binder, 8);
  if (next(node, 0) != MAIN(Z64HD_VT_AREA_LOGIC_NODE)) return;
  bool active;
  if (!player_void(node, &active)) return;
  sensor_count++;
  uintptr_t controller = next(next(next(actor, 0x270), 0x50), 0x20);
  uint32_t controllers;
  uintptr_t list = next(controller, 0x40);
  if (!controller || !sys_read32(controller + 0x38, &controllers) || !controllers || controllers > 16 ||
      !readable(list, controllers * 8)) {
    missing_bounds++; return;
  }
  /* Every sensor controller, then its array of 24-byte rigid-body entries. */
  for (uint32_t c = 0; c < controllers; c++) {
    uintptr_t pool = next(sys_ptr(list + c * 8), 8);
    uintptr_t entries = next(pool, 0x18);
    uint32_t count;
    if (!pool || !sys_read32(pool + 0x10, &count) || !count || count > 64 ||
        !readable(entries, count * 0x18)) { missing_bounds++; continue; }
    for (uint32_t i = 0; i < count; i++)
      if (!sensor_box(sys_ptr(entries + i * 0x18 + 8), active)) missing_bounds++;
  }
}

void voids_frame(void)
{
  uint32_t id = NO_ACTOR;
  if (voids_mode == VOIDS_DISABLED || (voids_mode == VOIDS_WHILE_LEVITATING && levitate_active())) {
    uintptr_t actor = link_actor();
    if (actor && sys_ptr(actor) == MAIN(Z64HD_VT_LINKOBJ)) sys_read32(actor + 0x3E8, &id);
  }
  __atomic_store_n(&protected_actor, id, __ATOMIC_RELEASE);
  box_count = sensor_count = missing_bounds = 0;
  list_ready = false;
  if (voids_shown) {
    list_ready = actors_each(area, NULL);
    if (!list_ready) box_count = sensor_count = missing_bounds = 0;
  }
}

void voids_each_box(void (*draw)(const vec3f_t [8], bool, void *), void *ctx)
{
  for (int i = 0; i < box_count; i++) draw(boxes[i].corners, boxes[i].active, ctx);
}

void voids_summary(char *buf, int len)
{
  if (!voids_shown) snprintf(buf, len, "enable show voids to inspect");
  else if (!list_ready) snprintf(buf, len, "void sensors: N/A");
  else if (!sensor_count) snprintf(buf, len, "no identified void sensors in loaded area");
  else snprintf(buf, len, "%d sensors, %d bounds, %d unavailable", sensor_count, box_count, missing_bounds);
}

static bool protects(const uint32_t *target, uint32_t kind)
{
  uint32_t id = __atomic_load_n(&protected_actor, __ATOMIC_ACQUIRE);
  return id != NO_ACTOR && target && target[0] == id && target[1] == kind;
}

int32_t voids_abyss_message(uintptr_t sender, uintptr_t arg, const uint32_t *target, const uint32_t *payload)
{
  if (payload && payload[0] == FALL_INTO_ABYSS && protects(target, ABYSS_MESSAGE)) {
    return -1;
  }
  /* The fallback trampoline executes the displaced PACIBSP and resumes at
   * sender+4. Calling the patched sender entry here would recurse. */
  return MAIN_FN(int32_t (*)(uintptr_t, uintptr_t, const uint32_t *, const uint32_t *),
                 Z64HD_CAVE_START + OOTGZ_ABYSS_STUB_OFF + 16)
    (sender, arg, target, payload);
}

int32_t voids_sand_message(uintptr_t sender, uintptr_t arg, const uint32_t *target, const uint32_t *payload)
{
  if (protects(target, SAND_MESSAGE)) {
    return -1;
  }
  return MAIN_FN(int32_t (*)(uintptr_t, uintptr_t, const uint32_t *, const uint32_t *), Z64HD_SAND_SEND)
    (sender, arg, target, payload);
}
