#ifndef HSTEX_INTERNAL_H
#define HSTEX_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define HSTEX_PRINTF_FORMAT(format_index, first_argument_index)              \
    __attribute__((__format__(__printf__, format_index, first_argument_index)))
#define HSTEX_COLD_NOINLINE __attribute__((__cold__, __noinline__))
#else
#define HSTEX_PRINTF_FORMAT(format_index, first_argument_index)
#define HSTEX_COLD_NOINLINE
#endif

struct hstex_engine;

int hstex_rebuild_glyph_unicode_slots(struct hstex_engine *engine,
                                      char *error, size_t error_capacity);

/* The engine's assignable state to and from an open stream, for a mid-run
   checkpoint (which wraps it with the half-built page and reading position
   the format itself never holds). `to_file` writes the format's magic,
   layout, and body at the stream's current position; `from_buffer` reads
   them from `bytes` and reports how many bytes it took in `*consumed`. The
   checkpoint keeps its own process clock rather than the reader's. */
int hstex_engine_format_to_file(struct hstex_engine *engine, FILE *file);
int hstex_engine_format_from_buffer(struct hstex_engine *engine,
                                    const uint8_t *bytes, size_t length,
                                    size_t *consumed, char *error,
                                    size_t error_capacity);

#endif
