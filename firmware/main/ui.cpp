#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui.h"
#include "user_config.h"

// Fonts and icon are compiled into the binary (see main/CMakeLists.txt).
extern "C" {
extern const lv_font_t font_digits_96;   // 0-9 and ':' only, big clock
extern const lv_font_t font_ko_44;       // calendar header / fallback
extern const lv_font_t font_ko_28;       // quote body + source
extern const lv_image_dsc_t cat_icon;     // top-right cat face, 72x72
extern const lv_image_dsc_t cat_icon_48;  // calendar top-right, 48x48
}

static const char *TAG = "UI";

// Screens
static lv_obj_t *scr_clock;
static lv_obj_t *scr_cal;

// Clock widgets
static lv_obj_t *lbl_time;      // big HH:MM
static lv_obj_t *lbl_hl;        // inverted time-expression chip
static lv_obj_t *lbl_quote;     // quote body
static lv_obj_t *lbl_source;    // work + author

// Calendar widgets
static lv_obj_t *lbl_cal_head;
static lv_obj_t *cal_wday[7];
static lv_obj_t *cal_cell[42];

static const char *WDAY_KO[7] = { "일", "월", "화", "수", "목", "금", "토" };

static bool showing_calendar = false;

static void style_screen_white(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- Clock screen ---------------------------------------------------------
static void build_clock_screen(void) {
    scr_clock = lv_obj_create(NULL);
    style_screen_white(scr_clock);

    // Cat icon (72x72 dithered), top-right, always on.
    lv_obj_t *icon = lv_image_create(scr_clock);
    lv_image_set_src(icon, &cat_icon);
    lv_obj_set_style_image_recolor(icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_pos(icon, LCD_WIDTH - 72 - 6, 6);

    // Big time: the star of the screen (~40% of height).
    lbl_time = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_time, &font_digits_96, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_black(), 0);
    lv_label_set_text(lbl_time, "00:00");
    // Nudged 14px left of center to clear the larger 72x72 cat icon.
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, -14, 20);

    // Inverted time-expression chip (black bg, white text).
    lbl_hl = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_hl, &font_ko_28, 0);
    lv_obj_set_style_text_color(lbl_hl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_hl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_hl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(lbl_hl, 8, 0);
    lv_obj_set_style_pad_ver(lbl_hl, 2, 0);
    lv_label_set_text(lbl_hl, "");
    lv_obj_align(lbl_hl, LV_ALIGN_TOP_MID, 0, 140);

    // Quote body: secondary, wraps, truncates with dots if too long.
    lbl_quote = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_quote, &font_ko_28, 0);
    lv_obj_set_style_text_color(lbl_quote, lv_color_black(), 0);
    lv_label_set_long_mode(lbl_quote, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_quote, 380);
    lv_obj_set_height(lbl_quote, 90);
    lv_obj_set_style_text_align(lbl_quote, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_quote, "");
    lv_obj_align(lbl_quote, LV_ALIGN_TOP_MID, 0, 178);

    // Source: smallest, at the very bottom.
    lbl_source = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_source, &font_ko_28, 0);
    lv_obj_set_style_text_color(lbl_source, lv_color_black(), 0);
    lv_label_set_long_mode(lbl_source, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_source, 380);
    lv_obj_set_style_text_align(lbl_source, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_source, "");
    lv_obj_align(lbl_source, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void ui_set_time_text(int hour, int minute) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(lbl_time, buf);
}

