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
    if (index < mouth->line_raw_length) {
        return mouth->line_buffer[index];
    }
    return mouth->end_line_byte;
}

static int reserve_line(struct hstex_mouth *mouth, size_t required, char *error,
                        size_t error_capacity)
{
    if (required <= mouth->line_capacity) {
        return 0;
    }
    size_t capacity = mouth->line_capacity == 0U ? 128U : mouth->line_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "input line scratch overflow");
        }
        capacity *= 2U;
    }
    uint8_t *allocation = realloc(mouth->line_buffer, capacity);
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "input line scratch allocation failed");
    }
    mouth->line_buffer = allocation;
    mouth->line_capacity = capacity;
    return 0;
}

/* WHAT A COLLAPSE DOES TO THE LINE. The reference writes the character the
   notation stood for where the first `^' was and shifts what follows down
   over the bytes that go, so the line an error draws afterwards shows the
   character and not the notation it was written with. `gone' is two for
   `^^X' and three for `^^xx'. */
static void collapse_line(struct hstex_mouth *mouth, size_t index,
                          uint8_t value, size_t gone)
{
    mouth->line_buffer[index] = value;
    size_t tail = index + 1U + gone;
    if (tail < mouth->line_raw_length) {
        memmove(mouth->line_buffer + index + 1U, mouth->line_buffer + tail,
                mouth->line_raw_length - tail);
    }
    mouth->line_raw_length -= gone;
    /* What the line SHOWS stops short of the end-of-line character while
       that is still what stands last. A collapse that ate it leaves the
       whole of what is left to show -- which is what the reference does
       when `buffer[limit]' is no longer the end-of-line character. */
    mouth->line_content_length =
        mouth->has_end_line_byte && mouth->line_raw_length != 0U &&
                mouth->line_buffer[mouth->line_raw_length - 1U] ==
                    mouth->end_line_byte
            ? mouth->line_raw_length - 1U
            : mouth->line_raw_length;
    mouth->line_cursor = index + 1U;
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

/* -1 if the line could not be taken, 0 at the end of the input, 1 if a line
   is now loaded. */
static int load_line(struct hstex_mouth *mouth, char *error,
                     size_t error_capacity)
{
    if (mouth->next_line_offset >= mouth->length) {
        return 0;
    }
    if (mouth->line_number == UINT32_MAX) {
        return set_error(error, error_capacity,
                         "input has more than 2^32-1 lines");
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

    int32_t end_line_character = mouth->lexical_state->end_line_character;
    bool has_end_line_byte = end_line_character >= 0 && end_line_character <= 255;
    size_t content = end - start;
    size_t raw = content + (has_end_line_byte ? 1U : 0U);
    /* The end-of-line character is held IN the line, where the reference
       holds it, so that `^^' notation may collapse across it. */
    if (reserve_line(mouth, raw == 0U ? 1U : raw, error, error_capacity) != 0) {
        return -1;
    }
    if (content != 0U) {
        memcpy(mouth->line_buffer, mouth->data + start, content);
    }
    if (has_end_line_byte) {
        mouth->line_buffer[content] = (uint8_t)end_line_character;
    }

    mouth->next_line_offset = next;
    mouth->line_start = start;
    mouth->line_content_length = content;
    mouth->line_cursor = 0U;
    ++mouth->line_number;
    mouth->state = HSTEX_MOUTH_NEW_LINE;
    mouth->has_end_line_byte = has_end_line_byte;
    mouth->end_line_byte = has_end_line_byte ? (uint8_t)end_line_character : 0U;
    mouth->line_raw_length = raw;
    mouth->line_loaded = true;
    return 1;
}

/* WHERE A COLLAPSE IS WRITTEN BACK. The reference reduces `^^' notation in
   two places and they do not behave alike. Reading a control sequence's NAME
   it rewrites the line -- it has to, the name being built from bytes that
   must end up next to each other -- so `\catcode`\qq1qM' leaves `^^M'
   standing where the notation was. Reading an ordinary character it only
   steps over the notation and leaves the line alone, so `By ^^p' still says
   `^^p' after the `0' has been read from it. An error draws the line either
   way, which is how the difference shows. `rewrite' says which of the two
   this reading is. */
static bool read_logical_character(struct hstex_mouth *mouth,
                                   struct logical_character *character,
                                   bool rewrite)
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
        size_t gone = 2U;
        if (mouth->line_cursor + 2U < mouth->line_raw_length &&
            is_lower_hexadecimal(third)) {
            uint8_t fourth = raw_character(mouth, mouth->line_cursor + 2U);
            if (is_lower_hexadecimal(fourth)) {
                current = (uint8_t)((hexadecimal_value(third) << 4U) |
                                    hexadecimal_value(fourth));
                gone = 3U;
            }
        }
        if (gone == 2U) {
            current = third < UINT8_C(64) ? (uint8_t)(third + UINT8_C(64))
                                          : (uint8_t)(third - UINT8_C(64));
        }
        if (rewrite) {
            /* The line keeps what was collapsed, and the walk goes on from
               just past it -- so `qq1qM' becomes `qM' and then the one
               character that is, exactly as the reference reduces it. */
            collapse_line(mouth, first_index, current, gone);
        } else {
            mouth->line_cursor += gone;
        }
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
    if (!read_logical_character(mouth, &first, true)) {
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
            if (!read_logical_character(mouth, &next, true)) {
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
    free(mouth->line_buffer);
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
            int loaded = load_line(mouth, error, error_capacity);
            if (loaded < 0) {
                return HSTEX_MOUTH_ERROR;
            }
            if (loaded == 0) {
                return HSTEX_MOUTH_EOF;
            }
        }
        if (mouth->line_cursor >= mouth->line_raw_length) {
            mouth->line_loaded = false;
            continue;
        }

        struct logical_character character;
        if (!read_logical_character(mouth, &character, false)) {
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
            /* Read past and handed up: the engine reports it and goes on.
               It LEAVES THE STATE ALONE, so a space behind one is skipped
               where a space behind a letter is not: `\zz@ A' draws no blank
               space, and neither does an invalid character at the head of a
               line. trip line 351 ends `\a^^@^^@a@ %'. */
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
