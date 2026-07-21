/*
 * Tools Screen for Xiaomiao Loader
 *
 * Provides access to:
 * - WiFi File Manager (start/stop)
 * - Local File Manager (on-device file browser)
 * - Back to ROM Loader
 */

#ifndef TOOLS_SCREEN_H
#define TOOLS_SCREEN_H

#include "lvgl.h"

void tools_screen_show(lv_group_t *group);
void tools_screen_update_wifi_status(void);
void tools_screen_set_return_cb(void (*cb)(lv_group_t *group));

#endif /* TOOLS_SCREEN_H */
