#ifndef HSTEX_SOURCE_H
#define HSTEX_SOURCE_H

#include "hstex/input.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/token.h"

#include <stddef.h>

enum hstex_source_frame_kind {
    HSTEX_SOURCE_FILE = 0,
    HSTEX_SOURCE_TOKEN_LIST,
};

struct hstex_file_source {
    struct hstex_input input;
    struct hstex_mouth mouth;
    char *path;
};

struct hstex_token_source {
    const hstex_token *tokens;
    size_t count;
    size_t cursor;
    struct hstex_source_location location;
};

struct hstex_source_frame {
    enum hstex_source_frame_kind kind;
    union {
        struct hstex_file_source file;
        struct hstex_token_source token_list;
    } value;
};

struct hstex_source_stack {
    struct hstex_source_frame *frames;
    size_t count;
    size_t capacity;
    struct hstex_lexical_state *lexical_state;
};

void hstex_source_stack_init(struct hstex_source_stack *stack,
                             struct hstex_lexical_state *lexical_state);
void hstex_source_stack_destroy(struct hstex_source_stack *stack);
int hstex_source_push_file(struct hstex_source_stack *stack, const char *path,
                           char *error, size_t error_capacity);
int hstex_source_push_tokens(struct hstex_source_stack *stack,
                             const hstex_token *tokens, size_t count,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity);
enum hstex_mouth_result hstex_source_next(
    struct hstex_source_stack *stack, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity);
const char *hstex_source_current_name(const struct hstex_source_stack *stack);

#endif
