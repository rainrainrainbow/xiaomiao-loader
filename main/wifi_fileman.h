/*
 * WiFi AP + HTTP File Manager for Xiaomiao Loader
 * Provides web-based file management over WiFi
 */
#ifndef WIFI_FILEMAN_H
#define WIFI_FILEMAN_H

#include <stdbool.h>

/* Start WiFi AP and HTTP file server
 * AP SSID: Xiaomiao-Loader
 * AP URL:  http://192.168.4.1
 * Returns true on success
 */
bool wifi_fileman_start(void);

/* Stop WiFi and HTTP server */
void wifi_fileman_stop(void);

/* Check if file manager is running */
bool wifi_fileman_is_running(void);

#endif /* WIFI_FILEMAN_H */
