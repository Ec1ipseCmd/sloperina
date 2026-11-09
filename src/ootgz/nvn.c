/* Drawing the menu over the game's picture, through the game's own NVN.
 *
 * How the game uses NVN (main v131072): it gets nvnDeviceGetProcAddress from
 * nvnBootstrapLoader and then fetches every function by name (loader at main+0x129e000),
 * keeping the pointers in globals. So wrapping nvnBootstrapLoader (main's GOT, see got.c)
 * and the GetProcAddress it returns lets us replace any NVN function the game uses:
 *   nvnWindowBuilderSetTextures  remember the swapchain textures (called again when the
 *                                window is rebuilt, main+0x528288)
 *   nvnQueuePresentTexture       before the game's present, copy our panel into the texture
 *                                being presented
 *   nvnDeviceInitialize          remember the device
 *
 * Drawing uses no shaders (Switch 2 shader binaries can't be built with public tools): the
 * text is rasterised on the CPU into a buffer in our own memory pool, in the swapchain's
 * format, and copied into the texture with nvnCommandBufferCopyBufferToTexture. The panel is
 * opaque. Two slots (pixel buffer + command memory + sync) alternate, and a slot is only
 * reused once the GPU has finished with it.
 *
 * Signatures and enum values are NVN 1's. Checked against this game: WindowBuilderSetTextures
 * is called as (builder, count, textures); TextureBuilderSetFormat gets 37 (RGBA8), 1, 13 and
 * 61 (RGB10A2); MemoryPoolBuilderSetFlags gets 0x21, 0x12, 0x22, 0x24, 0xA1..., all NVN 1
 * values.
 */
#include "nvn.h"

#include "font.h"
#include "airwall.h"
#include "gfx.h"
#include "sys.h"

/* --- NVN types (opaque; sizes are NVN 1's, storage is oversized in case NVN 2 grew) ------- */

typedef struct nvn_device nvn_device_t;
typedef struct nvn_queue nvn_queue_t;
typedef struct nvn_window nvn_window_t;
typedef struct nvn_texture nvn_texture_t;
typedef uint64_t nvn_addr_t;
typedef uint64_t nvn_handle_t;
typedef struct { int32_t x, y, z, w, h, d; } nvn_region_t;
typedef struct { int32_t x, y, w, h; } nvn_rect_t;

#define OBJ_STORAGE 0x400 /* NVN 1: pool builder 0x40, pool 0x100, command buffer 0xA0, sync 0x40 */
typedef struct { _Alignas(16) uint8_t b[OBJ_STORAGE]; } nvn_obj_t;

enum {
  POOL_CPU_UNCACHED = 0x02, POOL_CPU_CACHED = 0x04, POOL_GPU_UNCACHED = 0x10, POOL_GPU_CACHED = 0x20,
  SYNC_ALL_GPU_COMMANDS_COMPLETE = 0,
  FMT_RGBA8 = 37, FMT_RGBA16F = 41, FMT_RGBX8_SRGB = 55, FMT_RGBA8_SRGB = 56, FMT_RGB10A2 = 61,
};

typedef void *(*getproc_t)(nvn_device_t *dev, const char *name);
extern void *nvnBootstrapLoader(const char *name); /* from sdk */
void *aligned_alloc(size_t align, size_t size);     /* from main */
void free(void *p);                                  /* from main */

static struct {
  void (*pool_builder_defaults)(nvn_obj_t *);
  void (*pool_builder_device)(nvn_obj_t *, nvn_device_t *);
  void (*pool_builder_flags)(nvn_obj_t *, int);
  void (*pool_builder_storage)(nvn_obj_t *, void *, size_t);
  bool (*pool_init)(nvn_obj_t *, const nvn_obj_t *);
  nvn_addr_t (*pool_address)(const nvn_obj_t *);
  bool (*cmd_init)(nvn_obj_t *, nvn_device_t *);
  void (*cmd_add_command)(nvn_obj_t *, const nvn_obj_t *pool, ptrdiff_t offset, size_t size);
  void (*cmd_add_control)(nvn_obj_t *, void *, size_t);
  void (*cmd_begin)(nvn_obj_t *);
  nvn_handle_t (*cmd_end)(nvn_obj_t *);
  void (*cmd_copy_to_texture)(nvn_obj_t *, nvn_addr_t, const nvn_texture_t *, const void *view,
                              const nvn_region_t *, int flags);
  void (*queue_submit)(nvn_queue_t *, int, const nvn_handle_t *);
  void (*queue_fence)(nvn_queue_t *, nvn_obj_t *sync, int condition, int flags);
  bool (*sync_init)(nvn_obj_t *, nvn_device_t *);
  int (*sync_wait)(const nvn_obj_t *, uint64_t timeout_ns);
  int (*tex_width)(const nvn_texture_t *);
  int (*tex_height)(const nvn_texture_t *);
  int (*tex_format)(const nvn_texture_t *);
  void (*window_crop)(const nvn_window_t *, nvn_rect_t *);
  void (*cmd_copy_to_buffer)(nvn_obj_t *, const nvn_texture_t *, const void *view, const nvn_region_t *,
                             nvn_addr_t, int flags);
  void (*pool_invalidate)(const nvn_obj_t *, ptrdiff_t offset, size_t size);
  void (*set_render_targets)(nvn_obj_t *, int n, nvn_texture_t *const *colors, const void *const *views,
                             const nvn_texture_t *depth, const void *depth_view);
  void (*set_scissor)(nvn_obj_t *, int x, int y, int w, int h);
  void (*clear_color)(nvn_obj_t *, int index, const float *rgba, int mask);
  int (*window_origin)(const nvn_device_t *);
  size_t (*cmd_free)(const nvn_obj_t *);
} fn;

static const char *const fn_names[] = {
  "nvnMemoryPoolBuilderSetDefaults", "nvnMemoryPoolBuilderSetDevice", "nvnMemoryPoolBuilderSetFlags",
  "nvnMemoryPoolBuilderSetStorage", "nvnMemoryPoolInitialize", "nvnMemoryPoolGetBufferAddress",
  "nvnCommandBufferInitialize", "nvnCommandBufferAddCommandMemory", "nvnCommandBufferAddControlMemory",
  "nvnCommandBufferBeginRecording", "nvnCommandBufferEndRecording",
  "nvnCommandBufferCopyBufferToTexture", "nvnQueueSubmitCommands", "nvnQueueFenceSync",
  "nvnSyncInitialize", "nvnSyncWait", "nvnTextureGetWidth", "nvnTextureGetHeight",
  "nvnTextureGetFormat", "nvnWindowGetCrop", "nvnCommandBufferCopyTextureToBuffer",
  "nvnMemoryPoolInvalidateMappedRange", "nvnCommandBufferSetRenderTargets", "nvnCommandBufferSetScissor",
  "nvnCommandBufferClearColor", "nvnDeviceGetWindowOriginMode", "nvnCommandBufferGetCommandMemoryFree",
};
_Static_assert(sizeof(fn_names) / sizeof(*fn_names) == sizeof(fn) / sizeof(void *), "fn table");

/* --- hooks ---------------------------------------------------------------------------------- */

static getproc_t real_getproc;
static void (*real_present)(nvn_queue_t *, nvn_window_t *, int);
static void (*real_set_textures)(void *builder, int n, nvn_texture_t *const *textures);
static bool (*real_device_init)(nvn_device_t *, const void *builder);

static nvn_device_t *device;
#define MAX_TEXTURES 8
static nvn_texture_t *textures[MAX_TEXTURES];
static int n_textures;

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static void draw(nvn_queue_t *queue, nvn_window_t *window, int index);

static void hook_present(nvn_queue_t *queue, nvn_window_t *window, int index)
{
  gfx_presents++;
  draw(queue, window, index);
  real_present(queue, window, index);
}

static void hook_set_textures(void *builder, int n, nvn_texture_t *const *t)
{
  n_textures = 0;
  for (int i = 0; i < n && i < MAX_TEXTURES; i++)
    textures[i] = t[i];
  n_textures = n < MAX_TEXTURES ? n : MAX_TEXTURES;
  real_set_textures(builder, n, t);
}

static bool hook_device_init(nvn_device_t *dev, const void *builder)
{
  bool ok = real_device_init(dev, builder);
  if (ok)
    device = dev;
  return ok;
}

static void *hook_getproc(nvn_device_t *dev, const char *name);

/* Swap in our version of a function the game looks up. */
static void *wrap(const char *name, void *p)
{
  if (!p)
    return p;
  if (same(name, "nvnDeviceGetProcAddress")) {
    real_getproc = (getproc_t)p;
    return (void *)hook_getproc;
  }
  if (same(name, "nvnQueuePresentTexture")) {
    real_present = p;
    return (void *)hook_present;
  }
  if (same(name, "nvnWindowBuilderSetTextures")) {
    real_set_textures = p;
    return (void *)hook_set_textures;
  }
  if (same(name, "nvnDeviceInitialize")) {
    real_device_init = p;
    return (void *)hook_device_init;
  }
  return p;
}

