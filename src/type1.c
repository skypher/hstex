#include "type1.h"

#include "internal.h"

#include <inttypes.h>
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
    HSTEX_PRINTF_FORMAT(3, 4);

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
                           "Type 1 buffer overflow");
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
                               "Type 1 buffer allocation failed");
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
                           "Type 1 buffer overflow");
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
                               "Type 1 buffer allocation failed");
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

static const uint8_t *type1_text_line_after(const uint8_t *start,
                                            const uint8_t *finish,
                                            const uint8_t **content_finish)
{
    const uint8_t *at = start;
    while (at < finish && *at != (uint8_t)'\r' &&
           *at != (uint8_t)'\n') {
        ++at;
    }
    *content_finish = at;
    if (at < finish && *at == (uint8_t)'\r') {
        ++at;
        if (at < finish && *at == (uint8_t)'\n') {
            ++at;
        }
    } else if (at < finish) {
        ++at;
    }
    return at;
}

static bool type1_zero_line(const uint8_t *start, const uint8_t *finish)
{
    if (start == finish || *start != (uint8_t)'0') {
        return false;
    }
    for (const uint8_t *at = start; at < finish; ++at) {
        if (*at != (uint8_t)'0') {
            return false;
        }
    }
    return true;
}

static int type1_append_normalized_text(struct type1_buffer *output,
                                        const uint8_t *start,
                                        const uint8_t *finish,
                                        bool omit_zero_lines, char *error,
                                        size_t error_capacity)
{
    const uint8_t *line = start;
    while (line < finish) {
        const uint8_t *content_finish = NULL;
        const uint8_t *after =
            type1_text_line_after(line, finish, &content_finish);
        if (!omit_zero_lines || !type1_zero_line(line, content_finish)) {
            if (type1_buffer_append(output, line,
                                    (size_t)(content_finish - line), error,
                                    error_capacity) != 0) {
                return -1;
            }
            if (after > content_finish &&
                type1_buffer_byte(output, (uint8_t)'\n', error,
                                  error_capacity) != 0) {
                return -1;
            }
        }
        line = after;
    }
    return 0;
}

static const struct type1_operator *type1_operator_by_code(uint8_t first,
                                                            uint8_t second,
                                                            size_t length)
{
    for (size_t index = 0U;
         index < sizeof(type1_operators) / sizeof(type1_operators[0]);
         ++index) {
        const struct type1_operator *operator = &type1_operators[index];
        if (operator->first == first && operator->second == second &&
            operator->length == length) {
            return operator;
        }
    }
    return NULL;
}

static int type1_disassembly_token(struct type1_buffer *output,
                                   const char *token, bool *line_start,
                                   char *error, size_t error_capacity)
{
    uint8_t separator = *line_start ? (uint8_t)'\t' : (uint8_t)' ';
    if (type1_buffer_byte(output, separator, error, error_capacity) != 0 ||
        type1_buffer_append(output, token, strlen(token), error,
                            error_capacity) != 0) {
        return -1;
    }
    *line_start = false;
    return 0;
}

