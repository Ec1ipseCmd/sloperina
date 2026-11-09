/* gz's on-screen keyboard (osk.c): a text line and rows of keys, for naming things (watch labels). */
#ifndef OSK_H
#define OSK_H

#include <stdint.h>

#include "menu.h"

#define OSK_MAX 12 /* characters */

/* Open the keyboard over the current menu with `initial` in it. When it's finished (L on the text,
 * or "ok"), done(text, ctx) is called and the menu goes back; return / R + d-left cancels. */
void osk_open(const char *title, const char *initial, void (*done)(const char *text, intptr_t ctx), intptr_t ctx);
void osk_build(void);

#endif
