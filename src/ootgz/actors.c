#include "actors.h"
#include "sys.h"
#include "z64hd.h"

#define ACTOR_LIST_NODE 0x190
#define MANAGER_LIST 0x68
#define MAX_ACTORS 4096
#define NAME_SIZE 96

static bool actor_readable(uintptr_t p, size_t n)
{
  return p && !(p & 7) && p <= UINTPTR_MAX - n && sys_mapped(p, n, SYS_PERM_R);
}

static uintptr_t actor_next(uintptr_t p, unsigned offset)
{
  return p && !(p & 7) && p <= UINTPTR_MAX - offset ? sys_ptr(p + offset) : 0;
}

/* The manager also owns non-actor processes: check the concrete actor vtable
 * before reading actor fields. Bound traversal and check both list links. */
bool actors_each(void (*visit)(uintptr_t, const char *, void *), void *ctx)
{
  uintptr_t slot = sys_ptr(MAIN(Z64HD_ACTOR_MGR_PTR));
  uintptr_t manager = slot ? sys_ptr(slot) : 0;
  if (!actor_readable(manager, 0x80))
    return false;
  uintptr_t sentinel = manager + MANAGER_LIST, previous = sentinel;
  uintptr_t node = sys_ptr(sentinel);
  for (int i = 0; node != sentinel && i < MAX_ACTORS; i++) {
    if (node < ACTOR_LIST_NODE || (node & 7) || !sys_mapped(node, 16, SYS_PERM_R) ||
        sys_ptr(node + 8) != previous)
      return false;
    uintptr_t next = sys_ptr(node), actor = node - ACTOR_LIST_NODE;
    if (sys_ptr(actor) == MAIN(Z64HD_VT_LINKOBJ) && sys_mapped(actor, 0x400, SYS_PERM_R)) {
      uintptr_t name = sys_ptr(actor + 0x20);
      char text[NAME_SIZE];
      if (name && sys_mapped(name, NAME_SIZE, SYS_PERM_R)) {
        memcpy(text, (const void *)name, sizeof(text));
        text[NAME_SIZE - 1] = 0;
        /* The name and list membership must still belong to this actor. */
        if (sys_ptr(actor + 0x20) == name && sys_ptr(node) == next)
          visit(actor, text, ctx);
      }
    }
    previous = node;
    node = next;
  }
  return node == sentinel && sys_ptr(sentinel + 8) == previous;
}

static bool health_parameter(uintptr_t p)
{
  if (!actor_readable(p, 12)) return false;
  uintptr_t vt = sys_ptr(p);
  return vt == MAIN(Z64HD_VT_ENEMY_PARAM) || vt == MAIN(Z64HD_VT_PARAM);
}

bool actor_health(uintptr_t actor, int32_t *current, int32_t *maximum)
{
  if (!actor_readable(actor, 0x400) || sys_ptr(actor) != MAIN(Z64HD_VT_LINKOBJ)) return false;
  uintptr_t components = actor_next(actor, 0x270);
  uintptr_t life = actor_next(components, 0x2B8);
  if (!life || actor_next(life, 0x1340) != actor)
    return false;
  uintptr_t cur = actor_next(life, 0x1C10), max = actor_next(life, 0x1C18);
  if (!health_parameter(cur) || !health_parameter(max))
    return false;
  *current = *(const volatile int32_t *)(cur + 8);
  *maximum = *(const volatile int32_t *)(max + 8);
  return *current >= 0 && *maximum > 0 && *current <= *maximum && *maximum <= 1000000;
}

static bool position_ok(vec3f_t p)
{
  return p.x > -100000 && p.x < 100000 && p.y > -100000 && p.y < 100000 && p.z > -100000 && p.z < 100000;
}

bool actor_label_position(uintptr_t actor, vec3f_t *out)
{
  if (!actor_readable(actor, 0x400) || sys_ptr(actor) != MAIN(Z64HD_VT_LINKOBJ))
    return false;
  vec3f_t root = *(const vec3f_t *)(actor + 0x31C);
  if (!position_ok(root))
    return false;
  *out = root;
  out->y += 1.5f; /* fallback for actors without position-calculator anchors */
  uintptr_t components = actor_next(actor, 0x270);
  uintptr_t calc = actor_next(components, 0x128);
  if (actor_readable(calc, 0x8C) && sys_ptr(calc) == MAIN(Z64HD_VT_POSITION_CALCULATOR) && actor_next(calc, 0x18) == actor) {
    /* Upper attachment points used by QueryActorPositionCalculatorgetPosition.
     * Use the highest valid point so crouching and differently sized actors follow. */
    static const unsigned offsets[] = { 0x44, 0x5C, 0x74, 0x80 };
    bool found = false;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++) {
      vec3f_t point = *(const vec3f_t *)(calc + offsets[i]);
      float dx = point.x - root.x, dy = point.y - root.y, dz = point.z - root.z;
      if (position_ok(point) && dy >= 0 && dx * dx + dy * dy + dz * dz < 10000 &&
          (!found || point.y > out->y)) {
        *out = point;
        found = true;
      }
    }
  }
  out->y += 0.35f;
  return true;
}
