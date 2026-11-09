/* Settings on the SD card, through the game's own nn::fs (exported by sdk). The file is plain
 * text, one "key=value" per line, so it can be edited by hand:
 *
 *   language=<code> (lang/<code>.json; en: English)   watch_label_<watch_t>=<name>   (a renamed watch)
 *   cheats=<bit per cheat_t>   watches_shown=0|1   watch_list=<watch_t>:<display type>:<hidden>,...   watch_position=<gfx_corner_t>
 *   opacity=0..100             arrival_cutscenes=0|1      levitate=0|1      levitate_speed=<units/s>
 *
 * The SD card is mounted with nn::fs::MountSdCard (MountSdCardForDebug if that's refused). On a
 * console this needs SD card access in the game's permissions (main.npdm); Lichii allows it. If the
 * mount fails the menu works as before, without saving. */
#include "settings.h"

#include "cheats.h"
#include "gfx.h"
#include "sys.h"
#include "warp.h"
#include "watches.h"
#include "levitate.h"
#include "airwall.h"

#include "sdfs.h"
#include "lang.h"
#include "menus.h"


#define MOUNT SD_MOUNT
#define DIR SD_DIR
#define SETTINGS_FILE DIR "/settings.ini"
#define LOG_FILE DIR "/log.txt" /* everything the module logs, from this start of the game */
#define LOG_FILE_GAP 30         /* frames between writes (about a second) */
#define OLD_SETTINGS_FILE SD_OLD_DIR "/settings.ini"
#define SAVE_GAP_FRAMES 60 /* at most one write every ~2 s */

typedef struct {
  uint32_t cheats;
  int32_t n_watches;
  uint8_t watches[W_COUNT], watch_types[W_COUNT], watch_hidden[W_COUNT];
  char watch_labels[W_COUNT][WATCH_LABEL_MAX + 1]; /* by watch_t; "" = its own name */
  char lang[8];
  int32_t frame_advance, pause_display, frame_advance_button;
  int32_t menu_scale, input_display, input_display_bg, input_parts, input_style, input_size, watches_shown, watch_position, watch_x, watch_y, opacity, arrival_cutscenes, levitate, levitate_speed, levitate_move,
    levitate_walls, airwalls_shown, airwalls_all;
} snapshot_t;

static bool mounted;
static snapshot_t saved;
static int since_save;

