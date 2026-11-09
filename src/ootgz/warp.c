/* Warps.
 *
 * Instant (warp_arm tries this first): request the same event an exit requests, with the same
 * parameters. Exits are FieldAndDungeonEntranceEventRequestActor; they request "Dm_SY_BancChangeGo"
 * with NextBanc / NextStartPos / NextStartPosAir / BootEventName / IsCheckAir, and that flow's
 * GameSystemActor.EventReserveBancChange (romfs GameSystemActor.pack, ReserveBancChange.module)
 * writes the Sequence_Reserved* GameData and sets Sequence_ControlInEvent = ChangeBancOnEvent, which
 * the Event sequence turns into the map change. The request is built and submitted the way the
 * SeqOneShotRequestEvent node does it (main+0x32db1a0), with params as QueryBancChangeEventRequestArg
 * (main+0x29bf380) fills them. So the warp gets the game's own fade, sound and arrival.
 *
 * Fallback, if the request is refused (game.py's method): clear Sequence_ReservedNextBanc, wait
 * for the game to fill it (Link entered an exit), then swap in our destination. Exits that lead
 * into a dungeon are left alone: swapping the map under their door transition left the screen
 * black for good (FINDINGS §5.16). */
#include "warp.h"

#include "gamedata.h"
#include "gfx.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

#define STANDARD_ARRIVAL "Dm_SY_BancChangeArrive"
/* Spawns no exit leads to (warp-song pads, blue warps, boss rooms...) have no event starter for the
 * standard arrival, which crashed (FINDINGS §5.18). The game enters the warp-song pads with no starter
 * and this arrival (Dm_SY_Ocarina › OcarinaWarp's EventReserveBancChange); it needs no starter. */
#define NO_STARTER_ARRIVAL "OcarinaWarp_Arrive"

static const char *arrival_event(const spawn_t *s)
{
  if (!s->starter)
    return NO_STARTER_ARRIVAL;
  return warp_arrival_cutscenes ? s->event : STANDARD_ARRIVAL;
}

/* After the game has reserved our map: what the event's own reservation got wrong for spawns
 * without a starter (Dm_SY_BancChangeGo always names the spawn as the starter). */
static void fix_reservation(const spawn_t *s)
{
  if (s->starter)
    return;
  gd_set_str(GD_STR_STARTER, "");
  gd_set_str(GD_STR_EVENT, NO_STARTER_ARRIVAL);
}
#define WARP_TIMEOUT_FRAMES (300 * 30)
#define REQUEST_TIMEOUT_FRAMES 90 /* the event writes the reserved map within this, or we fall back */

bool warp_arrival_cutscenes;

static const warp_map_t *w_map;
static const spawn_t *w_spawn;
static enum { W_NONE, W_ARMED, W_DONE, W_SKIPPED, W_CANCELLED, W_TIMED_OUT, W_REQUESTED } w_state;
static uint32_t w_deadline;
static char w_text[96];

static bool is_dungeon(const char *name)
{
  for (int i = 0; i < data_n_warp_maps; i++)
    if (data_warp_maps[i].dungeon) {
      const char *a = data_warp_maps[i].name, *b = name;
      while (*a && *a == *b)
        a++, b++;
      if (*a == *b)
        return true;
    }
  return false;
}

/* --- instant: request the exit's event ------------------------------------------------------ */

typedef void (*req_ctor_t)(void *req);
typedef bool (*req_submit_t)(void *mgr, void *req);
typedef void (*req_dtor_t)(void *sub);

static _Alignas(16) uint8_t request[Z64HD_EVENT_REQ_SIZE]; /* game thread only */
static char pos_air[GD_STR_LEN + 8];

