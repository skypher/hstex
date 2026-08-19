#ifndef HSTEX_MOUTH_H
#define HSTEX_MOUTH_H

#include "hstex/lex.h"
#include "hstex/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hstex_mouth_state {
    HSTEX_MOUTH_NEW_LINE = 0,
    HSTEX_MOUTH_MIDDLE_LINE,
    HSTEX_MOUTH_SKIP_SPACES,
};

enum hstex_mouth_result {
    HSTEX_MOUTH_ERROR = -1,
    HSTEX_MOUTH_EOF = 0,
    HSTEX_MOUTH_TOKEN = 1,
};

/* Where a token came from. Two words, because one of these is copied for
   every token the engine reads -- some hundreds of millions over the
   corpus -- and nothing has ever asked how far into the file it stood. */
struct hstex_source_location {
    uint32_t line;
    uint32_t column;
};

struct hstex_mouth {
    const uint8_t *data;
    size_t length;
    size_t next_line_offset;
    size_t line_start;
    size_t line_content_length;
    size_t line_cursor;
    size_t line_raw_length;
    uint32_t line_number;
    uint8_t end_line_byte;
    bool line_loaded;
    bool has_end_line_byte;
    enum hstex_mouth_state state;
    struct hstex_lexical_state *lexical_state;
    uint8_t *name_scratch;
    size_t name_length;
    size_t name_capacity;
};

void hstex_mouth_init(struct hstex_mouth *mouth, const uint8_t *data,
                      size_t length, struct hstex_lexical_state *lexical_state);
void hstex_mouth_destroy(struct hstex_mouth *mouth);
enum hstex_mouth_result hstex_mouth_next(
    struct hstex_mouth *mouth, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity);

#endif
