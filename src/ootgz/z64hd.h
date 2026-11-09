/* Ocarina of Time (Switch 2): game addresses and calls, selected by Z64HD_VERSION.
 * Like gz's z64.h, but the game is relocatable, so everything is main + offset. */
#ifndef Z64HD_H
#define Z64HD_H

#include <stdbool.h>
#include <stdint.h>

#define Z64HD_V131072 1

#ifndef Z64HD_VERSION
#define Z64HD_VERSION Z64HD_V131072
#endif

#if Z64HD_VERSION == Z64HD_V131072
#include "z64hd_v131072.h"
#else
#error unknown Z64HD_VERSION
#endif

/* main's load address, set by ootgz_module_init */
extern uintptr_t z64hd_main;

#define MAIN(off)          (z64hd_main + (uintptr_t)(off))
#define MAIN_FN(type, off) ((type)MAIN(off))

#endif
