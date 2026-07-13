#pragma once

#include <time.h>
#include "quote_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build both screens (clock + calendar) and show the clock. Call under the
// LVGL lock.
void ui_init(void);

// Start the KEY-button polling task (GPIO18, active low, 50ms debounce).
// Short press toggles clock <-> calendar. Takes the LVGL lock internally.
void ui_start_button_task(void);

// Cheap per-second update: just the big HH:MM digits.
void ui_set_time_text(int hour, int minute);

// Per-minute update: swap the top-right sprite to the next one from the icon
// pool shuffle bag. Call under the LVGL lock alongside the quote update.
void ui_next_top_icon(void);

// Per-minute update: quote body, source, and the inverted time-expression
// highlight. Pass q = NULL when no quote exists for this minute (time only).
void ui_set_quote(const quote_t *q);

// Rebuild the calendar grid for the given month, marking today.
void ui_build_calendar(const struct tm *now);

// Update the top-left temperature/humidity readout on the clock screen.
// temp_c in deg C (one decimal shown), humi_pct in %RH (integer shown). Pass
// NaN for either value on a failed sensor read to hide both icons and labels.
void ui_set_env(float temp_c, float humi_pct);

// Update the top-left battery readout, row 1 (top) of the environment block.
// plugged=true (USB feeding, V>=4.2) shows the USB glyph alone and hides the
// percent; plugged=false shows the battery glyph + percent (0..100 level).
// valid=false (a failed ADC read) hides the row.
void ui_set_battery(int percent, bool plugged, bool valid);

// Bottom-center sync toast: a wifi icon + one-line status text on an opaque
// white backing. ok => "시각 동기됨", else "동기 실패". Both calls take the
// LVGL lock at the caller (main loop). Hidden until shown.
void ui_show_sync_toast(bool ok);
void ui_hide_sync_toast(void);

#ifdef __cplusplus
}
#endif
