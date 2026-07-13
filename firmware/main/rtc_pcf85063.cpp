#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include "rtc_pcf85063.h"
#include "user_config.h"

static const char *TAG = "PCF85063";

#define PCF85063_ADDR      0x51
#define PCF85063_SEC_REG   0x04   // seconds; bit7 = clock-integrity (VL) flag

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static inline uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline uint8_t bcd2dec(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, pdMS_TO_TICKS(1000));
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len) {
    uint8_t tmp[8];
    if (len + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, pdMS_TO_TICKS(1000));
}

bool pcf85063_init(void) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = I2C_NUM_0;
    bus_cfg.scl_io_num                   = (gpio_num_t)ESP32_I2C_SCL_PIN;
    bus_cfg.sda_io_num                   = (gpio_num_t)ESP32_I2C_SDA_PIN;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = PCF85063_ADDR;
    dev_cfg.scl_speed_hz    = 100000;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_master_probe(s_bus, PCF85063_ADDR, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC not responding at 0x%02X: %s", PCF85063_ADDR, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "PCF85063 online");
    return true;
}

i2c_master_bus_handle_t pcf85063_get_bus(void) {
    // SHTC3 (0x70) shares this bus (SDA=13, SCL=14). Call after pcf85063_init;
    // returns NULL if the bus was never created.
    return s_bus;
}

bool rtc_get_time(struct tm *out) {
    if (!s_dev || !out) return false;
    uint8_t b[7];
    if (reg_read(PCF85063_SEC_REG, b, 7) != ESP_OK) {
        ESP_LOGE(TAG, "read failed");
        return false;
    }
    if (b[0] & 0x80) {
        ESP_LOGW(TAG, "clock integrity lost (power loss); time invalid");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(b[0] & 0x7F);
    out->tm_min  = bcd2dec(b[1] & 0x7F);
    out->tm_hour = bcd2dec(b[2] & 0x3F);
    out->tm_mday = bcd2dec(b[3] & 0x3F);
    out->tm_wday = b[4] & 0x07;
    out->tm_mon  = bcd2dec(b[5] & 0x1F) - 1;
    out->tm_year = bcd2dec(b[6]) + 100;   // years since 1900; RTC holds 20xx
    out->tm_isdst = 0;
    return true;
}

bool rtc_set_time(const struct tm *in) {
    if (!s_dev || !in) return false;
    uint8_t b[7];
    b[0] = dec2bcd(in->tm_sec) & 0x7F;   // clears VL flag
    b[1] = dec2bcd(in->tm_min);
    b[2] = dec2bcd(in->tm_hour);
    b[3] = dec2bcd(in->tm_mday);
    b[4] = (uint8_t)(in->tm_wday & 0x07);
    b[5] = dec2bcd(in->tm_mon + 1);
    b[6] = dec2bcd((uint8_t)((in->tm_year + 1900) % 100));
    if (reg_write(PCF85063_SEC_REG, b, 7) != ESP_OK) {
        ESP_LOGE(TAG, "write failed");
        return false;
    }
    return true;
}
