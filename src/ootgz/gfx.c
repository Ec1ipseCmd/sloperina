#include "gfx.h"

#include <stdarg.h>

#include "lang.h"
#include "sys.h"

/* gz: 20 frames solid, then 20 fading (at the N64's 20 fps); here in our frames (30 fps) */
#define LOG_SOLID_FRAMES 30
#define LOG_FADE_FRAMES 30
#define LOG_COLS 48

static gfx_panel_t menu, hud, input; /* drawn this frame */
static bool menu_open;
static struct { char msg[LOG_COLS + 1]; int age; } log_lines[GFX_LOG_MAX]; /* [0] the newest */
volatile uint32_t gfx_log_count;

static gfx_screen_t out[2];          /* published screens; out[pub] is the current one */
static volatile int pub = -1;        /* -1: nothing shown */

volatile bool gfx_on_screen;
volatile int gfx_menu_rows = GFX_ROWS;
volatile int gfx_opacity = 60;
volatile int gfx_menu_scale;
volatile int gfx_custom_x = -1, gfx_custom_y = -1;
volatile int gfx_pause_icon;
volatile uint32_t gfx_presents;

static void put(gfx_panel_t *p, int col, int row, gfx_style_t style, const char *s)
{
  if (row < 0 || row >= GFX_ROWS)
    return;
  for (int c = col; c >= 0 && c < GFX_COLS && *s; c++, s++) {
    p->ch[row][c] = *s;
    p->style[row][c] = style;
  }
}

void gfx_begin(void)
{
  memset(&menu, 0, sizeof(menu));
  menu_open = true;
}

void gfx_text(int col, int row, gfx_style_t style, const char *s) { put(&menu, col, row, style, s); }
void gfx_end(void) {}

void gfx_clear(void)
{
  menu_open = false;
}

void gfx_notice(const char *s)
{
  for (int i = GFX_LOG_MAX - 1; i > 0; i--)
    log_lines[i] = log_lines[i - 1];
  snprintf(log_lines[0].msg, sizeof(log_lines[0].msg), "%s", T(s));
  log_lines[0].age = 0;
  gfx_log_count++;
  log_printf("notice: %s", s);
}

void gfx_logf(const char *fmt, ...)
{
  char buf[LOG_COLS + 1];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), TF(fmt), va);
  va_end(va);
  gfx_notice(buf);
}

/* the start-up splash (gfx_splash): shown in the log's place, then faded out as the log is */
#define SPLASH_FADE 30
static struct { char text[LOG_COLS + 1]; uint8_t style, indent; } splash[GFX_LOG_ROWS];
static int n_splash, splash_frames;

void gfx_splash(const char *const *lines, const uint8_t *styles, const uint8_t *indents, int n, int frames)
{
  n_splash = n < GFX_LOG_ROWS ? n : GFX_LOG_ROWS;
  for (int i = 0; i < n_splash; i++) {
    snprintf(splash[i].text, sizeof(splash[i].text), "%s", lines[i] ? lines[i] : "");
    splash[i].style = styles[i], splash[i].indent = indents[i];
  }
  splash_frames = frames;
}

static bool build_splash(gfx_panel_t *p)
{
  if (splash_frames <= 0)
    return false;
  splash_frames--;
  memset(p, 0, sizeof(*p));
  p->corner = GFX_BOTTOM_RIGHT;
  p->clear = true;
  p->yoff = 1;
  unsigned a = splash_frames >= SPLASH_FADE ? 255 : (unsigned)splash_frames * 255 / SPLASH_FADE;
  int cols = 0;
  for (int i = 0; i < n_splash; i++) {
    int l = splash[i].indent + (int)strlen(splash[i].text);
    cols = l > cols ? l : cols;
  }
  for (int i = 0; i < n_splash; i++) {
    put(p, splash[i].indent, i, (gfx_style_t)splash[i].style, splash[i].text);
    p->alpha[i] = (uint8_t)a;
  }
  if (cols > 0) /* the block's size, so its right edge sits at the corner (fillers: drawn as nothing) */
    p->ch[0][cols - 1] = p->ch[0][cols - 1] ? p->ch[0][cols - 1] : 0x7F, p->ch[n_splash - 1][0] =
      p->ch[n_splash - 1][0] ? p->ch[n_splash - 1][0] : 0x7F;
  return true;
}

