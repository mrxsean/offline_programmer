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
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "nvs.h"

#include "storage.h"
#include "image_parser.h"
#include "rl78_prog.h"

static const char *TAG = "httpd";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ---------------- programming job state ---------------- */

#define LOG_BUF_SIZE 8192

static struct {
    SemaphoreHandle_t mu;
    SemaphoreHandle_t io_mu;        /* serializes target UART access */
    prog_state_t state;
    char selected_file[STORAGE_MAX_NAME];
    char file[STORAGE_MAX_NAME];
    nvs_handle_t nvs;
    char stage[32];
    char message[192];
    uint32_t percent;
    char log[LOG_BUF_SIZE];
    size_t log_len;
    bool cancel;
    bool cancelled;
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

static bool cancel_requested(void)
{
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    bool cancel = s_prog.cancel;
    xSemaphoreGive(s_prog.mu);
    return cancel;
}

void programmer_get_status(prog_status_t *status)
{
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    status->state = s_prog.state;
    status->percent = s_prog.percent;
    strlcpy(status->file, s_prog.file, sizeof(status->file));
    strlcpy(status->stage, s_prog.stage, sizeof(status->stage));
    status->cancelled = s_prog.cancelled;
    strlcpy(status->chip, s_prog.dev_valid ? s_prog.sig.device_name : "", sizeof(status->chip));
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
    for (uint32_t addr = start; addr < end_excl && !cancel_requested(); addr += blksz) {
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
    return cancel_requested() ? ESP_ERR_INVALID_STATE : ESP_OK;
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
    rl78_signature_t sig;
    result = rl78_enter_programming_mode(&sig);
    if (result != ESP_OK) {
        prog_log("目标连接失败: %s。请检查接线与供电后重试。", nack_hint());
        goto out;
    }
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    s_prog.sig = sig;
    s_prog.dev_valid = true;
    xSemaphoreGive(s_prog.mu);
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
        for (uint32_t addr = start; addr < img_end && !cancel_requested(); addr += blksz) {
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
        if (cancel_requested()) {
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
        for (uint32_t addr = start; addr < img_end && !cancel_requested(); addr += blksz) {
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
        if (cancel_requested()) {
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

    bool cancelled = cancel_requested();
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    s_prog.cancelled = cancelled;
    s_prog.cancel = false;
    xSemaphoreGive(s_prog.mu);
    if (result == ESP_OK && !cancelled) {
        prog_log("===== 任务完成 =====");
        set_prog_state(PROG_DONE, "完成", "烧录成功，目标已复位运行", 100);
    } else if (cancelled) {
        prog_log("===== 任务已取消 =====");
        set_prog_state(PROG_IDLE, "空闲", "已取消", 0);
    } else {
        prog_log("===== 任务失败: %s =====", esp_err_to_name(result));
        set_prog_state(PROG_FAILED, "失败", esp_err_to_name(result), 0);
    }

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
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    snprintf(acc.buf + acc.len, acc.cap - acc.len,
             "],\"total\":%u,\"used\":%u,\"selected\":\"%s\"}",
             (unsigned)total, (unsigned)used, s_prog.selected_file);
    xSemaphoreGive(s_prog.mu);

    esp_err_t err = send_json(req, acc.buf);
    free(acc.buf);
    return err;
}

static esp_err_t upload_file(httpd_req_t *req)
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

static esp_err_t delete_file(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME];
    if (!get_query_param(req, "name", name, sizeof(name)) || !storage_name_valid(name)) {
        return send_json_error(req, 400, "文件名无效");
    }
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    if (strcmp(name, s_prog.selected_file) == 0) {
        esp_err_t err = nvs_set_str(s_prog.nvs, "selected", "");
        if (err == ESP_OK) err = nvs_commit(s_prog.nvs);
        if (err != ESP_OK) {
            xSemaphoreGive(s_prog.mu);
            return send_json_error(req, 500, "清除固件选择失败");
        }
        s_prog.selected_file[0] = '\0';
        s_prog.file[0] = '\0';
        s_prog.state = PROG_IDLE;
        s_prog.percent = 0;
        s_prog.cancelled = false;
        s_prog.message[0] = '\0';
        strlcpy(s_prog.stage, "空闲", sizeof(s_prog.stage));
    }
    xSemaphoreGive(s_prog.mu);
    if (storage_delete(name) != ESP_OK) {
        return send_json_error(req, 404, "文件不存在");
    }
    ESP_LOGI(TAG, "deleted %s", name);
    return send_json(req, "{\"ok\":true}");
}

/* Lock order: io_mu, then mu. The worker owns io_mu while using the image/target. */
static bool try_idle_io(void)
{
    if (xSemaphoreTake(s_prog.io_mu, 0) != pdTRUE) return false;
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    bool idle = s_prog.state != PROG_RUNNING;
    xSemaphoreGive(s_prog.mu);
    if (!idle) xSemaphoreGive(s_prog.io_mu);
    return idle;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!try_idle_io()) return send_json_error(req, 500, "设备忙，请稍后重试");
    esp_err_t err = upload_file(req);
    xSemaphoreGive(s_prog.io_mu);
    return err;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    if (!try_idle_io()) return send_json_error(req, 500, "设备忙，请稍后重试");
    esp_err_t err = delete_file(req);
    xSemaphoreGive(s_prog.io_mu);
    return err;
}

/* ---------------- programmer firmware OTA ---------------- */

static esp_err_t receive_ota(httpd_req_t *req)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    const size_t header_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const size_t prefix_size = header_size + sizeof(esp_app_desc_t);
    const char *error = "固件接收中断或超时";
    int code = 400;
    uint8_t *buf = NULL;
    esp_ota_handle_t handle = 0;
    bool active = false;

    if (!partition) {
        error = "缺少 OTA 分区，请先通过串口烧录新分区表和固件";
        goto fail;
    }
    if (req->content_len < prefix_size || req->content_len > partition->size) {
        error = "固件大小无效或超出 OTA 分区";
        goto fail;
    }
    buf = malloc(4096);
    if (!buf) {
        error = "内存不足";
        code = 500;
        goto fail;
    }
    size_t received = 0;
    while (received < prefix_size) {
        int n = httpd_req_recv(req, (char *)buf + received, prefix_size - received);
        if (n <= 0) goto fail;
        received += n;
    }
    esp_app_desc_t desc;
    memcpy(&desc, buf + header_size, sizeof(desc));
    if (buf[0] != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(desc.project_name, esp_app_get_description()->project_name,
               sizeof(desc.project_name)) != 0) {
        error = "请选择本编程器的应用固件 offline_programmer.bin";
        goto fail;
    }
    if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        error = "无法开始 OTA";
        code = 500;
        goto fail;
    }
    active = true;
    size_t chunk = received;
    while (true) {
        if (esp_ota_write(handle, buf, chunk) != ESP_OK) {
            error = "固件写入失败";
            code = 500;
            goto fail;
        }
        if (received == req->content_len) break;
        size_t remaining = req->content_len - received;
        int n = httpd_req_recv(req, (char *)buf, remaining > 4096 ? 4096 : remaining);
        if (n <= 0) goto fail;
        chunk = n;
        received += n;
    }
    esp_err_t err = esp_ota_end(handle);
    active = false; /* esp_ota_end frees the handle even on validation failure. */
    if (err != ESP_OK) {
        error = "固件校验失败，请检查文件是否完整且适用于 ESP32-C3";
        goto fail;
    }
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        error = "设置启动分区失败";
        code = 500;
        goto fail;
    }
    free(buf);
    return ESP_OK;

fail:
    if (active) esp_ota_abort(handle);
    free(buf);
    send_json_error(req, code, error);
    return ESP_FAIL; /* Close the connection, including any unread request body. */
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!try_idle_io()) {
        send_json_error(req, 500, "设备忙，请稍后重试");
        return ESP_FAIL;
    }
    if (receive_ota(req) != ESP_OK) {
        xSemaphoreGive(s_prog.io_mu);
        return ESP_FAIL;
    }
    /* Keep target/file operations locked until restart, even if the client disconnects. */
    esp_err_t err = send_json(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return err;
}

/* Caller holds both io_mu and mu, as for HTTP selection. */
static esp_err_t select_file_locked(const char *name)
{
    esp_err_t err = ESP_OK;
    if (!storage_exists(name)) {
        err = ESP_ERR_NOT_FOUND;
    } else if (strcmp(name, s_prog.selected_file) != 0) {
        err = nvs_set_str(s_prog.nvs, "selected", name);
        if (err == ESP_OK) err = nvs_commit(s_prog.nvs);
    }
    if (err == ESP_OK) {
        strlcpy(s_prog.selected_file, name, sizeof(s_prog.selected_file));
        strlcpy(s_prog.file, name, sizeof(s_prog.file));
        s_prog.state = PROG_IDLE;
        s_prog.percent = 0;
        s_prog.cancelled = false;
        s_prog.message[0] = '\0';
        strlcpy(s_prog.stage, "空闲", sizeof(s_prog.stage));
    }
    return err;
}

typedef struct {
    const char *current;
    char first[STORAGE_MAX_NAME];
    char next[STORAGE_MAX_NAME];
} next_file_t;

static void find_next_file(const storage_file_info_t *info, void *ctx)
{
    next_file_t *files = ctx;
    if (!storage_name_valid(info->name)) return;
    if (!files->first[0] || strcmp(info->name, files->first) < 0)
        strlcpy(files->first, info->name, sizeof(files->first));
    if (strcmp(info->name, files->current) > 0 &&
        (!files->next[0] || strcmp(info->name, files->next) < 0))
        strlcpy(files->next, info->name, sizeof(files->next));
}

esp_err_t programmer_clear_failure(void)
{
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_prog.state == PROG_FAILED) {
        s_prog.state = PROG_IDLE;
        s_prog.percent = 0;
        s_prog.cancelled = false;
        s_prog.message[0] = '\0';
        strlcpy(s_prog.stage, "空闲", sizeof(s_prog.stage));
        err = ESP_OK;
    }
    xSemaphoreGive(s_prog.mu);
    return err;
}

