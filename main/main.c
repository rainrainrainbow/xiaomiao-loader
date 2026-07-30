/*
 * Xiaomiao ROM Loader - Lightweight Version
 * 
 * Features:
 * - ROM selection from SD card (/sdcard/boot/)
 * - Write to ota_0 partition
 * - WiFi file manager (web-based)
 * - Local file browser
 * - No LVGL dependency - uses custom lightweight UI
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_spi.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "ui/ui.h"
#include "wifi_fileman.h"

static const char *TAG = "loader";

/* Pin definitions */
#define PIN_NUM_LCD_CS      5
#define PIN_NUM_LCD_DC      4
#define PIN_NUM_LCD_CLK     18
#define PIN_NUM_LCD_MOSI    23
#define PIN_NUM_LCD_RST     -1
#define PIN_NUM_SD_CS       22

/* Button pins */
#define BTN_UP      32
#define BTN_DOWN    33
#define BTN_LEFT    25
#define BTN_RIGHT   26
#define BTN_A       27
#define BTN_B       12

/* Screen dimensions */
#define SCREEN_W    160
#define SCREEN_H    128

/* Colors */
#define COLOR_BG        0x0000  /* Black */
#define COLOR_FG        0xFFFF  /* White */
#define COLOR_SELECT    0x07E0  /* Green */
#define COLOR_TITLE_BG  0x001F  /* Blue */

/* ROM entry */
typedef struct {
    char name[64];
    char path[128];
    size_t size;
} rom_entry_t;

/* Global state */
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static sdmmc_card_t *sd_card = NULL;
static rom_entry_t rom_list[32];
static int rom_count = 0;
static int rom_selected = 0;
static int scroll_offset = 0;

/* UI state */
typedef enum {
    UI_MAIN,
    UI_FLASHING,
    UI_WIFI,
    UI_FILES,
} ui_state_t;

static ui_state_t current_ui = UI_MAIN;

/* Initialize LCD */
static void lcd_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCREEN_W * 2 * 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &lcd_io));

    /* ST7735 initialization commands */
    const struct {
        uint8_t cmd;
        uint8_t data[16];
        uint8_t len;
        uint32_t delay_ms;
    } st7735_init[] = {
        {0x01, {0}, 0, 150},           /* SWRESET */
        {0x11, {0}, 0, 255},           /* SLPOUT */
        {0x3A, {0x05}, 1, 0},          /* COLMOD: RGB565 */
        {0x36, {0x00}, 1, 0},          /* MADCTL: normal */
        {0x29, {0}, 0, 0},             /* DISPON */
    };

    for (int i = 0; i < sizeof(st7735_init)/sizeof(st7735_init[0]); i++) {
        esp_lcd_panel_io_tx_param(lcd_io, st7735_init[i].cmd, 
                                   st7735_init[i].data, st7735_init[i].len);
        if (st7735_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(st7735_init[i].delay_ms));
        }
    }

    ui_init(lcd_io);
    ESP_LOGI(TAG, "LCD initialized");
}

/* Initialize SD card */
static esp_err_t sd_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_LCD_MOSI,
        .miso_io_num = 19,
        .sclk_io_num = PIN_NUM_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO));

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_SD_CS;
    slot_config.host_id = host.slot;

    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted");
    return ESP_OK;
}

/* Initialize buttons */
static void buttons_init(void)
{
    const int btn_pins[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B};
    for (int i = 0; i < 6; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << btn_pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }
    ESP_LOGI(TAG, "Buttons initialized");
}

/* Read button state (active low) */
static int btn_read(int pin)
{
    return gpio_get_level(pin) == 0 ? 1 : 0;
}

