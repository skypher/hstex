#include "type1.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TYPE1_CHARSTRING_KEY = 4330,
    TYPE1_EEXEC_KEY = 55665,
    TYPE1_C1 = 52845,
    TYPE1_C2 = 22719,
};

struct type1_buffer {
    uint8_t *bytes;
    size_t count;
    size_t capacity;
};

struct type1_operator {
    const char *name;
    uint8_t first;
    uint8_t second;
    size_t length;
};

static const struct type1_operator type1_operators[] = {
    {"hstem", 1U, 0U, 1U},
    {"vstem", 3U, 0U, 1U},
    {"vmoveto", 4U, 0U, 1U},
    {"rlineto", 5U, 0U, 1U},
    {"hlineto", 6U, 0U, 1U},
    {"vlineto", 7U, 0U, 1U},
    {"rrcurveto", 8U, 0U, 1U},
    {"closepath", 9U, 0U, 1U},
    {"callsubr", 10U, 0U, 1U},
    {"return", 11U, 0U, 1U},
    {"hsbw", 13U, 0U, 1U},
    {"endchar", 14U, 0U, 1U},
    {"rmoveto", 21U, 0U, 1U},
    {"hmoveto", 22U, 0U, 1U},
    {"vhcurveto", 30U, 0U, 1U},
    {"hvcurveto", 31U, 0U, 1U},
    {"dotsection", 12U, 0U, 2U},
    {"vstem3", 12U, 1U, 2U},
    {"hstem3", 12U, 2U, 2U},
    {"seac", 12U, 6U, 2U},
    {"sbw", 12U, 7U, 2U},
    {"div", 12U, 12U, 2U},
    {"callothersubr", 12U, 16U, 2U},
    {"pop", 12U, 17U, 2U},
    {"setcurrentpoint", 12U, 33U, 2U},
};

static int type1_error(char *error, size_t capacity, const char *format, ...)
{
    if (error != NULL && capacity != 0U) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(error, capacity, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static int type1_buffer_append(struct type1_buffer *buffer, const void *bytes,
                               size_t count, char *error,
                               size_t error_capacity)
{
    if (count > SIZE_MAX - buffer->count) {
        return type1_error(error, error_capacity,
                           "Type 1 assembly buffer overflow");
    }
    size_t wanted = buffer->count + count;
    if (wanted > buffer->capacity) {
        size_t capacity = buffer->capacity == 0U ? 4096U : buffer->capacity;
        while (capacity < wanted) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = wanted;
                break;
            }
            capacity *= 2U;
        }
        uint8_t *grown = realloc(buffer->bytes, capacity);
        if (grown == NULL) {
            return type1_error(error, error_capacity,
                               "Type 1 assembly allocation failed");
        }
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    if (count != 0U) {
        memcpy(buffer->bytes + buffer->count, bytes, count);
    }
    buffer->count = wanted;
    return 0;
}

static int type1_buffer_byte(struct type1_buffer *buffer, uint8_t byte,
                             char *error, size_t error_capacity)
{
    return type1_buffer_append(buffer, &byte, 1U, error, error_capacity);
}

static int type1_buffer_zeroes(struct type1_buffer *buffer, size_t count,
                               char *error, size_t error_capacity)
{
    if (count > SIZE_MAX - buffer->count) {
        return type1_error(error, error_capacity,
                           "Type 1 assembly buffer overflow");
    }
    size_t wanted = buffer->count + count;
    if (wanted > buffer->capacity) {
        size_t capacity = buffer->capacity == 0U ? 4096U : buffer->capacity;
        while (capacity < wanted) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = wanted;
                break;
            }
            capacity *= 2U;
        }
        uint8_t *grown = realloc(buffer->bytes, capacity);
        if (grown == NULL) {
            return type1_error(error, error_capacity,
                               "Type 1 assembly allocation failed");
        }
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    if (count != 0U) {
        memset(buffer->bytes + buffer->count, 0, count);
    }
    buffer->count = wanted;
    return 0;
}

static const uint8_t *type1_find(const uint8_t *start, const uint8_t *finish,
                                 const char *text)
{
    size_t length = strlen(text);
    if (length == 0U || (size_t)(finish - start) < length) {
        return NULL;
    }
    const uint8_t *last = finish - length;
    for (const uint8_t *at = start; at <= last; ++at) {
        if (memcmp(at, text, length) == 0) {
            return at;
        }
    }
    return NULL;
}

