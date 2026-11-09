/* Levitate: L3 starts floating (Link rises), L3 again drops him (fall damage on landing is given
 * back), R3 held keeps the height, the left stick moves him. While it's enabled the game doesn't see
 * L3 (nor R3 while Link floats). Also the 2x speed cheat (CHEAT_SPEED), which moves him the same way. */
#ifndef LEVITATE_H
#define LEVITATE_H

#include <stdbool.h>

extern bool levitate_enabled;
extern bool levitate_through_walls;
extern int levitate_speed;        /* rise, units per second; 0 holds height without R3 */
extern int levitate_move_speed;   /* left stick, units per second */

void levitate_frame(bool menu_open);
const char *levitate_state(void); /* for watches/menus */

#endif
