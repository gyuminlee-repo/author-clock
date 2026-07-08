#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <esp_log.h>

#include "display_st7305.h"
#include "lvgl_port.h"
#include "rtc_pcf85063.h"
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
    // Retry until NTP succeeds: an always-on clock may boot with the router
    // down and needs to sync whenever it comes back. Once synced, lwip SNTP
    // keeps the clock refreshed, so the task exits. With no WIFI_SSID there is
    // nothing to retry, so the single RTC-only pass ends the task too.
    for (;;) {
        if (net_time_sync())               // synced from NTP; SNTP takes over
            break;
        if (!net_time_wifi_configured())   // RTC-only, retrying is pointless
            break;
        vTaskDelay(pdMS_TO_TICKS(NET_TIME_RETRY_SEC * 1000));
    }
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
        struct tm t;
        if (rtc_get_time(&t)) {
            time_t local = mktime(&t);
            struct timeval tv = { .tv_sec = local, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "system time seeded from RTC");
        }
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

    // 6. Update loop: HH:MM every second, quote + calendar on minute change.
    int last_min = -1;
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
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
