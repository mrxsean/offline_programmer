#include "io_debug.h"

#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define IO_COUNT 22
#define IO_MAX_DATA 64
typedef enum { IO_FREE, IO_GPIO, IO_I2C, IO_SPI, IO_ADC } owner_t;
static owner_t s_owner[IO_COUNT];
static bool s_output[IO_COUNT];
static int s_pull[IO_COUNT];
static struct { bool active; int sda, scl, hz; } s_i2c;
static struct { bool bus; spi_device_handle_t device; int pins[4], hz, mode; } s_spi;
static struct { adc_oneshot_unit_handle_t unit; adc_cali_handle_t cal; adc_channel_t channel; int pin, atten; } s_adc;

static const char *reserved_pin(int pin)
{
    if (pin == CONFIG_PROG_TOOL0_TX_GPIO || pin == CONFIG_PROG_TOOL0_RX_GPIO || pin == CONFIG_PROG_RESET_GPIO) return "RL78";
    if (pin == 5 || pin == 6) return "OLED";
    if (pin == 8) return "LED";
    if (pin == 9) return "BUTTON";
    if (pin >= 11 && pin <= 17) return "FLASH";
    if (pin == 18 || pin == 19) return "USB";
    if (pin == 20 || pin == 21) return "UART0";
    return "";
}

static bool pin_available(int pin, owner_t owner)
{
    return pin >= 0 && pin < IO_COUNT && !*reserved_pin(pin) &&
           (s_owner[pin] == IO_FREE || s_owner[pin] == owner);
}

static void release_pins(owner_t owner)
{
    for (int pin = 0; pin < IO_COUNT; pin++) {
        if (s_owner[pin] == owner) {
            gpio_reset_pin(pin);
            s_owner[pin] = IO_FREE;
        }
    }
}

static bool number(const cJSON *req, const char *key, int min, int max, int *value)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != (int)v->valuedouble) return false;
    *value = v->valueint;
    return true;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int read_hex(const cJSON *req, uint8_t *data)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, "hex");
    if (!cJSON_IsString(v)) return -1;
    size_t len = strlen(v->valuestring);
    if (len > IO_MAX_DATA * 2 || len % 2) return -1;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_digit(v->valuestring[i]), lo = hex_digit(v->valuestring[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        data[i / 2] = hi * 16 + lo;
    }
    return len / 2;
}

static void add_hex(cJSON *out, const uint8_t *data, size_t len)
{
    char text[IO_MAX_DATA * 2 + 1];
    const char *digits = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        text[i * 2] = digits[data[i] >> 4];
        text[i * 2 + 1] = digits[data[i] & 15];
    }
    text[len * 2] = 0;
    cJSON_AddStringToObject(out, "hex", text);
}

