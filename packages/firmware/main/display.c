/**
 * display.c — GC9A01 panel bring-up for ESP32-2424S012.
 *
 * Pins + panel quirks come from the vendor's LovyanGFX config; see
 * `pins.h` and the vendor sample at
 * `docs/1.28inch_ESP32-2424S012/1-Demo/Demo_Arduino/3_1-TFT-LVGL-Benchmark/`.
 *
 * MVP-1a scope: enough to verify pins + power + SPI. No LVGL yet.
 */
#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"

#include "pins.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8

static esp_err_t init_backlight(void)
{
    /* Vendor demo just toggles BL high; the rail uses a simple FET gate.
     * Brightness PWM is possible via LEDC but unverified on this revision,
     * so we keep this dead simple for MVP-1a. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DOUDOU_PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "bl gpio");
    return gpio_set_level(DOUDOU_PIN_LCD_BL, 0);  /* start off */
}

esp_err_t doudou_display_backlight(uint8_t duty)
{
    /* On/off only for MVP-1a — non-zero = on. */
    return gpio_set_level(DOUDOU_PIN_LCD_BL, duty > 0 ? 1 : 0);
}

esp_err_t doudou_display_init(void)
{
    ESP_LOGI(TAG, "init backlight");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight");

    ESP_LOGI(TAG, "init spi host=%d mosi=%d sck=%d cs=%d dc=%d",
             DOUDOU_LCD_HOST, DOUDOU_PIN_LCD_MOSI, DOUDOU_PIN_LCD_SCLK,
             DOUDOU_PIN_LCD_CS, DOUDOU_PIN_LCD_DC);
    spi_bus_config_t bus = {
        .mosi_io_num = DOUDOU_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = DOUDOU_PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DOUDOU_LCD_PIXEL_W * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DOUDOU_LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                       TAG, "spi bus init");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = DOUDOU_PIN_LCD_DC,
        .cs_gpio_num = DOUDOU_PIN_LCD_CS,
        /* Vendor runs 80 MHz; we match. Drop to 40 MHz if you see corruption. */
        .pclk_hz = 80 * 1000 * 1000,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DOUDOU_LCD_HOST,
                                                 &io_cfg, &s_io),
                       TAG, "panel io spi");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DOUDOU_PIN_LCD_RST,  /* -1: hardware-tied, no MCU pin */
        .rgb_ele_order = DOUDOU_LCD_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR
                                              : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(s_io, &panel_cfg, &s_panel),
                       TAG, "new panel gc9a01");

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, DOUDOU_LCD_INVERT_COLOR);
    /* Apply explicit mirror state. Several ESP32-2424S012 boards we've
     * tested boot the GC9A01 panel with MX bit set, which makes the
     * frame buffer render left-right mirrored relative to the
     * simulator (and to common sense). esp_lcd_new_panel_gc9a01 doesn't
     * clear MX itself, so we set it deterministically here.
     * If on your board everything reads BACKWARDS after flashing this,
     * flip the first argument. */
    esp_lcd_panel_mirror(s_panel, DOUDOU_LCD_MIRROR_X, DOUDOU_LCD_MIRROR_Y);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "panel ready (invert=%d bgr=%d mirror=%d/%d)",
             DOUDOU_LCD_INVERT_COLOR, DOUDOU_LCD_BGR_ORDER,
             DOUDOU_LCD_MIRROR_X, DOUDOU_LCD_MIRROR_Y);

    doudou_display_backlight(1);
    return ESP_OK;
}

esp_err_t doudou_display_fill(uint16_t rgb565)
{
    /* Line-by-line fill to keep memory tiny — full frame would be 112KB. */
    uint16_t line[DOUDOU_LCD_PIXEL_W];
    for (int i = 0; i < DOUDOU_LCD_PIXEL_W; i++) line[i] = rgb565;
    for (int y = 0; y < DOUDOU_LCD_PIXEL_H; y++) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, DOUDOU_LCD_PIXEL_W, y + 1, line);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t doudou_display_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, pixels);
}
