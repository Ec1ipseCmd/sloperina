/* debug > memory: gz's memory viewer (mem.c). Rows of 8 bytes as bytes, halfwords, words, floats or
 * pointers; an address to type in; places to jump to; editing; a watch from a row. */
#ifndef MEMVIEW_H
#define MEMVIEW_H

#include <stdint.h>

#include "menu.h"

extern menu_t memview_menu;

void memview_build(menu_t *watches_menu); /* a watch added from a row opens this menu */
/* gz's mem_open_watch: show addr with the view matching a memory watch type (MW_*) */
void memview_open(uintptr_t addr, int mw_type);

#endif
