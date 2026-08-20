#include "hstex/mouth.h"

#include "internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct logical_character {
    uint8_t value;
    struct hstex_source_location location;
};

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

static bool is_lower_hexadecimal(uint8_t character)
{
    return (character >= (uint8_t)'0' && character <= (uint8_t)'9') ||
           (character >= (uint8_t)'a' && character <= (uint8_t)'f');
}

static uint8_t hexadecimal_value(uint8_t character)
{
    if (character <= (uint8_t)'9') {
        return (uint8_t)(character - (uint8_t)'0');
    }
    return (uint8_t)(character - (uint8_t)'a' + 10U);
}

static uint8_t raw_character(const struct hstex_mouth *mouth, size_t index)
{
    if (index < mouth->line_content_length) {
        return mouth->data[mouth->line_start + index];
    }
    return mouth->end_line_byte;
}

static struct hstex_source_location raw_location(const struct hstex_mouth *mouth,
                                                  size_t index)
{
    struct hstex_source_location location;
    location.line = mouth->line_number;
    size_t column = index + 1U;
    location.column = column > UINT32_MAX ? UINT32_MAX : (uint32_t)column;
    return location;
}

static bool load_line(struct hstex_mouth *mouth, char *error,
                      size_t error_capacity)
{
    if (mouth->next_line_offset >= mouth->length) {
        return false;
    }
    if (mouth->line_number == UINT32_MAX) {
        (void)set_error(error, error_capacity, "input has more than 2^32-1 lines");
        return false;
    }

    size_t start = mouth->next_line_offset;
    size_t end = start;
    while (end < mouth->length && mouth->data[end] != (uint8_t)'\n' &&
           mouth->data[end] != (uint8_t)'\r') {
        ++end;
    }
    size_t next = end;
    if (next < mouth->length) {
        uint8_t terminator = mouth->data[next++];
        if (terminator == (uint8_t)'\r' && next < mouth->length &&
            mouth->data[next] == (uint8_t)'\n') {
            ++next;
        }
    }
    while (end > start && mouth->data[end - 1U] == (uint8_t)' ') {
        --end;
    }

    mouth->next_line_offset = next;
    mouth->line_start = start;
    mouth->line_content_length = end - start;
    mouth->line_cursor = 0U;
    ++mouth->line_number;
    mouth->state = HSTEX_MOUTH_NEW_LINE;
    int32_t end_line_character = mouth->lexical_state->end_line_character;
    mouth->has_end_line_byte = end_line_character >= 0 && end_line_character <= 255;
    mouth->end_line_byte = mouth->has_end_line_byte
                               ? (uint8_t)end_line_character
                               : 0U;
    mouth->line_raw_length = mouth->line_content_length +
                             (mouth->has_end_line_byte ? 1U : 0U);
    mouth->line_loaded = true;
    return true;
}

static bool read_logical_character(struct hstex_mouth *mouth,
                                   struct logical_character *character)
{
    if (mouth->line_cursor >= mouth->line_raw_length) {
        return false;
    }
    size_t first_index = mouth->line_cursor;
    uint8_t current = raw_character(mouth, mouth->line_cursor++);
    character->location = raw_location(mouth, first_index);

    for (;;) {
        uint8_t category = hstex_catcode_get(&mouth->lexical_state->catcodes,
                                             current);
        if (category != (uint8_t)HSTEX_CAT_SUPERSCRIPT ||
            mouth->line_cursor + 1U >= mouth->line_raw_length ||
            raw_character(mouth, mouth->line_cursor) != current) {
            break;
        }
        uint8_t third = raw_character(mouth, mouth->line_cursor + 1U);
        if (third >= UINT8_C(128)) {
            break;
        }
        if (mouth->line_cursor + 2U < mouth->line_raw_length &&
            is_lower_hexadecimal(third)) {
            uint8_t fourth = raw_character(mouth, mouth->line_cursor + 2U);
            if (is_lower_hexadecimal(fourth)) {
                current = (uint8_t)((hexadecimal_value(third) << 4U) |
                                    hexadecimal_value(fourth));
                mouth->line_cursor += 3U;
                continue;
            }
        }
        current = third < UINT8_C(64) ? (uint8_t)(third + UINT8_C(64))
                                      : (uint8_t)(third - UINT8_C(64));
        mouth->line_cursor += 2U;
    }
    character->value = current;
    return true;
}

