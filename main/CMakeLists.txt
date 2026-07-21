idf_component_register(SRCS "main.c"
                            "wifi_file_manager.c"
                            "local_file_manager.c"
                            "tools_screen.c"
                       PRIV_REQUIRES esp_lcd
                                     esp_timer
                                     esp_driver_gpio
                                     esp_driver_spi
                                     esp_driver_sdspi
                                     esp_hw_support
                                     heap
                                     spi_flash
                                     fatfs
                                     sdmmc
                                     lvgl
                                     app_update
                                     nvs_flash
                                     esp_wifi
                                     esp_event
                                     esp_netif
                                     esp_http_server
                       INCLUDE_DIRS "")


target_link_libraries(${COMPONENT_LIB} PRIVATE m)