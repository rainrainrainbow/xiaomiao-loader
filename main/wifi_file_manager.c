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

/* ── HTML Templates ────────────────────────────────────────────────────── */

static const char *HTML_HEAD = 
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Xiaomiao Loader - File Manager</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:0;padding:16px;background:#F6D34A;color:#1B1713;}"
    "h1{font-size:18px;margin:0 0 8px 0;padding:8px;background:#5C4220;color:#FFF3B0;text-align:center;border-radius:4px;}"
    ".nav{text-align:center;margin:8px 0;}"
    ".nav a{color:#5C4220;text-decoration:none;padding:6px 12px;border:1px solid #5C4220;border-radius:4px;margin:0 4px;display:inline-block;font-size:14px;}"
    ".nav a:hover{background:#5C4220;color:#FFF3B0;}"
    "table{width:100%;border-collapse:collapse;background:#FFF3B0;border-radius:4px;overflow:hidden;}"
    "th,td{padding:8px 6px;text-align:left;border-bottom:1px solid #ddd;font-size:13px;}"
    "th{background:#5C4220;color:#FFF3B0;}"
    "tr:hover{background:#F6D34A;}"
    "a{color:#5C4220;text-decoration:none;}"
    "a:hover{text-decoration:underline;}"
    ".folder{font-weight:bold;}"
    ".file{padding-left:4px;}"
    ".size{text-align:right;color:#888;font-size:12px;}"
    ".actions a{color:#E64B3C;margin:0 4px;font-size:12px;}"
    ".upload-box{margin:12px 0;padding:12px;background:#FFF3B0;border-radius:4px;}"
    ".upload-box h3{margin:0 0 8px 0;font-size:14px;}"
    "input[type=file]{font-size:13px;}"
    "input[type=submit]{background:#5C4220;color:#FFF3B0;border:none;padding:6px 16px;border-radius:4px;cursor:pointer;}"
    "input[type=submit]:hover{background:#1B1713;}"
    ".info{font-size:12px;color:#888;margin:8px 0;text-align:center;}"
    ".mkdir-box{margin:12px 0;padding:12px;background:#FFF3B0;border-radius:4px;}"
    ".mkdir-box input[type=text]{padding:4px;width:150px;}"
    ".sd-info{font-size:12px;margin:8px 0;padding:8px;background:#FFF3B0;border-radius:4px;}"
    "td .del-btn{color:#E64B3C;font-size:12px;padding:2px 6px;border:1px solid #E64B3C;border-radius:3px;}"
    "td .del-btn:hover{background:#E64B3C;color:#FFF3B0;}"
    "</style></head><body>";

static const char *HTML_FOOT = 
    "<div class='info'>Xiaomiao ROM Loader - WiFi File Manager</div>"
    "</body></html>";

/* ── Utility ───────────────────────────────────────────────────────────── */

static void format_size_str(char *buf, size_t bufsz, size_t bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, bufsz, "%.1f KB", (double)bytes / 1024);
    else
        snprintf(buf, bufsz, "%zu B", bytes);
}

static char *url_decode(const char *src)
{
    size_t len = strlen(src);
    char *dst = calloc(len + 1, 1);
    if (!dst) return NULL;
    char *p = dst;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            unsigned int hi, lo;
            sscanf(&src[i+1], "%1x%1x", &hi, &lo);
            *p++ = (char)(hi * 16 + lo);
            i += 2;
        } else if (src[i] == '+') {
            *p++ = ' ';
        } else {
            *p++ = src[i];
        }
    }
    *p = '\0';
    return dst;
}

/* Sanitize path: ensure it's within SD_BASE_PATH */
static char *sanitize_path(const char *path)
{
    if (!path || path[0] != '/') {
        return strdup(SD_BASE_PATH);
    }
    size_t len = strlen(path) + strlen(SD_BASE_PATH) + 1;
    char *full = malloc(len);
    if (!full) return strdup(SD_BASE_PATH);
    snprintf(full, len, "%s%s", SD_BASE_PATH, path);
    /* Prevent directory traversal */
    char *real = realpath(full, NULL);
    if (real) {
        if (strncmp(real, SD_BASE_PATH, strlen(SD_BASE_PATH)) == 0) {
            free(full);
            return real;
        }
        free(real);
    }
    free(full);
    return strdup(SD_BASE_PATH);
}

