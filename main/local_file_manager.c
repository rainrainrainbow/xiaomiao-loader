#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_err.h"

#include "local_file_manager.h"

__attribute__((unused)) static const char *TAG = "local_fm";

#define UI_YELLOW   0xF6D34A
#define UI_BLACK    0x1B1713
#define UI_BROWN    0x5C4220
#define UI_RED      0xE64B3C
#define UI_CREAM    0xFFF3B0
#define UI_GREEN    0x2DD466

static lv_obj_t *s_fm_screen = NULL;
static lv_obj_t *s_file_list = NULL;
static lv_obj_t *s_path_label = NULL;
static lv_group_t *s_fm_group = NULL;
static char s_current_dir[2048] = "/sdcard";

static void normalize_path(char *buf, size_t bufsz) {
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';
}

static void join_path(char *buf, size_t bufsz, const char *a, const char *b) {
    snprintf(buf, bufsz, "%s/%s", a, b);
    normalize_path(buf, bufsz);
}

static void format_size_lfm(char *buf, size_t bufsz, size_t bytes) {
    if (bytes >= 1024 * 1024)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, bufsz, "%.1f KB", (double)bytes / 1024);
    else
        snprintf(buf, bufsz, "%zu B", bytes);
}

static void apply_bar_style_lfm(lv_obj_t *label, uint32_t bg, uint32_t fg) {
    lv_obj_set_style_bg_color(label, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
    lv_obj_set_style_pad_hor(label, 4, 0);
}

static void enter_dir(const char *path) {
    snprintf(s_current_dir, sizeof(s_current_dir), "%s", path);
    normalize_path(s_current_dir, sizeof(s_current_dir));
}

static void go_up(void) {
    char *last_slash = strrchr(s_current_dir, '/');
    if (last_slash && last_slash != s_current_dir) *last_slash = '\0';
}

static void on_file_clicked(lv_event_t *e) {
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    char full_path[2048];
    join_path(full_path, sizeof(full_path), s_current_dir, name);
    struct stat st;
    if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        enter_dir(full_path);
        local_fm_show(s_fm_group);
    }
}

__attribute__((unused)) static void on_file_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    lv_group_t *grp = (lv_group_t *)lv_event_get_user_data(e);
    if (key == LV_KEY_ESC) { go_up(); local_fm_show(grp); }
    else if (key == LV_KEY_UP) { lv_group_focus_prev(grp); lv_obj_t *f = lv_group_get_focused(grp); if (f) lv_obj_scroll_to_view(f, LV_ANIM_OFF); }
    else if (key == LV_KEY_DOWN) { lv_group_focus_next(grp); lv_obj_t *f = lv_group_get_focused(grp); if (f) lv_obj_scroll_to_view(f, LV_ANIM_OFF); }
    else if (key == LV_KEY_ENTER) {
        lv_obj_t *focused = lv_group_get_focused(grp);
        if (focused) {
            const char *name = (const char *)lv_obj_get_user_data(focused);
            if (name) {
                char full_path[2048];
                join_path(full_path, sizeof(full_path), s_current_dir, name);
                struct stat st;
                if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    enter_dir(full_path);
                    local_fm_show(grp);
                }
            }
        }
    }
}

void local_fm_show(lv_group_t *group) {
    s_fm_group = group;
    if (s_fm_screen) { lv_screen_load(s_fm_screen); lv_obj_clean(s_file_list); }
    else {
        lv_obj_t *scr = lv_obj_create(NULL);
        s_fm_screen = scr;
        lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(scr, 0, 0);
        lv_obj_t *title = lv_label_create(scr);
        apply_bar_style_lfm(title, UI_BROWN, UI_CREAM);
        lv_obj_set_width(title, lv_pct(100));
        lv_label_set_text(title, "FILE MANAGER");
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        s_path_label = lv_label_create(scr);
        apply_bar_style_lfm(s_path_label, UI_YELLOW, UI_BROWN);
        lv_obj_set_width(s_path_label, lv_pct(100));
        lv_obj_set_style_text_font(s_path_label, &lv_font_montserrat_10, 0);
        s_file_list = lv_obj_create(scr);
        lv_obj_set_width(s_file_list, lv_pct(100));
        lv_obj_set_flex_grow(s_file_list, 1);
        lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(s_file_list, lv_color_hex(UI_YELLOW), 0);
        lv_obj_set_style_bg_opa(s_file_list, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_file_list, 0, 0);
        lv_obj_set_style_pad_all(s_file_list, 2, 0);
        lv_obj_set_style_pad_row(s_file_list, 1, 0);
        lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_clear_flag(s_file_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_t *hint = lv_label_create(scr);
        apply_bar_style_lfm(hint, UI_BROWN, UI_CREAM);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "A:Open  B:Back");
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_screen_load(scr);
    }
    lv_label_set_text(s_path_label, s_current_dir);
    lv_obj_clean(s_file_list);
    DIR *dir = opendir(s_current_dir);
    if (!dir) {
        lv_obj_t *lbl = lv_label_create(s_file_list);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_RED), 0);
        lv_label_set_text(lbl, "Cannot open directory");
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    if (strcmp(s_current_dir, "/sdcard") != 0) {
        lv_obj_t *btn = lv_button_create(s_file_list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_ver(btn, 3, 0);
        lv_obj_set_style_pad_hor(btn, 6, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_BROWN), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "\xf0\x9f\x93\x81 ../");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, on_file_clicked, LV_EVENT_SHORT_CLICKED, (void *)"..");
        lv_group_add_obj(group, btn);
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full_path[2048];
        join_path(full_path, sizeof(full_path), s_current_dir, ent->d_name);
        struct stat st;
        bool is_dir = false;
        if (stat(full_path, &st) == 0) is_dir = S_ISDIR(st.st_mode);
        lv_obj_t *btn = lv_button_create(s_file_list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_ver(btn, 3, 0);
        lv_obj_set_style_pad_hor(btn, 6, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_BROWN), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
        char label_text[512];
        if (is_dir) snprintf(label_text, sizeof(label_text), "\xf0\x9f\x93\x81 %s/", ent->d_name);
        else {
            char sz[32];
            format_size_lfm(sz, sizeof(sz), st.st_size);
            snprintf(label_text, sizeof(label_text), "\xf0\x9f\x93\x84 %s  [%s]", ent->d_name, sz);
        }
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label_text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        char *name_copy = strdup(ent->d_name);
        lv_obj_set_user_data(btn, name_copy);
        lv_obj_add_event_cb(btn, on_file_clicked, LV_EVENT_SHORT_CLICKED, name_copy);
        lv_group_add_obj(group, btn);
    }
    closedir(dir);
    lv_group_focus_obj(lv_obj_get_child(s_file_list, 0));
}
