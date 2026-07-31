idf_component_register(
    SRCS "main.c" "wifi_fileman.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_wifi esp_http_server nvs_flash app_update esp_vfs_fat sdmmc spi_flash
)