static bool type1_space(uint8_t byte)
{
    return byte == (uint8_t)' ' || byte == (uint8_t)'\t' ||
           byte == (uint8_t)'\r' || byte == (uint8_t)'\n' ||
           byte == (uint8_t)'\f' || byte == 0U;
}

static bool type1_horizontal_space(uint8_t byte)
{
    return byte == (uint8_t)' ' || byte == (uint8_t)'\t' ||
           byte == (uint8_t)'\r' || byte == (uint8_t)'\f';
}

static const uint8_t *type1_line_after(const uint8_t *start,
                                       const uint8_t *finish)
{
    const uint8_t *newline = memchr(start, '\n', (size_t)(finish - start));
    return newline == NULL ? finish : newline + 1;
}

static const uint8_t *type1_line_content_finish(const uint8_t *start,
                                                const uint8_t *after)
{
    const uint8_t *finish = after;
    if (finish > start && finish[-1] == (uint8_t)'\n') {
        --finish;
    }
    if (finish > start && finish[-1] == (uint8_t)'\r') {
        --finish;
    }
    return finish;
}

static const uint8_t *type1_skip_horizontal(const uint8_t *at,
                                            const uint8_t *finish)
{
    while (at < finish && type1_horizontal_space(*at)) {
        ++at;
    }
    return at;
}

static bool type1_header_tail(const uint8_t *at, const uint8_t *finish)
{
    at = type1_skip_horizontal(at, finish);
    return at == finish || *at == (uint8_t)'%';
}

static const uint8_t *type1_subr_brace(const uint8_t *start,
                                       const uint8_t *finish)
{
    const uint8_t *at = type1_skip_horizontal(start, finish);
    if ((size_t)(finish - at) < 3U || memcmp(at, "dup", 3U) != 0) {
        return NULL;
    }
    at += 3U;
    if (at == finish || !type1_horizontal_space(*at)) {
        return NULL;
    }
    at = type1_skip_horizontal(at, finish);
    if (at == finish || *at < (uint8_t)'0' || *at > (uint8_t)'9') {
        return NULL;
    }
    while (at < finish && *at >= (uint8_t)'0' && *at <= (uint8_t)'9') {
        ++at;
    }
    at = type1_skip_horizontal(at, finish);
    if (at == finish || *at != (uint8_t)'{' ||
        !type1_header_tail(at + 1, finish)) {
        return NULL;
    }
    return at;
}

static const uint8_t *type1_glyph_brace(const uint8_t *start,
                                        const uint8_t *finish)
{
    const uint8_t *at = type1_skip_horizontal(start, finish);
    if (at == finish || *at != (uint8_t)'/') {
        return NULL;
    }
    const uint8_t *name = ++at;
    while (at < finish && !type1_horizontal_space(*at) &&
           *at != (uint8_t)'{') {
        ++at;
    }
    if (at == name) {
        return NULL;
    }
    at = type1_skip_horizontal(at, finish);
    if (at == finish || *at != (uint8_t)'{' ||
        !type1_header_tail(at + 1, finish)) {
        return NULL;
    }
    return at;
}

static int type1_parse_integer(const uint8_t *start, const uint8_t *finish,
                               int32_t *value)
{
    const uint8_t *at = start;
    bool negative = false;
    if (at < finish && (*at == (uint8_t)'-' || *at == (uint8_t)'+')) {
        negative = *at == (uint8_t)'-';
        ++at;
    }
    if (at == finish || *at < (uint8_t)'0' || *at > (uint8_t)'9') {
        return -1;
    }
    uint64_t magnitude = 0U;
    uint64_t limit = negative ? (uint64_t)INT32_MAX + 1U
                              : (uint64_t)INT32_MAX;
    while (at < finish) {
        if (*at < (uint8_t)'0' || *at > (uint8_t)'9') {
            return -1;
        }
        uint64_t digit = (uint64_t)(*at - (uint8_t)'0');
        if (magnitude > (limit - digit) / 10U) {
            return -1;
        }
        magnitude = magnitude * 10U + digit;
        ++at;
    }
    if (negative) {
        *value = magnitude == (uint64_t)INT32_MAX + 1U
                     ? INT32_MIN
                     : -(int32_t)magnitude;
    } else {
        *value = (int32_t)magnitude;
    }
    return 0;
}