esp_err_t programmer_select_next(void)
{
    if (!try_idle_io()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    next_file_t files = {.current = s_prog.selected_file};
    storage_list(find_next_file, &files);
    const char *name = files.next[0] ? files.next : files.first;
    esp_err_t err = name[0] ? select_file_locked(name) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(s_prog.mu);
    xSemaphoreGive(s_prog.io_mu);
    return err;
}

static esp_err_t select_handler(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME];
    if (!get_query_param(req, "file", name, sizeof(name)) || !storage_name_valid(name)) {
        return send_json_error(req, 400, "文件名无效");
    }
    if (!try_idle_io()) return send_json_error(req, 500, "设备忙，请稍后重试");
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    esp_err_t err = select_file_locked(name);
    xSemaphoreGive(s_prog.mu);
    xSemaphoreGive(s_prog.io_mu);
    if (err != ESP_OK) return send_json_error(req, err == ESP_ERR_NOT_FOUND ? 404 : 500,
                                             err == ESP_ERR_NOT_FOUND ? "文件不存在" : "保存固件选择失败");
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
    if (!try_idle_io()) return send_json_error(req, 500, "设备忙，请稍后重试");
    rl78_signature_t sig;
    esp_err_t err = rl78_enter_programming_mode(&sig);
    if (err != ESP_OK) {
        xSemaphoreGive(s_prog.io_mu);
        prog_log("连接目标失败: %s", nack_hint());
        return send_json_error(req, 500, "目标无响应，请检查接线与供电");
    }
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    memcpy(&s_prog.sig, &sig, sizeof(sig));
    s_prog.dev_valid = true;
    xSemaphoreGive(s_prog.mu);
    xSemaphoreGive(s_prog.io_mu);

    prog_log("目标芯片: %s (代码闪存 %" PRIu32 " KB)", sig.device_name,
             (sig.code_flash_end + 1) / 1024);

    char dev[384];
    format_device_json(dev, sizeof(dev), &sig);
    char resp[448];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"device\":{%s}}", dev);
    return send_json(req, resp);
}

