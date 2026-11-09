/* debug > memory (gz's mem.c, for the Switch: 64-bit addresses, little-endian values).
 *
 *   address   L: type one in (d-left/right a digit, d-up/down change it, L go)   d-left/right: a page
 *   view      byte / halfword / word / float / pointer
 *   go to     gz's domains, here the places worth looking at: main, ootgz, Link, the camera...
 *   arrows    gz's scroll buttons, over the rows: L scrolls a row up or down (held: keeps going)
 *   rows      8 bytes each. L: edit the row: d-left/right moves along it, d-up/down changes the digit
 *             under the cursor (written at once, if the memory is writable). On the row's address,
 *             L adds a watch there, as gz's row button does; in the pointer view, L on a value goes
 *             where it points. R + d-left stops editing.
 *
 * Nothing is read that isn't mapped (svcQueryMemory, sys_mapped); unreadable bytes show as ??. */
#include "memview.h"

#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "link.h"
#include "lang.h"
#include "sys.h"
#include "watches.h"
#include "z64hd.h"

#define ROWS 12
#define ROW_BYTES 8
#define ADDR_DIGITS 10 /* the Switch's 39-bit addresses */
#define CELLS_COL (ADDR_DIGITS + 2)

menu_t memview_menu;
static menu_t *watches;

enum { V_BYTE, V_HALF, V_WORD, V_FLOAT, V_PTR, V_N };
static const char *const view_names[V_N] = { "byte", "halfword", "word", "float", "pointer" };
static const int view_size[V_N] = { 1, 2, 4, 4, 8 };
static const uint8_t view_watch_type[V_N] = { MW_X8, MW_X16, MW_X32, MW_F32, MW_X64 };

static uintptr_t view_addr; /* the first row's */
static int view;

/* --- where to go (gz's domains) ----------------------------------------------------------------- */

extern char __module_start[];
static uintptr_t at_main(void) { return z64hd_main; }
static uintptr_t at_ootgz(void) { return (uintptr_t)__module_start; }
static uintptr_t at_link(void) { return link_actor(); }
static uintptr_t at_status(void)
{
  uintptr_t o = link_actor(), s = o ? sys_ptr(o + 8) : 0;
  return s ? sys_ptr(s + 0x6A0) : 0;
}
static uintptr_t at_camera(void) { return camera(); }
static uintptr_t at_gamedata(void) { return (uintptr_t)gd_manager(); }
static uintptr_t at_strings(void) { return (uintptr_t)gd_get_str(0); }

static const struct { const char *name; uintptr_t (*addr)(void); } places[] = {
  { "main", at_main }, { "sloperinaGZ", at_ootgz }, { "link actor", at_link }, { "link status", at_status },
  { "camera", at_camera }, { "game data", at_gamedata }, { "save strings", at_strings },
};
#define N_PLACES ((int)(sizeof(places) / sizeof(*places)))
static const char *place_names[N_PLACES];
static int place = 0;

static void show(uintptr_t a)
{
  view_addr = a & ~(uintptr_t)(ROW_BYTES - 1);
}

/* --- values --------------------------------------------------------------------------------------- */

static bool read_cell(uintptr_t a, int size, uint64_t *v)
{
  *v = 0;
  if (!sys_mapped(a, (size_t)size, SYS_PERM_R))
    return false;
  memcpy(v, (const void *)a, (size_t)size);
  return true;
}

static int cell_width(void) { return view == V_FLOAT ? 13 : 2 * view_size[view]; }
static int cell_col(int c) { return CELLS_COL + c * (cell_width() + 1); }

static void row_text(menu_item_t *it, char *buf, int len)
{
  uintptr_t a = view_addr + (uintptr_t)it->data * ROW_BYTES;
  int n = snprintf(buf, len, "%0*lx  ", ADDR_DIGITS, (unsigned long)a);
  int size = view_size[view], w = cell_width();
  for (int c = 0; c < ROW_BYTES / size && n < len; c++) {
    uint64_t v;
    bool ok = read_cell(a + (uintptr_t)(c * size), size, &v);
    if (!ok)
      n += snprintf(buf + n, len - n, "%.*s", w, "?????????????????");
    else if (view == V_FLOAT) {
      float f;
      uint32_t b = (uint32_t)v;
      memcpy(&f, &b, 4);
      n += snprintf(buf + n, len - n, "%13.6g", (double)f);
    } else
      n += snprintf(buf + n, len - n, "%0*llx", w, (unsigned long long)v);
    if (n < len - 1)
      buf[n++] = ' ', buf[n] = 0;
  }
}

