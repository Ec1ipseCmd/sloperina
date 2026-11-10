/* Use the same physics APIs as OneShotPhysicsCctChangePosition and
 * OneshotChangeForceNoHitMode. Writing a scanned position record alone leaves the
 * collision solver active and can disagree with the controller's other transforms.
 * This module changes neither code nor collision geometry. */
#include "cct.h"

#include "sys.h"
#include "z64hd.h"

typedef struct { uintptr_t actor, property, body; } controller_t;
static controller_t owner;
/* A controller selects different bodies while Link changes posture/action.
 * Remember every body we changed, not just the currently selected one. */
static struct { uintptr_t body; bool no_hit; } saved_bodies[128];
static unsigned saved_count;
static bool restore_wait_logged;

static uintptr_t next(uintptr_t p, size_t offset)
{
  return p && p <= UINTPTR_MAX - offset ? sys_ptr(p + offset) : 0;
}

static bool writable(uintptr_t p, size_t size)
{
  return p && !(p & 7) && sys_mapped(p, size, SYS_PERM_R | SYS_PERM_W);
}

static bool resolve(controller_t *out)
{
  uintptr_t actor = link_actor(), p = actor;
  static const unsigned offsets[] = { 0x270, 0x50, 0x20, 0x10, 8 };
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++)
    p = next(p, offsets[i]);
  uintptr_t body = next(p, 0x20);
  if (!writable(p, 0x60) || !writable(body, 0x320) ||
      sys_ptr(p) != MAIN(Z64HD_VT_CCT_PROPERTY) || sys_ptr(body) != MAIN(Z64HD_VT_CCT_BODY))
    return false;
  /* Position setting visits the controller's attached-body lists. Do not call it
   * during partial construction/teardown or with an unreadable list. */
  uintptr_t lists = next(p, 0x58);
  if (!writable(lists, 0x60))
    return false;
  for (unsigned off = 0x10; off <= 0x50; off += 0x10) {
    uint32_t count;
    if (!sys_read32(lists + off, &count) || count > 128)
      return false;
    uintptr_t entries = next(lists, off + 8);
    if (count && (!entries || !sys_mapped(entries, count * sizeof(uintptr_t), SYS_PERM_R)))
      return false;
    for (unsigned i = 0; i < count; i++)
      if (!writable(sys_ptr(entries + i * sizeof(uintptr_t)), off == 0x10 ? 0x320 : 0x38))
        return false;
  }
  uintptr_t pending = next(body, 0x60);
  if (pending && !writable(pending, 0x108))
    return false;
  *out = (controller_t){ actor, p, body };
  return true;
}

static bool same(controller_t a, controller_t b)
{
  return a.actor == b.actor && a.property == b.property;
}

static bool attached(controller_t controller, uintptr_t body)
{
  uintptr_t lists = next(controller.property, 0x58);
  uint32_t count;
  if (!sys_read32(lists + 0x10, &count) || count > 128 ||
      !writable(body, 0x320) || sys_ptr(body) != MAIN(Z64HD_VT_CCT_BODY))
    return false;
  uintptr_t entries = next(lists, 0x18);
  for (unsigned i = 0; i < count; i++)
    if (next(entries, i * 8) == body) {
      uintptr_t pending = next(body, 0x60);
      return !pending || writable(pending, 0x108);
    }
  return false;
}

static bool no_hit(uintptr_t body)
{
  /* The setter checks the queued flag first: an enable/disable may not have
   * reached the physics thread yet. +0x100 bit 19 makes +0xF8 bit 0 authoritative. */
  uintptr_t pending = next(body, 0x60);
  if (pending && (*(volatile uint64_t *)(pending + 0x100) & (1ull << 19)))
    return (*(volatile uint32_t *)(pending + 0xF8) & 1) != 0;
  return (*(volatile uint64_t *)(body + 0x70) & (1ull << 11)) != 0;
}

static void set_no_hit(uintptr_t body, bool enabled)
{
  MAIN_FN(void (*)(uintptr_t, bool), Z64HD_CCT_FORCE_NO_HIT)(body, enabled);
}

bool cct_position(uintptr_t expected_actor, vec3f_t *position)
{
  controller_t current;
  if (!position || !expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  uintptr_t pending = next(current.body, 0x60), transform = current.body + 0x80;
  /* Match the native position setter: a queued transform takes precedence. */
  if (pending && (*(volatile uint64_t *)(pending + 0x100) & (1ull << 6)))
    transform = pending;
  vec3f_t p = { *(volatile float *)(transform + 0x0C),
                *(volatile float *)(transform + 0x1C),
                *(volatile float *)(transform + 0x2C) };
  if (!(p.x > -100000 && p.x < 100000 && p.y > -100000 && p.y < 100000 &&
        p.z > -100000 && p.z < 100000))
    return false;
  *position = p;
  return true;
}

bool cct_reset_vertical_velocity(uintptr_t expected_actor)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  /* The game's vertical-gravity reducer reads this same applied velocity and
   * uses this setter (main+0x278AA90). The setter locks and queues the change;
   * writing the scanned Havok record leaves this velocity at terminal speed. */
  vec3f_t v = *(volatile vec3f_t *)(current.body + 0xE0);
  if (!(v.x > -10000 && v.x < 10000 && v.z > -10000 && v.z < 10000))
    return false;
  v.y = 0;
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_VELOCITY)(current.body, &v);
  return true;
}

