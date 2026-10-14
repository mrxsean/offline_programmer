/*
 * Firmware image parser: Intel HEX, Motorola S-Record and raw binary.
 * Parsed content is kept as a list of address-sorted data segments.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_segment {
    uint32_t addr;
    uint32_t len;
    uint8_t *data;
    struct img_segment *next;
} img_segment_t;

typedef struct {
    img_segment_t *head;
    img_segment_t *tail;
    size_t seg_count;
    size_t total_data;
} image_t;

/*
 * Parse a firmware file (format detected by content: ':' Intel HEX,
 * 'S' S-Record, otherwise raw binary loaded at address 0).
 */
esp_err_t image_parse_file(const char *path, image_t *img);

void image_free(image_t *img);

/* Address extent of the image. Returns false when the image is empty. */
bool image_extent(const image_t *img, uint32_t *min_addr, uint32_t *max_end);

/*
 * Build a flash-block image: fill `buf` (blksz bytes) with 0xFF and copy all
 * segment data overlapping [addr, addr+blksz) into it.
 * Returns true if the block contains any actual data.
 */
bool image_build_block(const image_t *img, uint32_t addr, uint8_t *buf, uint32_t blksz);

#ifdef __cplusplus
}
#endif
