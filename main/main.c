/*
 * Xiaomiao ROM Loader
 *
 * ESP32-WROVER-B + ST7735 SPI TFT + MicroSD + 6-key keypad.
 * Scans /sdcard/boot for .bin files, shows a selection list, extracts the
 * app image from the chosen merged-bin and writes it to the ota_0
 * partition, then reboots into the ROM.  Each ROM's app_main() is
 * expected to call esp_ota_set_boot_partition(factory) at startup
 * so that any subsequent reset returns to this loader.
 *
 * Burning interface (esptool via GD32 USB-UART bridge) is always
 * available: the loader never touches the bootloader (0x1000) or
 * the partition table (0x8000).
 */

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

/* ── Pin & hardware constants (from original xiaomiao firmware) ────────── */

#define LCD_HOST                    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ          (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES            128
#define LCD_NATIVE_V_RES            160
#define LCD_H_RES                   160
#define LCD_V_RES                   128
#define LCD_DRAW_BUF_LINES          LCD_V_RES
#define LCD_DRAW_BUF_COUNT          3
#define LCD_DPI                     60
#define LCD_CMD_BITS                8
#define LCD_PARAM_BITS              8

#define PIN_NUM_LCD_SCLK            GPIO_NUM_18
#define PIN_NUM_LCD_MOSI            GPIO_NUM_23
#define PIN_NUM_LCD_MISO            GPIO_NUM_19
#define PIN_NUM_LCD_CS              GPIO_NUM_5
#define PIN_NUM_LCD_DC              GPIO_NUM_4
#define PIN_NUM_SD_CS               GPIO_NUM_22

#define LCD_X_GAP                   0
#define LCD_Y_GAP                   0

#define LVGL_TICK_PERIOD_MS         1
#define LVGL_TASK_STACK_SIZE        (10 * 1024)
#define LVGL_TASK_PRIORITY          5
#define LVGL_TASK_MIN_DELAY_MS      1
#define LVGL_TASK_MAX_DELAY_MS      16

#define BUTTON_ACTIVE_LEVEL         0
#define BUTTON_DEBOUNCE_MS          25

#define SD_SPI_MAX_FREQ_KHZ         10000

/* ST7735 register addresses */
#define ST7735_SWRESET              0x01
#define ST7735_SLPOUT               0x11
#define ST7735_NORON                0x13
#define ST7735_INVOFF               0x20
#define ST7735_DISPOFF              0x28
#define ST7735_DISPON               0x29
#define ST7735_CASET                0x2A
#define ST7735_RASET                0x2B
#define ST7735_RAMWR                0x2C
#define ST7735_MADCTL               0x36
#define ST7735_COLMOD               0x3A
#define ST7735_FRMCTR1              0xB1
#define ST7735_FRMCTR2              0xB2
#define ST7735_FRMCTR3              0xB3
#define ST7735_INVCTR               0xB4
#define ST7735_PWCTR1               0xC0
#define ST7735_PWCTR2               0xC1
#define ST7735_PWCTR3               0xC2
#define ST7735_PWCTR4               0xC3
#define ST7735_PWCTR5               0xC4
#define ST7735_VMCTR1               0xC5
#define ST7735_GMCTRP1              0xE0
#define ST7735_GMCTRN1              0xE1

#define MADCTL_MY                   0x80
#define MADCTL_MX                   0x40
#define MADCTL_MV                   0x20
#define MADCTL_RGB                  0x00

/* ── UI palette (original yellow theme) ────────────────────────────────── */

#define UI_YELLOW   0xF6D34A
#define UI_BLACK    0x1B1713
#define UI_BROWN    0x5C4220
#define UI_RED      0xE64B3C
#define UI_CREAM    0xFFF3B0
#define UI_GREEN    0x2DD466

/* ── Data types ────────────────────────────────────────────────────────── */

#define MAX_ROMS        32
#define ROM_DIR         "/sdcard/boot"
#define OTA_CHUNK_SZ    4096

#define ESP_IMAGE_MAGIC 0xE9
#define APP_OFFSET_MERGED  0x10000   /* app image start within a merged bin */

#define NVS_NS_LOADER   "loader"
#define NVS_KEY_NAME    "cur_name"
#define NVS_KEY_FSIZE   "cur_fsize"

typedef struct {
    char     name[64];      /* display name (filename without .bin)       */
    char     path[280];     /* full path on SD card                        */
    size_t   file_size;     /* total file size in bytes                    */
    size_t   app_offset;    /* 0 for app-only, 0x10000 for merged bin     */
    size_t   app_size;      /* extracted app image size                    */
    bool     valid;         /* true if a valid ESP32 image was detected    */
} rom_entry_t;

typedef struct {
    bool     valid;         /* true if ota_0 holds a known ROM             */
    char     name[64];      /* ROM name currently in ota_0                 */
    uint32_t file_size;     /* file size of the ROM currently in ota_0     */
} ota0_state_t;

