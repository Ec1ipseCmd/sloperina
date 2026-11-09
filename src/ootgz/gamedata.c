#include "gamedata.h"

#include "sys.h"
#include "z64hd.h"

/* --- hash ------------------------------------------------------------------------------ */

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

uint32_t murmur3(const char *s)
{
  const uint32_t c1 = 0xCC9E2D51, c2 = 0x1B873593;
  uint32_t h = 0, n = 0;
  while (s[n])
    n++;
  const uint8_t *p = (const uint8_t *)s;
  uint32_t i;
  for (i = 0; i + 4 <= n; i += 4) {
    uint32_t k = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
    k = rotl32(k * c1, 15) * c2;
    h = rotl32(h ^ k, 13) * 5 + 0xE6546B64;
  }
  uint32_t k = 0;
  switch (n & 3) {
    case 3: k ^= p[i + 2] << 16; /* fall through */
    case 2: k ^= p[i + 1] << 8;  /* fall through */
    case 1: k ^= p[i];
            h ^= rotl32(k * c1, 15) * c2;
  }
  h ^= n;
  h = (h ^ (h >> 16)) * 0x85EBCA6B;
  h = (h ^ (h >> 13)) * 0xC2B2AE35;
  return h ^ (h >> 16);
}

/* --- manager and type entries ---------------------------------------------------------- */

typedef bool (*gd_getint_t)(void *mgr, int32_t *out, uint32_t hash);
typedef void (*gd_setint_t)(void *mgr, int32_t value, uint32_t hash);

static const struct {
  uintptr_t vt;      /* type entry vtable (main+), 0 = fixed offset */
  uint32_t  stride;  /* object size */
  uint32_t  value;   /* value offset in the object; a copy follows it */
} kinds[GD_NTYPES] = {
  [GD_INT]   = { 0,                   0x28, 0x1C },
  [GD_BOOL]  = { Z64HD_VT_BOOL_TYPE,  0x18, 0x16 },
  [GD_FLOAT] = { Z64HD_VT_FLOAT_TYPE, 0x28, 0x1C },
  [GD_ENUM]  = { Z64HD_VT_ENUM_TYPE,  0x38, 0x1C },
};

static uint32_t type_off[GD_NTYPES]; /* offset of each type entry in the manager; 0 = not found */
static uintptr_t str_base;           /* address of String64 index 0's text */
static void *str_mgr;                /* manager str_base was found for */
static gd_type_t *str_type;          /* the String64 type entry */

void *gd_manager(void)
{
  uintptr_t p = sys_ptr(MAIN(Z64HD_GD_MGR_PTR));
  return (void *)(p ? sys_ptr(p) : 0);
}

void gd_reset(void)
{
  for (int i = 0; i < GD_NTYPES; i++)
    type_off[i] = 0;
  str_base = 0;
  str_mgr = 0;
}

static bool type_plausible(const gd_type_t *t)
{
  if (!sys_mapped((uintptr_t)t, sizeof(*t), SYS_PERM_R))
    return false;
  if (t->count == 0 || t->count >= 0x10000 || t->table_size == 0 || t->table_size >= 0x100000)
    return false;
  return sys_mapped((uintptr_t)t->table, (size_t)t->table_size * 8, SYS_PERM_R) &&
         sys_mapped((uintptr_t)t->objects, 8, SYS_PERM_R | SYS_PERM_W);
}

static gd_type_t *gd_type(gd_kind_t kind)
{
  uint8_t *mgr = gd_manager();
  if (!mgr)
    return 0;
  if (kind == GD_INT)
    return type_plausible((gd_type_t *)(mgr + Z64HD_GD_INT_TYPE)) ?
           (gd_type_t *)(mgr + Z64HD_GD_INT_TYPE) : 0;
  uintptr_t vt = MAIN(kinds[kind].vt);
  if (type_off[kind]) {
    gd_type_t *t = (gd_type_t *)(mgr + type_off[kind]);
    if (t->vtable == vt && type_plausible(t))
      return t;
    type_off[kind] = 0;
  }
  if (!sys_mapped((uintptr_t)mgr, Z64HD_GD_TYPES_SCAN + sizeof(gd_type_t), SYS_PERM_R))
    return 0;
  for (uint32_t o = 0; o < Z64HD_GD_TYPES_SCAN; o += 8) {
    gd_type_t *t = (gd_type_t *)(mgr + o);
    if (t->vtable == vt && type_plausible(t)) {
      type_off[kind] = o;
      return t;
    }
  }
  return 0;
}

/* The (hash, index) pair for `hash`, probing like GetInt does. */
static uint32_t *gd_lookup(const gd_type_t *t, uint32_t hash)
{
  if (!hash || !t->table_size)
    return 0;
  uint32_t start = hash % t->table_size, i = start;
  do {
    uint32_t *e = t->table + 2 * i;
    if (e[0] == hash)
      return e;
    if (e[0] == 0)
      return 0;
    i = i + 1 >= t->table_size ? 0 : i + 1;
  } while (i != start);
  return 0;
}

static uint8_t *gd_value(gd_kind_t kind, uint32_t hash)
{
  gd_type_t *t = gd_type(kind);
  if (!t)
    return 0;
  uint32_t *e = gd_lookup(t, hash);
  if (!e || e[1] >= t->count)
    return 0;
  return t->objects + (size_t)e[1] * kinds[kind].stride + kinds[kind].value;
}

bool gd_ready(void)
{
  gd_type_t *t = gd_type(GD_INT);
  return t && sys_ptr((uintptr_t)t->objects) == MAIN(Z64HD_VT_INT_FIELD);
}

/* --- Int ------------------------------------------------------------------------------- */

bool gd_get_int(uint32_t hash, int32_t *out)
{
  void *mgr = gd_manager();
  if (!mgr || !gd_ready())
    return false;
  *out = 0;
  return MAIN_FN(gd_getint_t, Z64HD_GD_GETINT)(mgr, out, hash);
}

void gd_set_int(uint32_t hash, int32_t value)
{
  void *mgr = gd_manager();
  if (mgr && gd_ready())
    MAIN_FN(gd_setint_t, Z64HD_GD_SETINT)(mgr, value, hash);
}

bool gd_poke_int(uint32_t hash, int32_t value)
{
  int32_t *v = (int32_t *)gd_value(GD_INT, hash);
  if (!v)
    return false;
  v[0] = v[1] = value;
  return true;
}

/* --- other types (direct, value + copy, as game.py) -------------------------------------- */

bool gd_get_bool(uint32_t hash, bool *out)
{
  uint8_t *v = gd_value(GD_BOOL, hash);
  if (v)
    *out = v[0] != 0;
  return v != 0;
}

bool gd_set_bool(uint32_t hash, bool value)
{
  uint8_t *v = gd_value(GD_BOOL, hash);
  if (v)
    v[0] = v[1] = value;
  return v != 0;
}

bool gd_get_float(uint32_t hash, float *out)
{
  float *v = (float *)gd_value(GD_FLOAT, hash);
  if (v)
    *out = v[0];
  return v != 0;
}

bool gd_set_float(uint32_t hash, float value)
{
  float *v = (float *)gd_value(GD_FLOAT, hash);
  if (v)
    v[0] = v[1] = value;
  return v != 0;
}

bool gd_get_enum(uint32_t hash, uint32_t *out)
{
  uint32_t *v = (uint32_t *)gd_value(GD_ENUM, hash);
  if (v)
    *out = v[0];
  return v != 0;
}

bool gd_set_enum(uint32_t hash, uint32_t value)
{
  uint32_t *v = (uint32_t *)gd_value(GD_ENUM, hash);
  if (v)
    v[0] = v[1] = value;
  return v != 0;
}

/* --- String64 ---------------------------------------------------------------------------- */

static bool plausible_name(const char *s)
{
  int n = 0;
  for (; n < GD_STR_LEN && s[n]; n++) {
    char c = s[n];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return n >= 4 && n < GD_STR_LEN;
}

/* game.py finds the pair (Sequence_CurrentBanc, 0x1F) in the String64 hash table and
 * takes index 0's text at pair + 0x207C - 0x1F * 0x108. Same here, but the table is found
 * among the manager's type entries instead of by scanning the heap. */
static bool find_strings(void)
{
  uint8_t *mgr = gd_manager();
  if (!mgr)
    return false;
  if (str_base && str_mgr == mgr && plausible_name((char *)str_base + GD_STR_MAP * Z64HD_STR_STRIDE))
    return true;
  str_base = 0;
  if (!sys_mapped((uintptr_t)mgr, Z64HD_GD_TYPES_SCAN + sizeof(gd_type_t), SYS_PERM_R))
    return false;
  for (uint32_t o = 0; o < Z64HD_GD_TYPES_SCAN; o += 8) {
    gd_type_t *t = (gd_type_t *)(mgr + o);
    if (t->vtable < z64hd_main || t->vtable >= z64hd_main + 0x6000000 || !type_plausible(t))
      continue;
    uint32_t *e = gd_lookup(t, Z64HD_STR_ANCHOR_HASH);
    if (!e || e[1] != Z64HD_STR_ANCHOR_IDX)
      continue;
    uintptr_t base = (uintptr_t)e + Z64HD_STR_FIRST_VALUE - Z64HD_STR_ANCHOR_IDX * Z64HD_STR_STRIDE;
    uintptr_t map = base + GD_STR_MAP * Z64HD_STR_STRIDE;
    if (sys_mapped(map, GD_STR_LEN, SYS_PERM_R | SYS_PERM_W) && plausible_name((char *)map)) {
      str_base = base;
      str_mgr = mgr;
      str_type = t;
      return true;
    }
  }
  return false;
}

const char *gd_get_str(int idx)
{
  if (!find_strings())
    return 0;
  return (const char *)(str_base + (uintptr_t)idx * Z64HD_STR_STRIDE);
}

bool gd_set_str(int idx, const char *s)
{
  if (!find_strings())
    return false;
  char *d = (char *)(str_base + (uintptr_t)idx * Z64HD_STR_STRIDE);
  int i = 0;
  for (; i < GD_STR_LEN - 1 && s[i]; i++)
    d[i] = s[i];
  for (; i < GD_STR_LEN; i++)
    d[i] = 0;
  return true;
}

int gd_str_index(uint32_t hash)
{
  if (!find_strings() || !str_type)
    return -1;
  uint32_t *e = gd_lookup(str_type, hash);
  return e && e[1] < str_type->count ? (int)e[1] : -1;
}
