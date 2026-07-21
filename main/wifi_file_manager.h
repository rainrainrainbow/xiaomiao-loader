/*
 * WiFi File Manager for Xiaomiao Loader
 *
 * ESP32 WiFi AP + HTTP server providing web-based file management
 * for the SD card. Accessible at http://192.168.4.1/
 */

#ifndef WIFI_FILE_MANAGER_H
#define WIFI_FILE_MANAGER_H

#include "esp_err.h"

esp_err_t wifi_file_manager_start(void);
void wifi_file_manager_stop(void);
bool wifi_file_manager_is_running(void);
const char *wifi_file_manager_get_ip(void);

#endif /* WIFI_FILE_MANAGER_H */
