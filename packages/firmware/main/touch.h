/**
 * touch.h — CST816D capacitive touch over I2C (Hynitron, addr 0x15).
 *
 * Single-touch + gesture support. Register map distilled from the vendor's
 * Arduino driver at
 * `docs/1.28inch_ESP32-2424S012/1-Demo/Demo_Arduino/.../CST816D.cpp`.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  gesture;  /* DOUDOU_GESTURE_* from pins.h */
    bool     pressed;
} doudou_touch_event_t;

/** Wire up the I2C bus, reset the controller, disable auto-sleep. */
esp_err_t doudou_touch_init(void);

/** Poll once. Returns ESP_OK regardless; `out->pressed` tells you if the
 *  user is touching the panel right now. */
esp_err_t doudou_touch_read(doudou_touch_event_t *out);

#ifdef __cplusplus
}
#endif
