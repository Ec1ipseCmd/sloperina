/* Shared by the module and the IPS generator: the module only installs its frame hook when
 * the patch in the game is the one it was built with. Bump when the stub or site changes. */
#ifndef OOTGZ_VERSION_H
#define OOTGZ_VERSION_H

#define OOTGZ_PATCH_VERSION 2
#define OOTGZ_MARKER        "OOTGZ\0\0\0"  /* 8 bytes at CAVE_START, then the u32 version */
#define OOTGZ_STUB_OFF      0x40           /* stub at CAVE_START + this */
#define OOTGZ_ABYSS_STUB_OFF 0x60
#define OOTGZ_SAND_STUB_OFF  0x80

#endif
