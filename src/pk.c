#include "pk.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HSTEX_PK_POST = 245,
    HSTEX_PK_NO_OP = 246,
    HSTEX_PK_PRE = 247,
    HSTEX_PK_ID = 89,
};

struct pk_reader {
    const uint8_t *bytes;
    size_t length;
    size_t position;
};

struct pk_nybbles {
    const uint8_t *bytes;
    size_t length;
    size_t position;
};

static int pk_error(char *error, size_t capacity, const char *message)
{
    if (error != NULL && capacity != 0U) {
        (void)snprintf(error, capacity, "%s", message);
    }
    return -1;
}

static int pk_error_unsigned(char *error, size_t capacity, const char *message,
                             uint32_t value)
{
    if (error != NULL && capacity != 0U) {
        (void)snprintf(error, capacity, "%s%" PRIu32, message, value);
    }
    return -1;
}

static int pk_unsigned(struct pk_reader *reader, size_t count, uint32_t *value,
                       char *error, size_t error_capacity)
{
    if (count == 0U || count > 4U || count > reader->length - reader->position) {
        return pk_error(error, error_capacity, "truncated PK file");
    }
    uint32_t result = 0U;
    for (size_t index = 0U; index < count; ++index) {
        result = result * UINT32_C(256) +
                 (uint32_t)reader->bytes[reader->position++];
    }
    *value = result;
    return 0;
}

static int32_t pk_signed_value(uint32_t value, size_t count)
{
    uint32_t sign = count == 4U ? UINT32_C(0x80000000)
                                : UINT32_C(1) << (count * 8U - 1U);
    if ((value & sign) == 0U) {
        return (int32_t)value;
    }
    uint32_t mask = count == 4U ? UINT32_MAX
                                : (UINT32_C(1) << (count * 8U)) - 1U;
    return -1 - (int32_t)(mask - value);
}

static int pk_signed(struct pk_reader *reader, size_t count, int32_t *value,
                     char *error, size_t error_capacity)
{
    uint32_t encoded = 0U;
    if (pk_unsigned(reader, count, &encoded, error, error_capacity) != 0) {
        return -1;
    }
    *value = pk_signed_value(encoded, count);
    return 0;
}

static int pk_skip(struct pk_reader *reader, size_t count, char *error,
                   size_t error_capacity)
{
    if (count > reader->length - reader->position) {
        return pk_error(error, error_capacity, "truncated PK file");
    }
    reader->position += count;
    return 0;
}

static int pk_nybble(struct pk_nybbles *reader, uint8_t *value, char *error,
                     size_t error_capacity)
{
    if (reader->position / 2U >= reader->length) {
        return pk_error(error, error_capacity, "truncated PK raster");
    }
    uint8_t byte = reader->bytes[reader->position / 2U];
    *value = (reader->position % 2U) == 0U ? (uint8_t)(byte >> 4U)
                                                : (uint8_t)(byte & 15U);
    ++reader->position;
    return 0;
}

static int pk_packed_number(struct pk_nybbles *reader, uint8_t dynamic,
                            uint32_t *repeat, bool allow_repeat,
                            uint32_t *value, char *error,
                            size_t error_capacity)
{
    uint8_t first = 0U;
    if (pk_nybble(reader, &first, error, error_capacity) != 0) {
        return -1;
    }
    if (first == 0U) {
        size_t zeros = 0U;
        uint8_t digit = 0U;
        do {
            if (pk_nybble(reader, &digit, error, error_capacity) != 0) {
                return -1;
            }
            ++zeros;
        } while (digit == 0U);
        uint64_t result = (uint64_t)digit;
        while (zeros != 0U) {
            if (pk_nybble(reader, &digit, error, error_capacity) != 0) {
                return -1;
            }
            if (result > (UINT32_MAX - (uint64_t)digit) / UINT64_C(16)) {
                return pk_error(error, error_capacity,
                                "PK packed number is too large");
            }
            result = result * UINT64_C(16) + (uint64_t)digit;
            --zeros;
        }
        uint64_t base = (uint64_t)(13U - dynamic) * UINT64_C(16) +
                        (uint64_t)dynamic;
        if (result < UINT64_C(15) ||
            result - UINT64_C(15) + base > UINT32_MAX) {
            return pk_error(error, error_capacity,
                            "PK packed number is out of range");
        }
        *value = (uint32_t)(result - UINT64_C(15) + base);
        return 0;
    }
    if (first <= dynamic) {
        *value = (uint32_t)first;
        return 0;
    }
    if (first < 14U) {
        uint8_t second = 0U;
        if (pk_nybble(reader, &second, error, error_capacity) != 0) {
            return -1;
        }
        *value = (uint32_t)(first - dynamic - 1U) * UINT32_C(16) +
                 (uint32_t)second + (uint32_t)dynamic + UINT32_C(1);
        return 0;
    }
    if (!allow_repeat || *repeat != 0U) {
        return pk_error(error, error_capacity,
                        "second PK repeat count in one row");
    }
    uint32_t repeated = 1U;
    if (first == 14U) {
        uint32_t nested_repeat = 0U;
        if (pk_packed_number(reader, dynamic, &nested_repeat, false, &repeated,
                             error, error_capacity) != 0) {
            return -1;
        }
    }
    *repeat = repeated;
    return pk_packed_number(reader, dynamic, repeat, false, value, error,
                            error_capacity);
}

