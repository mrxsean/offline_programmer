#include "drv_touch_key.h"

#include "sdkconfig.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "touch_key";

#define TOUCH_ADDR            0x28
#define TOUCH_POLL_MS         30
#define TOUCH_IO_TIMEOUT_MS   100
#define TOUCH_RETRY_MS        1000
#define TOUCH_ERROR_LIMIT     3

#define KEY_LONG_MS           1000    /* long-press threshold */
#define KEY_REPEAT_MS         100     /* auto-repeat period for UP/DOWN while held */

/* touch chip variants */
#define CHIP_UNKNOWN   0
#define CHIP_CAP1298   1     /* Microchip CAP1298-compatible: ID 0x71/0x5D @0xFD/0xFE */
#define CHIP_CYPRESS   2     /* Cypress: no config, status register at 0x14 */

static uint8_t s_chip = CHIP_UNKNOWN;

static QueueHandle_t que_touch = NULL;       /* ISR -> read task (edge wake, optional) */
QueueHandle_t que_key = NULL;                /* read task -> UI (key_msg_t) */

static const i2c_port_t s_i2c_port = I2C_NUM_1;
static bool s_i2c_installed;

/* CAP1298 register configuration table (address, value).
 * NOTE: tuned on the real hardware, contains non-standard registers (0x80/0x81).
 * Do NOT replace with values from the standard datasheet. */
static const uint8_t CAP_defaultConfiguration[][2] = {
    { 0x00, 0x4C },
    { 0x1F, 0x3F },
    { 0x20, 0x90 },
    { 0x21, 0x3F },
    { 0x22, 0xA4 },
    { 0x23, 0x00 },
    { 0x24, 0x38 },
    { 0x26, 0xC0 },
    { 0x27, 0x3F },
    { 0x28, 0x00 },
    { 0x29, 0x00 },
    { 0x2A, 0x00 },
    { 0x2B, 0x84 },
    { 0x2D, 0x3F },
    { 0x2F, 0x00 },
    { 0x30, 0x3E },
    { 0x31, 0x50 },
    { 0x32, 0x46 },
    { 0x33, 0x6E },
    { 0x34, 0x3F },
    { 0x35, 0x41 },
    { 0x36, 0x49 },
    { 0x37, 0x49 },
    { 0x38, 0x03 },
    { 0x40, 0x00 },
    { 0x41, 0x30 },
    { 0x42, 0x02 },
    { 0x43, 0x40 },
    { 0x44, 0x44 },
    { 0x60, 0x00 },
    { 0x61, 0x22 },
    { 0x80, 0x55 },
    { 0x81, 0x55 },
};

/* ---------------- I2C ---------------- */

static esp_err_t touch_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_PROG_TOUCH_SDA_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = CONFIG_PROG_TOUCH_SCL_GPIO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err = i2c_param_config(s_i2c_port, &conf);
    if (err == ESP_OK) {
        err = i2c_driver_install(s_i2c_port, I2C_MODE_MASTER, 0, 0, 0);
        s_i2c_installed = (err == ESP_OK);
    }
    return err;
}

static esp_err_t touch_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(s_i2c_port, TOUCH_ADDR, &reg, 1,
                                        data, len, pdMS_TO_TICKS(TOUCH_IO_TIMEOUT_MS));
}

static esp_err_t touch_write(uint8_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg, value};
    return i2c_master_write_to_device(s_i2c_port, TOUCH_ADDR, bytes, sizeof(bytes),
                                      pdMS_TO_TICKS(TOUCH_IO_TIMEOUT_MS));
}

static esp_err_t touch_wait_scl_high(void)
{
    for (int i = 0; i < 100; i++) {
        if (gpio_get_level(CONFIG_PROG_TOUCH_SCL_GPIO)) {
            return ESP_OK;
        }
        esp_rom_delay_us(10);
    }
    return ESP_ERR_TIMEOUT;
}

/* Release a slave stuck partway through a byte: 9 clocks, then STOP,
 * then reinstall the controller. Only the read task owns I2C1. */
