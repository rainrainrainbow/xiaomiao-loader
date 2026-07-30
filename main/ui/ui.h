/*
 * Lightweight UI framework for Xiaomiao Loader
 * Replaces LVGL with direct framebuffer rendering on ST7735
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_io.h"

/* Logical screen dimensions (rotated 90 deg from native 128x160) */
#define SCR_W       160
#define SCR_H       128
#define SCR_PX_SZ   2       /* RGB565 = 2 bytes/pixel */
#define FB_SIZE     (SCR_W * SCR_H * SCR_PX_SZ)

/* Color type: RGB565 byte-swapped for ST7735 SPI */
typedef uint16_t ui_color_t;

/* Predefined colors (byte-swapped RGB565) */
#define C_BLACK     0x0000
#define C_WHITE     0xFFFF
#define C_YELLOW    0x07E0  /* swap(0xF6D34A) approx */
#define C_BROWN     0x4209  /* swap(0x5C4220) approx */
#define C_CREAM     0xFFC7  /* swap(0xFFF3B0) approx */
#define C_RED       0x001F  /* swap(0x00F8) approx */
#define C_GREEN     0x07E0
#define C_DARK      0x1082  /* dark brown bg */
#define C_GRAY      0x7BEF
#define C_BLUE      0xF800  /* swap(0x001F) */
#define C_ORANGE    0x052F  /* swap(0xFD20) approx */

/* ---- Framebuffer ---- */
void ui_init(esp_lcd_panel_io_handle_t io);
void ui_clear(ui_color_t color);
void ui_flush(void);
uint16_t *ui_get_fb(void);

/* ---- Drawing primitives ---- */
void ui_pixel(int x, int y, ui_color_t color);
void ui_hline(int x, int y, int w, ui_color_t color);
void ui_vline(int x, int y, int h, ui_color_t color);
void ui_rect(int x, int y, int w, int h, ui_color_t color);
void ui_fill_rect(int x, int y, int w, int h, ui_color_t color);
void ui_char(int x, int y, char c, ui_color_t fg, ui_color_t bg);
void ui_text(int x, int y, const char *s, ui_color_t fg, ui_color_t bg);
/* centered text within a width band */
void ui_text_center(int y, const char *s, ui_color_t fg, ui_color_t bg);
/* truncated text to fit max_w pixels */
void ui_text_clip(int x, int y, const char *s, int max_w, ui_color_t fg, ui_color_t bg);

/* ---- Progress bar ---- */
void ui_progress(int x, int y, int w, int h, int pct, ui_color_t fg, ui_color_t bg);

/* ---- Helper: RGB888 to RGB565 byte-swapped ---- */
static inline ui_color_t ui_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);  /* byte-swap for ST7735 */
}

#endif /* UI_H */