/* Scan ROMs in /sdcard/boot/ */
static void scan_roms(void)
{
    rom_count = 0;
    DIR *dir = opendir("/sdcard/boot");
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open /sdcard/boot");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && rom_count < 32) {
        if (ent->d_type != DT_REG) continue;
        
        char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".bin") != 0 && strcasecmp(ext, ".img") != 0) continue;

        rom_entry_t *rom = &rom_list[rom_count];
        strncpy(rom->name, ent->d_name, sizeof(rom->name) - 1);
        snprintf(rom->path, sizeof(rom->path), "/sdcard/boot/%s", ent->d_name);
        
        struct stat st;
        if (stat(rom->path, &st) == 0) {
            rom->size = st.st_size;
        }
        
        rom_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Found %d ROMs", rom_count);
}

/* Draw main UI */
static void ui_draw_main(void)
{
    ui_clear(COLOR_BG);

    /* Title bar */
    ui_fill_rect(0, 0, SCREEN_W, 16, COLOR_TITLE_BG);
    ui_text_center(4, "XIAOMIAO LOADER", COLOR_FG, COLOR_TITLE_BG);

    if (rom_count == 0) {
        ui_text_center(50, "No ROMs found", COLOR_FG, COLOR_BG);
        ui_text_center(65, "Put .bin/.img in", COLOR_FG, COLOR_BG);
        ui_text_center(75, "/sdcard/boot/", COLOR_FG, COLOR_BG);
    } else {
        /* ROM list */
        int y = 20;
        int visible = (SCREEN_H - 20 - 16) / 16;  /* 16px per item */
        
        for (int i = 0; i < visible && (i + scroll_offset) < rom_count; i++) {
            int idx = i + scroll_offset;
            bool selected = (idx == rom_selected);
            uint16_t bg = selected ? COLOR_SELECT : COLOR_BG;
            uint16_t fg = selected ? COLOR_BG : COLOR_FG;
            
            ui_fill_rect(0, y, SCREEN_W, 16, bg);
            
            char line[64];
            snprintf(line, sizeof(line), "%c %s", 
                     selected ? '>' : ' ', rom_list[idx].name);
            ui_text(2, y + 4, line, fg, bg);
            
            y += 16;
        }
    }

    /* Bottom bar */
    ui_fill_rect(0, SCREEN_H - 16, SCREEN_W, 16, COLOR_TITLE_BG);
    ui_text(2, SCREEN_H - 12, "A:Flash B:WiFi", COLOR_FG, COLOR_TITLE_BG);
    ui_text(SCREEN_W - 60, SCREEN_H - 12, "L:Files", COLOR_FG, COLOR_TITLE_BG);

    ui_flush();
}

/* Draw flashing progress */
static void ui_draw_flash(const char *name, int percent)
{
    ui_clear(COLOR_BG);
    ui_fill_rect(0, 0, SCREEN_W, 16, COLOR_TITLE_BG);
    ui_text_center(4, "FLASHING ROM", COLOR_FG, COLOR_TITLE_BG);

    ui_text_center(40, name, COLOR_FG, COLOR_BG);
    
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", percent);
    ui_text_center(60, pct_str, COLOR_FG, COLOR_BG);
    
    ui_progress(20, 80, SCREEN_W - 40, 12, percent, COLOR_SELECT, COLOR_FG);
    
    ui_flush();
}

