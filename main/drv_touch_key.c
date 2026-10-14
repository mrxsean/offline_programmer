#include "drv_touch_key.h"

#include "sdkconfig.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "touch_key";

#define CAP1298_ADDR           0x28          /* touch chip I2C 7-bit address */
#define CAP1298_TOUCH_REG      0x00          /* touch status register (6 bytes, byte3 bit0-5 = 6 keys) */

#define KEY_LONG_MS            (1000)        /* long-press threshold */
#define KEY_REPEAT_MS          (100)         /* auto-repeat period while held */
#define KEY_SCAN_TIMEOUT_MS    (5000)        /* no-ISR timeout -> rewrite config (anti drift) */

static QueueHandle_t que_touch = NULL;       /* ISR -> read task */
QueueHandle_t que_key = NULL;                /* read task -> UI (key_msg_t) */

static esp_timer_handle_t s_long_timer;
static esp_timer_handle_t s_repeat_timer;
static uint16_t s_hold_key = 0;
static volatile uint8_t s_has_longpress = 0;

static i2c_port_t s_i2c_port = I2C_NUM_1;

/* Register configuration table (address, value).
 * NOTE: tuned on the real hardware and contains non-standard CAP1298 registers
 * (0x80/0x81 etc). Do NOT replace with values from the standard datasheet. */
static const uint8_t CAP_defaultConfiguration[][2] = {
    { 0x00, 0x4C },
    { 0x1F, 0x4F },
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
    { 0x30, 0x50 },
    { 0x31, 0x50 },
    { 0x32, 0x50 },
    { 0x33, 0x50 },
    { 0x34, 0x50 },
    { 0x35, 0x50 },
    { 0x36, 0x50 },
    { 0x37, 0x50 },
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

static void touch_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_PROG_TOUCH_SDA_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = CONFIG_PROG_TOUCH_SCL_GPIO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(s_i2c_port, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(s_i2c_port, I2C_MODE_MASTER, 0, 0, 0));
}

static esp_err_t touch_i2c_read(uint8_t reg, uint8_t *buf, int len)
{
    esp_err_t ret;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CAP1298_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(cmd, reg, 1);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CAP1298_ADDR << 1) | I2C_MASTER_READ, 1);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C read reg 0x%02x fail: %d", reg, ret);
    }
    return ret;
}

static esp_err_t touch_i2c_write(uint8_t reg, const uint8_t *buf, int len)
{
    esp_err_t ret;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (CAP1298_ADDR << 1) | I2C_MASTER_WRITE, 1);
    i2c_master_write_byte(cmd, reg, 1);
    if (buf && len > 0) {
        i2c_master_write(cmd, buf, len, 1);
    }
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C write reg 0x%02x fail: %d", reg, ret);
    }
    return ret;
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
    case 0x05: return KEY_OK | KEY_DOWN;
    case 0x21: return KEY_OK | KEY_UP;
    case 0x30: return KEY_MODE | KEY_UP;
    case 0x14: return KEY_MODE | KEY_DOWN;
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

static void long_timer_cb(void *arg)
{
    (void)arg;
    s_has_longpress = 1;
    key_send(s_hold_key, KEY_EVT_LONG);
    if (s_hold_key == KEY_UP || s_hold_key == KEY_DOWN) {
        esp_timer_stop(s_repeat_timer);
        esp_timer_start_periodic(s_repeat_timer, KEY_REPEAT_MS * 1000);
    }
}

static void repeat_timer_cb(void *arg)
{
    (void)arg;
    key_send(s_hold_key, KEY_EVT_REPEAT);
}

static void touch_key_timers_init(void)
{
    esp_timer_create_args_t args = {
        .callback = &long_timer_cb,
        .name = "key_long",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_long_timer));

    args.callback = &repeat_timer_cb;
    args.name = "key_repeat";
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_repeat_timer));
}

/* ---------------- read task ---------------- */

static esp_err_t cap1298_write_config(void)
{
    uint8_t v;
    for (int i = 0; i < (int)(sizeof(CAP_defaultConfiguration) / sizeof(CAP_defaultConfiguration[0])); i++) {
        v = CAP_defaultConfiguration[i][1];
        if (touch_i2c_write(CAP_defaultConfiguration[i][0], &v, 1) != ESP_OK) {
            ESP_LOGE(TAG, "write config reg 0x%02X fail", CAP_defaultConfiguration[i][0]);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void touch_key_read_task(void *pvParameter)
{
    (void)pvParameter;
    uint8_t touch_value[6] = { 0 };
    uint8_t raw_old = 0;
    uint32_t evt = 0;

    if (cap1298_write_config() == ESP_OK) {
        ESP_LOGI(TAG, "touch chip configured (addr 0x%02X)", CAP1298_ADDR);
    } else {
        ESP_LOGE(TAG, "touch chip not responding @0x%02X", CAP1298_ADDR);
    }

    while (1) {
        if (xQueueReceive(que_touch, &evt, pdMS_TO_TICKS(KEY_SCAN_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "no ISR for %dms, rewrite config", KEY_SCAN_TIMEOUT_MS);
            cap1298_write_config();
            continue;
        }

        if (touch_i2c_read(CAP1298_TOUCH_REG, touch_value, 6) != ESP_OK) {
            continue;
        }

        uint8_t raw = touch_value[3] & 0x3F;
        if (raw != raw_old) {
            if (raw != 0) {
                uint16_t code = raw_to_key(raw);
                if (code) {
                    s_hold_key = code;
                    s_has_longpress = 0;
                    esp_timer_stop(s_repeat_timer);
                    esp_timer_stop(s_long_timer);
                    esp_timer_start_once(s_long_timer, KEY_LONG_MS * 1000);
                    key_send(code, KEY_EVT_PRESS);
                }
            } else {
                esp_timer_stop(s_repeat_timer);
                esp_timer_stop(s_long_timer);
                if (s_hold_key && !s_has_longpress) {
                    key_send(s_hold_key, KEY_EVT_RELEASE);
                }
                s_hold_key = 0;
                s_has_longpress = 0;
            }
        }
        raw_old = raw;

        /* clear the chip's interrupt flag (bit0 = 0) */
        touch_value[0] &= 0xFE;
        touch_i2c_write(CAP1298_TOUCH_REG, touch_value, 1);
    }
}

/* ---------------- interrupt & startup ---------------- */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t task_awoken = pdFALSE;
    static uint32_t s_isr_evt = 1;
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
        /* INT goes low while a key is active and recovers after flag clear;
         * any-edge so no transition is missed */
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CONFIG_PROG_TOUCH_INT_GPIO, gpio_isr_handler, NULL);
}

void touch_key_start(void)
{
    if (que_touch == NULL) {
        que_touch = xQueueCreate(TOUCH_BUTTON_NUM, sizeof(uint32_t));
    }
    if (que_key == NULL) {
        que_key = xQueueCreate(8, sizeof(key_msg_t));
    }
    touch_i2c_init();
    touch_key_timers_init();
    touch_key_interrupt_init();
    xTaskCreate(touch_key_read_task, "touch_key_task", 4096, NULL, 5, NULL);
}
