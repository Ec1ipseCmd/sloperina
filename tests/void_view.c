/* Production visibility raster: camera clipping, exact solid depth and tint. */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "../src/ootgz/void_view.c"

uintptr_t camera(void) { return 0; }
bool sys_mapped(uintptr_t a,size_t n,uint32_t p) { (void)a;(void)n;(void)p;return false; }
static cam_t cam={.right={1,0,0},.up={0,1,0},.forward={0,0,1},.tan_x=1,.tan_y=1};
static vec3f_t box[8];
static uint8_t result[VOID_VIEW_PIXELS];
static void bounds(float x,float y,float z,float half)
{
  for (int i=0;i<8;i++) box[i]=(vec3f_t){x+(i&1?half:-half),y+(i&2?half:-half),z+(i&4?half:-half)};
}
static void wall(float left,float right,float bottom,float top,float z,int kind)
{
  vec3f_t a[3]={{left,bottom,z},{right,bottom,z},{right,top,z}};
  vec3f_t b[3]={{left,bottom,z},{right,top,z},{left,top,z}};
  void_view_occluder(a,kind,NULL);void_view_occluder(b,kind,NULL);
}
static unsigned finish(void)
{
  void_view_finish();memset(result,0,sizeof(result));
  bool any=void_view_copy(result);unsigned count=0;
  for (unsigned i=0;i<sizeof(result);i++) count+=result[i]!=0;
  assert(any==(count!=0));return count;
}
static void start(void) { void_view_begin(&cam,60);void_view_box(box,true,NULL); }
static void render(const char *path,bool foreground)
{
  FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n640 360\n255\n");
  for (int y=0;y<360;y++) for (int x=0;x<640;x++) {
    bool dark=((x/32+y/32)&1)!=0;
    uint32_t p=dark?0xff947f68u:0xffb8a086u;
    if (foreground && x<320) p=((x/50+y/25)&1)?0xff707478u:0xff606468u;
    if (result[(y/2)*VOID_VIEW_WIDTH+x/2]) p=void_view_blend(p);
    unsigned char rgb[3]={p&255,(p>>8)&255,(p>>16)&255};fwrite(rgb,1,3,f);
  }
  fclose(f);
}
int main(void)
{
  bounds(0,0,10,2);start();unsigned clear=finish();assert(clear>3000 && clear<4000);
  void_nearest_t nearest;assert(void_view_nearest((vec3f_t){0},&nearest));
  assert(nearest.distance>=8 && nearest.distance<8.01f);
  /* A nearer solid hides every fragment, including when its winding reverses. */
  for (int kind=0;kind<3;kind++) {
    start();wall(-100,100,-100,100,4,kind);assert(!finish());
    start();wall(100,-100,-100,100,4,kind);assert(!finish());
  }
  start();wall(-100,100,-100,100,14,COLLISION_WALL);assert(finish()==clear);
  start();wall(-100,100,-100,100,4,COLLISION_TRIGGER);assert(finish()==clear);
  start();wall(-100,0,-100,100,4,COLLISION_WALL);
  unsigned half=finish();assert(half>clear/3 && half<clear/2);
  for (int y=0;y<VOID_VIEW_HEIGHT;y++) for (int x=0;x<=VOID_VIEW_WIDTH/2;x++)
    assert(!result[y*VOID_VIEW_WIDTH+x]);
  /* A doorway remains visible; exact triangle depth does not fill its opening. */
  start();wall(-100,-.5f,-100,100,4,COLLISION_WALL);wall(.5f,100,-100,100,4,COLLISION_WALL);
  unsigned doorway=finish();assert(doorway>0 && doorway<clear);
  assert(result[90*VOID_VIEW_WIDTH+160]);
  start();void_view_box(box,true,NULL);assert(finish()==clear); /* overlap is one tint */
  void_view_begin(&cam,60);void_view_box(box,false,NULL);assert(!finish());
  void_view_begin(&cam,4);void_view_box(box,true,NULL);assert(!finish());
  void_view_begin(&cam,0);void_view_box(box,true,NULL);assert(!finish());
  bounds(0,0,-10,2);start();assert(!finish());
  bounds(100,0,10,2);start();assert(!finish());
  /* Camera inside a giant box: fill visible faces even with no visible perimeter. */
  for (int i=0;i<8;i++) box[i]=(vec3f_t){i&1?100:-100,i&2?100:-100,i&4?5:-5};
  start();assert(finish()==VOID_VIEW_PIXELS);render("obj/tests/void-filled.ppm",false);
  start();wall(-100,0,-100,100,2,COLLISION_WALL);assert(finish()>20000);
  render("obj/tests/void-occluded.ppm",true);
  cam.pos[2]=4.94f;start();assert(finish()==VOID_VIEW_PIXELS);
  cam.pos[2]=0;start();assert(finish()==VOID_VIEW_PIXELS);
  void_view_clear();assert(!void_view_copy(result));assert(!void_view_nearest((vec3f_t){0},&nearest));
  /* Invalid inputs never reach float-to-integer raster conversion. */
  box[0].x=NAN;start();finish();
  assert(void_view_blend(0x12000000u)==0x12142080u);
  assert(void_view_blend(0x12ffffffu)==0x1294a0ffu);
  bounds(0,0,10,2);
  clock_t before=clock();
  for (int frame=0;frame<20;frame++) {
    start();
    for (int i=0;i<32768;i++) {
      float x=(i%256)*.04f-5,y=(i/256)*.04f-2.5f;
      vec3f_t v[3]={{x,y,6},{x+.05f,y,6},{x,y+.05f,6}};
      void_view_occluder(v,COLLISION_WALL,NULL);
    }
    finish();
  }
  printf("PASS: visible void faces, occlusion/openings, clipping, inside, snapshots, 50%% blend; synthetic 32768-triangle CPU frame %.2f ms\n",
         1000.0*(clock()-before)/CLOCKS_PER_SEC/20);
}
