/*
 * ESP32-C3 offline programmer for Renesas RL78 MCUs.
 *
 * - WiFi SoftAP + HTTP server for firmware file management (upload / list /
 *   download / delete) and programming control.
 * - RL78 serial programming (Protocol A) over 1-wire UART (TOOL0 + RESET).
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "storage.h"
#include "wifi_ap.h"
#include "http_server.h"
#include "rl78_prog.h"
#include "front_panel.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(rl78_prog_init());
    ESP_ERROR_CHECK(wifi_ap_init());
    ESP_ERROR_CHECK(http_server_start());
    ESP_ERROR_CHECK(front_panel_init());

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "RL78 offline programmer ready");
    ESP_LOGI(TAG, "WiFi  SSID : %s", CONFIG_PROG_WIFI_SSID);
    ESP_LOGI(TAG, "TOOL0      : TX=GPIO%d RX=GPIO%d (tie to RL78 TOOL0)",
             CONFIG_PROG_TOOL0_TX_GPIO, CONFIG_PROG_TOOL0_RX_GPIO);
    ESP_LOGI(TAG, "RESET      : GPIO%d -> RL78 RESET", CONFIG_PROG_RESET_GPIO);
    ESP_LOGI(TAG, "Baud rate  : %d", CONFIG_PROG_BAUD_RATE);
    ESP_LOGI(TAG, "============================================");
}
