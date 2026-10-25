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
#define OLED_CHAR_WIDTH       8
#define OLED_GLYPH_WIDTH      7
#define OLED_GLYPH_HEIGHT     11
#define BUTTON_LONG_MS        1000
#define OLED_REFRESH_MS       100
#define OLED_SCROLL_PAUSE_MS  1000
#define OLED_SCROLL_STEP      2
#define OLED_SCROLL_GAP_CHARS 3
#define OLED_RETRY_MS         1000

#define PANEL_PIN(pin) ((pin) == LED_GPIO || (pin) == BUTTON_GPIO || \
                        (pin) == OLED_SDA_GPIO || (pin) == OLED_SCL_GPIO)
_Static_assert(!PANEL_PIN(CONFIG_PROG_TOOL0_TX_GPIO) &&
               !PANEL_PIN(CONFIG_PROG_TOOL0_RX_GPIO) &&
               !PANEL_PIN(CONFIG_PROG_RESET_GPIO), "Target GPIO conflicts with front panel");

static const char *TAG = "panel";
static i2c_master_bus_handle_t s_oled_bus;
static i2c_master_dev_handle_t s_oled;
static bool s_oled_ready;
static TickType_t s_oled_refreshed;
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

static void draw_text(unsigned y, const char *text, TickType_t elapsed)
{
    size_t len = strlen(text);
    size_t cycle_chars = len + OLED_SCROLL_GAP_CHARS;
    size_t offset = 0;
    bool scrolling = len * OLED_CHAR_WIDTH > OLED_WIDTH;
    if (scrolling) {
        TickType_t pause = pdMS_TO_TICKS(OLED_SCROLL_PAUSE_MS);
        TickType_t step_ticks = pdMS_TO_TICKS(OLED_REFRESH_MS);
        TickType_t cycle_ticks = pause +
            cycle_chars * OLED_CHAR_WIDTH / OLED_SCROLL_STEP * step_ticks;
        TickType_t phase = elapsed % cycle_ticks;
        if (phase > pause) offset = (phase - pause) / step_ticks * OLED_SCROLL_STEP;
    }
    int x = -(int)(offset % OLED_CHAR_WIDTH);
    for (size_t i = offset / OLED_CHAR_WIDTH; x < OLED_WIDTH; i++, x += OLED_CHAR_WIDTH) {
        size_t index = scrolling ? i % cycle_chars : i;
        if (index >= len) continue;
        unsigned char c = text[index];
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
        /* Scale 5x7 glyphs to 7x11, including pixels spanning page boundaries. */
        for (unsigned col = 0; col < OLED_GLYPH_WIDTH; col++) {
            int pixel = x + col;
            if (pixel < 0 || pixel >= OLED_WIDTH) continue;
            for (unsigned row = 0; row < OLED_GLYPH_HEIGHT; row++) {
                unsigned py = y + row;
                if (py < OLED_PAGES * 8 && (bits[col * 5 / OLED_GLYPH_WIDTH] &
                    (1U << (row * 7 / OLED_GLYPH_HEIGHT)))) {
                    s_frame[1 + py / 8 * OLED_WIDTH + pixel] |= 1U << (py % 8);
                }
            }
        }
    }
}

static void render_status(const prog_status_t *status, TickType_t elapsed)
{
    memset(s_frame, 0, sizeof(s_frame));
    s_frame[0] = 0x40;
    size_t len = strlen(status->file);
    if (len) {
        draw_text(0, status->file, elapsed);
    } else {
        draw_text(0, "NO FILE", elapsed);
    }
    const char *label = "READY";
    char line[32];
    unsigned pct = status->percent > 100 ? 100 : status->percent;
    if (status->state == PROG_RUNNING) {
        label = "RUNNING";
        if (strcmp(status->stage, "解析文件") == 0) label = "LOADING";
        else if (strcmp(status->stage, "连接目标") == 0) label = "CONNECT";
        else if (strcmp(status->stage, "编程") == 0) label = "PROGRAM";
        else if (strcmp(status->stage, "校验") == 0) label = "VERIFY";
        else if (strstr(status->stage, "擦除")) label = "ERASE";
    } else if (status->state == PROG_DONE) {
        label = "SUCCESS";
    } else if (status->state == PROG_FAILED) {
        label = "FAILED";
    } else if (status->cancelled) {
        label = "CANCELLED";
    }
    draw_text(14, status->chip[0] ? status->chip : "NO CHIP", elapsed);
    if (status->state == PROG_RUNNING || status->state == PROG_DONE) {
        snprintf(line, sizeof(line), "%s %u%%", label, pct);
        label = line;
    }
    draw_text(28, label, elapsed);
}

static esp_err_t oled_flush(void)
{
    const uint8_t window[] = {0x00, 0x21, OLED_COLUMN, OLED_COLUMN + OLED_WIDTH - 1,
                             0x22, 0, OLED_PAGES - 1};
    esp_err_t err = i2c_master_transmit(s_oled, window, sizeof(window), 50);
    if (err == ESP_OK) err = i2c_master_transmit(s_oled, s_frame, sizeof(s_frame), 50);
    return err;
}

