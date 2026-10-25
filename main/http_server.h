/* HTTP management server: REST API + embedded web page. */
#pragma once

#include "esp_err.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_server_start(void);

typedef enum { PROG_IDLE, PROG_RUNNING, PROG_DONE, PROG_FAILED } prog_state_t;

typedef struct {
    prog_state_t state;
    uint32_t percent;
    char file[STORAGE_MAX_NAME];
    char stage[32];
    bool cancelled;
} prog_status_t;

/* Call after http_server_start(). Same job/status for HTTP and front panel. */
esp_err_t programmer_start_selected(void);
void programmer_get_status(prog_status_t *status);

#ifdef __cplusplus
}
#endif
