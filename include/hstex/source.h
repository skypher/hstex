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

/* What a token-list frame is besides its tokens. */
enum hstex_token_source_flag {
    /* A token put back on its own is held in the frame itself rather than in
       an allocation of its own; the frame then points at `held`, and the
       stack points it there again whenever the frames move. */
    HSTEX_TOKEN_SOURCE_HOLDS_OWN = 1U << 0,
    /* The frame owns the tokens it reads and gives them back when it is
       popped; what it owns starts where `tokens` points. */
    HSTEX_TOKEN_SOURCE_OWNS = 1U << 1,
    /* The tokens stand in the stack's own store, which is given back to
       `store_base` when the frame is popped. */
    HSTEX_TOKEN_SOURCE_FROM_STORE = 1U << 2
};

/* A frame is pushed and popped for every macro call a document makes -- some
   thirty million over the corpus -- so it is kept as narrow as it can be:
   counts are words rather than double words, what the frame owns and where
   it stands are told by flags, and a file, of which a run opens a few
   hundred, keeps its reading state elsewhere. */
struct hstex_token_source {
    const hstex_token *tokens;
    uint32_t count;
    uint32_t cursor;
    struct hstex_source_location location;
    hstex_token held;
    /* The definition whose own tokens the frame is reading, which it holds
       until it is done with them, or zero. See docs/DECISIONS.md,
       a-definition-nothing-holds. */
    uint32_t definition;
    uint32_t store_base;
    uint8_t flags;
};

struct hstex_source_frame {
    enum hstex_source_frame_kind kind;
    union {
        struct hstex_file_source *file;
        struct hstex_token_source token_list;
    } value;
};

struct hstex_source_stack {
    struct hstex_source_frame *frames;
    size_t count;
    size_t capacity;
    /* The frame at the top, where it is a list of tokens, and null where it
       is anything else. Almost every token a run reads comes from there, and
       finding it again costs more than keeping it. */
    struct hstex_token_source *top;
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
/* Room in the store with a frame already standing on it, which is the two
   above in one: the tokens are written into what it returns before anything
   reads them. Null where the store has no room to spare. */
hstex_token *hstex_source_push_room(struct hstex_source_stack *stack,
                                    size_t count,
                                    struct hstex_source_location location);

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
    for (;;) {
        struct hstex_token_source *source = stack->top;
        if (source == NULL) {
            return false;
        }
        if (source->cursor < source->count) {
            *token = source->tokens[source->cursor++];
            *location = source->location;
            return true;
        }
        /* The frame has been read to the end. One that holds nothing to be
           given back by hand -- no allocation of its own, no definition it
           is keeping -- is popped here rather than in a call of its own,
           which is two frames in three over the corpus. */
        if (source->definition != 0U ||
            (source->flags & (uint8_t)HSTEX_TOKEN_SOURCE_OWNS) != 0U) {
            return false;
        }
        if ((source->flags & (uint8_t)HSTEX_TOKEN_SOURCE_FROM_STORE) != 0U) {
            stack->store_count = source->store_base;
        }
        --stack->count;
        stack->top =
            stack->count != 0U && stack->frames[stack->count - 1U].kind ==
                                      HSTEX_SOURCE_TOKEN_LIST
                ? &stack->frames[stack->count - 1U].value.token_list
                : NULL;
    }
}
const char *hstex_source_current_name(const struct hstex_source_stack *stack);

#endif
