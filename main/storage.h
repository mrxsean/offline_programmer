/*
 * Firmware file storage on the internal flash (LittleFS, mounted at /storage).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_BASE_PATH   "/storage"
#define STORAGE_MAX_NAME    48

typedef struct {
    char name[STORAGE_MAX_NAME];
    size_t size;
} storage_file_info_t;

typedef void (*storage_list_cb_t)(const storage_file_info_t *info, void *ctx);

/* Mount the LittleFS partition (formats on first boot). */
esp_err_t storage_init(void);

/* Validate a user supplied file name (charset, length, no path traversal). */
bool storage_name_valid(const char *name);

/* Build the full path for a file name. Returns NULL when invalid. */
const char *storage_path(const char *name, char *buf, size_t buf_size);

bool storage_exists(const char *name);

/* Iterate over stored files, cb is invoked for each. Returns file count. */
int storage_list(storage_list_cb_t cb, void *ctx);

esp_err_t storage_delete(const char *name);

/* Total / used bytes of the storage partition. */
void storage_usage(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif
