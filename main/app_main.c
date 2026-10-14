/*
 * ESP32-S3 offline programmer for Renesas RL78 MCUs.
 *
 * - WiFi SoftAP + HTTP server for firmware file management and job control
 * - ST7789V 320x240 LCD (LVGL) + 6 I2C touch keys for standalone operation
 * - RL78 serial programming (Protocol A) over 1-wire UART (TOOL0 + RESET)
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"

#include "storage.h"
#include "wifi_ap.h"
#include "http_server.h"
#include "rl78_prog.h"
#include "prog_job.h"
#include "drv_st7789v.h"
#include "drv_touch_key.h"
#include "drv_beep.h"
#include "ui_main.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    beep_init();
    beep_play(BEEP_PAT_BOOT);   /* power-on chirp */

    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(rl78_prog_init());
    ESP_ERROR_CHECK(prog_job_init());
    ESP_ERROR_CHECK(wifi_ap_init());
    ESP_ERROR_CHECK(http_server_start());

    /* OTA rollback protection: everything is up -> confirm this firmware */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA firmware marked valid (partition %s)", running->label);
    }

    /* LCD + LVGL + UI (all UI objects are created before the LVGL task starts) */
    lcd_st7789v_start();
    ui_main_start();
    lcd_st7789v_run();
    touch_key_start();

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "RL78 offline programmer ready (ESP32-S3)");
    ESP_LOGI(TAG, "WiFi  SSID : %s", CONFIG_PROG_WIFI_SSID);
    ESP_LOGI(TAG, "TOOL0      : TX=GPIO%d RX=GPIO%d (tie to RL78 TOOL0)",
             CONFIG_PROG_TOOL0_TX_GPIO, CONFIG_PROG_TOOL0_RX_GPIO);
    ESP_LOGI(TAG, "RESET      : GPIO%d -> RL78 RESET", CONFIG_PROG_RESET_GPIO);
    ESP_LOGI(TAG, "LCD        : ST7789V %dx%d, touch keys on I2C1", 320, 240);
    ESP_LOGI(TAG, "============================================");
}
