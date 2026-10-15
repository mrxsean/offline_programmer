/*
 * Programming job engine shared by the HTTP API and the LCD UI.
 * Owns the job state, log buffer and the programming worker task.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "rl78_prog.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PROG_IDLE, PROG_RUNNING, PROG_DONE, PROG_FAILED } prog_state_t;

typedef enum {
    PROG_TARGET_NONE = 0,
    PROG_TARGET_RL78 = 1,
    PROG_TARGET_STM32 = 2,
} prog_target_t;

typedef struct {
    char file[STORAGE_MAX_NAME];
    bool erase;
    bool program;
    bool verify;
} prog_job_req_t;

typedef struct {
    prog_state_t state;
    char stage[32];
    char message[192];
    uint32_t percent;
    bool dev_valid;
    prog_target_t target;       /* detected target interface */
    char dev_name[24];          /* RL78 device name or STM32 family name */
    uint32_t dev_flash_kb;      /* code flash size in KB */
    uint16_t dev_id;            /* STM32 DBGMCU DEV_ID (0 for RL78) */
    rl78_signature_t sig;       /* valid when target == PROG_TARGET_RL78 */
} prog_status_t;

esp_err_t prog_job_init(void);

/* Start a job. Returns ESP_ERR_INVALID_STATE when one is already running. */
esp_err_t prog_job_start(const prog_job_req_t *req);

/* Request cancellation of the running job. */
void prog_job_cancel(void);

bool prog_job_busy(void);

/* Thread-safe snapshot of the current status (no log). */
void prog_job_snapshot(prog_status_t *out);

/* Copy the job log into buf (NUL-terminated). Returns length copied. */
size_t prog_job_log_read(char *buf, size_t cap);

/*
 * Manually enter programming mode and read the silicon signature
 * (updates the shared device info). Serialized with running jobs.
 */
esp_err_t prog_job_connect(rl78_signature_t *sig);

/* Append a line to the job log (also mirrored to ESP_LOG). */
void prog_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