static void *hook_getproc(nvn_device_t *dev, const char *name)
{
  if (dev && !device)
    device = dev;
  return wrap(name, real_getproc(dev, name));
}

void *nvn_bootstrap_hook(const char *name)
{
  return wrap(name, nvnBootstrapLoader(name));
}

/* --- overlay ---------------------------------------------------------------------------------- */

#define SLOTS 2
#define CMD_SIZE 0x40000  /* command memory per slot (see-through: a copy per sampled row) */
#define CTRL_SIZE 0x4000  /* control memory per slot */
#define MAX_SCALE 3
#define CELL_H (FONT_H + 2)
/* the text's top in a panel: half a cell of margin (then 4 font pixels over the text and 6 under it,
 * the last line's gap + the margin); panels with yoff 1 have it in the middle (watches, log, input) */
#define TEXT_TOP (FONT_H / 2)
/* Scales are in sixteenths of a screen pixel per font pixel (SC_ONE = 1x), so the text can be 1.75x
 * or 2.5x; a font pixel then covers 1 or 2 (or 2 or 3) screen pixels, nearest neighbour. */
#define SC_ONE 16
#define PANEL_W(cols, sc) ((((cols) + 2) * FONT_W * (sc) + SC_ONE - 1) / SC_ONE)
#define PANEL_H(rows, sc) ((((rows) * CELL_H + FONT_H) * (sc) + SC_ONE - 1) / SC_ONE)
/* the watches panel (1) is never wider/taller than this */
#define HUD_COLS 32
#define HUD_ROWS 20
#define LOG_COLS 48
#define INPUT_COLS 58 /* both sticks (10 + 10) and every button (36) */
#define PAD_COLS 25    /* the controller drawing */
#define PAD_ROWS 10
/* The menu panel goes under the HUD's D-pad / ocarina icon. From the HUD layout
 * (romfs UI/Layout2Archive/Common › PlayerStatus_00 › Pa_GuideCross_00 › PaGuideCross_00, a
 * 3840x2160 layout): the cluster ends 33.5% down the screen and starts 3.5% in from the left. */
#define MENU_X_PERMILLE 35
#define MENU_Y_PERMILLE 345

static enum { OV_NONE, OV_READY, OV_FAILED } state;
static nvn_obj_t cmd, pix_pool, cmd_pool, sync[SLOTS];
static uint8_t *pix_mem, *cmd_mem, *ctrl_mem;
/* Opaque: the menu panel changes only on input, so it has one buffer, rewritten after waiting
 * for the GPU; the watches change every frame, so they have one buffer per slot. */
static const int n_bufs[GFX_PANELS] = { 1, SLOTS, SLOTS, SLOTS };
static size_t pix_off[GFX_PANELS][SLOTS], pix_size[GFX_PANELS];
static int max_cols[GFX_PANELS] = { GFX_COLS, HUD_COLS, LOG_COLS, INPUT_COLS },
           max_rows[GFX_PANELS] = { GFX_ROWS, HUD_ROWS, GFX_LOG_ROWS, PAD_ROWS };
/* the input display is either one long row or the controller (PAD_COLS x PAD_ROWS), never both at
 * once: its buffers are sized for the larger of the two */
static size_t panel_area(int p, int sc)
{
  size_t a = (size_t)PANEL_W(max_cols[p], sc) * PANEL_H(p == GFX_PANEL_INPUT ? 1 : max_rows[p], sc);
  if (p == GFX_PANEL_INPUT) {
    size_t b = (size_t)PANEL_W(PAD_COLS, sc) * PANEL_H(PAD_ROWS, sc);
    a = a > b ? a : b;
  }
  return a;
}
static bool slot_used[SLOTS];
static uint32_t drawn_hash[GFX_PANELS][SLOTS];
static int drawn_w[GFX_PANELS][SLOTS], drawn_h[GFX_PANELS][SLOTS];
static unsigned frame;
static int fmt, bpp, init_scale;
static bool origin_lower_left; /* scissor rectangles count from the bottom (NVN's default) */
static bool warned_format;

static void fail(const char *why)
{
  log_printf("on-screen drawing off: %s", why);
  state = OV_FAILED;
}

static size_t round_page(size_t n) { return (n + 0xFFF) & ~(size_t)0xFFF; }

static bool make_pool(nvn_obj_t *pool, void *mem, size_t size, int flags)
{
  nvn_obj_t b;
  memset(&b, 0, sizeof(b));
  fn.pool_builder_defaults(&b);
  fn.pool_builder_device(&b, device);
  fn.pool_builder_flags(&b, flags);
  fn.pool_builder_storage(&b, mem, size);
  return fn.pool_init(pool, &b);
}

static int format_bpp(int f)
{
  switch (f) {
    case FMT_RGBA8: case FMT_RGBA8_SRGB: case FMT_RGBX8_SRGB: case FMT_RGB10A2: return 4;
    case FMT_RGBA16F: return 8;
    default: return 0;
  }
}

static bool is_rgba8(int f) { return f == FMT_RGBA8 || f == FMT_RGBA8_SRGB || f == FMT_RGBX8_SRGB; }

static int scale_for(int height)
{
  int s = height / 360;
  return s < 1 ? 1 : s > MAX_SCALE ? MAX_SCALE : s;
}

static bool init(void)
{
  if (!real_getproc || !device)
    return false; /* not yet: try again next frame */
  void **slots = (void **)&fn;
  for (unsigned i = 0; i < sizeof(fn_names) / sizeof(*fn_names); i++)
    if (!(slots[i] = real_getproc(device, fn_names[i]))) {
      log_printf("NVN has no %s", fn_names[i]);
      fail("missing NVN function");
      return false;
    }
  int th = fn.tex_height(textures[0]);
  fmt = fn.tex_format(textures[0]);
  bpp = format_bpp(fmt);
  log_printf("swapchain: %d textures, %dx%d, format %d", n_textures, fn.tex_width(textures[0]), th, fmt);
  if (!bpp) {
    fail("unsupported swapchain format (see the log line above)");
    return false;
  }
  /* sized for the scale at this resolution; a larger picture later keeps this scale */
  init_scale = scale_for(th);
  size_t total = 0;
  for (int p = 0; p < GFX_PANELS; p++) {
    pix_size[p] = round_page(panel_area(p, init_scale * SC_ONE) * bpp);
    for (int b = 0; b < n_bufs[p]; b++) {
      pix_off[p][b] = total;
      total += pix_size[p];
    }
  }
  pix_mem = aligned_alloc(0x1000, total);
  cmd_mem = aligned_alloc(0x1000, CMD_SIZE * SLOTS);
  ctrl_mem = aligned_alloc(0x1000, CTRL_SIZE * SLOTS);
  if (!pix_mem || !cmd_mem || !ctrl_mem) {
    fail("out of memory");
    return false;
  }
  if (!make_pool(&pix_pool, pix_mem, total, POOL_CPU_UNCACHED | POOL_GPU_UNCACHED) ||
      !make_pool(&cmd_pool, cmd_mem, CMD_SIZE * SLOTS, POOL_CPU_UNCACHED | POOL_GPU_CACHED)) {
    fail("memory pool");
    return false;
  }
  if (!fn.cmd_init(&cmd, device)) {
    fail("command buffer");
    return false;
  }
  for (int s = 0; s < SLOTS; s++)
    if (!fn.sync_init(&sync[s], device)) {
      fail("sync");
      return false;
    }
  origin_lower_left = fn.window_origin(device) == 0;
  log_printf("on-screen drawing ready (scale %d, %u KiB of panels, origin %s)", init_scale, (unsigned)(total / 1024),
             origin_lower_left ? "lower left" : "upper left");
  state = OV_READY;
  return true;
}

/* --- see-through background (opacity < 100) ------------------------------------------------------
 * A copy can't blend, so: each frame the region under a panel is also copied out of the picture
 * (texture to buffer, before the panel goes in), and two frames later, once that slot's fence has
 * passed, the CPU blends the panel's background with it. The background therefore lags the game
 * by two frames. To keep the memory small, only one row per text-pixel row (every `scale`th row)
 * is read back, and the text mask is kept at font resolution. Only for RGBA8 pictures; allocated
 * the first time it's used, in separate pieces (the game's heap had no room for one big block). */