typedef struct {
    int rom_index;
} btn_ctx_t;

/* ── Globals ───────────────────────────────────────────────────────────── */

static const char *TAG = "rom_loader";

static const gpio_num_t s_btn_gpios[] = {
    GPIO_NUM_2,    /* UP    */
    GPIO_NUM_13,   /* DOWN  */
    GPIO_NUM_27,   /* LEFT  */
    GPIO_NUM_35,   /* RIGHT */
    GPIO_NUM_34,   /* A / ENTER */
    GPIO_NUM_12,   /* B / ESC   */
};
static const uint32_t s_btn_keys[] = {
    LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT, LV_KEY_ENTER, LV_KEY_ESC,
};
#define NUM_BUTTONS  (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

static rom_entry_t s_roms[MAX_ROMS];
static int         s_rom_count = 0;
static int         s_flash_rom_idx = -1;   /* pending flash request, -1 = none */
static btn_ctx_t   s_btn_ctxs[MAX_ROMS];
static ota0_state_t s_ota0_state;          /* what's currently in ota_0       */

static lv_draw_buf_t          s_draw_buf3;
static esp_lcd_panel_io_handle_t s_lcd_io_handle;
static sdmmc_card_t           *s_sd_card;
static bool   s_sd_mounted;
static char   s_sd_name[24] = "NO CARD";
static uint32_t s_sd_mb;
static volatile bool s_lcd_first_flush_done;
static bool s_lcd_display_on;

/* UI screen tracking */
static lv_obj_t *s_main_screen;
static lv_obj_t *s_about_screen;
static lv_obj_t *s_rom_btns[MAX_ROMS];
static lv_obj_t *s_about_area;

/* ── Forward declarations ──────────────────────────────────────────────── */

static void ui_build_main(lv_group_t *group);
static void ui_build_about(lv_group_t *group);
static void ui_show_flash(const rom_entry_t *rom);
static void ui_show_skip(const rom_entry_t *rom);
static void ui_update_flash(int pct, size_t done, size_t total);
static void ui_flash_result(bool ok, const char *detail);

/* ── Utility ───────────────────────────────────────────────────────────── */

static void format_size(char *buf, size_t bufsz, size_t bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(buf, bufsz, "%lu.%luMB",
                 (unsigned long)(bytes / (1024 * 1024)),
                 (unsigned long)((bytes / (1024 * 102)) % 10));
    else
        snprintf(buf, bufsz, "%luKB", (unsigned long)(bytes / 1024));
}

/* ── Buttons ───────────────────────────────────────────────────────────── */

static void buttons_init(void)
{
    uint64_t pin_mask = 0;
    uint64_t pullup_mask = 0;

    for (size_t i = 0; i < NUM_BUTTONS; ++i) {
        pin_mask |= 1ULL << s_btn_gpios[i];
        if (s_btn_gpios[i] != GPIO_NUM_34 && s_btn_gpios[i] != GPIO_NUM_35)
            pullup_mask |= 1ULL << s_btn_gpios[i];
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    if (pullup_mask) {
        gpio_config_t pullup_conf = {
            .pin_bit_mask = pullup_mask,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pullup_conf));
    }
}

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static int last_raw = -1;
    static int stable   = -1;
    static uint32_t raw_changed_ms = 0;
    static uint32_t last_key = LV_KEY_ENTER;
    int raw = -1;
    const uint32_t now = lv_tick_get();

    for (size_t i = 0; i < NUM_BUTTONS; ++i) {
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) {
            raw = (int)i;
            break;
        }
    }

    if (raw != last_raw) {
        last_raw = raw;
        raw_changed_ms = now;
        if (raw < 0) stable = -1;
    }
    if (lv_tick_elaps(raw_changed_ms) >= BUTTON_DEBOUNCE_MS)
        stable = last_raw;

    if (stable >= 0) {
        last_key = s_btn_keys[stable];
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}

/* ── LCD / ST7735 ──────────────────────────────────────────────────────── */

static void st7735_tx_param(esp_lcd_panel_io_handle_t io, int cmd,
                            const void *param, size_t param_size)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd, param, param_size));
}

