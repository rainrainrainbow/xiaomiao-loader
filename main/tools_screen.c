#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "tools_screen.h"
#include "wifi_file_manager.h"
#include "local_file_manager.h"

static const char *TAG = "tools_screen";

#define UI_YELLOW   0xF6D34A
#define UI_BLACK    0x1B1713
#define UI_BROWN    0x5C4220
#define UI_RED      0xE64B3C
#define UI_CREAM    0xFFF3B0
#define UI_GREEN    0x2DD466

static lv_obj_t *s_tools_screen = NULL;
static lv_obj_t *s_wifi_btn = NULL;
static lv_obj_t *s_wifi_status = NULL;
static lv_obj_t *s_local_fm_btn = NULL;
static lv_obj_t *s_back_btn = NULL;
static void (*s_return_to_main_cb)(lv_group_t *group) = NULL;

static void apply_bar_style_ts(lv_obj_t *label, uint32_t bg, uint32_t fg) {
    lv_obj_set_style_bg_color(label, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
    lv_obj_set_style_pad_hor(label, 4, 0);
}

void tools_screen_update_wifi_status(void) {
    if (!s_wifi_status) return;
    if (wifi_file_manager_is_running()) {
        const char *ip = wifi_file_manager_get_ip();
        char buf[64];
        snprintf(buf, sizeof(buf), "WiFi: ON  IP: %s", ip);
        lv_label_set_text(s_wifi_status, buf);
        if (s_wifi_btn) lv_label_set_text(lv_obj_get_child(s_wifi_btn, 0), "\xe2\x8f\xb9 Stop WiFi");
    } else {
        lv_label_set_text(s_wifi_status, "WiFi: OFF");
        if (s_wifi_btn) lv_label_set_text(lv_obj_get_child(s_wifi_btn, 0), "\xf0\x9f\x93\xa1 Start WiFi");
    }
}

void tools_screen_show(lv_group_t *group) {
    if (s_tools_screen) {
        lv_screen_load(s_tools_screen);
        if (s_wifi_btn) lv_group_focus_obj(s_wifi_btn);
        tools_screen_update_wifi_status();
        return;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    s_tools_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_t *title = lv_label_create(scr);
    apply_bar_style_ts(title, UI_BROWN, UI_CREAM);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "TOOLS");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    s_wifi_status = lv_label_create(scr);
    apply_bar_style_ts(s_wifi_status, UI_YELLOW, UI_BROWN);
    lv_obj_set_width(s_wifi_status, lv_pct(100));
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_10, 0);
    lv_label_set_text(s_wifi_status, "WiFi: OFF");
    lv_obj_t *menu = lv_obj_create(scr);
    lv_obj_set_width(menu, lv_pct(100));
    lv_obj_set_flex_grow(menu, 1);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(menu, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_set_style_pad_all(menu, 4, 0);
    lv_obj_set_style_pad_row(menu, 4, 0);
    lv_obj_set_scrollbar_mode(menu, LV_SCROLLBAR_MODE_AUTO);
    s_wifi_btn = lv_button_create(menu);
    lv_obj_set_width(s_wifi_btn, lv_pct(100));
    lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_opa(s_wifi_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_btn, 0, 0);
    lv_obj_set_style_pad_ver(s_wifi_btn, 8, 0);
    lv_obj_set_style_pad_hor(s_wifi_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_wifi_btn, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_wifi_btn, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_color(s_wifi_btn, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
    lv_obj_t *wifi_lbl = lv_label_create(s_wifi_btn);
    lv_label_set_text(wifi_lbl, "\xf0\x9f\x93\xa1 Start WiFi");
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(wifi_lbl);
    lv_group_add_obj(group, s_wifi_btn);
    s_local_fm_btn = lv_button_create(menu);
    lv_obj_set_width(s_local_fm_btn, lv_pct(100));
    lv_obj_set_style_bg_color(s_local_fm_btn, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_opa(s_local_fm_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_local_fm_btn, 0, 0);
    lv_obj_set_style_pad_ver(s_local_fm_btn, 8, 0);
    lv_obj_set_style_pad_hor(s_local_fm_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_local_fm_btn, 0, 0);
    lv_obj_set_style_bg_color(s_local_fm_btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_local_fm_btn, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_color(s_local_fm_btn, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
    lv_obj_t *fm_lbl = lv_label_create(s_local_fm_btn);
    lv_label_set_text(fm_lbl, "\xf0\x9f\x93\x81 Local File Manager");
    lv_obj_set_style_text_font(fm_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(fm_lbl);
    lv_group_add_obj(group, s_local_fm_btn);
    lv_obj_t *spacer = lv_obj_create(menu);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    s_back_btn = lv_button_create(menu);
    lv_obj_set_width(s_back_btn, lv_pct(100));
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_set_style_pad_ver(s_back_btn, 8, 0);
    lv_obj_set_style_pad_hor(s_back_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
    lv_obj_t *back_lbl = lv_label_create(s_back_btn);
    lv_label_set_text(back_lbl, "\xe2\xac\x85\xef\xb8\x8f Back to ROM Loader");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(back_lbl);
    lv_group_add_obj(group, s_back_btn);
    lv_obj_t *hint = lv_label_create(scr);
    apply_bar_style_ts(hint, UI_BROWN, UI_CREAM);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_text(hint, "A:Select  B:Back");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_screen_load(scr);
    if (s_wifi_btn) lv_group_focus_obj(s_wifi_btn);
    tools_screen_update_wifi_status();
}

void tools_screen_set_return_cb(void (*cb)(lv_group_t *group)) {
    s_return_to_main_cb = cb;
}