#define TL_PIECES (1 + 2 * SLOTS)
static enum { TL_NONE, TL_READY, TL_FAILED } tl_state;
static nvn_obj_t tl_out_pool, tl_rb_pool[GFX_PANELS][SLOTS];
static uint8_t *tl_out_mem, *tl_rb_mem[GFX_PANELS][SLOTS], *tl_mask[GFX_PANELS];
static size_t tl_out_size, tl_rb_size[GFX_PANELS], tl_mask_size[GFX_PANELS];
static nvn_region_t tl_rb_region[GFX_PANELS][SLOTS]; /* what each readback holds */
static uint32_t mask_hash[GFX_PANELS];
static int mask_cols[GFX_PANELS], mask_rows[GFX_PANELS];
#define MASK_W(cols) (((cols) + 2) * FONT_W)            /* font pixels, unscaled */
#define MASK_H(rows) ((rows) * CELL_H + FONT_H)

static void tl_free(void)
{
  free(tl_out_mem);
  tl_out_mem = 0;
  for (int p = 0; p < GFX_PANELS; p++) {
    free(tl_mask[p]);
    tl_mask[p] = 0;
    for (int b = 0; b < SLOTS; b++) {
      free(tl_rb_mem[p][b]);
      tl_rb_mem[p][b] = 0;
    }
  }
}

/* max_h0: the tallest the menu panel can be on screen (under the D-pad) */
static bool tl_init(int max_h0)
{
  int max_w[GFX_PANELS], max_h[GFX_PANELS];
  for (int p = 0; p < GFX_PANELS; p++) {
    max_w[p] = PANEL_W(max_cols[p], init_scale * SC_ONE);
    max_h[p] = PANEL_H(max_rows[p], init_scale * SC_ONE);
  }
  if (max_h0 > 0 && max_h0 < max_h[0])
    max_h[0] = max_h0;
  size_t total = 0;
  tl_out_size = round_page((size_t)max_w[0] * max_h[0] * 4); /* the menu panel's second slot */
  tl_out_mem = aligned_alloc(0x1000, tl_out_size);
  total += tl_out_size;
  bool ok = tl_out_mem && make_pool(&tl_out_pool, tl_out_mem, tl_out_size, POOL_CPU_UNCACHED | POOL_GPU_UNCACHED);
  for (int p = 0; p < GFX_PANELS && ok; p++) {
    /* one row per font-pixel row: at most the panel's font height, whatever the scale */
    tl_rb_size[p] = round_page(p == GFX_PANEL_INPUT ? panel_area(p, init_scale * SC_ONE) / init_scale * 4 + max_w[p] * 8
                                                    : (size_t)max_w[p] * (MASK_H(max_rows[p]) + 1) * 4);
    tl_mask_size[p] = (size_t)MASK_W(max_cols[p]) * MASK_H(max_rows[p]);
    ok = (tl_mask[p] = aligned_alloc(64, tl_mask_size[p])) != 0;
    for (int b = 0; b < SLOTS && ok; b++) {
      ok = (tl_rb_mem[p][b] = aligned_alloc(0x1000, tl_rb_size[p])) != 0 &&
           make_pool(&tl_rb_pool[p][b], tl_rb_mem[p][b], tl_rb_size[p], POOL_CPU_CACHED | POOL_GPU_UNCACHED);
      total += tl_rb_size[p];
    }
    total += tl_mask_size[p];
  }
  if (!ok) {
    log_printf("see-through background off: not enough memory (needs %u KiB)", (unsigned)(total / 1024));
    gfx_notice("see-through background: the game has no memory free for it");
    tl_free();
    tl_state = TL_FAILED;
    return false;
  }
  log_printf("see-through background ready (%u KiB)", (unsigned)(total / 1024));
  tl_state = TL_READY;
  return true;
}

/* where panel p's buffer for slot/buffer b lives: CPU pointer and GPU address */
static uint8_t *out_ptr(int p, int b)
{
  return p == 0 && b == 1 ? tl_out_mem : pix_mem + pix_off[p][b];
}
static nvn_addr_t out_addr(int p, int b)
{
  return p == 0 && b == 1 ? fn.pool_address(&tl_out_pool) : fn.pool_address(&pix_pool) + pix_off[p][b];
}

/* --- drawing ------------------------------------------------------------------------------------- */

/* colours (sRGB 0..255) */
static const uint8_t style_rgb[GFX_NSTYLES][3] = {
  [GFX_NORMAL] = { 0xFF, 0xFF, 0xFF }, [GFX_SELECTED] = { 0x80, 0xFF, 0x80 },
  [GFX_TITLE] = { 0xA0, 0xC8, 0xFF }, [GFX_DIM] = { 0x90, 0x90, 0x90 },
  [GFX_NOTICE] = { 0xFF, 0xE0, 0x60 }, [GFX_LOG] = { 0xC0, 0xC0, 0xC0 }, /* gz's log colour */
  [GFX_FAINT] = { 0x50, 0x50, 0x58 },  /* the controller's buttons not held */
};
static const uint8_t bg_rgb[3] = { 0x18, 0x18, 0x20 };

static uint16_t half(uint8_t v) /* v / 255 as an IEEE half (0..1) */
{
  if (!v)
    return 0;
  float f = v / 255.0f;
  uint32_t b;
  memcpy(&b, &f, 4);
  int e = (int)((b >> 23) & 0xFF) - 127 + 15;
  return (uint16_t)(e << 10 | ((b >> 13) & 0x3FF));
}

static uint64_t pixel(const uint8_t rgb[3])
{
  switch (fmt) {
    case FMT_RGB10A2:
      return (uint32_t)(rgb[0] * 1023 / 255) | (uint32_t)(rgb[1] * 1023 / 255) << 10 |
             (uint32_t)(rgb[2] * 1023 / 255) << 20 | 3u << 30;
    case FMT_RGBA16F:
      return half(rgb[0]) | (uint64_t)half(rgb[1]) << 16 | (uint64_t)half(rgb[2]) << 32 |
             (uint64_t)0x3C00 << 48;
    default:
      return rgb[0] | rgb[1] << 8 | rgb[2] << 16 | 0xFFu << 24;
  }
}

/* Palette of a panel pixel: 0 background, 1 + style for text, then gz's checkbox colours. */
enum {
  PAL_TEXT = 1,
  PAL_OUTLINE = PAL_TEXT + GFX_NSTYLES, PAL_BEVEL_HI, PAL_BEVEL_LO, PAL_TICK,
  PAL_REMOVE, PAL_REMOVE_HOT, PAL_HOT_LIGHT, PAL_FOLDER, /* + 0..3: the folder's yellows */
  PAL_SHADOW = PAL_FOLDER + 4, /* the log's drop shadow */
  PAL_FILL, /* + style: the box's inside, half the style colour */
  PAL_COUNT = PAL_FILL + GFX_NSTYLES
};

/* gz's checkbox.png, 10x10 tiles: the box (tile 0) and the tick drawn over it (tile 2). '#' outline,
 * 'c'/'b' the inner edge (light, dark), 'w' the inside, 'G' the tick. */
static const char check_box[10][11] = {
  "..........", "########..", "#cccccc#..", "#cwwwwb#..", "#cwwwwb#..",
  "#cwwwwb#..", "#cwwwwb#..", "#cbbbbb#..", "########..", "..........",
};
static const char check_tick[10][11] = {
  "........GG", ".......GG.", "......GG..", ".....GG...", "GG...GG...",
  ".GG.GG....", "..GGGG....", "...GG.....", "....G.....", "..........",
};
/* list_icons.png tile 1 (gz draws it white tinted red; its soft edges made solid) and file_icons.png
 * tile 1, 'C'..'F' its yellows. Rows as the checkbox's: row 1 is the glyph's top. */
static const char icon_remove[10][11] = {
  "........", "XX....XX", "XXX..XXX", ".XXXXXX.", "..XXXX..",
  "..XXXX..", ".XXXXXX.", "XXX..XXX", "XX....XX", "........",
};
static const char icon_wrench[10][11] = { /* wrench.png, its soft edges made solid */
  "........", "....XX..", "....X...", "...XX..X", "...XXXX.",
  "..XXX...", ".XXX....", "XXX.....", "XX......", "........",
};
static const char icon_up[10][11] = { /* arrow_icons.png tiles 0 and 1 */
  "........", "........", "...XX...", "..XXXX..", ".XX..XX.", ".X....X.", "X......X", "X......X",
  "........", "........",
};
static const char icon_down[10][11] = {
  "........", "........", "X......X", "X......X", ".X....X.", ".XX..XX.", "..XXXX..", "...XX...",
  "........", "........",
};
static const char icon_add[10][11] = { /* '#' the outline, 'X' the plus (gz tints it green) */
  "........", "..####..", "..#XX#..", "###XX###", "#XXXXXX#", "#XXXXXX#", "###XX###", "..#XX#..",
  "..####..", "........",
};
static const char icon_right[10][11] = { /* arrow_icons.png tiles 2 and 3 */
  "........", ".XX.....", "...XX...", "....XX..", ".....XX.", ".....XX.", "....XX..", "...XX...", ".XX.....",
  "........",
};
static const char icon_left[10][11] = {
  "........", ".....XX.", "...XX...", "..XX....", ".XX.....", ".XX.....", "..XX....", "...XX...", ".....XX.",
  "........",
};
static const char icon_folder[10][11] = {
  ".........", ".CCC.....", "CDDDC....", "EEEEEEEE.", "EFFFFFFFE",
  "EFFFFFFFE", "EFFFFFFFE", "EEEEEEEEE", ".........", ".........",
};