static void pk_set_black(uint8_t *bitmap, size_t row_bytes, size_t row,
                         size_t column)
{
    bitmap[row * row_bytes + column / 8U] |=
        (uint8_t)(UINT8_C(0x80) >> (column % 8U));
}

static int pk_decode_raw(const uint8_t *raster, size_t raster_length,
                         struct hstex_pk_glyph *glyph, char *error,
                         size_t error_capacity)
{
    size_t width = (size_t)glyph->width;
    size_t height = (size_t)glyph->height;
    if (height != 0U && width > SIZE_MAX / height) {
        return pk_error(error, error_capacity, "PK raster is too large");
    }
    size_t bits = width * height;
    if (raster_length > SIZE_MAX / 8U || bits > raster_length * 8U) {
        return pk_error(error, error_capacity, "truncated PK bitmap raster");
    }
    size_t row_bytes = (width + 7U) / 8U;
    if (height != 0U && row_bytes > SIZE_MAX / height) {
        return pk_error(error, error_capacity, "PK raster is too large");
    }
    glyph->bitmap_length = row_bytes * height;
    glyph->bitmap = calloc(glyph->bitmap_length == 0U ? 1U
                                                      : glyph->bitmap_length,
                           1U);
    if (glyph->bitmap == NULL) {
        return pk_error(error, error_capacity, "PK raster allocation failed");
    }
    for (size_t bit = 0U; bit < bits; ++bit) {
        if ((raster[bit / 8U] &
             (uint8_t)(UINT8_C(0x80) >> (bit % 8U))) != 0U) {
            pk_set_black(glyph->bitmap, row_bytes, bit / width, bit % width);
        }
    }
    return 0;
}

static int pk_decode_runs(const uint8_t *raster, size_t raster_length,
                          uint8_t dynamic, bool black,
                          struct hstex_pk_glyph *glyph, char *error,
                          size_t error_capacity)
{
    size_t width = (size_t)glyph->width;
    size_t height = (size_t)glyph->height;
    size_t row_bytes = (width + 7U) / 8U;
    if (height != 0U && row_bytes > SIZE_MAX / height) {
        return pk_error(error, error_capacity, "PK raster is too large");
    }
    glyph->bitmap_length = row_bytes * height;
    glyph->bitmap = calloc(glyph->bitmap_length == 0U ? 1U
                                                      : glyph->bitmap_length,
                           1U);
    if (glyph->bitmap == NULL) {
        return pk_error(error, error_capacity, "PK raster allocation failed");
    }
    if (width == 0U || height == 0U) {
        return 0;
    }

    struct pk_nybbles nybbles = {raster, raster_length, 0U};
    size_t row = 0U;
    size_t column = 0U;
    uint32_t repeat = 0U;
    while (row < height) {
        uint32_t run = 0U;
        if (pk_packed_number(&nybbles, dynamic, &repeat, true, &run, error,
                             error_capacity) != 0 ||
            run == 0U) {
            return run == 0U
                       ? pk_error(error, error_capacity,
                                  "zero-length PK raster run")
                       : -1;
        }
        size_t remaining = (size_t)run;
        while (remaining != 0U) {
            size_t available = width - column;
            size_t taken = remaining < available ? remaining : available;
            if (black) {
                for (size_t pixel = 0U; pixel < taken; ++pixel) {
                    pk_set_black(glyph->bitmap, row_bytes, row,
                                 column + pixel);
                }
            }
            column += taken;
            remaining -= taken;
            if (column == width) {
                if ((size_t)repeat > height - row - 1U) {
                    return pk_error(error, error_capacity,
                                    "PK repeat count exceeds glyph height");
                }
                for (size_t copy = 1U; copy <= (size_t)repeat; ++copy) {
                    memcpy(glyph->bitmap + (row + copy) * row_bytes,
                           glyph->bitmap + row * row_bytes, row_bytes);
                }
                row += (size_t)repeat + 1U;
                column = 0U;
                repeat = 0U;
                if (row == height && remaining != 0U) {
                    return pk_error(error, error_capacity,
                                    "PK raster exceeds glyph height");
                }
            }
        }
        black = !black;
    }
    return column == 0U && repeat == 0U
               ? 0
               : pk_error(error, error_capacity, "incomplete PK raster row");
}

