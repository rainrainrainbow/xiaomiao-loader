/*
 * Lightweight UI implementation
 * Direct framebuffer rendering for ST7735 160x128 RGB565
 */
#include "ui.h"
#include "ui_font.h"
#include <string.h>
#include "esp_lcd_panel_io.h"
#include "esp_log.h"

static const char *TAG = "ui";
static uint16_t *s_fb = NULL;  /* 160x128 RGB565 buffer in PSRAM */
static esp_lcd_panel_io_handle_t s_io = NULL;

/* ST7735 register addresses */
#define ST7735_CASET  0x2A
#define ST7735_RASET  0x2B
#define ST7735_RAMWR  0x2C

/* LCD gap (physical offset after rotation) */
#define LCD_X_GAP 0
#define LCD_Y_GAP 0

void ui_init(esp_lcd_panel_io_handle_t io)
{
    s_io = io;
    /* Allocate framebuffer in PSRAM */
    s_fb = heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "Failed to alloc framebuffer");
        return;
    }
    memset(s_fb, 0, FB_SIZE);
    ESP_LOGI(TAG, "UI init: fb=%p size=%d", s_fb, FB_SIZE);
}

uint16_t *ui_get_fb(void)
{
    return s_fb;
}

void ui_clear(ui_color_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < SCR_W * SCR_H; i++) {
        s_fb[i] = color;
    }
}

void ui_flush(void)
{
    if (!s_fb || !s_io) return;
    const uint8_t caset[] = {LCD_X_GAP>>8, LCD_X_GAP&0xFF,
                             (LCD_X_GAP+SCR_W-1)>>8, (LCD_X_GAP+SCR_W-1)&0xFF};
    const uint8_t raset[] = {LCD_Y_GAP>>8, LCD_Y_GAP&0xFF,
                             (LCD_Y_GAP+SCR_H-1)>>8, (LCD_Y_GAP+SCR_H-1)&0xFF};
    esp_lcd_panel_io_tx_param(s_io, ST7735_CASET, caset, 4);
    esp_lcd_panel_io_tx_param(s_io, ST7735_RASET, raset, 4);
    esp_lcd_panel_io_tx_color(s_io, ST7735_RAMWR, s_fb, FB_SIZE);
}

void ui_pixel(int x, int y, ui_color_t color)
{
    if (!s_fb || x<0 || x>=SCR_W || y<0 || y>=SCR_H) return;
    s_fb[y*SCR_W + x] = color;
}

void ui_hline(int x, int y, int w, ui_color_t color)
{
    if (!s_fb || y<0 || y>=SCR_H) return;
    if (x < 0) { w += x; x = 0; }
    if (x+w > SCR_W) w = SCR_W - x;
    for (int i = 0; i < w; i++) s_fb[y*SCR_W + x + i] = color;
}

void ui_vline(int x, int y, int h, ui_color_t color)
{
    if (!s_fb || x<0 || x>=SCR_W) return;
    if (y < 0) { h += y; y = 0; }
    if (y+h > SCR_H) h = SCR_H - y;
    for (int i = 0; i < h; i++) s_fb[(y+i)*SCR_W + x] = color;
}

void ui_rect(int x, int y, int w, int h, ui_color_t color)
{
    ui_hline(x, y, w, color);
    ui_hline(x, y+h-1, w, color);
    ui_vline(x, y, h, color);
    ui_vline(x+w-1, y, h, color);
}

void ui_fill_rect(int x, int y, int w, int h, ui_color_t color)
{
    if (!s_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x+w > SCR_W) w = SCR_W - x;
    if (y+h > SCR_H) h = SCR_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *row = &s_fb[(y+j)*SCR_W + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void ui_char(int x, int y, char c, ui_color_t fg, ui_color_t bg)
{
    if (!s_fb || c < 32 || c > 126) return;
    const uint8_t *glyph = font_6x8[c - 32];
    for (int col = 0; col < FONT_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            int px = x + col;
            int py = y + row;
            if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H) {
                s_fb[py*SCR_W + px] = (bits & (1<<row)) ? fg : bg;
            }
        }
    }
}

void ui_text(int x, int y, const char *s, ui_color_t fg, ui_color_t bg)
{
    while (*s) {
        ui_char(x, y, *s, fg, bg);
        x += FONT_W;
        s++;
    }
}

void ui_text_center(int y, const char *s, ui_color_t fg, ui_color_t bg)
{
    int len = strlen(s);
    int w = len * FONT_W;
    int x = (SCR_W - w) / 2;
    ui_text(x, y, s, fg, bg);
}

void ui_text_clip(int x, int y, const char *s, int max_w, ui_color_t fg, ui_color_t bg)
{
    int max_chars = max_w / FONT_W;
    int len = strlen(s);
    if (len <= max_chars) {
        ui_text(x, y, s, fg, bg);
    } else {
        /* truncate with ... */
        char buf[32];
        int copy = max_chars - 3;
        if (copy < 1) copy = 1;
        memcpy(buf, s, copy);
        buf[copy] = '.'; buf[copy+1] = '.'; buf[copy+2] = '.';
        buf[copy+3] = '\0';
        ui_text(x, y, buf, fg, bg);
    }
}

void ui_progress(int x, int y, int w, int h, int pct, ui_color_t fg, ui_color_t bg)
{
    ui_fill_rect(x, y, w, h, bg);
    int fill = (w * pct) / 100;
    if (fill > 0) ui_fill_rect(x, y, fill, h, fg);
    ui_rect(x, y, w, h, fg);
}
