idf_component_register(
    SRCS "main.c" "wifi_fileman.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_wifi esp_http_server nvs_flash esp_partition esp_ota_ops esp_vfs_fat sdmmc
)
