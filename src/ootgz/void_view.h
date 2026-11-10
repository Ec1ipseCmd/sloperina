#ifndef VOID_VIEW_H
#define VOID_VIEW_H
#include "voids.h"
#include "world_view.h"
#define VOID_VIEW_WIDTH 320
#define VOID_VIEW_HEIGHT 180
#define VOID_VIEW_PIXELS (VOID_VIEW_WIDTH * VOID_VIEW_HEIGHT)
#define VIEW_LINES 16384
/* Zero is transparent; every occupied pixel is blended exactly once. */
enum { VIEW_VOID=1, VIEW_BLOCKS, VIEW_INSIDE, VIEW_OTHER,
       VIEW_FLOOR, VIEW_WALL, VIEW_CEILING, VIEW_TRIGGER, VIEW_COLORS };
typedef struct { float x0,y0,x1,y1;uint8_t color; } view_line_t;
typedef struct {
  uint8_t mask[VOID_VIEW_PIXELS];
  view_line_t lines[VIEW_LINES];
  unsigned count;
  float link[4]; /* conservative projected actor rectangle; no overlay over Link */
  bool hide_link;
} view_image_t;
void void_view_line(vec3f_t a,vec3f_t b,uint8_t color);
bool void_view_link(vec3f_t feet,vec3f_t head);
bool void_view_snapshot(view_image_t *out);
void void_view_band(const view_image_t *view,int width,int height,int y,int rows,uint8_t *out);
uint32_t void_view_color(uint32_t background,uint8_t color);
void void_view_begin(const cam_t *cam,float range);
void void_view_occluder(const vec3f_t v[3],int kind,void *ctx);
void void_view_box(const vec3f_t corners[8],bool active,void *ctx);
void void_view_finish(void);
void void_view_clear(void);
bool void_view_nearest(vec3f_t player,void_nearest_t *out);
/* CPU snapshot only; the renderer never retains a pointer to the game thread. */
bool void_view_copy(uint8_t out[VOID_VIEW_PIXELS]);
/* Exact 50% tint, preserving the framebuffer's alpha byte. */
uint32_t void_view_blend(uint32_t background);
#endif
