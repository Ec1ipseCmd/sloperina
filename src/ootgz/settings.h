/* Settings kept on the SD card (sd:/ootgz/settings.ini), loaded on the first frame and saved
 * when they change (with the menu closed). Never touches the game's save data. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

void settings_load(void);            /* first frame: mount the SD card, read the file */
void settings_frame(bool menu_open);
void settings_log_frame(void); /* writes the log file (sd:/sloperinaGZ/log.txt) */ /* save if anything changed */

#endif
