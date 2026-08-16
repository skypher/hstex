#include "hstex/catcode.h"

#include <stddef.h>
#include <stdint.h>

void hstex_catcodes_init_ini(struct hstex_catcode_table *table)
{
    for (size_t index = 0U; index < 256U; ++index) {
        table->values[index] = (uint8_t)HSTEX_CAT_OTHER;
    }
    table->values[0] = (uint8_t)HSTEX_CAT_IGNORED;
    table->values[13] = (uint8_t)HSTEX_CAT_END_OF_LINE;
    table->values[32] = (uint8_t)HSTEX_CAT_SPACE;
    table->values[37] = (uint8_t)HSTEX_CAT_COMMENT;
    table->values[92] = (uint8_t)HSTEX_CAT_ESCAPE;
    table->values[127] = (uint8_t)HSTEX_CAT_INVALID;
    for (uint32_t character = (uint32_t)'A'; character <= (uint32_t)'Z';
         ++character) {
        table->values[character] = (uint8_t)HSTEX_CAT_LETTER;
    }
    for (uint32_t character = (uint32_t)'a'; character <= (uint32_t)'z';
         ++character) {
        table->values[character] = (uint8_t)HSTEX_CAT_LETTER;
    }
    table->generation = UINT64_C(1);
}

int hstex_catcode_set(struct hstex_catcode_table *table, uint32_t character,
                      uint32_t category)
{
    if (table == NULL || character > UINT32_C(255) ||
        category > (uint32_t)HSTEX_CAT_INVALID) {
        return -1;
    }
    uint8_t new_value = (uint8_t)category;
    if (table->values[character] != new_value) {
        table->values[character] = new_value;
        ++table->generation;
        if (table->generation == UINT64_C(0)) {
            table->generation = UINT64_C(1);
        }
    }
    return 0;
}

uint8_t hstex_catcode_get(const struct hstex_catcode_table *table,
                          uint8_t character)
{
    return table->values[character];
}
