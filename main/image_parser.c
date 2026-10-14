#include "image_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "image";

void image_free(image_t *img)
{
    img_segment_t *seg = img->head;
    while (seg) {
        img_segment_t *next = seg->next;
        free(seg->data);
        free(seg);
        seg = next;
    }
    memset(img, 0, sizeof(*img));
}

static esp_err_t image_append(image_t *img, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    /* Merge with the tail segment when contiguous (the common case) */
    img_segment_t *tail = img->tail;
    if (tail && addr == tail->addr + tail->len) {
        uint8_t *nd = realloc(tail->data, tail->len + len);
        if (!nd) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(nd + tail->len, data, len);
        tail->data = nd;
        tail->len += len;
        img->total_data += len;
        return ESP_OK;
    }
    img_segment_t *seg = calloc(1, sizeof(*seg));
    if (!seg) {
        return ESP_ERR_NO_MEM;
    }
    seg->data = malloc(len);
    if (!seg->data) {
        free(seg);
        return ESP_ERR_NO_MEM;
    }
    memcpy(seg->data, data, len);
    seg->addr = addr;
    seg->len = len;
    if (!img->head) {
        img->head = seg;
    } else {
        img->tail->next = seg;
    }
    img->tail = seg;
    img->seg_count++;
    img->total_data += len;
    return ESP_OK;
}

bool image_extent(const image_t *img, uint32_t *min_addr, uint32_t *max_end)
{
    if (!img->head) {
        return false;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const img_segment_t *s = img->head; s; s = s->next) {
        if (s->addr < lo) {
            lo = s->addr;
        }
        if (s->addr + s->len > hi) {
            hi = s->addr + s->len;
        }
    }
    *min_addr = lo;
    *max_end = hi;
    return true;
}

bool image_build_block(const image_t *img, uint32_t addr, uint8_t *buf, uint32_t blksz)
{
    memset(buf, 0xFF, blksz);
    bool any = false;
    uint32_t blk_end = addr + blksz;
    for (const img_segment_t *s = img->head; s; s = s->next) {
        uint32_t seg_end = s->addr + s->len;
        if (seg_end <= addr || s->addr >= blk_end) {
            continue;
        }
        uint32_t lo = s->addr > addr ? s->addr : addr;
        uint32_t hi = seg_end < blk_end ? seg_end : blk_end;
        memcpy(buf + (lo - addr), s->data + (lo - s->addr), hi - lo);
        any = true;
    }
    return any;
}

/* ---------------- Intel HEX ---------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex_byte(const char *p)
{
    int hi = hex_nibble(p[0]);
    int lo = hex_nibble(p[1]);
    if (hi < 0 || lo < 0) {
        return -1;
    }
    return (hi << 4) | lo;
}

static esp_err_t parse_intel_hex(FILE *f, image_t *img)
{
    char line[640];
    uint32_t base = 0;
    uint32_t line_no = 0;
    bool eof = false;

    while (!eof && fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;
        if (*p != ':') {
            ESP_LOGE(TAG, "hex: bad line %lu", (unsigned long)line_no);
            return ESP_ERR_INVALID_ARG;
        }
        p++;
        int len = hex_byte(p);
        int addr_hi = hex_byte(p + 2);
        int addr_lo = hex_byte(p + 4);
        int type = hex_byte(p + 6);
        if (len < 0 || addr_hi < 0 || addr_lo < 0 || type < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t sum = (uint8_t)len + (uint8_t)addr_hi + (uint8_t)addr_lo + (uint8_t)type;
        uint8_t data[256];
        for (int i = 0; i < len; i++) {
            int b = hex_byte(p + 8 + i * 2);
            if (b < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            data[i] = (uint8_t)b;
            sum += (uint8_t)b;
        }
        int ck = hex_byte(p + 8 + len * 2);
        if (ck < 0 || (uint8_t)(sum + (uint8_t)ck) != 0) {
            ESP_LOGE(TAG, "hex: checksum error line %lu", (unsigned long)line_no);
            return ESP_ERR_INVALID_CRC;
        }
        uint32_t addr = ((uint32_t)addr_hi << 8) | (uint32_t)addr_lo;
        switch (type) {
        case 0x00: /* data */
            ESP_RETURN_ON_ERROR(image_append(img, base + addr, data, len), TAG, "hex: append failed");
            break;
        case 0x01: /* EOF */
            eof = true;
            break;
        case 0x02: /* extended segment address */
            if (len == 2) {
                base = ((uint32_t)data[0] << 8 | data[1]) << 4;
            }
            break;
        case 0x04: /* extended linear address */
            if (len == 2) {
                base = ((uint32_t)data[0] << 8 | data[1]) << 16;
            }
            break;
        case 0x03: /* start segment address */
        case 0x05: /* start linear address */
            break;  /* not needed for flashing */
        default:
            ESP_LOGW(TAG, "hex: ignore record type %d", type);
            break;
        }
    }
    return img->head ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ---------------- Motorola S-Record ---------------- */

