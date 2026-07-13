#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// SHTC3 temperature + humidity sensor over I2C (addr 0x70), sharing the same
// bus the PCF85063 RTC created (SDA=GPIO13, SCL=GPIO14). Call shtc3_init AFTER
// pcf85063_init so the bus handle exists.

// Add the SHTC3 as a device on the RTC-owned bus and probe it.
// Returns true if the sensor acknowledges (ID read + CRC ok).
bool shtc3_init(void);

// One polling measurement: wake, measure, read 6 bytes, verify both CRCs,
// sleep. On success writes temperature (deg C) and relative humidity (%RH) and
// returns true. On any I2C or CRC error returns false and leaves *temp_c /
// *humi_pct untouched.
bool shtc3_read(float *temp_c, float *humi_pct);

#ifdef __cplusplus
}
#endif
