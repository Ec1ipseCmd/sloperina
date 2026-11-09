/* Settings kept on the SD card (sd:/ootgz/settings.ini), loaded on the first frame and saved
 * when they change (with the menu closed). Never touches the game's save data. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

void settings_load(void);            /* first frame: mount the SD card, read the file */
void settings_frame(bool menu_open); /* save if anything changed */

#endif
