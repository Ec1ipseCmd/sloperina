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

# World geometry is rendered through the production band compositor by run_void_view.py.
