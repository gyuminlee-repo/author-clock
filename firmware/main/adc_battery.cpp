// 18650 battery gauge, ported from the Waveshare 03_ADC_Test adc_bsp.cpp:
// ADC1 channel 3 (GPIO4), ATTEN_DB_12, one-shot read + curve-fitting cali.
// Unlike the reference, this fails soft (returns false, no ESP_ERROR_CHECK
// abort) so a missing/failed ADC never bricks the clock, and it averages 8
// raw samples per read to steady the USB-plugged heuristic.
#include <stdio.h>
#include <esp_log.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "adc_battery.h"

static const char *TAG = "BATT";

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t         cali_handle = NULL;
static bool                      initialized = false;

#define BATT_CHANNEL   ADC_CHANNEL_3     // GPIO4 on ADC1
#define BATT_SAMPLES   8

bool battery_init(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &adc1_handle) != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_new_unit failed");
        adc1_handle = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    chan_cfg.atten    = ADC_ATTEN_DB_12;
    if (adc_oneshot_config_channel(adc1_handle, BATT_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_config_channel failed");
        return false;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id  = ADC_UNIT_1;
    cali_cfg.atten    = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) != ESP_OK) {
        ESP_LOGW(TAG, "adc_cali curve-fitting scheme failed");
        cali_handle = NULL;
        return false;
    }

    initialized = true;
    ESP_LOGI(TAG, "battery ADC ready on ADC1_CH3");
    return true;
}

bool battery_read(float *volts, int *percent, bool *plugged) {
    // Fail-safe: if init never succeeded the handles are NULL and a read would
    // crash, so bail before touching the ADC.
    if (!initialized || adc1_handle == NULL || cali_handle == NULL) {
        return false;
    }

    // Average the raw counts across BATT_SAMPLES reads, then convert once.
    long raw_sum = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc1_handle, BATT_CHANNEL, &raw) != ESP_OK) {
            return false;
        }
        raw_sum += raw;
    }
    int raw_avg = (int)(raw_sum / BATT_SAMPLES);

    int mv = 0;
    if (adc_cali_raw_to_voltage(cali_handle, raw_avg, &mv) != ESP_OK) {
        return false;
    }

    // Undo the 1/3 divider: pack V = pin mV * 0.001 * 3.
    float v = 0.001f * (float)mv * 3.0f;

    float lvl = ((v - 3.0f) / 1.12f) * 100.0f;   // 3.0V=0%, 4.12V=100%
    if (lvl < 0.0f)   lvl = 0.0f;
    if (lvl > 100.0f) lvl = 100.0f;

    if (volts)   *volts = v;
    if (percent) *percent = (int)(lvl + 0.5f);
    // USB feeding pushes the rail near full-charge voltage; a cell on its own
    // sags below this in use, so a high reading means plugged. This board has
    // no VBUS/charge-status pin (confirmed against the Waveshare schematic and
    // ADC example), so voltage is the only signal. Measured charging voltage
    // sits flat at ~4.10V, right on the old 4.10 compare, which made plugged
    // flicker. Latch it with hysteresis: turn on above PLUG_ON, and only turn
    // off once the cell has clearly sagged below PLUG_OFF under load.
    static bool s_plugged = false;
    if (v >= 4.05f)      s_plugged = true;
    else if (v < 3.95f)  s_plugged = false;
    if (plugged) *plugged = s_plugged;
    return true;
}
