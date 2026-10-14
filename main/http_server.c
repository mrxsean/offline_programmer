#include "http_server.h"

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
#include "esp_http_server.h"

#include "storage.h"
#include "image_parser.h"
#include "rl78_prog.h"

static const char *TAG = "httpd";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ---------------- programming job state ---------------- */

typedef enum { PROG_IDLE, PROG_RUNNING, PROG_DONE, PROG_FAILED } prog_state_t;

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

typedef struct {
    char file[STORAGE_MAX_NAME];
    bool erase;
    bool program;
    bool verify;
} prog_job_t;

static void prog_log(const char *fmt, ...)
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
    prog_job_t job = *(prog_job_t *)arg;
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

/* ---------------- HTTP helpers ---------------- */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_error(httpd_req_t *req, int http_code, const char *msg)
{
    char buf[192];
    if (http_code != 200) {
        httpd_resp_set_status(req, http_code == 400 ? "400 Bad Request" :
                                 http_code == 404 ? "404 Not Found" : "500 Internal Server Error");
    }
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return send_json(req, buf);
}

static bool get_query_param(httpd_req_t *req, const char *key, char *buf, size_t len)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(q, key, buf, len) == ESP_OK;
}

/* ---------------- page handler ---------------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* ---------------- file API ---------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool first;
} json_acc_t;

static void files_cb(const storage_file_info_t *info, void *ctx)
{
    json_acc_t *acc = ctx;
    int n = snprintf(acc->buf + acc->len, acc->cap - acc->len, "%s{\"name\":\"%s\",\"size\":%u}",
                     acc->first ? "" : ",", info->name, (unsigned)info->size);
    if (n > 0) {
        acc->len += (size_t)n < acc->cap - acc->len ? (size_t)n : acc->cap - acc->len - 1;
    }
    acc->first = false;
}

static esp_err_t files_list_handler(httpd_req_t *req)
{
    json_acc_t acc;
    acc.cap = 4096;
    acc.buf = malloc(acc.cap);
    if (!acc.buf) {
        return send_json_error(req, 500, "内存不足");
    }
    acc.first = true;
    acc.len = snprintf(acc.buf, acc.cap, "{\"ok\":true,\"files\":[");

    storage_list(files_cb, &acc);

    size_t total = 0, used = 0;
    storage_usage(&total, &used);
    snprintf(acc.buf + acc.len, acc.cap - acc.len,
             "],\"total\":%u,\"used\":%u}", (unsigned)total, (unsigned)used);

    esp_err_t err = send_json(req, acc.buf);
    free(acc.buf);
    return err;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME];
    if (!get_query_param(req, "name", name, sizeof(name)) || !storage_name_valid(name)) {
        return send_json_error(req, 400, "文件名无效");
    }
    char path[96];
    storage_path(name, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        return send_json_error(req, 500, "无法创建文件");
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        return send_json_error(req, 500, "内存不足");
    }

    esp_err_t result = ESP_OK;
    size_t written = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, (char *)buf, chunk);
        if (r <= 0) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        written += r;
        remaining -= r;
    }
    free(buf);
    fclose(f);

    if (result != ESP_OK) {
        storage_delete(name);
        return send_json_error(req, 500, result == ESP_ERR_NO_MEM ? "存储空间不足" : "接收中断");
    }

    ESP_LOGI(TAG, "uploaded %s (%u bytes)", name, (unsigned)written);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"size\":%u}", (unsigned)written);
    return send_json(req, resp);
}

static esp_err_t download_handler(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME];
    if (!get_query_param(req, "name", name, sizeof(name)) || !storage_name_valid(name)) {
        return send_json_error(req, 400, "文件名无效");
    }
    char path[96];
    storage_path(name, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return send_json_error(req, 404, "文件不存在");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    uint8_t *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        return send_json_error(req, 500, "内存不足");
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        err = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (err != ESP_OK) {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    return err;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME];
    if (!get_query_param(req, "name", name, sizeof(name)) || !storage_name_valid(name)) {
        return send_json_error(req, 400, "文件名无效");
    }
    if (storage_delete(name) != ESP_OK) {
        return send_json_error(req, 404, "文件不存在");
    }
    ESP_LOGI(TAG, "deleted %s", name);
    return send_json(req, "{\"ok\":true}");
}

/* ---------------- target / program API ---------------- */

