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
// Charging = voltage rising over the history window. At the 30s read cadence,
// 6 samples span ~3 min; a charger lifts the cell by more than BATT_RISE_V over
// that window while a discharging cell holds or falls. Tune on real hardware.
#define BATT_HIST      6
#define BATT_RISE_V    0.015f

// Resting open-circuit-voltage -> state-of-charge table for a single Li-ion
// cell (18650). The discharge curve is strongly nonlinear (a long 3.7-3.9V
// plateau holds most of the charge), so the vendor straight-line map is wrong
// through the middle. This device steady load is ~1mA (reflective LCD in LPM),
// so measured voltage sits within a few mV of the true OCV and no
// current-sense / IR compensation is needed for the table to be accurate here.
// Pairs are voltage-descending; percent is linearly interpolated between rows.
typedef struct { float v; float pct; } ocv_point_t;
static const ocv_point_t OCV_LUT[] = {
    {4.20f, 100}, {4.13f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80},
    {3.98f, 75},  {3.95f, 70}, {3.91f, 65}, {3.87f, 60}, {3.85f, 55},
    {3.84f, 50},  {3.82f, 45}, {3.80f, 40}, {3.79f, 35}, {3.77f, 30},
    {3.75f, 25},  {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},
    {3.27f, 0},
};
static float ocv_to_percent(float v) {
    const int n = sizeof(OCV_LUT) / sizeof(OCV_LUT[0]);
    if (v >= OCV_LUT[0].v)   return 100.0f;
    if (v <= OCV_LUT[n - 1].v) return 0.0f;
    for (int i = 0; i < n - 1; i++) {
        float vhi = OCV_LUT[i].v, vlo = OCV_LUT[i + 1].v;
        if (v <= vhi && v >= vlo) {
            float t = (v - vlo) / (vhi - vlo);   // 0 at vlo, 1 at vhi
            return OCV_LUT[i + 1].pct + t * (OCV_LUT[i].pct - OCV_LUT[i + 1].pct);
        }
    }
    return 0.0f;   // unreachable: the range guards above cover every v
}

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

    if (volts) *volts = v;

    // Nonlinear OCV->SoC via the table, then a light EMA so a momentary sag
    // (the brief WiFi burst on a resync) does not make the gauge jump. At the
    // 30s read cadence a gauge that eases over ~1 min reads as steady, not
    // laggy. The first reading seeds the filter directly. The charging
    // heuristic below still uses the raw averaged v, so it is unaffected.
    float lvl = ocv_to_percent(v);
    static float lvl_ema = -1.0f;
    if (lvl_ema < 0.0f) lvl_ema = lvl;
    else                lvl_ema = 0.5f * lvl_ema + 0.5f * lvl;
    if (percent) *percent = (int)(lvl_ema + 0.5f);
    // Charging detection without a VBUS/charge-status pin (this board exposes
    // none, confirmed against the Waveshare schematic and ADC example). An
    // absolute-voltage compare cannot work: a near-full cell on its own sits at
    // ~4.1V, indistinguishable from the ~4.1V charging plateau, so a threshold
    // falsely reports "plugged" while running on battery. Instead track a short
    // voltage history and report charging only when the voltage is clearly
    // RISING, which a charger does (CC/CV) and a discharging cell never does.
    // A full cell on USB reads flat, so it shows as not-charging (honest: it is
    // simply full); *percent stays valid either way so the UI always has a level.
    static float hist[BATT_HIST] = {0};
    static int   hist_n = 0;
    int idx = hist_n % BATT_HIST;
    float oldest = hist[idx];              // value from BATT_HIST reads ago
    hist[idx] = v;
    hist_n++;
    bool charging = (hist_n > BATT_HIST) && ((v - oldest) > BATT_RISE_V);
    if (plugged) *plugged = charging;
    return true;
}
