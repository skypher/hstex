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
    return 0;
}

static void pop_frame(struct hstex_source_stack *stack)
{
    struct hstex_source_frame *frame = &stack->frames[stack->count - 1U];
    if (frame->kind == HSTEX_SOURCE_FILE) {
        hstex_mouth_destroy(&frame->value.file.mouth);
        hstex_input_close(&frame->value.file.input);
        free(frame->value.file.path);
    } else if (frame->kind == HSTEX_SOURCE_TOKEN_LIST) {
        free(frame->value.token_list.owned_allocation);
    }
    --stack->count;
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

    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = HSTEX_SOURCE_FILE;
    frame->value.file.input = input;
    frame->value.file.path = path_copy;
    hstex_mouth_init(&frame->value.file.mouth, input.data, input.length,
                     stack->lexical_state);
    return 0;
}

int hstex_source_push_tokens(struct hstex_source_stack *stack,
                             const hstex_token *tokens, size_t count,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    if (stack == NULL || (count != 0U && tokens == NULL)) {
        return set_error(error, error_capacity, "invalid token-source request");
    }
    pop_exhausted_token_frames(stack);
    if (reserve_frames(stack, stack->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_source_frame *frame = &stack->frames[stack->count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    frame->value.token_list.tokens = tokens;
    frame->value.token_list.count = count;
    frame->value.token_list.location = location;
    return 0;
}

int hstex_source_push_owned_tokens(struct hstex_source_stack *stack,
                                   hstex_token *tokens, size_t count,
                                   struct hstex_source_location location,
                                   char *error, size_t error_capacity)
{
    if (stack == NULL || (count != 0U && tokens == NULL)) {
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
    memset(frame, 0, sizeof(*frame));
    frame->kind = HSTEX_SOURCE_TOKEN_LIST;
    frame->value.token_list.tokens = tokens;
    frame->value.token_list.owned_allocation = tokens;
    frame->value.token_list.count = count;
    frame->value.token_list.location = location;
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
    return 0;
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
        struct hstex_mouth *mouth = &frame->value.file.mouth;
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
            &frame->value.file.mouth, token, location, error, error_capacity);
        if (result == HSTEX_MOUTH_EOF) {
            pop_frame(stack);
            continue;
        }
        return result;
    }
    return HSTEX_MOUTH_EOF;
}

const char *hstex_source_current_name(const struct hstex_source_stack *stack)
{
    if (stack == NULL || stack->count == 0U) {
        return NULL;
    }
    for (size_t index = stack->count; index > 0U; --index) {
        const struct hstex_source_frame *frame = &stack->frames[index - 1U];
        if (frame->kind == HSTEX_SOURCE_FILE) {
            return frame->value.file.path;
        }
    }
    return NULL;
}
