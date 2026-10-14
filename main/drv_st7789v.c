#include "drv_st7789v.h"

#include <assert.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7789.h"

static const char *TAG = "lcd";

#define LCD_HOST             SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ   CONFIG_PROG_LCD_PIXEL_CLOCK_HZ
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8
#define LVGL_TICK_PERIOD_MS  10

/* ---------------- backlight (GPIO on/off at boot, LEDC PWM afterwards) ---------------- */

static bool s_ledc_ready;

void lcd_backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (!s_ledc_ready) {
        const ledc_timer_config_t timer = {
            .duty_resolution = LEDC_TIMER_13_BIT,
            .freq_hz = 400,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = LEDC_TIMER_0,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        const ledc_channel_config_t channel = {
            .channel = LEDC_CHANNEL_0,
            .gpio_num = CONFIG_PROG_LCD_BL_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        if (ledc_timer_config(&timer) != ESP_OK ||
            ledc_channel_config(&channel) != ESP_OK) {
            ESP_LOGE(TAG, "backlight ledc init failed");
            return;
        }
        s_ledc_ready = true;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(8191 * percent / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------------- LVGL flush / tick ---------------- */

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    /* Synchronous transfer (blocks until the SPI transaction completes) */
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_map);
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

void lcd_st7789v_start(void)
{
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    /* Backlight off while initializing */
    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CONFIG_PROG_LCD_BL_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    gpio_set_level(CONFIG_PROG_LCD_BL_GPIO, 0);

    ESP_LOGI(TAG, "Initialize SPI bus (SPI2, %d MHz)", LCD_PIXEL_CLOCK_HZ / 1000000);
    spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_PROG_LCD_SCLK_GPIO,
        .mosi_io_num = CONFIG_PROG_LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_PROG_LCD_DC_GPIO,
        .cs_gpio_num = CONFIG_PROG_LCD_CS_GPIO,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_PROG_LCD_RST_GPIO,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));      /* init sequence holds MADCTL=0x60 (landscape) */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    gpio_set_level(CONFIG_PROG_LCD_BL_GPIO, 1);

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();
    /* Double buffering in PSRAM: 320x40 px x2 */
    size_t buf_bytes = LCD_H_RES * 40 * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    /* full_refresh intentionally left 0: with the synchronous flush above,
     * full-screen redraws inside lv_timer_handler would hog the task watchdog.
     * Only dirty areas are flushed (fast). */
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "LVGL ready (%dx%d)", LCD_H_RES, LCD_V_RES);
}

/* All lv_timer_handler calls and UI code run in this task context:
 * LVGL objects are single-context, no locking needed. */
static void task_lvgl(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}

void lcd_st7789v_run(void)
{
    xTaskCreate(task_lvgl, "task_lvgl", 6144, NULL, 6, NULL);
}