static esp_err_t oled_init(const prog_status_t *status, TickType_t elapsed)
{
    if (!s_oled_bus) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = OLED_SDA_GPIO,
            .scl_io_num = OLED_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_oled_bus), TAG, "I2C init");
    }
    /* The OLED stays powered when the ESP32 resets, possibly mid-transfer. */
    ESP_RETURN_ON_ERROR(i2c_master_bus_reset(s_oled_bus), TAG, "I2C recovery");
    if (!s_oled) {
        unsigned addr;
        for (addr = 0x3c; addr <= 0x3d; addr++) {
            esp_err_t err = i2c_master_probe(s_oled_bus, addr, 50);
            if (err == ESP_OK) break;
            if (err != ESP_ERR_NOT_FOUND) return err;
        }
        if (addr > 0x3d) return ESP_ERR_NOT_FOUND;
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_oled_bus, &config, &s_oled),
                            TAG, "OLED device");
        ESP_LOGI(TAG, "SSD1315 72x40 at 0x%02x", addr);
    }
    /* Drain parameters left pending when the MCU reset mid-command. */
    const uint8_t sync[] = {0x00, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_oled, sync, sizeof(sync), 50), TAG, "OLED sync");
    /* SSD1315: 40 COM lines, internal IREF/charge pump, columns 28..99. */
    const uint8_t init[] = {
        0x00, 0xae, 0x2e, 0xd5, 0x80, 0xa8, 0x27, 0xd3, 0x00,
        0x40, 0xad, 0x30, 0x8d, 0x14, 0x20, 0x00, 0xa1,
        0xc8, 0xda, 0x12, 0x81, 0x7f, 0xd9, 0x22, 0xdb, 0x20,
        0xa4, 0xa6, 0x23, 0x00,
    };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_oled, init, sizeof(init), 50), TAG, "OLED setup");
    render_status(status, elapsed);
    ESP_RETURN_ON_ERROR(oled_flush(), TAG, "OLED redraw");
    const uint8_t on[] = {0x00, 0xaf};
    return i2c_master_transmit(s_oled, on, sizeof(on), 50);
}

static void oled_update(const prog_status_t *status, TickType_t elapsed, TickType_t now)
{
    unsigned interval_ms = s_oled_ready ? OLED_REFRESH_MS : OLED_RETRY_MS;
    if (now - s_oled_refreshed < pdMS_TO_TICKS(interval_ms)) return;
    s_oled_refreshed = now;
    if (!s_oled_ready) {
        s_oled_ready = oled_init(status, elapsed) == ESP_OK;
        if (s_oled_ready) ESP_LOGI(TAG, "OLED recovered");
        return;
    }
    render_status(status, elapsed);
    esp_err_t err = oled_flush();
    if (err != ESP_OK) {
        s_oled_ready = false;
        ESP_LOGW(TAG, "OLED refresh failed: %s; retrying in %dms", esp_err_to_name(err), OLED_RETRY_MS);
    }
}

typedef enum { BUTTON_NONE, BUTTON_START, BUTTON_NEXT } button_action_t;

typedef struct {
    bool raw_pressed;
    bool pressed;
    bool armed;
    bool blocked;
    TickType_t changed;
    TickType_t down_since;
} button_t;

static button_action_t button_update(button_t *button, bool pressed, bool busy, TickType_t now)
{
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->changed = now;
    }
    if (busy && (pressed || button->pressed)) button->blocked = true;
    if (pressed != button->pressed && now - button->changed >= pdMS_TO_TICKS(40)) {
        button->pressed = pressed;
        if (pressed) {
            button->down_since = button->changed;
        } else {
            bool start = button->armed && !button->blocked && !busy;
            button->armed = true;
            button->blocked = false;
            if (start) return button->changed - button->down_since >= pdMS_TO_TICKS(BUTTON_LONG_MS)
                              ? BUTTON_NEXT : BUTTON_START;
        }
    }
    if (pressed && button->pressed && button->armed && !button->blocked && !busy &&
        now - button->down_since >= pdMS_TO_TICKS(BUTTON_LONG_MS)) {
        button->blocked = true;
        return BUTTON_NEXT;
    }
    return BUTTON_NONE;
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
    TickType_t state_since = xTaskGetTickCount();
    TickType_t name_since = state_since;
    prog_state_t previous = PROG_IDLE;
    char previous_file[STORAGE_MAX_NAME] = "";
    while (true) {
        TickType_t now = xTaskGetTickCount();
        prog_status_t status;
        programmer_get_status(&status);
        button_action_t action = button_update(&button, gpio_get_level(BUTTON_GPIO) == 0,
                                               status.state == PROG_RUNNING, now);
        if (action != BUTTON_NONE) {
            esp_err_t err;
            if (action == BUTTON_NEXT) err = programmer_select_next();
            else if (status.state == PROG_FAILED) err = programmer_clear_failure();
            else err = programmer_start_selected();
            if (err != ESP_OK) ESP_LOGW(TAG, "button action: %s", esp_err_to_name(err));
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
        oled_update(&status, now - name_since, now);
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
    vTaskDelay(pdMS_TO_TICKS(100));
    prog_status_t status;
    programmer_get_status(&status);
    esp_err_t err = oled_init(&status, 0);
    s_oled_ready = err == ESP_OK;
    s_oled_refreshed = xTaskGetTickCount();
    if (err != ESP_OK) ESP_LOGW(TAG, "OLED unavailable: %s; button/LED remain active", esp_err_to_name(err));
    return xTaskCreate(panel_task, "front_panel", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
