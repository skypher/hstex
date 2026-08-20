#include "hstex/source.h"

#include "internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int set_error(char *error, size_t capacity, const char *format, ...)
    HSTEX_PRINTF_FORMAT(3, 4);

static int set_error(char *error, size_t capacity, const char *format, ...)
{
    if (error != NULL && capacity != 0U) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(error, capacity, format, arguments);
        va_end(arguments);
    }
    return -1;
}

/* The frame at the top, where it is a list of tokens; see the note on
   `top` in the header. Everything that moves the frames says so here. */
static void note_top_frame(struct hstex_source_stack *stack)
{
    stack->top = stack->count != 0U &&
                         stack->frames[stack->count - 1U].kind ==
                             HSTEX_SOURCE_TOKEN_LIST
                     ? &stack->frames[stack->count - 1U].value.token_list
                     : NULL;
}

static int reserve_frames(struct hstex_source_stack *stack, size_t required,
                          char *error, size_t error_capacity)
{
    if (required <= stack->capacity) {
        return 0;
    }
    size_t capacity = stack->capacity == 0U ? 16U : stack->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "input stack overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*stack->frames)) {
        return set_error(error, error_capacity, "input stack allocation overflow");
    }
    void *allocation = realloc(stack->frames, capacity * sizeof(*stack->frames));
    if (allocation == NULL) {
        return set_error(error, error_capacity, "input stack allocation failed");
    }
    stack->frames = allocation;
    stack->capacity = capacity;
    /* A frame that holds its own token pointed into the frames that have
       just moved. */
    for (size_t index = 0U; index < stack->count; ++index) {
        struct hstex_source_frame *frame = &stack->frames[index];
        if (frame->kind == HSTEX_SOURCE_TOKEN_LIST &&
            (frame->value.token_list.flags &
             (uint8_t)HSTEX_TOKEN_SOURCE_HOLDS_OWN) != 0U) {
            frame->value.token_list.tokens = &frame->value.token_list.held;
        }
    }
    note_top_frame(stack);
    return 0;
}

static void pop_frame(struct hstex_source_stack *stack)
{
    struct hstex_source_frame *frame = &stack->frames[stack->count - 1U];
    const bool was_file = frame->kind == HSTEX_SOURCE_FILE;
    if (frame->kind == HSTEX_SOURCE_FILE) {
        hstex_mouth_destroy(&frame->value.file->mouth);
        hstex_input_close(&frame->value.file->input);
        free(frame->value.file->path);
        free(frame->value.file);
    } else if (frame->kind == HSTEX_SOURCE_TOKEN_LIST) {
        struct hstex_token_source *source = &frame->value.token_list;
        /* Most frames hold no allocation of their own -- an expansion stands
           in the stack's own store -- and asking the library to give nothing
           back is 48 million calls over the corpus. */
        if ((source->flags & (uint8_t)HSTEX_TOKEN_SOURCE_OWNS) != 0U) {
            hstex_token *own =
                (hstex_token *)(uintptr_t)(const void *)source->tokens;
            if (stack->tokens_release != NULL) {
                stack->tokens_release(stack->definition_owner, own,
                                      source->count);
            } else {
                free(own);
            }
        }
        if ((source->flags & (uint8_t)HSTEX_TOKEN_SOURCE_FROM_STORE) != 0U) {
            stack->store_count = source->store_base;
        }
        if (source->definition != 0U && stack->definition_release != NULL) {
            stack->definition_release(stack->definition_owner,
                                      source->definition);
        }
    }
    --stack->count;
    if (was_file) {
        /* A run opens a few hundred files against thirty million token
           frames, so the one below is looked for here rather than kept. */
        stack->file_top = 0U;
        for (size_t index = stack->count; index != 0U; --index) {
            if (stack->frames[index - 1U].kind == HSTEX_SOURCE_FILE) {
                stack->file_top = index;
                break;
            }
        }
    }
    note_top_frame(stack);
}

static void pop_exhausted_token_frames(struct hstex_source_stack *stack)
{
    while (stack->count != 0U) {
        struct hstex_source_frame *frame =
            &stack->frames[stack->count - 1U];
        if (frame->kind != HSTEX_SOURCE_TOKEN_LIST ||
            frame->value.token_list.cursor < frame->value.token_list.count) {
            break;
        }
        pop_frame(stack);
    }
}