/* the log panel: newest at the bottom, each line right-aligned, as gz draws it. While anything is
 * shown the panel only grows (a new size means its blend with the picture starts over, 2 frames
 * without it), and it starts small again once it's empty. */
static void build_log(gfx_panel_t *p)
{
  static int keep_cols, keep_rows;
  if (build_splash(p))
    return;
  memset(p, 0, sizeof(*p));
  p->corner = GFX_BOTTOM_RIGHT;
  p->clear = true;
  p->yoff = 1; /* text in the middle of the panel (nvn.c TEXT_TOP) */
  int n = 0, cols = 0;
  for (int i = 0; i < GFX_LOG_MAX; i++) {
    if (!log_lines[i].msg[0])
      continue;
    if (++log_lines[i].age > LOG_SOLID_FRAMES + LOG_FADE_FRAMES) {
      log_lines[i].msg[0] = 0;
      continue;
    }
    int l = (int)strlen(log_lines[i].msg);
    if (l > cols)
      cols = l;
    n = i + 1;
  }
  if (!n) {
    keep_cols = keep_rows = 0;
    return;
  }
  if (cols > keep_cols)
    keep_cols = cols;
  if (n > keep_rows)
    keep_rows = n;
  for (int i = 0; i < n; i++) {
    int row = keep_rows - 1 - i, age = log_lines[i].age;
    if (!log_lines[i].msg[0])
      continue;
    put(p, keep_cols - (int)strlen(log_lines[i].msg), row, GFX_LOG, log_lines[i].msg);
    p->alpha[row] = (uint8_t)(age <= LOG_SOLID_FRAMES ? 255 : 255 - (age - LOG_SOLID_FRAMES) * 255 / LOG_FADE_FRAMES);
  }
  /* the panel's size: a filler (drawn as nothing) in the top-left and bottom-right corners */
  if (!p->ch[0][0])
    p->ch[0][0] = 0x7F;
  if (!p->ch[keep_rows - 1][keep_cols - 1])
    p->ch[keep_rows - 1][keep_cols - 1] = 0x7F;
}

void gfx_input_background(bool on) { input.clear = !on; }
void gfx_input_scale(int percent) { input.scale_pct = (uint8_t)percent; }

void gfx_input(int col, int row, gfx_style_t style, const char *s)
{
  input.corner = GFX_BOTTOM_LEFT;
  input.yoff = 1; /* text in the middle of the panel (nvn.c TEXT_TOP) */
  if (row >= 0 && row < GFX_ROWS)
    input.alpha[row] = 255;
  put(&input, col, row, style, s);
}

void gfx_input_pad(const int8_t sticks[4], uint8_t clicks)
{
  input.pad = 1;
  memcpy(input.sticks, sticks, 4);
  input.clicks = clicks;
}

void gfx_hud_at(int x_permille, int y_permille)
{
  hud.x = (int16_t)x_permille, hud.y = (int16_t)y_permille;
}

void gfx_hud(gfx_corner_t corner, int row, gfx_style_t style, const char *s)
{
  hud.corner = corner;
  hud.yoff = 1; /* text in the middle of the panel (nvn.c TEXT_TOP) */
  put(&hud, 0, row, style, s);
}

/* --- log fallback (until drawing works) ------------------------------------------------- */

static uint32_t logged_hash, logged_sel;
static int since_log;

