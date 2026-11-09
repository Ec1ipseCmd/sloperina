/* Exercise the production void outline projection, clipping and shared dot cap. */
#include <assert.h>
#include <stdio.h>
#include "../src/ootgz/airwall.c"

const airshape_t data_airshapes[1] = {0};
const airwall_t data_airwalls[1] = {0};
const airmap_t data_airmaps[1] = {0};
const int data_n_airmaps = 0;
bool voids_shown;
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

int main(void)
{
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
  puts("void outlines: projection, near-plane clipping, range, colours and shared dot budget passed");
}