/* a pixel of icon c at (sx, ty): sx across its cells, ty its tile row */
static uint8_t sprite_pixel(unsigned char c, int sx, int ty, int style)
{
  if (sx < 0 || ty < 0 || ty >= 10)
    return 0;
  if (c == (unsigned char)GFX_ICON_REMOVE)
    return sx < 8 && icon_remove[ty][sx] == 'X' ? (style == GFX_SELECTED ? PAL_REMOVE_HOT : PAL_REMOVE) : 0;
  if (c == (unsigned char)GFX_ICON_ADD) {
    char k = sx < 8 ? icon_add[ty][sx] : '.';
    return k == '#' ? PAL_OUTLINE : k == 'X' ? (style == GFX_SELECTED ? PAL_REMOVE_HOT : PAL_TICK) : 0;
  }
  if (c == (unsigned char)GFX_ICON_WRENCH || c == (unsigned char)GFX_ICON_UP || c == (unsigned char)GFX_ICON_DOWN ||
      c == (unsigned char)GFX_ICON_LEFT || c == (unsigned char)GFX_ICON_RIGHT) {
    const char (*t)[11] = c == (unsigned char)GFX_ICON_WRENCH ? icon_wrench
                          : c == (unsigned char)GFX_ICON_UP   ? icon_up
                          : c == (unsigned char)GFX_ICON_DOWN ? icon_down
                          : c == (unsigned char)GFX_ICON_LEFT ? icon_left
                                                              : icon_right;
    /* white, or the highlight colour */
    /* in the style's colour (white in the menu, faint on the controller), blue when selected */
    return sx < 8 && t[ty][sx] == 'X' ? (style == GFX_SELECTED ? PAL_REMOVE_HOT : PAL_TEXT + style) : 0;
  }
  if (sx >= 10)
    return 0;
  if (c == (unsigned char)GFX_ICON_FOLDER) { /* selected: in the highlight colour */
    char k = icon_folder[ty][sx];
    if (k < 'C' || k > 'F')
      return 0;
    return style == GFX_SELECTED ? (k == 'E' ? PAL_REMOVE_HOT : PAL_HOT_LIGHT) : (uint8_t)(PAL_FOLDER + (k - 'C'));
  }
  if (c == (unsigned char)GFX_CHECK_ON && check_tick[ty][sx] == 'G') /* the tick: blue when selected */
    return style == GFX_SELECTED ? PAL_REMOVE_HOT : PAL_TICK;
  switch (check_box[ty][sx]) {
    case '#': return PAL_OUTLINE;
    case 'c': return PAL_BEVEL_HI;
    case 'b': return PAL_BEVEL_LO;
    case 'w': return (uint8_t)(PAL_FILL + style);
    default: return 0;
  }
}

static inline bool is_sprite(unsigned char c)
{
  return c == (unsigned char)GFX_CHECK_OFF || c == (unsigned char)GFX_CHECK_ON ||
         c == (unsigned char)GFX_ICON_REMOVE || c == (unsigned char)GFX_ICON_FOLDER ||
         c == (unsigned char)GFX_ICON_WRENCH || c == (unsigned char)GFX_ICON_UP || c == (unsigned char)GFX_ICON_DOWN ||
         c == (unsigned char)GFX_ICON_ADD || c == (unsigned char)GFX_ICON_LEFT || c == (unsigned char)GFX_ICON_RIGHT;
}

/* The input display's controller drawing (a Switch 2 Pro Controller from above), in font pixels from
 * the text's top-left: the body's outline (a bar with rounded corners and a grip under each end), the
 * sticks (a ring with a dot where the stick is, filled while clicked in) and the d-pad (a cross, an arm
 * filled while held). The other buttons are text (watches.c). Rows of 10, columns of 8: the body
 * takes rows 1..8 of 24 columns, the readouts row 9. */
#define PAD_W 192
static const struct { int cx, cy; } pad_sticks[2] = { { 36, 36 }, { 128, 56 } };
#define PAD_STICK_R 10
#define DPAD_X 64
#define DPAD_Y 56
#define DPAD_ARM 9  /* from the centre to an arm's end */
#define DPAD_HALF 3 /* half an arm's width */

static bool pad_inside(int x, int y)
{
  /* the bar: x 4..188, y 12..70, corners rounded (14 at the top, 10 at the bottom) */
  if (x >= 4 && x <= 188 && y >= 12 && y <= 70) {
    int r = y < 41 ? 14 : 10, cy = y < 41 ? 12 + r : 70 - r;
    int cx = x < 4 + r ? 4 + r : x > 188 - r ? 188 - r : x;
    if (cx == x || (y >= 12 + 14 && y <= 70 - 10))
      return true;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
  }
  /* the grips: circles under each end */
  int dl = (x - 36) * (x - 36) + (y - 70) * (y - 70), dr = (x - 156) * (x - 156) + (y - 70) * (y - 70);
  return dl <= 18 * 18 || dr <= 18 * 18;
}

static bool dpad_inside(int dx, int dy)
{
  int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
  return (ax <= DPAD_HALF && ay <= DPAD_ARM) || (ay <= DPAD_HALF && ax <= DPAD_ARM);
}

static uint8_t pad_pixel(const gfx_panel_t *s, int x, int y)
{
  for (int k = 0; k < 2; k++) {
    int dx = x - pad_sticks[k].cx, dy = y - pad_sticks[k].cy, d = dx * dx + dy * dy;
    if (d > PAD_STICK_R * PAD_STICK_R)
      continue;
    int sx = pad_sticks[k].cx + s->sticks[2 * k] * (PAD_STICK_R - 3) / 100;
    int sy = pad_sticks[k].cy - s->sticks[2 * k + 1] * (PAD_STICK_R - 3) / 100;
    int ex = x - sx, ey = y - sy;
    if (ex * ex + ey * ey <= 5)
      return PAL_TEXT + GFX_NORMAL;                              /* where the stick is */
    if (d >= (PAD_STICK_R - 1) * (PAD_STICK_R - 1))
      return PAL_TEXT + GFX_DIM;                                 /* its ring */
    return s->clicks >> k & 1 ? PAL_TEXT + GFX_FAINT : 0;        /* clicked in */
  }
  int dx = x - DPAD_X, dy = y - DPAD_Y;
  if (dpad_inside(dx, dy)) {
    if (!dpad_inside(dx - 1, dy) || !dpad_inside(dx + 1, dy) || !dpad_inside(dx, dy - 1) || !dpad_inside(dx, dy + 1))
      return PAL_TEXT + GFX_DIM; /* its edge */
    /* the arm a pixel is on: up, down, left, right (clicks bits 2..5) */
    int arm = dy < -DPAD_HALF ? 2 : dy > DPAD_HALF ? 3 : dx < -DPAD_HALF ? 4 : dx > DPAD_HALF ? 5 : -1;
    return arm >= 0 && (s->clicks >> arm & 1) ? PAL_TEXT + GFX_NORMAL : 0;
  }
  if (x < 0 || x > PAD_W || !pad_inside(x, y))
    return 0;
  if (!pad_inside(x - 1, y) || !pad_inside(x + 1, y) || !pad_inside(x, y - 1) || !pad_inside(x, y + 1))
    return PAL_TEXT + GFX_FAINT; /* the outline */
  return 0;
}

/* What's at pixel (x, y) of a panel (a PAL_ index). Rows of 8x8 glyphs plus 2 px spacing, a cell of
 * margin left and right and half a cell above and below, scaled. A checkbox is 10 px tall: its
 * top row is in the spacing below the row above. */
