/* Production frame integration: shared visible geometry, no HUD rectangles. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define read_camera test_read_camera
#include "../src/ootgz/airwall.c"
#undef read_camera
const airshape_t data_airshapes[1]={0};
const airwall_t data_airwalls[1]={0};
const airmap_t data_airmaps[1]={0};
const int data_n_airmaps=0;
bool voids_shown,collision_shown;
static bool paused,ready=true,player_ready=true;
void collision_frame(void) {}
float collision_occlusion_range(vec3f_t p) { (void)p;return ready?60:0; }
void collision_each_occluder(void (*f)(const vec3f_t[3],int,void *),void *ctx) {
  vec3f_t v[3]={{-20,-20,20},{20,-20,20},{0,20,20}};f(v,COLLISION_WALL,ctx);
}
void collision_each_triangle(void (*f)(const vec3f_t[3],int,void *),void *ctx) {
  vec3f_t v[3]={{-2,-1,8},{2,-1,8},{0,2,8}};f(v,COLLISION_FLOOR,ctx);f(v,COLLISION_FLOOR,ctx);
}
const char *T(const char *s) { return s; }
const char *TF(const char *s) { return s; }
const char *gd_get_str(int i) { (void)i;return NULL; }
void log_printf(const char *s,...) { (void)s; }
bool link_drawn_pos(vec3f_t *p) { *p=(vec3f_t){0,0,4};return player_ready; }
uintptr_t link_actor(void) { return 123; }
bool actor_label_position(uintptr_t a,vec3f_t *p) { assert(a==123);*p=(vec3f_t){0,1.8f,4};return player_ready; }
bool gd_get_bool(uint32_t h,bool *out) { (void)h;*out=paused;return true; }
bool gd_get_int(uint32_t h,int32_t *out) { (void)h;*out=0;return true; }
void voids_each_box(void (*f)(const vec3f_t[8],bool,void *),void *ctx) {
  vec3f_t box[8];for (int i=0;i<8;i++) box[i]=(vec3f_t){i&1?2:-2,i&2?2:-2,i&4?12:8};f(box,true,ctx);
}
uintptr_t camera(void) { return 0; }
bool sys_mapped(uintptr_t a,size_t n,uint32_t p) { (void)a;(void)n;(void)p;return false; }
bool test_read_camera(cam_t *cam) { *cam=(cam_t){.right={1,0,0},.up={0,1,0},.forward={0,0,1},.tan_x=1,.tan_y=1};return true; }
int main(void) {
  static view_image_t view;
  collision_shown=true;airwall_frame();assert(void_view_snapshot(&view));
  assert(surface_edge_count==3 && view.count==3 && view.hide_link);
  for (unsigned i=0;i<view.count;i++) assert(view.lines[i].color==VIEW_FLOOR);
  voids_shown=true;collision_shown=false;airwall_frame();assert(void_view_snapshot(&view));
  assert(view.count>0 && view.hide_link);
  for (unsigned i=0;i<view.count;i++) assert(view.lines[i].color==VIEW_VOID_EDGE);
  paused=true;airwall_frame();assert(!void_view_snapshot(&view));paused=false;
  ready=false;airwall_frame();assert(!void_view_snapshot(&view));ready=true;
  player_ready=false;airwall_frame();assert(!void_view_snapshot(&view));player_ready=true;
  voids_shown=false;airwall_frame();assert(!void_view_snapshot(&view));
  puts("PASS: world-only geometry frame, unique edges, independent toggles, player mask, pause/unavailable clearing");
}
