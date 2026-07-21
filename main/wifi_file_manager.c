/*
 * WiFi File Manager for Xiaomiao Loader
 *
 * ESP32 WiFi AP + HTTP server providing web-based file management
 * for the SD card. Accessible at http://192.168.4.1/
 *
 * Features:
 * - Browse SD card directory tree
 * - Upload files (multipart/form-data)
 * - Download files
 * - Delete files/directories
 * - Create directories
 * - File size & SD card info display
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "esp_mac.h"

#include "wifi_file_manager.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static const char *TAG = "wifi_fm";

static bool s_wifi_running = false;
static char s_ip_addr[16] = {0};
static httpd_handle_t s_server = NULL;

/* WiFi AP configuration */
#define WIFI_AP_SSID        "Xiaomiao-Loader"
#define WIFI_AP_PASS        "12345678"
#define WIFI_AP_MAX_CONN    4
#define WIFI_AP_CHANNEL     1

/* SD card base path */
#define SD_BASE_PATH        "/sdcard"

/* Max file size for upload (50MB) */
#define MAX_UPLOAD_SIZE     (50 * 1024 * 1024)