static inline uint8_t cell_at(const gfx_panel_t *s, int x, int y, int sc)
{
  int py = y * SC_ONE / sc - TEXT_TOP - s->yoff, px = x * SC_ONE / sc - FONT_W;
  if (py < 0 || px < 0)
    return 0;
  int row = py / CELL_H, gy = py % CELL_H, col = px / FONT_W, gx = px % FONT_W;
  if (row >= GFX_ROWS || col >= GFX_COLS)
    return 0;
  if (s->pad) { /* the controller drawing, under the buttons' text */
    unsigned char c0 = (unsigned char)s->ch[row][col];
    bool glyph = gy < FONT_H && c0 >= FONT_FIRST && c0 < FONT_FIRST + FONT_COUNT &&
                 (font_8x8[c0 - FONT_FIRST][gy] >> (7 - gx) & 1);
    if (!glyph && !is_sprite(c0)) {
      uint8_t k = pad_pixel(s, px, py);
      if (k)
        return k;
    }
  }
  for (int dr = 0; dr <= 1; dr++) { /* this row, or the top of a checkbox in the row below */
    int r = row + dr, ty = dr ? 0 : gy + 1;
    if (r >= GFX_ROWS || (dr && gy != CELL_H - 1))
      break;
    unsigned char c = (unsigned char)s->ch[r][col];
    int cc = col, sx = gx;
    if (c == (unsigned char)GFX_WIDE_REST && col > 0)
      cc = col - 1, sx = gx + FONT_W, c = (unsigned char)s->ch[r][cc];
    if (is_sprite(c)) {
      int st = s->style[r][cc] < GFX_NSTYLES ? s->style[r][cc] : 0;
      uint8_t k = sprite_pixel(c, sx, ty, st);
      if (k || !dr)
        return k;
    }
  }
  if (gy >= FONT_H)
    return 0;
  unsigned char c = (unsigned char)s->ch[row][col];
  if (c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT || !(font_8x8[c - FONT_FIRST][gy] >> (7 - gx) & 1))
    return 0;
  return PAL_TEXT + (s->style[row][col] < GFX_NSTYLES ? s->style[row][col] : 0);
}

/* the colours of the PAL_ indices (alphas from checkbox.png, blended over the background) */
static void palette(uint8_t pal[PAL_COUNT][3])
{
  memcpy(pal[0], bg_rgb, 3);
  for (int i = 0; i < GFX_NSTYLES; i++) {
    memcpy(pal[PAL_TEXT + i], style_rgb[i], 3);
    for (int c = 0; c < 3; c++)
      pal[PAL_FILL + i][c] = (uint8_t)((style_rgb[i][c] * 0x80 + bg_rgb[c] * 0x7F) / 255);
  }
  for (int c = 0; c < 3; c++) {
    pal[PAL_OUTLINE][c] = 0;
    pal[PAL_BEVEL_HI][c] = (uint8_t)((0x1D * 0xE5 + bg_rgb[c] * 0x1A) / 255);
    pal[PAL_BEVEL_LO][c] = (uint8_t)((0x0F * 0xF1 + bg_rgb[c] * 0x0E) / 255);
    pal[PAL_TICK][c] = c == 1 ? 0xFF : 0;
    pal[PAL_REMOVE][c] = c == 0 ? 0xFF : 0;
    /* gz's highlight colour is 0x2020FF; lightened, as on our dark background that was hard to see */
    pal[PAL_REMOVE_HOT][c] = c == 2 ? 0xFF : 0x58;
    pal[PAL_HOT_LIGHT][c] = c == 2 ? 0xFF : 0x98;
  }
  static const uint8_t folder[4][3] = {
    { 247, 209, 57 }, { 250, 237, 122 }, { 255, 227, 91 }, { 255, 255, 136 },
  };
  memcpy(pal[PAL_FOLDER], folder, sizeof(folder));
  memset(pal[PAL_SHADOW], 0, 3);
}

/* Opaque: write the panel straight into a buffer in the picture's format. Rows are built in a
 * cached line buffer and then copied to the (uncached) pool memory. */
static _Alignas(16) uint8_t line[PANEL_W(GFX_COLS, MAX_SCALE * SC_ONE) * 8];
static int font_x[PANEL_W(GFX_COLS, MAX_SCALE * SC_ONE)]; /* screen x -> font x */

static void rasterise(const gfx_panel_t *s, uint8_t *dst, int sc, int w, int h)
{
  uint8_t pal[PAL_COUNT][3];
  uint64_t v[PAL_COUNT];
  palette(pal);
  for (int i = 0; i < PAL_COUNT; i++)
    v[i] = pixel(pal[i]);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint64_t c = v[cell_at(s, x, y, sc)];
      if (bpp == 8)
        memcpy(line + x * 8, &c, 8);
      else {
        uint32_t c32 = (uint32_t)c;
        memcpy(line + x * 4, &c32, 4);
      }
    }
    memcpy(dst + (size_t)y * w * bpp, line, (size_t)w * bpp);
  }
}

/* See-through: the text as a byte mask at font resolution (cached; rebuilt when it changes) ... */
static void build_mask(int p, const gfx_panel_t *s, int cols, int rows)
{
  int mw = MASK_W(cols), mh = MASK_H(rows);
  if ((size_t)mw * mh > tl_mask_size[p])
    return;
  for (int y = 0; y < mh; y++)
    for (int x = 0; x < mw; x++)
      tl_mask[p][(size_t)y * mw + x] = cell_at(s, x, y, SC_ONE);
  if (s->clear) /* no background (log, input display): gz's drop shadow, one pixel down and right */
    for (int y = mh - 1; y > 0; y--)
      for (int x = mw - 1; x > 0; x--) {
        uint8_t *m = &tl_mask[p][(size_t)y * mw + x], up = tl_mask[p][(size_t)(y - 1) * mw + x - 1];
        if (!*m && up && up != PAL_SHADOW)
          *m = PAL_SHADOW;
      }
  mask_hash[p] = s->hash, mask_cols[p] = cols, mask_rows[p] = rows;
}

/* --- panels with no background (the log; the input display without its background) ------------
 * Only their pixels are drawn, as cleared rectangles straight into the picture (as the air walls are):
 * a run of one colour in a font-pixel row is a rectangle, and runs repeated in the rows under it (in the
 * same text row) grow it. Nothing around the text is touched, so nothing behind it lags or sticks. A
 * clear can't be see-through: a log line fading out is blended by fade_compose instead. */
#define CLEAR_MASK (MASK_W(INPUT_COLS) * MASK_H(PAD_ROWS))
#define CLEAR_RECTS 3072
typedef struct { uint16_t x, y, w, h; uint8_t k, row; } crect_t;
static uint8_t clear_mask[2][CLEAR_MASK];
static crect_t crects[2][CLEAR_RECTS];
static int n_crects[2], crect_cols[2], crect_rows[2];
static uint32_t crect_hash[2];

static void build_clear(int i, const gfx_panel_t *s, int cols, int rows)
{
  int mw = MASK_W(cols), mh = MASK_H(rows);
  n_crects[i] = 0;
  crect_hash[i] = s->hash, crect_cols[i] = cols, crect_rows[i] = rows;
  if ((size_t)mw * mh > CLEAR_MASK)
    return;
  uint8_t *m = clear_mask[i];
  for (int y = 0; y < mh; y++)
    for (int x = 0; x < mw; x++)
      m[(size_t)y * mw + x] = cell_at(s, x, y, SC_ONE);
  for (int y = mh - 1; y > 0; y--) /* gz's drop shadow, one pixel down and right */
    for (int x = mw - 1; x > 0; x--) {
      uint8_t *p = &m[(size_t)y * mw + x], up = m[(size_t)(y - 1) * mw + x - 1];
      if (!*p && up && up != PAL_SHADOW)
        *p = PAL_SHADOW;
    }
  crect_t *r = crects[i];
  static uint16_t act[2][512]; /* the rectangles that reached the row above (still growing) */
  int n = 0, n_act = 0, flip = 0;
  for (int y = 0; y < mh; y++) {
    int py = y - TEXT_TOP - s->yoff, row = py < 0 ? 0 : py / CELL_H, n_cur = 0;
    uint16_t *prev = act[flip], *cur = act[!flip];
    for (int x = 0; x < mw;) {
      uint8_t k = m[(size_t)y * mw + x];
      int x0 = x;
      while (x < mw && m[(size_t)y * mw + x] == k)
        x++;
      if (!k)
        continue;
      int j = 0; /* the same run in the row above (same text row): grow it */
      while (j < n_act && !(r[prev[j]].x == x0 && r[prev[j]].w == x - x0 && r[prev[j]].k == k &&
                            r[prev[j]].row == row && r[prev[j]].y + r[prev[j]].h == y))
        j++;
      int idx;
      if (j < n_act) {
        idx = prev[j];
        r[idx].h++;
      } else {
        if (n >= CLEAR_RECTS)
          continue;
        idx = n++;
        r[idx] = (crect_t){ (uint16_t)x0, (uint16_t)y, (uint16_t)(x - x0), 1, k, (uint8_t)row };
      }
      if (n_cur < 512)
        cur[n_cur++] = (uint16_t)idx;
    }
    n_act = n_cur, flip = !flip;
  }
  n_crects[i] = n;
}

