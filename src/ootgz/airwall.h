/* Air walls (port of airwall.py): which walls are in the current map, whether Link is inside one,
 * and their outlines in the shared visibility-tested world overlay. */
#ifndef AIRWALL_H
#define AIRWALL_H

#include <stdbool.h>
#include <stdint.h>

extern bool airwalls_shown; /* draw them */
extern bool airwalls_all;   /* also the kinds that don't stop Link (enemies, sight, objects) */

void airwall_frame(void); /* game thread; publishes the shared world view */
void airwall_summary(char *buf, int len);     /* "12 walls, 3 within 60" */
void airwall_link_text(char *buf, int len);   /* the air wall watch: depth inside, or "outside" */
#endif