static esp_err_t start_program(const char *file, bool erase, bool program, bool verify)
{
    if (file && !storage_name_valid(file)) return ESP_ERR_INVALID_ARG;
    if (!erase && !program && !verify) return ESP_ERR_INVALID_ARG;
    if (!try_idle_io()) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    const char *name = file ? file : s_prog.selected_file;
    esp_err_t err = ESP_OK;
    prog_job_t *job = NULL;
    if (!storage_exists(name)) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        job = calloc(1, sizeof(prog_job_t));
        if (!job) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        strlcpy(job->file, name, sizeof(job->file));
        job->erase = erase;
        job->program = program;
        job->verify = verify;
        strlcpy(s_prog.file, name, sizeof(s_prog.file));
        s_prog.log_len = 0;
        s_prog.log[0] = '\0';
        s_prog.cancel = false;
        s_prog.cancelled = false;
        s_prog.percent = 0;
        s_prog.message[0] = '\0';
        strlcpy(s_prog.stage, "解析文件", sizeof(s_prog.stage));
        s_prog.state = PROG_RUNNING;
        if (xTaskCreate(program_task, "prog_task", 6144, job, 5, NULL) != pdPASS) {
            free(job);
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK && (!file || s_prog.state == PROG_RUNNING)) {
        s_prog.state = PROG_FAILED;
        s_prog.percent = 0;
        s_prog.cancelled = false;
        strlcpy(s_prog.file, name, sizeof(s_prog.file));
        strlcpy(s_prog.stage, err == ESP_ERR_NOT_FOUND ? "无固件" : "失败", sizeof(s_prog.stage));
        strlcpy(s_prog.message, esp_err_to_name(err), sizeof(s_prog.message));
    }
    xSemaphoreGive(s_prog.mu);
    xSemaphoreGive(s_prog.io_mu);
    return err;
}