static void st7735_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void st7735_clear_black(esp_lcd_panel_io_handle_t io_handle)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    memset(line, 0, sizeof(line));
    st7735_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        st7735_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset));
        st7735_tx_param(io_handle, ST7735_RAMWR, line,
                        (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
}

static void st7735_init_black_tab_rot90(esp_lcd_panel_io_handle_t io)
{
    const uint8_t frmctr[]  = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t invctr[]  = {0x07};
    const uint8_t pwctr1[]  = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[]  = {0xC5};
    const uint8_t pwctr3[]  = {0x0A, 0x00};
    const uint8_t pwctr4[]  = {0x8A, 0x2A};
    const uint8_t pwctr5[]  = {0x8A, 0xEE};
    const uint8_t vmctr1[]  = {0x0E};
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t colmod[]   = {0x05};
    const uint8_t caset[]    = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};
    const uint8_t raset[]    = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};
    const uint8_t gp[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                          0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    const uint8_t gn[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                          0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    st7735_tx_param(io, ST7735_DISPOFF, NULL, 0);
    st7735_tx_param(io, ST7735_SWRESET, NULL, 0);
    st7735_delay_ms(150);
    st7735_tx_param(io, ST7735_SLPOUT, NULL, 0);
    st7735_delay_ms(500);
    st7735_tx_param(io, ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st7735_tx_param(io, ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st7735_tx_param(io, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st7735_tx_param(io, ST7735_INVCTR,  invctr, sizeof(invctr));
    st7735_tx_param(io, ST7735_PWCTR1,  pwctr1, sizeof(pwctr1));
    st7735_tx_param(io, ST7735_PWCTR2,  pwctr2, sizeof(pwctr2));
    st7735_tx_param(io, ST7735_PWCTR3,  pwctr3, sizeof(pwctr3));
    st7735_tx_param(io, ST7735_PWCTR4,  pwctr4, sizeof(pwctr4));
    st7735_tx_param(io, ST7735_PWCTR5,  pwctr5, sizeof(pwctr5));
    st7735_tx_param(io, ST7735_VMCTR1,  vmctr1, sizeof(vmctr1));
    st7735_tx_param(io, ST7735_INVOFF,  NULL, 0);
    st7735_tx_param(io, ST7735_MADCTL,  madctl_d, sizeof(madctl_d));
    st7735_tx_param(io, ST7735_COLMOD,  colmod, sizeof(colmod));
    st7735_tx_param(io, ST7735_CASET,   caset, sizeof(caset));
    st7735_tx_param(io, ST7735_RASET,   raset, sizeof(raset));
    st7735_tx_param(io, ST7735_GMCTRP1, gp, sizeof(gp));
    st7735_tx_param(io, ST7735_GMCTRN1, gn, sizeof(gn));
    st7735_tx_param(io, ST7735_NORON,   NULL, 0);
    st7735_delay_ms(10);
    st7735_tx_param(io, ST7735_MADCTL,  madctl_r, sizeof(madctl_r));
    st7735_clear_black(io);
}

static void lcd_display_on(void)
{
    if (s_lcd_display_on || !s_lcd_io_handle) return;
    st7735_tx_param(s_lcd_io_handle, ST7735_DISPON, NULL, 0);
    st7735_delay_ms(20);
    s_lcd_display_on = true;
}

static esp_lcd_panel_io_handle_t lcd_init(void)
{
    ESP_LOGI(TAG, "Init SPI bus + ST7735 TFT");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_LCD_SCLK,
        .mosi_io_num = PIN_NUM_LCD_MOSI,
        .miso_io_num = PIN_NUM_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));
    s_lcd_io_handle = io;
    s_lcd_display_on = false;
    s_lcd_first_flush_done = false;
    st7735_init_black_tab_rot90(io);
    return io;
}

/* ── LVGL display + input ──────────────────────────────────────────────── */

static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel_io; (void)edata;
    s_lcd_first_flush_done = true;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display,
                          const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_io_handle_t io = lv_display_get_user_data(display);
    const uint16_t x1 = area->x1 + LCD_X_GAP;
    const uint16_t x2 = area->x2 + LCD_X_GAP;
    const uint16_t y1 = area->y1 + LCD_Y_GAP;
    const uint16_t y2 = area->y2 + LCD_Y_GAP;
    const uint8_t caset[] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    const uint8_t raset[] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    esp_lcd_panel_io_tx_param(io, ST7735_CASET, caset, sizeof(caset));
    esp_lcd_panel_io_tx_param(io, ST7735_RASET, raset, sizeof(raset));
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px_map, w * h * sizeof(uint16_t));
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static lv_display_t *lvgl_display_init(esp_lcd_panel_io_handle_t io)
{
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    assert(disp);

    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    const uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, cf);
    const size_t buf_sz = stride * LCD_DRAW_BUF_LINES;
    void *b1 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    void *b2 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    void *b3 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    assert(b1 && b2 && b3);

    lv_display_set_color_format(disp, cf);
    lv_display_set_dpi(disp, LCD_DPI);
    lv_display_set_buffers(disp, b1, b2, buf_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_result_t r = lv_draw_buf_init(&s_draw_buf3, LCD_H_RES, LCD_DRAW_BUF_LINES,
                                     cf, stride, b3, buf_sz);
    assert(r == LV_RESULT_OK);
    lv_display_set_3rd_draw_buffer(disp, &s_draw_buf3);
    lv_display_set_user_data(disp, io);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    return disp;
}

static lv_group_t *lvgl_input_init(lv_display_t *disp)
{
    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, disp);
    lv_indev_set_group(indev, group);
    lv_indev_set_read_cb(indev, keypad_read_cb);
    lv_indev_set_long_press_time(indev, 360);
    lv_indev_set_long_press_repeat_time(indev, 130);
    return group;
}

