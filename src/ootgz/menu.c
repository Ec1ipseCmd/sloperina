#include "menu.h"

#include "gfx.h"
#include "input.h"
#include "lang.h"
#include "sys.h"

#define POOL_ITEMS 1400

static menu_item_t pool[POOL_ITEMS];
static int pool_used;

menu_t *menu_current;
static menu_t *menu_last; /* where the menu was when it closed; it reopens there */

void menu_init(menu_t *m, const char *title, int cap)
{
  menu_t *parent = m->parent; /* may already be linked by menu_add_submenu */
  memset(m, 0, sizeof(*m));
  m->parent = parent;
  m->title = title;
  if (pool_used + cap > POOL_ITEMS) {
    log_printf("menu pool full (%s)", title);
    cap = POOL_ITEMS - pool_used;
  }
  m->items = &pool[pool_used];
  m->cap = cap;
  pool_used += cap;
}

menu_item_t *menu_add(menu_t *m, menu_item_kind_t kind, const char *label)
{
  if (m->n_items >= m->cap) {
    log_printf("menu %s: more than %d items", m->title, m->cap);
    return 0;
  }
  menu_item_t *it = &m->items[m->n_items++];
  memset(it, 0, sizeof(*it));
  it->kind = kind;
  it->label = label;
  it->indent = m->grouping && kind != MI_BACK;
  return it;
}

menu_item_t *menu_add_submenu(menu_t *m, const char *label, menu_t *sub)
{
  menu_item_t *it = menu_add(m, MI_SUBMENU, label);
  if (it) {
    it->submenu = sub;
    sub->parent = m;
  }
  return it;
}

menu_item_t *menu_add_action(menu_t *m, const char *label, void (*fn)(menu_item_t *), intptr_t data)
{
  menu_item_t *it = menu_add(m, MI_ACTION, label);
  if (it)
    it->action = fn, it->data = data;
  return it;
}

menu_item_t *menu_add_check(menu_t *m, const char *label, bool (*get)(menu_item_t *),
                            void (*set)(menu_item_t *, bool), intptr_t data)
{
  menu_item_t *it = menu_add(m, MI_CHECK, label);
  if (it)
    it->get_check = get, it->set_check = set, it->data = data;
  return it;
}

menu_item_t *menu_add_option(menu_t *m, const char *label, int n, const char *const *choices,
                             int (*get)(menu_item_t *), void (*set)(menu_item_t *, int), intptr_t data)
{
  menu_item_t *it = menu_add(m, MI_OPTION, label);
  if (it) {
    it->n_choices = n, it->choices = choices;
    it->get_option = get, it->set_option = set, it->data = data;
  }
  return it;
}

menu_item_t *menu_add_number(menu_t *m, const char *label, int32_t (*get)(menu_item_t *),
                             void (*set)(menu_item_t *, int32_t), int32_t (*max)(menu_item_t *),
                             void (*fill)(menu_item_t *), intptr_t data)
{
  menu_item_t *it = menu_add(m, MI_NUMBER, label);
  if (it)
    it->get_num = get, it->set_num = set, it->num_max = max, it->action = fill, it->data = data;
  return it;
}

menu_item_t *menu_add_text(menu_t *m, void (*text)(menu_item_t *, char *, int), intptr_t data)
{
  menu_item_t *it = menu_add(m, MI_TEXT, 0);
  if (it)
    it->text = text, it->data = data;
  return it;
}

static void heading_text(menu_item_t *it, char *buf, int len) { snprintf(buf, len, "%s", T(it->label)); }

menu_item_t *menu_add_heading(menu_t *m, const char *label)
{
  m->grouping = false;
  menu_item_t *it = menu_add(m, MI_TEXT, label);
  if (it)
    it->text = heading_text;
  m->grouping = true;
  return it;
}

void menu_end_group(menu_t *m) { m->grouping = false; }

menu_item_t *menu_add_return(menu_t *m)
{
  return menu_add(m, MI_BACK, "return");
}

static bool selectable(menu_item_t *it) { return it->kind != MI_TEXT; }

