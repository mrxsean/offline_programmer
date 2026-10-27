#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SERIAL_DEBUG_TX_MAX 256
#define SERIAL_DEBUG_RX_CHUNK 512

typedef struct {
    uint8_t data[SERIAL_DEBUG_RX_CHUNK];
    size_t size;
    uint64_t next, lost;
    uint32_t baud, errors;
    bool more, reset;
} serial_debug_rx_t;

esp_err_t serial_debug_init(void);
esp_err_t serial_debug_set_baud(uint32_t baud);
esp_err_t serial_debug_send(const uint8_t *data, size_t size, size_t *written);
esp_err_t serial_debug_read(uint64_t cursor, serial_debug_rx_t *rx);
