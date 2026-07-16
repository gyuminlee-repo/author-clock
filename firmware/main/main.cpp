#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>

#include "display_st7305.h"
#include "lvgl_port.h"
#include "rtc_pcf85063.h"
#include "shtc3.h"
#include "adc_battery.h"
#include "quote_store.h"
#include "net_time.h"
#include "ui.h"
#include "user_config.h"

static const char *TAG = "main";

// Global panel instance (mosi, sck, dc, cs, rst, w, h).
static DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN,
                            RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

// LVGL RGB565 partial tile -> 1bpp threshold -> persistent framebuffer.
// Push the panel only on the last flush of a refresh (1MHz SPI is slow).
static void Lvgl_FlushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_map) {
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            RlcdPort.RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    if (lv_display_flush_is_last(disp)) {
        RlcdPort.RLCD_Display();
    }
    lv_display_flush_ready(disp);
}

// Human-readable reset cause, logged at boot so a mystery power-off can be
// classified next time: BROWNOUT means the battery voltage sagged (power),
// TASK_WDT/INT_WDT/PANIC mean a firmware crash, POWERON means a clean start.
static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "POWERON";
        case ESP_RST_SW:       return "SW";
        case ESP_RST_PANIC:    return "PANIC(crash)";
        case ESP_RST_INT_WDT:  return "INT_WDT(hang)";
        case ESP_RST_TASK_WDT: return "TASK_WDT(hang)";
        case ESP_RST_WDT:      return "WDT(hang)";
        case ESP_RST_BROWNOUT: return "BROWNOUT(voltage sag)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:               return "OTHER";
    }
}

// Persistent "black box" in NVS: the last battery voltage and uptime survive a
// power-off, so an on-battery death (which leaves no serial trace) can be read
// on the next boot. Low batt_min => the cell sagged (brownout); a high value at
// death => the battery was fine and the cause is elsewhere.
typedef struct {
    uint32_t boot_count;
    uint32_t last_uptime_s;
    uint16_t last_batt_mv;
    uint16_t min_batt_mv;
} blackbox_t;

static nvs_handle_t s_bb = 0;
static blackbox_t s_bb_data;

static void blackbox_load_and_log(esp_reset_reason_t rr) {
    if (nvs_open("blackbox", NVS_READWRITE, &s_bb) != ESP_OK) return;
    size_t sz = sizeof(s_bb_data);
    if (nvs_get_blob(s_bb, "d", &s_bb_data, &sz) != ESP_OK || sz != sizeof(s_bb_data)) {
        memset(&s_bb_data, 0, sizeof(s_bb_data));
        s_bb_data.min_batt_mv = 0xFFFF;
    }
    ESP_LOGW(TAG, "BLACKBOX prev run: boot#%lu ran=%lus batt_last=%umV batt_min=%umV | this reset=%s",
             (unsigned long)s_bb_data.boot_count, (unsigned long)s_bb_data.last_uptime_s,
             (unsigned)s_bb_data.last_batt_mv, (unsigned)s_bb_data.min_batt_mv,
             reset_reason_str(rr));
    s_bb_data.boot_count++;
    s_bb_data.last_uptime_s = 0;
    s_bb_data.last_batt_mv = 0;
    s_bb_data.min_batt_mv = 0xFFFF;      // per-run minimum
}

static void blackbox_save(uint32_t uptime_s, uint16_t batt_mv) {
    if (!s_bb) return;
    s_bb_data.last_uptime_s = uptime_s;
    s_bb_data.last_batt_mv = batt_mv;
    if (batt_mv && batt_mv < s_bb_data.min_batt_mv) s_bb_data.min_batt_mv = batt_mv;
    nvs_set_blob(s_bb, "d", &s_bb_data, sizeof(s_bb_data));
    nvs_commit(s_bb);
}

