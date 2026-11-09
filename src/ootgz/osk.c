/* gz's on-screen keyboard (src/gz/osk.c), in our menu: the text line, gz's four rows of keys, and a
 * row of what gz does with Z + the d-pad (shift, space, backspace, delete) plus "ok".
 *
 *   text line   d-left/right: move the cursor   L: done
 *   key rows    d-left/right: pick a key (the column is kept between rows)   L: type it */
#include "osk.h"

#include "gfx.h"
#include "sys.h"

static menu_t osk_menu;
static char text[OSK_MAX + 1];
static int cursor, col;
static bool shift;
static void (*on_done)(const char *, intptr_t);
static intptr_t done_ctx;

static const char *const rows[] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,." };
#define N_ROWS 4
enum { K_SHIFT, K_SPACE, K_BACK, K_DELETE, K_OK, N_COMMANDS };
static const char *const commands[N_COMMANDS] = { "shift", "space", "back", "del", "ok" };

static void insert(char c)
{
  int n = (int)strlen(text);
  if (n >= OSK_MAX)
    return;
  for (int i = n; i > cursor; i--)
    text[i] = text[i - 1];
  text[cursor++] = c;
  text[n + 1] = 0;
}

static void erase(int at)
{
  int n = (int)strlen(text);
  if (at < 0 || at >= n)
    return;
  for (int i = at; i < n; i++)
    text[i] = text[i + 1];
}

static void finish(void)
{
  void (*fn)(const char *, intptr_t) = on_done;
  on_done = 0;
  menu_back();
  if (fn)
    fn(text, done_ctx);
}

/* --- the text line ------------------------------------------------------------------------------ */

static void text_line(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, "%s_", text); /* the _ shows the cursor at the end */
}

static bool text_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = cursor, *len = 1;
  return true;
}

static void text_move(menu_item_t *it, int dir)
{
  cursor += dir;
  int n = (int)strlen(text);
  cursor = cursor < 0 ? 0 : cursor > n ? n : cursor;
}

static void text_done(menu_item_t *it) { finish(); }

/* --- keys ------------------------------------------------------------------------------------------ */

static char key_char(int row, int c)
{
  char k = rows[row][c];
  if (shift && k >= 'a' && k <= 'z')
    k = (char)(k - 'a' + 'A');
  else if (shift && k == '-')
    k = '_';
  return k;
}

static int row_len(int row) { return row < N_ROWS ? (int)strlen(rows[row]) : N_COMMANDS; }

static void key_line(menu_item_t *it, char *buf, int len)
{
  int r = (int)it->data, n = 0;
  if (r < N_ROWS) {
    for (int c = 0; c < row_len(r) && n < len - 2; c++)
      buf[n++] = key_char(r, c), buf[n++] = ' ';
    buf[n ? n - 1 : 0] = 0;
  } else {
    for (int c = 0; c < N_COMMANDS && n < len; c++)
      n += snprintf(buf + n, len - n, "%s%s", c ? " " : "", c == K_SHIFT && shift ? "SHIFT" : commands[c]);
  }
}

static int key_col(const menu_item_t *it)
{
  int n = row_len((int)it->data);
  return col < n ? col : n - 1;
}

static bool key_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  int r = (int)it->data, c = key_col(it);
  if (r < N_ROWS) {
    *start = 2 * c, *len = 1;
    return true;
  }
  int at = 0; /* the command words, one space apart */
  for (int i = 0; i < c; i++)
    at += (int)strlen(commands[i]) + 1;
  *start = at, *len = (int)strlen(commands[c]);
  return true;
}

static void key_move(menu_item_t *it, int dir)
{
  int n = row_len((int)it->data);
  col = (key_col(it) + dir + n) % n;
}

static void key_press(menu_item_t *it)
{
  int r = (int)it->data, c = key_col(it);
  if (r < N_ROWS) {
    insert(key_char(r, c));
    return;
  }
  switch (c) {
    case K_SHIFT: shift = !shift; break;
    case K_SPACE: insert(' '); break;
    case K_BACK:
      if (cursor > 0)
        erase(--cursor);
      break;
    case K_DELETE: erase(cursor); break;
    default: finish(); break;
  }
}

void osk_build(void)
{
  menu_init(&osk_menu, "name", N_ROWS + 4);
  menu_item_t *t = menu_add_action(&osk_menu, "", text_done, 0);
  if (t)
    t->text = text_line, t->cursor = text_cursor, t->on_change = text_move,
    t->hint = "d-left/right: cursor   L: done";
  for (int r = 0; r <= N_ROWS; r++) {
    menu_item_t *k = menu_add_action(&osk_menu, "", key_press, r);
    if (k)
      k->text = key_line, k->cursor = key_cursor, k->on_change = key_move,
      k->hint = r < N_ROWS ? "d-left/right: key   L: type it" : "L: shift / space / backspace / delete / done";
  }
  menu_add_return(&osk_menu);
}

void osk_open(const char *title, const char *initial, void (*done)(const char *, intptr_t), intptr_t ctx)
{
  snprintf(text, sizeof(text), "%s", initial ? initial : "");
  cursor = (int)strlen(text);
  shift = false;
  on_done = done, done_ctx = ctx;
  osk_menu.title = title;
  osk_menu.parent = menu_current; /* return (and R + d-left) go back there without changing anything */
  menu_goto(&osk_menu);
  osk_menu.selected = 0;
}
