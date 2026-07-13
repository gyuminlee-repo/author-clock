#include <stdio.h>
#include <string.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
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
extern const lv_image_dsc_t therm_icon;   // top-left temperature glyph, 20x20
extern const lv_image_dsc_t drop_icon;    // top-left humidity glyph, 20x20
}

static const char *TAG = "UI";

// Screens
static lv_obj_t *scr_clock;
static lv_obj_t *scr_cal;

// Clock widgets
static lv_obj_t *lbl_time;      // big HH:MM
static lv_obj_t *canvas_quote;  // quote body, drawn with inline inverted highlight
static uint8_t  *canvas_buf;    // RGB565 canvas backing buffer (PSRAM)
static lv_obj_t *lbl_source;    // work + author

// Top-left environment readout (mirrors the top-right cat icon).
static lv_obj_t *therm_img;     // thermometer glyph, row 1
static lv_obj_t *lbl_temp;      // temperature value, row 1
static lv_obj_t *drop_img;      // water-drop glyph, row 2
static lv_obj_t *lbl_humi;      // humidity value, row 2

// Calendar widgets
static lv_obj_t *lbl_cal_head;
static lv_obj_t *cal_wday[7];
static lv_obj_t *cal_cell[42];

static const char *WDAY_KO[7] = { "일", "월", "화", "수", "목", "금", "토" };

static bool showing_calendar = false;

// Clock-screen quote layout, two modes re-evaluated on every quote change:
//
// Normal: 96px time; quote box sits between the time and the 28px source
//   label. The time expression is highlighted inline inside the quote (black
//   box + white letters), so no separate chip is needed. Ladder 28 -> 22 ->
//   18 px must fit ~112px.
// Compact (long quotes): time shrinks to font_ko_44 at the top, the source
//   label drops to 18px, and the quote box grows to ~217px. Ladder 22 -> 18px.
//
// The quote is rendered onto ONE fixed RGB565 canvas sized to the compact box
// and repositioned per mode; only the drawn box height differs and the unused
// area is filled white. In normal mode the canvas overhangs the source label,
// so the source is created AFTER the canvas to stay on top. Do not reorder the
// widget creation in build_clock_screen.
static const int32_t QUOTE_W  = 380;
static const int32_t CANVAS_W = 380;
static const int32_t CANVAS_H = 217;

// Normal mode: source is now always font_ko_18 (line_height 19) at BOTTOM_MID
// -6, so the quote box reclaims the space the old 28px source used and grows
// downward (bottom 262 -> 272). It also grows upward: the gap under the 96px
// clock (glyph bottom ~120) was halved, so the box top moved 150 -> 132. The
// wider box lets the auto-fit ladder pick larger fonts more often.
static const int32_t QUOTE_TOP_N = 132;
static const int32_t QUOTE_BOT_N = LCD_HEIGHT - 6 - 19 - 3;     // 272
static const int32_t QUOTE_H_N   = QUOTE_BOT_N - QUOTE_TOP_N;   // 140

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

// Per-font line pitch; includes headroom so the highlight box does not clip
// the line above/below.
static int32_t quote_line_h(const lv_font_t *f) {
    if (f == &font_ko_28) return 34;
    if (f == &font_ko_22) return 28;
    if (f == &font_ko_18) return 23;
    return lv_font_get_line_height(f) + 4;
}

#define MAX_QLINES 12

