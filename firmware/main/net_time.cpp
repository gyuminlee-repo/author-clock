#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include "net_time.h"
#include "rtc_pcf85063.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#warning "wifi_secrets.h missing; copy wifi_secrets.h.example. Using empty credentials."
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif

static const char *TAG = "NetTime";
#define KST_TZ "KST-9"

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     6
static int s_retry = 0;

// Set once esp_wifi_start has run, so net_time_shutdown knows whether there is
// a radio to stop (the no-SSID path never starts it).
static bool s_wifi_started = false;

// Coarse sync state published for the UI toast. Written only from the sync
// task, read from the main loop; a 32-bit int store is atomic on the ESP32.
static volatile int s_status = NET_SYNC_NONE;

// OK is terminal: once synced, the task self-deletes, so never let a later
// FAIL (window close) clobber a real OK. TRYING/OK always overwrite.
static void set_status(net_sync_state_t st) {
    if (st == NET_SYNC_FAIL && s_status == NET_SYNC_OK) return;
    s_status = (int)st;
}

net_sync_state_t net_time_get_status(void) {
    return (net_sync_state_t)s_status;
}

void net_time_set_fail(void) {
    set_status(NET_SYNC_FAIL);
}

static void restore_from_rtc(void) {
    struct tm t;
    if (rtc_get_time(&t)) {
        setenv("TZ", KST_TZ, 1);
        tzset();
        time_t local = mktime(&t);   // t is local (KST) wall time from RTC
        struct timeval tv = { .tv_sec = local, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system time restored from RTC");
    } else {
        ESP_LOGW(TAG, "RTC time unavailable; clock starts unset");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "retry wifi connect (%d)", s_retry);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

bool net_time_wifi_configured(void) {
    return strlen(WIFI_SSID) != 0;
}

void net_time_shutdown(void) {
    if (!s_wifi_started) return;   // radio never came up; nothing to stop
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) {
        s_wifi_started = false;
        ESP_LOGI(TAG, "WiFi stopped; radio off after NTP window");
    } else {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
}

bool net_time_sync(void) {
    if (!net_time_wifi_configured()) {
        ESP_LOGW(TAG, "no WIFI_SSID configured; using RTC only");
        restore_from_rtc();
        return false;
    }

    set_status(NET_SYNC_TRYING);   // covers both first bring-up and the retry path

    // One-time bring-up. esp_netif_init/esp_event_loop_create_default/
    // esp_wifi_init/handler registration/esp_wifi_start are not reentrant and
    // abort under ESP_ERROR_CHECK if called twice, so guard them. A retry after
    // a failed connect re-enters via the else branch: reset the retry counter,
    // clear stale bits, and reconnect from esp_wifi_connect().
    static bool s_wifi_inited = false;
    if (!s_wifi_inited) {
        s_wifi_events = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t loop_err = esp_event_loop_create_default();
        if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(loop_err);
        }
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t any_id, got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, &any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, &got_ip));

        wifi_config_t wcfg = {};
        strncpy((char *)wcfg.sta.ssid, WIFI_SSID, sizeof(wcfg.sta.ssid) - 1);
        strncpy((char *)wcfg.sta.password, WIFI_PASSWORD, sizeof(wcfg.sta.password) - 1);
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
        ESP_ERROR_CHECK(esp_wifi_start());   // STA_START handler calls esp_wifi_connect()
        s_wifi_inited = true;
        s_wifi_started = true;
    } else {
        s_retry = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "retrying wifi connect");
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "wifi connect failed; falling back to RTC");
        restore_from_rtc();
        return false;
    }

    // SNTP: init once, then let lwip keep polling. A retry that got past the
    // WiFi step but timed out on SNTP re-enters here with SNTP already running,
    // so guard the one-time setup instead of re-initializing it.
    static bool s_sntp_inited = false;
    if (!s_sntp_inited) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        // Phone hotspots often fail to resolve or route pool.ntp.org; list
        // several widely-reachable servers so any one succeeding syncs time.
        esp_sntp_setservername(0, "time.google.com");
        esp_sntp_setservername(1, "time.cloudflare.com");
        esp_sntp_setservername(2, "kr.pool.ntp.org");
        esp_sntp_init();
        s_sntp_inited = true;
    }

    int wait = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && wait < 25) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait++;
    }

    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "SNTP timeout; falling back to RTC");
        restore_from_rtc();
        return false;
    }

    setenv("TZ", KST_TZ, 1);
    tzset();

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (rtc_set_time(&local)) {
        ESP_LOGI(TAG, "NTP synced and RTC updated: %04d-%02d-%02d %02d:%02d:%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
    }
    set_status(NET_SYNC_OK);
    return true;
}
