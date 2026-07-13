#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "display_st7305.h"
#include "lvgl_port.h"
#include "rtc_pcf85063.h"
#include "shtc3.h"
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

static void net_time_task(void *arg) {
    // Bounded NTP window: try for up to NET_TIME_MAX_TRY_SEC after boot. A
    // successful sync ends the task early and leaves the radio up so lwip SNTP
    // keeps the clock refreshed. If the window closes without a sync (or there
    // is no WIFI_SSID to retry), stop the radio and end the task so an always-on
    // desk clock does not hold WiFi up forever. Each net_time_sync call blocks
    // up to ~45s, so a few attempts fit inside the window at 30s spacing.
    int64_t start = esp_timer_get_time();
    const int64_t budget_us = (int64_t)NET_TIME_MAX_TRY_SEC * 1000000;
    while ((esp_timer_get_time() - start) < budget_us) {
        if (net_time_sync()) {             // synced from NTP; SNTP takes over
            vTaskDelete(NULL);
            return;
        }
        if (!net_time_wifi_configured())   // RTC-only, retrying is pointless
            break;
        vTaskDelay(pdMS_TO_TICKS(30 * 1000));
    }
    net_time_shutdown();                    // window closed or RTC-only: radio off
    vTaskDelete(NULL);
}

extern "C" void app_main(void) {
    // 1. NVS (WiFi needs it)
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

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
    xTaskCreatePinnedToCore(net_time_task, "nettime", 5 * 1024, NULL, 4, NULL, 0);

    // 6. Update loop: HH:MM every second, quote + calendar on minute change,
    // temperature/humidity every 30s.
    int last_min = -1;
    int env_ticks = 30;   // 30 => fire the first env read on iteration 1
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
                quote_t q;
                if (quote_for_minute(lt.tm_hour, lt.tm_min, &q))
                    ui_set_quote(&q);
                else
                    ui_set_quote(NULL);
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
            if (Lvgl_lock(-1)) {
                ui_set_env(tc, hp);
                Lvgl_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
