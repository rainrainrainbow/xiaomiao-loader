/*
 * WiFi AP + HTTP File Manager implementation
 * Provides web-based file management for SD card
 */
#include "wifi_fileman.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_fm";
static httpd_handle_t s_server = NULL;
static bool s_running = false;

#define AP_SSID     "Xiaomiao-Loader"
#define AP_PASS     "12345678"
#define AP_IP       "192.168.4.1"
#define SD_ROOT     "/sdcard"

/* HTML page header */
static const char HTML_HEAD[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Xiaomiao File Manager</title>"
    "<style>"
    "body{font-family:sans-serif;margin:0;padding:10px;background:#1a1a2e;color:#eee}"
    ".header{background:#16213e;padding:15px;border-radius:8px;margin-bottom:15px}"
    ".header h1{margin:0;color:#e94560;font-size:1.5em}"
    ".path{color:#0f3460;background:#16213e;padding:8px;border-radius:4px;margin-bottom:10px;word-break:break-all}"
    ".file-list{list-style:none;padding:0}"
    ".file-item{background:#16213e;margin:5px 0;padding:12px;border-radius:6px;display:flex;align-items:center;justify-content:space-between}"
    ".file-item:hover{background:#1f4068}"
    ".file-name{flex:1;word-break:break-all}"
    ".file-name a{color:#eee;text-decoration:none}"
    ".file-name a:hover{color:#e94560}"
    ".file-size{color:#888;font-size:0.85em;margin-left:10px}"
    ".btn{background:#e94560;color:#fff;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;font-size:0.85em}"
    ".btn:hover{background:#c73e54}"
    ".btn-del{background:#533483}"
    ".btn-del:hover{background:#6d44a0}"
    ".upload-area{background:#16213e;padding:15px;border-radius:8px;margin-top:15px}"
    "input[type=file]{color:#eee}"
    ".icon{margin-right:8px;font-size:1.2em}"
    ".folder{color:#ffd700}"
    ".file{color:#87ceeb}"
    "</style></head><body>"
    "<div class='header'><h1>🐱 Xiaomiao File Manager</h1></div>";

static const char HTML_TAIL[] = "</body></html>";

/* Format file size */
static void format_size(char *buf, size_t bufsz, size_t bytes)
{
    if (bytes >= 1024*1024)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes/(1024*1024));
    else if (bytes >= 1024)
        snprintf(buf, bufsz, "%.1f KB", (double)bytes/1024);
    else
        snprintf(buf, bufsz, "%u B", (unsigned)bytes);
}

/* URL decode */
static void url_decode(char *dst, const char *src)
{
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* GET / - list files */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *query = req->uri;
    char path[256] = SD_ROOT;
    
    /* Parse ?path=xxx */
    const char *p = strstr(query, "?path=");
    if (p) {
        char decoded[256];
        url_decode(decoded, p + 6);
        snprintf(path, sizeof(path), "%s%s", SD_ROOT, decoded);
    }
    
    /* Build HTML */
    char html[4096];
    int len = snprintf(html, sizeof(html), "%s", HTML_HEAD);
    
    /* Path breadcrumb */
    len += snprintf(html+len, sizeof(html)-len, 
        "<div class='path'>📁 %s</div>", path + strlen(SD_ROOT));
    
    /* File list */
    len += snprintf(html+len, sizeof(html)-len, "<ul class='file-list'>");
    
    /* Parent directory link */
    if (strlen(path) > strlen(SD_ROOT)) {
        char parent[256];
        strncpy(parent, path, sizeof(parent));
        char *last = strrchr(parent, '/');
        if (last && last != path + strlen(SD_ROOT)) *last = '\0';
        else strcpy(parent, SD_ROOT);
        len += snprintf(html+len, sizeof(html)-len,
            "<li class='file-item'><div class='file-name'><span class='icon folder'>📁</span>"
            "<a href='/?path=%s'>..</a></div></li>", parent + strlen(SD_ROOT));
    }
    
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            
            char fullpath[300];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
            struct stat st;
            stat(fullpath, &st);
            
            bool is_dir = S_ISDIR(st.st_mode);
            char sizebuf[32];
            format_size(sizebuf, sizeof(sizebuf), st.st_size);
            
            if (is_dir) {
                char subpath[256];
                snprintf(subpath, sizeof(subpath), "%s/%s", 
                    strlen(path) > strlen(SD_ROOT) ? path + strlen(SD_ROOT) : "", ent->d_name);
                len += snprintf(html+len, sizeof(html)-len,
                    "<li class='file-item'><div class='file-name'>"
                    "<span class='icon folder'>📁</span>"
                    "<a href='/?path=%s'>%s</a></div></li>",
                    subpath, ent->d_name);
            } else {
                len += snprintf(html+len, sizeof(html)-len,
                    "<li class='file-item'>"
                    "<div class='file-name'><span class='icon file'>📄</span>%s</div>"
                    "<span class='file-size'>%s</span>"
                    "<a href='/download?file=%s/%s' class='btn'>⬇</a>"
                    "<form action='/delete' method='post' style='display:inline;margin-left:5px'>"
                    "<input type='hidden' name='file' value='%s/%s'>"
                    "<button type='submit' class='btn btn-del'>✕</button></form>"
                    "</li>",
                    ent->d_name, sizebuf,
                    path + strlen(SD_ROOT), ent->d_name,
                    path + strlen(SD_ROOT), ent->d_name);
            }
            
            if (len > sizeof(html) - 512) break;  /* prevent overflow */
        }
        closedir(dir);
    } else {
        len += snprintf(html+len, sizeof(html)-len, "<li class='file-item'>Cannot open directory</li>");
    }
    
    len += snprintf(html+len, sizeof(html)-len, "</ul>");
    
    /* Upload form */
    len += snprintf(html+len, sizeof(html)-len,
        "<div class='upload-area'>"
        "<h3>📤 Upload File</h3>"
        "<form action='/upload' method='post' enctype='multipart/form-data'>"
        "<input type='hidden' name='path' value='%s'>"
        "<input type='file' name='file'>"
        "<button type='submit' class='btn'>Upload</button>"
        "</form></div>",
        strlen(path) > strlen(SD_ROOT) ? path + strlen(SD_ROOT) : "");
    
    len += snprintf(html+len, sizeof(html)-len, "%s", HTML_TAIL);
    
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req, html, len);
    return ESP_OK;
}

