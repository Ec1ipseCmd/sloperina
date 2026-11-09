#ifndef WORLD_VIEW_H
#define WORLD_VIEW_H
#include <stdbool.h>
typedef struct {
  float pos[3], forward[3], right[3], up[3];
  float tan_x, tan_y; /* tangents of half the horizontal and vertical fov */
} cam_t;
bool read_camera(cam_t *cam);
void to_view(const cam_t *cam, const float x[3], float v[3]);
void to_screen(const cam_t *cam, const float v[3], float *sx, float *sy);
#endif
