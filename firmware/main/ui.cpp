#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui.h"
#include "icon_pool.h"
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
extern const lv_image_dsc_t batt_icon;    // top-left battery glyph, 20x20
extern const lv_image_dsc_t usb_icon;     // top-left USB-plugged glyph, 20x20
extern const lv_image_dsc_t wifi_icon;    // sync toast glyph, 20x20
}

static const char *TAG = "UI";

// Screens
static lv_obj_t *scr_clock;
static lv_obj_t *scr_cal;

// Clock widgets
static lv_obj_t *lbl_time;      // big HH:MM
static lv_obj_t *lbl_date;      // date + weekday, under the clock (normal mode)
static lv_obj_t *top_icon;      // top-right pool sprite, one per minute
static lv_obj_t *canvas_quote;  // quote body, drawn with inline inverted highlight
static uint8_t  *canvas_buf;    // RGB565 canvas backing buffer (PSRAM)
static lv_obj_t *lbl_source;    // work + author

// Top-left environment readout (mirrors the top-right cat icon).
static lv_obj_t *batt_img;      // battery / USB glyph, row 1 (src swapped)
static lv_obj_t *lbl_batt;      // battery percent value, row 1
static lv_obj_t *therm_img;     // thermometer glyph, row 2
static lv_obj_t *lbl_temp;      // temperature value, row 2
static lv_obj_t *drop_img;      // water-drop glyph, row 3
static lv_obj_t *lbl_humi;      // humidity value, row 3

// Bottom-center NTP sync toast (opaque white pill: wifi icon + status text).
static lv_obj_t *sync_toast;    // container, hidden until ui_show_sync_toast
static lv_obj_t *wifi_img;      // wifi glyph, left
static lv_obj_t *lbl_sync;      // status text, right

// Calendar widgets
static lv_obj_t *lbl_cal_head;
static lv_obj_t *cal_icon;       // top-right, mirrors the clock pool sprite
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

// Pixel width of the byte run [a, b) in the given font.
static int32_t run_w(const lv_font_t *f, const char *t, uint32_t a, uint32_t b) {
    int32_t w = 0;
    for (uint32_t j = a; j < b; ) {
        uint32_t cp;
        int nb = utf8_next(&t[j], &cp);
        w += lv_font_get_glyph_width(f, cp, 0);
        j += nb;
    }
    return w;
}

// Draw one text run [a, b) of the original string at (x, gy) with the given
// color, left-aligned. Uses text_length so no copy or NUL terminator is needed,
// and one lv_draw_label per run (not per glyph) keeps the layer's draw-task list
// short: per-glyph lv_draw_character on a ~180-char quote piled up enough tasks
// to wedge lv_draw_add_task's list walk (the panel then froze on that frame).
static void draw_run(lv_layer_t *layer, lv_draw_label_dsc_t *dsc, const char *text,
                     uint32_t a, uint32_t b, int32_t x, int32_t gy, int32_t fh,
                     int32_t box_w, lv_color_t color) {
    if (b <= a) return;
    dsc->text = &text[a];
    dsc->text_length = b - a;
    dsc->color = color;
    lv_area_t area = { x, gy, box_w - 1, gy + fh + 2 };
    lv_draw_label(layer, dsc, &area);
}