esp_err_t programmer_start_selected(void)
{
    return start_program(NULL, true, true, true);
}

static esp_err_t program_handler(httpd_req_t *req)
{
    char file[STORAGE_MAX_NAME], val[8];
    if (!get_query_param(req, "file", file, sizeof(file))) {
        return send_json_error(req, 400, "文件名无效");
    }
    bool erase = get_query_param(req, "erase", val, sizeof(val)) && val[0] == '1';
    bool program = get_query_param(req, "program", val, sizeof(val)) && val[0] == '1';
    bool verify = get_query_param(req, "verify", val, sizeof(val)) && val[0] == '1';
    esp_err_t err = start_program(file, erase, program, verify);
    if (err != ESP_OK) {
        return send_json_error(req, err == ESP_ERR_INVALID_ARG ? 400 :
                                   err == ESP_ERR_NOT_FOUND ? 404 : 500,
                               err == ESP_ERR_INVALID_ARG ? "文件名或操作无效" :
                               err == ESP_ERR_NOT_FOUND ? "文件不存在" :
                               err == ESP_ERR_INVALID_STATE ? "设备忙，请稍后重试" : "任务创建失败");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t cancel_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_prog.mu, portMAX_DELAY);
    if (s_prog.state == PROG_RUNNING) s_prog.cancel = true;
    xSemaphoreGive(s_prog.mu);
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

static esp_err_t info_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *keys[] = {"version", "build_date", "build_time"};
    const char *values[] = {app->version, app->date, app->time};
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, "{\"ok\":true", HTTPD_RESP_USE_STRLEN);
    for (size_t i = 0; err == ESP_OK && i < 3; i++) {
        char key[32];
        snprintf(key, sizeof(key), ",\"%s\":\"", keys[i]);
        err = httpd_resp_send_chunk(req, key, HTTPD_RESP_USE_STRLEN);
        if (err == ESP_OK) err = send_json_string_chunked(req, values[i], strlen(values[i]));
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\"", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
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

static esp_err_t load_selection(void)
{
    ESP_RETURN_ON_ERROR(nvs_open("programmer", NVS_READWRITE, &s_prog.nvs), TAG, "open selection");
    size_t len = sizeof(s_prog.selected_file);
    esp_err_t err = nvs_get_str(s_prog.nvs, "selected", s_prog.selected_file, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH) {
        s_prog.selected_file[0] = '\0';
    } else if (err != ESP_OK) {
        return err;
    }
    if (!storage_name_valid(s_prog.selected_file)) s_prog.selected_file[0] = '\0';
    strlcpy(s_prog.file, s_prog.selected_file, sizeof(s_prog.file));
    return ESP_OK;
}

/* ---------------- server setup ---------------- */

esp_err_t http_server_start(void)
{
    s_prog.mu = xSemaphoreCreateMutex();
    s_prog.io_mu = xSemaphoreCreateMutex();
    configASSERT(s_prog.mu && s_prog.io_mu);
    s_prog.state = PROG_IDLE;
    strlcpy(s_prog.stage, "空闲", sizeof(s_prog.stage));
    ESP_RETURN_ON_ERROR(load_selection(), TAG, "load selection failed");

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
        { .uri = "/api/select",  .method = HTTP_POST,   .handler = select_handler },
        { .uri = "/api/program", .method = HTTP_POST,   .handler = program_handler },
        { .uri = "/api/cancel",  .method = HTTP_POST,   .handler = cancel_handler },
        { .uri = "/api/status",  .method = HTTP_GET,    .handler = status_handler },
        { .uri = "/api/ota",     .method = HTTP_POST,   .handler = ota_handler },
        { .uri = "/api/info",    .method = HTTP_GET,    .handler = info_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register uri failed");
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
