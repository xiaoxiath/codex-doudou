/**
 * touch.c — CST816D driver using the IDF v5.3 i2c_master API.
 *
 * Equivalent to the vendor's Arduino `CST816D` class but as plain C.
 * Register layout (verified against vendor sketch):
 *   0x01 [ro] : gesture ID (DOUDOU_GESTURE_*)
 *   0x02 [ro] : finger count (0 or 1 for this chip)
 *   0x03..06  : touch coords — { xH, xL, yH, yL }, 12-bit each
 *   0xFE 0xFF : disable auto-sleep (write at init)
 */
#include "touch.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pins.h"

static const char *TAG = "touch";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

#define I2C_TIMEOUT_MS 50

static esp_err_t cst_write_u8(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t cst_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

static esp_err_t init_pins_and_reset(void)
{
    /* INT pin: vendor pulses it HIGH then LOW pre-reset to put CST816D into a
     * known idle state. We mirror that, then leave the pin as input WITH
     * pull-up so the open-drain INT line has a defined high level
     * between touches (CST816D drives it low on touch, expects host to
     * sense the line; floating input + intermittent reads was returning
     * garbage on real hardware). */
    if (DOUDOU_PIN_TOUCH_INT >= 0) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << DOUDOU_PIN_TOUCH_INT,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "int gpio out");
        gpio_set_level(DOUDOU_PIN_TOUCH_INT, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(DOUDOU_PIN_TOUCH_INT, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        /* Flip to input with pull-up enabled. */
        gpio_config_t int_in = {
            .pin_bit_mask = 1ULL << DOUDOU_PIN_TOUCH_INT,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_in), TAG, "int gpio in");
    }

    /* Hardware reset: 10ms low, 300ms high. */
    if (DOUDOU_PIN_TOUCH_RST >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << DOUDOU_PIN_TOUCH_RST,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio");
        gpio_set_level(DOUDOU_PIN_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(DOUDOU_PIN_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return ESP_OK;
}

esp_err_t doudou_touch_init(void)
{
    ESP_LOGI(TAG, "init i2c sda=%d scl=%d addr=0x%02x",
             DOUDOU_PIN_TOUCH_SDA, DOUDOU_PIN_TOUCH_SCL, DOUDOU_TOUCH_I2C_ADDR);

    ESP_RETURN_ON_ERROR(init_pins_and_reset(), TAG, "reset");

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = DOUDOU_I2C_NUM,
        .scl_io_num = DOUDOU_PIN_TOUCH_SCL,
        .sda_io_num = DOUDOU_PIN_TOUCH_SDA,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "new i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DOUDOU_TOUCH_I2C_ADDR,
        .scl_speed_hz = 400 * 1000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                       TAG, "add i2c dev");

    /* 0xFE / 0xFF = disable auto-enter-deep-sleep, per vendor init. */
    esp_err_t err = cst_write_u8(0xFE, 0xFF);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init register write failed (controller silent?): %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "touch ready");
    return ESP_OK;
}

esp_err_t doudou_touch_read(doudou_touch_event_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    out->pressed = false;
    out->x = out->y = 0;
    out->gesture = DOUDOU_GESTURE_NONE;
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    static uint32_t s_read_count = 0;
    static uint32_t s_pressed_count = 0;
    s_read_count++;

    uint8_t finger = 0;
    esp_err_t err = cst_read(0x02, &finger, 1);
    if (err != ESP_OK) {
        if (s_read_count % 500 == 0) {
            ESP_LOGW(TAG, "i2c reads keep failing (%lu reads): %s",
                     (unsigned long)s_read_count, esp_err_to_name(err));
        }
        return ESP_OK;
    }
    /* The CST816D supports a SINGLE finger only. Anything other than
     * exactly 1 (e.g. 64, 0x80) is I²C bus noise, not a touch. Treating
     * it as "pressed" was the root cause of phantom screen switches —
     * a stray bus glitch + the chip's latched gesture register would
     * race past the streak filter together. */
    out->pressed = (finger == 1);
    if (out->pressed) {
        s_pressed_count++;
        if (s_pressed_count <= 5 || s_pressed_count % 20 == 0) {
            ESP_LOGI(TAG, "press fingers=%u (count=%lu)", finger, (unsigned long)s_pressed_count);
        }
    } else if (finger != 0) {
        /* Garbage finger count (not 0, not 1) — log sparsely so we can
         * see how often the bus glitches without flooding UART. */
        static uint32_t s_garbage = 0;
        if (++s_garbage <= 5 || s_garbage % 50 == 0) {
            ESP_LOGW(TAG, "ignoring bogus finger=%u (garbage #%lu)",
                     finger, (unsigned long)s_garbage);
        }
    }

    uint8_t gesture = 0;
    if (cst_read(0x01, &gesture, 1) == ESP_OK) {
        /* Only log on CHANGE — the CST816D latches the gesture register
         * for 1+ second after a touch, so logging on every read at
         * 100 Hz floods the UART (~130 lines/s) and starves the WS task
         * enough that the bridge drops the connection. */
        static uint8_t s_last_gesture = 0xFF;
        if (gesture != s_last_gesture) {
            if (gesture != 0) {
                ESP_LOGI(TAG, "gesture=0x%02x", gesture);
            }
            s_last_gesture = gesture;
        }
        out->gesture = gesture;
    }

    uint8_t buf[4] = {0};
    if (cst_read(0x03, buf, 4) == ESP_OK) {
        out->x = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
        out->y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
        /* Coords outside the 240x240 panel mean the read raced an
         * internal chip update — invalidate the press. */
        if (out->pressed && (out->x >= 240 || out->y >= 240)) {
            out->pressed = false;
        }
    }
    return ESP_OK;
}
