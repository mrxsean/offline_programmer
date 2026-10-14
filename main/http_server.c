#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"

#include "storage.h"
#include "prog_job.h"

static const char *TAG = "httpd";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

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
    prog_log("文件上传: %s (%u 字节)", name, (unsigned)written);
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
    prog_log("文件删除: %s", name);
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
    if (prog_job_busy()) {
        return send_json_error(req, 500, "烧录进行中");
    }
    rl78_signature_t sig;
    esp_err_t err = prog_job_connect(&sig);
    if (err != ESP_OK) {
        return send_json_error(req, 500, "目标无响应，请检查接线与供电");
    }
    char dev[384];
    format_device_json(dev, sizeof(dev), &sig);
    char resp[448];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"device\":{%s}}", dev);
    return send_json(req, resp);
}

static esp_err_t program_handler(httpd_req_t *req)
{
    prog_job_req_t job = {0};
    char val[8];
    if (!get_query_param(req, "file", job.file, sizeof(job.file)) || !storage_name_valid(job.file)) {
        return send_json_error(req, 400, "文件名无效");
    }
    if (!storage_exists(job.file)) {
        return send_json_error(req, 404, "文件不存在");
    }
    if (get_query_param(req, "erase", val, sizeof(val)))   job.erase   = val[0] == '1';
    if (get_query_param(req, "program", val, sizeof(val))) job.program = val[0] == '1';
    if (get_query_param(req, "verify", val, sizeof(val)))  job.verify  = val[0] == '1';
    if (!job.erase && !job.program && !job.verify) {
        return send_json_error(req, 400, "未选择任何操作");
    }

    esp_err_t err = prog_job_start(&job);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, 500, "已有烧录任务进行中");
    }
    if (err != ESP_OK) {
        return send_json_error(req, 500, "任务启动失败");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t cancel_handler(httpd_req_t *req)
{
    prog_job_cancel();
    return send_json(req, "{\"ok\":true}");
}

/* ---------------- OTA (self firmware update) ---------------- */

static esp_err_t ota_info_handler(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"version\":\"%s\",\"project\":\"%s\","
             "\"date\":\"%s\",\"time\":\"%s\",\"partition\":\"%s\"}",
             desc->version, desc->project_name, desc->date, desc->time,
             running ? running->label : "?");
    return send_json(req, buf);
}

static void ota_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (prog_job_busy()) {
        return send_json_error(req, 500, "烧录进行中，无法升级");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_json_error(req, 500, "无可用 OTA 分区");
    }
    ESP_LOGI(TAG, "OTA begin -> partition %s", part->label);

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        return send_json_error(req, 500, "OTA 初始化失败");
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(h);
        return send_json_error(req, 500, "内存不足");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, (char *)buf, chunk);
        if (r <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) {
            break;
        }
        remaining -= r;
    }
    free(buf);

    if (err == ESP_OK) {
        err = esp_ota_end(h);   /* validates the image (magic, chip id, sha256) */
    } else {
        esp_ota_abort(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        prog_log("OTA 升级失败: %s", esp_err_to_name(err));
        return send_json_error(req, 500, "固件校验失败（非有效固件镜像）");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return send_json_error(req, 500, "设置启动分区失败");
    }

    ESP_LOGI(TAG, "OTA success, rebooting into %s", part->label);
    prog_log("OTA 升级完成，设备重启中 ...");
    send_json(req, "{\"ok\":true}");
    xTaskCreate(ota_restart_task, "ota_rst", 2048, NULL, 5, NULL);
    return ESP_OK;
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

    prog_status_t snap;
    prog_job_snapshot(&snap);

    size_t log_len = 0;
    char *log_copy = malloc(8192);
    if (log_copy) {
        log_len = prog_job_log_read(log_copy, 8192);
    }

    char head[320];
    int hl = snprintf(head, sizeof(head),
                      "{\"state\":\"%s\",\"stage\":\"%s\",\"message\":\"%s\",\"percent\":%" PRIu32 ",",
                      state_str(snap.state), snap.stage, snap.message, snap.percent);
    esp_err_t err = httpd_resp_send_chunk(req, head, hl);
    if (err == ESP_OK && snap.dev_valid) {
        char dev[384];
        format_device_json(dev, sizeof(dev), &snap.sig);
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
        { .uri = "/api/ota",     .method = HTTP_POST,   .handler = ota_handler },
        { .uri = "/api/ota/info",.method = HTTP_GET,    .handler = ota_info_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register uri failed");
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
