/*
 * WiFi File Manager - Web-based file management for SD card
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_fileman.h"

static const char *TAG = "wifi_fileman";

#define WIFI_SSID "Xiaomiao-Loader"
#define WIFI_PASS "12345678"
#define SD_ROOT "/sdcard"

static httpd_handle_t server = NULL;

/* Helper: send HTTP error response (renamed to avoid conflict with esp_http_server) */
static esp_err_t send_error(httpd_req_t *req, const char *msg)
{
    char err_str[128];
    snprintf(err_str, sizeof(err_str), "{\"error\": \"%s\"}", msg ? msg : "Unknown error");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, err_str, strlen(err_str));
    return ESP_OK;
}

/* HTML template for file list */
static const char *html_header = 
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Xiaomiao File Manager</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#1a1a2e;color:#eee}"
    "h1{color:#e94560;text-align:center}"
    ".path{background:#16213e;padding:10px;border-radius:5px;margin:10px 0}"
    ".file-list{list-style:none;padding:0}"
    ".file-item{background:#0f3460;margin:5px 0;padding:10px;border-radius:5px;display:flex;align-items:center}"
    ".file-item:hover{background:#1a4a7a}"
    ".file-icon{margin-right:10px;font-size:20px}"
    ".file-name{flex:1}"
    ".file-size{color:#888;margin-right:10px}"
    ".btn{padding:5px 15px;border:none;border-radius:3px;cursor:pointer;color:white;text-decoration:none}"
    ".btn-download{background:#2ecc71}"
    ".btn-delete{background:#e74c3c}"
    ".upload-form{background:#16213e;padding:15px;border-radius:5px;margin-top:20px}"
    "input[type=file]{margin:10px 0}"
    "a{color:#3498db}"
    "</style></head><body>"
    "<h1>Xiaomiao File Manager</h1>";

static const char *html_footer = "</body></html>";

/* Format file size */
static void format_size(char *buf, size_t bufsize, size_t bytes)
{
    if (bytes < 1024) {
        snprintf(buf, bufsize, "%u B", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, bufsize, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, bufsize, "%.1f MB", bytes / (1024.0 * 1024.0));
    }
}

/* URL decode */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* GET / - List files */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char path[512];
    char query[256] = "";
    
    /* Get path from query string */
    const char *q = strchr(req->uri, '?');
    if (q && strncmp(q, "?path=", 6) == 0) {
        url_decode(query, q + 6, sizeof(query));
    }
    
    if (strlen(query) > 0) {
        snprintf(path, sizeof(path), "%.400s%.100s", SD_ROOT, query);
    } else {
        snprintf(path, sizeof(path), "%s", SD_ROOT);
    }
    
    /* Build HTML response */
    char html[4096];
    int len = 0;
    
    len += snprintf(html + len, sizeof(html) - len, "%s", html_header);
    len += snprintf(html + len, sizeof(html) - len, "<div class='path'>%s</div>", path);
    len += snprintf(html + len, sizeof(html) - len, "<ul class='file-list'>");
    
    /* Parent directory link */
    if (strlen(query) > 1) {
        char parent[256] = "";
        char *last_slash = strrchr(query, '/');
        if (last_slash) {
            size_t plen = last_slash - query;
            if (plen == 0) {
                snprintf(parent, sizeof(parent), "/");
            } else {
                snprintf(parent, sizeof(parent), "%.*s", (int)plen, query);
            }
        }
        len += snprintf(html + len, sizeof(html) - len, 
            "<li class='file-item'><span class='file-icon'>[..]</span>"
            "<span class='file-name'><a href='/?path=%s'>..</a></span></li>", parent);
    }
    
    /* List directory contents */
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            
            char fullpath[600];
            snprintf(fullpath, sizeof(fullpath), "%.400s/%.150s", path, ent->d_name);
            struct stat st;
            stat(fullpath, &st);
            
            char relpath[512];
            snprintf(relpath, sizeof(relpath), "%.400s/%.100s", 
                     strlen(query) > 0 ? query : "", ent->d_name);
            
            bool is_dir = S_ISDIR(st.st_mode);
            char size_str[32];
            format_size(size_str, sizeof(size_str), st.st_size);
            
            if (is_dir) {
                len += snprintf(html + len, sizeof(html) - len,
                    "<li class='file-item'>"
                    "<span class='file-icon'>[D]</span>"
                    "<span class='file-name'><a href='/?path=%s'>%s</a></span>"
                    "</li>", relpath, ent->d_name);
            } else {
                len += snprintf(html + len, sizeof(html) - len,
                    "<li class='file-item'>"
                    "<span class='file-icon'>[F]</span>"
                    "<span class='file-name'>%s</span>"
                    "<span class='file-size'>%s</span>"
                    "<a class='btn btn-download' href='/download?file=%s'>DL</a> "
                    "<a class='btn btn-delete' href='/delete?file=%s'>DEL</a>"
                    "</li>", ent->d_name, size_str, relpath, relpath);
            }
            
            if (len > (int)sizeof(html) - 512) break;
        }
        closedir(dir);
    }
    
    len += snprintf(html + len, sizeof(html) - len, "</ul>");
    
    /* Upload form */
    len += snprintf(html + len, sizeof(html) - len,
        "<div class='upload-form'>"
        "<h3>Upload File</h3>"
        "<form action='/upload' method='POST' enctype='multipart/form-data'>"
        "<input type='hidden' name='path' value='%.200s'>"
        "<input type='file' name='file'>"
        "<button type='submit' class='btn btn-download'>Upload</button>"
        "</form></div>", query);
    
    len += snprintf(html + len, sizeof(html) - len, "%s", html_footer);
    
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req, html, len);
    return ESP_OK;
}