static esp_err_t touch_bus_recover(void)
{
    if (s_i2c_installed) {
        esp_err_t err = i2c_driver_delete(s_i2c_port);
        if (err != ESP_OK) {
            return err;
        }
        s_i2c_installed = false;
    }
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << CONFIG_PROG_TOUCH_SDA_GPIO) | (1ULL << CONFIG_PROG_TOUCH_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(CONFIG_PROG_TOUCH_SDA_GPIO, 1);
    gpio_set_level(CONFIG_PROG_TOUCH_SCL_GPIO, 1);
    esp_err_t err = gpio_config(&conf);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 9; i++) {
        err = touch_wait_scl_high();
        if (err != ESP_OK) {
            return err;
        }
        if (gpio_get_level(CONFIG_PROG_TOUCH_SDA_GPIO)) {
            break;
        }
        gpio_set_level(CONFIG_PROG_TOUCH_SCL_GPIO, 0);
        esp_rom_delay_us(5);
        gpio_set_level(CONFIG_PROG_TOUCH_SCL_GPIO, 1);
        esp_rom_delay_us(5);
    }
    /* STOP condition */
    gpio_set_level(CONFIG_PROG_TOUCH_SCL_GPIO, 0);
    gpio_set_level(CONFIG_PROG_TOUCH_SDA_GPIO, 0);
    esp_rom_delay_us(5);
    gpio_set_level(CONFIG_PROG_TOUCH_SCL_GPIO, 1);
    err = touch_wait_scl_high();
    esp_rom_delay_us(5);
    gpio_set_level(CONFIG_PROG_TOUCH_SDA_GPIO, 1);
    esp_rom_delay_us(5);
    if (err != ESP_OK || !gpio_get_level(CONFIG_PROG_TOUCH_SDA_GPIO)) {
        return ESP_ERR_TIMEOUT;
    }
    return touch_i2c_init();
}

/* ---------------- chip detection & init ---------------- */

static esp_err_t touch_chip_initialize(void)
{
    uint8_t id1 = 0, id2 = 0, status[6];
    esp_err_t err;

    if (s_chip != CHIP_CYPRESS) {
        err = touch_read(0xFD, &id1, 1);
        if (err == ESP_OK) {
            err = touch_read(0xFE, &id2, 1);
        }
        if (err == ESP_OK && id1 == 0x71 && id2 == 0x5D) {
            s_chip = CHIP_CAP1298;
            for (size_t i = 0; i < sizeof(CAP_defaultConfiguration) / sizeof(CAP_defaultConfiguration[0]); i++) {
                err = touch_write(CAP_defaultConfiguration[i][0], CAP_defaultConfiguration[i][1]);
                if (err != ESP_OK) {
                    return err;
                }
            }
            return ESP_OK;
        }
        /* Once identified as CAP1298, a communication failure must not
         * silently downgrade the chip type. */
        if (s_chip == CHIP_CAP1298) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
    }
    /* Cypress variant: no config table, just require a successful status read */
    err = touch_read(0x14, status, sizeof(status));
    if (err == ESP_OK) {
        s_chip = CHIP_CYPRESS;
    }
    return err;
}

/* ---------------- key mapping & events ---------------- */

static uint16_t raw_to_key(uint8_t raw)
{
    switch (raw) {
    case 0x20: return KEY_UP;
    case 0x10: return KEY_MODE;
    case 0x08: return KEY_ONOFF;
    case 0x04: return KEY_DOWN;
    case 0x02: return KEY_MENU;
    case 0x01: return KEY_OK;
    case 0x22: return KEY_UP | KEY_MENU;
    case 0x06: return KEY_DOWN | KEY_MENU;
    case 0x24: return KEY_UP | KEY_DOWN;
    case 0x05: return KEY_MODE | KEY_DOWN;
    case 0x21: return KEY_MODE | KEY_UP;
    default:   return 0;
    }
}

static void key_send(uint16_t code, key_evt_t evt)
{
    if (que_key == NULL || code == 0) {
        return;
    }
    key_msg_t m = { .code = code, .evt = evt };
    if (xQueueSend(que_key, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "key queue full, drop 0x%04x/%d", code, evt);
    }
}

/* Key state machine: PRESS on change, LONG after KEY_LONG_MS,
 * REPEAT every KEY_REPEAT_MS for held UP/DOWN, RELEASE on all-up. */
typedef struct {
    uint16_t key;
    bool longpress;
    bool wait_release;
    int64_t deadline_us;
} key_state_t;

static void cancel_key(key_state_t *st)
{
    *st = (key_state_t){ .wait_release = true };
}

