idf_component_register(
    SRCS "main.c" "wifi_fileman.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_wifi esp_http_server nvs_flash app_update fatfs sdmmc spi_flash spiffs
)
