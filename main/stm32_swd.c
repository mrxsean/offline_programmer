#include "stm32_swd.h"

#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "swd";

/* Half period of the SWD clock in microseconds (500 kHz). */
#define SWD_HALF_PERIOD_US 1

/* DP register addresses (byte address, bits [3:2] go into the request) */
#define DP_DPIDR   0x0   /* read only */
#define DP_ABORT   0x0   /* write only */
#define DP_CTRL    0x4   /* CTRL/STAT */
#define DP_SELECT  0x8
#define DP_RDBUFF  0xC

/* MEM-AP registers */
#define AP_CSW  0x0
#define AP_TAR  0x4
#define AP_DRW  0xC

#define ACK_OK    1
#define ACK_WAIT  2
#define ACK_FAULT 4

#define SWD_RETRIES 3

static int s_clk;
static int s_dio;
static int s_rst;
static uint32_t s_idcode;

/* ---------------- bit level ---------------- */

static inline void swclk(void)
{
    gpio_set_level(s_clk, 1);
    esp_rom_delay_us(SWD_HALF_PERIOD_US);
    gpio_set_level(s_clk, 0);
    esp_rom_delay_us(SWD_HALF_PERIOD_US);
}

static inline void dio_output(void)
{
    gpio_set_direction(s_dio, GPIO_MODE_OUTPUT);
}

