#include "../main/front_panel.c"

typedef enum {
    IO_NONE, IO_NEW_BUS, IO_RESET, IO_PROBE, IO_ADD_DEVICE,
    IO_SYNC, IO_INIT, IO_WINDOW, IO_FRAME, IO_ON, IO_COUNT
} io_event_t;

static char mock_bus, mock_device;
static const char *mock_ssid = "RL78-Flasher-A1B2";
const char *wifi_ap_ssid(void) { return mock_ssid; }
static const esp_app_desc_t mock_app = {
    .version = "1.0.0", .date = "Oct 26 2026", .time = "18:12:34"
};
const esp_app_desc_t *esp_app_get_description(void) { return &mock_app; }
static io_event_t io_events[128];
static unsigned io_count, io_calls[IO_COUNT], transfers;
static io_event_t fail_event;
static uint16_t present_address = 0x3c;
static uint8_t last_frame[sizeof(s_frame)];

static esp_err_t record_io(io_event_t event) {
    assert(io_count < sizeof(io_events) / sizeof(io_events[0]));
    io_events[io_count++] = event;
    io_calls[event]++;
    if (fail_event == event) {
        fail_event = IO_NONE;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *handle) {
    assert(config->i2c_port == I2C_NUM_0);
    assert(config->sda_io_num == 5 && config->scl_io_num == 6);
    assert(config->flags.enable_internal_pullup);
    esp_err_t err = record_io(IO_NEW_BUS);
    if (err == ESP_OK) *handle = &mock_bus;
    return err;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus) {
    assert(bus == &mock_bus);
    return record_io(IO_RESET);
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout) {
    assert(bus == &mock_bus && timeout == 50);
    assert(address == 0x3c || address == 0x3d);
    esp_err_t err = record_io(IO_PROBE);
    if (err != ESP_OK) return err;
    return address == present_address ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *handle) {
    assert(bus == &mock_bus && config->device_address == present_address);
    assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7 && config->scl_speed_hz == 100000);
    esp_err_t err = record_io(IO_ADD_DEVICE);
    if (err == ESP_OK) *handle = &mock_device;
    return err;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data,
                              size_t len, int timeout) {
    assert(dev == &mock_device && timeout == 50);
    assert(len >= 2);
    transfers++;
    io_event_t event;
    if (data[0] == 0x40) {
        assert(len == sizeof(s_frame));
        event = IO_FRAME;
    } else {
        assert(data[0] == 0x00);
        if (data[1] == 0xe3) {
            assert(len >= 7);
            for (size_t i = 1; i < len; i++) assert(data[i] == 0xe3);
            event = IO_SYNC;
        } else if (data[1] == 0xae) {
            assert(len > 7);
            event = IO_INIT;
        } else if (data[1] == 0x21) {
            const uint8_t expected[] = {0, 0x21, 28, 99, 0x22, 0, 4};
            assert(len == sizeof(expected) && memcmp(data, expected, len) == 0);
            event = IO_WINDOW;
        } else {
            assert(len == 2 && data[1] == 0xaf);
            event = IO_ON;
        }
    }
    esp_err_t err = record_io(event);
    if (err == ESP_OK && event == IO_FRAME) memcpy(last_frame, data, len);
    return err;
}

static void reset_oled_mock(void) {
    s_oled_bus = NULL;
    s_oled = NULL;
    s_oled_ready = false;
    s_oled_refreshed = 0;
    io_count = transfers = 0;
    memset(io_calls, 0, sizeof(io_calls));
    memset(last_frame, 0, sizeof(last_frame));
    fail_event = IO_NONE;
    present_address = 0x3c;
}