// Decode one UTF-8 char at s, return its byte length, write codepoint to *cp.
static int utf8_next(const char *s, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
              ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        *cp = ((uint32_t)(c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
              (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

// Manual word-agnostic wrap (Korean breaks anywhere). Fills per-line byte
// ranges [starts[k], ends[k]); leading spaces at each line start are skipped.
// Returns the line count (capped at MAX_QLINES). Both the fit measurement and
// the render use this so they never disagree.
static int wrap_quote(const char *text, const lv_font_t *font, int32_t box_w,
                      uint32_t *starts, uint32_t *ends) {
    int n = 0;
    uint32_t i = 0;
    while (text[i] != '\0' && n < MAX_QLINES) {
        while (text[i] == ' ') i++;                 // skip leading spaces
        if (text[i] == '\0') break;
        starts[n] = i;
        int32_t w = 0;
        while (text[i] != '\0' && text[i] != '\n') {
            uint32_t cp;
            int nb = utf8_next(&text[i], &cp);
            int32_t gw = lv_font_get_glyph_width(font, cp, 0);
            if (w + gw > box_w && i > starts[n]) break;   // wrap before this char
            w += gw;
            i += nb;
        }
        ends[n] = i;
        n++;
        if (text[i] == '\n') i++;
    }
    return n;
}

static int quote_nlines(const char *text, const lv_font_t *font, int32_t box_w) {
    uint32_t s[MAX_QLINES], e[MAX_QLINES];
    return wrap_quote(text, font, box_w, s, e);
}

// Draw the quote onto the canvas: vertically centered block, each line
// horizontally centered. Glyphs in [hl_start, hl_end) get a black box + white
// letter (inline inverted highlight); the rest are plain black on white.
static void render_quote(lv_obj_t *canvas, const char *text,
                         uint32_t hl_start, uint32_t hl_end,
                         const lv_font_t *font, int32_t line_h,
                         int32_t box_w, int32_t box_h) {
    uint32_t starts[MAX_QLINES], ends[MAX_QLINES];
    int n = wrap_quote(text, font, box_w, starts, ends);
    if (n <= 0) return;

    int32_t fh = lv_font_get_line_height(font);
    int32_t total_h = n * line_h;
    int32_t y = (box_h - total_h) / 2;
    if (y < 0) y = 0;                               // keep the top visible

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color = lv_color_black();
    rdsc.bg_opa   = LV_OPA_COVER;

    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.font  = font;
    ldsc.align = LV_TEXT_ALIGN_LEFT;

    for (int k = 0; k < n; k++) {
        int32_t lw = 0;
        for (uint32_t j = starts[k]; j < ends[k]; ) {
            uint32_t cp;
            int nb = utf8_next(&text[j], &cp);
            lw += lv_font_get_glyph_width(font, cp, 0);
            j += nb;
        }
        int32_t x = (box_w - lw) / 2;
        if (x < 0) x = 0;
        int32_t gy = y + (line_h - fh) / 2;
        if (gy < 0) gy = 0;

        for (uint32_t j = starts[k]; j < ends[k]; ) {
            uint32_t cp;
            int nb = utf8_next(&text[j], &cp);
            int32_t gw = lv_font_get_glyph_width(font, cp, 0);
            bool hl = (j >= hl_start && j < hl_end);
            if (hl) {
                lv_area_t r = { x - 1, y + 1, x + gw + 1, y + line_h - 1 };
                lv_draw_rect(&layer, &rdsc, &r);
                ldsc.color = lv_color_white();
            } else {
                ldsc.color = lv_color_black();
            }
            lv_point_t pt = { x, gy };
            lv_draw_character(&layer, &ldsc, &pt, cp);
            x += gw;
            j += nb;
        }
        y += line_h;
    }

    lv_canvas_finish_layer(canvas, &layer);
}

// Move the time / quote canvas / source widgets between the two layouts.
static void apply_clock_layout(bool compact) {
    if (compact) {
        // 44px "00:00" is ~130px wide centered (x ~135-265), clear of the
        // 72x72 cat icon at x >= 322, so the icon stays as-is.
        lv_obj_set_style_text_font(lbl_time, &font_ko_44, 0);
        lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_align(canvas_quote, LV_ALIGN_TOP_MID, 0, QUOTE_TOP_C);
        lv_obj_set_style_text_font(lbl_source, &font_ko_18, 0);
        lv_obj_align(lbl_source, LV_ALIGN_BOTTOM_MID, 0, -4);
    } else {
        // 96px "00:00" (~225px max) centered: left/right margins ~88px each,
        // clearing the top-left env block (ends x84) and the top-right 72px cat
        // icon. The old -14 nudge is gone now that both corners are balanced.
        lv_obj_set_style_text_font(lbl_time, &font_digits_96, 0);
        lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_align(canvas_quote, LV_ALIGN_TOP_MID, 0, QUOTE_TOP_N);
        // Source is always the smallest font so it never looks larger than an
        // auto-fit-shrunk quote body.
        lv_obj_set_style_text_font(lbl_source, &font_ko_18, 0);
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

    // Top-left environment readout, mirroring the top-right cat icon. Two rows
    // of [20x20 icon at x=6] + [font_ko_18 value at x=30]:
    //   row 1 y=8..28   thermometer + temperature ("23.4")
    //   row 2 y=30..50  water drop  + humidity    ("45%")
    // The block spans x 6..84, y 8..52. Overlap check: the centered 96px clock
    // has a left edge near x88 (clear), the normal quote canvas starts at
    // QUOTE_TOP_N=132 (clear), and the compact quote canvas starts at
    // QUOTE_TOP_C=58 (6px below the block's y52 bottom). Hidden until the first
    // successful sensor read; ui_set_env toggles visibility.
    therm_img = lv_image_create(scr_clock);
    lv_image_set_src(therm_img, &therm_icon);
    lv_obj_set_style_image_recolor(therm_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(therm_img, LV_OPA_COVER, 0);
    lv_obj_set_pos(therm_img, 6, 8);
    lv_obj_add_flag(therm_img, LV_OBJ_FLAG_HIDDEN);

    lbl_temp = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_temp, &font_ko_18, 0);
    lv_obj_set_style_text_color(lbl_temp, lv_color_black(), 0);
    lv_label_set_text(lbl_temp, "");
    lv_obj_set_pos(lbl_temp, 30, 9);
    lv_obj_add_flag(lbl_temp, LV_OBJ_FLAG_HIDDEN);

    drop_img = lv_image_create(scr_clock);
    lv_image_set_src(drop_img, &drop_icon);
    lv_obj_set_style_image_recolor(drop_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(drop_img, LV_OPA_COVER, 0);
    lv_obj_set_pos(drop_img, 6, 30);
    lv_obj_add_flag(drop_img, LV_OBJ_FLAG_HIDDEN);

    lbl_humi = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_humi, &font_ko_18, 0);
    lv_obj_set_style_text_color(lbl_humi, lv_color_black(), 0);
    lv_label_set_text(lbl_humi, "");
    lv_obj_set_pos(lbl_humi, 30, 31);
    lv_obj_add_flag(lbl_humi, LV_OBJ_FLAG_HIDDEN);

    // Big time: the star of the screen (~40% of height). Centered now that the
    // top-left env block and top-right cat icon balance the corners.
    lbl_time = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_time, &font_digits_96, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_black(), 0);
    lv_label_set_text(lbl_time, "00:00");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 20);

    // Quote canvas: one fixed RGB565 canvas (compact box size). Auto-fit font
    // and inline highlight are handled per quote in ui_set_quote. Created
    // BEFORE the source label so the source stays on top where the canvas
    // overhangs it in normal mode (see layout note above).
    canvas_buf = (uint8_t *)heap_caps_malloc(CANVAS_W * 2 * CANVAS_H, MALLOC_CAP_SPIRAM);
    canvas_quote = lv_canvas_create(scr_clock);
    if (canvas_buf) {
        lv_canvas_set_buffer(canvas_quote, canvas_buf, CANVAS_W, CANVAS_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(canvas_quote, lv_color_white(), LV_OPA_COVER);
    } else {
        ESP_LOGE(TAG, "quote canvas buffer alloc failed");
    }
    lv_obj_align(canvas_quote, LV_ALIGN_TOP_MID, 0, QUOTE_TOP_N);

    // Source: smallest, at the very bottom. Always font_ko_18 (see layout note)
    // so it never dwarfs an auto-fit-shrunk quote body.
    lbl_source = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_source, &font_ko_18, 0);
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
        if (canvas_buf) lv_canvas_fill_bg(canvas_quote, lv_color_white(), LV_OPA_COVER);
        lv_obj_invalidate(canvas_quote);
        lv_label_set_text(lbl_source, "");
        return;
    }

    // Auto-fit, re-evaluated per quote using the SAME manual wrap as the
    // renderer: try the normal box (big time) with 28 -> 22 -> 18px; if none
    // fit, switch to the compact layout (44px time, bigger box) with 22 ->
    // 18px. On overflow beyond the compact box the smallest font is drawn and
    // extra lines are clipped by the canvas.
    const lv_font_t *chosen = NULL;
    bool compact = false;
    for (int i = 0; i < QUOTE_FONT_N_CNT; i++) {
        const lv_font_t *f = QUOTE_FONTS_N[i];
        if (quote_nlines(q->q, f, QUOTE_W) * quote_line_h(f) <= QUOTE_H_N) {
            chosen = f;
            break;
        }
    }
    if (!chosen) {
        compact = true;
        for (int i = 0; i < QUOTE_FONT_C_CNT; i++) {
            const lv_font_t *f = QUOTE_FONTS_C[i];
            if (quote_nlines(q->q, f, QUOTE_W) * quote_line_h(f) <= QUOTE_H_C) {
                chosen = f;
                break;
            }
        }
        if (!chosen) chosen = QUOTE_FONTS_C[QUOTE_FONT_C_CNT - 1];   // overflow
    }
    apply_clock_layout(compact);

    // Inline highlight range: byte offsets of the time expression inside the
    // quote body. No match -> empty range -> all-black plain text.
    uint32_t hl_start = 0, hl_end = 0;
    if (q->t && q->t[0] != '\0') {
        const char *m = strstr(q->q, q->t);
        if (m) {
            hl_start = (uint32_t)(m - q->q);
            hl_end   = hl_start + (uint32_t)strlen(q->t);
        }
    }

    if (canvas_buf) {
        int32_t box_h = compact ? QUOTE_H_C : QUOTE_H_N;
        lv_canvas_fill_bg(canvas_quote, lv_color_white(), LV_OPA_COVER);
        render_quote(canvas_quote, q->q, hl_start, hl_end, chosen,
                     quote_line_h(chosen), CANVAS_W, box_h);
    }
    lv_obj_invalidate(canvas_quote);

    char src[160];
    const char *a = q->a ? q->a : "";
    const char *w = q->w ? q->w : "";
    if (a[0] && w[0]) snprintf(src, sizeof(src), "%s  ·  %s", a, w);
    else              snprintf(src, sizeof(src), "%s%s", a, w);
    lv_label_set_text(lbl_source, src);
}

void ui_set_env(float temp_c, float humi_pct) {
    // A failed read (NaN) hides the whole block rather than showing stale or
    // garbage numbers.
    if (isnan(temp_c) || isnan(humi_pct)) {
        lv_obj_add_flag(therm_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_temp,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(drop_img,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_humi,  LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // The font subset has no degree sign (U+00B0), so the thermometer icon
    // Font now carries degree + percent glyphs, so show explicit units.
    char t[16], h[16];
    snprintf(t, sizeof(t), "%.1f°C", temp_c);
    snprintf(h, sizeof(h), "%.0f%%", humi_pct);
    lv_label_set_text(lbl_temp, t);
    lv_label_set_text(lbl_humi, h);
    lv_obj_remove_flag(therm_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lbl_temp,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(drop_img,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lbl_humi,  LV_OBJ_FLAG_HIDDEN);
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
