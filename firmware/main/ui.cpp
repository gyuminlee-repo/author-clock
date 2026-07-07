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
extern const lv_font_t font_ko_28;       // quote body (largest) + source
extern const lv_font_t font_ko_22;       // quote body auto-fit step
extern const lv_font_t font_ko_18;       // quote body auto-fit step (smallest)
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

// Clock-screen quote layout, two modes re-evaluated on every quote change:
//
// Normal: 96px time + expression chip; quote box sits between the chip and
//   the 28px source label. Ladder 28 -> 22 -> 18 px must fit ~84px.
// Compact (long quotes): time shrinks to font_ko_44 at the top, the chip is
//   hidden (the expression is contained in the full quote text; longest chip
//   is 20 chars and cannot share the top row), the source label drops to
//   18px, and the quote box grows to ~217px. Ladder 22 -> 18 px. 18px
//   overflow beyond even the compact box falls back to dot-truncation.
static const int32_t QUOTE_W = 380;

// Normal mode: source font_ko_28 (line_height 29) at BOTTOM_MID -6.
static const int32_t QUOTE_TOP_N = 178;
static const int32_t QUOTE_BOT_N = LCD_HEIGHT - 6 - 29 - 3;     // 262
static const int32_t QUOTE_H_N   = QUOTE_BOT_N - QUOTE_TOP_N;   // 84

// Compact mode: time font_ko_44 (line_height 45) at y=8 ends at 53;
// source font_ko_18 (line_height 19) at BOTTOM_MID -4 starts at 277.
static const int32_t QUOTE_TOP_C = 58;
static const int32_t QUOTE_BOT_C = LCD_HEIGHT - 4 - 19 - 2;     // 275
static const int32_t QUOTE_H_C   = QUOTE_BOT_C - QUOTE_TOP_C;   // 217

// Auto-fit ladders, largest first.
static const lv_font_t *const QUOTE_FONTS_N[] = { &font_ko_28, &font_ko_22, &font_ko_18 };
static const int QUOTE_FONT_N_CNT = 3;
static const lv_font_t *const QUOTE_FONTS_C[] = { &font_ko_22, &font_ko_18 };
static const int QUOTE_FONT_C_CNT = 2;

// Move the time / quote / source widgets between the two layouts. The chip
// visibility is handled by ui_set_quote (hidden whenever compact).
static void apply_clock_layout(bool compact) {
    if (compact) {
        // 44px "00:00" is ~130px wide centered (x ~135-265), clear of the
        // 72x72 cat icon at x >= 322, so the icon stays as-is.
        lv_obj_set_style_text_font(lbl_time, &font_ko_44, 0);
        lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_height(lbl_quote, QUOTE_H_C);
        lv_obj_align(lbl_quote, LV_ALIGN_TOP_MID, 0, QUOTE_TOP_C);
        lv_obj_set_style_text_font(lbl_source, &font_ko_18, 0);
        lv_obj_align(lbl_source, LV_ALIGN_BOTTOM_MID, 0, -4);
    } else {
        lv_obj_set_style_text_font(lbl_time, &font_digits_96, 0);
        lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, -14, 20);
        lv_obj_set_height(lbl_quote, QUOTE_H_N);
        lv_obj_align(lbl_quote, LV_ALIGN_TOP_MID, 0, QUOTE_TOP_N);
        lv_obj_set_style_text_font(lbl_source, &font_ko_28, 0);
        lv_obj_align(lbl_source, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
}

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

    // Quote body: full text via auto-fit font (see ui_set_quote). Fixed box so
    // measurement and layout agree. letter/line space pinned to 0 so the
    // lv_text_get_size() fit check matches what the label actually renders.
    lbl_quote = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_quote, &font_ko_28, 0);
    lv_obj_set_style_text_color(lbl_quote, lv_color_black(), 0);
    lv_obj_set_style_text_letter_space(lbl_quote, 0, 0);
    lv_obj_set_style_text_line_space(lbl_quote, 0, 0);
    lv_label_set_long_mode(lbl_quote, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_quote, QUOTE_W);
    lv_obj_set_height(lbl_quote, QUOTE_H_N);
    lv_obj_set_style_text_align(lbl_quote, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_quote, "");
    lv_obj_align(lbl_quote, LV_ALIGN_TOP_MID, 0, QUOTE_TOP_N);

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
        apply_clock_layout(false);
        lv_obj_add_flag(lbl_hl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_quote, "");
        lv_label_set_text(lbl_source, "");
        return;
    }

    // Auto-fit, re-evaluated per quote: try the normal box (big time + chip)
    // with 28 -> 22 -> 18px; if none fit, switch to the compact layout (44px
    // time, no chip, bigger box) with 22 -> 18px. Dot-truncate only if the
    // quote overflows even the compact box at 18px.
    const lv_font_t *chosen = NULL;
    bool compact = false;
    bool overflow = false;
    for (int i = 0; i < QUOTE_FONT_N_CNT; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, q->q, QUOTE_FONTS_N[i], 0, 0, QUOTE_W, LV_TEXT_FLAG_NONE);
        if (sz.y <= QUOTE_H_N) { chosen = QUOTE_FONTS_N[i]; break; }
    }
    if (!chosen) {
        compact = true;
        for (int i = 0; i < QUOTE_FONT_C_CNT; i++) {
            lv_point_t sz;
            lv_text_get_size(&sz, q->q, QUOTE_FONTS_C[i], 0, 0, QUOTE_W, LV_TEXT_FLAG_NONE);
            if (sz.y <= QUOTE_H_C) { chosen = QUOTE_FONTS_C[i]; break; }
        }
        if (!chosen) {   // safety net, arithmetically unreachable with data
            chosen = QUOTE_FONTS_C[QUOTE_FONT_C_CNT - 1];
            overflow = true;
        }
    }
    apply_clock_layout(compact);

    // Chip only in normal mode; compact mode trades it for quote space.
    if (!compact && q->t && q->t[0] != '\0') {
        // Long expressions would overflow the 400px panel (auto-sized label);
        // clamp to a fixed width with dot-truncation only when needed.
        lv_point_t tsz;
        lv_text_get_size(&tsz, q->t, &font_ko_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (tsz.x > QUOTE_W - 16) {
            lv_obj_set_width(lbl_hl, QUOTE_W);
            lv_label_set_long_mode(lbl_hl, LV_LABEL_LONG_DOT);
        } else {
            lv_obj_set_width(lbl_hl, LV_SIZE_CONTENT);
            lv_label_set_long_mode(lbl_hl, LV_LABEL_LONG_WRAP);
        }
        lv_label_set_text(lbl_hl, q->t);
        lv_obj_align(lbl_hl, LV_ALIGN_TOP_MID, 0, 140);
        lv_obj_remove_flag(lbl_hl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_hl, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_text_font(lbl_quote, chosen, 0);
    lv_label_set_long_mode(lbl_quote,
                           overflow ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
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
