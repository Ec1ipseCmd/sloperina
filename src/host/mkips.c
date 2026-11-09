/* mkips: writes ootgz's IPS32 patch for main (build host tool).
 *
 *   mkips <out.ips>
 *
 * The patch (offsets are main offsets + 0x100, the NSO header, as Atmosphère and Lichii/Eden
 * expect; see FINDINGS §3):
 *   CAVE_START          "OOTGZ\0\0\0" + u32 version + 4 zero bytes (the module checks this)
 *   CAVE_START + 0x40   stub:  adrp x16, slot ; ldr x16, [x16, #slot_lo]
 *                              cbz  x16, 1f   ; br  x16
 *                          1:  b    unlock               (original behaviour until hooked)
 *   FRAME_SITE          b stub                            (was: b unlock, a tail call)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ootgz/ootgz_version.h"
#include "../ootgz/z64hd_v131072.h"

#define NSO_HEADER 0x100

static uint32_t b(uint32_t from, uint32_t to)
{
  return 0x14000000u | ((uint32_t)((int32_t)(to - from) >> 2) & 0x3FFFFFFu);
}

static uint32_t adrp(int rd, uint32_t from, uint32_t to)
{
  int32_t pages = (int32_t)((to & ~0xFFFu) - (from & ~0xFFFu)) >> 12;
  uint32_t lo = (uint32_t)pages & 3, hi = ((uint32_t)pages >> 2) & 0x7FFFF;
  return 0x90000000u | lo << 29 | hi << 5 | (uint32_t)rd;
}

static uint32_t ldr_x(int rt, int rn, uint32_t off)
{
  return 0xF9400000u | (off / 8) << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}

static uint32_t cbz_x(int rt, uint32_t from, uint32_t to)
{
  return 0xB4000000u | (((uint32_t)((int32_t)(to - from) >> 2) & 0x7FFFF) << 5) | (uint32_t)rt;
}

static uint32_t br(int rn) { return 0xD61F0000u | (uint32_t)rn << 5; }

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24;
}

static void record(FILE *f, uint32_t main_off, const uint8_t *data, uint16_t len)
{
  uint32_t o = main_off + NSO_HEADER;
  uint8_t h[6] = { o >> 24, o >> 16, o >> 8, o, len >> 8, len };
  fwrite(h, 1, 6, f);
  fwrite(data, 1, len, f);
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "usage: mkips <out.ips>\n");
    return 1;
  }
  _Static_assert((Z64HD_HOOK_SLOT & 7) == 0, "slot must be 8-aligned");
  _Static_assert(Z64HD_CAVE_START + OOTGZ_STUB_OFF + 20 <= Z64HD_CAVE_END, "stub outside cave");

  uint8_t marker[16] = { 0 };
  memcpy(marker, OOTGZ_MARKER, 8);
  put32(marker + 8, OOTGZ_PATCH_VERSION);

  uint32_t s = Z64HD_CAVE_START + OOTGZ_STUB_OFF;
  uint32_t code[5] = {
    adrp(16, s, Z64HD_HOOK_SLOT),
    ldr_x(16, 16, Z64HD_HOOK_SLOT & 0xFFF),
    cbz_x(16, s + 8, s + 16),
    br(16),
    b(s + 16, Z64HD_GD_UNLOCK),
  };
  uint8_t stub[20];
  for (int i = 0; i < 5; i++)
    put32(stub + 4 * i, code[i]);

  uint8_t site[4];
  put32(site, b(Z64HD_FRAME_SITE, s));

  FILE *f = fopen(argv[1], "wb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fwrite("IPS32", 1, 5, f);
  record(f, Z64HD_CAVE_START, marker, sizeof(marker));
  record(f, s, stub, sizeof(stub));
  record(f, Z64HD_FRAME_SITE, site, sizeof(site));
  fwrite("EEOF", 1, 4, f);
  if (fclose(f) != 0) {
    perror(argv[1]);
    return 1;
  }
  return 0;
}