static int type1_disassemble_charstring(const uint8_t *cipher,
                                        size_t cipher_count, int32_t len_iv,
                                        struct type1_buffer *output,
                                        char *error, size_t error_capacity)
{
    if (len_iv < -1 ||
        (len_iv >= 0 && (size_t)len_iv > cipher_count)) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 charstring prefix");
    }
    uint8_t *plain = malloc(cipher_count == 0U ? 1U : cipher_count);
    if (plain == NULL) {
        return type1_error(error, error_capacity,
                           "Type 1 charstring allocation failed");
    }
    if (cipher_count != 0U) {
        memcpy(plain, cipher, cipher_count);
    }
    size_t at = 0U;
    if (len_iv >= 0) {
        uint16_t state = (uint16_t)TYPE1_CHARSTRING_KEY;
        for (size_t index = 0U; index < cipher_count; ++index) {
            uint8_t byte = plain[index];
            plain[index] =
                (uint8_t)(byte ^ (uint8_t)(state >> 8U));
            state = (uint16_t)(((uint32_t)byte + (uint32_t)state) *
                                   (uint32_t)TYPE1_C1 +
                               (uint32_t)TYPE1_C2);
        }
        at = (size_t)len_iv;
    }
    bool line_start = true;
    int status = 0;
    while (status == 0 && at < cipher_count) {
        uint8_t first = plain[at++];
        if (first >= 32U) {
            int32_t value = 0;
            if (first <= 246U) {
                value = (int32_t)first - 139;
            } else if (first <= 250U) {
                if (at == cipher_count) {
                    status = type1_error(
                        error, error_capacity,
                        "truncated Type 1 positive integer");
                    break;
                }
                value = ((int32_t)first - 247) * 256 + 108 +
                        (int32_t)plain[at++];
            } else if (first <= 254U) {
                if (at == cipher_count) {
                    status = type1_error(
                        error, error_capacity,
                        "truncated Type 1 negative integer");
                    break;
                }
                value = -(((int32_t)first - 251) * 256) - 108 -
                        (int32_t)plain[at++];
            } else {
                if (cipher_count - at < 4U) {
                    status = type1_error(error, error_capacity,
                                         "truncated Type 1 integer");
                    break;
                }
                uint32_t encoded = (uint32_t)plain[at] << 24U |
                                   (uint32_t)plain[at + 1U] << 16U |
                                   (uint32_t)plain[at + 2U] << 8U |
                                   (uint32_t)plain[at + 3U];
                value = encoded <= (uint32_t)INT32_MAX
                            ? (int32_t)encoded
                            : -1 - (int32_t)(UINT32_MAX - encoded);
                at += 4U;
            }
            char number[32];
            int length = snprintf(number, sizeof(number), "%" PRId32,
                                  value);
            if (length < 0 || (size_t)length >= sizeof(number)) {
                status = type1_error(error, error_capacity,
                                     "Type 1 integer formatting failed");
            } else {
                status = type1_disassembly_token(
                    output, number, &line_start, error, error_capacity);
            }
            continue;
        }
        uint8_t second = 0U;
        size_t operator_length = 1U;
        if (first == 12U) {
            if (at == cipher_count) {
                status = type1_error(error, error_capacity,
                                     "truncated Type 1 escape operator");
                break;
            }
            second = plain[at++];
            operator_length = 2U;
        }
        const struct type1_operator *operator =
            type1_operator_by_code(first, second, operator_length);
        if (operator == NULL) {
            status = operator_length == 1U
                         ? type1_error(error, error_capacity,
                                       "unsupported Type 1 operator %u",
                                       (unsigned int)first)
                         : type1_error(
                               error, error_capacity,
                               "unsupported Type 1 escape operator %u",
                               (unsigned int)second);
            break;
        }
        status = type1_disassembly_token(output, operator->name, &line_start,
                                         error, error_capacity);
        if (status == 0) {
            status = type1_buffer_byte(output, (uint8_t)'\n', error,
                                       error_capacity);
            line_start = true;
        }
    }
    free(plain);
    return status;
}

