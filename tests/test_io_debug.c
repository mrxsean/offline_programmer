#include "../main/io_debug.c"

static int levels[22], operations, fail_gpio, fail_spi, fail_adc, cal_error;
static int reads[600], read_count, read_pos, stretch;
static uint8_t clock_bits[700];
static unsigned clock_count;
static bool clock_stuck;
static unsigned delay_us;
static char spi_handle, adc_handle, cal_handle;
static void check_pin(int pin) { assert(pin == 3 || pin == 4 || pin == 7 || pin == 10); }
esp_err_t gpio_config(const gpio_config_t *cfg) {
    operations++;
    for (int i = 0; i < 22; i++) if (cfg->pin_bit_mask & (1ULL << i)) check_pin(i);
    return fail_gpio ? ESP_FAIL : ESP_OK;
}
esp_err_t gpio_set_level(int pin, int value) {
    check_pin(pin); operations++;
    if (s_i2c.active && pin == s_i2c.scl && value && !levels[pin]) {
        if (clock_count < sizeof(clock_bits)) clock_bits[clock_count] = levels[s_i2c.sda];
        clock_count++;
    }
    levels[pin] = value; return ESP_OK;
}
int gpio_get_level(int pin) {
    check_pin(pin);
    if (s_i2c.active && pin == s_i2c.scl) {
        if (clock_stuck) return 0;
        if (stretch) { stretch--; return 0; }
    }
    if (s_i2c.active && pin == s_i2c.sda && read_pos < read_count) return reads[read_pos++];
    return levels[pin];
}
esp_err_t gpio_reset_pin(int pin) { check_pin(pin); operations++; return ESP_OK; }
void esp_rom_delay_us(uint32_t us) { delay_us += us; }
int64_t esp_timer_get_time(void) { return delay_us; }
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma) {
    assert(host == SPI2_HOST && dma == SPI_DMA_DISABLED && bus->max_transfer_sz == 64);
    check_pin(bus->sclk_io_num); check_pin(bus->mosi_io_num); check_pin(bus->miso_io_num);
    operations++; return fail_spi == 1 ? ESP_FAIL : ESP_OK;
}
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *cfg, spi_device_handle_t *dev) {
    check_pin(cfg->spics_io_num); assert(cfg->mode >= 0 && cfg->mode <= 3);
    if (fail_spi == 2) return ESP_FAIL;
    *dev = &spi_handle; return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t dev) { assert(dev == &spi_handle); return ESP_OK; }
