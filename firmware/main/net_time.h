#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Seconds a caller should wait before retrying net_time_sync() after a
// transient failure (router down at boot, SNTP timeout). One hour keeps the
// radio idle on an always-on desk clock while still recovering unattended.
#define NET_TIME_RETRY_SEC 3600

// Bounded NTP window: the sync task tries for at most this many seconds after
// boot, then releases the radio (see net_time_shutdown). Keeps an always-on
// desk clock from holding WiFi up indefinitely when NTP never succeeds.
#define NET_TIME_MAX_TRY_SEC 300

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

// Stop the WiFi radio after the NTP window closes (or immediately in the
// RTC-only path). Safe to call when WiFi was never started: it no-ops if the
// bring-up in net_time_sync never ran.
void net_time_shutdown(void);

#ifdef __cplusplus
}
#endif