static void draw_clear(int i, const gfx_panel_t *s, nvn_texture_t *tex, nvn_rect_t at, int th, int sc)
{
  uint8_t pal[PAL_COUNT][3];
  palette(pal);
  nvn_texture_t *rt[1] = { tex };
  fn.set_render_targets(&cmd, 1, rt, 0, 0, 0);
  const crect_t *r = crects[i];
  for (int j = 0; j < n_crects[i]; j++) {
    if ((j & 63) == 0 && fn.cmd_free(&cmd) < 16384)
      break;
    int row = r[j].row < GFX_ROWS ? r[j].row : GFX_ROWS - 1;
    unsigned a = s->alpha[row];
    if (a == 0)
      continue;
    int fx0 = r[j].x, fx1 = r[j].x + r[j].w; /* (a fading line is drawn by fade_compose; solid here) */
    float c[4] = { pal[r[j].k][0] / 255.0f, pal[r[j].k][1] / 255.0f, pal[r[j].k][2] / 255.0f, 1 };
    int x0 = (fx0 * sc + SC_ONE - 1) / SC_ONE, x1 = (fx1 * sc + SC_ONE - 1) / SC_ONE;
    int y0 = (r[j].y * sc + SC_ONE - 1) / SC_ONE, y1 = ((r[j].y + r[j].h) * sc + SC_ONE - 1) / SC_ONE;
    if (x1 > at.w)
      x1 = at.w;
    if (y1 > at.h)
      y1 = at.h;
    if (x1 <= x0 || y1 <= y0)
      continue;
    int y = at.y + y0, h = y1 - y0;
    fn.set_scissor(&cmd, at.x + x0, origin_lower_left ? th - y - h : y, x1 - x0, h);
    fn.clear_color(&cmd, 0, c, 0xF);
  }
  fn.set_scissor(&cmd, 0, 0, 65535, 65535);
}

/* --- the log's fade (gz's: the text's opacity going down) -----------------------------------------
 * While a log line fades, the picture under the log is read back first, in its own small command
 * buffer that's waited for (so it's this frame's picture, nothing stale), and the text is blended into
 * it at the line's opacity on the CPU; the result is copied over the log. Only during a fade (about a
 * second): the wait costs some time. If anything fails, that frame draws the text solid (draw_clear). */
#define FADE_CMD 0x8000  /* command / control memory taken from the end of the frame's slot */
#define FADE_CTRL 0x1000
static enum { FADE_NONE, FADE_READY, FADE_FAILED } fade_state;
static nvn_obj_t fade_pool, fade_sync;
static uint8_t *fade_mem;
static size_t fade_size;

static bool fade_init(void)
{
  fade_size = round_page((size_t)PANEL_W(LOG_COLS, init_scale * SC_ONE) * PANEL_H(GFX_LOG_ROWS, init_scale * SC_ONE) * 4);
  fade_mem = aligned_alloc(0x1000, fade_size);
  if (!fade_mem || !make_pool(&fade_pool, fade_mem, fade_size, POOL_CPU_CACHED | POOL_GPU_UNCACHED) ||
      !fn.sync_init(&fade_sync, device)) {
    log_printf("log fade off: no memory for it");
    fade_state = FADE_FAILED;
    return false;
  }
  fade_state = FADE_READY;
  return true;
}

static bool fading(const gfx_panel_t *s)
{
  for (int r = 0; r < s->rows && r < GFX_ROWS; r++)
    if (s->alpha[r] > 0 && s->alpha[r] < 255)
      return true;
  return false;
}

/* reads back the picture under `at` (this frame's), then blends the log's text into it at each line's
 * opacity, into dst; false if it couldn't */
static bool fade_compose(nvn_queue_t *queue, int slot, nvn_texture_t *tex, nvn_region_t at, const gfx_panel_t *s,
                         uint8_t *dst, int sc)
{
  if ((size_t)at.w * at.h * 4 > fade_size)
    return false;
  fn.cmd_add_command(&cmd, &cmd_pool, slot * CMD_SIZE + CMD_SIZE - FADE_CMD, FADE_CMD);
  fn.cmd_add_control(&cmd, ctrl_mem + slot * CTRL_SIZE + CTRL_SIZE - FADE_CTRL, FADE_CTRL);
  fn.cmd_begin(&cmd);
  for (int y = 0; y < at.h; y++) {
    nvn_region_t r = { at.x, at.y + y, 0, at.w, 1, 1 };
    fn.cmd_copy_to_buffer(&cmd, tex, 0, &r, fn.pool_address(&fade_pool) + (nvn_addr_t)y * at.w * 4, 0);
  }
  nvn_handle_t h = fn.cmd_end(&cmd);
  fn.queue_submit(queue, 1, &h);
  fn.queue_fence(queue, &fade_sync, SYNC_ALL_GPU_COMMANDS_COMPLETE, 0);
  if (fn.sync_wait(&fade_sync, 50 * 1000 * 1000ull) > 1)
    return false;
  fn.pool_invalidate(&fade_pool, 0, (size_t)at.w * at.h * 4);
  uint8_t pal[PAL_COUNT][3];
  palette(pal);
  int mw = MASK_W(crect_cols[0]);
  const uint8_t *m = clear_mask[0];
  const uint32_t *under = (const uint32_t *)fade_mem;
  uint32_t *out = (uint32_t *)line;
  for (int x = 0; x < at.w; x++)
    font_x[x] = x * SC_ONE / sc;
  for (int y = 0; y < at.h; y++) {
    int fy = y * SC_ONE / sc, py = fy - TEXT_TOP - s->yoff, row = py < 0 ? 0 : py / CELL_H;
    unsigned a = s->alpha[row < GFX_ROWS ? row : GFX_ROWS - 1];
    const uint8_t *mr = m + (size_t)fy * mw;
    const uint32_t *u = under + (size_t)y * at.w;
    for (int x = 0; x < at.w; x++) {
      uint8_t k = mr[font_x[x]];
      if (!k || !a) {
        out[x] = u[x];
        continue;
      }
      uint32_t r = 0, uu = u[x];
      for (int c = 0; c < 3; c++)
        r |= ((pal[k][c] * a + (uu >> (8 * c) & 0xFF) * (255 - a) + 127) / 255) << (8 * c);
      out[x] = r | 0xFFu << 24;
    }
    memcpy(dst + (size_t)y * at.w * 4, line, (size_t)at.w * 4);
  }
  return true;
}

/* ... then each frame: text pixels from the mask, background blended with what was under it.
 * `under` holds one row of the region per font-pixel row (its first screen row), w pixels each. */
static void compose(int p, uint8_t *dst, const uint8_t *under, int w, int h, int sc, int opacity,
                    const uint8_t *row_alpha, int yoff)
{
  uint8_t pal[PAL_COUNT][3];
  uint32_t fg[PAL_COUNT], bg_solid = (uint32_t)pixel(bg_rgb);
  palette(pal);
  for (int i = 1; i < PAL_COUNT; i++)
    fg[i] = (uint32_t)pixel(pal[i]);
  uint8_t lut[3][256]; /* channel -> bg * a + under * (1 - a) */
  int a = opacity * 255 / 100;
  for (int c = 0; c < 3; c++)
    for (int u = 0; u < 256; u++)
      lut[c][u] = (uint8_t)((bg_rgb[c] * a + u * (255 - a) + 127) / 255);
  int mw = MASK_W(mask_cols[p]);
  uint32_t *out = (uint32_t *)line;
  for (int x = 0; x < w; x++)
    font_x[x] = x * SC_ONE / sc;
  for (int y = 0; y < h; y++) {
    int fy = y * SC_ONE / sc;
    const uint8_t *m = tl_mask[p] + (size_t)fy * mw;
    const uint32_t *in = under ? (const uint32_t *)(under + (size_t)fy * w * 4) : 0;
    unsigned ta = 255; /* the text's opacity on this row (the log fading) */
    if (row_alpha) {
      int py = fy - TEXT_TOP - yoff, row = py < 0 ? 0 : py / CELL_H;
      ta = row_alpha[row < GFX_ROWS ? row : GFX_ROWS - 1];
    }
    for (int x = 0; x < w; x++) {
      uint8_t k = m[font_x[x]];
      if (k && (ta >= 255 || !in))
        out[x] = fg[k];
      else if (k) {
        uint32_t f = fg[k], u = in[x], r = 0;
        for (int sh = 0; sh < 24; sh += 8)
          r |= (((f >> sh & 0xFF) * ta + (u >> sh & 0xFF) * (255 - ta) + 127) / 255) << sh;
        out[x] = r | 0xFFu << 24;
      } else if (in) {
        uint32_t u = in[x];
        out[x] = lut[0][u & 0xFF] | lut[1][u >> 8 & 0xFF] << 8 | lut[2][u >> 16 & 0xFF] << 16 | 0xFFu << 24;
      } else
        out[x] = bg_solid;
    }
    memcpy(dst + (size_t)y * w * 4, line, (size_t)w * 4);
  }
}