/* Software I2C keeps the sole hardware I2C controller available to the OLED. */
static void i2c_delay(void) { esp_rom_delay_us(500000 / s_i2c.hz); }
static esp_err_t i2c_high(void)
{
    gpio_set_level(s_i2c.scl, 1);
    int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < 1000) {
        if (gpio_get_level(s_i2c.scl)) return ESP_OK;
        esp_rom_delay_us(1);
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t i2c_start(void)
{
    gpio_set_level(s_i2c.sda, 1);
    esp_err_t err = i2c_high();
    if (err != ESP_OK) return err;
    if (!gpio_get_level(s_i2c.sda)) return ESP_ERR_INVALID_STATE;
    i2c_delay();
    gpio_set_level(s_i2c.sda, 0);
    i2c_delay();
    gpio_set_level(s_i2c.scl, 0);
    return ESP_OK;
}
static esp_err_t i2c_stop(void)
{
    gpio_set_level(s_i2c.scl, 0);
    gpio_set_level(s_i2c.sda, 0);
    i2c_delay();
    esp_err_t err = i2c_high();
    i2c_delay();
    gpio_set_level(s_i2c.sda, 1);
    i2c_delay();
    return err;
}
static esp_err_t i2c_bit(int output, int *input)
{
    gpio_set_level(s_i2c.sda, output);
    i2c_delay();
    esp_err_t err = i2c_high();
    if (err != ESP_OK) return err;
    i2c_delay();
    if (input) *input = gpio_get_level(s_i2c.sda);
    gpio_set_level(s_i2c.scl, 0);
    return ESP_OK;
}
static esp_err_t i2c_write(uint8_t data)
{
    for (int bit = 7; bit >= 0; bit--) {
        esp_err_t err = i2c_bit((data >> bit) & 1, NULL);
        if (err != ESP_OK) return err;
    }
    int nack;
    esp_err_t err = i2c_bit(1, &nack);
    return err != ESP_OK ? err : (nack ? ESP_ERR_NOT_FOUND : ESP_OK);
}
static esp_err_t i2c_transfer(int addr, const uint8_t *tx, int tx_len, uint8_t *rx, int rx_len)
{
    esp_err_t err = i2c_start();
    if (err == ESP_OK) err = i2c_write((addr << 1) | (!tx_len && rx_len ? 1 : 0));
    for (int i = 0; err == ESP_OK && i < tx_len; i++) err = i2c_write(tx[i]);
    if (err == ESP_OK && tx_len && rx_len) {
        err = i2c_start();
        if (err == ESP_OK) err = i2c_write((addr << 1) | 1);
    }
    for (int i = 0; err == ESP_OK && i < rx_len; i++) {
        rx[i] = 0;
        for (int bit = 0; bit < 8 && err == ESP_OK; bit++) {
            int value;
            err = i2c_bit(1, &value);
            if (err == ESP_OK) rx[i] = (rx[i] << 1) | value;
        }
        if (err == ESP_OK) err = i2c_bit(i == rx_len - 1, NULL);
    }
    esp_err_t stop = i2c_stop();
    return err == ESP_OK ? stop : err;
}

static esp_err_t release_spi(void)
{
    esp_err_t err = ESP_OK;
    if (s_spi.device) {
        err = spi_bus_remove_device(s_spi.device);
        if (err != ESP_OK) return err;
        s_spi.device = NULL;
    }
    if (s_spi.bus) {
        err = spi_bus_free(SPI2_HOST);
        if (err != ESP_OK) return err;
        s_spi.bus = false;
    }
    release_pins(IO_SPI);
    return err;
}
static esp_err_t release_adc(void)
{
    if (s_adc.cal) {
        esp_err_t err = adc_cali_delete_scheme_curve_fitting(s_adc.cal);
        if (err != ESP_OK) return err;
        s_adc.cal = NULL;
    }
    if (s_adc.unit) {
        esp_err_t err = adc_oneshot_del_unit(s_adc.unit);
        if (err != ESP_OK) return err;
        s_adc.unit = NULL;
    }
    release_pins(IO_ADC);
    return ESP_OK;
}

static void status(cJSON *out)
{
    static const char *owners[] = {"", "GPIO", "I2C", "SPI", "ADC"};
    cJSON *pins = cJSON_AddArrayToObject(out, "pins");
    for (int pin = 0; pin < IO_COUNT; pin++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToArray(pins, p);
        cJSON_AddNumberToObject(p, "pin", pin);
        cJSON_AddStringToObject(p, "reserved", reserved_pin(pin));
        cJSON_AddStringToObject(p, "owner", owners[s_owner[pin]]);
        cJSON_AddBoolToObject(p, "adc", pin <= 4);
        if (s_owner[pin] == IO_GPIO) {
            cJSON_AddBoolToObject(p, "output", s_output[pin]);
            cJSON_AddNumberToObject(p, "pull", s_pull[pin]);
            cJSON_AddNumberToObject(p, "level", gpio_get_level(pin));
        }
    }
    cJSON *i2c = cJSON_AddObjectToObject(out, "i2c");
    cJSON_AddBoolToObject(i2c, "active", s_i2c.active);
    cJSON_AddNumberToObject(i2c, "sda", s_i2c.sda);
    cJSON_AddNumberToObject(i2c, "scl", s_i2c.scl);
    cJSON_AddNumberToObject(i2c, "hz", s_i2c.hz);
    cJSON *spi = cJSON_AddObjectToObject(out, "spi");
    cJSON_AddBoolToObject(spi, "active", s_spi.device != NULL);
    const char *names[] = {"sclk", "mosi", "miso", "cs"};
    for (int i = 0; i < 4; i++) cJSON_AddNumberToObject(spi, names[i], s_spi.pins[i]);
    cJSON_AddNumberToObject(spi, "hz", s_spi.hz);
    cJSON_AddNumberToObject(spi, "mode", s_spi.mode);
    cJSON *adc = cJSON_AddObjectToObject(out, "adc");
    cJSON_AddBoolToObject(adc, "active", s_adc.unit != NULL);
    cJSON_AddNumberToObject(adc, "pin", s_adc.pin);
    cJSON_AddNumberToObject(adc, "atten", s_adc.atten);
}

static esp_err_t execute(const cJSON *req, cJSON *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, "op");
    if (!cJSON_IsString(v)) return ESP_ERR_INVALID_ARG;
    const char *op = v->valuestring;
    int pin, level;
    uint8_t tx[IO_MAX_DATA + 2], rx[IO_MAX_DATA];
    if (!strcmp(op, "status")) { status(out); return ESP_OK; }
    if (!strcmp(op, "gpio_config")) {
        int direction, pull;
        if (!number(req, "pin", 0, 21, &pin) || !pin_available(pin, IO_GPIO) ||
            !number(req, "output", 0, 1, &direction) || !number(req, "pull", 0, 2, &pull) ||
            !number(req, "level", 0, 1, &level)) return ESP_ERR_INVALID_ARG;
        gpio_config_t config = {.pin_bit_mask = 1ULL << pin,
            .mode = direction ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = pull == 1, .pull_down_en = pull == 2};
        esp_err_t err = direction ? gpio_set_level(pin, level) : ESP_OK;
        if (err == ESP_OK) err = gpio_config(&config);
        if (err != ESP_OK) { gpio_reset_pin(pin); s_owner[pin] = IO_FREE; return err; }
        s_owner[pin] = IO_GPIO; s_output[pin] = direction; s_pull[pin] = pull;
        return ESP_OK;
    }
    if (!strcmp(op, "gpio_read") || !strcmp(op, "gpio_write") || !strcmp(op, "gpio_release")) {
        if (!number(req, "pin", 0, 21, &pin) || s_owner[pin] != IO_GPIO) return ESP_ERR_INVALID_ARG;
        if (!strcmp(op, "gpio_release")) {
            esp_err_t err = gpio_reset_pin(pin);
            if (err == ESP_OK) s_owner[pin] = IO_FREE;
            return err;
        }
        if (!strcmp(op, "gpio_write")) {
            if (!s_output[pin] || !number(req, "level", 0, 1, &level)) return ESP_ERR_INVALID_ARG;
            esp_err_t err = gpio_set_level(pin, level);
            if (err != ESP_OK) return err;
        }
        cJSON_AddNumberToObject(out, "level", gpio_get_level(pin));
        return ESP_OK;
    }
    if (!strcmp(op, "i2c_release")) {
        release_pins(IO_I2C); s_i2c.active = false; return ESP_OK;
    }
    if (!strcmp(op, "i2c_config")) {
        int sda, scl, hz;
        if (!number(req, "sda", 0, 21, &sda) || !number(req, "scl", 0, 21, &scl) || sda == scl ||
            !pin_available(sda, IO_I2C) || !pin_available(scl, IO_I2C) ||
            !number(req, "hz", 10000, 100000, &hz) || (hz != 10000 && hz != 50000 && hz != 100000)) return ESP_ERR_INVALID_ARG;
        release_pins(IO_I2C); s_i2c.active = false;
        gpio_set_level(sda, 1); gpio_set_level(scl, 1);
        gpio_config_t config = {.pin_bit_mask = (1ULL << sda) | (1ULL << scl),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD, .pull_up_en = GPIO_PULLUP_ENABLE};
        esp_err_t err = gpio_config(&config);
        if (err != ESP_OK) { gpio_reset_pin(sda); gpio_reset_pin(scl); return err; }
        s_i2c.sda = sda; s_i2c.scl = scl; s_i2c.hz = hz; s_i2c.active = true;
        s_owner[sda] = s_owner[scl] = IO_I2C;
        return ESP_OK;
    }
    if (!strcmp(op, "i2c_scan")) {
        if (!s_i2c.active) return ESP_ERR_INVALID_STATE;
        cJSON *found = cJSON_AddArrayToObject(out, "addresses");
        for (int addr = 3; addr <= 0x77; addr++) {
            esp_err_t err = i2c_transfer(addr, tx, 0, rx, 0);
            if (err == ESP_OK) cJSON_AddItemToArray(found, cJSON_CreateNumber(addr));
            else if (err != ESP_ERR_NOT_FOUND) return err;
        }
        return ESP_OK;
    }
    if (!strcmp(op, "i2c_transfer")) {
        int addr, count, size = read_hex(req, tx);
        if (!s_i2c.active) return ESP_ERR_INVALID_STATE;
        if (!number(req, "address", 3, 0x77, &addr) || !number(req, "read", 0, IO_MAX_DATA, &count) ||
            size < 0 || (!size && !count)) return ESP_ERR_INVALID_ARG;
        if (cJSON_HasObjectItem(req, "write")) {
            int length;
            if (!number(req, "write", 1, IO_MAX_DATA, &length) || length != size || count)
                return ESP_ERR_INVALID_ARG;
        }
        int reg_bytes = 0, reg = 0;
        if (cJSON_HasObjectItem(req, "register_bytes") &&
            !number(req, "register_bytes", 0, 2, &reg_bytes)) return ESP_ERR_INVALID_ARG;
        if (reg_bytes) {
            if (!number(req, "register", 0, reg_bytes == 1 ? 0xff : 0xffff, &reg))
                return ESP_ERR_INVALID_ARG;
            memmove(tx + reg_bytes, tx, size);
            for (int i = 0; i < reg_bytes; i++) tx[i] = reg >> (8 * (reg_bytes - 1 - i));
            size += reg_bytes;
        } else if (cJSON_HasObjectItem(req, "register")) return ESP_ERR_INVALID_ARG;
        esp_err_t err = i2c_transfer(addr, tx, size, rx, count);
        if (err == ESP_OK) add_hex(out, rx, count);
        return err;
    }
    if (!strcmp(op, "spi_release")) return release_spi();
    if (!strcmp(op, "spi_config")) {
        int pins[4], hz, mode;
        const char *names[] = {"sclk", "mosi", "miso", "cs"};
        for (int i = 0; i < 4; i++) {
            if (!number(req, names[i], 0, 21, &pins[i]) || !pin_available(pins[i], IO_SPI)) return ESP_ERR_INVALID_ARG;
            for (int j = 0; j < i; j++) if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
        }
        if (!number(req, "hz", 10000, 10000000, &hz) || !number(req, "mode", 0, 3, &mode)) return ESP_ERR_INVALID_ARG;
        esp_err_t err = release_spi();
        if (err != ESP_OK) return err;
        spi_bus_config_t bus = {.sclk_io_num = pins[0], .mosi_io_num = pins[1], .miso_io_num = pins[2],
            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = IO_MAX_DATA};
        err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
        if (err != ESP_OK) return err;
        s_spi.bus = true;
        for (int i = 0; i < 4; i++) { s_owner[pins[i]] = IO_SPI; s_spi.pins[i] = pins[i]; }
        spi_device_interface_config_t device = {.clock_speed_hz = hz, .mode = mode,
            .spics_io_num = pins[3], .queue_size = 1};
        err = spi_bus_add_device(SPI2_HOST, &device, &s_spi.device);
        if (err != ESP_OK) { release_spi(); return err; }
        s_spi.hz = hz; s_spi.mode = mode;
        return ESP_OK;
    }
    if (!strcmp(op, "spi_transfer")) {
        int size = read_hex(req, tx);
        if (!s_spi.device) return ESP_ERR_INVALID_STATE;
        if (size <= 0) return ESP_ERR_INVALID_ARG;
        spi_transaction_t transfer = {.length = size * 8, .tx_buffer = tx, .rx_buffer = rx};
        esp_err_t err = spi_device_transmit(s_spi.device, &transfer);
        if (err == ESP_OK) add_hex(out, rx, size);
        return err;
    }
    if (!strcmp(op, "adc_release")) return release_adc();
    if (!strcmp(op, "adc_config")) {
        int atten;
        adc_unit_t unit;
        adc_channel_t channel;
        if (!number(req, "pin", 0, 4, &pin) || !pin_available(pin, IO_ADC) ||
            !number(req, "atten", 0, 25, &atten)) return ESP_ERR_INVALID_ARG;
        adc_atten_t value;
        switch (atten) {
        case 0: value = ADC_ATTEN_DB_0; break;
        case 25: value = ADC_ATTEN_DB_2_5; break;
        case 6: value = ADC_ATTEN_DB_6; break;
        case 12: value = ADC_ATTEN_DB_12; break;
        default: return ESP_ERR_INVALID_ARG;
        }
        if (adc_oneshot_io_to_channel(pin, &unit, &channel) != ESP_OK || unit != ADC_UNIT_1) return ESP_ERR_INVALID_ARG;
        esp_err_t err = release_adc();
        if (err != ESP_OK) return err;
        adc_oneshot_unit_init_cfg_t config = {.unit_id = unit};
        err = adc_oneshot_new_unit(&config, &s_adc.unit);
        if (err != ESP_OK) return err;
        s_owner[pin] = IO_ADC; s_adc.pin = pin; s_adc.channel = channel; s_adc.atten = atten;
        adc_oneshot_chan_cfg_t chan = {.atten = value, .bitwidth = ADC_BITWIDTH_12};
        err = adc_oneshot_config_channel(s_adc.unit, channel, &chan);
        if (err == ESP_OK) {
            adc_cali_curve_fitting_config_t cal = {.unit_id = unit, .chan = channel, .atten = value, .bitwidth = ADC_BITWIDTH_12};
            err = adc_cali_create_scheme_curve_fitting(&cal, &s_adc.cal);
            if (err == ESP_ERR_NOT_SUPPORTED) err = ESP_OK;
        }
        if (err != ESP_OK) release_adc();
        return err;
    }
    if (!strcmp(op, "adc_read")) {
        if (!s_adc.unit) return ESP_ERR_INVALID_STATE;
        int raw, mv;
        esp_err_t err = adc_oneshot_read(s_adc.unit, s_adc.channel, &raw);
        if (err != ESP_OK) return err;
        cJSON_AddNumberToObject(out, "raw", raw);
        if (s_adc.cal && adc_cali_raw_to_voltage(s_adc.cal, raw, &mv) == ESP_OK)
            cJSON_AddNumberToObject(out, "mv", mv);
        else cJSON_AddNullToObject(out, "mv");
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

char *io_debug_execute(const char *request)
{
    /* Requests are flat objects. Bound parser recursion before calling cJSON. */
    bool flat = request && strlen(request) <= 512, quoted = false, escaped = false;
    int depth = 0;
    for (const char *p = request; flat && *p; p++) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '[' || (*p == '{' && ++depth > 1)) flat = false;
        else if (*p == '}' && --depth < 0) flat = false;
    }
    cJSON *req = flat ? cJSON_ParseWithOpts(request, NULL, true) : NULL;
    cJSON *out = cJSON_CreateObject();
    if (!out) { cJSON_Delete(req); return NULL; }
    esp_err_t err = cJSON_IsObject(req) ? execute(req, out) : ESP_ERR_INVALID_ARG;
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(out, "error", err == ESP_ERR_INVALID_ARG ? "参数无效或引脚已被占用" :
            err == ESP_ERR_INVALID_STATE ? "请先配置功能，或检查总线是否被拉低" :
            err == ESP_ERR_NOT_FOUND ? "I2C 无应答（NACK）" :
            err == ESP_ERR_TIMEOUT ? "通信超时，请检查接线与上拉" : "硬件操作失败，请检查配置");
        cJSON_AddStringToObject(out, "detail", esp_err_to_name(err));
    }
    char *result = cJSON_PrintUnformatted(out);
    cJSON_Delete(out); cJSON_Delete(req);
    return result;
}