static void move(menu_t *m, int dir)
{
  int part = m->n_items ? m->items[m->selected].part : 0;
  bool from_parts = m->n_items && m->items[m->selected].n_parts > 1;
  for (int n = 0; n < m->n_items; n++) {
    m->selected = (m->selected + dir + m->n_items) % m->n_items;
    menu_item_t *it = &m->items[m->selected];
    if (selectable(it)) {
      /* rows of buttons: the cursor stays in its column, as gz's does */
      if (it->n_parts > 1)
        it->part = from_parts ? (part < it->n_parts ? part : it->n_parts - 1) : 0;
      return;
    }
  }
}

void menu_refresh(menu_t *m)
{
  if (m->selected >= m->n_items)
    m->selected = m->n_items ? m->n_items - 1 : 0;
  if (m->n_items && !selectable(&m->items[m->selected])) {
    int keep = m->selected;
    move(m, 1);
    if (m->selected < keep) /* wrapped round: the item above instead */
      m->selected = keep, move(m, -1);
  }
}

void menu_reset(menu_t *m)
{
  m->n_items = 0;
  m->grouping = false;
}

static void enter(menu_t *m)
{
  menu_current = m;
  if (m->n_items && !selectable(&m->items[m->selected]))
    move(m, 1);
}

menu_item_t *menu_editing;

void menu_goto(menu_t *m)
{
  menu_editing = 0;
  enter(m);
}

void menu_edit(menu_item_t *it) { menu_editing = it; }

void menu_open(menu_t *root)
{
  enter(menu_last ? menu_last : root);
  input_block_game = true;
}

void menu_close(void)
{
  menu_editing = 0;
  menu_last = menu_current && menu_current->parent ? menu_current : 0; /* the top level: start there */
  menu_current = 0;
  input_block_game = false;
  gfx_clear();
}

void menu_back(void)
{
  if (menu_editing) {
    menu_editing = 0;
    return;
  }
  if (menu_current->parent)
    menu_current = menu_current->parent;
  else
    menu_close();
}

/* gz's log: what a switch or checkbox is now, or the action taken (unless it logged something itself) */
static void log_item(menu_item_t *it, uint32_t logged_before)
{
  if (gfx_log_count != logged_before || !it->label || !it->label[0])
    return;
  char val[64] = "";
  switch (it->kind) {
    case MI_CHECK:
      gfx_logf("%s %s", T(it->label), T(it->get_check(it) ? "on" : "off"));
      break;
    case MI_OPTION: {
      int c = it->get_option(it);
      if (c >= 0 && c < it->n_choices) {
        if (it->choices)
          snprintf(val, sizeof(val), "%s", T(it->choices[c]));
        else if (it->choice_text)
          it->choice_text(it, c, val, sizeof(val));
        gfx_logf("%s: %s", T(it->label), val);
      }
      break;
    }
    case MI_ACTION:
      if (it->action)
        gfx_logf("%s", T(it->label));
      break;
    default:
      break;
  }
}

static void change_do(menu_item_t *it, int dir)
{
  if (it->kind == MI_ACTION && it->on_change) {
    it->on_change(it, dir);
  } else if (it->kind == MI_OPTION && it->n_choices > 0) {
    int c = it->get_option(it);
    c = c < 0 ? 0 : (c + dir + it->n_choices) % it->n_choices;
    it->set_option(it, c);
  } else if (it->kind == MI_CHECK) {
    it->set_check(it, dir > 0);
  } else if (it->kind == MI_NUMBER) {
    int32_t v = it->get_num(it), hi = it->num_max ? it->num_max(it) : INT32_MAX;
    if (v == NUM_UNKNOWN)
      return;
    bool has_hi = hi != NUM_UNKNOWN && hi != INT32_MAX;
    if (dir > 1 || dir < -1) { /* by 10 (L held): stop at the ends, wrap only from an end */
      if (dir < 0)
        v = v == it->num_min && has_hi ? hi : (v + dir < it->num_min ? it->num_min : v + dir);
      else
        v = has_hi && v == hi ? it->num_min : (has_hi && v + dir > hi ? hi : v + dir);
    } else {
      v += dir;
      if (v < it->num_min)
        v = has_hi ? hi : it->num_min; /* wrap like gz */
      else if (has_hi && v > hi)
        v = it->num_min;
    }
    it->set_num(it, v);
  }
}