bool cct_on_ground(uintptr_t expected_actor, bool *grounded)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  /* QueryPhysicsCctIsOnGround (main+0x2AABA20): current result in a
   * three-slot buffer, state 0 means supported. Never infer landing from a
   * stationary position: pauses and midair hover are stationary too. */
  uintptr_t results = next(current.property, 0x48);
  uint32_t slot, state;
  if (!results || !sys_read32(results + 0x58, &slot) || slot >= 3)
    return false;
  uintptr_t result = next(results, 0x40 + slot * 8);
  if (!result || !sys_read32(result + 0x10, &state))
    return false;
  *grounded = state == 0;
  return true;
}

bool cct_clear_fall_distance(uintptr_t expected_actor)
{
  uintptr_t actor = link_actor();
  if (!expected_actor || actor != expected_actor)
    return false;
  uintptr_t components = next(actor, 0x270);
  uintptr_t player = next(components, 0x358), base = next(components, 0x2B8);
  if (!writable(player, 0x1910) || !writable(base, 0x18A0) ||
      next(player, 0x18) != actor || next(base, 0x1340) != actor ||
      !sys_mapped(actor + 0x320, 4, SYS_PERM_R))
    return false;
  float y = *(volatile float *)(actor + 0x320);
  if (!(y > -100000 && y < 100000))
    return false;
  /* main+0x5FCF6C computes player+0x190C from +0x18D4 minus actor Y;
   * +0x5FD128 maintains the peak at +0x18E0. +0x5FD19C feeds the base
   * fall origin, whose distance is computed at +0x484A10. Reset history,
   * never the actor's position, gravity, health, or JS Height (+0x18E8). */
  *(volatile float *)(player + 0x18D4) = y;
  *(volatile float *)(player + 0x18E0) = y;
  *(volatile float *)(player + 0x190C) = 0;
  *(volatile float *)(base + 0x1898) = y;
  *(volatile float *)(base + 0x189C) = 0;
  return true;
}

bool cct_noclip_active(void) { return owner.body != 0; }

bool cct_noclip_begin(void)
{
  controller_t current;
  if (!resolve(&current) || !attached(current, current.body))
    return false;
  if (owner.body && !same(owner, current))
    return false;
  if (!owner.body) saved_count = 0;
  unsigned i;
  for (i = 0; i < saved_count; i++)
    if (saved_bodies[i].body == current.body) break;
  if (i == saved_count) {
    if (saved_count == 128) return false;
    saved_bodies[i].body = current.body;
    saved_bodies[i].no_hit = no_hit(current.body);
    saved_count++;
    log_printf("levitate: controller %#lx, body %#lx, collision bypass on (was %d)",
               (unsigned long)current.property, (unsigned long)current.body, saved_bodies[i].no_hit);
  }
  owner = current;
  restore_wait_logged = false;
  if (!no_hit(owner.body))
    set_no_hit(owner.body, true);
  return true;
}

bool cct_noclip_move(const vec3f_t *position)
{
  controller_t current;
  if (!owner.body || !resolve(&current) || !same(owner, current) || current.body != owner.body ||
      !(position->x > -100000 && position->x < 100000 &&
        position->y > -100000 && position->y < 100000 &&
        position->z > -100000 && position->z < 100000))
    return false;
  if (!no_hit(owner.body))
    set_no_hit(owner.body, true);
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_POSITION)(owner.property, position);
  return true;
}

void cct_noclip_end(void)
{
  if (!owner.body)
    return;
  controller_t current;
  if (!resolve(&current)) {
    if (!restore_wait_logged)
      log_printf("levitate: controller unavailable; collision restore pending");
    restore_wait_logged = true;
    return; /* Retry while idle; never forget a temporarily unreadable owner. */
  }
  if (same(owner, current)) {
    for (unsigned i = 0; i < saved_count; i++) {
      uintptr_t body = saved_bodies[i].body;
      /* Revalidate membership in the live controller; never write a detached
       * or replaced body through a saved address. */
      if (attached(current, body) && no_hit(body) != saved_bodies[i].no_hit)
        set_no_hit(body, saved_bodies[i].no_hit);
    }
    log_printf("levitate: restored collision for %u controller bodies", saved_count);
  } else {
    log_printf("levitate: controller changed/unavailable; skipped stale collision restore");
  }
  owner = (controller_t){0};
  saved_count = 0;
}
