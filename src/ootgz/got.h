/* Redirect a module's imports by rewriting its GOT entries (after rtld has linked it).
 * Used at module init on main: main's calls to e.g. nvnBootstrapLoader go through these
 * slots, so this works whatever order rtld searches modules in. */
#ifndef GOT_H
#define GOT_H

#include <stdint.h>

/* Address of `module`'s GOT slot for imported symbol `name` (via its MOD0 and dynamic
 * section), or 0. */
uintptr_t *got_slot(uintptr_t module, const char *name);

/* *slot = replacement; returns 0 on success, or a message. */
const char *got_hook(uintptr_t module, const char *name, void *replacement);

#endif