/* GET /download - download file */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    const char *query = req->uri;
    const char *p = strstr(query, "?file=");
    if (!p) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    
    char path[256];
    char decoded[256];
    url_decode(decoded, p + 6);
    snprintf(path, sizeof(path), "%s%s", SD_ROOT, decoded);
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    
    /* Get filename from path */
    const char *fname = strrchr(path, '/') + 1;
    char disp[128];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Content-Type", "application/octet-stream");
    
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /delete - delete file */
static esp_err_t delete_post_handler(httpd_req_t *req)
{
    char buf[256];
    int received = 0;
    int remaining = req->content_len;
    
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) break;
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';
    
    /* Parse file=xxx */
    char *p = strstr(buf, "file=");
    if (!p) {
        httpd_resp_send_400(req);
        return ESP_OK;
    }
    
    char decoded[256];
    url_decode(decoded, p + 5);
    
    char path[256];
    snprintf(path, sizeof(path), "%s%s", SD_ROOT, decoded);
    
    ESP_LOGI(TAG, "Delete: %s", path);
    int err = remove(path);
    
    /* Redirect back */
    char redirect[128];
    char *last = strrchr(decoded, '/');
    if (last) *last = '\0';
    snprintf(redirect, sizeof(redirect), "/?path=%s", decoded);
    httpd_resp_set_hdr(req, "Location", redirect);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /upload - upload file (simplified, small files only) */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    /* For simplicity, reject large uploads */
    if (req->content_len > 512*1024) {
        httpd_resp_send_413(req);
        return ESP_OK;
    }
    
    /* Allocate buffer */
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) break;
        received += ret;
    }
    buf[received] = '\0';
    
    /* Parse multipart - simplified */
    /* Find filename */
    char *fname_start = strstr(buf, "filename=\"");
    if (!fname_start) {
        free(buf);
        httpd_resp_send_400(req);
        return ESP_OK;
    }
    fname_start += 10;
    char *fname_end = strchr(fname_start, '"');
    if (!fname_end) {
        free(buf);
        httpd_resp_send_400(req);
        return ESP_OK;
    }
    *fname_end = '\0';
    
    /* Find path */
    char path_buf[256] = SD_ROOT;
    char *path_start = strstr(buf, "name=\"path\"");
    if (path_start) {
        char *val = strchr(path_start, '\n');
        if (val) {
            val++;
            if (*val == '\r') val++;
            if (*val == '\n') val++;
            char *end = strstr(val, "\r\n--");
            if (end) {
                *end = '\0';
                if (strlen(val) > 0 && val[0] != '-') {
                    snprintf(path_buf, sizeof(path_buf), "%s/%s", SD_ROOT, val);
                }
            }
        }
    }
    
    /* Find file content (after double newline) */
    char *content = strstr(fname_end + 1, "\r\n\r\n");
    if (!content) {
        free(buf);
        httpd_resp_send_400(req);
        return ESP_OK;
    }
    content += 4;
    
    /* Find end (before boundary) */
    char *content_end = strstr(content, "\r\n--");
    if (content_end) *content_end = '\0';
    
    /* Build full path */
    char fullpath[300];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", path_buf, fname_start);
    
    ESP_LOGI(TAG, "Upload: %s (%d bytes)", fullpath, (int)strlen(content));
    
    /* Write file */
    FILE *f = fopen(fullpath, "wb");
    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
    }
    
    free(buf);
    
    /* Redirect back */
    char redirect[128];
    snprintf(redirect, sizeof(redirect), "/?path=%s", 
        strlen(path_buf) > strlen(SD_ROOT) ? path_buf + strlen(SD_ROOT) : "");
    httpd_resp_set_hdr(req, "Location", redirect);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi AP started: SSID=%s PASS=%s", AP_SSID, AP_PASS);
}

bool wifi_fileman_start(void)
{
    if (s_running) return true;
    
    start_wifi_ap();
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }
    
    /* Register handlers */
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t download_uri = {.uri = "/download", .method = HTTP_GET, .handler = download_get_handler};
    httpd_uri_t delete_uri = {.uri = "/delete", .method = HTTP_POST, .handler = delete_post_handler};
    httpd_uri_t upload_uri = {.uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler};
    
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &download_uri);
    httpd_register_uri_handler(s_server, &delete_uri);
    httpd_register_uri_handler(s_server, &upload_uri);
    
    s_running = true;
    ESP_LOGI(TAG, "HTTP server started at http://%s", AP_IP);
    return true;
}

void wifi_fileman_stop(void)
{
    if (!s_running) return;
    
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    s_running = false;
    ESP_LOGI(TAG, "WiFi file manager stopped");
}

bool wifi_fileman_is_running(void)
{
    return s_running;
}