static int type1_parse_size(const uint8_t *start, const uint8_t *finish,
                            size_t *value)
{
    if (start == finish || *start < (uint8_t)'0' ||
        *start > (uint8_t)'9') {
        return -1;
    }
    size_t parsed = 0U;
    for (const uint8_t *at = start; at < finish; ++at) {
        if (*at < (uint8_t)'0' || *at > (uint8_t)'9') {
            return -1;
        }
        size_t digit = (size_t)(*at - (uint8_t)'0');
        if (parsed > (SIZE_MAX - digit) / 10U) {
            return -1;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return 0;
}

static bool type1_disassembly_entry(const uint8_t *start,
                                    const uint8_t *finish,
                                    const char *charstring_start,
                                    size_t charstring_start_length,
                                    size_t *prefix_length,
                                    size_t *charstring_length,
                                    const uint8_t **charstring)
{
    if (charstring_start_length == 0U) {
        return false;
    }
    const uint8_t *at = start;
    while (at < finish && type1_horizontal_space(*at)) {
        ++at;
    }
    while (at < finish && !type1_horizontal_space(*at) &&
           *at != (uint8_t)'\n') {
        ++at;
    }
    if (at == finish || !type1_horizontal_space(*at)) {
        return false;
    }
    const uint8_t *first_space = at;
    at = type1_skip_horizontal(at, finish);
    const uint8_t *digits = at;
    while (at < finish && *at >= (uint8_t)'0' &&
           *at <= (uint8_t)'9') {
        ++at;
    }
    if (at == digits) {
        return false;
    }
    if (at < finish && type1_horizontal_space(*at)) {
        const uint8_t *next = type1_skip_horizontal(at, finish);
        if (next < finish && *next >= (uint8_t)'0' &&
            *next <= (uint8_t)'9') {
            first_space = at;
            digits = next;
            at = next;
            while (at < finish && *at >= (uint8_t)'0' &&
                   *at <= (uint8_t)'9') {
                ++at;
            }
        }
    }
    if (at == finish || *at != (uint8_t)' ' ||
        (size_t)(finish - (at + 1)) < charstring_start_length + 1U ||
        memcmp(at + 1, charstring_start, charstring_start_length) != 0 ||
        at[1U + charstring_start_length] != (uint8_t)' ' ||
        type1_parse_size(digits, at, charstring_length) != 0) {
        return false;
    }
    const uint8_t *data = at + charstring_start_length + 2U;
    if (*charstring_length > (size_t)(finish - data)) {
        return false;
    }
    *prefix_length = (size_t)(first_space - start);
    *charstring = data;
    return true;
}

static void type1_disassembly_settings(const uint8_t *start,
                                       const uint8_t *finish, int32_t *len_iv,
                                       char *charstring_start,
                                       size_t *charstring_start_length)
{
    const uint8_t *len = type1_find(start, finish, "/lenIV ");
    if (len != NULL) {
        len += strlen("/lenIV ");
        const uint8_t *number_finish = len;
        while (number_finish < finish &&
               !type1_horizontal_space(*number_finish)) {
            ++number_finish;
        }
        int32_t parsed = 0;
        if (type1_parse_integer(len, number_finish, &parsed) == 0) {
            *len_iv = parsed;
        }
    }
    const uint8_t *reader =
        type1_find(start, finish, "string currentfile");
    if (reader == NULL || type1_find(start, finish, "readstring") == NULL) {
        return;
    }
    const uint8_t *name = reader;
    while (name > start && name[-1] != (uint8_t)'/') {
        --name;
    }
    if (name == start) {
        return;
    }
    const uint8_t *name_finish = name;
    while (name_finish < reader &&
           !type1_horizontal_space(*name_finish) &&
           *name_finish != (uint8_t)'{') {
        ++name_finish;
    }
    size_t length = (size_t)(name_finish - name);
    if (length == 0U || length >= 64U) {
        return;
    }
    memcpy(charstring_start, name, length);
    charstring_start[length] = '\0';
    *charstring_start_length = length;
}

static const uint8_t *type1_inline_charstrings(const uint8_t *start,
                                                const uint8_t *finish)
{
    const uint8_t *header = type1_find(start, finish, "/CharStrings ");
    if (header == NULL) {
        return NULL;
    }
    const uint8_t *begin = type1_find(header, finish, "dict dup begin");
    if (begin == NULL) {
        return NULL;
    }
    begin += strlen("dict dup begin");
    while (begin < finish && type1_space(*begin)) {
        ++begin;
    }
    return begin < finish && *begin == (uint8_t)'/' ? begin : NULL;
}

static int type1_disassemble_private(const uint8_t *start,
                                     const uint8_t *finish,
                                     struct type1_buffer *output, char *error,
                                     size_t error_capacity)
{
    int32_t len_iv = 4;
    char charstring_start[64] = {0};
    size_t charstring_start_length = 0U;
    const uint8_t *at = start;
    while (at < finish) {
        size_t prefix_length = 0U;
        size_t charstring_length = 0U;
        const uint8_t *charstring = NULL;
        if (type1_disassembly_entry(
                at, finish, charstring_start, charstring_start_length,
                &prefix_length, &charstring_length, &charstring)) {
            if (type1_buffer_append(output, at, prefix_length, error,
                                    error_capacity) != 0 ||
                type1_buffer_append(output, " {\n", 3U, error,
                                    error_capacity) != 0 ||
                type1_disassemble_charstring(
                    charstring, charstring_length, len_iv, output, error,
                    error_capacity) != 0 ||
                type1_buffer_append(output, "\t}", 2U, error,
                                    error_capacity) != 0) {
                return -1;
            }
            at = charstring + charstring_length;
            const uint8_t *content_finish = NULL;
            const uint8_t *after =
                type1_text_line_after(at, finish, &content_finish);
            if (type1_buffer_append(output, at,
                                    (size_t)(content_finish - at), error,
                                    error_capacity) != 0 ||
                (after > content_finish &&
                 type1_buffer_byte(output, (uint8_t)'\n', error,
                                   error_capacity) != 0)) {
                return -1;
            }
            at = after;
            continue;
        }
        const uint8_t *content_finish = NULL;
        const uint8_t *after =
            type1_text_line_after(at, finish, &content_finish);
        const uint8_t *inline_entry =
            type1_inline_charstrings(at, content_finish);
        if (inline_entry != NULL) {
            if (type1_buffer_append(output, at,
                                    (size_t)(inline_entry - at), error,
                                    error_capacity) != 0 ||
                type1_buffer_byte(output, (uint8_t)'\n', error,
                                  error_capacity) != 0) {
                return -1;
            }
            at = inline_entry;
            continue;
        }
        type1_disassembly_settings(at, content_finish, &len_iv,
                                   charstring_start,
                                   &charstring_start_length);
        if (type1_buffer_append(output, at,
                                (size_t)(content_finish - at), error,
                                error_capacity) != 0 ||
            (after > content_finish &&
             type1_buffer_byte(output, (uint8_t)'\n', error,
                               error_capacity) != 0)) {
            return -1;
        }
        at = after;
    }
    return 0;
}

static bool type1_private_close_line(const uint8_t *start,
                                     const uint8_t *finish)
{
    return type1_find(start, finish, "currentfile closefile") != NULL;
}

static int type1_decrypt_eexec(const uint8_t *cipher, size_t cipher_count,
                               struct type1_buffer *plain,
                               size_t *cipher_consumed, char *error,
                               size_t error_capacity)
{
    uint16_t state = (uint16_t)TYPE1_EEXEC_KEY;
    size_t line_start = 0U;
    for (size_t index = 0U; index < cipher_count; ++index) {
        uint8_t byte = cipher[index];
        uint8_t decrypted =
            (uint8_t)(byte ^ (uint8_t)(state >> 8U));
        state = (uint16_t)(((uint32_t)byte + (uint32_t)state) *
                               (uint32_t)TYPE1_C1 +
                           (uint32_t)TYPE1_C2);
        if (index < 4U) {
            continue;
        }
        if (type1_buffer_byte(plain, decrypted, error, error_capacity) != 0) {
            return -1;
        }
        if (decrypted != (uint8_t)'\r' &&
            decrypted != (uint8_t)'\n') {
            continue;
        }
        const uint8_t *line = plain->bytes + line_start;
        const uint8_t *line_finish = plain->bytes + plain->count - 1U;
        if (type1_private_close_line(line, line_finish)) {
            *cipher_consumed = index + 1U;
            if (decrypted == (uint8_t)'\r' && index + 1U < cipher_count) {
                uint8_t next_cipher = cipher[index + 1U];
                uint8_t next_plain =
                    (uint8_t)(next_cipher ^ (uint8_t)(state >> 8U));
                if (next_plain == (uint8_t)'\n' &&
                    type1_buffer_byte(plain, next_plain, error,
                                      error_capacity) != 0) {
                    return -1;
                }
                if (next_plain == (uint8_t)'\n') {
                    ++*cipher_consumed;
                }
            }
            return 0;
        }
        line_start = plain->count;
    }
    if (cipher_count < 4U) {
        return type1_error(error, error_capacity,
                           "truncated Type 1 eexec prefix");
    }
    if (line_start < plain->count &&
        type1_private_close_line(plain->bytes + line_start,
                                 plain->bytes + plain->count)) {
        *cipher_consumed = cipher_count;
        return 0;
    }
    return type1_error(error, error_capacity,
                       "Type 1 eexec section has no private-section end");
}

static uint32_t type1_little_endian_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U |
           (uint32_t)bytes[2] << 16U | (uint32_t)bytes[3] << 24U;
}

static int type1_disassemble_pfb(const uint8_t *font, size_t font_length,
                                 struct type1_buffer *output, char *error,
                                 size_t error_capacity)
{
    struct type1_buffer public = {0};
    struct type1_buffer cipher = {0};
    struct type1_buffer trailer = {0};
    bool binary_seen = false;
    bool trailer_seen = false;
    bool done = false;
    size_t at = 0U;
    int status = 0;
    while (status == 0 && at < font_length) {
        if (font_length - at < 2U || font[at] != 0x80U) {
            status = type1_error(error, error_capacity,
                                 "invalid Type 1 PFB segment marker");
            break;
        }
        uint8_t kind = font[at + 1U];
        at += 2U;
        if (kind == 3U) {
            done = true;
            if (at != font_length) {
                status = type1_error(error, error_capacity,
                                     "data follows Type 1 PFB end marker");
            }
            break;
        }
        if ((kind != 1U && kind != 2U) || font_length - at < 4U) {
            status = type1_error(error, error_capacity,
                                 "invalid Type 1 PFB segment header");
            break;
        }
        size_t count = (size_t)type1_little_endian_u32(font + at);
        at += 4U;
        if (count > font_length - at) {
            status = type1_error(error, error_capacity,
                                 "truncated Type 1 PFB segment");
            break;
        }
        if (kind == 2U) {
            if (trailer_seen) {
                status = type1_error(
                    error, error_capacity,
                    "Type 1 PFB binary segment follows its trailer");
            } else {
                binary_seen = true;
                status = type1_buffer_append(&cipher, font + at, count,
                                             error, error_capacity);
            }
        } else if (binary_seen) {
            trailer_seen = true;
            status = type1_buffer_append(&trailer, font + at, count, error,
                                         error_capacity);
        } else {
            status = type1_buffer_append(&public, font + at, count, error,
                                         error_capacity);
        }
        at += count;
    }
    if (status == 0 && !done) {
        status = type1_error(error, error_capacity,
                             "Type 1 PFB has no end marker");
    }
    if (status == 0 && !binary_seen) {
        status = type1_error(error, error_capacity,
                             "Type 1 PFB has no binary eexec segment");
    }
    if (status == 0 && public.count == 0U) {
        status = type1_error(error, error_capacity,
                             "Type 1 PFB has no public segment");
    }
    if (status == 0) {
        status = type1_append_normalized_text(
            output, public.bytes, public.bytes + public.count, false, error,
            error_capacity);
    }
    struct type1_buffer plain = {0};
    size_t cipher_consumed = 0U;
    if (status == 0) {
        status = type1_decrypt_eexec(cipher.bytes, cipher.count, &plain,
                                    &cipher_consumed, error,
                                    error_capacity);
    }
    if (status == 0) {
        status = type1_disassemble_private(
            plain.bytes, plain.bytes + plain.count, output, error,
            error_capacity);
    }
    struct type1_buffer tail = {0};
    if (status == 0 && cipher_consumed < cipher.count) {
        status = type1_buffer_append(
            &tail, cipher.bytes + cipher_consumed,
            cipher.count - cipher_consumed, error, error_capacity);
    }
    if (status == 0) {
        status = type1_buffer_append(&tail, trailer.bytes, trailer.count,
                                     error, error_capacity);
    }
    if (status == 0 && tail.count != 0U) {
        status = type1_append_normalized_text(
            output, tail.bytes, tail.bytes + tail.count, true, error,
            error_capacity);
    }
    free(public.bytes);
    free(cipher.bytes);
    free(trailer.bytes);
    free(plain.bytes);
    free(tail.bytes);
    return status;
}

static int type1_hex_value(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return (int)(byte - (uint8_t)'0');
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return (int)(byte - (uint8_t)'A') + 10;
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return (int)(byte - (uint8_t)'a') + 10;
    }
    return -1;
}