static int reserve_name(struct hstex_mouth *mouth, size_t required, char *error,
                        size_t error_capacity)
{
    if (required <= mouth->name_capacity) {
        return 0;
    }
    size_t capacity = mouth->name_capacity == 0U ? 32U : mouth->name_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "control-sequence name scratch overflow");
        }
        capacity *= 2U;
    }
    void *allocation = realloc(mouth->name_scratch, capacity);
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "control-sequence name scratch allocation failed");
    }
    mouth->name_scratch = allocation;
    mouth->name_capacity = capacity;
    return 0;
}

static int append_name(struct hstex_mouth *mouth, uint8_t character, char *error,
                       size_t error_capacity)
{
    if (reserve_name(mouth, mouth->name_length + 1U, error, error_capacity) != 0) {
        return -1;
    }
    mouth->name_scratch[mouth->name_length++] = character;
    return 0;
}

static enum hstex_mouth_result scan_control_sequence(
    struct hstex_mouth *mouth, hstex_token *token,
    struct hstex_source_location escape_location, char *error,
    size_t error_capacity)
{
    mouth->name_length = 0U;
    struct logical_character first;
    if (!read_logical_character(mouth, &first)) {
        hstex_cs_id identifier = 0U;
        if (hstex_symbol_intern(&mouth->lexical_state->symbols,
                                HSTEX_SYMBOL_REGULAR, NULL, 0U, &identifier,
                                error, error_capacity) != 0) {
            return HSTEX_MOUTH_ERROR;
        }
        mouth->state = HSTEX_MOUTH_MIDDLE_LINE;
        *token = hstex_token_control_sequence(identifier);
        return HSTEX_MOUTH_TOKEN;
    }

    uint8_t category = hstex_catcode_get(&mouth->lexical_state->catcodes,
                                         first.value);
    if (category == (uint8_t)HSTEX_CAT_LETTER) {
        if (append_name(mouth, first.value, error, error_capacity) != 0) {
            return HSTEX_MOUTH_ERROR;
        }
        for (;;) {
            size_t saved_cursor = mouth->line_cursor;
            struct logical_character next;
            if (!read_logical_character(mouth, &next)) {
                break;
            }
            uint8_t next_category = hstex_catcode_get(
                &mouth->lexical_state->catcodes, next.value);
            if (next_category != (uint8_t)HSTEX_CAT_LETTER) {
                mouth->line_cursor = saved_cursor;
                break;
            }
            if (append_name(mouth, next.value, error, error_capacity) != 0) {
                return HSTEX_MOUTH_ERROR;
            }
        }
        mouth->state = HSTEX_MOUTH_SKIP_SPACES;
    } else {
        if (append_name(mouth, first.value, error, error_capacity) != 0) {
            return HSTEX_MOUTH_ERROR;
        }
        mouth->state = category == (uint8_t)HSTEX_CAT_SPACE
                           ? HSTEX_MOUTH_SKIP_SPACES
                           : HSTEX_MOUTH_MIDDLE_LINE;
    }

    hstex_cs_id identifier = 0U;
    if (hstex_symbol_intern(&mouth->lexical_state->symbols,
                            HSTEX_SYMBOL_REGULAR, mouth->name_scratch,
                            mouth->name_length, &identifier, error,
                            error_capacity) != 0) {
        return HSTEX_MOUTH_ERROR;
    }
    (void)escape_location;
    *token = hstex_token_control_sequence(identifier);
    return HSTEX_MOUTH_TOKEN;
}

void hstex_mouth_init(struct hstex_mouth *mouth, const uint8_t *data,
                      size_t length, struct hstex_lexical_state *lexical_state)
{
    memset(mouth, 0, sizeof(*mouth));
    mouth->data = data;
    mouth->length = length;
    mouth->lexical_state = lexical_state;
    mouth->state = HSTEX_MOUTH_NEW_LINE;
}

void hstex_mouth_destroy(struct hstex_mouth *mouth)
{
    if (mouth == NULL) {
        return;
    }
    free(mouth->name_scratch);
    memset(mouth, 0, sizeof(*mouth));
}

enum hstex_mouth_result hstex_mouth_next(
    struct hstex_mouth *mouth, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity)
{
    if (mouth == NULL || token == NULL || location == NULL ||
        mouth->lexical_state == NULL ||
        (mouth->length != 0U && mouth->data == NULL)) {
        (void)set_error(error, error_capacity, "invalid mouth state or output");
        return HSTEX_MOUTH_ERROR;
    }