static void format_device_json(char *buf, size_t size, const rl78_signature_t *sig)
{
    uint32_t data_size = 0;
    if (sig->data_flash_end >= 0xF1000) {
        data_size = sig->data_flash_end - 0xF1000 + 1;
    }
    snprintf(buf, size,
             "\"name\":\"%s\",\"code\":\"%02X%02X%02X\","
             "\"code_size\":%" PRIu32 ",\"data_size\":%" PRIu32 ","
             "\"version\":\"%X.%X%X\"",
             sig->device_name, sig->device_code[0], sig->device_code[1], sig->device_code[2],
             sig->code_flash_end + 1, data_size,
             sig->fw_version[0], sig->fw_version[1], sig->fw_version[2]);
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    if (s_prog.state == PROG_RUNNING) {
        return send_json_error(req, 500, "烧录进行中");
    }
    xSemaphoreTake(s_prog.io_mu, portMAX_DELAY);
    rl78_signature_t sig;
    esp_err_t err = rl78_enter_programming_mode(&sig);
    xSemaphoreGive(s_prog.io_mu);

    if (err != ESP_OK) {
        prog_log("连接目标失败: %s", nack_hint());
        return send_json_error(req, 500, "目标无响应，请检查接线与供电");
    }
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    memcpy(&s_prog.sig, &sig, sizeof(sig));
    s_prog.dev_valid = true;
    xSemaphoreGive(s_prog.mu);

    prog_log("目标芯片: %s (代码闪存 %" PRIu32 " KB)", sig.device_name,
             (sig.code_flash_end + 1) / 1024);

    char dev[384];
    format_device_json(dev, sizeof(dev), &sig);
    char resp[448];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"device\":{%s}}", dev);
    return send_json(req, resp);
}

