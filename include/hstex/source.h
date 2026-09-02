#ifndef HSTEX_SOURCE_H
#define HSTEX_SOURCE_H

#include "hstex/input.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

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
    HSTEX_TOKEN_SOURCE_FROM_STORE = 1U << 2,
    /* Below what the frame reads, in the same run of store, stand the
       lengths of the arguments the expansion was built from -- one word
       each, as many as the macro takes. They are there so that an error
       can draw the macro's own definition rather than the text that was
       substituted into it; see hstex_source_hold_arguments_below. */
    HSTEX_TOKEN_SOURCE_ARGUMENT_LENGTHS = 1U << 3
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
    /* What the frame is, for the line an error shows above the file's. See
       docs/DECISIONS.md, error-context. */
    uint8_t source_kind;
    /* For a macro, the control sequence it was called by; for a token
       parameter, which parameter it is. */
    uint32_t frame_name;
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
    /* What to give a frame's own tokens back to. The engine keeps free
       lists of token blocks, and a frame that owns its tokens holds one of
       them; giving it back to the library instead would take it out of the
       pool for good. Null where they are to be given back the plain way. */
    void (*tokens_release)(void *owner, hstex_token *tokens, size_t count);
    /* Where a frame's own tokens are asked FROM, so that a block restored from
       a checkpoint sits on the same free list `tokens_release` gives it back
       to -- a plain library block would be given back to a pool whose bookkeep
       overruns it. Null where a checkpoint is read outside the engine (the
       serializer test), where a plain allocation and a plain free suffice. */
    hstex_token *(*tokens_alloc)(void *owner, size_t count);
    /* Room the stack keeps for the expansions it is reading, given back in
       the order it was taken; an expansion that does not fit finds its own.
       The store only grows while nothing stands in it. */
    hstex_token *store;
    size_t store_count;
    size_t store_capacity;
    /* Counts files that have run out, so that the engine can insert
       \everyeof once for each; see docs/DECISIONS.md, everyeof. */
    size_t file_end_count;
    /* How many frames have been pushed since the stack was made, so that a
       caller can tell whether something it ran pushed one -- which counting
       frames cannot, since reading may pop as many as it pushes. */
    uint64_t pushes;
    /* Where the innermost file frame stands, one more than its index, or
       zero. Whoever asks where the reading is wants that frame, and finding
       it again is a walk down a stack that a deep expansion makes deep. */
    size_t file_top;
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
/* What an error calls a frame it shows. */
enum hstex_token_source_kind {
    HSTEX_TOKEN_SOURCE_INSERTED = 0,
    HSTEX_TOKEN_SOURCE_BACKED_UP,
    HSTEX_TOKEN_SOURCE_MACRO,
    HSTEX_TOKEN_SOURCE_TEMPLATE,
    HSTEX_TOKEN_SOURCE_ARGUMENT,
    HSTEX_TOKEN_SOURCE_TOKEN_PARAMETER,
    /* The text of a \write, which is expanded on its own and which the
       reference names for what it is. */
    HSTEX_TOKEN_SOURCE_WRITE,
    /* The text one of the mark primitives expands into. */
    HSTEX_TOKEN_SOURCE_MARK,
    /* The half of a template that follows the entry. It is named `<template>'
       as the half before it is, but it is NOT cleared away when it has been
       read: the reference keeps it, and a fault under it says so. */
    HSTEX_TOKEN_SOURCE_TEMPLATE_AFTER
};

/* Say what the frame just pushed is, so that an error can name it. */
void hstex_source_name_top(struct hstex_source_stack *stack, uint8_t kind,
                           uint32_t name);

/* One token put back, which the frame holds itself. */
int hstex_source_push_one(struct hstex_source_stack *stack, hstex_token token,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity);
/* The same, BEGUN rather than put back: it clears nothing away under it. */
int hstex_source_begin_one(struct hstex_source_stack *stack, hstex_token token,
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

/* Take `held` words off the front of what the frame on top reads, leaving
   them in the store below it: the frame gives them back when it is popped,
   and until then they are the caller's to read at `tokens - held`. The frame
   also takes the definition, which it holds until it is done -- so that what
   an error draws of a macro can be its own definition rather than the text
   that was built from it. See src/engine.c, instantiate_macro. */
void hstex_source_hold_arguments_below(struct hstex_source_stack *stack,
                                       size_t held, uint32_t definition);

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

/* Drop the token-list frames that have been read to the end. Every push
   does this first, so a caller that wants to note where the stack stands
   before pushing has to do it too, or the push moves the floor under it. */
void hstex_source_settle(struct hstex_source_stack *stack);

/* Drop the frame on top, giving back whatever it holds. Used where an
   expansion is made only to be read off the stack again. */
void hstex_source_pop(struct hstex_source_stack *stack);
int hstex_source_pop_boundary(struct hstex_source_stack *stack, char *error,
                              size_t error_capacity);
/* The same, but the spent half of an alignment template standing on top of
   the boundary is left standing rather than taken away with it. */
int hstex_source_pop_boundary_keeping_template(struct hstex_source_stack *stack,
                                               char *error,
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
/* The file being read, for the line an error names. Null where nothing on
   the stack is a file. */
struct hstex_file_source *hstex_source_current_file(
    const struct hstex_source_stack *stack);

/* The whole reading position, written to `out` and read back, so that a run
   can be checkpointed to disk at a page boundary and taken up again by a fresh
   process. A file frame is written as its path and the mouth's cursor (the
   file is reopened on restore); a token frame is written with its tokens
   snapshotted inline and comes back owning a copy of them -- the definition it
   was reading and the store it shared are not carried over, so what an error
   would draw of a macro being read is lost, but every token still to be read
   is exactly preserved. Deserialize expects a freshly initialised stack. */
int hstex_source_serialize(const struct hstex_source_stack *stack, FILE *out);
int hstex_source_deserialize(struct hstex_source_stack *stack, FILE *in,
                             struct hstex_lexical_state *lexical_state,
                             char *error, size_t error_capacity);

#endif
