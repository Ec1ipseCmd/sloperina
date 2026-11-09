#ifndef ENEMY_HEALTH_H
#define ENEMY_HEALTH_H
#include <stdbool.h>
#include <stdint.h>
#define ENEMY_LABEL_MAX 32
#define ENEMY_LABEL_TEXT 16
typedef struct { uint16_t x, y; char text[ENEMY_LABEL_TEXT]; } enemy_label_t;
void enemy_health_frame(bool visible);
/* Copies a bounded snapshot; renderer never accesses game objects. */
int enemy_health_copy(enemy_label_t out[ENEMY_LABEL_MAX]);
#endif