/* ── HTTP Handlers ─────────────────────────────────────────────────────── */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char *dir_path = SD_BASE_PATH;
    char *query = NULL;

    /* Check if a directory is specified */
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        query = malloc(query_len + 1);
        if (query) {
            httpd_req_get_url_query_str(req, query, query_len + 1);
            char param[64];
            if (httpd_query_key_value(query, "dir", param, sizeof(param)) == ESP_OK) {
                char *decoded = url_decode(param);
                if (decoded) {
                    char *sanitized = sanitize_path(decoded);
                    if (sanitized) {
                        dir_path = sanitized;
                    }
                    free(decoded);
                }
            }
        }
    }

    /* Build the response */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>📁 Xiaomiao File Manager</h1>");

    /* SD card info */
    {
        char sd_info[256];
        /* Just show current path */
        snprintf(sd_info, sizeof(sd_info),
                 "<div class='sd-info'>📀 SD: %s</div>", dir_path);
        httpd_resp_sendstr_chunk(req, sd_info);
    }

    /* Navigation */
    httpd_resp_sendstr_chunk(req, "<div class='nav'>");
    httpd_resp_sendstr_chunk(req, "<a href='/'>🏠 Root</a>");
    httpd_resp_sendstr_chunk(req, "<a href='/'>🔄 Refresh</a>");
    httpd_resp_sendstr_chunk(req, "<a href='/'>⬅️ Up</a>");
    httpd_resp_sendstr_chunk(req, "</div>");

    /* Upload form */
    httpd_resp_sendstr_chunk(req, "<div class='upload-box'>");
    httpd_resp_sendstr_chunk(req, "<h3>📤 Upload File</h3>");
    httpd_resp_sendstr_chunk(req, "<form method='POST' action='/upload' enctype='multipart/form-data'>");
    httpd_resp_sendstr_chunk(req, "<input type='file' name='file'> ");
    httpd_resp_sendstr_chunk(req, "<input type='submit' value='Upload'>");
    httpd_resp_sendstr_chunk(req, "</form></div>");

    /* Mkdir form */
    httpd_resp_sendstr_chunk(req, "<div class='mkdir-box'>");
    httpd_resp_sendstr_chunk(req, "<h3>📂 Create Directory</h3>");
    httpd_resp_sendstr_chunk(req, "<form method='POST' action='/mkdir' style='display:inline'>");
    httpd_resp_sendstr_chunk(req, "<input type='text' name='name' placeholder='dir name'> ");
    httpd_resp_sendstr_chunk(req, "<input type='submit' value='Create'>");
    httpd_resp_sendstr_chunk(req, "</form></div>");

    /* File listing */
    httpd_resp_sendstr_chunk(req, "<table><tr><th>Name</th><th>Size</th><th>Actions</th></tr>");

    DIR *dir = opendir(dir_path);
    if (!dir) {
        httpd_resp_sendstr_chunk(req, "<tr><td colspan='3'>Cannot open directory</td></tr>");
    } else {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

            struct stat st;
            bool is_dir = false;
            if (stat(full_path, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
            }

            char size_str[32] = "-";
            if (!is_dir) {
                format_size_str(size_str, sizeof(size_str), st.st_size);
            }

            /* Relative path for links */
            const char *rel = ent->d_name;

            httpd_resp_sendstr_chunk(req, "<tr>");
            if (is_dir) {
                char buf[1024];
                snprintf(buf, sizeof(buf),
                         "<td class='folder'>📁 <a href='/?dir=%s'>%s/</a></td>",
                         rel, ent->d_name);
                httpd_resp_sendstr_chunk(req, buf);
                httpd_resp_sendstr_chunk(req, "<td class='size'>-</td>");
            } else {
                char buf[1024];
                snprintf(buf, sizeof(buf),
                         "<td class='file'>📄 <a href='/download?file=%s'>%s</a></td>",
                         rel, ent->d_name);
                httpd_resp_sendstr_chunk(req, buf);
                char sz[64];
                snprintf(sz, sizeof(sz), "<td class='size'>%s</td>", size_str);
                httpd_resp_sendstr_chunk(req, sz);
            }
            /* Delete action */
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "<td class='actions'><a href='/delete?file=%s' class='del-btn' onclick=\"return confirm('Delete %s?')\">🗑</a></td>",
                         rel, ent->d_name);
                httpd_resp_sendstr_chunk(req, buf);
            }
            httpd_resp_sendstr_chunk(req, "</tr>");
        }
        closedir(dir);
    }

    if (query) free(query);

    httpd_resp_sendstr_chunk(req, "</table>");
    httpd_resp_sendstr_chunk(req, HTML_FOOT);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    char file_param[256];
    if (httpd_req_get_url_query_str(req, file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
        return ESP_FAIL;
    }

    char file_val[64];
    if (httpd_query_key_value(file_param, "file", file_val, sizeof(file_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file");
        return ESP_FAIL;
    }

    char *decoded = url_decode(file_val);
    if (!decoded) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Decode failed");
        return ESP_FAIL;
    }

    char *full_path = sanitize_path(decoded);
    free(decoded);

    struct stat st;
    if (!full_path || stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (full_path) free(full_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        free(full_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    /* Set content type based on extension */
    const char *ext = strrchr(full_path, '.');
    if (ext) {
        if (strcasecmp(ext, ".bin") == 0 || strcasecmp(ext, ".img") == 0)
            httpd_resp_set_type(req, "application/octet-stream");
        else if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0)
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
        else if (strcasecmp(ext, ".html") == 0)
            httpd_resp_set_type(req, "text/html; charset=utf-8");
        else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
            httpd_resp_set_type(req, "image/jpeg");
        else if (strcasecmp(ext, ".png") == 0)
            httpd_resp_set_type(req, "image/png");
        else if (strcasecmp(ext, ".gif") == 0)
            httpd_resp_set_type(req, "image/gif");
        else
            httpd_resp_set_type(req, "application/octet-stream");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    /* Set Content-Disposition for download */
    char disp[512];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", strrchr(full_path, '/') + 1);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    /* Stream the file */
    char buf[1024];
    size_t read;
    while ((read = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    free(full_path);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Simple multipart upload handler */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    /* Get content length */
    size_t total_len = req->content_len;
    if (total_len > MAX_UPLOAD_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large");
        return ESP_FAIL;
    }

    /* Read the full body into a buffer */
    char *body = malloc(total_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t received = 0;
    int ret;
    while (received < total_len) {
        ret = httpd_req_recv(req, body + received, total_len - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv error");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[total_len] = '\0';

    /* Very simple multipart parser - find filename and content */
    char *filename = NULL;
    size_t content_len = 0;

    /* Find boundary */
    char *boundary_start = strstr(body, "boundary");
    if (!boundary_start) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary");
        return ESP_FAIL;
    }

    /* Find filename */
    char *fn_start = strstr(body, "filename=\"");
    if (fn_start) {
        fn_start += 10;
        char *fn_end = strchr(fn_start, '"');
        if (fn_end) {
            filename = strndup(fn_start, fn_end - fn_start);
        }
    }

    if (!filename) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_FAIL;
    }

    /* Find content start (after the double CRLF) */
    char *content = strstr(body, "\r\n\r\n");
    if (content) {
        content += 4;
        content_len = total_len - (content - body);
        /* Remove trailing boundary markers */
        char *trailer = strstr(content, "\r\n--");
        if (trailer) {
            content_len = trailer - content;
        }
    }

    /* Save to SD */
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_BASE_PATH, filename);

    FILE *f = fopen(full_path, "wb");
    if (!f) {
        free(body);
        free(filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write");
        return ESP_FAIL;
    }

    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);

    ESP_LOGI(TAG, "Uploaded: %s (%zu bytes)", filename, written);

    free(body);
    free(filename);

    /* Redirect back to root */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

static esp_err_t delete_get_handler(httpd_req_t *req)
{
    char file_param[256];
    if (httpd_req_get_url_query_str(req, file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing param");
        return ESP_FAIL;
    }

    char file_val[64];
    if (httpd_query_key_value(file_param, "file", file_val, sizeof(file_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file");
        return ESP_FAIL;
    }

    char *decoded = url_decode(file_val);
    if (!decoded) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Decode failed");
        return ESP_FAIL;
    }

    char *full_path = sanitize_path(decoded);
    free(decoded);

    if (!full_path) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Path error");
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        free(full_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    int result;
    if (S_ISDIR(st.st_mode)) {
        result = rmdir(full_path);
    } else {
        result = unlink(full_path);
    }
    free(full_path);

    if (result != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

static esp_err_t mkdir_post_handler(httpd_req_t *req)
{
    char buf[256];
    size_t len = MIN(req->content_len, sizeof(buf) - 1);
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char name_val[64] = {0};
    if (httpd_query_key_value(buf, "name", name_val, sizeof(name_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    char *decoded = url_decode(name_val);
    if (!decoded) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Decode failed");
        return ESP_FAIL;
    }

    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_BASE_PATH, decoded);
    free(decoded);

    mkdir(full_path, 0755);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

/* ── WiFi AP & Server ────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected: " MACSTR, MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station disconnected: " MACSTR, MAC2STR(event->mac));
    }
}

static void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .channel = WIFI_AP_CHANNEL,
        },
    };
    if (strlen(WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started. SSID: '%s' Pass: '%s'", WIFI_AP_SSID, WIFI_AP_PASS);
}

static httpd_uri_t s_uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL,
};

static httpd_uri_t s_uri_download = {
    .uri       = "/download",
    .method    = HTTP_GET,
    .handler   = download_get_handler,
    .user_ctx  = NULL,
};

static httpd_uri_t s_uri_upload = {
    .uri       = "/upload",
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx  = NULL,
};

static httpd_uri_t s_uri_delete = {
    .uri       = "/delete",
    .method    = HTTP_GET,
    .handler   = delete_get_handler,
    .user_ctx  = NULL,
};

static httpd_uri_t s_uri_mkdir = {
    .uri       = "/mkdir",
    .method    = HTTP_POST,
    .handler   = mkdir_post_handler,
    .user_ctx  = NULL,
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.server_port = 80;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &s_uri_root);
        httpd_register_uri_handler(server, &s_uri_download);
        httpd_register_uri_handler(server, &s_uri_upload);
        httpd_register_uri_handler(server, &s_uri_delete);
        httpd_register_uri_handler(server, &s_uri_mkdir);
        return server;
    }
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t wifi_file_manager_start(void)
{
    if (s_wifi_running) {
        return ESP_OK;
    }

    /* Initialize WiFi */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);

    wifi_init_ap();

    /* Get AP IP */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(netif, &ip_info);
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(s_ip_addr, sizeof(s_ip_addr), "192.168.4.1");
    }

    /* Start HTTP server */
    s_server = start_webserver();
    if (!s_server) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        esp_wifi_stop();
        return ESP_FAIL;
    }

    s_wifi_running = true;
    ESP_LOGI(TAG, "WiFi File Manager started at http://%s/", s_ip_addr);
    ESP_LOGI(TAG, "SSID: %s  Password: %s", WIFI_AP_SSID, WIFI_AP_PASS);
    return ESP_OK;
}

void wifi_file_manager_stop(void)
{
    if (!s_wifi_running) return;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_running = false;
    ESP_LOGI(TAG, "WiFi File Manager stopped");
}

bool wifi_file_manager_is_running(void)
{
    return s_wifi_running;
}

const char *wifi_file_manager_get_ip(void)
{
    return s_ip_addr;
}