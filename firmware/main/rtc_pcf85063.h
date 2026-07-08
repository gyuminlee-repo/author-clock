#pragma once

#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PCF85063 RTC over I2C (SDA=GPIO13, SCL=GPIO14).
// Minimal register-level driver: time registers start at 0x04 (sec,min,hour,
// day,weekday,month,year), all BCD.

// Initialize the I2C master bus and probe the RTC.
// Returns true if the chip acknowledges.
bool pcf85063_init(void);

// Read the RTC into a struct tm (tm_year since 1900, tm_mon 0-11).
// Returns false on I2C error or if the clock-integrity flag reports a
// power loss (uninitialized time).
bool rtc_get_time(struct tm *out);

// Write a struct tm to the RTC.
bool rtc_set_time(const struct tm *in);

#ifdef __cplusplus
}
#endif