static int pk_character(struct pk_reader *reader, uint8_t flag,
                        struct hstex_pk_font *font, char *error,
                        size_t error_capacity)
{
    uint8_t dynamic = (uint8_t)(flag >> 4U);
    uint8_t form = (uint8_t)(flag & 7U);
    bool black = (flag & 8U) != 0U;
    uint32_t packet_length = 0U;
    uint32_t character = 0U;
    size_t size_bytes = 0U;

    if (form <= 3U) {
        uint32_t low = 0U;
        if (pk_unsigned(reader, 1U, &low, error, error_capacity) != 0 ||
            pk_unsigned(reader, 1U, &character, error, error_capacity) != 0) {
            return -1;
        }
        packet_length = (uint32_t)(flag & 3U) * UINT32_C(256) + low;
        size_bytes = 1U;
    } else if (form <= 6U) {
        uint32_t low = 0U;
        if (pk_unsigned(reader, 2U, &low, error, error_capacity) != 0 ||
            pk_unsigned(reader, 1U, &character, error, error_capacity) != 0) {
            return -1;
        }
        packet_length = (uint32_t)(flag & 3U) * UINT32_C(65536) + low;
        size_bytes = 2U;
    } else {
        if (pk_unsigned(reader, 4U, &packet_length, error, error_capacity) != 0 ||
            pk_unsigned(reader, 4U, &character, error, error_capacity) != 0) {
            return -1;
        }
        size_bytes = 4U;
    }
    if ((size_t)packet_length > reader->length - reader->position) {
        return pk_error(error, error_capacity, "truncated PK character packet");
    }
    size_t packet_finish = reader->position + (size_t)packet_length;

    int32_t tfm_width = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t horizontal_offset = 0;
    int32_t vertical_offset = 0;
    if (size_bytes < 4U) {
        uint32_t tfm = 0U;
        uint32_t movement = 0U;
        uint32_t unsigned_width = 0U;
        uint32_t unsigned_height = 0U;
        if (pk_unsigned(reader, 3U, &tfm, error, error_capacity) != 0 ||
            pk_unsigned(reader, size_bytes, &movement, error, error_capacity) !=
                0 ||
            pk_unsigned(reader, size_bytes, &unsigned_width, error,
                        error_capacity) != 0 ||
            pk_unsigned(reader, size_bytes, &unsigned_height, error,
                        error_capacity) != 0 ||
            pk_signed(reader, size_bytes, &horizontal_offset, error,
                      error_capacity) != 0 ||
            pk_signed(reader, size_bytes, &vertical_offset, error,
                      error_capacity) != 0) {
            return -1;
        }
        if (movement > (uint32_t)INT32_MAX / UINT32_C(65536) ||
            unsigned_width > (uint32_t)INT32_MAX ||
            unsigned_height > (uint32_t)INT32_MAX) {
            return pk_error(error, error_capacity,
                            "PK character dimensions are too large");
        }
        tfm_width = (int32_t)tfm;
        dx = (int32_t)(movement * UINT32_C(65536));
        width = (int32_t)unsigned_width;
        height = (int32_t)unsigned_height;
    } else if (pk_signed(reader, 4U, &tfm_width, error, error_capacity) != 0 ||
               pk_signed(reader, 4U, &dx, error, error_capacity) != 0 ||
               pk_signed(reader, 4U, &dy, error, error_capacity) != 0 ||
               pk_signed(reader, 4U, &width, error, error_capacity) != 0 ||
               pk_signed(reader, 4U, &height, error, error_capacity) != 0 ||
               pk_signed(reader, 4U, &horizontal_offset, error,
                         error_capacity) != 0 ||
               pk_signed(reader, 4U, &vertical_offset, error,
                         error_capacity) != 0) {
        return -1;
    }
    if (width < 0 || height < 0 || reader->position > packet_finish) {
        return pk_error(error, error_capacity, "invalid PK character packet");
    }

    if (character < 256U) {
        struct hstex_pk_glyph *glyph = &font->glyphs[character];
        if (glyph->present) {
            return pk_error_unsigned(error, error_capacity,
                                     "duplicate PK character ", character);
        }
        glyph->present = true;
        glyph->tfm_width = tfm_width;
        glyph->dx = dx;
        glyph->dy = dy;
        glyph->width = width;
        glyph->height = height;
        glyph->horizontal_offset = horizontal_offset;
        glyph->vertical_offset = vertical_offset;
        const uint8_t *raster = reader->bytes + reader->position;
        size_t raster_length = packet_finish - reader->position;
        int status = dynamic == 14U
                         ? pk_decode_raw(raster, raster_length, glyph, error,
                                         error_capacity)
                         : pk_decode_runs(raster, raster_length, dynamic, black,
                                          glyph, error, error_capacity);
        if (status != 0) {
            return -1;
        }
    }
    reader->position = packet_finish;
    return 0;
}

