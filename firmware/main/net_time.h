#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Bring up WiFi STA, run SNTP against pool.ntp.org in KST, and on success set
// the system clock and persist it to the PCF85063. On any failure the system
// clock is restored from the RTC and the device keeps running.
//
// Blocking; call from a dedicated task (not app_main's critical path).
// Returns true if NTP synced, false if it fell back to RTC.
bool net_time_sync(void);

#ifdef __cplusplus
}
#endif
