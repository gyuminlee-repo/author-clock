#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 18650 battery gauge on ADC1 channel 3 (GPIO4), 1/3 resistor divider.
// Independent of the RTC/SHTC3 I2C bus, so init it unconditionally.

// One-shot ADC unit + curve-fitting calibration on ADC1_CH3, ATTEN_DB_12.
// Returns false if the unit, channel, or calibration scheme fails to init;
// battery_read then always returns false so callers hide the readout.
bool battery_init(void);

// Average 8 raw samples on CH3, convert once (raw -> mV -> V), where
// V = cali_mV * 0.001 * 3 (undo the 1/3 divider). Fills:
//   *volts   = pack voltage in V
//   *percent = ((V - 3.0) / 1.12) * 100, clamped 0..100 (3.0V=0%, 4.12V=100%)
//   *plugged = V >= 4.2 (USB feeding; no dedicated charge-status pin exists)
// Returns false if init failed or an ADC read errors; outputs untouched then.
bool battery_read(float *volts, int *percent, bool *plugged);

#ifdef __cplusplus
}
#endif