    for (;;) {
        if (!mouth->line_loaded) {
            if (!load_line(mouth, error, error_capacity)) {
                if (mouth->line_number == UINT32_MAX &&
                    mouth->next_line_offset < mouth->length) {
                    return HSTEX_MOUTH_ERROR;
                }
                return HSTEX_MOUTH_EOF;
            }
        }
        if (mouth->line_cursor >= mouth->line_raw_length) {
            mouth->line_loaded = false;
            continue;
        }

        struct logical_character character;
        if (!read_logical_character(mouth, &character)) {
            mouth->line_loaded = false;
            continue;
        }
        uint8_t category = hstex_catcode_get(&mouth->lexical_state->catcodes,
                                             character.value);
        enum hstex_mouth_state previous_state = mouth->state;

        switch ((enum hstex_catcode)category) {
        case HSTEX_CAT_ESCAPE: {
            enum hstex_mouth_result result = scan_control_sequence(
                mouth, token, character.location, error, error_capacity);
            if (result == HSTEX_MOUTH_TOKEN) {
                *location = character.location;
            }
            return result;
        }
        case HSTEX_CAT_END_OF_LINE:
            mouth->line_cursor = mouth->line_raw_length;
            mouth->line_loaded = false;
            mouth->state = HSTEX_MOUTH_NEW_LINE;
            if (previous_state == HSTEX_MOUTH_NEW_LINE) {
                *token = hstex_token_control_sequence(
                    mouth->lexical_state->paragraph_control_sequence);
                *location = character.location;
                return HSTEX_MOUTH_TOKEN;
            }
            if (previous_state == HSTEX_MOUTH_MIDDLE_LINE) {
                *token = hstex_token_character((uint8_t)HSTEX_CAT_SPACE,
                                               (uint8_t)' ');
                *location = character.location;
                return HSTEX_MOUTH_TOKEN;
            }
            continue;
        case HSTEX_CAT_IGNORED:
            continue;
        case HSTEX_CAT_SPACE:
            if (previous_state == HSTEX_MOUTH_MIDDLE_LINE) {
                mouth->state = HSTEX_MOUTH_SKIP_SPACES;
                *token = hstex_token_character((uint8_t)HSTEX_CAT_SPACE,
                                               (uint8_t)' ');
                *location = character.location;
                return HSTEX_MOUTH_TOKEN;
            }
            continue;
        case HSTEX_CAT_ACTIVE: {
            hstex_cs_id identifier = 0U;
            if (hstex_symbol_intern(&mouth->lexical_state->symbols,
                                    HSTEX_SYMBOL_ACTIVE, &character.value, 1U,
                                    &identifier, error, error_capacity) != 0) {
                return HSTEX_MOUTH_ERROR;
            }
            mouth->state = HSTEX_MOUTH_MIDDLE_LINE;
            *token = hstex_token_control_sequence(identifier);
            *location = character.location;
            return HSTEX_MOUTH_TOKEN;
        }
        case HSTEX_CAT_COMMENT:
            mouth->line_cursor = mouth->line_raw_length;
            mouth->line_loaded = false;
            mouth->state = HSTEX_MOUTH_NEW_LINE;
            continue;
        case HSTEX_CAT_INVALID:
            /* Read past and handed up: the engine reports it and goes on. */
            mouth->state = HSTEX_MOUTH_MIDDLE_LINE;
            *location = character.location;
            return HSTEX_MOUTH_INVALID;
        case HSTEX_CAT_BEGIN_GROUP:
        case HSTEX_CAT_END_GROUP:
        case HSTEX_CAT_MATH_SHIFT:
        case HSTEX_CAT_ALIGNMENT_TAB:
        case HSTEX_CAT_PARAMETER:
        case HSTEX_CAT_SUPERSCRIPT:
        case HSTEX_CAT_SUBSCRIPT:
        case HSTEX_CAT_LETTER:
        case HSTEX_CAT_OTHER:
            mouth->state = HSTEX_MOUTH_MIDDLE_LINE;
            *token = hstex_token_character(category, character.value);
            *location = character.location;
            return HSTEX_MOUTH_TOKEN;
        }
    }
}