static int type1_encode_number(struct type1_buffer *output, int32_t value,
                               char *error, size_t error_capacity)
{
    uint8_t bytes[5];
    size_t count = 0U;
    if (value >= -107 && value <= 107) {
        bytes[count++] = (uint8_t)(value + 139);
    } else if (value >= 108 && value <= 1131) {
        uint32_t adjusted = (uint32_t)(value - 108);
        bytes[count++] = (uint8_t)(247U + adjusted / 256U);
        bytes[count++] = (uint8_t)(adjusted % 256U);
    } else if (value >= -1131 && value <= -108) {
        uint32_t adjusted = (uint32_t)(-(int64_t)value - 108);
        bytes[count++] = (uint8_t)(251U + adjusted / 256U);
        bytes[count++] = (uint8_t)(adjusted % 256U);
    } else {
        uint32_t encoded = (uint32_t)value;
        bytes[count++] = 255U;
        bytes[count++] = (uint8_t)(encoded >> 24U);
        bytes[count++] = (uint8_t)(encoded >> 16U);
        bytes[count++] = (uint8_t)(encoded >> 8U);
        bytes[count++] = (uint8_t)encoded;
    }
    return type1_buffer_append(output, bytes, count, error, error_capacity);
}

static int type1_encode_operator(struct type1_buffer *output,
                                 const uint8_t *start, const uint8_t *finish,
                                 char *error, size_t error_capacity)
{
    size_t token_length = (size_t)(finish - start);
    for (size_t index = 0U;
         index < sizeof(type1_operators) / sizeof(type1_operators[0]);
         ++index) {
        const struct type1_operator *operator = &type1_operators[index];
        if (strlen(operator->name) != token_length ||
            memcmp(operator->name, start, token_length) != 0) {
            continue;
        }
        uint8_t bytes[2] = {operator->first, operator->second};
        return type1_buffer_append(output, bytes, operator->length, error,
                                   error_capacity);
    }
    size_t shown = token_length < 80U ? token_length : 80U;
    return type1_error(error, error_capacity,
                       "unsupported Type 1 charstring token '%.*s'",
                       (int)shown, (const char *)start);
}

static void type1_encrypt(uint8_t *bytes, size_t count, uint16_t key)
{
    uint16_t state = key;
    for (size_t index = 0U; index < count; ++index) {
        uint8_t cipher = (uint8_t)(bytes[index] ^ (uint8_t)(state >> 8U));
        bytes[index] = cipher;
        state = (uint16_t)(((uint32_t)cipher + (uint32_t)state) *
                               (uint32_t)TYPE1_C1 +
                           (uint32_t)TYPE1_C2);
    }
}

static int type1_encode_charstring(const uint8_t *start,
                                   const uint8_t *finish, int32_t len_iv,
                                   struct type1_buffer *output, char *error,
                                   size_t error_capacity)
{
    if (len_iv < -1) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 lenIV value");
    }
    if (len_iv >= 0 &&
        type1_buffer_zeroes(output, (size_t)len_iv, error, error_capacity) !=
            0) {
        return -1;
    }
    const uint8_t *at = start;
    while (at < finish) {
        while (at < finish && type1_space(*at)) {
            ++at;
        }
        if (at == finish) {
            break;
        }
        if (*at == (uint8_t)'%') {
            at = type1_line_after(at, finish);
            continue;
        }
        const uint8_t *token = at;
        while (at < finish && !type1_space(*at) &&
               *at != (uint8_t)'%') {
            ++at;
        }
        int32_t number = 0;
        int status = type1_parse_integer(token, at, &number) == 0
                         ? type1_encode_number(output, number, error,
                                               error_capacity)
                         : type1_encode_operator(output, token, at, error,
                                                 error_capacity);
        if (status != 0) {
            return -1;
        }
        if (at < finish && *at == (uint8_t)'%') {
            at = type1_line_after(at, finish);
        }
    }
    if (len_iv >= 0) {
        type1_encrypt(output->bytes, output->count,
                      (uint16_t)TYPE1_CHARSTRING_KEY);
    }
    return 0;
}