/* gz's scroll arrows, then its cell header: the low digit of each cell's address */
static void header_text(menu_item_t *it, char *buf, int len)
{
  int n = snprintf(buf, len, "%c %c%*s", GFX_ICON_UP, GFX_ICON_DOWN, CELLS_COL - 3, "");
  int size = view_size[view];
  for (int c = 0; c < ROW_BYTES / size && n < len; c++)
    n += snprintf(buf + n, len - n, "%-*x", cell_width() + 1, (unsigned)((view_addr + (uintptr_t)(c * size)) & 0xF));
}

/* --- the address line ----------------------------------------------------------------------------- */

static uintptr_t edit_addr;
static int edit_pos;

static void address_text(menu_item_t *it, char *buf, int len)
{
  uintptr_t a = menu_editing == it ? edit_addr : view_addr;
  snprintf(buf, len, TF("address  %0*lx"), ADDR_DIGITS, (unsigned long)a);
}

static bool address_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  if (menu_editing != it)
    return false;
  *start = 9 + edit_pos, *len = 1;
  return true;
}

static bool address_edit(menu_item_t *it)
{
  if (input_repeat(BTN_DLEFT))
    edit_pos = edit_pos > 0 ? edit_pos - 1 : 0;
  else if (input_repeat(BTN_DRIGHT))
    edit_pos = edit_pos < ADDR_DIGITS - 1 ? edit_pos + 1 : ADDR_DIGITS - 1;
  else if (input_repeat(BTN_DUP) || input_repeat(BTN_DDOWN)) {
    int shift = (ADDR_DIGITS - 1 - edit_pos) * 4, dir = input_repeat(BTN_DUP) ? 1 : -1;
    uintptr_t nib = (edit_addr >> shift & 0xF) + (uintptr_t)(dir + 16);
    edit_addr = (edit_addr & ~((uintptr_t)0xF << shift)) | (nib & 0xF) << shift;
  } else if (input_pressed() & BTN_L) {
    show(edit_addr);
    gfx_logf("memory %0*lx", ADDR_DIGITS, (unsigned long)view_addr);
    return false;
  }
  return true;
}

static void act_address(menu_item_t *it)
{
  edit_addr = view_addr;
  edit_pos = ADDR_DIGITS - 1;
  menu_edit(it);
}

static void page(menu_item_t *it, int dir)
{
  uintptr_t step = ROWS * ROW_BYTES;
  if (dir < 0 && view_addr < step)
    view_addr = 0;
  else
    view_addr += dir < 0 ? (uintptr_t)0 - step : step;
}

/* --- view and places --------------------------------------------------------------------------------- */

static int get_view(menu_item_t *it) { return view; }
static void set_view(menu_item_t *it, int v) { view = v; }
static int get_place(menu_item_t *it) { return place; }
static void set_place(menu_item_t *it, int p)
{
  place = p;
  uintptr_t a = places[p].addr();
  if (a)
    show(a);
  else
    gfx_logf("%s: not found right now", places[p].name);
}

/* --- rows ----------------------------------------------------------------------------------------- */

static int row_positions(void) { return 1 + (view == V_FLOAT ? 0 : 2 * ROW_BYTES); }

static bool row_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  if (menu_editing != it)
    return false;
  if (edit_pos == 0) {
    *start = 0, *len = ADDR_DIGITS;
  } else {
    int k = edit_pos - 1, digits = 2 * view_size[view];
    *start = cell_col(k / digits) + k % digits, *len = 1;
  }
  return true;
}

static void change_digit(uintptr_t row, int dir)
{
  int k = edit_pos - 1, size = view_size[view], digits = 2 * size;
  uintptr_t a = row + (uintptr_t)(k / digits * size);
  int shift = (digits - 1 - k % digits) * 4;
  uint64_t v;
  if (!read_cell(a, size, &v))
    return;
  uint64_t nib = ((v >> shift & 0xF) + (uint64_t)(dir + 16)) & 0xF;
  v = (v & ~((uint64_t)0xF << shift)) | nib << shift;
  if (!sys_mapped(a, (size_t)size, SYS_PERM_R | SYS_PERM_W)) {
    gfx_notice("that memory is read-only");
    return;
  }
  memcpy((void *)a, &v, (size_t)size);
}

