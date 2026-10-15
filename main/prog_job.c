#include "prog_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "image_parser.h"
#include "stm32_flash.h"

static const char *TAG = "prog_job";

#define LOG_BUF_SIZE 8192

static struct {
    SemaphoreHandle_t mu;
    SemaphoreHandle_t io_mu;        /* serializes target bus access */
    prog_state_t state;
    char stage[32];
    char message[192];
    uint32_t percent;
    char log[LOG_BUF_SIZE];
    size_t log_len;
    bool cancel;
    bool dev_valid;
    prog_target_t target;
    char dev_name[24];
    uint32_t dev_flash_kb;
    uint16_t dev_id;
    rl78_signature_t sig;
    stm32_info_t s32;
} s_prog;

void prog_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "%s", line);

    size_t need = strlen(line) + 1; /* + '\n' */
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    if (need >= LOG_BUF_SIZE) {
        need = LOG_BUF_SIZE - 1;
    }
    if (s_prog.log_len + need > LOG_BUF_SIZE - 1) {
        /* drop the oldest half */
        size_t drop = LOG_BUF_SIZE / 2;
        memmove(s_prog.log, s_prog.log + drop, s_prog.log_len - drop);
        s_prog.log_len -= drop;
    }
    size_t copy = strlen(line);
    if (copy > need - 1) {
        copy = need - 1;
    }
    memcpy(s_prog.log + s_prog.log_len, line, copy);
    s_prog.log_len += copy;
    s_prog.log[s_prog.log_len++] = '\n';
    s_prog.log[s_prog.log_len] = '\0';
    xSemaphoreGive(s_prog.mu);
}

static void set_prog_state(prog_state_t state, const char *stage, const char *message, uint32_t percent)
{
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    s_prog.state = state;
    if (stage) {
        strlcpy(s_prog.stage, stage, sizeof(s_prog.stage));
    }
    if (message) {
        strlcpy(s_prog.message, message, sizeof(s_prog.message));
    } else {
        s_prog.message[0] = '\0';
    }
    s_prog.percent = percent > 100 ? 100 : percent;
    xSemaphoreGive(s_prog.mu);
}

/* ---------------- target detection (called under io_mu) ---------------- */