/* Air wall outlines (airwall.c): each dot is a small square cleared into the picture with a scissor
 * rectangle, which needs no shader. Before the panels, so the menu stays on top. Stops well before
 * the command memory runs out. */
static void draw_dots(const aw_dot_t *dots, int n, nvn_texture_t *tex, nvn_rect_t area, int th, int sc)
{
  static const float colors[AW_NCOLORS][4] = {
    [AW_BLOCKS] = { 0.23f, 0.84f, 1.0f, 1.0f }, /* cyan: stops Link */
    [AW_INSIDE] = { 1.0f, 0.28f, 0.28f, 1.0f }, /* red: Link is inside it */
    [AW_OTHER] = { 0.60f, 0.55f, 1.0f, 1.0f },  /* violet: other kinds */
  };
  int size = sc / SC_ONE < 2 ? 2 : sc / SC_ONE;
  nvn_texture_t *rt[1] = { tex };
  fn.set_render_targets(&cmd, 1, rt, 0, 0, 0);
  for (int i = 0; i < n; i++) {
    if ((i & 63) == 0 && fn.cmd_free(&cmd) < 16384)
      break;
    /* to texture pixels; a side thinner than the line width is widened to it, about its middle */
    int x = (int)((int64_t)dots[i].x0 * area.w / 65535), x1 = (int)((int64_t)dots[i].x1 * area.w / 65535);
    int y = (int)((int64_t)dots[i].y0 * area.h / 65535), y1 = (int)((int64_t)dots[i].y1 * area.h / 65535);
    int w = x1 - x, h = y1 - y;
    if (w < size)
      x = (x + x1 - size) / 2, w = size;
    if (h < size)
      y = (y + y1 - size) / 2, h = size;
    x = x < 0 ? 0 : x + w > area.w ? area.w - w : x;
    y = y < 0 ? 0 : y + h > area.h ? area.h - h : y;
    x += area.x, y += area.y;
    fn.set_scissor(&cmd, x, origin_lower_left ? th - y - h : y, w, h);
    fn.clear_color(&cmd, 0, colors[dots[i].color < AW_NCOLORS ? dots[i].color : 0], 0xF);
  }
  fn.set_scissor(&cmd, 0, 0, 65535, 65535);
}

/* gz's pause icons (pause_icons.png tiles 1 and 2), 16 font pixels tall, at the top left: grey edges
 * (gz's 0xBFBFBF) round white (gz's colour for the icon), each run of a row one cleared rectangle. */
static void pause_rect(nvn_rect_t area, int th, int sc, int ox, int oy, int x, int y, int w, int h, const float *c)
{
  int px = area.x + ox + x * sc / SC_ONE, py = area.y + oy + y * sc / SC_ONE;
  int pw = (x + w) * sc / SC_ONE - x * sc / SC_ONE, ph = (y + h) * sc / SC_ONE - y * sc / SC_ONE;
  if (pw <= 0 || ph <= 0)
    return;
  fn.set_scissor(&cmd, px, origin_lower_left ? th - py - ph : py, pw, ph);
  fn.clear_color(&cmd, 0, c, 0xF);
}

static void draw_pause(int icon, nvn_texture_t *tex, nvn_rect_t area, int th, int sc)
{
  static const float edge[4] = { 0.75f, 0.75f, 0.75f, 1 }, fill[4] = { 1, 1, 1, 1 };
  int ox = area.w * 35 / 1000, oy = area.h * 45 / 1000;
  nvn_texture_t *rt[1] = { tex };
  fn.set_render_targets(&cmd, 1, rt, 0, 0, 0);
  /* a bar (both icons), and a second bar (paused) or a play triangle (advancing) */
  pause_rect(area, th, sc, ox, oy, 0, 0, 6, 16, edge);
  pause_rect(area, th, sc, ox, oy, 1, 1, 4, 14, fill);
  if (icon == 1) {
    pause_rect(area, th, sc, ox, oy, 7, 0, 6, 16, edge);
    pause_rect(area, th, sc, ox, oy, 8, 1, 4, 14, fill);
  } else {
    for (int r = 0; r < 16; r++) { /* rows 2, 3, ... 9 wide, then back down */
      int w = r < 8 ? r + 2 : 17 - r;
      pause_rect(area, th, sc, ox, oy, 7, r, w, 1, edge);
      if (w > 2 && r > 0 && r < 15)
        pause_rect(area, th, sc, ox, oy, 8, r, w - 2, 1, fill);
    }
  }
  fn.set_scissor(&cmd, 0, 0, 65535, 65535);
}

/* Wait until the GPU has finished every copy we submitted. NVN: 0 already signalled,
 * 1 satisfied, 2 timed out, 3 failed. */
static bool wait_idle(void)
{
  for (int s = 0; s < SLOTS; s++)
    if (slot_used[s] && fn.sync_wait(&sync[s], 1000000000ull) > 1) {
      fail("GPU didn't finish the previous copy");
      return false;
    }
  return true;
}

/* The part of the texture that's shown. The game can render into a larger texture than it
 * displays (Lichii: 2560x1440 textures, crop 1920x1080), so positions come from the crop. */
static nvn_rect_t visible_area(const nvn_window_t *window, int tw, int th)
{
  nvn_rect_t r = { 0, 0, 0, 0 };
  if (window)
    fn.window_crop(window, &r);
  if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > tw || r.y + r.h > th)
    r = (nvn_rect_t){ 0, 0, tw, th };
  return r;
}

