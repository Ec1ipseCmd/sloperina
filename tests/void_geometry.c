/* Exercise the production void outline projection, clipping and shared dot cap. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ootgz/airwall.c"

const airshape_t data_airshapes[1] = {0};
const airwall_t data_airwalls[1] = {0};
const airmap_t data_airmaps[1] = {0};
const int data_n_airmaps = 0;
bool voids_shown;
bool collision_shown;
void collision_frame(void) {}
void collision_each_triangle(void (*draw)(const vec3f_t [3],int,void *),void *ctx) { (void)draw;(void)ctx; }
const char *T(const char *s) { return s; }
const char *TF(const char *s) { return s; }
const char *gd_get_str(int i) { (void)i; return NULL; }
void log_printf(const char *fmt, ...) { (void)fmt; }
bool link_drawn_pos(vec3f_t *out) { (void)out; return false; }
bool gd_get_bool(uint32_t h, bool *out) { (void)h; *out=false; return true; }
bool gd_get_int(uint32_t h, int32_t *out) { (void)h; *out=0; return true; }
void voids_each_box(void (*draw)(const vec3f_t [8], bool, void *), void *ctx) { (void)draw; (void)ctx; }
uintptr_t camera(void) { return 0; }
bool sys_mapped(uintptr_t a, size_t n, uint32_t p) { (void)a; (void)n; (void)p; return false; }

static void test_continuous_lines(void)
{
  /* Every slope, direction, clipped boundary and zero-length segment forms one
   * connected raster component, including both endpoints. */
  static uint8_t visited[SURFACE_WIDTH*SURFACE_HEIGHT];
  static unsigned queue[SURFACE_WIDTH*SURFACE_HEIGHT];
  for (int slope=0;slope<16;slope++) for (int reverse=0;reverse<2;reverse++) {
    float endpoints[4]={.5f,.5f,(slope%4)/3.0f,(slope/4)/3.0f};
    surface_line_count=1;
    surface_lines[0].x0=endpoints[reverse?2:0];surface_lines[0].y0=endpoints[reverse?3:1];
    surface_lines[0].x1=endpoints[reverse?0:2];surface_lines[0].y1=endpoints[reverse?1:3];
    surface_lines[0].color=AW_WALL;
    surface_rasterize(640,360);
    memset(visited,0,sizeof(visited));
    unsigned head=0,tail=0,total=0;
    for (unsigned i=0;i<sizeof(surface_mask);i++) if (surface_mask[i]) total++;
    unsigned first=(unsigned)(surface_coord(surface_lines[0].y0,360)*640+surface_coord(surface_lines[0].x0,640));
    assert(surface_mask[first]==AW_WALL);queue[tail++]=first;visited[first]=1;
    while (head<tail) {
      unsigned at=queue[head++];int x=(int)(at%640),y=(int)(at/640);
      for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
        int xx=x+dx,yy=y+dy;
        if (xx<0 || xx>=640 || yy<0 || yy>=360) continue;
        unsigned next=(unsigned)(yy*640+xx);
        if (!visited[next] && surface_mask[next]) { visited[next]=1;queue[tail++]=next; }
      }
    }
    unsigned end=(unsigned)(surface_coord(surface_lines[0].y1,360)*640+surface_coord(surface_lines[0].x1,640));
    assert(visited[end] && tail==total);
  }
  surface_lines[0].x0=surface_lines[0].x1=.5f;
  surface_lines[0].y0=surface_lines[0].y1=.5f;
  surface_rasterize(640,360);
  assert(surface_pack(640,360,1)==1);
}