static void process_key(key_state_t *st, uint8_t value, int64_t now)
{
    uint16_t key = raw_to_key(value);
    if (st->wait_release) {
        if (value == 0) {
            st->wait_release = false;
        }
        return;
    }
    if (value && key == 0) {        /* unknown combo: ignore until all released */
        cancel_key(st);
        return;
    }
    if (key) {
        if (key != st->key) {
            st->key = key;
            st->longpress = false;
            st->deadline_us = now + KEY_LONG_MS * 1000;
            key_send(key, KEY_EVT_PRESS);
        } else if (!st->longpress && now >= st->deadline_us) {
            st->longpress = true;
            key_send(key, KEY_EVT_LONG);
            st->deadline_us = now + KEY_REPEAT_MS * 1000;
        } else if (st->longpress && now >= st->deadline_us &&
                   (key == KEY_UP || key == KEY_DOWN)) {
            key_send(key, KEY_EVT_REPEAT);
            st->deadline_us = now + KEY_REPEAT_MS * 1000;
        }
    } else {
        if (st->key && !st->longpress) {
            key_send(st->key, KEY_EVT_RELEASE);
        }
        st->key = 0;
        st->longpress = false;
    }
}

/* ---------------- read task ---------------- */

static void touch_key_read_task(void *pvParameter)
{
    (void)pvParameter;
    key_state_t st = { .wait_release = true };
    unsigned int errors = 0;
    bool ready = false;

    esp_err_t err = touch_i2c_init();
    if (err == ESP_OK) {
        err = touch_chip_initialize();
    }
    ready = (err == ESP_OK);
    if (ready) {
        ESP_LOGI(TAG, "touch IC type: %d (%s)", s_chip,
                 s_chip == CHIP_CAP1298 ? "CAP1298" : "Cypress");
    }

    while (1) {
        if (!ready) {
            cancel_key(&st);
            ESP_LOGW(TAG, "touch unavailable: %s; SDA=%d SCL=%d; retry in %dms",
                     esp_err_to_name(err),
                     gpio_get_level(CONFIG_PROG_TOUCH_SDA_GPIO),
                     gpio_get_level(CONFIG_PROG_TOUCH_SCL_GPIO), TOUCH_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(TOUCH_RETRY_MS));
            err = touch_bus_recover();
            if (err == ESP_OK) {
                err = touch_chip_initialize();
            }
            if (err != ESP_OK) {
                continue;
            }
            ready = true;
            errors = 0;
            ESP_LOGI(TAG, "touch recovered, IC type: %d", s_chip);
        }

        /* Poll at a fixed rate (also drain the ISR queue), so a missed
         * interrupt edge can never stall key input. */
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        int evt;
        xQueueReceive(que_touch, &evt, 0);

        uint8_t status[6] = {0};
        err = touch_read(s_chip == CHIP_CAP1298 ? 0x00 : 0x14, status, sizeof(status));
        if (err == ESP_OK) {
            /* clear the interrupt flag (bit0 = 0) */
            err = touch_write(0x00, status[0] & 0xFE);
        }
        if (err != ESP_OK) {
            cancel_key(&st);
            if (errors++ == 0) {
                ESP_LOGW(TAG, "touch io failed: %s", esp_err_to_name(err));
            }
            if (errors >= TOUCH_ERROR_LIMIT) {
                ready = false;
            }
            continue;
        }
        errors = 0;

        uint8_t value = status[s_chip == CHIP_CAP1298 ? 3 : 0] & 0x3F;
        process_key(&st, value, esp_timer_get_time());
    }
}

/* ---------------- interrupt & startup ---------------- */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t task_awoken = pdFALSE;
    static int s_isr_evt = 1;
    xQueueSendFromISR(que_touch, &s_isr_evt, &task_awoken);
    if (task_awoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void touch_key_interrupt_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_PROG_TOUCH_INT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CONFIG_PROG_TOUCH_INT_GPIO, gpio_isr_handler, NULL);
}

void touch_key_start(void)
{
    if (que_touch == NULL) {
        que_touch = xQueueCreate(TOUCH_BUTTON_NUM, sizeof(int));
    }
    if (que_key == NULL) {
        que_key = xQueueCreate(8, sizeof(key_msg_t));
    }
    touch_key_interrupt_init();
    xTaskCreate(touch_key_read_task, "touch_key_task", 4096, NULL, 5, NULL);
}