/* Flash ROM to ota_0 */
static esp_err_t flash_rom(const rom_entry_t *rom)
{
    ESP_LOGI(TAG, "Flashing ROM: %s", rom->name);
    
    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota0) {
        ESP_LOGE(TAG, "OTA_0 partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(rom->path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", rom->path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Check if it's a merged binary (app at 0x10000) */
    fseek(f, 0x10000, SEEK_SET);
    uint8_t magic;
    if (fread(&magic, 1, 1, f) != 1 || magic != 0xE9) {
        /* Not merged, try app at offset 0 */
        fseek(f, 0, SEEK_SET);
        if (fread(&magic, 1, 1, f) != 1 || magic != 0xE9) {
            fclose(f);
            ESP_LOGE(TAG, "Invalid ROM format");
            return ESP_ERR_INVALID_ARG;
        }
        fseek(f, 0, SEEK_SET);
    } else {
        fseek(f, 0x10000, SEEK_SET);
    }

    /* Erase partition */
    ESP_ERROR_CHECK(esp_partition_erase_range(ota0, 0, ota0->size));

    /* Write in chunks */
    uint8_t buf[4096];
    size_t written = 0;
    size_t total = rom->size;
    
    while (!feof(f)) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        
        esp_err_t err = esp_partition_write(ota0, written, buf, n);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
        written += n;
        
        int percent = (written * 100) / total;
        ui_draw_flash(rom->name, percent);
    }
    fclose(f);

    /* Set boot partition */
    ESP_ERROR_CHECK(esp_ota_set_boot_partition(ota0));
    
    /* Save to NVS */
    nvs_handle_t nvs;
    if (nvs_open("loader", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "last_rom", rom->name);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Flash complete, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

/* Main task */
static void main_task(void *arg)
{
    /* Initialize hardware */
    lcd_init();
    buttons_init();
    
    /* Show splash */
    ui_clear(COLOR_BG);
    ui_text_center(40, "XIAOMIAO LOADER", COLOR_FG, COLOR_BG);
    ui_text_center(60, "Initializing...", COLOR_FG, COLOR_BG);
    ui_flush();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Mount SD card */
    if (sd_init() != ESP_OK) {
        ui_clear(COLOR_BG);
        ui_text_center(50, "SD Card Error!", 0x001F, COLOR_BG);
        ui_flush();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* Scan ROMs */
    scan_roms();

    /* Check if B button pressed on boot -> WiFi mode */
    if (btn_read(BTN_B)) {
        current_ui = UI_WIFI;
        wifi_fileman_start();
        
        ui_clear(COLOR_BG);
        ui_text_center(30, "WIFI MODE", COLOR_FG, COLOR_BG);
        ui_text_center(50, "SSID: Xiaomiao-Loader", COLOR_FG, COLOR_BG);
        ui_text_center(65, "PASS: 12345678", COLOR_FG, COLOR_BG);
        ui_text_center(80, "http://192.168.4.1", COLOR_FG, COLOR_BG);
        ui_text_center(100, "Press B to exit", COLOR_FG, COLOR_BG);
        ui_flush();
        
        while (current_ui == UI_WIFI) {
            if (btn_read(BTN_B)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                if (btn_read(BTN_B)) {
                    wifi_fileman_stop();
                    current_ui = UI_MAIN;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    /* Main loop */
    while (1) {
        if (current_ui == UI_MAIN) {
            ui_draw_main();
            
            /* Handle buttons */
            if (btn_read(BTN_UP)) {
                if (rom_selected > 0) {
                    rom_selected--;
                    if (rom_selected < scroll_offset) scroll_offset = rom_selected;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            else if (btn_read(BTN_DOWN)) {
                if (rom_selected < rom_count - 1) {
                    rom_selected++;
                    int visible = (SCREEN_H - 20 - 16) / 16;
                    if (rom_selected >= scroll_offset + visible) {
                        scroll_offset = rom_selected - visible + 1;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            else if (btn_read(BTN_A)) {
                if (rom_count > 0) {
                    flash_rom(&rom_list[rom_selected]);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            else if (btn_read(BTN_B)) {
                current_ui = UI_WIFI;
                wifi_fileman_start();
                
                ui_clear(COLOR_BG);
                ui_text_center(30, "WIFI MODE", COLOR_FG, COLOR_BG);
                ui_text_center(50, "SSID: Xiaomiao-Loader", COLOR_FG, COLOR_BG);
                ui_text_center(65, "PASS: 12345678", COLOR_FG, COLOR_BG);
                ui_text_center(80, "http://192.168.4.1", COLOR_FG, COLOR_BG);
                ui_text_center(100, "Press B to exit", COLOR_FG, COLOR_BG);
                ui_flush();
                
                while (current_ui == UI_WIFI) {
                    if (btn_read(BTN_B)) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        if (btn_read(BTN_B)) {
                            wifi_fileman_stop();
                            current_ui = UI_MAIN;
                            break;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            else if (btn_read(BTN_LEFT)) {
                current_ui = UI_FILES;
                /* TODO: Implement local file browser */
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Start main task */
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}