/* ── SD card ───────────────────────────────────────────────────────────── */

static void sd_try_mount(void)
{
    if (s_sd_mounted) return;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = LCD_HOST;
    slot_cfg.gpio_cs = PIN_NUM_SD_CS;
    slot_cfg.wait_for_miso = 20;

    esp_vfs_fat_mount_config_t mnt_cfg = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mnt_cfg.format_if_mount_failed = false;
    mnt_cfg.max_files = 4;

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg,
                                            &mnt_cfg, &s_sd_card);
    if (err == ESP_OK && s_sd_card) {
        s_sd_mounted = true;
        memset(s_sd_name, 0, sizeof(s_sd_name));
        memcpy(s_sd_name, s_sd_card->cid.name,
               MIN(sizeof(s_sd_card->cid.name), sizeof(s_sd_name) - 1));
        s_sd_mb = (uint32_t)(((uint64_t)s_sd_card->csd.capacity *
                              s_sd_card->csd.sector_size) / (1024 * 1024));
        ESP_LOGI(TAG, "SD mounted: %s  %luMB", s_sd_name, (unsigned long)s_sd_mb);
    } else {
        s_sd_mounted = false;
        s_sd_card = NULL;
        strncpy(s_sd_name, "NO CARD", sizeof(s_sd_name) - 1);
        s_sd_mb = 0;
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
    }
}

static void sd_deinit(void)
{
    if (!s_sd_mounted || !s_sd_card)
        return;

    gpio_set_direction(PIN_NUM_SD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_SD_CS, 1);

    esp_vfs_fat_sdcard_unmount("/sdcard", s_sd_card);
    s_sd_card = NULL;
    s_sd_mounted = false;

    /* Send 80 dummy clocks with CS high to reset the SD card
     * into a clean idle state before handing off to the ROM. */
    spi_device_handle_t tmp_handle;
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = 400 * 1000,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    if (spi_bus_add_device(LCD_HOST, &devcfg, &tmp_handle) == ESP_OK) {
        uint8_t dummy[10] = {0xFF};
        spi_transaction_t t = {
            .length = 80,
            .tx_buffer = dummy,
        };
        spi_device_polling_transmit(tmp_handle, &t);
        spi_bus_remove_device(tmp_handle);
    }

    gpio_set_level(PIN_NUM_SD_CS, 1);
    ESP_LOGI(TAG, "SD deinitialized");
}

/* ── ROM scanning & app image parsing ──────────────────────────────────── */

/**
 * Detect whether a .bin file is an app-only image (magic 0xE9 at byte 0)
 * or a merged flash image (magic 0xE9 at offset 0x10000).
 * Returns the app offset, or (size_t)-1 if neither.
 */
static size_t rom_detect_app_offset(FILE *f)
{
    uint8_t b0, b_merged;
    if (fseek(f, 0, SEEK_SET) != 0) return (size_t)-1;
    if (fread(&b0, 1, 1, f) != 1) return (size_t)-1;
    if (b0 == ESP_IMAGE_MAGIC) return 0;

    if (fseek(f, APP_OFFSET_MERGED, SEEK_SET) != 0) return (size_t)-1;
    if (fread(&b_merged, 1, 1, f) != 1) return (size_t)-1;
    if (b_merged == ESP_IMAGE_MAGIC) return APP_OFFSET_MERGED;

    return (size_t)-1;
}

/**
 * Parse the ESP-IDF app image header + segment table to calculate the
 * total image size (header + segments + padding + checksum + hash).
 */
static size_t rom_calc_app_size(FILE *f, size_t offset)
{
    uint8_t hdr[24];
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return 0;
    if (hdr[0] != ESP_IMAGE_MAGIC) return 0;

    uint8_t seg_count = hdr[1];
    uint8_t hash_appended = hdr[23];

    size_t pos = sizeof(hdr);   /* 24 bytes for header */
    for (uint8_t i = 0; i < seg_count; i++) {
        uint8_t sh[8];
        if (fseek(f, (long)(offset + pos), SEEK_SET) != 0) return 0;
        if (fread(sh, 1, sizeof(sh), f) != sizeof(sh)) return 0;
        uint32_t seg_len;
        memcpy(&seg_len, &sh[4], 4);
        pos += sizeof(sh) + seg_len;
    }

    /* pad to 16-byte boundary, add checksum, pad again, add hash */
    pos = (pos + 15) & ~(size_t)15;
    pos += 1;                                       /* checksum byte */
    pos = (pos + 15) & ~(size_t)15;
    if (hash_appended) pos += 32;                   /* SHA-256 */

    return pos;
}

