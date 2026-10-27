#include "../main/serial_debug.c"

static int lock, fail_at, deleted, events;
static bool read_fail, write_short, write_fail, wait_fail, baud_fail;
static uint8_t input[12000], output[256];
static size_t input_size, input_pos, output_size;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_at == 1 ? NULL : &lock; }
int xSemaphoreTake(SemaphoreHandle_t mu, TickType_t wait) {
    assert(mu == &lock && !lock); lock = 1; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mu) { assert(lock); lock = 0; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t mu) { assert(!lock); }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    assert(task == receive_task && stack >= 2048); return fail_at == 5 ? 0 : pdPASS;
}
void vTaskDelay(TickType_t delay) { assert(delay == 10); }
int xQueueReceive(QueueHandle_t queue, void *dst, TickType_t wait) {
    assert(queue == &events && lock);
    if (!events) return 0;
    ((uart_event_t *)dst)->type = events--;
    return pdTRUE;
}
esp_err_t uart_driver_install(int port, int rx, int tx, int count, QueueHandle_t *queue, int flags) {
    assert(port == UART_NUM_0 && rx == 4096 && tx == 0 && count == 32);
    *queue = &events; return fail_at == 2 ? ESP_FAIL : ESP_OK;
}
esp_err_t uart_driver_delete(int port) { assert(port == UART_NUM_0); deleted++; return ESP_OK; }
esp_err_t uart_param_config(int port, const uart_config_t *cfg) {
    assert(port == UART_NUM_0 && cfg->baud_rate == 115200);
    assert(cfg->data_bits == UART_DATA_8_BITS && cfg->parity == UART_PARITY_DISABLE);
    assert(cfg->stop_bits == UART_STOP_BITS_1 && cfg->flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
    return fail_at == 3 ? ESP_FAIL : ESP_OK;
}
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts) {
    assert(port == UART_NUM_0 && tx == 21 && rx == 20);
    assert(rts == UART_PIN_NO_CHANGE && cts == UART_PIN_NO_CHANGE);
    return fail_at == 4 ? ESP_FAIL : ESP_OK;
}
esp_err_t uart_set_baudrate(int port, uint32_t baud) {
    assert(port == UART_NUM_0 && lock); return baud_fail ? ESP_FAIL : ESP_OK;
}
int uart_read_bytes(int port, void *dst, uint32_t size, TickType_t wait) {
    assert(port == UART_NUM_0 && lock && wait == 0);
    if (read_fail) return -1;
    if (size > input_size - input_pos) size = input_size - input_pos;
    memcpy(dst, input + input_pos, size); input_pos += size; return size;
}
int uart_write_bytes(int port, const void *data, size_t size) {
    assert(port == UART_NUM_0 && lock && size <= sizeof(output));
    if (write_fail) return -1;
    output_size = size - (write_short ? 1 : 0);
    memcpy(output, data, output_size); return output_size;
}
esp_err_t uart_wait_tx_done(int port, TickType_t wait) {
    assert(port == UART_NUM_0 && lock && wait == 1000);
    return wait_fail ? ESP_ERR_TIMEOUT : ESP_OK;
}

int main(void) {
    serial_debug_rx_t rx, other;
    assert(serial_debug_read(0, &rx) == ESP_ERR_INVALID_STATE);
    for (fail_at = 1; fail_at <= 5; fail_at++) {
        deleted = 0;
        assert(serial_debug_init() != ESP_OK && !s_mu);
        assert(deleted == (fail_at >= 3));
    }
    fail_at = 0;
    assert(serial_debug_init() == ESP_OK);
    assert(serial_debug_read(0, &rx) == ESP_OK && !rx.size && rx.baud == 115200);
    assert(serial_debug_set_baud(0) == ESP_ERR_INVALID_ARG);
    assert(serial_debug_set_baud(115201) == ESP_ERR_INVALID_ARG);
    assert(serial_debug_set_baud(921600) == ESP_OK);
    baud_fail = true;
    assert(serial_debug_set_baud(9600) == ESP_FAIL && s_baud == 921600);
    baud_fail = false;
    for (size_t i = 0; i < sizeof(input); i++) input[i] = i;
    input_size = sizeof(input);
    while (input_pos < input_size) receive_once();
    assert(s_received == sizeof(input));
    assert(serial_debug_read(0, &rx) == ESP_OK);
    assert(rx.lost == sizeof(input) - RX_HISTORY_SIZE && rx.size == SERIAL_DEBUG_RX_CHUNK && rx.more);
    assert(memcmp(rx.data, input + rx.lost, rx.size) == 0);
    assert(serial_debug_read(0, &other) == ESP_OK && memcmp(&rx, &other, sizeof(rx)) == 0);
    while (rx.more) {
        uint64_t cursor = rx.next;
        assert(serial_debug_read(cursor, &rx) == ESP_OK && !rx.lost && !rx.reset);
        assert(memcmp(rx.data, input + cursor, rx.size) == 0);
    }
    assert(rx.next == sizeof(input));
    assert(serial_debug_read(rx.next, &rx) == ESP_OK && !rx.size);
    assert(serial_debug_read(UINT64_MAX, &rx) == ESP_OK && rx.reset && !rx.lost);
    events = 4; receive_once(); assert(s_errors == 4);
    read_fail = true; receive_once(); assert(s_errors == 5); read_fail = false;
    size_t written;
    assert(serial_debug_send(input, 0, &written) == ESP_ERR_INVALID_SIZE && !written);
    assert(serial_debug_send(input, 257, &written) == ESP_ERR_INVALID_SIZE && !written);
    assert(serial_debug_send(NULL, 1, &written) == ESP_ERR_INVALID_SIZE);
    assert(serial_debug_send(input, 256, &written) == ESP_OK && written == 256);
    assert(output_size == 256 && memcmp(input, output, 256) == 0);
    write_short = true;
    assert(serial_debug_send(input, 256, &written) == ESP_FAIL && written == 255);
    write_short = false; write_fail = true;
    assert(serial_debug_send(input, 256, &written) == ESP_FAIL && !written);
    write_fail = false; wait_fail = true;
    assert(serial_debug_send(input, 256, &written) == ESP_ERR_TIMEOUT && written == 256);
    assert(!lock);
    puts("serial debug: init cleanup, UART0 pins, 8N1, baud errors, history wrap, independent readers, overflow and TX failures PASS");
}
