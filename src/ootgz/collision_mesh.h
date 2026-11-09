/* Bounded, read-only decoder for collision bytes already loaded by the game. */
#ifndef COLLISION_MESH_H
#define COLLISION_MESH_H
#include "link.h"
#include <stddef.h>
enum { COLLISION_FLOOR, COLLISION_WALL, COLLISION_CEILING, COLLISION_TRIGGER, COLLISION_KINDS };
typedef struct {
  size_t skip, remaining, visited;
  unsigned nodes, unsupported;
  unsigned kinds;
  uintptr_t main;
  void (*triangle)(const vec3f_t [3], int kind, void *);
  void *ctx;
} collision_scan_t;
void collision_mesh_scan(const void *bytes, size_t size, collision_scan_t *scan);
#endif
