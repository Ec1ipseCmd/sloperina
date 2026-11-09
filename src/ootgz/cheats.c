#include "cheats.h"

#include "gamedata.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

const char *const cheat_names[CHEAT_COUNT] = {
  "energy", "rupees", "magic", "deku sticks", "deku nuts", "deku seeds", "bombs", "arrows",
  "bombchus", "magic beans", "2x movement speed",
};
bool cheats[CHEAT_COUNT];

/* Int fields (field_hashes.json titles) */
#define F_RUPEES        0xA77921D7u
#define F_WALLET_CAP    0xE0010A50u
#define F_MAGIC         0x76B18200u /* Current magic */
#define F_MAGIC_CAP     0x12D63A97u /* Magic capacity */

/* (count field, capacity field or 0, fixed maximum) */
static const struct { uint32_t count, cap; int32_t fixed; } ammo[] = {
  [CHEAT_STICKS - CHEAT_STICKS]   = { 0xB9494411, 0x457616B0, 0 },
  [CHEAT_NUTS - CHEAT_STICKS]     = { 0xF30D9F54, 0x735B00F7, 0 },
  [CHEAT_SEEDS - CHEAT_STICKS]    = { 0x5FADF7FC, 0xAE8CC742, 0 },
  [CHEAT_BOMBS - CHEAT_STICKS]    = { 0xDB1BCD4F, 0xE753508B, 0 },
  [CHEAT_ARROWS - CHEAT_STICKS]   = { 0x518EC74B, 0x418DB791, 0 },
  [CHEAT_BOMBCHUS - CHEAT_STICKS] = { 0xDF7C517A, 0, 50 },
  [CHEAT_BEANS - CHEAT_STICKS]    = { 0xB003124D, 0, 10 },
};

static void fill(uint32_t field, int32_t top)
{
  int32_t v;
  if (gd_get_int(field, &v) && v != top)
    gd_set_int(field, top);
}

/* The magic meter object holds the real current magic: it writes clamp(value, 0, capacity)
 * into the save field every frame (FINDINGS §1 "Magic"). It holds the two field hashes at
 * +0x38/+0x3C and the value at +0x44. */
static uintptr_t magic_obj;
static uint32_t magic_retry;

static bool magic_ok(uintptr_t addr, void *unused)
{
  (void)unused;
  return addr >= 0x38 && sys_mapped(addr - 0x38, 0x48, SYS_PERM_R | SYS_PERM_W);
}

static int32_t *magic_value(void)
{
  const uint64_t keys = (uint64_t)F_MAGIC_CAP << 32 | F_MAGIC;
  if (magic_obj && sys_mapped(magic_obj + 0x38, 8, SYS_PERM_R) &&
      *(uint64_t *)(magic_obj + 0x38) == keys)
    return (int32_t *)(magic_obj + 0x44);
  magic_obj = 0;
  uint32_t now = frame_counter();
  if ((int32_t)(now - magic_retry) < 0)
    return 0;
  magic_retry = now + 150; /* ~5 s between searches while it's missing */
  uintptr_t at = mem_find_u64(keys, magic_ok, 0);
  magic_obj = at ? at - 0x38 : 0;
  return magic_obj ? (int32_t *)(magic_obj + 0x44) : 0;
}

void magic_set(int32_t value)
{
  int32_t *v = magic_value();
  if (v)
    *v = value;
  gd_set_int(F_MAGIC, value);
}

void cheats_frame(void)
{
  if (!gd_ready())
    return;
  if (cheats[CHEAT_ENERGY]) {
    int32_t *cur, *max;
    if (link_health(&cur, &max) && *cur < *max)
      *cur = *max;
  }
  if (cheats[CHEAT_RUPEES]) {
    int32_t wallet;
    if (gd_get_int(F_WALLET_CAP, &wallet))
      fill(F_RUPEES, wallet);
  }
  if (cheats[CHEAT_MAGIC]) {
    int32_t cap, cur;
    if (gd_get_int(F_MAGIC_CAP, &cap) && cap > 0 && gd_get_int(F_MAGIC, &cur) && cur != cap) {
      magic_set(cap);
    }
  }
  for (int c = CHEAT_STICKS; c <= CHEAT_BEANS; c++) {
    if (!cheats[c])
      continue;
    int32_t top = ammo[c - CHEAT_STICKS].fixed;
    if (ammo[c - CHEAT_STICKS].cap && !gd_get_int(ammo[c - CHEAT_STICKS].cap, &top))
      continue;
    fill(ammo[c - CHEAT_STICKS].count, top);
  }
}