static void activate_do(menu_item_t *it)
{
  switch (it->kind) {
    case MI_SUBMENU:
      enter(it->submenu);
      break;
    case MI_ACTION:
      if (it->action)
        it->action(it);
      break;
    case MI_CHECK:
      it->set_check(it, !it->get_check(it));
      break;
    case MI_OPTION:
      change_do(it, 1);
      break;
    case MI_NUMBER:
      if (it->action)
        it->action(it);
      break;
    case MI_BACK:
      menu_back();
      break;
    default:
      break;
  }
}

/* numbers aren't logged (scrolling would fill the log); the rest is, as gz logs its commands */
static void change(menu_item_t *it, int dir)
{
  uint32_t before = gfx_log_count;
  change_do(it, dir);
  if (it->kind != MI_NUMBER)
    log_item(it, before);
}

static void activate(menu_item_t *it)
{
  uint32_t before = gfx_log_count;
  activate_do(it);
  log_item(it, before);
}

void menu_input(void)
{
  menu_t *m = menu_current;
  if (!m || !m->n_items)
    return;
  menu_item_t *it = &m->items[m->selected];
  if (menu_editing) {
    if (menu_editing != it || !it->edit || !it->edit(it))
      menu_editing = 0;
    return;
  }
  bool parts = it->n_parts > 1;
  /* numbers: L held makes d-left/right go by 10, so their L (fill) happens when L is let go,
   * if the d-pad wasn't used meanwhile */
  static bool l_on_number, l_used;
  bool l_held = (input_held() & BTN_L) != 0;
  if (it->kind == MI_NUMBER) {
    if (input_pressed() & BTN_L)
      l_on_number = true, l_used = false;
    if (l_held && (input_repeat(BTN_DLEFT) || input_repeat(BTN_DRIGHT))) {
      l_used = true;
      change(it, input_repeat(BTN_DLEFT) ? -10 : 10);
      return;
    }
    if (!l_held && l_on_number) {
      l_on_number = false;
      if (!l_used)
        activate(it);
      return;
    }
    if (input_pressed() & BTN_L)
      return;
  } else {
    l_on_number = false;
  }
  if (input_repeat(BTN_DUP)) {
    if (!it->on_move || !it->on_move(it, -1))
      move(m, -1);
  } else if (input_repeat(BTN_DDOWN)) {
    if (!it->on_move || !it->on_move(it, 1))
      move(m, 1);
  } else if (parts && input_repeat(BTN_DLEFT))
    it->part = it->part > 0 ? it->part - 1 : 0;
  else if (parts && input_repeat(BTN_DRIGHT))
    it->part = it->part < it->n_parts - 1 ? it->part + 1 : it->n_parts - 1;
  else if (input_repeat(BTN_DLEFT))
    change(it, -1);
  else if (input_repeat(BTN_DRIGHT))
    change(it, 1);
  else if (it->press && ((input_pressed() & BTN_L) || (it->press_repeat && input_repeat(BTN_L))))
    it->press(it, it->part < it->n_parts ? it->part : 0);
  else if (input_pressed() & BTN_L)
    activate(it);
}

static void item_line(menu_item_t *it, char *buf, int len);

void menu_item_line(menu_item_t *it, char *buf, int len)
{
  if (!it->icon) {
    item_line(it, buf, len);
    return;
  }
  bool wide = it->icon == GFX_ICON_FOLDER;
  char rest[96];
  item_line(it, rest, sizeof(rest));
  snprintf(buf, len, "%c%s %s", it->icon, wide ? "\x03" : "", rest);
}

