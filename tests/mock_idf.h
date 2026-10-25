#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_SIZE 5
#define ESP_ERR_TIMEOUT 6
#define ESP_ERR_NVS_NOT_FOUND 7
#define ESP_ERR_NVS_INVALID_LENGTH 8
static inline const char *esp_err_to_name(esp_err_t err) { return err ? "ERROR" : "OK"; }
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t e_ = (expr); if (e_) return e_; } while (0)
static inline size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) { size_t copied = len < n - 1 ? len : n - 1; memcpy(dst, src, copied); dst[copied] = 0; }
    return len;
}
#define CONFIG_PROG_FLASH_BLOCK_SIZE 1024
#define CONFIG_PROG_TOOL0_TX_GPIO 0
#define CONFIG_PROG_TOOL0_RX_GPIO 1
#define CONFIG_PROG_RESET_GPIO 2

typedef uint32_t TickType_t;
typedef int *SemaphoreHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY UINT32_MAX
#define configASSERT assert
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mu, TickType_t wait);
int xSemaphoreGive(SemaphoreHandle_t mu);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle);
void vTaskDelete(void *task);
void vTaskDelay(TickType_t delay);
TickType_t xTaskGetTickCount(void);

typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_commit(nvs_handle_t);

typedef struct { int content_len; const char *query; char response[4096]; int status; } httpd_req_t;
typedef void *httpd_handle_t;
typedef struct { int max_uri_handlers, stack_size, lru_purge_enable, recv_wait_timeout, send_wait_timeout; } httpd_config_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTPD_RESP_USE_STRLEN -1
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTP_DELETE 2
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t);
esp_err_t httpd_query_key_value(const char *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);

typedef struct { const char *base_path, *partition_label; bool format_if_mount_failed, dont_mount; } esp_vfs_littlefs_conf_t;
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *);
esp_err_t esp_littlefs_info(const char *, size_t *, size_t *);

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt; struct { bool enable_internal_pullup; } flags; } i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t, uint16_t, int);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
#define GPIO_PULLUP_ENABLE 1
esp_err_t gpio_set_level(int, int);
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(int);
