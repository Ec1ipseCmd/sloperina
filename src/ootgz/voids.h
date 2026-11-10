/* Player void sensors: dynamic inspection and optional message suppression. */
#ifndef VOIDS_H
#define VOIDS_H
#include <stdbool.h>
#include <stdint.h>
#include "link.h"

enum { VOIDS_NORMAL, VOIDS_WHILE_LEVITATING, VOIDS_DISABLED, VOIDS_MODE_COUNT };
extern bool voids_shown;
extern int voids_mode; /* session only; normal on every launch */
extern const char *const voids_mode_names[VOIDS_MODE_COUNT];
void voids_frame(void);
void voids_each_box(void (*draw)(const vec3f_t corners[8], bool active, void *ctx), void *ctx);
void voids_summary(char *buf, int len);
typedef struct { vec3f_t point; float distance; bool inside; } void_nearest_t;
/* Distance to an identified active sensor's boundary, not a safe clearance. */
bool voids_nearest(vec3f_t position, void_nearest_t *out);
/* Mid-function classifier hook; returns the native continuation address. */
uintptr_t voids_transition_target(uintptr_t player, uint32_t abyss_type);
void void_state_entry(void);

/* IPS message hooks; all unmatched messages go to the original game function. */
int32_t voids_abyss_message(uintptr_t sender, uintptr_t arg, const uint32_t *target, const uint32_t *payload);
int32_t voids_sand_message(uintptr_t sender, uintptr_t arg, const uint32_t *target, const uint32_t *payload);
#endif
