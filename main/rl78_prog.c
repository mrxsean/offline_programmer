#include "rl78_prog.h"

#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

static const char *TAG = "rl78";

/* ---- Protocol constants (RL78 Protocol A) ---- */
#define SOH 0x01
#define STX 0x02
#define ETX 0x03
#define ETB 0x17

#define CMD_RESET             0x00
#define CMD_VERIFY            0x13
#define CMD_BLOCK_ERASE       0x22
#define CMD_BLOCK_BLANK_CHECK 0x32
#define CMD_PROGRAMMING       0x40
#define CMD_BAUD_RATE_SET     0x9A
#define CMD_SILICON_SIGNATURE 0xC0

#define STATUS_COMMAND_NUMBER_ERROR 0x04
#define STATUS_PARAMETER_ERROR      0x05
#define STATUS_ACK                  0x06
#define STATUS_CHECKSUM_ERROR       0x07
#define STATUS_VERIFY_ERROR         0x0F
#define STATUS_PROTECT_ERROR        0x10
#define STATUS_NACK                 0x15
#define STATUS_ERASE_ERROR          0x1A
#define STATUS_BLANK_ERROR          0x1B
#define STATUS_WRITE_ERROR          0x1C

#define SET_MODE_1WIRE_UART 0x3A

#define DEFAULT_BAUD 115200
#define RX_BUF_SIZE  1024

/* Timeouts (ms) */
#define T_ECHO   100
#define T_RESP   1000
#define T_WRITE  3000
#define T_FINISH 3000

static uart_port_t s_uart;
static int s_tx_gpio;
static int s_rx_gpio;
static int s_reset_gpio;
static uint8_t s_last_nack;

uint8_t rl78_last_nack(void)
{
    return s_last_nack;
}

const char *rl78_status_str(uint8_t status)
{
    switch (status) {
    case STATUS_ACK:                  return "ACK";
    case STATUS_NACK:                 return "NACK";
    case STATUS_COMMAND_NUMBER_ERROR: return "命令编号错误";
    case STATUS_PARAMETER_ERROR:      return "参数错误";
    case STATUS_CHECKSUM_ERROR:       return "校验和错误";
    case STATUS_VERIFY_ERROR:         return "校验错误";
    case STATUS_PROTECT_ERROR:        return "保护错误(已加密)";
    case STATUS_ERASE_ERROR:          return "擦除错误";
    case STATUS_BLANK_ERROR:          return "非空白";
    case STATUS_WRITE_ERROR:          return "写入错误";
    default:                          return "未知状态";
    }
}

/* Negative-sum checksum over the LEN..DATA fields. */
static uint8_t frame_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum -= data[i];
    }
    return (uint8_t)(sum & 0xFF);
}

/* Read exactly `len` bytes, returns number of bytes read. */
static int read_exact(uint8_t *buf, int len, uint32_t timeout_ms)
{
    int got = 0;
    while (got < len) {
        int r = uart_read_bytes(s_uart, buf + got, len - got, pdMS_TO_TICKS(timeout_ms));
        if (r <= 0) {
            break;
        }
        got += r;
    }
    return got;
}

/* Discard our own echo in 1-wire mode. */
static void discard_echo(int len)
{
    uint8_t dump[128];
    int got = 0;
    while (got < len) {
        int chunk = len - got;
        if (chunk > (int)sizeof(dump)) {
            chunk = sizeof(dump);
        }
        int r = uart_read_bytes(s_uart, dump, chunk, pdMS_TO_TICKS(T_ECHO));
        if (r <= 0) {
            ESP_LOGW(TAG, "echo short: %d/%d", got, len);
            break;
        }
        got += r;
    }
}