static bool same_region(const nvn_region_t *a, const nvn_region_t *b)
{
  return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static void draw(nvn_queue_t *queue, nvn_window_t *window, int index)
{
  const gfx_screen_t *scr = gfx_published();
  int n_dots;
  const aw_dot_t *dots = airwall_published(&n_dots);
  int pause_icon = gfx_pause_icon;
  if ((!scr && !n_dots && !pause_icon) || state == OV_FAILED)
    return;
  if (index < 0 || index >= n_textures) {
    static bool warned;
    if (!warned)
      log_printf("present of texture %d, but %d swapchain textures seen", index, n_textures);
    warned = true;
    return;
  }
  if (state == OV_NONE && !init())
    return;
  nvn_texture_t *tex = textures[index];
  int tw = fn.tex_width(tex), th = fn.tex_height(tex);
  if (fn.tex_format(tex) != fmt) {
    if (!warned_format)
      log_printf("swapchain format changed to %d; not drawing", fn.tex_format(tex));
    warned_format = true;
    return;
  }
  nvn_rect_t area = visible_area(window, tw, th);
  static nvn_rect_t logged_area;
  if (area.x != logged_area.x || area.y != logged_area.y || area.w != logged_area.w || area.h != logged_area.h) {
    logged_area = area;
    log_printf("visible area %dx%d at %d,%d of %dx%d", area.w, area.h, area.x, area.y, tw, th);
  }
  /* settings > menu > size: auto, 2x, 2.5x, 3x */
  static const int sizes[] = { 0, 32, 40, 48 };
  int sc = gfx_menu_scale > 0 && gfx_menu_scale < 4 ? sizes[gfx_menu_scale] : scale_for(area.h) * SC_ONE;
  if (sc > init_scale * SC_ONE)
    sc = init_scale * SC_ONE;
  int margin = 8 * sc / SC_ONE;
  int menu_y = area.h * MENU_Y_PERMILLE / 1000;
  /* tell the menu how many rows fit under the D-pad */
  int fit = ((area.h - menu_y - margin) * SC_ONE / sc - FONT_H) / CELL_H;
  gfx_menu_rows = fit < 6 ? 6 : fit > GFX_ROWS ? GFX_ROWS : fit;

  int opacity = gfx_opacity;
  bool see_through = opacity < 100 && is_rgba8(fmt) && tl_state != TL_FAILED;
  if (see_through && tl_state == TL_NONE && !tl_init(area.h - menu_y - margin))
    see_through = false;
  /* panels with no background are drawn as rectangles (draw_clear): no read-back for them */
  int hud_bottom_right = -1; /* the watches' top edge when they're at the bottom right: the log goes above */
  int hud_bottom_left = -1;  /* ... at the bottom left: the input display goes above */

  int slot = frame++ % SLOTS;
  if (slot_used[slot] && fn.sync_wait(&sync[slot], 1000000000ull) > 1) { /* see wait_idle */
    fail("GPU didn't finish the previous copy");
    return;
  }

  nvn_region_t regions[GFX_PANELS];
  nvn_addr_t srcs[GFX_PANELS], readback[GFX_PANELS];
  int readback_rows[GFX_PANELS];
  bool copy[GFX_PANELS];
  int scales[GFX_PANELS], clear_of[GFX_PANELS], panel_of[GFX_PANELS];
  int n = 0;
  for (int pi = 0; pi < GFX_PANELS && scr; pi++) {
    int p = (pi + 1) % GFX_PANELS; /* the menu (0) last: it's drawn over everything else */
    const gfx_panel_t *pn = &scr->panel[p];
    if (!pn->rows)
      continue;
    int cols = pn->cols > 16 ? pn->cols : 16, rows = pn->rows;
    if (cols > max_cols[p])
      cols = max_cols[p];
    if (rows > max_rows[p])
      rows = max_rows[p];
    /* a panel can be drawn smaller than the menu (the input display's size), not below 1x */
    int psc = pn->scale_pct ? sc * pn->scale_pct / 100 : sc;
    if (psc < SC_ONE)
      psc = SC_ONE;
    int w = PANEL_W(cols, psc), h = PANEL_H(rows, psc);
    int x = margin, y = margin; /* top-left */
    switch (p == 0 ? -1 : pn->corner) {
      case -1: x = area.w * MENU_X_PERMILLE / 1000, y = menu_y; break; /* menu: under the D-pad */
      case GFX_TOP_RIGHT: x = area.w - w - margin; break;
      case GFX_BOTTOM_LEFT: y = area.h - h - margin; break;
      case GFX_BOTTOM_RIGHT: x = area.w - w - margin, y = area.h - h - margin; break;
      case GFX_TOP_CENTRE: x = (area.w - w) / 2; break;
      case GFX_BOTTOM_CENTRE: x = (area.w - w) / 2, y = area.h - h - margin; break;
      case GFX_CUSTOM: /* kept whole on screen */
        x = area.w * pn->x / 1000, y = area.h * pn->y / 1000;
        if (x > area.w - w - margin)
          x = area.w - w - margin;
        if (y > area.h - h - margin)
          y = area.h - h - margin;
        if (x < margin)
          x = margin;
        if (y < margin)
          y = margin;
        gfx_custom_x = x * 1000 / area.w, gfx_custom_y = y * 1000 / area.h;
        break;
      default: break;
    }
    if (p == 1 && pn->corner == GFX_BOTTOM_RIGHT)
      hud_bottom_right = y;
    if (p == 1 && pn->corner == GFX_BOTTOM_LEFT)
      hud_bottom_left = y;
    if (p == GFX_PANEL_LOG && hud_bottom_right >= 0 && y + h > hud_bottom_right)
      y = hud_bottom_right - h;
    if (p == GFX_PANEL_INPUT && hud_bottom_left >= 0 && y + h > hud_bottom_left)
      y = hud_bottom_left - h;
    if (y < margin)
      y = margin;
    if (w > area.w - x - margin)
      w = area.w - x - margin;
    if (h > area.h - y - margin)
      h = area.h - y - margin;
    if (w <= 0 || h <= 0)
      continue;
    if (p != 0 && (size_t)w * h * bpp > pix_size[p]) /* never: the buffers are sized for the most */
      continue;
    nvn_region_t region = { area.x + x, area.y + y, 0, w, h, 1 };
    readback[n] = 0;
    copy[n] = true;
    clear_of[n] = -1;
    if (pn->clear && p >= GFX_PANEL_LOG) { /* drawn as rectangles in the command buffer below */
      int ci = p - GFX_PANEL_LOG;
      if (crect_hash[ci] != pn->hash || crect_cols[ci] != cols || crect_rows[ci] != rows)
        build_clear(ci, pn, cols, rows);
      clear_of[n] = ci, panel_of[n] = p, copy[n] = false, scales[n] = psc;
      /* the log fading: blended into this frame's picture instead (fade_compose) */
      if (p == GFX_PANEL_LOG && fading(pn) && is_rgba8(fmt) && fade_state != FADE_FAILED &&
          (fade_state == FADE_READY || fade_init()) && (size_t)w * h * 4 <= pix_size[p] &&
          fade_compose(queue, slot, tex, region, pn, out_ptr(p, slot), psc)) {
        clear_of[n] = -1, copy[n] = true, srcs[n] = out_addr(p, slot);
        drawn_hash[p][slot] = 0;
      }
      regions[n++] = region;
      continue;
    }
    bool blend = see_through;

    if (blend) {
      int b = slot;
      int rb_rows = (h * SC_ONE + psc - 1) / psc; /* font-pixel rows */
      if (p == 0 && (size_t)w * h * 4 > tl_out_size) /* taller than the menu can be on screen */
        h = (int)(tl_out_size / ((size_t)w * 4)), region.h = h, rb_rows = (h * SC_ONE + psc - 1) / psc;
      if ((size_t)w * rb_rows * 4 > tl_rb_size[p])
        continue;
      if (mask_hash[p] != pn->hash || mask_cols[p] != cols || mask_rows[p] != rows)
        build_mask(p, pn, cols, rows);
      /* this slot's readback is from two frames ago (its fence has passed) */
      const uint8_t *under = 0;
      if (slot_used[slot] && same_region(&tl_rb_region[p][slot], &region)) {
        fn.pool_invalidate(&tl_rb_pool[p][slot], 0, (size_t)w * rb_rows * 4);
        under = tl_rb_mem[p][slot];
      }
      bool log = pn->clear; /* no background */
      compose(p, out_ptr(p, b), under, w, h, psc, log ? 0 : opacity, log ? pn->alpha : 0, pn->yoff);
      if (log && !under) /* nothing to blend with yet (2 frames): read what's there, show it later */
        copy[n] = false;
      drawn_hash[p][b] = 0; /* the opaque path redraws when it's next used */
      tl_rb_region[p][slot] = region;
      readback[n] = fn.pool_address(&tl_rb_pool[p][slot]);
      readback_rows[n] = rb_rows;
      srcs[n] = out_addr(p, b);
    } else {
      int b = n_bufs[p] > 1 ? slot : 0;
      if (drawn_hash[p][b] != pn->hash || drawn_w[p][b] != w || drawn_h[p][b] != h) {
        if (n_bufs[p] == 1 && !wait_idle()) /* the GPU may still be copying the old one */
          return;
        rasterise(pn, out_ptr(p, b), psc, w, h);
        drawn_hash[p][b] = pn->hash, drawn_w[p][b] = w, drawn_h[p][b] = h;
      }
      srcs[n] = out_addr(p, b);
    }
    scales[n] = psc;
    regions[n++] = region;
  }
  if (!n && !n_dots && !pause_icon)
    return;
  __asm__ volatile("dsb sy" ::: "memory"); /* pixels reach memory before the GPU copies them */

  fn.cmd_add_command(&cmd, &cmd_pool, slot * CMD_SIZE, CMD_SIZE - FADE_CMD); /* the end: fade_compose's */
  fn.cmd_add_control(&cmd, ctrl_mem + slot * CTRL_SIZE, CTRL_SIZE - FADE_CTRL);
  fn.cmd_begin(&cmd);
  if (n_dots)
    draw_dots(dots, n_dots, tex, area, th, sc);
  if (pause_icon)
    draw_pause(pause_icon, tex, area, th, sc);
  for (int i = 0; i < n; i++) {
    /* what the game drew there, before the panel covers it: one row per font-pixel row */
    for (int k = 0; readback[i] && k < readback_rows[i]; k++) {
      /* font row k starts at screen row ceil(k * scale / 16) */
      nvn_region_t r = { regions[i].x, regions[i].y + (k * scales[i] + SC_ONE - 1) / SC_ONE, 0, regions[i].w, 1, 1 };
      if (r.y >= regions[i].y + regions[i].h)
        break;
      fn.cmd_copy_to_buffer(&cmd, tex, 0, &r, readback[i] + (nvn_addr_t)k * regions[i].w * 4, 0);
    }
    if (clear_of[i] >= 0) {
      nvn_rect_t at = { regions[i].x, regions[i].y, regions[i].w, regions[i].h };
      draw_clear(clear_of[i], &scr->panel[panel_of[i]], tex, at, th, scales[i]);
    } else if (copy[i])
      fn.cmd_copy_to_texture(&cmd, srcs[i], tex, 0, &regions[i], 0);
  }
  nvn_handle_t handle = fn.cmd_end(&cmd);
  fn.queue_submit(queue, 1, &handle);
  fn.queue_fence(queue, &sync[slot], SYNC_ALL_GPU_COMMANDS_COMPLETE, 0);
  slot_used[slot] = true;

  if (!gfx_on_screen) {
    gfx_on_screen = true;
    log_printf("drawing on screen: scale %d/16 on %dx%d", sc, tw, th);
  }
}