static bool type1_four_hex_digits(const uint8_t *at, const uint8_t *finish)
{
    return (size_t)(finish - at) >= 4U && type1_hex_value(at[0]) >= 0 &&
           type1_hex_value(at[1]) >= 0 && type1_hex_value(at[2]) >= 0 &&
           type1_hex_value(at[3]) >= 0;
}

static int type1_decode_hex_lines(const uint8_t *start,
                                  const uint8_t *finish,
                                  struct type1_buffer *cipher,
                                  const uint8_t **trailer, char *error,
                                  size_t error_capacity)
{
    int high = -1;
    const uint8_t *line = start;
    *trailer = finish;
    while (line < finish) {
        const uint8_t *content_finish = NULL;
        const uint8_t *after =
            type1_text_line_after(line, finish, &content_finish);
        if (type1_zero_line(line, content_finish)) {
            *trailer = after;
            if (high >= 0) {
                return type1_error(error, error_capacity,
                                   "odd Type 1 PFA hexadecimal digit");
            }
            return 0;
        }
        for (const uint8_t *at = line; at < content_finish; ++at) {
            if (type1_horizontal_space(*at)) {
                continue;
            }
            int value = type1_hex_value(*at);
            if (value < 0) {
                return type1_error(error, error_capacity,
                                   "invalid Type 1 PFA hexadecimal data");
            }
            if (high < 0) {
                high = value;
            } else {
                uint8_t byte = (uint8_t)((unsigned int)high << 4U |
                                         (unsigned int)value);
                if (type1_buffer_byte(cipher, byte, error,
                                      error_capacity) != 0) {
                    return -1;
                }
                high = -1;
            }
        }
        line = after;
    }
    return high < 0
               ? 0
               : type1_error(error, error_capacity,
                             "odd Type 1 PFA hexadecimal digit");
}

