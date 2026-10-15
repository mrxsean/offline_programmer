#include "stm32_flash.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "stm32_swd.h"

static const char *TAG = "stm32";

/* Cortex-M debug registers */
#define REG_DHCSR  0xE000EDF0
#define REG_DEMCR  0xE000EDFC
#define REG_AIRCR  0xE000ED0C

#define DHCSR_HALT_DEBUG   0xA05F0003   /* KEY | C_HALT | C_DEBUGEN */
#define DEMCR_VC_CORERESET 0x00000001
#define AIRCR_SYSRESETREQ  0x05FA0004

/* F1-style flash controller (F0/F1/F3) */
#define F1_KEYR  0x40022004
#define F1_SR    0x4002200C
#define F1_CR    0x40022010
#define F1_AR    0x40022014
#define F1_CR_PG     (1 << 0)
#define F1_CR_PER    (1 << 1)
#define F1_CR_MER    (1 << 2)
#define F1_CR_STRT   (1 << 4)
#define F1_CR_LOCK   (1 << 7)
#define F1_SR_BSY    (1 << 0)
#define F1_SR_PGERR  (1 << 2)
#define F1_SR_WRPRT  (1 << 4)
#define F1_SR_EOP    (1 << 5)

/* F4-style flash controller (F2/F4) */
#define F4_KEYR  0x40023C04
#define F4_SR    0x40023C0C
#define F4_CR    0x40023C10
#define F4_CR_PG      (1 << 0)
#define F4_CR_SER     (1 << 1)
#define F4_CR_MER     (1 << 2)
#define F4_CR_SNB(n)  (((n) & 0xF) << 3)
#define F4_CR_PSIZE32 (2 << 8)
#define F4_CR_STRT    (1 << 16)
#define F4_CR_LOCK    (1UL << 31)
#define F4_SR_EOP     (1 << 0)
#define F4_SR_ERRS    ((1 << 1) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7))
#define F4_SR_BSY     (1 << 16)

/* G0/L4-style flash controller */
#define G0_KEYR  0x40022008
#define G0_SR    0x40022010
#define G0_CR    0x40022014
#define G0_CR_PG      (1 << 0)
#define G0_CR_PER     (1 << 1)
#define G0_CR_MER1    (1 << 2)
#define G0_CR_PNB(n)  (((n) & 0xFF) << 3)
#define G0_CR_STRT    (1 << 16)
#define G0_CR_LOCK    (1UL << 31)
#define G0_SR_EOP     (1 << 0)
#define G0_SR_BSY1    (1 << 16)

#define FLASH_KEY1 0x45670123
#define FLASH_KEY2 0xCDEF89AB

/* flash base address on all supported families */
#define FLASH_BASE 0x08000000

typedef struct {
    uint16_t dev_id;
    stm32_style_t style;
    const char *name;
    uint32_t size_reg;      /* 16-bit flash size register (KB) */
    uint32_t page_size;     /* erase page size (F1/G0 styles) */
} family_t;

