/* Loaded collision, invisible barriers and void bounds share a thin, blended,
 * visibility-tested world overlay. The original air-wall watch is retained. */
#include "airwall.h"
#include "world_view.h"
#include "voids.h"
#include "void_view.h"
#include "collision.h"
#include "actors.h"

#include "data.h"
#include "gamedata.h"
#include "link.h"
#include "lang.h"
#include "sys.h"

float sinf(float x); /* from sdk */
float cosf(float x);
float sqrtf(float x);

bool airwalls_shown, airwalls_all;

#define AW_RANGE 60.0f
#define MAX_WALLS 256
#define MAX_POINTS 4096
#define MAX_EDGES 8192
#define PHYS_Y 0.585f     /* Link's physics position is this far above his drawn one */
#define FACE_UPRIGHT 0.7f /* a face normal with more height than this points up (or down) */

#define WALL_KIND_BLOCKS 0 /* the kind of wall that stops Link */
#define SHAPE_BOX 0        /* any other shape type is a cylinder */
#define CYLINDER_SEGMENTS 16
#define CYLINDER_SIDE_EVERY 4 /* a side line at every 4th segment: four in all */
#define TWO_PI 6.2831853f

/* --- geometry, worked out once per map ------------------------------------------------------------ */

static const airmap_t *current_map;
static char current_map_name[64];
static struct {
  float R[9]; /* rotation, row-major */
  float centre[3], radius;
  uint16_t first_edge, n_edges;
} wall_geo[MAX_WALLS];
static int n_walls;
static float points[MAX_POINTS][3];
static uint16_t edges[MAX_EDGES][2];
static int n_points, n_edges;

static const airwall_t *map_wall(int index)
{
  return &data_airwalls[current_map->first + index];
}

static bool wall_shown(const airwall_t *wall)
{
  return wall->kind == WALL_KIND_BLOCKS || airwalls_all;
}

/* R = Rz Ry Rx for rotation angles (x, y, z) */
static void rotation_matrix(const float angles[3], float R[9])
{
  float sx = sinf(angles[0]), cx = cosf(angles[0]);
  float sy = sinf(angles[1]), cy = cosf(angles[1]);
  float sz = sinf(angles[2]), cz = cosf(angles[2]);
  float m[9] = { cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz,
                 cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz,
                 -sy,     sx * cy,                cx * cy };
  memcpy(R, m, sizeof(m));
}

/* t + R (s * local) */
static void to_world(const airwall_t *wall, const float R[9], const float local[3], float out[3])
{
  float scaled[3] = { local[0] * wall->s[0], local[1] * wall->s[1], local[2] * wall->s[2] };
  for (int i = 0; i < 3; i++)
    out[i] = R[i * 3] * scaled[0] + R[i * 3 + 1] * scaled[1] + R[i * 3 + 2] * scaled[2] + wall->t[i];
}

static int add_point(const float p[3])
{
  if (n_points >= MAX_POINTS)
    return -1;
  memcpy(points[n_points], p, sizeof(points[0]));
  return n_points++;
}

static void add_edge(int a, int b)
{
  if (a >= 0 && b >= 0 && n_edges < MAX_EDGES) {
    edges[n_edges][0] = (uint16_t)a;
    edges[n_edges][1] = (uint16_t)b;
    n_edges++;
  }
}

/* A box: 8 corners (bit k of the corner number picks the side on axis k) and 12 edges. */
static void add_box(const airwall_t *wall, const float R[9], const airshape_t *box)
{
  int base = n_points;
  for (int corner = 0; corner < 8; corner++) {
    float local[3], world[3];
    for (int axis = 0; axis < 3; axis++)
      local[axis] = box->a[axis] + (corner >> axis & 1 ? box->b[axis] : -box->b[axis]);
    to_world(wall, R, local, world);
    add_point(world);
  }
  for (int corner = 0; corner < 8; corner++)
    for (int axis = 0; axis < 3; axis++)
      if (!(corner >> axis & 1))
        add_edge(base + corner, base + (corner | 1 << axis));
}