static void log_menu(const gfx_panel_t *s)
{
  if (!s) {
    if (logged_hash)
      log_printf("---- menu closed ----");
    logged_hash = logged_sel = 0;
    return;
  }
  /* at once when the selection or title changes, otherwise at most every 2 s */
  uint32_t sel = 0;
  for (int r = 0; r < s->rows; r++)
    for (int c = 0; c < s->cols; c++)
      if (s->style[r][c] == GFX_SELECTED || s->style[r][c] == GFX_TITLE)
        sel = sel * 31 + (uint8_t)s->ch[r][c] + r;
  since_log++;
  if (s->hash == logged_hash || (sel == logged_sel && since_log < 120))
    return;
  logged_hash = s->hash;
  logged_sel = sel;
  since_log = 0;
  log_printf("---- menu ----");
  for (int r = 0; r < s->rows; r++) {
    char line[GFX_COLS + 3];
    bool selected = false;
    for (int c = 0; c < GFX_COLS; c++) {
      char ch = s->ch[r][c];
      line[c + 2] = ch == GFX_CHECK_OFF ? 'o' : ch == GFX_CHECK_ON ? 'x' : ch == GFX_ICON_REMOVE ? '-'
                  : ch == GFX_ICON_FOLDER ? '+' : ch == GFX_ICON_WRENCH ? '*' : ch == GFX_ICON_UP ? '^' : ch == GFX_ICON_DOWN ? 'v' : ch == GFX_ICON_ADD ? '+' : ch == GFX_ICON_LEFT ? '<' : ch == GFX_ICON_RIGHT ? '>' : !ch || ch == GFX_WIDE_REST ? ' ' : ch;
      selected |= s->style[r][c] == GFX_SELECTED;
    }
    line[0] = selected ? '>' : ' ';
    line[1] = ' ';
    int n = GFX_COLS + 2;
    while (n > 0 && line[n - 1] == ' ')
      n--;
    line[n] = 0;
    log_printf("%s", line);
  }
}

/* --- publish ------------------------------------------------------------------------------ */

static void measure(gfx_panel_t *p)
{
  p->rows = p->cols = 0;
  uint32_t h = 2166136261u ^ p->corner ^ (uint32_t)p->clear << 8; /* the shadow depends on clear */
  h = (h ^ (uint32_t)p->pad << 9 ^ (uint32_t)p->clicks << 10 ^ (uint32_t)(uint8_t)p->yoff << 12 ^
       (uint32_t)p->scale_pct << 16) * 16777619u;
  for (int i = 0; i < 4; i++) /* the sticks are drawn from these */
    h = (h ^ (uint8_t)p->sticks[i]) * 16777619u;
  for (int r = 0; r < GFX_ROWS; r++)
    for (int c = 0; c < GFX_COLS; c++) {
      if (p->ch[r][c] && p->ch[r][c] != ' ') {
        if (r + 1 > p->rows)
          p->rows = r + 1;
        if (c + 1 > p->cols)
          p->cols = c + 1;
      }
      h = (h ^ (uint8_t)p->ch[r][c] ^ (uint32_t)p->style[r][c] << 8) * 16777619u;
    }
  p->hash = h ? h : 1;
}

void gfx_frame(void)
{
  int next = pub == 0 ? 1 : 0;
  gfx_screen_t *s = &out[next];

  gfx_panel_t *p = &s->panel[0];
  if (menu_open)
    *p = menu;
  else
    memset(p, 0, sizeof(*p));
  p->corner = GFX_TOP_LEFT;
  measure(p);

  s->panel[1] = hud;
  measure(&s->panel[1]);
  memset(&hud, 0, sizeof(hud));

  build_log(&s->panel[GFX_PANEL_LOG]);
  measure(&s->panel[GFX_PANEL_LOG]);
  s->panel[GFX_PANEL_INPUT] = input;
  measure(&s->panel[GFX_PANEL_INPUT]);
  memset(&input, 0, sizeof(input));

  bool any = s->panel[0].rows || s->panel[1].rows || s->panel[GFX_PANEL_LOG].rows || s->panel[GFX_PANEL_INPUT].rows;
  __atomic_store_n(&pub, any ? next : -1, __ATOMIC_RELEASE);
  if (!gfx_on_screen)
    log_menu(menu_open ? &s->panel[0] : 0);
}

const gfx_screen_t *gfx_published(void)
{
  int p = __atomic_load_n(&pub, __ATOMIC_ACQUIRE);
  return p < 0 ? 0 : &out[p];
}
