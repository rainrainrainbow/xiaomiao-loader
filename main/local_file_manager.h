/*
 * Local File Manager for Xiaomiao Loader
 *
 * On-device file browser using LVGL UI.
 * Browse SD card, navigate directories, delete files.
 * Accessed via the ROM Loader menu.
 */

#ifndef LOCAL_FILE_MANAGER_H
#define LOCAL_FILE_MANAGER_H

#include "lvgl.h"

void local_fm_show(lv_group_t *group);

#endif /* LOCAL_FILE_MANAGER_H */
