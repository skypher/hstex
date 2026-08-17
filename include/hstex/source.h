#ifndef HSTEX_SOURCE_H
#define HSTEX_SOURCE_H

#include "hstex/input.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/token.h"

#include <stdbool.h>
#include <stddef.h>

enum hstex_source_frame_kind {
    HSTEX_SOURCE_FILE = 0,
    HSTEX_SOURCE_TOKEN_LIST,
    HSTEX_SOURCE_BOUNDARY,
};

struct hstex_file_source {
    struct hstex_input input;
    struct hstex_mouth mouth;
    char *path;
};

struct hstex_token_source {
    const hstex_token *tokens;
    hstex_token *owned_allocation;
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
    /* Counts files that have run out, so that the engine can insert
       \everyeof once for each; see docs/DECISIONS.md, everyeof. */
    size_t file_end_count;
};

void hstex_source_stack_init(struct hstex_source_stack *stack,
                             struct hstex_lexical_state *lexical_state);
void hstex_source_stack_destroy(struct hstex_source_stack *stack);
int hstex_source_push_file(struct hstex_source_stack *stack, const char *path,
                           char *error, size_t error_capacity);
int hstex_source_push_pseudo_file(struct hstex_source_stack *stack,
                                  uint8_t *bytes, size_t length,
                                  const char *name, char *error,
                                  size_t error_capacity);
int hstex_source_push_tokens(struct hstex_source_stack *stack,
                             const hstex_token *tokens, size_t count,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity);
int hstex_source_push_owned_tokens(struct hstex_source_stack *stack,
                                   hstex_token *tokens, size_t count,
                                   struct hstex_source_location location,
                                   char *error, size_t error_capacity);
int hstex_source_push_boundary(struct hstex_source_stack *stack, char *error,
                               size_t error_capacity);
/* True when the input has run down to a boundary rather than to nothing at
   all: the reading is over, but the document is not. */
bool hstex_source_at_boundary(const struct hstex_source_stack *stack);

int hstex_source_pop_boundary(struct hstex_source_stack *stack, char *error,
                              size_t error_capacity);
int hstex_source_end_current_file(struct hstex_source_stack *stack, char *error,
                                  size_t error_capacity);
enum hstex_mouth_result hstex_source_next(
    struct hstex_source_stack *stack, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity);
const char *hstex_source_current_name(const struct hstex_source_stack *stack);

#endif