static int type1_disassemble_pfa(const uint8_t *font, size_t font_length,
                                 struct type1_buffer *output, char *error,
                                 size_t error_capacity)
{
    const uint8_t *finish = font + font_length;
    const uint8_t *marker =
        type1_find(font, finish, "currentfile eexec");
    if (marker == NULL) {
        return type1_error(error, error_capacity,
                           "Type 1 PFA has no eexec section");
    }
    const uint8_t *word_finish = marker + strlen("currentfile eexec");
    if (word_finish == finish || !type1_space(*word_finish)) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 PFA eexec boundary");
    }
    const uint8_t *content_finish = NULL;
    const uint8_t *after_marker =
        type1_text_line_after(marker, finish, &content_finish);
    const uint8_t *payload = word_finish;
    while (payload < content_finish && type1_horizontal_space(*payload)) {
        ++payload;
    }
    const uint8_t *public_finish = NULL;
    if (payload < content_finish) {
        public_finish = payload;
    } else {
        public_finish = after_marker;
        payload = after_marker;
        while (payload < finish && type1_space(*payload)) {
            ++payload;
        }
    }
    int status = type1_append_normalized_text(
        output, font, public_finish, false, error, error_capacity);
    bool hexadecimal = type1_four_hex_digits(payload, finish);
    struct type1_buffer cipher = {0};
    const uint8_t *trailer = finish;
    if (status == 0 && hexadecimal) {
        status = type1_decode_hex_lines(payload, finish, &cipher, &trailer,
                                        error, error_capacity);
    }
    struct type1_buffer plain = {0};
    size_t cipher_consumed = 0U;
    if (status == 0 && hexadecimal) {
        status = type1_decrypt_eexec(cipher.bytes, cipher.count, &plain,
                                    &cipher_consumed, error,
                                    error_capacity);
    } else if (status == 0) {
        status = type1_decrypt_eexec(
            payload, (size_t)(finish - payload), &plain, &cipher_consumed,
            error, error_capacity);
        trailer = payload + cipher_consumed;
    }
    if (status == 0) {
        status = type1_disassemble_private(
            plain.bytes, plain.bytes + plain.count, output, error,
            error_capacity);
    }
    if (status == 0 && hexadecimal && cipher_consumed < cipher.count) {
        status = type1_append_normalized_text(
            output, cipher.bytes + cipher_consumed,
            cipher.bytes + cipher.count, true, error, error_capacity);
    }
    if (status == 0) {
        status = type1_append_normalized_text(output, trailer, finish, true,
                                              error, error_capacity);
    }
    free(cipher.bytes);
    free(plain.bytes);
    return status;
}

int hstex_type1_disassemble(const uint8_t *font, size_t font_length,
                            uint8_t **disassembly,
                            size_t *disassembly_length, char *error,
                            size_t error_capacity)
{
    if (disassembly == NULL || disassembly_length == NULL) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 disassembly destination");
    }
    *disassembly = NULL;
    *disassembly_length = 0U;
    if (font == NULL || font_length == 0U) {
        return type1_error(error, error_capacity,
                           "invalid Type 1 font program");
    }
    struct type1_buffer output = {0};
    int status = font[0] == 0x80U
                     ? type1_disassemble_pfb(font, font_length, &output,
                                             error, error_capacity)
                     : font[0] == (uint8_t)'%'
                           ? type1_disassemble_pfa(
                                 font, font_length, &output, error,
                                 error_capacity)
                           : type1_error(
                                 error, error_capacity,
                                 "Type 1 font has no PFB or PFA marker");
    if (status == 0 &&
        type1_buffer_byte(&output, 0U, error, error_capacity) != 0) {
        status = -1;
    }
    if (status != 0) {
        free(output.bytes);
        return -1;
    }
    --output.count;
    *disassembly = output.bytes;
    *disassembly_length = output.count;
    return 0;
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
