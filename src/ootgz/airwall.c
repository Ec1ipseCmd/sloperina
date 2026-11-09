/* Air walls: port of airwall.py (FINDINGS §5.11), drawn as dotted outlines.
 *
 * Walls come from data.c (airwalls.json, built from the map layouts by tools/airwalls.py). A wall's
 * world point for a local shape point p is t + R (s * p), with R = Rz Ry Rx from its rotation, as
 * sead's makeR. Each frame the walls within AW_RANGE of the camera are projected with the game's
 * camera (+0x38 position, +0x44 look-at, +0x130 vertical fov, +0x140 aspect) and their edges turned
 * into rectangles, nearest wall first, which nvn.c draws by clearing them (no shaders). Edges within
 * about 19 degrees of level or upright are solid lines of a few long rectangles (a staircase); steeper
 * ones are dots, which takes fewer clears than a staircase there (FINDINGS
 * §5.11 had see-through faces; here only the outlines, and like the Python view they show through
 * scenery). */
#include "airwall.h"

#include "data.h"
#include "gamedata.h"
#include "link.h"
#include "lang.h"
#include "sys.h"

float sinf(float x); /* from sdk */
float cosf(float x);
float tanf(float x);
float sqrtf(float x);

bool airwalls_shown, airwalls_all;

#define AW_RANGE 60.0f
#define MAX_WALLS 256
#define MAX_PTS 4096
#define MAX_EDGES 8192
#define MAX_DOTS 3000
#define DOT_SPACING 6.0f  /* pixels of a 1920x1080 picture between dots */
#define LINE_WIDTH 2.0f   /* the same pixels: a line's thickness, one staircase step at most */
#define STRAIGHT 0.344f   /* tan 19 degrees: up to this slope a staircase takes fewer clears */
#define NEAR 0.05f
#define PHYS_Y 0.585f

/* --- geometry, worked out once per map ------------------------------------------------------------ */

static const airmap_t *cur_map;
static char cur_name[64];
static struct {
  float R[9];
  float centre[3], radius;
  uint16_t first_edge, n_edges;
} geo[MAX_WALLS];
static int n_geo;
static float pts[MAX_PTS][3];
static uint16_t edges[MAX_EDGES][2];
static int n_pts, n_edges;

static void rot(const float r[3], float R[9])
{
  float sx = sinf(r[0]), cx = cosf(r[0]), sy = sinf(r[1]), cy = cosf(r[1]), sz = sinf(r[2]), cz = cosf(r[2]);
  float m[9] = { cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz,
                 cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz,
                 -sy,     sx * cy,                cx * cy };
  memcpy(R, m, sizeof(m));
}

static void world(const airwall_t *w, const float R[9], const float p[3], float out[3])
{
  float q[3] = { p[0] * w->s[0], p[1] * w->s[1], p[2] * w->s[2] };
  for (int i = 0; i < 3; i++)
    out[i] = R[i * 3] * q[0] + R[i * 3 + 1] * q[1] + R[i * 3 + 2] * q[2] + w->t[i];
}

static int add_pt(const float p[3])
{
  if (n_pts >= MAX_PTS)
    return -1;
  memcpy(pts[n_pts], p, sizeof(pts[0]));
  return n_pts++;
}

static void add_edge(int a, int b)
{
  if (a >= 0 && b >= 0 && n_edges < MAX_EDGES)
    edges[n_edges][0] = (uint16_t)a, edges[n_edges][1] = (uint16_t)b, n_edges++;
}

static void build(const airmap_t *m)
{
  n_geo = n_pts = n_edges = 0;
  for (int i = 0; i < m->n && n_geo < MAX_WALLS; i++) {
    const airwall_t *w = &data_airwalls[m->first + i];
    __typeof__(geo[0]) *g = &geo[n_geo++];
    rot(w->r, g->R);
    g->first_edge = (uint16_t)n_edges;
    int p0 = n_pts;
    for (int k = 0; k < w->n_shapes; k++) {
      const airshape_t *sh = &data_airshapes[w->first_shape + k];
      if (sh->type == 0) { /* box: 8 corners, 12 edges */
        int base = n_pts;
        for (int c = 0; c < 8; c++) {
          float l[3], o[3];
          for (int a = 0; a < 3; a++)
            l[a] = sh->a[a] + (c >> a & 1 ? sh->b[a] : -sh->b[a]);
          world(w, g->R, l, o);
          add_pt(o);
        }
        for (int c = 0; c < 8; c++)
          for (int a = 0; a < 3; a++)
            if (!(c >> a & 1))
              add_edge(base + c, base + (c | 1 << a));
      } else { /* cylinder: two rings of 16 and four sides */
        int ra = n_pts;
        for (int ring = 0; ring < 2; ring++) {
          const float *c = ring ? sh->b : sh->a;
          for (int j = 0; j < 16; j++) {
            float ang = 6.2831853f * j / 16, l[3] = { c[0] + sh->radius * cosf(ang), c[1], c[2] + sh->radius * sinf(ang) }, o[3];
            world(w, g->R, l, o);
            add_pt(o);
          }
        }
        for (int j = 0; j < 16; j++) {
          add_edge(ra + j, ra + (j + 1) % 16);
          add_edge(ra + 16 + j, ra + 16 + (j + 1) % 16);
          if (j % 4 == 0)
            add_edge(ra + j, ra + 16 + j);
        }
      }
    }
    g->n_edges = (uint16_t)(n_edges - g->first_edge);
    /* bounding sphere */
    float c[3] = { 0 };
    int np = n_pts - p0;
    for (int j = p0; j < n_pts; j++)
      for (int a = 0; a < 3; a++)
        c[a] += pts[j][a];
    for (int a = 0; a < 3; a++)
      g->centre[a] = np ? c[a] / np : w->t[a];
    g->radius = 0;
    for (int j = p0; j < n_pts; j++) {
      float dx = pts[j][0] - g->centre[0], dy = pts[j][1] - g->centre[1], dz = pts[j][2] - g->centre[2];
      float r = sqrtf(dx * dx + dy * dy + dz * dz);
      if (r > g->radius)
        g->radius = r;
    }
  }
  log_printf("air walls: %s: %d walls, %d edges", m->map, n_geo, n_edges);
}

