#include "serial_debug.h"

#include <string.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#if !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#error "UART0 debug requires the system console on USB Serial/JTAG"
#endif
_Static_assert(CONFIG_PROG_UART_NUM == 1, "RL78 must use UART1; UART0 is reserved for debugging");
#define DEBUG_PIN(pin) ((pin) == 20 || (pin) == 21)
_Static_assert(!DEBUG_PIN(CONFIG_PROG_TOOL0_TX_GPIO) &&
               !DEBUG_PIN(CONFIG_PROG_TOOL0_RX_GPIO) &&
               !DEBUG_PIN(CONFIG_PROG_RESET_GPIO), "Target GPIO conflicts with debug UART0");

#define RX_HISTORY_SIZE 8192
static uint8_t s_history[RX_HISTORY_SIZE];
static uint64_t s_received;
static uint32_t s_baud = 115200, s_errors;
static SemaphoreHandle_t s_mu;
static QueueHandle_t s_events;

static void receive_once(void)
{
    uint8_t buf[1024];
    uart_event_t event;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    while (xQueueReceive(s_events, &event, 0) == pdTRUE) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) s_errors++;
    }
    int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), 0);
    if (n < 0) s_errors++;
    for (int i = 0; i < n; i++) s_history[s_received++ % RX_HISTORY_SIZE] = buf[i];
    xSemaphoreGive(s_mu);
}

static void receive_task(void *arg)
{
    while (true) {
        receive_once();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t serial_debug_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) return ESP_ERR_NO_MEM;
    esp_err_t err = uart_driver_install(UART_NUM_0, 4096, 0, 32, &s_events, 0);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
        return err;
    }
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(UART_NUM_0, &config);
    if (err == ESP_OK) err = uart_set_pin(UART_NUM_0, 21, 20, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK && xTaskCreate(receive_task, "serial_rx", 3072, NULL, 4, NULL) != pdPASS)
        err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        uart_driver_delete(UART_NUM_0);
        vSemaphoreDelete(s_mu);
        s_mu = NULL;
    }
    return err;
}

esp_err_t serial_debug_set_baud(uint32_t baud)
{
    switch (baud) {
    case 9600: case 19200: case 38400: case 57600:
    case 115200: case 230400: case 460800: case 921600: break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (!s_mu) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    esp_err_t err = uart_set_baudrate(UART_NUM_0, baud);
    if (err == ESP_OK) s_baud = baud;
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t serial_debug_send(const uint8_t *data, size_t size, size_t *written)
{
    if (!written) return ESP_ERR_INVALID_ARG;
    *written = 0;
    if (!data || !size || size > SERIAL_DEBUG_TX_MAX) return ESP_ERR_INVALID_SIZE;
    if (!s_mu) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int n = uart_write_bytes(UART_NUM_0, data, size);
    if (n > 0) *written = n;
    esp_err_t err = n == (int)size ? uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000)) : ESP_FAIL;
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t serial_debug_read(uint64_t cursor, serial_debug_rx_t *rx)
{
    if (!rx) return ESP_ERR_INVALID_ARG;
    if (!s_mu) return ESP_ERR_INVALID_STATE;
    memset(rx, 0, sizeof(*rx));
    xSemaphoreTake(s_mu, portMAX_DELAY);
    uint64_t oldest = s_received > RX_HISTORY_SIZE ? s_received - RX_HISTORY_SIZE : 0;
    if (cursor > s_received) {
        cursor = oldest;
        rx->reset = true;
    }
    if (cursor < oldest) {
        rx->lost = oldest - cursor;
        cursor = oldest;
    }
    uint64_t count = s_received - cursor;
    rx->size = count > SERIAL_DEBUG_RX_CHUNK ? SERIAL_DEBUG_RX_CHUNK : count;
    for (size_t i = 0; i < rx->size; i++) rx->data[i] = s_history[(cursor + i) % RX_HISTORY_SIZE];
    rx->next = cursor + rx->size;
    rx->more = rx->next < s_received;
    rx->baud = s_baud;
    rx->errors = s_errors;
    xSemaphoreGive(s_mu);
    return ESP_OK;
}
