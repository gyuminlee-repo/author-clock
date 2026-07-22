#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Coarse NTP sync state, polled by the render loop to drive the on-screen
// toast. Values are stored/read as a plain int (32-bit aligned, atomic on the
// ESP32) since net_time_task (core 0) writes while the main loop reads.
typedef enum {
    NET_SYNC_NONE = 0,   // nothing attempted yet
    NET_SYNC_TRYING,     // WiFi connect / SNTP in progress
    NET_SYNC_OK,         // NTP synced and RTC updated
    NET_SYNC_FAIL,       // window closed or unconfigured without a sync
} net_sync_state_t;

// Current sync state. Safe to call from any task; returns the latest value the
// sync task published.
net_sync_state_t net_time_get_status(void);

// Publish NET_SYNC_FAIL from the sync task when its window closes without a
// sync. No-op if an OK was already recorded, so a real sync is never masked.
void net_time_set_fail(void);

// Seconds a caller should wait before retrying net_time_sync() after a
// transient failure (router down at boot, SNTP timeout). One hour keeps the
// radio idle on an always-on desk clock while still recovering unattended.
#define NET_TIME_RETRY_SEC 3600

// Bounded NTP window: the sync task tries for at most this many seconds after
// boot, then releases the radio (see net_time_shutdown). Keeps an always-on
// desk clock from holding WiFi up indefinitely when NTP never succeeds.
#define NET_TIME_MAX_TRY_SEC 300

// Manual-resync window (KEY calendar->clock): shorter than boot since the user
// has just turned the hotspot on and expects a quick sync then radio-off.
#define NET_TIME_RESYNC_SEC 60

// Start the single sync task: it runs the boot NTP window, then blocks waiting
// for net_time_resync() requests. Call once from app_main.
void net_time_start(void);

// Request a one-off WiFi resync (radio up -> NTP -> radio off). Triggered by the
// KEY button on the calendar->clock transition. No-op if unconfigured.
void net_time_resync(void);

// Bring up WiFi STA, run SNTP against pool.ntp.org in KST, and on success set
// the system clock and persist it to the PCF85063. On any failure the system
// clock is restored from the RTC and the device keeps running.
//
// Reentrant: the one-time WiFi/netif/SNTP bring-up runs on the first call; a
// later call after a failure just clears the event bits and reconnects.
//
// Blocking; call from a dedicated task (not app_main's critical path).
// Returns true if NTP synced, false if it fell back to RTC.
bool net_time_sync(void);

// True when a non-empty WIFI_SSID is compiled in. A caller uses this to skip
// retrying net_time_sync() when no credentials exist (RTC-only, no radio).
bool net_time_wifi_configured(void);

// True while the WiFi radio is powered (between esp_wifi_start and
// net_time_shutdown). Callers use this to drop ADC battery samples taken during
// a radio burst, which sags the cell far below its true resting voltage.
bool net_time_radio_active(void);

// Stop the WiFi radio after the NTP window closes (or immediately in the
// RTC-only path). Safe to call when WiFi was never started: it no-ops if the
// bring-up in net_time_sync never ran.
void net_time_shutdown(void);

#ifdef __cplusplus
}
#endif