// Draw the quote onto the canvas: vertically centered block, each line
// horizontally centered. The time expression [hl_start, hl_end) is drawn as a
// black box with white text (inline inverted highlight); the rest is black.
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
    // EXPAND stops lv_draw_label from wrapping a run when its area is a hair
    // narrower than the text: a full-width line would otherwise wrap its last
    // glyph onto a second row inside a one-line-tall area, overlapping the line
    // below. Each run is pre-measured to sit on one line, so no wrap is wanted.
    ldsc.flag = LV_TEXT_FLAG_EXPAND;

    for (int k = 0; k < n; k++) {
        // Never draw past the box: an off-canvas glyph or rect becomes a draw
        // task lv_canvas_finish_layer can never dispatch, wedging its drain loop
        // (the panel then freezes on the previous frame). The glyph height fh can
        // exceed the line pitch, so cap on the larger of the two; box_h never
        // exceeds the canvas. Quotes too long to fit lose trailing lines instead
        // of hanging.
        int32_t line_extent = (fh > line_h) ? fh : line_h;
        if (y + line_extent > box_h) break;
        uint32_t ls = starts[k], le = ends[k];
        int32_t lw = run_w(font, text, ls, le);
        int32_t x = (box_w - lw) / 2;
        if (x < 0) x = 0;
        int32_t gy = y + (line_h - fh) / 2;
        if (gy < 0) gy = 0;

        // Clip the highlight span to this line, splitting it into up to three
        // runs: normal / highlighted / normal.
        uint32_t hs = hl_start > ls ? hl_start : ls;
        uint32_t he = hl_end   < le ? hl_end   : le;
        bool has_hl = hs < he;

        uint32_t seg1_end = has_hl ? hs : le;
        draw_run(&layer, &ldsc, text, ls, seg1_end, x, gy, fh, box_w, lv_color_black());
        x += run_w(font, text, ls, seg1_end);

        if (has_hl) {
            int32_t hw = run_w(font, text, hs, he);
            int32_t rx1 = x - 1;         if (rx1 < 0) rx1 = 0;
            int32_t rx2 = x + hw + 1;    if (rx2 > box_w - 1) rx2 = box_w - 1;
            lv_area_t r = { rx1, y + 1, rx2, y + line_h - 1 };
            lv_draw_rect(&layer, &rdsc, &r);
            draw_run(&layer, &ldsc, text, hs, he, x, gy, fh, box_w, lv_color_white());
            x += hw;
            draw_run(&layer, &ldsc, text, he, le, x, gy, fh, box_w, lv_color_black());
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
        // No room under the shrunk time: the compact quote box starts at 58.
        lv_obj_add_flag(lbl_date, LV_OBJ_FLAG_HIDDEN);
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
        lv_obj_remove_flag(lbl_date, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 96);
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

    // Top-right pool sprite (72x72), recolored black, one per minute via the
    // shuffle bag in ui_next_top_icon. The cat is only the boot placeholder
    // shown for the ~1s before the first minute tick swaps in a pool sprite.
    // Vertically center the icon on the big clock instead of floating it at the
    // top: the clock label sits at TOP_MID y=20 (normal mode), so its center is
    // at 20 + line_height/2; place the 72px icon so its center matches.
    top_icon = lv_image_create(scr_clock);
    lv_image_set_src(top_icon, &cat_icon);
    lv_obj_set_style_image_recolor(top_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(top_icon, LV_OPA_COVER, 0);
    int32_t clock_cy = 20 + lv_font_get_line_height(&font_digits_96) / 2;
    lv_obj_set_pos(top_icon, LCD_WIDTH - 72 - 6, clock_cy - 72 / 2);

    // Top-left environment readout, mirroring the top-right cat icon. Three
    // rows of [20x20 icon at x=6] + [font_ko_18 value at x=30], battery on top:
    //   row 1 y=8..28   battery/USB + charge %   ("83%")
    //   row 2 y=30..50  thermometer + temperature ("23.4°C")
    //   row 3 y=52..72  water drop  + humidity    ("45%")
    // The block now spans x 6..84, y 8..72. Overlap check: the centered 96px
    // clock has a left edge near x88 (clear), and the normal quote canvas
    // starts at QUOTE_TOP_N=132 (clear). Compact mode is the exception: its
    // quote canvas starts at QUOTE_TOP_C=58 and, being created after these env
    // widgets, draws on top, so row 3 (humidity, y52..72) is occluded while a
    // long quote is on screen. Three 20px rows from y8 cannot clear y58, so
    // this is structural, not a coordinate nudge; it only affects long quotes.
    // Row 1 hidden until the first battery read (ui_set_battery); rows 2-3
    // until the first sensor read (ui_set_env).

    // Row 1: battery. One image whose source is swapped between batt_icon and
    // usb_icon in ui_set_battery, plus the percent label.
    batt_img = lv_image_create(scr_clock);
    lv_image_set_src(batt_img, &batt_icon);
    lv_obj_set_style_image_recolor(batt_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(batt_img, LV_OPA_COVER, 0);
    // The three env rows are shifted down together so the block center (row 2,
    // temperature) lines up with the big clock center, matching the top-right
    // icon. Rows were at y 8/30/52 (center 40); +15 puts the center near 55.
    lv_obj_set_pos(batt_img, 6, 23);
    lv_obj_add_flag(batt_img, LV_OBJ_FLAG_HIDDEN);

    lbl_batt = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_batt, &font_ko_18, 0);
    lv_obj_set_style_text_color(lbl_batt, lv_color_black(), 0);
    lv_label_set_text(lbl_batt, "");
    lv_obj_set_pos(lbl_batt, 30, 24);
    lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);

    therm_img = lv_image_create(scr_clock);
    lv_image_set_src(therm_img, &therm_icon);
    lv_obj_set_style_image_recolor(therm_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(therm_img, LV_OPA_COVER, 0);
    lv_obj_set_pos(therm_img, 6, 45);
    lv_obj_add_flag(therm_img, LV_OBJ_FLAG_HIDDEN);

    lbl_temp = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_temp, &font_ko_18, 0);
    lv_obj_set_style_text_color(lbl_temp, lv_color_black(), 0);
    lv_label_set_text(lbl_temp, "");
    lv_obj_set_pos(lbl_temp, 30, 46);
    lv_obj_add_flag(lbl_temp, LV_OBJ_FLAG_HIDDEN);

    drop_img = lv_image_create(scr_clock);
    lv_image_set_src(drop_img, &drop_icon);
    lv_obj_set_style_image_recolor(drop_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(drop_img, LV_OPA_COVER, 0);
    lv_obj_set_pos(drop_img, 6, 67);
    lv_obj_add_flag(drop_img, LV_OBJ_FLAG_HIDDEN);

    lbl_humi = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_humi, &font_ko_18, 0);
    lv_obj_set_style_text_color(lbl_humi, lv_color_black(), 0);
    lv_label_set_text(lbl_humi, "");
    lv_obj_set_pos(lbl_humi, 30, 68);
    lv_obj_add_flag(lbl_humi, LV_OBJ_FLAG_HIDDEN);

    // Big time: the star of the screen (~40% of height). Centered now that the
    // top-left env block and top-right cat icon balance the corners.
    lbl_time = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_time, &font_digits_96, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_black(), 0);
    lv_label_set_text(lbl_time, "00:00");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 20);

    // Date + weekday in the gap under the 96px clock (normal mode only; compact
    // mode hides it, see apply_clock_layout). font_ko_22 sits between the big
    // time above and the quote box (QUOTE_TOP_N=132) below.
    lbl_date = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(lbl_date, &font_ko_22, 0);
    lv_obj_set_style_text_color(lbl_date, lv_color_black(), 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 96);

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

    // Sync toast: a wifi icon + one-line status text on an opaque white pill at
    // BOTTOM_MID -32 (its 20px row spans screen y ~249..268). That band sits
    // INSIDE the quote canvas' drawn box (normal reaches y272, compact y275),
    // so an opaque white background is required to occlude any quote text under
    // it for the toast' 4s life; a bare label/A8 icon would let text bleed
    // through. It clears the source label (top ~y275) below and the top-left
    // env block (y<=52) above. A flex row (LVGL9) centers icon + text and
    // content-sizes the pill; created LAST so it draws over the canvas.
    sync_toast = lv_obj_create(scr_clock);
    lv_obj_set_style_bg_color(sync_toast, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sync_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sync_toast, 0, 0);
    lv_obj_set_style_pad_all(sync_toast, 4, 0);
    lv_obj_set_style_pad_column(sync_toast, 4, 0);   // gap between icon and text
    lv_obj_remove_flag(sync_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sync_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sync_toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sync_toast, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(sync_toast, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_add_flag(sync_toast, LV_OBJ_FLAG_HIDDEN);

    wifi_img = lv_image_create(sync_toast);
    lv_image_set_src(wifi_img, &wifi_icon);
    lv_obj_set_style_image_recolor(wifi_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(wifi_img, LV_OPA_COVER, 0);

    lbl_sync = lv_label_create(sync_toast);
    lv_obj_set_style_text_font(lbl_sync, &font_ko_18, 0);
    lv_obj_set_style_text_color(lbl_sync, lv_color_black(), 0);
    lv_label_set_text(lbl_sync, "");
}

void ui_set_time_text(int hour, int minute) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(lbl_time, buf);
}

// Date line under the clock: "M월 D일 (요일)". wday is 0=Sun..6=Sat. Only shown
// in normal mode; apply_clock_layout hides the label in compact mode.
void ui_set_date_text(int mon, int mday, int wday) {
    if (wday < 0 || wday > 6) wday = 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d월 %d일 (%s)", mon, mday, WDAY_KO[wday]);
    lv_label_set_text(lbl_date, buf);
}

// Shuffle bag over the icon pool: deal the pool in a random order, one sprite
// per minute, and only reshuffle once every sprite has been shown. This makes
// each sprite reappear at a fixed spacing of exactly icon_pool_count minutes,
// so nothing repeats at short intervals (unlike plain random draws). Across a
// reshuffle the first of the new bag is kept different from the last shown.
static uint16_t *s_bag = NULL;
static int s_bag_pos = 0;
static int s_last_idx = -1;

static void reshuffle_bag(void) {
    int n = icon_pool_count;
    for (int i = 0; i < n; i++) s_bag[i] = (uint16_t)i;
    for (int i = n - 1; i > 0; i--) {          // Fisher-Yates
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint16_t t = s_bag[i]; s_bag[i] = s_bag[j]; s_bag[j] = t;
    }
    if (n > 1 && s_bag[0] == s_last_idx) {     // avoid back-to-back repeat
        uint16_t t = s_bag[0]; s_bag[0] = s_bag[1]; s_bag[1] = t;
    }
    s_bag_pos = 0;
}

// Advance to the next pool sprite. Call under the LVGL lock, once per minute.
void ui_next_top_icon(void) {
    if (icon_pool_count <= 0 || !top_icon) return;
    if (!s_bag) {
        s_bag = (uint16_t *)malloc(sizeof(uint16_t) * icon_pool_count);
        if (!s_bag) return;                    // stay on the boot placeholder
        reshuffle_bag();
    }
    if (s_bag_pos >= icon_pool_count) reshuffle_bag();
    s_last_idx = s_bag[s_bag_pos++];
    const lv_image_dsc_t *img = icon_pool[s_last_idx];
    lv_image_set_src(top_icon, img);
    if (cal_icon) lv_image_set_src(cal_icon, img);   // calendar mirrors the clock
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
    // Integer temperature ("23°C"): the decimal form ("23.4°C") reached ~x96,
    // past the 96px clock's left edge (~x87), so the two overlapped.
    snprintf(t, sizeof(t), "%.0f°C", temp_c);
    snprintf(h, sizeof(h), "%.0f%%", humi_pct);
    lv_label_set_text(lbl_temp, t);
    lv_label_set_text(lbl_humi, h);
    lv_obj_remove_flag(therm_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lbl_temp,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(drop_img,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lbl_humi,  LV_OBJ_FLAG_HIDDEN);
}

void ui_set_battery(int percent, bool charging, bool valid) {
    // A failed ADC read (valid=false) hides row 1 rather than showing a stale
    // or bogus level.
    if (!valid) {
        lv_obj_add_flag(batt_img,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_batt,  LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Always show the percent. Swap the glyph to USB only while the voltage is
    // actively rising (charging, detected in adc_battery.cpp). A discharging or
    // full-on-USB cell keeps the battery glyph, so the icon never falsely claims
    // "plugged" while running on battery (which a fixed voltage threshold did).
    lv_image_set_src(batt_img, charging ? &usb_icon : &batt_icon);
    char b[16];
    snprintf(b, sizeof(b), "%d%%", percent);
    lv_label_set_text(lbl_batt, b);
    lv_obj_remove_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(batt_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_sync_toast(bool ok) {
    // Flex re-lays out the row on the new text; lv_obj_align keeps the pill
    // centered as its content width changes between the two strings.
    lv_label_set_text(lbl_sync, ok ? "시각 동기됨" : "동기 실패");
    lv_obj_remove_flag(sync_toast, LV_OBJ_FLAG_HIDDEN);
}

void ui_hide_sync_toast(void) {
    lv_obj_add_flag(sync_toast, LV_OBJ_FLAG_HIDDEN);
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

// Thin full-width black rule at (x, y), used for the header underline and the
// week separators. remove_style_all strips the default lv_obj border/padding.
static void cal_rule(int x, int y, int w, int h) {
    lv_obj_t *r = lv_obj_create(scr_cal);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_pos(r, x, y);
}

static void build_calendar_screen(void) {
    scr_cal = lv_obj_create(NULL);
    style_screen_white(scr_cal);

    // Top-right dot sprite, mirrors the clock: ui_next_top_icon points it at the
    // same pool sprite each minute. Pool sprites are 72px; scale to ~48px so it
    // clears the weekday row. The cat is only the boot placeholder.
    cal_icon = lv_image_create(scr_cal);
    lv_image_set_src(cal_icon, &cat_icon);
    lv_obj_set_style_image_recolor(cal_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(cal_icon, LV_OPA_COVER, 0);
    lv_image_set_scale(cal_icon, 171);            // 72 -> ~48 px, centered in box
    lv_obj_set_pos(cal_icon, LCD_WIDTH - 72 - 4, 2);

    // Header "YYYY년 M월", top-left so it clears the top-right sprite.
    lbl_cal_head = lv_label_create(scr_cal);
    lv_obj_set_style_text_font(lbl_cal_head, &font_ko_44, 0);
    lv_obj_set_style_text_color(lbl_cal_head, lv_color_black(), 0);
    lv_label_set_text(lbl_cal_head, "");
    lv_obj_align(lbl_cal_head, LV_ALIGN_TOP_LEFT, 12, 8);

    const int x0 = 12, y0 = 68, cw = 53, ch = 32;
    for (int c = 0; c < 7; c++) {
        cal_wday[c] = lv_label_create(scr_cal);
        lv_obj_set_style_text_font(cal_wday[c], &font_ko_28, 0);
        lv_obj_set_width(cal_wday[c], cw);
        lv_obj_set_style_text_align(cal_wday[c], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_ver(cal_wday[c], 1, 0);
        if (c == 0 || c == 6) {          // weekend: inverted marker
            lv_obj_set_style_bg_color(cal_wday[c], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(cal_wday[c], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cal_wday[c], 4, 0);
            lv_obj_set_style_text_color(cal_wday[c], lv_color_white(), 0);
        } else {
            lv_obj_set_style_text_color(cal_wday[c], lv_color_black(), 0);
        }
        lv_label_set_text(cal_wday[c], WDAY_KO[c]);
        lv_obj_set_pos(cal_wday[c], x0 + c * cw, y0);
    }
    cal_rule(x0, y0 + 30, 7 * cw, 2);              // underline below weekdays

    const int gy = y0 + 36;
    for (int i = 0; i < 42; i++) {
        int r = i / 7, c = i % 7;
        cal_cell[i] = lv_label_create(scr_cal);
        lv_obj_set_style_text_font(cal_cell[i], &font_ko_28, 0);
        lv_obj_set_style_text_color(cal_cell[i], lv_color_black(), 0);
        lv_obj_set_style_pad_all(cal_cell[i], 2, 0);
        lv_obj_set_style_radius(cal_cell[i], 5, 0);   // rounds the today box
        lv_obj_set_width(cal_cell[i], cw);
        lv_obj_set_style_text_align(cal_cell[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(cal_cell[i], "");
        lv_obj_set_pos(cal_cell[i], x0 + c * cw, gy + r * ch);
    }
    for (int r = 0; r < 5; r++)                    // thin week separators
        cal_rule(x0, gy + (r + 1) * ch - 2, 7 * cw, 1);
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