static inline void dio_input(void)
{
    gpio_set_direction(s_dio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(s_dio, GPIO_PULLUP_ONLY);   /* SWDIO needs a pull-up while undriven */
}

static void swd_write_bit(int b)
{
    gpio_set_level(s_dio, b);
    swclk();
}

static int swd_read_bit(void)
{
    /* SWD: the transmitter changes SWDIO on the falling edge and the
     * receiver samples on the rising edge -> sample with clock HIGH. */
    gpio_set_level(s_clk, 1);
    esp_rom_delay_us(SWD_HALF_PERIOD_US);
    int b = gpio_get_level(s_dio);
    gpio_set_level(s_clk, 0);
    esp_rom_delay_us(SWD_HALF_PERIOD_US);
    return b;
}

/* 50+ clocks with SWDIO high */
static void swd_line_reset(void)
{
    dio_output();
    gpio_set_level(s_dio, 1);
    for (int i = 0; i < 51; i++) {
        swclk();
    }
}

/* JTAG -> SWD select sequence: 0x79E7 sent LSB first */
static void swd_jtag_to_swd(void)
{
    dio_output();
    for (int i = 0; i < 16; i++) {
        swd_write_bit((0x79E7 >> i) & 1);
    }
}

static void swd_idle_clocks(int n)
{
    dio_output();
    gpio_set_level(s_dio, 0);
    for (int i = 0; i < n; i++) {
        swclk();
    }
}

/* ---------------- transfer layer ---------------- */

static esp_err_t swd_dp_write_raw(uint8_t reg, uint32_t val);

/*
 * One SWD transaction (request/ack/data) with retry on WAIT/FAULT
 * and parity check on reads.
 * read: 1 = read, 0 = write. ap: 1 = AP, 0 = DP.
 */
static esp_err_t swd_transfer(int ap, int read, uint8_t reg, uint32_t *data)
{
    for (int attempt = 0; attempt < SWD_RETRIES; attempt++) {
        uint8_t a2 = (reg >> 2) & 1;
        uint8_t a3 = (reg >> 3) & 1;
        uint8_t parity = (uint8_t)((ap ^ read ^ a2 ^ a3) & 1);
        /* start(1) | ap | rnw | a2 a3 | parity | stop(0) | park(1) */
        uint8_t req = 0x81 | ((ap & 1) << 1) | ((read & 1) << 2) |
                      (a2 << 3) | (a3 << 4) | (parity << 5);

        dio_output();
        for (int i = 0; i < 8; i++) {
            swd_write_bit((req >> i) & 1);
        }

        /* turnaround -> target */
        dio_input();
        swclk();

        int ack = 0;
        for (int i = 0; i < 3; i++) {
            ack |= swd_read_bit() << i;
        }

        if (ack == ACK_OK) {
            if (read) {
                uint32_t v = 0;
                int par = 0;
                for (int i = 0; i < 32; i++) {
                    int b = swd_read_bit();
                    v |= ((uint32_t)b << i);
                    par ^= b;
                }
                int pbit = swd_read_bit();
                /* turnaround -> host */
                dio_output();
                swclk();
                if (par != pbit) {
                    ESP_LOGW(TAG, "read parity error (ap=%d reg=0x%X)", ap, reg);
                    swd_line_reset();
                    continue;
                }
                *data = v;
                return ESP_OK;
            }
            /* write: turnaround -> host, then 32 data + parity */
            dio_output();
            swclk();
            uint32_t v = *data;
            int par = 0;
            for (int i = 0; i < 32; i++) {
                int b = (v >> i) & 1;
                swd_write_bit(b);
                par ^= b;
            }
            swd_write_bit(par);
            return ESP_OK;
        }

        /* WAIT or FAULT: a few idle clocks (drive low), then recover */
        dio_output();
        gpio_set_level(s_dio, 0);
        for (int i = 0; i < 4; i++) {
            swclk();
        }
        if (ack == ACK_FAULT) {
            ESP_LOGW(TAG, "FAULT (ap=%d reg=0x%X), recover", ap, reg);
            swd_line_reset();
            /* ABORT: clear sticky error flags (best effort) */
            uint32_t abort_val = 0x1E;
            swd_dp_write_raw(DP_ABORT, abort_val);
        } else {
            /* ack==0: line held low; ack==7: floating high (no target driving);
               ack==2: WAIT; ack==3/5/6: garbled (timing?) */
            ESP_LOGW(TAG, "no ACK=OK (ack=%d, ap=%d %s reg=0x%X, try %d)%s",
                     ack, ap, read ? "RD" : "WR", reg, attempt + 1,
                     ack == 0 ? " [line stuck low]" :
                     ack == 7 ? " [line floating high - no target?]" : "");
        }
    }
    ESP_LOGE(TAG, "transfer failed (ap=%d %s reg=0x%X)", ap, read ? "RD" : "WR", reg);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t swd_dp_write_raw(uint8_t reg, uint32_t val)
{
    return swd_transfer(0, 0, reg, &val);
}

static esp_err_t dp_read(uint8_t reg, uint32_t *val)
{
    return swd_transfer(0, 1, reg, val);
}

static esp_err_t dp_write(uint8_t reg, uint32_t val)
{
    return swd_transfer(0, 0, reg, &val);
}

static esp_err_t ap_write(uint8_t reg, uint32_t val)
{
    return swd_transfer(1, 0, reg, &val);
}

/* AP reads are pipelined: the data of an AP read request arrives on the NEXT
 * transaction. Issue the read, then fetch the value via DP RDBUFF. */
static esp_err_t ap_read(uint8_t reg, uint32_t *val)
{
    uint32_t dummy;
    esp_err_t err = swd_transfer(1, 1, reg, &dummy);
    if (err != ESP_OK) {
        return err;
    }
    return dp_read(DP_RDBUFF, val);
}

/* ---------------- public API ---------------- */

void swd_host_init(void)
{
    s_clk = CONFIG_PROG_SWD_CLK_GPIO;
    s_dio = CONFIG_PROG_SWD_DIO_GPIO;
    s_rst = CONFIG_PROG_SWD_RST_GPIO;

    gpio_reset_pin(s_clk);
    gpio_set_direction(s_clk, GPIO_MODE_OUTPUT);
    gpio_set_level(s_clk, 0);

    gpio_reset_pin(s_dio);
    dio_output();
    gpio_set_level(s_dio, 0);

    if (s_rst >= 0) {
        gpio_reset_pin(s_rst);
        gpio_set_direction(s_rst, GPIO_MODE_OUTPUT);
        gpio_set_level(s_rst, 1);   /* idle: target running */
    }
}

void swd_target_reset(bool assert)
{
    if (s_rst < 0) {
        return;   /* reset line disabled: software reset-halt is used instead */
    }
    gpio_set_level(s_rst, assert ? 0 : 1);
}

uint32_t swd_idcode(void)
{
    return s_idcode;
}

static esp_err_t swd_connect_once(void)
{
    s_idcode = 0;

    /* The RL78 TX pin (TOOL0) may share the SWDIO harness through a series
     * resistor. Its UART driver idles push-pull high and would fight the
     * line, so release it to a hi-Z input while SWD is active. It is
     * re-claimed automatically the next time RL78 mode is entered. */
    gpio_reset_pin(CONFIG_PROG_TOOL0_TX_GPIO);

    /* diagnose the undriven line level: with the internal pull-up SWDIO should
     * read high; a low level here means the line is shorted or held down */
    dio_input();
    int idle_lvl = gpio_get_level(s_dio);
    ESP_LOGI(TAG, "link idle: SWDIO=%d%s (expect 1)", idle_lvl,
             idle_lvl ? "" : " <-- 线路被拉低，检查接线/短路");

    /* connect-under-reset: hold the target in reset while attaching
     * (no-op when the reset line is disabled) */
    swd_target_reset(true);
    vTaskDelay(pdMS_TO_TICKS(2));

    swd_line_reset();
    swd_jtag_to_swd();
    swd_line_reset();
    swd_idle_clocks(2);

    /* read DP IDCODE */
    esp_err_t err = dp_read(DP_DPIDR, &s_idcode);
    if (err != ESP_OK || s_idcode == 0 || s_idcode == 0xFFFFFFFF) {
        ESP_LOGE(TAG, "no SWD target (idcode=0x%08" PRIX32 ")", s_idcode);
        swd_target_reset(false);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "DP IDCODE = 0x%08" PRIX32, s_idcode);

    /* clear sticky errors, select AP 0 bank 0, power-up system + debug */
    dp_write(DP_ABORT, 0x1E);
    dp_write(DP_SELECT, 0x00000000);
    dp_write(DP_CTRL, 0x50000000);

    uint32_t stat = 0;
    int timeout = 100;
    do {
        err = dp_read(DP_CTRL, &stat);
        if (err != ESP_OK) {
            swd_target_reset(false);
            return err;
        }
        if ((stat & 0xF0000000) == 0xF0000000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (--timeout);
    if ((stat & 0xF0000000) != 0xF0000000) {
        ESP_LOGE(TAG, "debug power-up failed (STAT=0x%08" PRIX32 ")", stat);
        swd_target_reset(false);
        return ESP_ERR_TIMEOUT;
    }

    /* AHB-AP: 32-bit, auto-increment, privileged */
    err = ap_write(AP_CSW, 0x23000012);
    if (err != ESP_OK) {
        swd_target_reset(false);
        return err;
    }
    return ESP_OK;
}

esp_err_t swd_connect(void)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = swd_connect_once();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "swd connect attempt %d failed", attempt);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err;
}

void swd_disconnect(void)
{
    swd_idle_clocks(8);
    dio_input();
    swd_target_reset(false);
}

esp_err_t swd_csw_set_size(uint8_t size)
{
    uint32_t csw = 0x23000010 | (size & 0x3);   /* keep auto-increment */
    return ap_write(AP_CSW, csw);
}

esp_err_t swd_mem_read32(uint32_t addr, uint32_t *val)
{
    esp_err_t err = ap_write(AP_TAR, addr);
    if (err != ESP_OK) {
        return err;
    }
    return ap_read(AP_DRW, val);
}

esp_err_t swd_mem_write32(uint32_t addr, uint32_t val)
{
    esp_err_t err = ap_write(AP_TAR, addr);
    if (err != ESP_OK) {
        return err;
    }
    return ap_write(AP_DRW, val);
}
