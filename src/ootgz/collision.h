#ifndef COLLISION_H
#define COLLISION_H
#include "collision_mesh.h"
extern bool collision_shown;
extern int collision_kinds; /* bit per COLLISION_* */
extern int collision_radius;
extern int collision_detail;
void collision_frame(void);
void collision_each_triangle(void (*draw)(const vec3f_t [3], int, void *), void *ctx);
void collision_summary(char *buf, int len);
/* Exact solid triangles for visibility, independent of display filters/grid.
 * Range shrinks conservatively if the bounded collection fills up. */
float collision_occlusion_range(vec3f_t camera);
void collision_each_occluder(void (*visit)(const vec3f_t [3],int,void *),void *ctx);
#endif
