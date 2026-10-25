#include "../main/http_server.c"

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
int httpd_req_recv(httpd_req_t *r, char *dst, size_t len) { assert(false); return -1; }

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
    puts("programmer: persistence, invalid input, busy guards, full job, errors and cancellation PASS");
}