static bool same(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static void update_map(void)
{
  const char *m = gd_get_str(GD_STR_MAP);
  if (!m || !m[0]) {
    cur_map = 0;
    return;
  }
  if (cur_map && same(cur_name, m))
    return;
  snprintf(cur_name, sizeof(cur_name), "%s", m);
  cur_map = 0;
  for (int i = 0; i < data_n_airmaps; i++)
    if (same(data_airmaps[i].map, m)) {
      cur_map = &data_airmaps[i];
      build(cur_map);
      break;
    }
  if (!cur_map)
    n_geo = 0;
}

/* --- Link inside a wall (airwall.py inside()) ------------------------------------------------------- */

/* depth of point P inside wall w (0 if outside); *up: the nearest face points up (+1), down (-1),
 * or sideways (0) */
static float inside(int wi, const float P[3], int *up)
{
  const airwall_t *w = &data_airwalls[cur_map->first + wi];
  const float *R = geo[wi].R;
  float d[3] = { P[0] - w->t[0], P[1] - w->t[1], P[2] - w->t[2] }, q[3];
  for (int i = 0; i < 3; i++)
    q[i] = R[i] * d[0] + R[3 + i] * d[1] + R[6 + i] * d[2]; /* R^T */
  for (int k = 0; k < w->n_shapes; k++) {
    const airshape_t *sh = &data_airshapes[w->first_shape + k];
    if (sh->type != 0)
      continue;
    float best = 1e9f, nrm[3] = { 0 };
    bool in = true;
    for (int a = 0; a < 3 && in; a++) {
      float s = w->s[a] < 0 ? -w->s[a] : w->s[a];
      if (s < 1e-9f)
        s = 1e-9f;
      float l = q[a] / s - sh->a[a];
      if (l <= -sh->b[a] || l >= sh->b[a]) {
        in = false;
        break;
      }
      for (int sign = -1; sign <= 1; sign += 2) {
        float depth = (sh->b[a] - sign * l) * s;
        if (depth < best) {
          best = depth;
          for (int i = 0; i < 3; i++)
            nrm[i] = R[i * 3 + a] * sign;
        }
      }
    }
    if (in) {
      *up = nrm[1] > 0.7f ? 1 : nrm[1] < -0.7f ? -1 : 0;
      return best;
    }
  }
  return 0;
}

static float link_depth;
static int link_wall = -1, link_up;

static void check_link(void)
{
  vec3f_t d;
  link_wall = -1;
  if (!cur_map || !link_drawn_pos(&d))
    return;
  float P[3] = { d.x, d.y + PHYS_Y, d.z };
  float best = 0;
  for (int i = 0; i < n_geo; i++) {
    const airwall_t *w = &data_airwalls[cur_map->first + i];
    if (w->kind != 0 && !airwalls_all)
      continue;
    float dx = P[0] - geo[i].centre[0], dy = P[1] - geo[i].centre[1], dz = P[2] - geo[i].centre[2];
    if (dx * dx + dy * dy + dz * dz > geo[i].radius * geo[i].radius)
      continue;
    int up;
    float depth = inside(i, P, &up);
    if (depth > best)
      best = depth, link_wall = i, link_up = up;
  }
  link_depth = best;
}

void airwall_link_text(char *buf, int len)
{
  if (!cur_map)
    snprintf(buf, len, TF("no walls here"));
  else if (link_wall < 0)
    snprintf(buf, len, TF("outside"));
  else
    snprintf(buf, len, TF("inside %.2f, out %s"), link_depth, T(link_up > 0 ? "up" : link_up < 0 ? "down" : "sideways"));
}

/* --- projection to dots ------------------------------------------------------------------------- */

static aw_dot_t dots[2][MAX_DOTS];
static int n_dots[2];
static volatile int pub = -1;
static int n_near;

typedef struct { float p[3], f[3], r[3], u[3], tx, ty; } cam_t;

static bool read_camera(cam_t *c)
{
  uintptr_t a = camera();
  if (!a || !sys_mapped(a + 0x38, 0x140 - 0x38 + 4, SYS_PERM_R))
    return false;
  const float *pos = (const float *)(a + 0x38), *look = (const float *)(a + 0x44);
  float fov = *(const float *)(a + 0x130), aspect = *(const float *)(a + 0x140);
  if (!(fov > 0.1f && fov < 3.0f) || !(aspect > 0.5f && aspect < 4.0f))
    return false;
  float f[3] = { look[0] - pos[0], look[1] - pos[1], look[2] - pos[2] };
  float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (n < 1e-4f)
    return false;
  for (int i = 0; i < 3; i++)
    c->p[i] = pos[i], c->f[i] = f[i] / n;
  float r[3] = { -c->f[2], 0, c->f[0] }; /* f x up */
  float rn = sqrtf(r[0] * r[0] + r[2] * r[2]);
  if (rn < 1e-4f)
    return false;
  c->r[0] = r[0] / rn, c->r[1] = 0, c->r[2] = r[2] / rn;
  c->u[0] = c->r[1] * c->f[2] - c->r[2] * c->f[1];
  c->u[1] = c->r[2] * c->f[0] - c->r[0] * c->f[2];
  c->u[2] = c->r[0] * c->f[1] - c->r[1] * c->f[0];
  c->ty = tanf(fov / 2), c->tx = c->ty * aspect;
  return true;
}

static void view(const cam_t *c, const float x[3], float v[3])
{
  float d[3] = { x[0] - c->p[0], x[1] - c->p[1], x[2] - c->p[2] };
  v[0] = d[0] * c->r[0] + d[1] * c->r[1] + d[2] * c->r[2];
  v[1] = d[0] * c->u[0] + d[1] * c->u[1] + d[2] * c->u[2];
  v[2] = d[0] * c->f[0] + d[1] * c->f[1] + d[2] * c->f[2];
}

/* 0..1 across and down the picture */
static void screen(const cam_t *c, const float v[3], float *sx, float *sy)
{
  *sx = (v[0] / (v[2] * c->tx) + 1) / 2;
  *sy = (1 - v[1] / (v[2] * c->ty)) / 2;
}

static uint16_t norm(float v, float size)
{
  v = v / size * 65535;
  return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

/* a rectangle from two corners in reference pixels */
static void put(aw_dot_t *out, int *n, float ax, float ay, float bx, float by, uint8_t color)
{
  aw_dot_t *d = &out[(*n)++];
  d->x0 = norm(ax < bx ? ax : bx, 1920), d->x1 = norm(ax < bx ? bx : ax, 1920);
  d->y0 = norm(ay < by ? ay : by, 1080), d->y1 = norm(ay < by ? by : ay, 1080);
  d->color = color;
}

static void add_dots(aw_dot_t *out, int *n, const cam_t *c, const float a[3], const float b[3], uint8_t color)
{
  float va[3], vb[3];
  view(c, a, va), view(c, b, vb);
  if (va[2] < NEAR && vb[2] < NEAR)
    return;
  if (va[2] < NEAR || vb[2] < NEAR) { /* clip at the near plane */
    float t = (NEAR - va[2]) / (vb[2] - va[2]), cut[3];
    for (int i = 0; i < 3; i++)
      cut[i] = va[i] + t * (vb[i] - va[i]);
    if (va[2] < NEAR)
      memcpy(va, cut, sizeof(cut));
    else
      memcpy(vb, cut, sizeof(cut));
  }
  float x0, y0, x1, y1;
  screen(c, va, &x0, &y0), screen(c, vb, &x1, &y1);
  /* to the picture (1920x1080 reference pixels), clipped to it (Liang-Barsky) */
  x0 *= 1920, x1 *= 1920, y0 *= 1080, y1 *= 1080;
  float dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
  float p[4] = { -dx, dx, -dy, dy }, q[4] = { x0, 1920 - x0, y0, 1080 - y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0)
        return;
      continue;
    }
    float r = q[i] / p[i];
    if (p[i] < 0) {
      if (r > t1)
        return;
      if (r > t0)
        t0 = r;
    } else {
      if (r < t0)
        return;
      if (r < t1)
        t1 = r;
    }
  }
  x1 = x0 + t1 * dx, y1 = y0 + t1 * dy, x0 += t0 * dx, y0 += t0 * dy;
  dx = x1 - x0, dy = y1 - y0;
  float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  bool level = adx >= ady;
  float major = level ? adx : ady, minor = level ? ady : adx;
  if (minor <= major * STRAIGHT) {
    /* a staircase: k pieces along the main direction, each at most LINE_WIDTH off the next */
    int k = (int)(minor / LINE_WIDTH) + 1;
    for (int i = 0; i < k && *n < MAX_DOTS; i++) {
      float ta = (float)i / k, tb = (float)(i + 1) / k, tm = (ta + tb) / 2;
      float ax, ay, bx, by;
      if (level)
        ax = x0 + dx * ta, bx = x0 + dx * tb, ay = by = y0 + dy * tm;
      else
        ay = y0 + dy * ta, by = y0 + dy * tb, ax = bx = x0 + dx * tm;
      put(out, n, ax, ay, bx, by, color);
    }
    return;
  }
  int steps = (int)(major / DOT_SPACING);
  if (steps > 2000)
    steps = 2000;
  for (int i = 0; i <= steps && *n < MAX_DOTS; i++) {
    float t = steps ? (float)i / steps : 0, x = x0 + dx * t, y = y0 + dy * t;
    put(out, n, x, y, x, y, color);
  }
}