static esp_err_t program_handler(httpd_req_t *req)
{
    if (s_prog.state == PROG_RUNNING) {
        return send_json_error(req, 500, "已有烧录任务进行中");
    }
    prog_job_t *job = calloc(1, sizeof(prog_job_t));
    if (!job) {
        return send_json_error(req, 500, "内存不足");
    }
    char val[8];
    if (!get_query_param(req, "file", job->file, sizeof(job->file)) || !storage_name_valid(job->file)) {
        free(job);
        return send_json_error(req, 400, "文件名无效");
    }
    if (!storage_exists(job->file)) {
        free(job);
        return send_json_error(req, 404, "文件不存在");
    }
    if (get_query_param(req, "erase", val, sizeof(val)))   job->erase   = val[0] == '1';
    if (get_query_param(req, "program", val, sizeof(val))) job->program = val[0] == '1';
    if (get_query_param(req, "verify", val, sizeof(val)))  job->verify  = val[0] == '1';
    if (!job->erase && !job->program && !job->verify) {
        free(job);
        return send_json_error(req, 400, "未选择任何操作");
    }
    /* verify-only and erase-only don't need the image except for extent; keep it simple */

    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    s_prog.log_len = 0;
    s_prog.log[0] = '\0';
    s_prog.cancel = false;
    s_prog.state = PROG_RUNNING;
    xSemaphoreGive(s_prog.mu);

    if (xTaskCreate(program_task, "prog_task", 6144, job, 5, NULL) != pdPASS) {
        free(job);
        set_prog_state(PROG_IDLE, "空闲", NULL, 0);
        return send_json_error(req, 500, "任务创建失败");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t cancel_handler(httpd_req_t *req)
{
    if (s_prog.state == PROG_RUNNING) {
        s_prog.cancel = true;
    }
    return send_json(req, "{\"ok\":true}");
}

static const char *state_str(prog_state_t s)
{
    switch (s) {
    case PROG_RUNNING: return "running";
    case PROG_DONE:    return "done";
    case PROG_FAILED:  return "failed";
    default:           return "idle";
    }
}

/* Send `src` as a JSON string value (chunked, escaped). */
static esp_err_t send_json_string_chunked(httpd_req_t *req, const char *src, size_t src_len)
{
    char out[1024];
    size_t oi = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        const char *rep = NULL;
        char tmp[8];
        switch (c) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                rep = tmp;
            }
            break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (oi + rl >= sizeof(out)) {
                if (httpd_resp_send_chunk(req, out, oi) != ESP_OK) return ESP_FAIL;
                oi = 0;
            }
            memcpy(out + oi, rep, rl);
            oi += rl;
        } else {
            if (oi + 1 >= sizeof(out)) {
                if (httpd_resp_send_chunk(req, out, oi) != ESP_OK) return ESP_FAIL;
                oi = 0;
            }
            out[oi++] = c;
        }
    }
    if (oi > 0) {
        return httpd_resp_send_chunk(req, out, oi);
    }
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* snapshot */
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    prog_state_t state = s_prog.state;
    char stage[sizeof(s_prog.stage)];
    char message[sizeof(s_prog.message)];
    strlcpy(stage, s_prog.stage, sizeof(stage));
    strlcpy(message, s_prog.message, sizeof(message));
    uint32_t percent = s_prog.percent;
    bool dev_valid = s_prog.dev_valid;
    rl78_signature_t sig = s_prog.sig;
    size_t log_len = s_prog.log_len;
    char *log_copy = malloc(log_len + 1);
    if (log_copy) {
        memcpy(log_copy, s_prog.log, log_len + 1);
    }
    xSemaphoreGive(s_prog.mu);

    char head[256];
    int hl = snprintf(head, sizeof(head),
                      "{\"state\":\"%s\",\"stage\":\"%s\",\"message\":\"%s\",\"percent\":%" PRIu32 ",",
                      state_str(state), stage, message, percent);
    esp_err_t err = httpd_resp_send_chunk(req, head, hl);
    if (err == ESP_OK && dev_valid) {
        char dev[384];
        format_device_json(dev, sizeof(dev), &sig);
        char seg[416];
        int sl = snprintf(seg, sizeof(seg), "\"device\":{%s},", dev);
        err = httpd_resp_send_chunk(req, seg, sl);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "\"log\":\"", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK && log_copy) {
        err = send_json_string_chunked(req, log_copy, log_len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(log_copy);
    return err;
}

/* ---------------- server setup ---------------- */

esp_err_t http_server_start(void)
{
    s_prog.mu = xSemaphoreCreateMutex();
    s_prog.io_mu = xSemaphoreCreateMutex();
    configASSERT(s_prog.mu && s_prog.io_mu);
    s_prog.state = PROG_IDLE;
    strlcpy(s_prog.stage, "空闲", sizeof(s_prog.stage));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd start failed");

    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,    .handler = index_handler },
        { .uri = "/api/files",   .method = HTTP_GET,    .handler = files_list_handler },
        { .uri = "/api/upload",  .method = HTTP_POST,   .handler = upload_handler },
        { .uri = "/api/file",    .method = HTTP_GET,    .handler = download_handler },
        { .uri = "/api/file",    .method = HTTP_DELETE, .handler = delete_handler },
        { .uri = "/api/connect", .method = HTTP_POST,   .handler = connect_handler },
        { .uri = "/api/program", .method = HTTP_POST,   .handler = program_handler },
        { .uri = "/api/cancel",  .method = HTTP_POST,   .handler = cancel_handler },
        { .uri = "/api/status",  .method = HTTP_GET,    .handler = status_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register uri failed");
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