void ui_set_quote(const quote_t *q) {
    if (!q || !q->q || q->q[0] == '\0') {
        lv_obj_add_flag(lbl_hl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_quote, "");
        lv_label_set_text(lbl_source, "");
        return;
    }

    if (q->t && q->t[0] != '\0') {
        lv_label_set_text(lbl_hl, q->t);
        lv_obj_remove_flag(lbl_hl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_hl, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(lbl_quote, q->q);

    char src[160];
    const char *a = q->a ? q->a : "";
    const char *w = q->w ? q->w : "";
    if (a[0] && w[0]) snprintf(src, sizeof(src), "%s  ·  %s", a, w);
    else              snprintf(src, sizeof(src), "%s%s", a, w);
    lv_label_set_text(lbl_source, src);
}

// ---- Calendar screen ------------------------------------------------------
static int days_in_month(int year, int mon0) {  // mon0: 0-11
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (mon0 == 1) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return d[mon0];
}

static void build_calendar_screen(void) {
    scr_cal = lv_obj_create(NULL);
    style_screen_white(scr_cal);

    // Cat icon (48x48 dithered), top-right of the calendar.
    lv_obj_t *cal_icon = lv_image_create(scr_cal);
    lv_image_set_src(cal_icon, &cat_icon_48);
    lv_obj_set_style_image_recolor(cal_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(cal_icon, LV_OPA_COVER, 0);
    lv_obj_set_pos(cal_icon, LCD_WIDTH - 48 - 6, 4);

    lbl_cal_head = lv_label_create(scr_cal);
    lv_obj_set_style_text_font(lbl_cal_head, &font_ko_44, 0);
    lv_obj_set_style_text_color(lbl_cal_head, lv_color_black(), 0);
    lv_label_set_text(lbl_cal_head, "");
    lv_obj_align(lbl_cal_head, LV_ALIGN_TOP_MID, 0, 8);

    const int x0 = 12, y0 = 70, cw = 53, ch = 36;
    for (int c = 0; c < 7; c++) {
        cal_wday[c] = lv_label_create(scr_cal);
        lv_obj_set_style_text_font(cal_wday[c], &font_ko_28, 0);
        lv_obj_set_style_text_color(cal_wday[c], lv_color_black(), 0);
        lv_obj_set_width(cal_wday[c], cw);
        lv_obj_set_style_text_align(cal_wday[c], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(cal_wday[c], WDAY_KO[c]);
        lv_obj_set_pos(cal_wday[c], x0 + c * cw, y0);
    }

    for (int i = 0; i < 42; i++) {
        int r = i / 7, c = i % 7;
        cal_cell[i] = lv_label_create(scr_cal);
        lv_obj_set_style_text_font(cal_cell[i], &font_ko_28, 0);
        lv_obj_set_style_text_color(cal_cell[i], lv_color_black(), 0);
        lv_obj_set_style_pad_all(cal_cell[i], 2, 0);
        lv_obj_set_width(cal_cell[i], cw);
        lv_obj_set_style_text_align(cal_cell[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(cal_cell[i], "");
        lv_obj_set_pos(cal_cell[i], x0 + c * cw, y0 + 34 + r * ch);
    }
}

void ui_build_calendar(const struct tm *now) {
    if (!now) return;
    int year = now->tm_year + 1900;
    int mon0 = now->tm_mon;
    int today = now->tm_mday;

    char head[32];
    snprintf(head, sizeof(head), "%d년 %d월", year, mon0 + 1);
    lv_label_set_text(lbl_cal_head, head);

    // Weekday (0=Sun) of the first day of the month.
    int first_wday = ((now->tm_wday - (today - 1)) % 7 + 7) % 7;
    int dim = days_in_month(year, mon0);

    for (int i = 0; i < 42; i++) {
        int day = i - first_wday + 1;
        // Reset style each rebuild.
        lv_obj_set_style_bg_opa(cal_cell[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(cal_cell[i], lv_color_black(), 0);
        if (day < 1 || day > dim) {
            lv_label_set_text(cal_cell[i], "");
            continue;
        }
        char d[4];
        snprintf(d, sizeof(d), "%d", day);
        lv_label_set_text(cal_cell[i], d);
        if (day == today) {   // inverted block for today
            lv_obj_set_style_bg_color(cal_cell[i], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(cal_cell[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(cal_cell[i], lv_color_white(), 0);
        }
    }
}

// ---- Screen switching + button -------------------------------------------
static void show_screen(bool calendar) {
    showing_calendar = calendar;
    lv_screen_load(calendar ? scr_cal : scr_clock);
}

static void button_task(void *arg) {
    gpio_config_t io = {};
    io.intr_type    = GPIO_INTR_DISABLE;
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << KEY_BUTTON_PIN);
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    int last = 1;   // active low
    for (;;) {
        int lvl = gpio_get_level((gpio_num_t)KEY_BUTTON_PIN);
        if (last == 1 && lvl == 0) {          // press edge
            vTaskDelay(pdMS_TO_TICKS(50));     // debounce
            if (gpio_get_level((gpio_num_t)KEY_BUTTON_PIN) == 0) {
                if (Lvgl_lock(-1)) {
                    show_screen(!showing_calendar);
                    Lvgl_unlock();
                }
                // wait for release
                while (gpio_get_level((gpio_num_t)KEY_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        last = lvl;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_start_button_task(void) {
    xTaskCreatePinnedToCore(button_task, "KEY", 3 * 1024, NULL, 3, NULL, 1);
}

void ui_init(void) {
    build_clock_screen();
    build_calendar_screen();
    show_screen(false);
    ESP_LOGI(TAG, "UI ready");
}
