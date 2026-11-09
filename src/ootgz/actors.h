/* Read-only access to the current actor list. No addresses survive a frame. */
#ifndef ACTORS_H
#define ACTORS_H
#include "link.h"
#include <stddef.h>
#include <stdint.h>
bool actors_each(void (*visit)(uintptr_t actor, const char *name, void *ctx), void *ctx);
bool actor_health(uintptr_t actor, int32_t *current, int32_t *maximum);
bool actor_label_position(uintptr_t actor, vec3f_t *out);
#endif
