#include "front_panel.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "http_server.h"

#define LED_GPIO       8
#define BUTTON_GPIO    9
#define OLED_SDA_GPIO  5
#define OLED_SCL_GPIO  6
#define OLED_WIDTH     72
#define OLED_PAGES     5
#define OLED_COLUMN    28

#define PANEL_PIN(pin) ((pin) == LED_GPIO || (pin) == BUTTON_GPIO || \
                        (pin) == OLED_SDA_GPIO || (pin) == OLED_SCL_GPIO)
_Static_assert(!PANEL_PIN(CONFIG_PROG_TOOL0_TX_GPIO) &&
               !PANEL_PIN(CONFIG_PROG_TOOL0_RX_GPIO) &&
               !PANEL_PIN(CONFIG_PROG_RESET_GPIO), "Target GPIO conflicts with front panel");

static const char *TAG = "panel";
static i2c_master_dev_handle_t s_oled;
static uint8_t s_frame[1 + OLED_WIDTH * OLED_PAGES];

/* Five columns, bit 0 at the top. Only filename characters and UI glyphs. */
static const uint8_t font_digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
};
static const uint8_t font_upper[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t font_lower[26][5] = {
    {0x20,0x54,0x54,0x54,0x78}, {0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7e,0x09,0x01,0x02},
    {0x0c,0x52,0x52,0x52,0x3e}, {0x7f,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00},
    {0x7c,0x04,0x18,0x04,0x78}, {0x7c,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38}, {0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c}, {0x7c,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20}, {0x04,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c}, {0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44},
    {0x0c,0x50,0x50,0x50,0x3c}, {0x44,0x64,0x54,0x4c,0x44},
};

static void draw_text(unsigned page, const char *text, size_t len)
{
    for (size_t i = 0; i < len && i < 12 && text[i]; i++) {
        unsigned char c = text[i];
        uint8_t glyph[5] = {0};
        const uint8_t *bits = glyph;
        if (c >= '0' && c <= '9') bits = font_digits[c - '0'];
        else if (c >= 'A' && c <= 'Z') bits = font_upper[c - 'A'];
        else if (c >= 'a' && c <= 'z') bits = font_lower[c - 'a'];
        else if (c == '.') glyph[2] = 0x60;
        else if (c == '_') memset(glyph, 0x40, sizeof(glyph));
        else if (c == '-') memset(glyph, 0x08, sizeof(glyph));
        else if (c == '%') {
            glyph[0] = 0x63; glyph[1] = 0x13; glyph[2] = 0x08;
            glyph[3] = 0x64; glyph[4] = 0x63;
        }
        memcpy(&s_frame[1 + page * OLED_WIDTH + i * 6], bits, 5);
    }
}

static void render_status(const prog_status_t *status, unsigned name_page)
{
    memset(s_frame, 0, sizeof(s_frame));
    s_frame[0] = 0x40;
    size_t len = strlen(status->file);
    if (len) {
        size_t offset = (name_page % ((len + 23) / 24)) * 24;
        draw_text(0, status->file + offset, 12);
        if (len - offset > 12) draw_text(1, status->file + offset + 12, 12);
    } else {
        draw_text(0, "NO FILE", 12);
    }
    const char *label = "READY";
    const char *hint = len ? "PRESS KEY" : "SELECT WEB";
    char percent[12];
    unsigned pct = status->percent > 100 ? 100 : status->percent;
    snprintf(percent, sizeof(percent), "%u%%", pct);
    if (status->state == PROG_RUNNING) {
        label = "RUNNING";
        if (strcmp(status->stage, "解析文件") == 0) label = "LOADING";
        else if (strcmp(status->stage, "连接目标") == 0) label = "CONNECT";
        else if (strcmp(status->stage, "编程") == 0) label = "PROGRAM";
        else if (strcmp(status->stage, "校验") == 0) label = "VERIFY";
        else if (strstr(status->stage, "擦除")) label = "ERASE";
        hint = percent;
    } else if (status->state == PROG_DONE) {
        label = "SUCCESS";
        hint = percent;
    } else if (status->state == PROG_FAILED) {
        label = "FAILED";
        hint = strcmp(status->stage, "无固件") == 0 ? "SELECT WEB" : "CHECK LOG";
    } else if (status->cancelled) {
        label = "CANCELLED";
    }
    draw_text(2, label, 12);
    draw_text(3, hint, 12);
    if (status->state == PROG_RUNNING || status->state == PROG_DONE) {
        unsigned fill = pct * (OLED_WIDTH - 2) / 100;
        for (unsigned x = 0; x < OLED_WIDTH; x++) {
            s_frame[1 + 4 * OLED_WIDTH + x] =
                x == 0 || x == OLED_WIDTH - 1 || x <= fill ? 0x7e : 0x42;
        }
    }
}

static esp_err_t oled_flush(void)
{
    const uint8_t window[] = {0x00, 0x21, OLED_COLUMN, OLED_COLUMN + OLED_WIDTH - 1,
                             0x22, 0, OLED_PAGES - 1};
    esp_err_t err = i2c_master_transmit(s_oled, window, sizeof(window), 50);
    if (err == ESP_OK) err = i2c_master_transmit(s_oled, s_frame, sizeof(s_frame), 50);
    return err;
}

static esp_err_t oled_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "I2C init");
    unsigned addr;
    for (addr = 0x3c; addr <= 0x3d; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) break;
    }
    if (addr > 0x3d) {
        i2c_del_master_bus(bus);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &s_oled);
    if (err == ESP_OK) {
        /* SSD1315: 40 COM lines, internal IREF/charge pump, columns 28..99. */
        const uint8_t init[] = {
            0x00, 0xae, 0xd5, 0x80, 0xa8, 0x27, 0xd3, 0x00,
            0x40, 0xad, 0x30, 0x8d, 0x14, 0x20, 0x00, 0xa1,
            0xc8, 0xda, 0x12, 0x81, 0x7f, 0xd9, 0x22, 0xdb, 0x20,
            0xa4, 0xa6, 0x2e,
        };
        err = i2c_master_transmit(s_oled, init, sizeof(init), 50);
        if (err == ESP_OK) {
            prog_status_t status;
            programmer_get_status(&status);
            render_status(&status, 0);
            err = oled_flush();
        }
        if (err == ESP_OK) {
            const uint8_t on[] = {0x00, 0xaf};
            err = i2c_master_transmit(s_oled, on, sizeof(on), 50);
        }
    }
    if (err != ESP_OK) {
        if (s_oled) i2c_master_bus_rm_device(s_oled);
        s_oled = NULL;
        i2c_del_master_bus(bus);
    } else {
        ESP_LOGI(TAG, "SSD1315 72x40 at 0x%02x", addr);
    }
    return err;
}

