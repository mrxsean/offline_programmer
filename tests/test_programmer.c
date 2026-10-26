#include "../main/http_server.c"

const uint8_t index_html_start[] = {0};
const uint8_t index_html_end[] = {0};
esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used) {
    *total = 0x1e0000; *used = 0; return ESP_OK;
}

static int state_lock, io_lock, task_count;
static bool task_fail, commit_fail, file_exists = true, target_fail, verify_fail, cancel_verify;
static char saved[STORAGE_MAX_NAME], pending[STORAGE_MAX_NAME];
static void (*worker)(void *);
static void *worker_arg;
static unsigned erased, programmed, verified, reset_count;

int xSemaphoreTake(SemaphoreHandle_t mu, TickType_t wait) {
    if (*mu) { assert(wait == 0); return 0; }
    *mu = 1; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t mu) { assert(*mu); *mu = 0; return pdTRUE; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    if (task_fail) return 0;
    assert(!worker_arg); worker = task; worker_arg = arg; task_count++; return pdPASS;
}
void vTaskDelete(void *task) { }

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { *h = 1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *dst, size_t *len) {
    if (!saved[0]) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < strlen(saved) + 1) return ESP_ERR_NVS_INVALID_LENGTH;
    strlcpy(dst, saved, *len); return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    strlcpy(pending, value, sizeof(pending)); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    if (commit_fail) return ESP_FAIL;
    strlcpy(saved, pending, sizeof(saved)); return ESP_OK;
}
bool __wrap_storage_exists(const char *name) {
    return file_exists && (strcmp(name, "App_v1.hex") == 0 || strcmp(name, "Other.bin") == 0);
}
int __wrap_storage_list(storage_list_cb_t cb, void *ctx) {
    if (!file_exists) return 0;
    storage_file_info_t info = {.name = "Other.bin"};
    cb(&info, ctx);
    strlcpy(info.name, "App_v1.hex", sizeof(info.name));
    cb(&info, ctx);
    return 2;
}
esp_err_t __wrap_storage_delete(const char *name) {
    if (!__wrap_storage_exists(name)) return ESP_ERR_NOT_FOUND;
    file_exists = false; return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { r->status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *s, ssize_t len) {
    if (len == HTTPD_RESP_USE_STRLEN) len = strlen(s);
    assert((size_t)len < sizeof(r->response));
    memcpy(r->response, s, len); r->response[len] = 0; return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *s, ssize_t len) {
    if (!s) return ESP_OK;
    if (len == HTTPD_RESP_USE_STRLEN) len = strlen(s);
    size_t offset = strlen(r->response);
    assert(offset + len < sizeof(r->response));
    memcpy(r->response + offset, s, len); r->response[offset + len] = 0;
    return ESP_OK;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *dst, size_t len) {
    if (strlen(r->query) >= len) return ESP_ERR_INVALID_SIZE;
    strlcpy(dst, r->query, len); return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *q, const char *key, char *dst, size_t len) {
    char prefix[32]; snprintf(prefix, sizeof(prefix), "%s=", key);
    const char *value = strstr(q, prefix);
    if (!value) return ESP_ERR_NOT_FOUND;
    value += strlen(prefix); size_t n = strcspn(value, "&");
    if (n >= len) return ESP_ERR_INVALID_SIZE;
    memcpy(dst, value, n); dst[n] = 0; return ESP_OK;
}
static uint8_t ota_image[9000];
static size_t ota_rx, ota_written, ota_fragment, ota_disconnect;
static int ota_fail, ota_aborts, ota_ends, ota_boots, ota_restarts, ota_begins;
static bool ota_partition_missing;
static const esp_partition_t ota_partition = {.size = 0x100000};
static const esp_app_desc_t ota_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD, .project_name = "offline_programmer",
    .version = "1.0.0", .date = "Oct 26 2026", .time = "18:12:34"
};
int httpd_req_recv(httpd_req_t *r, char *dst, size_t len) {
    assert(io_lock && programmer_start_selected() == ESP_ERR_INVALID_STATE);
    if (ota_rx >= ota_disconnect) return -1;
    if (len > ota_fragment) len = ota_fragment;
    if (len > ota_disconnect - ota_rx) len = ota_disconnect - ota_rx;
    assert(ota_rx + len <= sizeof(ota_image));
    memcpy(dst, ota_image + ota_rx, len); ota_rx += len; return len;
}
const esp_app_desc_t *esp_app_get_description(void) { return &ota_desc; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {
    return ota_partition_missing ? NULL : &ota_partition;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h) {
    assert(p == &ota_partition && size == OTA_WITH_SEQUENTIAL_WRITES);
    ota_begins++; *h = 1; return ota_fail == 1 ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t len) {
    assert(h == 1 && ota_written + len <= sizeof(ota_image));
    assert(memcmp(data, ota_image + ota_written, len) == 0);
    ota_written += len; return ota_fail == 2 ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) {
    assert(h == 1); ota_ends++; return ota_fail == 3 ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h) { assert(h == 1); ota_aborts++; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {
    assert(ota_ends == 1 && ota_fail != 3); ota_boots++;
    return ota_fail == 4 ? ESP_FAIL : ESP_OK;
}
void vTaskDelay(TickType_t delay) { assert(delay == 1500 && io_lock); }
void esp_restart(void) {
    assert(io_lock && programmer_start_selected() == ESP_ERR_INVALID_STATE);
    ota_restarts++;
}

uint8_t rl78_last_nack(void) { return 0; }
const char *rl78_status_str(uint8_t status) { return "NACK"; }
esp_err_t rl78_enter_programming_mode(rl78_signature_t *sig) {
    memset(sig, 0, sizeof(*sig)); sig->code_flash_end = 4095;
    return target_fail ? ESP_FAIL : ESP_OK;
}
void rl78_reset_target(void) { reset_count++; }
esp_err_t rl78_block_blank_check(uint32_t a, uint32_t n, bool *blank) { *blank = false; return ESP_OK; }
esp_err_t rl78_block_erase(uint32_t a) { erased++; return ESP_OK; }
esp_err_t rl78_program(uint32_t a, const uint8_t *data, uint32_t n) { programmed++; return ESP_OK; }
esp_err_t rl78_verify(uint32_t a, const uint8_t *data, uint32_t n) {
    verified++;
    if (cancel_verify) { httpd_req_t req = {0}; cancel_handler(&req); }
    return verify_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t image_parse_file(const char *path, image_t *img) {
    memset(img, 0, sizeof(*img)); img->seg_count = 1; img->total_data = 1024; return ESP_OK;
}
void image_free(image_t *img) { }
bool image_extent(const image_t *img, uint32_t *start, uint32_t *end) { *start = 0; *end = 1024; return true; }
bool image_build_block(const image_t *img, uint32_t addr, uint8_t *buf, uint32_t len) {
    memset(buf, 0x55, len); return true;
}

static void boot(void) {
    assert(!worker_arg);
    memset(&s_prog, 0, sizeof(s_prog));
    state_lock = io_lock = 0;
    s_prog.mu = &state_lock; s_prog.io_mu = &io_lock;
    assert(load_selection() == ESP_OK);
}
static void run_worker(void) {
    void *arg = worker_arg; worker_arg = NULL;
    assert(arg); worker(arg);
    assert(!io_lock && !state_lock);
}
static void select_file(const char *query, bool ok) {
    httpd_req_t req = {.query = query};
    assert(select_handler(&req) == ESP_OK);
    assert(strstr(req.response, ok ? "\"ok\":true" : "\"ok\":false"));
}

static void reset_ota(void) {
    boot();
    memset(ota_image, 0x5a, sizeof(ota_image));
    ota_image[0] = ESP_IMAGE_HEADER_MAGIC;
    memcpy(ota_image + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           &ota_desc, sizeof(ota_desc));
    ota_rx = ota_written = 0;
    ota_fragment = 4096;
    ota_disconnect = sizeof(ota_image);
    ota_fail = ota_aborts = ota_ends = ota_boots = ota_restarts = ota_begins = 0;
    ota_partition_missing = false;
}
static void check_ota_failure(int size) {
    httpd_req_t req = {.content_len = size};
    assert(ota_handler(&req) == ESP_FAIL);
    assert(strstr(req.response, "\"ok\":false"));
    assert(!io_lock && !state_lock && !ota_restarts);
}
static void test_ota(void) {
    reset_ota(); check_ota_failure(0); assert(!ota_begins);
    reset_ota(); check_ota_failure(287); assert(!ota_begins);
    reset_ota(); check_ota_failure(ota_partition.size + 1); assert(!ota_begins);
    reset_ota(); ota_partition_missing = true; check_ota_failure(sizeof(ota_image));
    reset_ota(); ota_image[0] = 0; check_ota_failure(sizeof(ota_image)); assert(!ota_begins);
    reset_ota(); ota_image[32] = 0; check_ota_failure(sizeof(ota_image)); assert(!ota_begins);
    reset_ota(); ota_image[32 + offsetof(esp_app_desc_t, project_name)] = 'X'; check_ota_failure(sizeof(ota_image)); assert(!ota_begins);
    reset_ota(); ota_disconnect = 20; check_ota_failure(sizeof(ota_image)); assert(!ota_begins);
    reset_ota(); ota_disconnect = 500; check_ota_failure(sizeof(ota_image));
    assert(ota_aborts == 1 && !ota_ends && !ota_boots);
    for (int failure = 1; failure <= 4; failure++) {
        reset_ota(); ota_fail = failure; check_ota_failure(sizeof(ota_image));
        assert(ota_aborts == (failure == 2));
        assert(ota_boots == (failure == 4));
    }
    reset_ota(); s_prog.state = PROG_RUNNING;
    check_ota_failure(sizeof(ota_image)); assert(!ota_begins && !ota_rx);
    reset_ota(); io_lock = 1;
    httpd_req_t busy = {.content_len = sizeof(ota_image)};
    assert(ota_handler(&busy) == ESP_FAIL && io_lock && !ota_begins);
    io_lock = 0;
    for (unsigned fragment = 1; fragment <= 4096; fragment *= 8) {
        reset_ota(); ota_fragment = fragment;
        httpd_req_t req = {.content_len = sizeof(ota_image)};
        assert(ota_handler(&req) == ESP_OK);
        assert(strstr(req.response, "\"ok\":true"));
        assert(ota_written == sizeof(ota_image) && ota_ends == 1 && ota_boots == 1);
        assert(!ota_aborts && ota_restarts == 1 && io_lock);
        io_lock = 0;
    }
    puts("OTA: sizes, headers, project, disconnects, API failures, fragmented stream and busy guards PASS");
}

int main(void) {
    boot();
    assert(programmer_start_selected() == ESP_ERR_NOT_FOUND);
    assert(s_prog.state == PROG_FAILED && task_count == 0);
    select_file("file=../bad.hex", false);
    select_file("file=missing.hex", false);
    select_file("file=App_v1.hex", true);
    assert(strcmp(saved, "App_v1.hex") == 0 && s_prog.state == PROG_IDLE);
    boot();
    prog_status_t status;
    programmer_get_status(&status);
    assert(strcmp(status.file, "App_v1.hex") == 0);
    commit_fail = true;
    select_file("file=Other.bin", false);
    assert(strcmp(s_prog.selected_file, "App_v1.hex") == 0);
    commit_fail = false;
    boot(); assert(strcmp(s_prog.selected_file, "App_v1.hex") == 0);

    /* Busy file upload/target session, then duplicate web/button requests. */
    io_lock = 1;
    assert(programmer_start_selected() == ESP_ERR_INVALID_STATE);
    io_lock = 0;
    assert(programmer_start_selected() == ESP_OK);
    prog_job_t *job = worker_arg;
    assert(job->erase && job->program && job->verify);
    assert(strcmp(job->file, "App_v1.hex") == 0);
    assert(programmer_start_selected() == ESP_ERR_INVALID_STATE);
    assert(start_program("Other.bin", true, true, true) == ESP_ERR_INVALID_STATE);
    assert(task_count == 1);
    select_file("file=Other.bin", false);
    httpd_req_t req = {.query = "name=App_v1.hex"};
    delete_handler(&req); assert(strstr(req.response, "\"ok\":false") && file_exists);
    upload_handler(&req); assert(strstr(req.response, "\"ok\":false"));
    connect_handler(&req); assert(strstr(req.response, "\"ok\":false"));
    run_worker();
    programmer_get_status(&status);
    assert(status.state == PROG_DONE && status.percent == 100);
    assert(erased == 1 && programmed == 1 && verified == 1 && reset_count == 1);

    /* Target/verify errors and cancellation must never report success. */
    target_fail = true;
    assert(programmer_start_selected() == ESP_OK); run_worker();
    assert(s_prog.state == PROG_FAILED && s_prog.percent == 0);
    target_fail = false; verify_fail = true;
    assert(programmer_start_selected() == ESP_OK); run_worker();
    assert(s_prog.state == PROG_FAILED);
    verify_fail = false; cancel_verify = true;
    assert(programmer_start_selected() == ESP_OK); run_worker();
    assert(s_prog.state == PROG_IDLE && s_prog.cancelled);
    cancel_verify = false;

    task_fail = true;
    assert(programmer_start_selected() == ESP_ERR_NO_MEM);
    assert(s_prog.state == PROG_FAILED && !io_lock && !state_lock);
    task_fail = false;
    assert(start_program("../invalid", true, true, true) == ESP_ERR_INVALID_ARG);
    assert(start_program("App_v1.hex", false, false, false) == ESP_ERR_INVALID_ARG);
    file_exists = false;
    assert(programmer_start_selected() == ESP_ERR_NOT_FOUND);
    assert(s_prog.state == PROG_FAILED && strcmp(s_prog.stage, "无固件") == 0);
    file_exists = true;
    commit_fail = true;
    delete_handler(&req); assert(file_exists && saved[0]);
    commit_fail = false;
    delete_handler(&req); assert(!file_exists && !saved[0]);
    boot(); assert(!s_prog.selected_file[0]);
    s_prog.state = PROG_FAILED;
    s_prog.percent = 33;
    strlcpy(s_prog.message, "failure", sizeof(s_prog.message));
    char failed_file[STORAGE_MAX_NAME];
    strlcpy(failed_file, s_prog.file, sizeof(failed_file));
    int before_tasks = task_count;
    assert(programmer_clear_failure() == ESP_OK);
    assert(s_prog.state == PROG_IDLE && s_prog.percent == 0 && !s_prog.message[0]);
    assert(strcmp(s_prog.file, failed_file) == 0 && task_count == before_tasks);
    assert(programmer_clear_failure() == ESP_ERR_INVALID_STATE);
    s_prog.state = PROG_RUNNING;
    assert(programmer_clear_failure() == ESP_ERR_INVALID_STATE);
    assert(s_prog.state == PROG_RUNNING);
    s_prog.state = PROG_IDLE;
    file_exists = true;
    assert(programmer_select_next() == ESP_OK);
    assert(strcmp(s_prog.file, "App_v1.hex") == 0);
    assert(programmer_select_next() == ESP_OK);
    assert(strcmp(s_prog.file, "Other.bin") == 0 && strcmp(saved, "Other.bin") == 0);
    commit_fail = true;
    assert(programmer_select_next() == ESP_FAIL);
    assert(strcmp(s_prog.file, "Other.bin") == 0);
    commit_fail = false;
    assert(programmer_select_next() == ESP_OK);
    assert(strcmp(s_prog.file, "App_v1.hex") == 0);
    s_prog.state = PROG_RUNNING;
    assert(programmer_select_next() == ESP_ERR_INVALID_STATE);
    s_prog.state = PROG_IDLE;
    io_lock = 1;
    assert(programmer_select_next() == ESP_ERR_INVALID_STATE);
    io_lock = 0;
    file_exists = false;
    assert(programmer_select_next() == ESP_ERR_NOT_FOUND);
    assert(!state_lock && !io_lock);
    s_prog.dev_valid = true;
    strlcpy(s_prog.sig.device_name, "R5F100LE", sizeof(s_prog.sig.device_name));
    prog_status_t panel_status;
    programmer_get_status(&panel_status);
    assert(strcmp(panel_status.chip, "R5F100LE") == 0);
    test_ota();
    httpd_req_t info = {0};
    assert(info_handler(&info) == ESP_OK);
    assert(strcmp(info.response, "{\"ok\":true,\"version\":\"1.0.0\","
                  "\"build_date\":\"Oct 26 2026\",\"build_time\":\"18:12:34\"}") == 0);
    puts("programmer: persistence, invalid input, busy guards, full job, errors and cancellation PASS");
}