static int rom_scan(void)
{
    DIR *dir = opendir(ROM_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", ROM_DIR);
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < MAX_ROMS) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcasecmp(dot, ".bin") != 0) continue;

        rom_entry_t *r = &s_roms[count];
        memset(r, 0, sizeof(*r));

        snprintf(r->path, sizeof(r->path), "%s/%s", ROM_DIR, ent->d_name);

        size_t name_len = (size_t)(dot - ent->d_name);
        if (name_len >= sizeof(r->name)) name_len = sizeof(r->name) - 1;
        memcpy(r->name, ent->d_name, name_len);
        r->name[name_len] = '\0';

        struct stat st;
        if (stat(r->path, &st) == 0)
            r->file_size = (size_t)st.st_size;

        FILE *f = fopen(r->path, "rb");
        if (f) {
            r->app_offset = rom_detect_app_offset(f);
            if (r->app_offset != (size_t)-1) {
                r->app_size = rom_calc_app_size(f, r->app_offset);
                r->valid = (r->app_size > 0);
            } else {
                r->valid = false;
            }
            fclose(f);
        }

        ESP_LOGI(TAG, "ROM: %-20s  file=%zu  app_off=0x%zx  app=%zu  valid=%d",
                 r->name, r->file_size, r->app_offset, r->app_size, r->valid);
        count++;
    }
    closedir(dir);
    return count;
}

/* ── ota_0 state tracking (NVS-backed) ─────────────────────────────────── */

/**
 * Read NVS record and verify ota_0 actually has a valid app image.
 * Returns populated ota0_state_t (valid=false if ota_0 is empty or
 * NVS has no record).
 */
static void ota0_load_state(void)
{
    memset(&s_ota0_state, 0, sizeof(s_ota0_state));

    /* verify ota_0 partition has a real app image (not erased) */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!part) return;

    uint8_t magic = 0xFF;
    if (esp_partition_read(part, 0, &magic, 1) != ESP_OK) return;
    if (magic != ESP_IMAGE_MAGIC) return;

    /* read the NVS record for name + size */
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LOADER, NVS_READONLY, &h) != ESP_OK) return;

    size_t name_len = sizeof(s_ota0_state.name);
    if (nvs_get_str(h, NVS_KEY_NAME, s_ota0_state.name, &name_len) != ESP_OK) {
        nvs_close(h);
        return;
    }

    uint32_t fsize = 0;
    if (nvs_get_u32(h, NVS_KEY_FSIZE, &fsize) != ESP_OK) {
        nvs_close(h);
        return;
    }

    nvs_close(h);
    s_ota0_state.file_size = fsize;
    s_ota0_state.valid = true;

    ESP_LOGI(TAG, "ota_0 contains: %s (%lu bytes)",
             s_ota0_state.name, (unsigned long)s_ota0_state.file_size);
}

/**
 * Check whether a ROM entry matches what's already in ota_0.
 * Compares display name and file size.
 */
static bool ota0_rom_matches(const rom_entry_t *rom)
{
    if (!s_ota0_state.valid) return false;
    return strcmp(rom->name, s_ota0_state.name) == 0 &&
           rom->file_size == s_ota0_state.file_size;
}

/**
 * Save the current ROM identity to NVS after a successful flash.
 */
static void ota0_save_state(const rom_entry_t *rom)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LOADER, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(RW) failed");
        return;
    }
    nvs_set_str(h, NVS_KEY_NAME, rom->name);
    nvs_set_u32(h, NVS_KEY_FSIZE, (uint32_t)rom->file_size);
    nvs_commit(h);
    nvs_close(h);
}

/* ── OTA flash ─────────────────────────────────────────────────────────── */

/**
 * Extract the app image from the ROM file and write it to the ota_0
 * partition.  Updates the progress bar via lv_timer_handler between
 * chunks so the display stays live.
 *
 * Returns ESP_OK on success, or an error code.
 */