extern "C" void app_main(void) {
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGW(TAG, "boot: reset reason = %s", reset_reason_str(rr));

    // 1. NVS (WiFi needs it)
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    blackbox_load_and_log(rr);

    // Timezone first, unconditionally: every later time path (RTC seed, NTP,
    // the render loop) must interpret wall time as KST even when RTC or WiFi
    // fails, or the clock shows UTC.
    setenv("TZ", "KST-9", 1);
    tzset();


    // 2. RTC -> seed system time (KST). NTP task refines it later.
    if (pcf85063_init()) {
#ifdef AC_SET_RTC_EPOCH
        // Build-time override: unconditionally set system + RTC time from the
        // epoch baked in at compile time (a UTC unix epoch the build passes,
        // e.g. `date +%s`). TZ is already KST-9, so localtime_r yields KST wall
        // time and rtc_set_time stores it exactly as net_time.cpp's NTP-success
        // path does. System time is set regardless; the RTC write is
        // best-effort.
        struct timeval tv = { .tv_sec = (time_t)AC_SET_RTC_EPOCH, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        time_t forced = time(NULL);
        struct tm flt;
        localtime_r(&forced, &flt);
        rtc_set_time(&flt);
        ESP_LOGI(TAG, "RTC force-set from build epoch: %04d-%02d-%02d %02d:%02d:%02d",
                 flt.tm_year + 1900, flt.tm_mon + 1, flt.tm_mday,
                 flt.tm_hour, flt.tm_min, flt.tm_sec);
#else
        struct tm t;
        if (rtc_get_time(&t)) {
            time_t local = mktime(&t);
            struct timeval tv = { .tv_sec = local, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "system time seeded from RTC");
        }
#endif
        // SHTC3 shares the RTC I2C bus, so init it only after the bus exists.
        if (!shtc3_init())
            ESP_LOGW(TAG, "SHTC3 init failed; env readout stays hidden");
    }

    // 2c. Battery ADC (independent of the I2C bus). Reads stay hidden if init
    // fails, so this never blocks the clock.
    if (!battery_init())
        ESP_LOGW(TAG, "battery ADC init failed; battery readout stays hidden");

    // 3. Quote data (PSRAM)
    if (!quote_store_init()) {
        ESP_LOGE(TAG, "quote store init failed; time-only mode");
    }

    // 4. Display + LVGL + UI
    RlcdPort.RLCD_Init();
    Lvgl_PortInit(LCD_WIDTH, LCD_HEIGHT, Lvgl_FlushCallback);
    if (Lvgl_lock(-1)) {
        ui_init();
        Lvgl_unlock();
    }
    ui_start_button_task();

    // 5. WiFi/NTP asynchronously (never blocks the clock).
    net_time_start();   // boot NTP window, then waits for KEY-triggered resyncs

    // 6. Update loop: HH:MM every second, quote + calendar on minute change,
    // temperature/humidity every 30s.
    int last_min = -1;
    int env_ticks = 30;   // 30 => fire the first env read on iteration 1
    // Sync toast latch: show once on the first OK/FAIL transition, auto-hide
    // after 4 ticks (~4s), then never again. tick counts loop iterations (1s).
    int tick = 0;
    net_sync_state_t last_st = NET_SYNC_NONE;  // edge-trigger the toast
    int toast_hide_tick = -1;                  // >=0 while a toast is showing
    for (;;) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);

        if (Lvgl_lock(-1)) {
            // The clock face shows HH:MM only, so touch LVGL just once per
            // minute: each label update repaints and pushes the whole panel
            // over 1MHz SPI (~120ms), wasteful every second.
            if (lt.tm_min != last_min) {
                last_min = lt.tm_min;
                ui_set_time_text(lt.tm_hour, lt.tm_min);
                ui_set_date_text(lt.tm_mon + 1, lt.tm_mday, lt.tm_wday);
                quote_t q;
                if (quote_for_minute(lt.tm_hour, lt.tm_min, &q))
                    ui_set_quote(&q);
                else
                    ui_set_quote(NULL);
                ui_next_top_icon();
                ui_build_calendar(&lt);
            }
            Lvgl_unlock();
        }

        // Environment readout every 30s. The SHTC3 measurement blocks ~70ms of
        // I2C and needs no LVGL lock, so read it outside the lock; only the
        // ui_set_env label update touches LVGL. A failed read passes NaN, which
        // hides the readout until the next cycle recovers it.
        if (++env_ticks >= 30) {
            env_ticks = 0;
            float tc = NAN, hp = NAN;
            if (!shtc3_read(&tc, &hp)) {
                tc = NAN;
                hp = NAN;
            }
            // Battery ADC read shares the 30s cadence. Like the SHTC3 read it
            // needs no LVGL lock (independent peripheral), so read it outside
            // the lock and only the ui_set_battery label update touches LVGL. A
            // failed read passes valid=false, which hides row 3 until recovery.
            float bv = 0.0f;
            int bpct = 0;
            bool bcharging = false;
            bool bok = battery_read(&bv, &bpct, &bcharging);
            if (Lvgl_lock(-1)) {
                ui_set_env(tc, hp);
                ui_set_battery(bpct, bcharging, bok);
                Lvgl_unlock();
            }
            // Black box: persist uptime + battery voltage so an on-battery death
            // is readable next boot. Also log heap to rule OOM back in if needed.
            blackbox_save((uint32_t)(esp_timer_get_time() / 1000000),
                          bok ? (uint16_t)(bv * 1000.0f) : 0);
            ESP_LOGW(TAG, "MEM heap=%lu min=%lu | batt=%umV",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size(),
                     bok ? (unsigned)(bv * 1000.0f) : 0);
        }

        // Sync toast: show whenever the sync state settles into OK/FAIL. This
        // covers the boot sync AND a KEY-triggered resync (which sets the state
        // back to TRYING first), so it is edge-triggered on the TRYING->OK/FAIL
        // transition and auto-hidden ~4 ticks later. UI calls take the LVGL lock.
        net_sync_state_t st = net_time_get_status();
        if ((st == NET_SYNC_OK || st == NET_SYNC_FAIL) &&
            (last_st == NET_SYNC_TRYING || last_st == NET_SYNC_NONE)) {
            if (Lvgl_lock(-1)) {
                ui_show_sync_toast(st == NET_SYNC_OK);
                Lvgl_unlock();
            }
            toast_hide_tick = tick + 4;
        }
        if (toast_hide_tick >= 0 && tick >= toast_hide_tick) {
            if (Lvgl_lock(-1)) {
                ui_hide_sync_toast();
                Lvgl_unlock();
            }
            toast_hide_tick = -1;
        }
        last_st = st;

        tick++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
