#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *t;   // time expression inside the quote (for highlight match)
    const char *q;   // quote body
    const char *a;   // work / title
    const char *w;   // author
    int         k;   // 0 = original (preferred), 1 = translated
} quote_t;

// Parse the embedded quotes_min.json once into the PSRAM heap.
// Returns false if parsing failed.
bool quote_store_init(void);

// Fill *out with a quote for HH:MM. When several exist, k==0 (original) wins.
// Returns false when no entry exists for that minute (caller shows time only).
bool quote_for_minute(int hour, int minute, quote_t *out);

#ifdef __cplusplus
}
#endif