/* A cylinder: rings at both ends, joined by four side lines. */
static void add_cylinder(const airwall_t *wall, const float R[9], const airshape_t *cylinder)
{
  int base = n_points;
  for (int ring = 0; ring < 2; ring++) {
    const float *centre = ring ? cylinder->b : cylinder->a;
    for (int j = 0; j < CYLINDER_SEGMENTS; j++) {
      float angle = TWO_PI * j / CYLINDER_SEGMENTS;
      float local[3] = { centre[0] + cylinder->radius * cosf(angle), centre[1],
                         centre[2] + cylinder->radius * sinf(angle) };
      float world[3];
      to_world(wall, R, local, world);
      add_point(world);
    }
  }
  for (int j = 0; j < CYLINDER_SEGMENTS; j++) {
    add_edge(base + j, base + (j + 1) % CYLINDER_SEGMENTS);
    add_edge(base + CYLINDER_SEGMENTS + j, base + CYLINDER_SEGMENTS + (j + 1) % CYLINDER_SEGMENTS);
    if (j % CYLINDER_SIDE_EVERY == 0)
      add_edge(base + j, base + CYLINDER_SEGMENTS + j);
  }
}

/* The sphere around points[first_point..n_points), for skipping walls quickly. */
static void bounding_sphere(const airwall_t *wall, int first_point, float centre[3], float *radius)
{
  float sum[3] = { 0 };
  int count = n_points - first_point;
  for (int j = first_point; j < n_points; j++)
    for (int axis = 0; axis < 3; axis++)
      sum[axis] += points[j][axis];
  for (int axis = 0; axis < 3; axis++)
    centre[axis] = count ? sum[axis] / count : wall->t[axis];
  *radius = 0;
  for (int j = first_point; j < n_points; j++) {
    float dx = points[j][0] - centre[0];
    float dy = points[j][1] - centre[1];
    float dz = points[j][2] - centre[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance > *radius)
      *radius = distance;
  }
}

static void build_map_geometry(const airmap_t *map)
{
  n_walls = 0;
  n_points = 0;
  n_edges = 0;
  for (int i = 0; i < map->n && n_walls < MAX_WALLS; i++) {
    const airwall_t *wall = &data_airwalls[map->first + i];
    __typeof__(wall_geo[0]) *geo = &wall_geo[n_walls++];
    rotation_matrix(wall->r, geo->R);
    geo->first_edge = (uint16_t)n_edges;
    int first_point = n_points;
    for (int k = 0; k < wall->n_shapes; k++) {
      const airshape_t *shape = &data_airshapes[wall->first_shape + k];
      if (shape->type == SHAPE_BOX)
        add_box(wall, geo->R, shape);
      else
        add_cylinder(wall, geo->R, shape);
    }
    geo->n_edges = (uint16_t)(n_edges - geo->first_edge);
    bounding_sphere(wall, first_point, geo->centre, &geo->radius);
  }
  log_printf("air walls: %s: %d walls, %d edges", map->map, n_walls, n_edges);
}