static void test_oled_recovery(void) {
    prog_status_t status = {.state = PROG_RUNNING, .percent = 25};
    strlcpy(status.file, "firmware.hex", sizeof(status.file));
    reset_oled_mock();
    assert(oled_init(&status, 0) == ESP_OK);
    const io_event_t startup[] = {
        IO_NEW_BUS, IO_RESET, IO_PROBE, IO_ADD_DEVICE,
        IO_SYNC, IO_INIT, IO_WINDOW, IO_FRAME, IO_ON
    };
    assert(io_count == sizeof(startup) / sizeof(startup[0]));
    assert(memcmp(io_events, startup, sizeof(startup)) == 0);
    render_status(&status, 0);
    assert(memcmp(last_frame, s_frame, sizeof(s_frame)) == 0);

    /* Every startup failure can recover, with no repeated resource allocation. */
    const io_event_t failures[] = {
        IO_NEW_BUS, IO_RESET, IO_PROBE, IO_ADD_DEVICE,
        IO_SYNC, IO_INIT, IO_WINDOW, IO_FRAME, IO_ON
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        reset_oled_mock();
        fail_event = failures[i];
        oled_update(&status, 0, 1000);
        assert(!s_oled_ready && s_oled_refreshed == 1000);
        if (failures[i] == IO_NEW_BUS) assert(s_oled_bus == NULL);
        unsigned failed_count = io_count;
        oled_update(&status, 0, 1999);
        assert(io_count == failed_count);
        oled_update(&status, 0, 2000);
        assert(s_oled_ready && s_oled_refreshed == 2000);
        assert(io_calls[IO_NEW_BUS] == (failures[i] == IO_NEW_BUS ? 2U : 1U));
        assert(io_calls[IO_ADD_DEVICE] == (failures[i] == IO_ADD_DEVICE ? 2U : 1U));
        assert(io_events[io_count - 1] == IO_ON);

        unsigned recovered_count = io_count;
        oled_update(&status, 0, 2099);
        assert(io_count == recovered_count);
        oled_update(&status, 0, 2100);
        assert(s_oled_ready && io_count == recovered_count + 2);
        assert(io_events[recovered_count] == IO_WINDOW);
        assert(io_events[recovered_count + 1] == IO_FRAME);
    }

    /* No display at boot: a later display at either supported address can recover. */
    reset_oled_mock();
    present_address = 0;
    oled_update(&status, 0, 1000);
    assert(!s_oled_ready && s_oled == NULL && io_calls[IO_PROBE] == 2);
    present_address = 0x3d;
    oled_update(&status, 0, 1999);
    assert(io_calls[IO_PROBE] == 2);
    oled_update(&status, 0, 2000);
    assert(s_oled_ready && io_calls[IO_PROBE] == 4);
    assert(io_calls[IO_NEW_BUS] == 1 && io_calls[IO_ADD_DEVICE] == 1);

    /* A transfer interrupted during normal refresh no longer disables OLED forever. */
    fail_event = IO_FRAME;
    oled_update(&status, 0, 2100);
    assert(!s_oled_ready);
    unsigned failed_count = io_count;
    oled_update(&status, 0, 2200);
    oled_update(&status, 0, 3099);
    assert(io_count == failed_count);
    strlcpy(status.file, "new.hex", sizeof(status.file));
    status.percent = 75;
    oled_update(&status, 0, 3100);
    assert(s_oled_ready && io_count == failed_count + 6);
    assert(io_events[failed_count] == IO_RESET);
    assert(io_events[io_count - 1] == IO_ON);
    assert(io_calls[IO_NEW_BUS] == 1 && io_calls[IO_ADD_DEVICE] == 1);
    render_status(&status, 0);
    assert(memcmp(last_frame, s_frame, sizeof(s_frame)) == 0);
    /* Both healthy refresh and delayed recovery retain unsigned tick wrap handling. */
    s_oled_refreshed = UINT32_MAX - 50;
    failed_count = io_count;
    oled_update(&status, 0, 48);
    assert(io_count == failed_count);
    oled_update(&status, 0, 49);
    assert(s_oled_ready && io_count == failed_count + 2);
    s_oled_ready = false;
    s_oled_refreshed = UINT32_MAX - 500;
    failed_count = io_count;
    oled_update(&status, 0, 498);
    assert(io_count == failed_count);
    oled_update(&status, 0, 499);
    assert(s_oled_ready && io_count == failed_count + 6);
}

