/* ST7789V 320x240 LCD (esp_lcd) + LVGL 8.3 port layer. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_H_RES   320
#define LCD_V_RES   240

/* Initialize SPI bus, ST7789 panel, LVGL core, draw buffers and tick timer.
 * Must be called before creating any LVGL objects. */
void lcd_st7789v_start(void);

/* Start the LVGL scheduler task (lv_timer_handler loop).
 * Call AFTER all UI objects have been created (single-context design, no locks). */
void lcd_st7789v_run(void);

/* Backlight level 0..100 (0 = off). LEDC PWM, lazily initialized. */
void lcd_backlight_set(int percent);

#ifdef __cplusplus
}
#endif
