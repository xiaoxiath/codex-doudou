/**
 * lvgl_port.c — bridges LVGL v9 to our esp_lcd panel + CST816D touch.
 *
 *   doudou_display_blit()  ◄── flush_cb  ◄──  LVGL
 *   doudou_touch_read()    ──► read_cb   ──►  LVGL indev
 *   esp_timer (5ms)        ──► lv_tick_inc()
 *   FreeRTOS task          ──► lv_timer_handler()
 *
 * Uses partial buffers (240 × 40 × 2 bytes = 19.2KB each, two buffers
 * for double-buffered DMA). Keeps the C3's tight SRAM comfortable.
 */
#include "lvgl_port.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "display.h"
#include "touch.h"
#include "pins.h"

static const char *TAG = "lvgl_port";

#define LVGL_BUF_LINES        40
#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_STACK     6144
/* Same priority as the touch + WS tasks (both 5). At prio 2 the LVGL
 * task was being preempted ~every 10 ms by touch polling and again by
 * inbound WS messages, which chopped up animation frames and made
 * breath/blink visibly stutter. Same prio means cooperative time
 * slicing — LVGL gets fair CPU share. */
#define LVGL_TASK_PRIO         5

static SemaphoreHandle_t s_lock = NULL;
static lv_display_t *s_display = NULL;
static lv_indev_t   *s_indev   = NULL;
static lv_color_t   *s_buf_a   = NULL;
static lv_color_t   *s_buf_b   = NULL;

bool doudou_lvgl_lock(int timeout_ms)
{
    if (!s_lock) return false;
    const TickType_t t = (timeout_ms < 0)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE;
}

void doudou_lvgl_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    /* LVGL v9 writes RGB565 in CPU native byte order (little-endian on
     * the C3). GC9A01 (and most TFT controllers) expect big-endian over
     * SPI — without this swap, yellow renders as cyan, dark as pink, etc.
     * Vendor LovyanGFX driver does the same via setSwapBytes(true).
     * On the Wokwi ILI9341 stand-in the model interprets either order,
     * so the swap is a no-op visually there. */
#ifndef CONFIG_DOUDOU_LCD_FOR_WOKWI
    lv_draw_sw_rgb565_swap(px_map, w * h);
#endif
    doudou_display_blit(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static void lvgl_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    doudou_touch_event_t e = {0};
    doudou_touch_read(&e);
    data->state = e.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = e.x;
    data->point.y = e.y;
    /* Cheap diagnostic: log every 5th press so we can see what coords
     * LVGL is getting and confirm the indev pipeline is alive. */
    static int s_press_log = 0;
    if (e.pressed) {
        if ((s_press_log++ % 5) == 0) {
            ESP_LOGI("touch", "indev x=%d y=%d", e.x, e.y);
        }
    }
}

static void tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

extern void doudou_pet_apply_pending_screen(void);

static void lvgl_task(void *arg)
{
    (void)arg;
    static int s_iter = 0;
    while (1) {
        uint32_t sleep_ms = 10;
        /* Verbose only for the first few iterations (boot bring-up sanity)
         * and on slow frames (timer > 200 ms = obvious stutter). Steady-
         * state chatter at INFO level was burning UART bandwidth and
         * blocking other tasks that wanted to log. */
        bool verbose = s_iter < 5;
        if (verbose) ESP_LOGI(TAG, "iter=%d → take_lock", s_iter);
        bool locked = doudou_lvgl_lock(50);
        if (verbose) ESP_LOGI(TAG, "iter=%d locked=%d", s_iter, locked ? 1 : 0);
        if (locked) {
            doudou_pet_apply_pending_screen();
            int64_t t0 = esp_timer_get_time();
            sleep_ms = lv_timer_handler();
            int64_t dt_us = esp_timer_get_time() - t0;
            if (verbose || dt_us > 200000) {
                ESP_LOGI(TAG, "iter=%d timer %lld us sleep=%lu",
                         s_iter, dt_us, (unsigned long)sleep_ms);
            }
            doudou_lvgl_unlock();
        }
        s_iter++;
        if (sleep_ms < 1)  sleep_ms = 1;
        if (sleep_ms > 50) sleep_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

esp_err_t doudou_lvgl_init(void)
{
    ESP_LOGI(TAG, "init LVGL %d.%d.%d",
             lv_version_major(), lv_version_minor(), lv_version_patch());

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    /* Two partial draw buffers — DMA-capable, internal SRAM. */
    const size_t buf_px = DOUDOU_LCD_PIXEL_W * LVGL_BUF_LINES;
    s_buf_a = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf_b = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf_a || !s_buf_b) {
        ESP_LOGE(TAG, "draw buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_display = lv_display_create(DOUDOU_LCD_PIXEL_W, DOUDOU_LCD_PIXEL_H);
    lv_display_set_buffers(s_display,
                           s_buf_a, s_buf_b,
                           buf_px * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_indev_read_cb);

    /* esp_timer ticks LVGL's clock independent of the task scheduler. */
    const esp_timer_create_args_t tick_args = {
        .callback = &tick_timer_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, LVGL_TICK_PERIOD_MS * 1000));

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL ready (display=%dx%d, %d-line buffers, tick=%dms)",
             DOUDOU_LCD_PIXEL_W, DOUDOU_LCD_PIXEL_H, LVGL_BUF_LINES, LVGL_TICK_PERIOD_MS);
    return ESP_OK;
}