static int type1_find_entry_finish(const uint8_t *start,
                                   const uint8_t *finish,
                                   const char *terminator,
                                   const uint8_t **brace,
                                   const uint8_t **after_terminator,
                                   const uint8_t **after_line, char *error,
                                   size_t error_capacity)
{
    size_t terminator_length = strlen(terminator);
    const uint8_t *line = start;
    while (line < finish) {
        const uint8_t *line_after = type1_line_after(line, finish);
        const uint8_t *line_finish =
            type1_line_content_finish(line, line_after);
        const uint8_t *at = type1_skip_horizontal(line, line_finish);
        if (at < line_finish && *at == (uint8_t)'}') {
            const uint8_t *name = type1_skip_horizontal(at + 1, line_finish);
            if ((size_t)(line_finish - name) >= terminator_length) {
                const uint8_t *name_finish = name + terminator_length;
                if (memcmp(name, terminator, terminator_length) == 0 &&
                    (name_finish == line_finish ||
                     type1_horizontal_space(*name_finish) ||
                     *name_finish == (uint8_t)'%')) {
                    *brace = at;
                    *after_terminator = name_finish;
                    *after_line = line_after;
                    return 0;
                }
            }
        }
        line = line_after;
    }
    return type1_error(error, error_capacity,
                       "unterminated Type 1 charstring entry");
}