void airwall_frame(void)
{
  update_map();
  check_link();
  int next = pub == 0 ? 1 : 0, n = 0;
  n_near = 0;
  cam_t c;
  if (airwalls_shown && cur_map && read_camera(&c)) {
    /* nearest wall first, so the dot budget goes to what's close */
    static int16_t order[MAX_WALLS];
    static float dist[MAX_WALLS];
    int no = 0;
    for (int i = 0; i < n_geo; i++) {
      const airwall_t *w = &data_airwalls[cur_map->first + i];
      if (w->kind != 0 && !airwalls_all)
        continue;
      float dx = geo[i].centre[0] - c.p[0], dy = geo[i].centre[1] - c.p[1], dz = geo[i].centre[2] - c.p[2];
      float d = sqrtf(dx * dx + dy * dy + dz * dz) - geo[i].radius;
      if (d > AW_RANGE)
        continue;
      int j = no++;
      while (j > 0 && dist[j - 1] > d)
        order[j] = order[j - 1], dist[j] = dist[j - 1], j--;
      order[j] = (int16_t)i, dist[j] = d;
    }
    n_near = no;
    for (int k = 0; k < no && n < MAX_DOTS; k++) {
      int i = order[k];
      const airwall_t *w = &data_airwalls[cur_map->first + i];
      uint8_t color = i == link_wall ? AW_INSIDE : w->kind == 0 ? AW_BLOCKS : AW_OTHER;
      for (int e = geo[i].first_edge; e < geo[i].first_edge + geo[i].n_edges; e++)
        add_dots(dots[next], &n, &c, pts[edges[e][0]], pts[edges[e][1]], color);
    }
  }
  n_dots[next] = n;
  __atomic_store_n(&pub, n ? next : -1, __ATOMIC_RELEASE);
}

const aw_dot_t *airwall_published(int *n)
{
  int p = __atomic_load_n(&pub, __ATOMIC_ACQUIRE);
  *n = p < 0 ? 0 : n_dots[p];
  return p < 0 ? 0 : dots[p];
}

void airwall_summary(char *buf, int len)
{
  if (!cur_map)
    snprintf(buf, len, TF("no air walls in this map"));
  else
    snprintf(buf, len, "%d walls here, %d within %.0f", n_geo, n_near, (double)AW_RANGE);
}
