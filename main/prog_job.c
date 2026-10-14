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

#include "image_parser.h"

static const char *TAG = "prog_job";

#define LOG_BUF_SIZE 8192

static struct {
    SemaphoreHandle_t mu;
    SemaphoreHandle_t io_mu;        /* serializes target UART access */
    prog_state_t state;
    char stage[32];
    char message[192];
    uint32_t percent;
    char log[LOG_BUF_SIZE];
    size_t log_len;
    bool cancel;
    bool dev_valid;
    rl78_signature_t sig;
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

/* ---------------- programming task ---------------- */

static const char *nack_hint(void)
{
    uint8_t nack = rl78_last_nack();
    return nack ? rl78_status_str(nack) : "目标无响应";
}

static esp_err_t erase_range(uint32_t start, uint32_t end_excl, uint32_t blksz,
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

    /* 2. enter programming mode and read the signature */
    set_prog_state(PROG_RUNNING, "连接目标", NULL, 2);
    prog_log("进入目标编程模式 ...");
    result = rl78_enter_programming_mode(&s_prog.sig);
    if (result != ESP_OK) {
        prog_log("目标连接失败: %s。请检查接线与供电后重试。", nack_hint());
        goto out;
    }
    s_prog.dev_valid = true;
    {
        uint32_t code_kb = (s_prog.sig.code_flash_end + 1) / 1024;
        prog_log("目标芯片: %s, 设备编码 %02X%02X%02X, 代码闪存 %" PRIu32 " KB",
                 s_prog.sig.device_name, s_prog.sig.device_code[0],
                 s_prog.sig.device_code[1], s_prog.sig.device_code[2], code_kb);
    }

    if (job.program && s_prog.sig.code_flash_end &&
        img_end - 1 > s_prog.sig.code_flash_end) {
        prog_log("警告: 固件末尾 0x%06" PRIX32 " 超出代码闪存范围 0x%06" PRIX32,
                 img_end - 1, s_prog.sig.code_flash_end);
    }

    blk = malloc(CONFIG_PROG_FLASH_BLOCK_SIZE);
    if (!blk) {
        prog_log("内存不足");
        result = ESP_ERR_NO_MEM;
        goto out;
    }

    /* 3. erase-only mode: erase the whole code flash */
    if (job.erase && !job.program) {
        set_prog_state(PROG_RUNNING, "全片擦除", NULL, 5);
        prog_log("全片擦除 (%" PRIu32 " KB) ...", (s_prog.sig.code_flash_end + 1) / 1024);
        result = erase_range(0, s_prog.sig.code_flash_end + 1,
                             CONFIG_PROG_FLASH_BLOCK_SIZE, 5, 90);
        if (result == ESP_OK) {
            prog_log("擦除完成");
        }
        goto out;
    }

    /* 4. program (with optional per-block erase) */
    if (job.program) {
        uint32_t blksz = CONFIG_PROG_FLASH_BLOCK_SIZE;
        uint32_t start = img_min & ~(blksz - 1);
        uint32_t total = (img_end - start + blksz - 1) / blksz;
        uint32_t done = 0;
        uint32_t pct_span = job.verify ? 60 : 95;
        set_prog_state(PROG_RUNNING, "编程", NULL, 5);
        for (uint32_t addr = start; addr < img_end && !s_prog.cancel; addr += blksz) {
            done++;
            bool any = image_build_block(&img, addr, blk, blksz);
            set_prog_state(PROG_RUNNING, "编程", NULL, 5 + pct_span * done / total);
            if (!any) {
                continue;
            }
            bool blank;
            result = rl78_block_blank_check(addr, blksz, &blank);
            if (result != ESP_OK) {
                prog_log("空白检查失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                goto out;
            }
            if (!blank) {
                if (!job.erase) {
                    prog_log("块 @0x%06" PRIX32 " 非空，且未勾选擦除，终止", addr);
                    result = ESP_ERR_INVALID_STATE;
                    goto out;
                }
                result = rl78_block_erase(addr);
                if (result != ESP_OK) {
                    prog_log("擦除失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                    goto out;
                }
            }
            result = rl78_program(addr, blk, blksz);
            if (result != ESP_OK) {
                prog_log("编程失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                goto out;
            }
        }
        if (s_prog.cancel) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }
        prog_log("编程完成 (%" PRIu32 " 块)", total);
    }

    /* 5. verify */
    if (job.verify) {
        uint32_t blksz = CONFIG_PROG_FLASH_BLOCK_SIZE;
        uint32_t start = img_min & ~(blksz - 1);
        uint32_t total = (img_end - start + blksz - 1) / blksz;
        uint32_t done = 0;
        uint32_t pct_base = job.program ? 65 : 5;
        set_prog_state(PROG_RUNNING, "校验", NULL, pct_base);
        for (uint32_t addr = start; addr < img_end && !s_prog.cancel; addr += blksz) {
            done++;
            bool any = image_build_block(&img, addr, blk, blksz);
            set_prog_state(PROG_RUNNING, "校验", NULL, pct_base + (95 - pct_base) * done / total);
            if (!any) {
                continue;
            }
            result = rl78_verify(addr, blk, blksz);
            if (result != ESP_OK) {
                prog_log("校验失败 @0x%06" PRIX32 ": %s", addr, nack_hint());
                goto out;
            }
        }
        if (s_prog.cancel) {
            result = ESP_ERR_INVALID_STATE;
            goto out;
        }
        prog_log("校验通过");
    }

out:
    free(blk);
    if (img_loaded) {
        image_free(&img);
    }
    rl78_reset_target();

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
    rl78_signature_t tmp;
    esp_err_t err = rl78_enter_programming_mode(&tmp);
    xSemaphoreGive(s_prog.io_mu);

    if (err != ESP_OK) {
        prog_log("连接目标失败: %s", nack_hint());
        return err;
    }
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    memcpy(&s_prog.sig, &tmp, sizeof(tmp));
    s_prog.dev_valid = true;
    xSemaphoreGive(s_prog.mu);

    prog_log("目标芯片: %s (代码闪存 %" PRIu32 " KB)", tmp.device_name,
             (tmp.code_flash_end + 1) / 1024);
    if (sig) {
        *sig = tmp;
    }
    return ESP_OK;
}
