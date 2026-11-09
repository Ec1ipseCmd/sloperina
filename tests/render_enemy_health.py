#!/usr/bin/env python3
"""Exercise nvn.c's actual enemy-label drawing with a headless clear-rectangle backend."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.rfind("\nstatic ", 0, source.index(name + "(")) + 1
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    out = ROOT / "obj" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/ootgz/nvn.c").read_text(encoding="utf-8")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "font.h"
#include "enemy_health.h"
#define SC_ONE 16
typedef int nvn_texture_t;
typedef struct { int x, y, w, h; } nvn_rect_t;
static unsigned char pixels[360][640][3];
static int clears, resets, budget = 100000;
static void set_render_target(nvn_texture_t *tex) { (void)tex; }
static void reset_scissor(void) { resets++; }
static bool command_memory_low(int i) { return i >= budget; }
static void clear_scissored(int x, int y, int w, int h, int height, const float rgba[4])
{
  assert(height == 360 && x >= 0 && y >= 0 && w > 0 && h > 0 && x+w <= 640 && y+h <= 360);
  clears++;
  for (int j=y; j<y+h; j++) for (int i=x; i<x+w; i++)
    for (int k=0; k<3; k++) pixels[j][i][k] = (unsigned char)(rgba[k] * 255);
}
'''
    harness += "\n".join(function(source, name) for name in
                         ("font_pixel", "font_to_screen_ceil", "draw_enemy_labels"))
    harness += r'''
int main(int argc, char **argv)
{
  assert(argc == 2);
  memset(pixels, 55, sizeof(pixels));
  enemy_label_t labels[] = {{16384, 32768, "12/12"}, {32768, 32768, "2/6"}, {49151, 32768, "1/1"}};
  nvn_rect_t area = {0, 0, 640, 360};
  draw_enemy_labels(labels, 3, NULL, area, 360, 32);
  assert(clears > 20 && resets == 1);
  FILE *file = fopen(argv[1], "wb"); assert(file);
  fprintf(file, "P6\n640 360\n255\n"); fwrite(pixels, 1, sizeof(pixels), file); fclose(file);
  int saved = clears;
  labels[0].x = 0; labels[0].y = 0;
  draw_enemy_labels(labels, 1, NULL, area, 360, 32);
  assert(clears == saved && resets == 2);
  budget = 1;
  draw_enemy_labels(labels+1, 1, NULL, area, 360, 32);
  assert(clears == saved+1 && resets == 3);
  budget = 100000;
  /* Exercise viewport offsets, smallest and fractional scales. */
  for (int scale=1; scale<=96; scale++) {
    nvn_rect_t inset = {10, 10, 620, 340};
    draw_enemy_labels(labels+1, 2, NULL, inset, 360, scale);
  }
  puts("enemy labels: production font/raster, clipping, scales and command-budget checks passed");
}
'''
    path = out / "render_enemy_health.c"
    path.write_text(harness, encoding="utf-8")
    exe = out / ("render_enemy_health.exe" if os.name == "nt" else "render_enemy_health")
    env = os.environ.copy()
    if Path(args.cc).is_absolute():
        env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    for command in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Isrc/ootgz", str(path), "src/ootgz/font.c", "-o", str(exe)],
                    [str(exe), str(out / "enemy_health.ppm")]):
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, **flags)
        print(result.stdout, end="")
        print(result.stderr, end="")
        result.check_returncode()


if __name__ == "__main__":
    main()
