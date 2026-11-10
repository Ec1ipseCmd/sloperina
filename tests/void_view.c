/* Production visibility raster: camera clipping, exact solid depth and tint. */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
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
  assert(!count || any);return count;
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
static void test_lines(void)
{
  static view_image_t view;
  static uint8_t band[3840*64];
  void_view_begin(&cam,60);
  wall(-100,100,-100,100,4,COLLISION_WALL);
  void_view_line((vec3f_t){-2,0,8},(vec3f_t){2,0,8},VIEW_WALL);
  void_view_finish();assert(!void_view_snapshot(&view));
  /* A diagonal edge on a sloped solid keeps its full length; per-cell depth
   * gradients prevent alternating visible/hidden pieces along the same face. */
  void_view_begin(&cam,60);
  vec3f_t slope[3]={{-3,-2,6},{3,-2,10},{0,3,8}};
  void_view_occluder(slope,COLLISION_WALL,NULL);
  void_view_line(slope[0],slope[1],VIEW_WALL);
  void_view_finish();assert(void_view_snapshot(&view) && view.count==1);
  void_view_begin(&cam,60);slope[2].z=12;
  void_view_occluder(slope,COLLISION_WALL,NULL);
  void_view_line(slope[0],slope[2],VIEW_WALL);
  void_view_finish();assert(void_view_snapshot(&view) && view.count==1);
  const int widths[]={640,1280,1920,2560,3840};
  for (unsigned size=0;size<sizeof(widths)/sizeof(*widths);size++) {
    int width=widths[size];
    int height=width*9/16;
    memset(&view,0,sizeof(view));view.count=2;
    view.lines[0]=(view_line_t){0,.5f,1,.5f,VIEW_WALL};
    view.lines[1]=(view_line_t){0,.5f,1,.5f,VIEW_WALL}; /* duplicate never adds opacity */
    unsigned pixels=0;
    for (int y=0;y<height;y+=64) {
      int rows=height-y<64?height-y:64;
      void_view_band(&view,width,height,y,rows,band);
      for (int row=0;row<rows;row++) for (int x=0;x<width;x++) if (band[row*width+x]) {
        pixels++;assert(row+y==(int)(.5f*(height-1)+.5f));
        assert(band[row*width+x]==VIEW_WALL);
      }
    }
    assert(pixels==(unsigned)width); /* exactly one physical pixel, every scale */
  }
  for (int slope=0;slope<16;slope++) for (int reverse=0;reverse<2;reverse++) {
    float x=(slope%4)/3.0f,y=(slope/4)/3.0f;
    memset(&view,0,sizeof(view));view.count=1;
    view.lines[0]=reverse?(view_line_t){x,y,.5f,.5f,VIEW_WALL}:(view_line_t){.5f,.5f,x,y,VIEW_WALL};
    void_view_band(&view,128,72,0,72,band);
    int x0=64,y0=36,x1=(int)(x*127+.5f),y1=(int)(y*71+.5f);
    int dx=abs(x1-x0),dy=abs(y1-y0),count=0;
    assert(band[y0*128+x0] && band[y1*128+x1]);
    for (int i=0;i<128*72;i++) count+=band[i]!=0;
    assert(count==(dx>dy?dx:dy)+1); /* no widening on steep edges or reversal */
  }
  void_view_begin(&cam,60);
  void_view_line((vec3f_t){-2,0,8},(vec3f_t){2,0,8},VIEW_FLOOR);
  void_view_line((vec3f_t){-2,-.6f,4},(vec3f_t){2,-.6f,4},VIEW_FLOOR);
  void_view_link((vec3f_t){0,-.5f,4},(vec3f_t){0,1.8f,4});
  void_view_finish();assert(void_view_snapshot(&view) && view.hide_link);
  void_view_band(&view,640,360,160,64,band);
  assert(!band[20*640+320]); /* torso */
  assert(band[20*640+260] && band[20*640+380]); /* no surrounding box */
  assert(band[46*640+320]); /* ground below the feet remains visible */
  assert(void_view_color(0x12000000u,VIEW_WALL)==0x12bf7d30u);
  assert(void_view_color(0x12345678u,0)==0x12345678u);
  assert(void_view_color(0x12ffffffu,VIEW_WALL)==0x12ffbc70u);
  assert(void_view_color(0x12000000u,VIEW_VOID)==0x12142080u);
  void_view_begin(&cam,60);
  assert(void_view_link((vec3f_t){0,0,-4},(vec3f_t){0,2,-4}) && !pending.hide_link);
  assert(!void_view_link((vec3f_t){0},(vec3f_t){0,20,0}));
  puts("PASS: line occlusion, sloped-face continuity, one-pixel width across resolutions, single 75% line tint, Link exclusion");
}
static void render_world(void)
{
  static view_image_t view;
  static uint8_t band[1920*64];
  void_view_begin(&cam,60);
  wall(-100,-1,-100,100,6,COLLISION_WALL);
  vec3f_t floor[2][3]={{{-20,-1,2},{20,-1,2},{20,-1,30}},{{-20,-1,2},{20,-1,30},{-20,-1,30}}};
  for (int i=0;i<2;i++) void_view_occluder(floor[i],COLLISION_FLOOR,NULL);
  for (int i=0;i<8;i++) box[i]=(vec3f_t){i&1?3:-3,i&2?2:-3,i&4?12:9};
  void_view_box(box,true,NULL);
  for (int i=-8;i<=8;i++) {
    void_view_line((vec3f_t){i,-1,2},(vec3f_t){i,-1,25},VIEW_FLOOR);
    if (i>=2) void_view_line((vec3f_t){-8,-1,i},(vec3f_t){8,-1,i},VIEW_FLOOR);
  }
  void_view_link((vec3f_t){0,-1,4},(vec3f_t){0,.8f,4});
  void_view_finish();assert(void_view_snapshot(&view));
  FILE *f=fopen("obj/tests/world-thin-visible.ppm","wb");assert(f);fprintf(f,"P6\n1920 1080\n255\n");
  for (int y=0;y<1080;y+=64) {
    int rows=1080-y<64?1080-y:64;void_view_band(&view,1920,1080,y,rows,band);
    for (int yy=0;yy<rows;yy++) for (int x=0;x<1920;x++) {
      uint32_t p=(x/80+(y+yy)/80)&1?0xff554738u:0xff625342u;
      if (x<800 && y+yy<=630) p=0xff383838u; /* foreground wall, floor in front */
      if (x>900 && x<1020 && y+yy>440 && y+yy<680) p=0xff44722cu; /* synthetic player */
      p=void_view_color(p,band[yy*1920+x]);
      unsigned char rgb[3]={p&255,(p>>8)&255,(p>>16)&255};fwrite(rgb,1,3,f);
    }
  }
  fclose(f);
  clock_t before=clock();
  for (int frame=0;frame<10;frame++) for (int y=0;y<1080;y+=64)
    void_view_band(&view,1920,1080,y,1080-y<64?1080-y:64,band);
  printf("World overlay: production 1080p thin-line render, %.2f ms per synthetic band pass\n",1000.0*(clock()-before)/CLOCKS_PER_SEC/10);
}
int main(void)
{
  test_lines();
  render_world();
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
