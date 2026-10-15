/*
 * SWD (Serial Wire Debug) host for ARM Cortex-M (STM32).
 * Bit-banged SWDIO/SWCLK + MEM-AP (AHB-AP) 32-bit memory access.
 *
 * Reference: ARM Debug Interface Architecture Specification ADIv5.0.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure SWD GPIOs (from Kconfig). Call once at boot. */
void swd_host_init(void);

/*
 * Full connect sequence: assert nRESET, JTAG->SWD switch, line reset,
 * read DP IDCODE, power-up debug, configure AHB-AP (CSW 32-bit).
 * Returns ESP_OK on success (IDCODE nonzero and sane).
 */
esp_err_t swd_connect(void);

/* Release bus (SWDIO/SWCLK to inputs, nRESET released high). */
void swd_disconnect(void);

uint32_t swd_idcode(void);

/* Assert/deassert target nRESET pin. */
void swd_target_reset(bool assert);

/* MEM-AP CSW data size: 1 = 16-bit, 2 = 32-bit (default after connect). */
esp_err_t swd_csw_set_size(uint8_t size);

esp_err_t swd_mem_read32(uint32_t addr, uint32_t *val);
esp_err_t swd_mem_write32(uint32_t addr, uint32_t val);

#ifdef __cplusplus
}
#endif
