#ifndef HSTEX_CATCODE_H
#define HSTEX_CATCODE_H

#include <stdint.h>

enum hstex_catcode {
    HSTEX_CAT_ESCAPE = 0,
    HSTEX_CAT_BEGIN_GROUP = 1,
    HSTEX_CAT_END_GROUP = 2,
    HSTEX_CAT_MATH_SHIFT = 3,
    HSTEX_CAT_ALIGNMENT_TAB = 4,
    HSTEX_CAT_END_OF_LINE = 5,
    HSTEX_CAT_PARAMETER = 6,
    HSTEX_CAT_SUPERSCRIPT = 7,
    HSTEX_CAT_SUBSCRIPT = 8,
    HSTEX_CAT_IGNORED = 9,
    HSTEX_CAT_SPACE = 10,
    HSTEX_CAT_LETTER = 11,
    HSTEX_CAT_OTHER = 12,
    HSTEX_CAT_ACTIVE = 13,
    HSTEX_CAT_COMMENT = 14,
    HSTEX_CAT_INVALID = 15,
};

struct hstex_catcode_table {
    uint8_t values[256];
    uint64_t generation;
};

void hstex_catcodes_init_ini(struct hstex_catcode_table *table);
int hstex_catcode_set(struct hstex_catcode_table *table, uint32_t character,
                      uint32_t category);
uint8_t hstex_catcode_get(const struct hstex_catcode_table *table,
                          uint8_t character);

#endif
