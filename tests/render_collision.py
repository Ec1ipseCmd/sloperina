"""Render the actual collision menu construction using the host menu backend."""
import argparse
import os
from pathlib import Path
import subprocess
from render_enemy_health import function

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
args = parser.parse_args()
out = root / "obj/tests"
out.mkdir(parents=True, exist_ok=True)
source = (root / "src/ootgz/menus.c").read_text(encoding="utf-8")
callbacks = source[source.index("static bool get_air_walls_shown"):source.index("static void build_scene")]
start = source.index('  menu_init(&m_surfaces,')
end = source.index('\n}', start)
construction = source[start:end]
harness = r'''
#define main watch_test_main
#include "../../tests/js_height.c"
#undef main
#include "voids.h"
#include "collision.h"
static menu_t m_collision,m_surfaces;
int voids_mode;
const char *const voids_mode_names[]={"normal","off while levitating","off"};
void voids_summary(char *b,int n) { snprintf(b,n,"no identified void sensors in loaded area"); }
void collision_summary(char *b,int n) { snprintf(b,n,"463 triangles, 1 tiles"); }
void airwall_summary(char *b,int n) { snprintf(b,n,"12 walls here, 3 within 60"); }
'''
harness += callbacks + "\nint main(void) {\n" + construction + r'''
  collision_shown=true;
  assert(m_surfaces.n_items==9 && m_collision.n_items==11);
  for (int kind=0;kind<4;kind++) {
    menu_item_t *it=&m_surfaces.items[kind+1];
    int previous=collision_kinds;
    set_surface_kind(it,false);assert(!get_surface_kind(it));
    assert((collision_kinds&~(1<<kind))==(previous&~(1<<kind)));
    set_surface_kind(it,true);assert(get_surface_kind(it));
  }
  set_surface_range(NULL,3);assert(collision_radius==20);
  set_surface_detail(NULL,2);assert(get_surface_detail(NULL)==2);
  for (int page=0;page<2;page++) {
    menu_current=page?&m_surfaces:&m_collision;
    menu_current->selected=page?2:0;
    menu_draw();gfx_frame();
    render_panel(&gfx_published()->panel[0],page?"obj/tests/surfaces-menu.ppm":"obj/tests/collision-menu.ppm");
    gfx_clear();
  }
  puts("PASS: production collision menus, independent filters, range and headless rendering");
  return 0;
}
'''
path = out / "collision_menu.c"
path.write_text(harness, encoding="utf-8")
env = os.environ.copy()
env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
exe = out / ("collision_menu.exe" if os.name == "nt" else "collision_menu")
flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
for cmd in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-Isrc/ootgz", str(path), "src/ootgz/watches.c", "src/ootgz/settings.c", "src/ootgz/gfx.c",
             "src/ootgz/font.c", "src/ootgz/menu.c", "-lm", "-o", str(exe)], [str(exe)]):
    result = subprocess.run(cmd, cwd=root, env=env, capture_output=True, text=True, **flags)
    print(result.stdout, end="")
    print(result.stderr, end="")
    result.check_returncode()

# Also exercise the production projection and NVN rectangle raster together.
nvn = (root / "src/ootgz/nvn.c").read_text(encoding="utf-8")
overlay = r'''
#define main geometry_test_main
#include "../../tests/void_geometry.c"
#undef main
#include "font.h"
#define SC_ONE 16
#define DOT_COORD_MAX 65535
#define MIN_DOT_SIZE 2
typedef int nvn_texture_t;
typedef struct { int x,y,w,h; } nvn_rect_t;
static unsigned char pixels[360][640][3];
static int clears;
static void set_render_target(nvn_texture_t *t) { (void)t; }
static void reset_scissor(void) {}
static bool command_memory_low(int i) { return i>=MAX_DOTS; }
static void clear_scissored(int x,int y,int w,int h,int height,const float rgba[4])
{
  assert(height==360 && x>=0 && y>=0 && x+w<=640 && y+h<=360);
  clears++;
  for (int j=y;j<y+h;j++) for (int i=x;i<x+w;i++)
    for (int k=0;k<3;k++) pixels[j][i][k]=(unsigned char)(rgba[k]*255);
}
'''
overlay += "\n".join(function(nvn,name) for name in
                      ("font_pixel","font_to_screen_ceil","draw_airwall_dots","void_hint_text","draw_void_hint"))
