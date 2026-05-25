/**
 * pins.h — board pin assignments for ESP32-2424S012 (Doudou v0 hardware).
 *
 * VERIFIED against vendor schematic + their Arduino sample
 * (docs/1.28inch_ESP32-2424S012/1-Demo/Demo_Arduino/3_1-TFT-LVGL-Benchmark/
 *  LvglBenchmark/LvglBenchmark.ino). No more "TBD" — all pins are real.
 *
 * Touch chip: Hynitron CST816D @ I2C 0x15. Supports gestures (slide/tap/long).
 */
#pragma once

/* ============================================================
 * GC9A01 round LCD over SPI2_HOST
 * ============================================================ */
#define DOUDOU_LCD_HOST          SPI2_HOST
#define DOUDOU_PIN_LCD_SCLK      6
#define DOUDOU_PIN_LCD_MOSI      7
#define DOUDOU_PIN_LCD_DC        2
#define DOUDOU_PIN_LCD_CS        10
/* PCB ties LCD reset to the rail — no MCU pin connected. */
#define DOUDOU_PIN_LCD_RST       -1
/**
 * Backlight: GPIO 3 enables the backlight rail. Vendor sample just does
 * digitalWrite(3, HIGH) once at boot. We default to that; PWM brightness
 * is experimental (we can drive LEDC on GPIO 3 if the circuit allows, but
 * the simple ON path is the documented one).
 */
#define DOUDOU_PIN_LCD_BL        3

#define DOUDOU_LCD_PIXEL_W       240
#define DOUDOU_LCD_PIXEL_H       240
/* Panel-specific quirks observed in vendor LovyanGFX config.
 *
 * Real device uses GC9A01 which needs colour inversion and BGR pixel
 * order. Wokwi's only available LCD model is ILI9341 which needs
 * neither — set CONFIG_DOUDOU_LCD_FOR_WOKWI=y (or use
 * sdkconfig.defaults.wokwi) to flip both so the preview renders the
 * intended colours. */
#include "sdkconfig.h"
#ifdef CONFIG_DOUDOU_LCD_FOR_WOKWI
#  define DOUDOU_LCD_INVERT_COLOR  false
#  define DOUDOU_LCD_BGR_ORDER     false
#  define DOUDOU_LCD_MIRROR_X      false
#  define DOUDOU_LCD_MIRROR_Y      false
#else
#  define DOUDOU_LCD_INVERT_COLOR  true
#  define DOUDOU_LCD_BGR_ORDER     true
/* GC9A01 on the ESP32-2424S012 boards we've tested boots with MX set,
 * which makes everything read left-right mirrored. Force MX=true here
 * to invert that back to natural orientation. Flip if your board acts
 * the other way around. */
#  define DOUDOU_LCD_MIRROR_X      true
#  define DOUDOU_LCD_MIRROR_Y      false
#endif

/* ============================================================
 * Touch: Hynitron CST816D over I2C
 * ============================================================ */
#define DOUDOU_I2C_NUM           I2C_NUM_0
#define DOUDOU_PIN_TOUCH_SDA     4
#define DOUDOU_PIN_TOUCH_SCL     5
#define DOUDOU_PIN_TOUCH_RST     1
#define DOUDOU_PIN_TOUCH_INT     0
#define DOUDOU_TOUCH_I2C_ADDR    0x15

/* CST816D gesture codes (matches the chip's GESTURE_ID register values). */
#define DOUDOU_GESTURE_NONE        0x00
#define DOUDOU_GESTURE_SLIDE_DOWN  0x01
#define DOUDOU_GESTURE_SLIDE_UP    0x02
#define DOUDOU_GESTURE_SLIDE_LEFT  0x03
#define DOUDOU_GESTURE_SLIDE_RIGHT 0x04
#define DOUDOU_GESTURE_SINGLE_TAP  0x05
#define DOUDOU_GESTURE_DOUBLE_TAP  0x0B
#define DOUDOU_GESTURE_LONG_PRESS  0x0C
