#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// Waveshare ESP32-S3-RLCD-4.2 pin map.
// ST7305 mono reflective panel, 400x300 landscape, 1bpp. No backlight.

// Panel resolution (landscape)
#define LCD_WIDTH   400
#define LCD_HEIGHT  300

// ST7305 SPI (clock must stay at or below 1MHz)
#define RLCD_DC_PIN     5
#define RLCD_CS_PIN     40
#define RLCD_SCK_PIN    11
#define RLCD_MOSI_PIN   12
#define RLCD_RST_PIN    41
#define RLCD_TE_PIN     6      // optional tearing-effect line, unused
#define RLCD_SPI_HZ     (1 * 1000 * 1000)   // 1MHz ceiling for ST7305

// PCF85063 RTC (I2C)
#define ESP32_I2C_SDA_PIN   13
#define ESP32_I2C_SCL_PIN   14

// Buttons
#define KEY_BUTTON_PIN  18    // active low, short press toggles clock/calendar
// BOOT (GPIO0) is unused.

#endif