static esp_err_t uart_send_frame(const uint8_t *buf, int len)
{
    int w = uart_write_bytes(s_uart, buf, len);
    if (w != len) {
        return ESP_ERR_TIMEOUT;
    }
    if (uart_wait_tx_done(s_uart, pdMS_TO_TICKS(T_RESP)) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    discard_echo(len);
    return ESP_OK;
}

/* Command frame: SOH LEN CMD DATA.. SUM ETX */
static esp_err_t send_cmd(uint8_t cmd, const uint8_t *data, int len)
{
    uint8_t buf[16];
    if (len > (int)sizeof(buf) - 5) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = SOH;
    buf[1] = (uint8_t)(len + 1);
    buf[2] = cmd;
    if (len > 0) {
        memcpy(&buf[3], data, len);
    }
    buf[len + 3] = frame_checksum(&buf[1], len + 2);
    buf[len + 4] = ETX;
    return uart_send_frame(buf, len + 5);
}

/* Data frame: STX LEN DATA.. SUM ETB/ETX  (LEN==0 means 256) */
static esp_err_t send_data_frame(const uint8_t *data, int len, bool last)
{
    uint8_t buf[260 + 4];
    if (len > 256) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = STX;
    buf[1] = (uint8_t)(len & 0xFF);
    memcpy(&buf[2], data, len);
    buf[len + 2] = frame_checksum(&buf[1], len + 1);
    buf[len + 3] = last ? ETX : ETB;
    return uart_send_frame(buf, len + 4);
}

/*
 * Receive a response frame: STX LEN DATA.. SUM ETB/ETX.
 * Returns data length (>=0) or a negative error code.
 */
static int recv_response(uint8_t *data, int max_len, uint32_t timeout_ms)
{
    uint8_t hdr[2];
    int hn = read_exact(hdr, 2, timeout_ms);
    if (hn != 2) {
        ESP_LOGW(TAG, "resp header timeout (%d/2 bytes)", hn);
        return -1;
    }
    if (hdr[0] != STX) {
        uint8_t extra[8];
        int n = uart_read_bytes(s_uart, extra, sizeof(extra), pdMS_TO_TICKS(30));
        ESP_LOGW(TAG, "resp bad start: %02X %02X (+%dB) - line noise or wrong baud?", hdr[0], hdr[1], n);
        return -2;
    }
    int dlen = (hdr[1] == 0) ? 256 : hdr[1];
    if (dlen > max_len) {
        ESP_LOGW(TAG, "resp too long %d", dlen);
        return -3;
    }
    uint8_t tail[2];
    if (read_exact(data, dlen, timeout_ms) != dlen ||
        read_exact(tail, 2, timeout_ms) != 2) {
        ESP_LOGW(TAG, "resp body timeout");
        return -1;
    }
    if (tail[1] != ETX && tail[1] != ETB) {
        ESP_LOGW(TAG, "resp bad end 0x%02X", tail[1]);
        return -2;
    }
    /* SUM must equal the two's complement of (LEN + DATA bytes) */
    uint32_t acc = 0;
    acc -= hdr[1];
    for (int i = 0; i < dlen; i++) {
        acc -= data[i];
    }
    if ((uint8_t)(acc & 0xFF) != tail[0]) {
        ESP_LOGW(TAG, "resp checksum error: calc 0x%02X != recv 0x%02X (len=%d)",
                 (uint8_t)(acc & 0xFF), tail[0], dlen);
        return -4;
    }
    return dlen;
}

/* Receive a response and require ACK as the first byte. Returns payload length or negative error. */
static int recv_ack(uint8_t *data, int max_len, int expect_len, uint32_t timeout_ms)
{
    int n = recv_response(data, max_len, timeout_ms);
    if (n < 0) {
        return n;
    }
    if (n != expect_len) {
        ESP_LOGW(TAG, "resp len %d != expected %d", n, expect_len);
        return -5;
    }
    if (data[0] != STATUS_ACK) {
        s_last_nack = data[0];
        ESP_LOGW(TAG, "target NACK 0x%02X (%s)", data[0], rl78_status_str(data[0]));
        return -6;
    }
    return n;
}

/* ---------------- commands ---------------- */

static esp_err_t cmd_baud_rate_set(int baud, int voltage_x10)
{
    uint8_t code;
    switch (baud) {
    case 250000:  code = 0x01; break;
    case 500000:  code = 0x02; break;
    case 1000000: code = 0x03; break;
    default:
        baud = DEFAULT_BAUD;
        /* fall through */
    case DEFAULT_BAUD:
        code = 0x00;
        break;
    }

    uint8_t req[2] = {code, (uint8_t)voltage_x10};
    ESP_RETURN_ON_ERROR(send_cmd(CMD_BAUD_RATE_SET, req, sizeof(req)), TAG, "baud set send failed");

    uint8_t resp[3];
    if (recv_ack(resp, sizeof(resp), 3, T_RESP) < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "baud rate set OK: %d bps, osc %u MHz, %s",
             baud, resp[1], resp[2] == 0 ? "full-speed" : "wide-voltage");

    if (baud != DEFAULT_BAUD) {
        ESP_RETURN_ON_ERROR(uart_set_baudrate(s_uart, baud), TAG, "uart_set_baudrate failed");
    }
    return ESP_OK;
}

static esp_err_t cmd_silicon_signature(rl78_signature_t *sig)
{
    ESP_RETURN_ON_ERROR(send_cmd(CMD_SILICON_SIGNATURE, NULL, 0), TAG, "signature send failed");

    uint8_t ack[1];
    if (recv_ack(ack, sizeof(ack), 1, T_RESP) < 0) {
        return ESP_FAIL;
    }
    uint8_t data[22];
    if (recv_response(data, sizeof(data), T_RESP) != 22) {
        return ESP_FAIL;
    }
    if (sig) {
        memcpy(sig->device_code, data, 3);
        memcpy(sig->device_name, data + 3, 10);
        sig->device_name[10] = '\0';
        sig->code_flash_end = (uint32_t)data[13] | ((uint32_t)data[14] << 8) | ((uint32_t)data[15] << 16);
        sig->data_flash_end = (uint32_t)data[16] | ((uint32_t)data[17] << 8) | ((uint32_t)data[18] << 16);
        memcpy(sig->fw_version, data + 19, 3);
    }
    return ESP_OK;
}

/* ---------------- public API ---------------- */

esp_err_t rl78_prog_init(void)
{
    s_uart       = (uart_port_t)CONFIG_PROG_UART_NUM;
    s_tx_gpio    = CONFIG_PROG_TOOL0_TX_GPIO;
    s_rx_gpio    = CONFIG_PROG_TOOL0_RX_GPIO;
    s_reset_gpio = CONFIG_PROG_RESET_GPIO;

    uart_config_t ucfg = {
        .baud_rate = DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(s_uart, RX_BUF_SIZE, 0, 0, NULL, 0), TAG, "uart install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(s_uart, &ucfg), TAG, "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(s_uart, s_tx_gpio, s_rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart set pin failed");

    /* RESET line: push-pull output, idle high (target running) */
    gpio_reset_pin(s_reset_gpio);
    gpio_set_direction(s_reset_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_reset_gpio, 1);

    return ESP_OK;
}

/*
 * Single mode-entry attempt. NOTE: all short delays use esp_rom_delay_us()
 * because pdMS_TO_TICKS() truncates to 0 below 10ms (CONFIG_FREERTOS_HZ=100),
 * which previously collapsed the critical TOOL0-low hold time.
 */
static esp_err_t try_enter_once(void)
{
    /* Take manual control of TOOL0 (TX pin) */
    gpio_reset_pin(s_tx_gpio);
    gpio_set_direction(s_tx_gpio, GPIO_MODE_OUTPUT);

    /* Mode entry sequence: TOOL0 low while RESET rises */
    gpio_set_level(s_reset_gpio, 0);
    gpio_set_level(s_tx_gpio, 0);
    esp_rom_delay_us(5000);
    uart_flush_input(s_uart);
    gpio_set_level(s_reset_gpio, 1);
    esp_rom_delay_us(10000);    /* tHD: keep TOOL0 low after RESET release */

    /* Release TOOL0: hand the pin back to the UART (idle level high) */
    ESP_RETURN_ON_ERROR(uart_set_pin(s_uart, s_tx_gpio, s_rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart re-attach failed");
    esp_rom_delay_us(5000);     /* allow the target UART to initialise */
    uart_flush_input(s_uart);

    /* Select 1-wire UART mode */
    uint8_t mode = SET_MODE_1WIRE_UART;
    uart_write_bytes(s_uart, &mode, 1);
    uart_wait_tx_done(s_uart, pdMS_TO_TICKS(T_ECHO));
    uint8_t echo = 0;
    int e = uart_read_bytes(s_uart, &echo, 1, pdMS_TO_TICKS(T_ECHO));
    if (e == 1 && echo == SET_MODE_1WIRE_UART) {
        ESP_LOGI(TAG, "mode byte echo OK (TX/RX loop works)");
    } else {
        ESP_LOGW(TAG, "mode byte echo missing (r=%d v=0x%02X): check TOOL0 wiring/levels", e, echo);
    }
    esp_rom_delay_us(2000);

    /* Negotiate baud rate */
    return cmd_baud_rate_set(CONFIG_PROG_BAUD_RATE, CONFIG_PROG_TARGET_VOLTAGE_X10);
}

esp_err_t rl78_enter_programming_mode(rl78_signature_t *sig)
{
    s_last_nack = 0;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= 3; attempt++) {
        err = try_enter_once();
        if (err == ESP_OK) {
            err = cmd_silicon_signature(sig);
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "enter programming mode: attempt %d failed", attempt);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGE(TAG, "cannot enter programming mode: target not responding");
    return ESP_ERR_NOT_FOUND;
}

void rl78_reset_target(void)
{
    gpio_set_level(s_reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_reset_gpio, 1);
}

esp_err_t rl78_block_blank_check(uint32_t addr, uint32_t block_size, bool *is_blank)
{
    uint32_t end = addr + block_size - 1;
    uint8_t req[7] = {
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)(end & 0xFF),  (uint8_t)((end >> 8) & 0xFF),  (uint8_t)((end >> 16) & 0xFF),
        0x00,
    };
    ESP_RETURN_ON_ERROR(send_cmd(CMD_BLOCK_BLANK_CHECK, req, sizeof(req)), TAG, "blank check send failed");

    uint8_t resp[1];
    int n = recv_response(resp, sizeof(resp), T_RESP);
    if (n != 1) {
        return ESP_FAIL;
    }
    if (resp[0] == STATUS_ACK) {
        *is_blank = true;
        return ESP_OK;
    }
    if (resp[0] == STATUS_BLANK_ERROR) {
        *is_blank = false;
        return ESP_OK;
    }
    s_last_nack = resp[0];
    ESP_LOGW(TAG, "blank check NACK 0x%02X (%s)", resp[0], rl78_status_str(resp[0]));
    return ESP_FAIL;
}

esp_err_t rl78_block_erase(uint32_t addr)
{
    uint8_t req[3] = {
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
    };
    ESP_RETURN_ON_ERROR(send_cmd(CMD_BLOCK_ERASE, req, sizeof(req)), TAG, "erase send failed");

    uint8_t resp[1];
    if (recv_ack(resp, sizeof(resp), 1, T_WRITE) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rl78_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    uint32_t end = addr + len - 1;
    uint8_t req[6] = {
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)(end & 0xFF),  (uint8_t)((end >> 8) & 0xFF),  (uint8_t)((end >> 16) & 0xFF),
    };
    ESP_RETURN_ON_ERROR(send_cmd(CMD_PROGRAMMING, req, sizeof(req)), TAG, "program send failed");

    uint8_t resp[2];
    if (recv_ack(resp, sizeof(resp), 1, T_RESP) < 0) {
        return ESP_FAIL;
    }

    uint32_t remaining = len;
    const uint8_t *p = data;
    while (remaining > 0) {
        uint32_t chunk = remaining > 256 ? 256 : remaining;
        bool last = (remaining <= 256);
        ESP_RETURN_ON_ERROR(send_data_frame(p, chunk, last), TAG, "program data send failed");
        if (recv_ack(resp, sizeof(resp), 2, T_WRITE) < 0) {
            return ESP_FAIL;
        }
        if (resp[1] != STATUS_ACK) {
            s_last_nack = resp[1];
            ESP_LOGW(TAG, "program unit NACK 0x%02X (%s) @0x%06" PRIX32,
                     resp[1], rl78_status_str(resp[1]), addr + (uint32_t)(p - data));
            return ESP_FAIL;
        }
        p += chunk;
        remaining -= chunk;
    }

    /* Wait for the write to complete, then read the completion status (Protocol A) */
    esp_rom_delay_us((len / 1024 + 1) * 1500);
    if (recv_ack(resp, sizeof(resp), 1, T_FINISH) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rl78_verify(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    uint32_t end = addr + len - 1;
    uint8_t req[6] = {
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF), (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)(end & 0xFF),  (uint8_t)((end >> 8) & 0xFF),  (uint8_t)((end >> 16) & 0xFF),
    };
    ESP_RETURN_ON_ERROR(send_cmd(CMD_VERIFY, req, sizeof(req)), TAG, "verify send failed");

    uint8_t resp[2];
    if (recv_ack(resp, sizeof(resp), 1, T_RESP) < 0) {
        return ESP_FAIL;
    }

    uint32_t remaining = len;
    const uint8_t *p = data;
    while (remaining > 0) {
        uint32_t chunk = remaining > 256 ? 256 : remaining;
        bool last = (remaining <= 256);
        ESP_RETURN_ON_ERROR(send_data_frame(p, chunk, last), TAG, "verify data send failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        if (recv_ack(resp, sizeof(resp), 2, T_WRITE) < 0) {
            return ESP_FAIL;
        }
        if (resp[1] != STATUS_ACK) {
            s_last_nack = resp[1];
            ESP_LOGW(TAG, "verify mismatch NACK 0x%02X (%s) @0x%06" PRIX32,
                     resp[1], rl78_status_str(resp[1]), addr + (uint32_t)(p - data));
            return ESP_ERR_INVALID_CRC;
        }
        p += chunk;
        remaining -= chunk;
    }
    return ESP_OK;
}