static void item_line(menu_item_t *it, char *buf, int len)
{
  char val[96] = "";
  switch (it->kind) {
    case MI_SUBMENU:
      snprintf(buf, len, "%s >", T(it->label));
      return;
    case MI_CHECK: {
      char box[3] = { it->get_check(it) ? GFX_CHECK_ON : GFX_CHECK_OFF, GFX_WIDE_REST, 0 };
      if (it->text) {
        it->text(it, val, sizeof(val));
        snprintf(buf, len, "%s%-13s %s", box, T(it->label), val);
      } else {
        snprintf(buf, len, "%s%s", box, T(it->label));
      }
      return;
    }
    case MI_OPTION: {
      int c = it->get_option(it);
      if (c < 0 || c >= it->n_choices)
        snprintf(val, sizeof(val), "-");
      else if (it->choices)
        snprintf(val, sizeof(val), "%s", T(it->choices[c]));
      else if (it->choice_text)
        it->choice_text(it, c, val, sizeof(val));
      snprintf(buf, len, "%s  < %s >", T(it->label), val);
      return;
    }
    case MI_NUMBER: {
      int32_t v = it->get_num(it);
      if (v == NUM_UNKNOWN)
        snprintf(val, sizeof(val), "-");
      else if (it->text)
        it->text(it, val, sizeof(val));
      else
        snprintf(val, sizeof(val), "%d", (int)v);
      int32_t hi = it->num_max ? it->num_max(it) : NUM_UNKNOWN;
      if (it->num_max && hi != NUM_UNKNOWN && v != NUM_UNKNOWN && !it->text)
        snprintf(buf, len, "%s  < %s > / %d", T(it->label), val, (int)hi);
      else
        snprintf(buf, len, "%s  < %s >", T(it->label), val);
      return;
    }
    case MI_TEXT:
      it->text(it, buf, len);
      return;
    default:
      if (it->text && !it->label[0]) /* the text is the whole line */
        it->text(it, buf, len);
      else if (it->text) {
        it->text(it, val, sizeof(val));
        snprintf(buf, len, "%s  %s", T(it->label), val);
      } else {
        snprintf(buf, len, "%s", T(it->label));
      }
      return;
  }
}

/* What the selected item's controls do: its own hint, or one for its kind. */
static const char *hint_raw(const menu_item_t *it);
static const char *hint_of(const menu_item_t *it) { return T(hint_raw(it)); }
static const char *hint_raw(const menu_item_t *it)
{
  if (it->part_hints && it->n_parts > 1 && it->part < it->n_parts && it->part_hints[it->part])
    return it->part_hints[it->part];
  if (it->hint)
    return it->hint[0] ? it->hint : 0;
  switch (it->kind) {
    case MI_NUMBER: return it->action ? "d-left/right: change (L held: by 10)   L: fill"
                                      : "d-left/right: change (L held: by 10)";
    case MI_OPTION: return "d-left/right or L: change";
    default: return 0;
  }
}

