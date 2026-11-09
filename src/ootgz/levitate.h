/* Levitate: L3 toggles floating, X/B rise/descend, neither or R3 holds height;
 * left stick moves horizontally. Through-walls mode leases the controller's
 * collision bypass. The game does not see L3 while enabled, nor X/B/R3 while
 * floating. Also the 2x speed cheat (CHEAT_SPEED), using the physics record. */
#ifndef LEVITATE_H
#define LEVITATE_H

#include <stdbool.h>

extern bool levitate_enabled;
extern bool levitate_through_walls;
extern int levitate_speed;        /* X/B units/s; legacy 0 uses 4, idle always hovers */
extern int levitate_move_speed;   /* left stick, units per second */

void levitate_frame(bool menu_open);
bool levitate_active(void);
const char *levitate_state(void); /* for watches/menus */

#endif