typedef struct {
    bool raw_pressed;
    bool pressed;
    bool armed;
    bool blocked;
    TickType_t changed;
} button_t;

static bool button_update(button_t *button, bool pressed, bool busy, TickType_t now)
{
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->changed = now;
        button->blocked = busy;
    }
    if (pressed && busy) button->blocked = true;
    if (pressed == button->pressed || now - button->changed < pdMS_TO_TICKS(40)) return false;
    button->pressed = pressed;
    if (!pressed) {
        button->armed = true;
        return false;
    }
    bool start = button->armed && !busy && !button->blocked;
    button->armed = false;
    return start;
}

static int led_level(prog_state_t state, TickType_t elapsed)
{
    bool on = state == PROG_DONE;
    if (state == PROG_RUNNING) on = (elapsed / pdMS_TO_TICKS(500)) % 2 == 0;
    if (state == PROG_FAILED) on = (elapsed / pdMS_TO_TICKS(125)) % 2 == 0;
    return on ? 0 : 1;
}

static void panel_task(void *arg)
{
    /* Require a debounced release after boot, including when held at power-on. */
    button_t button = {.raw_pressed = true, .pressed = true};
    TickType_t refreshed = xTaskGetTickCount();
    TickType_t state_since = refreshed;
    TickType_t name_since = refreshed;
    prog_state_t previous = PROG_IDLE;
    char previous_file[STORAGE_MAX_NAME] = "";
    bool display_ok = s_oled != NULL;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        prog_status_t status;
        programmer_get_status(&status);
        if (button_update(&button, gpio_get_level(BUTTON_GPIO) == 0,
                          status.state == PROG_RUNNING, now)) {
            esp_err_t err = programmer_start_selected();
            if (err != ESP_OK) ESP_LOGW(TAG, "button start: %s", esp_err_to_name(err));
            programmer_get_status(&status);
        }
        if (status.state != previous) {
            previous = status.state;
            state_since = now;
        }
        gpio_set_level(LED_GPIO, led_level(status.state, now - state_since));
        if (strcmp(previous_file, status.file) != 0) {
            strlcpy(previous_file, status.file, sizeof(previous_file));
            name_since = now;
        }
        if (display_ok && now - refreshed >= pdMS_TO_TICKS(200)) {
            render_status(&status, (now - name_since) / pdMS_TO_TICKS(2000));
            esp_err_t err = oled_flush();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "OLED update stopped: %s", esp_err_to_name(err));
                display_ok = false;
            }
            refreshed = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t front_panel_init(void)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(LED_GPIO, 1), TAG, "LED off");
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "LED init");
    config.pin_bit_mask = 1ULL << BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "button init");
    esp_err_t err = oled_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "OLED unavailable: %s; button/LED remain active", esp_err_to_name(err));
    return xTaskCreate(panel_task, "front_panel", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