static esp_err_t rom_flash_ota(const rom_entry_t *rom)
{
    FILE *f = fopen(rom->path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", rom->path);
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!part) {
        fclose(f);
        ESP_LOGE(TAG, "ota_0 partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (rom->app_size > part->size) {
        fclose(f);
        ESP_LOGE(TAG, "App image %zu bytes > ota_0 %lu bytes",
                 rom->app_size, (unsigned long)part->size);
        return ESP_ERR_NO_MEM;
    }

    /* Write all available data from app_offset to EOF so esp_ota_end
     * can verify the complete image — manual size calculation may
     * undercount, causing incomplete writes and verification failure. */
    size_t write_size = rom->file_size - rom->app_offset;
    if (write_size > part->size) write_size = part->size;

    ESP_LOGI(TAG, "Flashing %s: offset=0x%zx write=%zu → ota_0@0x%08lx",
            rom->name, rom->app_offset, write_size,
            (unsigned long)part->address);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, write_size, &handle);
    if (err != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    if (fseek(f, (long)rom->app_offset, SEEK_SET) != 0) {
        esp_ota_abort(handle);
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_CHUNK_SZ);
    if (!buf) {
        esp_ota_abort(handle);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < write_size) {
        size_t want = MIN(OTA_CHUNK_SZ, write_size - total);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            ESP_LOGE(TAG, "fread returned 0 at offset %zu", total);
            free(buf);
            esp_ota_abort(handle);
            fclose(f);
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(handle);
            fclose(f);
            return err;
        }
        total += got;

        int pct = (int)(total * 100 / write_size);
        ui_update_flash(pct, total, write_size);

        /* keep LVGL display alive during the write */
        lv_tick_inc(LVGL_TASK_MAX_DELAY_MS);
        lv_timer_handler();
    }

    free(buf);
    fclose(f);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA write complete, rebooting into %s", rom->name);
    return ESP_OK;
}

/* ── UI: main screen (ROM list) ────────────────────────────────────────── */

static void on_rom_clicked(lv_event_t *e)
{
    btn_ctx_t *ctx = (btn_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->rom_index >= 0 && ctx->rom_index < s_rom_count)
        s_flash_rom_idx = ctx->rom_index;
}

static void on_rom_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    lv_group_t *grp = (lv_group_t *)lv_event_get_user_data(e);
    if (key == LV_KEY_ESC) {
        if (grp) ui_build_about(grp);
    } else if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
        lv_group_focus_prev(grp);
    } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
        lv_group_focus_next(grp);
    }
}

static void apply_bar_style(lv_obj_t *label, uint32_t bg, uint32_t fg)
{
    lv_obj_set_style_bg_color(label, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
    lv_obj_set_style_pad_hor(label, 4, 0);
}

static void ui_build_main(lv_group_t *group)
{
    if (s_main_screen) {
        lv_screen_load(s_main_screen);
        if (s_rom_count > 0 && s_rom_btns[0])
            lv_group_focus_obj(s_rom_btns[0]);
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    s_main_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);

    /* Title bar */
    lv_obj_t *title = lv_label_create(scr);
    apply_bar_style(title, UI_BROWN, UI_CREAM);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "ROM LOADER");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* Status line */
    char status_buf[64];
    if (s_sd_mounted) {
        snprintf(status_buf, sizeof(status_buf), "SD: %s %luMB  ROMs: %d",
                 s_sd_name, (unsigned long)s_sd_mb, s_rom_count);
    } else {
        snprintf(status_buf, sizeof(status_buf), "No SD card!  %s",
                 ROM_DIR);
    }
    lv_obj_t *status = lv_label_create(scr);
    apply_bar_style(status, UI_YELLOW, UI_BROWN);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_text(status, status_buf);

    /* ROM list area */
    lv_obj_t *list_area = lv_obj_create(scr);
    lv_obj_set_width(list_area, lv_pct(100));
    lv_obj_set_flex_grow(list_area, 1);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(list_area, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(list_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list_area, 0, 0);
    lv_obj_set_style_pad_all(list_area, 2, 0);
    lv_obj_set_style_pad_row(list_area, 1, 0);
    lv_obj_set_scrollbar_mode(list_area, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(list_area, LV_OBJ_FLAG_SCROLL_ELASTIC);

    if (s_rom_count <= 0) {
        lv_obj_t *empty = lv_label_create(list_area);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_BROWN), 0);
        lv_label_set_text(empty,
            "No .bin files found.\n\n"
            "Put ROM images in:\n" ROM_DIR);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        for (int i = 0; i < s_rom_count; i++) {
            const rom_entry_t *r = &s_roms[i];
            char szbuf[16];
            format_size(szbuf, sizeof(szbuf), r->file_size);

            lv_obj_t *btn = lv_button_create(list_area);
            lv_obj_set_width(btn, lv_pct(100));
            lv_obj_set_style_bg_color(btn, lv_color_hex(UI_CREAM), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_pad_ver(btn, 3, 0);
            lv_obj_set_style_pad_hor(btn, 6, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);

            /* focused state — label inherits text_color from button */
            lv_obj_set_style_bg_color(btn, lv_color_hex(UI_BROWN),
                                      LV_STATE_FOCUSED);
            lv_obj_set_style_text_color(btn, lv_color_hex(UI_BROWN), 0);
            lv_obj_set_style_text_color(btn, lv_color_hex(UI_CREAM),
                                        LV_STATE_FOCUSED);

            char label_text[128];
            bool loaded = ota0_rom_matches(r);
            snprintf(label_text, sizeof(label_text), "%s%s  %s%s",
                     loaded ? "> " : "  ",
                     r->name, szbuf,
                     r->valid ? "" : " [invalid]");
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, label_text);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(lbl);

            s_btn_ctxs[i].rom_index = i;
            s_rom_btns[i] = btn;
            lv_obj_add_event_cb(btn, on_rom_clicked,
                                LV_EVENT_SHORT_CLICKED, &s_btn_ctxs[i]);
            lv_obj_add_event_cb(btn, on_rom_key,
                                LV_EVENT_KEY, group);
            lv_group_add_obj(group, btn);
        }
        /* focus first ROM so user can navigate immediately */
        if (s_rom_count > 0)
            lv_group_focus_obj(s_rom_btns[0]);
    }

    /* Hint bar */
    lv_obj_t *hint = lv_label_create(scr);
    apply_bar_style(hint, UI_BROWN, UI_CREAM);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_text(hint, "A:Load  B:About");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(scr);
}