static esp_err_t detect_target(void)
{
    s_prog.dev_valid = false;

    esp_err_t err = rl78_enter_programming_mode(&s_prog.sig);
    if (err == ESP_OK) {
        s_prog.target = PROG_TARGET_RL78;
        strlcpy(s_prog.dev_name, s_prog.sig.device_name, sizeof(s_prog.dev_name));
        s_prog.dev_flash_kb = (s_prog.sig.code_flash_end + 1) / 1024;
        s_prog.dev_id = 0;
        s_prog.dev_valid = true;
        return ESP_OK;
    }
    prog_log("RL78 无响应，尝试 STM32 SWD ...");

    err = stm32_connect(&s_prog.s32);
    if (err == ESP_OK) {
        s_prog.target = PROG_TARGET_STM32;
        strlcpy(s_prog.dev_name, s_prog.s32.name, sizeof(s_prog.dev_name));
        s_prog.dev_flash_kb = s_prog.s32.flash_kb;
        s_prog.dev_id = s_prog.s32.dev_id;
        s_prog.dev_valid = true;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---------------- RL78 flow ---------------- */

static const char *nack_hint(void)
{
    uint8_t nack = rl78_last_nack();
    return nack ? rl78_status_str(nack) : "目标无响应";
}

static esp_err_t rl78_erase_range(uint32_t start, uint32_t end_excl, uint32_t blksz,
                                  uint32_t pct_base, uint32_t pct_span)
{
    bool blank;
    uint32_t total = (end_excl - start + blksz - 1) / blksz;
    uint32_t done = 0;
    for (uint32_t addr = start; addr < end_excl && !s_prog.cancel; addr += blksz) {
        esp_err_t err = rl78_block_blank_check(addr, blksz, &blank);
        if (err != ESP_OK) {
            prog_log("空白检查失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
            return err;
        }
        if (!blank) {
            err = rl78_block_erase(addr);
            if (err != ESP_OK) {
                prog_log("擦除失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                return err;
            }
        }
        done++;
        set_prog_state(PROG_RUNNING, "擦除", NULL, pct_base + pct_span * done / total);
    }
    return s_prog.cancel ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t rl78_job_flow(const prog_job_req_t *job, image_t *img,
                               uint32_t img_min, uint32_t img_end, uint8_t *blk)
{
    const uint32_t blksz = CONFIG_PROG_FLASH_BLOCK_SIZE;
    esp_err_t result;

    /* erase-only: whole code flash */
    if (job->erase && !job->program) {
        set_prog_state(PROG_RUNNING, "全片擦除", NULL, 5);
        prog_log("全片擦除 (%" PRIu32 " KB) ...", s_prog.dev_flash_kb);
        result = rl78_erase_range(0, s_prog.sig.code_flash_end + 1, blksz, 5, 90);
        if (result == ESP_OK) {
            prog_log("擦除完成");
        }
        return result;
    }

    /* program */
    if (job->program) {
        uint32_t start = img_min & ~(blksz - 1);
        uint32_t total = (img_end - start + blksz - 1) / blksz;
        uint32_t done = 0;
        uint32_t pct_span = job->verify ? 60 : 95;
        set_prog_state(PROG_RUNNING, "编程", NULL, 5);
        for (uint32_t addr = start; addr < img_end && !s_prog.cancel; addr += blksz) {
            done++;
            bool any = image_build_block(img, addr, blk, blksz);
            set_prog_state(PROG_RUNNING, "编程", NULL, 5 + pct_span * done / total);
            if (!any) {
                continue;
            }
            bool blank;
            result = rl78_block_blank_check(addr, blksz, &blank);
            if (result != ESP_OK) {
                prog_log("空白检查失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                return result;
            }
            if (!blank) {
                if (!job->erase) {
                    prog_log("块 @0x%06" PRIX32 " 非空，且未勾选擦除，终止", addr);
                    return ESP_ERR_INVALID_STATE;
                }
                result = rl78_block_erase(addr);
                if (result != ESP_OK) {
                    prog_log("擦除失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                    return result;
                }
            }
            result = rl78_program(addr, blk, blksz);
            if (result != ESP_OK) {
                prog_log("编程失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                return result;
            }
        }
        if (s_prog.cancel) {
            return ESP_ERR_INVALID_STATE;
        }
        prog_log("编程完成 (%" PRIu32 " 块)", total);
    }

    /* verify */
    if (job->verify) {
        uint32_t start = img_min & ~(blksz - 1);
        uint32_t total = (img_end - start + blksz - 1) / blksz;
        uint32_t done = 0;
        uint32_t pct_base = job->program ? 65 : 5;
        set_prog_state(PROG_RUNNING, "校验", NULL, pct_base);
        for (uint32_t addr = start; addr < img_end && !s_prog.cancel; addr += blksz) {
            done++;
            bool any = image_build_block(img, addr, blk, blksz);
            set_prog_state(PROG_RUNNING, "校验", NULL, pct_base + (95 - pct_base) * done / total);
            if (!any) {
                continue;
            }
            result = rl78_verify(addr, blk, blksz);
            if (result != ESP_OK) {
                prog_log("校验失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                return result;
            }
        }
        if (s_prog.cancel) {
            return ESP_ERR_INVALID_STATE;
        }
        prog_log("校验通过");
    }
    return ESP_OK;
}

/* ---------------- STM32 flow ---------------- */

static esp_err_t stm32_job_flow(const prog_job_req_t *job, image_t *img,
                                uint32_t img_min, uint32_t img_end)
{
    const stm32_info_t *s32 = &s_prog.s32;
    esp_err_t result;

    /* erase-only: mass erase */
    if (job->erase && !job->program) {
        set_prog_state(PROG_RUNNING, "全片擦除", NULL, 5);
        prog_log("STM32 全片擦除 (%" PRIu32 " KB)，可能需要数十秒 ...", s32->flash_kb);
        result = stm32_mass_erase(s32);
        if (result == ESP_OK) {
            prog_log("擦除完成");
        } else {
            prog_log("全片擦除失败");
        }
        return result;
    }

    /* effective address range: 0-based images are flashed at 0x08000000 */
    uint32_t base = img_min >= STM32_FLASH_BASE ? 0 : STM32_FLASH_BASE;
    uint32_t eff_min = base + img_min;
    uint32_t eff_end = base + img_end;

    /* count units in range for progress reporting */
    uint32_t total = 0;
    uint32_t u_addr = 0, u_size = 0;
    for (uint32_t idx = 0; stm32_unit_bounds(s32, idx, &u_addr, &u_size); idx++) {
        if (u_addr >= eff_end) break;
        if (u_addr + u_size > eff_min) total++;
    }
    if (total == 0) total = 1;

    uint8_t *buf = NULL;
    uint32_t done = 0;

    for (int pass = 0; pass < 2; pass++) {
        bool do_program = (pass == 0) && job->program;
        bool do_verify = (pass == 1) && job->verify;
        if (!do_program && !do_verify) {
            continue;
        }
        uint32_t pct_base = do_program ? 5 : (job->program ? 65 : 5);
        uint32_t pct_span = do_program ? (job->verify ? 60 : 95) : (95 - pct_base);
        set_prog_state(PROG_RUNNING, do_program ? "编程" : "校验", NULL, pct_base);

        done = 0;
        for (uint32_t idx = 0;
             stm32_unit_bounds(s32, idx, &u_addr, &u_size) && !s_prog.cancel;
             idx++) {
            if (u_addr >= eff_end) break;
            if (u_addr + u_size <= eff_min) continue;

            if (u_size > 0 && !buf) {
                buf = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
                if (!buf) {
                    prog_log("内存不足");
                    return ESP_ERR_NO_MEM;
                }
            }
            /* build the unit image relative to the effective range */
            bool any = image_build_block(img, u_addr - base, buf, u_size);
            done++;
            set_prog_state(PROG_RUNNING, do_program ? "编程" : "校验", NULL,
                           pct_base + pct_span * done / total);
            if (!any) {
                continue;
            }
            /* trim trailing 0xFF to the largest program unit (8 bytes) */
            uint32_t trim = u_size;
            while (trim > 0 && buf[trim - 1] == 0xFF) trim--;
            trim = (trim + 7) & ~7U;

            if (do_program) {
                if (job->erase) {
                    result = stm32_erase_region(s32, u_addr, u_addr + u_size);
                    if (result != ESP_OK) {
                        prog_log("擦除失败 @0x%08" PRIX32, u_addr);
                        return result;
                    }
                }
                result = stm32_program(s32, u_addr, buf, trim);
                if (result != ESP_OK) {
                    prog_log("编程失败 @0x%08" PRIX32, u_addr);
                    return result;
                }
            } else {
                result = stm32_verify(s32, u_addr, buf, trim);
                if (result != ESP_OK) {
                    prog_log("校验失败 @0x%08" PRIX32, u_addr);
                    return result;
                }
            }
        }
        if (s_prog.cancel) {
            free(buf);
            return ESP_ERR_INVALID_STATE;
        }
        prog_log("%s完成", do_program ? "编程" : "校验");
    }
    free(buf);
    return ESP_OK;
}

/* ---------------- programming task ---------------- */

static void program_task(void *arg)
{
    prog_job_req_t job = *(prog_job_req_t *)arg;
    free(arg);

    esp_err_t result = ESP_OK;
    image_t img;
    bool img_loaded = false;
    uint8_t *blk = NULL;
    char path[96];

    xSemaphoreTake(s_prog.io_mu, portMAX_DELAY);

    set_prog_state(PROG_RUNNING, "解析文件", NULL, 0);
    prog_log("===== 开始烧录任务: %s (擦除:%d 编程:%d 校验:%d) =====",
             job.file, job.erase, job.program, job.verify);

    /* 1. parse the firmware image */
    if (!storage_path(job.file, path, sizeof(path))) {
        prog_log("文件名无效");
        result = ESP_ERR_INVALID_ARG;
        goto out;
    }
    result = image_parse_file(path, &img);
    if (result != ESP_OK) {
        prog_log("文件解析失败: %s", esp_err_to_name(result));
        goto out;
    }
    img_loaded = true;

    uint32_t img_min = 0, img_end = 0;
    if (!image_extent(&img, &img_min, &img_end) && (job.program || job.verify)) {
        prog_log("固件文件无数据");
        result = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    if (job.program || job.verify) {
        prog_log("固件: %u 段, %u 字节, 地址范围 0x%06" PRIX32 "..0x%06" PRIX32,
                 (unsigned)img.seg_count, (unsigned)img.total_data, img_min,
                 img_end > 0 ? img_end - 1 : 0);
    }

    /* 2. detect the target (RL78 serial, then STM32 SWD) */
    set_prog_state(PROG_RUNNING, "连接目标", NULL, 2);
    prog_log("探测目标芯片 (RL78 -> STM32 SWD) ...");
    result = detect_target();
    if (result != ESP_OK) {
        prog_log("目标连接失败，请检查接线与供电后重试。");
        goto out;
    }
    if (s_prog.target == PROG_TARGET_RL78) {
        prog_log("目标芯片: RL78 %s, 代码闪存 %" PRIu32 " KB",
                 s_prog.dev_name, s_prog.dev_flash_kb);
    } else {
        prog_log("目标芯片: %s, DEV_ID 0x%03X, 闪存 %" PRIu32 " KB",
                 s_prog.dev_name, s_prog.dev_id, s_prog.dev_flash_kb);
    }

    /* 3. run the target-specific flow */
    if (s_prog.target == PROG_TARGET_STM32) {
        if (s_prog.s32.style == STM32_STYLE_F4 &&
            job.program && img_end - 1 > s_prog.s32.flash_kb * 1024) {
            /* guard against out-of-flash images */
            prog_log("警告: 固件超出闪存范围");
        }
        result = stm32_job_flow(&job, &img, img_min, img_end);
    } else {
        blk = malloc(CONFIG_PROG_FLASH_BLOCK_SIZE);
        if (!blk) {
            prog_log("内存不足");
            result = ESP_ERR_NO_MEM;
            goto out;
        }
        result = rl78_job_flow(&job, &img, img_min, img_end, blk);
    }

out:
    free(blk);
    if (img_loaded) {
        image_free(&img);
    }
    /* return the target to its application */
    if (s_prog.dev_valid) {
        if (s_prog.target == PROG_TARGET_STM32) {
            stm32_reset_run(&s_prog.s32);
        } else {
            rl78_reset_target();
        }
    }

    bool cancelled = s_prog.cancel;
    if (result == ESP_OK) {
        prog_log("===== 任务完成 =====");
        set_prog_state(PROG_DONE, "完成", "烧录成功，目标已复位运行", 100);
    } else if (cancelled) {
        prog_log("===== 任务已取消 =====");
        set_prog_state(PROG_IDLE, "空闲", "已取消", 0);
    } else {
        prog_log("===== 任务失败: %s =====", esp_err_to_name(result));
        set_prog_state(PROG_FAILED, "失败", esp_err_to_name(result), 0);
    }

    s_prog.cancel = false;
    xSemaphoreGive(s_prog.io_mu);
    vTaskDelete(NULL);
}

/* ---------------- public API ---------------- */

esp_err_t prog_job_init(void)
{
    s_prog.mu = xSemaphoreCreateMutex();
    s_prog.io_mu = xSemaphoreCreateMutex();
    configASSERT(s_prog.mu && s_prog.io_mu);
    s_prog.state = PROG_IDLE;
    s_prog.target = PROG_TARGET_NONE;
    strlcpy(s_prog.stage, "空闲", sizeof(s_prog.stage));
    return ESP_OK;
}

esp_err_t prog_job_start(const prog_job_req_t *req)
{
    if (s_prog.state == PROG_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!req->erase && !req->program && !req->verify) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_exists(req->file)) {
        return ESP_ERR_NOT_FOUND;
    }

    prog_job_req_t *job = malloc(sizeof(*job));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    *job = *req;

    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    s_prog.log_len = 0;
    s_prog.log[0] = '\0';
    s_prog.cancel = false;
    s_prog.state = PROG_RUNNING;
    xSemaphoreGive(s_prog.mu);

    if (xTaskCreate(program_task, "prog_task", 6144, job, 5, NULL) != pdPASS) {
        free(job);
        set_prog_state(PROG_IDLE, "空闲", NULL, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void prog_job_cancel(void)
{
    if (s_prog.state == PROG_RUNNING) {
        s_prog.cancel = true;
    }
}

bool prog_job_busy(void)
{
    return s_prog.state == PROG_RUNNING;
}

void prog_job_snapshot(prog_status_t *out)
{
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    out->state = s_prog.state;
    strlcpy(out->stage, s_prog.stage, sizeof(out->stage));
    strlcpy(out->message, s_prog.message, sizeof(out->message));
    out->percent = s_prog.percent;
    out->dev_valid = s_prog.dev_valid;
    out->target = s_prog.target;
    strlcpy(out->dev_name, s_prog.dev_name, sizeof(out->dev_name));
    out->dev_flash_kb = s_prog.dev_flash_kb;
    out->dev_id = s_prog.dev_id;
    memcpy(&out->sig, &s_prog.sig, sizeof(out->sig));
    xSemaphoreGive(s_prog.mu);
}

size_t prog_job_log_read(char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    size_t n = s_prog.log_len < cap - 1 ? s_prog.log_len : cap - 1;
    memcpy(buf, s_prog.log, n);
    buf[n] = '\0';
    xSemaphoreGive(s_prog.mu);
    return n;
}

esp_err_t prog_job_connect(rl78_signature_t *sig)
{
    if (s_prog.state == PROG_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_prog.io_mu, portMAX_DELAY);
    esp_err_t err = detect_target();
    xSemaphoreGive(s_prog.io_mu);

    if (err != ESP_OK) {
        prog_log("目标连接失败");
        return err;
    }
    if (s_prog.target == PROG_TARGET_RL78) {
        prog_log("目标芯片: RL78 %s (代码闪存 %" PRIu32 " KB)",
                 s_prog.dev_name, s_prog.dev_flash_kb);
        if (sig) {
            *sig = s_prog.sig;
        }
    } else {
        prog_log("目标芯片: %s (DEV_ID 0x%03X, 闪存 %" PRIu32 " KB)",
                 s_prog.dev_name, s_prog.dev_id, s_prog.dev_flash_kb);
    }
    return ESP_OK;
}