void hstex_pk_destroy(struct hstex_pk_font *font)
{
    if (font == NULL) {
        return;
    }
    for (size_t code = 0U; code < 256U; ++code) {
        free(font->glyphs[code].bitmap);
    }
    memset(font, 0, sizeof(*font));
}

int hstex_pk_parse(const uint8_t *bytes, size_t length,
                   struct hstex_pk_font *font, char *error,
                   size_t error_capacity)
{
    if (bytes == NULL || font == NULL) {
        return pk_error(error, error_capacity, "invalid PK parser input");
    }
    memset(font, 0, sizeof(*font));
    struct pk_reader reader = {bytes, length, 0U};
    uint32_t command = 0U;
    uint32_t identifier = 0U;
    uint32_t comment_length = 0U;
    if (pk_unsigned(&reader, 1U, &command, error, error_capacity) != 0 ||
        command != HSTEX_PK_PRE ||
        pk_unsigned(&reader, 1U, &identifier, error, error_capacity) != 0 ||
        identifier != HSTEX_PK_ID ||
        pk_unsigned(&reader, 1U, &comment_length, error, error_capacity) != 0 ||
        pk_skip(&reader, (size_t)comment_length, error, error_capacity) != 0 ||
        pk_unsigned(&reader, 4U, &font->design_size, error, error_capacity) !=
            0 ||
        pk_unsigned(&reader, 4U, &font->checksum, error, error_capacity) != 0 ||
        pk_unsigned(&reader, 4U, &font->horizontal_pixels_per_point, error,
                    error_capacity) != 0 ||
        pk_unsigned(&reader, 4U, &font->vertical_pixels_per_point, error,
                    error_capacity) != 0) {
        hstex_pk_destroy(font);
        return command == HSTEX_PK_PRE && identifier == HSTEX_PK_ID
                   ? -1
                   : pk_error(error, error_capacity, "invalid PK preamble");
    }

    bool postamble = false;
    while (reader.position < reader.length) {
        if (pk_unsigned(&reader, 1U, &command, error, error_capacity) != 0) {
            hstex_pk_destroy(font);
            return -1;
        }
        if (command < 240U) {
            if (pk_character(&reader, (uint8_t)command, font, error,
                             error_capacity) != 0) {
                hstex_pk_destroy(font);
                return -1;
            }
            continue;
        }
        if (command >= 240U && command <= 243U) {
            uint32_t special_length = 0U;
            size_t count = (size_t)(command - 239U);
            if (pk_unsigned(&reader, count, &special_length, error,
                            error_capacity) != 0 ||
                pk_skip(&reader, (size_t)special_length, error,
                        error_capacity) != 0) {
                hstex_pk_destroy(font);
                return -1;
            }
        } else if (command == 244U) {
            if (pk_skip(&reader, 4U, error, error_capacity) != 0) {
                hstex_pk_destroy(font);
                return -1;
            }
        } else if (command == HSTEX_PK_POST) {
            postamble = true;
            break;
        } else if (command != HSTEX_PK_NO_OP) {
            hstex_pk_destroy(font);
            return pk_error_unsigned(error, error_capacity,
                                     "undefined PK command ", command);
        }
    }
    while (postamble && reader.position < reader.length) {
        if (reader.bytes[reader.position++] != HSTEX_PK_NO_OP) {
            hstex_pk_destroy(font);
            return pk_error(error, error_capacity,
                            "data follows the PK postamble");
        }
    }
    if (!postamble) {
        hstex_pk_destroy(font);
        return pk_error(error, error_capacity, "PK postamble is missing");
    }
    return 0;
}