static bool pixel(unsigned x, unsigned y) {
    return (s_frame[1 + y / 8 * OLED_WIDTH + x] >> (y % 8)) & 1;
}

int main(void) {
    button_t b = {.raw_pressed = true, .pressed = true};
    assert(button_update(&b, true, false, 2000) == BUTTON_NONE);
    assert(button_update(&b, false, false, 2010) == BUTTON_NONE);
    assert(button_update(&b, false, false, 2050) == BUTTON_NONE);
    assert(button_update(&b, true, false, 2100) == BUTTON_NONE);
    assert(button_update(&b, false, false, 2110) == BUTTON_NONE);
    assert(button_update(&b, true, false, 2120) == BUTTON_NONE);
    assert(button_update(&b, true, false, 2160) == BUTTON_NONE);
    assert(button_update(&b, false, false, 2200) == BUTTON_NONE);
    assert(button_update(&b, false, false, 2240) == BUTTON_START);
    assert(button_update(&b, true, false, 2300) == BUTTON_NONE);
    assert(button_update(&b, true, false, 2340) == BUTTON_NONE);
    assert(button_update(&b, true, false, 3299) == BUTTON_NONE);
    assert(button_update(&b, true, false, 3300) == BUTTON_NEXT);
    assert(button_update(&b, true, false, 8000) == BUTTON_NONE);
    assert(button_update(&b, false, false, 8010) == BUTTON_NONE);
    assert(button_update(&b, false, false, 8050) == BUTTON_NONE);
    /* Busy press is consumed even if the job completes during debounce. */
    assert(button_update(&b, true, true, 8100) == BUTTON_NONE);
    assert(button_update(&b, true, false, 8140) == BUTTON_NONE);
    assert(button_update(&b, true, false, 9200) == BUTTON_NONE);
    assert(button_update(&b, false, false, 9210) == BUTTON_NONE);
    assert(button_update(&b, false, false, 9250) == BUTTON_NONE);
    b = (button_t){.armed = true};
    assert(button_update(&b, true, false, UINT32_MAX - 20) == BUTTON_NONE);
    assert(button_update(&b, true, false, 20) == BUTTON_NONE);
    assert(button_update(&b, true, false, 979) == BUTTON_NEXT);
    /* Release first observed at the threshold must still count as long press. */
    b = (button_t){.armed = true};
    assert(button_update(&b, true, false, 100) == BUTTON_NONE);
    assert(button_update(&b, true, false, 140) == BUTTON_NONE);
    assert(button_update(&b, false, false, 1100) == BUTTON_NONE);
    assert(button_update(&b, false, false, 1140) == BUTTON_NEXT);

    assert(led_level(PROG_IDLE, 0) == 1);
    assert(led_level(PROG_DONE, 0) == 0);
    assert(led_level(PROG_RUNNING, 0) == 0);
    assert(led_level(PROG_RUNNING, 500) == 1);
    assert(led_level(PROG_FAILED, 125) == 1);

    prog_status_t status = {.state = PROG_RUNNING, .percent = 50};
    strlcpy(status.file, "ABCDEFGHI", sizeof(status.file));
    strlcpy(status.chip, "R5F100LE", sizeof(status.chip));
    strlcpy(status.stage, "擦除", sizeof(status.stage));
    render_status(&status, 0);
    /* Every scaled glyph pixel lands at the intended row, across page boundaries. */
    for (unsigned y = 0; y < OLED_GLYPH_HEIGHT; y++) {
        for (unsigned x = 0; x < OLED_GLYPH_WIDTH; x++) {
            assert(pixel(x, y) == !!(font_upper[0][x * 5 / 7] & (1U << (y * 7 / 11))));
            assert(pixel(x, 14 + y) == !!(font_upper['R' - 'A'][x * 5 / 7] & (1U << (y * 7 / 11))));
            assert(pixel(x, 28 + y) == !!(font_upper['E' - 'A'][x * 5 / 7] & (1U << (y * 7 / 11))));
        }
    }
    uint8_t frame[sizeof(s_frame)];
    memcpy(frame, s_frame, sizeof(frame));
    render_status(&status, 1300);
    assert(memcmp(frame, s_frame, sizeof(frame)) == 0);
    strlcpy(status.file, "Abcdefghijklmnopqrstuvwxy0123456789_ABCDE12.hex", sizeof(status.file));
    render_status(&status, 0);
    memcpy(frame, s_frame, sizeof(frame));
    render_status(&status, 1100);
    for (unsigned y = 0; y < 40; y++) {
        for (unsigned x = 0; x < OLED_WIDTH; x++) {
            if (y < 11 && x < OLED_WIDTH - 2)
                assert(pixel(x, y) == !!(frame[1 + y / 8 * OLED_WIDTH + x + 2] & (1U << (y % 8))));
            if (y >= 11)
                assert(pixel(x, y) == !!(frame[1 + y / 8 * OLED_WIDTH + x] & (1U << (y % 8))));
        }
    }
    for (unsigned t = 0; t < 22000; t += 100) {
        render_status(&status, t);
        for (unsigned y = 11; y < 14; y++)
            for (unsigned x = 0; x < OLED_WIDTH; x++) assert(!pixel(x, y));
        for (unsigned y = 25; y < 28; y++)
            for (unsigned x = 0; x < OLED_WIDTH; x++) assert(!pixel(x, y));
    }
    test_oled_recovery();
    /* Splash stays visible through 2999ms, then restores the normal status. */
    s_wifi_splash = true;
    s_wifi_since = 100;
    s_oled_refreshed = 100;
    render_status(&status, 0);
    memcpy(frame, s_frame, sizeof(frame));
    oled_update(&status, 0, 3099);
    assert(s_wifi_splash && memcmp(frame, s_frame, sizeof(frame)) == 0);
    oled_update(&status, 0, 3100);
    assert(!s_wifi_splash);
    assert(memcmp(frame, s_frame, sizeof(frame)) != 0);
    /* Five small rows show the SSID, version, date and time simultaneously. */
    s_wifi_splash = true;
    render_status(&status, 0);
    for (unsigned y = 0; y < 7; y++) {
        for (unsigned x = 0; x < 5; x++) {
            assert(pixel(x, y) == !!(font_upper['R' - 'A'][x] & (1U << y)));
            assert(pixel(x, 8 + y) == !!(0x08 & (1U << y)));
            assert(pixel(x, 16 + y) == !!(font_upper['V' - 'A'][x] & (1U << y)));
            assert(pixel(x, 24 + y) == !!(font_upper['O' - 'A'][x] & (1U << y)));
            assert(pixel(x, 32 + y) == !!(font_digits[1][x] & (1U << y)));
            assert(pixel(12 + x, 32 + y) == (x == 2 && !!(0x14 & (1U << y))));
        }
    }
    for (unsigned y = 7; y < 40; y += 8)
        for (unsigned x = 0; x < OLED_WIDTH; x++) assert(!pixel(x, y));
    /* A maximum-length prefix scrolls, while its MAC suffix stays visible. */
    mock_ssid = "123456789012345678901234567-A1B2";
    render_status(&status, 0);
    memcpy(frame, s_frame, sizeof(frame));
    render_status(&status, 800);
    assert(memcmp(frame, s_frame, OLED_WIDTH + 1) != 0);
    assert(memcmp(frame + OLED_WIDTH + 1, s_frame + OLED_WIDTH + 1,
                  sizeof(frame) - OLED_WIDTH - 1) == 0);
    for (unsigned y = 0; y < 7; y++)
        for (unsigned x = 0; x < 5; x++)
            assert(pixel(11 * 6 + x, 8 + y) == !!(font_digits[2][x] & (1U << y)));
    s_wifi_splash = false;
    puts("front panel: short/long press, busy/boot guards, scaled three rows, scrolling, OLED recovery PASS");
}