void hstex_source_stack_init(struct hstex_source_stack *stack,
                             struct hstex_lexical_state *lexical_state)
{
    memset(stack, 0, sizeof(*stack));
    stack->lexical_state = lexical_state;
}

void hstex_source_stack_destroy(struct hstex_source_stack *stack)
{
    if (stack == NULL) {
        return;
    }
    while (stack->count != 0U) {
        pop_frame(stack);
    }
    free(stack->frames);
    free(stack->store);
    memset(stack, 0, sizeof(*stack));
}

int hstex_source_push_file(struct hstex_source_stack *stack, const char *path,
                           char *error, size_t error_capacity)
{
    if (stack == NULL || stack->lexical_state == NULL || path == NULL) {
        return set_error(error, error_capacity, "invalid file-source request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }

    struct hstex_input input;
    if (hstex_input_open(path, &input, error, error_capacity) != 0) {
        return -1;
    }
    size_t path_length = strlen(path);
    char *path_copy = malloc(path_length + 1U);
    if (path_copy == NULL) {
        hstex_input_close(&input);
        return set_error(error, error_capacity, "input path allocation failed");
    }
    memcpy(path_copy, path, path_length + 1U);

    struct hstex_file_source *file = calloc(1U, sizeof(*file));
    if (file == NULL) {
        free(path_copy);
        hstex_input_close(&input);
        return set_error(error, error_capacity, "input file allocation failed");
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = HSTEX_SOURCE_FILE;
    frame->value.file = file;
    stack->file_top = stack->count;
    file->input = input;
    file->path = path_copy;
    hstex_mouth_init(&file->mouth, input.data, input.length,
                     stack->lexical_state);
    note_top_frame(stack);
    return 0;
}

/* A pseudo-file reads an in-memory string with the same line handling as a
   real file, which is what \scantokens needs. The bytes are taken over. */
int hstex_source_push_pseudo_file(struct hstex_source_stack *stack,
                                  uint8_t *bytes, size_t length,
                                  const char *name, char *error,
                                  size_t error_capacity)
{
    if (stack == NULL || stack->lexical_state == NULL ||
        (length != 0U && bytes == NULL)) {
        free(bytes);
        return set_error(error, error_capacity,
                         "invalid pseudo-file source request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    size_t name_length = strlen(name);
    char *name_copy = malloc(name_length + 1U);
    if (name_copy == NULL) {
        free(bytes);
        return set_error(error, error_capacity,
                         "pseudo-file name allocation failed");
    }
    memcpy(name_copy, name, name_length + 1U);

    struct hstex_file_source *file = calloc(1U, sizeof(*file));
    if (file == NULL) {
        free(name_copy);
        free(bytes);
        return set_error(error, error_capacity,
                         "pseudo-file allocation failed");
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = HSTEX_SOURCE_FILE;
    frame->value.file = file;
    stack->file_top = stack->count;
    file->input.data = bytes;
    file->input.length = length;
    file->input.storage = HSTEX_INPUT_STORAGE_OWNED;
    file->path = name_copy;
    hstex_mouth_init(&file->mouth, bytes, length, stack->lexical_state);
    note_top_frame(stack);
    return 0;
}

int hstex_source_push_tokens(struct hstex_source_stack *stack,
                             const hstex_token *tokens, size_t count,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    if (stack == NULL || (count != 0U && tokens == NULL) ||
        count > UINT32_MAX) {
        return set_error(error, error_capacity, "invalid token-source request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    struct hstex_token_source *source = &frame->value.token_list;
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    source->tokens = tokens;
    source->count = (uint32_t)count;
    source->cursor = 0U;
    source->location = location;
    source->held = 0U;
    source->definition = 0U;
    source->store_base = 0U;
    source->flags = 0U;
    note_top_frame(stack);
    return 0;
}

int hstex_source_push_owned_tokens(struct hstex_source_stack *stack,
                                   hstex_token *tokens, size_t count,
                                   struct hstex_source_location location,
                                   char *error, size_t error_capacity)
{
    if (stack == NULL || (count != 0U && tokens == NULL) ||
        count > UINT32_MAX) {
        free(tokens);
        return set_error(error, error_capacity,
                         "invalid owned token-source request");
    }
    pop_exhausted_token_frames(stack);
    if (count == 0U) {
        free(tokens);
        return 0;
    }
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        free(tokens);
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    struct hstex_token_source *source = &frame->value.token_list;
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    source->tokens = tokens;
    source->count = (uint32_t)count;
    source->cursor = 0U;
    source->location = location;
    source->held = 0U;
    source->definition = 0U;
    source->store_base = 0U;
    source->flags = (uint8_t)HSTEX_TOKEN_SOURCE_OWNS;
    note_top_frame(stack);
    return 0;
}

int hstex_source_push_one(struct hstex_source_stack *stack, hstex_token token,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity)
{
    if (stack == NULL) {
        return set_error(error, error_capacity, "invalid token-source request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    struct hstex_token_source *source = &frame->value.token_list;
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    source->held = token;
    source->tokens = &source->held;
    source->count = 1U;
    source->cursor = 0U;
    source->location = location;
    source->definition = 0U;
    source->store_base = 0U;
    source->flags = (uint8_t)HSTEX_TOKEN_SOURCE_HOLDS_OWN;
    note_top_frame(stack);
    return 0;
}

hstex_token *hstex_source_reserve(struct hstex_source_stack *stack,
                                  size_t count)
{
    if (stack == NULL || count == 0U) {
        return NULL;
    }
    /* What has been read to the end is given back first, so that the room a
       run of expansions took comes back as each of them finishes. */
    pop_exhausted_token_frames(stack);
    if (count > stack->store_capacity - stack->store_count) {
        /* The store grows only while nothing stands in it, since what stands
           in it points into it. */
        if (stack->store_count != 0U) {
            return NULL;
        }
        size_t capacity = stack->store_capacity == 0U ? 65536U
                                                      : stack->store_capacity;
        while (capacity < count) {
            if (capacity > SIZE_MAX / 2U) {
                return NULL;
            }
            capacity *= 2U;
        }
        if (capacity > SIZE_MAX / sizeof(*stack->store)) {
            return NULL;
        }
        hstex_token *grown =
            realloc(stack->store, capacity * sizeof(*stack->store));
        if (grown == NULL) {
            return NULL;
        }
        stack->store = grown;
        stack->store_capacity = capacity;
    }
    return stack->store + stack->store_count;
}

int hstex_source_push_reserved(struct hstex_source_stack *stack, size_t count,
                               struct hstex_source_location location,
                               char *error, size_t error_capacity)
{
    if (stack == NULL || count == 0U ||
        count > stack->store_capacity - stack->store_count ||
        stack->store_count + count > UINT32_MAX) {
        return set_error(error, error_capacity,
                         "invalid reserved token-source request");
    }
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    struct hstex_token_source *source = &frame->value.token_list;
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    source->tokens = stack->store + stack->store_count;
    source->count = (uint32_t)count;
    source->cursor = 0U;
    source->location = location;
    source->held = 0U;
    source->definition = 0U;
    source->store_base = (uint32_t)stack->store_count;
    source->flags = (uint8_t)HSTEX_TOKEN_SOURCE_FROM_STORE;
    stack->store_count += count;
    note_top_frame(stack);
    return 0;
}

hstex_token *hstex_source_push_room(struct hstex_source_stack *stack,
                                    size_t count,
                                    struct hstex_source_location location)
{
    hstex_token *room = hstex_source_reserve(stack, count);
    if (room == NULL ||
        hstex_source_push_reserved(stack, count, location, NULL, 0U) != 0) {
        return NULL;
    }
    return room;
}

int hstex_source_push_definition(struct hstex_source_stack *stack,
                                 const hstex_token *tokens, size_t count,
                                 uint32_t definition,
                                 struct hstex_source_location location,
                                 char *error, size_t error_capacity)
{
    if (stack == NULL || count == 0U || tokens == NULL ||
        count > UINT32_MAX) {
        return set_error(error, error_capacity,
                         "invalid definition-source request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    struct hstex_token_source *source = &frame->value.token_list;
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    source->tokens = tokens;
    source->count = (uint32_t)count;
    source->cursor = 0U;
    source->location = location;
    source->held = 0U;
    source->definition = definition;
    source->store_base = 0U;
    source->flags = 0U;
    note_top_frame(stack);
    return 0;
}

int hstex_source_push_boundary(struct hstex_source_stack *stack, char *error,
                               size_t error_capacity)
{
    if (stack == NULL) {
        return set_error(error, error_capacity, "invalid source boundary request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = HSTEX_SOURCE_BOUNDARY;
    note_top_frame(stack);
    return 0;
}

bool hstex_source_at_boundary(const struct hstex_source_stack *stack)
{
    return stack != NULL && stack->count != 0U &&
           stack->frames[stack->count - 1U].kind == HSTEX_SOURCE_BOUNDARY;
}

void hstex_source_pop(struct hstex_source_stack *stack)
{
    if (stack != NULL && stack->count != 0U) {
        pop_frame(stack);
    }
}

int hstex_source_pop_boundary(struct hstex_source_stack *stack, char *error,
                              size_t error_capacity)
{
    if (stack == NULL) {
        return set_error(error, error_capacity, "invalid source boundary pop");
    }
    size_t boundary = stack->count;
    while (boundary != 0U) {
        --boundary;
        if (stack->frames[boundary].kind == HSTEX_SOURCE_BOUNDARY) {
            while (stack->count > boundary) {
                pop_frame(stack);
            }
            return 0;
        }
    }
    return set_error(error, error_capacity, "source boundary is not active");
}

int hstex_source_end_current_file(struct hstex_source_stack *stack, char *error,
                                  size_t error_capacity)
{
    if (stack == NULL) {
        return set_error(error, error_capacity,
                         "invalid end-current-file request");
    }
    for (size_t index = stack->count; index != 0U; --index) {
        struct hstex_source_frame *frame = &stack->frames[index - 1U];
        if (frame->kind != HSTEX_SOURCE_FILE) {
            continue;
        }
        struct hstex_mouth *mouth = &frame->value.file->mouth;
        mouth->next_line_offset = mouth->length;
        mouth->line_loaded = false;
        mouth->has_end_line_byte = false;
        return 0;
    }
    return set_error(error, error_capacity,
                     "endinput used without an active file");
}

enum hstex_mouth_result hstex_source_next(
    struct hstex_source_stack *stack, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity)
{
    if (stack == NULL || token == NULL || location == NULL) {
        (void)set_error(error, error_capacity, "invalid input-stack request");
        return HSTEX_MOUTH_ERROR;
    }
    while (stack->count != 0U) {
        struct hstex_source_frame *frame = &stack->frames[stack->count - 1U];
        if (frame->kind == HSTEX_SOURCE_BOUNDARY) {
            return HSTEX_MOUTH_EOF;
        }
        if (frame->kind == HSTEX_SOURCE_TOKEN_LIST) {
            struct hstex_token_source *source = &frame->value.token_list;
            if (source->cursor >= source->count) {
                pop_frame(stack);
                continue;
            }
            *token = source->tokens[source->cursor++];
            *location = source->location;
            return HSTEX_MOUTH_TOKEN;
        }
        enum hstex_mouth_result result = hstex_mouth_next(
            &frame->value.file->mouth, token, location, error, error_capacity);
        if (result == HSTEX_MOUTH_EOF) {
            pop_frame(stack);
            ++stack->file_end_count;
            continue;
        }
        return result;
    }
    return HSTEX_MOUTH_EOF;
}

struct hstex_file_source *hstex_source_current_file(
    const struct hstex_source_stack *stack)
{
    if (stack == NULL || stack->count == 0U) {
        return NULL;
    }
    if (stack->file_top != 0U && stack->file_top <= stack->count) {
        return stack->frames[stack->file_top - 1U].value.file;
    }
    return NULL;
}

const char *hstex_source_current_name(const struct hstex_source_stack *stack)
{
    if (stack == NULL || stack->count == 0U) {
        return NULL;
    }
    if (stack->file_top != 0U && stack->file_top <= stack->count) {
        return stack->frames[stack->file_top - 1U].value.file->path;
    }
    return NULL;
}
