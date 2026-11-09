/* On-screen drawing through the game's own NVN (see nvn.c). */
#ifndef NVN_H
#define NVN_H

/* Replacement for main's nvnBootstrapLoader import (installed with got_hook at module init). */
void *nvn_bootstrap_hook(const char *name);

#endif
