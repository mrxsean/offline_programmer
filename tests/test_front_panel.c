#include "../main/front_panel.c"

static int transfers;
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout) {
    assert(timeout == 50);
    if (transfers++ == 0) {
        const uint8_t expected[] = {0, 0x21, 28, 99, 0x22, 0, 4};
        assert(len == sizeof(expected) && memcmp(data, expected, len) == 0);
    } else assert(len == 361 && data[0] == 0x40);
    return ESP_OK;
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

    /* Full 47-character name remains accessible, including case and suffix. */
    prog_status_t status = {.state = PROG_RUNNING, .percent = 50};
    strlcpy(status.file, "Abcdefghijklmnopqrstuvwxy0123456789_ABCDE12.hex", sizeof(status.file));
    assert(strlen(status.file) == 47);
    render_status(&status, 0);
    assert(memcmp(s_frame + 1, font_upper[0], 5) == 0);
    assert(memcmp(s_frame + 7, font_lower[1], 5) == 0);
    unsigned fill = 0;
    for (unsigned x = 1; x < 71; x++) if (s_frame[1 + 4 * 72 + x] == 0x7e) fill++;
    assert(fill == 35);
    render_status(&status, 1);
    assert(memcmp(s_frame + 1, font_lower['y' - 'a'], 5) == 0);
    assert(memcmp(s_frame + 1 + 72 + 10 * 6, font_lower['x' - 'a'], 5) == 0);
    status.state = PROG_DONE; status.percent = 100;
    render_status(&status, 0);
    for (unsigned x = 0; x < 72; x++) assert(s_frame[1 + 4 * 72 + x] == 0x7e);
    assert(oled_flush() == ESP_OK && transfers == 2);
    status = (prog_status_t){.state = PROG_FAILED};
    strlcpy(status.stage, "无固件", sizeof(status.stage));
    render_status(&status, 0);
    assert(memcmp(s_frame + 1 + 3 * 72, font_upper['S' - 'A'], 5) == 0);
    for (unsigned x = 0; x < 72; x++) assert(s_frame[1 + 4 * 72 + x] == 0);
    puts("front panel: debounce, held/busy key, tick wrap, active-low LED, filename pages, pixels/I2C bounds PASS");
}
