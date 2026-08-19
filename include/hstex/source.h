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
    /* A token put back on its own is held in the frame itself rather than in
       an allocation of its own; the frame then points at `held`, and the
       stack points it there again whenever the frames move. */
    hstex_token held;
    bool holds_own;
    /* The definition whose own tokens the frame is reading, which it holds
       until it is done with them, or zero. See docs/DECISIONS.md,
       a-definition-nothing-holds. */
    uint32_t definition;
    /* Where in the stack's own store the frame's tokens stand, when they
       stand there: the store is given back to that mark when the frame is
       popped. */
    bool from_store;
    size_t store_base;
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
    /* What to tell when a frame lets go of the definition it was reading. */
    void *definition_owner;
    void (*definition_release)(void *owner, uint32_t definition);
    /* Room the stack keeps for the expansions it is reading, given back in
       the order it was taken; an expansion that does not fit finds its own.
       The store only grows while nothing stands in it. */
    hstex_token *store;
    size_t store_count;
    size_t store_capacity;
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
/* One token put back, which the frame holds itself. */
int hstex_source_push_one(struct hstex_source_stack *stack, hstex_token token,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity);
/* Room in the stack's own store for tokens about to be read, or NULL where
   the store has none to spare. What it returns stands until the frame that
   `hstex_source_push_reserved` makes for it is popped, and nothing else may
   be pushed in between. */
hstex_token *hstex_source_reserve(struct hstex_source_stack *stack,
                                  size_t count);
int hstex_source_push_reserved(struct hstex_source_stack *stack, size_t count,
                               struct hstex_source_location location,
                               char *error, size_t error_capacity);

/* A definition's own tokens, read where they stand rather than copied: the
   frame holds the definition until it has read them. */
int hstex_source_push_definition(struct hstex_source_stack *stack,
                                 const hstex_token *tokens, size_t count,
                                 uint32_t definition,
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

/* The next token where it comes from a list that has one left, which is
   where all but a few of them come from: a macro's expansion, a token
   register, a token put back. Anything else -- a file, a boundary, a list
   that has run out -- is left to `hstex_source_next`. */
static inline bool hstex_source_take(struct hstex_source_stack *stack,
                                     hstex_token *token,
                                     struct hstex_source_location *location)
{
    if (stack->count == 0U) {
        return false;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count - 1U];
    if (frame->kind != HSTEX_SOURCE_TOKEN_LIST) {
        return false;
    }
    struct hstex_token_source *source = &frame->value.token_list;
    if (source->cursor >= source->count) {
        return false;
    }
    *token = source->tokens[source->cursor++];
    *location = source->location;
    return true;
}
const char *hstex_source_current_name(const struct hstex_source_stack *stack);

#endif
