/* Age and map reload.
 *
 * Reload: the current map (Sequence_CurrentBanc) through the warp code's event request, at the start
 * point Link last arrived at (Sequence_ReservedNextStartPosName keeps it) if the map has it, else the
 * map's first entrance an exit leads to.
 *
 * New age: Sequence_IsChildhood, the map's other-age version (X_Child <-> X_Adult; maps without one,
 * like the dungeons, are shared), and the sword and shield swapped with PlayerStatus.AnotherEraEquip*,
 * which hold the other age's (the Temple of Time does the same). Adult-only tunics and boots are taken
 * off for a child. Nothing is applied to Link until the map loads. */
#include "age.h"

#include "data.h"
#include "gamedata.h"
#include "gfx.h"
#include "sys.h"
#include "warp.h"

#define F_IS_CHILDHOOD 0xCEA220D3u /* Sequence_IsChildhood */
#define F_OTHER_WEAPON 0xA1F7EE2Bu /* PlayerStatus.AnotherEraEquipWeapon */
#define F_OTHER_SHIELD 0x2342B619u /* PlayerStatus.AnotherEraEquipShield */

static int wanted = -1;

int age_current(void)
{
  bool child;
  if (!gd_get_bool(F_IS_CHILDHOOD, &child))
    return -1;
  return child ? AGE_CHILD : AGE_ADULT;
}

int age_wanted(void) { return wanted >= 0 ? wanted : age_current(); }
void age_want(int age) { wanted = age == AGE_ADULT ? AGE_ADULT : AGE_CHILD; }

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static bool starts(const char *s, const char *p)
{
  while (*p && *s == *p)
    s++, p++;
  return !*p;
}

static const warp_map_t *find_map(const char *name)
{
  for (int i = 0; i < data_n_warp_maps; i++)
    if (same(data_warp_maps[i].name, name))
      return &data_warp_maps[i];
  return 0;
}

/* name's version for `age` into out: X_Child <-> X_Adult (X_Adult_Alternative -> X_Child). false if
 * the map has an age and the other version isn't known. */
static bool age_version(const char *name, int age, char *out, int len)
{
  const char *u = name;
  for (const char *p = name; *p; p++)
    if (starts(p, "_Child") || starts(p, "_Adult")) {
      u = p;
      break;
    }
  if (u == name) { /* no age in the name: shared */
    snprintf(out, len, "%s", name);
    return true;
  }
  bool is_child = starts(u, "_Child");
  if ((age == AGE_CHILD) == is_child) {
    snprintf(out, len, "%s", name);
    return true;
  }
  snprintf(out, len, "%.*s%s", (int)(u - name), name, age == AGE_CHILD ? "_Child" : "_Adult");
  return find_map(out) != 0;
}

static void swap_str(int a, int b)
{
  const char *pa = gd_get_str(a), *pb = gd_get_str(b);
  if (!pa || !pb)
    return;
  char ta[GD_STR_LEN], tb[GD_STR_LEN];
  snprintf(ta, sizeof(ta), "%s", pa);
  snprintf(tb, sizeof(tb), "%s", pb);
  gd_set_str(a, tb);
  gd_set_str(b, ta);
}

static void take_off_adult(int slot)
{
  const char *v = gd_get_str(slot);
  if (v && starts(v, "Armor_Adult_"))
    gd_set_str(slot, "");
}

static void apply_age(int age)
{
  int ow = gd_str_index(F_OTHER_WEAPON), os = gd_str_index(F_OTHER_SHIELD);
  log_printf("age: %s (sword %s, shield %s; other age's %d, %d)", age == AGE_CHILD ? "child" : "adult",
             gd_get_str(GD_STR_SWORD) ? gd_get_str(GD_STR_SWORD) : "?",
             gd_get_str(GD_STR_SHIELD) ? gd_get_str(GD_STR_SHIELD) : "?", ow, os);
  if (ow >= 0)
    swap_str(GD_STR_SWORD, ow);
  if (os >= 0)
    swap_str(GD_STR_SHIELD, os);
  if (age == AGE_CHILD) {
    take_off_adult(GD_STR_TUNIC);
    take_off_adult(GD_STR_BOOTS);
  }
  gd_set_bool(F_IS_CHILDHOOD, age == AGE_CHILD);
}

/* warp_arm keeps the pointers until the map has changed */
static warp_map_t any_map;
static spawn_t any_spawn;
static char any_name[GD_STR_LEN], any_pos[GD_STR_LEN];

void age_reload_map(void)
{
  const char *cur = gd_get_str(GD_STR_MAP), *last = gd_get_str(GD_STR_SPAWN);
  int now = age_current(), age = age_wanted();
  if (!cur || !cur[0] || now < 0) {
    gfx_notice("can't reload: game data not found");
    return;
  }
  char name[GD_STR_LEN];
  if (!age_version(cur, age, name, sizeof(name))) {
    gfx_notice(age == AGE_CHILD ? "this map has no child version" : "this map has no adult version");
    return;
  }
  log_printf("reload: %s -> %s, last start point \"%s\", age %d -> %d", cur, name, last ? last : "?", now, age);
  const warp_map_t *map = find_map(name);
  const spawn_t *spawn = 0;
  if (map) {
    for (int i = 0; i < map->n && last && last[0] && !spawn; i++)
      if (same(data_spawns[map->first + i].pos, last))
        spawn = &data_spawns[map->first + i];
    for (int i = 0; i < map->n && !spawn; i++) /* an entrance an exit leads to */
      if (data_spawns[map->first + i].starter)
        spawn = &data_spawns[map->first + i];
    if (!spawn && map->n)
      spawn = &data_spawns[map->first];
  } else if (last && last[0]) {
    /* a map the warps don't list (houses, shops...): its last start point, arriving as from a
     * warp song (no starter actor needed) */
    snprintf(any_name, sizeof(any_name), "%s", name);
    snprintf(any_pos, sizeof(any_pos), "%s", last);
    any_map = (warp_map_t){ any_name, any_name, WC_OTHER, 0, 0, 0 };
    any_spawn = (spawn_t){ any_pos, any_pos, "Dm_SY_BancChangeArrive", 0 };
    map = &any_map;
    spawn = &any_spawn;
  }
  if (!map || !spawn) {
    gfx_notice("can't reload: no start point known for this map");
    return;
  }
  if (age != now)
    apply_age(age);
  wanted = -1; /* from now on, follow the game's again */
  warp_arm(map, spawn);
}
