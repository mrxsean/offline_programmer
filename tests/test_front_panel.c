#include "../main/front_panel.c"

typedef enum {
    IO_NONE, IO_NEW_BUS, IO_RESET, IO_PROBE, IO_ADD_DEVICE,
    IO_SYNC, IO_INIT, IO_WINDOW, IO_FRAME, IO_ON, IO_COUNT
} io_event_t;

static char mock_bus, mock_device;
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
    assert(memcmp(last_frame + 1, font_lower['f' - 'a'], 5) == 0);

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
    assert(memcmp(last_frame + 1, font_lower['n' - 'a'], 5) == 0);
    unsigned fill = 0;
    for (unsigned x = 1; x < OLED_WIDTH - 1; x++) {
        if (last_frame[1 + 4 * OLED_WIDTH + x] == 0x7e) fill++;
    }
    assert(fill == 52);

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

static void assert_blank_page(unsigned page) {
    for (unsigned x = 0; x < OLED_WIDTH; x++) {
        assert(s_frame[1 + page * OLED_WIDTH + x] == 0);
    }
}

int main(void) {
    button_t b = {.raw_pressed = true, .pressed = true};
    /* Power-on held must not program; release and press with bounce. */
    assert(!button_update(&b, true, false, 1000));
    assert(!button_update(&b, false, false, 1010));
    assert(!button_update(&b, false, false, 1050));
    assert(!button_update(&b, true, false, 1100));
    assert(!button_update(&b, false, false, 1110));
    assert(!button_update(&b, true, false, 1120));
    assert(!button_update(&b, true, false, 1159));
    assert(button_update(&b, true, false, 1160));
    assert(!button_update(&b, true, false, 5000));
    /* Busy press consumed even when the job completes during debounce. */
    assert(!button_update(&b, false, true, 5010));
    assert(!button_update(&b, false, true, 5050));
    assert(!button_update(&b, true, true, 5100));
    assert(!button_update(&b, true, false, 5140));
    assert(!button_update(&b, true, false, 9000));
    assert(!button_update(&b, false, false, 9010));
    assert(!button_update(&b, false, false, 9050));
    assert(!button_update(&b, true, false, 9060));
    assert(button_update(&b, true, false, 9100));
    /* Unsigned tick wrap. */
    b = (button_t){.armed = true};
    assert(!button_update(&b, true, false, UINT32_MAX - 20));
    assert(button_update(&b, true, false, 20));

    assert(led_level(PROG_IDLE, 0) == 1);
    assert(led_level(PROG_DONE, 0) == 0);
    assert(led_level(PROG_RUNNING, 0) == 0);
    assert(led_level(PROG_RUNNING, 500) == 1);
    assert(led_level(PROG_FAILED, 0) == 0);
    assert(led_level(PROG_FAILED, 125) == 1);

    /* Names that exactly fit the line never move. */
    prog_status_t short_status = {.state = PROG_IDLE};
    strlcpy(short_status.file, "ABCDEFGHIJKL", sizeof(short_status.file));
    render_status(&short_status, 0);
    uint8_t short_frame[sizeof(s_frame)];
    memcpy(short_frame, s_frame, sizeof(short_frame));
    assert(memcmp(s_frame + 1 + 66, font_upper['L' - 'A'], 5) == 0);
    render_status(&short_status, pdMS_TO_TICKS(1300));
    assert(memcmp(s_frame, short_frame, sizeof(short_frame)) == 0);
    render_status(&short_status, UINT32_MAX);
    assert(memcmp(s_frame, short_frame, sizeof(short_frame)) == 0);
    assert_blank_page(1);

    /* A thirteenth character triggers scrolling; both edges clip by pixels. */
    strlcpy(short_status.file, "ABCDEFGHIJKLM", sizeof(short_status.file));
    render_status(&short_status, pdMS_TO_TICKS(1100));
    assert(memcmp(s_frame + 1, font_upper[0] + 2, 3) == 0);
    assert(s_frame[1 + 3] == 0);
    assert(memcmp(s_frame + 1 + 4, font_upper[1], 5) == 0);
    assert(memcmp(s_frame + 1 + 70, font_upper['M' - 'A'], 2) == 0);
    assert_blank_page(1);

    /* Full 47-character name scrolls on one line, including case and suffix. */
    prog_status_t status = {.state = PROG_RUNNING, .percent = 50};
    strlcpy(status.file, "Abcdefghijklmnopqrstuvwxy0123456789_ABCDE12.hex", sizeof(status.file));
    assert(strlen(status.file) == 47);
    render_status(&status, 0);
    assert(memcmp(s_frame + 1, font_upper[0], 5) == 0);
    assert(memcmp(s_frame + 7, font_lower[1], 5) == 0);
    assert_blank_page(1);
    uint8_t first_line[OLED_WIDTH];
    uint8_t status_rows[OLED_WIDTH * (OLED_PAGES - 1)];
    memcpy(first_line, s_frame + 1, sizeof(first_line));
    memcpy(status_rows, s_frame + 1 + OLED_WIDTH, sizeof(status_rows));
    const unsigned pause_times[] = {500, 999, 1000, 1099};
    for (unsigned i = 0; i < sizeof(pause_times) / sizeof(pause_times[0]); i++) {
        render_status(&status, pdMS_TO_TICKS(pause_times[i]));
        assert(memcmp(s_frame + 1, first_line, sizeof(first_line)) == 0);
    }
    render_status(&status, pdMS_TO_TICKS(1100));
    assert(memcmp(s_frame + 1, first_line + 2, OLED_WIDTH - 2) == 0);
    render_status(&status, pdMS_TO_TICKS(1300));
    assert(memcmp(s_frame + 1, font_lower[1], 5) == 0);

    /* The final twelve characters fit together before the loop separator. */
    render_status(&status, pdMS_TO_TICKS(11500));
    assert(s_frame[1 + 50] == 0x60);
    assert(memcmp(s_frame + 1 + 54, font_lower['h' - 'a'], 5) == 0);
    assert(memcmp(s_frame + 1 + 60, font_lower['e' - 'a'], 5) == 0);
    assert(memcmp(s_frame + 1 + 66, font_lower['x' - 'a'], 5) == 0);

    /* Three blank characters separate the tail from the returning head. */
    render_status(&status, pdMS_TO_TICKS(15100));
    for (unsigned x = 0; x < 18; x++) assert(s_frame[1 + x] == 0);
    assert(memcmp(s_frame + 1 + 18, font_upper[0], 5) == 0);
    render_status(&status, pdMS_TO_TICKS(15900));
    assert(s_frame[1] == 0 && s_frame[2] == 0);
    assert(memcmp(s_frame + 1 + 2, font_upper[0], 5) == 0);
    const unsigned next_pause_times[] = {16000, 16999, 17099};
    for (unsigned i = 0; i < sizeof(next_pause_times) / sizeof(next_pause_times[0]); i++) {
        render_status(&status, pdMS_TO_TICKS(next_pause_times[i]));
        assert(memcmp(s_frame + 1, first_line, sizeof(first_line)) == 0);
    }
    render_status(&status, pdMS_TO_TICKS(17100));
    assert(memcmp(s_frame + 1, first_line + 2, OLED_WIDTH - 2) == 0);

    /* Scrolling never writes into the separator, status, hint, or progress. */
    for (unsigned elapsed = 0; elapsed <= 17100; elapsed += 100) {
        render_status(&status, pdMS_TO_TICKS(elapsed));
        assert(s_frame[0] == 0x40);
        assert(memcmp(s_frame + 1 + OLED_WIDTH, status_rows, sizeof(status_rows)) == 0);
    }
    unsigned fill = 0;
    for (unsigned x = 1; x < 71; x++) if (s_frame[1 + 4 * 72 + x] == 0x7e) fill++;
    assert(fill == 35);
    status.state = PROG_DONE; status.percent = 100;
    render_status(&status, 0);
    for (unsigned x = 0; x < 72; x++) assert(s_frame[1 + 4 * 72 + x] == 0x7e);
    s_oled = &mock_device;
    assert(oled_flush() == ESP_OK && transfers == 2);
    status = (prog_status_t){.state = PROG_FAILED};
    strlcpy(status.stage, "无固件", sizeof(status.stage));
    render_status(&status, 0);
    assert(memcmp(s_frame + 1, font_upper['N' - 'A'], 5) == 0);
    for (unsigned x = 42; x < OLED_WIDTH; x++) assert(s_frame[1 + x] == 0);
    assert_blank_page(1);
    assert(memcmp(s_frame + 1 + 3 * 72, font_upper['S' - 'A'], 5) == 0);
    for (unsigned x = 0; x < 72; x++) assert(s_frame[1 + 4 * 72 + x] == 0);
    test_oled_recovery();
    puts("front panel: debounce, LED, single-line scrolling, pixels/I2C bounds, OLED reset/retry/recovery PASS");
}