int main(void)
{
  test_continuous_lines();
  cam_t cam = { .right={1,0,0}, .up={0,1,0}, .forward={0,0,1}, .tan_x=1, .tan_y=1 };
  aw_dot_t output[MAX_DOTS+1];
  memset(output,0x5A,sizeof(output));
  int n=0;
  void_draw_t draw={output,&n,&cam};
  vec3f_t corners[8];
  for (int i=0;i<8;i++) corners[i]=(vec3f_t){ i&1 ? 1:-1, i&2 ? 1:-1, i&4 ? 6:4 };
  draw_void_box(corners,true,&draw);
  assert(n>12 && n<=MAX_DOTS);
  for (int i=0;i<n;i++) assert(output[i].color==AW_VOID && output[i].x0<=output[i].x1 && output[i].y0<=output[i].y1);
  for (int i=0;i<8;i++) corners[i].z-=10;
  n=0; draw_void_box(corners,true,&draw); assert(n==0); /* all behind the camera */
  for (int i=0;i<8;i++) corners[i].z+=5;
  n=0; draw_void_box(corners,false,&draw); assert(n>0 && n<=MAX_DOTS); /* crossing near plane */
  for (int i=0;i<n;i++) assert(output[i].color==AW_VOID_INACTIVE);
  for (int i=0;i<8;i++) corners[i].z+=100;
  n=0; draw_void_box(corners,true,&draw); assert(n==0); /* range culling */
  for (int i=0;i<8;i++) corners[i].z-=95;
  n=MAX_DOTS-1; draw_void_box(corners,true,&draw); assert(n==MAX_DOTS);
  const unsigned char *guard=(const unsigned char *)&output[MAX_DOTS];
  for (unsigned i=0;i<sizeof(output[0]);i++) assert(guard[i]==0x5A);
  /* Inside a tower-sized slab: its perimeter misses the entire viewport. */
  for (int i=0;i<8;i++) corners[i]=(vec3f_t){i&1?100:-100,i&2?100:-100,i&4?5:-5};
  n=0;draw_void_box(corners,true,&draw);assert(n>0 && n<MAX_DOTS);
  vec3f_t triangle[3]={{-1,-1,5},{1,-1,5},{0,1,5}};
  for (int kind=0;kind<COLLISION_KINDS;kind++) {
    n=0;surface_edges_reset();
    draw_surface(triangle,kind,&draw);
    draw_surface_edges(&draw);
    assert(n>0 && n<=MAX_DOTS);
    for (int i=0;i<n;i++) assert(output[i].color==AW_FLOOR+kind);
    int original=n;
    unsigned edges=surface_edge_count;
    draw_surface(triangle,kind,&draw); assert(n==original && surface_edge_count==edges); /* shared edges */
  }
  /* Dense near geometry must not exhaust the budget before a distant edge. */
  n=0;surface_edges_reset();
  for (int i=0;i<1000;i++) {
    float x=-3+(float)(i%40)*.15f,y=-2+(float)(i/40)*.15f;
    vec3f_t v[3]={{x,y,5},{x+.2f,y,5},{x+.1f,y+.2f,5}};
    draw_surface(v,COLLISION_FLOOR,&draw);
  }
  vec3f_t distant[3]={{-4,-3,6},{4,-3,6},{0,3,6}};
  draw_surface(distant,COLLISION_TRIGGER,&draw);draw_surface_edges(&draw);
  assert(n>0 && n<=SURFACE_RECTS && surface_width<SURFACE_WIDTH);
  unsigned distant_dots=0;
  for (int i=0;i<n;i++) if (output[i].color==AW_TRIGGER) distant_dots++;
  assert(distant_dots>0);
  /* Packing must retain every colored pixel, even after the dense scene forces
   * a coarser raster. No sampling or budget truncation of individual edges. */
  static uint8_t rebuilt[SURFACE_WIDTH*SURFACE_HEIGHT];
  memset(rebuilt,0,sizeof(rebuilt));
  for (int i=0;i<n;i++) {
    assert(surface_rects[i].x1<=surface_width && surface_rects[i].y1<=surface_height);
    for (int y=surface_rects[i].y0;y<surface_rects[i].y1;y++)
      for (int x=surface_rects[i].x0;x<surface_rects[i].x1;x++)
        rebuilt[y*surface_width+x]=surface_rects[i].color;
  }
  assert(!memcmp(rebuilt,surface_mask,(size_t)surface_width*surface_height));
  for (unsigned i=0;i<sizeof(output[0]);i++) assert(guard[i]==0x5A);
  puts("PASS: continuous collision lines, lossless rectangle packing, dense-view fallback, distant geometry, clipping and shared budget");
  return 0;
}