overlay += r'''
int main(void)
{
  geometry_test_main();
  memset(pixels,30,sizeof(pixels));
  cam_t cam={.right={1,0,0},.up={0,1,0},.forward={0,0,1},.tan_x=1,.tan_y=1};
  aw_dot_t dots[MAX_DOTS];
  for (int kind=0;kind<4;kind++) {
    int n=0;surface_edges_reset();float x=-3.5f+kind*2;
    vec3f_t v[3]={{x,-1,5},{x+1,-1,5},{x+.5f,1,5}};
    void_draw_t draw={dots,&n,&cam};draw_surface(v,kind,&draw);draw_surface_edges(&draw);assert(n>0);
    draw_airwall_dots(dots,n,NULL,(nvn_rect_t){0,0,640,360},360,32);
  }
  assert(clears>100);
  FILE *file=fopen("obj/tests/collision-overlay.ppm","wb");assert(file);
  fprintf(file,"P6\n640 360\n255\n");fwrite(pixels,1,sizeof(pixels),file);fclose(file);
  memset(pixels,30,sizeof(pixels));surface_edges_reset();
  int n=0;void_draw_t dense={dots,&n,&cam};
  for (int i=0;i<1000;i++) {
    float x=-3+(float)(i%40)*.15f,y=-2+(float)(i/40)*.15f;
    vec3f_t v[3]={{x,y,5},{x+.2f,y,5},{x+.1f,y+.2f,5}};
    draw_surface(v,COLLISION_FLOOR,&dense);
  }
  vec3f_t outline[3]={{-4,-3,6},{4,-3,6},{0,3,6}};
  draw_surface(outline,COLLISION_TRIGGER,&dense);draw_surface_edges(&dense);
  assert(n>0 && n<=SURFACE_RECTS && surface_width<SURFACE_WIDTH);
  draw_airwall_dots(dots,n,NULL,(nvn_rect_t){0,0,640,360},360,32);
  /* Every pixel in the accepted raster reaches the actual NVN rectangle draw;
   * the drawing cap cannot silently truncate its lower/farther edges. */
  for (int y=0;y<surface_height;y++) for (int x=0;x<surface_width;x++)
    if (surface_mask[y*surface_width+x]) {
      /* Coordinates pass through the overlay's 16-bit normalized space. */
      int px=(int)((uint64_t)((2*x+1)*65535/(2*surface_width))*640/65535);
      int py=(int)((uint64_t)((2*y+1)*65535/(2*surface_height))*360/65535);
      assert(pixels[py][px][0]!=30 || pixels[py][px][1]!=30 || pixels[py][px][2]!=30);
    }
  file=fopen("obj/tests/collision-dense.ppm","wb");assert(file);
  fprintf(file,"P6\n640 360\n255\n");fwrite(pixels,1,sizeof(pixels),file);fclose(file);
  printf("Dense scene: %u projected edges, %dx%d raster, %d rectangles\n",surface_line_count,surface_width,surface_height,n);
  memset(pixels,30,sizeof(pixels));n=0;
  vec3f_t corners[8];
  for (int i=0;i<8;i++) corners[i]=(vec3f_t){i&1?100:-100,i&2?100:-100,i&4?5:-5};
  draw_void_box(corners,true,&dense);assert(n>0);
  draw_airwall_dots(dots,n,NULL,(nvn_rect_t){0,0,640,360},360,32);
  file=fopen("obj/tests/void-large-volume.ppm","wb");assert(file);
  fprintf(file,"P6\n640 360\n255\n");fwrite(pixels,1,sizeof(pixels),file);fclose(file);
  for (int scenario=0;scenario<3;scenario++) {
    memset(pixels,40,sizeof(pixels));n=0;
    for (int i=0;i<8;i++) corners[i]=(vec3f_t){i&1?2:-2,i&2?2:-1,i&4?7:5};
    if (scenario==1) for (int i=0;i<8;i++) corners[i]=(vec3f_t){i&1?100:-100,i&2?100:-100,i&4?5:-5};
    position_ready=nearest_ready=true;
    fake_nearest=(void_nearest_t){.point={0,0,5},.distance=2.3f,.inside=scenario==1};
    if (scenario==2) nearest_ready=false;
    else draw_void_box(corners,true,&dense);
    publish_void_hint(&cam,true);void_hint_t hint;assert(airwall_void_hint(&hint));
    draw_airwall_dots(dots,n,NULL,(nvn_rect_t){0,0,640,360},360,32);
    draw_void_hint(&hint,NULL,(nvn_rect_t){0,0,640,360},360,32);
    char path[80];snprintf(path,sizeof(path),"obj/tests/void-warning-%d.ppm",scenario);
    file=fopen(path,"wb");assert(file);
    fprintf(file,"P6\n640 360\n255\n");fwrite(pixels,1,sizeof(pixels),file);fclose(file);
    for (int scale=1;scale<96;scale++)
      draw_void_hint(&hint,NULL,(nvn_rect_t){10,10,620,340},360,scale);
  }
  puts("PASS: production surface projection and NVN colors/raster");
  return 0;
}
'''
path = out / "collision_overlay.c"
path.write_text(overlay, encoding="utf-8")
exe = out / ("collision_overlay.exe" if os.name == "nt" else "collision_overlay")
for cmd in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc/ootgz", str(path),
             "src/ootgz/world_view.c", "src/ootgz/font.c", "-lm", "-o", str(exe)], [str(exe)]):
    result = subprocess.run(cmd, cwd=root, env=env, capture_output=True, text=True, **flags)
    print(result.stdout, end="")
    print(result.stderr, end="")
    result.check_returncode()