esp_err_t spi_bus_free(int host) { return fail_spi == 4 ? ESP_FAIL : ESP_OK; }
esp_err_t spi_device_transmit(spi_device_handle_t dev, spi_transaction_t *t) {
    assert(dev == &spi_handle && t->length <= 512 && t->length > 0);
    for (size_t i = 0; i < t->length / 8; i++) ((uint8_t *)t->rx_buffer)[i] = ((const uint8_t *)t->tx_buffer)[i] ^ 0xff;
    return fail_spi == 3 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_io_to_channel(int pin, adc_unit_t *unit, adc_channel_t *channel) {
    check_pin(pin); assert(pin <= 4); *unit = ADC_UNIT_1; *channel = pin; return ESP_OK;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *unit) {
    assert(cfg->unit_id == ADC_UNIT_1);
    if (fail_adc == 1) return ESP_FAIL;
    *unit = &adc_handle; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t unit, adc_channel_t chan, const adc_oneshot_chan_cfg_t *cfg) {
    assert(unit == &adc_handle && cfg->bitwidth == 12); return fail_adc == 2 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t unit) { assert(unit == &adc_handle); return ESP_OK; }
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t unit, adc_channel_t chan, int *raw) {
    *raw = 2048; return fail_adc == 3 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *cal) {
    if (cal_error) return cal_error;
    *cal = &cal_handle; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t cal) { assert(cal == &cal_handle); return ESP_OK; }
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t cal, int raw, int *mv) {
    assert(cal == &cal_handle && raw == 2048); *mv = 1234; return ESP_OK;
}
static void request(const char *req, bool ok, const char *contains) {
    char *out = io_debug_execute(req);
    assert(out);
    cJSON *json = cJSON_Parse(out);
    assert(json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")) == ok);
    if (contains) assert(strstr(out, contains));
    cJSON_Delete(json); free(out);
}
static const char *spi_config = "{\"op\":\"spi_config\",\"sclk\":3,\"mosi\":4,\"miso\":7,\"cs\":10,\"hz\":1000000,\"mode\":0}";
static const char *adc_config = "{\"op\":\"adc_config\",\"pin\":3,\"atten\":12}";
int main(void) {
    request("{\"op\":\"status\"}", true, "\"reserved\":\"UART0\"");
    const char *invalid[] = {"{", "[]", "null", "{\"op\":{}}", "{\"op\":\"status\"}junk",
        "{\"op\":\"other\"}", "{\"op\":\"gpio_config\",\"pin\":3.5}",
        "{\"op\":\"adc_config\",\"pin\":3,\"atten\":1e999}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) request(invalid[i], false, NULL);
    for (int pin = -1; pin <= 22; pin++) {
        if (pin == 3 || pin == 4 || pin == 7 || pin == 10) continue;
        char req[128];
        snprintf(req, sizeof(req), "{\"op\":\"gpio_config\",\"pin\":%d,\"output\":1,\"pull\":0,\"level\":1}", pin);
        request(req, false, NULL);
    }
    assert(!operations);
    request("{\"op\":\"gpio_config\",\"pin\":3,\"output\":0,\"pull\":1,\"level\":0}", true, NULL);
    request("{\"op\":\"gpio_write\",\"pin\":3,\"level\":1}", false, NULL);
    request(adc_config, false, NULL);
    request("{\"op\":\"gpio_config\",\"pin\":3,\"output\":1,\"pull\":0,\"level\":1}", true, NULL);
    request("{\"op\":\"gpio_read\",\"pin\":3}", true, "\"level\":1");
    request("{\"op\":\"gpio_write\",\"pin\":3,\"level\":0}", true, "\"level\":0");
    request("{\"op\":\"gpio_release\",\"pin\":3}", true, NULL);
    request("{\"op\":\"i2c_config\",\"sda\":3,\"scl\":3,\"hz\":100000}", false, NULL);
    request("{\"op\":\"i2c_config\",\"sda\":3,\"scl\":4,\"hz\":100000}", true, NULL);
    request(spi_config, false, NULL);
    int samples[] = {1,0,0,1,0,1,0,1,0,0,1,0,1};
    memcpy(reads, samples, sizeof(samples)); read_count = sizeof(samples) / sizeof(samples[0]);
    stretch = 3;
    request("{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"00\",\"read\":1}", true, "\"hex\":\"A5\"");
    assert(read_pos == read_count && levels[3] == 1 && levels[4] == 1 && !stretch);
    /* Independent write: address 0x51, exactly two data bytes and no read. */
    read_pos = 0; read_count = 4; reads[0] = 1;
    reads[1] = reads[2] = reads[3] = 0; clock_count = 0;
    request("{\"op\":\"i2c_transfer\",\"address\":81,\"hex\":\"A55A\",\"read\":0,\"write\":2}", true, "\"hex\":\"\"");
    assert(read_pos == read_count && clock_count == 28);
    const uint8_t sent[] = {0xa2, 0xa5, 0x5a};
    for (unsigned byte = 0; byte < sizeof(sent); byte++)
        for (unsigned bit = 0; bit < 8; bit++)
            assert(clock_bits[byte * 9 + bit] == ((sent[byte] >> (7 - bit)) & 1));
    /* Pure 64-byte read: R/W=1, ACK intermediate bytes, NACK the final byte. */
    read_pos = 0; read_count = 514; reads[0] = 1; reads[1] = 0;
    for (int i = 2; i < read_count; i++) reads[i] = (i & 1) == 0;
    clock_count = 0;
    request("{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":64}", true, "\"hex\":\"AAAA");
    assert(read_pos == read_count && clock_count == 586);
    for (unsigned bit = 0; bit < 8; bit++) assert(clock_bits[bit] == ((0xa1 >> (7 - bit)) & 1));
    for (unsigned byte = 0; byte < 64; byte++) assert(clock_bits[9 + byte * 9 + 8] == (byte == 63));
    /* A 16-bit register read writes both address bytes before repeated START. */
    int reg_samples[] = {1,0,0,0,1,0};
    memcpy(reads, reg_samples, sizeof(reg_samples));
    for (int bit = 0; bit < 16; bit++) reads[6 + bit] = (0x1234 >> (15 - bit)) & 1;
    read_pos = 0; read_count = 22; clock_count = 0;
    request("{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":2,\"register_bytes\":2,\"register\":4660}", true, "\"hex\":\"1234\"");
    assert(read_pos == read_count && clock_count == 56);
    const uint8_t prefix[] = {0xa0, 0x12, 0x34};
    for (unsigned byte = 0; byte < sizeof(prefix); byte++)
        for (unsigned bit = 0; bit < 8; bit++)
            assert(clock_bits[byte * 9 + bit] == ((prefix[byte] >> (7 - bit)) & 1));
    /* 64 data bytes plus a 16-bit register fit without truncating the data. */
    char long_write[300], payload[129];
    memset(payload, 'A', 128); payload[128] = 0;
    snprintf(long_write, sizeof(long_write), "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"%s\",\"read\":0,\"write\":64,\"register_bytes\":2,\"register\":4660}", payload);
    memset(reads, 0, sizeof(reads)); reads[0] = 1;
    read_pos = 0; read_count = 68; clock_count = 0;
    request(long_write, true, "\"hex\":\"\"");
    assert(read_pos == read_count && clock_count == 604);
    for (unsigned byte = 0; byte < 67; byte++) {
        uint8_t value = byte < 3 ? prefix[byte] : 0xaa;
        for (unsigned bit = 0; bit < 8; bit++) assert(clock_bits[byte * 9 + bit] == ((value >> (7 - bit)) & 1));
    }
    const char *bad_lengths[] = {
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"00FF\",\"read\":0,\"write\":1}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"00\",\"read\":0,\"write\":0}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"00\",\"read\":0,\"write\":65}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"00\",\"read\":0,\"write\":1.5}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"00\",\"read\":1,\"write\":1}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":65}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1,\"register_bytes\":3,\"register\":0}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1,\"register_bytes\":1,\"register\":256}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1,\"register_bytes\":2,\"register\":65536}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1,\"register_bytes\":2,\"register\":-1}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1,\"register_bytes\":2}",
        "{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1,\"register\":0}"
    };
    int before = operations;
    for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) request(bad_lengths[i], false, NULL);
    assert(operations == before);
    request("{\"op\":\"i2c_transfer\",\"address\":128,\"hex\":\"\",\"read\":1}", false, NULL);
    request("{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"0Z\",\"read\":1}", false, NULL);
    request("{\"op\":\"i2c_transfer\",\"address\":80,\"hex\":\"\",\"read\":1}", false, "NACK");
    read_pos = 0; read_count = 2; reads[0] = 1; reads[1] = 0;
    request("{\"op\":\"i2c_scan\"}", true, "\"addresses\":[3]");
    clock_stuck = true; delay_us = 0;
    request("{\"op\":\"i2c_scan\"}", false, "超时");
    assert(delay_us < 3000); clock_stuck = false;
    request("{\"op\":\"i2c_release\"}", true, NULL);
    for (fail_spi = 1; fail_spi <= 2; fail_spi++) {
        request(spi_config, false, NULL);
        assert(!s_spi.bus && !s_spi.device && s_owner[3] == IO_FREE);
    }
    fail_spi = 0; request(spi_config, true, NULL);
    request(adc_config, false, NULL);
    request("{\"op\":\"spi_transfer\",\"hex\":\"00FF55\"}", true, "\"hex\":\"FF00AA\"");
    request("{\"op\":\"spi_transfer\",\"hex\":\"F\"}", false, NULL);
    fail_spi = 3;
    request("{\"op\":\"spi_transfer\",\"hex\":\"FF\"}", false, NULL);
    fail_spi = 4; request("{\"op\":\"spi_release\"}", false, NULL);
    assert(s_owner[3] == IO_SPI);
    fail_spi = 0; request("{\"op\":\"spi_release\"}", true, NULL);
    for (fail_adc = 1; fail_adc <= 2; fail_adc++) {
        request(adc_config, false, NULL); assert(!s_adc.unit && s_owner[3] == IO_FREE);
    }
    fail_adc = 0; cal_error = ESP_ERR_NOT_SUPPORTED;
    request(adc_config, true, NULL);
    request("{\"op\":\"adc_read\"}", true, "\"mv\":null");
    request("{\"op\":\"adc_release\"}", true, NULL);
    cal_error = ESP_ERR_NO_MEM; request(adc_config, false, NULL);
    assert(!s_adc.unit && s_owner[3] == IO_FREE);
    cal_error = 0; request(adc_config, true, NULL);
    request("{\"op\":\"adc_read\"}", true, "\"mv\":1234");
    fail_adc = 3; request("{\"op\":\"adc_read\"}", false, NULL);
    request("{\"op\":\"adc_release\"}", true, NULL);
    for (int pin = 0; pin < 22; pin++) assert(s_owner[pin] == IO_FREE);
    puts("IO debug: reserved pins, conflicts, malformed requests, GPIO direction, I2C restart/ACK/NACK/stretch/timeout, SPI and ADC cleanup/calibration PASS");
}
