/*
 * WiFi AP + HTTP File Manager for Xiaomiao Loader
 */
#ifndef WIFI_FILEMAN_H
#define WIFI_FILEMAN_H

#include "esp_err.h"

/* Start WiFi AP and HTTP file server */
esp_err_t wifi_fileman_start(void);

/* Stop WiFi and HTTP server */
void wifi_fileman_stop(void);

#endif