static const family_t s_families[] = {
    { 0x410, STM32_STYLE_F1, "STM32F1/GD32F1 MD", 0x1FFFF7E0, 1024 },
    { 0x412, STM32_STYLE_F1, "STM32F1 LD",   0x1FFFF7E0, 1024 },
    { 0x414, STM32_STYLE_F1, "STM32F1 HD",   0x1FFFF7E0, 2048 },
    { 0x418, STM32_STYLE_F1, "STM32F105/107", 0x1FFFF7E0, 2048 },
    { 0x420, STM32_STYLE_F1, "STM32F100",    0x1FFFF7E0, 1024 },
    { 0x428, STM32_STYLE_F1, "STM32F100 HD", 0x1FFFF7E0, 2048 },
    { 0x430, STM32_STYLE_F1, "STM32F1 XL",   0x1FFFF7E0, 2048 },
    { 0x440, STM32_STYLE_F1, "STM32F030/05x", 0x1FFFF7CC, 1024 },
    { 0x442, STM32_STYLE_F1, "STM32F030xC/09x", 0x1FFFF7CC, 2048 },
    { 0x444, STM32_STYLE_F1, "STM32F03x",    0x1FFFF7CC, 1024 },
    { 0x445, STM32_STYLE_F1, "STM32F04x/070", 0x1FFFF7CC, 1024 },
    { 0x448, STM32_STYLE_F1, "STM32F07x/09x", 0x1FFFF7CC, 2048 },
    { 0x422, STM32_STYLE_F1, "STM32F302/303", 0x1FFFF7CC, 2048 },
    { 0x432, STM32_STYLE_F1, "STM32F37x",    0x1FFFF7CC, 2048 },
    { 0x438, STM32_STYLE_F1, "STM32F303x468", 0x1FFFF7CC, 2048 },
    { 0x439, STM32_STYLE_F1, "STM32F301/318", 0x1FFFF7CC, 2048 },
    { 0x446, STM32_STYLE_F1, "STM32F30xD/E", 0x1FFFF7CC, 2048 },
    { 0x411, STM32_STYLE_F4, "STM32F2xx",    0x1FFF7A22, 0 },
    { 0x413, STM32_STYLE_F4, "STM32F40x/41x", 0x1FFF7A22, 0 },
    { 0x419, STM32_STYLE_F4, "STM32F42x/43x", 0x1FFF7A22, 0 },
    { 0x423, STM32_STYLE_F4, "STM32F401xB/C", 0x1FFF7A22, 0 },
    { 0x433, STM32_STYLE_F4, "STM32F401xD/E", 0x1FFF7A22, 0 },
    { 0x431, STM32_STYLE_F4, "STM32F411/410", 0x1FFF7A22, 0 },
    { 0x421, STM32_STYLE_F4, "STM32F446",    0x1FFF7A22, 0 },
    { 0x434, STM32_STYLE_F4, "STM32F469/479", 0x1FFF7A22, 0 },
    { 0x441, STM32_STYLE_F4, "STM32F412",    0x1FFF7A22, 0 },
    { 0x458, STM32_STYLE_F4, "STM32F410",    0x1FFF7A22, 0 },
    { 0x463, STM32_STYLE_F4, "STM32F413/423", 0x1FFF7A22, 0 },
    { 0x460, STM32_STYLE_G0, "STM32G030/050/070", 0x1FFF75E0, 2048 },
    { 0x466, STM32_STYLE_G0, "STM32G03x/04x", 0x1FFF75E0, 2048 },
    { 0x467, STM32_STYLE_G0, "STM32G0B0/C0", 0x1FFF75E0, 2048 },
    { 0x415, STM32_STYLE_G0, "STM32L4x6",    0x1FFF75E0, 2048 },
    { 0x435, STM32_STYLE_G0, "STM32L43x/44x", 0x1FFF75E0, 2048 },
    { 0x462, STM32_STYLE_G0, "STM32L45x/46x", 0x1FFF75E0, 2048 },
};

/* ---------------- helpers ---------------- */