static bool row_edit(menu_item_t *it)
{
  uintptr_t row = view_addr + (uintptr_t)it->data * ROW_BYTES;
  int n = row_positions();
  if (edit_pos >= n)
    edit_pos = n - 1;
  if (input_repeat(BTN_DLEFT))
    edit_pos = edit_pos > 0 ? edit_pos - 1 : 0;
  else if (input_repeat(BTN_DRIGHT))
    edit_pos = edit_pos < n - 1 ? edit_pos + 1 : n - 1;
  else if (edit_pos > 0 && (input_repeat(BTN_DUP) || input_repeat(BTN_DDOWN)))
    change_digit(row, input_repeat(BTN_DUP) ? 1 : -1);
  else if (input_pressed() & BTN_L) {
    if (edit_pos == 0) { /* gz's row button: a watch here */
      if (watch_add_memory(row, view_watch_type[view])) {
        gfx_logf("added watch %0*lx", ADDR_DIGITS, (unsigned long)row);
        if (watches)
          menu_goto(watches);
      } else {
        gfx_notice("that address is already watched");
      }
      return false;
    }
    if (view == V_PTR) { /* follow the pointer */
      uint64_t v;
      if (read_cell(row + (uintptr_t)((edit_pos - 1) / 16 * 8), 8, &v) && sys_mapped((uintptr_t)v, 1, SYS_PERM_R)) {
        show((uintptr_t)v);
        gfx_logf("memory %0*lx", ADDR_DIGITS, (unsigned long)view_addr);
      } else {
        gfx_notice("that doesn't point anywhere readable");
      }
    }
    return false;
  }
  return true;
}

static const char *const arrow_hints[] = { "L: scroll up a row (held: keeps going)",
                                           "L: scroll down a row (held: keeps going)" };

static bool arrows_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = it->part ? 2 : 0, *len = 1;
  return true;
}

static void arrows_press(menu_item_t *it, int part)
{
  if (part == 0)
    view_addr = view_addr >= ROW_BYTES ? view_addr - ROW_BYTES : 0;
  else
    view_addr += ROW_BYTES;
}

static void act_row(menu_item_t *it)
{
  edit_pos = 0;
  menu_edit(it);
}

/* --- menu ----------------------------------------------------------------------------------------- */

static int first_row;

void memview_build(menu_t *watches_menu)
{
  watches = watches_menu;
  for (int i = 0; i < N_PLACES; i++)
    place_names[i] = places[i].name;
  show(z64hd_main);
  menu_init(&memview_menu, "memory", ROWS + 6);
  menu_item_t *ad = menu_add_action(&memview_menu, "", act_address, 0);
  if (ad)
    ad->text = address_text, ad->on_change = page, ad->cursor = address_cursor, ad->edit = address_edit,
    ad->hint = "L: type an address   d-left/right: a page back/on",
    ad->edit_hint = "d-left/right: digit   d-up/down: change   L: go";
  menu_add_option(&memview_menu, "view", V_N, view_names, get_view, set_view, 0);
  menu_item_t *gt = menu_add_option(&memview_menu, "go to", N_PLACES, place_names, get_place, set_place, 0);
  if (gt)
    gt->hint = "d-left/right: jump there";
  menu_item_t *ar = menu_add_action(&memview_menu, "", 0, 0); /* gz's arrows, on the header line */
  if (ar)
    ar->text = header_text, ar->cursor = arrows_cursor, ar->press = arrows_press, ar->press_repeat = true,
    ar->n_parts = 2, ar->part_hints = arrow_hints;
  first_row = memview_menu.n_items;
  for (int r = 0; r < ROWS; r++) {
    menu_item_t *it = menu_add_action(&memview_menu, "", act_row, r);
    if (it)
      it->text = row_text, it->on_change = page, it->cursor = row_cursor, it->edit = row_edit,
      it->hint = "L: edit this row   d-left/right: a page back/on",
      it->edit_hint = "d-l/r: move  d-u/d: change  L on address: watch it";
  }
  menu_add_return(&memview_menu);
}

void memview_open(uintptr_t addr, int mw_type)
{
  int size = mw_type >= 0 && mw_type < MW_NTYPES ? memory_type_size[mw_type] : 1;
  view = size == 1 ? V_BYTE : size == 2 ? V_HALF : size == 8 ? V_PTR : mw_type == MW_F32 ? V_FLOAT : V_WORD;
  show(addr);
  menu_goto(&memview_menu);
  memview_menu.selected = first_row;
}
