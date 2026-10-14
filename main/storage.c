#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "storage";

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: total %u KB, used %u KB",
             (unsigned)(total / 1024), (unsigned)(used / 1024));
    return ESP_OK;
}

bool storage_name_valid(const char *name)
{
    if (!name || !*name || *name == '.') {
        return false;
    }
    size_t len = strlen(name);
    if (len == 0 || len >= STORAGE_MAX_NAME) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

const char *storage_path(const char *name, char *buf, size_t buf_size)
{
    if (!storage_name_valid(name)) {
        return NULL;
    }
    int n = snprintf(buf, buf_size, "%s/%s", STORAGE_BASE_PATH, name);
    if (n < 0 || (size_t)n >= buf_size) {
        return NULL;
    }
    return buf;
}

bool storage_exists(const char *name)
{
    char path[96];
    if (!storage_path(name, path, sizeof(path))) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0;
}

int storage_list(storage_list_cb_t cb, void *ctx)
{
    DIR *dir = opendir(STORAGE_BASE_PATH);
    if (!dir) {
        return 0;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE_PATH, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        storage_file_info_t info;
        strlcpy(info.name, ent->d_name, sizeof(info.name));
        info.size = (size_t)st.st_size;
        cb(&info, ctx);
        count++;
    }
    closedir(dir);
    return count;
}

esp_err_t storage_delete(const char *name)
{
    char path[96];
    if (!storage_path(name, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

void storage_usage(size_t *total, size_t *used)
{
    size_t t = 0, u = 0;
    if (esp_littlefs_info("storage", &t, &u) != ESP_OK) {
        t = u = 0;
    }
    if (total) *total = t;
    if (used)  *used = u;
}
