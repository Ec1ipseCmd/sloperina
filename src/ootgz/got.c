#include "got.h"

#include "sys.h"

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23 };

typedef struct { int64_t tag; uint64_t val; } dyn_t;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } sym_t;
typedef struct { uint64_t offset, info; int64_t addend; } rela_t;

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

uintptr_t *got_slot(uintptr_t module, const char *name)
{
  uintptr_t mod0 = module + *(const uint32_t *)(module + 4);
  if (*(const uint32_t *)mod0 != 0x30444F4D) /* "MOD0" */
    return 0;
  const dyn_t *d = (const dyn_t *)(mod0 + *(const int32_t *)(mod0 + 4));
  uint64_t symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
  for (int i = 0; i < 256 && d[i].tag != DT_NULL; i++) {
    uint64_t v = d[i].val >= module ? d[i].val - module : d[i].val; /* offset either way */
    switch (d[i].tag) {
      case DT_SYMTAB: symtab = v; break;
      case DT_STRTAB: strtab = v; break;
      case DT_RELA: rela = v; break;
      case DT_RELASZ: relasz = d[i].val; break;
      case DT_JMPREL: jmprel = v; break;
      case DT_PLTRELSZ: pltrelsz = d[i].val; break;
    }
  }
  if (!symtab || !strtab)
    return 0;
  const struct { uint64_t at, size; } tables[] = { { jmprel, pltrelsz }, { rela, relasz } };
  for (int t = 0; t < 2; t++) {
    if (!tables[t].at)
      continue;
    const rela_t *r = (const rela_t *)(module + tables[t].at);
    for (uint64_t i = 0; i < tables[t].size / sizeof(rela_t); i++) {
      uint32_t type = (uint32_t)r[i].info, sym = (uint32_t)(r[i].info >> 32);
      if (!sym || (type != 1025 /* GLOB_DAT */ && type != 1026 /* JUMP_SLOT */ && type != 257))
        continue;
      const sym_t *s = (const sym_t *)(module + symtab) + sym;
      if (s->shndx == 0 && same((const char *)(module + strtab + s->name), name))
        return (uintptr_t *)(module + r[i].offset);
    }
  }
  return 0;
}

const char *got_hook(uintptr_t module, const char *name, void *replacement)
{
  uintptr_t *slot = got_slot(module, name);
  if (!slot)
    return "import not found";
  if (!sys_mapped((uintptr_t)slot, 8, SYS_PERM_R | SYS_PERM_W))
    return "GOT is read-only";
  *slot = (uintptr_t)replacement;
  return 0;
}
