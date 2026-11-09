/* Languages (lang.h). There's no directory listing among our imports, so the files are found by
 * trying the codes below; each file names its language ("language"). A loaded file is kept whole in
 * memory: its strings are decoded in place and the translations looked up by the MurmurHash3 of the
 * English. The reader takes every "key": "string" pair in the file, at any depth, so the layout is
 * forgiving: "language" names it and every other pair is a translation. */
#include "lang.h"

#include "gamedata.h"
#include "sdfs.h"
#include "sys.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);

#define LANG_DIR SD_DIR "/lang"
#define OLD_LANG_DIR SD_OLD_DIR "/lang" /* before the rename */
#define MAX_FILE (256 * 1024)
#define TABLE 4096 /* open addressing, a power of two */

static const char *const codes[] = { "es", "fr", "de", "it", "pt", "nl", "pl", "sv", "da", "no", "fi", "cs",
                                     "hu", "tr", "ro", "ca", "id", "custom" };
#define N_CODES ((int)(sizeof(codes) / sizeof(*codes)))

static const char *names[N_CODES + 1] = { "english" };
static char name_buf[N_CODES + 1][24];
static int code_of[N_CODES + 1]; /* names[i], i > 0: codes[code_of[i]] */
static int n_found = 1, current;

static char *text; /* the loaded file */
static struct { uint32_t hash; const char *value; } table[TABLE];
static int n_strings;

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

/* --- reading ------------------------------------------------------------------------------------ */

static char *read_file(const char *code)
{
  char path[64];
  snprintf(path, sizeof(path), LANG_DIR "/%s.json", code);
  fs_file_t f;
  if (fs_open(&f, path, FS_READ) != 0) {
    snprintf(path, sizeof(path), OLD_LANG_DIR "/%s.json", code);
    if (fs_open(&f, path, FS_READ) != 0)
      return 0;
  }
  int64_t size = 0;
  fs_size(&size, f);
  char *buf = 0;
  if (size > 0 && size < MAX_FILE && (buf = aligned_alloc(16, (size_t)size + 1))) {
    size_t got = 0;
    fs_read(&got, f, 0, buf, (size_t)size);
    buf[got] = 0;
  }
  fs_close(f);
  return buf;
}

/* U+00C0..U+00FF without its accent (the font has ASCII only) */
static const char latin1[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static char fold(unsigned v) { return v < 0x80 ? (char)v : v >= 0xC0 && v <= 0xFF ? latin1[v - 0xC0] : '?'; }

/* p just after an opening quote: decodes the string where it is (it only gets shorter), returns the
 * position after the closing quote, or 0 if there isn't one */
static char *decode(char *p)
{
  char *out = p;
  while (*p && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    if (c == '\\') {
      char e = *p ? *p++ : 0;
      if (e == 'u') {
        unsigned v = 0;
        for (int i = 0; i < 4 && *p; i++, p++)
          v = v * 16 + (unsigned)(*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0');
        *out++ = fold(v);
      } else
        *out++ = e == 'n' || e == 't' ? ' ' : e;
    } else if (c < 0x80) {
      *out++ = (char)c;
    } else { /* UTF-8 */
      int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
      unsigned v = c & (0x3Fu >> more);
      for (int i = 0; i < more && ((unsigned char)*p & 0xC0) == 0x80; i++)
        v = v << 6 | ((unsigned char)*p++ & 0x3F);
      *out++ = fold(v);
    }
  }
  if (*p != '"')
    return 0;
  *out = 0;
  return p + 1;
}

/* Calls pair(key, value) for each "key": "value" in the text (decoding it in place). */
static void each_pair(char *p, void (*pair)(const char *key, const char *value, void *ctx), void *ctx)
{
  const char *key = 0;
  bool colon = false;
  while (*p) {
    char c = *p++;
    if (c == '"') {
      char *s = p, *end = decode(p);
      if (!end)
        return;
      p = end;
      if (key && colon)
        pair(key, s, ctx), key = 0;
      else
        key = s;
      colon = false;
    } else if (c == ':') {
      colon = key != 0;
    } else if (c == ',' || c == '{' || c == '}' || c == '[' || c == ']') {
      key = 0, colon = false;
    }
  }
}

/* --- the table ---------------------------------------------------------------------------------- */

static void add(const char *key, const char *value, void *ctx)
{
  if (same(key, "language")) {
    if (ctx)
      snprintf((char *)ctx, 24, "%s", value);
    return;
  }
  if (!value[0] || ctx) /* empty: stays English; ctx set: only after the name */
    return;
  uint32_t h = murmur3(key);
  for (unsigned i = h & (TABLE - 1), n = 0; n < TABLE; i = (i + 1) & (TABLE - 1), n++)
    if (!table[i].value || table[i].hash == h) {
      n_strings += !table[i].value;
      table[i].hash = h, table[i].value = value;
      return;
    }
}

const char *T(const char *english)
{
  if (!text || !english || !english[0])
    return english;
  uint32_t h = murmur3(english);
  for (unsigned i = h & (TABLE - 1), n = 0; n < TABLE && table[i].value; i = (i + 1) & (TABLE - 1), n++)
    if (table[i].hash == h)
      return table[i].value;
  return english;
}

/* the conversions of a format, in order ("%-12s %d" -> "sd") */
static void conversions(const char *f, char *out, int len)
{
  int n = 0;
  for (; *f && n < len - 1; f++) {
    if (*f != '%')
      continue;
    f++;
    while (*f && !((*f >= 'a' && *f <= 'z' && *f != 'l' && *f != 'h' && *f != 'z') || *f == '%' || *f == 'X'))
      f++;
    if (!*f)
      break;
    if (*f != '%')
      out[n++] = *f;
  }
  out[n] = 0;
}

const char *TF(const char *english)
{
  const char *t = T(english);
  if (t == english)
    return t;
  char a[16], b[16];
  conversions(english, a, sizeof(a));
  conversions(t, b, sizeof(b));
  return same(a, b) ? t : english;
}

/* --- languages ---------------------------------------------------------------------------------- */

void lang_init(void)
{
  n_found = 1;
  for (int c = 0; c < N_CODES; c++) {
    char *t = read_file(codes[c]);
    if (!t)
      continue;
    snprintf(name_buf[n_found], sizeof(name_buf[n_found]), "%s", codes[c]);
    each_pair(t, add, name_buf[n_found]); /* only the name */
    free(t);
    names[n_found] = name_buf[n_found];
    code_of[n_found++] = c;
  }
  log_printf("lang: %d language file%s found in " LANG_DIR, n_found - 1, n_found == 2 ? "" : "s");
}

int lang_count(void) { return n_found; }
const char *const *lang_names(void) { return names; }
int lang_current(void) { return current; }
const char *lang_code(void) { return current ? codes[code_of[current]] : "en"; }

bool lang_select(int i)
{
  if (i < 0 || i >= n_found)
    return false;
  char *old = text;
  text = 0;
  memset(table, 0, sizeof(table));
  n_strings = 0;
  free(old);
  current = 0;
  if (i == 0)
    return true;
  char *t = read_file(codes[code_of[i]]);
  if (!t) {
    log_printf("lang: couldn't read %s.json", codes[code_of[i]]);
    return false;
  }
  each_pair(t, add, 0);
  text = t, current = i;
  log_printf("lang: %s, %d strings", names[i], n_strings);
  return true;
}

bool lang_select_code(const char *code)
{
  for (int i = 1; i < n_found; i++)
    if (same(codes[code_of[i]], code))
      return lang_select(i);
  return lang_select(0);
}
