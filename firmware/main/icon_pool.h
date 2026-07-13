#pragma once

// Top-right icon pool. The array and count are defined in the generated
// main/icon_pool.c (built by tools/make_pool.py from ../assets/pool_src +
// ../assets/pool_local). ui.cpp draws one member per minute via a shuffle bag.
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t *const icon_pool[];
extern const int icon_pool_count;

#ifdef __cplusplus
}
#endif
