#ifndef VOID_VIEW_H
#define VOID_VIEW_H
#include "voids.h"
#include "world_view.h"
#define VOID_VIEW_WIDTH 320
#define VOID_VIEW_HEIGHT 180
#define VOID_VIEW_PIXELS (VOID_VIEW_WIDTH * VOID_VIEW_HEIGHT)
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