static int type1_append_entry(struct type1_buffer *output,
                              const uint8_t *line,
                              const uint8_t *section_finish,
                              const uint8_t *opening_brace,
                              const char *terminator, int32_t len_iv,
                              const uint8_t **next, char *error,
                              size_t error_capacity)
{
    const uint8_t *closing_brace = NULL;
    const uint8_t *after_terminator = NULL;
    const uint8_t *after_line = NULL;
    if (type1_find_entry_finish(opening_brace + 1, section_finish, terminator,
                                &closing_brace, &after_terminator, &after_line,
                                error, error_capacity) != 0) {
        return -1;
    }
    struct type1_buffer charstring = {0};
    if (type1_encode_charstring(opening_brace + 1, closing_brace, len_iv,
                                &charstring, error, error_capacity) != 0) {
        free(charstring.bytes);
        return -1;
    }
    int status = type1_buffer_append(output, line,
                                     (size_t)(opening_brace - line), error,
                                     error_capacity);
    if (status == 0 && opening_brace > line &&
        !type1_horizontal_space(opening_brace[-1])) {
        status = type1_buffer_byte(output, (uint8_t)' ', error,
                                   error_capacity);
    }
    char length[32];
    int width = snprintf(length, sizeof(length), "%zu", charstring.count);
    static const char read_string[] = " RD ";
    if (status == 0 &&
        (width < 0 || (size_t)width >= sizeof(length))) {
        status = type1_error(error, error_capacity,
                             "Type 1 charstring length is too large");
    }
    if (status == 0) {
        status = type1_buffer_append(output, length, (size_t)width, error,
                                     error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_append(output, read_string,
                                     sizeof(read_string) - 1U, error,
                                     error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_append(output, charstring.bytes,
                                     charstring.count, error, error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_byte(output, (uint8_t)' ', error,
                                   error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_append(output, terminator, strlen(terminator),
                                     error, error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_append(
            output, after_terminator,
            (size_t)(after_line - after_terminator), error, error_capacity);
    }
    free(charstring.bytes);
    if (status != 0) {
        return -1;
    }
    *next = after_line;
    return 0;
}

static int type1_transform_entries(struct type1_buffer *output,
                                   const uint8_t *start,
                                   const uint8_t *finish, bool subroutines,
                                   int32_t len_iv, char *error,
                                   size_t error_capacity)
{
    const char *terminator = subroutines ? "NP" : "ND";
    const uint8_t *line = start;
    while (line < finish) {
        const uint8_t *after_line = type1_line_after(line, finish);
        const uint8_t *line_finish =
            type1_line_content_finish(line, after_line);
        const uint8_t *brace =
            subroutines ? type1_subr_brace(line, line_finish)
                        : type1_glyph_brace(line, line_finish);
        if (brace == NULL) {
            if (type1_buffer_append(output, line,
                                    (size_t)(after_line - line), error,
                                    error_capacity) != 0) {
                return -1;
            }
            line = after_line;
            continue;
        }
        if (type1_append_entry(output, line, finish, brace, terminator, len_iv,
                               &line, error, error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}

static int type1_private_len_iv(const uint8_t *start, const uint8_t *finish,
                                int32_t *len_iv, char *error,
                                size_t error_capacity)
{
    const uint8_t *at = type1_find(start, finish, "/lenIV");
    if (at == NULL) {
        *len_iv = 4;
        return 0;
    }
    at += strlen("/lenIV");
    while (at < finish && type1_space(*at)) {
        ++at;
    }
    const uint8_t *number = at;
    while (at < finish && !type1_space(*at)) {
        ++at;
    }
    if (type1_parse_integer(number, at, len_iv) != 0 || *len_iv < -1) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 lenIV value");
    }
    return 0;
}

int hstex_type1_assemble(const uint8_t *disassembly,
                         size_t disassembly_length, uint8_t **program,
                         size_t *program_length, size_t *length1,
                         size_t *length2, char *error,
                         size_t error_capacity)
{
    if (program == NULL || program_length == NULL || length1 == NULL ||
        length2 == NULL) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 assembly destination");
    }
    *program = NULL;
    *program_length = 0U;
    *length1 = 0U;
    *length2 = 0U;
    if (disassembly == NULL) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 disassembly");
    }
    const uint8_t *finish = disassembly + disassembly_length;
    static const char eexec_text[] = "currentfile eexec";
    const uint8_t *eexec = type1_find(disassembly, finish, eexec_text);
    if (eexec == NULL) {
        return type1_error(error, error_capacity,
                           "Type 1 disassembly has no eexec section");
    }
    const uint8_t *public_finish =
        type1_line_after(eexec + sizeof(eexec_text) - 1U, finish);
    static const char close_text[] = "mark currentfile closefile";
    const uint8_t *close = type1_find(public_finish, finish, close_text);
    if (close == NULL) {
        return type1_error(error, error_capacity,
                           "Type 1 disassembly has no private-section end");
    }
    const uint8_t *private_finish =
        type1_line_after(close + sizeof(close_text) - 1U, finish);
    const uint8_t *subrs = type1_find(public_finish, close, "/Subrs ");
    const uint8_t *charstrings =
        subrs == NULL ? NULL : type1_find(subrs, close, "/CharStrings ");
    if (subrs == NULL || charstrings == NULL) {
        return type1_error(error, error_capacity,
                           "Type 1 disassembly has no character dictionaries");
    }
    const uint8_t *subr_entries = type1_line_after(subrs, charstrings);
    const uint8_t *glyph_entries = type1_line_after(charstrings, close);
    int32_t len_iv = 4;
    if (type1_private_len_iv(public_finish, subrs, &len_iv, error,
                             error_capacity) != 0) {
        return -1;
    }

    struct type1_buffer private = {0};
    int status = type1_buffer_zeroes(&private, 4U, error, error_capacity);
    if (status == 0) {
        status = type1_buffer_append(&private, public_finish,
                                     (size_t)(subr_entries - public_finish),
                                     error, error_capacity);
    }
    if (status == 0) {
        status = type1_transform_entries(&private, subr_entries, charstrings,
                                         true, len_iv, error,
                                         error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_append(&private, charstrings,
                                     (size_t)(glyph_entries - charstrings),
                                     error, error_capacity);
    }
    if (status == 0) {
        status = type1_transform_entries(&private, glyph_entries,
                                         private_finish, false, len_iv, error,
                                         error_capacity);
    }
    if (status != 0) {
        free(private.bytes);
        return -1;
    }
    type1_encrypt(private.bytes, private.count, (uint16_t)TYPE1_EEXEC_KEY);

    size_t public_length = (size_t)(public_finish - disassembly);
    if (private.count > SIZE_MAX - public_length) {
        free(private.bytes);
        return type1_error(error, error_capacity,
                           "Type 1 assembly buffer overflow");
    }
    size_t total = public_length + private.count;
    uint8_t *assembled = malloc(total == 0U ? 1U : total);
    if (assembled == NULL) {
        free(private.bytes);
        return type1_error(error, error_capacity,
                           "Type 1 assembly allocation failed");
    }
    memcpy(assembled, disassembly, public_length);
    memcpy(assembled + public_length, private.bytes, private.count);
    free(private.bytes);
    *program = assembled;
    *program_length = total;
    *length1 = public_length;
    *length2 = total - public_length;
    return 0;
}