static esp_err_t parse_srec(FILE *f, image_t *img)
{
    char line[640];
    uint32_t line_no = 0;
    bool eof = false;

    while (!eof && fgets(line, sizeof(line), f)) {
        line_no++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;
        if (p[0] != 'S') {
            ESP_LOGE(TAG, "srec: bad line %lu", (unsigned long)line_no);
            return ESP_ERR_INVALID_ARG;
        }
        int type = hex_nibble(p[1]);
        if (type < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        int count = hex_byte(p + 2);
        if (count < 1) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t rec[260];
        uint8_t sum = (uint8_t)count;
        for (int i = 0; i < count; i++) {
            int b = hex_byte(p + 4 + i * 2);
            if (b < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            rec[i] = (uint8_t)b;
            sum += (uint8_t)b;
        }
        if (sum != 0xFF) {
            ESP_LOGE(TAG, "srec: checksum error line %lu", (unsigned long)line_no);
            return ESP_ERR_INVALID_CRC;
        }
        int addr_len, data_len;
        switch (type) {
        case 1: addr_len = 2; break;
        case 2: addr_len = 3; break;
        case 3: addr_len = 4; break;
        case 0: /* header */
        case 5: /* record count */
        case 6:
            continue;
        case 7: case 8: case 9: /* termination */
            eof = true;
            continue;
        default:
            ESP_LOGW(TAG, "srec: ignore record S%d", type);
            continue;
        }
        data_len = count - addr_len - 1; /* minus checksum byte */
        if (data_len < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t addr = 0;
        for (int i = 0; i < addr_len; i++) {
            addr = (addr << 8) | rec[i];
        }
        ESP_RETURN_ON_ERROR(image_append(img, addr, rec + addr_len, data_len), TAG, "srec: append failed");
    }
    return img->head ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ---------------- raw binary ---------------- */

static esp_err_t parse_binary(FILE *f, image_t *img)
{
    uint8_t buf[4096];
    uint32_t addr = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ESP_RETURN_ON_ERROR(image_append(img, addr, buf, n), TAG, "bin: append failed");
        addr += n;
    }
    return img->head ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ---------------- entry point ---------------- */

esp_err_t image_parse_file(const char *path, image_t *img)
{
    memset(img, 0, sizeof(*img));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Detect format by the first non-space character */
    int c;
    do {
        c = fgetc(f);
    } while (c != EOF && isspace(c));
    rewind(f);

    esp_err_t err;
    if (c == ':') {
        err = parse_intel_hex(f, img);
    } else if (c == 'S') {
        err = parse_srec(f, img);
    } else {
        err = parse_binary(f, img);
    }
    fclose(f);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse %s failed: %s", path, esp_err_to_name(err));
        image_free(img);
        return err;
    }

    uint32_t lo, hi;
    if (image_extent(img, &lo, &hi)) {
        ESP_LOGI(TAG, "%s: %u segments, %u bytes, range 0x%06" PRIX32 "..0x%06" PRIX32,
                 path, (unsigned)img->seg_count, (unsigned)img->total_data, lo, hi - 1);
    }
    return ESP_OK;
}