/* ── UI: about screen ──────────────────────────────────────────────────── */

static void on_about_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        if (s_main_screen) {
            lv_screen_load(s_main_screen);
            if (s_rom_count > 0 && s_rom_btns[0])
                lv_group_focus_obj(s_rom_btns[0]);
        }
    } else if (key == LV_KEY_UP && s_about_area) {
        lv_obj_scroll_by(s_about_area, 0, 20, LV_ANIM_ON);
    } else if (key == LV_KEY_DOWN && s_about_area) {
        lv_obj_scroll_by(s_about_area, 0, -20, LV_ANIM_ON);
    }
}

static void ui_build_about(lv_group_t *group)
{
    if (s_about_screen) {
        lv_screen_load(s_about_screen);
        lv_group_focus_obj(s_about_area);
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    s_about_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);

    lv_obj_t *title = lv_label_create(scr);
    apply_bar_style(title, UI_BROWN, UI_CREAM);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "ABOUT");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* Scrollable info area */
    s_about_area = lv_obj_create(scr);
    lv_obj_t *area = s_about_area;
    lv_obj_set_width(area, lv_pct(100));
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_style_bg_color(area, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 4, 0);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLL_ELASTIC);

    const esp_app_desc_t *desc = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char info[576];
    snprintf(info, sizeof(info),
        "Xiaomiao ROM Loader\n"
        "Version: %s  Build: %s\n\n"
        "Chip: %s rev %d\n"
        "IDF: %s\n"
        "Cores: %d  CPU: %dMHz\n\n"
        "Flash: 4MB QIO 80MHz\n"
        "PSRAM: 8MB (VSPI)\n"
        "LCD: ST7735 %dx%d\n"
        "SD: %s %luMB\n\n"
        "ROMs: %s\n"
        "ROMs found: %d\n\n"
        "Factory: 0x10000 (696KB)\n"
        "OTA_0:  0xC0000 (3.25MB)\n\n"
        "Burning via USB is always\n"
        "available (GD32 UART bridge).\n\n"
        "Author: Jia Sui\n"
        "github.com/jsfaint/xueersi-loader",
        desc->version,
        desc->date,
        CONFIG_IDF_TARGET, chip.revision,
        esp_get_idf_version(),
        chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        LCD_H_RES, LCD_V_RES,
        s_sd_name, (unsigned long)s_sd_mb,
        ROM_DIR, s_rom_count);
    lv_obj_t *lbl = lv_label_create(area);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_text(lbl, info);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

    lv_obj_add_event_cb(area, on_about_key, LV_EVENT_KEY, group);
    lv_group_add_obj(group, area);
    lv_group_focus_obj(area);

    /* Hint */
    lv_obj_t *hint = lv_label_create(scr);
    apply_bar_style(hint, UI_BROWN, UI_CREAM);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_text(hint, "B: Back");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(scr);
}

/* ── UI: flash progress screen ─────────────────────────────────────────── */

static lv_obj_t *s_flash_bar;
static lv_obj_t *s_flash_lbl;

static void ui_show_flash(const rom_entry_t *rom)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(scr);
    apply_bar_style(title, UI_BROWN, UI_CREAM);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "FLASHING");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* spacer */
    lv_obj_t *sp1 = lv_obj_create(scr);
    lv_obj_set_height(sp1, 12);
    lv_obj_set_style_bg_opa(sp1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp1, 0, 0);

    /* ROM name */
    lv_obj_t *name = lv_label_create(scr);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_color(name, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    char namebuf[80];
    snprintf(namebuf, sizeof(namebuf), "%s", rom->name);
    lv_label_set_text(name, namebuf);

    /* size info */
    char szbuf[32];
    format_size(szbuf, sizeof(szbuf), rom->app_size);
    lv_obj_t *szlbl = lv_label_create(scr);
    lv_obj_set_width(szlbl, lv_pct(100));
    lv_obj_set_style_text_color(szlbl, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_align(szlbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(szlbl, szbuf);

    lv_obj_t *sp2 = lv_obj_create(scr);
    lv_obj_set_height(sp2, 8);
    lv_obj_set_style_bg_opa(sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp2, 0, 0);

    /* progress bar */
    s_flash_bar = lv_bar_create(scr);
    lv_obj_set_width(s_flash_bar, 140);
    lv_obj_set_height(s_flash_bar, 14);
    lv_obj_set_style_bg_color(s_flash_bar, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_opa(s_flash_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_flash_bar, lv_color_hex(UI_GREEN),
                              LV_PART_INDICATOR);
    lv_bar_set_range(s_flash_bar, 0, 100);
    lv_bar_set_value(s_flash_bar, 0, LV_ANIM_OFF);

    /* progress text */
    s_flash_lbl = lv_label_create(scr);
    lv_obj_set_width(s_flash_lbl, lv_pct(100));
    lv_obj_set_style_text_color(s_flash_lbl, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_align(s_flash_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_flash_lbl, "0%");

    /* warning at bottom */
    lv_obj_t *warn = lv_label_create(scr);
    apply_bar_style(warn, UI_RED, UI_CREAM);
    lv_obj_set_width(warn, lv_pct(100));
    lv_label_set_text(warn, "Do not power off!");
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(scr);
    lv_refr_now(NULL);
}

static void ui_update_flash(int pct, size_t done, size_t total)
{
    if (s_flash_bar) lv_bar_set_value(s_flash_bar, pct, LV_ANIM_OFF);
    if (s_flash_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d%%  %zu/%zu KB",
                 pct, done / 1024, total / 1024);
        lv_label_set_text(s_flash_lbl, buf);
    }
}

static void ui_flash_result(bool ok, const char *detail)
{
    if (s_flash_lbl) {
        char buf[128];
        if (ok)
            snprintf(buf, sizeof(buf), "Done! Rebooting...");
        else
            snprintf(buf, sizeof(buf), "FAILED: %s", detail ? detail : "?");
        lv_label_set_text(s_flash_lbl, buf);
    }
    lv_refr_now(NULL);
}

static void ui_show_skip(const rom_entry_t *rom)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(scr);
    apply_bar_style(title, UI_GREEN, UI_CREAM);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "BOOTING");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *name = lv_label_create(scr);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_color(name, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_label_set_text(name, rom->name);

    lv_obj_t *info = lv_label_create(scr);
    lv_obj_set_width(info, lv_pct(100));
    lv_obj_set_style_text_color(info, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(info, "Already in flash\nSkipping write");

    lv_screen_load(scr);
    lv_refr_now(NULL);
}

/* ── LVGL task + app_main ──────────────────────────────────────────────── */

static void lvgl_task(void *arg)
{
    lv_group_t *group = (lv_group_t *)arg;

    ESP_LOGI(TAG, "ROM Loader start");
    sd_try_mount();
    s_rom_count = rom_scan();
    ota0_load_state();

    ui_build_main(group);
    s_lcd_first_flush_done = false;
    lv_refr_now(NULL);
    for (uint8_t i = 0; i < 100 && !s_lcd_first_flush_done; ++i)
        vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();

    while (true) {
        if (s_flash_rom_idx >= 0) {
            int idx = s_flash_rom_idx;
            s_flash_rom_idx = -1;
            if (idx < s_rom_count && s_roms[idx].valid) {
                const rom_entry_t *rom = &s_roms[idx];

                if (ota0_rom_matches(rom)) {
                    /* ROM already in ota_0 — skip flash, just boot */
                    ESP_LOGI(TAG, "ROM %s already in ota_0, skipping write",
                             rom->name);
                    ui_show_skip(rom);
                    const esp_partition_t *part =
                        esp_partition_find_first(
                            ESP_PARTITION_TYPE_APP,
                            ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
                    if (part)
                        esp_ota_set_boot_partition(part);
                    vTaskDelay(pdMS_TO_TICKS(800));
                    sd_deinit();
                    esp_restart();
                } else {
                    /* normal flash write */
                    ui_show_flash(rom);
                    esp_err_t err = rom_flash_ota(rom);
                    if (err == ESP_OK) {
                        ota0_save_state(rom);
                        ui_flash_result(true, NULL);
                        vTaskDelay(pdMS_TO_TICKS(800));
                        sd_deinit();
                        esp_restart();
                    } else {
                        ui_flash_result(false, esp_err_to_name(err));
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        if (s_main_screen)
                            lv_screen_load(s_main_screen);
                    }
                }
            }
        }

        uint32_t delay_ms = lv_timer_handler();
        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Xiaomiao ROM Loader boot");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    buttons_init();

    esp_lcd_panel_io_handle_t io = lcd_init();

    lv_init();
    lv_display_t *disp = lvgl_display_init(io);
    lv_group_t *group = lvgl_input_init(disp);

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                                             LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, group,
                LVGL_TASK_PRIORITY, NULL);
}