static bool same_text(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static snapshot_t take(void)
{
  snapshot_t s;
  memset(&s, 0, sizeof(s)); /* padding too: snapshots are compared byte by byte */
  for (int c = 0; c < CHEAT_COUNT; c++)
    s.cheats |= (uint32_t)cheats[c] << c;
  for (int i = 0; i < watch_count && s.n_watches < W_COUNT; i++) /* memory watches aren't kept */
    if (watch_list[i].id < W_COUNT) {
      s.watches[s.n_watches++] = (uint8_t)watch_list[i].id;
      memcpy(s.watch_labels[watch_list[i].id], watch_list[i].label, sizeof(s.watch_labels[0]));
      s.watch_hidden[watch_list[i].id] = watch_list[i].hidden;
    }
  memcpy(s.watch_types, watch_types, sizeof(s.watch_types));
  s.watches_shown = watches_shown;
  s.watch_position = watches_corner;
  s.watch_x = watches_x, s.watch_y = watches_y;
  s.opacity = gfx_opacity;
  s.menu_scale = gfx_menu_scale;
  s.frame_advance = frame_advance, s.pause_display = pause_display, s.frame_advance_button = frame_advance_button;
  snprintf(s.lang, sizeof(s.lang), "%s", lang_code());
  s.input_display = input_display;
  s.input_display_bg = input_display_bg;
  s.input_style = input_style;
  s.input_size = input_size;
  s.input_parts = input_show_left | input_show_right << 1 | input_show_buttons << 2;
  s.arrival_cutscenes = warp_arrival_cutscenes;
  s.levitate = levitate_enabled;
  s.levitate_speed = levitate_speed;
  s.levitate_move = levitate_move_speed;
  s.levitate_walls = levitate_through_walls;
  s.airwalls_shown = airwalls_shown;
  s.airwalls_all = airwalls_all;
  return s;
}

static void apply(const snapshot_t *s)
{
  for (int c = 0; c < CHEAT_COUNT; c++)
    cheats[c] = s->cheats >> c & 1;
  watch_t list[W_COUNT];
  int n = s->n_watches < 0 ? 0 : s->n_watches > W_COUNT ? W_COUNT : s->n_watches;
  for (int i = 0; i < n; i++)
    list[i] = (watch_t)s->watches[i];
  watch_set_list(list, n);
  for (int i = 0; i < watch_count; i++)
    if (watch_list[i].id < W_COUNT) {
      watch_set_label(i, s->watch_labels[watch_list[i].id]);
      watch_set_hidden(i, s->watch_hidden[watch_list[i].id] != 0);
    }
  for (int w = 0; w < W_COUNT; w++)
    watch_set_type((watch_t)w, s->watch_types[w]);
  watches_shown = s->watches_shown != 0;
  if (s->watch_position >= 0 && s->watch_position < GFX_NPLACES)
    watches_corner = s->watch_position;
  if (s->watch_x >= 0 && s->watch_x <= 1000 && s->watch_y >= 0 && s->watch_y <= 1000)
    watches_x = s->watch_x, watches_y = s->watch_y;
  if (!same_text(s->lang, lang_code()))
    lang_select_code(s->lang);
  frame_advance = s->frame_advance != 0, pause_display = s->pause_display != 0;
  frame_advance_button = s->frame_advance_button == 1;
  input_display = s->input_display != 0;
  input_display_bg = s->input_display_bg != 0;
  input_style = s->input_style == 1;
  if (s->input_size >= 0 && s->input_size < 3)
    input_size = s->input_size;
  input_show_left = s->input_parts & 1, input_show_right = s->input_parts >> 1 & 1;
  input_show_buttons = s->input_parts >> 2 & 1;
  if (s->menu_scale >= 0 && s->menu_scale <= 3)
    gfx_menu_scale = s->menu_scale;
  if (s->opacity >= 0 && s->opacity <= 100)
    gfx_opacity = s->opacity / 10 * 10;
  warp_arrival_cutscenes = s->arrival_cutscenes != 0;
  levitate_enabled = s->levitate != 0;
  if (s->levitate_speed > 0 && s->levitate_speed <= 50)
    levitate_speed = s->levitate_speed;
  if (s->levitate_move > 0 && s->levitate_move <= 50)
    levitate_move_speed = s->levitate_move;
  levitate_through_walls = s->levitate_walls != 0;
  airwalls_shown = s->airwalls_shown != 0;
  airwalls_all = s->airwalls_all != 0;
}

static int64_t parse_int(const char *p)
{
  int64_t v = 0;
  bool neg = *p == '-';
  if (neg)
    p++;
  while (*p >= '0' && *p <= '9')
    v = v * 10 + (*p++ - '0');
  return neg ? -v : v;
}

static bool starts_with(const char *line, const char *prefix)
{
  while (*prefix && *line == *prefix)
    line++, prefix++;
  return !*prefix;
}

static bool key_is(const char *line, const char *key, const char **value)
{
  while (*key && *line == *key)
    line++, key++;
  if (*key || *line != '=')
    return false;
  *value = line + 1;
  return true;
}

void settings_load(void)
{
  if (fs_mount_sd(MOUNT) != 0 && fs_mount_sd_debug(MOUNT) != 0) {
    log_printf("settings: no SD card access; settings won't be kept");
    gfx_notice("sloperinaGZ: no SD card access, settings won't be kept");
    return;
  }
  mounted = true;
  fs_create_dir(DIR);
  fs_delete_file(LOG_FILE); /* a fresh one each time the game starts */
  fs_create_file(LOG_FILE, 0);
  lang_init(); /* the language files on the SD card, for the language setting */
  snapshot_t s = take(); /* defaults for anything the file lacks */
  bool have_list = false;
  fs_file_t f;
  static char buf[2048];
  size_t got = 0;
  const char *from = 0;
  bool old = false; /* saved before the rename: saved again in the new folder */
  if (fs_open(&f, SETTINGS_FILE, FS_READ) == 0)
    from = SETTINGS_FILE;
  else if (fs_open(&f, OLD_SETTINGS_FILE, FS_READ) == 0)
    from = OLD_SETTINGS_FILE, old = true;
  if (from) {
    int64_t size = 0;
    fs_size(&size, f);
    if (size > (int64_t)sizeof(buf) - 1)
      size = sizeof(buf) - 1;
    fs_read(&got, f, 0, buf, (size_t)size);
    fs_close(f);
  }
  buf[got] = 0;
  for (char *line = buf; *line;) {
    char *end = line;
    while (*end && *end != '\n' && *end != '\r')
      end++;
    char keep = *end;
    *end = 0;
    const char *v;
    if (key_is(line, "cheats", &v)) s.cheats = (uint32_t)parse_int(v);
    else if (key_is(line, "watches", &v) && !have_list) { /* before watch_list: a bit per watch */
      uint32_t bits = (uint32_t)parse_int(v);
      s.n_watches = 0;
      for (int w = 0; w < W_COUNT; w++)
        if (bits >> w & 1)
          s.watches[s.n_watches++] = (uint8_t)w;
    } else if (key_is(line, "watch_list", &v)) {
      have_list = true;
      s.n_watches = 0;
      while (*v >= '0' && *v <= '9' && s.n_watches < W_COUNT) {
        int64_t w = parse_int(v);
        while (*v >= '0' && *v <= '9')
          v++;
        int64_t t = 0, h = 0;
        if (*v == ':') { /* its display type */
          t = parse_int(++v);
          while (*v >= '0' && *v <= '9')
            v++;
        }
        if (*v == ':') { /* 1: hidden */
          h = parse_int(++v);
          while (*v >= '0' && *v <= '9')
            v++;
        }
        if (w < W_COUNT) {
          s.watches[s.n_watches++] = (uint8_t)w;
          s.watch_types[w] = (uint8_t)(t < 0 || t > 15 ? 0 : t);
          s.watch_hidden[w] = h != 0;
        }
        if (*v == ',')
          v++;
      }
    }
    else if (key_is(line, "watches_shown", &v)) s.watches_shown = (int32_t)parse_int(v);
    else if (key_is(line, "watch_position", &v)) s.watch_position = (int32_t)parse_int(v);
    else if (key_is(line, "watch_x", &v)) s.watch_x = (int32_t)parse_int(v);
    else if (key_is(line, "watch_y", &v)) s.watch_y = (int32_t)parse_int(v);
    else if (key_is(line, "opacity", &v)) s.opacity = (int32_t)parse_int(v);
    else if (key_is(line, "menu_size", &v)) s.menu_scale = (int32_t)parse_int(v);
    else if (key_is(line, "frame_advance", &v)) s.frame_advance = (int32_t)parse_int(v);
    else if (key_is(line, "pause_display", &v)) s.pause_display = (int32_t)parse_int(v);
    else if (key_is(line, "frame_advance_button", &v)) s.frame_advance_button = (int32_t)parse_int(v);
    else if (key_is(line, "language", &v)) snprintf(s.lang, sizeof(s.lang), "%s", v);
    else if (key_is(line, "input_display", &v)) s.input_display = (int32_t)parse_int(v);
    else if (key_is(line, "input_display_background", &v)) s.input_display_bg = (int32_t)parse_int(v);
    else if (key_is(line, "input_display_style", &v)) s.input_style = (int32_t)parse_int(v);
    else if (key_is(line, "input_display_size", &v)) s.input_size = (int32_t)parse_int(v);
    else if (key_is(line, "input_display_parts", &v)) s.input_parts = (int32_t)parse_int(v); /* 1 left, 2 right, 4 buttons */
    else if (key_is(line, "arrival_cutscenes", &v)) s.arrival_cutscenes = (int32_t)parse_int(v);
    else if (key_is(line, "levitate", &v)) s.levitate = (int32_t)parse_int(v);
    else if (key_is(line, "levitate_speed", &v)) s.levitate_speed = (int32_t)parse_int(v);
    else if (key_is(line, "levitate_move_speed", &v)) s.levitate_move = (int32_t)parse_int(v);
    else if (key_is(line, "levitate_through_walls", &v)) s.levitate_walls = (int32_t)parse_int(v);
    else if (starts_with(line, "watch_label_")) { /* watch_label_<watch_t>=<name> */
      const char *p = line + 12;
      int64_t w = parse_int(p);
      while (*p >= '0' && *p <= '9')
        p++;
      if (*p == '=' && w >= 0 && w < W_COUNT)
        snprintf(s.watch_labels[w], sizeof(s.watch_labels[w]), "%s", p + 1);
    }
    else if (key_is(line, "airwalls_shown", &v)) s.airwalls_shown = (int32_t)parse_int(v);
    else if (key_is(line, "airwalls_all", &v)) s.airwalls_all = (int32_t)parse_int(v);
    line = keep ? end + 1 : end;
  }
  apply(&s);
  saved = take();
  if (got)
    log_printf("settings: loaded %s", from);
  else
    log_printf("settings: none yet (" SETTINGS_FILE ")");
  if (old)
    saved.cheats ^= 1; /* differs from now: written to the new folder at the next chance */
}

static void save(const snapshot_t *s)
{
  char list[W_COUNT * 9 + 1] = "";
  for (int i = 0, l = 0; i < s->n_watches && i < W_COUNT; i++)
    l += snprintf(list + l, sizeof(list) - l, "%s%d:%d:%d", i ? "," : "", s->watches[i],
                  s->watch_types[s->watches[i] % W_COUNT], s->watch_hidden[s->watches[i] % W_COUNT]);
  static char buf[2048];
  int n = snprintf(buf, sizeof(buf),
                   "cheats=%u\nwatch_list=%s\nwatches_shown=%d\nwatch_position=%d\nwatch_x=%d\nwatch_y=%d\nopacity=%d\n"
                   "arrival_cutscenes=%d\nlevitate=%d\nlevitate_speed=%d\nlevitate_move_speed=%d\n"
                   "levitate_through_walls=%d\nairwalls_shown=%d\nairwalls_all=%d\n",
                   (unsigned)s->cheats, list, (int)s->watches_shown, (int)s->watch_position,
                   (int)s->watch_x, (int)s->watch_y,
                   (int)s->opacity, (int)s->arrival_cutscenes, (int)s->levitate, (int)s->levitate_speed,
                   (int)s->levitate_move, (int)s->levitate_walls, (int)s->airwalls_shown, (int)s->airwalls_all);
  if (n < (int)sizeof(buf))
    n += snprintf(buf + n, sizeof(buf) - n, "language=%s\nframe_advance=%d\nframe_advance_button=%d\npause_display=%d\nmenu_size=%d\ninput_display=%d\ninput_display_background=%d\ninput_display_parts=%d\ninput_display_style=%d\ninput_display_size=%d\n",
                  s->lang, (int)s->frame_advance, (int)s->frame_advance_button, (int)s->pause_display, (int)s->menu_scale, (int)s->input_display, (int)s->input_display_bg, (int)s->input_parts,
                  (int)s->input_style, (int)s->input_size);
  for (int w = 0; w < W_COUNT && n < (int)sizeof(buf); w++)
    if (s->watch_labels[w][0])
      n += snprintf(buf + n, sizeof(buf) - n, "watch_label_%d=%s\n", w, s->watch_labels[w]);
  if (n > (int)sizeof(buf) - 1)
    n = (int)sizeof(buf) - 1;
  fs_create_dir(DIR); /* fails harmlessly if it exists */
  fs_delete_file(SETTINGS_FILE);
  fs_file_t f;
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  if (fs_create_file(SETTINGS_FILE, n) != 0 || fs_open(&f, SETTINGS_FILE, FS_WRITE) != 0) {
    log_printf("settings: couldn't write " SETTINGS_FILE);
    return;
  }
  nn_result_t r = fs_write(f, 0, buf, (size_t)n, &flush);
  fs_close(f);
  log_printf(r == 0 ? "settings: saved" : "settings: write failed");
}

/* (memcmp isn't one of the functions we import) */
static bool same_bytes(const void *a, const void *b, size_t n)
{
  const uint8_t *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return false;
  return true;
}

/* appends what's been logged to the log file, about once a second (the game's thread) */
void settings_log_frame(void)
{
  static int since;
  static char chunk[8192];
  if (!mounted || ++since < LOG_FILE_GAP)
    return;
  since = 0;
  size_t n = log_take_pending(chunk, sizeof(chunk));
  if (!n)
    return;
  fs_file_t f;
  if (fs_open(&f, LOG_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0) {
    fs_create_file(LOG_FILE, 0);
    if (fs_open(&f, LOG_FILE, FS_WRITE | FS_ALLOW_APPEND) != 0)
      return;
  }
  int64_t size = 0;
  fs_size(&size, f);
  fs_write_option_t flush = { FS_WRITE_FLUSH };
  fs_write(f, size, chunk, n, &flush);
  fs_close(f);
}

void settings_frame(bool menu_open)
{
  if (!mounted)
    return;
  since_save++;
  if (menu_open || since_save < SAVE_GAP_FRAMES)
    return;
  snapshot_t now = take();
  if (!same_bytes(&now, &saved, sizeof(now))) {
    save(&now);
    saved = now;
    since_save = 0;
  }
}