/* GET /download - Download file */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char query[256] = "";
    char path[512];
    
    const char *q = strchr(req->uri, '?');
    if (!q || strncmp(q, "?file=", 6) != 0) {
        return send_error(req, "Missing file parameter");
    }
    
    url_decode(query, q + 6, sizeof(query));
    snprintf(path, sizeof(path), "%.400s%.100s", SD_ROOT, query);
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        return send_error(req, "File not found");
    }
    
    /* Get filename */
    const char *filename = strrchr(path, '/') + 1;
    char content_disp[128];
    snprintf(content_disp, sizeof(content_disp), "attachment; filename=\"%.80s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disp);
    httpd_resp_set_hdr(req, "Content-Type", "application/octet-stream");
    
    /* Stream file */
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    
    return ESP_OK;
}

/* GET /delete - Delete file */
static esp_err_t delete_get_handler(httpd_req_t *req)
{
    char query[256] = "";
    char path[512];
    
    const char *q = strchr(req->uri, '?');
    if (!q || strncmp(q, "?file=", 6) != 0) {
        return send_error(req, "Missing file parameter");
    }
    
    url_decode(query, q + 6, sizeof(query));
    snprintf(path, sizeof(path), "%.400s%.100s", SD_ROOT, query);
    
    if (remove(path) == 0) {
        /* Redirect back to parent directory */
        char redirect[300];
        char *last_slash = strrchr(query, '/');
        if (last_slash) {
            *last_slash = '\0';
            snprintf(redirect, sizeof(redirect), "/?path=%.200s", query);
        } else {
            snprintf(redirect, sizeof(redirect), "/");
        }
        httpd_resp_set_hdr(req, "Location", redirect);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_send(req, NULL, 0);
    } else {
        return send_error(req, "Failed to delete file");
    }
    
    return ESP_OK;
}

/* POST /upload - Upload file */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    int remaining = req->content_len;
    
    if (remaining > 512 * 1024) {
        return send_error(req, "File too large (max 512KB)");
    }
    
    char *content = malloc(remaining + 1);
    if (!content) {
        return send_error(req, "Out of memory");
    }
    
    char buf[1024];
    int received = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) break;
        memcpy(content + received, buf, ret);
        received += ret;
        remaining -= ret;
    }
    content[received] = '\0';
    
    /* Parse multipart form data (simplified) */
    char *path_start = strstr(content, "name=\"path\"");
    char *file_start = strstr(content, "name=\"file\"");
    char *filename_start = strstr(content, "filename=\"");
    
    if (!path_start || !file_start || !filename_start) {
        free(content);
        return send_error(req, "Invalid form data");
    }
    
    /* Extract path */
    char path_value[256] = "";
    char *p = strchr(path_start, '\n');
    if (p) {
        p++;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        char *end = strstr(p, "\r\n--");
        if (end) {
            size_t len = end - p;
            if (len < sizeof(path_value)) {
                strncpy(path_value, p, len);
            }
        }
    }
    
    /* Extract filename */
    char filename[128] = "";
    filename_start += 10;
    char *quote = strchr(filename_start, '"');
    if (quote) {
        size_t len = quote - filename_start;
        if (len < sizeof(filename)) {
            strncpy(filename, filename_start, len);
        }
    }
    
    /* Extract file content */
    char *content_start = strstr(file_start, "\r\n\r\n");
    if (!content_start) {
        free(content);
        return send_error(req, "Invalid file data");
    }
    content_start += 4;
    
    char *content_end = strstr(content_start, "\r\n--");
    if (!content_end) {
        free(content);
        return send_error(req, "Invalid file data");
    }
    
    /* Build full path */
    char fullpath[600];
    if (strlen(path_value) > 0) {
        snprintf(fullpath, sizeof(fullpath), "%.400s%.100s/%.100s", SD_ROOT, path_value, filename);
    } else {
        snprintf(fullpath, sizeof(fullpath), "%.400s/%.100s", SD_ROOT, filename);
    }
    
    /* Write file */
    FILE *f = fopen(fullpath, "wb");
    if (!f) {
        free(content);
        return send_error(req, "Cannot create file");
    }
    
    size_t content_len = content_end - content_start;
    fwrite(content_start, 1, content_len, f);
    fclose(f);
    free(content);
    
    /* Redirect back */
    char redirect[300];
    snprintf(redirect, sizeof(redirect), "/?path=%.200s", path_value);
    httpd_resp_set_hdr(req, "Location", redirect);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, NULL, 0);
    
    return ESP_OK;
}

/* Start WiFi AP and HTTP server */
esp_err_t wifi_fileman_start(void)
{
    /* Configure WiFi AP */
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Password=%s", WIFI_SSID, WIFI_PASS);
    
    /* Start HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    /* Register URI handlers */
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(server, &root_uri);
    
    httpd_uri_t download_uri = {
        .uri = "/download",
        .method = HTTP_GET,
        .handler = download_get_handler,
    };
    httpd_register_uri_handler(server, &download_uri);
    
    httpd_uri_t delete_uri = {
        .uri = "/delete",
        .method = HTTP_GET,
        .handler = delete_get_handler,
    };
    httpd_register_uri_handler(server, &delete_uri);
    
    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
    };
    httpd_register_uri_handler(server, &upload_uri);
    
    ESP_LOGI(TAG, "HTTP server started at http://192.168.4.1");
    return ESP_OK;
}

/* Stop WiFi and HTTP server */
void wifi_fileman_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi file manager stopped");
}
