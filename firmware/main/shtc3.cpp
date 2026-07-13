#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include "shtc3.h"
#include "rtc_pcf85063.h"

static const char *TAG = "SHTC3";

#define SHTC3_ADDR   0x70

// 16-bit commands, MSB first on the wire.
#define CMD_WAKEUP           0x3517
#define CMD_SLEEP            0xB098
#define CMD_SOFT_RESET       0x805D
#define CMD_MEAS_T_RH_POLL   0x7866
#define CMD_READ_ID          0xEFC8

// CRC-8, polynomial 0x31 (x^8+x^5+x^4+1), init 0xFF. The datasheet writes the
// polynomial as 0x131; the low 8 bits (0x31) are used in the shift-xor loop.
#define CRC_POLY  0x31

static i2c_master_dev_handle_t s_dev = NULL;

// SHTC3 datasheet CRC over nbytes, compared against checksum. Returns true on
// match. Ported from the Waveshare demo Shtc3_CheckCrc.
static bool shtc3_crc_ok(const uint8_t *data, int nbytes, uint8_t checksum) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < nbytes; i++) {
        crc ^= data[i];
        for (int bit = 8; bit > 0; --bit) {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ CRC_POLY);
            else            crc = (uint8_t)(crc << 1);
        }
    }
    return crc == checksum;
}

// Send one 16-bit command (MSB first). i2c_master_transmit (no register
// pointer) so the two raw command bytes go on the wire unchanged.
static esp_err_t shtc3_cmd(uint16_t cmd) {
    uint8_t b[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_dev, b, 2, pdMS_TO_TICKS(1000));
}

bool shtc3_init(void) {
    i2c_master_bus_handle_t bus = pcf85063_get_bus();
    if (!bus) {
        ESP_LOGE(TAG, "shared I2C bus not ready (call after pcf85063_init)");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = SHTC3_ADDR;
    dev_cfg.scl_speed_hz    = 100000;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        s_dev = NULL;
        return false;
    }

    // Wake, read ID (with CRC), then sleep. Confirms the sensor is present and
    // talking before the render loop starts polling it.
    shtc3_cmd(CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t idcmd[2] = { (uint8_t)(CMD_READ_ID >> 8), (uint8_t)(CMD_READ_ID & 0xFF) };
    uint8_t idbuf[3] = { 0, 0, 0 };
    err = i2c_master_transmit_receive(s_dev, idcmd, 2, idbuf, 3, pdMS_TO_TICKS(1000));
    shtc3_cmd(CMD_SLEEP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ID read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!shtc3_crc_ok(idbuf, 2, idbuf[2])) {
        ESP_LOGE(TAG, "ID CRC mismatch");
        return false;
    }
    ESP_LOGI(TAG, "online, ID=0x%04X", (idbuf[0] << 8) | idbuf[1]);
    return true;
}

bool shtc3_read(float *temp_c, float *humi_pct) {
    if (!s_dev || !temp_c || !humi_pct) return false;

    if (shtc3_cmd(CMD_WAKEUP) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(50));

    if (shtc3_cmd(CMD_MEAS_T_RH_POLL) != ESP_OK) {
        shtc3_cmd(CMD_SLEEP);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t b[6] = { 0 };
    esp_err_t err = i2c_master_receive(s_dev, b, 6, pdMS_TO_TICKS(1000));
    shtc3_cmd(CMD_SLEEP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "measurement read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!shtc3_crc_ok(&b[0], 2, b[2]) || !shtc3_crc_ok(&b[3], 2, b[5])) {
        ESP_LOGW(TAG, "measurement CRC mismatch");
        return false;
    }

    uint16_t raw_t = (uint16_t)((b[0] << 8) | b[1]);
    uint16_t raw_h = (uint16_t)((b[3] << 8) | b[4]);
    *temp_c   = -45.0f + 175.0f * (float)raw_t / 65536.0f;
    *humi_pct = 100.0f * (float)raw_h / 65536.0f;
    return true;
}
