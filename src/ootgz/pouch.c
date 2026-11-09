#include "pouch.h"

#include "gamedata.h"
#include "job.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

/* The name argument of the pouch functions: {char *top; s32 size = 0x80; char buf[]}
 * (built on the stack exactly like this by the old assembly hook, tools/hook_asm.py). */
typedef struct {
  char   *top;
  int32_t size;
  char    buf[0x80];
} pouch_name_t;

typedef bool (*pouch_add_t)(void *mgr, pouch_name_t *name, int32_t a2, int32_t count, int64_t reason,
                            int32_t a5, int32_t a6, bool equip_direct, bool stack_arg);
typedef void (*pouch_notify_t)(void *target, pouch_name_t *name, bool ok, int64_t a3);
typedef void (*pouch_remove_t)(void *mgr, pouch_name_t *name, int32_t count, int32_t a3, int32_t a4,
                               int32_t a5);
typedef void (*equip_slot_t)(uintptr_t component, int32_t slot);
typedef void (*equip_all_t)(uintptr_t owner, int32_t zero);
typedef void (*ui_method_t)(uintptr_t screen);

static bool streq(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static void set_name(pouch_name_t *n, const char *item)
{
  int i = 0;
  for (; item[i] && i < (int)sizeof(n->buf) - 1; i++)
    n->buf[i] = item[i];
  n->buf[i] = 0;
  n->top = n->buf;
  n->size = sizeof(n->buf);
}

static void *pouch_manager(void)
{
  uintptr_t p = sys_ptr(MAIN(Z64HD_POUCH_MGR_PTR));
  return (void *)(p ? sys_ptr(p) : 0);
}

static uint32_t hash_prefixed(const char *prefix, const char *item, const char *suffix)
{
  char buf[160];
  snprintf(buf, sizeof(buf), "%s%s%s", prefix, item, suffix);
  return murmur3(buf);
}

int32_t pouch_stock(const char *item)
{
  int32_t v;
  return gd_get_int(hash_prefixed("PouchData.", item, ".StockNum"), &v) ? v : -1;
}

/* --- single calls (game thread only) ------------------------------------------------------ */

bool pouch_add_now(const char *item, bool equip_direct)
{
  void *mgr = pouch_manager();
  if (!mgr)
    return false;
  pouch_name_t name;
  set_name(&name, item);
  /* as the OneShotAddOrRemovePouchContent event node calls it (Saria gives the ocarina with
   * IsEquipDirect = false) */
  bool ok = MAIN_FN(pouch_add_t, Z64HD_POUCH_ADD)(mgr, &name, 0, 1, 0, 0, 0, equip_direct, false) & 1;
  uintptr_t target = sys_ptr(MAIN(Z64HD_POUCH_NOTIFY_PTR));
  if (target)
    MAIN_FN(pouch_notify_t, Z64HD_POUCH_NOTIFY)((void *)target, &name, ok, 0);
  log_printf("add %s: %s", item, ok ? "ok" : "refused");
  return ok;
}

bool pouch_remove_now(const char *item)
{
  void *mgr = pouch_manager();
  if (!mgr)
    return false;
  pouch_name_t name;
  set_name(&name, item);
  /* what the event node does for Num < 0 (AddPouch with a negative count does nothing) */
  MAIN_FN(pouch_remove_t, Z64HD_POUCH_REMOVE)(mgr, &name, 1, 0, 0, 0);
  log_printf("remove %s", item);
  return true;
}

static uintptr_t ui_manager(void)
{
  uintptr_t p = sys_ptr(MAIN(Z64HD_UI_MGR_PTR));
  return p ? sys_ptr(p) : 0;
}

/* The UI manager's screens: count at +8, array at +0x10. pick(screen) chooses one. */
static uintptr_t find_screen(bool (*pick)(uintptr_t screen, uintptr_t arg), uintptr_t arg)
{
  uintptr_t uim = ui_manager();
  uint32_t cnt;
  if (!uim || !sys_read32(uim + 8, &cnt))
    return 0;
  uintptr_t arr = sys_ptr(uim + 0x10);
  for (uint32_t i = 0; arr && i < cnt && i < 256; i++) {
    uintptr_t s = sys_ptr(arr + 8 * i);
    if (s && pick(s, arg))
      return s;
  }
  return 0;
}

static bool pick_vtable(uintptr_t s, uintptr_t vt) { return sys_ptr(s) == vt; }

static bool pick_hud(uintptr_t s, uintptr_t unused)
{
  (void)unused;
  uintptr_t w = sys_ptr(s + 0x330);
  return w && sys_ptr(w) == MAIN(Z64HD_VT_DPAD);
}

static bool call_refresh(uintptr_t screen)
{
  uintptr_t vt = sys_ptr(screen);
  uintptr_t fn = vt ? sys_ptr(vt + 8 * Z64HD_UI_REFRESH) : 0;
  if (!fn)
    return false;
  ((ui_method_t)fn)(screen);
  return true;
}

bool ui_refresh_now(uintptr_t vtable_off)
{
  uintptr_t s = find_screen(pick_vtable, MAIN(vtable_off));
  return s && call_refresh(s);
}

bool hud_refresh_now(void)
{
  uintptr_t s = find_screen(pick_hud, 0);
  return s && call_refresh(s);
}

/* --- queued operations --------------------------------------------------------------------- */

#define KEY(kind, n) ((uint32_t)(kind) << 24 | ((uint32_t)(n) & 0xFFFFFF))
enum { K_GIVE = 1, K_GEAR, K_UNEQUIP, K_REFRESH, K_UPGRADE, K_OCARINA };

static bool job_give_here(job_t *j)
{
  const char *item = (const char *)j->arg[0];
  if (j->step == 0)
    return !pouch_add_now(item, false);
  hud_refresh_now();
  return true;
}

void pouch_give_here(const char *item)
{
  job_submit(KEY(K_GIVE, murmur3(item)), job_give_here, (intptr_t)item, 0, 0, 0);
}

/* FINDINGS §5.1 / §5.4: the pouch and PlayerStatus.Equip* are only data; Link's usable gear is
 * created by main+0x2df6c80(component, slot) (slot 0xd = EquipOcarina) and taken off by the full
 * re-apply main+0xf201b0(owner, 0) once the slot is empty. Only ocarinas use this. */
static bool job_gear(job_t *j)
{
  const char *item = (const char *)j->arg[0];
  bool remove = j->arg[1];
  switch (j->step) {
    case 0: {
      const char *cur = gd_get_str(GD_STR_OCARINA);
      if (!remove)
        gd_set_str(GD_STR_OCARINA, item);
      else if (cur && streq(cur, item))
        gd_set_str(GD_STR_OCARINA, "");
      if (remove)
        pouch_remove_now(item);
      else
        pouch_add_now(item, false);
      return false;
    }
    case 1:
      if (remove) {
        uintptr_t owner = link_equip_owner();
        if (!owner) {
          log_printf("%s: owner of Link's equipment not found", item);
          return true;
        }
        MAIN_FN(equip_all_t, Z64HD_EQUIP_ALL_FN)(owner, 0);
      } else {
        uintptr_t comp = link_equip_component();
        if (!comp) {
          log_printf("%s: Link's equipment component not found", item);
          return true;
        }
        MAIN_FN(equip_slot_t, Z64HD_EQUIP_SLOT_FN)(comp, 0xD);
      }
      return false;
    default:
      if (remove)
        gd_set_bool(hash_prefixed("IsGet.", item, ""), false);
      hud_refresh_now();
      log_printf("%s %s: done", remove ? "remove" : "give", item);
      return true;
  }
}

void pouch_gear(const char *item, bool remove)
{
  job_submit(KEY(K_GEAR, murmur3(item)), job_gear, (intptr_t)item, remove, 0, 0);
}

/* --- ocarina choice ---------------------------------------------------------------------------- */

static const char *const ocarina_items[3] = { 0, "Item_Ocarina_Fairy", "Item_Ocarina_Time" };
const char *const ocarina_names[3] = { "none", "fairy ocarina", "ocarina of time" };
static int ocarina_wanted;

static bool ocarina_busy(void)
{
  return job_pending(KEY(K_OCARINA, 0)) || job_pending(KEY(K_GEAR, murmur3(ocarina_items[1]))) ||
         job_pending(KEY(K_GEAR, murmur3(ocarina_items[2])));
}

int pouch_ocarina(void)
{
  if (ocarina_busy())
    return ocarina_wanted;
  if (pouch_stock(ocarina_items[2]) > 0)
    return 2;
  return pouch_stock(ocarina_items[1]) > 0 ? 1 : 0;
}

/* Decided when it runs (not when chosen), so it starts from what Link holds by then; it
 * queues the removal of the other ocarina and then the give, each the full gear sequence. */
static bool job_ocarina(job_t *j)
{
  int want = (int)j->arg[0];
  for (int o = 1; o <= 2; o++)
    if (o != want && pouch_stock(ocarina_items[o]) > 0)
      pouch_gear(ocarina_items[o], true);
  if (want && pouch_stock(ocarina_items[want]) <= 0)
    pouch_gear(ocarina_items[want], false);
  return true;
}

void pouch_set_ocarina(int choice)
{
  if (choice < 0 || choice > 2)
    return;
  ocarina_wanted = choice;
  job_submit(KEY(K_OCARINA, 0), job_ocarina, choice, 0, 0, 0);
}

static bool job_unequip(job_t *j)
{
  int slot = (int)j->arg[0];
  const char *item = (const char *)j->arg[1];
  if (j->step == 0) {
    const char *cur = gd_get_str(slot);
    if (!cur || !streq(cur, item))
      return true;
    gd_set_str(slot, "");
    uintptr_t owner = link_equip_owner();
    if (!owner)
      return true;
    MAIN_FN(equip_all_t, Z64HD_EQUIP_ALL_FN)(owner, 0);
    return false;
  }
  hud_refresh_now();
  return true;
}

void pouch_unequip(int str_slot, const char *item)
{
  job_submit(KEY(K_UNEQUIP, murmur3(item)), job_unequip, str_slot, (intptr_t)item, 0, 0);
}

static bool job_refresh(job_t *j)
{
  ui_refresh_now((uintptr_t)j->arg[0]);
  return true;
}

void pouch_refresh_screen(uintptr_t vtable_off)
{
  job_submit(KEY(K_REFRESH, vtable_off), job_refresh, (intptr_t)vtable_off, 0, 0, 0);
}

/* --- item buttons -------------------------------------------------------------------------- */

/* The four item-button Enums (names not recovered); values are murmur3 of the choice name. */
static const uint32_t button_slots[] = { 0x6BEE602D, 0x76F85E6D, 0xA0029837, 0xBC32CE24 };

/* The item wheel keeps its own copy of the buttons and writes it back when it closes, so a
 * clear is repeated for a while (FINDINGS §5.6). */
#define BUTTON_WATCH_FRAMES (120 * 30)
static struct { uint32_t choice; uint32_t until; } button_watch[8];

void pouch_clear_buttons(const char *choice)
{
  uint32_t h = murmur3(choice);
  int free_slot = -1;
  for (int i = 0; i < 8; i++) {
    if (button_watch[i].choice == h)
      free_slot = i;
    else if (!button_watch[i].choice && free_slot < 0)
      free_slot = i;
  }
  if (free_slot >= 0) {
    button_watch[free_slot].choice = h;
    button_watch[free_slot].until = frame_counter() + BUTTON_WATCH_FRAMES;
  }
}

void pouch_clear_buttons_cancel(const char *choice)
{
  uint32_t h = murmur3(choice);
  for (int i = 0; i < 8; i++)
    if (button_watch[i].choice == h)
      button_watch[i].choice = 0;
}

void pouch_frame(void)
{
  uint32_t now = frame_counter(), empty = 0;
  bool changed = false;
  for (int i = 0; i < 8; i++) {
    if (!button_watch[i].choice)
      continue;
    if ((int32_t)(now - button_watch[i].until) > 0) {
      button_watch[i].choice = 0;
      continue;
    }
    for (unsigned s = 0; s < sizeof(button_slots) / sizeof(*button_slots); s++) {
      uint32_t v;
      if (gd_get_enum(button_slots[s], &v) && v == button_watch[i].choice) {
        if (!empty)
          empty = murmur3("Empty");
        gd_set_enum(button_slots[s], empty);
        changed = true;
      }
    }
  }
  if (changed)
    pouch_refresh_screen(Z64HD_VT_HUD);
}

/* --- upgrades ------------------------------------------------------------------------------ */

#define BAG3(n, h, a, b, c, pre)                                                         \
  { n, h, 4, { "none", #a, #b, #c }, { 0, a, b, c },                                    \
    { { 0 }, { pre "_Small" }, { pre "_Medium" }, { pre "_Large" } } }

const upgrade_def_t upgrades[UPG_COUNT] = {
  [UPG_STICK]  = BAG3("deku stick bag", 0x457616B0, 10, 20, 30, "Item_StickBag"),
  [UPG_NUT]    = BAG3("deku nut bag",   0x735B00F7, 20, 30, 40, "Item_NutBag"),
  [UPG_SEED]   = BAG3("deku seed bag",  0xAE8CC742, 30, 40, 50, "Item_BulletBag"),
  [UPG_BOMB]   = BAG3("bomb bag",       0xE753508B, 20, 30, 40, "Item_BombBag"),
  [UPG_QUIVER] = BAG3("quiver",         0x418DB791, 30, 40, 50, "Item_Quiver"),
  [UPG_WALLET] = BAG3("wallet",         0xE0010A50, 200, 500, 1000, "Item_Wallet"),
  [UPG_MAGIC]  = { "magic meter", 0x12D63A97, 3, { "none", "single", "double" }, { 0, 240, 480 },
                   { { 0 }, { "Item_FairyBless_MP" }, { "Item_FairyBless_MP", "Item_FairyBless_MPDouble" } } },
  [UPG_DEFENSE] = { "double defense", 0, 2, { "off", "on" }, { 0 },
                    { { 0 }, { "Item_FairyBless_DefenseDouble" } } },
  /* strength: the bracelet (child) and the gauntlets (adult) are separate pouch items */
  [UPG_BRACELET] = { "goron's bracelet", 0, 2, { "off", "on" }, { 0 }, { { 0 }, { "Armor_Child_Gauntlet_Goron" } } },
  [UPG_GAUNTLETS] = { "gauntlets", 0, 3, { "none", "silver", "gold" }, { 0 },
                      { { 0 }, { "Armor_Adult_Gauntlet_Silver" }, { "Armor_Adult_Gauntlet_Gold" } } },
  [UPG_SCALE]  = { "scale", 0, 3, { "none", "silver scale", "golden scale" }, { 0 },
                   { { 0 }, { "Item_Scale_Silver" }, { "Item_Scale_Gold" } } },
};

static int pending_level[UPG_COUNT] = { [0 ... UPG_COUNT - 1] = -1 };

/* every pouch item an upgrade uses, once each */
static int upgrade_items(upgrade_t u, const char **out)
{
  const upgrade_def_t *d = &upgrades[u];
  int n = 0;
  for (int l = 1; l < d->n_levels; l++)
    for (int k = 0; k < 2; k++) {
      const char *it = d->items[l][k];
      bool seen = !it;
      for (int m = 0; it && m < n; m++)
        seen |= streq(out[m], it);
      if (!seen)
        out[n++] = it;
    }
  return n;
}

static bool level_has(const upgrade_def_t *d, int level, const char *item)
{
  for (int k = 0; k < 2; k++)
    if (d->items[level][k] && streq(d->items[level][k], item))
      return true;
  return false;
}

int upgrade_level(upgrade_t u)
{
  const upgrade_def_t *d = &upgrades[u];
  const char *items[8];
  int n = upgrade_items(u, items), best = 0, best_n = 0;
  bool any = false;
  for (int i = 0; i < n; i++)
    any |= pouch_stock(items[i]) > 0;
  if (!any)
    return 0;
  for (int l = 1; l < d->n_levels; l++) {
    bool exact = true, subset = true;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
      bool held = pouch_stock(items[i]) > 0, want = level_has(d, l, items[i]);
      exact &= held == want;
      subset &= !want || held;
      cnt += want;
    }
    if (exact)
      return l;
    if (subset && cnt > best_n)
      best = l, best_n = cnt;
  }
  return best ? best : -1;
}

int upgrade_pending_level(upgrade_t u)
{
  return job_pending(KEY(K_UPGRADE, u)) ? pending_level[u] : -1;
}

/* Remove the upgrade's other items, then add the missing ones, one per frame; then write the
 * capacity and redraw the HUD (patch.set_upgrade). */
static bool job_upgrade(job_t *j)
{
  upgrade_t u = (upgrade_t)j->arg[0];
  int level = (int)j->arg[1];
  const upgrade_def_t *d = &upgrades[u];
  const char *items[8];
  int n = upgrade_items(u, items);
  while (j->i < 2 * n) {
    int k = j->i++;
    const char *it = items[k % n];
    bool want = level_has(d, level, it);
    int32_t stock = pouch_stock(it);
    if (k < n && !want && stock > 0) {
      pouch_remove_now(it);
      gd_set_bool(hash_prefixed("IsGet.", it, ""), false);
      return false;
    }
    if (k >= n && want && stock == 0) {
      pouch_add_now(it, false);
      return false;
    }
  }
  if (d->capacity_hash)
    gd_set_int(d->capacity_hash, d->capacity[level]);
  hud_refresh_now();
  log_printf("%s = %s", d->name, d->level_name[level]);
  return true;
}

void upgrade_set(upgrade_t u, int level)
{
  if (u >= UPG_COUNT || level < 0 || level >= upgrades[u].n_levels)
    return;
  pending_level[u] = level;
  job_submit(KEY(K_UPGRADE, u), job_upgrade, u, level, 0, 0);
}