static void copy_str(char *dst, uint32_t cap, const char *src)
{
  uint32_t i = 0;
  for (; cap && i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  if (cap)
    dst[i] = 0;
}

static bool add_param(const char *name, uint32_t type, const void *value)
{
  uint32_t *count = (uint32_t *)(request + Z64HD_EVENT_REQ_NPARAMS);
  uint32_t cap = *(uint32_t *)request;
  uint8_t *array = *(uint8_t **)(request + Z64HD_EVENT_REQ_PARAMS);
  if (!array || *count >= cap)
    return false;
  uint8_t *e = array + (size_t)*count * Z64HD_EVENT_PARAM_SIZE;
  copy_str(*(char **)(e + 8), *(uint32_t *)(e + 0x10), name);
  *(uint32_t *)e = type;
  if (type == Z64HD_EVENT_PARAM_BOOL)
    *(uint8_t *)(e + 0x98) = *(const bool *)value;
  else
    *(const void **)(e + 0x98) = value;
  ++*count;
  return true;
}

static bool request_warp_event(const warp_map_t *map, const spawn_t *spawn)
{
  uintptr_t p = sys_ptr(MAIN(Z64HD_EVENT_MGR_PTR));
  void *mgr = (void *)(p ? sys_ptr(p) : 0);
  if (!mgr)
    return false;
  memset(request, 0, sizeof(request));
  MAIN_FN(req_ctor_t, Z64HD_EVENT_REQ_CTOR)(request);
  copy_str(*(char **)(request + Z64HD_EVENT_REQ_NAME), *(uint32_t *)(request + Z64HD_EVENT_REQ_NAME + 8),
           "Dm_SY_BancChangeGo");
  snprintf(pos_air, sizeof(pos_air), "%s_Air", spawn->pos);
  static const bool no = false;
  bool ok = add_param("NextBanc", Z64HD_EVENT_PARAM_STRING, map->name) &&
            add_param("NextStartPos", Z64HD_EVENT_PARAM_STRING, spawn->pos) &&
            add_param("NextStartPosAir", Z64HD_EVENT_PARAM_STRING, pos_air) &&
            add_param("BootEventName", Z64HD_EVENT_PARAM_STRING, arrival_event(spawn)) &&
            add_param("IsCheckAir", Z64HD_EVENT_PARAM_BOOL, &no);
  if (ok)
    ok = MAIN_FN(req_submit_t, Z64HD_EVENT_REQ_SUBMIT)(mgr, request);
  /* destroy it as the node does: base vtable into both sub-objects, then their destructor */
  uintptr_t vt = MAIN(Z64HD_EVENT_REQ_SUB_VT) + 0x10;
  *(uintptr_t *)(request + Z64HD_EVENT_REQ_SUB_B) = vt;
  MAIN_FN(req_dtor_t, Z64HD_EVENT_REQ_SUB_DTOR)(request + Z64HD_EVENT_REQ_SUB_B);
  *(uintptr_t *)(request + Z64HD_EVENT_REQ_SUB_A) = vt;
  MAIN_FN(req_dtor_t, Z64HD_EVENT_REQ_SUB_DTOR)(request + Z64HD_EVENT_REQ_SUB_A);
  return ok;
}

/* --- fallback: swap the destination of the next exit Link takes ---------------------------------- */

static void arm_exit_swap(const warp_map_t *map, const spawn_t *spawn)
{
  if (!gd_set_str(GD_STR_NEXT_MAP, "")) {
    gfx_notice("can't warp: game data not found");
    return;
  }
  w_map = map;
  w_spawn = spawn;
  w_state = W_ARMED;
  w_deadline = frame_counter() + WARP_TIMEOUT_FRAMES;
  log_printf("warp armed: %s / %s", map->name, spawn->pos);
  gfx_notice("warp ready: walk through any exit or loading zone");
}

void warp_arm(const warp_map_t *map, const spawn_t *spawn)
{
  if (w_state == W_ARMED)
    w_state = W_CANCELLED;
  if (request_warp_event(map, spawn)) {
    w_map = map;
    w_spawn = spawn;
    w_state = W_REQUESTED;
    w_deadline = frame_counter() + REQUEST_TIMEOUT_FRAMES;
    log_printf("warp: requested Dm_SY_BancChangeGo to %s / %s", map->name, spawn->pos);
    return;
  }
  log_printf("warp: the game refused the event request; falling back to the next exit");
  arm_exit_swap(map, spawn);
}

void warp_cancel(void)
{
  if (w_state == W_ARMED || w_state == W_REQUESTED) {
    w_state = W_CANCELLED;
    gfx_notice("warp cancelled");
  } else {
    gfx_notice("no warp armed");
  }
}

static bool same_str(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

void warp_frame(void)
{
  if (w_state == W_REQUESTED) {
    /* the event's EventReserveBancChange writes our map into Sequence_ReservedNextBanc */
    const char *next = gd_get_str(GD_STR_NEXT_MAP);
    if (next && same_str(next, w_map->name)) {
      fix_reservation(w_spawn);
      w_state = W_DONE;
      log_printf("warp: the game reserved %s%s", next, w_spawn->starter ? "" : " (no starter: warp-song arrival)");
    } else if ((int32_t)(frame_counter() - w_deadline) > 0) {
      log_printf("warp: the event didn't start; falling back to the next exit");
      arm_exit_swap(w_map, w_spawn);
    }
    return;
  }
  if (w_state != W_ARMED)
    return;
  if ((int32_t)(frame_counter() - w_deadline) > 0) {
    w_state = W_TIMED_OUT;
    gfx_notice("warp timed out");
    return;
  }
  const char *next = gd_get_str(GD_STR_NEXT_MAP);
  if (!next || !next[0])
    return;
  if (is_dungeon(next)) {
    w_state = W_SKIPPED;
    log_printf("warp skipped: the exit leads into %s", next);
    gfx_notice("warp skipped: that exit is a dungeon door; warp again from there");
    return;
  }
  gd_set_str(GD_STR_NEXT_MAP, w_map->name);
  gd_set_str(GD_STR_SPAWN, w_spawn->pos);
  gd_set_str(GD_STR_STARTER, w_spawn->starter ? w_spawn->pos : "");
  gd_set_str(GD_STR_EVENT, arrival_event(w_spawn));
  w_state = W_DONE;
  log_printf("warp: %s replaced by %s / %s", next, w_map->name, w_spawn->pos);
}

const char *warp_state(void)
{
  static const char *const names[] = { "none", "armed", "done", "skipped (dungeon door)", "cancelled",
                                       "timed out", "starting" };
  if (w_state == W_NONE)
    return "none";
  snprintf(w_text, sizeof(w_text), "%s: %s", names[w_state], w_map->label);
  return w_text;
}