static bool str_equal(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* Rebuild the geometry when the map changes. */
static void update_map(void)
{
  const char *name = gd_get_str(GD_STR_MAP);
  if (!name || !name[0]) {
    current_map = 0;
    return;
  }
  if (current_map && str_equal(current_map_name, name))
    return;
  snprintf(current_map_name, sizeof(current_map_name), "%s", name);
  current_map = 0;
  for (int i = 0; i < data_n_airmaps; i++)
    if (str_equal(data_airmaps[i].map, name)) {
      current_map = &data_airmaps[i];
      build_map_geometry(current_map);
      break;
    }
  if (!current_map)
    n_walls = 0;
}

/* --- Link inside a wall (airwall.py inside()) ------------------------------------------------------- */

/* How deep point P is inside wall `index` (0 if outside). *up: the nearest face points up (+1),
 * down (-1) or sideways (0). Only boxes count. */
static float depth_inside(int index, const float P[3], int *up)
{
  const airwall_t *wall = map_wall(index);
  const float *R = wall_geo[index].R;
  float d[3] = { P[0] - wall->t[0], P[1] - wall->t[1], P[2] - wall->t[2] };
  float rotated[3]; /* R^T d: P in the wall's own axes, still scaled */
  for (int i = 0; i < 3; i++)
    rotated[i] = R[i] * d[0] + R[3 + i] * d[1] + R[6 + i] * d[2];
  for (int k = 0; k < wall->n_shapes; k++) {
    const airshape_t *box = &data_airshapes[wall->first_shape + k];
    if (box->type != SHAPE_BOX)
      continue;
    float best = 1e9f;
    float normal[3] = { 0 };
    bool in = true;
    for (int axis = 0; axis < 3; axis++) {
      float scale = wall->s[axis] < 0 ? -wall->s[axis] : wall->s[axis];
      if (scale < 1e-9f)
        scale = 1e-9f;
      float local = rotated[axis] / scale - box->a[axis];
      if (local <= -box->b[axis] || local >= box->b[axis]) {
        in = false;
        break;
      }
      /* distance to the face on each side of this axis */
      for (int sign = -1; sign <= 1; sign += 2) {
        float depth = (box->b[axis] - sign * local) * scale;
        if (depth < best) {
          best = depth;
          for (int i = 0; i < 3; i++)
            normal[i] = R[i * 3 + axis] * sign;
        }
      }
    }
    if (in) {
      *up = normal[1] > FACE_UPRIGHT ? 1 : normal[1] < -FACE_UPRIGHT ? -1 : 0;
      return best;
    }
  }
  return 0;
}

static float link_depth;
static int link_wall = -1; /* the wall Link is deepest in, or -1 */
static int link_up;

static void check_link(void)
{
  vec3f_t drawn;
  link_wall = -1;
  if (!current_map || !link_drawn_pos(&drawn))
    return;
  float P[3] = { drawn.x, drawn.y + PHYS_Y, drawn.z };
  float best = 0;
  for (int i = 0; i < n_walls; i++) {
    if (!wall_shown(map_wall(i)))
      continue;
    float dx = P[0] - wall_geo[i].centre[0];
    float dy = P[1] - wall_geo[i].centre[1];
    float dz = P[2] - wall_geo[i].centre[2];
    if (dx * dx + dy * dy + dz * dz > wall_geo[i].radius * wall_geo[i].radius)
      continue;
    int up;
    float depth = depth_inside(i, P, &up);
    if (depth > best) {
      best = depth;
      link_wall = i;
      link_up = up;
    }
  }
  link_depth = best;
}

void airwall_link_text(char *buf, int len)
{
  if (!current_map)
    snprintf(buf, len, TF("no walls here"));
  else if (link_wall < 0)
    snprintf(buf, len, TF("outside"));
  else
    snprintf(buf, len, TF("inside %.2f, out %s"), link_depth,
             T(link_up > 0 ? "up" : link_up < 0 ? "down" : "sideways"));
}

static int n_walls_near;

#define GD_SEQUENCE_IS_PAUSE 0x604AEBE1u
#define GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT 0x64A61098u

static bool game_is_paused(void)
{
  bool is_pause = false;
  int32_t pause_requests = 0;
  gd_get_bool(GD_SEQUENCE_IS_PAUSE, &is_pause);
  gd_get_int(GD_SEQUENCE_GAME_PAUSE_REQUEST_COUNT, &pause_requests);
  bool paused = is_pause || pause_requests > 0;

  /* Log each change, with both values, so it can be checked which one the game's menus set. */
  static bool was_paused;
  if (paused != was_paused) {
    if (paused)
      log_printf("air walls: hidden, game paused (IsPause %d, GamePauseRequestCount %d)", (int)is_pause,
                 (int)pause_requests);
    else
      log_printf("air walls: shown again, game unpaused");
    was_paused = paused;
  }
  return paused;
}

/* --- frame ---------------------------------------------------------------------------------------- */

static int16_t near_order[MAX_WALLS];  /* walls within AW_RANGE of the camera, nearest first */
static float near_distance[MAX_WALLS];

/* Fill near_order (an insertion sort) and return how many walls are in it. */
static int sort_near_walls(const cam_t *cam)
{
  int count = 0;
  for (int i = 0; i < n_walls; i++) {
    if (!wall_shown(map_wall(i)))
      continue;
    float dx = wall_geo[i].centre[0] - cam->pos[0];
    float dy = wall_geo[i].centre[1] - cam->pos[1];
    float dz = wall_geo[i].centre[2] - cam->pos[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz) - wall_geo[i].radius;
    if (distance > AW_RANGE)
      continue;
    int j = count++;
    while (j > 0 && near_distance[j - 1] > distance) {
      near_order[j] = near_order[j - 1];
      near_distance[j] = near_distance[j - 1];
      j--;
    }
    near_order[j] = (int16_t)i;
    near_distance[j] = distance;
  }
  return count;
}

#define SURFACE_EDGES 6144
static struct { vec3f_t a,b; int kind; } surface_edges[SURFACE_EDGES];
static uint16_t surface_edge_slots[16384];
static unsigned surface_edge_count;
static bool same_point(vec3f_t a,vec3f_t b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
static void surface_edges_reset(void)
{ surface_edge_count=0;memset(surface_edge_slots,0,sizeof(surface_edge_slots)); }
static void draw_surface(const vec3f_t v[3],int kind,void *arg)
{
  (void)arg;
  for (int i=0;i<3 && surface_edge_count<SURFACE_EDGES;i++) {
    vec3f_t a=v[i],b=v[(i+1)%3];
    if (a.x>b.x || (a.x==b.x && (a.y>b.y || (a.y==b.y && a.z>b.z)))) {
      vec3f_t t=a;a=b;b=t;
    }
    uint32_t words[6],hash=2166136261u;
    memcpy(words,&a,sizeof(a));memcpy(words+3,&b,sizeof(b));
    for (int j=0;j<6;j++) {
      if ((words[j]&0x7fffffffu)==0) words[j]=0;
      hash=(hash^words[j])*16777619u;
    }
    hash=(hash^(unsigned)kind)*16777619u;
    bool duplicate=false;
    unsigned slot=0,probe;
    for (probe=0;probe<64;probe++) {
      slot=(hash+probe)&16383;
      if (!surface_edge_slots[slot]) break;
      unsigned at=surface_edge_slots[slot]-1;
      if (surface_edges[at].kind==kind && same_point(a,surface_edges[at].a) &&
          same_point(b,surface_edges[at].b)) { duplicate=true;break; }
    }
    if (duplicate) continue;
    if (probe<64) surface_edge_slots[slot]=(uint16_t)(surface_edge_count+1);
    surface_edges[surface_edge_count].a=a;surface_edges[surface_edge_count].b=b;
    surface_edges[surface_edge_count++].kind=kind;
  }
}

void airwall_frame(void)
{
  collision_frame();update_map();check_link();n_walls_near=0;
  cam_t cam;
  if (!(airwalls_shown || voids_shown || collision_shown) || game_is_paused() || !read_camera(&cam)) {
    void_view_clear();return;
  }
  float range=collision_occlusion_range((vec3f_t){cam.pos[0],cam.pos[1],cam.pos[2]});
  void_view_begin(&cam,range);
  if (range>0) {
    collision_each_occluder(void_view_occluder,NULL);
    vec3f_t feet,head;
    if (!link_drawn_pos(&feet) || !actor_label_position(link_actor(),&head) || !void_view_link(feet,head)) {
      void_view_clear();return; /* no geometry over an unresolved player */
    }
    if (voids_shown) voids_each_box(void_view_box,NULL);
    surface_edges_reset();
    if (collision_shown) {
      collision_each_triangle(draw_surface,NULL);
      for (unsigned i=0;i<surface_edge_count;i++)
        void_view_line(surface_edges[i].a,surface_edges[i].b,(uint8_t)(VIEW_FLOOR+surface_edges[i].kind));
    }
    n_walls_near=airwalls_shown && current_map?sort_near_walls(&cam):0;
    for (int k=0;k<n_walls_near;k++) {
      int i=near_order[k];const airwall_t *wall=map_wall(i);
      uint8_t color=i==link_wall?VIEW_INSIDE:wall->kind==WALL_KIND_BLOCKS?VIEW_BLOCKS:VIEW_OTHER;
      for (int e=wall_geo[i].first_edge;e<wall_geo[i].first_edge+wall_geo[i].n_edges;e++) {
        const float *a=points[edges[e][0]],*b=points[edges[e][1]];
        void_view_line((vec3f_t){a[0],a[1],a[2]},(vec3f_t){b[0],b[1],b[2]},color);
      }
    }
  }
  void_view_finish();
}

void airwall_summary(char *buf, int len)
{
  if (!current_map)
    snprintf(buf, len, TF("no air walls in this map"));
  else
    snprintf(buf, len, "%d walls here, %d within %.0f", n_walls, n_walls_near, (double)AW_RANGE);
}
