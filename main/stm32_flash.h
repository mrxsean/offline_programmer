/*
 * STM32 flash programming over SWD (via AHB-AP + STM32 flash controller).
 * Supported families: F0/F1/F3 (half-word, page erase), F2/F4 (word, sector
 * erase), G0/L4 (double-word, page erase).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STM32_STYLE_F1 = 1,     /* F0/F1/F3: 16-bit programming, page erase */
    STM32_STYLE_F4 = 2,     /* F2/F4: 32-bit programming, sector erase */
    STM32_STYLE_G0 = 3,     /* G0/L4: 64-bit double-word, page erase */
} stm32_style_t;

typedef struct {
    stm32_style_t style;
    uint16_t dev_id;        /* DBGMCU DEV_ID */
    char name[24];          /* family name */
    uint32_t flash_kb;      /* flash size from the size register */
    uint32_t page_size;     /* erase page size (F1/G0 styles) */
} stm32_info_t;

/*
 * Connect (swd_connect), halt the core under reset, identify the family,
 * read the flash size and unlock the flash controller.
 */
esp_err_t stm32_connect(stm32_info_t *info);

/* Erase the flash pages/sectors covering [addr, end). */
esp_err_t stm32_erase_region(const stm32_info_t *info, uint32_t addr, uint32_t end);

/* Mass erase of the whole flash. May take many seconds (F4). */
esp_err_t stm32_mass_erase(const stm32_info_t *info);

/* Program `len` bytes at `addr`. len must be a multiple of the program unit
 * (2 for F1, 4 for F4, 8 for G0). */
esp_err_t stm32_program(const stm32_info_t *info, uint32_t addr, const uint8_t *data, uint32_t len);

/* Verify `len` bytes at `addr` against the device content. */
esp_err_t stm32_verify(const stm32_info_t *info, uint32_t addr, const uint8_t *data, uint32_t len);

/* Lock the flash, un-halt, reset and let the application run. */
void stm32_reset_run(const stm32_info_t *info);

#define STM32_FLASH_BASE 0x08000000

/*
 * Iterate erase/program units (pages for F1/G0, sectors for F4).
 * Returns false when `index` is past the end of the flash.
 */
bool stm32_unit_bounds(const stm32_info_t *info, uint32_t index, uint32_t *addr, uint32_t *size);

#ifdef __cplusplus
}
#endif
