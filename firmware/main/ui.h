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

// Per-minute update: quote body, source, and the inverted time-expression
// highlight. Pass q = NULL when no quote exists for this minute (time only).
void ui_set_quote(const quote_t *q);

// Rebuild the calendar grid for the given month, marking today.
void ui_build_calendar(const struct tm *now);

#ifdef __cplusplus
}
#endif
