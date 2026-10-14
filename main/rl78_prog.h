/*
 * RL78 serial programming (RL78 Protocol A) over 1-wire UART.
 *
 * Protocol reference: Renesas R01AN0815 "RL78 Microcontrollers
 * (RL78 Protocol A) Serial Programming Guide".
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  device_code[3];    /* DEC: device code */
    char     device_name[11];   /* DEV: device name, NUL-terminated */
    uint32_t code_flash_end;    /* CEN: last address of code flash */
    uint32_t data_flash_end;    /* DEN: last address of data flash, 0 if absent */
    uint8_t  fw_version[3];     /* VER: boot firmware version */
} rl78_signature_t;

/* Initialize UART/GPIOs. Must be called once before any other function. */
esp_err_t rl78_prog_init(void);

/*
 * Enter serial programming mode:
 * pulse RESET with TOOL0 held low, select 1-wire UART mode, set the baud
 * rate and read the silicon signature.
 * Returns ESP_OK on success and fills `sig` (when non-NULL).
 */
esp_err_t rl78_enter_programming_mode(rl78_signature_t *sig);

/* Pulse RESET to restart the target application. */
void rl78_reset_target(void);

/* Blank-check one flash block. *is_blank set on success. */
esp_err_t rl78_block_blank_check(uint32_t addr, uint32_t block_size, bool *is_blank);

/* Erase one flash block (address must be block aligned). */
esp_err_t rl78_block_erase(uint32_t addr);

/* Program `len` bytes at `addr`. addr..addr+len-1 must not cross a block boundary. */
esp_err_t rl78_program(uint32_t addr, const uint8_t *data, uint32_t len);

/* Verify `len` bytes at `addr` against the device content. */
esp_err_t rl78_verify(uint32_t addr, const uint8_t *data, uint32_t len);

/* Human readable string for a protocol status byte. */
const char *rl78_status_str(uint8_t status);

/* Status byte returned by the target when the last command failed with NACK. */
uint8_t rl78_last_nack(void);

#ifdef __cplusplus
}
#endif
