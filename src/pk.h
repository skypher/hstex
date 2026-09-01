#ifndef HSTEX_PK_H
#define HSTEX_PK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hstex_pk_glyph {
    bool present;
    int32_t tfm_width;
    int32_t dx;
    int32_t dy;
    int32_t width;
    int32_t height;
    int32_t horizontal_offset;
    int32_t vertical_offset;
    uint8_t *bitmap;
    size_t bitmap_length;
};

struct hstex_pk_font {
    uint32_t design_size;
    uint32_t checksum;
    uint32_t horizontal_pixels_per_point;
    uint32_t vertical_pixels_per_point;
    struct hstex_pk_glyph glyphs[256];
};

int hstex_pk_parse(const uint8_t *bytes, size_t length,
                   struct hstex_pk_font *font, char *error,
                   size_t error_capacity);
void hstex_pk_destroy(struct hstex_pk_font *font);

#endif
