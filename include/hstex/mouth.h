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
    /* A character of category 15, which the reference reports and forgets.
       The mouth has already read past it; it hands the fault up because
       only the engine can report one. */
    HSTEX_MOUTH_INVALID = 2,
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
    /* THE LINE IS HELD AS A COPY, NOT AS A WINDOW ON THE FILE, because
       collapsing `^^' notation rewrites it. The reference reduces `^^M' to
       one character in its own buffer and shifts the rest of the line down
       over the two bytes that go; everything downstream -- above all the
       line an error draws -- then sees what was collapsed, not what the
       file holds. A window on `data' could not be written to, so a line
       that collapsed anything would draw its raw bytes instead: trip line
       429 sets `q' to superscript and writes ^^M as `qq1qM', which the
       reference draws as `^^M'. See tests/trip/probes. */
    uint8_t *line_buffer;
    size_t line_capacity;
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