static esp_err_t wait_sr_clear(uint32_t sr_reg, uint32_t mask, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint32_t sr = 0;
    do {
        esp_err_t err = swd_mem_read32(sr_reg, &sr);
        if (err != ESP_OK) {
            return err;
        }
        if ((sr & mask) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "SR 0x%08" PRIX32 " timeout (SR=0x%08" PRIX32 ")", sr_reg, sr);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t flash_unlock(uint32_t keyr)
{
    ESP_RETURN_ON_ERROR(swd_mem_write32(keyr, FLASH_KEY1), TAG, "unlock key1");
    ESP_RETURN_ON_ERROR(swd_mem_write32(keyr, FLASH_KEY2), TAG, "unlock key2");
    return ESP_OK;
}

/* Halt the core (connect-under-reset already holds nRESET low). */
static esp_err_t core_halt_under_reset(void)
{
    ESP_RETURN_ON_ERROR(swd_mem_write32(REG_DHCSR, DHCSR_HALT_DEBUG), TAG, "dhcsr halt");
    ESP_RETURN_ON_ERROR(swd_mem_write32(REG_DEMCR, DEMCR_VC_CORERESET), TAG, "demcr vc_reset");
    ESP_RETURN_ON_ERROR(swd_mem_write32(REG_AIRCR, AIRCR_SYSRESETREQ), TAG, "aircr sysreset");

    swd_target_reset(false);   /* release external reset: core resets and halts */
    vTaskDelay(pdMS_TO_TICKS(10));

    uint32_t dhcsr = 0;
    int timeout = 50;
    do {
        ESP_RETURN_ON_ERROR(swd_mem_read32(REG_DHCSR, &dhcsr), TAG, "read dhcsr");
        if (dhcsr & (1UL << 17)) {   /* S_HALT */
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (--timeout);
    ESP_LOGW(TAG, "core did not halt (DHCSR=0x%08" PRIX32 "), continue anyway", dhcsr);
    return ESP_OK;
}

static const family_t *find_family(uint16_t dev_id)
{
    for (size_t i = 0; i < sizeof(s_families) / sizeof(s_families[0]); i++) {
        if (s_families[i].dev_id == dev_id) {
            return &s_families[i];
        }
    }
    return NULL;
}

/* ---------------- public API ---------------- */

esp_err_t stm32_connect(stm32_info_t *info)
{
    memset(info, 0, sizeof(*info));

    esp_err_t err = swd_connect();
    if (err != ESP_OK) {
        return err;
    }
    err = core_halt_under_reset();
    if (err != ESP_OK) {
        goto fail;
    }

    /* identify the family: F1/F3/F4 DBGMCU at 0xE0042000, F0/G0/L4 at 0x40015800 */
    uint32_t idcode_mcu = 0;
    err = swd_mem_read32(0xE0042000, &idcode_mcu);
    if (err != ESP_OK || (idcode_mcu & 0xFFF) == 0 || (idcode_mcu & 0xFFF) == 0xFFF) {
        err = swd_mem_read32(0x40015800, &idcode_mcu);
    }
    if (err != ESP_OK) {
        goto fail;
    }
    info->dev_id = idcode_mcu & 0xFFF;
    ESP_LOGI(TAG, "DBGMCU DEV_ID = 0x%03" PRIX32, (uint32_t)info->dev_id);

    const family_t *fam = find_family(info->dev_id);
    if (!fam) {
        ESP_LOGE(TAG, "unknown DEV_ID 0x%03" PRIX32, (uint32_t)info->dev_id);
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    info->style = fam->style;
    info->page_size = fam->page_size;
    strlcpy(info->name, fam->name, sizeof(info->name));

    /* flash size (16-bit KB) */
    uint32_t kb = 0;
    err = swd_mem_read32(fam->size_reg, &kb);
    if (err != ESP_OK) {
        goto fail;
    }
    info->flash_kb = kb & 0xFFFF;
    if (info->flash_kb == 0 || info->flash_kb > 2048) {
        ESP_LOGW(TAG, "suspicious flash size %" PRIu32 " KB", info->flash_kb);
    }
    /* GD32 clones report STM32F103's medium-density DEV_ID (0x410) regardless
     * of their real capacity; derive the F1 page size from the flash size
     * (like OpenOCD does): >=128KB -> 2KB pages (e.g. GD32F103RCT6, 256KB). */
    if (info->style == STM32_STYLE_F1) {
        info->page_size = (info->flash_kb >= 128) ? 2048 : 1024;
    }
    ESP_LOGI(TAG, "%s, flash %" PRIu32 " KB", info->name, info->flash_kb);

    /* unlock the flash controller */
    uint32_t keyr = info->style == STM32_STYLE_F4 ? F4_KEYR :
                    info->style == STM32_STYLE_G0 ? G0_KEYR : F1_KEYR;
    err = flash_unlock(keyr);
    if (err != ESP_OK) {
        goto fail;
    }
    return ESP_OK;

fail:
    swd_disconnect();
    return err;
}

esp_err_t stm32_mass_erase(const stm32_info_t *info)
{
    esp_err_t err = ESP_OK;
    switch (info->style) {
    case STM32_STYLE_F1:
        ESP_RETURN_ON_ERROR(wait_sr_clear(F1_SR, F1_SR_BSY, 1000), TAG, "bsy before mass erase");
        ESP_RETURN_ON_ERROR(swd_mem_write32(F1_CR, F1_CR_MER), TAG, "mer set");
        ESP_RETURN_ON_ERROR(swd_mem_write32(F1_CR, F1_CR_MER | F1_CR_STRT), TAG, "mer start");
        err = wait_sr_clear(F1_SR, F1_SR_BSY, 30000);
        swd_mem_write32(F1_CR, 0);
        return err;
    case STM32_STYLE_F4:
        ESP_RETURN_ON_ERROR(wait_sr_clear(F4_SR, F4_SR_BSY, 1000), TAG, "bsy before mass erase");
        ESP_RETURN_ON_ERROR(swd_mem_write32(F4_CR, F4_CR_PSIZE32 | F4_CR_MER | F4_CR_STRT), TAG, "mer start");
        err = wait_sr_clear(F4_SR, F4_SR_BSY, 60000);
        swd_mem_write32(F4_CR, 0);
        return err;
    case STM32_STYLE_G0:
        ESP_RETURN_ON_ERROR(wait_sr_clear(G0_SR, G0_SR_BSY1, 1000), TAG, "bsy before mass erase");
        ESP_RETURN_ON_ERROR(swd_mem_write32(G0_CR, G0_CR_MER1 | G0_CR_STRT), TAG, "mer start");
        err = wait_sr_clear(G0_SR, G0_SR_BSY1, 30000);
        swd_mem_write32(G0_CR, 0);
        return err;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t stm32_erase_region(const stm32_info_t *info, uint32_t addr, uint32_t end)
{
    if (addr < FLASH_BASE || end <= addr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t offset = addr - FLASH_BASE;

    switch (info->style) {
    case STM32_STYLE_F1: {
        for (uint32_t a = addr; a < end; a += info->page_size) {
            ESP_RETURN_ON_ERROR(wait_sr_clear(F1_SR, F1_SR_BSY, 1000), TAG, "bsy before page erase");
            ESP_RETURN_ON_ERROR(swd_mem_write32(F1_CR, F1_CR_PER), TAG, "per set");
            ESP_RETURN_ON_ERROR(swd_mem_write32(F1_AR, a), TAG, "ar set");
            ESP_RETURN_ON_ERROR(swd_mem_write32(F1_CR, F1_CR_PER | F1_CR_STRT), TAG, "per start");
            ESP_RETURN_ON_ERROR(wait_sr_clear(F1_SR, F1_SR_BSY, 500), TAG, "page erase timeout");
            uint32_t sr;
            ESP_RETURN_ON_ERROR(swd_mem_read32(F1_SR, &sr), TAG, "read sr");
            if (sr & (F1_SR_PGERR | F1_SR_WRPRT)) {
                ESP_LOGE(TAG, "page erase error @0x%08" PRIX32 " (SR=0x%08" PRIX32 ")", a, sr);
                swd_mem_write32(F1_SR, F1_SR_PGERR | F1_SR_WRPRT | F1_SR_EOP);
                return ESP_FAIL;
            }
        }
        swd_mem_write32(F1_CR, 0);
        return ESP_OK;
    }
    case STM32_STYLE_F4: {
        /* standard single-bank sector map: 4x16KB, 1x64KB, then 128KB */
        uint32_t sector = 0;
        uint32_t s_addr = FLASH_BASE;
        while (s_addr < FLASH_BASE + info->flash_kb * 1024 && sector < 16) {
            uint32_t s_size = sector < 4 ? 16 * 1024 :
                              sector == 4 ? 64 * 1024 : 128 * 1024;
            uint32_t s_end = s_addr + s_size;
            if (s_end > addr && s_addr < end) {
                ESP_RETURN_ON_ERROR(wait_sr_clear(F4_SR, F4_SR_BSY, 2000), TAG, "bsy before sector erase");
                ESP_RETURN_ON_ERROR(swd_mem_write32(F4_CR, F4_CR_PSIZE32 | F4_CR_SER | F4_CR_SNB(sector) | F4_CR_STRT),
                                    TAG, "sector erase start");
                ESP_RETURN_ON_ERROR(wait_sr_clear(F4_SR, F4_SR_BSY, 4000), TAG, "sector erase timeout");
                uint32_t sr;
                ESP_RETURN_ON_ERROR(swd_mem_read32(F4_SR, &sr), TAG, "read sr");
                if (sr & F4_SR_ERRS) {
                    ESP_LOGE(TAG, "sector %" PRIu32 " erase error (SR=0x%08" PRIX32 ")", sector, sr);
                    swd_mem_write32(F4_SR, F4_SR_ERRS | F4_SR_EOP);
                    return ESP_FAIL;
                }
            }
            s_addr = s_end;
            sector++;
        }
        swd_mem_write32(F4_CR, 0);
        return ESP_OK;
    }
    case STM32_STYLE_G0: {
        uint32_t first = offset / info->page_size;
        uint32_t last = (end - 1 - FLASH_BASE) / info->page_size;
        for (uint32_t p = first; p <= last; p++) {
            ESP_RETURN_ON_ERROR(wait_sr_clear(G0_SR, G0_SR_BSY1, 1000), TAG, "bsy before page erase");
            ESP_RETURN_ON_ERROR(swd_mem_write32(G0_CR, G0_CR_PER | G0_CR_PNB(p)), TAG, "per set");
            ESP_RETURN_ON_ERROR(swd_mem_write32(G0_CR, G0_CR_PER | G0_CR_PNB(p) | G0_CR_STRT), TAG, "per start");
            ESP_RETURN_ON_ERROR(wait_sr_clear(G0_SR, G0_SR_BSY1, 500), TAG, "page erase timeout");
        }
        swd_mem_write32(G0_CR, 0);
        return ESP_OK;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t stm32_program(const stm32_info_t *info, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        return ESP_OK;
    }

    switch (info->style) {
    case STM32_STYLE_F1: {
        if (len % 2) {
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_RETURN_ON_ERROR(swd_csw_set_size(1), TAG, "csw 16-bit");
        ESP_RETURN_ON_ERROR(wait_sr_clear(F1_SR, F1_SR_BSY, 1000), TAG, "bsy before program");
        ESP_RETURN_ON_ERROR(swd_mem_write32(F1_CR, F1_CR_PG), TAG, "pg set");
        esp_err_t err = ESP_OK;
        for (uint32_t i = 0; i < len; i += 2) {
            uint16_t hw = (uint16_t)(data[i] | (data[i + 1] << 8));
            err = swd_mem_write32(addr + i, hw);
            if (err != ESP_OK) {
                break;
            }
            err = wait_sr_clear(F1_SR, F1_SR_BSY, 50);
            if (err != ESP_OK) {
                break;
            }
        }
        uint32_t sr;
        if (swd_mem_read32(F1_SR, &sr) == ESP_OK && (sr & (F1_SR_PGERR | F1_SR_WRPRT))) {
            ESP_LOGE(TAG, "program error (SR=0x%08" PRIX32 ")", sr);
            err = ESP_FAIL;
        }
        swd_mem_write32(F1_CR, 0);
        swd_csw_set_size(2);
        return err;
    }
    case STM32_STYLE_F4: {
        if (len % 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_RETURN_ON_ERROR(wait_sr_clear(F4_SR, F4_SR_BSY, 1000), TAG, "bsy before program");
        ESP_RETURN_ON_ERROR(swd_mem_write32(F4_CR, F4_CR_PSIZE32 | F4_CR_PG), TAG, "pg set");
        esp_err_t err = ESP_OK;
        for (uint32_t i = 0; i < len; i += 4) {
            uint32_t w;
            memcpy(&w, data + i, 4);
            err = swd_mem_write32(addr + i, w);
            if (err != ESP_OK) {
                break;
            }
            err = wait_sr_clear(F4_SR, F4_SR_BSY, 50);
            if (err != ESP_OK) {
                break;
            }
        }
        uint32_t sr;
        if (swd_mem_read32(F4_SR, &sr) == ESP_OK && (sr & F4_SR_ERRS)) {
            ESP_LOGE(TAG, "program error (SR=0x%08" PRIX32 ")", sr);
            err = ESP_FAIL;
        }
        swd_mem_write32(F4_SR, F4_SR_ERRS | F4_SR_EOP);
        swd_mem_write32(F4_CR, 0);
        return err;
    }
    case STM32_STYLE_G0: {
        if (len % 8) {
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_RETURN_ON_ERROR(wait_sr_clear(G0_SR, G0_SR_BSY1, 1000), TAG, "bsy before program");
        ESP_RETURN_ON_ERROR(swd_mem_write32(G0_CR, G0_CR_PG), TAG, "pg set");
        esp_err_t err = ESP_OK;
        for (uint32_t i = 0; i < len; i += 8) {
            uint32_t lo, hi;
            memcpy(&lo, data + i, 4);
            memcpy(&hi, data + i + 4, 4);
            err = swd_mem_write32(addr + i, lo);
            if (err != ESP_OK) break;
            err = swd_mem_write32(addr + i + 4, hi);
            if (err != ESP_OK) break;
            err = wait_sr_clear(G0_SR, G0_SR_BSY1, 50);
            if (err != ESP_OK) break;
        }
        swd_mem_write32(G0_SR, G0_SR_EOP);
        swd_mem_write32(G0_CR, 0);
        return err;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t stm32_verify(const stm32_info_t *info, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (info->style == STM32_STYLE_F1) {
        ESP_RETURN_ON_ERROR(swd_csw_set_size(1), TAG, "csw 16-bit");
    }
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t v;
        esp_err_t err = swd_mem_read32(addr + i, &v);
        if (err != ESP_OK) {
            return err;
        }
        uint32_t expect = 0xFFFFFFFF;
        uint32_t chunk = len - i < 4 ? len - i : 4;
        memcpy(&expect, data + i, chunk);
        if (chunk < 4) {
            v &= (0xFFFFFFFF >> ((4 - chunk) * 8));
            expect &= (0xFFFFFFFF >> ((4 - chunk) * 8));
        }
        if (v != expect) {
            ESP_LOGE(TAG, "verify mismatch @0x%08" PRIX32 ": got 0x%08" PRIX32 " expect 0x%08" PRIX32,
                     addr + i, v, expect);
            if (info->style == STM32_STYLE_F1) {
                swd_csw_set_size(2);
            }
            return ESP_ERR_INVALID_CRC;
        }
    }
    if (info->style == STM32_STYLE_F1) {
        return swd_csw_set_size(2);
    }
    return ESP_OK;
}

bool stm32_unit_bounds(const stm32_info_t *info, uint32_t index, uint32_t *addr, uint32_t *size)
{
    uint32_t flash_end = STM32_FLASH_BASE + info->flash_kb * 1024;
    if (info->style == STM32_STYLE_F4) {
        uint32_t s_addr = STM32_FLASH_BASE;
        for (uint32_t i = 0; i <= index; i++) {
            uint32_t s_size = i < 4 ? 16 * 1024 : i == 4 ? 64 * 1024 : 128 * 1024;
            if (s_addr >= flash_end) {
                return false;
            }
            if (i == index) {
                *addr = s_addr;
                *size = s_size;
                return true;
            }
            s_addr += s_size;
        }
        return false;
    }
    uint32_t a = STM32_FLASH_BASE + index * info->page_size;
    if (a >= flash_end) {
        return false;
    }
    *addr = a;
    *size = info->page_size;
    return true;
}

void stm32_reset_run(const stm32_info_t *info)
{
    /* lock the flash controller */
    uint32_t cr = info->style == STM32_STYLE_F4 ? F4_CR :
                  info->style == STM32_STYLE_G0 ? G0_CR : F1_CR;
    uint32_t lock = info->style == STM32_STYLE_F1 ? F1_CR_LOCK :
                    info->style == STM32_STYLE_F4 ? F4_CR_LOCK : G0_CR_LOCK;
    swd_mem_write32(cr, lock);

    /* clear halt + debug, then reset to run the application */
    swd_mem_write32(REG_DHCSR, 0xA05F0000);
    swd_mem_write32(REG_DEMCR, 0x00000000);
    swd_mem_write32(REG_AIRCR, AIRCR_SYSRESETREQ);
    vTaskDelay(pdMS_TO_TICKS(20));
    swd_disconnect();
}
