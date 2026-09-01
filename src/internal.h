#ifndef HSTEX_INTERNAL_H
#define HSTEX_INTERNAL_H

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define HSTEX_PRINTF_FORMAT(format_index, first_argument_index)              \
    __attribute__((__format__(__printf__, format_index, first_argument_index)))
#else
#define HSTEX_PRINTF_FORMAT(format_index, first_argument_index)
#endif

struct hstex_engine;

int hstex_rebuild_glyph_unicode_slots(struct hstex_engine *engine,
                                      char *error, size_t error_capacity);

#endif
