#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "cJSON.h"
#include "quote_store.h"

static const char *TAG = "QuoteStore";

// Quote DB compiled in by tools/embed_quotes.py (null-terminated).
extern "C" const char quotes_min_json[];
extern "C" const size_t quotes_min_json_len;

static cJSON *s_root = NULL;

// Route cJSON allocations to PSRAM.
static void *psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p)     { heap_caps_free(p); }

bool quote_store_init(void) {
    cJSON_Hooks hooks;
    hooks.malloc_fn = psram_malloc;
    hooks.free_fn   = psram_free;
    cJSON_InitHooks(&hooks);

    // The embedded blob is null-terminated by EMBED_TXTFILES.
    s_root = cJSON_Parse(quotes_min_json);
    if (!s_root) {
        ESP_LOGE(TAG, "quotes_min.json parse failed (%d bytes)",
                 (int)((quotes_min_json + quotes_min_json_len) - quotes_min_json));
        return false;
    }
    ESP_LOGI(TAG, "quotes loaded: %d minute keys", cJSON_GetArraySize(s_root));
    return true;
}

static const char *item_str(const cJSON *obj, const char *field) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, field);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

bool quote_for_minute(int hour, int minute, quote_t *out) {
    if (!s_root || !out) return false;
    char key[6];
    snprintf(key, sizeof(key), "%02d:%02d", hour, minute);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(s_root, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) return false;

    // Pick the shortest quote for this minute: the fewer characters, the larger
    // the auto-fit font and the less chance a long quote must shrink or clip.
    // Ties break toward an original (k=0) over a translation.
    const cJSON *chosen = NULL;
    size_t best_len = (size_t)-1;
    int best_k = 2;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        const cJSON *qj = cJSON_GetObjectItemCaseSensitive(e, "q");
        if (!cJSON_IsString(qj) || !qj->valuestring) continue;
        size_t len = strlen(qj->valuestring);
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(e, "k");
        int kv = cJSON_IsNumber(k) ? k->valueint : 1;
        if (!chosen || len < best_len ||
            (len == best_len && kv == 0 && best_k != 0)) {
            chosen = e;
            best_len = len;
            best_k = kv;
        }
    }
    if (!chosen) return false;

    out->t = item_str(chosen, "t");
    out->q = item_str(chosen, "q");
    out->a = item_str(chosen, "a");
    out->w = item_str(chosen, "w");
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(chosen, "k");
    out->k = cJSON_IsNumber(k) ? k->valueint : 1;
    return true;
}