void menu_draw(void)
{
  menu_t *m = menu_current;
  if (!m)
    return;
  /* item rows that fit on screen (title and a blank line above them; a blank and the hint below) */
  const char *hint = m->n_items ? hint_of(&m->items[m->selected]) : 0;
  if (menu_editing && menu_editing->edit_hint)
    hint = T(menu_editing->edit_hint);
  if (menu_editing && menu_editing->edit_alone) { /* out of the way of what's being moved */
    gfx_begin();
    gfx_text(0, 0, GFX_TITLE, T(m->title));
    gfx_text(1, 2, GFX_DIM, hint ? hint : "");
    gfx_end();
    return;
  }
  int visible = gfx_menu_rows - 4; /* always room for the hint, so the list doesn't jump */
  if (visible < 3)
    visible = 3;
  /* headings and text just above the cursor come into view with it (they can't be selected, so
   * the cursor alone would never scroll back to a heading at the top) */
  int top = m->selected;
  while (top > 0 && !selectable(&m->items[top - 1]) && m->selected - (top - 1) < visible)
    top--;
  /* and the same below it: the read-only lines under a menu's return */
  int bottom = m->selected;
  while (bottom + 1 < m->n_items && !selectable(&m->items[bottom + 1]) && bottom + 1 - top < visible)
    bottom++;
  if (top < m->scroll)
    m->scroll = top;
  else if (bottom >= m->scroll + visible)
    m->scroll = bottom - visible + 1;
  if (m->scroll > m->n_items - visible)
    m->scroll = m->n_items - visible > 0 ? m->n_items - visible : 0;
  char line[GFX_COLS * 2];
  gfx_begin();
  /* breadcrumb: root > ... > this menu; long paths keep their end */
  const menu_t *path[16];
  int depth = 0;
  for (const menu_t *p = m; p && depth < 16; p = p->parent)
    path[depth++] = p;
  char crumb[GFX_COLS * 4];
  int len = 0;
  for (int i = depth - 1; i >= 0; i--)
    len += snprintf(crumb + len, len < (int)sizeof(crumb) ? sizeof(crumb) - len : 0, "%s%s",
                    i == depth - 1 ? "" : " > ", T(path[i]->title));
  if (len > (int)sizeof(crumb) - 1)
    len = sizeof(crumb) - 1;
  const char *shown = crumb;
  if (len > GFX_COLS - 1) { /* "... > " + the end of the path */
    shown = crumb + len - (GFX_COLS - 7);
    snprintf(line, sizeof(line), "... >%s", shown);
    shown = line;
  }
  gfx_text(0, 0, GFX_TITLE, shown);
  int title_len = (int)strlen(shown); /* (shown can be `line`, which the items reuse) */
  for (int r = 0; r < visible && m->scroll + r < m->n_items; r++) {
    int i = m->scroll + r;
    menu_item_line(&m->items[i], line, sizeof(line));
    gfx_style_t st = i == m->selected ? GFX_SELECTED : m->items[i].kind == MI_TEXT ? GFX_DIM : GFX_NORMAL;
    int col = 1 + m->items[i].indent * MENU_INDENT, cs = 0, cl = 0;
    if (i == m->selected && m->items[i].cursor && m->items[i].cursor(&m->items[i], line, &cs, &cl)) {
      int l = (int)strlen(line);
      gfx_text(col, 2 + r, GFX_NORMAL, line);
      if (cs >= 0 && cs < l) {
        if (cs + cl < l)
          line[cs + cl] = 0;
        gfx_text(col + cs, 2 + r, GFX_SELECTED, line + cs);
      }
    } else if (i == m->selected && m->items[i].icon_select) { /* gz: a button icon in the highlight colour */
      char icon[2] = { line[0], 0 };
      gfx_text(col, 2 + r, GFX_NORMAL, line);
      gfx_text(col, 2 + r, GFX_SELECTED, icon);
      char *lt = line, *gt; /* and its switch (strchr isn't imported) */
      while (*lt && *lt != '<')
        lt++;
      for (gt = lt; *gt && *gt != '>'; gt++)
        ;
      if (*lt && *gt) {
        gt[1] = 0;
        gfx_text(col + (int)(lt - line), 2 + r, GFX_SELECTED, lt);
      }
    } else {
      gfx_text(col, 2 + r, st, line);
    }
  }
  if (m->scroll > 0)
    gfx_text(0, 2, GFX_DIM, "^");
  if (m->scroll + visible < m->n_items)
    gfx_text(0, 2 + visible - 1, GFX_DIM, "v");
  /* The panel keeps one size while in this menu (a size change redraws it, which flashes): the hint
   * row is always there, and the width is the widest line or hint of any item, not just the visible
   * ones. FILL is drawn as nothing but counts as content. */
  static const char fill[2] = { 0x7F, 0 };
  int n_shown = m->n_items - m->scroll < visible ? m->n_items - m->scroll : visible;
  gfx_text(1, 2 + n_shown + 1, GFX_DIM, hint ? hint : fill);
  int width = title_len;
  for (int i = 0; i < m->n_items; i++) {
    const char *h = hint_of(&m->items[i]), *eh = T(m->items[i].edit_hint);
    int w = 1 + (int)strlen(h ? h : "");
    if (eh && 1 + (int)strlen(eh) > w)
      w = 1 + (int)strlen(eh);
    for (int k = 0; m->items[i].part_hints && k < m->items[i].n_parts; k++)
      if (m->items[i].part_hints[k] && 1 + (int)strlen(T(m->items[i].part_hints[k])) > w)
        w = 1 + (int)strlen(T(m->items[i].part_hints[k]));
    menu_item_line(&m->items[i], line, sizeof(line));
    int lw = 1 + m->items[i].indent * MENU_INDENT + (int)strlen(line);
    if (lw > w)
      w = lw;
    if (w > width)
      width = w;
  }
  if (width > GFX_COLS)
    width = GFX_COLS;
  gfx_text(width - 1, 1, GFX_DIM, fill);
  gfx_end();
}
