#include "hstex/engine.h"

#include "hstex/catcode.h"
#include "internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    HSTEX_INITIAL_MEANING_CAPACITY = 64,
    HSTEX_INITIAL_MACRO_CAPACITY = 32,
    HSTEX_INITIAL_TOKEN_LIST_CAPACITY = 32,
    HSTEX_INITIAL_FONT_CAPACITY = 16,
    HSTEX_INITIAL_FONT_DIMEN_CAPACITY = 8,
    HSTEX_INITIAL_NODE_CAPACITY = 256,
    HSTEX_INITIAL_LIST_ITEM_CAPACITY = 256,
    HSTEX_INITIAL_HBOX_ITEM_CAPACITY = 16,
    HSTEX_INITIAL_VBOX_ITEM_CAPACITY = 16,
    HSTEX_INITIAL_HYPHEN_NODE_CAPACITY = 1024,
    HSTEX_INITIAL_HYPHEN_VALUE_CAPACITY = 4096,
    HSTEX_INITIAL_HYPHEN_EXCEPTION_CAPACITY = 32,
    HSTEX_INITIAL_HYPHEN_EXCEPTION_DATA_CAPACITY = 1024,
    HSTEX_MAX_HYPHEN_PATTERN_LENGTH = 255,
    HSTEX_MAX_FONT_DIMENS = 1048576,
    HSTEX_INITIAL_SAVE_CAPACITY = 64,
    HSTEX_INITIAL_CONDITIONAL_CAPACITY = 32,
    HSTEX_COUNT_REGISTER_CAPACITY = 32768,
    HSTEX_MAX_PARAMETERS = 9,
};

struct token_vector {
    hstex_token *data;
    size_t count;
    size_t capacity;
};

struct hstex_hbox_builder {
    uint32_t *node_identifiers;
    size_t count;
    size_t capacity;
    int64_t width;
    int32_t height;
    int32_t depth;
};

/* A vertical list is measured as TeX packages it: `extent` accumulates
   everything above the trailing depth, and `trailing_depth` is the depth of
   the last box or rule, which glue and penalties reset. */
struct hstex_vbox_builder {
    uint32_t *node_identifiers;
    size_t count;
    size_t capacity;
    int64_t extent;
    int32_t trailing_depth;
    int32_t width;
};

static int set_error(char *error, size_t capacity, const char *format, ...)
    HSTEX_PRINTF_FORMAT(3, 4);
static void clear_match_groups(struct hstex_engine *engine);
static const struct hstex_token_list *token_list_by_identifier(
    const struct hstex_engine *engine, uint32_t identifier);
static int push_one(struct hstex_engine *engine, hstex_token token,
                    struct hstex_source_location location, char *error,
                    size_t error_capacity);
static void describe_token(struct hstex_engine *engine, hstex_token token,
                           char *buffer, size_t capacity);
static const char *current_source_line(const struct hstex_engine *engine,
                                      uint32_t *line);
static const struct hstex_node *current_list_last_node(
    const struct hstex_engine *engine);
static int32_t last_node_type(const struct hstex_node *node);
static int expand_scan_tokens(struct hstex_engine *engine,
                              struct hstex_source_location location,
                              char *error, size_t error_capacity);
static int expand_pdf_color_stack_init(struct hstex_engine *engine,
                                       struct hstex_source_location location,
                                       char *error, size_t error_capacity);
static bool conditional_test_pending(const struct hstex_engine *engine);
static int push_relax_before(struct hstex_engine *engine, hstex_token token,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity);
enum pdf_escape_kind {
    PDF_ESCAPE_STRING = 0,
    PDF_ESCAPE_NAME,
    PDF_ESCAPE_HEX,
    PDF_UNESCAPE_HEX,
};

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

static bool token_is_category(hstex_token token, enum hstex_catcode category)
{
    return hstex_token_is_character(token) &&
           hstex_token_category(token) == (uint8_t)category;
}

static bool token_is_space(hstex_token token)
{
    return token_is_category(token, HSTEX_CAT_SPACE);
}

static bool token_is_other_character(hstex_token token, uint8_t character)
{
    return hstex_token_is_character(token) &&
           hstex_token_category(token) == (uint8_t)HSTEX_CAT_OTHER &&
           hstex_token_character_code(token) == character;
}

static hstex_token normalize_unexpanded_control_sequence(hstex_token token)
{
    if (hstex_token_is_unexpanded_control_sequence(token)) {
        return hstex_token_control_sequence(
            hstex_token_control_sequence_id(token));
    }
    return token;
}

static hstex_token normalize_one_shot_token(hstex_token token)
{
    token = normalize_unexpanded_control_sequence(token);
    if (hstex_token_is_unexpanded_non_control(token)) {
        token = hstex_token_normalize_unexpanded_non_control(token);
    }
    return token;
}

static hstex_token normalize_frozen_control_sequence(hstex_token token)
{
    if (hstex_token_is_frozen_control_sequence(token)) {
        return hstex_token_control_sequence(
            hstex_token_control_sequence_id(token));
    }
    return token;
}

static bool token_is_effective_space(const struct hstex_engine *engine,
                                     hstex_token token)
{
    if (token_is_space(token)) {
        return true;
    }
    if (engine == NULL || !hstex_token_is_control_sequence(token)) {
        return false;
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    return meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
           token_is_space(meaning->value.token);
}

/* An implicit begin-group character - a control sequence \let to a catcode-1
   token - opens a box just as an explicit brace does, because TeX inspects the
   meaning rather than the token. Balanced-text scanning is deliberately not
   changed: pdfTeX runs off the end of the file on \toks0=\bgroup abc\egroup,
   so there only explicit braces count. See docs/DECISIONS.md, implicit-braces. */
static bool token_is_effective_begin_group(struct hstex_engine *engine,
                                           hstex_token token)
{
    if (token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
        return true;
    }
    if (engine == NULL || !hstex_token_is_control_sequence(token)) {
        return false;
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    return meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
           token_is_category(meaning->value.token, HSTEX_CAT_BEGIN_GROUP);
}

/* The preamble is read without expansion, so a control sequence \let to a
   parameter or tab character has to be recognised by its meaning -- which is
   how LaTeX's \@sharp reaches \halign. */
static bool token_is_effective_category(struct hstex_engine *engine,
                                        hstex_token token, uint8_t category)
{
    if (token_is_category(token, category)) {
        return true;
    }
    if (engine == NULL || !hstex_token_is_control_sequence(token)) {
        return false;
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    return meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
           token_is_category(meaning->value.token, category);
}

static bool token_is_effective_end_group(struct hstex_engine *engine,
                                         hstex_token token)
{
    if (token_is_category(token, HSTEX_CAT_END_GROUP)) {
        return true;
    }
    if (engine == NULL || !hstex_token_is_control_sequence(token)) {
        return false;
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    return meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
           token_is_category(meaning->value.token, HSTEX_CAT_END_GROUP);
}


static int code_table_index(enum hstex_command command)
{
    switch (command) {
    case HSTEX_COMMAND_SF_CODE:
        return 0;
    case HSTEX_COMMAND_LC_CODE:
        return 1;
    case HSTEX_COMMAND_UC_CODE:
        return 2;
    case HSTEX_COMMAND_MATH_CODE:
        return 3;
    case HSTEX_COMMAND_DEL_CODE:
        return 4;
    default:
        return -1;
    }
}

static void vector_destroy(struct token_vector *vector)
{
    free(vector->data);
    memset(vector, 0, sizeof(*vector));
}

static int vector_reserve(struct token_vector *vector, size_t required,
                          char *error, size_t error_capacity)
{
    if (required <= vector->capacity) {
        return 0;
    }
    size_t capacity = vector->capacity == 0U ? 32U : vector->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "token-vector capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*vector->data)) {
        return set_error(error, error_capacity,
                         "token-vector allocation overflow");
    }
    void *allocation = realloc(vector->data, capacity * sizeof(*vector->data));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "token-vector allocation failed");
    }
    vector->data = allocation;
    vector->capacity = capacity;
    return 0;
}

static int vector_push(struct token_vector *vector, hstex_token token,
                       char *error, size_t error_capacity)
{
    if (vector_reserve(vector, vector->count + 1U, error, error_capacity) != 0) {
        return -1;
    }
    vector->data[vector->count++] = token;
    return 0;
}

static int vector_append(struct token_vector *vector, const hstex_token *tokens,
                         size_t count, char *error, size_t error_capacity)
{
    if (count == 0U) {
        return 0;
    }
    if (tokens == NULL || count > SIZE_MAX - vector->count ||
        vector_reserve(vector, vector->count + count, error, error_capacity) !=
            0) {
        return set_error(error, error_capacity,
                         "invalid token-vector append");
    }
    memcpy(vector->data + vector->count, tokens, count * sizeof(*tokens));
    vector->count += count;
    return 0;
}

static int reserve_meanings(struct hstex_engine *engine, size_t required,
                            char *error, size_t error_capacity)
{
    if (required <= engine->meaning_capacity) {
        return 0;
    }
    size_t capacity = engine->meaning_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_MEANING_CAPACITY
                          : engine->meaning_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "meaning-table capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->meanings)) {
        return set_error(error, error_capacity,
                         "meaning-table allocation overflow");
    }
    size_t old_capacity = engine->meaning_capacity;
    void *allocation = realloc(engine->meanings,
                               capacity * sizeof(*engine->meanings));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "meaning-table allocation failed");
    }
    engine->meanings = allocation;
    memset(engine->meanings + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->meanings));
    engine->meaning_capacity = capacity;
    return 0;
}

static int reserve_macros(struct hstex_engine *engine, size_t required,
                          char *error, size_t error_capacity)
{
    if (required <= engine->macro_capacity) {
        return 0;
    }
    size_t capacity = engine->macro_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_MACRO_CAPACITY
                          : engine->macro_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "macro capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->macros)) {
        return set_error(error, error_capacity, "macro allocation overflow");
    }
    size_t old_capacity = engine->macro_capacity;
    void *allocation = realloc(engine->macros,
                               capacity * sizeof(*engine->macros));
    if (allocation == NULL) {
        return set_error(error, error_capacity, "macro allocation failed");
    }
    engine->macros = allocation;
    memset(engine->macros + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->macros));
    engine->macro_capacity = capacity;
    return 0;
}

static int reserve_token_lists(struct hstex_engine *engine, size_t required,
                               char *error, size_t error_capacity)
{
    if (required <= engine->token_list_capacity) {
        return 0;
    }
    size_t capacity = engine->token_list_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_TOKEN_LIST_CAPACITY
                          : engine->token_list_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "token-list capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->token_lists)) {
        return set_error(error, error_capacity,
                         "token-list allocation overflow");
    }
    size_t old_capacity = engine->token_list_capacity;
    void *allocation = realloc(engine->token_lists,
                               capacity * sizeof(*engine->token_lists));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "token-list allocation failed");
    }
    engine->token_lists = allocation;
    memset(engine->token_lists + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->token_lists));
    engine->token_list_capacity = capacity;
    return 0;
}

static int reserve_fonts(struct hstex_engine *engine, size_t required,
                         char *error, size_t error_capacity)
{
    if (required <= engine->font_capacity) {
        return 0;
    }
    size_t capacity = engine->font_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_FONT_CAPACITY
                          : engine->font_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "font capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->fonts)) {
        return set_error(error, error_capacity, "font allocation overflow");
    }
    size_t old_capacity = engine->font_capacity;
    void *allocation = realloc(engine->fonts,
                               capacity * sizeof(*engine->fonts));
    if (allocation == NULL) {
        return set_error(error, error_capacity, "font allocation failed");
    }
    engine->fonts = allocation;
    memset(engine->fonts + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->fonts));
    engine->font_capacity = capacity;
    return 0;
}

static int reserve_nodes(struct hstex_engine *engine, size_t required,
                         char *error, size_t error_capacity)
{
    if (required <= engine->node_capacity) {
        return 0;
    }
    size_t capacity = engine->node_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_NODE_CAPACITY
                          : engine->node_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "typesetting-node capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->nodes) ||
        capacity > (size_t)UINT32_MAX) {
        return set_error(error, error_capacity,
                         "typesetting-node allocation overflow");
    }
    size_t old_capacity = engine->node_capacity;
    void *allocation =
        realloc(engine->nodes, capacity * sizeof(*engine->nodes));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "typesetting-node allocation failed");
    }
    engine->nodes = allocation;
    memset(engine->nodes + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->nodes));
    engine->node_capacity = capacity;
    return 0;
}

static int reserve_list_items(struct hstex_engine *engine, size_t required,
                              char *error, size_t error_capacity)
{
    if (required <= engine->list_item_capacity) {
        return 0;
    }
    size_t capacity = engine->list_item_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_LIST_ITEM_CAPACITY
                          : engine->list_item_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "box-list capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->list_items) ||
        capacity > (size_t)UINT32_MAX) {
        return set_error(error, error_capacity,
                         "box-list allocation overflow");
    }
    void *allocation =
        realloc(engine->list_items, capacity * sizeof(*engine->list_items));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "box-list allocation failed");
    }
    engine->list_items = allocation;
    engine->list_item_capacity = capacity;
    return 0;
}

static int reserve_font_dimens(struct hstex_font *font, size_t required,
                               char *error, size_t error_capacity)
{
    if (required <= font->dimen_capacity) {
        return 0;
    }
    if (required > (size_t)HSTEX_MAX_FONT_DIMENS) {
        return set_error(error, error_capacity,
                         "fontdimen index exceeds supported range");
    }
    size_t capacity = font->dimen_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_FONT_DIMEN_CAPACITY
                          : font->dimen_capacity;
    while (capacity < required) {
        if (capacity > (size_t)HSTEX_MAX_FONT_DIMENS / 2U) {
            capacity = (size_t)HSTEX_MAX_FONT_DIMENS;
            break;
        }
        capacity *= 2U;
    }
    void *allocation = realloc(font->dimens, capacity * sizeof(*font->dimens));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "fontdimen allocation failed");
    }
    font->dimens = allocation;
    memset(font->dimens + font->dimen_capacity, 0,
           (capacity - font->dimen_capacity) * sizeof(*font->dimens));
    font->dimen_capacity = capacity;
    return 0;
}

static int reserve_hyphen_nodes(struct hstex_engine *engine, size_t required,
                                char *error, size_t error_capacity)
{
    if (required <= engine->hyphen_node_capacity) {
        return 0;
    }
    size_t capacity = engine->hyphen_node_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_HYPHEN_NODE_CAPACITY
                          : engine->hyphen_node_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "hyphen-trie capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->hyphen_nodes) ||
        capacity > (size_t)UINT32_MAX) {
        return set_error(error, error_capacity,
                         "hyphen-trie allocation overflow");
    }
    size_t old_capacity = engine->hyphen_node_capacity;
    void *allocation = realloc(engine->hyphen_nodes,
                               capacity * sizeof(*engine->hyphen_nodes));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "hyphen-trie allocation failed");
    }
    engine->hyphen_nodes = allocation;
    memset(engine->hyphen_nodes + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->hyphen_nodes));
    engine->hyphen_node_capacity = capacity;
    return 0;
}

static int reserve_byte_arena(uint8_t **data, size_t *capacity,
                              size_t initial_capacity, size_t required,
                              const char *label, char *error,
                              size_t error_capacity)
{
    if (required <= *capacity) {
        return 0;
    }
    size_t next = *capacity == 0U ? initial_capacity : *capacity;
    while (next < required) {
        if (next > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "%s capacity overflow",
                             label);
        }
        next *= 2U;
    }
    void *allocation = realloc(*data, next);
    if (allocation == NULL) {
        return set_error(error, error_capacity, "%s allocation failed", label);
    }
    *data = allocation;
    *capacity = next;
    return 0;
}

static int reserve_hyphen_exceptions(struct hstex_engine *engine,
                                     size_t required, char *error,
                                     size_t error_capacity)
{
    if (required <= engine->hyphen_exception_capacity) {
        return 0;
    }
    size_t capacity = engine->hyphen_exception_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_HYPHEN_EXCEPTION_CAPACITY
                          : engine->hyphen_exception_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "hyphen-exception capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->hyphen_exceptions)) {
        return set_error(error, error_capacity,
                         "hyphen-exception allocation overflow");
    }
    size_t old_capacity = engine->hyphen_exception_capacity;
    void *allocation = realloc(
        engine->hyphen_exceptions,
        capacity * sizeof(*engine->hyphen_exceptions));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "hyphen-exception allocation failed");
    }
    engine->hyphen_exceptions = allocation;
    memset(engine->hyphen_exceptions + old_capacity, 0,
           (capacity - old_capacity) * sizeof(*engine->hyphen_exceptions));
    engine->hyphen_exception_capacity = capacity;
    return 0;
}

static char *resolve_with_kpsewhich(const char *filename);

static uint16_t read_big_endian_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
}

static int32_t read_big_endian_i32(const uint8_t *bytes)
{
    uint32_t value = ((uint32_t)bytes[0] << 24U) |
                     ((uint32_t)bytes[1] << 16U) |
                     ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
    return (int32_t)value;
}

static char *tfm_filename(const char *name, char *error,
                          size_t error_capacity)
{
    size_t length = strlen(name);
    bool has_extension =
        length >= 4U && strcmp(name + length - 4U, ".tfm") == 0;
    size_t suffix_length = has_extension ? 0U : 4U;
    if (length > SIZE_MAX - suffix_length - 1U) {
        (void)set_error(error, error_capacity, "font filename is too long");
        return NULL;
    }
    char *filename = malloc(length + suffix_length + 1U);
    if (filename == NULL) {
        (void)set_error(error, error_capacity,
                        "font filename allocation failed");
        return NULL;
    }
    memcpy(filename, name, length);
    if (!has_extension) {
        memcpy(filename + length, ".tfm", 5U);
    } else {
        filename[length] = '\0';
    }
    return filename;
}

/* A TFM dimension is a fixed-point multiple of the font's size, with twenty
   fractional bits. Truncating the product is what the reference does; see
   docs/DECISIONS.md, font-character-metrics. */
static int32_t scale_fix_word(int32_t fixed, int32_t size)
{
    int64_t scaled = ((int64_t)fixed * (int64_t)size) >> 20;
    if (scaled < -INT64_C(1073741823)) {
        return -INT32_C(1073741823);
    }
    if (scaled > INT64_C(1073741823)) {
        return INT32_C(1073741823);
    }
    return (int32_t)scaled;
}

static int open_tfm(const char *name, struct hstex_input *input, char *error,
                    size_t error_capacity)
{
    char *filename = tfm_filename(name, error, error_capacity);
    if (filename == NULL) {
        return -1;
    }
    char *path = access(filename, R_OK) == 0 ? strdup(filename)
                                             : resolve_with_kpsewhich(filename);
    free(filename);
    if (path == NULL) {
        return set_error(error, error_capacity,
                         "font metric file not found: %s", name);
    }
    int status = hstex_input_open(path, input, error, error_capacity);
    free(path);
    if (status != 0) {
        return -1;
    }
    if (input->length < 24U || input->length % 4U != 0U) {
        hstex_input_close(input);
        return set_error(error, error_capacity, "invalid TFM header: %s", name);
    }
    return 0;
}

/* The design size is a fixed-point number of points in the second header
   word, which carries twenty fractional bits where a scaled point has
   sixteen. */
static int tfm_design_size(const char *name, int32_t *design, char *error,
                           size_t error_capacity)
{
    struct hstex_input input;
    if (open_tfm(name, &input, error, error_capacity) != 0) {
        return -1;
    }
    uint16_t header_length = read_big_endian_u16(input.data + 2U);
    if (header_length < 2U || (size_t)(6U + header_length) * 4U > input.length) {
        hstex_input_close(&input);
        return set_error(error, error_capacity,
                         "TFM header is too short: %s", name);
    }
    int32_t fixed = read_big_endian_i32(input.data + 7U * 4U);
    hstex_input_close(&input);
    if (fixed <= 0) {
        return set_error(error, error_capacity,
                         "TFM design size is not positive: %s", name);
    }
    *design = fixed / 16;
    return 0;
}

static int load_tfm_parameters(struct hstex_font *font, const char *name,
                               int32_t size, char *error,
                               size_t error_capacity)
{
    struct hstex_input input;
    if (open_tfm(name, &input, error, error_capacity) != 0) {
        return -1;
    }

    uint16_t fields[12];
    for (size_t index = 0U; index < 12U; ++index) {
        fields[index] = read_big_endian_u16(input.data + index * 2U);
    }
    uint16_t lf = fields[0];
    uint16_t lh = fields[1];
    uint16_t bc = fields[2];
    uint16_t ec = fields[3];
    size_t character_count = bc <= ec ? (size_t)ec - (size_t)bc + 1U : 0U;
    uint64_t expected_words = UINT64_C(6) + (uint64_t)lh + character_count;
    for (size_t index = 4U; index < 12U; ++index) {
        expected_words += fields[index];
    }
    /* A metric file may be longer than its own table lengths say -- some in
       the distribution are padded -- and the reference simply reads the
       words it was told about. See docs/DECISIONS.md, padded-tfm-files. */
    if ((uint64_t)lf != expected_words || (size_t)lf * 4U > input.length ||
        bc > 255U || ec > 255U ||
        (bc > ec && !(bc == 1U && ec == 0U))) {
        hstex_input_close(&input);
        return set_error(error, error_capacity,
                         "inconsistent TFM table lengths: %s", name);
    }

    /* Character metrics are indexes into shared width, height, depth and
       italic tables; index zero in the width table means the font does not
       define that character. */
    size_t width_base = 6U + (size_t)lh + character_count;
    size_t height_base = width_base + (size_t)fields[4];
    size_t depth_base = height_base + (size_t)fields[5];
    size_t italic_base = depth_base + (size_t)fields[6];
    struct hstex_char_metric *characters =
        calloc(HSTEX_FONT_CHARACTER_COUNT, sizeof(*characters));
    if (characters == NULL) {
        hstex_input_close(&input);
        return set_error(error, error_capacity,
                         "font character metric allocation failed");
    }
    for (size_t index = 0U; index < HSTEX_FONT_CHARACTER_COUNT; ++index) {
        characters[index].tag = -1;
        characters[index].expansion_factor = HSTEX_DEFAULT_EXPANSION_FACTOR;
    }
    for (size_t index = 0U; index < character_count; ++index) {
        size_t code = (size_t)bc + index;
        if (code >= HSTEX_FONT_CHARACTER_COUNT) {
            break;
        }
        const uint8_t *info = input.data + (6U + (size_t)lh + index) * 4U;
        size_t width_index = info[0];
        if (width_index == 0U || width_index >= (size_t)fields[4]) {
            continue;
        }
        size_t height_index = (size_t)(info[1] >> 4);
        size_t depth_index = (size_t)(info[1] & 0x0FU);
        size_t italic_index = (size_t)(info[2] >> 2);
        struct hstex_char_metric *metric = &characters[code];
        metric->tag = (int32_t)(info[2] & 0x03U);
        metric->remainder = (int32_t)info[3];
        metric->width = scale_fix_word(
            read_big_endian_i32(input.data + (width_base + width_index) * 4U),
            size);
        if (height_index != 0U && height_index < (size_t)fields[5]) {
            metric->height = scale_fix_word(
                read_big_endian_i32(input.data +
                                    (height_base + height_index) * 4U),
                size);
        }
        if (depth_index != 0U && depth_index < (size_t)fields[6]) {
            metric->depth = scale_fix_word(
                read_big_endian_i32(input.data +
                                    (depth_base + depth_index) * 4U),
                size);
        }
        if (italic_index != 0U && italic_index < (size_t)fields[7]) {
            metric->italic = scale_fix_word(
                read_big_endian_i32(input.data +
                                    (italic_base + italic_index) * 4U),
                size);
        }
    }
    free(font->characters);
    font->characters = characters;

    /* The ligature and kerning program, and the kerns it refers to. */
    size_t lig_kern_base = italic_base + (size_t)fields[7];
    size_t kern_base = lig_kern_base + (size_t)fields[8];
    struct hstex_lig_kern *lig_kern = NULL;
    int32_t *kerns = NULL;
    if (fields[8] != 0U) {
        lig_kern = calloc((size_t)fields[8], sizeof(*lig_kern));
        if (lig_kern == NULL) {
            hstex_input_close(&input);
            return set_error(error, error_capacity,
                             "font ligature program allocation failed");
        }
        for (size_t index = 0U; index < (size_t)fields[8]; ++index) {
            const uint8_t *step = input.data + (lig_kern_base + index) * 4U;
            lig_kern[index].skip = step[0];
            lig_kern[index].next = step[1];
            lig_kern[index].operation = step[2];
            lig_kern[index].remainder = step[3];
        }
    }
    if (fields[9] != 0U) {
        kerns = calloc((size_t)fields[9], sizeof(*kerns));
        if (kerns == NULL) {
            free(lig_kern);
            hstex_input_close(&input);
            return set_error(error, error_capacity,
                             "font kern table allocation failed");
        }
        for (size_t index = 0U; index < (size_t)fields[9]; ++index) {
            kerns[index] = scale_fix_word(
                read_big_endian_i32(input.data + (kern_base + index) * 4U),
                size);
        }
    }
    /* The recipes a delimiter too tall for any single character is built
       from; see docs/DECISIONS.md, extensible-delimiters. */
    size_t extensible_base = kern_base + (size_t)fields[9];
    struct hstex_extensible *extensibles = NULL;
    if (fields[10] != 0U) {
        extensibles = calloc((size_t)fields[10], sizeof(*extensibles));
        if (extensibles == NULL) {
            free(lig_kern);
            free(kerns);
            hstex_input_close(&input);
            return set_error(error, error_capacity,
                             "font extensible table allocation failed");
        }
        for (size_t index = 0U; index < (size_t)fields[10]; ++index) {
            const uint8_t *recipe =
                input.data + (extensible_base + index) * 4U;
            extensibles[index].top = recipe[0];
            extensibles[index].middle = recipe[1];
            extensibles[index].bottom = recipe[2];
            extensibles[index].repeated = recipe[3];
        }
    }

    free(font->lig_kern);
    free(font->kerns);
    free(font->extensibles);
    font->lig_kern = lig_kern;
    font->lig_kern_count = (size_t)fields[8];
    font->kerns = kerns;
    font->kern_count = (size_t)fields[9];
    font->extensibles = extensibles;
    font->extensible_count = (size_t)fields[10];

    size_t parameter_count = fields[11];
    size_t parameter_word =
        6U + (size_t)lh + character_count + (size_t)fields[4] +
        (size_t)fields[5] + (size_t)fields[6] + (size_t)fields[7] +
        (size_t)fields[8] + (size_t)fields[9] + (size_t)fields[10];
    if (reserve_font_dimens(font, parameter_count, error, error_capacity) != 0) {
        hstex_input_close(&input);
        return -1;
    }
    for (size_t index = 0U; index < parameter_count; ++index) {
        int32_t fix_word =
            read_big_endian_i32(input.data + (parameter_word + index) * 4U);
        int32_t scale = index == 0U ? INT32_C(65536) : size;
        int64_t scaled = (int64_t)fix_word * (int64_t)scale /
                         INT64_C(1048576);
        if (scaled < -INT64_C(1073741823) ||
            scaled > INT64_C(1073741823)) {
            hstex_input_close(&input);
            return set_error(error, error_capacity,
                             "TFM parameter is outside TeX's range: %s", name);
        }
        font->dimens[index] = (int32_t)scaled;
    }
    font->dimen_count = parameter_count;
    hstex_input_close(&input);
    return 0;
}

static struct hstex_font *font_by_identifier(struct hstex_engine *engine,
                                             uint32_t identifier)
{
    if (identifier == 0U || (size_t)identifier > engine->font_count) {
        return NULL;
    }
    return &engine->fonts[identifier - 1U];
}

static int find_or_create_font(struct hstex_engine *engine, const char *name,
                               int32_t size, uint32_t *identifier,
                               char *error, size_t error_capacity)
{
    for (size_t index = 0U; index < engine->font_count; ++index) {
        if (engine->fonts[index].size == size &&
            strcmp(engine->fonts[index].name, name) == 0) {
            *identifier = (uint32_t)(index + 1U);
            return 0;
        }
    }
    if (engine->font_count >= (size_t)INT32_MAX ||
        reserve_fonts(engine, engine->font_count + 1U, error,
                      error_capacity) != 0) {
        return set_error(error, error_capacity, "too many fonts");
    }
    char *name_copy = strdup(name);
    if (name_copy == NULL) {
        return set_error(error, error_capacity, "font-name allocation failed");
    }
    struct hstex_font *font = &engine->fonts[engine->font_count];
    memset(font, 0, sizeof(*font));
    font->name = name_copy;
    font->size = size;
    font->hyphen_character =
        engine->integer_parameters[HSTEX_INTEGER_DEFAULT_HYPHEN_CHAR];
    font->skew_character =
        engine->integer_parameters[HSTEX_INTEGER_DEFAULT_SKEW_CHAR];
    if (strcmp(name, "nullfont") == 0) {
        if (reserve_font_dimens(font, 7U, error, error_capacity) != 0) {
            free(font->name);
            memset(font, 0, sizeof(*font));
            return -1;
        }
        font->dimen_count = 7U;
    } else if (load_tfm_parameters(font, name, size, error, error_capacity) !=
               0) {
        free(font->name);
        free(font->dimens);
        memset(font, 0, sizeof(*font));
        return -1;
    }
    ++engine->font_count;
    *identifier = (uint32_t)engine->font_count;
    return 0;
}

static int reserve_saves(struct hstex_engine *engine, size_t required,
                         char *error, size_t error_capacity)
{
    if (required <= engine->save_capacity) {
        return 0;
    }
    size_t capacity = engine->save_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_SAVE_CAPACITY
                          : engine->save_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "save-stack capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->saves)) {
        return set_error(error, error_capacity,
                         "save-stack allocation overflow");
    }
    void *allocation = realloc(engine->saves,
                               capacity * sizeof(*engine->saves));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "save-stack allocation failed");
    }
    engine->saves = allocation;
    engine->save_capacity = capacity;
    return 0;
}

static int reserve_conditionals(struct hstex_engine *engine, size_t required,
                                char *error, size_t error_capacity)
{
    if (required <= engine->conditional_capacity) {
        return 0;
    }
    size_t capacity = engine->conditional_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_CONDITIONAL_CAPACITY
                          : engine->conditional_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "conditional-stack capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*engine->conditionals)) {
        return set_error(error, error_capacity,
                         "conditional-stack allocation overflow");
    }
    void *allocation = realloc(
        engine->conditionals, capacity * sizeof(*engine->conditionals));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "conditional-stack allocation failed");
    }
    engine->conditionals = allocation;
    engine->conditional_capacity = capacity;
    return 0;
}

const struct hstex_meaning *hstex_engine_meaning(
    const struct hstex_engine *engine, hstex_cs_id identifier)
{
    static const struct hstex_meaning undefined_meaning = {
        .command = HSTEX_COMMAND_UNDEFINED,
        .level = 0U,
        .value = {.macro_identifier = 0U},
    };
    if (engine == NULL || identifier == 0U ||
        (size_t)identifier > engine->meaning_capacity) {
        return &undefined_meaning;
    }
    return &engine->meanings[identifier - 1U];
}

static int set_meaning(struct hstex_engine *engine, hstex_cs_id identifier,
                       struct hstex_meaning meaning, bool global, char *error,
                       size_t error_capacity)
{
    if (identifier == 0U ||
        reserve_meanings(engine, (size_t)identifier, error, error_capacity) !=
            0) {
        return set_error(error, error_capacity,
                         "invalid control-sequence assignment");
    }
    struct hstex_meaning *destination = &engine->meanings[identifier - 1U];
    bool local = !global && engine->group_level != 0U;
    if (local) {
        if (reserve_saves(engine, engine->save_count + 1U, error,
                          error_capacity) != 0) {
            return -1;
        }
        struct hstex_save_entry *save = &engine->saves[engine->save_count++];
        save->kind = HSTEX_SAVE_MEANING;
        save->index = identifier;
        save->level = engine->group_level;
        save->previous_level = destination->level;
        save->previous.meaning = *destination;
        meaning.level = engine->group_level;
    } else {
        meaning.level = 0U;
    }
    *destination = meaning;
    return 0;
}

static bool assignment_is_global(const struct hstex_engine *engine,
                                 bool requested_global)
{
    int32_t global_defs =
        engine->integer_parameters[HSTEX_INTEGER_GLOBAL_DEFS];
    if (global_defs > 0) {
        return true;
    }
    if (global_defs < 0) {
        return false;
    }
    return requested_global;
}

static int save_value(struct hstex_engine *engine, enum hstex_save_kind kind,
                      uint32_t index, uint32_t previous_level,
                      int32_t previous_integer, uint8_t previous_category,
                      char *error, size_t error_capacity)
{
    if (reserve_saves(engine, engine->save_count + 1U, error, error_capacity) !=
        0) {
        return -1;
    }
    struct hstex_save_entry *save = &engine->saves[engine->save_count++];
    memset(save, 0, sizeof(*save));
    save->kind = kind;
    save->index = index;
    save->level = engine->group_level;
    save->previous_level = previous_level;
    if (kind == HSTEX_SAVE_CAT_CODE) {
        save->previous.category = previous_category;
    } else {
        save->previous.integer = previous_integer;
    }
    return 0;
}

static int save_token_list_identifier(
    struct hstex_engine *engine, enum hstex_save_kind kind, uint32_t index,
    uint32_t previous_level, uint32_t previous_identifier, char *error,
    size_t error_capacity)
{
    if (reserve_saves(engine, engine->save_count + 1U, error, error_capacity) !=
        0) {
        return -1;
    }
    struct hstex_save_entry *save = &engine->saves[engine->save_count++];
    memset(save, 0, sizeof(*save));
    save->kind = kind;
    save->index = index;
    save->level = engine->group_level;
    save->previous_level = previous_level;
    save->previous.token_list_identifier = previous_identifier;
    return 0;
}

static int store_token_list(struct hstex_engine *engine,
                            struct token_vector *tokens,
                            uint32_t *identifier, char *error,
                            size_t error_capacity)
{
    if (tokens->count == 0U) {
        vector_destroy(tokens);
        *identifier = 0U;
        return 0;
    }
    if (engine->token_list_count >= (size_t)UINT32_MAX) {
        return set_error(error, error_capacity,
                         "token-list identifier space exhausted");
    }
    if (reserve_token_lists(engine, engine->token_list_count + 1U, error,
                            error_capacity) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < tokens->count; ++index) {
        if (hstex_token_is_frozen_control_sequence(tokens->data[index])) {
            tokens->data[index] = hstex_token_control_sequence(
                hstex_token_control_sequence_id(tokens->data[index]));
        }
    }
    struct hstex_token_list *list =
        &engine->token_lists[engine->token_list_count];
    list->tokens = tokens->data;
    list->count = tokens->count;
    tokens->data = NULL;
    tokens->count = 0U;
    tokens->capacity = 0U;
    ++engine->token_list_count;
    *identifier = (uint32_t)engine->token_list_count;
    return 0;
}

static const struct hstex_token_list *token_list_by_identifier(
    const struct hstex_engine *engine, uint32_t identifier)
{
    if (identifier == 0U || (size_t)identifier > engine->token_list_count) {
        return NULL;
    }
    return &engine->token_lists[identifier - 1U];
}

static int assign_token_register(struct hstex_engine *engine, uint32_t index,
                                 uint32_t identifier, bool requested_global,
                                 char *error, size_t error_capacity)
{
    if ((size_t)index >= engine->count_capacity ||
        (identifier != 0U &&
         token_list_by_identifier(engine, identifier) == NULL)) {
        return set_error(error, error_capacity,
                         "invalid token-register assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_token_list_identifier(
                engine, HSTEX_SAVE_TOKEN_REGISTER, index,
                engine->token_register_levels[index],
                engine->token_registers[index], error, error_capacity) != 0) {
            return -1;
        }
        engine->token_register_levels[index] = engine->group_level;
    } else {
        engine->token_register_levels[index] = 0U;
    }
    engine->token_registers[index] = identifier;
    return 0;
}

static int assign_token_parameter(struct hstex_engine *engine, uint32_t index,
                                  uint32_t identifier,
                                  bool requested_global, char *error,
                                  size_t error_capacity)
{
    if (index >= (uint32_t)HSTEX_TOKEN_PARAMETER_COUNT ||
        (identifier != 0U &&
         token_list_by_identifier(engine, identifier) == NULL)) {
        return set_error(error, error_capacity,
                         "invalid token-parameter assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_token_list_identifier(
                engine, HSTEX_SAVE_TOKEN_PARAMETER, index,
                engine->token_parameter_levels[index],
                engine->token_parameters[index], error, error_capacity) != 0) {
            return -1;
        }
        engine->token_parameter_levels[index] = engine->group_level;
    } else {
        engine->token_parameter_levels[index] = 0U;
    }
    engine->token_parameters[index] = identifier;
    return 0;
}

static int assign_catcode(struct hstex_engine *engine, uint32_t character,
                          uint32_t category, bool requested_global, char *error,
                          size_t error_capacity)
{
    if (character > UINT32_C(255) ||
        category > (uint32_t)HSTEX_CAT_INVALID) {
        return set_error(error, error_capacity,
                         "catcode assignment outside 0..255 or 0..15");
    }
    bool global = assignment_is_global(engine, requested_global);
    uint32_t *level = &engine->catcode_levels[character];
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_CAT_CODE, character, *level, 0,
                       hstex_catcode_get(&engine->lexical_state.catcodes,
                                         (uint8_t)character),
                       error, error_capacity) != 0) {
            return -1;
        }
        *level = engine->group_level;
    } else {
        *level = 0U;
    }
    return hstex_catcode_set(&engine->lexical_state.catcodes, character,
                             category);
}

static int assign_code(struct hstex_engine *engine, uint32_t table,
                       uint32_t character, int32_t value,
                       bool requested_global, char *error,
                       size_t error_capacity)
{
    if (table >= 5U || character > 255U) {
        return set_error(error, error_capacity,
                         "invalid code-table assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    uint32_t *level = &engine->code_levels[table][character];
    if (!global && engine->group_level != 0U) {
        uint32_t encoded_index = table * 256U + character;
        if (save_value(engine, HSTEX_SAVE_CODE, encoded_index, *level,
                       engine->code_tables[table][character], 0U, error,
                       error_capacity) != 0) {
            return -1;
        }
        *level = engine->group_level;
    } else {
        *level = 0U;
    }
    engine->code_tables[table][character] = value;
    return 0;
}

static int assign_count(struct hstex_engine *engine, uint32_t index,
                        int32_t value, bool requested_global, char *error,
                        size_t error_capacity)
{
    if ((size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "count register outside supported range");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_COUNT, index,
                       engine->count_levels[index], engine->counts[index], 0U,
                       error, error_capacity) != 0) {
            return -1;
        }
        engine->count_levels[index] = engine->group_level;
    } else {
        engine->count_levels[index] = 0U;
    }
    engine->counts[index] = value;
    return 0;
}

static int assign_dimen(struct hstex_engine *engine, uint32_t index,
                        int32_t value, bool requested_global, char *error,
                        size_t error_capacity)
{
    if ((size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "dimen register outside supported range");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_DIMEN, index,
                       engine->dimen_levels[index], engine->dimens[index], 0U,
                       error, error_capacity) != 0) {
            return -1;
        }
        engine->dimen_levels[index] = engine->group_level;
    } else {
        engine->dimen_levels[index] = 0U;
    }
    engine->dimens[index] = value;
    return 0;
}

static int assign_glue(struct hstex_engine *engine, uint32_t index,
                       struct hstex_glue value, bool requested_global,
                       char *error, size_t error_capacity)
{
    if ((size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "glue register outside supported range");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (reserve_saves(engine, engine->save_count + 1U, error,
                          error_capacity) != 0) {
            return -1;
        }
        struct hstex_save_entry *save = &engine->saves[engine->save_count++];
        memset(save, 0, sizeof(*save));
        save->kind = HSTEX_SAVE_GLUE;
        save->index = index;
        save->level = engine->group_level;
        save->previous_level = engine->glue_levels[index];
        save->previous.glue = engine->glues[index];
        engine->glue_levels[index] = engine->group_level;
    } else {
        engine->glue_levels[index] = 0U;
    }
    engine->glues[index] = value;
    return 0;
}

static int assign_muglue(struct hstex_engine *engine, uint32_t index,
                         struct hstex_glue value, bool requested_global,
                         char *error, size_t error_capacity)
{
    if ((size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "muskip register outside supported range");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (reserve_saves(engine, engine->save_count + 1U, error,
                          error_capacity) != 0) {
            return -1;
        }
        struct hstex_save_entry *save = &engine->saves[engine->save_count++];
        memset(save, 0, sizeof(*save));
        save->kind = HSTEX_SAVE_MUGLUE;
        save->index = index;
        save->level = engine->group_level;
        save->previous_level = engine->muglue_levels[index];
        save->previous.glue = engine->muglues[index];
        engine->muglue_levels[index] = engine->group_level;
    } else {
        engine->muglue_levels[index] = 0U;
    }
    engine->muglues[index] = value;
    return 0;
}

static int assign_box(struct hstex_engine *engine, uint32_t index,
                      struct hstex_box value, bool requested_global,
                      char *error, size_t error_capacity)
{
    if ((size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (reserve_saves(engine, engine->save_count + 1U, error,
                          error_capacity) != 0) {
            return -1;
        }
        struct hstex_save_entry *save = &engine->saves[engine->save_count++];
        memset(save, 0, sizeof(*save));
        save->kind = HSTEX_SAVE_BOX;
        save->index = index;
        save->level = engine->group_level;
        save->previous_level = engine->box_levels[index];
        save->previous.box = engine->boxes[index];
        engine->box_levels[index] = engine->group_level;
    } else {
        engine->box_levels[index] = 0U;
    }
    engine->boxes[index] = value;
    return 0;
}

/* \parshape holds a whole list of pairs, so the value saved for a group is
   the offset of the shape in the arena rather than the shape itself; see
   docs/DECISIONS.md, parshape. */
static int assign_parshape(struct hstex_engine *engine, uint32_t shape,
                           bool requested_global, char *error,
                           size_t error_capacity)
{
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_PAR_SHAPE, 0U,
                       engine->parshape_level, (int32_t)engine->parshape, 0U,
                       error, error_capacity) != 0) {
            return -1;
        }
        engine->parshape_level = engine->group_level;
    } else {
        engine->parshape_level = 0U;
    }
    engine->parshape = shape;
    return 0;
}

static int assign_dimen_parameter(struct hstex_engine *engine, uint32_t index,
                                  int32_t value, bool requested_global,
                                  char *error, size_t error_capacity)
{
    if (index >= (uint32_t)HSTEX_DIMEN_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid dimen-parameter assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_DIMEN_PARAMETER, index,
                       engine->dimen_parameter_levels[index],
                       engine->dimen_parameters[index], 0U, error,
                       error_capacity) != 0) {
            return -1;
        }
        engine->dimen_parameter_levels[index] = engine->group_level;
    } else {
        engine->dimen_parameter_levels[index] = 0U;
    }
    engine->dimen_parameters[index] = value;
    return 0;
}

static int assign_glue_parameter(struct hstex_engine *engine, uint32_t index,
                                 struct hstex_glue value,
                                 bool requested_global, char *error,
                                 size_t error_capacity)
{
    if (index >= (uint32_t)HSTEX_GLUE_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid glue-parameter assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (reserve_saves(engine, engine->save_count + 1U, error,
                          error_capacity) != 0) {
            return -1;
        }
        struct hstex_save_entry *save = &engine->saves[engine->save_count++];
        memset(save, 0, sizeof(*save));
        save->kind = HSTEX_SAVE_GLUE_PARAMETER;
        save->index = index;
        save->level = engine->group_level;
        save->previous_level = engine->glue_parameter_levels[index];
        save->previous.glue = engine->glue_parameters[index];
        engine->glue_parameter_levels[index] = engine->group_level;
    } else {
        engine->glue_parameter_levels[index] = 0U;
    }
    engine->glue_parameters[index] = value;
    return 0;
}

static int assign_muglue_parameter(struct hstex_engine *engine, uint32_t index,
                                   struct hstex_glue value,
                                   bool requested_global, char *error,
                                   size_t error_capacity)
{
    if (index >= (uint32_t)HSTEX_MUGLUE_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid math-glue-parameter assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (reserve_saves(engine, engine->save_count + 1U, error,
                          error_capacity) != 0) {
            return -1;
        }
        struct hstex_save_entry *save = &engine->saves[engine->save_count++];
        memset(save, 0, sizeof(*save));
        save->kind = HSTEX_SAVE_MUGLUE_PARAMETER;
        save->index = index;
        save->level = engine->group_level;
        save->previous_level = engine->muglue_parameter_levels[index];
        save->previous.glue = engine->muglue_parameters[index];
        engine->muglue_parameter_levels[index] = engine->group_level;
    } else {
        engine->muglue_parameter_levels[index] = 0U;
    }
    engine->muglue_parameters[index] = value;
    return 0;
}

static int assign_integer_parameter(struct hstex_engine *engine,
                                    uint32_t index, int32_t value,
                                    bool requested_global, char *error,
                                    size_t error_capacity)
{
    if (index >= (uint32_t)HSTEX_INTEGER_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid integer parameter assignment");
    }
    bool global = assignment_is_global(engine, requested_global);
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_INTEGER_PARAMETER, index,
                       engine->integer_parameter_levels[index],
                       engine->integer_parameters[index], 0U, error,
                       error_capacity) != 0) {
            return -1;
        }
        engine->integer_parameter_levels[index] = engine->group_level;
    } else {
        engine->integer_parameter_levels[index] = 0U;
    }
    engine->integer_parameters[index] = value;
    if (index == (uint32_t)HSTEX_INTEGER_END_LINE_CHARACTER) {
        engine->lexical_state.end_line_character = value;
    }
    return 0;
}

static int register_integer_primitive(struct hstex_engine *engine,
                                      const char *name,
                                      enum hstex_command command,
                                      int32_t value, char *error,
                                      size_t error_capacity)
{
    hstex_cs_id identifier = 0U;
    size_t length = strlen(name);
    if (hstex_symbol_intern(&engine->lexical_state.symbols,
                            HSTEX_SYMBOL_REGULAR, (const uint8_t *)name, length,
                            &identifier, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_meaning meaning = {
        .command = command,
        .level = 0U,
        .primitive_origin = identifier,
        .value = {.integer = value},
    };
    return set_meaning(engine, identifier, meaning, true, error,
                       error_capacity);
}

static int register_primitive(struct hstex_engine *engine, const char *name,
                              enum hstex_command command, char *error,
                              size_t error_capacity)
{
    hstex_cs_id identifier = 0U;
    size_t length = strlen(name);
    if (hstex_symbol_intern(&engine->lexical_state.symbols,
                            HSTEX_SYMBOL_REGULAR, (const uint8_t *)name, length,
                            &identifier, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_meaning meaning = {
        .command = command,
        .level = 0U,
        .primitive_origin = identifier,
        .value = {.macro_identifier = 0U},
    };
    return set_meaning(engine, identifier, meaning, true, error,
                       error_capacity);
}

int hstex_engine_init(struct hstex_engine *engine, char *error,
                      size_t error_capacity)
{
    if (engine == NULL) {
        return set_error(error, error_capacity, "hstex_engine_init: null engine");
    }
    memset(engine, 0, sizeof(*engine));
    if (hstex_lexical_state_init(&engine->lexical_state, error,
                                 error_capacity) != 0) {
        return -1;
    }
    hstex_source_stack_init(&engine->sources, &engine->lexical_state);
    engine->count_capacity = (size_t)HSTEX_COUNT_REGISTER_CAPACITY;
    engine->counts = calloc(engine->count_capacity, sizeof(*engine->counts));
    engine->count_levels =
        calloc(engine->count_capacity, sizeof(*engine->count_levels));
    engine->dimens = calloc(engine->count_capacity, sizeof(*engine->dimens));
    engine->dimen_levels =
        calloc(engine->count_capacity, sizeof(*engine->dimen_levels));
    engine->glues = calloc(engine->count_capacity, sizeof(*engine->glues));
    engine->glue_levels =
        calloc(engine->count_capacity, sizeof(*engine->glue_levels));
    engine->muglues = calloc(engine->count_capacity, sizeof(*engine->muglues));
    engine->muglue_levels =
        calloc(engine->count_capacity, sizeof(*engine->muglue_levels));
    engine->token_registers =
        calloc(engine->count_capacity, sizeof(*engine->token_registers));
    engine->token_register_levels =
        calloc(engine->count_capacity, sizeof(*engine->token_register_levels));
    engine->boxes = calloc(engine->count_capacity, sizeof(*engine->boxes));
    engine->box_levels =
        calloc(engine->count_capacity, sizeof(*engine->box_levels));
    engine->hyphen_roots =
        calloc(engine->count_capacity, sizeof(*engine->hyphen_roots));
    engine->page_builder = calloc(1U, sizeof(*engine->page_builder));
    if (engine->counts == NULL || engine->count_levels == NULL ||
        engine->dimens == NULL || engine->dimen_levels == NULL ||
        engine->glues == NULL || engine->glue_levels == NULL ||
        engine->muglues == NULL || engine->muglue_levels == NULL ||
        engine->token_registers == NULL ||
        engine->token_register_levels == NULL || engine->boxes == NULL ||
        engine->box_levels == NULL || engine->hyphen_roots == NULL ||
        engine->page_builder == NULL) {
        (void)set_error(error, error_capacity,
                        "register allocation failed");
        hstex_engine_destroy(engine);
        return -1;
    }
    engine->integer_parameters[HSTEX_INTEGER_END_LINE_CHARACTER] = 13;
    engine->integer_parameters[HSTEX_INTEGER_NEW_LINE_CHARACTER] = -1;
    engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER] = 92;
    engine->integer_parameters[HSTEX_INTEGER_DEFAULT_HYPHEN_CHAR] = 45;
    engine->integer_parameters[HSTEX_INTEGER_DEFAULT_SKEW_CHAR] = -1;
    engine->integer_parameters[HSTEX_INTEGER_MAGNIFICATION] = 1000;
    /* The non-zero defaults the reference starts an INITEX run with. */
    engine->integer_parameters[HSTEX_INTEGER_TOLERANCE] = 10000;
    engine->integer_parameters[HSTEX_INTEGER_HANG_AFTER] = 1;
    /* pdfTeX defaults that are not zero: one big point of pixel size and a
       72 dpi image resolution. */
    engine->integer_parameters[HSTEX_INTEGER_PDF_IMAGE_RESOLUTION] = 72;
    engine->dimen_parameters[HSTEX_DIMEN_PDF_PX_DIMEN] = 65782;
    engine->integer_parameters[HSTEX_INTEGER_MAX_DEAD_CYCLES] = 25;
    engine->prev_depth = -INT32_C(1000) * INT32_C(65536);
    engine->active_vbox_builder = engine->page_builder;
    engine->interaction_mode = HSTEX_INTERACTION_ERROR_STOP;
    for (size_t character = 0U; character < 256U; ++character) {
        engine->code_tables[0][character] = 1000;
        engine->code_tables[3][character] = (int32_t)character;
        engine->code_tables[4][character] = -1;
    }
    /* A full stop is the one character that names no delimiter at all. */
    engine->code_tables[4][(size_t)'.'] = 0;
    /* A letter is a variable-family italic and a digit a variable-family
       roman before any format has said otherwise; see
       docs/DECISIONS.md, initex-math-codes. */
    for (uint32_t character = (uint32_t)'0'; character <= (uint32_t)'9';
         ++character) {
        engine->code_tables[3][character] = (int32_t)(character + 0x7000U);
    }
    for (uint32_t character = (uint32_t)'A'; character <= (uint32_t)'Z';
         ++character) {
        engine->code_tables[0][character] = 999;
        engine->code_tables[1][character] =
            (int32_t)(character + ((uint32_t)'a' - (uint32_t)'A'));
        engine->code_tables[2][character] = (int32_t)character;
        engine->code_tables[3][character] = (int32_t)(character + 0x7100U);
    }
    for (uint32_t character = (uint32_t)'a'; character <= (uint32_t)'z';
         ++character) {
        engine->code_tables[1][character] = (int32_t)character;
        engine->code_tables[2][character] =
            (int32_t)(character - ((uint32_t)'a' - (uint32_t)'A'));
        engine->code_tables[3][character] = (int32_t)(character + 0x7100U);
    }

    time_t now = time(NULL);
    struct tm broken_down;
    if (now != (time_t)-1 && localtime_r(&now, &broken_down) != NULL) {
        engine->integer_parameters[HSTEX_INTEGER_TIME] =
            (int32_t)(broken_down.tm_hour * 60 + broken_down.tm_min);
        engine->integer_parameters[HSTEX_INTEGER_DAY] = broken_down.tm_mday;
        engine->integer_parameters[HSTEX_INTEGER_MONTH] = broken_down.tm_mon + 1;
        engine->integer_parameters[HSTEX_INTEGER_YEAR] =
            broken_down.tm_year + 1900;
    }
    static const struct {
        const char *name;
        enum hstex_command command;
    } primitives[] = {
        {"relax", HSTEX_COMMAND_RELAX},
        {"def", HSTEX_COMMAND_DEF},
        {"gdef", HSTEX_COMMAND_GDEF},
        {"edef", HSTEX_COMMAND_EDEF},
        {"xdef", HSTEX_COMMAND_XDEF},
        {"let", HSTEX_COMMAND_LET},
        {"futurelet", HSTEX_COMMAND_FUTURE_LET},
        {"afterassignment", HSTEX_COMMAND_AFTER_ASSIGNMENT},
        {"aftergroup", HSTEX_COMMAND_AFTER_GROUP},
        {"long", HSTEX_COMMAND_LONG},
        {"outer", HSTEX_COMMAND_OUTER},
        {"protected", HSTEX_COMMAND_PROTECTED},
        {"sfcode", HSTEX_COMMAND_SF_CODE},
        {"lccode", HSTEX_COMMAND_LC_CODE},
        {"uccode", HSTEX_COMMAND_UC_CODE},
        {"mathcode", HSTEX_COMMAND_MATH_CODE},
        {"delcode", HSTEX_COMMAND_DEL_CODE},
        {"ifdefined", HSTEX_COMMAND_IF_DEFINED},
        {"ifcsname", HSTEX_COMMAND_IF_CS_NAME},
        {"ifcat", HSTEX_COMMAND_IF_CAT},
        {"ifodd", HSTEX_COMMAND_IF_ODD},
        {"ifcase", HSTEX_COMMAND_IF_CASE},
        {"or", HSTEX_COMMAND_OR},
        {"iffontchar", HSTEX_COMMAND_IF_FONT_CHAR},
        {"unless", HSTEX_COMMAND_UNLESS},
        {"lowercase", HSTEX_COMMAND_LOWER_CASE},
        {"uppercase", HSTEX_COMMAND_UPPER_CASE},
        {"ignorespaces", HSTEX_COMMAND_IGNORE_SPACES},
        {"dump", HSTEX_COMMAND_DUMP},
        {"global", HSTEX_COMMAND_GLOBAL},
        {"expandafter", HSTEX_COMMAND_EXPAND_AFTER},
        {"noexpand", HSTEX_COMMAND_NO_EXPAND},
        {"csname", HSTEX_COMMAND_CS_NAME},
        {"endcsname", HSTEX_COMMAND_END_CS_NAME},
        {"expanded", HSTEX_COMMAND_EXPANDED},
        {"unexpanded", HSTEX_COMMAND_UNEXPANDED},
        {"detokenize", HSTEX_COMMAND_DETOKENIZE},
        {"scantokens", HSTEX_COMMAND_SCAN_TOKENS},
        {"begingroup", HSTEX_COMMAND_BEGIN_GROUP},
        {"endgroup", HSTEX_COMMAND_END_GROUP},
        {"catcode", HSTEX_COMMAND_CAT_CODE},
        {"chardef", HSTEX_COMMAND_CHAR_DEF},
        {"countdef", HSTEX_COMMAND_COUNT_DEF},
        {"count", HSTEX_COMMAND_COUNT},
        {"ifnum", HSTEX_COMMAND_IF_NUM},
        {"ifdim", HSTEX_COMMAND_IF_DIM},
        {"ifhmode", HSTEX_COMMAND_IF_H_MODE},
        {"ifvmode", HSTEX_COMMAND_IF_V_MODE},
        {"ifmmode", HSTEX_COMMAND_IF_M_MODE},
        {"ifinner", HSTEX_COMMAND_IF_INNER},
        {"ifincsname", HSTEX_COMMAND_IF_IN_CS_NAME},
        {"ifx", HSTEX_COMMAND_IF_X},
        {"iftrue", HSTEX_COMMAND_IF_TRUE},
        {"iffalse", HSTEX_COMMAND_IF_FALSE},
        {"else", HSTEX_COMMAND_ELSE},
        {"fi", HSTEX_COMMAND_FI},
        {"input", HSTEX_COMMAND_INPUT},
        {"pdftexrevision", HSTEX_COMMAND_PDF_TEX_REVISION},
        {"pdffilesize", HSTEX_COMMAND_PDF_FILE_SIZE},
        {"pdfstrcmp", HSTEX_COMMAND_PDF_STRING_COMPARE},
        {"pdfmatch", HSTEX_COMMAND_PDF_MATCH},
        {"pdflastmatch", HSTEX_COMMAND_PDF_LAST_MATCH},
        {"pdfescapestring", HSTEX_COMMAND_PDF_ESCAPE_STRING},
        {"pdfescapename", HSTEX_COMMAND_PDF_ESCAPE_NAME},
        {"pdfescapehex", HSTEX_COMMAND_PDF_ESCAPE_HEX},
        {"pdfunescapehex", HSTEX_COMMAND_PDF_UNESCAPE_HEX},
        {"pdfglyphtounicode", HSTEX_COMMAND_PDF_GLYPH_TO_UNICODE},
        {"end", HSTEX_COMMAND_END},
        {"endinput", HSTEX_COMMAND_END_INPUT},
        {"errmessage", HSTEX_COMMAND_ERROR_MESSAGE},
        {"advance", HSTEX_COMMAND_ADVANCE},
        {"multiply", HSTEX_COMMAND_MULTIPLY},
        {"divide", HSTEX_COMMAND_DIVIDE},
        {"the", HSTEX_COMMAND_THE},
        {"number", HSTEX_COMMAND_NUMBER},
        {"romannumeral", HSTEX_COMMAND_ROMAN_NUMERAL},
        {"numexpr", HSTEX_COMMAND_NUM_EXPR},
        {"dimexpr", HSTEX_COMMAND_DIM_EXPR},
        {"glueexpr", HSTEX_COMMAND_GLUE_EXPR},
        {"muexpr", HSTEX_COMMAND_MU_EXPR},
        {"immediate", HSTEX_COMMAND_IMMEDIATE},
        {"openout", HSTEX_COMMAND_OPEN_OUT},
        {"write", HSTEX_COMMAND_WRITE},
        {"closeout", HSTEX_COMMAND_CLOSE_OUT},
        {"openin", HSTEX_COMMAND_OPEN_IN},
        {"read", HSTEX_COMMAND_READ},
        {"readline", HSTEX_COMMAND_READ_LINE},
        {"closein", HSTEX_COMMAND_CLOSE_IN},
        {"ifeof", HSTEX_COMMAND_IF_EOF},
        {"meaning", HSTEX_COMMAND_MEANING},
        {"string", HSTEX_COMMAND_STRING},
        {"jobname", HSTEX_COMMAND_JOB_NAME},
        {"if", HSTEX_COMMAND_IF_CHAR},
        {"inputlineno", HSTEX_COMMAND_INPUT_LINE_NUMBER},
        {"message", HSTEX_COMMAND_MESSAGE},
        {"mathchardef", HSTEX_COMMAND_MATH_CHAR_DEF},
        {"radical", HSTEX_COMMAND_RADICAL},
        {"marks", HSTEX_COMMAND_MARKS},
        {"patterns", HSTEX_COMMAND_PATTERNS},
        {"hyphenation", HSTEX_COMMAND_HYPHENATION},
        {"dimendef", HSTEX_COMMAND_DIMEN_DEF},
        {"skipdef", HSTEX_COMMAND_SKIP_DEF},
        {"muskipdef", HSTEX_COMMAND_MUSKIP_DEF},
        {"toksdef", HSTEX_COMMAND_TOKS_DEF},
        {"dimen", HSTEX_COMMAND_DIMEN},
        {"skip", HSTEX_COMMAND_SKIP},
        {"muskip", HSTEX_COMMAND_MUSKIP},
        {"toks", HSTEX_COMMAND_TOKS},
        {"box", HSTEX_COMMAND_BOX},
        {"copy", HSTEX_COMMAND_COPY},
        {"setbox", HSTEX_COMMAND_SET_BOX},
        {"hbox", HSTEX_COMMAND_HBOX},
        {"vbox", HSTEX_COMMAND_VBOX},
        {"vtop", HSTEX_COMMAND_VTOP},
        {"vsplit", HSTEX_COMMAND_VSPLIT},
        {"lastbox", HSTEX_COMMAND_LAST_BOX},
        {"char", HSTEX_COMMAND_CHAR},
        {" ", HSTEX_COMMAND_CONTROL_SPACE},
        {"/", HSTEX_COMMAND_ITALIC_CORRECTION},
        {"penalty", HSTEX_COMMAND_PENALTY},
        {"vrule", HSTEX_COMMAND_VRULE},
        {"hrule", HSTEX_COMMAND_HRULE},
        {"kern", HSTEX_COMMAND_KERN},
        {"pdfcatalog", HSTEX_COMMAND_PDF_CATALOG},
        {"pdfinfo", HSTEX_COMMAND_PDF_INFO},
        {"pdfobj", HSTEX_COMMAND_PDF_OBJECT},
        {"pdfrefobj", HSTEX_COMMAND_PDF_REF_OBJECT},
        {"pdfliteral", HSTEX_COMMAND_PDF_LITERAL},
        {"pdfdest", HSTEX_COMMAND_PDF_DEST},
        {"pdfstartlink", HSTEX_COMMAND_PDF_START_LINK},
        {"pdfendlink", HSTEX_COMMAND_PDF_END_LINK},
        {"pdfoutline", HSTEX_COMMAND_PDF_OUTLINE},
        {"pdfxform", HSTEX_COMMAND_PDF_XFORM},
        {"pdfannot", HSTEX_COMMAND_PDF_ANNOT},
        {"pdfcolorstack", HSTEX_COMMAND_PDF_COLOR_STACK},
        {"pdfcolorstackinit", HSTEX_COMMAND_PDF_COLOR_STACK_INIT},
        {"font", HSTEX_COMMAND_FONT},
        {"fontdimen", HSTEX_COMMAND_FONT_DIMEN},
        {"hyphenchar", HSTEX_COMMAND_HYPHEN_CHAR},
        {"skewchar", HSTEX_COMMAND_SKEW_CHAR},
        {"fontname", HSTEX_COMMAND_FONT_NAME},
        {"prevdepth", HSTEX_COMMAND_PREV_DEPTH},
    };
    for (size_t index = 0U; index < sizeof(primitives) / sizeof(primitives[0]);
         ++index) {
        if (register_primitive(engine, primitives[index].name,
                               primitives[index].command, error,
                               error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        int32_t subtype;
        enum hstex_command command;
    } skip_primitives[] = {
        {"vskip", 0, HSTEX_COMMAND_VSKIP},
        {"vfil", 1, HSTEX_COMMAND_VSKIP},
        {"vfill", 2, HSTEX_COMMAND_VSKIP},
        {"vss", 3, HSTEX_COMMAND_VSKIP},
        {"vfilneg", 4, HSTEX_COMMAND_VSKIP},
        {"hskip", 0, HSTEX_COMMAND_HSKIP},
        {"hfil", 1, HSTEX_COMMAND_HSKIP},
        {"hfill", 2, HSTEX_COMMAND_HSKIP},
        {"hss", 3, HSTEX_COMMAND_HSKIP},
        {"hfilneg", 4, HSTEX_COMMAND_HSKIP},
        {"leaders", 0, HSTEX_COMMAND_LEADERS},
        {"cleaders", 1, HSTEX_COMMAND_LEADERS},
        {"xleaders", 2, HSTEX_COMMAND_LEADERS},
        {"indent", 1, HSTEX_COMMAND_INDENT},
        {"noindent", 0, HSTEX_COMMAND_INDENT},
        {"spacefactor", 0, HSTEX_COMMAND_SPACE_FACTOR},
        {"unskip", (int32_t)HSTEX_NODE_GLUE, HSTEX_COMMAND_REMOVE_LAST},
        {"unkern", (int32_t)HSTEX_NODE_KERN, HSTEX_COMMAND_REMOVE_LAST},
        {"unpenalty", (int32_t)HSTEX_NODE_PENALTY, HSTEX_COMMAND_REMOVE_LAST},
    };
    for (size_t index = 0U;
         index < sizeof(skip_primitives) / sizeof(skip_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, skip_primitives[index].name,
                skip_primitives[index].command,
                skip_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_command command;
        int32_t subtype;
    } math_font_primitives[] = {
        {"textfont", HSTEX_COMMAND_MATH_FONT, (int32_t)HSTEX_MATH_TEXT},
        {"scriptfont", HSTEX_COMMAND_MATH_FONT, (int32_t)HSTEX_MATH_SCRIPT},
        {"scriptscriptfont", HSTEX_COMMAND_MATH_FONT,
         (int32_t)HSTEX_MATH_SCRIPT_SCRIPT},
        {"mathchar", HSTEX_COMMAND_MATH_CHAR, 0},
        {"mskip", HSTEX_COMMAND_MATH_SKIP, 0},
        {"mkern", HSTEX_COMMAND_MATH_KERN, 0},
        {"mathord", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_ORD},
        {"mathop", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_OP},
        {"mathbin", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_BIN},
        {"mathrel", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_REL},
        {"mathopen", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_OPEN},
        {"mathclose", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_CLOSE},
        {"mathpunct", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_PUNCT},
        {"mathinner", HSTEX_COMMAND_MATH_CLASS, (int32_t)HSTEX_ATOM_INNER},
        {"displaystyle", HSTEX_COMMAND_MATH_STYLE,
         (int32_t)HSTEX_STYLE_DISPLAY},
        {"textstyle", HSTEX_COMMAND_MATH_STYLE, (int32_t)HSTEX_STYLE_TEXT},
        {"scriptstyle", HSTEX_COMMAND_MATH_STYLE, (int32_t)HSTEX_STYLE_SCRIPT},
        {"scriptscriptstyle", HSTEX_COMMAND_MATH_STYLE,
         (int32_t)HSTEX_STYLE_SCRIPT_SCRIPT},
        {"mathchoice", HSTEX_COMMAND_MATH_CHOICE, 0},
        {"delimiter", HSTEX_COMMAND_DELIMITER, 0},
        {"left", HSTEX_COMMAND_LEFT_RIGHT, 0},
        {"right", HSTEX_COMMAND_LEFT_RIGHT, 1},
        {"middle", HSTEX_COMMAND_LEFT_RIGHT, 2},
        {"accent", HSTEX_COMMAND_ACCENT, 0},
        {"vcenter", HSTEX_COMMAND_VCENTER, 0},
        {"nonscript", HSTEX_COMMAND_NON_SCRIPT, 0},
        {"parshape", HSTEX_COMMAND_PAR_SHAPE, 0},
        {"leftmarginkern", HSTEX_COMMAND_MARGIN_KERN,
         (int32_t)HSTEX_MARGIN_KERN_LEFT},
        {"rightmarginkern", HSTEX_COMMAND_MARGIN_KERN,
         (int32_t)HSTEX_MARGIN_KERN_RIGHT},
        {"eqno", HSTEX_COMMAND_EQUATION_NUMBER, 0},
        {"leqno", HSTEX_COMMAND_EQUATION_NUMBER, 1},
        {"halign", HSTEX_COMMAND_HALIGN, 0},
        {"cr", HSTEX_COMMAND_CR, 0},
        {"crcr", HSTEX_COMMAND_CR, 1},
        {"noalign", HSTEX_COMMAND_NO_ALIGN, 0},
        {"omit", HSTEX_COMMAND_OMIT, 0},
        {"span", HSTEX_COMMAND_SPAN, 0},
        {"limits", HSTEX_COMMAND_MATH_LIMITS, 1},
        {"nolimits", HSTEX_COMMAND_MATH_LIMITS, 0},
        {"displaylimits", HSTEX_COMMAND_MATH_LIMITS, 2},
    };
    for (size_t index = 0U;
         index < sizeof(math_font_primitives) / sizeof(math_font_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, math_font_primitives[index].name,
                math_font_primitives[index].command,
                math_font_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const char *fraction_primitives[] = {
        "over",
        "atop",
        "above",
        "overwithdelims",
        "atopwithdelims",
        "abovewithdelims",
    };
    for (size_t index = 0U;
         index < sizeof(fraction_primitives) / sizeof(fraction_primitives[0]);
         ++index) {
        if (register_integer_primitive(engine, fraction_primitives[index],
                                       HSTEX_COMMAND_FRACTION, (int32_t)index,
                                       error, error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const char *line_primitives[] = {
        "overline",
        "underline",
    };
    for (size_t index = 0U;
         index < sizeof(line_primitives) / sizeof(line_primitives[0]);
         ++index) {
        if (register_integer_primitive(engine, line_primitives[index],
                                       HSTEX_COMMAND_OVER_UNDER_LINE,
                                       (int32_t)index, error,
                                       error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const char *penalty_array_primitives[] = {
        "interlinepenalties",
        "clubpenalties",
        "widowpenalties",
        "displaywidowpenalties",
    };
    for (size_t index = 0U;
         index < sizeof(penalty_array_primitives) /
                     sizeof(penalty_array_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, penalty_array_primitives[index],
                HSTEX_COMMAND_PENALTY_ARRAY, (int32_t)index, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const char *engine_state_integer_primitives[] = {
        "currentgrouplevel",
        "currentgrouptype",
        "currentiflevel",
        "currentiftype",
        "currentifbranch",
        "badness",
    };
    for (size_t index = 0U;
         index < sizeof(engine_state_integer_primitives) /
                     sizeof(engine_state_integer_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, engine_state_integer_primitives[index],
                HSTEX_COMMAND_ENGINE_STATE_INTEGER, (int32_t)index, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_unbox subtype;
    } unbox_primitives[] = {
        {"unhbox", HSTEX_UNBOX_HORIZONTAL},
        {"unhcopy", HSTEX_UNBOX_HORIZONTAL_COPY},
        {"unvbox", HSTEX_UNBOX_VERTICAL},
        {"unvcopy", HSTEX_UNBOX_VERTICAL_COPY},
    };
    for (size_t index = 0U;
         index < sizeof(unbox_primitives) / sizeof(unbox_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, unbox_primitives[index].name, HSTEX_COMMAND_UNBOX,
                (int32_t)unbox_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_shift_box subtype;
    } shift_box_primitives[] = {
        {"raise", HSTEX_SHIFT_RAISE},
        {"lower", HSTEX_SHIFT_LOWER},
        {"moveleft", HSTEX_SHIFT_MOVE_LEFT},
        {"moveright", HSTEX_SHIFT_MOVE_RIGHT},
    };
    for (size_t index = 0U;
         index < sizeof(shift_box_primitives) / sizeof(shift_box_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, shift_box_primitives[index].name,
                HSTEX_COMMAND_SHIFT_BOX,
                (int32_t)shift_box_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_last_item subtype;
    } last_item_primitives[] = {
        {"lastpenalty", HSTEX_LAST_PENALTY},
        {"lastkern", HSTEX_LAST_KERN},
        {"lastskip", HSTEX_LAST_SKIP},
        {"lastnodetype", HSTEX_LAST_NODE_TYPE},
    };
    for (size_t index = 0U;
         index < sizeof(last_item_primitives) / sizeof(last_item_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, last_item_primitives[index].name,
                HSTEX_COMMAND_LAST_ITEM,
                (int32_t)last_item_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_pdf_last subtype;
    } pdf_last_primitives[] = {
        {"pdflastobj", HSTEX_PDF_LAST_OBJECT},
        {"pdflastannot", HSTEX_PDF_LAST_ANNOTATION},
        {"pdflastlink", HSTEX_PDF_LAST_LINK},
        {"pdflastxform", HSTEX_PDF_LAST_FORM},
        {"pdflastximage", HSTEX_PDF_LAST_IMAGE},
    };
    for (size_t index = 0U;
         index < sizeof(pdf_last_primitives) / sizeof(pdf_last_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, pdf_last_primitives[index].name,
                HSTEX_COMMAND_PDF_LAST_NUMBER,
                (int32_t)pdf_last_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_if_box subtype;
    } if_box_primitives[] = {
        {"ifhbox", HSTEX_IF_BOX_HORIZONTAL},
        {"ifvbox", HSTEX_IF_BOX_VERTICAL},
        {"ifvoid", HSTEX_IF_BOX_VOID},
    };
    for (size_t index = 0U;
         index < sizeof(if_box_primitives) / sizeof(if_box_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, if_box_primitives[index].name, HSTEX_COMMAND_IF_BOX,
                (int32_t)if_box_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_font_char_code subtype;
    } font_code_primitives[] = {
        {"lpcode", HSTEX_FONT_CODE_LEFT_PROTRUSION},
        {"rpcode", HSTEX_FONT_CODE_RIGHT_PROTRUSION},
        {"efcode", HSTEX_FONT_CODE_EXPANSION},
        {"tagcode", HSTEX_FONT_CODE_TAG},
    };
    for (size_t index = 0U;
         index < sizeof(font_code_primitives) / sizeof(font_code_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, font_code_primitives[index].name,
                HSTEX_COMMAND_FONT_CHAR_CODE,
                (int32_t)font_code_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_font_char_dimen subtype;
    } font_char_primitives[] = {
        {"fontcharwd", HSTEX_FONT_CHAR_WIDTH},
        {"fontcharht", HSTEX_FONT_CHAR_HEIGHT},
        {"fontchardp", HSTEX_FONT_CHAR_DEPTH},
        {"fontcharic", HSTEX_FONT_CHAR_ITALIC},
    };
    for (size_t index = 0U;
         index < sizeof(font_char_primitives) / sizeof(font_char_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, font_char_primitives[index].name,
                HSTEX_COMMAND_FONT_CHAR_DIMEN,
                (int32_t)font_char_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_box_dimen subtype;
    } box_dimen_primitives[] = {
        {"wd", HSTEX_BOX_DIMEN_WIDTH},
        {"ht", HSTEX_BOX_DIMEN_HEIGHT},
        {"dp", HSTEX_BOX_DIMEN_DEPTH},
    };
    for (size_t index = 0U;
         index < sizeof(box_dimen_primitives) / sizeof(box_dimen_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, box_dimen_primitives[index].name,
                HSTEX_COMMAND_BOX_DIMEN,
                (int32_t)box_dimen_primitives[index].subtype, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const char *page_integer_primitives[] = {
        "deadcycles",
        "insertpenalties",
    };
    for (size_t index = 0U;
         index < sizeof(page_integer_primitives) /
                     sizeof(page_integer_primitives[0]);
         ++index) {
        if (register_integer_primitive(engine, page_integer_primitives[index],
                                       HSTEX_COMMAND_PAGE_INTEGER,
                                       (int32_t)index, error,
                                       error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const char *page_dimen_primitives[] = {
        "pagegoal",       "pagetotal",       "pagestretch",
        "pagefilstretch", "pagefillstretch", "pagefilllstretch",
        "pageshrink",     "pagedepth",
    };
    for (size_t index = 0U;
         index < sizeof(page_dimen_primitives) /
                     sizeof(page_dimen_primitives[0]);
         ++index) {
        if (register_integer_primitive(engine, page_dimen_primitives[index],
                                       HSTEX_COMMAND_PAGE_DIMEN,
                                       (int32_t)index, error,
                                       error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    if (register_integer_primitive(engine, "pdfshellescape",
                                   HSTEX_COMMAND_INTEGER_CONSTANT, 0, error,
                                   error_capacity) != 0 ||
        register_integer_primitive(engine, "pdftexversion",
                                   HSTEX_COMMAND_INTEGER_CONSTANT,
                                   HSTEX_PDFTEX_VERSION, error,
                                   error_capacity) != 0) {
        hstex_engine_destroy(engine);
        return -1;
    }
    static const struct {
        const char *name;
        enum hstex_interaction_mode mode;
    } interaction_primitives[] = {
        {"batchmode", HSTEX_INTERACTION_BATCH},
        {"nonstopmode", HSTEX_INTERACTION_NONSTOP},
        {"scrollmode", HSTEX_INTERACTION_SCROLL},
        {"errorstopmode", HSTEX_INTERACTION_ERROR_STOP},
    };
    for (size_t index = 0U;
         index < sizeof(interaction_primitives) /
                     sizeof(interaction_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, interaction_primitives[index].name,
                HSTEX_COMMAND_INTERACTION_MODE,
                (int32_t)interaction_primitives[index].mode, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_integer_parameter parameter;
    } integer_primitives[] = {
        {"endlinechar", HSTEX_INTEGER_END_LINE_CHARACTER},
        {"newlinechar", HSTEX_INTEGER_NEW_LINE_CHARACTER},
        {"escapechar", HSTEX_INTEGER_ESCAPE_CHARACTER},
        {"globaldefs", HSTEX_INTEGER_GLOBAL_DEFS},
        {"time", HSTEX_INTEGER_TIME},
        {"day", HSTEX_INTEGER_DAY},
        {"month", HSTEX_INTEGER_MONTH},
        {"year", HSTEX_INTEGER_YEAR},
        {"pretolerance", HSTEX_INTEGER_PRETOLERANCE},
        {"tolerance", HSTEX_INTEGER_TOLERANCE},
        {"hbadness", HSTEX_INTEGER_HBADNESS},
        {"vbadness", HSTEX_INTEGER_VBADNESS},
        {"linepenalty", HSTEX_INTEGER_LINE_PENALTY},
        {"hyphenpenalty", HSTEX_INTEGER_HYPHEN_PENALTY},
        {"exhyphenpenalty", HSTEX_INTEGER_EX_HYPHEN_PENALTY},
        {"binoppenalty", HSTEX_INTEGER_BIN_OP_PENALTY},
        {"relpenalty", HSTEX_INTEGER_REL_PENALTY},
        {"clubpenalty", HSTEX_INTEGER_CLUB_PENALTY},
        {"widowpenalty", HSTEX_INTEGER_WIDOW_PENALTY},
        {"displaywidowpenalty", HSTEX_INTEGER_DISPLAY_WIDOW_PENALTY},
        {"brokenpenalty", HSTEX_INTEGER_BROKEN_PENALTY},
        {"predisplaypenalty", HSTEX_INTEGER_PRE_DISPLAY_PENALTY},
        {"postdisplaypenalty", HSTEX_INTEGER_POST_DISPLAY_PENALTY},
        {"interlinepenalty", HSTEX_INTEGER_INTERLINE_PENALTY},
        {"doublehyphendemerits", HSTEX_INTEGER_DOUBLE_HYPHEN_DEMERITS},
        {"finalhyphendemerits", HSTEX_INTEGER_FINAL_HYPHEN_DEMERITS},
        {"adjdemerits", HSTEX_INTEGER_ADJ_DEMERITS},
        {"tracinglostchars", HSTEX_INTEGER_TRACING_LOST_CHARS},
        {"tracingstats", HSTEX_INTEGER_TRACING_STATS},
        {"uchyph", HSTEX_INTEGER_UC_HYPH},
        {"defaulthyphenchar", HSTEX_INTEGER_DEFAULT_HYPHEN_CHAR},
        {"defaultskewchar", HSTEX_INTEGER_DEFAULT_SKEW_CHAR},
        {"delimiterfactor", HSTEX_INTEGER_DELIMITER_FACTOR},
        {"showboxbreadth", HSTEX_INTEGER_SHOW_BOX_BREADTH},
        {"showboxdepth", HSTEX_INTEGER_SHOW_BOX_DEPTH},
        {"errorcontextlines", HSTEX_INTEGER_ERROR_CONTEXT_LINES},
        {"maxdeadcycles", HSTEX_INTEGER_MAX_DEAD_CYCLES},
        {"lefthyphenmin", HSTEX_INTEGER_LEFT_HYPHEN_MIN},
        {"righthyphenmin", HSTEX_INTEGER_RIGHT_HYPHEN_MIN},
        {"language", HSTEX_INTEGER_LANGUAGE},
        {"mathgroup", HSTEX_INTEGER_MATH_GROUP},
        {"mag", HSTEX_INTEGER_MAGNIFICATION},
        {"tracingonline", HSTEX_INTEGER_TRACING_ONLINE},
        {"tracingcommands", HSTEX_INTEGER_TRACING_COMMANDS},
        {"tracingmacros", HSTEX_INTEGER_TRACING_MACROS},
        {"tracingparagraphs", HSTEX_INTEGER_TRACING_PARAGRAPHS},
        {"tracingpages", HSTEX_INTEGER_TRACING_PAGES},
        {"tracingoutput", HSTEX_INTEGER_TRACING_OUTPUT},
        {"tracingrestores", HSTEX_INTEGER_TRACING_RESTORES},
        {"tracingassigns", HSTEX_INTEGER_TRACING_ASSIGNS},
        {"tracinggroups", HSTEX_INTEGER_TRACING_GROUPS},
        {"tracingifs", HSTEX_INTEGER_TRACING_IFS},
        {"tracingscantokens", HSTEX_INTEGER_TRACING_SCAN_TOKENS},
        {"tracingnesting", HSTEX_INTEGER_TRACING_NESTING},
        {"pausing", HSTEX_INTEGER_PAUSING},
        {"holdinginserts", HSTEX_INTEGER_HOLDING_INSERTS},
        {"outputpenalty", HSTEX_INTEGER_OUTPUT_PENALTY},
        {"hangafter", HSTEX_INTEGER_HANG_AFTER},
        {"floatingpenalty", HSTEX_INTEGER_FLOATING_PENALTY},
        {"looseness", HSTEX_INTEGER_LOOSENESS},
        {"fam", HSTEX_INTEGER_FAMILY},
        {"predisplaydirection", HSTEX_INTEGER_PRE_DISPLAY_DIRECTION},
        {"lastlinefit", HSTEX_INTEGER_LAST_LINE_FIT},
        {"savingvdiscards", HSTEX_INTEGER_SAVING_VDISCARDS},
        {"savinghyphcodes", HSTEX_INTEGER_SAVING_HYPH_CODES},
        {"TeXXeTstate", HSTEX_INTEGER_TEXXET_STATE},
        {"pdfoutput", HSTEX_INTEGER_PDF_OUTPUT},
        {"pdfmajorversion", HSTEX_INTEGER_PDF_MAJOR_VERSION},
        {"pdfminorversion", HSTEX_INTEGER_PDF_MINOR_VERSION},
        {"pdfcompresslevel", HSTEX_INTEGER_PDF_COMPRESS_LEVEL},
        {"pdfobjcompresslevel", HSTEX_INTEGER_PDF_OBJ_COMPRESS_LEVEL},
        {"pdfdecimaldigits", HSTEX_INTEGER_PDF_DECIMAL_DIGITS},
        {"pdfpkresolution", HSTEX_INTEGER_PDF_PK_RESOLUTION},
        {"pdfdraftmode", HSTEX_INTEGER_PDF_DRAFT_MODE},
        {"pdfadjustspacing", HSTEX_INTEGER_PDF_ADJUST_SPACING},
        {"pdfprotrudechars", HSTEX_INTEGER_PDF_PROTRUDE_CHARS},
        {"pdfgentounicode", HSTEX_INTEGER_PDF_GEN_TO_UNICODE},
        {"pdfuniqueresname", HSTEX_INTEGER_PDF_UNIQUE_RES_NAME},
        {"pdfimageresolution", HSTEX_INTEGER_PDF_IMAGE_RESOLUTION},
    };
    for (size_t index = 0U;
         index < sizeof(integer_primitives) / sizeof(integer_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, integer_primitives[index].name,
                HSTEX_COMMAND_INTEGER_PARAMETER,
                (int32_t)integer_primitives[index].parameter, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_dimen_parameter parameter;
    } dimen_primitives[] = {
        {"hfuzz", HSTEX_DIMEN_HFUZZ},
        {"vfuzz", HSTEX_DIMEN_VFUZZ},
        {"overfullrule", HSTEX_DIMEN_OVERFULL_RULE},
        {"maxdepth", HSTEX_DIMEN_MAX_DEPTH},
        {"splitmaxdepth", HSTEX_DIMEN_SPLIT_MAX_DEPTH},
        {"boxmaxdepth", HSTEX_DIMEN_BOX_MAX_DEPTH},
        {"delimitershortfall", HSTEX_DIMEN_DELIMITER_SHORTFALL},
        {"nulldelimiterspace", HSTEX_DIMEN_NULL_DELIMITER_SPACE},
        {"scriptspace", HSTEX_DIMEN_SCRIPT_SPACE},
        {"parindent", HSTEX_DIMEN_PAR_INDENT},
        {"hsize", HSTEX_DIMEN_HSIZE},
        {"vsize", HSTEX_DIMEN_VSIZE},
        {"lineskiplimit", HSTEX_DIMEN_LINE_SKIP_LIMIT},
        {"mathsurround", HSTEX_DIMEN_MATH_SURROUND},
        {"predisplaysize", HSTEX_DIMEN_PRE_DISPLAY_SIZE},
        {"displaywidth", HSTEX_DIMEN_DISPLAY_WIDTH},
        {"displayindent", HSTEX_DIMEN_DISPLAY_INDENT},
        {"hangindent", HSTEX_DIMEN_HANG_INDENT},
        {"hoffset", HSTEX_DIMEN_HOFFSET},
        {"voffset", HSTEX_DIMEN_VOFFSET},
        {"emergencystretch", HSTEX_DIMEN_EMERGENCY_STRETCH},
        {"pdfpagewidth", HSTEX_DIMEN_PDF_PAGE_WIDTH},
        {"pdfpageheight", HSTEX_DIMEN_PDF_PAGE_HEIGHT},
        {"pdfhorigin", HSTEX_DIMEN_PDF_HORIGIN},
        {"pdfvorigin", HSTEX_DIMEN_PDF_VORIGIN},
        {"pdflinkmargin", HSTEX_DIMEN_PDF_LINK_MARGIN},
        {"pdfdestmargin", HSTEX_DIMEN_PDF_DEST_MARGIN},
        {"pdfthreadmargin", HSTEX_DIMEN_PDF_THREAD_MARGIN},
        {"pdfpxdimen", HSTEX_DIMEN_PDF_PX_DIMEN},
    };
    for (size_t index = 0U;
         index < sizeof(dimen_primitives) / sizeof(dimen_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, dimen_primitives[index].name,
                HSTEX_COMMAND_DIMEN_PARAMETER,
                (int32_t)dimen_primitives[index].parameter, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_glue_parameter parameter;
    } glue_primitives[] = {
        {"parskip", HSTEX_GLUE_PAR_SKIP},
        {"abovedisplayskip", HSTEX_GLUE_ABOVE_DISPLAY_SKIP},
        {"abovedisplayshortskip", HSTEX_GLUE_ABOVE_DISPLAY_SHORT_SKIP},
        {"belowdisplayskip", HSTEX_GLUE_BELOW_DISPLAY_SKIP},
        {"belowdisplayshortskip", HSTEX_GLUE_BELOW_DISPLAY_SHORT_SKIP},
        {"topskip", HSTEX_GLUE_TOP_SKIP},
        {"splittopskip", HSTEX_GLUE_SPLIT_TOP_SKIP},
        {"parfillskip", HSTEX_GLUE_PAR_FILL_SKIP},
        {"baselineskip", HSTEX_GLUE_BASELINE_SKIP},
        {"lineskip", HSTEX_GLUE_LINE_SKIP},
        {"leftskip", HSTEX_GLUE_LEFT_SKIP},
        {"rightskip", HSTEX_GLUE_RIGHT_SKIP},
        {"tabskip", HSTEX_GLUE_TAB_SKIP},
        {"spaceskip", HSTEX_GLUE_SPACE_SKIP},
        {"xspaceskip", HSTEX_GLUE_XSPACE_SKIP},
    };
    for (size_t index = 0U;
         index < sizeof(glue_primitives) / sizeof(glue_primitives[0]); ++index) {
        if (register_integer_primitive(
                engine, glue_primitives[index].name,
                HSTEX_COMMAND_GLUE_PARAMETER,
                (int32_t)glue_primitives[index].parameter, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_muglue_parameter parameter;
    } muglue_primitives[] = {
        {"thinmuskip", HSTEX_MUGLUE_THIN},
        {"medmuskip", HSTEX_MUGLUE_MEDIUM},
        {"thickmuskip", HSTEX_MUGLUE_THICK},
    };
    for (size_t index = 0U;
         index < sizeof(muglue_primitives) / sizeof(muglue_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, muglue_primitives[index].name,
                HSTEX_COMMAND_MUGLUE_PARAMETER,
                (int32_t)muglue_primitives[index].parameter, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    static const struct {
        const char *name;
        enum hstex_token_parameter parameter;
    } token_primitives[] = {
        {"output", HSTEX_TOKEN_OUTPUT},
        {"everypar", HSTEX_TOKEN_EVERY_PAR},
        {"everymath", HSTEX_TOKEN_EVERY_MATH},
        {"everydisplay", HSTEX_TOKEN_EVERY_DISPLAY},
        {"everyhbox", HSTEX_TOKEN_EVERY_HBOX},
        {"everyvbox", HSTEX_TOKEN_EVERY_VBOX},
        {"everyjob", HSTEX_TOKEN_EVERY_JOB},
        {"everycr", HSTEX_TOKEN_EVERY_CR},
        {"errhelp", HSTEX_TOKEN_ERROR_HELP},
        {"everyeof", HSTEX_TOKEN_EVERY_EOF},
    };
    for (size_t index = 0U;
         index < sizeof(token_primitives) / sizeof(token_primitives[0]);
         ++index) {
        if (register_integer_primitive(
                engine, token_primitives[index].name,
                HSTEX_COMMAND_TOKEN_PARAMETER,
                (int32_t)token_primitives[index].parameter, error,
                error_capacity) != 0) {
            hstex_engine_destroy(engine);
            return -1;
        }
    }
    if (register_integer_primitive(engine, "eTeXversion",
                                   HSTEX_COMMAND_CHAR_GIVEN, 2, error,
                                   error_capacity) != 0) {
        hstex_engine_destroy(engine);
        return -1;
    }
    uint32_t null_font = 0U;
    if (find_or_create_font(engine, "nullfont",
                            INT32_C(10) * INT32_C(65536), &null_font, error,
                            error_capacity) != 0 ||
        register_integer_primitive(engine, "nullfont", HSTEX_COMMAND_FONT_GIVEN,
                                   (int32_t)null_font, error,
                                   error_capacity) != 0) {
        hstex_engine_destroy(engine);
        return -1;
    }
    struct hstex_font *null_font_entry = font_by_identifier(engine, null_font);
    hstex_cs_id null_font_cs = 0U;
    if (null_font_entry == NULL ||
        hstex_symbol_find(&engine->lexical_state.symbols, HSTEX_SYMBOL_REGULAR,
                          (const uint8_t *)"nullfont", 8U, &null_font_cs) != 1) {
        (void)set_error(error, error_capacity, "nullfont registration failed");
        hstex_engine_destroy(engine);
        return -1;
    }
    null_font_entry->identifier_cs = null_font_cs;
    engine->current_font = null_font;
    /* Every math family starts as \nullfont, so \the\textfont7 names a font
       even before any family has been set up. */
    for (size_t size = 0U; size < (size_t)HSTEX_MATH_SIZE_COUNT; ++size) {
        for (size_t family = 0U; family < 16U; ++family) {
            engine->math_fonts[size][family] = null_font;
        }
    }
    engine->output_directory = strdup(".");
    if (engine->output_directory == NULL) {
        (void)set_error(error, error_capacity,
                        "output-directory allocation failed");
        hstex_engine_destroy(engine);
        return -1;
    }
    struct hstex_meaning paragraph = {
        .command = HSTEX_COMMAND_PAR,
        .level = 0U,
        .primitive_origin = engine->lexical_state.paragraph_control_sequence,
        .value = {.macro_identifier = 0U},
    };
    if (set_meaning(engine, engine->lexical_state.paragraph_control_sequence,
                    paragraph, true, error, error_capacity) != 0) {
        hstex_engine_destroy(engine);
        return -1;
    }
    return 0;
}

void hstex_engine_destroy(struct hstex_engine *engine)
{
    if (engine == NULL) {
        return;
    }
    hstex_source_stack_destroy(&engine->sources);
    for (size_t index = 0U; index < engine->math_depth; ++index) {
        free(engine->math_stack[index].noads);
    }
    free(engine->math_items);
    engine->math_items = NULL;
    engine->math_item_count = 0U;
    engine->math_item_capacity = 0U;
    free(engine->parshapes);
    engine->parshapes = NULL;
    engine->parshape_used = 0U;
    engine->parshape_capacity = 0U;
    free(engine->math_sublists);
    engine->math_sublists = NULL;
    engine->math_sublist_count = 0U;
    engine->math_sublist_capacity = 0U;
    free(engine->math_stack);
    engine->math_stack = NULL;
    engine->math_depth = 0U;
    engine->math_capacity = 0U;
    if (engine->display_rows != NULL) {
        free(engine->display_rows->node_identifiers);
        free(engine->display_rows);
        engine->display_rows = NULL;
    }
    for (size_t index = 0U; index < 16U; ++index) {
        if (engine->write_streams[index] != NULL) {
            (void)fclose(engine->write_streams[index]);
        }
        if (engine->read_streams[index] != NULL) {
            (void)fclose(engine->read_streams[index]);
        }
    }
    for (size_t index = 0U; index < engine->macro_count; ++index) {
        free(engine->macros[index].parameter_text);
        free(engine->macros[index].replacement);
    }
    for (size_t index = 0U; index < engine->token_list_count; ++index) {
        free(engine->token_lists[index].tokens);
    }
    for (size_t index = 0U; index < engine->font_count; ++index) {
        free(engine->fonts[index].name);
        free(engine->fonts[index].dimens);
        free(engine->fonts[index].characters);
        free(engine->fonts[index].lig_kern);
        free(engine->fonts[index].kerns);
        free(engine->fonts[index].extensibles);
    }
    free(engine->meanings);
    free(engine->macros);
    free(engine->saves);
    free(engine->conditionals);
    free(engine->counts);
    free(engine->count_levels);
    free(engine->dimens);
    free(engine->dimen_levels);
    free(engine->glues);
    free(engine->glue_levels);
    free(engine->muglues);
    free(engine->muglue_levels);
    free(engine->token_registers);
    free(engine->token_register_levels);
    free(engine->boxes);
    free(engine->box_levels);
    free(engine->nodes);
    free(engine->list_items);
    free(engine->token_lists);
    free(engine->fonts);
    free(engine->hyphen_roots);
    free(engine->hyphen_nodes);
    free(engine->hyphen_values);
    free(engine->hyphen_exceptions);
    free(engine->hyphen_exception_data);
    if (engine->page_builder != NULL) {
        free(engine->page_builder->node_identifiers);
    }
    free(engine->page_builder);
    clear_match_groups(engine);
    for (size_t index = 0U; index < engine->pdf_object_count; ++index) {
        free(engine->pdf_objects[index].attributes);
        free(engine->pdf_objects[index].content);
    }
    free(engine->pdf_objects);
    for (size_t index = 0U; index < engine->pdf_literal_count; ++index) {
        free(engine->pdf_literals[index].content);
    }
    free(engine->pdf_literals);
    for (size_t index = 0U; index < engine->pdf_record_count; ++index) {
        free(engine->pdf_records[index].name);
        free(engine->pdf_records[index].content);
    }
    free(engine->pdf_records);
    for (size_t index = 0U; index < engine->color_stack_count; ++index) {
        struct hstex_color_stack *stack = &engine->color_stacks[index];
        for (size_t depth = 0U; depth < stack->count; ++depth) {
            free(stack->values[depth]);
        }
        free(stack->values);
        free(stack->initial);
    }
    free(engine->color_stacks);
    free(engine->pdf_catalog);
    free(engine->pdf_info);
    for (size_t index = 0U; index < engine->glyph_unicode_count; ++index) {
        free(engine->glyph_unicode[index].glyph);
        free(engine->glyph_unicode[index].unicode);
    }
    free(engine->glyph_unicode);
    free(engine->output_directory);
    free(engine->job_name);
    hstex_lexical_state_destroy(&engine->lexical_state);
    memset(engine, 0, sizeof(*engine));
}

static char *job_name_from_path(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    size_t length = strlen(base);
    size_t stem_length = length;
    for (size_t index = length; index > 1U; --index) {
        if (base[index - 1U] == '.') {
            stem_length = index - 1U;
            break;
        }
    }
    char *name = malloc(stem_length + 1U);
    if (name == NULL) {
        return NULL;
    }
    memcpy(name, base, stem_length);
    name[stem_length] = '\0';
    return name;
}

int hstex_engine_push_file(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity)
{
    if (engine == NULL || path == NULL) {
        return set_error(error, error_capacity, "null engine file input");
    }
    char *job_name = NULL;
    if (engine->job_name == NULL) {
        job_name = job_name_from_path(path);
        if (job_name == NULL) {
            return set_error(error, error_capacity,
                             "job-name allocation failed");
        }
    }
    int status = hstex_source_push_file(&engine->sources, path, error,
                                        error_capacity);
    if (status != 0) {
        free(job_name);
        return -1;
    }
    if (job_name != NULL) {
        engine->job_name = job_name;
    }
    return 0;
}

int hstex_engine_begin_job(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity)
{
    if (engine == NULL || path == NULL || !engine->dump_requested ||
        engine->group_level != 0U || engine->conditional_count != 0U) {
        return set_error(error, error_capacity,
                         "invalid document-job transition");
    }
    hstex_source_stack_destroy(&engine->sources);
    hstex_source_stack_init(&engine->sources, &engine->lexical_state);
    for (size_t index = 0U; index < 16U; ++index) {
        if (engine->write_streams[index] != NULL) {
            (void)fclose(engine->write_streams[index]);
            engine->write_streams[index] = NULL;
        }
        if (engine->read_streams[index] != NULL) {
            (void)fclose(engine->read_streams[index]);
            engine->read_streams[index] = NULL;
        }
    }
    free(engine->job_name);
    engine->job_name = NULL;
    engine->mode = HSTEX_MODE_VERTICAL;
    engine->prev_depth = -INT32_C(1000) * INT32_C(65536);
    engine->inner_mode = false;
    engine->interaction_mode = HSTEX_INTERACTION_ERROR_STOP;
    engine->after_assignment_token = 0U;
    memset(&engine->after_assignment_location, 0,
           sizeof(engine->after_assignment_location));
    engine->has_after_assignment = false;
    engine->pending_macro_flags = 0U;
    engine->pending_global = false;
    engine->returned_unexpanded = false;
    engine->returned_unexpanded_executable = false;
    engine->inhibit_protected_expansion = false;
    engine->negate_next_conditional = false;
    engine->dump_requested = false;
    engine->output_group_floor = 0U;
    engine->output_conditional_floor = 0U;
    engine->active_hbox_builder = NULL;
    engine->page_builder->count = 0U;
    engine->page_builder->extent = 0;
    engine->page_builder->trailing_depth = 0;
    engine->page_builder->width = 0;
    engine->active_vbox_builder = engine->page_builder;
    if (hstex_engine_push_file(engine, path, error, error_capacity) != 0) {
        return -1;
    }
    uint32_t every_job = engine->token_parameters[HSTEX_TOKEN_EVERY_JOB];
    if (every_job != 0U) {
        const struct hstex_token_list *list =
            token_list_by_identifier(engine, every_job);
        struct hstex_source_location location = {0};
        if (list == NULL ||
            hstex_source_push_tokens(&engine->sources, list->tokens,
                                     list->count, location, error,
                                     error_capacity) != 0) {
            return set_error(error, error_capacity,
                             "could not install everyjob tokens");
        }
    }
    return 0;
}

int hstex_engine_set_output_directory(struct hstex_engine *engine,
                                      const char *path, char *error,
                                      size_t error_capacity)
{
    if (engine == NULL || path == NULL || path[0] == '\0') {
        return set_error(error, error_capacity,
                         "invalid output-directory request");
    }
    char *copy = strdup(path);
    if (copy == NULL) {
        return set_error(error, error_capacity,
                         "output-directory allocation failed");
    }
    free(engine->output_directory);
    engine->output_directory = copy;
    return 0;
}

/* A file that runs out inserts \everyeof, once, before whatever follows it;
   see docs/DECISIONS.md, everyeof. */
static enum hstex_engine_result raw_next(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity)
{
    for (;;) {
        size_t ended_before = engine->sources.file_end_count;
        enum hstex_mouth_result result = hstex_source_next(
            &engine->sources, token, location, error, error_capacity);
        if (result == HSTEX_MOUTH_ERROR) {
            return HSTEX_ENGINE_ERROR;
        }
        if (engine->sources.file_end_count == ended_before ||
            engine->token_parameters[HSTEX_TOKEN_EVERY_EOF] == 0U) {
            return result == HSTEX_MOUTH_EOF ? HSTEX_ENGINE_EOF
                                             : HSTEX_ENGINE_TOKEN;
        }
        /* The token just read belongs after the inserted list, so it goes
           back first. */
        if (result != HSTEX_MOUTH_EOF &&
            push_one(engine, *token, *location, error, error_capacity) != 0) {
            return HSTEX_ENGINE_ERROR;
        }
        const struct hstex_token_list *list = token_list_by_identifier(
            engine, engine->token_parameters[HSTEX_TOKEN_EVERY_EOF]);
        if (list == NULL ||
            hstex_source_push_tokens(&engine->sources, list->tokens,
                                     list->count, *location, error,
                                     error_capacity) != 0) {
            (void)set_error(error, error_capacity,
                            "could not install everyeof tokens");
            return HSTEX_ENGINE_ERROR;
        }
    }
}

static enum hstex_engine_result raw_next_non_space(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity)
{
    for (;;) {
        enum hstex_engine_result result = raw_next(
            engine, token, location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN || !token_is_space(*token)) {
            return result;
        }
    }
}

static int push_owned_vector(struct hstex_engine *engine,
                             struct token_vector *vector,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    hstex_token *data = vector->data;
    size_t count = vector->count;
    memset(vector, 0, sizeof(*vector));
    return hstex_source_push_owned_tokens(&engine->sources, data, count, location,
                                          error, error_capacity);
}

static int push_one(struct hstex_engine *engine, hstex_token token,
                    struct hstex_source_location location, char *error,
                    size_t error_capacity)
{
    hstex_token *allocation = malloc(sizeof(*allocation));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "single-token input allocation failed");
    }
    allocation[0] = token;
    return hstex_source_push_owned_tokens(&engine->sources, allocation, 1U,
                                          location, error, error_capacity);
}

static int finish_assignment(struct hstex_engine *engine, int status,
                             char *error, size_t error_capacity)
{
    if (status != 0) {
        return -1;
    }
    if (!engine->has_after_assignment) {
        return 0;
    }
    hstex_token token = engine->after_assignment_token;
    struct hstex_source_location location =
        engine->after_assignment_location;
    engine->has_after_assignment = false;
    return push_one(engine, token, location, error, error_capacity);
}

static int scan_after_assignment(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (raw_next(engine, &token, &location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input after afterassignment");
    }
    engine->after_assignment_token = token;
    engine->after_assignment_location = location;
    engine->has_after_assignment = true;
    return 0;
}

static int scan_after_group(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    if (engine->group_level == 0U || engine->pending_global ||
        engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "aftergroup requires an unprefixed active group");
    }
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (raw_next(engine, &token, &location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input after aftergroup");
    }
    if (reserve_saves(engine, engine->save_count + 1U, error,
                      error_capacity) != 0) {
        return -1;
    }
    struct hstex_save_entry *save = &engine->saves[engine->save_count++];
    memset(save, 0, sizeof(*save));
    save->kind = HSTEX_SAVE_AFTER_GROUP;
    save->level = engine->group_level;
    save->previous.after_group.token = token;
    save->previous.after_group.location = location;
    return 0;
}

static enum hstex_engine_result expanded_next_non_space(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity);
static enum hstex_engine_result expanded_next_non_space_unrestricted(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity);

static int append_byte(uint8_t **bytes, size_t *count, size_t *capacity,
                       uint8_t byte, char *error, size_t error_capacity)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0U ? 64U : *capacity;
        if (new_capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "byte-vector capacity overflow");
        }
        new_capacity *= 2U;
        void *allocation = realloc(*bytes, new_capacity);
        if (allocation == NULL) {
            return set_error(error, error_capacity,
                             "byte-vector allocation failed");
        }
        *bytes = allocation;
        *capacity = new_capacity;
    }
    (*bytes)[(*count)++] = byte;
    return 0;
}

static int scan_input_filename(struct hstex_engine *engine, char **filename,
                               char *error, size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &token, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning a filename");
    }
    bool braced = token_is_category(token, HSTEX_CAT_BEGIN_GROUP);
    bool quoted = false;
    size_t depth = braced ? 1U : 0U;
    uint8_t *bytes = NULL;
    size_t count = 0U;
    size_t capacity = 0U;

    if (braced) {
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input in braced filename");
        }
    }
    for (;;) {
        if (braced && token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (braced && token_is_category(token, HSTEX_CAT_END_GROUP)) {
            --depth;
            if (depth == 0U) {
                break;
            }
        } else if (hstex_token_is_character(token) &&
                   hstex_token_character_code(token) == (uint8_t)'"') {
            quoted = !quoted;
        } else if (!braced && !quoted && token_is_space(token)) {
            break;
        } else if (hstex_token_is_character(token)) {
            if (append_byte(&bytes, &count, &capacity,
                            hstex_token_character_code(token), error,
                            error_capacity) != 0) {
                free(bytes);
                return -1;
            }
        } else {
            if (!braced &&
                push_one(engine, token, location, error, error_capacity) == 0) {
                break;
            }
            free(bytes);
            return set_error(error, error_capacity,
                             "non-character token in filename");
        }

        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            if (braced) {
                free(bytes);
                return set_error(error, error_capacity,
                                 "end of input in braced filename");
            }
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            free(bytes);
            return -1;
        }
    }
    if (quoted) {
        free(bytes);
        return set_error(error, error_capacity,
                         "unterminated quote in filename");
    }
    if (count == 0U || append_byte(&bytes, &count, &capacity, 0U, error,
                                   error_capacity) != 0) {
        free(bytes);
        return set_error(error, error_capacity, "empty input filename");
    }
    *filename = (char *)bytes;
    return 0;
}

static char *join_directory_filename(const char *source_path,
                                     const char *filename)
{
    const char *slash = strrchr(source_path, '/');
    if (slash == NULL) {
        return NULL;
    }
    size_t directory_length = (size_t)(slash - source_path) + 1U;
    size_t filename_length = strlen(filename);
    if (directory_length > SIZE_MAX - filename_length - 1U) {
        return NULL;
    }
    char *candidate = malloc(directory_length + filename_length + 1U);
    if (candidate == NULL) {
        return NULL;
    }
    memcpy(candidate, source_path, directory_length);
    memcpy(candidate + directory_length, filename, filename_length + 1U);
    return candidate;
}

static char *join_path(const char *directory, const char *filename)
{
    size_t directory_length = strlen(directory);
    size_t filename_length = strlen(filename);
    bool needs_separator = directory_length != 0U &&
                           directory[directory_length - 1U] != '/';
    size_t separator_length = needs_separator ? 1U : 0U;
    if (directory_length >
        SIZE_MAX - separator_length - filename_length - 1U) {
        return NULL;
    }
    char *path = malloc(directory_length + separator_length + filename_length +
                        1U);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, directory, directory_length);
    if (needs_separator) {
        path[directory_length] = '/';
    }
    memcpy(path + directory_length + separator_length, filename,
           filename_length + 1U);
    return path;
}

static char *resolve_with_kpsewhich(const char *filename)
{
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        return NULL;
    }
    pid_t child = fork();
    if (child < 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return NULL;
    }
    if (child == 0) {
        (void)close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        (void)close(descriptors[1]);
        execlp("kpsewhich", "kpsewhich", filename, (char *)NULL);
        _exit(127);
    }
    (void)close(descriptors[1]);
    uint8_t *bytes = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    uint8_t buffer[512];
    for (;;) {
        ssize_t received = read(descriptors[0], buffer, sizeof(buffer));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            break;
        }
        for (ssize_t index = 0; index < received; ++index) {
            if (append_byte(&bytes, &count, &capacity, buffer[index], NULL,
                            0U) != 0) {
                free(bytes);
                bytes = NULL;
                count = 0U;
                break;
            }
        }
        if (bytes == NULL && count == 0U) {
            break;
        }
    }
    (void)close(descriptors[0]);
    int child_status = 0;
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        free(bytes);
        return NULL;
    }
    while (count != 0U &&
           (bytes[count - 1U] == (uint8_t)'\n' ||
            bytes[count - 1U] == (uint8_t)'\r')) {
        --count;
    }
    if (count == 0U || append_byte(&bytes, &count, &capacity, 0U, NULL, 0U) !=
                           0) {
        free(bytes);
        return NULL;
    }
    return (char *)bytes;
}

static bool filename_has_extension(const char *filename)
{
    const char *slash = strrchr(filename, '/');
    const char *dot = strrchr(filename, '.');
    return dot != NULL && (slash == NULL || dot > slash);
}

static char *append_tex_extension(const char *filename)
{
    size_t length = strlen(filename);
    if (length > SIZE_MAX - 5U) {
        return NULL;
    }
    char *extended = malloc(length + 5U);
    if (extended == NULL) {
        return NULL;
    }
    memcpy(extended, filename, length);
    memcpy(extended + length, ".tex", 5U);
    return extended;
}

static char *resolve_input_path(struct hstex_engine *engine,
                                const char *filename)
{
    const char *current_source = hstex_source_current_name(&engine->sources);
    const char *variants[2] = {filename, NULL};
    char *extended = NULL;
    if (!filename_has_extension(filename)) {
        extended = append_tex_extension(filename);
        variants[1] = extended;
    }
    for (size_t index = 0U; index < 2U && variants[index] != NULL; ++index) {
        const char *variant = variants[index];
        if (engine->output_directory != NULL && filename[0] != '/') {
            char *candidate = join_path(engine->output_directory, variant);
            if (candidate != NULL && access(candidate, R_OK) == 0) {
                free(extended);
                return candidate;
            }
            free(candidate);
        }
        if (access(variant, R_OK) == 0) {
            char *resolved = strdup(variant);
            free(extended);
            return resolved;
        }
        if (current_source != NULL && filename[0] != '/') {
            char *candidate = join_directory_filename(current_source, variant);
            if (candidate != NULL && access(candidate, R_OK) == 0) {
                free(extended);
                return candidate;
            }
            free(candidate);
        }
        char *resolved = resolve_with_kpsewhich(variant);
        if (resolved != NULL) {
            free(extended);
            return resolved;
        }
    }
    free(extended);
    return NULL;
}

int hstex_engine_push_input(struct hstex_engine *engine, const char *name,
                            char *error, size_t error_capacity)
{
    if (engine == NULL || name == NULL) {
        return set_error(error, error_capacity, "null engine input name");
    }
    char *path = resolve_input_path(engine, name);
    if (path == NULL) {
        return set_error(error, error_capacity, "input file not found: %s",
                         name);
    }
    int status = hstex_engine_push_file(engine, path, error, error_capacity);
    free(path);
    return status;
}

static int execute_input(struct hstex_engine *engine, char *error,
                         size_t error_capacity)
{
    char *filename = NULL;
    if (scan_input_filename(engine, &filename, error, error_capacity) != 0) {
        return -1;
    }
    char *path = resolve_input_path(engine, filename);
    if (path == NULL) {
        int status = set_error(error, error_capacity, "input file not found: %s",
                               filename);
        free(filename);
        return status;
    }
    int status = hstex_source_push_file(&engine->sources, path, error,
                                        error_capacity);
    free(path);
    free(filename);
    return status;
}

static enum hstex_engine_result expanded_next_non_space(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity)
{
    for (;;) {
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, token, location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN || !token_is_space(*token)) {
            return result;
        }
    }
}

static enum hstex_engine_result expanded_next_non_space_unrestricted(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    enum hstex_engine_result result = expanded_next_non_space(
        engine, token, location, error, error_capacity);
    engine->inhibit_protected_expansion = previous_inhibition;
    return result;
}

static int token_character_constant(struct hstex_engine *engine,
                                    hstex_token token, int32_t *value)
{
    if (hstex_token_is_character(token)) {
        *value = (int32_t)hstex_token_character_code(token);
        return 0;
    }
    if (!hstex_token_is_control_sequence(token)) {
        return -1;
    }
    enum hstex_symbol_kind kind;
    const uint8_t *name = NULL;
    size_t length = 0U;
    if (hstex_symbol_name(&engine->lexical_state.symbols,
                          hstex_token_control_sequence_id(token), &kind, &name,
                          &length) != 0 ||
        length != 1U) {
        return -1;
    }
    (void)kind;
    *value = (int32_t)name[0];
    return 0;
}

static int scan_integer(struct hstex_engine *engine, int32_t *value,
                        char *error, size_t error_capacity);
static int scan_integer_impl(struct hstex_engine *engine, int32_t *value,
                             char *error, size_t error_capacity);
static int scan_num_expression(struct hstex_engine *engine, int32_t *value,
                               char *error, size_t error_capacity);
static int scan_dim_expression(struct hstex_engine *engine, int32_t *value,
                               char *error, size_t error_capacity);
static int dimen_from_meaning(struct hstex_engine *engine,
                              const struct hstex_meaning *meaning,
                              int32_t *value, char *error,
                              size_t error_capacity);
static int scan_glue_expression(struct hstex_engine *engine,
                                struct hstex_glue *value, char *error,
                                size_t error_capacity);
static int scan_math_glue_expression(struct hstex_engine *engine,
                                     struct hstex_glue *value, char *error,
                                     size_t error_capacity);

static int scan_font_identifier(struct hstex_engine *engine,
                                uint32_t *identifier, char *error,
                                size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &token, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(token)) {
        return set_error(error, error_capacity,
                         "font identifier requires a defined font");
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_FONT) {
        if (engine->current_font == 0U ||
            font_by_identifier(engine, engine->current_font) == NULL) {
            return set_error(error, error_capacity,
                             "current font is not defined");
        }
        *identifier = engine->current_font;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_MATH_FONT) {
        int32_t size = meaning->value.integer;
        int32_t family = 0;
        if (scan_integer(engine, &family, error, error_capacity) != 0) {
            return -1;
        }
        if (size < 0 || size >= (int32_t)HSTEX_MATH_SIZE_COUNT || family < 0 ||
            family > 15) {
            return set_error(error, error_capacity,
                             "math family %d is outside 0..15", family);
        }
        uint32_t assigned = engine->math_fonts[size][family];
        if (font_by_identifier(engine, assigned) == NULL) {
            return set_error(error, error_capacity,
                             "math family %d has no font in this size",
                             family);
        }
        *identifier = assigned;
        return 0;
    }
    if (meaning->command != HSTEX_COMMAND_FONT_GIVEN ||
        meaning->value.integer <= 0 ||
        font_by_identifier(engine, (uint32_t)meaning->value.integer) == NULL) {
        return set_error(error, error_capacity,
                         "font identifier requires a defined font");
    }
    *identifier = (uint32_t)meaning->value.integer;
    return 0;
}

static int scan_font_dimen_reference(struct hstex_engine *engine,
                                     bool allow_extension,
                                     struct hstex_font **font,
                                     size_t *dimen_index, char *error,
                                     size_t error_capacity)
{
    int32_t parameter = 0;
    uint32_t identifier = 0U;
    if (scan_integer(engine, &parameter, error, error_capacity) != 0 ||
        parameter <= 0 ||
        scan_font_identifier(engine, &identifier, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid fontdimen reference");
    }
    struct hstex_font *selected = font_by_identifier(engine, identifier);
    size_t index = (size_t)parameter - 1U;
    if (selected == NULL ||
        (!allow_extension && index >= selected->dimen_count)) {
        return set_error(error, error_capacity,
                         "fontdimen index is not defined");
    }
    if (allow_extension &&
        reserve_font_dimens(selected, index + 1U, error, error_capacity) != 0) {
        return -1;
    }
    *font = selected;
    *dimen_index = index;
    return 0;
}

static int integer_from_control_sequence(
    struct hstex_engine *engine, const struct hstex_meaning *meaning,
    int32_t *value, char *error, size_t error_capacity)
{
    switch (meaning->command) {
    case HSTEX_COMMAND_PAR_SHAPE:
        *value = engine->parshape == 0U
                     ? 0
                     : engine->parshapes[engine->parshape - 1U];
        return 0;
    case HSTEX_COMMAND_INPUT_LINE_NUMBER:
        for (size_t index = engine->sources.count; index > 0U; --index) {
            const struct hstex_source_frame *frame =
                &engine->sources.frames[index - 1U];
            if (frame->kind == HSTEX_SOURCE_FILE) {
                uint32_t line = frame->value.file.mouth.line_number;
                *value = line > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)line;
                return 0;
            }
        }
        *value = 0;
        return 0;
    case HSTEX_COMMAND_INTEGER_CONSTANT:
        *value = meaning->value.integer;
        return 0;
    case HSTEX_COMMAND_CHAR_GIVEN:
    case HSTEX_COMMAND_MATH_CHAR_GIVEN:
        *value = meaning->value.integer;
        return 0;
    case HSTEX_COMMAND_COUNT_REGISTER: {
        int32_t index = meaning->value.integer;
        if (index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid count-register meaning");
        }
        *value = engine->counts[(size_t)index];
        return 0;
    }
    case HSTEX_COMMAND_INTEGER_PARAMETER: {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_INTEGER_PARAMETER_COUNT) {
            return set_error(error, error_capacity,
                             "invalid integer-parameter meaning");
        }
        *value = engine->integer_parameters[(size_t)index];
        return 0;
    }
    case HSTEX_COMMAND_PAGE_INTEGER: {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_PAGE_INTEGER_COUNT) {
            return set_error(error, error_capacity,
                             "invalid page-integer meaning");
        }
        *value = engine->page_integers[(size_t)index];
        return 0;
    }
    case HSTEX_COMMAND_COUNT: {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "count register outside supported range");
        }
        *value = engine->counts[(size_t)index];
        return 0;
    }
    case HSTEX_COMMAND_NUM_EXPR:
        return scan_num_expression(engine, value, error, error_capacity);
    case HSTEX_COMMAND_ENGINE_STATE_INTEGER:
        switch (meaning->value.integer) {
        case 0:
            *value = engine->group_level > (uint32_t)INT32_MAX
                         ? INT32_MAX
                         : (int32_t)engine->group_level;
            return 0;
        case 1:
            *value = engine->group_level == 0U ? 0 : 1;
            return 0;
        case 2:
            *value = engine->conditional_count > (size_t)INT32_MAX
                         ? INT32_MAX
                         : (int32_t)engine->conditional_count;
            return 0;
        case 3:
            *value = engine->conditional_count == 0U ? 0 : 1;
            return 0;
        case 5:
            *value = engine->badness;
            return 0;
        case 4:
            *value = engine->conditional_count == 0U
                         ? 0
                         : (engine->conditionals[engine->conditional_count - 1U]
                                    .branch_true
                                ? 1
                                : 0);
            return 0;
        default:
            return set_error(error, error_capacity,
                             "invalid engine-state integer subtype");
        }
    case HSTEX_COMMAND_SPACE_FACTOR:
        if (engine->mode != HSTEX_MODE_HORIZONTAL) {
            return set_error(error, error_capacity,
                             "spacefactor is only available in horizontal "
                             "mode");
        }
        *value = engine->space_factor;
        return 0;
    case HSTEX_COMMAND_LAST_ITEM: {
        const struct hstex_node *node = current_list_last_node(engine);
        if (meaning->value.integer == (int32_t)HSTEX_LAST_NODE_TYPE) {
            *value = last_node_type(node);
            return 0;
        }
        if (meaning->value.integer == (int32_t)HSTEX_LAST_PENALTY) {
            *value = node != NULL && node->kind == HSTEX_NODE_PENALTY
                         ? node->value.penalty
                         : 0;
            return 0;
        }
        /* \lastkern and \lastskip are dimensions; reaching them here means
           the caller wanted an integer, so the dimension is coerced. */
        int coerced = dimen_from_meaning(engine, meaning, value, error,
                                         error_capacity);
        if (coerced < 0) {
            return -1;
        }
        if (coerced == 0) {
            return set_error(error, error_capacity,
                             "last-item query did not provide a value");
        }
        return 0;
    }
    case HSTEX_COMMAND_PDF_LAST_NUMBER:
        if (meaning->value.integer < 0 ||
            meaning->value.integer >=
                (int32_t)(sizeof(engine->pdf_last) /
                          sizeof(engine->pdf_last[0]))) {
            return set_error(error, error_capacity,
                             "invalid pdf counter subtype");
        }
        *value = engine->pdf_last[meaning->value.integer];
        return 0;
    case HSTEX_COMMAND_FONT_CHAR_CODE: {
        uint32_t identifier = 0U;
        int32_t code = 0;
        if (scan_font_identifier(engine, &identifier, error, error_capacity) !=
                0 ||
            scan_integer(engine, &code, error, error_capacity) != 0) {
            return -1;
        }
        const struct hstex_font *font = font_by_identifier(engine, identifier);
        if (font == NULL || font->characters == NULL) {
            return set_error(error, error_capacity,
                             "font code requires a defined font");
        }
        if (code < 0 || (size_t)code >= HSTEX_FONT_CHARACTER_COUNT) {
            return set_error(error, error_capacity, "bad character code (%d)",
                             code);
        }
        const struct hstex_char_metric *metric =
            &font->characters[(size_t)code];
        switch ((enum hstex_font_char_code)meaning->value.integer) {
        case HSTEX_FONT_CODE_LEFT_PROTRUSION:
            *value = metric->left_protrusion;
            return 0;
        case HSTEX_FONT_CODE_RIGHT_PROTRUSION:
            *value = metric->right_protrusion;
            return 0;
        case HSTEX_FONT_CODE_EXPANSION:
            *value = metric->expansion_factor;
            return 0;
        case HSTEX_FONT_CODE_TAG:
            *value = metric->tag;
            return 0;
        }
        return set_error(error, error_capacity, "invalid font code subtype");
    }
    case HSTEX_COMMAND_DIMEN_REGISTER:
    case HSTEX_COMMAND_DIMEN_PARAMETER:
    case HSTEX_COMMAND_DIMEN:
    case HSTEX_COMMAND_DIM_EXPR:
    case HSTEX_COMMAND_FONT_DIMEN:
    case HSTEX_COMMAND_FONT_CHAR_DIMEN:
    case HSTEX_COMMAND_BOX_DIMEN:
    case HSTEX_COMMAND_MARGIN_KERN:
    case HSTEX_COMMAND_PAGE_DIMEN:
    case HSTEX_COMMAND_PREV_DEPTH:
    case HSTEX_COMMAND_SKIP_REGISTER:
    case HSTEX_COMMAND_GLUE_PARAMETER:
    case HSTEX_COMMAND_SKIP:
    case HSTEX_COMMAND_GLUE_EXPR: {
        int result = dimen_from_meaning(engine, meaning, value, error,
                                        error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            return set_error(error, error_capacity,
                             "internal dimension did not provide a value");
        }
        return 0;
    }
    case HSTEX_COMMAND_HYPHEN_CHAR:
    case HSTEX_COMMAND_SKEW_CHAR: {
        uint32_t identifier = 0U;
        if (scan_font_identifier(engine, &identifier, error,
                                 error_capacity) != 0) {
            return -1;
        }
        struct hstex_font *font = font_by_identifier(engine, identifier);
        *value = meaning->command == HSTEX_COMMAND_HYPHEN_CHAR
                     ? font->hyphen_character
                     : font->skew_character;
        return 0;
    }
    case HSTEX_COMMAND_CAT_CODE: {
        int32_t character = 0;
        if (scan_integer(engine, &character, error, error_capacity) != 0 ||
            character < 0 || character > 255) {
            return set_error(error, error_capacity,
                             "catcode query outside 0..255");
        }
        *value = (int32_t)hstex_catcode_get(
            &engine->lexical_state.catcodes, (uint8_t)character);
        return 0;
    }
    case HSTEX_COMMAND_SF_CODE:
    case HSTEX_COMMAND_LC_CODE:
    case HSTEX_COMMAND_UC_CODE:
    case HSTEX_COMMAND_MATH_CODE:
    case HSTEX_COMMAND_DEL_CODE: {
        int32_t character = 0;
        int table = code_table_index(meaning->command);
        if (table < 0 ||
            scan_integer(engine, &character, error, error_capacity) != 0 ||
            character < 0 || character > 255) {
            return set_error(error, error_capacity,
                             "code-table query outside 0..255");
        }
        *value = engine->code_tables[(size_t)table][(size_t)character];
        return 0;
    }
    case HSTEX_COMMAND_TOKEN_ALIAS:
        if (hstex_token_is_character(meaning->value.token)) {
            *value = (int32_t)hstex_token_character_code(meaning->value.token);
            return 0;
        }
        break;
    default:
        break;
    }
    return set_error(error, error_capacity, "missing integer");
}

static bool token_is_decimal_digit(hstex_token token)
{
    return token_is_category(token, HSTEX_CAT_OTHER) &&
           hstex_token_character_code(token) >= (uint8_t)'0' &&
           hstex_token_character_code(token) <= (uint8_t)'9';
}

static int token_digit_value(hstex_token token, unsigned int radix)
{
    if (!hstex_token_is_character(token)) {
        return -1;
    }
    uint8_t character = hstex_token_character_code(token);
    unsigned int value;
    if (character >= (uint8_t)'0' && character <= (uint8_t)'9') {
        value = (unsigned int)(character - (uint8_t)'0');
    } else if (character >= (uint8_t)'A' && character <= (uint8_t)'F') {
        value = 10U + (unsigned int)(character - (uint8_t)'A');
    } else {
        return -1;
    }
    return value < radix ? (int)value : -1;
}

static int scan_integer(struct hstex_engine *engine, int32_t *value,
                        char *error, size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    int status = scan_integer_impl(engine, value, error, error_capacity);
    engine->inhibit_protected_expansion = previous_inhibition;
    return status;
}

static int scan_integer_impl(struct hstex_engine *engine, int32_t *value,
                             char *error, size_t error_capacity)
{
    int sign = 1;
    hstex_token token = 0U;
    struct hstex_source_location location;
    for (;;) {
        if (expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input while scanning an integer");
        }
        if (token_is_other_character(token, (uint8_t)'+')) {
            continue;
        }
        if (token_is_other_character(token, (uint8_t)'-')) {
            sign = -sign;
            continue;
        }
        break;
    }

    int32_t magnitude = 0;
    if (token_is_other_character(token, (uint8_t)'`')) {
        hstex_token character_token = 0U;
        struct hstex_source_location character_location;
        if (raw_next(engine, &character_token, &character_location, error,
                     error_capacity) != HSTEX_ENGINE_TOKEN ||
            token_character_constant(engine, character_token, &magnitude) != 0) {
            return set_error(error, error_capacity,
                             "invalid alphabetic character constant");
        }
        enum hstex_engine_result terminator = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        if (terminator == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        if (terminator == HSTEX_ENGINE_TOKEN &&
            !token_is_effective_space(engine, token) &&
            push_one(engine, token, location, error, error_capacity) != 0) {
            return -1;
        }
    } else if (token_is_other_character(token, (uint8_t)'\'') ||
               token_is_other_character(token, (uint8_t)'"')) {
        unsigned int radix =
            token_is_other_character(token, (uint8_t)'\'') ? 8U : 16U;
        uint64_t accumulated = 0U;
        bool saw_digit = false;
        for (;;) {
            enum hstex_engine_result result = hstex_engine_next_expanded(
                engine, &token, &location, error, error_capacity);
            if (result == HSTEX_ENGINE_EOF) {
                break;
            }
            if (result == HSTEX_ENGINE_ERROR) {
                return -1;
            }
            int digit = token_digit_value(token, radix);
            if (digit < 0) {
                if (!token_is_effective_space(engine, token) &&
                    push_one(engine, token, location, error,
                             error_capacity) != 0) {
                    return -1;
                }
                break;
            }
            saw_digit = true;
            accumulated = accumulated * radix + (unsigned int)digit;
            if (accumulated > (uint64_t)INT32_MAX + 1U) {
                return set_error(error, error_capacity,
                                 "integer constant overflow");
            }
        }
        if (!saw_digit ||
            (sign > 0 && accumulated > (uint64_t)INT32_MAX)) {
            char found[128];
            describe_token(engine, token, found, sizeof(found));
            uint32_t line = 0U;
            const char *origin = current_source_line(engine, &line);
            return set_error(error, error_capacity,
                             "invalid based integer constant, found %s for %s "
                             "at %s:%u",
                             found, engine->executing_name, origin,
                             (unsigned int)line);
        }
        *value = sign > 0 ? (int32_t)accumulated
                          : (int32_t)(-(int64_t)accumulated);
        return 0;
    } else if (token_is_decimal_digit(token)) {
        int64_t accumulated = 0;
        for (;;) {
            accumulated = accumulated * 10 +
                          (int64_t)(hstex_token_character_code(token) -
                                    (uint8_t)'0');
            if (accumulated > (int64_t)INT32_MAX + 1) {
                return set_error(error, error_capacity,
                                 "integer constant overflow");
            }
            enum hstex_engine_result result = hstex_engine_next_expanded(
                engine, &token, &location, error, error_capacity);
            if (result == HSTEX_ENGINE_EOF) {
                break;
            }
            if (result == HSTEX_ENGINE_ERROR) {
                return -1;
            }
            if (token_is_decimal_digit(token)) {
                continue;
            }
            if (!token_is_effective_space(engine, token) &&
                push_one(engine, token, location, error, error_capacity) != 0) {
                return -1;
            }
            break;
        }
        if ((sign > 0 && accumulated > INT32_MAX) ||
            (sign < 0 && accumulated > (int64_t)INT32_MAX + 1)) {
            return set_error(error, error_capacity, "integer constant overflow");
        }
        *value = sign > 0 ? (int32_t)accumulated : (int32_t)(-accumulated);
        return 0;
    } else if (hstex_token_is_control_sequence(token)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(token));
        if (integer_from_control_sequence(engine, meaning, &magnitude, error,
                                          error_capacity) != 0) {
            /* Name the control sequence: an integer scan that fails on a
               control sequence is usually a primitive HSTeX has not
               implemented, and the name is what identifies it. */
            enum hstex_symbol_kind kind;
            const uint8_t *name = NULL;
            size_t length = 0U;
            char reason[256];
            (void)snprintf(reason, sizeof(reason), "%s", error);
            if (hstex_symbol_name(&engine->lexical_state.symbols,
                                  hstex_token_control_sequence_id(token), &kind,
                                  &name, &length) == 0) {
                uint32_t line = 0U;
                const char *origin = current_source_line(engine, &line);
                return set_error(error, error_capacity,
                                 "%s scanning \\%.*s for %s, at %s:%u", reason,
                                 (int)length, (const char *)name,
                                 engine->executing_name, origin,
                                 (unsigned int)line);
            }
            return -1;
        }
    } else {
        return set_error(error, error_capacity, "missing integer");
    }

    int64_t signed_value = sign > 0 ? (int64_t)magnitude : -(int64_t)magnitude;
    if (signed_value < INT32_MIN || signed_value > INT32_MAX) {
        return set_error(error, error_capacity, "integer value overflow");
    }
    *value = (int32_t)signed_value;
    return 0;
}

static int checked_num_expression_value(int64_t value, int32_t *result,
                                        char *error, size_t error_capacity)
{
    if (value < INT32_MIN || value > INT32_MAX) {
        return set_error(error, error_capacity,
                         "integer expression overflow");
    }
    *result = (int32_t)value;
    return 0;
}

static int scan_num_expression_sum(struct hstex_engine *engine,
                                   int32_t *value, char *error,
                                   size_t error_capacity);

static int scan_num_expression_primary(struct hstex_engine *engine,
                                       int32_t *value, char *error,
                                       size_t error_capacity)
{
    int sign = 1;
    hstex_token token = 0U;
    struct hstex_source_location location;
    for (;;) {
        if (expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "missing integer-expression operand");
        }
        if (token_is_other_character(token, (uint8_t)'+')) {
            continue;
        }
        if (token_is_other_character(token, (uint8_t)'-')) {
            sign = -sign;
            continue;
        }
        break;
    }

    int32_t magnitude = 0;
    if (token_is_other_character(token, (uint8_t)'(')) {
        if (scan_num_expression_sum(engine, &magnitude, error,
                                    error_capacity) != 0 ||
            expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN ||
            !token_is_other_character(token, (uint8_t)')')) {
            return set_error(error, error_capacity,
                             "unbalanced integer-expression parentheses");
        }
    } else {
        if (push_one(engine, token, location, error, error_capacity) != 0 ||
            scan_integer(engine, &magnitude, error, error_capacity) != 0) {
            return -1;
        }
    }
    int64_t signed_value =
        sign < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
    return checked_num_expression_value(signed_value, value, error,
                                        error_capacity);
}

static int divide_num_expression(int32_t numerator, int32_t denominator,
                                 int32_t *result, char *error,
                                 size_t error_capacity)
{
    if (denominator == 0) {
        return set_error(error, error_capacity,
                         "division by zero in integer expression");
    }
    uint64_t magnitude_numerator = numerator < 0
                                       ? (uint64_t)(-(int64_t)numerator)
                                       : (uint64_t)numerator;
    uint64_t magnitude_denominator = denominator < 0
                                         ? (uint64_t)(-(int64_t)denominator)
                                         : (uint64_t)denominator;
    uint64_t rounded =
        (magnitude_numerator + magnitude_denominator / 2U) /
        magnitude_denominator;
    int64_t signed_result = (numerator < 0) != (denominator < 0)
                                ? -(int64_t)rounded
                                : (int64_t)rounded;
    return checked_num_expression_value(signed_result, result, error,
                                        error_capacity);
}

static int scan_num_expression_term(struct hstex_engine *engine,
                                    int32_t *value, char *error,
                                    size_t error_capacity)
{
    if (scan_num_expression_primary(engine, value, error, error_capacity) != 0) {
        return -1;
    }
    for (;;) {
        hstex_token operation = 0U;
        struct hstex_source_location operation_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &operation, &operation_location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result != HSTEX_ENGINE_TOKEN) {
            return -1;
        }
        bool multiply = token_is_other_character(operation, (uint8_t)'*');
        bool divide = token_is_other_character(operation, (uint8_t)'/');
        if (!multiply && !divide) {
            return push_one(engine, operation, operation_location, error,
                            error_capacity);
        }
        int32_t right = 0;
        if (scan_num_expression_primary(engine, &right, error,
                                        error_capacity) != 0) {
            return -1;
        }
        if (multiply) {
            if (checked_num_expression_value((int64_t)*value * (int64_t)right,
                                             value, error,
                                             error_capacity) != 0) {
                return -1;
            }
        } else if (divide_num_expression(*value, right, value, error,
                                         error_capacity) != 0) {
            return -1;
        }
    }
}

static int scan_num_expression_sum(struct hstex_engine *engine,
                                   int32_t *value, char *error,
                                   size_t error_capacity)
{
    if (scan_num_expression_term(engine, value, error, error_capacity) != 0) {
        return -1;
    }
    for (;;) {
        hstex_token operation = 0U;
        struct hstex_source_location operation_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &operation, &operation_location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result != HSTEX_ENGINE_TOKEN) {
            return -1;
        }
        bool add = token_is_other_character(operation, (uint8_t)'+');
        bool subtract = token_is_other_character(operation, (uint8_t)'-');
        if (!add && !subtract) {
            return push_one(engine, operation, operation_location, error,
                            error_capacity);
        }
        int32_t right = 0;
        if (scan_num_expression_term(engine, &right, error, error_capacity) !=
            0) {
            return -1;
        }
        int64_t combined = add ? (int64_t)*value + (int64_t)right
                               : (int64_t)*value - (int64_t)right;
        if (checked_num_expression_value(combined, value, error,
                                         error_capacity) != 0) {
            return -1;
        }
    }
}

static int scan_num_expression(struct hstex_engine *engine, int32_t *value,
                               char *error, size_t error_capacity)
{
    if (scan_num_expression_sum(engine, value, error, error_capacity) != 0) {
        return -1;
    }
    hstex_token terminator = 0U;
    struct hstex_source_location terminator_location;
    enum hstex_engine_result result = expanded_next_non_space(
        engine, &terminator, &terminator_location, error, error_capacity);
    if (result == HSTEX_ENGINE_EOF) {
        return 0;
    }
    if (result != HSTEX_ENGINE_TOKEN) {
        return -1;
    }
    if (hstex_token_is_control_sequence(terminator) &&
        hstex_engine_meaning(engine,
                             hstex_token_control_sequence_id(terminator))
                ->command == HSTEX_COMMAND_RELAX) {
        return 0;
    }
    return push_one(engine, terminator, terminator_location, error,
                    error_capacity);
}

static int scan_optional_equals(struct hstex_engine *engine, char *error,
                                size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result = expanded_next_non_space(
        engine, &token, &location, error, error_capacity);
    if (result != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning an assignment");
    }
    if (token_is_other_character(token, (uint8_t)'=')) {
        return 0;
    }
    return push_one(engine, token, location, error, error_capacity);
}

/* A decimal factor keeps the fraction as an exact rational so that a scanned
   dimension can be quantized to scaled points before any unit conversion. */
#define HSTEX_DECIMAL_FRACTION_DIGITS 17U
#define HSTEX_MAX_DIMEN INT32_C(1073741823)

struct decimal_factor {
    int sign;
    uint64_t whole;
    uint64_t fraction;
    uint64_t denominator;
};

static int glue_from_meaning(struct hstex_engine *engine,
                             const struct hstex_meaning *meaning,
                             struct hstex_glue *value, char *error,
                             size_t error_capacity);

static bool page_is_empty(const struct hstex_engine *engine);

static int dimen_from_meaning(struct hstex_engine *engine,
                              const struct hstex_meaning *meaning,
                              int32_t *value, char *error,
                              size_t error_capacity)
{
    if (meaning->command == HSTEX_COMMAND_PREV_DEPTH) {
        if (engine->mode != HSTEX_MODE_VERTICAL) {
            return set_error(error, error_capacity,
                             "prevdepth is only available in vertical mode");
        }
        *value = engine->prev_depth;
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_DIMEN_REGISTER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid dimen-register meaning");
        }
        *value = engine->dimens[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_DIMEN_PARAMETER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_DIMEN_PARAMETER_COUNT) {
            return set_error(error, error_capacity,
                             "invalid dimen-parameter meaning");
        }
        *value = engine->dimen_parameters[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_LAST_ITEM &&
        meaning->value.integer == (int32_t)HSTEX_LAST_KERN) {
        const struct hstex_node *node = current_list_last_node(engine);
        *value = node != NULL && node->kind == HSTEX_NODE_KERN ? node->width : 0;
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_FONT_CHAR_DIMEN) {
        uint32_t identifier = 0U;
        int32_t code = 0;
        if (scan_font_identifier(engine, &identifier, error, error_capacity) !=
                0 ||
            scan_integer(engine, &code, error, error_capacity) != 0) {
            return -1;
        }
        const struct hstex_font *font = font_by_identifier(engine, identifier);
        if (font == NULL) {
            return set_error(error, error_capacity,
                             "font character dimension requires a font");
        }
        /* A character outside the font measures zero, as does one the font
           leaves undefined. */
        if (code < 0 || (size_t)code >= HSTEX_FONT_CHARACTER_COUNT ||
            font->characters == NULL) {
            *value = 0;
            return 1;
        }
        const struct hstex_char_metric *metric =
            &font->characters[(size_t)code];
        *value = meaning->value.integer == (int32_t)HSTEX_FONT_CHAR_HEIGHT
                     ? metric->height
                     : meaning->value.integer == (int32_t)HSTEX_FONT_CHAR_DEPTH
                           ? metric->depth
                           : meaning->value.integer ==
                                     (int32_t)HSTEX_FONT_CHAR_ITALIC
                                 ? metric->italic
                                 : metric->width;
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_MARGIN_KERN) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0) {
            return -1;
        }
        if (index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "box register %d is outside 0..%zu", index,
                             engine->count_capacity - 1U);
        }
        /* Nothing protrudes, so nothing hangs in the margin. */
        *value = 0;
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_BOX_DIMEN) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
        const struct hstex_box *box = &engine->boxes[(size_t)index];
        /* A void box measures zero in every direction. */
        if (box->kind == HSTEX_BOX_VOID) {
            *value = 0;
            return 1;
        }
        *value = meaning->value.integer == (int32_t)HSTEX_BOX_DIMEN_HEIGHT
                     ? box->height
                     : meaning->value.integer == (int32_t)HSTEX_BOX_DIMEN_DEPTH
                           ? box->depth
                           : box->width;
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_PAGE_DIMEN) {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_PAGE_DIMEN_COUNT) {
            return set_error(error, error_capacity,
                             "invalid page-dimen meaning");
        }
        /* While the page is empty the page dimensions read as a fixed pair:
           the goal is \maxdimen and every other total is zero. Once a box has
           reached the page the totals come from the page builder, which does
           not exist yet; reporting the stored zeros would diverge silently. */
        if (!page_is_empty(engine)) {
            return set_error(error, error_capacity,
                             "page totals require the page builder");
        }
        *value = index == (int32_t)HSTEX_PAGE_GOAL ? HSTEX_MAX_DIMEN : 0;
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_DIMEN) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 || index < 0 ||
            (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "dimen register outside supported range");
        }
        *value = engine->dimens[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_DIM_EXPR) {
        if (scan_dim_expression(engine, value, error, error_capacity) != 0) {
            return -1;
        }
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_FONT_DIMEN) {
        struct hstex_font *font = NULL;
        size_t index = 0U;
        if (scan_font_dimen_reference(engine, false, &font, &index, error,
                                      error_capacity) != 0) {
            return -1;
        }
        *value = font->dimens[index];
        return 1;
    }
    struct hstex_glue glue;
    int glue_result =
        glue_from_meaning(engine, meaning, &glue, error, error_capacity);
    if (glue_result < 0) {
        return -1;
    }
    if (glue_result > 0) {
        *value = glue.width;
        return 1;
    }
    return 0;
}

static bool meaning_supplies_integer_factor(enum hstex_command command)
{
    switch (command) {
    case HSTEX_COMMAND_PAR_SHAPE:
    case HSTEX_COMMAND_INPUT_LINE_NUMBER:
    case HSTEX_COMMAND_INTEGER_CONSTANT:
    case HSTEX_COMMAND_CHAR_GIVEN:
    case HSTEX_COMMAND_MATH_CHAR_GIVEN:
    case HSTEX_COMMAND_COUNT_REGISTER:
    case HSTEX_COMMAND_INTEGER_PARAMETER:
    case HSTEX_COMMAND_COUNT:
    case HSTEX_COMMAND_NUM_EXPR:
    case HSTEX_COMMAND_ENGINE_STATE_INTEGER:
    case HSTEX_COMMAND_PAGE_INTEGER:
    case HSTEX_COMMAND_FONT_CHAR_DIMEN:
    case HSTEX_COMMAND_FONT_CHAR_CODE:
    case HSTEX_COMMAND_PDF_LAST_NUMBER:
    case HSTEX_COMMAND_LAST_ITEM:
    case HSTEX_COMMAND_SPACE_FACTOR:
    case HSTEX_COMMAND_BOX_DIMEN:
    case HSTEX_COMMAND_MARGIN_KERN:
    case HSTEX_COMMAND_CAT_CODE:
    case HSTEX_COMMAND_SF_CODE:
    case HSTEX_COMMAND_LC_CODE:
    case HSTEX_COMMAND_UC_CODE:
    case HSTEX_COMMAND_MATH_CODE:
    case HSTEX_COMMAND_DEL_CODE:
    case HSTEX_COMMAND_HYPHEN_CHAR:
    case HSTEX_COMMAND_SKEW_CHAR:
        return true;
    default:
        return false;
    }
}

static int scan_decimal_factor(struct hstex_engine *engine,
                               struct decimal_factor *factor,
                               bool *direct_dimen, int32_t *direct_value,
                               char *error, size_t error_capacity)
{
    memset(factor, 0, sizeof(*factor));
    factor->sign = 1;
    factor->denominator = 1U;
    *direct_dimen = false;
    hstex_token token = 0U;
    struct hstex_source_location location;
    for (;;) {
        if (expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input while scanning a dimension");
        }
        if (token_is_other_character(token, (uint8_t)'+')) {
            continue;
        }
        if (token_is_other_character(token, (uint8_t)'-')) {
            factor->sign = -factor->sign;
            continue;
        }
        break;
    }
    if (hstex_token_is_control_sequence(token)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(token));
        int result = dimen_from_meaning(
            engine, meaning, direct_value, error, error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            if (factor->sign < 0) {
                *direct_value = -*direct_value;
            }
            *direct_dimen = true;
            return 0;
        }
        if (meaning_supplies_integer_factor(meaning->command)) {
            int32_t integer = 0;
            if (integer_from_control_sequence(engine, meaning, &integer, error,
                                              error_capacity) != 0) {
                return -1;
            }
            if (integer < 0) {
                factor->sign = -factor->sign;
                factor->whole = (uint64_t)(-(int64_t)integer);
            } else {
                factor->whole = (uint64_t)integer;
            }
            return 0;
        }
    }

    bool saw_digit = false;
    bool saw_decimal = false;
    size_t fraction_digits = 0U;
    for (;;) {
        if (token_is_decimal_digit(token)) {
            saw_digit = true;
            uint8_t digit = (uint8_t)(hstex_token_character_code(token) -
                                      (uint8_t)'0');
            if (!saw_decimal) {
                if (factor->whole > (UINT64_MAX - digit) / 10U) {
                    return set_error(error, error_capacity,
                                     "dimension number overflow");
                }
                factor->whole = factor->whole * 10U + digit;
            } else if (fraction_digits < HSTEX_DECIMAL_FRACTION_DIGITS) {
                factor->fraction = factor->fraction * 10U + digit;
                factor->denominator *= 10U;
                ++fraction_digits;
            }
        } else if (!saw_decimal && hstex_token_is_character(token) &&
                   (hstex_token_character_code(token) == (uint8_t)'.' ||
                    hstex_token_character_code(token) == (uint8_t)',')) {
            saw_decimal = true;
        } else {
            if (!token_is_effective_space(engine, token) &&
                push_one(engine, token, location, error, error_capacity) != 0) {
                return -1;
            }
            break;
        }
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
    }
    if (!saw_digit) {
        char found[128];
        describe_token(engine, token, found, sizeof(found));
        return set_error(error, error_capacity,
                         "dimension requires a numeric factor, found %s",
                         found);
    }
    return 0;
}

static int try_keyword(struct hstex_engine *engine, const char *keyword,
                       bool *matched, char *error, size_t error_capacity);

/* A unit of measure ends with one optional space. */
static int skip_optional_space(struct hstex_engine *engine, char *error,
                               size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result = hstex_engine_next_expanded(
        engine, &token, &location, error, error_capacity);
    if (result == HSTEX_ENGINE_EOF) {
        return 0;
    }
    if (result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (token_is_effective_space(engine, token)) {
        return 0;
    }
    return push_one(engine, token, location, error, error_capacity);
}

/* Round the decimal fraction to scaled points, half away from zero. Binary
   long division keeps seventeen decimal digits inside 64 bits: the running
   remainder stays below the denominator, so doubling it cannot overflow. */
static uint64_t scaled_fraction(const struct decimal_factor *factor)
{
    uint64_t denominator = factor->denominator;
    if (denominator == 0U) {
        return 0U;
    }
    uint64_t remainder = factor->fraction;
    uint64_t quotient = 0U;
    for (unsigned int step = 0U; step < 16U; ++step) {
        remainder *= 2U;
        quotient *= 2U;
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient += 1U;
        }
    }
    if (remainder * 2U >= denominator) {
        quotient += 1U;
    }
    return quotient;
}

static int finish_scaled(uint64_t magnitude, bool negative, int32_t *value,
                         char *error, size_t error_capacity)
{
    if (magnitude > (uint64_t)HSTEX_MAX_DIMEN) {
        return set_error(error, error_capacity,
                         "dimension exceeds TeX's maximum");
    }
    *value = negative ? (int32_t)(-(int64_t)magnitude) : (int32_t)magnitude;
    return 0;
}

/* Convert a decimal factor through the rational ratio of a physical unit.
   The fraction is quantized to scaled points first, then the integer and
   fractional halves are converted separately. Deriving the conversion from
   the exact decimal instead would drift by several scaled points on ordinary
   values such as `0.3cm`; see docs/DECISIONS.md, dimension-unit-arithmetic. */
static int scaled_physical_unit(const struct decimal_factor *factor,
                                uint64_t numerator, uint64_t denominator,
                                int32_t *value, char *error,
                                size_t error_capacity)
{
    if (factor->whole > (uint64_t)HSTEX_MAX_DIMEN) {
        return set_error(error, error_capacity,
                         "dimension exceeds TeX's maximum");
    }
    uint64_t fraction = scaled_fraction(factor);
    uint64_t scaled_whole = factor->whole * numerator;
    uint64_t magnitude =
        (scaled_whole / denominator) * UINT64_C(65536) +
        (numerator * fraction + UINT64_C(65536) * (scaled_whole % denominator)) /
            denominator;
    return finish_scaled(magnitude, factor->sign < 0, value, error,
                         error_capacity);
}

/* Convert a decimal factor that multiplies an internal dimension, such as
   `2.5\parindent`, `1.5em`, or a glue component. The quantized fraction
   scales the unit and truncates, matching the observed reference values. */
static int scaled_internal_unit(const struct decimal_factor *factor,
                                int32_t unit, int32_t *value, char *error,
                                size_t error_capacity)
{
    if (factor->whole > (uint64_t)HSTEX_MAX_DIMEN) {
        return set_error(error, error_capacity,
                         "dimension exceeds TeX's maximum");
    }
    uint64_t fraction = scaled_fraction(factor);
    uint64_t magnitude = unit < 0 ? (uint64_t)(-(int64_t)unit) : (uint64_t)unit;
    uint64_t product = factor->whole * magnitude;
    if (product > (uint64_t)HSTEX_MAX_DIMEN) {
        return set_error(error, error_capacity,
                         "dimension exceeds TeX's maximum");
    }
    product += magnitude * fraction / UINT64_C(65536);
    return finish_scaled(product, (factor->sign < 0) != (unit < 0), value,
                         error, error_capacity);
}

/* Scan `fil`, `fill`, or `filll` and report the infinite order. */
static int scan_infinite_order(struct hstex_engine *engine, bool *matched,
                               uint8_t *order, char *error,
                               size_t error_capacity)
{
    if (try_keyword(engine, "fil", matched, error, error_capacity) != 0) {
        return -1;
    }
    if (!*matched) {
        return 0;
    }
    *order = 1U;
    for (;;) {
        bool another = false;
        if (try_keyword(engine, "l", &another, error, error_capacity) != 0) {
            return -1;
        }
        if (!another) {
            return 0;
        }
        if (*order >= 3U) {
            return set_error(error, error_capacity,
                             "infinite glue order beyond filll");
        }
        ++*order;
    }
}

static const struct {
    const char *name;
    uint64_t numerator;
    uint64_t denominator;
} hstex_physical_units[] = {
    {"pt", UINT64_C(1), UINT64_C(1)},
    {"in", UINT64_C(7227), UINT64_C(100)},
    {"pc", UINT64_C(12), UINT64_C(1)},
    {"cm", UINT64_C(7227), UINT64_C(254)},
    {"mm", UINT64_C(7227), UINT64_C(2540)},
    {"bp", UINT64_C(7227), UINT64_C(7200)},
    {"dd", UINT64_C(1238), UINT64_C(1157)},
    {"cc", UINT64_C(14856), UINT64_C(1157)},
};

/* Units are matched as keywords with backtracking rather than as a maximal
   run of letters: `12ptpt` is twelve points followed by the letters `pt`, and
   LaTeX's \@defaultunits depends on that leftover. */
static int scan_dimension_component(struct hstex_engine *engine, bool allow_fil,
                                    int32_t *value, uint8_t *order, char *error,
                                    size_t error_capacity)
{
    struct decimal_factor factor;
    bool direct_dimen = false;
    int32_t direct_value = 0;
    if (scan_decimal_factor(engine, &factor, &direct_dimen, &direct_value, error,
                            error_capacity) != 0) {
        return -1;
    }
    *order = 0U;
    if (direct_dimen) {
        *value = direct_value;
        return 0;
    }

    bool matched = false;
    if (allow_fil) {
        if (scan_infinite_order(engine, &matched, order, error,
                                error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            return scaled_physical_unit(&factor, UINT64_C(1), UINT64_C(1),
                                        value, error, error_capacity);
        }
    }

    hstex_token possible_unit = 0U;
    struct hstex_source_location possible_unit_location;
    if (expanded_next_non_space(engine, &possible_unit,
                                &possible_unit_location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning a dimension unit");
    }
    if (hstex_token_is_control_sequence(possible_unit)) {
        int32_t internal_unit = 0;
        int internal_result = dimen_from_meaning(
            engine,
            hstex_engine_meaning(
                engine, hstex_token_control_sequence_id(possible_unit)),
            &internal_unit, error, error_capacity);
        if (internal_result < 0) {
            return -1;
        }
        if (internal_result > 0) {
            return scaled_internal_unit(&factor, internal_unit, value, error,
                                        error_capacity);
        }
    }
    if (push_one(engine, possible_unit, possible_unit_location, error,
                 error_capacity) != 0) {
        return -1;
    }

    const char *font_unit = NULL;
    size_t font_parameter = 0U;
    if (try_keyword(engine, "em", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched) {
        font_unit = "em";
        font_parameter = 5U;
    } else {
        if (try_keyword(engine, "ex", &matched, error, error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            font_unit = "ex";
            font_parameter = 4U;
        }
    }
    if (font_unit != NULL) {
        const struct hstex_font *font =
            font_by_identifier(engine, engine->current_font);
        if (font == NULL || font_parameter >= font->dimen_count) {
            return set_error(error, error_capacity,
                             "current font does not define %s", font_unit);
        }
        int32_t unit = font->dimens[font_parameter];
        if (skip_optional_space(engine, error, error_capacity) != 0) {
            return -1;
        }
        return scaled_internal_unit(&factor, unit, value, error,
                                    error_capacity);
    }

    /* A `true` unit is measured before magnification, so the factor is
       scaled by 1000/\mag before the unit conversion; see
       docs/DECISIONS.md, dimension-unit-arithmetic. */
    if (try_keyword(engine, "true", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched) {
        int32_t magnification =
            engine->integer_parameters[HSTEX_INTEGER_MAGNIFICATION];
        if (magnification <= 0) {
            return set_error(error, error_capacity,
                             "magnification must be positive");
        }
        if (magnification != 1000) {
            if (factor.whole > (uint64_t)HSTEX_MAX_DIMEN) {
                return set_error(error, error_capacity,
                                 "dimension exceeds TeX's maximum");
            }
            uint64_t magnitude = (uint64_t)magnification;
            uint64_t scaled_whole = factor.whole * UINT64_C(1000);
            uint64_t whole = scaled_whole / magnitude;
            uint64_t remainder = scaled_whole % magnitude;
            uint64_t fraction =
                (UINT64_C(1000) * scaled_fraction(&factor) +
                 UINT64_C(65536) * remainder) /
                magnitude;
            whole += fraction / UINT64_C(65536);
            fraction %= UINT64_C(65536);
            /* Re-express the adjusted pair as an exact fraction of 65536 so
               that the unit conversion below sees it unchanged. */
            factor.whole = whole;
            factor.fraction = fraction;
            factor.denominator = UINT64_C(65536);
        }
    }

    for (size_t index = 0U;
         index < sizeof(hstex_physical_units) / sizeof(hstex_physical_units[0]);
         ++index) {
        if (try_keyword(engine, hstex_physical_units[index].name, &matched,
                        error, error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            if (skip_optional_space(engine, error, error_capacity) != 0) {
                return -1;
            }
            return scaled_physical_unit(&factor,
                                        hstex_physical_units[index].numerator,
                                        hstex_physical_units[index].denominator,
                                        value, error, error_capacity);
        }
    }

    /* A scaled-point factor discards its fraction. */
    if (try_keyword(engine, "sp", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched) {
        if (skip_optional_space(engine, error, error_capacity) != 0) {
            return -1;
        }
        return finish_scaled(factor.whole, factor.sign < 0, value, error,
                             error_capacity);
    }
    return set_error(error, error_capacity, "illegal unit of measure");
}

static int scan_dimension(struct hstex_engine *engine, int32_t *value,
                          char *error, size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    uint8_t order = 0U;
    int status = scan_dimension_component(engine, false, value, &order, error,
                                          error_capacity);
    engine->inhibit_protected_expansion = previous_inhibition;
    return status;
}

static int checked_dim_expression_value(int64_t value, int32_t *result,
                                        char *error, size_t error_capacity)
{
    if (value < -INT64_C(1073741823) || value > INT64_C(1073741823)) {
        return set_error(error, error_capacity,
                         "dimension expression exceeds TeX's maximum");
    }
    *result = (int32_t)value;
    return 0;
}

static int scan_dim_expression_sum(struct hstex_engine *engine,
                                   int32_t *value, char *error,
                                   size_t error_capacity);

static int scan_dim_expression_primary(struct hstex_engine *engine,
                                       int32_t *value, char *error,
                                       size_t error_capacity)
{
    int sign = 1;
    hstex_token token = 0U;
    struct hstex_source_location location;
    for (;;) {
        if (expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "missing dimension-expression operand");
        }
        if (token_is_other_character(token, (uint8_t)'+')) {
            continue;
        }
        if (token_is_other_character(token, (uint8_t)'-')) {
            sign = -sign;
            continue;
        }
        break;
    }

    int32_t magnitude = 0;
    if (token_is_other_character(token, (uint8_t)'(')) {
        if (scan_dim_expression_sum(engine, &magnitude, error,
                                    error_capacity) != 0 ||
            expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN ||
            !token_is_other_character(token, (uint8_t)')')) {
            return set_error(error, error_capacity,
                             "unbalanced dimension-expression parentheses");
        }
    } else {
        if (push_one(engine, token, location, error, error_capacity) != 0 ||
            scan_dimension(engine, &magnitude, error, error_capacity) != 0) {
            return -1;
        }
    }
    int64_t signed_value =
        sign < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
    return checked_dim_expression_value(signed_value, value, error,
                                        error_capacity);
}

static int divide_dim_expression(int32_t numerator, int32_t denominator,
                                 int32_t *result, char *error,
                                 size_t error_capacity)
{
    if (denominator == 0) {
        return set_error(error, error_capacity,
                         "division by zero in dimension expression");
    }
    uint64_t magnitude_numerator = numerator < 0
                                       ? (uint64_t)(-(int64_t)numerator)
                                       : (uint64_t)numerator;
    uint64_t magnitude_denominator = denominator < 0
                                         ? (uint64_t)(-(int64_t)denominator)
                                         : (uint64_t)denominator;
    uint64_t rounded =
        (magnitude_numerator + magnitude_denominator / 2U) /
        magnitude_denominator;
    int64_t signed_result = (numerator < 0) != (denominator < 0)
                                ? -(int64_t)rounded
                                : (int64_t)rounded;
    return checked_dim_expression_value(signed_result, result, error,
                                        error_capacity);
}

static int scan_dim_expression_term(struct hstex_engine *engine,
                                    int32_t *value, char *error,
                                    size_t error_capacity)
{
    if (scan_dim_expression_primary(engine, value, error, error_capacity) !=
        0) {
        return -1;
    }
    for (;;) {
        hstex_token operation = 0U;
        struct hstex_source_location operation_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &operation, &operation_location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result != HSTEX_ENGINE_TOKEN) {
            return -1;
        }
        bool multiply = token_is_other_character(operation, (uint8_t)'*');
        bool divide = token_is_other_character(operation, (uint8_t)'/');
        if (!multiply && !divide) {
            return push_one(engine, operation, operation_location, error,
                            error_capacity);
        }
        int32_t right = 0;
        if (scan_num_expression_primary(engine, &right, error,
                                        error_capacity) != 0) {
            return -1;
        }
        if (multiply) {
            if (checked_dim_expression_value((int64_t)*value * (int64_t)right,
                                             value, error,
                                             error_capacity) != 0) {
                return -1;
            }
        } else if (divide_dim_expression(*value, right, value, error,
                                         error_capacity) != 0) {
            return -1;
        }
    }
}

static int scan_dim_expression_sum(struct hstex_engine *engine,
                                   int32_t *value, char *error,
                                   size_t error_capacity)
{
    if (scan_dim_expression_term(engine, value, error, error_capacity) != 0) {
        return -1;
    }
    for (;;) {
        hstex_token operation = 0U;
        struct hstex_source_location operation_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &operation, &operation_location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result != HSTEX_ENGINE_TOKEN) {
            return -1;
        }
        bool add = token_is_other_character(operation, (uint8_t)'+');
        bool subtract = token_is_other_character(operation, (uint8_t)'-');
        if (!add && !subtract) {
            return push_one(engine, operation, operation_location, error,
                            error_capacity);
        }
        int32_t right = 0;
        if (scan_dim_expression_term(engine, &right, error, error_capacity) !=
            0) {
            return -1;
        }
        int64_t combined = add ? (int64_t)*value + (int64_t)right
                               : (int64_t)*value - (int64_t)right;
        if (checked_dim_expression_value(combined, value, error,
                                         error_capacity) != 0) {
            return -1;
        }
    }
}

static int scan_dim_expression(struct hstex_engine *engine, int32_t *value,
                               char *error, size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    int status = scan_dim_expression_sum(engine, value, error, error_capacity);
    if (status == 0) {
        hstex_token terminator = 0U;
        struct hstex_source_location terminator_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &terminator, &terminator_location, error, error_capacity);
        if (result == HSTEX_ENGINE_ERROR) {
            status = -1;
        } else if (result == HSTEX_ENGINE_TOKEN &&
                   !(hstex_token_is_control_sequence(terminator) &&
                     hstex_engine_meaning(
                         engine,
                         hstex_token_control_sequence_id(terminator))
                             ->command == HSTEX_COMMAND_RELAX)) {
            status = push_one(engine, terminator, terminator_location, error,
                              error_capacity);
        }
    }
    engine->inhibit_protected_expansion = previous_inhibition;
    return status;
}

static int try_keyword(struct hstex_engine *engine, const char *keyword,
                       bool *matched, char *error, size_t error_capacity)
{
    size_t length = strlen(keyword);
    if (length > 15U) {
        return set_error(error, error_capacity, "internal keyword too long");
    }
    hstex_token tokens[16];
    struct hstex_source_location locations[16];
    size_t consumed = 0U;
    for (size_t index = 0U; index < length; ++index) {
        enum hstex_engine_result result =
            index == 0U
                ? expanded_next_non_space(engine, &tokens[index],
                                          &locations[index], error,
                                          error_capacity)
                : hstex_engine_next_expanded(engine, &tokens[index],
                                             &locations[index], error,
                                             error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            if (result == HSTEX_ENGINE_ERROR) {
                return -1;
            }
            for (size_t restore = consumed; restore > 0U; --restore) {
                if (push_one(engine, tokens[restore - 1U],
                             locations[restore - 1U], error,
                             error_capacity) != 0) {
                    return -1;
                }
            }
            *matched = false;
            return 0;
        }
        ++consumed;
        uint8_t character = hstex_token_is_character(tokens[index])
                                ? hstex_token_character_code(tokens[index])
                                : 0U;
        if (character >= (uint8_t)'A' && character <= (uint8_t)'Z') {
            character = (uint8_t)(character + ((uint8_t)'a' - (uint8_t)'A'));
        }
        if (character != (uint8_t)keyword[index]) {
            for (size_t restore = consumed; restore > 0U; --restore) {
                if (push_one(engine, tokens[restore - 1U],
                             locations[restore - 1U], error,
                             error_capacity) != 0) {
                    return -1;
                }
            }
            *matched = false;
            return 0;
        }
    }
    *matched = true;
    return 0;
}

static int glue_from_meaning(struct hstex_engine *engine,
                             const struct hstex_meaning *meaning,
                             struct hstex_glue *value, char *error,
                             size_t error_capacity)
{
    if (meaning->command == HSTEX_COMMAND_LAST_ITEM &&
        meaning->value.integer == (int32_t)HSTEX_LAST_SKIP) {
        const struct hstex_node *node = current_list_last_node(engine);
        memset(value, 0, sizeof(*value));
        if (node != NULL && node->kind == HSTEX_NODE_GLUE) {
            value->width = node->width;
            value->stretch = node->value.glue.stretch;
            value->shrink = node->value.glue.shrink;
            value->stretch_order = node->value.glue.stretch_order;
            value->shrink_order = node->value.glue.shrink_order;
        }
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_SKIP_REGISTER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid skip-register meaning");
        }
        *value = engine->glues[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_GLUE_PARAMETER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_GLUE_PARAMETER_COUNT) {
            return set_error(error, error_capacity,
                             "invalid glue-parameter meaning");
        }
        *value = engine->glue_parameters[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_SKIP) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "skip register outside supported range");
        }
        *value = engine->glues[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_GLUE_EXPR) {
        if (scan_glue_expression(engine, value, error, error_capacity) != 0) {
            return -1;
        }
        return 1;
    }
    return 0;
}

static int math_glue_from_meaning(struct hstex_engine *engine,
                                  const struct hstex_meaning *meaning,
                                  struct hstex_glue *value, char *error,
                                  size_t error_capacity)
{
    if (meaning->command == HSTEX_COMMAND_MUSKIP_REGISTER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid muskip-register meaning");
        }
        *value = engine->muglues[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_MUGLUE_PARAMETER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_MUGLUE_PARAMETER_COUNT) {
            return set_error(error, error_capacity,
                             "invalid math-glue-parameter meaning");
        }
        *value = engine->muglue_parameters[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_MUSKIP) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "muskip register outside supported range");
        }
        *value = engine->muglues[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_MU_EXPR) {
        if (scan_math_glue_expression(engine, value, error, error_capacity) !=
            0) {
            return -1;
        }
        return 1;
    }
    return 0;
}

static int scan_mu_dimension_component(struct hstex_engine *engine,
                                       bool allow_fil, int32_t *value,
                                       uint8_t *order, char *error,
                                       size_t error_capacity)
{
    struct decimal_factor factor;
    bool direct_dimen = false;
    int32_t direct_value = 0;
    if (scan_decimal_factor(engine, &factor, &direct_dimen, &direct_value, error,
                            error_capacity) != 0) {
        return -1;
    }
    if (direct_dimen) {
        return set_error(error, error_capacity,
                         "ordinary dimension used as a math-glue component");
    }
    *order = 0U;
    bool matched = false;
    if (allow_fil) {
        if (scan_infinite_order(engine, &matched, order, error,
                                error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            return scaled_physical_unit(&factor, UINT64_C(1), UINT64_C(1),
                                        value, error, error_capacity);
        }
    }
    if (try_keyword(engine, "mu", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (!matched) {
        return set_error(error, error_capacity,
                         "illegal unit of measure in math glue");
    }
    if (skip_optional_space(engine, error, error_capacity) != 0) {
        return -1;
    }
    return scaled_physical_unit(&factor, UINT64_C(1), UINT64_C(1), value, error,
                                error_capacity);
}

static int scan_glue_literal(struct hstex_engine *engine,
                             struct hstex_glue *glue, char *error,
                             size_t error_capacity)
{
    memset(glue, 0, sizeof(*glue));
    if (scan_dimension(engine, &glue->width, error, error_capacity) != 0) {
        return -1;
    }
    bool matched = false;
    if (try_keyword(engine, "plus", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched &&
        scan_dimension_component(engine, true, &glue->stretch,
                                 &glue->stretch_order, error,
                                 error_capacity) != 0) {
        return -1;
    }
    if (try_keyword(engine, "minus", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched &&
        scan_dimension_component(engine, true, &glue->shrink,
                                 &glue->shrink_order, error,
                                 error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int scan_math_glue_literal(struct hstex_engine *engine,
                                  struct hstex_glue *glue, char *error,
                                  size_t error_capacity)
{
    memset(glue, 0, sizeof(*glue));
    uint8_t width_order = 0U;
    if (scan_mu_dimension_component(engine, false, &glue->width,
                                    &width_order, error, error_capacity) != 0) {
        return -1;
    }
    bool matched = false;
    if (try_keyword(engine, "plus", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched &&
        scan_mu_dimension_component(engine, true, &glue->stretch,
                                    &glue->stretch_order, error,
                                    error_capacity) != 0) {
        return -1;
    }
    if (try_keyword(engine, "minus", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched &&
        scan_mu_dimension_component(engine, true, &glue->shrink,
                                    &glue->shrink_order, error,
                                    error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int scan_glue(struct hstex_engine *engine, struct hstex_glue *glue,
                     char *error, size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    hstex_token token = 0U;
    struct hstex_source_location location;
    int sign = 1;
    enum hstex_engine_result result;
    for (;;) {
        result = expanded_next_non_space(engine, &token, &location, error,
                                         error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            break;
        }
        if (token_is_other_character(token, (uint8_t)'+')) {
            continue;
        }
        if (token_is_other_character(token, (uint8_t)'-')) {
            sign = -sign;
            continue;
        }
        break;
    }
    int status = 0;
    if (result == HSTEX_ENGINE_ERROR) {
        status = -1;
    } else if (result == HSTEX_ENGINE_EOF) {
        status = set_error(error, error_capacity,
                           "end of input while scanning glue");
    } else if (hstex_token_is_control_sequence(token)) {
        int internal_result = glue_from_meaning(
            engine,
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token)),
            glue, error, error_capacity);
        if (internal_result < 0) {
            status = -1;
        } else if (internal_result > 0) {
            if (sign < 0) {
                glue->width = -glue->width;
                glue->stretch = -glue->stretch;
                glue->shrink = -glue->shrink;
            }
        } else {
            if (push_one(engine, token, location, error, error_capacity) != 0 ||
                (sign < 0 &&
                 push_one(engine,
                          hstex_token_character((uint8_t)HSTEX_CAT_OTHER,
                                                (uint8_t)'-'),
                          location, error, error_capacity) != 0)) {
                status = -1;
            } else {
                status =
                    scan_glue_literal(engine, glue, error, error_capacity);
            }
        }
    } else {
        if (push_one(engine, token, location, error, error_capacity) != 0 ||
            (sign < 0 &&
             push_one(engine,
                      hstex_token_character((uint8_t)HSTEX_CAT_OTHER,
                                            (uint8_t)'-'),
                      location, error, error_capacity) != 0)) {
            status = -1;
        } else {
            status = scan_glue_literal(engine, glue, error, error_capacity);
        }
    }
    engine->inhibit_protected_expansion = previous_inhibition;
    return status;
}

static int scan_math_glue(struct hstex_engine *engine,
                          struct hstex_glue *glue, char *error,
                          size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    hstex_token token = 0U;
    struct hstex_source_location location;
    /* The signs come first, and may stand in front of a muskip register just
       as they may in front of a literal: LaTeX's \, is \mskip+\thinmuskip. */
    int sign = 1;
    enum hstex_engine_result result;
    for (;;) {
        result = expanded_next_non_space(engine, &token, &location, error,
                                         error_capacity);
        if (result != HSTEX_ENGINE_TOKEN ||
            !token_is_category(token, HSTEX_CAT_OTHER)) {
            break;
        }
        uint8_t code = hstex_token_character_code(token);
        if (code == (uint8_t)'-') {
            sign = -sign;
            continue;
        }
        if (code != (uint8_t)'+') {
            break;
        }
    }
    int status = 0;
    if (result != HSTEX_ENGINE_TOKEN) {
        status = set_error(error, error_capacity,
                           "end of input while scanning math glue");
    } else if (hstex_token_is_control_sequence(token)) {
        int internal_result = math_glue_from_meaning(
            engine,
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token)),
            glue, error, error_capacity);
        if (internal_result < 0) {
            status = -1;
        } else if (internal_result == 0 &&
                   push_one(engine, token, location, error, error_capacity) !=
                       0) {
            status = -1;
        } else if (internal_result == 0) {
            status =
                scan_math_glue_literal(engine, glue, error, error_capacity);
        }
    } else if (push_one(engine, token, location, error, error_capacity) != 0) {
        status = -1;
    } else {
        status = scan_math_glue_literal(engine, glue, error, error_capacity);
    }
    if (status == 0 && sign < 0) {
        glue->width = -glue->width;
        glue->stretch = -glue->stretch;
        glue->shrink = -glue->shrink;
    }
    engine->inhibit_protected_expansion = previous_inhibition;
    return status;
}

static void normalize_glue_order(int32_t value, uint8_t *order)
{
    if (value == 0) {
        *order = 0U;
    }
}

static int combine_glue_component(int32_t left, uint8_t left_order,
                                  int32_t right, uint8_t right_order,
                                  int sign, int32_t *result,
                                  uint8_t *result_order, char *error,
                                  size_t error_capacity)
{
    int64_t signed_right = sign < 0 ? -(int64_t)right : (int64_t)right;
    if (left == 0) {
        if (checked_dim_expression_value(signed_right, result, error,
                                         error_capacity) != 0) {
            return -1;
        }
        *result_order = right_order;
    } else if (right == 0 || left_order > right_order) {
        *result = left;
        *result_order = left_order;
    } else if (right_order > left_order) {
        if (checked_dim_expression_value(signed_right, result, error,
                                         error_capacity) != 0) {
            return -1;
        }
        *result_order = right_order;
    } else {
        if (checked_dim_expression_value((int64_t)left + signed_right, result,
                                         error, error_capacity) != 0) {
            return -1;
        }
        *result_order = left_order;
    }
    normalize_glue_order(*result, result_order);
    return 0;
}

static int combine_glue(struct hstex_glue *left,
                        const struct hstex_glue *right, int sign, char *error,
                        size_t error_capacity)
{
    int64_t signed_width =
        sign < 0 ? -(int64_t)right->width : (int64_t)right->width;
    if (checked_dim_expression_value((int64_t)left->width + signed_width,
                                     &left->width, error,
                                     error_capacity) != 0 ||
        combine_glue_component(left->stretch, left->stretch_order,
                               right->stretch, right->stretch_order, sign,
                               &left->stretch, &left->stretch_order, error,
                               error_capacity) != 0 ||
        combine_glue_component(left->shrink, left->shrink_order,
                               right->shrink, right->shrink_order, sign,
                               &left->shrink, &left->shrink_order, error,
                               error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int scale_glue_component(int32_t value, int32_t factor, bool divide,
                                int32_t *result, char *error,
                                size_t error_capacity)
{
    if (divide) {
        return divide_dim_expression(value, factor, result, error,
                                     error_capacity);
    }
    return checked_dim_expression_value((int64_t)value * (int64_t)factor,
                                        result, error, error_capacity);
}

static int scale_glue(struct hstex_glue *value, int32_t factor, bool divide,
                      char *error, size_t error_capacity)
{
    if (scale_glue_component(value->width, factor, divide, &value->width,
                             error, error_capacity) != 0 ||
        scale_glue_component(value->stretch, factor, divide, &value->stretch,
                             error, error_capacity) != 0 ||
        scale_glue_component(value->shrink, factor, divide, &value->shrink,
                             error, error_capacity) != 0) {
        return -1;
    }
    normalize_glue_order(value->stretch, &value->stretch_order);
    normalize_glue_order(value->shrink, &value->shrink_order);
    return 0;
}

static int scan_glue_expression_sum(struct hstex_engine *engine,
                                    struct hstex_glue *value, bool math,
                                    char *error, size_t error_capacity);

static int scan_glue_expression_primary(struct hstex_engine *engine,
                                        struct hstex_glue *value, bool math,
                                        char *error, size_t error_capacity)
{
    int sign = 1;
    hstex_token token = 0U;
    struct hstex_source_location location;
    for (;;) {
        if (expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "missing glue-expression operand");
        }
        if (token_is_other_character(token, (uint8_t)'+')) {
            continue;
        }
        if (token_is_other_character(token, (uint8_t)'-')) {
            sign = -sign;
            continue;
        }
        break;
    }
    if (token_is_other_character(token, (uint8_t)'(')) {
        if (scan_glue_expression_sum(engine, value, math, error,
                                     error_capacity) !=
                0 ||
            expanded_next_non_space(engine, &token, &location, error,
                                    error_capacity) != HSTEX_ENGINE_TOKEN ||
            !token_is_other_character(token, (uint8_t)')')) {
            return set_error(error, error_capacity,
                             "unbalanced glue-expression parentheses");
        }
    } else {
        if (push_one(engine, token, location, error, error_capacity) != 0) {
            return -1;
        }
        int status = math ? scan_math_glue(engine, value, error, error_capacity)
                          : scan_glue(engine, value, error, error_capacity);
        if (status != 0) {
            return -1;
        }
    }
    if (sign < 0 && scale_glue(value, -1, false, error, error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int scan_glue_expression_term(struct hstex_engine *engine,
                                     struct hstex_glue *value, bool math,
                                     char *error, size_t error_capacity)
{
    if (scan_glue_expression_primary(engine, value, math, error,
                                     error_capacity) != 0) {
        return -1;
    }
    for (;;) {
        hstex_token operation = 0U;
        struct hstex_source_location operation_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &operation, &operation_location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result != HSTEX_ENGINE_TOKEN) {
            return -1;
        }
        bool multiply = token_is_other_character(operation, (uint8_t)'*');
        bool divide = token_is_other_character(operation, (uint8_t)'/');
        if (!multiply && !divide) {
            return push_one(engine, operation, operation_location, error,
                            error_capacity);
        }
        int32_t right = 0;
        if (scan_num_expression_primary(engine, &right, error,
                                        error_capacity) != 0 ||
            scale_glue(value, right, divide, error, error_capacity) != 0) {
            return -1;
        }
    }
}

static int scan_glue_expression_sum(struct hstex_engine *engine,
                                    struct hstex_glue *value, bool math,
                                    char *error, size_t error_capacity)
{
    if (scan_glue_expression_term(engine, value, math, error, error_capacity) !=
        0) {
        return -1;
    }
    for (;;) {
        hstex_token operation = 0U;
        struct hstex_source_location operation_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &operation, &operation_location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result != HSTEX_ENGINE_TOKEN) {
            return -1;
        }
        bool add = token_is_other_character(operation, (uint8_t)'+');
        bool subtract = token_is_other_character(operation, (uint8_t)'-');
        if (!add && !subtract) {
            return push_one(engine, operation, operation_location, error,
                            error_capacity);
        }
        struct hstex_glue right;
        if (scan_glue_expression_term(engine, &right, math, error,
                                      error_capacity) != 0 ||
            combine_glue(value, &right, add ? 1 : -1, error,
                         error_capacity) != 0) {
            return -1;
        }
    }
}

static int scan_glue_expression_mode(struct hstex_engine *engine,
                                     struct hstex_glue *value, bool math,
                                     char *error, size_t error_capacity)
{
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    int status = scan_glue_expression_sum(engine, value, math, error,
                                          error_capacity);
    if (status == 0) {
        hstex_token terminator = 0U;
        struct hstex_source_location terminator_location;
        enum hstex_engine_result result = expanded_next_non_space(
            engine, &terminator, &terminator_location, error, error_capacity);
        if (result == HSTEX_ENGINE_ERROR) {
            status = -1;
        } else if (result == HSTEX_ENGINE_TOKEN &&
                   !(hstex_token_is_control_sequence(terminator) &&
                     hstex_engine_meaning(
                         engine,
                         hstex_token_control_sequence_id(terminator))
                             ->command == HSTEX_COMMAND_RELAX)) {
            status = push_one(engine, terminator, terminator_location, error,
                              error_capacity);
        }
    }
    engine->inhibit_protected_expansion = previous_inhibition;
    return status;
}

static int scan_glue_expression(struct hstex_engine *engine,
                                struct hstex_glue *value, char *error,
                                size_t error_capacity)
{
    return scan_glue_expression_mode(engine, value, false, error,
                                     error_capacity);
}

static int scan_math_glue_expression(struct hstex_engine *engine,
                                     struct hstex_glue *value, char *error,
                                     size_t error_capacity)
{
    return scan_glue_expression_mode(engine, value, true, error,
                                     error_capacity);
}

static int push_integer_expansion(struct hstex_engine *engine, int32_t value,
                                  struct hstex_source_location location,
                                  char *error, size_t error_capacity)
{
    char digits[32];
    int length = snprintf(digits, sizeof(digits), "%lld", (long long)value);
    if (length <= 0 || (size_t)length >= sizeof(digits)) {
        return set_error(error, error_capacity,
                         "could not format integer expansion");
    }
    struct token_vector expansion = {0};
    for (int index = 0; index < length; ++index) {
        if (vector_push(&expansion,
                        hstex_token_character((uint8_t)HSTEX_CAT_OTHER,
                                              (uint8_t)digits[index]),
                        error, error_capacity) != 0) {
            vector_destroy(&expansion);
            return -1;
        }
    }
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int format_scaled_value(int32_t value, const char *unit, char *digits,
                               size_t digits_capacity)
{
    uint64_t magnitude =
        value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
    uint64_t whole = magnitude / UINT64_C(65536);
    uint64_t remainder = magnitude % UINT64_C(65536);
    /* Print the shortest decimal that reads back as this scaled value, and at
       that length the one nearest to it. Five digits always suffice, because
       consecutive scaled points are more than 10^-5 apart; see
       docs/DECISIONS.md, scaled-printing. */
    uint64_t fraction = 0U;
    unsigned int fraction_digits = 1U;
    uint64_t power = 1U;
    for (unsigned int width = 1U; width <= 5U; ++width) {
        power *= UINT64_C(10);
        uint64_t candidate =
            (remainder * power + UINT64_C(32768)) / UINT64_C(65536);
        if (candidate >= power) {
            continue;
        }
        uint64_t restored =
            (candidate * UINT64_C(65536) + power / UINT64_C(2)) / power;
        if (restored == remainder) {
            fraction = candidate;
            fraction_digits = width;
            break;
        }
    }
    int length = snprintf(digits, digits_capacity, "%s%llu.%0*llu%s",
                          value < 0 ? "-" : "", (unsigned long long)whole,
                          (int)fraction_digits, (unsigned long long)fraction,
                          unit);
    return length > 0 && (size_t)length < digits_capacity ? length : -1;
}

static int push_other_character_expansion(
    struct hstex_engine *engine, const char *digits, size_t length,
    struct hstex_source_location location, char *error,
    size_t error_capacity)
{
    struct token_vector expansion = {0};
    for (size_t index = 0U; index < length; ++index) {
        /* Text turned back into tokens is all of category twelve except
           for the space, which keeps category ten; see
           docs/DECISIONS.md, spaces-in-expanded-text. */
        uint8_t category = digits[index] == ' '
                               ? (uint8_t)HSTEX_CAT_SPACE
                               : (uint8_t)HSTEX_CAT_OTHER;
        if (vector_push(&expansion,
                        hstex_token_character(category,
                                              (uint8_t)digits[index]),
                        error, error_capacity) != 0) {
            vector_destroy(&expansion);
            return -1;
        }
    }
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int expand_job_name(struct hstex_engine *engine,
                           struct hstex_source_location location, char *error,
                           size_t error_capacity)
{
    if (engine->job_name == NULL) {
        return set_error(error, error_capacity,
                         "jobname requested before an input file");
    }
    return push_other_character_expansion(
        engine, engine->job_name, strlen(engine->job_name), location, error,
        error_capacity);
}

static int expand_pdf_tex_revision(struct hstex_engine *engine,
                                   struct hstex_source_location location,
                                   char *error, size_t error_capacity)
{
    static const char revision[] = HSTEX_PDFTEX_REVISION;
    return push_other_character_expansion(engine, revision,
                                          sizeof(revision) - 1U, location,
                                          error, error_capacity);
}

static int push_dimension_expansion(struct hstex_engine *engine, int32_t value,
                                    struct hstex_source_location location,
                                    char *error, size_t error_capacity)
{
    char digits[64];
    int length = format_scaled_value(value, "pt", digits, sizeof(digits));
    if (length < 0) {
        return set_error(error, error_capacity,
                         "could not format dimension expansion");
    }
    return push_other_character_expansion(engine, digits, (size_t)length,
                                          location, error, error_capacity);
}

static int expand_font_name(struct hstex_engine *engine,
                            struct hstex_source_location location,
                            char *error, size_t error_capacity)
{
    uint32_t identifier = 0U;
    if (scan_font_identifier(engine, &identifier, error, error_capacity) != 0) {
        return -1;
    }
    const struct hstex_font *font = font_by_identifier(engine, identifier);
    struct token_vector expansion = {0};
    size_t name_length = strlen(font->name);
    for (size_t index = 0U; index < name_length; ++index) {
        uint8_t category = font->name[index] == ' '
                               ? (uint8_t)HSTEX_CAT_SPACE
                               : (uint8_t)HSTEX_CAT_OTHER;
        if (vector_push(&expansion,
                        hstex_token_character(category,
                                              (uint8_t)font->name[index]),
                        error, error_capacity) != 0) {
            vector_destroy(&expansion);
            return -1;
        }
    }
    if (font->size != INT32_C(10) * INT32_C(65536)) {
        static const char label[] = " at ";
        for (size_t index = 0U; index < sizeof(label) - 1U; ++index) {
            uint8_t category = label[index] == ' '
                                   ? (uint8_t)HSTEX_CAT_SPACE
                                   : (uint8_t)HSTEX_CAT_OTHER;
            if (vector_push(&expansion,
                            hstex_token_character(category,
                                                  (uint8_t)label[index]),
                            error, error_capacity) != 0) {
                vector_destroy(&expansion);
                return -1;
            }
        }
        char size_text[64];
        int size_length =
            format_scaled_value(font->size, "pt", size_text,
                                sizeof(size_text));
        if (size_length < 0) {
            vector_destroy(&expansion);
            return set_error(error, error_capacity,
                             "could not format font size");
        }
        for (int index = 0; index < size_length; ++index) {
            if (vector_push(&expansion,
                            hstex_token_character((uint8_t)HSTEX_CAT_OTHER,
                                                  (uint8_t)size_text[index]),
                            error, error_capacity) != 0) {
                vector_destroy(&expansion);
                return -1;
            }
        }
    }
    if (push_owned_vector(engine, &expansion, location, error,
                          error_capacity) != 0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int push_glue_expansion(struct hstex_engine *engine,
                               const struct hstex_glue *glue,
                               bool math,
                               struct hstex_source_location location,
                               char *error, size_t error_capacity)
{
    char digits[256];
    const char *finite_unit = math ? "mu" : "pt";
    int length = format_scaled_value(glue->width, finite_unit, digits,
                                     sizeof(digits));
    if (length < 0) {
        return set_error(error, error_capacity,
                         "could not format glue expansion");
    }
    const char *orders[] = {finite_unit, "fil", "fill", "filll"};
    const int32_t components[] = {glue->stretch, glue->shrink};
    const uint8_t component_orders[] = {glue->stretch_order,
                                        glue->shrink_order};
    const char *labels[] = {" plus ", " minus "};
    for (size_t component = 0U; component < 2U; ++component) {
        if (components[component] == 0) {
            continue;
        }
        if (component_orders[component] > 3U) {
            return set_error(error, error_capacity,
                             "invalid glue order in expansion");
        }
        int label_length = snprintf(digits + length, sizeof(digits) - (size_t)length,
                                    "%s", labels[component]);
        if (label_length < 0 ||
            (size_t)label_length >= sizeof(digits) - (size_t)length) {
            return set_error(error, error_capacity,
                             "could not format glue expansion");
        }
        length += label_length;
        int component_length = format_scaled_value(
            components[component], orders[component_orders[component]],
            digits + length, sizeof(digits) - (size_t)length);
        if (component_length < 0) {
            return set_error(error, error_capacity,
                             "could not format glue expansion");
        }
        length += component_length;
    }
    return push_other_character_expansion(engine, digits, (size_t)length,
                                          location, error, error_capacity);
}

static int expand_integer_primitive(struct hstex_engine *engine,
                                    struct hstex_source_location location,
                                    char *error, size_t error_capacity)
{
    int32_t value = 0;
    if (scan_integer(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    return push_integer_expansion(engine, value, location, error,
                                  error_capacity);
}

static int expand_roman_numeral(struct hstex_engine *engine,
                                struct hstex_source_location location,
                                char *error, size_t error_capacity)
{
    int32_t value = 0;
    if (scan_integer(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    if (value <= 0) {
        return 0;
    }
    static const struct {
        int32_t value;
        const char *digits;
    } parts[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
        {100, "c"},  {90, "xc"},  {50, "l"},  {40, "xl"},
        {10, "x"},   {9, "ix"},   {5, "v"},   {4, "iv"},
        {1, "i"},
    };
    struct token_vector expansion = {0};
    for (size_t part = 0U; part < sizeof(parts) / sizeof(parts[0]); ++part) {
        while (value >= parts[part].value) {
            for (size_t index = 0U; parts[part].digits[index] != '\0'; ++index) {
                if (vector_push(
                        &expansion,
                        hstex_token_character(
                            (uint8_t)HSTEX_CAT_OTHER,
                            (uint8_t)parts[part].digits[index]),
                        error, error_capacity) != 0) {
                    vector_destroy(&expansion);
                    return -1;
                }
            }
            value -= parts[part].value;
        }
    }
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int expand_pdf_file_size(struct hstex_engine *engine,
                                struct hstex_source_location location,
                                char *error, size_t error_capacity)
{
    char *filename = NULL;
    if (scan_input_filename(engine, &filename, error, error_capacity) != 0) {
        return -1;
    }
    char *path = resolve_input_path(engine, filename);
    free(filename);
    if (path == NULL) {
        return 0;
    }
    struct stat status;
    if (stat(path, &status) != 0 || status.st_size < 0) {
        free(path);
        return 0;
    }
    free(path);

    char digits[64];
    int length = snprintf(digits, sizeof(digits), "%" PRIuMAX,
                          (uintmax_t)status.st_size);
    if (length <= 0 || (size_t)length >= sizeof(digits)) {
        return set_error(error, error_capacity,
                         "could not format file size");
    }
    struct token_vector expansion = {0};
    for (int index = 0; index < length; ++index) {
        if (vector_push(&expansion,
                        hstex_token_character((uint8_t)HSTEX_CAT_OTHER,
                                              (uint8_t)digits[index]),
                        error, error_capacity) != 0) {
            vector_destroy(&expansion);
            return -1;
        }
    }
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int token_list_identifier_from_meaning(
    struct hstex_engine *engine, const struct hstex_meaning *meaning,
    uint32_t *identifier, char *error, size_t error_capacity)
{
    if (meaning->command == HSTEX_COMMAND_TOKS_REGISTER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid token-register meaning");
        }
        *identifier = engine->token_registers[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_TOKEN_PARAMETER) {
        int32_t index = meaning->value.integer;
        if (index < 0 || index >= (int32_t)HSTEX_TOKEN_PARAMETER_COUNT) {
            return set_error(error, error_capacity,
                             "invalid token-parameter meaning");
        }
        *identifier = engine->token_parameters[(size_t)index];
        return 1;
    }
    if (meaning->command == HSTEX_COMMAND_TOKS) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "token register outside supported range");
        }
        *identifier = engine->token_registers[(size_t)index];
        return 1;
    }
    return 0;
}

static int push_token_list_expansion(
    struct hstex_engine *engine, uint32_t identifier,
    struct hstex_source_location location, char *error,
    size_t error_capacity)
{
    if (identifier == 0U) {
        return 0;
    }
    const struct hstex_token_list *list =
        token_list_by_identifier(engine, identifier);
    if (list == NULL) {
        return set_error(error, error_capacity,
                         "invalid token-list identifier");
    }
    struct token_vector expansion = {0};
    if (vector_reserve(&expansion, list->count, error, error_capacity) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < list->count; ++index) {
        hstex_token token = list->tokens[index];
        if (hstex_token_is_control_sequence(token) ||
            hstex_token_is_frozen_control_sequence(token)) {
            token = hstex_token_unexpanded_control_sequence(
                hstex_token_control_sequence_id(token));
        } else {
            token = hstex_token_unexpanded_non_control(token);
        }
        expansion.data[expansion.count++] = token;
    }
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int expand_the_primitive(struct hstex_engine *engine,
                                struct hstex_source_location location,
                                char *error, size_t error_capacity)
{
    hstex_token subject = 0U;
    struct hstex_source_location subject_location;
    if (expanded_next_non_space(engine, &subject, &subject_location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning the");
    }
    if (hstex_token_is_control_sequence(subject)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(subject));
        uint32_t identifier = 0U;
        int result = token_list_identifier_from_meaning(
            engine, meaning, &identifier, error, error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            return push_token_list_expansion(engine, identifier, location,
                                             error, error_capacity);
        }
        struct hstex_glue glue;
        result = glue_from_meaning(engine, meaning, &glue, error,
                                   error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            return push_glue_expansion(engine, &glue, false, location, error,
                                       error_capacity);
        }
        result = math_glue_from_meaning(engine, meaning, &glue, error,
                                        error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            return push_glue_expansion(engine, &glue, true, location, error,
                                       error_capacity);
        }
        int32_t dimension = 0;
        result = dimen_from_meaning(engine, meaning, &dimension, error,
                                    error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            return push_dimension_expansion(engine, dimension, location, error,
                                            error_capacity);
        }
        /* \the on a font yields that font's identifier control sequence, not
           a printed representation. */
        if (meaning->command == HSTEX_COMMAND_FONT ||
            meaning->command == HSTEX_COMMAND_FONT_GIVEN ||
            meaning->command == HSTEX_COMMAND_MATH_FONT) {
            uint32_t font_identifier = 0U;
            if (meaning->command == HSTEX_COMMAND_MATH_FONT) {
                int32_t size = meaning->value.integer;
                int32_t family = 0;
                if (scan_integer(engine, &family, error, error_capacity) != 0) {
                    return -1;
                }
                if (size < 0 || size >= (int32_t)HSTEX_MATH_SIZE_COUNT ||
                    family < 0 || family > 15) {
                    return set_error(error, error_capacity,
                                     "math family %d is outside 0..15",
                                     family);
                }
                font_identifier = engine->math_fonts[size][family];
            } else {
                font_identifier =
                    meaning->command == HSTEX_COMMAND_FONT
                        ? engine->current_font
                        : (uint32_t)meaning->value.integer;
            }
            const struct hstex_font *font =
                font_by_identifier(engine, font_identifier);
            if (font == NULL || font->identifier_cs == 0U) {
                return set_error(error, error_capacity,
                                 "the requires a defined font");
            }
            return push_one(engine,
                            hstex_token_control_sequence(font->identifier_cs),
                            location, error, error_capacity);
        }
    }
    if (push_one(engine, subject, subject_location, error, error_capacity) != 0) {
        return -1;
    }
    return expand_integer_primitive(engine, location, error, error_capacity);
}

static int scan_cs_name_bytes(struct hstex_engine *engine, uint8_t **name,
                              size_t *name_count, char *error,
                              size_t error_capacity)
{
    uint8_t *bytes = NULL;
    size_t count = 0U;
    size_t name_capacity = 0U;
    /* \protected suppresses expansion while an \edef or \write builds a token
       list, but a csname is expanded in full regardless: \edef\z{\csname
       \protectedmacro\endcsname} expands the macro. */
    bool previous_inhibition = engine->inhibit_protected_expansion;
    engine->inhibit_protected_expansion = false;
    bool previous_in_cs_name = engine->building_cs_name;
    engine->building_cs_name = true;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location token_location;
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &token_location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;
            free(bytes);
            return set_error(error, error_capacity,
                             "end of input inside csname");
        }
        (void)token_location;
        if (hstex_token_is_control_sequence(token)) {
            if (engine->returned_unexpanded &&
                engine->returned_unexpanded_executable) {
                /* \noexpand only defers one expansion step, so the token is
                   re-read without its marking and expands normally. This
                   terminates because the token pushed back has already been
                   normalized, and because protected macros cannot reach here
                   with expansion inhibition cleared above. */
                if (push_one(engine, token, token_location, error,
                             error_capacity) != 0) {
                    engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;
                    free(bytes);
                    return -1;
                }
                continue;
            }
            const struct hstex_meaning *meaning = hstex_engine_meaning(
                engine, hstex_token_control_sequence_id(token));
            if (meaning->command == HSTEX_COMMAND_END_CS_NAME) {
                break;
            }
            enum hstex_symbol_kind kind;
            const uint8_t *unexpected_name = NULL;
            size_t unexpected_length = 0U;
            if (hstex_symbol_name(
                    &engine->lexical_state.symbols,
                    hstex_token_control_sequence_id(token), &kind,
                    &unexpected_name, &unexpected_length) == 0) {
                (void)kind;
                int printable_length = unexpected_length > (size_t)INT_MAX
                                           ? INT_MAX
                                           : (int)unexpected_length;
                engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;
                free(bytes);
                return set_error(error, error_capacity,
                                 "non-character token inside csname: \\%.*s",
                                 printable_length,
                                 (const char *)unexpected_name);
            }
            engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;
            free(bytes);
            return set_error(error, error_capacity,
                             "non-character token inside csname");
        }
        if (!hstex_token_is_character(token)) {
            engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;
            free(bytes);
            return set_error(error, error_capacity,
                             "internal token inside csname");
        }
        if (append_byte(&bytes, &count, &name_capacity,
                        hstex_token_character_code(token), error,
                        error_capacity) != 0) {
            engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;
            free(bytes);
            return -1;
        }
    }
    engine->inhibit_protected_expansion = previous_inhibition;
            engine->building_cs_name = previous_in_cs_name;

    *name = bytes;
    *name_count = count;
    return 0;
}

static int expand_cs_name(struct hstex_engine *engine,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity)
{
    uint8_t *name = NULL;
    size_t name_count = 0U;
    if (scan_cs_name_bytes(engine, &name, &name_count, error, error_capacity) !=
        0) {
        return -1;
    }

    hstex_cs_id identifier = 0U;
    if (hstex_symbol_intern(&engine->lexical_state.symbols,
                            HSTEX_SYMBOL_REGULAR, name, name_count,
                            &identifier, error, error_capacity) != 0) {
        free(name);
        return -1;
    }
    free(name);
    if (hstex_engine_meaning(engine, identifier)->command ==
        HSTEX_COMMAND_UNDEFINED) {
        struct hstex_meaning relax = {
            .command = HSTEX_COMMAND_RELAX,
            .level = 0U,
            .value = {.macro_identifier = 0U},
        };
        if (set_meaning(engine, identifier, relax, false, error,
                        error_capacity) != 0) {
            return -1;
        }
    }
    return push_one(engine, hstex_token_control_sequence(identifier), location,
                    error, error_capacity);
}

static bool token_is_paragraph(const struct hstex_engine *engine,
                               hstex_token token)
{
    return hstex_token_is_control_sequence(token) &&
           hstex_token_control_sequence_id(token) ==
               engine->lexical_state.paragraph_control_sequence;
}

/* Balanced text counts only explicit braces, the way the reference compares
   tokens; see docs/DECISIONS.md, implicit-braces. */
static int scan_balanced_group(struct hstex_engine *engine,
                               struct token_vector *argument, bool long_macro,
                               char *error, size_t error_capacity)
{
    size_t depth = 1U;
    while (depth != 0U) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = raw_next(
            engine, &token, &location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "runaway macro argument at end of input");
        }
        token = normalize_unexpanded_control_sequence(token);
        if (!long_macro && token_is_paragraph(engine, token)) {
            return set_error(error, error_capacity,
                             "paragraph ended a non-long macro argument");
        }
        if (token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(token, HSTEX_CAT_END_GROUP)) {
            --depth;
            if (depth == 0U) {
                break;
            }
        }
        if (vector_push(argument, token, error, error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}


static int case_shift_active_character(struct hstex_engine *engine,
                                       hstex_token *token, size_t table,
                                       char *error, size_t error_capacity)
{
    enum hstex_symbol_kind kind;
    const uint8_t *name = NULL;
    size_t length = 0U;
    if (hstex_symbol_name(&engine->lexical_state.symbols,
                          hstex_token_control_sequence_id(*token), &kind, &name,
                          &length) != 0) {
        return set_error(error, error_capacity,
                         "invalid control sequence in case-shift text");
    }
    if (kind != HSTEX_SYMBOL_ACTIVE || length != 1U) {
        return 0;
    }
    int32_t mapped = engine->code_tables[table][name[0]];
    if (mapped == 0) {
        return 0;
    }
    uint8_t mapped_character = (uint8_t)mapped;
    hstex_cs_id mapped_identifier = 0U;
    if (hstex_symbol_intern(&engine->lexical_state.symbols,
                            HSTEX_SYMBOL_ACTIVE, &mapped_character, 1U,
                            &mapped_identifier, error, error_capacity) != 0 ||
        reserve_meanings(engine, (size_t)mapped_identifier, error,
                         error_capacity) != 0) {
        return -1;
    }
    *token = hstex_token_control_sequence(mapped_identifier);
    return 0;
}

static int execute_case_shift(struct hstex_engine *engine, size_t table,
                              struct hstex_source_location location,
                              char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location opening_location;
    enum hstex_engine_result opening_result =
        expanded_next_non_space_unrestricted(
            engine, &opening, &opening_location, error, error_capacity);
    if (opening_result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (opening_result != HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "case-shift primitive requires a braced token list");
    }
    (void)opening_location;

    struct token_vector text = {0};
    if (scan_balanced_group(engine, &text, true, error, error_capacity) != 0) {
        vector_destroy(&text);
        return -1;
    }
    for (size_t index = 0U; index < text.count; ++index) {
        hstex_token shifted =
            normalize_frozen_control_sequence(text.data[index]);
        if (hstex_token_is_character(shifted)) {
            uint8_t character = hstex_token_character_code(shifted);
            int32_t mapped = engine->code_tables[table][character];
            if (mapped != 0) {
                shifted = hstex_token_character(hstex_token_category(shifted),
                                                (uint8_t)mapped);
            }
        } else if (hstex_token_is_control_sequence(shifted) &&
                   case_shift_active_character(engine, &shifted, table, error,
                                               error_capacity) != 0) {
            vector_destroy(&text);
            return -1;
        }
        text.data[index] = shifted;
    }
    if (push_owned_vector(engine, &text, location, error, error_capacity) != 0) {
        vector_destroy(&text);
        return -1;
    }
    return 0;
}

static int expand_unexpanded_text(struct hstex_engine *engine,
                                  struct hstex_source_location location,
                                  char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location opening_location;
    enum hstex_engine_result opening_result =
        expanded_next_non_space_unrestricted(
            engine, &opening, &opening_location, error, error_capacity);
    if (opening_result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (opening_result != HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "unexpanded requires a braced token list");
    }
    struct token_vector output = {0};
    if (scan_balanced_group(engine, &output, true, error, error_capacity) != 0) {
        vector_destroy(&output);
        return -1;
    }
    (void)opening_location;
    for (size_t index = 0U; index < output.count; ++index) {
        if (hstex_token_is_control_sequence(output.data[index]) ||
            hstex_token_is_frozen_control_sequence(output.data[index])) {
            output.data[index] = hstex_token_unexpanded_control_sequence(
                hstex_token_control_sequence_id(output.data[index]));
        } else {
            output.data[index] =
                hstex_token_unexpanded_non_control(output.data[index]);
        }
    }
    if (push_owned_vector(engine, &output, location, error, error_capacity) !=
        0) {
        vector_destroy(&output);
        return -1;
    }
    return 0;
}

static int push_detokenized_character(struct token_vector *output,
                                      uint8_t character, char *error,
                                      size_t error_capacity)
{
    uint8_t category = character == (uint8_t)' '
                           ? (uint8_t)HSTEX_CAT_SPACE
                           : (uint8_t)HSTEX_CAT_OTHER;
    return vector_push(output, hstex_token_character(category, character),
                       error, error_capacity);
}

static bool regular_control_sequence_needs_space(
    const struct hstex_engine *engine, enum hstex_symbol_kind kind,
    const uint8_t *name, size_t name_length)
{
    return kind == HSTEX_SYMBOL_REGULAR &&
           (name_length > 1U ||
            (name_length == 1U &&
             hstex_catcode_get(&engine->lexical_state.catcodes, name[0]) ==
                 (uint8_t)HSTEX_CAT_LETTER));
}

static int expand_detokenize(struct hstex_engine *engine,
                             struct hstex_source_location location,
                             char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location opening_location;
    enum hstex_engine_result opening_result =
        expanded_next_non_space_unrestricted(
            engine, &opening, &opening_location, error, error_capacity);
    if (opening_result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (opening_result != HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "detokenize requires a braced token list");
    }
    struct token_vector input = {0};
    if (scan_balanced_group(engine, &input, true, error, error_capacity) != 0) {
        vector_destroy(&input);
        return -1;
    }
    (void)opening_location;

    struct token_vector output = {0};
    for (size_t index = 0U; index < input.count; ++index) {
        hstex_token token = input.data[index];
        if (hstex_token_is_character(token)) {
            uint8_t character = hstex_token_character_code(token);
            if (token_is_category(token, HSTEX_CAT_PARAMETER) &&
                push_detokenized_character(&output, character, error,
                                           error_capacity) != 0) {
                vector_destroy(&input);
                vector_destroy(&output);
                return -1;
            }
            if (push_detokenized_character(&output, character, error,
                                           error_capacity) != 0) {
                vector_destroy(&input);
                vector_destroy(&output);
                return -1;
            }
            continue;
        }
        if (!hstex_token_is_control_sequence(token) &&
            !hstex_token_is_frozen_control_sequence(token)) {
            vector_destroy(&input);
            vector_destroy(&output);
            return set_error(error, error_capacity,
                             "internal token inside detokenize");
        }
        enum hstex_symbol_kind kind;
        const uint8_t *name = NULL;
        size_t name_length = 0U;
        if (hstex_symbol_name(&engine->lexical_state.symbols,
                              hstex_token_control_sequence_id(token), &kind,
                              &name, &name_length) != 0) {
            vector_destroy(&input);
            vector_destroy(&output);
            return set_error(error, error_capacity,
                             "invalid control sequence inside detokenize");
        }
        int32_t escape =
            engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER];
        if (kind == HSTEX_SYMBOL_REGULAR && escape >= 0 && escape <= 255 &&
            push_detokenized_character(&output, (uint8_t)escape, error,
                                       error_capacity) != 0) {
            vector_destroy(&input);
            vector_destroy(&output);
            return -1;
        }
        for (size_t name_index = 0U; name_index < name_length; ++name_index) {
            if (push_detokenized_character(&output, name[name_index], error,
                                           error_capacity) != 0) {
                vector_destroy(&input);
                vector_destroy(&output);
                return -1;
            }
        }
        if (regular_control_sequence_needs_space(engine, kind, name,
                                                 name_length) &&
            push_detokenized_character(&output, (uint8_t)' ', error,
                                       error_capacity) != 0) {
            vector_destroy(&input);
            vector_destroy(&output);
            return -1;
        }
    }
    vector_destroy(&input);
    if (push_owned_vector(engine, &output, location, error, error_capacity) !=
        0) {
        vector_destroy(&output);
        return -1;
    }
    return 0;
}

static int expand_expanded_text(struct hstex_engine *engine,
                                struct hstex_source_location location,
                                char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location opening_location;
    enum hstex_engine_result opening_result =
        expanded_next_non_space_unrestricted(
            engine, &opening, &opening_location, error, error_capacity);
    if (opening_result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (opening_result != HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "expanded requires a braced token list");
    }
    (void)opening_location;

    struct token_vector expansion = {0};
    size_t depth = 1U;
    while (depth != 0U) {
        hstex_token token = 0U;
        struct hstex_source_location token_location;
        bool previous_inhibition = engine->inhibit_protected_expansion;
        engine->inhibit_protected_expansion = true;
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &token_location, error, error_capacity);
        engine->inhibit_protected_expansion = previous_inhibition;
        if (result == HSTEX_ENGINE_ERROR) {
            vector_destroy(&expansion);
            return -1;
        }
        if (result == HSTEX_ENGINE_EOF) {
            vector_destroy(&expansion);
            return set_error(error, error_capacity,
                             "end of input while scanning expanded text");
        }
        (void)token_location;
        if (token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(token, HSTEX_CAT_END_GROUP)) {
            --depth;
            if (depth == 0U) {
                break;
            }
        }
        /* \expanded yields a plain token list. Whatever protected a token
           from this expansion -- \unexpanded, \the, \noexpand -- does not
           protect it from the next one, so the marking is not carried over;
           see docs/DECISIONS.md, expanded-is-plain. */
        if (vector_push(&expansion, token, error, error_capacity) != 0) {
            vector_destroy(&expansion);
            return -1;
        }
    }
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    engine->returned_unexpanded = false;
    engine->returned_unexpanded_executable = false;
    return 0;
}

static bool vector_has_suffix(const struct token_vector *vector,
                              const hstex_token *suffix, size_t suffix_count)
{
    if (suffix_count > vector->count) {
        return false;
    }
    size_t start = vector->count - suffix_count;
    for (size_t index = 0U; index < suffix_count; ++index) {
        if (normalize_one_shot_token(vector->data[start + index]) !=
            normalize_one_shot_token(suffix[index])) {
            return false;
        }
    }
    return true;
}

static void strip_single_outer_group(struct token_vector *argument)
{
    if (argument->count < 2U ||
        !token_is_category(argument->data[0], HSTEX_CAT_BEGIN_GROUP) ||
        !token_is_category(argument->data[argument->count - 1U],
                           HSTEX_CAT_END_GROUP)) {
        return;
    }
    size_t depth = 0U;
    for (size_t index = 0U; index < argument->count; ++index) {
        if (token_is_category(argument->data[index], HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(argument->data[index],
                                     HSTEX_CAT_END_GROUP)) {
            if (depth == 0U) {
                return;
            }
            --depth;
            if (depth == 0U && index + 1U != argument->count) {
                return;
            }
        }
    }
    if (depth == 0U) {
        memmove(argument->data, argument->data + 1U,
                (argument->count - 2U) * sizeof(*argument->data));
        argument->count -= 2U;
    }
}

static int scan_delimited_argument(struct hstex_engine *engine,
                                   struct token_vector *argument,
                                   const hstex_token *delimiter,
                                   size_t delimiter_count, bool long_macro,
                                   char *error, size_t error_capacity)
{
    size_t depth = 0U;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = raw_next(
            engine, &token, &location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "runaway delimited macro argument");
        }
        token = normalize_unexpanded_control_sequence(token);
        if (!long_macro && token_is_paragraph(engine, token)) {
            return set_error(error, error_capacity,
                             "paragraph ended a non-long macro argument");
        }
        if (vector_push(argument, token, error, error_capacity) != 0) {
            return -1;
        }
        if (depth == 0U &&
            vector_has_suffix(argument, delimiter, delimiter_count)) {
            argument->count -= delimiter_count;
            strip_single_outer_group(argument);
            return 0;
        }
        if (token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(token, HSTEX_CAT_END_GROUP) && depth != 0U) {
            --depth;
        }
    }
}

/* Report the expected and actual token so that a mismatch identifies the
   delimiter that failed rather than only the macro. */
static void describe_token(struct hstex_engine *engine, hstex_token token,
                           char *buffer, size_t capacity)
{
    if (token == 0U) {
        (void)snprintf(buffer, capacity, "end of input");
        return;
    }
    token = normalize_frozen_control_sequence(token);
    if (hstex_token_is_character(token)) {
        (void)snprintf(buffer, capacity, "character '%c' of category %u",
                       (char)hstex_token_character_code(token),
                       (unsigned int)hstex_token_category(token));
        return;
    }
    if (hstex_token_is_control_sequence(token)) {
        enum hstex_symbol_kind kind;
        const uint8_t *name = NULL;
        size_t length = 0U;
        if (hstex_symbol_name(&engine->lexical_state.symbols,
                              hstex_token_control_sequence_id(token), &kind,
                              &name, &length) == 0) {
            (void)snprintf(buffer, capacity, "\\%.*s", (int)length,
                           (const char *)name);
            return;
        }
    }
    (void)snprintf(buffer, capacity, "an internal token");
}

/* Remember which primitive the executor is about to run. Scanners report the
   token they choked on, but not who asked for a value; both halves are needed
   to place a failure inside a macro expansion. */
static void record_executing_name(struct hstex_engine *engine, hstex_token token)
{
    describe_token(engine, token, engine->executing_name,
                   sizeof(engine->executing_name));
}

static int match_parameter_prefix(struct hstex_engine *engine,
                                  const hstex_token *tokens, size_t count,
                                  char *error, size_t error_capacity)
{
    for (size_t index = 0U; index < count; ++index) {
        hstex_token actual = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = raw_next(
            engine, &actual, &location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN ||
            normalize_one_shot_token(actual) !=
                normalize_one_shot_token(tokens[index])) {
            char expected[128];
            char found[128];
            describe_token(engine, tokens[index], expected, sizeof(expected));
            describe_token(engine, result == HSTEX_ENGINE_TOKEN ? actual : 0U,
                           found, sizeof(found));
            return set_error(error, error_capacity,
                             "macro invocation does not match parameter text: "
                             "expected %s, found %s",
                             expected, found);
        }
    }
    return 0;
}

static int instantiate_macro(struct hstex_engine *engine,
                             const struct hstex_macro *macro,
                             struct hstex_source_location call_location,
                             char *error, size_t error_capacity)
{
    struct token_vector arguments[HSTEX_MAX_PARAMETERS] = {{0}};
    size_t cursor = 0U;
    uint8_t next_parameter = 1U;
    int status = -1;

    while (next_parameter <= macro->parameter_count) {
        size_t marker = cursor;
        while (marker < macro->parameter_count_tokens &&
               !(hstex_token_is_parameter(macro->parameter_text[marker]) &&
                 hstex_token_parameter_number(macro->parameter_text[marker]) ==
                     next_parameter)) {
            ++marker;
        }
        if (marker >= macro->parameter_count_tokens ||
            match_parameter_prefix(engine, macro->parameter_text + cursor,
                                   marker - cursor, error, error_capacity) != 0) {
            goto cleanup;
        }
        size_t delimiter_start = marker + 1U;
        size_t delimiter_end = delimiter_start;
        while (delimiter_end < macro->parameter_count_tokens &&
               !hstex_token_is_parameter(
                   macro->parameter_text[delimiter_end])) {
            ++delimiter_end;
        }
        size_t delimiter_count = delimiter_end - delimiter_start;
        struct token_vector *argument = &arguments[next_parameter - 1U];
        bool long_macro = (macro->flags & (uint8_t)HSTEX_MACRO_LONG) != 0U;
        if (delimiter_count == 0U) {
            hstex_token first = 0U;
            struct hstex_source_location location;
            enum hstex_engine_result result = raw_next_non_space(
                engine, &first, &location, error, error_capacity);
            if (result != HSTEX_ENGINE_TOKEN) {
                (void)set_error(error, error_capacity,
                                "runaway undelimited macro argument");
                goto cleanup;
            }
            first = normalize_unexpanded_control_sequence(first);
            if (!long_macro && token_is_paragraph(engine, first)) {
                (void)set_error(error, error_capacity,
                                "paragraph ended a non-long macro argument");
                goto cleanup;
            }
            if (token_is_category(first, HSTEX_CAT_BEGIN_GROUP)) {
                if (scan_balanced_group(engine, argument, long_macro, error,
                                        error_capacity) != 0) {
                    goto cleanup;
                }
            } else if (vector_push(argument, first, error, error_capacity) != 0) {
                goto cleanup;
            }
        } else if (scan_delimited_argument(
                       engine, argument, macro->parameter_text + delimiter_start,
                       delimiter_count, long_macro, error, error_capacity) != 0) {
            goto cleanup;
        }
        cursor = delimiter_end;
        ++next_parameter;
    }
    if (match_parameter_prefix(engine, macro->parameter_text + cursor,
                               macro->parameter_count_tokens - cursor, error,
                               error_capacity) != 0) {
        goto cleanup;
    }

    struct token_vector expansion = {0};
    for (size_t index = 0U; index < macro->replacement_count; ++index) {
        hstex_token token = macro->replacement[index];
        if (hstex_token_is_parameter(token)) {
            uint8_t parameter = hstex_token_parameter_number(token);
            if (parameter == 0U || parameter > macro->parameter_count ||
                vector_append(&expansion, arguments[parameter - 1U].data,
                              arguments[parameter - 1U].count, error,
                              error_capacity) != 0) {
                vector_destroy(&expansion);
                goto cleanup;
            }
        } else if (vector_push(&expansion, token, error, error_capacity) != 0) {
            vector_destroy(&expansion);
            goto cleanup;
        }
    }
    if (push_owned_vector(engine, &expansion, call_location, error,
                          error_capacity) != 0) {
        vector_destroy(&expansion);
        goto cleanup;
    }
    status = 0;

cleanup:
    for (size_t index = 0U; index < HSTEX_MAX_PARAMETERS; ++index) {
        vector_destroy(&arguments[index]);
    }
    return status;
}

static int expand_token_once(struct hstex_engine *engine, hstex_token token,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity);

static bool mode_conditional_value(const struct hstex_engine *engine,
                                   enum hstex_command command)
{
    switch (command) {
    case HSTEX_COMMAND_IF_H_MODE:
        return engine->mode == HSTEX_MODE_HORIZONTAL;
    case HSTEX_COMMAND_IF_V_MODE:
        return engine->mode == HSTEX_MODE_VERTICAL;
    case HSTEX_COMMAND_IF_M_MODE:
        return engine->mode == HSTEX_MODE_MATH;
    case HSTEX_COMMAND_IF_INNER:
        return engine->inner_mode;
    case HSTEX_COMMAND_IF_IN_CS_NAME:
        return engine->building_cs_name;
    default:
        return false;
    }
}
static int scan_if_num(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_dim(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_x(struct hstex_engine *engine, char *error,
                     size_t error_capacity);
static int scan_if_char(struct hstex_engine *engine, char *error,
                        size_t error_capacity);
static int scan_if_cat(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_odd(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_case(struct hstex_engine *engine, char *error,
                        size_t error_capacity);
static int scan_if_eof(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_defined(struct hstex_engine *engine, char *error,
                           size_t error_capacity);
static int scan_if_cs_name(struct hstex_engine *engine, char *error,
                           size_t error_capacity);
static bool command_starts_conditional(enum hstex_command command);
static int push_conditional(struct hstex_engine *engine, size_t *index,
                            char *error, size_t error_capacity);
static int finish_conditional(struct hstex_engine *engine, size_t index,
                              bool condition, char *error,
                              size_t error_capacity);
static int set_undefined_control_sequence_error(
    const struct hstex_engine *engine, hstex_token token, char *error,
    size_t error_capacity);
static int start_conditional(struct hstex_engine *engine, bool condition,
                             char *error, size_t error_capacity);
static int skip_conditional(struct hstex_engine *engine, size_t target,
                            bool stop_at_else, char *error,
                            size_t error_capacity);
static int execute_else(struct hstex_engine *engine, char *error,
                        size_t error_capacity);
static int execute_or(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
static int execute_fi(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
static int skip_case_remainder(struct hstex_engine *engine, size_t target,
                               char *error, size_t error_capacity);
static int scan_if_font_char(struct hstex_engine *engine, char *error,
                             size_t error_capacity);
static int scan_if_box(struct hstex_engine *engine, int32_t subtype,
                       char *error, size_t error_capacity);
static int expand_meaning(struct hstex_engine *engine,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity);
static int expand_string(struct hstex_engine *engine,
                         struct hstex_source_location location, char *error,
                         size_t error_capacity);
static int expand_pdf_string_compare(
    struct hstex_engine *engine, struct hstex_source_location location,
    char *error, size_t error_capacity);
static int expand_pdf_match(struct hstex_engine *engine,
                            struct hstex_source_location location, char *error,
                            size_t error_capacity);
static int expand_pdf_escape(struct hstex_engine *engine,
                             enum pdf_escape_kind kind,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity);
static int expand_pdf_last_match(struct hstex_engine *engine,
                                 struct hstex_source_location location,
                                 char *error, size_t error_capacity);
static int expand_token_once(struct hstex_engine *engine, hstex_token token,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity);

static int expand_after(struct hstex_engine *engine,
                        struct hstex_source_location location, char *error,
                        size_t error_capacity)
{
    hstex_token first = 0U;
    hstex_token second = 0U;
    struct hstex_source_location first_location;
    struct hstex_source_location second_location;
    if (raw_next(engine, &first, &first_location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        raw_next(engine, &second, &second_location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input in expandafter");
    }
    if (expand_token_once(engine, second, second_location, error,
                          error_capacity) != 0) {
        return -1;
    }
    (void)location;
    return push_one(engine, first, first_location, error, error_capacity);
}

static int no_expand_once(struct hstex_engine *engine,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity)
{
    hstex_token next = 0U;
    struct hstex_source_location next_location;
    if (raw_next(engine, &next, &next_location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in noexpand");
    }
    if (hstex_token_is_control_sequence(next)) {
        next = hstex_token_frozen_control_sequence(
            hstex_token_control_sequence_id(next));
    }
    (void)location;
    return push_one(engine, next, next_location, error, error_capacity);
}

static int expand_unless(struct hstex_engine *engine,
                         struct hstex_source_location location, char *error,
                         size_t error_capacity)
{
    hstex_token conditional = 0U;
    struct hstex_source_location conditional_location;
    if (raw_next_non_space(engine, &conditional, &conditional_location, error,
                           error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(conditional)) {
        return set_error(error, error_capacity,
                         "unless requires a conditional primitive");
    }
    enum hstex_command command =
        hstex_engine_meaning(engine,
                             hstex_token_control_sequence_id(conditional))
            ->command;
    if (!command_starts_conditional(command) ||
        command == HSTEX_COMMAND_IF_CASE) {
        return set_error(error, error_capacity,
                         "unless requires a non-case conditional primitive");
    }
    engine->negate_next_conditional = true;
    if (expand_token_once(engine, conditional, conditional_location, error,
                          error_capacity) != 0) {
        engine->negate_next_conditional = false;
        return -1;
    }
    (void)location;
    return 0;
}

static int expand_token_once(struct hstex_engine *engine, hstex_token token,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    /* A control sequence inserted by \the carries a one-shot marker so that
       an enclosing \edef or \write receives it verbatim. In ordinary
       execution it is an ordinary token, so \expandafter expands it. A
       \noexpand marker is not executable and still suppresses expansion. */
    if (hstex_token_is_unexpanded_control_sequence(token)) {
        token = hstex_token_control_sequence(
            hstex_token_control_sequence_id(token));
    }
    if (!hstex_token_is_control_sequence(token)) {
        if (hstex_token_is_unexpanded_non_control(token)) {
            token = hstex_token_normalize_unexpanded_non_control(token);
        }
        return push_one(engine, token, location, error, error_capacity);
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_MACRO) {
        if (meaning->value.macro_identifier == 0U ||
            (size_t)meaning->value.macro_identifier > engine->macro_count) {
            return set_error(error, error_capacity, "invalid macro meaning");
        }
        const struct hstex_macro *macro =
            &engine->macros[meaning->value.macro_identifier - 1U];
        if (engine->inhibit_protected_expansion &&
            (macro->flags & (uint8_t)HSTEX_MACRO_PROTECTED) != 0U) {
            return push_one(engine, token, location, error, error_capacity);
        }
        return instantiate_macro(engine, macro, location, error,
                                 error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_EXPAND_AFTER) {
        return expand_after(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_NO_EXPAND) {
        return no_expand_once(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_UNLESS) {
        return expand_unless(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_CS_NAME) {
        return expand_cs_name(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_EXPANDED) {
        return expand_expanded_text(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_UNEXPANDED) {
        return expand_unexpanded_text(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_DETOKENIZE) {
        return expand_detokenize(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_FILE_SIZE) {
        return expand_pdf_file_size(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_STRING_COMPARE) {
        return expand_pdf_string_compare(engine, location, error,
                                         error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_SCAN_TOKENS) {
        return expand_scan_tokens(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_COLOR_STACK_INIT) {
        return expand_pdf_color_stack_init(engine, location, error,
                                           error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_MATCH) {
        return expand_pdf_match(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_ESCAPE_STRING) {
        return expand_pdf_escape(engine, PDF_ESCAPE_STRING, location, error,
                                 error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_ESCAPE_NAME) {
        return expand_pdf_escape(engine, PDF_ESCAPE_NAME, location, error,
                                 error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_ESCAPE_HEX) {
        return expand_pdf_escape(engine, PDF_ESCAPE_HEX, location, error,
                                 error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_UNESCAPE_HEX) {
        return expand_pdf_escape(engine, PDF_UNESCAPE_HEX, location, error,
                                 error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_LAST_MATCH) {
        return expand_pdf_last_match(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_THE) {
        return expand_the_primitive(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_NUMBER) {
        return expand_integer_primitive(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_ROMAN_NUMERAL) {
        return expand_roman_numeral(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_MEANING) {
        return expand_meaning(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_STRING) {
        return expand_string(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_JOB_NAME) {
        return expand_job_name(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_PDF_TEX_REVISION) {
        return expand_pdf_tex_revision(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_FONT_NAME) {
        return expand_font_name(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_NUM) {
        return scan_if_num(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_DIM) {
        return scan_if_dim(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_H_MODE ||
        meaning->command == HSTEX_COMMAND_IF_V_MODE ||
        meaning->command == HSTEX_COMMAND_IF_M_MODE ||
        meaning->command == HSTEX_COMMAND_IF_INNER ||
        meaning->command == HSTEX_COMMAND_IF_IN_CS_NAME) {
        return start_conditional(
            engine, mode_conditional_value(engine, meaning->command), error,
            error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_X) {
        return scan_if_x(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_CHAR) {
        return scan_if_char(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_CAT) {
        return scan_if_cat(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_ODD) {
        return scan_if_odd(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_FONT_CHAR) {
        return scan_if_font_char(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_BOX) {
        return scan_if_box(engine, meaning->value.integer, error,
                           error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_CASE) {
        return scan_if_case(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_EOF) {
        return scan_if_eof(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_DEFINED) {
        return scan_if_defined(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_CS_NAME) {
        return scan_if_cs_name(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_TRUE ||
        meaning->command == HSTEX_COMMAND_IF_FALSE) {
        return start_conditional(engine,
                                 meaning->command == HSTEX_COMMAND_IF_TRUE,
                                 error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_ELSE ||
        meaning->command == HSTEX_COMMAND_OR ||
        meaning->command == HSTEX_COMMAND_FI) {
        if (conditional_test_pending(engine)) {
            return push_relax_before(engine, token, location, error,
                                     error_capacity);
        }
        if (meaning->command == HSTEX_COMMAND_ELSE) {
            return execute_else(engine, error, error_capacity);
        }
        if (meaning->command == HSTEX_COMMAND_OR) {
            return execute_or(engine, error, error_capacity);
        }
        return execute_fi(engine, error, error_capacity);
    }
    return push_one(engine, token, location, error, error_capacity);
}

enum hstex_engine_result hstex_engine_next_expanded(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity)
{
    if (engine == NULL || token == NULL || location == NULL) {
        (void)set_error(error, error_capacity,
                        "invalid expanded-token request");
        return HSTEX_ENGINE_ERROR;
    }
    for (;;) {
        engine->returned_unexpanded = false;
        engine->returned_unexpanded_executable = false;
        enum hstex_engine_result result = raw_next(
            engine, token, location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            return result;
        }
        if (hstex_token_is_unexpanded_non_control(*token)) {
            *token = hstex_token_normalize_unexpanded_non_control(*token);
            engine->returned_unexpanded = true;
            engine->returned_unexpanded_executable = false;
            return HSTEX_ENGINE_TOKEN;
        }
        if (hstex_token_is_frozen_control_sequence(*token)) {
            bool executable = hstex_token_is_unexpanded_control_sequence(*token);
            *token = hstex_token_control_sequence(
                hstex_token_control_sequence_id(*token));
            engine->returned_unexpanded = true;
            engine->returned_unexpanded_executable = executable;
            return HSTEX_ENGINE_TOKEN;
        }
        if (!hstex_token_is_control_sequence(*token)) {
            return HSTEX_ENGINE_TOKEN;
        }
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(*token));
        if (meaning->command == HSTEX_COMMAND_MACRO) {
            if (meaning->value.macro_identifier == 0U ||
                (size_t)meaning->value.macro_identifier > engine->macro_count) {
                return (enum hstex_engine_result)set_error(
                    error, error_capacity,
                    "invalid macro meaning: identifier %u exceeds macro count %zu",
                    meaning->value.macro_identifier, engine->macro_count);
            }
            const struct hstex_macro *macro =
                &engine->macros[meaning->value.macro_identifier - 1U];
            if (engine->inhibit_protected_expansion &&
                (macro->flags & (uint8_t)HSTEX_MACRO_PROTECTED) != 0U) {
                engine->returned_unexpanded = true;
                engine->returned_unexpanded_executable = true;
                return HSTEX_ENGINE_TOKEN;
            }
            if (instantiate_macro(engine, macro, *location, error,
                                  error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_EXPAND_AFTER) {
            if (expand_after(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_NO_EXPAND) {
            hstex_token next = 0U;
            struct hstex_source_location next_location;
            if (raw_next(engine, &next, &next_location, error, error_capacity) !=
                HSTEX_ENGINE_TOKEN) {
                (void)set_error(error, error_capacity,
                                "end of input in noexpand");
                return HSTEX_ENGINE_ERROR;
            }
            *token = next;
            *location = next_location;
            /* \noexpand suppresses the expansion of a control sequence. A
               character token is not expandable, so it passes through
               unmarked and still counts as, say, a parameter marker. Token
               lists inserted by \unexpanded and \the are the ones that bypass
               later scanning. */
            if (hstex_token_is_control_sequence(next)) {
                engine->returned_unexpanded = true;
                engine->returned_unexpanded_executable = false;
            }
            return HSTEX_ENGINE_TOKEN;
        }
        if (meaning->command == HSTEX_COMMAND_UNLESS) {
            if (expand_unless(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_CS_NAME) {
            if (expand_cs_name(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_EXPANDED) {
            if (expand_expanded_text(engine, *location, error,
                                     error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_UNEXPANDED) {
            if (expand_unexpanded_text(engine, *location, error,
                                       error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_DETOKENIZE) {
            if (expand_detokenize(engine, *location, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_FILE_SIZE) {
            if (expand_pdf_file_size(engine, *location, error,
                                     error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_STRING_COMPARE) {
            if (expand_pdf_string_compare(engine, *location, error,
                                          error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_SCAN_TOKENS) {
            if (expand_scan_tokens(engine, *location, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_COLOR_STACK_INIT) {
            if (expand_pdf_color_stack_init(engine, *location, error,
                                            error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_MATCH) {
            if (expand_pdf_match(engine, *location, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_ESCAPE_STRING ||
            meaning->command == HSTEX_COMMAND_PDF_ESCAPE_NAME ||
            meaning->command == HSTEX_COMMAND_PDF_ESCAPE_HEX ||
            meaning->command == HSTEX_COMMAND_PDF_UNESCAPE_HEX) {
            enum pdf_escape_kind kind =
                meaning->command == HSTEX_COMMAND_PDF_ESCAPE_STRING
                    ? PDF_ESCAPE_STRING
                    : meaning->command == HSTEX_COMMAND_PDF_ESCAPE_NAME
                          ? PDF_ESCAPE_NAME
                          : meaning->command == HSTEX_COMMAND_PDF_ESCAPE_HEX
                                ? PDF_ESCAPE_HEX
                                : PDF_UNESCAPE_HEX;
            if (expand_pdf_escape(engine, kind, *location, error,
                                  error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_LAST_MATCH) {
            if (expand_pdf_last_match(engine, *location, error,
                                      error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_THE) {
            if (expand_the_primitive(engine, *location, error,
                                     error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_NUMBER) {
            if (expand_integer_primitive(engine, *location, error,
                                         error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_ROMAN_NUMERAL) {
            if (expand_roman_numeral(engine, *location, error,
                                     error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_MEANING) {
            if (expand_meaning(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_STRING) {
            if (expand_string(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_JOB_NAME) {
            if (expand_job_name(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_PDF_TEX_REVISION) {
            if (expand_pdf_tex_revision(engine, *location, error,
                                        error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_FONT_NAME) {
            if (expand_font_name(engine, *location, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_NUM) {
            if (scan_if_num(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_DIM) {
            if (scan_if_dim(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_H_MODE ||
            meaning->command == HSTEX_COMMAND_IF_V_MODE ||
            meaning->command == HSTEX_COMMAND_IF_M_MODE ||
            meaning->command == HSTEX_COMMAND_IF_INNER ||
        meaning->command == HSTEX_COMMAND_IF_IN_CS_NAME) {
            if (start_conditional(
                    engine, mode_conditional_value(engine, meaning->command),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_X) {
            if (scan_if_x(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_CHAR) {
            if (scan_if_char(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_CAT) {
            if (scan_if_cat(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_ODD) {
            if (scan_if_odd(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_CASE) {
            if (scan_if_case(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_FONT_CHAR) {
            if (scan_if_font_char(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_BOX) {
            if (scan_if_box(engine, meaning->value.integer, error,
                            error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_EOF) {
            if (scan_if_eof(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_DEFINED) {
            if (scan_if_defined(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_CS_NAME) {
            if (scan_if_cs_name(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_IF_TRUE ||
            meaning->command == HSTEX_COMMAND_IF_FALSE) {
            if (start_conditional(
                    engine, meaning->command == HSTEX_COMMAND_IF_TRUE, error,
                    error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_ELSE ||
            meaning->command == HSTEX_COMMAND_OR ||
            meaning->command == HSTEX_COMMAND_FI) {
            if (conditional_test_pending(engine)) {
                if (push_relax_before(engine, *token, *location, error,
                                      error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            int status = meaning->command == HSTEX_COMMAND_ELSE
                             ? execute_else(engine, error, error_capacity)
                             : meaning->command == HSTEX_COMMAND_OR
                                   ? execute_or(engine, error, error_capacity)
                                   : execute_fi(engine, error, error_capacity);
            if (status != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        return HSTEX_ENGINE_TOKEN;
    }
}

static int scan_definition(struct hstex_engine *engine, bool inherent_global,
                           bool expanded_replacement, char *error,
                           size_t error_capacity)
{
    hstex_token target = 0U;
    struct hstex_source_location target_location;
    if (raw_next_non_space(engine, &target, &target_location, error,
                           error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(
            target = normalize_frozen_control_sequence(target))) {
        return set_error(error, error_capacity,
                         "def requires a control-sequence target");
    }

    uint32_t origin_line = 0U;
    const char *origin = current_source_line(engine, &origin_line);
    struct token_vector parameter_text = {0};
    uint8_t parameter_count = 0U;
    bool has_hash_brace = false;
    hstex_token hash_brace = 0U;
    for (;;) {
        hstex_token current = 0U;
        struct hstex_source_location location;
        if (raw_next(engine, &current, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            vector_destroy(&parameter_text);
            return set_error(error, error_capacity,
                             "end of input in macro parameter text");
        }
        current = normalize_frozen_control_sequence(current);
        if (token_is_category(current, HSTEX_CAT_BEGIN_GROUP)) {
            break;
        }
        if (token_is_category(current, HSTEX_CAT_PARAMETER)) {
            hstex_token number = 0U;
            if (raw_next(engine, &number, &location, error, error_capacity) !=
                HSTEX_ENGINE_TOKEN) {
                vector_destroy(&parameter_text);
                return set_error(error, error_capacity,
                                 "end of input after macro parameter marker");
            }
            number = normalize_frozen_control_sequence(number);
            if (token_is_category(number, HSTEX_CAT_BEGIN_GROUP)) {
                if (vector_push(&parameter_text, number, error,
                                error_capacity) != 0) {
                    vector_destroy(&parameter_text);
                    return -1;
                }
                has_hash_brace = true;
                hash_brace = number;
                break;
            }
            if (!hstex_token_is_character(number) ||
                hstex_token_character_code(number) !=
                    (uint8_t)('1' + parameter_count) ||
                parameter_count >= HSTEX_MAX_PARAMETERS) {
                vector_destroy(&parameter_text);
                enum hstex_symbol_kind kind;
                const uint8_t *name = NULL;
                size_t length = 0U;
                char found[128];
                describe_token(engine, number, found, sizeof(found));
                if (hstex_symbol_name(&engine->lexical_state.symbols,
                                      hstex_token_control_sequence_id(target),
                                      &kind, &name, &length) == 0) {
                    return set_error(
                        error, error_capacity,
                        "parameter %u of \\%.*s should be #%u, found %s",
                        (unsigned int)parameter_count + 1U, (int)length,
                        (const char *)name,
                        (unsigned int)parameter_count + 1U, found);
                }
                return set_error(error, error_capacity,
                                 "macro parameters must be numbered consecutively");
            }
            ++parameter_count;
            current = hstex_token_parameter(parameter_count);
        }
        if (vector_push(&parameter_text, current, error, error_capacity) != 0) {
            vector_destroy(&parameter_text);
            return -1;
        }
    }

    struct token_vector replacement = {0};
    size_t depth = 1U;
    while (depth != 0U) {
        hstex_token current = 0U;
        struct hstex_source_location location;
        bool previous_inhibition = engine->inhibit_protected_expansion;
        if (expanded_replacement) {
            engine->inhibit_protected_expansion = true;
        }
        enum hstex_engine_result result =
            expanded_replacement
                ? hstex_engine_next_expanded(engine, &current, &location, error,
                                             error_capacity)
                : raw_next(engine, &current, &location, error, error_capacity);
        engine->inhibit_protected_expansion = previous_inhibition;
        /* Tokens delivered by \unexpanded or \the are inserted verbatim and
           are not rescanned for parameter markers. */
        bool current_unexpanded =
            expanded_replacement && engine->returned_unexpanded;
        if (result != HSTEX_ENGINE_TOKEN) {
            vector_destroy(&parameter_text);
            vector_destroy(&replacement);
            enum hstex_symbol_kind kind;
            const uint8_t *name = NULL;
            size_t length = 0U;
            if (hstex_symbol_name(&engine->lexical_state.symbols,
                                  hstex_token_control_sequence_id(target),
                                  &kind, &name, &length) == 0) {
                return set_error(error, error_capacity,
                                 "end of input while defining \\%.*s, "
                                 "started at %s:%u",
                                 (int)length, (const char *)name, origin,
                                 (unsigned int)origin_line);
            }
            return set_error(error, error_capacity,
                             "end of input in macro replacement text");
        }
        current = normalize_frozen_control_sequence(current);
        if (token_is_category(current, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(current, HSTEX_CAT_END_GROUP)) {
            --depth;
            if (depth == 0U) {
                break;
            }
        } else if (token_is_category(current, HSTEX_CAT_PARAMETER) &&
                   !current_unexpanded) {
            hstex_token following = 0U;
            if (raw_next(engine, &following, &location, error, error_capacity) !=
                HSTEX_ENGINE_TOKEN) {
                vector_destroy(&parameter_text);
                vector_destroy(&replacement);
                return set_error(error, error_capacity,
                                 "end of input after macro parameter marker");
            }
            following = normalize_frozen_control_sequence(following);
            if (token_is_category(following, HSTEX_CAT_PARAMETER)) {
                /* Two parameter markers produce one literal marker token. */
            } else if (hstex_token_is_character(following) &&
                       hstex_token_character_code(following) >= (uint8_t)'1' &&
                       hstex_token_character_code(following) <=
                           (uint8_t)('0' + parameter_count)) {
                current = hstex_token_parameter((uint8_t)(
                    hstex_token_character_code(following) - (uint8_t)'0'));
            } else {
                vector_destroy(&parameter_text);
                vector_destroy(&replacement);
                enum hstex_symbol_kind target_kind;
                const uint8_t *target_name = NULL;
                size_t target_length = 0U;
                if (hstex_symbol_name(&engine->lexical_state.symbols,
                                      hstex_token_control_sequence_id(target),
                                      &target_kind, &target_name,
                                      &target_length) != 0) {
                    target_name = (const uint8_t *)"?";
                    target_length = 1U;
                }
                if (hstex_token_is_character(following)) {
                    return set_error(
                        error, error_capacity,
                        "illegal macro parameter #%c in replacement text of "
                        "\\%.*s, which has %u parameters",
                        (char)hstex_token_character_code(following),
                        (int)target_length, (const char *)target_name,
                        (unsigned int)parameter_count);
                }
                enum hstex_symbol_kind kind;
                const uint8_t *name = NULL;
                size_t length = 0U;
                if (hstex_symbol_name(&engine->lexical_state.symbols,
                                      hstex_token_control_sequence_id(following),
                                      &kind, &name, &length) == 0) {
                    return set_error(error, error_capacity,
                                     "illegal macro parameter #\\%.*s in "
                                     "replacement text",
                                     (int)length, (const char *)name);
                }
                return set_error(error, error_capacity,
                                 "illegal macro parameter in replacement text");
            }
        }
        if (vector_push(&replacement, current, error, error_capacity) != 0) {
            vector_destroy(&parameter_text);
            vector_destroy(&replacement);
            return -1;
        }
    }

    if (has_hash_brace &&
        vector_push(&replacement, hash_brace, error, error_capacity) != 0) {
        vector_destroy(&parameter_text);
        vector_destroy(&replacement);
        return -1;
    }

    if (reserve_macros(engine, engine->macro_count + 1U, error,
                       error_capacity) != 0) {
        vector_destroy(&parameter_text);
        vector_destroy(&replacement);
        return -1;
    }
    struct hstex_macro *macro = &engine->macros[engine->macro_count];
    macro->parameter_text = parameter_text.data;
    macro->parameter_count_tokens = parameter_text.count;
    macro->replacement = replacement.data;
    macro->replacement_count = replacement.count;
    macro->parameter_count = parameter_count;
    macro->flags = engine->pending_macro_flags;
    ++engine->macro_count;

    struct hstex_meaning meaning = {
        .command = HSTEX_COMMAND_MACRO,
        .level = 0U,
        .value = {.macro_identifier = (uint32_t)engine->macro_count},
    };
    bool global = assignment_is_global(
        engine, inherent_global || engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    (void)target_location;
    return set_meaning(engine, hstex_token_control_sequence_id(target), meaning,
                       global, error, error_capacity);
}

static int scan_let(struct hstex_engine *engine, char *error,
                    size_t error_capacity)
{
    hstex_token target = 0U;
    struct hstex_source_location location;
    if (raw_next_non_space(engine, &target, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(target)) {
        return set_error(error, error_capacity,
                         "let requires a control-sequence target");
    }
    hstex_token source = 0U;
    if (raw_next_non_space(engine, &source, &location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in let");
    }
    if (token_is_other_character(source, (uint8_t)'=')) {
        if (raw_next(engine, &source, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity, "end of input after let=");
        }
        if (token_is_space(source) &&
            raw_next(engine, &source, &location, error, error_capacity) !=
                HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity, "end of input after let= ");
        }
    }
    struct hstex_meaning meaning;
    if (hstex_token_is_control_sequence(source)) {
        meaning = *hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(source));
    } else {
        meaning.command = HSTEX_COMMAND_TOKEN_ALIAS;
        meaning.level = 0U;
        meaning.value.token = source;
    }
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, hstex_token_control_sequence_id(target), meaning,
                       global, error, error_capacity);
}

static int scan_future_let(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    hstex_token target = 0U;
    struct hstex_source_location target_location;
    if (raw_next_non_space(engine, &target, &target_location, error,
                           error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(
            target = normalize_frozen_control_sequence(target))) {
        return set_error(error, error_capacity,
                         "futurelet requires a control-sequence target");
    }

    hstex_token first = 0U;
    hstex_token second = 0U;
    struct hstex_source_location first_location;
    struct hstex_source_location second_location;
    if (raw_next(engine, &first, &first_location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        raw_next(engine, &second, &second_location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in futurelet");
    }

    hstex_token source = normalize_one_shot_token(second);
    struct hstex_meaning meaning;
    if (hstex_token_is_control_sequence(source)) {
        meaning = *hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(source));
    } else {
        meaning.command = HSTEX_COMMAND_TOKEN_ALIAS;
        meaning.level = 0U;
        meaning.value.token = source;
    }

    struct token_vector replay = {0};
    if (vector_push(&replay, first, error, error_capacity) != 0 ||
        vector_push(&replay, second, error, error_capacity) != 0) {
        vector_destroy(&replay);
        return -1;
    }
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    if (set_meaning(engine, hstex_token_control_sequence_id(target), meaning,
                    global, error, error_capacity) != 0) {
        vector_destroy(&replay);
        return -1;
    }
    (void)target_location;
    (void)second_location;
    return push_owned_vector(engine, &replay, first_location, error,
                             error_capacity);
}

static int scan_font_definition(struct hstex_engine *engine, char *error,
                                size_t error_capacity)
{
    hstex_token target = 0U;
    struct hstex_source_location location;
    if (raw_next_non_space(engine, &target, &location, error,
                           error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(
            target = normalize_frozen_control_sequence(target)) ||
        scan_optional_equals(engine, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "font requires a control-sequence target");
    }

    char *name = NULL;
    if (scan_input_filename(engine, &name, error, error_capacity) != 0) {
        return -1;
    }
    int32_t size = 0;
    bool matched_at = false;
    bool matched_scaled = false;
    if (try_keyword(engine, "at", &matched_at, error, error_capacity) != 0) {
        free(name);
        return -1;
    }
    if (matched_at) {
        if (scan_dimension(engine, &size, error, error_capacity) != 0 ||
            size <= 0) {
            free(name);
            return set_error(error, error_capacity,
                             "invalid requested font size");
        }
    } else {
        if (try_keyword(engine, "scaled", &matched_scaled, error,
                        error_capacity) != 0) {
            free(name);
            return -1;
        }
        /* Without `at`, the font is used at its own design size, and
           `scaled` is a thousandth part of that rather than of ten points. */
        int32_t design = 0;
        if (tfm_design_size(name, &design, error, error_capacity) != 0) {
            free(name);
            return -1;
        }
        size = design;
        if (matched_scaled) {
            int32_t scale = 0;
            if (scan_integer(engine, &scale, error, error_capacity) != 0 ||
                scale <= 0 || scale > 32768) {
                free(name);
                return set_error(error, error_capacity,
                                 "invalid font scale");
            }
            int64_t scaled = ((int64_t)design * scale) / 1000;
            if (scaled <= 0 || scaled > INT32_C(1073741823)) {
                free(name);
                return set_error(error, error_capacity,
                                 "requested font size is outside range");
            }
            size = (int32_t)scaled;
        }
    }

    uint32_t identifier = 0U;
    int status = find_or_create_font(engine, name, size, &identifier, error,
                                     error_capacity);
    free(name);
    if (status != 0) {
        return -1;
    }
    /* A reused font is renamed to the control sequence that just declared it,
       so \the\font reports the most recent declaration. */
    struct hstex_font *declared = font_by_identifier(engine, identifier);
    if (declared != NULL) {
        declared->identifier_cs = hstex_token_control_sequence_id(target);
    }
    struct hstex_meaning meaning = {
        .command = HSTEX_COMMAND_FONT_GIVEN,
        .level = 0U,
        .value = {.integer = (int32_t)identifier},
    };
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, hstex_token_control_sequence_id(target), meaning,
                       global, error, error_capacity);
}

static int scan_font_dimen_assignment(struct hstex_engine *engine,
                                      char *error, size_t error_capacity)
{
    struct hstex_font *font = NULL;
    size_t index = 0U;
    int32_t value = 0;
    if (scan_font_dimen_reference(engine, true, &font, &index, error,
                                  error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid fontdimen assignment");
    }
    font->dimens[index] = value;
    if (font->dimen_count <= index) {
        font->dimen_count = index + 1U;
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return 0;
}

static int scan_font_integer_assignment(struct hstex_engine *engine,
                                        enum hstex_command command,
                                        char *error, size_t error_capacity)
{
    uint32_t identifier = 0U;
    int32_t value = 0;
    if (scan_font_identifier(engine, &identifier, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid font-integer assignment");
    }
    struct hstex_font *font = font_by_identifier(engine, identifier);
    if (command == HSTEX_COMMAND_HYPHEN_CHAR) {
        font->hyphen_character = value;
    } else {
        font->skew_character = value;
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return 0;
}

static int begin_group(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int end_group(struct hstex_engine *engine, char *error,
                     size_t error_capacity);

static int reserve_hbox_items(struct hstex_hbox_builder *builder,
                              size_t required, char *error,
                              size_t error_capacity)
{
    if (required <= builder->capacity) {
        return 0;
    }
    size_t capacity = builder->capacity == 0U
                          ? (size_t)HSTEX_INITIAL_HBOX_ITEM_CAPACITY
                          : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "hbox item capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*builder->node_identifiers)) {
        return set_error(error, error_capacity,
                         "hbox item allocation overflow");
    }
    void *allocation = realloc(
        builder->node_identifiers,
        capacity * sizeof(*builder->node_identifiers));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "hbox item allocation failed");
    }
    builder->node_identifiers = allocation;
    builder->capacity = capacity;
    return 0;
}

static int reserve_vbox_items(struct hstex_vbox_builder *builder,
                              size_t required, char *error,
                              size_t error_capacity)
{
    if (required <= builder->capacity) {
        return 0;
    }
    size_t capacity = builder->capacity == 0U
                          ? (size_t)HSTEX_INITIAL_VBOX_ITEM_CAPACITY
                          : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "vbox item capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*builder->node_identifiers)) {
        return set_error(error, error_capacity,
                         "vbox item allocation overflow");
    }
    void *allocation = realloc(
        builder->node_identifiers,
        capacity * sizeof(*builder->node_identifiers));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "vbox item allocation failed");
    }
    builder->node_identifiers = allocation;
    builder->capacity = capacity;
    return 0;
}

static int store_node(struct hstex_engine *engine,
                      const struct hstex_node *node, uint32_t *identifier,
                      char *error, size_t error_capacity)
{
    if (node == NULL || identifier == NULL ||
        engine->node_count >= (size_t)UINT32_MAX ||
        reserve_nodes(engine, engine->node_count + 1U, error,
                      error_capacity) != 0) {
        return node == NULL || identifier == NULL
                   ? set_error(error, error_capacity,
                               "invalid typesetting node")
                   : -1;
    }
    engine->nodes[engine->node_count] = *node;
    *identifier = (uint32_t)engine->node_count + 1U;
    ++engine->node_count;
    return 0;
}

/* A running dimension contributes nothing while a box is measured; the
   enclosing box supplies its value when the page is shipped. */
static int append_current_list_node(struct hstex_engine *engine,
                                    const struct hstex_node *node,
                                    char *error, size_t error_capacity);
static int scan_rule_dimensions(struct hstex_engine *engine,
                                struct hstex_node *rule, char *error,
                                size_t error_capacity);

static int append_hbox_item(struct hstex_engine *engine, uint32_t identifier,
                            char *error, size_t error_capacity);
static int scan_vsplit(struct hstex_engine *engine, struct hstex_box *box,
                       char *error, size_t error_capacity);
static int scan_last_box(struct hstex_engine *engine, struct hstex_box *box,
                         char *error, size_t error_capacity);
static int drop_last_list_node(struct hstex_engine *engine, char *error,
                               size_t error_capacity);
static int append_character_node(struct hstex_engine *engine, uint8_t code,
                                 bool ligature, char *error,
                                 size_t error_capacity);
static void advance_space_factor(struct hstex_engine *engine, uint8_t code);
static int flush_pending_character(struct hstex_engine *engine, char *error,
                                   size_t error_capacity);
static int start_paragraph(struct hstex_engine *engine, bool indent,
                           char *error, size_t error_capacity);
static int ensure_horizontal_mode(struct hstex_engine *engine, char *error,
                                  size_t error_capacity);
static int finish_paragraph(struct hstex_engine *engine, char *error,
                            size_t error_capacity);
static int append_horizontal_character(struct hstex_engine *engine,
                                       uint8_t code, char *error,
                                       size_t error_capacity);
static int append_interword_glue(struct hstex_engine *engine, char *error,
                                 size_t error_capacity);
static int font_lig_kern(const struct hstex_font *font, uint8_t left,
                         uint8_t right, bool *kerned, int32_t *kern,
                         bool *ligatured, uint8_t *ligature, char *error,
                         size_t error_capacity);
static int append_vbox_item(struct hstex_engine *engine, uint32_t identifier,
                            char *error, size_t error_capacity);
static int math_append_node(struct hstex_engine *engine,
                            const struct hstex_node *node, char *error,
                            size_t error_capacity);
static int math_append_box(struct hstex_engine *engine,
                           const struct hstex_box *box, char *error,
                           size_t error_capacity);
static int math_append_atom(struct hstex_engine *engine,
                            struct hstex_noad *noad, char *error,
                            size_t error_capacity);
static int math_append_box_field(struct hstex_engine *engine,
                                 const struct hstex_box *box,
                                 bool single_character, uint32_t sublist,
                                 uint8_t list_style, char *error,
                                 size_t error_capacity);
static void pop_math_list(struct hstex_engine *engine);

static int32_t packed_dimen(int32_t value)
{
    return value == HSTEX_RUNNING_DIMEN ? 0 : value;
}

/* Half a length, rounded the way the reference rounds it: an odd value goes
   up, which is not the same as truncating (value + 1) / 2 once the value is
   negative and even. */
static int32_t half_of(int64_t value)
{
    return (int32_t)(value % 2 != 0 ? (value + 1) / 2 : value / 2);
}

/* How bad it is to stretch or shrink `available` by `needed`. The reference
   computes an integer approximation of a hundred times the cube of the ratio,
   which is not the same as rounding that cube: see docs/DECISIONS.md,
   badness. */
static int32_t glue_badness(int32_t needed, int32_t available)
{
    if (needed == 0) {
        return 0;
    }
    if (available <= 0) {
        return HSTEX_INFINITE_BADNESS;
    }
    int64_t ratio;
    if (needed <= INT64_C(7230584)) {
        ratio = ((int64_t)needed * 297) / available;
    } else if (available >= INT64_C(1663497)) {
        ratio = (int64_t)needed / ((int64_t)available / 297);
    } else {
        ratio = needed;
    }
    if (ratio > 1290) {
        return HSTEX_INFINITE_BADNESS;
    }
    return (int32_t)((ratio * ratio * ratio + INT64_C(131072)) /
                     INT64_C(262144));
}

/* The glue a list can give: the totals at the highest order present, which
   is all the packing needs. */
static struct hstex_glue list_total_glue(const struct hstex_engine *engine,
                                         const uint32_t *identifiers,
                                         size_t count)
{
    int64_t stretch[4] = {0, 0, 0, 0};
    int64_t shrink[4] = {0, 0, 0, 0};
    for (size_t index = 0U; index < count; ++index) {
        uint32_t identifier = identifiers[index];
        if (identifier == 0U || (size_t)identifier > engine->node_count) {
            continue;
        }
        const struct hstex_node *node = &engine->nodes[identifier - 1U];
        if (node->kind != HSTEX_NODE_GLUE) {
            continue;
        }
        uint8_t up = node->value.glue.stretch_order;
        uint8_t down = node->value.glue.shrink_order;
        if (up < 4U) {
            stretch[up] += node->value.glue.stretch;
        }
        if (down < 4U) {
            shrink[down] += node->value.glue.shrink;
        }
    }
    struct hstex_glue total = {0};
    for (int order = 3; order >= 0; --order) {
        if (stretch[order] != 0) {
            total.stretch = (int32_t)stretch[order];
            total.stretch_order = (uint8_t)order;
            break;
        }
    }
    for (int order = 3; order >= 0; --order) {
        if (shrink[order] != 0) {
            total.shrink = (int32_t)shrink[order];
            total.shrink_order = (uint8_t)order;
            break;
        }
    }
    return total;
}

/* What a box that has just been packed to a width reports through \badness:
   nothing if it fits, the badness of the glue it had to move, or a million
   if it could not be shrunk far enough. */
static int32_t packing_badness(int64_t natural, int64_t target,
                               const struct hstex_glue *total)
{
    if (target == natural) {
        return 0;
    }
    if (target > natural) {
        if (total->stretch_order != 0U) {
            return 0;
        }
        return glue_badness((int32_t)(target - natural), total->stretch);
    }
    if (total->shrink_order != 0U) {
        return 0;
    }
    if (natural - target > total->shrink) {
        return HSTEX_OVERFULL_BADNESS;
    }
    return glue_badness((int32_t)(natural - target), total->shrink);
}

static int append_hbox_node(struct hstex_engine *engine,
                            const struct hstex_node *node, char *error,
                            size_t error_capacity)
{
    struct hstex_hbox_builder *builder = engine->active_hbox_builder;
    if (builder == NULL || node == NULL) {
        uint32_t line = 0U;
        const char *origin = current_source_line(engine, &line);
        return set_error(error, error_capacity,
                         "horizontal node used outside an hbox, for %s at "
                         "%s:%u",
                         engine->executing_name, origin, (unsigned int)line);
    }
    /* A horizontal list that has not been packed yet has no width of its
       own -- a paragraph runs to whatever length it runs to, and only the
       lines it is broken into have to fit a dimension. See
       docs/DECISIONS.md, the-main-vertical-list. */
    int64_t width = builder->width + packed_dimen(node->width);
    if (width < INT64_MIN / 2 || width > INT64_MAX / 2) {
        return set_error(error, error_capacity,
                         "horizontal list width overflow");
    }
    uint32_t identifier = 0U;
    if (reserve_hbox_items(builder, builder->count + 1U, error,
                           error_capacity) != 0 ||
        store_node(engine, node, &identifier, error, error_capacity) != 0) {
        return -1;
    }
    return append_hbox_item(engine, identifier, error, error_capacity);
}

/* Glue that \leaders filled reaches as far across as the box it repeats,
   whichever way the list runs. See docs/DECISIONS.md, leaders. */
static const struct hstex_node *leader_box_of(const struct hstex_engine *engine,
                                              const struct hstex_node *node)
{
    if (node->kind != HSTEX_NODE_GLUE || node->value.glue.leader == 0U ||
        (size_t)node->value.glue.leader > engine->node_count) {
        return NULL;
    }
    return &engine->nodes[node->value.glue.leader - 1U];
}

/* Take a node already in the arena into the horizontal list being built.
   Unboxing splices identifiers this way rather than copying nodes. */
static int append_hbox_item(struct hstex_engine *engine, uint32_t identifier,
                            char *error, size_t error_capacity)
{
    struct hstex_hbox_builder *builder = engine->active_hbox_builder;
    if (builder == NULL || identifier == 0U ||
        (size_t)identifier > engine->node_count) {
        return set_error(error, error_capacity,
                         "horizontal node used outside an hbox");
    }
    const struct hstex_node *node = &engine->nodes[identifier - 1U];
    /* A horizontal list that has not been packed yet has no width of its
       own -- a paragraph runs to whatever length it runs to, and only the
       lines it is broken into have to fit a dimension. See
       docs/DECISIONS.md, the-main-vertical-list. */
    int64_t width = builder->width + packed_dimen(node->width);
    if (width < INT64_MIN / 2 || width > INT64_MAX / 2) {
        return set_error(error, error_capacity,
                         "horizontal list width overflow");
    }
    if (reserve_hbox_items(builder, builder->count + 1U, error,
                           error_capacity) != 0) {
        return -1;
    }
    builder->node_identifiers[builder->count++] = identifier;
    builder->width = width;
    /* A box shifted down reaches lower and rises less. */
    int32_t raised = packed_dimen(node->height) - node->shift;
    int32_t dropped = packed_dimen(node->depth) + node->shift;
    const struct hstex_node *leader = leader_box_of(engine, node);
    if (leader != NULL) {
        raised = packed_dimen(leader->height);
        dropped = packed_dimen(leader->depth);
    }
    if (raised > builder->height) {
        builder->height = raised;
    }
    if (dropped > builder->depth) {
        builder->depth = dropped;
    }
    return 0;
}

#define HSTEX_IGNORE_DEPTH (-INT32_C(1000) * INT32_C(65536))

static int append_vbox_node(struct hstex_engine *engine,
                            const struct hstex_node *node, char *error,
                            size_t error_capacity);

static int normal_paragraph(struct hstex_engine *engine, char *error,
                            size_t error_capacity);

/* Boxes in a vertical list are separated so that their baselines sit
   \baselineskip apart. When that would bring them closer than
   \lineskiplimit, \lineskip is used instead. The first box on a list gets no
   such glue, which \prevdepth records by staying at -1000pt. */
static int append_interline_glue(struct hstex_engine *engine,
                                 int32_t following_height, char *error,
                                 size_t error_capacity)
{
    if (engine->prev_depth <= HSTEX_IGNORE_DEPTH) {
        return 0;
    }
    struct hstex_glue baseline =
        engine->glue_parameters[HSTEX_GLUE_BASELINE_SKIP];
    int64_t separation = (int64_t)baseline.width -
                         (int64_t)engine->prev_depth -
                         (int64_t)following_height;
    struct hstex_glue glue;
    if (separation <
        (int64_t)engine->dimen_parameters[HSTEX_DIMEN_LINE_SKIP_LIMIT]) {
        glue = engine->glue_parameters[HSTEX_GLUE_LINE_SKIP];
    } else {
        if (separation < -INT64_C(1073741823) ||
            separation > INT64_C(1073741823)) {
            return set_error(error, error_capacity,
                             "interline glue exceeds TeX's dimension range");
        }
        glue = baseline;
        glue.width = (int32_t)separation;
    }
    struct hstex_node node = {
        .kind = HSTEX_NODE_GLUE,
        .width = glue.width,
        .height = 0,
        .depth = 0,
        .shift = 0,
        .value.glue = {
            .stretch = glue.stretch,
            .shrink = glue.shrink,
            .stretch_order = glue.stretch_order,
            .shrink_order = glue.shrink_order,
        },
    };
    return append_vbox_node(engine, &node, error, error_capacity);
}

static int append_vbox_node(struct hstex_engine *engine,
                            const struct hstex_node *node, char *error,
                            size_t error_capacity)
{
    struct hstex_vbox_builder *builder = engine->active_vbox_builder;
    if (builder == NULL || node == NULL) {
        return set_error(error, error_capacity,
                         "vertical node used outside a vbox or page");
    }
    if (node->kind == HSTEX_NODE_LIST &&
        append_interline_glue(engine, packed_dimen(node->height), error,
                              error_capacity) != 0) {
        return -1;
    }
    builder = engine->active_vbox_builder;
    int64_t extent = builder->extent;
    int32_t trailing_depth = builder->trailing_depth;
    if (node->kind == HSTEX_NODE_GLUE || node->kind == HSTEX_NODE_KERN) {
        extent += (int64_t)trailing_depth + packed_dimen(node->width);
        trailing_depth = 0;
    } else if (node->kind == HSTEX_NODE_RULE ||
               node->kind == HSTEX_NODE_CHARACTER ||
               node->kind == HSTEX_NODE_LIST) {
        extent += (int64_t)trailing_depth + packed_dimen(node->height);
        trailing_depth = packed_dimen(node->depth);
    }
    if (extent < INT64_MIN / 2 || extent > INT64_MAX / 2) {
        return set_error(error, error_capacity, "vertical extent overflow");
    }
    uint32_t identifier = 0U;
    if (reserve_vbox_items(builder, builder->count + 1U, error,
                           error_capacity) != 0 ||
        store_node(engine, node, &identifier, error, error_capacity) != 0) {
        return -1;
    }
    builder->node_identifiers[builder->count++] = identifier;
    builder->extent = extent;
    builder->trailing_depth = trailing_depth;
    /* A box sets the reference for the next one; a rule suppresses it. */
    if (node->kind == HSTEX_NODE_LIST) {
        engine->prev_depth = packed_dimen(node->depth);
    } else if (node->kind == HSTEX_NODE_RULE) {
        engine->prev_depth = HSTEX_IGNORE_DEPTH;
    }
    int32_t reach = packed_dimen(node->width) + node->shift;
    if ((node->kind == HSTEX_NODE_RULE || node->kind == HSTEX_NODE_LIST) &&
        reach > builder->width) {
        builder->width = reach;
    }
    const struct hstex_node *leader = leader_box_of(engine, node);
    if (leader != NULL && packed_dimen(leader->width) > builder->width) {
        builder->width = packed_dimen(leader->width);
    }
    return 0;
}

/* Take a node already in the arena into the vertical list being built. No
   interline glue is inserted: an unboxed list carries its own. */
static int append_vbox_item(struct hstex_engine *engine, uint32_t identifier,
                            char *error, size_t error_capacity)
{
    struct hstex_vbox_builder *builder = engine->active_vbox_builder;
    if (builder == NULL || identifier == 0U ||
        (size_t)identifier > engine->node_count) {
        return set_error(error, error_capacity,
                         "vertical node used outside a vbox or page");
    }
    const struct hstex_node *node = &engine->nodes[identifier - 1U];
    int64_t extent = builder->extent;
    int32_t trailing_depth = builder->trailing_depth;
    if (node->kind == HSTEX_NODE_GLUE || node->kind == HSTEX_NODE_KERN) {
        extent += (int64_t)trailing_depth + packed_dimen(node->width);
        trailing_depth = 0;
    } else if (node->kind == HSTEX_NODE_RULE ||
               node->kind == HSTEX_NODE_CHARACTER ||
               node->kind == HSTEX_NODE_LIST) {
        extent += (int64_t)trailing_depth + packed_dimen(node->height);
        trailing_depth = packed_dimen(node->depth);
    }
    if (extent < INT64_MIN / 2 || extent > INT64_MAX / 2) {
        return set_error(error, error_capacity, "vertical extent overflow");
    }
    if (reserve_vbox_items(builder, builder->count + 1U, error,
                           error_capacity) != 0) {
        return -1;
    }
    builder->node_identifiers[builder->count++] = identifier;
    builder->extent = extent;
    builder->trailing_depth = trailing_depth;
    /* A box sets the reference for the next one; a rule suppresses it. */
    if (node->kind == HSTEX_NODE_LIST) {
        engine->prev_depth = packed_dimen(node->depth);
    } else if (node->kind == HSTEX_NODE_RULE) {
        engine->prev_depth = HSTEX_IGNORE_DEPTH;
    }
    /* A box shifted right widens the list by the displacement. */
    int32_t reach = packed_dimen(node->width) + node->shift;
    if ((node->kind == HSTEX_NODE_RULE || node->kind == HSTEX_NODE_LIST) &&
        reach > builder->width) {
        builder->width = reach;
    }
    const struct hstex_node *leader = leader_box_of(engine, node);
    if (leader != NULL && packed_dimen(leader->width) > builder->width) {
        builder->width = packed_dimen(leader->width);
    }
    return 0;
}

/* A rule keeps repeating width, height, and depth specifications, the last
   of each winning; whatever is not given stays as the rule's default. */
static int scan_rule_dimensions(struct hstex_engine *engine,
                                struct hstex_node *rule, char *error,
                                size_t error_capacity)
{
    for (;;) {
        bool matched = false;
        if (try_keyword(engine, "width", &matched, error, error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            if (scan_dimension(engine, &rule->width, error, error_capacity) !=
                0) {
                return -1;
            }
            continue;
        }
        if (try_keyword(engine, "height", &matched, error, error_capacity) !=
            0) {
            return -1;
        }
        if (matched) {
            if (scan_dimension(engine, &rule->height, error, error_capacity) !=
                0) {
                return -1;
            }
            continue;
        }
        if (try_keyword(engine, "depth", &matched, error, error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            if (scan_dimension(engine, &rule->depth, error, error_capacity) !=
                0) {
                return -1;
            }
            continue;
        }
        return 0;
    }
}

static int execute_vrule(struct hstex_engine *engine, char *error,
                         size_t error_capacity)
{
    struct hstex_node rule = {
        .kind = HSTEX_NODE_RULE,
        .width = 26214,
        .height = HSTEX_RUNNING_DIMEN,
        .depth = HSTEX_RUNNING_DIMEN,
        .value.penalty = 0,
    };
    if (scan_rule_dimensions(engine, &rule, error, error_capacity) != 0) {
        return -1;
    }
    return append_current_list_node(engine, &rule, error, error_capacity);
}

/* \hrule spans the width of the box that encloses it, and ends a paragraph
   in the same way \par does not concern us yet. Its depth is an ordinary
   zero, only the width is running. */
static int execute_hrule(struct hstex_engine *engine, char *error,
                         size_t error_capacity)
{
    if (engine->mode != HSTEX_MODE_VERTICAL || engine->pending_global ||
        engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "hrule requires unprefixed vertical mode");
    }
    struct hstex_node rule = {
        .kind = HSTEX_NODE_RULE,
        .width = HSTEX_RUNNING_DIMEN,
        .height = 26214,
        .depth = 0,
        .value.penalty = 0,
    };
    if (scan_rule_dimensions(engine, &rule, error, error_capacity) != 0) {
        return -1;
    }
    return append_vbox_node(engine, &rule, error, error_capacity);
}

/* \kern is rigid: it measures like glue but cannot stretch or shrink, and it
   leaves \prevdepth alone, so a box after a kern still gets interline glue. */
static int execute_kern(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "kern does not accept prefixes");
    }
    int32_t amount = 0;
    if (scan_dimension(engine, &amount, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_node kern = {
        .kind = HSTEX_NODE_KERN,
        .explicit_kern = true,
        .width = amount,
        .height = 0,
        .depth = 0,
        .value.penalty = 0,
    };
    return append_current_list_node(engine, &kern, error, error_capacity);
}

static int append_current_list_node(struct hstex_engine *engine,
                                    const struct hstex_node *node,
                                    char *error, size_t error_capacity)
{
    if (engine->mode == HSTEX_MODE_HORIZONTAL) {
        return append_hbox_node(engine, node, error, error_capacity);
    }
    if (engine->mode == HSTEX_MODE_VERTICAL) {
        return append_vbox_node(engine, node, error, error_capacity);
    }
    return math_append_node(engine, node, error, error_capacity);
}

/* The five glue commands are the same in both directions; only the list they
   join differs. */
static int execute_glue(struct hstex_engine *engine, int32_t subtype,
                        bool vertical, char *error, size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "glue does not accept prefixes");
    }
    if (!vertical && engine->mode != HSTEX_MODE_MATH &&
        ensure_horizontal_mode(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (vertical ? engine->mode != HSTEX_MODE_VERTICAL
                 : engine->mode == HSTEX_MODE_VERTICAL) {
        return set_error(error, error_capacity,
                         vertical ? "vertical glue requires vertical mode"
                                  : "horizontal glue requires horizontal mode");
    }
    struct hstex_glue glue = {0};
    if (subtype == 0) {
        if (scan_glue(engine, &glue, error, error_capacity) != 0) {
            return -1;
        }
    } else if (subtype == 1) {
        glue.stretch = INT32_C(65536);
        glue.stretch_order = 1U;
    } else if (subtype == 2) {
        glue.stretch = INT32_C(65536);
        glue.stretch_order = 2U;
    } else if (subtype == 3) {
        glue.stretch = INT32_C(65536);
        glue.shrink = INT32_C(65536);
        glue.stretch_order = 1U;
        glue.shrink_order = 1U;
    } else if (subtype == 4) {
        glue.stretch = -INT32_C(65536);
        glue.stretch_order = 1U;
    } else {
        return set_error(error, error_capacity, "invalid glue subtype");
    }
    struct hstex_node node = {
        .kind = HSTEX_NODE_GLUE,
        .width = glue.width,
        .height = 0,
        .depth = 0,
        .value.glue = {
            .stretch = glue.stretch,
            .shrink = glue.shrink,
            .stretch_order = glue.stretch_order,
            .shrink_order = glue.shrink_order,
            .leader = engine->pending_leader,
            .leader_kind = engine->pending_leader_kind,
        },
    };
    engine->pending_leader = 0U;
    engine->pending_leader_kind = 0U;
    return append_current_list_node(engine, &node, error, error_capacity);
}

static int scan_box_operand(struct hstex_engine *engine, struct hstex_box *box,
                            char *error, size_t error_capacity);

static int store_box_node(struct hstex_engine *engine,
                          const struct hstex_box *box, int32_t shift,
                          uint32_t *identifier, char *error,
                          size_t error_capacity);

/* \leaders, \cleaders and \xleaders read a box or a rule and then the glue
   it is to fill. See docs/DECISIONS.md, leaders. */
static int execute_leaders(struct hstex_engine *engine, int32_t kind,
                           char *error, size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "leaders do not accept prefixes");
    }
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result =
        expanded_next_non_space(engine, &token, &location, error,
                                error_capacity);
    if (result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    const struct hstex_meaning *meaning =
        result == HSTEX_ENGINE_TOKEN && hstex_token_is_control_sequence(token)
            ? hstex_engine_meaning(engine,
                                   hstex_token_control_sequence_id(token))
            : NULL;
    uint32_t identifier = 0U;
    if (meaning != NULL && (meaning->command == HSTEX_COMMAND_VRULE ||
                            meaning->command == HSTEX_COMMAND_HRULE)) {
        bool vertical_rule = meaning->command == HSTEX_COMMAND_VRULE;
        struct hstex_node rule = {
            .kind = HSTEX_NODE_RULE,
            .width = vertical_rule ? 26214 : HSTEX_RUNNING_DIMEN,
            .height = vertical_rule ? HSTEX_RUNNING_DIMEN : 26214,
            .depth = vertical_rule ? HSTEX_RUNNING_DIMEN : 0,
        };
        if (scan_rule_dimensions(engine, &rule, error, error_capacity) != 0 ||
            store_node(engine, &rule, &identifier, error, error_capacity) !=
                0) {
            return -1;
        }
    } else {
        if (result == HSTEX_ENGINE_TOKEN &&
            push_one(engine, token, location, error, error_capacity) != 0) {
            return -1;
        }
        struct hstex_box box = {0};
        if (scan_box_operand(engine, &box, error, error_capacity) != 0 ||
            store_box_node(engine, &box, 0, &identifier, error,
                           error_capacity) != 0) {
            return -1;
        }
    }
    engine->pending_leader = identifier;
    engine->pending_leader_kind = (uint8_t)kind;

    /* The glue must come next: it is what the box is repeated across. */
    hstex_token glue_token = 0U;
    struct hstex_source_location where;
    result = expanded_next_non_space(engine, &glue_token, &where, error,
                                     error_capacity);
    const struct hstex_meaning *glue_meaning =
        result == HSTEX_ENGINE_TOKEN &&
                hstex_token_is_control_sequence(glue_token)
            ? hstex_engine_meaning(
                  engine, hstex_token_control_sequence_id(glue_token))
            : NULL;
    if (glue_meaning == NULL ||
        (glue_meaning->command != HSTEX_COMMAND_HSKIP &&
         glue_meaning->command != HSTEX_COMMAND_VSKIP)) {
        engine->pending_leader = 0U;
        engine->pending_leader_kind = 0U;
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        char found[128];
        describe_token(engine, result == HSTEX_ENGINE_TOKEN ? glue_token : 0U,
                       found, sizeof(found));
        return set_error(error, error_capacity,
                         "leaders must be followed by glue, found %s", found);
    }
    int status = execute_glue(
        engine, glue_meaning->value.integer,
        glue_meaning->command == HSTEX_COMMAND_VSKIP, error, error_capacity);
    engine->pending_leader = 0U;
    engine->pending_leader_kind = 0U;
    return status;
}

static int execute_penalty(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "penalty does not accept definition prefixes");
    }
    int32_t penalty = 0;
    if (scan_integer(engine, &penalty, error, error_capacity) != 0) {
        return -1;
    }
    if (penalty > 10000) {
        penalty = 10000;
    } else if (penalty < -10000) {
        penalty = -10000;
    }
    struct hstex_node node = {
        .kind = HSTEX_NODE_PENALTY,
        .width = 0,
        .height = 0,
        .depth = 0,
        .value.penalty = penalty,
    };
    return append_current_list_node(engine, &node, error, error_capacity);
}

/* The body runs on the live input, the way the reference executes it, so a
   box may be opened by one macro and closed by an \egroup another produces.
   See docs/DECISIONS.md, streaming-box-bodies. */
static int evaluate_hbox_contents(struct hstex_engine *engine,
                                  struct hstex_hbox_builder *builder,
                                  char *error, size_t error_capacity)
{
    uint32_t base_group_level = engine->group_level;
    uint32_t previous_group_floor = engine->output_group_floor;
    size_t previous_conditional_floor = engine->output_conditional_floor;
    struct hstex_hbox_builder *previous_builder =
        engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    bool previous_inner_mode = engine->inner_mode;
    uint32_t previous_stop_level = engine->group_stop_level;
    bool previous_stop_armed = engine->group_stop_armed;
    bool previous_stop_hit = engine->group_stop_hit;
    /* A box body is a fresh place for formulas: a $ inside it opens and
       closes its own math list, not the one the box interrupted. */
    size_t previous_math_depth = engine->math_depth;
    size_t previous_math_floor = engine->math_floor;
    bool previous_displayed = engine->displayed_math;
    /* A body is not part of whatever paragraph encloses it; it gets a list
       of its own if it starts one. */
    bool previous_building_paragraph = engine->building_paragraph;
    struct hstex_hbox_builder *previous_paragraph = engine->paragraph_builder;
    if (begin_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    engine->output_group_floor = engine->group_level;
    engine->output_conditional_floor = engine->conditional_count;
    engine->group_stop_level = base_group_level;
    engine->group_stop_armed = true;
    engine->group_stop_hit = false;
    engine->math_floor = engine->math_depth;
    engine->displayed_math = false;
    engine->building_paragraph = false;
    engine->paragraph_builder = NULL;
    engine->active_hbox_builder = builder;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    engine->inner_mode = true;

    int32_t previous_space_factor = engine->space_factor;
    bool previous_has_pending = engine->has_pending_character;
    engine->space_factor = 1000;
    engine->has_pending_character = false;
    int status = 0;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            status = -1;
            break;
        }
        if (token_is_space(token)) {
            status = flush_pending_character(engine, error, error_capacity);
            if (status == 0) {
                status = append_interword_glue(engine, error, error_capacity);
            }
            if (status != 0) {
                break;
            }
            engine->space_factor = 1000;
            continue;
        }
        if (!hstex_token_is_character(token)) {
            char found[128];
            describe_token(engine, token, found, sizeof(found));
            uint32_t line = 0U;
            const char *origin = current_source_line(engine, &line);
            status = set_error(error, error_capacity,
                               "unsupported horizontal-mode token inside hbox: "
                               "%s at %s:%u",
                               found, origin, (unsigned int)line);
            break;
        }
        status = append_horizontal_character(
            engine, hstex_token_character_code(token), error, error_capacity);
        if (status != 0) {
            break;
        }
    }
    if (status == 0) {
        status = flush_pending_character(engine, error, error_capacity);
    }
    if (status == 0 && !engine->group_stop_hit) {
        status = set_error(error, error_capacity,
                           "input ended inside an hbox");
    }
    engine->space_factor = previous_space_factor;
    engine->has_pending_character = previous_has_pending;

    engine->active_hbox_builder = previous_builder;
    engine->mode = previous_mode;
    engine->inner_mode = previous_inner_mode;
    engine->output_group_floor = previous_group_floor;
    engine->output_conditional_floor = previous_conditional_floor;
    engine->group_stop_level = previous_stop_level;
    engine->group_stop_armed = previous_stop_armed;
    engine->group_stop_hit = previous_stop_hit;
    while (engine->math_depth > previous_math_depth) {
        pop_math_list(engine);
    }
    engine->math_depth = previous_math_depth;
    engine->math_floor = previous_math_floor;
    engine->displayed_math = previous_displayed;
    if (engine->paragraph_builder != NULL &&
        engine->paragraph_builder != previous_paragraph) {
        free(engine->paragraph_builder->node_identifiers);
        free(engine->paragraph_builder);
    }
    engine->paragraph_builder = previous_paragraph;
    engine->building_paragraph = previous_building_paragraph;
    while (engine->group_level > base_group_level) {
        if (end_group(engine, error, error_capacity) != 0) {
            status = -1;
            break;
        }
    }
    if (engine->group_level != base_group_level) {
        status = set_error(error, error_capacity,
                           "hbox content closed an outer group");
    }
    return status;
}

static int finalize_hbox(struct hstex_engine *engine,
                         struct hstex_hbox_builder *builder,
                         bool matched_to, bool matched_spread,
                         int32_t requested_width, struct hstex_box *box,
                         char *error, size_t error_capacity)
{
    if (builder->count > (size_t)UINT32_MAX ||
        engine->list_item_count > (size_t)UINT32_MAX - builder->count ||
        reserve_list_items(engine, engine->list_item_count + builder->count,
                           error, error_capacity) != 0) {
        return -1;
    }
    /* The reference sums the natural width into a scaled and lets it wrap;
       a box wider than a dimension is not an error there, so it is not one
       here. See docs/DECISIONS.md, oversize-boxes. */
    memset(box, 0, sizeof(*box));
    box->kind = HSTEX_BOX_HLIST;
    box->width = (int32_t)(uint32_t)(uint64_t)builder->width;
    box->height = builder->height;
    box->depth = builder->depth;
    if (matched_to) {
        box->width = requested_width;
    } else if (matched_spread) {
        int64_t spread_width = builder->width + (int64_t)requested_width;
        box->width = (int32_t)(uint32_t)(uint64_t)spread_width;
    }
    struct hstex_glue total =
        list_total_glue(engine, builder->node_identifiers, builder->count);
    engine->badness = packing_badness(builder->width, box->width, &total);
    if (builder->count != 0U) {
        box->node_start = (uint32_t)engine->list_item_count;
        box->node_count = (uint32_t)builder->count;
        memcpy(engine->list_items + engine->list_item_count,
               builder->node_identifiers,
               builder->count * sizeof(*engine->list_items));
        engine->list_item_count += builder->count;
    }
    return 0;
}

static int scan_hbox(struct hstex_engine *engine, struct hstex_box *box,
                     char *error, size_t error_capacity)
{
    bool matched_to = false;
    bool matched_spread = false;
    int32_t requested_width = 0;
    if (try_keyword(engine, "to", &matched_to, error, error_capacity) != 0) {
        return -1;
    }
    if (matched_to) {
        if (scan_dimension(engine, &requested_width, error, error_capacity) !=
            0) {
            return -1;
        }
    } else {
        if (try_keyword(engine, "spread", &matched_spread, error,
                        error_capacity) != 0) {
            return -1;
        }
        if (matched_spread &&
            scan_dimension(engine, &requested_width, error, error_capacity) !=
                0) {
            return -1;
        }
    }

    hstex_token opening = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result = expanded_next_non_space_unrestricted(
        engine, &opening, &location, error, error_capacity);
    if (result != HSTEX_ENGINE_TOKEN ||
        !token_is_effective_begin_group(engine, opening)) {
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        char found[128];
        describe_token(engine, result == HSTEX_ENGINE_TOKEN ? opening : 0U,
                       found, sizeof(found));
        uint32_t line = 0U;
        const char *origin = current_source_line(engine, &line);
        return set_error(error, error_capacity,
                         "hbox requires a braced token list, found %s at %s:%u",
                         found, origin, (unsigned int)line);
    }
    struct hstex_hbox_builder builder = {0};
    int status = evaluate_hbox_contents(engine, &builder, error,
                                        error_capacity);
    if (status == 0) {
        status = finalize_hbox(engine, &builder, matched_to, matched_spread,
                              requested_width, box, error, error_capacity);
    }
    free(builder.node_identifiers);
    return status;
}

/* One token of a vertical list, wherever that list is: the page, a \vbox, or
   a \noalign. A character starts a paragraph, and once one is being built
   its characters and spaces belong to it. See docs/DECISIONS.md,
   the-main-vertical-list. */
static int handle_vertical_list_token(struct hstex_engine *engine,
                                      hstex_token token,
                                      struct hstex_source_location location,
                                      char *error, size_t error_capacity)
{
    if (engine->building_paragraph) {
        if (token_is_space(token)) {
            if (flush_pending_character(engine, error, error_capacity) != 0 ||
                append_interword_glue(engine, error, error_capacity) != 0) {
                return -1;
            }
            engine->space_factor = 1000;
            return 0;
        }
        if (hstex_token_is_character(token)) {
            return append_horizontal_character(
                engine, hstex_token_character_code(token), error,
                error_capacity);
        }
    }
    if (token_is_space(token)) {
        return 0;
    }
    if (hstex_token_is_control_sequence(token) &&
        hstex_engine_meaning(engine, hstex_token_control_sequence_id(token))
                ->command == HSTEX_COMMAND_PAR) {
        return 0;
    }
    if (hstex_token_is_character(token)) {
        /* A character in a vertical list starts a paragraph, and is read
           again once \everypar has run. */
        if (push_one(engine, token, location, error, error_capacity) != 0) {
            return -1;
        }
        return start_paragraph(engine, true, error, error_capacity);
    }
    char found[128];
    describe_token(engine, token, found, sizeof(found));
    return set_error(error, error_capacity,
                     "%s is not supported in a vertical list", found);
}

static int evaluate_vbox_contents(struct hstex_engine *engine,
                                  struct hstex_vbox_builder *builder,
                                  char *error, size_t error_capacity)
{
    uint32_t base_group_level = engine->group_level;
    uint32_t previous_group_floor = engine->output_group_floor;
    size_t previous_conditional_floor = engine->output_conditional_floor;
    struct hstex_hbox_builder *previous_hbox_builder =
        engine->active_hbox_builder;
    struct hstex_vbox_builder *previous_vbox_builder =
        engine->active_vbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    bool previous_inner_mode = engine->inner_mode;
    int32_t previous_depth = engine->prev_depth;
    uint32_t previous_stop_level = engine->group_stop_level;
    bool previous_stop_armed = engine->group_stop_armed;
    bool previous_stop_hit = engine->group_stop_hit;
    size_t previous_math_depth = engine->math_depth;
    size_t previous_math_floor = engine->math_floor;
    bool previous_displayed = engine->displayed_math;
    /* A body is not part of whatever paragraph encloses it; it gets a list
       of its own if it starts one. */
    bool previous_building_paragraph = engine->building_paragraph;
    struct hstex_hbox_builder *previous_paragraph = engine->paragraph_builder;
    if (begin_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    engine->output_group_floor = engine->group_level;
    engine->output_conditional_floor = engine->conditional_count;
    engine->group_stop_level = base_group_level;
    engine->group_stop_armed = true;
    engine->group_stop_hit = false;
    engine->math_floor = engine->math_depth;
    engine->displayed_math = false;
    engine->building_paragraph = false;
    engine->paragraph_builder = NULL;
    engine->active_hbox_builder = NULL;
    engine->active_vbox_builder = builder;
    engine->mode = HSTEX_MODE_VERTICAL;
    engine->inner_mode = true;
    engine->prev_depth = -INT32_C(1000) * INT32_C(65536);

    int status = normal_paragraph(engine, error, error_capacity);
    while (status == 0) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            status = -1;
            break;
        }
        status = handle_vertical_list_token(engine, token, location, error,
                                            error_capacity);
        if (status != 0) {
            break;
        }
    }

    /* A vertical list ends any paragraph it was still filling, the way an
       implied \par would. */
    if (status == 0 && engine->building_paragraph) {
        status = finish_paragraph(engine, error, error_capacity);
    }
    if (status == 0 && !engine->group_stop_hit) {
        status = set_error(error, error_capacity, "input ended inside a vbox");
    }
    engine->active_hbox_builder = previous_hbox_builder;
    engine->active_vbox_builder = previous_vbox_builder;
    engine->mode = previous_mode;
    engine->inner_mode = previous_inner_mode;
    engine->prev_depth = previous_depth;
    engine->output_group_floor = previous_group_floor;
    engine->output_conditional_floor = previous_conditional_floor;
    engine->group_stop_level = previous_stop_level;
    engine->group_stop_armed = previous_stop_armed;
    engine->group_stop_hit = previous_stop_hit;
    while (engine->math_depth > previous_math_depth) {
        pop_math_list(engine);
    }
    engine->math_depth = previous_math_depth;
    engine->math_floor = previous_math_floor;
    engine->displayed_math = previous_displayed;
    if (engine->paragraph_builder != NULL &&
        engine->paragraph_builder != previous_paragraph) {
        free(engine->paragraph_builder->node_identifiers);
        free(engine->paragraph_builder);
    }
    engine->paragraph_builder = previous_paragraph;
    engine->building_paragraph = previous_building_paragraph;
    while (engine->group_level > base_group_level) {
        if (end_group(engine, error, error_capacity) != 0) {
            status = -1;
            break;
        }
    }
    if (engine->group_level != base_group_level) {
        status = set_error(error, error_capacity,
                           "vbox content closed an outer group");
    }
    return status;
}

static int finalize_vbox(struct hstex_engine *engine,
                         struct hstex_vbox_builder *builder,
                         bool matched_to, bool matched_spread,
                         int32_t requested_height, struct hstex_box *box,
                         char *error, size_t error_capacity)
{
    if (builder->count > (size_t)UINT32_MAX ||
        engine->list_item_count > (size_t)UINT32_MAX - builder->count ||
        reserve_list_items(engine, engine->list_item_count + builder->count,
                           error, error_capacity) != 0) {
        return -1;
    }
    /* The trailing depth becomes the box's depth, but only up to
       \boxmaxdepth; whatever exceeds it counts as height instead. */
    int64_t height = builder->extent;
    int32_t depth = builder->trailing_depth;
    int32_t limit = engine->dimen_parameters[HSTEX_DIMEN_BOX_MAX_DEPTH];
    if (depth > limit) {
        height += (int64_t)depth - limit;
        depth = limit;
    }
    int64_t natural = height;
    if (matched_to) {
        height = requested_height;
    } else if (matched_spread) {
        height += requested_height;
    }
    struct hstex_glue total =
        list_total_glue(engine, builder->node_identifiers, builder->count);
    engine->badness = packing_badness(natural, height, &total);
    memset(box, 0, sizeof(*box));
    box->kind = HSTEX_BOX_VLIST;
    box->width = builder->width;
    box->height = (int32_t)(uint32_t)(uint64_t)height;
    box->depth = depth;
    if (builder->count != 0U) {
        box->node_start = (uint32_t)engine->list_item_count;
        box->node_count = (uint32_t)builder->count;
        memcpy(engine->list_items + engine->list_item_count,
               builder->node_identifiers,
               builder->count * sizeof(*engine->list_items));
        engine->list_item_count += builder->count;
    }
    return 0;
}

static int scan_vbox(struct hstex_engine *engine, bool top,
                     struct hstex_box *box, char *error, size_t error_capacity)
{
    bool matched_to = false;
    bool matched_spread = false;
    int32_t requested_height = 0;
    if (try_keyword(engine, "to", &matched_to, error, error_capacity) != 0) {
        return -1;
    }
    if (matched_to) {
        if (scan_dimension(engine, &requested_height, error, error_capacity) !=
            0) {
            return -1;
        }
    } else {
        if (try_keyword(engine, "spread", &matched_spread, error,
                        error_capacity) != 0) {
            return -1;
        }
        if (matched_spread &&
            scan_dimension(engine, &requested_height, error, error_capacity) !=
                0) {
            return -1;
        }
    }

    hstex_token opening = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result = expanded_next_non_space_unrestricted(
        engine, &opening, &location, error, error_capacity);
    if (result != HSTEX_ENGINE_TOKEN ||
        !token_is_effective_begin_group(engine, opening)) {
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        char found[128];
        describe_token(engine, result == HSTEX_ENGINE_TOKEN ? opening : 0U,
                       found, sizeof(found));
        uint32_t line = 0U;
        const char *origin = current_source_line(engine, &line);
        return set_error(error, error_capacity,
                         "vbox requires a braced token list, found %s at %s:%u",
                         found, origin, (unsigned int)line);
    }
    struct hstex_vbox_builder builder = {0};
    int status = evaluate_vbox_contents(engine, &builder, error,
                                        error_capacity);
    if (status == 0) {
        status = finalize_vbox(engine, &builder, matched_to, matched_spread,
                              requested_height, box, error, error_capacity);
        /* \vtop keeps only the first item's height; everything else becomes
           depth. See docs/DECISIONS.md, vtop-and-lastbox. */
        if (status == 0 && top) {
            int32_t total = box->height + box->depth;
            int32_t first = 0;
            if (box->node_count != 0U) {
                uint32_t identifier = engine->list_items[box->node_start];
                if (identifier != 0U &&
                    (size_t)identifier <= engine->node_count) {
                    const struct hstex_node *node =
                        &engine->nodes[identifier - 1U];
                    if (node->kind == HSTEX_NODE_LIST ||
                        node->kind == HSTEX_NODE_RULE) {
                        first = packed_dimen(node->height);
                    }
                }
            }
            box->height = first;
            box->depth = total - first;
        }
    }
    free(builder.node_identifiers);
    return status;
}

static int append_shifted_box_node(struct hstex_engine *engine,
                                   const struct hstex_box *box, int32_t shift,
                                   char *error, size_t error_capacity)
{
    struct hstex_node node = {
        .kind = HSTEX_NODE_LIST,
        .width = box->width,
        .height = box->height,
        .depth = box->depth,
        .shift = shift,
        .value.list = {
            .node_start = box->node_start,
            .node_count = box->node_count,
            .box_kind = box->kind,
        },
    };
    return append_current_list_node(engine, &node, error, error_capacity);
}

static int append_box_node(struct hstex_engine *engine,
                           const struct hstex_box *box, char *error,
                           size_t error_capacity)
{
    return append_shifted_box_node(engine, box, 0, error, error_capacity);
}

/* \vsplit takes the top of a vertical list, leaving the rest behind. A void
   register splits to nothing, which is the whole of what is implemented; see
   docs/DECISIONS.md, vtop-and-lastbox. */
/* Take the last node off the list being built. The builder's totals were
   accumulated forwards and include maxima the node may have set, so they are
   recomputed from what remains rather than undone. */
static int drop_last_list_node(struct hstex_engine *engine, char *error,
                               size_t error_capacity)
{
    if (engine->mode == HSTEX_MODE_HORIZONTAL) {
        struct hstex_hbox_builder *builder = engine->active_hbox_builder;
        uint32_t *identifiers = builder->node_identifiers;
        size_t count = builder->count - 1U;
        builder->count = 0U;
        builder->width = 0;
        builder->height = 0;
        builder->depth = 0;
        for (size_t index = 0U; index < count; ++index) {
            if (append_hbox_item(engine, identifiers[index], error,
                                 error_capacity) != 0) {
                return -1;
            }
        }
        return 0;
    }
    struct hstex_vbox_builder *builder = engine->active_vbox_builder;
    uint32_t *identifiers = builder->node_identifiers;
    size_t count = builder->count - 1U;
    builder->count = 0U;
    builder->extent = 0;
    builder->trailing_depth = 0;
    builder->width = 0;
    for (size_t index = 0U; index < count; ++index) {
        if (append_vbox_item(engine, identifiers[index], error,
                             error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}

/* \unskip, \unkern and \unpenalty each remove the last node if it is of
   their own kind, and do nothing otherwise. */
static int execute_remove_last(struct hstex_engine *engine, int32_t kind,
                               char *error, size_t error_capacity)
{
    if (flush_pending_character(engine, error, error_capacity) != 0) {
        return -1;
    }
    const struct hstex_node *node = current_list_last_node(engine);
    if (node == NULL || node->kind != (enum hstex_node_kind)kind) {
        return 0;
    }
    return drop_last_list_node(engine, error, error_capacity);
}

static int scan_vsplit(struct hstex_engine *engine, struct hstex_box *box,
                       char *error, size_t error_capacity)
{
    int32_t index = 0;
    int32_t height = 0;
    bool matched = false;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    if (try_keyword(engine, "to", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (!matched) {
        return set_error(error, error_capacity, "vsplit requires a height");
    }
    if (scan_dimension(engine, &height, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_box source = engine->boxes[(size_t)index];
    if (source.kind == HSTEX_BOX_VOID) {
        memset(box, 0, sizeof(*box));
        box->kind = HSTEX_BOX_VOID;
        return 0;
    }
    /* Asking for at least the whole list takes all of it: no break can leave
       less material and come nearer the goal. The result is packaged to the
       height asked for, however far off it is, and the register is emptied. */
    if (source.kind != HSTEX_BOX_VLIST ||
        (int64_t)source.height + source.depth > height) {
        return set_error(error, error_capacity,
                         "splitting box %d, a %s of height %d, is not "
                         "implemented",
                         index,
                         source.kind == HSTEX_BOX_HLIST ? "horizontal list"
                                                        : "vertical list",
                         source.height);
    }
    *box = source;
    box->height = height;
    box->depth = 0;
    struct hstex_box empty = {0};
    empty.kind = HSTEX_BOX_VOID;
    return assign_box(engine, (uint32_t)index, empty, true, error,
                      error_capacity);
}

/* \lastbox takes the last box off the list being built. The builder's totals
   are recomputed from what is left, since they were accumulated forwards. */
static int scan_last_box(struct hstex_engine *engine, struct hstex_box *box,
                         char *error, size_t error_capacity)
{
    memset(box, 0, sizeof(*box));
    box->kind = HSTEX_BOX_VOID;
    const struct hstex_node *node = current_list_last_node(engine);
    if (node == NULL || node->kind != HSTEX_NODE_LIST) {
        return 0;
    }
    box->kind = node->value.list.box_kind;
    box->width = node->width;
    box->height = node->height;
    box->depth = node->depth;
    box->node_start = node->value.list.node_start;
    box->node_count = node->value.list.node_count;

    return drop_last_list_node(engine, error, error_capacity);
}

/* TeX's <box>: an explicit \hbox or \vbox, or a register fetched with \box,
   which voids it, or \copy, which does not. Registers hold immutable node
   ranges, so a copy shares the range. */
static int scan_box_operand(struct hstex_engine *engine, struct hstex_box *box,
                            char *error, size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result =
        expanded_next_non_space(engine, &token, &location, error,
                                error_capacity);
    if (result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (result != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(token)) {
        char found[128];
        describe_token(engine, result == HSTEX_ENGINE_TOKEN ? token : 0U, found,
                       sizeof(found));
        return set_error(error, error_capacity, "a box was expected, found %s",
                         found);
    }
    const struct hstex_meaning *meaning =
        hstex_engine_meaning(engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_HBOX) {
        return scan_hbox(engine, box, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_VBOX) {
        return scan_vbox(engine, false, box, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_VTOP) {
        return scan_vbox(engine, true, box, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_VSPLIT) {
        return scan_vsplit(engine, box, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_LAST_BOX) {
        return scan_last_box(engine, box, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_BOX ||
        meaning->command == HSTEX_COMMAND_COPY) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
        *box = engine->boxes[(size_t)index];
        if (meaning->command == HSTEX_COMMAND_BOX) {
            struct hstex_box empty = {0};
            /* Emptying a register outlives the group it happened in. */
            empty.kind = HSTEX_BOX_VOID;
            if (assign_box(engine, (uint32_t)index, empty, true, error,
                           error_capacity) != 0) {
                return -1;
            }
        }
        return 0;
    }
    char found[128];
    describe_token(engine, token, found, sizeof(found));
    return set_error(error, error_capacity, "a box was expected, found %s",
                     found);
}

/* \raise and \lower displace a box in a horizontal list, \moveleft and
   \moveright in a vertical one. */
static int execute_shift_box(struct hstex_engine *engine, int32_t subtype,
                             char *error, size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "box displacement does not accept prefixes");
    }
    bool vertical = subtype == (int32_t)HSTEX_SHIFT_MOVE_LEFT ||
                    subtype == (int32_t)HSTEX_SHIFT_MOVE_RIGHT;
    if (vertical ? engine->mode != HSTEX_MODE_VERTICAL
                 : engine->mode == HSTEX_MODE_VERTICAL) {
        return set_error(error, error_capacity,
                         vertical
                             ? "moveleft and moveright require vertical mode"
                             : "raise and lower require horizontal mode");
    }
    int32_t amount = 0;
    if (scan_dimension(engine, &amount, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_box box;
    if (scan_box_operand(engine, &box, error, error_capacity) != 0) {
        return -1;
    }
    if (box.kind == HSTEX_BOX_VOID) {
        return 0;
    }
    /* \raise and \moveleft displace against the positive direction. */
    int32_t shift = subtype == (int32_t)HSTEX_SHIFT_RAISE ||
                            subtype == (int32_t)HSTEX_SHIFT_MOVE_LEFT
                        ? -amount
                        : amount;
    struct hstex_node node = {
        .kind = HSTEX_NODE_LIST,
        .width = box.width,
        .height = box.height,
        .depth = box.depth,
        .shift = shift,
        .value.list = {
            .node_start = box.node_start,
            .node_count = box.node_count,
            .box_kind = box.kind,
        },
    };
    /* A displaced box in a formula is an ordinary atom, so that it spaces
       against its neighbours like any other. */
    if (engine->mode == HSTEX_MODE_MATH) {
        uint32_t identifier = 0U;
        if (store_node(engine, &node, &identifier, error, error_capacity) !=
            0) {
            return -1;
        }
        struct hstex_noad noad = {
            .kind = (uint8_t)HSTEX_NOAD_ATOM,
            .atom_class = (uint8_t)HSTEX_ATOM_ORD,
            .nucleus = {.kind = (uint8_t)HSTEX_MATH_FIELD_BOX,
                        .node = identifier},
        };
        return math_append_atom(engine, &noad, error, error_capacity);
    }
    return append_current_list_node(engine, &node, error, error_capacity);
}

/* \box and \copy used on their own contribute the box to the current list. */
static int execute_box_reference(struct hstex_engine *engine,
                                 enum hstex_command command, char *error,
                                 size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "box reference does not accept prefixes");
    }
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    struct hstex_box box = engine->boxes[(size_t)index];
    if (command == HSTEX_COMMAND_BOX) {
        struct hstex_box empty = {0};
        /* Emptying a register outlives the group it happened in. */
        empty.kind = HSTEX_BOX_VOID;
        if (assign_box(engine, (uint32_t)index, empty, true, error,
                       error_capacity) != 0) {
            return -1;
        }
    }
    if (box.kind == HSTEX_BOX_VOID) {
        return 0;
    }
    return append_box_node(engine, &box, error, error_capacity);
}

static int execute_box_constructor(struct hstex_engine *engine,
                                   enum hstex_command command, char *error,
                                   size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "box construction does not accept prefixes");
    }
    struct hstex_box box;
    int status = 0;
    switch (command) {
    case HSTEX_COMMAND_HBOX:
        status = scan_hbox(engine, &box, error, error_capacity);
        break;
    case HSTEX_COMMAND_VTOP:
        status = scan_vbox(engine, true, &box, error, error_capacity);
        break;
    case HSTEX_COMMAND_VSPLIT:
        status = scan_vsplit(engine, &box, error, error_capacity);
        break;
    case HSTEX_COMMAND_LAST_BOX:
        status = scan_last_box(engine, &box, error, error_capacity);
        break;
    default:
        status = scan_vbox(engine, false, &box, error, error_capacity);
        break;
    }
    if (status == 0 && box.kind == HSTEX_BOX_VOID) {
        return 0;
    }
    if (status != 0) {
        return -1;
    }
    return engine->mode == HSTEX_MODE_MATH
               ? math_append_box(engine, &box, error, error_capacity)
               : append_box_node(engine, &box, error, error_capacity);
}

static int execute_set_box(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;

    int32_t register_index = 0;
    if (scan_integer(engine, &register_index, error, error_capacity) != 0) {
        return -1;
    }
    if (register_index < 0 || (size_t)register_index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", register_index,
                         engine->count_capacity - 1U);
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_box value;
    if (scan_box_operand(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    return assign_box(engine, (uint32_t)register_index, value,
                      requested_global, error, error_capacity);
}

static int scan_definition_target(struct hstex_engine *engine,
                                  hstex_cs_id *identifier, char *error,
                                  size_t error_capacity)
{
    hstex_token target = 0U;
    struct hstex_source_location location;
    if (raw_next_non_space(engine, &target, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(target)) {
        return set_error(error, error_capacity,
                         "definition requires a control-sequence target");
    }
    *identifier = hstex_token_control_sequence_id(target);
    return 0;
}

static int execute_else(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
{
    if (engine->conditional_count == 0U) {
        return set_error(error, error_capacity, "extra else");
    }
    struct hstex_conditional *conditional =
        &engine->conditionals[engine->conditional_count - 1U];
    if (conditional->else_seen) {
        return set_error(error, error_capacity,
                         "second else in the conditional opened at %s:%u",
                         conditional->origin, (unsigned int)conditional->line);
    }
    conditional->else_seen = true;
    return skip_conditional(engine, engine->conditional_count - 1U, false,
                            error, error_capacity);
}

static int execute_or(struct hstex_engine *engine, char *error,
                      size_t error_capacity)
{
    if (engine->conditional_count == 0U) {
        return set_error(error, error_capacity, "extra or");
    }
    size_t target = engine->conditional_count - 1U;
    struct hstex_conditional *conditional = &engine->conditionals[target];
    if (!conditional->case_conditional || conditional->else_seen) {
        return set_error(error, error_capacity,
                         "or outside an active ifcase branch");
    }
    return skip_case_remainder(engine, target, error, error_capacity);
}

static int execute_fi(struct hstex_engine *engine, char *error,
                      size_t error_capacity)
{
    if (engine->conditional_count == 0U) {
        return set_error(error, error_capacity, "extra fi");
    }
    --engine->conditional_count;
    return 0;
}

static int scan_char_definition(struct hstex_engine *engine, char *error,
                                size_t error_capacity)
{
    hstex_cs_id identifier = 0U;
    int32_t value = 0;
    if (scan_definition_target(engine, &identifier, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0 || value < 0 ||
        value > 255) {
        return set_error(error, error_capacity,
                         "chardef value outside 0..255");
    }
    struct hstex_meaning meaning = {
        .command = HSTEX_COMMAND_CHAR_GIVEN,
        .level = 0U,
        .value = {.integer = value},
    };
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, identifier, meaning, global, error,
                       error_capacity);
}

static int scan_math_char_definition(struct hstex_engine *engine, char *error,
                                     size_t error_capacity)
{
    hstex_cs_id identifier = 0U;
    int32_t value = 0;
    if (scan_definition_target(engine, &identifier, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0 || value < 0 ||
        value > 32767) {
        return set_error(error, error_capacity,
                         "mathchardef value outside 0..32767");
    }
    struct hstex_meaning meaning = {
        .command = HSTEX_COMMAND_MATH_CHAR_GIVEN,
        .level = 0U,
        .value = {.integer = value},
    };
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, identifier, meaning, global, error,
                       error_capacity);
}

static int scan_register_definition(struct hstex_engine *engine,
                                    enum hstex_command register_command,
                                    char *error, size_t error_capacity)
{
    hstex_cs_id identifier = 0U;
    int32_t index = 0;
    if (scan_definition_target(engine, &identifier, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &index, error, error_capacity) != 0 || index < 0 ||
        index >= 32768) {
        return set_error(error, error_capacity,
                         "register definition outside supported range");
    }
    struct hstex_meaning meaning = {
        .command = register_command,
        .level = 0U,
        .value = {.integer = index},
    };
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, identifier, meaning, global, error,
                       error_capacity);
}

static int scan_count_definition(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    hstex_cs_id identifier = 0U;
    int32_t index = 0;
    if (scan_definition_target(engine, &identifier, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &index, error, error_capacity) != 0 || index < 0 ||
        (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "countdef register outside supported range");
    }
    struct hstex_meaning meaning = {
        .command = HSTEX_COMMAND_COUNT_REGISTER,
        .level = 0U,
        .value = {.integer = index},
    };
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, identifier, meaning, global, error,
                       error_capacity);
}

static int find_or_add_hyphen_child(struct hstex_engine *engine,
                                    uint32_t language, uint32_t parent,
                                    uint8_t character, uint32_t *child,
                                    char *error, size_t error_capacity)
{
    if ((size_t)language >= engine->count_capacity ||
        parent > engine->hyphen_node_count) {
        return set_error(error, error_capacity,
                         "invalid hyphen-trie insertion point");
    }
    uint32_t current = parent == 0U
                           ? engine->hyphen_roots[language]
                           : engine->hyphen_nodes[parent - 1U].first_child;
    uint32_t previous = 0U;
    while (current != 0U) {
        if ((size_t)current > engine->hyphen_node_count) {
            return set_error(error, error_capacity,
                             "corrupt hyphen-trie sibling");
        }
        const struct hstex_hyphen_trie_node *node =
            &engine->hyphen_nodes[current - 1U];
        if (node->character >= character) {
            if (node->character == character) {
                *child = current;
                return 0;
            }
            break;
        }
        previous = current;
        current = node->next_sibling;
    }
    if (engine->hyphen_node_count >= (size_t)UINT32_MAX ||
        reserve_hyphen_nodes(engine, engine->hyphen_node_count + 1U, error,
                             error_capacity) != 0) {
        return -1;
    }
    uint32_t identifier = (uint32_t)engine->hyphen_node_count + 1U;
    struct hstex_hyphen_trie_node *node =
        &engine->hyphen_nodes[engine->hyphen_node_count++];
    node->character = character;
    node->next_sibling = current;
    if (previous != 0U) {
        engine->hyphen_nodes[previous - 1U].next_sibling = identifier;
    } else if (parent != 0U) {
        engine->hyphen_nodes[parent - 1U].first_child = identifier;
    } else {
        engine->hyphen_roots[language] = identifier;
    }
    *child = identifier;
    return 0;
}

static int insert_hyphen_pattern(struct hstex_engine *engine,
                                 uint32_t language, const uint8_t *letters,
                                 const uint8_t *values, size_t letter_count,
                                 char *error, size_t error_capacity)
{
    if (letter_count == 0U ||
        letter_count > (size_t)HSTEX_MAX_HYPHEN_PATTERN_LENGTH) {
        return set_error(error, error_capacity,
                         "invalid hyphenation pattern length");
    }
    uint32_t node_identifier = 0U;
    for (size_t index = 0U; index < letter_count; ++index) {
        if (find_or_add_hyphen_child(engine, language, node_identifier,
                                     letters[index], &node_identifier, error,
                                     error_capacity) != 0) {
            return -1;
        }
    }
    size_t value_count = letter_count + 1U;
    if (value_count > SIZE_MAX - engine->hyphen_value_count ||
        engine->hyphen_value_count > (size_t)UINT32_MAX ||
        reserve_byte_arena(&engine->hyphen_values,
                           &engine->hyphen_value_capacity,
                           (size_t)HSTEX_INITIAL_HYPHEN_VALUE_CAPACITY,
                           engine->hyphen_value_count + value_count,
                           "hyphen-value arena", error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "hyphen-value insertion failed");
    }
    struct hstex_hyphen_trie_node *terminal =
        &engine->hyphen_nodes[node_identifier - 1U];
    if (terminal->value_count == 0U) {
        ++engine->hyphen_pattern_count;
    }
    terminal->value_offset = (uint32_t)engine->hyphen_value_count;
    terminal->value_count = (uint16_t)value_count;
    memcpy(engine->hyphen_values + engine->hyphen_value_count, values,
           value_count);
    engine->hyphen_value_count += value_count;
    return 0;
}

static int insert_hyphen_exception(struct hstex_engine *engine,
                                   uint32_t language, const uint8_t *letters,
                                   const uint8_t *breaks, size_t letter_count,
                                   char *error, size_t error_capacity)
{
    if (letter_count == 0U || language > (uint32_t)UINT16_MAX ||
        letter_count > (size_t)HSTEX_MAX_HYPHEN_PATTERN_LENGTH ||
        letter_count > (SIZE_MAX - 1U) / 2U) {
        return set_error(error, error_capacity,
                         "invalid hyphenation exception");
    }
    size_t required_data = letter_count * 2U + 1U;
    if (required_data > SIZE_MAX - engine->hyphen_exception_data_count ||
        engine->hyphen_exception_data_count + required_data >
            (size_t)UINT32_MAX ||
        reserve_hyphen_exceptions(engine,
                                  engine->hyphen_exception_count + 1U, error,
                                  error_capacity) != 0 ||
        reserve_byte_arena(
            &engine->hyphen_exception_data,
            &engine->hyphen_exception_data_capacity,
            (size_t)HSTEX_INITIAL_HYPHEN_EXCEPTION_DATA_CAPACITY,
            engine->hyphen_exception_data_count + required_data,
            "hyphen-exception arena", error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_hyphen_exception *exception =
        &engine->hyphen_exceptions[engine->hyphen_exception_count++];
    exception->language = (uint16_t)language;
    exception->letter_count = (uint16_t)letter_count;
    exception->letter_offset =
        (uint32_t)engine->hyphen_exception_data_count;
    memcpy(engine->hyphen_exception_data +
               engine->hyphen_exception_data_count,
           letters, letter_count);
    engine->hyphen_exception_data_count += letter_count;
    exception->break_offset = (uint32_t)engine->hyphen_exception_data_count;
    memcpy(engine->hyphen_exception_data +
               engine->hyphen_exception_data_count,
           breaks, letter_count + 1U);
    engine->hyphen_exception_data_count += letter_count + 1U;
    return 0;
}

static int normalized_hyphen_character(const struct hstex_engine *engine,
                                       uint8_t character, bool pattern,
                                       uint8_t *normalized, char *error,
                                       size_t error_capacity)
{
    if (pattern && character == (uint8_t)'.') {
        *normalized = character;
        return 0;
    }
    int32_t lowercase = engine->code_tables[1][character];
    if (lowercase <= 0 || lowercase > 255) {
        return set_error(error, error_capacity,
                         "hyphenation text contains a character with zero lccode");
    }
    *normalized = (uint8_t)lowercase;
    return 0;
}

static int finish_hyphen_item(struct hstex_engine *engine, bool patterns,
                              uint32_t language, uint8_t *letters,
                              uint8_t *values_or_breaks, size_t *letter_count,
                              char *error, size_t error_capacity)
{
    if (*letter_count == 0U) {
        return 0;
    }
    int status = patterns
                     ? insert_hyphen_pattern(engine, language, letters,
                                             values_or_breaks, *letter_count,
                                             error, error_capacity)
                     : insert_hyphen_exception(engine, language, letters,
                                               values_or_breaks, *letter_count,
                                               error, error_capacity);
    *letter_count = 0U;
    memset(values_or_breaks, 0,
           (size_t)HSTEX_MAX_HYPHEN_PATTERN_LENGTH + 1U);
    return status;
}

static int scan_hyphen_data(struct hstex_engine *engine, bool patterns,
                            char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space_unrestricted(
            engine, &opening, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         patterns ? "patterns requires a braced list"
                                  : "hyphenation requires a braced list");
    }
    int32_t language_value =
        engine->integer_parameters[HSTEX_INTEGER_LANGUAGE];
    if (language_value < 0 ||
        (size_t)language_value >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "hyphenation language outside supported range");
    }
    uint32_t language = (uint32_t)language_value;
    uint8_t letters[HSTEX_MAX_HYPHEN_PATTERN_LENGTH];
    uint8_t values_or_breaks[HSTEX_MAX_HYPHEN_PATTERN_LENGTH + 1U] = {0};
    size_t letter_count = 0U;
    for (;;) {
        hstex_token token = 0U;
        bool previous_inhibition = engine->inhibit_protected_expansion;
        engine->inhibit_protected_expansion = false;
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        engine->inhibit_protected_expansion = previous_inhibition;
        if (result != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input in hyphenation data");
        }
        if (token_is_category(token, HSTEX_CAT_END_GROUP)) {
            if (finish_hyphen_item(engine, patterns, language, letters,
                                   values_or_breaks, &letter_count, error,
                                   error_capacity) != 0) {
                return -1;
            }
            engine->pending_global = false;
            engine->pending_macro_flags = 0U;
            return 0;
        }
        if (token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
            return set_error(error, error_capacity,
                             "nested group in hyphenation data");
        }
        if (token_is_effective_space(engine, token)) {
            if (finish_hyphen_item(engine, patterns, language, letters,
                                   values_or_breaks, &letter_count, error,
                                   error_capacity) != 0) {
                return -1;
            }
            continue;
        }
        if (!hstex_token_is_character(token)) {
            return set_error(error, error_capacity,
                             "control sequence remained in hyphenation data");
        }
        uint8_t character = hstex_token_character_code(token);
        if (patterns && character >= (uint8_t)'0' &&
            character <= (uint8_t)'9') {
            values_or_breaks[letter_count] =
                (uint8_t)(character - (uint8_t)'0');
            continue;
        }
        if (!patterns && character == (uint8_t)'-') {
            values_or_breaks[letter_count] = 1U;
            continue;
        }
        if (letter_count == (size_t)HSTEX_MAX_HYPHEN_PATTERN_LENGTH) {
            return set_error(error, error_capacity,
                             "hyphenation item is too long");
        }
        if (normalized_hyphen_character(engine, character, patterns,
                                        &letters[letter_count], error,
                                        error_capacity) != 0) {
            return -1;
        }
        ++letter_count;
    }
}

static uint32_t find_hyphen_child(const struct hstex_engine *engine,
                                  uint32_t first, uint8_t character)
{
    uint32_t current = first;
    while (current != 0U && (size_t)current <= engine->hyphen_node_count) {
        const struct hstex_hyphen_trie_node *node =
            &engine->hyphen_nodes[current - 1U];
        if (node->character >= character) {
            return node->character == character ? current : 0U;
        }
        current = node->next_sibling;
    }
    return 0U;
}

static void apply_hyphen_minima(const struct hstex_engine *engine,
                                uint8_t *break_before, size_t length)
{
    int32_t left_value =
        engine->integer_parameters[HSTEX_INTEGER_LEFT_HYPHEN_MIN];
    int32_t right_value =
        engine->integer_parameters[HSTEX_INTEGER_RIGHT_HYPHEN_MIN];
    size_t left = left_value > 0 ? (size_t)left_value : 0U;
    size_t right = right_value > 0 ? (size_t)right_value : 0U;
    for (size_t index = 1U; index < length; ++index) {
        if (index < left || length - index < right) {
            break_before[index] = 0U;
        }
    }
}

int hstex_engine_hyphenate_word(const struct hstex_engine *engine,
                                int32_t language, const uint8_t *word,
                                size_t length, uint8_t *break_before,
                                size_t break_capacity, char *error,
                                size_t error_capacity)
{
    if (engine == NULL || word == NULL || break_before == NULL ||
        language < 0 || (size_t)language >= engine->count_capacity ||
        length > (size_t)HSTEX_MAX_HYPHEN_PATTERN_LENGTH ||
        break_capacity < length + 1U) {
        return set_error(error, error_capacity,
                         "invalid word-hyphenation request");
    }
    memset(break_before, 0, length + 1U);
    if (length == 0U) {
        return 0;
    }
    uint8_t lowercase[HSTEX_MAX_HYPHEN_PATTERN_LENGTH];
    for (size_t index = 0U; index < length; ++index) {
        int32_t code = engine->code_tables[1][word[index]];
        if (code <= 0 || code > 255) {
            return 0;
        }
        lowercase[index] = (uint8_t)code;
    }
    for (size_t index = engine->hyphen_exception_count; index > 0U; --index) {
        const struct hstex_hyphen_exception *exception =
            &engine->hyphen_exceptions[index - 1U];
        if ((int32_t)exception->language != language ||
            (size_t)exception->letter_count != length ||
            (size_t)exception->letter_offset + length >
                engine->hyphen_exception_data_count ||
            memcmp(engine->hyphen_exception_data + exception->letter_offset,
                   lowercase, length) != 0) {
            continue;
        }
        if ((size_t)exception->break_offset + length + 1U >
            engine->hyphen_exception_data_count) {
            return set_error(error, error_capacity,
                             "corrupt hyphenation exception");
        }
        memcpy(break_before,
               engine->hyphen_exception_data + exception->break_offset,
               length + 1U);
        apply_hyphen_minima(engine, break_before, length);
        return 0;
    }

    uint8_t augmented[HSTEX_MAX_HYPHEN_PATTERN_LENGTH + 2U];
    uint8_t scores[HSTEX_MAX_HYPHEN_PATTERN_LENGTH + 3U] = {0};
    augmented[0] = (uint8_t)'.';
    memcpy(augmented + 1U, lowercase, length);
    augmented[length + 1U] = (uint8_t)'.';
    size_t augmented_length = length + 2U;
    for (size_t start = 0U; start < augmented_length; ++start) {
        uint32_t siblings = engine->hyphen_roots[(size_t)language];
        for (size_t cursor = start; cursor < augmented_length; ++cursor) {
            uint32_t identifier =
                find_hyphen_child(engine, siblings, augmented[cursor]);
            if (identifier == 0U) {
                break;
            }
            const struct hstex_hyphen_trie_node *node =
                &engine->hyphen_nodes[identifier - 1U];
            if (node->value_count != 0U) {
                if ((size_t)node->value_offset + node->value_count >
                    engine->hyphen_value_count) {
                    return set_error(error, error_capacity,
                                     "corrupt hyphenation pattern values");
                }
                for (size_t value = 0U; value < node->value_count &&
                                       start + value < sizeof(scores);
                     ++value) {
                    uint8_t candidate =
                        engine->hyphen_values[node->value_offset + value];
                    if (candidate > scores[start + value]) {
                        scores[start + value] = candidate;
                    }
                }
            }
            siblings = node->first_child;
        }
    }
    for (size_t index = 1U; index < length; ++index) {
        break_before[index] = (uint8_t)(scores[index + 1U] & 1U);
    }
    apply_hyphen_minima(engine, break_before, length);
    return 0;
}

static int scan_catcode_assignment(struct hstex_engine *engine, char *error,
                                   size_t error_capacity)
{
    int32_t character = 0;
    int32_t category = 0;
    if (scan_integer(engine, &character, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &category, error, error_capacity) != 0 ||
        character < 0 || category < 0) {
        return set_error(error, error_capacity, "invalid catcode assignment");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_catcode(engine, (uint32_t)character, (uint32_t)category,
                          requested_global, error, error_capacity);
}

static int scan_code_assignment(struct hstex_engine *engine,
                                enum hstex_command command, char *error,
                                size_t error_capacity)
{
    int table = code_table_index(command);
    int32_t character = 0;
    int32_t value = 0;
    if (table < 0 || scan_integer(engine, &character, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0 ||
        character < 0 || character > 255) {
        return set_error(error, error_capacity,
                         "invalid code-table assignment");
    }
    if ((command == HSTEX_COMMAND_LC_CODE ||
         command == HSTEX_COMMAND_UC_CODE) &&
        (value < 0 || value > 255)) {
        return set_error(error, error_capacity,
                         "case code outside 0..255");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_code(engine, (uint32_t)table, (uint32_t)character, value,
                       requested_global, error, error_capacity);
}

static int scan_count_assignment(struct hstex_engine *engine, int32_t index,
                                 char *error, size_t error_capacity)
{
    int32_t value = 0;
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity, "invalid count assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_count(engine, (uint32_t)index, value, requested_global, error,
                        error_capacity);
}

static int scan_count_family_assignment(struct hstex_engine *engine, char *error,
                                        size_t error_capacity)
{
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    return scan_count_assignment(engine, index, error, error_capacity);
}

static int scan_integer_parameter_assignment(
    struct hstex_engine *engine, int32_t parameter, char *error,
    size_t error_capacity)
{
    int32_t value = 0;
    if (parameter < 0 ||
        parameter >= (int32_t)HSTEX_INTEGER_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid integer-parameter assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_integer_parameter(engine, (uint32_t)parameter, value,
                                    requested_global, error, error_capacity);
}

static int scan_dimen_assignment(struct hstex_engine *engine, int32_t index,
                                 char *error, size_t error_capacity)
{
    int32_t value = 0;
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity, "invalid dimen assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_dimen(engine, (uint32_t)index, value, requested_global, error,
                        error_capacity);
}

static int scan_dimen_family_assignment(struct hstex_engine *engine,
                                        char *error, size_t error_capacity)
{
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    return scan_dimen_assignment(engine, index, error, error_capacity);
}

static int scan_glue_assignment(struct hstex_engine *engine, int32_t index,
                                char *error, size_t error_capacity)
{
    struct hstex_glue value;
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity, "invalid glue assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_glue(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_glue(engine, (uint32_t)index, value, requested_global, error,
                       error_capacity);
}

static int scan_glue_family_assignment(struct hstex_engine *engine, char *error,
                                       size_t error_capacity)
{
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    return scan_glue_assignment(engine, index, error, error_capacity);
}

static int scan_muglue_assignment(struct hstex_engine *engine, int32_t index,
                                  char *error, size_t error_capacity)
{
    struct hstex_glue value;
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "invalid math-glue assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_math_glue(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_muglue(engine, (uint32_t)index, value, requested_global, error,
                         error_capacity);
}

static int scan_muglue_family_assignment(struct hstex_engine *engine,
                                         char *error, size_t error_capacity)
{
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    return scan_muglue_assignment(engine, index, error, error_capacity);
}

static int scan_dimen_parameter_assignment(struct hstex_engine *engine,
                                           int32_t parameter, char *error,
                                           size_t error_capacity)
{
    int32_t value = 0;
    if (parameter < 0 || parameter >= (int32_t)HSTEX_DIMEN_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid dimen-parameter assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_dimen_parameter(engine, (uint32_t)parameter, value,
                                  requested_global, error, error_capacity);
}

/* The page stays empty until a box or rule reaches the main vertical list;
   glue and penalties contributed before that are discarded by the page
   builder and leave the page totals untouched. */
static bool page_is_empty(const struct hstex_engine *engine)
{
    const struct hstex_vbox_builder *page = engine->page_builder;
    if (page == NULL) {
        return true;
    }
    for (size_t index = 0U; index < page->count; ++index) {
        uint32_t identifier = page->node_identifiers[index];
        if (identifier == 0U || (size_t)identifier > engine->node_count) {
            continue;
        }
        enum hstex_node_kind kind = engine->nodes[identifier - 1U].kind;
        if (kind == HSTEX_NODE_RULE || kind == HSTEX_NODE_LIST) {
            return false;
        }
    }
    return true;
}

/* Page state is not saved by grouping, so these assignments bypass the save
   stack and ignore \global. */
static int scan_page_integer_assignment(struct hstex_engine *engine,
                                        int32_t index, char *error,
                                        size_t error_capacity)
{
    int32_t value = 0;
    if (index < 0 || index >= (int32_t)HSTEX_PAGE_INTEGER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid page-integer assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    engine->page_integers[(size_t)index] = value;
    return 0;
}

static int scan_page_dimen_assignment(struct hstex_engine *engine,
                                      int32_t index, char *error,
                                      size_t error_capacity)
{
    int32_t value = 0;
    if (index < 0 || index >= (int32_t)HSTEX_PAGE_DIMEN_COUNT) {
        return set_error(error, error_capacity,
                         "invalid page-dimen assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    /* An assignment to a page dimension is discarded while the page is empty;
       the value is still scanned. */
    if (!page_is_empty(engine)) {
        engine->page_dimens[(size_t)index] = value;
    }
    return 0;
}

/* Setting a dimension of a void box has no effect; the value is still
   scanned. */
static int scan_box_dimen_assignment(struct hstex_engine *engine,
                                     int32_t subtype, char *error,
                                     size_t error_capacity)
{
    int32_t index = 0;
    int32_t value = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    struct hstex_box box = engine->boxes[(size_t)index];
    if (box.kind == HSTEX_BOX_VOID) {
        return 0;
    }
    if (subtype == (int32_t)HSTEX_BOX_DIMEN_HEIGHT) {
        box.height = value;
    } else if (subtype == (int32_t)HSTEX_BOX_DIMEN_DEPTH) {
        box.depth = value;
    } else {
        box.width = value;
    }
    return assign_box(engine, (uint32_t)index, box, requested_global, error,
                      error_capacity);
}

/* Protrusion and expansion settings belong to the font rather than to a
   group, so they bypass the save stack and \global changes nothing. */
static int scan_font_char_code_assignment(struct hstex_engine *engine,
                                          int32_t subtype, char *error,
                                          size_t error_capacity)
{
    if (subtype == (int32_t)HSTEX_FONT_CODE_TAG) {
        return set_error(error, error_capacity,
                         "the metric file's tag cannot be assigned");
    }
    uint32_t identifier = 0U;
    int32_t code = 0;
    int32_t value = 0;
    if (scan_font_identifier(engine, &identifier, error, error_capacity) != 0 ||
        scan_integer(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    struct hstex_font *font = font_by_identifier(engine, identifier);
    if (font == NULL || font->characters == NULL) {
        return set_error(error, error_capacity,
                         "font code requires a defined font");
    }
    if (code < 0 || (size_t)code >= HSTEX_FONT_CHARACTER_COUNT) {
        return set_error(error, error_capacity, "bad character code (%d)",
                         code);
    }
    struct hstex_char_metric *metric = &font->characters[(size_t)code];
    if (subtype == (int32_t)HSTEX_FONT_CODE_LEFT_PROTRUSION) {
        metric->left_protrusion = value;
    } else if (subtype == (int32_t)HSTEX_FONT_CODE_RIGHT_PROTRUSION) {
        metric->right_protrusion = value;
    } else {
        metric->expansion_factor = value;
    }
    return 0;
}

static int scan_prev_depth_assignment(struct hstex_engine *engine,
                                      char *error, size_t error_capacity)
{
    int32_t value = 0;
    if (engine->mode != HSTEX_MODE_VERTICAL) {
        return set_error(error, error_capacity,
                         "prevdepth is only assignable in vertical mode");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    engine->prev_depth = value;
    return 0;
}

static int scan_glue_parameter_assignment(struct hstex_engine *engine,
                                          int32_t parameter, char *error,
                                          size_t error_capacity)
{
    struct hstex_glue value;
    if (parameter < 0 || parameter >= (int32_t)HSTEX_GLUE_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid glue-parameter assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_glue(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_glue_parameter(engine, (uint32_t)parameter, value,
                                 requested_global, error, error_capacity);
}

static int scan_muglue_parameter_assignment(struct hstex_engine *engine,
                                             int32_t parameter, char *error,
                                             size_t error_capacity)
{
    struct hstex_glue value;
    if (parameter < 0 ||
        parameter >= (int32_t)HSTEX_MUGLUE_PARAMETER_COUNT) {
        return set_error(error, error_capacity,
                         "invalid math-glue-parameter assignment");
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_math_glue(engine, &value, error, error_capacity) != 0) {
        return -1;
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_muglue_parameter(engine, (uint32_t)parameter, value,
                                   requested_global, error, error_capacity);
}

static int scan_token_list_value(struct hstex_engine *engine,
                                 uint32_t *identifier, char *error,
                                 size_t error_capacity)
{
    hstex_token first = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &first, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning a token list");
    }
    if (hstex_token_is_control_sequence(first)) {
        int result = token_list_identifier_from_meaning(
            engine,
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(first)),
            identifier, error, error_capacity);
        if (result != 0) {
            return result < 0 ? -1 : 0;
        }
    }
    if (!token_is_category(first, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "token-list assignment requires a register or braces");
    }
    struct token_vector tokens = {0};
    if (scan_balanced_group(engine, &tokens, true, error, error_capacity) != 0 ||
        store_token_list(engine, &tokens, identifier, error, error_capacity) !=
            0) {
        vector_destroy(&tokens);
        return -1;
    }
    return 0;
}

static int scan_token_register_assignment(struct hstex_engine *engine,
                                          int32_t index, char *error,
                                          size_t error_capacity)
{
    uint32_t identifier = 0U;
    if (index < 0 || (size_t)index >= engine->count_capacity ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_token_list_value(engine, &identifier, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid token-register assignment");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_token_register(engine, (uint32_t)index, identifier,
                                 requested_global, error, error_capacity);
}

static int scan_token_family_assignment(struct hstex_engine *engine,
                                        char *error, size_t error_capacity)
{
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    return scan_token_register_assignment(engine, index, error,
                                          error_capacity);
}

static int scan_token_parameter_assignment(struct hstex_engine *engine,
                                           int32_t parameter, char *error,
                                           size_t error_capacity)
{
    uint32_t identifier = 0U;
    if (parameter < 0 || parameter >= (int32_t)HSTEX_TOKEN_PARAMETER_COUNT ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_token_list_value(engine, &identifier, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid token-parameter assignment");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_token_parameter(engine, (uint32_t)parameter, identifier,
                                  requested_global, error, error_capacity);
}

enum arithmetic_variable_kind {
    ARITHMETIC_VARIABLE_COUNT = 0,
    ARITHMETIC_VARIABLE_INTEGER_PARAMETER,
    ARITHMETIC_VARIABLE_DIMEN,
    ARITHMETIC_VARIABLE_DIMEN_PARAMETER,
    ARITHMETIC_VARIABLE_PREV_DEPTH,
    ARITHMETIC_VARIABLE_GLUE,
    ARITHMETIC_VARIABLE_GLUE_PARAMETER,
    ARITHMETIC_VARIABLE_MUGLUE,
    ARITHMETIC_VARIABLE_MUGLUE_PARAMETER,
};

struct arithmetic_variable {
    enum arithmetic_variable_kind kind;
    uint32_t index;
};

static int scan_arithmetic_variable(struct hstex_engine *engine,
                                    struct arithmetic_variable *variable,
                                    char *error, size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &token, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(token)) {
        return set_error(error, error_capacity,
                         "arithmetic operation requires a variable");
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_COUNT_REGISTER) {
        if (meaning->value.integer < 0 ||
            (size_t)meaning->value.integer >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid count-register variable");
        }
        variable->kind = ARITHMETIC_VARIABLE_COUNT;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_COUNT) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 || index < 0 ||
            (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "count variable outside supported range");
        }
        variable->kind = ARITHMETIC_VARIABLE_COUNT;
        variable->index = (uint32_t)index;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_INTEGER_PARAMETER &&
        meaning->value.integer >= 0 &&
        meaning->value.integer < (int32_t)HSTEX_INTEGER_PARAMETER_COUNT) {
        variable->kind = ARITHMETIC_VARIABLE_INTEGER_PARAMETER;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_DIMEN_REGISTER &&
        meaning->value.integer >= 0 &&
        (size_t)meaning->value.integer < engine->count_capacity) {
        variable->kind = ARITHMETIC_VARIABLE_DIMEN;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_DIMEN) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "dimen arithmetic target outside supported range");
        }
        variable->kind = ARITHMETIC_VARIABLE_DIMEN;
        variable->index = (uint32_t)index;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_DIMEN_PARAMETER &&
        meaning->value.integer >= 0 &&
        meaning->value.integer < (int32_t)HSTEX_DIMEN_PARAMETER_COUNT) {
        variable->kind = ARITHMETIC_VARIABLE_DIMEN_PARAMETER;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_PREV_DEPTH) {
        if (engine->mode != HSTEX_MODE_VERTICAL) {
            return set_error(error, error_capacity,
                             "prevdepth arithmetic requires vertical mode");
        }
        variable->kind = ARITHMETIC_VARIABLE_PREV_DEPTH;
        variable->index = 0U;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_SKIP_REGISTER &&
        meaning->value.integer >= 0 &&
        (size_t)meaning->value.integer < engine->count_capacity) {
        variable->kind = ARITHMETIC_VARIABLE_GLUE;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_SKIP) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "skip arithmetic target outside supported range");
        }
        variable->kind = ARITHMETIC_VARIABLE_GLUE;
        variable->index = (uint32_t)index;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_GLUE_PARAMETER &&
        meaning->value.integer >= 0 &&
        meaning->value.integer < (int32_t)HSTEX_GLUE_PARAMETER_COUNT) {
        variable->kind = ARITHMETIC_VARIABLE_GLUE_PARAMETER;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_MUSKIP_REGISTER &&
        meaning->value.integer >= 0 &&
        (size_t)meaning->value.integer < engine->count_capacity) {
        variable->kind = ARITHMETIC_VARIABLE_MUGLUE;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_MUSKIP) {
        int32_t index = 0;
        if (scan_integer(engine, &index, error, error_capacity) != 0 ||
            index < 0 || (size_t)index >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "muskip arithmetic target outside supported range");
        }
        variable->kind = ARITHMETIC_VARIABLE_MUGLUE;
        variable->index = (uint32_t)index;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_MUGLUE_PARAMETER &&
        meaning->value.integer >= 0 &&
        meaning->value.integer < (int32_t)HSTEX_MUGLUE_PARAMETER_COUNT) {
        variable->kind = ARITHMETIC_VARIABLE_MUGLUE_PARAMETER;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    return set_error(error, error_capacity,
                     "arithmetic target is not a supported variable");
}

static bool token_has_character(hstex_token token, uint8_t character)
{
    return hstex_token_is_character(token) &&
           hstex_token_character_code(token) == character;
}

static int scan_optional_by(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    hstex_token first = 0U;
    struct hstex_source_location first_location;
    if (expanded_next_non_space(engine, &first, &first_location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input in arithmetic operation");
    }
    if (!token_has_character(first, (uint8_t)'b')) {
        return push_one(engine, first, first_location, error, error_capacity);
    }
    hstex_token second = 0U;
    struct hstex_source_location second_location;
    if (hstex_engine_next_expanded(engine, &second, &second_location, error,
                                   error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input after arithmetic keyword prefix");
    }
    if (token_has_character(second, (uint8_t)'y')) {
        return 0;
    }
    if (push_one(engine, second, second_location, error, error_capacity) != 0 ||
        push_one(engine, first, first_location, error, error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static bool arithmetic_variable_is_integer(
    enum arithmetic_variable_kind kind)
{
    return kind == ARITHMETIC_VARIABLE_COUNT ||
           kind == ARITHMETIC_VARIABLE_INTEGER_PARAMETER;
}

static bool arithmetic_variable_is_dimen(enum arithmetic_variable_kind kind)
{
    return kind == ARITHMETIC_VARIABLE_DIMEN ||
           kind == ARITHMETIC_VARIABLE_DIMEN_PARAMETER ||
           kind == ARITHMETIC_VARIABLE_PREV_DEPTH;
}

static bool arithmetic_variable_is_glue(enum arithmetic_variable_kind kind)
{
    return kind == ARITHMETIC_VARIABLE_GLUE ||
           kind == ARITHMETIC_VARIABLE_GLUE_PARAMETER;
}

static int arithmetic_scalar_result(int32_t current, int32_t operand,
                                    enum hstex_command operation,
                                    int64_t lower, int64_t upper,
                                    int32_t *value, char *error,
                                    size_t error_capacity)
{
    int64_t result = 0;
    if (operation == HSTEX_COMMAND_ADVANCE) {
        result = (int64_t)current + operand;
    } else if (operation == HSTEX_COMMAND_MULTIPLY) {
        result = (int64_t)current * operand;
    } else {
        if (operand == 0) {
            return set_error(error, error_capacity, "division by zero");
        }
        result = (int64_t)current / operand;
    }
    if (result < lower || result > upper) {
        return set_error(error, error_capacity, "arithmetic overflow");
    }
    *value = (int32_t)result;
    return 0;
}

static int advance_glue_component(int32_t *left, uint8_t *left_order,
                                  int32_t right, uint8_t right_order,
                                  char *error, size_t error_capacity)
{
    if (right_order > *left_order) {
        *left = right;
        *left_order = right_order;
        return 0;
    }
    if (right_order < *left_order) {
        return 0;
    }
    int64_t combined = (int64_t)*left + right;
    if (combined < -INT64_C(1073741823) ||
        combined > INT64_C(1073741823)) {
        return set_error(error, error_capacity,
                         "glue arithmetic overflow");
    }
    *left = (int32_t)combined;
    return 0;
}

static int advance_glue_value(struct hstex_glue *left,
                              const struct hstex_glue *right, char *error,
                              size_t error_capacity)
{
    int64_t width = (int64_t)left->width + right->width;
    if (width < -INT64_C(1073741823) || width > INT64_C(1073741823)) {
        return set_error(error, error_capacity,
                         "glue-width arithmetic overflow");
    }
    struct hstex_glue result = *left;
    result.width = (int32_t)width;
    if (advance_glue_component(&result.stretch, &result.stretch_order,
                               right->stretch, right->stretch_order, error,
                               error_capacity) != 0 ||
        advance_glue_component(&result.shrink, &result.shrink_order,
                               right->shrink, right->shrink_order, error,
                               error_capacity) != 0) {
        return -1;
    }
    *left = result;
    return 0;
}

static int scale_glue_value(struct hstex_glue *value, int32_t operand,
                            enum hstex_command operation, char *error,
                            size_t error_capacity)
{
    struct hstex_glue result = *value;
    if (arithmetic_scalar_result(result.width, operand, operation,
                                 -INT64_C(1073741823),
                                 INT64_C(1073741823), &result.width, error,
                                 error_capacity) != 0 ||
        arithmetic_scalar_result(result.stretch, operand, operation,
                                 -INT64_C(1073741823),
                                 INT64_C(1073741823), &result.stretch, error,
                                 error_capacity) != 0 ||
        arithmetic_scalar_result(result.shrink, operand, operation,
                                 -INT64_C(1073741823),
                                 INT64_C(1073741823), &result.shrink, error,
                                 error_capacity) != 0) {
        return -1;
    }
    *value = result;
    return 0;
}

static int execute_arithmetic(struct hstex_engine *engine,
                              enum hstex_command operation, char *error,
                              size_t error_capacity)
{
    struct arithmetic_variable variable = {0};
    if (scan_arithmetic_variable(engine, &variable, error, error_capacity) !=
            0 ||
        scan_optional_by(engine, error, error_capacity) != 0) {
        return -1;
    }

    bool requested_global = engine->pending_global;
    if (arithmetic_variable_is_integer(variable.kind) ||
        arithmetic_variable_is_dimen(variable.kind)) {
        int32_t operand = 0;
        if ((operation == HSTEX_COMMAND_ADVANCE &&
             arithmetic_variable_is_dimen(variable.kind)
                 ? scan_dimension(engine, &operand, error, error_capacity)
                 : scan_integer(engine, &operand, error, error_capacity)) !=
            0) {
            return -1;
        }
        int32_t current = 0;
        if (variable.kind == ARITHMETIC_VARIABLE_COUNT) {
            current = engine->counts[variable.index];
        } else if (variable.kind ==
                   ARITHMETIC_VARIABLE_INTEGER_PARAMETER) {
            current = engine->integer_parameters[variable.index];
        } else if (variable.kind == ARITHMETIC_VARIABLE_DIMEN) {
            current = engine->dimens[variable.index];
        } else if (variable.kind == ARITHMETIC_VARIABLE_PREV_DEPTH) {
            current = engine->prev_depth;
        } else {
            current = engine->dimen_parameters[variable.index];
        }
        int32_t result = 0;
        int64_t lower = arithmetic_variable_is_integer(variable.kind)
                            ? (int64_t)INT32_MIN
                            : -INT64_C(1073741823);
        int64_t upper = arithmetic_variable_is_integer(variable.kind)
                            ? (int64_t)INT32_MAX
                            : INT64_C(1073741823);
        if (arithmetic_scalar_result(current, operand, operation, lower, upper,
                                     &result, error, error_capacity) != 0) {
            return -1;
        }
        engine->pending_global = false;
        engine->pending_macro_flags = 0U;
        switch (variable.kind) {
        case ARITHMETIC_VARIABLE_COUNT:
            return assign_count(engine, variable.index, result,
                                requested_global, error, error_capacity);
        case ARITHMETIC_VARIABLE_INTEGER_PARAMETER:
            return assign_integer_parameter(engine, variable.index, result,
                                            requested_global, error,
                                            error_capacity);
        case ARITHMETIC_VARIABLE_DIMEN:
            return assign_dimen(engine, variable.index, result,
                                requested_global, error, error_capacity);
        case ARITHMETIC_VARIABLE_DIMEN_PARAMETER:
            return assign_dimen_parameter(engine, variable.index, result,
                                          requested_global, error,
                                          error_capacity);
        case ARITHMETIC_VARIABLE_PREV_DEPTH:
            engine->prev_depth = result;
            return 0;
        default:
            return set_error(error, error_capacity,
                             "internal scalar arithmetic target mismatch");
        }
    }

    struct hstex_glue value;
    if (variable.kind == ARITHMETIC_VARIABLE_GLUE) {
        value = engine->glues[variable.index];
    } else if (variable.kind == ARITHMETIC_VARIABLE_GLUE_PARAMETER) {
        value = engine->glue_parameters[variable.index];
    } else if (variable.kind == ARITHMETIC_VARIABLE_MUGLUE) {
        value = engine->muglues[variable.index];
    } else {
        value = engine->muglue_parameters[variable.index];
    }
    if (operation == HSTEX_COMMAND_ADVANCE) {
        struct hstex_glue operand;
        int scan_status = arithmetic_variable_is_glue(variable.kind)
                              ? scan_glue(engine, &operand, error,
                                          error_capacity)
                              : scan_math_glue(engine, &operand, error,
                                               error_capacity);
        if (scan_status != 0 ||
            advance_glue_value(&value, &operand, error, error_capacity) != 0) {
            return -1;
        }
    } else {
        int32_t operand = 0;
        if (scan_integer(engine, &operand, error, error_capacity) != 0 ||
            scale_glue_value(&value, operand, operation, error,
                             error_capacity) != 0) {
            return -1;
        }
    }
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    switch (variable.kind) {
    case ARITHMETIC_VARIABLE_GLUE:
        return assign_glue(engine, variable.index, value, requested_global,
                           error, error_capacity);
    case ARITHMETIC_VARIABLE_GLUE_PARAMETER:
        return assign_glue_parameter(engine, variable.index, value,
                                     requested_global, error, error_capacity);
    case ARITHMETIC_VARIABLE_MUGLUE:
        return assign_muglue(engine, variable.index, value, requested_global,
                             error, error_capacity);
    case ARITHMETIC_VARIABLE_MUGLUE_PARAMETER:
        return assign_muglue_parameter(engine, variable.index, value,
                                       requested_global, error,
                                       error_capacity);
    default:
        return set_error(error, error_capacity,
                         "internal glue arithmetic target mismatch");
    }
}

static int scan_stream_number(struct hstex_engine *engine, int32_t *stream,
                              char *error, size_t error_capacity)
{
    if (scan_integer(engine, stream, error, error_capacity) != 0 || *stream < -1 ||
        *stream > 17) {
        return set_error(error, error_capacity,
                         "stream number outside supported range -1..17");
    }
    return 0;
}

static char *output_path(struct hstex_engine *engine, const char *filename)
{
    if (filename[0] == '/') {
        return strdup(filename);
    }
    return join_path(engine->output_directory == NULL ? "."
                                                     : engine->output_directory,
                     filename);
}

static int execute_open_out(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    int32_t stream = 0;
    char *filename = NULL;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_input_filename(engine, &filename, error, error_capacity) != 0) {
        free(filename);
        return -1;
    }
    if (stream < 0 || stream >= 16) {
        free(filename);
        return 0;
    }
    char *path = output_path(engine, filename);
    free(filename);
    if (path == NULL) {
        return set_error(error, error_capacity,
                         "output path allocation failed");
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        int saved_errno = errno;
        int status = set_error(error, error_capacity, "cannot open %s: %s", path,
                               strerror(saved_errno));
        free(path);
        return status;
    }
    free(path);
    if (engine->write_streams[(size_t)stream] != NULL) {
        (void)fclose(engine->write_streams[(size_t)stream]);
    }
    engine->write_streams[(size_t)stream] = file;
    return 0;
}

static int execute_close_out(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    int32_t stream = 0;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0) {
        return -1;
    }
    if (stream >= 0 && stream < 16 &&
        engine->write_streams[(size_t)stream] != NULL) {
        if (fclose(engine->write_streams[(size_t)stream]) != 0) {
            engine->write_streams[(size_t)stream] = NULL;
            return set_error(error, error_capacity,
                             "failed to close output stream");
        }
        engine->write_streams[(size_t)stream] = NULL;
    }
    return 0;
}

static int serialize_control_sequence(struct hstex_engine *engine,
                                      hstex_token token, uint8_t **bytes,
                                      size_t *count, size_t *capacity,
                                      bool terminate_control_word,
                                      char *error, size_t error_capacity)
{
    enum hstex_symbol_kind kind;
    const uint8_t *name = NULL;
    size_t length = 0U;
    if (hstex_symbol_name(&engine->lexical_state.symbols,
                          hstex_token_control_sequence_id(token), &kind, &name,
                          &length) != 0) {
        return set_error(error, error_capacity,
                         "invalid control sequence in write text");
    }
    int32_t escape =
        engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER];
    if (kind == HSTEX_SYMBOL_REGULAR && escape >= 0 && escape <= 255 &&
        append_byte(bytes, count, capacity, (uint8_t)escape, error,
                    error_capacity) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (append_byte(bytes, count, capacity, name[index], error,
                        error_capacity) != 0) {
            return -1;
        }
    }
    if (terminate_control_word &&
        regular_control_sequence_needs_space(engine, kind, name, length) &&
        append_byte(bytes, count, capacity, (uint8_t)' ', error,
                    error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int append_text_bytes(uint8_t **bytes, size_t *count, size_t *capacity,
                             const char *text, char *error,
                             size_t error_capacity)
{
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        if (append_byte(bytes, count, capacity, (uint8_t)text[index], error,
                        error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}

static int append_token_description(struct hstex_engine *engine,
                                    hstex_token token, uint8_t **bytes,
                                    size_t *count, size_t *capacity, char *error,
                                    size_t error_capacity)
{
    token = normalize_frozen_control_sequence(token);
    if (hstex_token_is_character(token)) {
        uint8_t character = hstex_token_character_code(token);
        /* A character token of the parameter category is displayed doubled,
           so that reading the display back yields the same token. */
        if (token_is_category(token, HSTEX_CAT_PARAMETER) &&
            append_byte(bytes, count, capacity, character, error,
                        error_capacity) != 0) {
            return -1;
        }
        return append_byte(bytes, count, capacity, character, error,
                           error_capacity);
    }
    if (hstex_token_is_parameter(token)) {
        if (append_byte(bytes, count, capacity, (uint8_t)'#', error,
                        error_capacity) != 0) {
            return -1;
        }
        return append_byte(bytes, count, capacity,
                           (uint8_t)('0' +
                                     hstex_token_parameter_number(token)),
                           error, error_capacity);
    }
    if (hstex_token_is_control_sequence(token)) {
        return serialize_control_sequence(engine, token, bytes, count, capacity,
                                          true, error, error_capacity);
    }
    return set_error(error, error_capacity,
                     "internal token in meaning description");
}

/* \string makes tokens, and a token list is never written in ^^ notation:
   the reference reserves that for what it prints to the terminal. So every
   byte goes through as it stands, however unprintable it would look. See
   docs/DECISIONS.md, string-is-not-escaped. */
static int append_string_character(uint8_t **bytes, size_t *count,
                                   size_t *capacity, uint8_t character,
                                   char *error, size_t error_capacity)
{
    return append_byte(bytes, count, capacity, character, error,
                       error_capacity);
}

static int expand_string(struct hstex_engine *engine,
                         struct hstex_source_location location, char *error,
                         size_t error_capacity)
{
    hstex_token subject = 0U;
    struct hstex_source_location subject_location;
    if (raw_next(engine, &subject, &subject_location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input after string");
    }
    (void)subject_location;
    uint8_t *bytes = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    if (hstex_token_is_character(subject)) {
        if (append_string_character(&bytes, &count, &capacity,
                                    hstex_token_character_code(subject), error,
                                    error_capacity) != 0) {
            free(bytes);
            return -1;
        }
    } else if (hstex_token_is_control_sequence(subject)) {
        enum hstex_symbol_kind kind;
        const uint8_t *name = NULL;
        size_t length = 0U;
        if (hstex_symbol_name(&engine->lexical_state.symbols,
                              hstex_token_control_sequence_id(subject), &kind,
                              &name, &length) != 0) {
            free(bytes);
            return set_error(error, error_capacity,
                             "invalid control sequence after string");
        }
        if (kind == HSTEX_SYMBOL_ACTIVE && length == 1U) {
            if (append_string_character(&bytes, &count, &capacity, name[0],
                                        error, error_capacity) != 0) {
                free(bytes);
                return -1;
            }
        } else {
            int32_t escape =
                engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER];
            if (escape >= 0 && escape <= 255 &&
                append_string_character(&bytes, &count, &capacity,
                                        (uint8_t)escape, error,
                                        error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            for (size_t index = 0U; index < length; ++index) {
                if (append_string_character(&bytes, &count, &capacity,
                                            name[index], error,
                                            error_capacity) != 0) {
                    free(bytes);
                    return -1;
                }
            }
        }
    } else {
        free(bytes);
        return set_error(error, error_capacity,
                         "internal token after string");
    }
    struct token_vector expansion = {0};
    for (size_t index = 0U; index < count; ++index) {
        uint8_t category = bytes[index] == (uint8_t)' '
                               ? (uint8_t)HSTEX_CAT_SPACE
                               : (uint8_t)HSTEX_CAT_OTHER;
        if (vector_push(&expansion,
                        hstex_token_character(category, bytes[index]), error,
                        error_capacity) != 0) {
            free(bytes);
            vector_destroy(&expansion);
            return -1;
        }
    }
    free(bytes);
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

/* A control sequence defined by \countdef and friends reports itself as the
   primitive it stands for, so \meaning\scratchcounter is \count298 rather
   than the name; see docs/DECISIONS.md, defined-register-meanings. */
static const char *defined_register_primitive(enum hstex_command command)
{
    switch (command) {
    case HSTEX_COMMAND_COUNT_REGISTER:
        return "count";
    case HSTEX_COMMAND_DIMEN_REGISTER:
        return "dimen";
    case HSTEX_COMMAND_SKIP_REGISTER:
        return "skip";
    case HSTEX_COMMAND_MUSKIP_REGISTER:
        return "muskip";
    case HSTEX_COMMAND_TOKS_REGISTER:
        return "toks";
    case HSTEX_COMMAND_CHAR_GIVEN:
        return "char";
    case HSTEX_COMMAND_MATH_CHAR_GIVEN:
        return "mathchar";
    default:
        return NULL;
    }
}

static int append_defined_register_meaning(struct hstex_engine *engine,
                                           const struct hstex_meaning *meaning,
                                           uint8_t **bytes, size_t *count,
                                           size_t *capacity, char *error,
                                           size_t error_capacity)
{
    const char *primitive = defined_register_primitive(meaning->command);
    char rendered[64];
    /* Character codes are shown in hexadecimal, register numbers in decimal. */
    bool hexadecimal = meaning->command == HSTEX_COMMAND_CHAR_GIVEN ||
                       meaning->command == HSTEX_COMMAND_MATH_CHAR_GIVEN;
    int length = hexadecimal
                     ? snprintf(rendered, sizeof(rendered), "%s\"%" PRIX32,
                                primitive, meaning->value.integer)
                     : snprintf(rendered, sizeof(rendered), "%s%" PRId32,
                                primitive, meaning->value.integer);
    if (length <= 0 || (size_t)length >= sizeof(rendered)) {
        return set_error(error, error_capacity,
                         "could not format a defined register meaning");
    }
    int32_t escape = engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER];
    if (escape >= 0 && escape <= 255 &&
        append_byte(bytes, count, capacity, (uint8_t)escape, error,
                    error_capacity) != 0) {
        return -1;
    }
    return append_text_bytes(bytes, count, capacity, rendered, error,
                             error_capacity);
}

/* What the reference calls a character token: the category has a name of its
   own, and the character follows it. See docs/DECISIONS.md,
   implicit-characters. */
static int append_character_meaning(struct hstex_engine *engine,
                                    hstex_token token, uint8_t **bytes,
                                    size_t *count, size_t *capacity,
                                    char *error, size_t error_capacity)
{
    static const struct {
        uint8_t category;
        const char *name;
    } names[] = {
        {(uint8_t)HSTEX_CAT_BEGIN_GROUP, "begin-group character "},
        {(uint8_t)HSTEX_CAT_END_GROUP, "end-group character "},
        {(uint8_t)HSTEX_CAT_MATH_SHIFT, "math shift character "},
        {(uint8_t)HSTEX_CAT_ALIGNMENT_TAB, "alignment tab character "},
        {(uint8_t)HSTEX_CAT_PARAMETER, "macro parameter character "},
        {(uint8_t)HSTEX_CAT_SUPERSCRIPT, "superscript character "},
        {(uint8_t)HSTEX_CAT_SUBSCRIPT, "subscript character "},
        {(uint8_t)HSTEX_CAT_SPACE, "blank space "},
        {(uint8_t)HSTEX_CAT_LETTER, "the letter "},
        {(uint8_t)HSTEX_CAT_OTHER, "the character "},
    };
    (void)engine;
    uint8_t category = hstex_token_category(token);
    const char *name = "the character ";
    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]);
         ++index) {
        if (names[index].category == category) {
            name = names[index].name;
            break;
        }
    }
    if (append_text_bytes(bytes, count, capacity, name, error,
                          error_capacity) != 0) {
        return -1;
    }
    return append_byte(bytes, count, capacity,
                       hstex_token_character_code(token), error,
                       error_capacity);
}

static int expand_meaning(struct hstex_engine *engine,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity)
{
    hstex_token subject = 0U;
    struct hstex_source_location subject_location;
    if (raw_next(engine, &subject, &subject_location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input after meaning");
    }
    (void)subject_location;
    uint8_t *bytes = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    if (hstex_token_is_character(subject)) {
        if (append_character_meaning(engine, subject, &bytes, &count, &capacity,
                                     error, error_capacity) != 0) {
            free(bytes);
            return -1;
        }
    } else if (hstex_token_is_control_sequence(subject)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(subject));
        /* A control sequence \\let to a character has that character's
           meaning, not a name of its own. */
        if (meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
            hstex_token_is_character(meaning->value.token)) {
            if (append_character_meaning(engine, meaning->value.token, &bytes,
                                         &count, &capacity, error,
                                         error_capacity) != 0) {
                free(bytes);
                return -1;
            }
        } else if (meaning->command == HSTEX_COMMAND_MACRO &&
            meaning->value.macro_identifier != 0U &&
            (size_t)meaning->value.macro_identifier <= engine->macro_count) {
            const struct hstex_macro *macro =
                &engine->macros[meaning->value.macro_identifier - 1U];
            /* The prefixes are named in the order \protected, \long,
               \outer, each behind the escape character and with no
               separator; one space then precedes `macro:'. */
            static const struct {
                uint8_t flag;
                const char *name;
            } prefixes[] = {
                {(uint8_t)HSTEX_MACRO_PROTECTED, "protected"},
                {(uint8_t)HSTEX_MACRO_LONG, "long"},
                {(uint8_t)HSTEX_MACRO_OUTER, "outer"},
            };
            bool prefixed = false;
            for (size_t index = 0U;
                 index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
                if ((macro->flags & prefixes[index].flag) == 0U) {
                    continue;
                }
                int32_t escape =
                    engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER];
                if ((escape >= 0 && escape <= 255 &&
                     append_byte(&bytes, &count, &capacity, (uint8_t)escape,
                                 error, error_capacity) != 0) ||
                    append_text_bytes(&bytes, &count, &capacity,
                                      prefixes[index].name, error,
                                      error_capacity) != 0) {
                    free(bytes);
                    return -1;
                }
                prefixed = true;
            }
            if (prefixed &&
                append_byte(&bytes, &count, &capacity, (uint8_t)' ', error,
                            error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            if (append_text_bytes(&bytes, &count, &capacity, "macro:", error,
                                  error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            for (size_t index = 0U; index < macro->parameter_count_tokens;
                 ++index) {
                if (append_token_description(
                        engine, macro->parameter_text[index], &bytes, &count,
                        &capacity, error, error_capacity) != 0) {
                    free(bytes);
                    return -1;
                }
            }
            if (append_text_bytes(&bytes, &count, &capacity, "->", error,
                                  error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            for (size_t index = 0U; index < macro->replacement_count; ++index) {
                if (append_token_description(
                        engine, macro->replacement[index], &bytes, &count,
                        &capacity, error, error_capacity) != 0) {
                    free(bytes);
                    return -1;
                }
            }
        } else if (meaning->command == HSTEX_COMMAND_FONT_GIVEN) {
            uint32_t identifier = meaning->value.integer > 0
                                      ? (uint32_t)meaning->value.integer
                                      : 0U;
            const struct hstex_font *font =
                font_by_identifier(engine, identifier);
            if (font == NULL || font->name == NULL ||
                append_text_bytes(&bytes, &count, &capacity, "select font ",
                                  error, error_capacity) != 0 ||
                append_text_bytes(&bytes, &count, &capacity, font->name, error,
                                  error_capacity) != 0) {
                free(bytes);
                return font == NULL || font->name == NULL
                           ? set_error(error, error_capacity,
                                       "invalid font meaning")
                           : -1;
            }
            if (font->size != INT32_C(10) * INT32_C(65536)) {
                char size_text[64];
                int size_length = format_scaled_value(
                    font->size, "pt", size_text, sizeof(size_text));
                if (size_length < 0 ||
                    append_text_bytes(&bytes, &count, &capacity, " at ", error,
                                      error_capacity) != 0) {
                    free(bytes);
                    return size_length < 0
                               ? set_error(error, error_capacity,
                                           "could not format font meaning")
                               : -1;
                }
                for (int index = 0; index < size_length; ++index) {
                    if (append_byte(&bytes, &count, &capacity,
                                    (uint8_t)size_text[index], error,
                                    error_capacity) != 0) {
                        free(bytes);
                        return -1;
                    }
                }
            }
        } else if (meaning->command == HSTEX_COMMAND_UNDEFINED) {
            if (append_text_bytes(&bytes, &count, &capacity, "undefined", error,
                                  error_capacity) != 0) {
                free(bytes);
                return -1;
            }
        } else if (defined_register_primitive(meaning->command) != NULL) {
            if (append_defined_register_meaning(engine, meaning, &bytes, &count,
                                                &capacity, error,
                                                error_capacity) != 0) {
                free(bytes);
                return -1;
            }
        } else if (serialize_control_sequence(
                       engine,
                       meaning->primitive_origin == 0U
                           ? subject
                           : hstex_token_control_sequence(
                                 meaning->primitive_origin),
                       &bytes, &count, &capacity, false, error,
                                              error_capacity) != 0) {
            free(bytes);
            return -1;
        }
    } else {
        free(bytes);
        return set_error(error, error_capacity,
                         "internal token after meaning");
    }

    struct token_vector expansion = {0};
    for (size_t index = 0U; index < count; ++index) {
        uint8_t category = bytes[index] == (uint8_t)' '
                               ? (uint8_t)HSTEX_CAT_SPACE
                               : (uint8_t)HSTEX_CAT_OTHER;
        if (vector_push(&expansion,
                        hstex_token_character(category, bytes[index]), error,
                        error_capacity) != 0) {
            free(bytes);
            vector_destroy(&expansion);
            return -1;
        }
    }
    free(bytes);
    if (push_owned_vector(engine, &expansion, location, error, error_capacity) !=
        0) {
        vector_destroy(&expansion);
        return -1;
    }
    return 0;
}

static int scan_expanded_general_text(struct hstex_engine *engine,
                                      uint8_t **bytes, size_t *byte_count,
                                      char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space_unrestricted(
            engine, &opening, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "write requires a braced token list");
    }
    struct token_vector text = {0};
    if (scan_balanced_group(engine, &text, true, error, error_capacity) != 0) {
        vector_destroy(&text);
        return -1;
    }
    if (hstex_source_push_boundary(&engine->sources, error, error_capacity) != 0) {
        vector_destroy(&text);
        return -1;
    }
    if (push_owned_vector(engine, &text, location, error, error_capacity) != 0) {
        (void)hstex_source_pop_boundary(&engine->sources, NULL, 0U);
        vector_destroy(&text);
        return -1;
    }

    uint8_t *result = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    for (;;) {
        hstex_token token = 0U;
        bool previous_inhibition = engine->inhibit_protected_expansion;
        engine->inhibit_protected_expansion = true;
        enum hstex_engine_result expansion_result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        engine->inhibit_protected_expansion = previous_inhibition;
        if (expansion_result == HSTEX_ENGINE_EOF) {
            if (hstex_source_pop_boundary(&engine->sources, error,
                                          error_capacity) != 0) {
                free(result);
                return -1;
            }
            break;
        }
        if (expansion_result != HSTEX_ENGINE_TOKEN) {
            (void)hstex_source_pop_boundary(&engine->sources, NULL, 0U);
            free(result);
            return -1;
        }
        if (hstex_token_is_character(token)) {
            uint8_t character = hstex_token_character_code(token);
            /* A parameter-category character is written doubled. */
            if ((token_is_category(token, HSTEX_CAT_PARAMETER) &&
                 append_byte(&result, &count, &capacity, character, error,
                             error_capacity) != 0) ||
                append_byte(&result, &count, &capacity, character, error,
                            error_capacity) != 0) {
                free(result);
                return -1;
            }
        } else if (hstex_token_is_control_sequence(token)) {
            if (serialize_control_sequence(engine, token, &result, &count,
                                           &capacity, true, error,
                                           error_capacity) != 0) {
                free(result);
                return -1;
            }
        } else {
            free(result);
            return set_error(error, error_capacity,
                             "internal token in expanded write text");
        }
    }
    *bytes = result;
    *byte_count = count;
    return 0;
}

static int hex_digit_value(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
        return byte - (uint8_t)'0';
    }
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') {
        return byte - (uint8_t)'a' + 10;
    }
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') {
        return byte - (uint8_t)'A' + 10;
    }
    return -1;
}

/* The four PDF string escapes. \pdfescapestring quotes the bytes PDF string
   syntax reserves, \pdfescapename applies PDF name syntax, and the hex pair
   converts to and from hexadecimal; see docs/DECISIONS.md, pdf-escapes. */
static int expand_pdf_escape(struct hstex_engine *engine,
                             enum pdf_escape_kind kind,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    uint8_t *bytes = NULL;
    size_t byte_count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &byte_count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    static const char hex_digits[] = "0123456789ABCDEF";
    /* Four output bytes per input byte covers the widest escape, \\ooo. */
    char *rendered = malloc(byte_count * 4U + 1U);
    if (rendered == NULL) {
        free(bytes);
        return set_error(error, error_capacity, "escape allocation failed");
    }
    size_t length = 0U;
    int status = 0;
    for (size_t index = 0U; index < byte_count; ++index) {
        uint8_t byte = bytes[index];
        switch (kind) {
        case PDF_ESCAPE_STRING:
            if (byte == (uint8_t)'(' || byte == (uint8_t)')' ||
                byte == (uint8_t)'\\') {
                rendered[length++] = '\\';
                rendered[length++] = (char)byte;
            } else if (byte < 0x21U || byte > 0x7EU) {
                rendered[length++] = '\\';
                rendered[length++] = (char)('0' + ((byte >> 6) & 0x07U));
                rendered[length++] = (char)('0' + ((byte >> 3) & 0x07U));
                rendered[length++] = (char)('0' + (byte & 0x07U));
            } else {
                rendered[length++] = (char)byte;
            }
            break;
        case PDF_ESCAPE_NAME:
            if (byte < 0x21U || byte > 0x7EU || byte == (uint8_t)'#' ||
                byte == (uint8_t)'(' || byte == (uint8_t)')' ||
                byte == (uint8_t)'<' || byte == (uint8_t)'>' ||
                byte == (uint8_t)'[' || byte == (uint8_t)']' ||
                byte == (uint8_t)'{' || byte == (uint8_t)'}' ||
                byte == (uint8_t)'/' || byte == (uint8_t)'%') {
                rendered[length++] = '#';
                rendered[length++] = hex_digits[(byte >> 4) & 0x0FU];
                rendered[length++] = hex_digits[byte & 0x0FU];
            } else {
                rendered[length++] = (char)byte;
            }
            break;
        case PDF_ESCAPE_HEX:
            rendered[length++] = hex_digits[(byte >> 4) & 0x0FU];
            rendered[length++] = hex_digits[byte & 0x0FU];
            break;
        case PDF_UNESCAPE_HEX: {
            int high = hex_digit_value(byte);
            if (high < 0) {
                continue;
            }
            int low = 0;
            while (index + 1U < byte_count) {
                int candidate = hex_digit_value(bytes[index + 1U]);
                ++index;
                if (candidate >= 0) {
                    low = candidate;
                    break;
                }
            }
            rendered[length++] = (char)((high << 4) | low);
            break;
        }
        }
    }
    free(bytes);
    if (status == 0) {
        status = push_other_character_expansion(engine, rendered, length,
                                                location, error,
                                                error_capacity);
    }
    free(rendered);
    return status;
}

/* \pdfglyphtounicode records the mapping the ToUnicode CMap is built from.
   The mapping is stored rather than discarded because extracted text is one
   of the correctness gates. */
static int execute_pdf_glyph_to_unicode(struct hstex_engine *engine,
                                        char *error, size_t error_capacity)
{
    uint8_t *glyph = NULL;
    uint8_t *unicode = NULL;
    size_t glyph_count = 0U;
    size_t unicode_count = 0U;
    if (scan_expanded_general_text(engine, &glyph, &glyph_count, error,
                                   error_capacity) != 0 ||
        scan_expanded_general_text(engine, &unicode, &unicode_count, error,
                                   error_capacity) != 0) {
        free(glyph);
        free(unicode);
        return -1;
    }
    if (engine->glyph_unicode_count == engine->glyph_unicode_capacity) {
        size_t capacity = engine->glyph_unicode_capacity == 0U
                              ? 64U
                              : engine->glyph_unicode_capacity * 2U;
        struct hstex_glyph_unicode *grown = realloc(
            engine->glyph_unicode, capacity * sizeof(*grown));
        if (grown == NULL) {
            free(glyph);
            free(unicode);
            return set_error(error, error_capacity,
                             "glyph-to-unicode allocation failed");
        }
        engine->glyph_unicode = grown;
        engine->glyph_unicode_capacity = capacity;
    }
    char *glyph_text = malloc(glyph_count + 1U);
    char *unicode_text = malloc(unicode_count + 1U);
    if (glyph_text == NULL || unicode_text == NULL) {
        free(glyph);
        free(unicode);
        free(glyph_text);
        free(unicode_text);
        return set_error(error, error_capacity,
                         "glyph-to-unicode allocation failed");
    }
    memcpy(glyph_text, glyph, glyph_count);
    glyph_text[glyph_count] = '\0';
    memcpy(unicode_text, unicode, unicode_count);
    unicode_text[unicode_count] = '\0';
    free(glyph);
    free(unicode);
    engine->glyph_unicode[engine->glyph_unicode_count].glyph = glyph_text;
    engine->glyph_unicode[engine->glyph_unicode_count].unicode = unicode_text;
    ++engine->glyph_unicode_count;
    return 0;
}

static void clear_match_groups(struct hstex_engine *engine)
{
    for (size_t index = 0U; index < engine->match_group_count; ++index) {
        free(engine->match_groups[index].text);
    }
    free(engine->match_groups);
    engine->match_groups = NULL;
    engine->match_group_count = 0U;
}

/* \pdfmatch reports 1 for a match, 0 for no match, and -1 for a pattern the
   matcher rejects; see docs/DECISIONS.md, pdf-match. */
static int expand_pdf_match(struct hstex_engine *engine,
                            struct hstex_source_location location, char *error,
                            size_t error_capacity)
{
    bool ignore_case = false;
    int32_t subcount = HSTEX_DEFAULT_MATCH_SUBCOUNT;
    for (;;) {
        bool matched = false;
        if (try_keyword(engine, "icase", &matched, error, error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            ignore_case = true;
            continue;
        }
        if (try_keyword(engine, "subcount", &matched, error, error_capacity) !=
            0) {
            return -1;
        }
        if (!matched) {
            break;
        }
        if (scan_integer(engine, &subcount, error, error_capacity) != 0) {
            return -1;
        }
        if (subcount < 0) {
            return set_error(error, error_capacity,
                             "pdfmatch subcount must not be negative");
        }
    }

    uint8_t *pattern = NULL;
    uint8_t *subject = NULL;
    size_t pattern_count = 0U;
    size_t subject_count = 0U;
    if (scan_expanded_general_text(engine, &pattern, &pattern_count, error,
                                   error_capacity) != 0 ||
        scan_expanded_general_text(engine, &subject, &subject_count, error,
                                   error_capacity) != 0) {
        free(pattern);
        free(subject);
        return -1;
    }
    char *pattern_text = malloc(pattern_count + 1U);
    char *subject_text = malloc(subject_count + 1U);
    if (pattern_text == NULL || subject_text == NULL) {
        free(pattern);
        free(subject);
        free(pattern_text);
        free(subject_text);
        return set_error(error, error_capacity, "match argument allocation failed");
    }
    memcpy(pattern_text, pattern, pattern_count);
    pattern_text[pattern_count] = '\0';
    memcpy(subject_text, subject, subject_count);
    subject_text[subject_count] = '\0';
    free(pattern);
    free(subject);

    clear_match_groups(engine);
    regex_t compiled;
    int flags = REG_EXTENDED | (ignore_case ? REG_ICASE : 0);
    int32_t result = 1;
    if (regcomp(&compiled, pattern_text, flags) != 0) {
        result = -1;
    } else {
        size_t group_capacity =
            subcount > 0 ? (size_t)subcount : (size_t)1;
        regmatch_t *positions = calloc(group_capacity, sizeof(*positions));
        if (positions == NULL) {
            regfree(&compiled);
            free(pattern_text);
            free(subject_text);
            return set_error(error, error_capacity,
                             "match position allocation failed");
        }
        if (regexec(&compiled, subject_text, group_capacity, positions, 0) !=
            0) {
            result = 0;
        } else if (subcount > 0) {
            engine->match_groups =
                calloc(group_capacity, sizeof(*engine->match_groups));
            if (engine->match_groups == NULL) {
                free(positions);
                regfree(&compiled);
                free(pattern_text);
                free(subject_text);
                return set_error(error, error_capacity,
                                 "match group allocation failed");
            }
            engine->match_group_count = group_capacity;
            for (size_t index = 0U; index < group_capacity; ++index) {
                engine->match_groups[index].offset = -1;
                if (positions[index].rm_so < 0) {
                    continue;
                }
                size_t start = (size_t)positions[index].rm_so;
                size_t length = (size_t)(positions[index].rm_eo -
                                         positions[index].rm_so);
                char *text = malloc(length + 1U);
                if (text == NULL) {
                    free(positions);
                    regfree(&compiled);
                    free(pattern_text);
                    free(subject_text);
                    clear_match_groups(engine);
                    return set_error(error, error_capacity,
                                     "match text allocation failed");
                }
                memcpy(text, subject_text + start, length);
                text[length] = '\0';
                engine->match_groups[index].offset = (int32_t)start;
                engine->match_groups[index].text = text;
            }
        }
        free(positions);
        regfree(&compiled);
    }
    free(pattern_text);
    free(subject_text);
    return push_integer_expansion(engine, result, location, error,
                                  error_capacity);
}

/* \pdflastmatch<n> expands to "<offset>-><text>", and to "-1->" when the
   group did not participate or lies beyond the stored subcount. */
static int expand_pdf_last_match(struct hstex_engine *engine,
                                 struct hstex_source_location location,
                                 char *error, size_t error_capacity)
{
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0) {
        return set_error(error, error_capacity, "bad match number (%d)", index);
    }
    const char *text = "";
    int32_t offset = -1;
    if ((size_t)index < engine->match_group_count &&
        engine->match_groups[(size_t)index].offset >= 0) {
        offset = engine->match_groups[(size_t)index].offset;
        text = engine->match_groups[(size_t)index].text;
    }
    char rendered[64];
    int length = snprintf(rendered, sizeof(rendered), "%" PRId32 "->", offset);
    if (length < 0 || (size_t)length >= sizeof(rendered)) {
        return set_error(error, error_capacity, "could not format last match");
    }
    size_t text_length = strlen(text);
    char *combined = malloc((size_t)length + text_length + 1U);
    if (combined == NULL) {
        return set_error(error, error_capacity, "last-match allocation failed");
    }
    memcpy(combined, rendered, (size_t)length);
    memcpy(combined + length, text, text_length + 1U);
    int status = push_other_character_expansion(
        engine, combined, (size_t)length + text_length, location, error,
        error_capacity);
    free(combined);
    return status;
}

/* \scantokens turns its argument into characters, without expanding it, and
   reads them back as though they came from a file: each line ends with
   \endlinechar and the catcodes in force at that moment apply; see
   docs/DECISIONS.md, scantokens. */
static int expand_scan_tokens(struct hstex_engine *engine,
                              struct hstex_source_location location,
                              char *error, size_t error_capacity)
{
    hstex_token opening = 0U;
    struct hstex_source_location opening_location;
    enum hstex_engine_result opening_result =
        expanded_next_non_space_unrestricted(engine, &opening,
                                             &opening_location, error,
                                             error_capacity);
    if (opening_result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    if (opening_result != HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "scantokens requires a braced token list");
    }
    struct token_vector input = {0};
    if (scan_balanced_group(engine, &input, true, error, error_capacity) != 0) {
        vector_destroy(&input);
        return -1;
    }
    (void)location;

    uint8_t *bytes = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    for (size_t index = 0U; index < input.count; ++index) {
        hstex_token token = normalize_frozen_control_sequence(input.data[index]);
        if (hstex_token_is_character(token)) {
            uint8_t character = hstex_token_character_code(token);
            if ((token_is_category(token, HSTEX_CAT_PARAMETER) &&
                 append_byte(&bytes, &count, &capacity, character, error,
                             error_capacity) != 0) ||
                append_byte(&bytes, &count, &capacity, character, error,
                            error_capacity) != 0) {
                vector_destroy(&input);
                free(bytes);
                return -1;
            }
            continue;
        }
        if (!hstex_token_is_control_sequence(token)) {
            vector_destroy(&input);
            free(bytes);
            return set_error(error, error_capacity,
                             "internal token inside scantokens");
        }
        if (serialize_control_sequence(engine, token, &bytes, &count, &capacity,
                                       true, error, error_capacity) != 0) {
            vector_destroy(&input);
            free(bytes);
            return -1;
        }
    }
    vector_destroy(&input);
    return hstex_source_push_pseudo_file(&engine->sources, bytes, count,
                                         "<scantokens>", error, error_capacity);
}

static int expand_pdf_string_compare(
    struct hstex_engine *engine, struct hstex_source_location location,
    char *error, size_t error_capacity)
{
    uint8_t *left = NULL;
    uint8_t *right = NULL;
    size_t left_count = 0U;
    size_t right_count = 0U;
    if (scan_expanded_general_text(engine, &left, &left_count, error,
                                   error_capacity) != 0 ||
        scan_expanded_general_text(engine, &right, &right_count, error,
                                   error_capacity) != 0) {
        free(left);
        free(right);
        return -1;
    }
    size_t common = left_count < right_count ? left_count : right_count;
    int comparison = common == 0U ? 0 : memcmp(left, right, common);
    int32_t result = 0;
    if (comparison < 0 || (comparison == 0 && left_count < right_count)) {
        result = -1;
    } else if (comparison > 0 ||
               (comparison == 0 && left_count > right_count)) {
        result = 1;
    }
    free(left);
    free(right);
    return push_integer_expansion(engine, result, location, error,
                                  error_capacity);
}

/* An action spec, as \pdfcatalog's openaction and the link and outline
   primitives take it. Nothing acts on it yet; it is scanned so that the
   tokens it covers are consumed exactly, and no more. */
static int scan_pdf_action(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    bool matched = false;
    if (try_keyword(engine, "user", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched) {
        uint8_t *bytes = NULL;
        size_t count = 0U;
        int status = scan_expanded_general_text(engine, &bytes, &count, error,
                                                error_capacity);
        free(bytes);
        return status;
    }
    bool thread = false;
    if (try_keyword(engine, "goto", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (!matched) {
        if (try_keyword(engine, "thread", &thread, error, error_capacity) !=
            0) {
            return -1;
        }
        if (!thread) {
            return set_error(error, error_capacity,
                             "an action specification was expected here");
        }
    }
    bool file = false;
    if (try_keyword(engine, "file", &file, error, error_capacity) != 0) {
        return -1;
    }
    if (file) {
        uint8_t *bytes = NULL;
        size_t count = 0U;
        int status = scan_expanded_general_text(engine, &bytes, &count, error,
                                                error_capacity);
        free(bytes);
        if (status != 0) {
            return -1;
        }
    }
    bool named = false;
    if (try_keyword(engine, "name", &named, error, error_capacity) != 0) {
        return -1;
    }
    if (named) {
        uint8_t *bytes = NULL;
        size_t count = 0U;
        int status = scan_expanded_general_text(engine, &bytes, &count, error,
                                                error_capacity);
        free(bytes);
        if (status != 0) {
            return -1;
        }
    } else {
        bool numbered = false;
        if (try_keyword(engine, "num", &numbered, error, error_capacity) != 0) {
            return -1;
        }
        if (numbered) {
            int32_t number = 0;
            if (scan_integer(engine, &number, error, error_capacity) != 0) {
                return -1;
            }
        } else if (!thread) {
            /* `page <number> <general text>` is the remaining goto form. */
            bool page = false;
            if (try_keyword(engine, "page", &page, error, error_capacity) !=
                0) {
                return -1;
            }
            if (!page) {
                return set_error(error, error_capacity,
                                 "a destination was expected here");
            }
            int32_t number = 0;
            uint8_t *bytes = NULL;
            size_t count = 0U;
            if (scan_integer(engine, &number, error, error_capacity) != 0 ||
                scan_expanded_general_text(engine, &bytes, &count, error,
                                           error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            free(bytes);
        }
    }
    bool window = false;
    if (try_keyword(engine, "newwindow", &window, error, error_capacity) != 0) {
        return -1;
    }
    if (!window &&
        try_keyword(engine, "nonewwindow", &window, error, error_capacity) !=
            0) {
        return -1;
    }
    return 0;
}

/* Copy a scanned general text into an owned string. */
static char *own_general_text(const uint8_t *bytes, size_t count)
{
    char *text = malloc(count + 1U);
    if (text == NULL) {
        return NULL;
    }
    memcpy(text, bytes, count);
    text[count] = '\0';
    return text;
}

/* \pdfcatalog and \pdfinfo each accumulate into one dictionary, so repeated
   uses append rather than replace. */
static int append_pdf_dictionary(uint8_t **buffer, size_t *length,
                                 size_t *capacity, const uint8_t *bytes,
                                 size_t count, char *error,
                                 size_t error_capacity)
{
    if (*length != 0U &&
        append_byte(buffer, length, capacity, (uint8_t)' ', error,
                    error_capacity) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (append_byte(buffer, length, capacity, bytes[index], error,
                        error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}

static int execute_pdf_dictionary(struct hstex_engine *engine, bool catalog,
                                  char *error, size_t error_capacity)
{
    uint8_t *bytes = NULL;
    size_t count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    /* \pdfcatalog may name an action to take when the document opens; it is
       scanned so that execution continues, and recorded with the rest. */
    bool matched = false;
    if (catalog &&
        try_keyword(engine, "openaction", &matched, error, error_capacity) !=
            0) {
        free(bytes);
        return -1;
    }
    if (matched && scan_pdf_action(engine, error, error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    int status = append_pdf_dictionary(
        catalog ? &engine->pdf_catalog : &engine->pdf_info,
        catalog ? &engine->pdf_catalog_length : &engine->pdf_info_length,
        catalog ? &engine->pdf_catalog_capacity : &engine->pdf_info_capacity,
        bytes, count, error, error_capacity);
    free(bytes);
    return status;
}

static int reserve_pdf_objects(struct hstex_engine *engine, size_t required,
                               char *error, size_t error_capacity)
{
    if (required <= engine->pdf_object_capacity) {
        return 0;
    }
    size_t capacity = engine->pdf_object_capacity == 0U
                          ? 16U
                          : engine->pdf_object_capacity * 2U;
    while (capacity < required) {
        capacity *= 2U;
    }
    struct hstex_pdf_object *grown =
        realloc(engine->pdf_objects, capacity * sizeof(*grown));
    if (grown == NULL) {
        return set_error(error, error_capacity, "pdf object table allocation failed");
    }
    engine->pdf_objects = grown;
    engine->pdf_object_capacity = capacity;
    return 0;
}

static struct hstex_pdf_object *pdf_object_by_number(struct hstex_engine *engine,
                                                     int32_t number)
{
    for (size_t index = 0U; index < engine->pdf_object_count; ++index) {
        if (engine->pdf_objects[index].number == number) {
            return &engine->pdf_objects[index];
        }
    }
    return NULL;
}

/* \pdfobj [reserveobjnum | useobjnum <n>] [stream [attr <text>]] <text> */
static int execute_pdf_object(struct hstex_engine *engine, char *error,
                              size_t error_capacity)
{
    bool reserve = false;
    bool use_number = false;
    bool stream = false;
    int32_t number = 0;
    if (try_keyword(engine, "reserveobjnum", &reserve, error, error_capacity) !=
        0) {
        return -1;
    }
    if (!reserve) {
        if (try_keyword(engine, "useobjnum", &use_number, error,
                        error_capacity) != 0) {
            return -1;
        }
        if (use_number &&
            scan_integer(engine, &number, error, error_capacity) != 0) {
            return -1;
        }
        if (try_keyword(engine, "stream", &stream, error, error_capacity) !=
            0) {
            return -1;
        }
    }
    uint8_t *attributes = NULL;
    size_t attribute_count = 0U;
    if (stream) {
        bool matched = false;
        if (try_keyword(engine, "attr", &matched, error, error_capacity) != 0) {
            return -1;
        }
        if (matched && scan_expanded_general_text(engine, &attributes,
                                                  &attribute_count, error,
                                                  error_capacity) != 0) {
            free(attributes);
            return -1;
        }
    }
    uint8_t *content = NULL;
    size_t content_count = 0U;
    if (!reserve && scan_expanded_general_text(engine, &content, &content_count,
                                               error, error_capacity) != 0) {
        free(attributes);
        free(content);
        return -1;
    }

    struct hstex_pdf_object *target = NULL;
    if (use_number) {
        target = pdf_object_by_number(engine, number);
        if (target == NULL) {
            free(attributes);
            free(content);
            return set_error(error, error_capacity,
                             "pdf object %d was never reserved", number);
        }
    } else {
        if (reserve_pdf_objects(engine, engine->pdf_object_count + 1U, error,
                                error_capacity) != 0) {
            free(attributes);
            free(content);
            return -1;
        }
        target = &engine->pdf_objects[engine->pdf_object_count++];
        memset(target, 0, sizeof(*target));
        target->number = ++engine->pdf_object_counter;
    }
    engine->pdf_last[HSTEX_PDF_LAST_OBJECT] = target->number;
    target->reserved = reserve;
    target->stream = stream;
    free(target->attributes);
    free(target->content);
    target->attributes = attributes == NULL
                             ? NULL
                             : own_general_text(attributes, attribute_count);
    target->content =
        content == NULL ? NULL : own_general_text(content, content_count);
    free(attributes);
    free(content);
    return 0;
}

/* \pdfrefobj marks an object as reachable; with no backend there is nothing
   to prune, so the number is only checked. */
static int execute_pdf_ref_object(struct hstex_engine *engine, char *error,
                                  size_t error_capacity)
{
    int32_t number = 0;
    if (scan_integer(engine, &number, error, error_capacity) != 0) {
        return -1;
    }
    if (pdf_object_by_number(engine, number) == NULL) {
        return set_error(error, error_capacity,
                         "pdf object %d does not exist", number);
    }
    return 0;
}

static int execute_pdf_literal(struct hstex_engine *engine, char *error,
                               size_t error_capacity)
{
    int32_t mode = (int32_t)HSTEX_PDF_LITERAL_SET;
    bool matched = false;
    if (try_keyword(engine, "direct", &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (matched) {
        mode = (int32_t)HSTEX_PDF_LITERAL_DIRECT;
    } else {
        if (try_keyword(engine, "page", &matched, error, error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            mode = (int32_t)HSTEX_PDF_LITERAL_PAGE;
        } else {
            if (try_keyword(engine, "shipout", &matched, error,
                            error_capacity) != 0) {
                return -1;
            }
            if (matched) {
                mode = (int32_t)HSTEX_PDF_LITERAL_SHIPOUT;
            }
        }
    }
    uint8_t *content = NULL;
    size_t count = 0U;
    if (scan_expanded_general_text(engine, &content, &count, error,
                                   error_capacity) != 0) {
        free(content);
        return -1;
    }
    if (engine->pdf_literal_count == engine->pdf_literal_capacity) {
        size_t capacity = engine->pdf_literal_capacity == 0U
                              ? 16U
                              : engine->pdf_literal_capacity * 2U;
        struct hstex_pdf_literal *grown =
            realloc(engine->pdf_literals, capacity * sizeof(*grown));
        if (grown == NULL) {
            free(content);
            return set_error(error, error_capacity,
                             "pdf literal table allocation failed");
        }
        engine->pdf_literals = grown;
        engine->pdf_literal_capacity = capacity;
    }
    struct hstex_pdf_literal *literal =
        &engine->pdf_literals[engine->pdf_literal_count];
    literal->mode = mode;
    literal->content = own_general_text(content, count);
    free(content);
    if (literal->content == NULL) {
        return set_error(error, error_capacity,
                         "pdf literal allocation failed");
    }
    ++engine->pdf_literal_count;
    return 0;
}

/* The node most recently contributed to the list being built, or NULL when
   that list is empty; see docs/DECISIONS.md, last-node-queries. */
static const struct hstex_node *current_list_last_node(
    const struct hstex_engine *engine)
{
    const uint32_t *identifiers = NULL;
    size_t count = 0U;
    if (engine->mode == HSTEX_MODE_HORIZONTAL) {
        if (engine->active_hbox_builder == NULL) {
            return NULL;
        }
        identifiers = engine->active_hbox_builder->node_identifiers;
        count = engine->active_hbox_builder->count;
    } else if (engine->mode == HSTEX_MODE_VERTICAL) {
        if (engine->active_vbox_builder == NULL) {
            return NULL;
        }
        identifiers = engine->active_vbox_builder->node_identifiers;
        count = engine->active_vbox_builder->count;
    }
    if (count == 0U || identifiers == NULL) {
        return NULL;
    }
    uint32_t identifier = identifiers[count - 1U];
    if (identifier == 0U || (size_t)identifier > engine->node_count) {
        return NULL;
    }
    return &engine->nodes[identifier - 1U];
}

/* The node type numbers are the ones the reference reports, with -1 for an
   empty list. */
static int32_t last_node_type(const struct hstex_node *node)
{
    if (node == NULL) {
        return -1;
    }
    switch (node->kind) {
    case HSTEX_NODE_CHARACTER:
        return 0;
    case HSTEX_NODE_LIST:
        return node->value.list.box_kind == HSTEX_BOX_VLIST ? 2 : 1;
    case HSTEX_NODE_RULE:
        return 3;
    case HSTEX_NODE_GLUE:
        return 11;
    case HSTEX_NODE_KERN:
        return 12;
    case HSTEX_NODE_LIGATURE:
        return 7;
    case HSTEX_NODE_PENALTY:
        return 13;
    }
    return -1;
}

/* Record something the backend will place. Nothing is written; see
   docs/DECISIONS.md, pdf-annotations. */
static struct hstex_pdf_record *add_pdf_record(struct hstex_engine *engine,
                                               enum hstex_pdf_record_kind kind,
                                               char *error,
                                               size_t error_capacity)
{
    if (engine->pdf_record_count == engine->pdf_record_capacity) {
        size_t capacity = engine->pdf_record_capacity == 0U
                              ? 16U
                              : engine->pdf_record_capacity * 2U;
        struct hstex_pdf_record *grown =
            realloc(engine->pdf_records, capacity * sizeof(*grown));
        if (grown == NULL) {
            (void)set_error(error, error_capacity,
                            "pdf record table allocation failed");
            return NULL;
        }
        engine->pdf_records = grown;
        engine->pdf_record_capacity = capacity;
    }
    struct hstex_pdf_record *record =
        &engine->pdf_records[engine->pdf_record_count++];
    memset(record, 0, sizeof(*record));
    record->kind = kind;
    return record;
}

/* An optional `attr <general text>`, returned as an owned string. */
static int scan_optional_pdf_text(struct hstex_engine *engine,
                                  const char *keyword, char **text,
                                  char *error, size_t error_capacity)
{
    *text = NULL;
    bool matched = false;
    if (try_keyword(engine, keyword, &matched, error, error_capacity) != 0) {
        return -1;
    }
    if (!matched) {
        return 0;
    }
    uint8_t *bytes = NULL;
    size_t count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    *text = own_general_text(bytes, count);
    free(bytes);
    return *text == NULL
               ? set_error(error, error_capacity, "pdf text allocation failed")
               : 0;
}

/* A destination type. The longer names are tried first, because `fitb` is a
   prefix of `fitbh` and `fit` of the rest. */
static int scan_pdf_destination_type(struct hstex_engine *engine, char **type,
                                     char *error, size_t error_capacity)
{
    static const char *const types[] = {"fitbh", "fitbv", "fitbh", "fitb",
                                        "fith",  "fitv",  "fitr",  "xyz",
                                        "fit"};
    for (size_t index = 0U; index < sizeof(types) / sizeof(types[0]); ++index) {
        bool matched = false;
        if (try_keyword(engine, types[index], &matched, error,
                        error_capacity) != 0) {
            return -1;
        }
        if (!matched) {
            continue;
        }
        *type = own_general_text((const uint8_t *)types[index],
                                 strlen(types[index]));
        if (*type == NULL) {
            return set_error(error, error_capacity,
                             "pdf destination allocation failed");
        }
        if (strcmp(types[index], "xyz") == 0) {
            bool zoom = false;
            if (try_keyword(engine, "zoom", &zoom, error, error_capacity) !=
                0) {
                return -1;
            }
            int32_t factor = 0;
            if (zoom &&
                scan_integer(engine, &factor, error, error_capacity) != 0) {
                return -1;
            }
        } else if (strcmp(types[index], "fitr") == 0) {
            struct hstex_node rule = {0};
            if (scan_rule_dimensions(engine, &rule, error, error_capacity) !=
                0) {
                return -1;
            }
        }
        return 0;
    }
    return set_error(error, error_capacity,
                     "a destination type was expected here");
}

static int execute_pdf_dest(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    bool named = false;
    int32_t number = 0;
    char *name = NULL;
    if (try_keyword(engine, "num", &named, error, error_capacity) != 0) {
        return -1;
    }
    if (named) {
        if (scan_integer(engine, &number, error, error_capacity) != 0) {
            return -1;
        }
    } else {
        bool by_name = false;
        if (try_keyword(engine, "name", &by_name, error, error_capacity) != 0) {
            return -1;
        }
        if (!by_name) {
            return set_error(error, error_capacity,
                             "a destination identifier was expected here");
        }
        uint8_t *bytes = NULL;
        size_t count = 0U;
        if (scan_expanded_general_text(engine, &bytes, &count, error,
                                       error_capacity) != 0) {
            free(bytes);
            return -1;
        }
        name = own_general_text(bytes, count);
        free(bytes);
        if (name == NULL) {
            return set_error(error, error_capacity,
                             "pdf destination allocation failed");
        }
    }
    char *type = NULL;
    if (scan_pdf_destination_type(engine, &type, error, error_capacity) != 0) {
        free(name);
        return -1;
    }
    struct hstex_pdf_record *record =
        add_pdf_record(engine, HSTEX_PDF_RECORD_DESTINATION, error,
                       error_capacity);
    if (record == NULL) {
        free(name);
        free(type);
        return -1;
    }
    record->value = number;
    record->name = name;
    record->content = type;
    return 0;
}

static int execute_pdf_start_link(struct hstex_engine *engine, char *error,
                                  size_t error_capacity)
{
    char *attributes = NULL;
    if (scan_optional_pdf_text(engine, "attr", &attributes, error,
                               error_capacity) != 0) {
        return -1;
    }
    if (scan_pdf_action(engine, error, error_capacity) != 0) {
        free(attributes);
        return -1;
    }
    struct hstex_pdf_record *record =
        add_pdf_record(engine, HSTEX_PDF_RECORD_LINK, error, error_capacity);
    if (record == NULL) {
        free(attributes);
        return -1;
    }
    record->number = ++engine->pdf_object_counter;
    engine->pdf_last[HSTEX_PDF_LAST_LINK] = record->number;
    record->content = attributes;
    return 0;
}

static int execute_pdf_outline(struct hstex_engine *engine, char *error,
                               size_t error_capacity)
{
    char *attributes = NULL;
    if (scan_optional_pdf_text(engine, "attr", &attributes, error,
                               error_capacity) != 0) {
        return -1;
    }
    if (scan_pdf_action(engine, error, error_capacity) != 0) {
        free(attributes);
        return -1;
    }
    bool counted = false;
    int32_t count = 0;
    if (try_keyword(engine, "count", &counted, error, error_capacity) != 0 ||
        (counted && scan_integer(engine, &count, error, error_capacity) != 0)) {
        free(attributes);
        return -1;
    }
    uint8_t *title = NULL;
    size_t title_count = 0U;
    if (scan_expanded_general_text(engine, &title, &title_count, error,
                                   error_capacity) != 0) {
        free(attributes);
        free(title);
        return -1;
    }
    struct hstex_pdf_record *record =
        add_pdf_record(engine, HSTEX_PDF_RECORD_OUTLINE, error, error_capacity);
    if (record == NULL) {
        free(attributes);
        free(title);
        return -1;
    }
    record->value = count;
    record->name = attributes;
    record->content = own_general_text(title, title_count);
    free(title);
    return 0;
}

static int execute_pdf_xform(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    char *attributes = NULL;
    char *resources = NULL;
    int32_t index = 0;
    if (scan_optional_pdf_text(engine, "attr", &attributes, error,
                               error_capacity) != 0) {
        return -1;
    }
    if (scan_optional_pdf_text(engine, "resources", &resources, error,
                               error_capacity) != 0) {
        free(attributes);
        return -1;
    }
    if (scan_integer(engine, &index, error, error_capacity) != 0 || index < 0 ||
        (size_t)index >= engine->count_capacity) {
        free(attributes);
        free(resources);
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    struct hstex_pdf_record *record =
        add_pdf_record(engine, HSTEX_PDF_RECORD_FORM, error, error_capacity);
    if (record == NULL) {
        free(attributes);
        free(resources);
        return -1;
    }
    /* A form takes two object numbers: the form itself and its resources. */
    record->number = ++engine->pdf_object_counter;
    ++engine->pdf_object_counter;
    engine->pdf_last[HSTEX_PDF_LAST_FORM] = record->number;
    record->value = index;
    record->name = attributes;
    record->content = resources;
    return 0;
}

static int execute_pdf_annot(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    struct hstex_node rule = {0};
    if (scan_rule_dimensions(engine, &rule, error, error_capacity) != 0) {
        return -1;
    }
    bool use_number = false;
    int32_t number = 0;
    if (try_keyword(engine, "useobjnum", &use_number, error, error_capacity) !=
            0 ||
        (use_number &&
         scan_integer(engine, &number, error, error_capacity) != 0)) {
        return -1;
    }
    uint8_t *bytes = NULL;
    size_t count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    struct hstex_pdf_record *record =
        add_pdf_record(engine, HSTEX_PDF_RECORD_ANNOTATION, error,
                       error_capacity);
    if (record == NULL) {
        free(bytes);
        return -1;
    }
    record->number = use_number ? number : ++engine->pdf_object_counter;
    engine->pdf_last[HSTEX_PDF_LAST_ANNOTATION] = record->number;
    record->content = own_general_text(bytes, count);
    free(bytes);
    return 0;
}

static int reserve_color_stacks(struct hstex_engine *engine, size_t required,
                                char *error, size_t error_capacity)
{
    if (required <= engine->color_stack_capacity) {
        return 0;
    }
    size_t capacity = engine->color_stack_capacity == 0U
                          ? 8U
                          : engine->color_stack_capacity * 2U;
    while (capacity < required) {
        capacity *= 2U;
    }
    struct hstex_color_stack *grown =
        realloc(engine->color_stacks, capacity * sizeof(*grown));
    if (grown == NULL) {
        return set_error(error, error_capacity,
                         "colour stack table allocation failed");
    }
    memset(grown + engine->color_stack_capacity, 0,
           (capacity - engine->color_stack_capacity) * sizeof(*grown));
    engine->color_stacks = grown;
    engine->color_stack_capacity = capacity;
    return 0;
}

/* \pdfcolorstackinit expands to the number of the stack it makes. Stack zero
   is the page's own, so the first one made is stack one. */
static int expand_pdf_color_stack_init(struct hstex_engine *engine,
                                       struct hstex_source_location location,
                                       char *error, size_t error_capacity)
{
    bool page = false;
    bool direct = false;
    if (try_keyword(engine, "page", &page, error, error_capacity) != 0 ||
        try_keyword(engine, "direct", &direct, error, error_capacity) != 0) {
        return -1;
    }
    uint8_t *bytes = NULL;
    size_t count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    /* The built-in page stack occupies slot zero. */
    if (engine->color_stack_count == 0U) {
        if (reserve_color_stacks(engine, 1U, error, error_capacity) != 0) {
            free(bytes);
            return -1;
        }
        engine->color_stack_count = 1U;
    }
    if (reserve_color_stacks(engine, engine->color_stack_count + 1U, error,
                             error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    struct hstex_color_stack *stack =
        &engine->color_stacks[engine->color_stack_count];
    memset(stack, 0, sizeof(*stack));
    stack->page = page;
    stack->direct = direct;
    stack->initial = own_general_text(bytes, count);
    free(bytes);
    if (stack->initial == NULL) {
        return set_error(error, error_capacity,
                         "colour stack allocation failed");
    }
    int32_t number = (int32_t)engine->color_stack_count++;
    return push_integer_expansion(engine, number, location, error,
                                  error_capacity);
}

/* \pdfcolorstack <number> (push <text> | pop | set <text> | current) */
static int execute_pdf_color_stack(struct hstex_engine *engine, char *error,
                                   size_t error_capacity)
{
    int32_t number = 0;
    if (scan_integer(engine, &number, error, error_capacity) != 0) {
        return -1;
    }
    if (number < 0) {
        return set_error(error, error_capacity, "invalid colour stack %d",
                         number);
    }
    if ((size_t)number >= engine->color_stack_count) {
        /* Stack zero exists without being made. */
        if (number != 0 ||
            reserve_color_stacks(engine, 1U, error, error_capacity) != 0) {
            return set_error(error, error_capacity,
                             "colour stack %d does not exist", number);
        }
        engine->color_stack_count = 1U;
    }
    struct hstex_color_stack *stack = &engine->color_stacks[(size_t)number];
    static const char *const actions[] = {"push", "pop", "set", "current"};
    size_t action = 0U;
    bool matched = false;
    for (; action < sizeof(actions) / sizeof(actions[0]); ++action) {
        if (try_keyword(engine, actions[action], &matched, error,
                        error_capacity) != 0) {
            return -1;
        }
        if (matched) {
            break;
        }
    }
    if (!matched) {
        return set_error(error, error_capacity,
                         "a colour stack action was expected here");
    }
    if (action == 1U) { /* pop */
        if (stack->count != 0U) {
            free(stack->values[--stack->count]);
        }
        return 0;
    }
    if (action == 3U) { /* current: re-states the value, changing nothing */
        return 0;
    }
    uint8_t *bytes = NULL;
    size_t count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    char *value = own_general_text(bytes, count);
    free(bytes);
    if (value == NULL) {
        return set_error(error, error_capacity,
                         "colour stack allocation failed");
    }
    if (action == 2U) { /* set: replaces the top rather than growing */
        if (stack->count != 0U) {
            free(stack->values[stack->count - 1U]);
            stack->values[stack->count - 1U] = value;
            return 0;
        }
    }
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity == 0U ? 8U : stack->capacity * 2U;
        char **grown = realloc(stack->values, capacity * sizeof(*grown));
        if (grown == NULL) {
            free(value);
            return set_error(error, error_capacity,
                             "colour stack allocation failed");
        }
        stack->values = grown;
        stack->capacity = capacity;
    }
    stack->values[stack->count++] = value;
    return 0;
}

/* Unboxing appends a box's own list to the one being built and discards the
   box itself; the copying forms leave the register alone. No interline glue
   is inserted, since the list already carries its own; see
   docs/DECISIONS.md, unboxing. */
static int execute_unbox(struct hstex_engine *engine, int32_t subtype,
                         char *error, size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "unboxing does not accept prefixes");
    }
    bool vertical = subtype == (int32_t)HSTEX_UNBOX_VERTICAL ||
                    subtype == (int32_t)HSTEX_UNBOX_VERTICAL_COPY;
    bool keep = subtype == (int32_t)HSTEX_UNBOX_HORIZONTAL_COPY ||
                subtype == (int32_t)HSTEX_UNBOX_VERTICAL_COPY;
    /* \unhbox is allowed in a formula, where LaTeX's \leavevmode reaches it
       as \unhbox\voidb@x; only a void register may be unboxed there. See
       docs/DECISIONS.md, unboxing-in-a-formula. */
    bool formula = !vertical && engine->mode == HSTEX_MODE_MATH;
    if (!vertical && !formula &&
        ensure_horizontal_mode(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (!formula && (vertical ? engine->mode != HSTEX_MODE_VERTICAL
                              : engine->mode != HSTEX_MODE_HORIZONTAL)) {
        return set_error(error, error_capacity,
                         vertical ? "unvbox requires vertical mode"
                                  : "unhbox requires horizontal mode");
    }
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0) {
        return -1;
    }
    if (index < 0 || (size_t)index >= engine->count_capacity) {
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    struct hstex_box box = engine->boxes[(size_t)index];
    if (box.kind == HSTEX_BOX_VOID) {
        return 0;
    }
    if (formula) {
        return set_error(error, error_capacity,
                         "box %d cannot be unboxed into a formula", index);
    }
    enum hstex_box_kind wanted =
        vertical ? HSTEX_BOX_VLIST : HSTEX_BOX_HLIST;
    if (box.kind != wanted) {
        return set_error(error, error_capacity,
                         "box %d cannot be unboxed into this list", index);
    }
    for (uint32_t offset = 0U; offset < box.node_count; ++offset) {
        size_t slot = (size_t)box.node_start + offset;
        if (slot >= engine->list_item_count) {
            return set_error(error, error_capacity,
                             "box %d refers outside the list arena", index);
        }
        uint32_t identifier = engine->list_items[slot];
        int status = vertical
                         ? append_vbox_item(engine, identifier, error,
                                            error_capacity)
                         : append_hbox_item(engine, identifier, error,
                                            error_capacity);
        if (status != 0) {
            return -1;
        }
    }
    if (keep) {
        return 0;
    }
    struct hstex_box empty = {0};
    /* Emptying a register outlives the group it happened in. */
    empty.kind = HSTEX_BOX_VOID;
    return assign_box(engine, (uint32_t)index, empty, true, error,
                      error_capacity);
}

/* \indent puts an empty box of width \parindent in the horizontal list;
   \noindent puts nothing. From vertical mode either starts a paragraph, which
   needs the paragraph builder; see docs/DECISIONS.md, indentation. */
static int execute_indent(struct hstex_engine *engine, bool indent,
                          char *error, size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "indentation does not accept prefixes");
    }
    if (engine->mode == HSTEX_MODE_VERTICAL) {
        return start_paragraph(engine, indent, error, error_capacity);
    }
    if (engine->mode != HSTEX_MODE_HORIZONTAL) {
        return set_error(error, error_capacity,
                         "indentation is not supported in math mode");
    }
    if (!indent) {
        return 0;
    }
    struct hstex_box box = {0};
    box.kind = HSTEX_BOX_HLIST;
    box.width = engine->dimen_parameters[HSTEX_DIMEN_PAR_INDENT];
    return append_box_node(engine, &box, error, error_capacity);
}

/* Walk a font's ligature and kerning program for a pair of characters. On a
   kern, *kern receives the amount; on a ligature, *ligature receives the
   replacement. Only the plain `=:' ligature is handled — the seven variants
   that keep one or both originals are refused rather than guessed; see
   docs/DECISIONS.md, ligatures-and-kerning. */
static int font_lig_kern(const struct hstex_font *font, uint8_t left,
                         uint8_t right, bool *kerned, int32_t *kern,
                         bool *ligatured, uint8_t *ligature, char *error,
                         size_t error_capacity)
{
    *kerned = false;
    *ligatured = false;
    if (font->characters == NULL || font->lig_kern_count == 0U) {
        return 0;
    }
    const struct hstex_char_metric *metric = &font->characters[left];
    if (metric->tag != 1) {
        return 0;
    }
    size_t step = (size_t)metric->remainder;
    if (step >= font->lig_kern_count) {
        return 0;
    }
    /* A first step that skips more than the table is a long jump to the real
       start of the program. */
    if (font->lig_kern[step].skip > 128U) {
        step = (size_t)font->lig_kern[step].operation * 256U +
               (size_t)font->lig_kern[step].remainder;
        if (step >= font->lig_kern_count) {
            return 0;
        }
    }
    for (;;) {
        const struct hstex_lig_kern *entry = &font->lig_kern[step];
        if (entry->skip <= 128U && entry->next == right) {
            if (entry->operation >= 128U) {
                size_t index = (size_t)(entry->operation - 128U) * 256U +
                               (size_t)entry->remainder;
                if (index >= font->kern_count) {
                    return set_error(error, error_capacity,
                                     "font kern index is out of range");
                }
                *kerned = true;
                *kern = font->kerns[index];
                return 0;
            }
            if (entry->operation != 0U) {
                return set_error(error, error_capacity,
                                 "ligature operation %u is not implemented",
                                 (unsigned int)entry->operation);
            }
            *ligatured = true;
            *ligature = entry->remainder;
            return 0;
        }
        if (entry->skip >= 128U) {
            return 0;
        }
        step += (size_t)entry->skip + 1U;
        if (step >= font->lig_kern_count) {
            return 0;
        }
    }
}

/* A character sets the space factor from its \sfcode, which the next space
   then uses. */
static void advance_space_factor(struct hstex_engine *engine, uint8_t code)
{
    int32_t factor = engine->code_tables[0][code];
    if (factor == 1000) {
        engine->space_factor = 1000;
    } else if (factor == 0) {
        return;
    } else if (factor < 1000) {
        engine->space_factor = factor;
    } else {
        engine->space_factor =
            engine->space_factor < 1000 ? 1000 : factor;
    }
}

/* Interword glue: the font's own, with the stretch scaled by the space factor
   and the shrink scaled against it. */
static int append_interword_glue(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    const struct hstex_font *font =
        font_by_identifier(engine, engine->current_font);
    if (font == NULL || font->dimen_count < 4U) {
        return set_error(error, error_capacity,
                         "current font does not define interword spacing");
    }
    struct hstex_glue glue = engine->glue_parameters[HSTEX_GLUE_SPACE_SKIP];
    if (glue.width == 0 && glue.stretch == 0 && glue.shrink == 0) {
        int32_t factor = engine->space_factor;
        glue.width = font->dimens[1];
        glue.stretch = font->dimens[2];
        glue.shrink = font->dimens[3];
        if (factor >= 2000 && font->dimen_count >= 7U) {
            glue.width += font->dimens[6];
        }
        if (factor != 1000 && factor > 0) {
            glue.stretch =
                (int32_t)(((int64_t)glue.stretch * factor) / 1000);
            glue.shrink =
                (int32_t)(((int64_t)glue.shrink * 1000) / factor);
        }
    }
    struct hstex_node node = {
        .kind = HSTEX_NODE_GLUE,
        .width = glue.width,
        .value.glue = {
            .stretch = glue.stretch,
            .shrink = glue.shrink,
            .stretch_order = glue.stretch_order,
            .shrink_order = glue.shrink_order,
        },
    };
    return append_hbox_node(engine, &node, error, error_capacity);
}

/* Put a character, or a ligature standing for several, into the list. */
static int append_character_node(struct hstex_engine *engine, uint8_t code,
                                 bool ligature, char *error,
                                 size_t error_capacity)
{
    const struct hstex_font *font =
        font_by_identifier(engine, engine->current_font);
    if (font == NULL || font->characters == NULL ||
        font->characters[code].tag < 0) {
        /* A character the font does not define contributes nothing at all,
           though it has already moved the space factor; see
           docs/DECISIONS.md, missing-characters. */
        return 0;
    }
    const struct hstex_char_metric *metric = &font->characters[code];
    struct hstex_node node = {
        .kind = ligature ? HSTEX_NODE_LIGATURE : HSTEX_NODE_CHARACTER,
        .width = metric->width,
        .height = metric->height,
        .depth = metric->depth,
        .value.character = {
            .font = engine->current_font,
            .character = code,
        },
    };
    return append_hbox_node(engine, &node, error, error_capacity);
}

/* Put the held-back character into the list. Everything that is not another
   character does this first, so a command between two characters breaks the
   pair exactly as it does in the reference. */
static int flush_pending_character(struct hstex_engine *engine, char *error,
                                   size_t error_capacity)
{
    if (!engine->has_pending_character) {
        return 0;
    }
    uint8_t code = engine->pending_character;
    bool ligature = engine->pending_is_ligature;
    engine->has_pending_character = false;
    int status = append_character_node(engine, code, ligature, error,
                                       error_capacity);
    advance_space_factor(engine, code);
    return status;
}

/* Take one character into the horizontal list, consulting the font's
   ligature and kerning program against the character held back before it. */
static int append_horizontal_character(struct hstex_engine *engine,
                                       uint8_t code, char *error,
                                       size_t error_capacity)
{
    if (engine->has_pending_character) {
        const struct hstex_font *font =
            font_by_identifier(engine, engine->current_font);
        bool kerned = false;
        bool ligatured = false;
        int32_t kern = 0;
        uint8_t ligature = 0U;
        if (font != NULL &&
            font_lig_kern(font, engine->pending_character, code, &kerned,
                          &kern, &ligatured, &ligature, error,
                          error_capacity) != 0) {
            return -1;
        }
        if (ligatured) {
            /* The pair becomes one character, which may ligature again. */
            engine->pending_character = ligature;
            engine->pending_is_ligature = true;
            return 0;
        }
        if (flush_pending_character(engine, error, error_capacity) != 0) {
            return -1;
        }
        if (kerned) {
            struct hstex_node node = {
                .kind = HSTEX_NODE_KERN,
                .width = kern,
            };
            if (append_hbox_node(engine, &node, error, error_capacity) != 0) {
                return -1;
            }
        }
    }
    engine->has_pending_character = true;
    engine->pending_is_ligature = false;
    engine->pending_character = code;
    return 0;
}

/* Begin a paragraph: the vertical list gets \parskip if it has anything in
   it, and the horizontal list gets the indentation and \everypar; see
   docs/DECISIONS.md, paragraphs. */
static int start_paragraph(struct hstex_engine *engine, bool indent,
                           char *error, size_t error_capacity)
{
    if (engine->paragraph_builder == NULL) {
        engine->paragraph_builder =
            calloc(1U, sizeof(*engine->paragraph_builder));
        if (engine->paragraph_builder == NULL) {
            return set_error(error, error_capacity,
                             "paragraph list allocation failed");
        }
    }
    if (engine->active_vbox_builder != NULL &&
        engine->active_vbox_builder->count != 0U) {
        struct hstex_glue skip = engine->glue_parameters[HSTEX_GLUE_PAR_SKIP];
        struct hstex_node node = {
            .kind = HSTEX_NODE_GLUE,
            .width = skip.width,
            .value.glue = {
                .stretch = skip.stretch,
                .shrink = skip.shrink,
                .stretch_order = skip.stretch_order,
                .shrink_order = skip.shrink_order,
            },
        };
        if (append_vbox_node(engine, &node, error, error_capacity) != 0) {
            return -1;
        }
    }
    engine->paragraph_builder->count = 0U;
    engine->paragraph_builder->width = 0;
    engine->paragraph_builder->height = 0;
    engine->paragraph_builder->depth = 0;
    engine->active_hbox_builder = engine->paragraph_builder;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    engine->inner_mode = false;
    engine->building_paragraph = true;
    engine->space_factor = 1000;
    engine->has_pending_character = false;
    if (indent) {
        struct hstex_box box = {0};
        box.kind = HSTEX_BOX_HLIST;
        box.width = engine->dimen_parameters[HSTEX_DIMEN_PAR_INDENT];
        if (append_box_node(engine, &box, error, error_capacity) != 0) {
            return -1;
        }
    }
    uint32_t every_par = engine->token_parameters[HSTEX_TOKEN_EVERY_PAR];
    if (every_par == 0U) {
        return 0;
    }
    const struct hstex_token_list *list =
        token_list_by_identifier(engine, every_par);
    struct hstex_source_location location = {0};
    if (list == NULL ||
        hstex_source_push_tokens(&engine->sources, list->tokens, list->count,
                                 location, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "could not install everypar tokens");
    }
    return 0;
}

/* End a paragraph: the list is finished off and broken into lines, which join
   the vertical list. Only a paragraph that fits on one line is handled. */
/* ---------------------------------------------------------------------- */
/* Breaking a paragraph into lines.                                       */
/*                                                                        */
/* The reference's optimal-fit method: every legal breakpoint is tried    */
/* against every break that could still start the line, and the sequence  */
/* with the fewest demerits wins. See docs/DECISIONS.md, line-breaking.   */
/* ---------------------------------------------------------------------- */

#define HSTEX_AWFUL_BADNESS INT64_C(0x3FFFFFFF)
#define HSTEX_EJECT_PENALTY (-10000)

enum hstex_fitness {
    HSTEX_FIT_VERY_LOOSE = 0,
    HSTEX_FIT_LOOSE,
    HSTEX_FIT_DECENT,
    HSTEX_FIT_TIGHT,
    HSTEX_FIT_COUNT,
};

/* Everything a line's measurement needs, summed over the nodes before a
   point so that any line's totals are one subtraction apart. */
struct hstex_break_totals {
    int64_t width;
    int64_t stretch[4];
    int64_t shrink;
};

/* One break that a later line could start from. */
struct hstex_break_record {
    size_t breakpoint;
    size_t start;
    int32_t line;
    uint8_t fitness;
    int64_t demerits;
    size_t previous;
};

struct hstex_break_state {
    struct hstex_break_totals *totals;
    size_t node_count;
    struct hstex_break_record *records;
    size_t record_count;
    size_t record_capacity;
    size_t *active;
    size_t active_count;
    size_t active_capacity;
};

static int emit_math_glue(struct hstex_engine *engine, struct hstex_glue glue,
                          char *error, size_t error_capacity);

static bool node_is_discardable(const struct hstex_node *node)
{
    switch (node->kind) {
    case HSTEX_NODE_GLUE:
    case HSTEX_NODE_PENALTY:
        return true;
    case HSTEX_NODE_KERN:
        return !node->explicit_kern;
    default:
        return false;
    }
}

/* Where the line after a break really begins: the reference throws away the
   glue, penalties and engine-made kerns that follow it. */
static size_t line_start_after(const struct hstex_engine *engine,
                               const uint32_t *items, size_t count,
                               size_t breakpoint)
{
    size_t index = breakpoint;
    while (index < count) {
        uint32_t identifier = items[index];
        if (identifier == 0U || (size_t)identifier > engine->node_count) {
            break;
        }
        if (!node_is_discardable(&engine->nodes[identifier - 1U])) {
            break;
        }
        ++index;
    }
    return index;
}

static int reserve_break_records(struct hstex_break_state *state, char *error,
                                 size_t error_capacity)
{
    if (state->record_count < state->record_capacity) {
        return 0;
    }
    size_t next = state->record_capacity == 0U ? 32U : state->record_capacity * 2U;
    if (next > SIZE_MAX / sizeof(*state->records)) {
        return set_error(error, error_capacity, "break record overflow");
    }
    void *allocation = realloc(state->records, next * sizeof(*state->records));
    if (allocation == NULL) {
        return set_error(error, error_capacity, "break record allocation failed");
    }
    state->records = allocation;
    state->record_capacity = next;
    return 0;
}

static int reserve_active_breaks(struct hstex_break_state *state, char *error,
                                 size_t error_capacity)
{
    if (state->active_count < state->active_capacity) {
        return 0;
    }
    size_t next = state->active_capacity == 0U ? 16U : state->active_capacity * 2U;
    if (next > SIZE_MAX / sizeof(*state->active)) {
        return set_error(error, error_capacity, "active break overflow");
    }
    void *allocation = realloc(state->active, next * sizeof(*state->active));
    if (allocation == NULL) {
        return set_error(error, error_capacity, "active break allocation failed");
    }
    state->active = allocation;
    state->active_capacity = next;
    return 0;
}

/* The badness and fitness of a line that runs short or long by `shortfall`,
   with `totals` the glue it has available. */
static int32_t line_badness(int64_t shortfall,
                            const struct hstex_break_totals *totals,
                            uint8_t *fitness)
{
    if (shortfall > 0) {
        if (totals->stretch[1] != 0 || totals->stretch[2] != 0 ||
            totals->stretch[3] != 0) {
            *fitness = (uint8_t)HSTEX_FIT_DECENT;
            return 0;
        }
        if (shortfall > INT64_C(7230584) && totals->stretch[0] < INT64_C(1663497)) {
            *fitness = (uint8_t)HSTEX_FIT_VERY_LOOSE;
            return HSTEX_INFINITE_BADNESS;
        }
        int32_t badness =
            glue_badness((int32_t)shortfall, (int32_t)totals->stretch[0]);
        *fitness = badness > 99   ? (uint8_t)HSTEX_FIT_VERY_LOOSE
                   : badness > 12 ? (uint8_t)HSTEX_FIT_LOOSE
                                  : (uint8_t)HSTEX_FIT_DECENT;
        return badness;
    }
    if (-shortfall > totals->shrink) {
        *fitness = (uint8_t)HSTEX_FIT_TIGHT;
        return HSTEX_INFINITE_BADNESS + 1;
    }
    int32_t badness =
        glue_badness((int32_t)-shortfall, (int32_t)totals->shrink);
    *fitness = badness > 12 ? (uint8_t)HSTEX_FIT_TIGHT
                            : (uint8_t)HSTEX_FIT_DECENT;
    return badness;
}

/* Try every active break as the start of a line ending here. */
static int try_break_at(struct hstex_engine *engine,
                        struct hstex_break_state *state,
                        const uint32_t *items, size_t count,
                        const struct hstex_break_totals *background,
                        int32_t line_width, size_t breakpoint,
                        int32_t penalty, int32_t threshold, bool final_pass,
                        char *error, size_t error_capacity)
{
    int64_t minimal[HSTEX_FIT_COUNT];
    size_t best[HSTEX_FIT_COUNT];
    int32_t best_line[HSTEX_FIT_COUNT];
    for (size_t fit = 0U; fit < (size_t)HSTEX_FIT_COUNT; ++fit) {
        minimal[fit] = HSTEX_AWFUL_BADNESS;
        best[fit] = SIZE_MAX;
        best_line[fit] = 0;
    }
    int64_t minimum = HSTEX_AWFUL_BADNESS;
    int32_t line_penalty = engine->integer_parameters[HSTEX_INTEGER_LINE_PENALTY];
    int32_t adjacent = engine->integer_parameters[HSTEX_INTEGER_ADJ_DEMERITS];

    size_t kept = 0U;
    for (size_t slot = 0U; slot < state->active_count; ++slot) {
        size_t index = state->active[slot];
        const struct hstex_break_record *record = &state->records[index];
        struct hstex_break_totals totals = *background;
        totals.width += state->totals[breakpoint].width -
                        state->totals[record->start].width;
        for (size_t order = 0U; order < 4U; ++order) {
            totals.stretch[order] += state->totals[breakpoint].stretch[order] -
                                     state->totals[record->start].stretch[order];
        }
        totals.shrink += state->totals[breakpoint].shrink -
                         state->totals[record->start].shrink;
        uint8_t fitness = (uint8_t)HSTEX_FIT_DECENT;
        int32_t badness =
            line_badness((int64_t)line_width - totals.width, &totals, &fitness);

        bool artificial = false;
        bool stays_active = true;
        if (badness > HSTEX_INFINITE_BADNESS || penalty == HSTEX_EJECT_PENALTY) {
            if (final_pass && minimum == HSTEX_AWFUL_BADNESS &&
                slot + 1U == state->active_count && kept == 0U) {
                artificial = true;
            } else if (badness > threshold) {
                continue; /* this break can never start a line again */
            }
            stays_active = false;
        } else {
            if (badness > threshold) {
                state->active[kept++] = index;
                continue;
            }
        }

        int64_t demerits;
        if (artificial) {
            demerits = 0;
        } else {
            int64_t scaled = (int64_t)line_penalty + badness;
            demerits = scaled >= HSTEX_INFINITE_BADNESS ||
                               scaled <= -HSTEX_INFINITE_BADNESS
                           ? INT64_C(100000000)
                           : scaled * scaled;
            if (penalty != 0) {
                if (penalty > 0) {
                    demerits += (int64_t)penalty * penalty;
                } else if (penalty > HSTEX_EJECT_PENALTY) {
                    demerits -= (int64_t)penalty * penalty;
                }
            }
            int fit_gap = (int)fitness - (int)record->fitness;
            if (fit_gap > 1 || fit_gap < -1) {
                demerits += adjacent;
            }
        }
        demerits += record->demerits;
        if (demerits <= minimal[fitness]) {
            minimal[fitness] = demerits;
            best[fitness] = index;
            best_line[fitness] = record->line;
            if (demerits < minimum) {
                minimum = demerits;
            }
        }
        if (stays_active) {
            state->active[kept++] = index;
        }
    }
    state->active_count = kept;

    if (minimum == HSTEX_AWFUL_BADNESS) {
        return 0;
    }
    int64_t ceiling = minimum + (adjacent < 0 ? -(int64_t)adjacent : adjacent);
    size_t start = line_start_after(engine, items, count, breakpoint);
    for (size_t fit = 0U; fit < (size_t)HSTEX_FIT_COUNT; ++fit) {
        if (minimal[fit] > ceiling) {
            continue;
        }
        if (reserve_break_records(state, error, error_capacity) != 0 ||
            reserve_active_breaks(state, error, error_capacity) != 0) {
            return -1;
        }
        struct hstex_break_record *record = &state->records[state->record_count];
        record->breakpoint = breakpoint;
        record->start = start;
        record->line = best_line[fit] + 1;
        record->fitness = (uint8_t)fit;
        record->demerits = minimal[fit];
        record->previous = best[fit];
        state->active[state->active_count++] = state->record_count;
        ++state->record_count;
    }
    return 0;
}

/* The indent and length \parshape gives a line, the last pair standing for
   every line after it. See docs/DECISIONS.md, parshape. */
static const int32_t *parshape_pair(const struct hstex_engine *engine,
                                    int32_t line)
{
    if (engine->parshape == 0U) {
        return NULL;
    }
    const int32_t *shape = engine->parshapes + (engine->parshape - 1U);
    int32_t count = shape[0];
    if (count <= 0) {
        return NULL;
    }
    int32_t which = line < 1 ? 1 : line;
    if (which > count) {
        which = count;
    }
    return shape + 1 + 2 * (which - 1);
}

/* The width a given line is set to. \parshape says so outright; failing
   that, hanging indentation narrows the lines it covers. */
static int32_t line_width_for(const struct hstex_engine *engine, int32_t line)
{
    const int32_t *pair = parshape_pair(engine, line);
    if (pair != NULL) {
        return pair[1];
    }
    int32_t hsize = engine->dimen_parameters[HSTEX_DIMEN_HSIZE];
    int32_t hang = engine->dimen_parameters[HSTEX_DIMEN_HANG_INDENT];
    if (hang == 0) {
        return hsize;
    }
    int32_t after = engine->integer_parameters[HSTEX_INTEGER_HANG_AFTER];
    bool hanging = after >= 0 ? line > after : line <= -after;
    int32_t amount = hang < 0 ? -hang : hang;
    return hanging ? hsize - amount : hsize;
}

static int32_t line_shift_for(const struct hstex_engine *engine, int32_t line)
{
    const int32_t *pair = parshape_pair(engine, line);
    if (pair != NULL) {
        return pair[0];
    }
    int32_t hang = engine->dimen_parameters[HSTEX_DIMEN_HANG_INDENT];
    if (hang <= 0) {
        return 0;
    }
    int32_t after = engine->integer_parameters[HSTEX_INTEGER_HANG_AFTER];
    bool hanging = after >= 0 ? line > after : line <= -after;
    return hanging ? hang : 0;
}

/* One pass over the paragraph at a given badness threshold. Returns 1 when a
   sequence of breaks was found, 0 when none was. */
static int find_paragraph_breaks(struct hstex_engine *engine,
                                 struct hstex_break_state *state,
                                 const uint32_t *items, size_t count,
                                 const struct hstex_break_totals *background,
                                 int32_t threshold, bool final_pass,
                                 size_t *best, char *error,
                                 size_t error_capacity)
{
    state->record_count = 0U;
    state->active_count = 0U;
    if (reserve_break_records(state, error, error_capacity) != 0 ||
        reserve_active_breaks(state, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_break_record *first = &state->records[state->record_count];
    first->breakpoint = 0U;
    first->start = 0U;
    first->line = 0;
    first->fitness = (uint8_t)HSTEX_FIT_DECENT;
    first->demerits = 0;
    first->previous = SIZE_MAX;
    state->active[state->active_count++] = state->record_count;
    ++state->record_count;

    bool after_box = false;
    for (size_t index = 0U; index < count; ++index) {
        uint32_t identifier = items[index];
        if (identifier == 0U || (size_t)identifier > engine->node_count) {
            continue;
        }
        const struct hstex_node *node = &engine->nodes[identifier - 1U];
        int32_t penalty = 0;
        bool legal = false;
        if (node->kind == HSTEX_NODE_GLUE) {
            legal = after_box;
        } else if (node->kind == HSTEX_NODE_KERN) {
            if (node->explicit_kern && index + 1U < count) {
                uint32_t next = items[index + 1U];
                legal = next != 0U && (size_t)next <= engine->node_count &&
                        engine->nodes[next - 1U].kind == HSTEX_NODE_GLUE;
            }
        } else if (node->kind == HSTEX_NODE_PENALTY) {
            penalty = node->value.penalty;
            legal = penalty < HSTEX_INFINITE_BADNESS;
        }
        if (legal && state->active_count != 0U) {
            int32_t line = state->records[state->active[0]].line + 1;
            if (try_break_at(engine, state, items, count, background,
                             line_width_for(engine, line), index, penalty,
                             threshold, final_pass, error,
                             error_capacity) != 0) {
                return -1;
            }
        }
        after_box = !node_is_discardable(node);
    }
    if (state->active_count == 0U) {
        *best = SIZE_MAX;
        return 0;
    }
    int32_t last_line = state->records[state->active[0]].line + 1;
    if (try_break_at(engine, state, items, count, background,
                     line_width_for(engine, last_line), count,
                     HSTEX_EJECT_PENALTY, threshold, final_pass, error,
                     error_capacity) != 0) {
        return -1;
    }
    /* Whatever the forced final break created is the answer; the cheapest of
       them is the paragraph's shape. */
    size_t chosen = SIZE_MAX;
    int64_t cheapest = HSTEX_AWFUL_BADNESS;
    for (size_t slot = 0U; slot < state->active_count; ++slot) {
        size_t index = state->active[slot];
        if (state->records[index].breakpoint != count) {
            continue;
        }
        if (state->records[index].demerits < cheapest) {
            cheapest = state->records[index].demerits;
            chosen = index;
        }
    }
    *best = chosen;
    return chosen == SIZE_MAX ? 0 : 1;
}

/* Package the chosen sequence of breaks into lines and contribute them. */
static int emit_paragraph_lines(struct hstex_engine *engine,
                                struct hstex_break_state *state,
                                const uint32_t *items, size_t count,
                                size_t best, struct hstex_box *out_last,
                                char *error, size_t error_capacity)
{
    size_t depth = 0U;
    for (size_t index = best; index != SIZE_MAX;
         index = state->records[index].previous) {
        ++depth;
    }
    size_t *chain = calloc(depth, sizeof(*chain));
    if (chain == NULL) {
        return set_error(error, error_capacity, "line chain allocation failed");
    }
    size_t position = depth;
    for (size_t index = best; index != SIZE_MAX;
         index = state->records[index].previous) {
        chain[--position] = index;
    }

    struct hstex_glue left = engine->glue_parameters[HSTEX_GLUE_LEFT_SKIP];
    struct hstex_glue right = engine->glue_parameters[HSTEX_GLUE_RIGHT_SKIP];
    int32_t interline =
        engine->integer_parameters[HSTEX_INTEGER_INTERLINE_PENALTY];
    int32_t club = engine->integer_parameters[HSTEX_INTEGER_CLUB_PENALTY];
    int32_t widow = engine->integer_parameters[HSTEX_INTEGER_WIDOW_PENALTY];
    int status = 0;
    size_t lines = depth - 1U;
    engine->paragraph_lines =
        lines > (size_t)INT32_MAX ? INT32_MAX : (int32_t)lines;
    for (size_t line = 1U; status == 0 && line < depth; ++line) {
        size_t from = state->records[chain[line - 1U]].start;
        size_t to = state->records[chain[line]].breakpoint;
        struct hstex_hbox_builder builder = {0};
        struct hstex_hbox_builder *previous = engine->active_hbox_builder;
        enum hstex_mode previous_mode = engine->mode;
        engine->active_hbox_builder = &builder;
        engine->mode = HSTEX_MODE_HORIZONTAL;
        status = emit_math_glue(engine, left, error, error_capacity);
        for (size_t index = from; status == 0 && index < to && index < count;
             ++index) {
            status = append_hbox_item(engine, items[index], error,
                                      error_capacity);
        }
        if (status == 0) {
            status = emit_math_glue(engine, right, error, error_capacity);
        }
        struct hstex_box box = {0};
        if (status == 0) {
            status = finalize_hbox(engine, &builder, true, false,
                                   line_width_for(engine, (int32_t)line), &box,
                                   error, error_capacity);
        }
        free(builder.node_identifiers);
        engine->active_hbox_builder = previous;
        engine->mode = previous_mode;
        if (status != 0) {
            break;
        }
        if (out_last != NULL) {
            *out_last = box;
        }
        struct hstex_node line_node = {
            .kind = HSTEX_NODE_LIST,
            .width = box.width,
            .height = box.height,
            .depth = box.depth,
            .shift = line_shift_for(engine, (int32_t)line),
            .value.list = {
                .node_start = box.node_start,
                .node_count = box.node_count,
                .box_kind = box.kind,
            },
        };
        status = append_vbox_node(engine, &line_node, error, error_capacity);
        if (status == 0 && line != lines) {
            int32_t penalty = interline;
            if (line == 1U) {
                penalty += club;
            }
            if (line + 1U == lines) {
                penalty += widow;
            }
            if (penalty != 0) {
                struct hstex_node node = {
                    .kind = HSTEX_NODE_PENALTY,
                    .value.penalty = penalty,
                };
                status = append_vbox_node(engine, &node, error, error_capacity);
            }
        }
    }
    free(chain);
    return status;
}

/* Break the paragraph the engine has been filling into lines. */
static int break_paragraph(struct hstex_engine *engine,
                           struct hstex_box *out_last, char *error,
                           size_t error_capacity)
{
    struct hstex_hbox_builder *paragraph = engine->paragraph_builder;
    size_t count = paragraph->count;
    const uint32_t *items = paragraph->node_identifiers;
    struct hstex_break_state state = {0};
    state.node_count = count;
    state.totals = calloc(count + 1U, sizeof(*state.totals));
    if (state.totals == NULL) {
        return set_error(error, error_capacity, "break totals allocation failed");
    }
    for (size_t index = 0U; index < count; ++index) {
        state.totals[index + 1U] = state.totals[index];
        uint32_t identifier = items[index];
        if (identifier == 0U || (size_t)identifier > engine->node_count) {
            continue;
        }
        const struct hstex_node *node = &engine->nodes[identifier - 1U];
        state.totals[index + 1U].width += packed_dimen(node->width);
        if (node->kind == HSTEX_NODE_GLUE) {
            uint8_t up = node->value.glue.stretch_order;
            if (up < 4U) {
                state.totals[index + 1U].stretch[up] += node->value.glue.stretch;
            }
            if (node->value.glue.shrink_order == 0U) {
                state.totals[index + 1U].shrink += node->value.glue.shrink;
            }
        }
    }
    struct hstex_break_totals background = {0};
    struct hstex_glue left = engine->glue_parameters[HSTEX_GLUE_LEFT_SKIP];
    struct hstex_glue right = engine->glue_parameters[HSTEX_GLUE_RIGHT_SKIP];
    background.width = (int64_t)left.width + right.width;
    if (left.stretch_order < 4U) {
        background.stretch[left.stretch_order] += left.stretch;
    }
    if (right.stretch_order < 4U) {
        background.stretch[right.stretch_order] += right.stretch;
    }
    if (left.shrink_order == 0U) {
        background.shrink += left.shrink;
    }
    if (right.shrink_order == 0U) {
        background.shrink += right.shrink;
    }

    size_t best = SIZE_MAX;
    int found = 0;
    int32_t pretolerance = engine->integer_parameters[HSTEX_INTEGER_PRETOLERANCE];
    if (pretolerance >= 0) {
        found = find_paragraph_breaks(engine, &state, items, count, &background,
                                      pretolerance, false, &best, error,
                                      error_capacity);
    }
    if (found == 0) {
        found = find_paragraph_breaks(
            engine, &state, items, count, &background,
            engine->integer_parameters[HSTEX_INTEGER_TOLERANCE], false, &best,
            error, error_capacity);
    }
    if (found == 0) {
        found = find_paragraph_breaks(
            engine, &state, items, count, &background,
            engine->integer_parameters[HSTEX_INTEGER_TOLERANCE], true, &best,
            error, error_capacity);
    }
    int status = found < 0 ? -1 : 0;
    if (status == 0 && found == 0) {
        status = set_error(error, error_capacity,
                           "no way to break this paragraph into lines");
    }
    if (status == 0) {
        status = emit_paragraph_lines(engine, &state, items, count, best,
                                      out_last, error, error_capacity);
    }
    free(state.totals);
    free(state.records);
    free(state.active);
    return status;
}

/* Package the paragraph so far as one line. A paragraph with nothing in it
   contributes no line at all, which is what the reference does and what
   \noindent$$ depends on. */
/* Package the paragraph so far into lines and contribute them. A paragraph
   with nothing in it contributes nothing at all, which is what the reference
   does and what \noindent$$ depends on. */
static int finish_paragraph_line(struct hstex_engine *engine,
                                 struct hstex_box *out_line, char *error,
                                 size_t error_capacity)
{
    if (flush_pending_character(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (engine->paragraph_builder == NULL ||
        engine->paragraph_builder->count == 0U) {
        engine->active_hbox_builder = NULL;
        engine->mode = HSTEX_MODE_VERTICAL;
        engine->inner_mode = false;
        engine->building_paragraph = false;
        engine->has_pending_character = false;
        return 0;
    }
    struct hstex_node penalty = {
        .kind = HSTEX_NODE_PENALTY,
        .value.penalty = HSTEX_INFINITE_PENALTY,
    };
    struct hstex_glue fill = engine->glue_parameters[HSTEX_GLUE_PAR_FILL_SKIP];
    struct hstex_node fill_node = {
        .kind = HSTEX_NODE_GLUE,
        .width = fill.width,
        .value.glue = {
            .stretch = fill.stretch,
            .shrink = fill.shrink,
            .stretch_order = fill.stretch_order,
            .shrink_order = fill.shrink_order,
        },
    };
    if (append_hbox_node(engine, &penalty, error, error_capacity) != 0 ||
        append_hbox_node(engine, &fill_node, error, error_capacity) != 0) {
        return -1;
    }
    int status = break_paragraph(engine, out_line, error, error_capacity);
    engine->active_hbox_builder = NULL;
    engine->mode = HSTEX_MODE_VERTICAL;
    engine->inner_mode = false;
    engine->building_paragraph = false;
    engine->has_pending_character = false;
    return status;
}

/* The shape of a paragraph belongs to that paragraph: the reference clears
   it whenever a new one may begin, which is after \par and at the start of a
   vertical box, but not at the start of an hbox. The assignments are local,
   and are made only when they change something. See docs/DECISIONS.md,
   parshape. */
static int normal_paragraph(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    if (engine->integer_parameters[HSTEX_INTEGER_LOOSENESS] != 0 &&
        assign_integer_parameter(engine, (uint32_t)HSTEX_INTEGER_LOOSENESS, 0,
                                 false, error, error_capacity) != 0) {
        return -1;
    }
    if (engine->dimen_parameters[HSTEX_DIMEN_HANG_INDENT] != 0 &&
        assign_dimen_parameter(engine, (uint32_t)HSTEX_DIMEN_HANG_INDENT, 0,
                               false, error, error_capacity) != 0) {
        return -1;
    }
    if (engine->integer_parameters[HSTEX_INTEGER_HANG_AFTER] != 1 &&
        assign_integer_parameter(engine, (uint32_t)HSTEX_INTEGER_HANG_AFTER, 1,
                                 false, error, error_capacity) != 0) {
        return -1;
    }
    if (engine->parshape != 0U &&
        assign_parshape(engine, 0U, false, error, error_capacity) != 0) {
        return -1;
    }
    return 0;
}

static int finish_paragraph(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    if (finish_paragraph_line(engine, NULL, error, error_capacity) != 0) {
        return -1;
    }
    return normal_paragraph(engine, error, error_capacity);
}

/* A horizontal command met in vertical mode starts an indented paragraph and
   is then read again, so that \everypar runs before the command's own
   operands are scanned; see docs/DECISIONS.md, starting-a-paragraph. The
   executor does the pushing back, so by the time an executor runs, the mode
   is already right and this only reports a genuinely misplaced command. */
static int ensure_horizontal_mode(struct hstex_engine *engine, char *error,
                                  size_t error_capacity)
{
    if (engine->mode == HSTEX_MODE_HORIZONTAL) {
        return 0;
    }
    uint32_t line = 0U;
    const char *origin = current_source_line(engine, &line);
    return set_error(error, error_capacity,
                     "horizontal command \\%s used outside horizontal mode, "
                     "at %s:%u",
                     engine->executing_name, origin, (unsigned int)line);
}

/* The commands that begin a paragraph when they are met in vertical mode.
   Everything else either belongs to the vertical list or is an error there:
   pdfTeX rejects \/ in internal vertical mode rather than starting one. */
/* The commands that end a paragraph when they are met while one is being
   built: the reference puts a \par in front of them and reads them again,
   so that what they make goes into the vertical list. See
   docs/DECISIONS.md, ending-a-paragraph. */
static bool command_ends_paragraph(const struct hstex_meaning *meaning)
{
    switch (meaning->command) {
    case HSTEX_COMMAND_HRULE:
    case HSTEX_COMMAND_VSKIP:
    case HSTEX_COMMAND_HALIGN:
        return true;
    case HSTEX_COMMAND_UNBOX:
        return meaning->value.integer == (int32_t)HSTEX_UNBOX_VERTICAL ||
               meaning->value.integer == (int32_t)HSTEX_UNBOX_VERTICAL_COPY;
    default:
        return false;
    }
}

static bool command_starts_paragraph(const struct hstex_meaning *meaning)
{
    switch (meaning->command) {
    case HSTEX_COMMAND_HSKIP:
    case HSTEX_COMMAND_VRULE:
    case HSTEX_COMMAND_CONTROL_SPACE:
    case HSTEX_COMMAND_CHAR:
    case HSTEX_COMMAND_ACCENT:
        return true;
    case HSTEX_COMMAND_UNBOX:
        return meaning->value.integer ==
                   (int32_t)HSTEX_UNBOX_HORIZONTAL ||
               meaning->value.integer ==
                   (int32_t)HSTEX_UNBOX_HORIZONTAL_COPY;
    default:
        return false;
    }
}

/* A control space is the font's own interword glue, with no space-factor
   adjustment; see docs/DECISIONS.md, control-space-and-italic. */
/* \  takes \spaceskip when that is set and the current font's interword
   glue otherwise, neither adjusted by the space factor, and a formula gets
   exactly the same glue as a paragraph does. See docs/DECISIONS.md,
   control-space. */
static int execute_control_space(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    if (engine->mode != HSTEX_MODE_MATH &&
        ensure_horizontal_mode(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (flush_pending_character(engine, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_glue glue = engine->glue_parameters[HSTEX_GLUE_SPACE_SKIP];
    if (glue.width == 0 && glue.stretch == 0 && glue.shrink == 0) {
        const struct hstex_font *font =
            font_by_identifier(engine, engine->current_font);
        if (font == NULL || font->dimen_count < 4U) {
            return set_error(error, error_capacity,
                             "current font does not define interword spacing");
        }
        glue.width = font->dimens[1];
        glue.stretch = font->dimens[2];
        glue.shrink = font->dimens[3];
        glue.stretch_order = 0U;
        glue.shrink_order = 0U;
    }
    struct hstex_node node = {
        .kind = HSTEX_NODE_GLUE,
        .width = glue.width,
        .value.glue = {
            .stretch = glue.stretch,
            .shrink = glue.shrink,
            .stretch_order = glue.stretch_order,
            .shrink_order = glue.shrink_order,
        },
    };
    return append_current_list_node(engine, &node, error, error_capacity);
}

/* \/ adds the italic correction of whatever character precedes it. */
static int execute_italic_correction(struct hstex_engine *engine, char *error,
                                     size_t error_capacity)
{
    if (ensure_horizontal_mode(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (flush_pending_character(engine, error, error_capacity) != 0) {
        return -1;
    }
    int32_t correction = 0;
    const struct hstex_node *node = current_list_last_node(engine);
    if (node != NULL && (node->kind == HSTEX_NODE_CHARACTER ||
                         node->kind == HSTEX_NODE_LIGATURE)) {
        const struct hstex_font *font =
            font_by_identifier(engine, node->value.character.font);
        if (font != NULL && font->characters != NULL) {
            correction =
                font->characters[node->value.character.character & 0xFFU]
                    .italic;
        }
    }
    struct hstex_node kern = {
        .kind = HSTEX_NODE_KERN,
        .width = correction,
    };
    return append_hbox_node(engine, &kern, error, error_capacity);
}

/* \char names a character by code; it joins the list like any other. */
static int execute_char(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
{
    int32_t code = 0;
    if (scan_integer(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (code < 0 || code > 255) {
        return set_error(error, error_capacity, "bad character code (%d)",
                         code);
    }
    if (engine->mode == HSTEX_MODE_VERTICAL) {
        if (start_paragraph(engine, true, error, error_capacity) != 0) {
            return -1;
        }
    }
    return append_horizontal_character(engine, (uint8_t)code, error,
                                       error_capacity);
}

/* ---------------------------------------------------------------------- */
/* Math lists.                                                            */
/*                                                                        */
/* A formula is collected as a list of noads and translated into a        */
/* horizontal list when it closes. Only the text size is implemented, and */
/* scripts, fractions and delimiters are not; see docs/DECISIONS.md,      */
/* math-mode, for what was measured and what is deferred.                 */
/* ---------------------------------------------------------------------- */

static struct hstex_math_builder *current_math_list(struct hstex_engine *engine)
{
    if (engine->math_depth == 0U) {
        return NULL;
    }
    return &engine->math_stack[engine->math_depth - 1U];
}

static int push_math_list(struct hstex_engine *engine, uint8_t style,
                          char *error, size_t error_capacity)
{
    if (engine->math_depth == engine->math_capacity) {
        size_t capacity =
            engine->math_capacity == 0U ? 8U : engine->math_capacity * 2U;
        if (capacity > SIZE_MAX / sizeof(*engine->math_stack)) {
            return set_error(error, error_capacity, "math list nesting overflow");
        }
        void *allocation =
            realloc(engine->math_stack, capacity * sizeof(*engine->math_stack));
        if (allocation == NULL) {
            return set_error(error, error_capacity,
                             "math list allocation failed");
        }
        engine->math_stack = allocation;
        engine->math_capacity = capacity;
    }
    struct hstex_math_builder *builder =
        &engine->math_stack[engine->math_depth++];
    memset(builder, 0, sizeof(*builder));
    builder->forced_class = -1;
    builder->style = style;
    builder->current_style = style;
    return 0;
}

static void pop_math_list(struct hstex_engine *engine)
{
    if (engine->math_depth == 0U) {
        return;
    }
    struct hstex_math_builder *builder =
        &engine->math_stack[--engine->math_depth];
    free(builder->noads);
    memset(builder, 0, sizeof(*builder));
}

static int reserve_noads(struct hstex_math_builder *builder, size_t required,
                         char *error, size_t error_capacity)
{
    if (required <= builder->capacity) {
        return 0;
    }
    size_t capacity = builder->capacity == 0U ? 16U : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "math list overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*builder->noads)) {
        return set_error(error, error_capacity, "math list overflow");
    }
    void *allocation =
        realloc(builder->noads, capacity * sizeof(*builder->noads));
    if (allocation == NULL) {
        return set_error(error, error_capacity, "math list allocation failed");
    }
    builder->noads = allocation;
    builder->capacity = capacity;
    return 0;
}

/* \radical, \overline and \underline make noads that are ordinary atoms
   once the list is set: they carry scripts and count for the spacing on
   either side of them, so everything that looks for an atom must see them
   too. */
static bool math_noad_is_atom(uint8_t kind)
{
    return kind == (uint8_t)HSTEX_NOAD_ATOM ||
           kind == (uint8_t)HSTEX_NOAD_RADICAL ||
           kind == (uint8_t)HSTEX_NOAD_OVERLINE ||
           kind == (uint8_t)HSTEX_NOAD_UNDERLINE ||
           kind == (uint8_t)HSTEX_NOAD_MIDDLE;
}

static int math_append(struct hstex_engine *engine,
                       const struct hstex_noad *noad, char *error,
                       size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (builder == NULL) {
        return set_error(error, error_capacity,
                         "math material used outside a formula");
    }
    if (reserve_noads(builder, builder->count + 1U, error, error_capacity) !=
        0) {
        return -1;
    }
    builder->noads[builder->count++] = *noad;
    return 0;
}

/* \mathord and its relatives set the class of the atom that follows. */
static int math_append_atom(struct hstex_engine *engine,
                            struct hstex_noad *noad, char *error,
                            size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (builder == NULL) {
        return set_error(error, error_capacity,
                         "math material used outside a formula");
    }
    /* A script mark is waiting: this atom is the script, not a list item. */
    if (builder->slot != (uint8_t)HSTEX_MATH_SLOT_NONE) {
        if (builder->slot_target >= builder->count) {
            return set_error(error, error_capacity,
                             "a script has no atom to attach to");
        }
        struct hstex_noad *target = &builder->noads[builder->slot_target];
        if (builder->slot == (uint8_t)HSTEX_MATH_SLOT_SUPERSCRIPT) {
            target->superscript = noad->nucleus;
        } else if (builder->slot == (uint8_t)HSTEX_MATH_SLOT_RADICAND) {
            target->nucleus = noad->nucleus;
        } else {
            target->subscript = noad->nucleus;
        }
        builder->slot = (uint8_t)HSTEX_MATH_SLOT_NONE;
        return 0;
    }
    if (builder->forced_class >= 0) {
        noad->atom_class = (uint8_t)builder->forced_class;
        builder->forced_class = -1;
    }
    return math_append(engine, noad, error, error_capacity);
}

/* A script mark attaches to the atom before it, or to a fresh ordinary atom
   with nothing in it when there is none. */
static int begin_math_script(struct hstex_engine *engine, bool superscript,
                             char *error, size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (builder == NULL) {
        return set_error(error, error_capacity,
                         "a script mark is only allowed in a formula");
    }
    if (builder->slot != (uint8_t)HSTEX_MATH_SLOT_NONE) {
        return set_error(error, error_capacity,
                         "one script mark followed another");
    }
    bool have_atom =
        builder->count != 0U &&
        math_noad_is_atom(builder->noads[builder->count - 1U].kind);
    if (have_atom) {
        const struct hstex_noad *last = &builder->noads[builder->count - 1U];
        const struct hstex_math_field *taken =
            superscript ? &last->superscript : &last->subscript;
        if (taken->kind != (uint8_t)HSTEX_MATH_FIELD_EMPTY) {
            return set_error(error, error_capacity,
                             "this atom already has a %s",
                             superscript ? "superscript" : "subscript");
        }
    } else {
        struct hstex_noad empty = {
            .kind = (uint8_t)HSTEX_NOAD_ATOM,
            .atom_class = (uint8_t)HSTEX_ATOM_ORD,
        };
        if (math_append(engine, &empty, error, error_capacity) != 0) {
            return -1;
        }
    }
    builder->slot = superscript ? (uint8_t)HSTEX_MATH_SLOT_SUPERSCRIPT
                                : (uint8_t)HSTEX_MATH_SLOT_SUBSCRIPT;
    builder->slot_target = builder->count - 1U;
    return 0;
}

/* A mathcode of class 7 means "the family is \fam if that is a real family",
   and is otherwise an ordinary atom in the code's own family; the reference
   sets \fam to -1 on entry to a formula. */
static int math_append_code(struct hstex_engine *engine, int32_t code,
                            char *error, size_t error_capacity)
{
    uint8_t class_code = (uint8_t)((code >> 12) & 0x7);
    uint8_t family = (uint8_t)((code >> 8) & 0xF);
    uint8_t character = (uint8_t)(code & 0xFF);
    if (class_code == 7U) {
        int32_t requested = engine->integer_parameters[HSTEX_INTEGER_FAMILY];
        if (requested >= 0 && requested < 16) {
            family = (uint8_t)requested;
        }
        class_code = (uint8_t)HSTEX_ATOM_ORD;
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_ATOM,
        .atom_class = class_code,
        .nucleus = {.kind = (uint8_t)HSTEX_MATH_FIELD_CHARACTER,
                    .family = family,
                    .character = character},
    };
    return math_append_atom(engine, &noad, error, error_capacity);
}

static int math_append_character(struct hstex_engine *engine, uint8_t code,
                                 char *error, size_t error_capacity)
{
    int table = code_table_index(HSTEX_COMMAND_MATH_CODE);
    if (table < 0) {
        return set_error(error, error_capacity, "no mathcode table");
    }
    return math_append_code(engine, engine->code_tables[(size_t)table][code],
                            error, error_capacity);
}

static int math_append_box_field(struct hstex_engine *engine,
                                 const struct hstex_box *box,
                                 bool single_character, uint32_t sublist,
                                 uint8_t list_style, char *error,
                                 size_t error_capacity)
{
    struct hstex_node node = {
        .kind = HSTEX_NODE_LIST,
        .width = box->width,
        .height = box->height,
        .depth = box->depth,
        .value.list = {
            .node_start = box->node_start,
            .node_count = box->node_count,
            .box_kind = box->kind,
        },
    };
    uint32_t identifier = 0U;
    if (store_node(engine, &node, &identifier, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_ATOM,
        .atom_class = (uint8_t)HSTEX_ATOM_ORD,
        .nucleus = {.kind = (uint8_t)HSTEX_MATH_FIELD_BOX,
                    .single_character = single_character ? 1U : 0U,
                    .node = identifier,
                    .sublist = sublist,
                    .list_style = list_style},
    };
    return math_append_atom(engine, &noad, error, error_capacity);
}

static int math_append_box(struct hstex_engine *engine,
                           const struct hstex_box *box, char *error,
                           size_t error_capacity)
{
    return math_append_box_field(engine, box, false, 0U, 0U, error,
                                 error_capacity);
}

/* Glue, kerns and penalties keep the shape they already have. */
static int math_append_node(struct hstex_engine *engine,
                            const struct hstex_node *node, char *error,
                            size_t error_capacity)
{
    uint32_t identifier = 0U;
    if (store_node(engine, node, &identifier, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_NODE,
        .node = identifier,
    };
    return math_append(engine, &noad, error, error_capacity);
}


/* The style decides which of the three font tables a family is read from. */
static uint8_t math_size_of_style(uint8_t style)
{
    if (style < (uint8_t)HSTEX_STYLE_SCRIPT) {
        return (uint8_t)HSTEX_MATH_TEXT;
    }
    if (style < (uint8_t)HSTEX_STYLE_SCRIPT_SCRIPT) {
        return (uint8_t)HSTEX_MATH_SCRIPT;
    }
    return (uint8_t)HSTEX_MATH_SCRIPT_SCRIPT;
}

static uint8_t math_superscript_style(uint8_t style)
{
    return (uint8_t)(2U * (style / 4U) + (uint8_t)HSTEX_STYLE_SCRIPT +
                     (style % 2U));
}

static uint8_t math_subscript_style(uint8_t style)
{
    return (uint8_t)(2U * (style / 4U) + (uint8_t)HSTEX_STYLE_SCRIPT + 1U);
}

static const struct hstex_font *math_family_font(struct hstex_engine *engine,
                                                 uint8_t size, uint8_t family)
{
    if (family >= 16U || size >= (uint8_t)HSTEX_MATH_SIZE_COUNT) {
        return NULL;
    }
    return font_by_identifier(engine, engine->math_fonts[size][family]);
}

/* One mu is a eighteenth of the quad of the symbol family, truncated; a mu
   quantity is that many times the stored value, again truncated toward zero.
   See docs/DECISIONS.md, math-mode. */
static int math_unit(struct hstex_engine *engine, uint8_t size, int32_t *unit,
                     char *error, size_t error_capacity)
{
    const struct hstex_font *symbols = math_family_font(engine, size, 2U);
    if (symbols == NULL || symbols->dimen_count < 6U) {
        return set_error(error, error_capacity,
                         "math units need \\textfont2 with six parameters");
    }
    *unit = symbols->dimens[5] / 18;
    return 0;
}

static int32_t scaled_by_unit(int32_t value, int32_t unit)
{
    bool negative = value < 0;
    int64_t magnitude = negative ? -(int64_t)value : (int64_t)value;
    int64_t result = magnitude * (int64_t)unit / INT64_C(65536);
    return negative ? (int32_t)-result : (int32_t)result;
}

static struct hstex_glue math_glue_in_points(struct hstex_glue glue,
                                             int32_t unit)
{
    struct hstex_glue result = glue;
    result.width = scaled_by_unit(glue.width, unit);
    result.stretch = scaled_by_unit(glue.stretch, unit);
    result.shrink = scaled_by_unit(glue.shrink, unit);
    return result;
}

/* The spacing between two adjacent atom classes, measured against pdfTeX:
   0 none, 1 \thinmuskip, 2 \medmuskip, 3 \thickmuskip. The entries a Bin
   atom can never reach are marked 0 because the class conversion below
   removes them first. */
/* Which of those spaces survive outside display and text style. Measured the
   same way inside a superscript: only the five that touch a large operator
   do. See docs/DECISIONS.md, math-scripts. */
static const uint8_t math_spacing_all_styles[HSTEX_ATOM_CLASS_COUNT]
                                            [HSTEX_ATOM_CLASS_COUNT] = {
    /*            ord op   bin  rel  open close punct inner */
    /* ord   */ {  0,  1,   0,   0,   0,   0,    0,    0 },
    /* op    */ {  1,  1,   0,   0,   0,   0,    0,    0 },
    /* bin   */ {  0,  0,   0,   0,   0,   0,    0,    0 },
    /* rel   */ {  0,  0,   0,   0,   0,   0,    0,    0 },
    /* open  */ {  0,  0,   0,   0,   0,   0,    0,    0 },
    /* close */ {  0,  1,   0,   0,   0,   0,    0,    0 },
    /* punct */ {  0,  0,   0,   0,   0,   0,    0,    0 },
    /* inner */ {  0,  1,   0,   0,   0,   0,    0,    0 },
};

static const uint8_t math_spacing_table[HSTEX_ATOM_CLASS_COUNT]
                                       [HSTEX_ATOM_CLASS_COUNT] = {
    /*            ord op   bin  rel  open close punct inner */
    /* ord   */ {  0,  1,   2,   3,   0,   0,    0,    1 },
    /* op    */ {  1,  1,   0,   3,   0,   0,    0,    1 },
    /* bin   */ {  2,  2,   0,   0,   2,   0,    0,    2 },
    /* rel   */ {  3,  3,   0,   0,   3,   0,    0,    3 },
    /* open  */ {  0,  0,   0,   0,   0,   0,    0,    0 },
    /* close */ {  0,  1,   2,   3,   0,   0,    0,    1 },
    /* punct */ {  1,  1,   1,   1,   1,   1,    1,    1 },
    /* inner */ {  1,  1,   2,   3,   1,   0,    1,    1 },
};

/* A Bin atom is only a binary operator between two things it can join. The
   reference turns it into an Ord at the start of a list, after another Bin,
   an Op, a Rel, an Open or a Punct, and before a Rel, a Close or a Punct or
   the end of the list. */
static void resolve_binary_atoms(struct hstex_math_builder *builder)
{
    int previous = -1;
    for (size_t index = 0U; index < builder->count; ++index) {
        struct hstex_noad *noad = &builder->noads[index];
        if (!math_noad_is_atom(noad->kind)) {
            continue;
        }
        if (noad->atom_class == (uint8_t)HSTEX_ATOM_BIN) {
            if (previous < 0 || previous == (int)HSTEX_ATOM_BIN ||
                previous == (int)HSTEX_ATOM_OP ||
                previous == (int)HSTEX_ATOM_REL ||
                previous == (int)HSTEX_ATOM_OPEN ||
                previous == (int)HSTEX_ATOM_PUNCT) {
                noad->atom_class = (uint8_t)HSTEX_ATOM_ORD;
            }
        } else if (noad->atom_class == (uint8_t)HSTEX_ATOM_REL ||
                   noad->atom_class == (uint8_t)HSTEX_ATOM_CLOSE ||
                   noad->atom_class == (uint8_t)HSTEX_ATOM_PUNCT) {
            for (size_t back = index; back != 0U; --back) {
                struct hstex_noad *earlier = &builder->noads[back - 1U];
                if (!math_noad_is_atom(earlier->kind)) {
                    continue;
                }
                if (earlier->atom_class == (uint8_t)HSTEX_ATOM_BIN) {
                    earlier->atom_class = (uint8_t)HSTEX_ATOM_ORD;
                }
                break;
            }
        }
        /* A \middle is a closing atom to what stands before it and an
           opening one to what follows; see docs/DECISIONS.md,
           middle-delimiters. */
        previous = noad->kind == (uint8_t)HSTEX_NOAD_MIDDLE
                       ? (int)HSTEX_ATOM_OPEN
                       : (int)noad->atom_class;
    }
    for (size_t index = builder->count; index != 0U; --index) {
        struct hstex_noad *noad = &builder->noads[index - 1U];
        if (!math_noad_is_atom(noad->kind)) {
            continue;
        }
        if (noad->atom_class == (uint8_t)HSTEX_ATOM_BIN) {
            noad->atom_class = (uint8_t)HSTEX_ATOM_ORD;
        }
        break;
    }
}

/* Returns 1 when the character is there, 0 when the font simply does not
   have it -- which the reference reports and then carries on without, so
   the atom contributes nothing. */
static int math_character_metric(struct hstex_engine *engine,
                                 const struct hstex_math_field *field,
                                 uint8_t size, const struct hstex_font **font,
                                 const struct hstex_char_metric **metric,
                                 char *error, size_t error_capacity)
{
    const struct hstex_font *resolved =
        math_family_font(engine, size, field->family);
    if (resolved == NULL) {
        return set_error(error, error_capacity,
                         "math family %u has no font in this size",
                         (unsigned int)field->family);
    }
    if (resolved->characters == NULL ||
        resolved->characters[field->character].tag < 0) {
        return 0;
    }
    *font = resolved;
    *metric = &resolved->characters[field->character];
    return 1;
}

/* Two adjacent ordinary characters of the same family go through that
   font's ligature and kerning program, exactly as they would in text. */
static int apply_math_ligatures(struct hstex_engine *engine,
                                struct hstex_math_builder *builder,
                                uint8_t style, char *error,
                                size_t error_capacity)
{
    uint8_t size = math_size_of_style(style);
    for (size_t index = 0U; index + 1U < builder->count;) {
        struct hstex_noad *left = &builder->noads[index];
        struct hstex_noad *right = &builder->noads[index + 1U];
        if (left->kind == (uint8_t)HSTEX_NOAD_STYLE) {
            size = math_size_of_style(left->atom_class);
        }
        if (left->kind != (uint8_t)HSTEX_NOAD_ATOM ||
            right->kind != (uint8_t)HSTEX_NOAD_ATOM ||
            left->atom_class != (uint8_t)HSTEX_ATOM_ORD ||
            right->atom_class != (uint8_t)HSTEX_ATOM_ORD ||
            left->nucleus.kind != (uint8_t)HSTEX_MATH_FIELD_CHARACTER ||
            right->nucleus.kind != (uint8_t)HSTEX_MATH_FIELD_CHARACTER ||
            left->nucleus.family != right->nucleus.family ||
            left->superscript.kind != (uint8_t)HSTEX_MATH_FIELD_EMPTY ||
            left->subscript.kind != (uint8_t)HSTEX_MATH_FIELD_EMPTY ||
            right->superscript.kind != (uint8_t)HSTEX_MATH_FIELD_EMPTY ||
            right->subscript.kind != (uint8_t)HSTEX_MATH_FIELD_EMPTY) {
            ++index;
            continue;
        }
        const struct hstex_font *font = NULL;
        const struct hstex_char_metric *metric = NULL;
        int present = math_character_metric(engine, &left->nucleus, size, &font,
                                            &metric, error, error_capacity);
        if (present < 0) {
            return -1;
        }
        if (present == 0) {
            ++index;
            continue;
        }
        bool kerned = false;
        bool ligatured = false;
        int32_t kern = 0;
        uint8_t ligature = 0U;
        if (font_lig_kern(font, (uint8_t)left->nucleus.character,
                          (uint8_t)right->nucleus.character, &kerned, &kern,
                          &ligatured, &ligature, error, error_capacity) != 0) {
            return -1;
        }
        if (ligatured) {
            left->nucleus.character = ligature;
            memmove(&builder->noads[index + 1U], &builder->noads[index + 2U],
                    (builder->count - index - 2U) * sizeof(*builder->noads));
            --builder->count;
            continue;
        }
        if (kerned) {
            struct hstex_node node = {
                .kind = HSTEX_NODE_KERN,
                .width = kern,
            };
            uint32_t identifier = 0U;
            struct hstex_noad inserted = {
                .kind = (uint8_t)HSTEX_NOAD_NODE,
            };
            if (store_node(engine, &node, &identifier, error,
                           error_capacity) != 0 ||
                reserve_noads(builder, builder->count + 1U, error,
                              error_capacity) != 0) {
                return -1;
            }
            inserted.node = identifier;
            memmove(&builder->noads[index + 2U], &builder->noads[index + 1U],
                    (builder->count - index - 1U) * sizeof(*builder->noads));
            builder->noads[index + 1U] = inserted;
            ++builder->count;
            index += 2U;
            continue;
        }
        ++index;
    }
    return 0;
}

/* A large operator sits with its middle on the axis of the symbol family.
   The builder derives the reach of a shifted node itself, so only the
   displacement is recorded here. */
static int centre_on_axis(struct hstex_engine *engine, uint8_t size,
                          struct hstex_node *node, char *error,
                          size_t error_capacity)
{
    const struct hstex_font *symbols = math_family_font(engine, size, 2U);
    if (symbols == NULL || symbols->dimen_count < 22U) {
        return set_error(error, error_capacity,
                         "a large operator needs \\textfont2 with "
                         "twenty-two parameters");
    }
    node->shift = half_of((int64_t)packed_dimen(node->height) -
                          packed_dimen(node->depth)) -
                  symbols->dimens[21];
    return 0;
}

static int emit_math_glue(struct hstex_engine *engine, struct hstex_glue glue,
                          char *error, size_t error_capacity)
{
    struct hstex_node node = {
        .kind = HSTEX_NODE_GLUE,
        .width = glue.width,
        .value.glue = {
            .stretch = glue.stretch,
            .shrink = glue.shrink,
            .stretch_order = glue.stretch_order,
            .shrink_order = glue.shrink_order,
        },
    };
    return append_hbox_node(engine, &node, error, error_capacity);
}

static int emit_math_kern(struct hstex_engine *engine, int32_t amount,
                          char *error, size_t error_capacity)
{
    struct hstex_node node = {
        .kind = HSTEX_NODE_KERN,
        .width = amount,
    };
    return append_hbox_node(engine, &node, error, error_capacity);
}

/* Translate one finished math list into the horizontal list the engine is
   building. The caller has already pointed active_hbox_builder at it. */
static int translate_math_list(struct hstex_engine *engine,
                               struct hstex_math_builder *builder, char *error,
                               size_t error_capacity);

static int translate_math_fraction(struct hstex_engine *engine,
                                   struct hstex_math_builder *builder,
                                   char *error, size_t error_capacity);

static int build_math_radical(struct hstex_engine *engine,
                              struct hstex_noad *noad, uint8_t style,
                              char *error, size_t error_capacity);

static int build_math_line(struct hstex_engine *engine,
                           struct hstex_noad *noad, uint8_t style, bool over,
                           char *error, size_t error_capacity);

static int variant_delimiter(struct hstex_engine *engine, int32_t code,
                             uint8_t size, int32_t wanted,
                             struct hstex_box *box, char *error,
                             size_t error_capacity);

/* Pack one field into a box at the given style. An empty field gives an empty
   box, a character gives that character with its italic correction, and a box
   field is the box that is already there. */
static int store_box_node(struct hstex_engine *engine,
                          const struct hstex_box *box, int32_t shift,
                          uint32_t *identifier, char *error,
                          size_t error_capacity);

/* Keep a sub-formula where a field can point at it. The records live as long
   as the engine, so a field never owns what it refers to. */
static int store_math_sublist(struct hstex_engine *engine,
                              const struct hstex_math_builder *list,
                              uint32_t *index, char *error,
                              size_t error_capacity)
{
    *index = 0U;
    size_t count = list->count;
    uint32_t start = 0U;
    if (count != 0U) {
        if (engine->math_item_count + count > engine->math_item_capacity) {
            size_t capacity = engine->math_item_capacity == 0U
                                  ? 64U
                                  : engine->math_item_capacity;
            while (capacity < engine->math_item_count + count) {
                if (capacity > SIZE_MAX / 2U) {
                    return set_error(error, error_capacity,
                                     "sub-formula arena overflow");
                }
                capacity *= 2U;
            }
            if (capacity > SIZE_MAX / sizeof(*engine->math_items)) {
                return set_error(error, error_capacity,
                                 "sub-formula arena overflow");
            }
            void *allocation = realloc(engine->math_items,
                                       capacity * sizeof(*engine->math_items));
            if (allocation == NULL) {
                return set_error(error, error_capacity,
                                 "sub-formula arena allocation failed");
            }
            engine->math_items = allocation;
            engine->math_item_capacity = capacity;
        }
        if (engine->math_item_count + count > (size_t)UINT32_MAX) {
            return set_error(error, error_capacity,
                             "sub-formula arena overflow");
        }
        start = (uint32_t)engine->math_item_count;
        memcpy(engine->math_items + engine->math_item_count, list->noads,
               count * sizeof(*list->noads));
        engine->math_item_count += count;
    }
    if (engine->math_sublist_count == engine->math_sublist_capacity) {
        size_t capacity = engine->math_sublist_capacity == 0U
                              ? 32U
                              : engine->math_sublist_capacity * 2U;
        if (capacity > SIZE_MAX / sizeof(*engine->math_sublists)) {
            return set_error(error, error_capacity,
                             "sub-formula record overflow");
        }
        void *allocation = realloc(engine->math_sublists,
                                   capacity * sizeof(*engine->math_sublists));
        if (allocation == NULL) {
            return set_error(error, error_capacity,
                             "sub-formula record allocation failed");
        }
        engine->math_sublists = allocation;
        engine->math_sublist_capacity = capacity;
    }
    if (engine->math_sublist_count >= (size_t)UINT32_MAX) {
        return set_error(error, error_capacity, "sub-formula record overflow");
    }
    struct hstex_math_sublist *record =
        &engine->math_sublists[engine->math_sublist_count++];
    record->start = start;
    record->count = (uint32_t)count;
    record->style = list->style;
    record->has_fraction = list->has_fraction;
    record->fraction_at = list->fraction_at;
    record->fraction_thickness = list->fraction_thickness;
    record->fraction_default_thickness = list->fraction_default_thickness;
    record->fraction_left = list->fraction_left;
    record->fraction_right = list->fraction_right;
    *index = (uint32_t)engine->math_sublist_count;
    return 0;
}

/* Fill a list from a kept sub-formula, to be set in the style asked for. */
static int load_math_sublist(struct hstex_engine *engine, uint32_t index,
                             uint8_t style, struct hstex_math_builder *list,
                             char *error, size_t error_capacity)
{
    memset(list, 0, sizeof(*list));
    list->forced_class = -1;
    list->style = style;
    list->current_style = style;
    if (index == 0U || (size_t)index > engine->math_sublist_count) {
        return set_error(error, error_capacity,
                         "a sub-formula record went missing");
    }
    const struct hstex_math_sublist *record =
        &engine->math_sublists[index - 1U];
    list->has_fraction = record->has_fraction;
    list->fraction_at = record->fraction_at;
    list->fraction_thickness = record->fraction_thickness;
    list->fraction_default_thickness = record->fraction_default_thickness;
    list->fraction_left = record->fraction_left;
    list->fraction_right = record->fraction_right;
    if (record->count == 0U) {
        return 0;
    }
    if (reserve_noads(list, record->count, error, error_capacity) != 0) {
        return -1;
    }
    memcpy(list->noads, engine->math_items + record->start,
           record->count * sizeof(*list->noads));
    list->count = record->count;
    return 0;
}

/* Set a sub-formula again, in the style it has turned out to be wanted in.
   Nothing happens unless the style differs from the one it was read in. */
static int math_field_restyle(struct hstex_engine *engine,
                              struct hstex_math_field *field, uint8_t style,
                              char *error, size_t error_capacity)
{
    if (field->kind != (uint8_t)HSTEX_MATH_FIELD_BOX || field->sublist == 0U ||
        field->list_style == style) {
        return 0;
    }
    struct hstex_math_builder part = {0};
    if (load_math_sublist(engine, field->sublist, style, &part, error,
                          error_capacity) != 0) {
        free(part.noads);
        return -1;
    }
    struct hstex_hbox_builder packed = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_hbox_builder = &packed;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    int status = translate_math_list(engine, &part, error, error_capacity);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    struct hstex_box box = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &packed, false, false, 0, &box, error,
                               error_capacity);
    }
    free(packed.node_identifiers);
    free(part.noads);
    if (status != 0) {
        return -1;
    }
    bool single = false;
    if (box.node_count == 1U &&
        (size_t)box.node_start < engine->list_item_count) {
        uint32_t item = engine->list_items[box.node_start];
        if (item != 0U && item <= engine->node_count) {
            const struct hstex_node *node = &engine->nodes[item - 1U];
            single = (node->kind == HSTEX_NODE_CHARACTER ||
                      node->kind == HSTEX_NODE_LIGATURE) &&
                     node->shift == 0;
        }
    }
    uint32_t identifier = 0U;
    if (store_box_node(engine, &box, 0, &identifier, error, error_capacity) !=
        0) {
        return -1;
    }
    field->node = identifier;
    field->single_character = single ? 1U : 0U;
    field->list_style = style;
    return 0;
}

static int math_field_box(struct hstex_engine *engine,
                          const struct hstex_math_field *field, uint8_t style,
                          struct hstex_box *box, char *error,
                          size_t error_capacity)
{
    memset(box, 0, sizeof(*box));
    box->kind = HSTEX_BOX_HLIST;
    struct hstex_math_field restyled = *field;
    if (math_field_restyle(engine, &restyled, style, error, error_capacity) !=
        0) {
        return -1;
    }
    field = &restyled;
    if (field->kind == (uint8_t)HSTEX_MATH_FIELD_BOX) {
        if (field->node == 0U || field->node > engine->node_count) {
            return set_error(error, error_capacity,
                             "math field refers to a missing node");
        }
        const struct hstex_node *node = &engine->nodes[field->node - 1U];
        box->kind = node->value.list.box_kind;
        box->width = node->width;
        box->height = node->height;
        box->depth = node->depth;
        box->node_start = node->value.list.node_start;
        box->node_count = node->value.list.node_count;
        return 0;
    }
    if (field->kind == (uint8_t)HSTEX_MATH_FIELD_EMPTY) {
        return 0;
    }
    struct hstex_math_builder single = {0};
    single.forced_class = -1;
    single.style = style;
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_ATOM,
        .atom_class = (uint8_t)HSTEX_ATOM_ORD,
        .nucleus = *field,
    };
    if (reserve_noads(&single, 1U, error, error_capacity) != 0) {
        return -1;
    }
    single.noads[0] = noad;
    single.count = 1U;
    struct hstex_hbox_builder packed = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &packed;
    int status = translate_math_list(engine, &single, error, error_capacity);
    engine->active_hbox_builder = previous;
    if (status == 0) {
        status = finalize_hbox(engine, &packed, false, false, 0, box, error,
                               error_capacity);
    }
    free(packed.node_identifiers);
    free(single.noads);
    return status;
}

static int store_box_node(struct hstex_engine *engine,
                          const struct hstex_box *box, int32_t shift,
                          uint32_t *identifier, char *error,
                          size_t error_capacity)
{
    struct hstex_node node = {
        .kind = HSTEX_NODE_LIST,
        .width = box->width,
        .height = box->height,
        .depth = box->depth,
        .shift = shift,
        .value.list = {
            .node_start = box->node_start,
            .node_count = box->node_count,
            .box_kind = box->kind,
        },
    };
    return store_node(engine, &node, identifier, error, error_capacity);
}

/* A family-two parameter at a given size. */
static int math_symbol_parameter(struct hstex_engine *engine, uint8_t size,
                                 size_t parameter, int32_t *value, char *error,
                                 size_t error_capacity)
{
    const struct hstex_font *symbols = math_family_font(engine, size, 2U);
    if (symbols == NULL || symbols->dimen_count < parameter) {
        return set_error(error, error_capacity,
                         "math family two needs at least %zu parameters in "
                         "this size",
                         parameter);
    }
    *value = symbols->dimens[parameter - 1U];
    return 0;
}

static int math_extension_parameter(struct hstex_engine *engine, uint8_t size,
                                    size_t parameter, int32_t *value,
                                    char *error, size_t error_capacity)
{
    const struct hstex_font *extension = math_family_font(engine, size, 3U);
    if (extension == NULL || extension->dimen_count < parameter) {
        return set_error(error, error_capacity,
                         "math family three needs at least %zu parameters in "
                         "this size",
                         parameter);
    }
    *value = extension->dimens[parameter - 1U];
    return 0;
}

/* Attach the scripts of one atom, after its nucleus has been contributed.
   All of the arithmetic here was measured; see docs/DECISIONS.md,
   math-scripts. */
static int attach_math_scripts(struct hstex_engine *engine,
                               const struct hstex_noad *noad, uint8_t style,
                               int32_t nucleus_height, int32_t nucleus_depth,
                               bool nucleus_is_character, int32_t italic,
                               char *error, size_t error_capacity)
{
    bool has_superscript = noad->superscript.kind !=
                           (uint8_t)HSTEX_MATH_FIELD_EMPTY ||
                           noad->superscript.node != 0U;
    bool has_subscript =
        noad->subscript.kind != (uint8_t)HSTEX_MATH_FIELD_EMPTY ||
        noad->subscript.node != 0U;
    if (!has_superscript && !has_subscript) {
        return italic == 0
                   ? 0
                   : emit_math_kern(engine, italic, error, error_capacity);
    }
    /* The italic correction is a kern of its own only when there is no
       subscript; otherwise it displaces the superscript instead. */
    int32_t delta = italic;
    if (!has_subscript && delta != 0) {
        if (emit_math_kern(engine, delta, error, error_capacity) != 0) {
            return -1;
        }
        delta = 0;
    }

    uint8_t size = math_size_of_style(style);
    uint8_t superscript_style = math_superscript_style(style);
    uint8_t subscript_style = math_subscript_style(style);
    uint8_t script_size = math_size_of_style(superscript_style);

    int32_t shift_up = 0;
    int32_t shift_down = 0;
    if (!nucleus_is_character) {
        int32_t superscript_drop = 0;
        int32_t subscript_drop = 0;
        if (math_symbol_parameter(engine, script_size, 18U, &superscript_drop,
                                  error, error_capacity) != 0 ||
            math_symbol_parameter(engine, script_size, 19U, &subscript_drop,
                                  error, error_capacity) != 0) {
            return -1;
        }
        shift_up = nucleus_height - superscript_drop;
        shift_down = nucleus_depth + subscript_drop;
    }

    int32_t x_height = 0;
    if (math_symbol_parameter(engine, size, 5U, &x_height, error,
                              error_capacity) != 0) {
        return -1;
    }
    int32_t x_height_magnitude = x_height < 0 ? -x_height : x_height;
    int32_t scriptspace = engine->dimen_parameters[HSTEX_DIMEN_SCRIPT_SPACE];

    struct hstex_box superscript = {0};
    struct hstex_box subscript = {0};
    if (has_superscript &&
        math_field_box(engine, &noad->superscript, superscript_style,
                       &superscript, error, error_capacity) != 0) {
        return -1;
    }
    if (has_subscript &&
        math_field_box(engine, &noad->subscript, subscript_style, &subscript,
                       error, error_capacity) != 0) {
        return -1;
    }
    superscript.width += scriptspace;
    subscript.width += scriptspace;

    if (!has_superscript) {
        int32_t floor_one = 0;
        if (math_symbol_parameter(engine, size, 16U, &floor_one, error,
                                  error_capacity) != 0) {
            return -1;
        }
        if (shift_down < floor_one) {
            shift_down = floor_one;
        }
        int32_t clearance =
            subscript.height - (int32_t)(((int64_t)x_height_magnitude * 4) / 5);
        if (shift_down < clearance) {
            shift_down = clearance;
        }
        uint32_t identifier = 0U;
        return store_box_node(engine, &subscript, shift_down, &identifier,
                              error, error_capacity) != 0
                   ? -1
                   : append_hbox_item(engine, identifier, error,
                                      error_capacity);
    }

    /* A cramped style takes the third superscript parameter, an uncramped one
       the second; the first belongs to display style, which is not here. */
    int32_t superscript_floor = 0;
    if (math_symbol_parameter(engine, size, (style % 2U) != 0U ? 15U : 14U,
                              &superscript_floor, error, error_capacity) != 0) {
        return -1;
    }
    if (shift_up < superscript_floor) {
        shift_up = superscript_floor;
    }
    int32_t clearance = superscript.depth + x_height_magnitude / 4;
    if (shift_up < clearance) {
        shift_up = clearance;
    }

    if (!has_subscript) {
        uint32_t identifier = 0U;
        return store_box_node(engine, &superscript, -shift_up, &identifier,
                              error, error_capacity) != 0
                   ? -1
                   : append_hbox_item(engine, identifier, error,
                                      error_capacity);
    }

    int32_t floor_two = 0;
    int32_t rule_thickness = 0;
    if (math_symbol_parameter(engine, size, 17U, &floor_two, error,
                              error_capacity) != 0 ||
        math_extension_parameter(engine, size, 8U, &rule_thickness, error,
                                 error_capacity) != 0) {
        return -1;
    }
    if (shift_down < floor_two) {
        shift_down = floor_two;
    }
    int64_t gap = (int64_t)4 * rule_thickness -
                  (((int64_t)shift_up - superscript.depth) -
                   ((int64_t)subscript.height - shift_down));
    if (gap > 0) {
        shift_down += (int32_t)gap;
        int64_t rise = ((int64_t)x_height_magnitude * 4) / 5 -
                       ((int64_t)shift_up - superscript.depth);
        if (rise > 0) {
            shift_up += (int32_t)rise;
            shift_down -= (int32_t)rise;
        }
    }

    /* The two scripts stack in a vertical list: the superscript on top,
       displaced right by the nucleus's italic correction, then a kern that
       holds them the computed distance apart, then the subscript. */
    int32_t separation = (shift_up - superscript.depth) -
                         (subscript.height - shift_down);
    uint32_t superscript_node = 0U;
    uint32_t subscript_node = 0U;
    uint32_t kern_node = 0U;
    struct hstex_node kern = {
        .kind = HSTEX_NODE_KERN,
        .width = separation,
    };
    if (store_box_node(engine, &superscript, delta, &superscript_node, error,
                       error_capacity) != 0 ||
        store_node(engine, &kern, &kern_node, error, error_capacity) != 0 ||
        store_box_node(engine, &subscript, 0, &subscript_node, error,
                       error_capacity) != 0) {
        return -1;
    }
    uint32_t items[3] = {superscript_node, kern_node, subscript_node};
    if (engine->list_item_count > (size_t)UINT32_MAX - 3U ||
        reserve_list_items(engine, engine->list_item_count + 3U, error,
                           error_capacity) != 0) {
        return -1;
    }
    struct hstex_box stack = {0};
    stack.kind = HSTEX_BOX_VLIST;
    stack.node_start = (uint32_t)engine->list_item_count;
    stack.node_count = 3U;
    for (size_t index = 0U; index < 3U; ++index) {
        engine->list_items[engine->list_item_count++] = items[index];
    }
    /* A vertical package reaches from the top of the first box down to the
       baseline of the last, and carries the last box's depth below it. */
    stack.height = superscript.height + superscript.depth + separation +
                   subscript.height;
    stack.depth = subscript.depth;
    int32_t left = superscript.width + delta;
    stack.width = left > subscript.width ? left : subscript.width;
    uint32_t identifier = 0U;
    return store_box_node(engine, &stack, shift_down, &identifier, error,
                          error_capacity) != 0
               ? -1
               : append_hbox_item(engine, identifier, error, error_capacity);
}

/* Translate one finished math list into the horizontal list the engine is
   building. The caller has already pointed active_hbox_builder at it. */
/* Put the branch of every \mathchoice that the style in force asks for in
   the list's place, before anything else is done with the list. A branch may
   hold a \mathchoice of its own, so the list is swept until none is left. */
static int expand_math_choices(struct hstex_engine *engine,
                               struct hstex_math_builder *builder, char *error,
                               size_t error_capacity)
{
    for (size_t sweep = 0U; sweep < 64U; ++sweep) {
        bool any = false;
        for (size_t index = 0U; index < builder->count; ++index) {
            if (builder->noads[index].kind == (uint8_t)HSTEX_NOAD_CHOICE) {
                any = true;
                break;
            }
        }
        if (!any) {
            return 0;
        }
        struct hstex_math_builder out = {0};
        uint8_t style = builder->style;
        int status = 0;
        for (size_t index = 0U; status == 0 && index < builder->count;
             ++index) {
            const struct hstex_noad *noad = &builder->noads[index];
            if (noad->kind == (uint8_t)HSTEX_NOAD_STYLE) {
                style = noad->atom_class;
            }
            if (noad->kind != (uint8_t)HSTEX_NOAD_CHOICE) {
                if (reserve_noads(&out, out.count + 1U, error,
                                  error_capacity) != 0) {
                    status = -1;
                    break;
                }
                out.noads[out.count++] = *noad;
                continue;
            }
            uint32_t record = noad->choices[style / 2U];
            if (record == 0U ||
                (size_t)record > engine->math_sublist_count) {
                continue;
            }
            const struct hstex_math_sublist *chosen =
                &engine->math_sublists[record - 1U];
            if (chosen->has_fraction) {
                /* A branch that is a fraction cannot be spliced, because a
                   fraction is a property of the list it ends: it goes in as
                   one atom, to be set when the style is known. */
                struct hstex_noad atom = {
                    .kind = (uint8_t)HSTEX_NOAD_ATOM,
                    .atom_class = (uint8_t)HSTEX_ATOM_INNER,
                    .nucleus = {.kind = (uint8_t)HSTEX_MATH_FIELD_BOX,
                                .sublist = record,
                                .list_style = (uint8_t)0xFFU},
                };
                if (reserve_noads(&out, out.count + 1U, error,
                                  error_capacity) != 0) {
                    status = -1;
                    break;
                }
                out.noads[out.count++] = atom;
                continue;
            }
            if (chosen->count == 0U) {
                continue;
            }
            if (reserve_noads(&out, out.count + chosen->count, error,
                              error_capacity) != 0) {
                status = -1;
                break;
            }
            memcpy(&out.noads[out.count], engine->math_items + chosen->start,
                   chosen->count * sizeof(*out.noads));
            out.count += chosen->count;
        }
        if (status != 0) {
            free(out.noads);
            return -1;
        }
        free(builder->noads);
        builder->noads = out.noads;
        builder->count = out.count;
        builder->capacity = out.capacity;
    }
    return set_error(error, error_capacity,
                     "\\mathchoice branches nest too deeply");
}

static int translate_math_list(struct hstex_engine *engine,
                               struct hstex_math_builder *builder, char *error,
                               size_t error_capacity)
{
    /* A fraction is split before anything else, because each of its sides
       is set in a style of its own -- which is what decides a \mathchoice
       inside it. */
    if (builder->has_fraction) {
        return translate_math_fraction(engine, builder, error, error_capacity);
    }
    if (expand_math_choices(engine, builder, error, error_capacity) != 0) {
        return -1;
    }
    uint8_t style = builder->style;
    uint8_t size = math_size_of_style(style);
    bool all_spacing = style < (uint8_t)HSTEX_STYLE_SCRIPT;
    resolve_binary_atoms(builder);
    if (apply_math_ligatures(engine, builder, style, error, error_capacity) !=
        0) {
        return -1;
    }
    int previous_class = -1;
    for (size_t index = 0U; index < builder->count; ++index) {
        struct hstex_noad *noad = &builder->noads[index];
        if (noad->kind == (uint8_t)HSTEX_NOAD_STYLE) {
            style = noad->atom_class;
            size = math_size_of_style(style);
            all_spacing = style < (uint8_t)HSTEX_STYLE_SCRIPT;
            continue;
        }
        if (noad->kind == (uint8_t)HSTEX_NOAD_RADICAL &&
            build_math_radical(engine, noad, style, error, error_capacity) !=
                0) {
            return -1;
        }
        if ((noad->kind == (uint8_t)HSTEX_NOAD_OVERLINE ||
             noad->kind == (uint8_t)HSTEX_NOAD_UNDERLINE) &&
            build_math_line(engine, noad, style,
                            noad->kind == (uint8_t)HSTEX_NOAD_OVERLINE, error,
                            error_capacity) != 0) {
            return -1;
        }
        if (noad->kind == (uint8_t)HSTEX_NOAD_NONSCRIPT) {
            /* In a script the glue or kern that follows goes away with it;
               see docs/DECISIONS.md, nonscript. */
            if (style >= (uint8_t)HSTEX_STYLE_SCRIPT &&
                index + 1U < builder->count) {
                const struct hstex_noad *next = &builder->noads[index + 1U];
                bool removable =
                    next->kind == (uint8_t)HSTEX_NOAD_MU_GLUE ||
                    next->kind == (uint8_t)HSTEX_NOAD_MU_KERN;
                if (!removable && next->kind == (uint8_t)HSTEX_NOAD_NODE &&
                    next->node != 0U && next->node <= engine->node_count) {
                    enum hstex_node_kind following =
                        engine->nodes[next->node - 1U].kind;
                    removable = following == HSTEX_NODE_GLUE ||
                                following == HSTEX_NODE_KERN;
                }
                if (removable) {
                    ++index;
                }
            }
            continue;
        }
        bool middle_atom = noad->kind == (uint8_t)HSTEX_NOAD_MIDDLE;
        if (middle_atom) {
            /* While the group is being measured the delimiter is not there
               yet; it takes its size from the whole group. */
            noad->kind = (uint8_t)HSTEX_NOAD_ATOM;
            noad->atom_class = (uint8_t)HSTEX_ATOM_CLOSE;
            memset(&noad->nucleus, 0, sizeof(noad->nucleus));
            if (engine->middle_delimiter_size >= 0) {
                struct hstex_box piece = {0};
                int32_t axis = 0;
                if (variant_delimiter(engine, noad->delimiter, size,
                                      engine->middle_delimiter_size, &piece,
                                      error, error_capacity) != 0 ||
                    math_symbol_parameter(engine, size, 22U, &axis, error,
                                          error_capacity) != 0) {
                    return -1;
                }
                uint32_t placed = 0U;
                if (store_box_node(engine, &piece,
                                   half_of((int64_t)piece.height -
                                           piece.depth) -
                                       axis,
                                   &placed, error, error_capacity) != 0) {
                    return -1;
                }
                noad->nucleus.kind = (uint8_t)HSTEX_MATH_FIELD_BOX;
                noad->nucleus.node = placed;
            }
        }
        if (noad->kind == (uint8_t)HSTEX_NOAD_NODE) {
            if (append_hbox_item(engine, noad->node, error, error_capacity) !=
                0) {
                return -1;
            }
            continue;
        }
        if (noad->kind == (uint8_t)HSTEX_NOAD_MU_GLUE ||
            noad->kind == (uint8_t)HSTEX_NOAD_MU_KERN) {
            int32_t unit = 0;
            if (math_unit(engine, size, &unit, error, error_capacity) != 0) {
                return -1;
            }
            int status =
                noad->kind == (uint8_t)HSTEX_NOAD_MU_GLUE
                    ? emit_math_glue(engine,
                                     math_glue_in_points(noad->glue, unit),
                                     error, error_capacity)
                    : emit_math_kern(engine, scaled_by_unit(noad->kern, unit),
                                     error, error_capacity);
            if (status != 0) {
                return -1;
            }
            continue;
        }
        if (previous_class >= 0) {
            uint8_t which =
                math_spacing_table[previous_class][noad->atom_class];
            if (which != 0U &&
                (all_spacing ||
                 math_spacing_all_styles[previous_class][noad->atom_class])) {
                static const size_t parameters[4] = {
                    0U,
                    (size_t)HSTEX_MUGLUE_THIN,
                    (size_t)HSTEX_MUGLUE_MEDIUM,
                    (size_t)HSTEX_MUGLUE_THICK,
                };
                int32_t unit = 0;
                if (math_unit(engine, size, &unit, error, error_capacity) !=
                    0) {
                    return -1;
                }
                struct hstex_glue amount = math_glue_in_points(
                    engine->muglue_parameters[parameters[which]], unit);
                if (emit_math_glue(engine, amount, error, error_capacity) !=
                    0) {
                    return -1;
                }
            }
        }
        previous_class = middle_atom ? (int)HSTEX_ATOM_OPEN
                                     : (int)noad->atom_class;

        int32_t nucleus_height = 0;
        int32_t nucleus_depth = 0;
        int32_t italic = 0;
        bool nucleus_is_character =
            noad->nucleus.kind == (uint8_t)HSTEX_MATH_FIELD_CHARACTER ||
            noad->nucleus.single_character != 0U;
        if (math_field_restyle(engine, &noad->nucleus, style, error,
                               error_capacity) != 0) {
            return -1;
        }
        nucleus_is_character =
            noad->nucleus.kind == (uint8_t)HSTEX_MATH_FIELD_CHARACTER ||
            noad->nucleus.single_character != 0U;
        if (noad->nucleus.kind == (uint8_t)HSTEX_MATH_FIELD_BOX) {
            if (noad->nucleus.node == 0U ||
                noad->nucleus.node > engine->node_count) {
                return set_error(error, error_capacity,
                                 "math list refers to a missing node");
            }
            struct hstex_node contributed = engine->nodes[noad->nucleus.node - 1U];
            if (noad->atom_class == (uint8_t)HSTEX_ATOM_OP &&
                centre_on_axis(engine, size, &contributed, error,
                               error_capacity) != 0) {
                return -1;
            }
            nucleus_height = contributed.height - contributed.shift;
            nucleus_depth = contributed.depth + contributed.shift;
            if (append_hbox_node(engine, &contributed, error, error_capacity) !=
                0) {
                return -1;
            }
        } else if (nucleus_is_character) {
            const struct hstex_font *font = NULL;
            const struct hstex_char_metric *metric = NULL;
            int present = math_character_metric(engine, &noad->nucleus, size,
                                                &font, &metric, error,
                                                error_capacity);
            if (present < 0) {
                return -1;
            }
            if (present == 0) {
                /* The font has no such character; the reference carries on
                   without it. */
                if (attach_math_scripts(engine, noad, style, 0, 0, true, 0,
                                        error, error_capacity) != 0) {
                    return -1;
                }
                continue;
            }
            struct hstex_node node = {
                .kind = HSTEX_NODE_CHARACTER,
                .width = metric->width,
                .height = metric->height,
                .depth = metric->depth,
                .value.character = {
                    .font = engine->math_fonts[size][noad->nucleus.family],
                    .character = noad->nucleus.character,
                },
            };
            if (noad->atom_class == (uint8_t)HSTEX_ATOM_OP &&
                centre_on_axis(engine, size, &node, error, error_capacity) !=
                    0) {
                return -1;
            }
            nucleus_height = node.height - node.shift;
            nucleus_depth = node.depth + node.shift;
            italic = metric->italic;
            if (append_hbox_node(engine, &node, error, error_capacity) != 0) {
                return -1;
            }
        }
        if (attach_math_scripts(engine, noad, style, nucleus_height,
                                nucleus_depth, nucleus_is_character, italic,
                                error, error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}

static int append_display_glue(struct hstex_engine *engine,
                               struct hstex_glue glue, char *error,
                               size_t error_capacity)
{
    struct hstex_node node = {
        .kind = HSTEX_NODE_GLUE,
        .width = glue.width,
        .value.glue = {
            .stretch = glue.stretch,
            .shrink = glue.shrink,
            .stretch_order = glue.stretch_order,
            .shrink_order = glue.shrink_order,
        },
    };
    return append_vbox_node(engine, &node, error, error_capacity);
}

/* The width the last line before a display reaches to, which decides whether
   the display gets the short skips. Glue that can stretch or shrink before
   the last visible node makes the width indeterminate. See
   docs/DECISIONS.md, display-math. */
static int32_t pre_display_size(struct hstex_engine *engine,
                                const struct hstex_box *line, bool had_line)
{
    if (!had_line) {
        return -HSTEX_MAX_DIMEN;
    }
    const struct hstex_font *font =
        font_by_identifier(engine, engine->current_font);
    int64_t reach = 0;
    if (font != NULL && font->dimen_count >= 6U) {
        reach += (int64_t)2 * font->dimens[5];
    }
    int64_t width = -(int64_t)HSTEX_MAX_DIMEN;
    bool indeterminate = false;
    for (uint32_t offset = 0U; offset < line->node_count; ++offset) {
        size_t slot = (size_t)line->node_start + offset;
        if (slot >= engine->list_item_count) {
            break;
        }
        uint32_t identifier = engine->list_items[slot];
        if (identifier == 0U || identifier > engine->node_count) {
            break;
        }
        const struct hstex_node *node = &engine->nodes[identifier - 1U];
        int32_t amount = packed_dimen(node->width);
        switch (node->kind) {
        case HSTEX_NODE_CHARACTER:
        case HSTEX_NODE_LIGATURE:
        case HSTEX_NODE_RULE:
        case HSTEX_NODE_LIST:
            if (indeterminate) {
                return HSTEX_MAX_DIMEN;
            }
            reach += amount;
            width = reach;
            break;
        case HSTEX_NODE_GLUE:
            /* Only glue that can stretch or shrink without limit makes the
               reach unknowable; finite glue just counts. */
            if (node->value.glue.stretch_order != 0U ||
                node->value.glue.shrink_order != 0U) {
                indeterminate = true;
            }
            reach += amount;
            break;
        default:
            reach += amount;
            break;
        }
    }
    if (width < -(int64_t)HSTEX_MAX_DIMEN) {
        width = -(int64_t)HSTEX_MAX_DIMEN;
    }
    return (int32_t)width;
}

/* $$ in a paragraph ends the lines built so far, then sets the formula in
   display style. */
static int begin_display_math(struct hstex_engine *engine, char *error,
                              size_t error_capacity)
{
    struct hstex_box line = {0};
    bool had_line = engine->paragraph_builder != NULL &&
                    engine->paragraph_builder->count != 0U;
    if (finish_paragraph_line(engine, &line, error, error_capacity) != 0) {
        return -1;
    }
    int32_t size = pre_display_size(engine, &line, had_line);
    /* The display stands where the line after the paragraph so far would,
       plus one: the reference counts the display as taking two. */
    int32_t where = had_line ? engine->paragraph_lines + 2 : 2;
    if (assign_dimen_parameter(engine, (uint32_t)HSTEX_DIMEN_PRE_DISPLAY_SIZE,
                               size, false, error, error_capacity) != 0 ||
        assign_dimen_parameter(engine, (uint32_t)HSTEX_DIMEN_DISPLAY_WIDTH,
                               line_width_for(engine, where), false, error,
                               error_capacity) != 0 ||
        assign_dimen_parameter(engine, (uint32_t)HSTEX_DIMEN_DISPLAY_INDENT,
                               line_shift_for(engine, where), false, error,
                               error_capacity) != 0) {
        return -1;
    }
    if (begin_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (assign_integer_parameter(engine, (uint32_t)HSTEX_INTEGER_FAMILY, -1,
                                 false, error, error_capacity) != 0 ||
        push_math_list(engine, (uint8_t)HSTEX_STYLE_DISPLAY, error,
                       error_capacity) != 0) {
        return -1;
    }
    engine->mode = HSTEX_MODE_MATH;
    engine->inner_mode = false;
    engine->displayed_math = true;
    uint32_t every = engine->token_parameters[HSTEX_TOKEN_EVERY_DISPLAY];
    if (every == 0U) {
        return 0;
    }
    const struct hstex_token_list *list = token_list_by_identifier(engine, every);
    struct hstex_source_location location = {0};
    if (list == NULL ||
        hstex_source_push_tokens(&engine->sources, list->tokens, list->count,
                                 location, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "could not install everydisplay tokens");
    }
    return 0;
}

/* The closing $$ centres the formula in \displaywidth, surrounds it with the
   display penalties and skips, and lets the paragraph carry on. */
/* Translate the formula the engine has been reading into a box. */
static int package_displayed_formula(struct hstex_engine *engine,
                                     struct hstex_box *box, char *error,
                                     size_t error_capacity)
{
    struct hstex_math_builder *list = current_math_list(engine);
    if (list == NULL || engine->math_depth != engine->math_floor + 1U) {
        return set_error(error, error_capacity,
                         "a display closed inside a math group");
    }
    struct hstex_hbox_builder builder = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &builder;
    int status = translate_math_list(engine, list, error, error_capacity);
    engine->active_hbox_builder = previous;
    if (status == 0) {
        status = finalize_hbox(engine, &builder, false, false, 0, box, error,
                               error_capacity);
    }
    free(builder.node_identifiers);
    pop_math_list(engine);
    return status;
}

/* Put one character in a box of its own, as wide as the character plus its
   italic correction. */
static int character_box(struct hstex_engine *engine, uint32_t font_identifier,
                         const struct hstex_font *font, uint8_t code,
                         struct hstex_box *box, char *error,
                         size_t error_capacity)
{
    const struct hstex_char_metric *metric = &font->characters[code];
    struct hstex_node node = {
        .kind = HSTEX_NODE_CHARACTER,
        .width = metric->width,
        .height = metric->height,
        .depth = metric->depth,
        .value.character = {.font = font_identifier, .character = code},
    };
    struct hstex_hbox_builder builder = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &builder;
    int status = append_hbox_node(engine, &node, error, error_capacity);
    engine->active_hbox_builder = previous;
    if (status == 0) {
        status = finalize_hbox(engine, &builder, false, false, 0, box, error,
                               error_capacity);
    }
    free(builder.node_identifiers);
    if (status == 0) {
        box->width = metric->width + metric->italic;
    }
    return status;
}

/* Stack the pieces of an extensible recipe until they reach `wanted`. */
static int extensible_box(struct hstex_engine *engine,
                          uint32_t font_identifier,
                          const struct hstex_font *font,
                          const struct hstex_extensible *recipe,
                          int32_t wanted, struct hstex_box *box, char *error,
                          size_t error_capacity)
{
    if (recipe->repeated == 0U ||
        font->characters[recipe->repeated].tag < 0) {
        return set_error(error, error_capacity,
                         "an extensible delimiter has no repeated piece");
    }
    const struct hstex_char_metric *piece =
        &font->characters[recipe->repeated];
    int64_t step = (int64_t)piece->height + piece->depth;
    int64_t reach = 0;
    uint8_t fixed[3] = {recipe->bottom, recipe->middle, recipe->top};
    for (size_t index = 0U; index < 3U; ++index) {
        if (fixed[index] != 0U && font->characters[fixed[index]].tag >= 0) {
            reach += (int64_t)font->characters[fixed[index]].height +
                     font->characters[fixed[index]].depth;
        }
    }
    size_t repeats = 0U;
    if (step > 0) {
        while (reach < wanted) {
            reach += step;
            ++repeats;
            if (recipe->middle != 0U) {
                reach += step;
            }
            if (repeats > 10000U) {
                return set_error(error, error_capacity,
                                 "an extensible delimiter did not converge");
            }
        }
    }

    /* The pieces go in from the bottom up, and the last one placed gives the
       box its height. */
    struct hstex_vbox_builder builder = {0};
    struct hstex_vbox_builder *previous_vbox = engine->active_vbox_builder;
    struct hstex_hbox_builder *previous_hbox = engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    int32_t previous_depth = engine->prev_depth;
    engine->active_vbox_builder = &builder;
    engine->active_hbox_builder = NULL;
    engine->mode = HSTEX_MODE_VERTICAL;
    engine->prev_depth = HSTEX_IGNORE_DEPTH;

    int status = 0;
    int32_t top_height = 0;
    int64_t total = 0;
    bool first = true;
    /* Written from the top down: top, then the repeats and the middle, then
       the repeats again, then the bottom. */
    size_t written = 0U;
    for (size_t stage = 0U; status == 0 && stage < 5U; ++stage) {
        size_t times = 1U;
        uint8_t code = 0U;
        switch (stage) {
        case 0: code = recipe->top; break;
        case 1: code = recipe->middle != 0U ? recipe->repeated : 0U;
                times = repeats; break;
        case 2: code = recipe->middle; break;
        case 3: code = recipe->repeated; times = repeats; break;
        default: code = recipe->bottom; break;
        }
        if (code == 0U || font->characters[code].tag < 0) {
            continue;
        }
        for (size_t index = 0U; status == 0 && index < times; ++index) {
            struct hstex_box piece_box = {0};
            status = character_box(engine, font_identifier, font, code,
                                   &piece_box, error, error_capacity);
            if (status != 0) {
                break;
            }
            if (first) {
                top_height = piece_box.height;
                first = false;
            }
            total += (int64_t)piece_box.height + piece_box.depth;
            ++written;
            struct hstex_node node = {
                .kind = HSTEX_NODE_LIST,
                .width = piece_box.width,
                .height = piece_box.height,
                .depth = piece_box.depth,
                .value.list = {
                    .node_start = piece_box.node_start,
                    .node_count = piece_box.node_count,
                    .box_kind = piece_box.kind,
                },
            };
            status = append_vbox_node(engine, &node, error, error_capacity);
        }
    }
    (void)written;
    struct hstex_box packed = {0};
    if (status == 0) {
        status = finalize_vbox(engine, &builder, false, false, 0, &packed,
                               error, error_capacity);
    }
    free(builder.node_identifiers);
    engine->active_vbox_builder = previous_vbox;
    engine->active_hbox_builder = previous_hbox;
    engine->mode = previous_mode;
    engine->prev_depth = previous_depth;
    if (status != 0) {
        return -1;
    }
    packed.height = top_height;
    packed.depth = (int32_t)(total - top_height);
    packed.width = piece->width + piece->italic;
    *box = packed;
    return 0;
}

/* Find a delimiter at least `wanted` tall: walk the small variant's chain of
   larger versions, then the large variant's, and fall back to building one
   out of pieces. See docs/DECISIONS.md, extensible-delimiters. */
static int variant_delimiter(struct hstex_engine *engine, int32_t code,
                             uint8_t size, int32_t wanted,
                             struct hstex_box *box, char *error,
                             size_t error_capacity)
{
    uint8_t families[2] = {(uint8_t)((code >> 20) & 0xF),
                           (uint8_t)((code >> 8) & 0xF)};
    uint8_t characters[2] = {(uint8_t)((code >> 12) & 0xFF),
                             (uint8_t)(code & 0xFF)};
    const struct hstex_font *best_font = NULL;
    uint32_t best_identifier = 0U;
    uint8_t best_character = 0U;
    int64_t best_reach = -1;
    bool extensible = false;

    for (size_t attempt = 0U; attempt < 2U && !extensible; ++attempt) {
        if (families[attempt] == 0U && characters[attempt] == 0U) {
            continue;
        }
        for (int32_t which = (int32_t)size; which >= 0; --which) {
            uint32_t identifier =
                engine->math_fonts[which][families[attempt]];
            const struct hstex_font *font =
                font_by_identifier(engine, identifier);
            if (font == NULL || font->characters == NULL) {
                continue;
            }
            uint8_t character = characters[attempt];
            for (size_t step = 0U; step < 256U; ++step) {
                const struct hstex_char_metric *metric =
                    &font->characters[character];
                if (metric->tag < 0) {
                    break;
                }
                if (metric->tag == 3) {
                    best_font = font;
                    best_identifier = identifier;
                    best_character = character;
                    extensible = true;
                    break;
                }
                int64_t reach = (int64_t)metric->height + metric->depth;
                if (reach > best_reach) {
                    best_font = font;
                    best_identifier = identifier;
                    best_character = character;
                    best_reach = reach;
                    if (reach >= wanted) {
                        break;
                    }
                }
                if (metric->tag != 2) {
                    break;
                }
                character = (uint8_t)metric->remainder;
            }
            if (extensible || best_reach >= wanted) {
                break;
            }
        }
        if (extensible || best_reach >= wanted) {
            break;
        }
    }

    memset(box, 0, sizeof(*box));
    box->kind = HSTEX_BOX_HLIST;
    if (best_font == NULL) {
        /* No variant at all: an empty box of \nulldelimiterspace. */
        box->width = engine->dimen_parameters[HSTEX_DIMEN_NULL_DELIMITER_SPACE];
        return 0;
    }
    if (extensible) {
        const struct hstex_char_metric *metric =
            &best_font->characters[best_character];
        if ((size_t)metric->remainder >= best_font->extensible_count) {
            return set_error(error, error_capacity,
                             "extensible recipe %d is out of range",
                             metric->remainder);
        }
        return extensible_box(
            engine, best_identifier, best_font,
            &best_font->extensibles[(size_t)metric->remainder], wanted, box,
            error, error_capacity);
    }
    return character_box(engine, best_identifier, best_font, best_character,
                         box, error, error_capacity);
}

/* The style a fraction sets its numerator in, and its denominator, which is
   the cramped variant of the same size. Both were measured with
   \mathchoice; see docs/DECISIONS.md, fractions. */
static uint8_t math_numerator_style(uint8_t style)
{
    return (uint8_t)(style + 2U - 2U * (style / 6U));
}

static uint8_t math_denominator_style(uint8_t style)
{
    return (uint8_t)(2U * (style / 2U) + 3U - 2U * (style / 6U));
}

/* The cramped variant of a style: the same size, with superscripts kept
   lower. */
static uint8_t math_cramped_style(uint8_t style)
{
    return (uint8_t)(style | 1U);
}

/* Set part of a math list -- one side of a fraction -- as a box of its own. */
static int math_range_box(struct hstex_engine *engine,
                          const struct hstex_math_builder *builder,
                          size_t begin, size_t end, uint8_t style,
                          struct hstex_box *box, char *error,
                          size_t error_capacity)
{
    struct hstex_math_builder part = {0};
    part.forced_class = -1;
    part.style = style;
    part.current_style = style;
    size_t count = end - begin;
    if (count != 0U) {
        if (reserve_noads(&part, count, error, error_capacity) != 0) {
            return -1;
        }
        memcpy(part.noads, builder->noads + begin,
               count * sizeof(*part.noads));
        part.count = count;
    }
    struct hstex_hbox_builder packed = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_hbox_builder = &packed;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    int status = translate_math_list(engine, &part, error, error_capacity);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    if (status == 0) {
        status = finalize_hbox(engine, &packed, false, false, 0, box, error,
                               error_capacity);
    }
    free(packed.node_identifiers);
    free(part.noads);
    return status;
}

/* Widen a box to match the other side of the fraction, its contents centred
   between two \hss. A box with nothing in it is simply declared wider. */
static int math_rebox(struct hstex_engine *engine, struct hstex_box *box,
                      int32_t width, char *error, size_t error_capacity)
{
    if (box->width == width) {
        return 0;
    }
    if (box->node_count == 0U) {
        box->width = width;
        return 0;
    }
    struct hstex_hbox_builder builder = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_hbox_builder = &builder;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    struct hstex_node fill = {
        .kind = HSTEX_NODE_GLUE,
        .value.glue = {
            .stretch = INT32_C(65536),
            .shrink = INT32_C(65536),
            .stretch_order = 1U,
            .shrink_order = 1U,
        },
    };
    int status = append_hbox_node(engine, &fill, error, error_capacity);
    for (uint32_t offset = 0U; status == 0 && offset < box->node_count;
         ++offset) {
        size_t slot = (size_t)box->node_start + offset;
        if (slot >= engine->list_item_count) {
            status = set_error(error, error_capacity,
                               "a fraction lost part of its list");
            break;
        }
        status = append_hbox_item(engine, engine->list_items[slot], error,
                                  error_capacity);
    }
    if (status == 0) {
        status = append_hbox_node(engine, &fill, error, error_capacity);
    }
    struct hstex_box packed = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &builder, true, false, width, &packed,
                               error, error_capacity);
    }
    free(builder.node_identifiers);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    if (status == 0) {
        *box = packed;
    }
    return status;
}

/* \radical puts a sign of its own height beside the radicand, with a rule
   over it. All of the arithmetic here was measured; see docs/DECISIONS.md,
   radicals. */
static int build_math_radical(struct hstex_engine *engine,
                              struct hstex_noad *noad, uint8_t style,
                              char *error, size_t error_capacity)
{
    uint8_t size = math_size_of_style(style);
    int32_t thickness = 0;
    if (math_extension_parameter(engine, size, 8U, &thickness, error,
                                 error_capacity) != 0) {
        return -1;
    }
    struct hstex_box radicand = {0};
    if (math_field_box(engine, &noad->nucleus, math_cramped_style(style),
                       &radicand, error, error_capacity) != 0) {
        return -1;
    }
    /* How much room to leave over the radicand: a quarter of the x height in
       display, a quarter of the rule's own thickness otherwise. */
    int32_t reference = thickness;
    if (style < (uint8_t)HSTEX_STYLE_TEXT &&
        math_symbol_parameter(engine, size, 5U, &reference, error,
                              error_capacity) != 0) {
        return -1;
    }
    if (reference < 0) {
        reference = -reference;
    }
    int64_t clearance = (int64_t)thickness + reference / 4;
    int64_t wanted =
        (int64_t)radicand.height + radicand.depth + clearance + thickness;
    if (wanted > HSTEX_MAX_DIMEN) {
        wanted = HSTEX_MAX_DIMEN;
    }
    struct hstex_box sign = {0};
    if (variant_delimiter(engine, noad->delimiter, size, (int32_t)wanted,
                          &sign, error, error_capacity) != 0) {
        return -1;
    }
    /* A sign taller than it needs to be spreads the extra room evenly. */
    int64_t excess = (int64_t)sign.depth -
                     ((int64_t)radicand.height + radicand.depth + clearance);
    if (excess > 0) {
        clearance += half_of(excess);
    }

    struct hstex_vbox_builder body = {0};
    struct hstex_vbox_builder *previous_vbox = engine->active_vbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_vbox_builder = &body;
    engine->mode = HSTEX_MODE_VERTICAL;
    /* The bar over the radicand is as thick as the sign is tall, and sits
       that same distance below the top of the whole thing; the default rule
       thickness only decides how much room is left over the radicand. */
    struct hstex_node above = {.kind = HSTEX_NODE_KERN, .width = sign.height};
    struct hstex_node rule = {
        .kind = HSTEX_NODE_RULE,
        .width = HSTEX_RUNNING_DIMEN,
        .height = sign.height,
        .depth = 0,
    };
    struct hstex_node gap = {.kind = HSTEX_NODE_KERN,
                             .width = (int32_t)clearance};
    uint32_t identifier = 0U;
    int status = append_vbox_node(engine, &above, error, error_capacity);
    if (status == 0) {
        status = append_vbox_node(engine, &rule, error, error_capacity);
    }
    if (status == 0) {
        status = append_vbox_node(engine, &gap, error, error_capacity);
    }
    if (status == 0 &&
        (store_box_node(engine, &radicand, 0, &identifier, error,
                        error_capacity) != 0 ||
         append_vbox_item(engine, identifier, error, error_capacity) != 0)) {
        status = -1;
    }
    struct hstex_box over = {0};
    if (status == 0) {
        status = finalize_vbox(engine, &body, false, false, 0, &over, error,
                               error_capacity);
    }
    free(body.node_identifiers);
    engine->active_vbox_builder = previous_vbox;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    over.width = radicand.width;
    over.height = (int32_t)(2 * (int64_t)sign.height + clearance +
                            radicand.height);
    over.depth = radicand.depth;

    struct hstex_hbox_builder whole = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &whole;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    status = 0;
    int32_t shift =
        (int32_t)(-((int64_t)radicand.height + clearance));
    identifier = 0U;
    if (store_box_node(engine, &sign, shift, &identifier, error,
                       error_capacity) != 0 ||
        append_hbox_item(engine, identifier, error, error_capacity) != 0 ||
        store_box_node(engine, &over, 0, &identifier, error,
                       error_capacity) != 0 ||
        append_hbox_item(engine, identifier, error, error_capacity) != 0) {
        status = -1;
    }
    struct hstex_box packed = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &whole, false, false, 0, &packed, error,
                               error_capacity);
    }
    free(whole.node_identifiers);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    identifier = 0U;
    if (store_box_node(engine, &packed, 0, &identifier, error,
                       error_capacity) != 0) {
        return -1;
    }
    noad->kind = (uint8_t)HSTEX_NOAD_ATOM;
    noad->atom_class = (uint8_t)HSTEX_ATOM_ORD;
    memset(&noad->nucleus, 0, sizeof(noad->nucleus));
    noad->nucleus.kind = (uint8_t)HSTEX_MATH_FIELD_BOX;
    noad->nucleus.node = identifier;
    return 0;
}

/* \overline and \underline put a rule of the default thickness over or under
   what follows, three thicknesses clear of it. See docs/DECISIONS.md,
   over-and-underline. */
static int build_math_line(struct hstex_engine *engine,
                           struct hstex_noad *noad, uint8_t style, bool over,
                           char *error, size_t error_capacity)
{
    uint8_t size = math_size_of_style(style);
    int32_t thickness = 0;
    if (math_extension_parameter(engine, size, 8U, &thickness, error,
                                 error_capacity) != 0) {
        return -1;
    }
    /* A bar over cramps what is under it; a bar under does not. */
    struct hstex_box inner = {0};
    if (math_field_box(engine, &noad->nucleus,
                       over ? math_cramped_style(style) : style, &inner, error,
                       error_capacity) != 0) {
        return -1;
    }
    struct hstex_node rule = {
        .kind = HSTEX_NODE_RULE,
        .width = HSTEX_RUNNING_DIMEN,
        .height = thickness,
        .depth = 0,
    };
    struct hstex_node edge = {.kind = HSTEX_NODE_KERN, .width = thickness};
    struct hstex_node gap = {.kind = HSTEX_NODE_KERN,
                             .width = (int32_t)(3 * (int64_t)thickness)};

    struct hstex_vbox_builder body = {0};
    struct hstex_vbox_builder *previous_vbox = engine->active_vbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_vbox_builder = &body;
    engine->mode = HSTEX_MODE_VERTICAL;
    uint32_t identifier = 0U;
    int status = 0;
    if (over) {
        if (append_vbox_node(engine, &edge, error, error_capacity) != 0 ||
            append_vbox_node(engine, &rule, error, error_capacity) != 0 ||
            append_vbox_node(engine, &gap, error, error_capacity) != 0 ||
            store_box_node(engine, &inner, 0, &identifier, error,
                           error_capacity) != 0 ||
            append_vbox_item(engine, identifier, error, error_capacity) != 0) {
            status = -1;
        }
    } else if (store_box_node(engine, &inner, 0, &identifier, error,
                              error_capacity) != 0 ||
               append_vbox_item(engine, identifier, error, error_capacity) !=
                   0 ||
               append_vbox_node(engine, &gap, error, error_capacity) != 0 ||
               append_vbox_node(engine, &rule, error, error_capacity) != 0) {
        status = -1;
    }
    struct hstex_box packed = {0};
    if (status == 0) {
        status = finalize_vbox(engine, &body, false, false, 0, &packed, error,
                               error_capacity);
    }
    free(body.node_identifiers);
    engine->active_vbox_builder = previous_vbox;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    packed.width = inner.width;
    if (over) {
        packed.height = (int32_t)(5 * (int64_t)thickness + inner.height);
        packed.depth = inner.depth;
    } else {
        /* One thickness more of clear space is left under the rule. */
        packed.height = inner.height;
        packed.depth =
            (int32_t)((int64_t)inner.depth + 5 * (int64_t)thickness);
    }
    identifier = 0U;
    if (store_box_node(engine, &packed, 0, &identifier, error,
                       error_capacity) != 0) {
        return -1;
    }
    noad->kind = (uint8_t)HSTEX_NOAD_ATOM;
    noad->atom_class = (uint8_t)HSTEX_ATOM_ORD;
    memset(&noad->nucleus, 0, sizeof(noad->nucleus));
    noad->nucleus.kind = (uint8_t)HSTEX_MATH_FIELD_BOX;
    noad->nucleus.node = identifier;
    return 0;
}

/* Set a list that holds a generalized fraction. All of the arithmetic here
   was measured; see docs/DECISIONS.md, fractions. */
static int translate_math_fraction(struct hstex_engine *engine,
                                   struct hstex_math_builder *builder,
                                   char *error, size_t error_capacity)
{
    uint8_t style = builder->style;
    uint8_t size = math_size_of_style(style);
    bool display = style < (uint8_t)HSTEX_STYLE_TEXT;
    int32_t axis = 0;
    int32_t default_thickness = 0;
    if (math_symbol_parameter(engine, size, 22U, &axis, error,
                              error_capacity) != 0 ||
        math_extension_parameter(engine, size, 8U, &default_thickness, error,
                                 error_capacity) != 0) {
        return -1;
    }
    int32_t thickness = builder->fraction_default_thickness
                            ? default_thickness
                            : builder->fraction_thickness;

    struct hstex_box numerator = {0};
    struct hstex_box denominator = {0};
    if (math_range_box(engine, builder, 0U, builder->fraction_at,
                       math_numerator_style(style), &numerator, error,
                       error_capacity) != 0 ||
        math_range_box(engine, builder, builder->fraction_at, builder->count,
                       math_denominator_style(style), &denominator, error,
                       error_capacity) != 0) {
        return -1;
    }
    int32_t width = numerator.width > denominator.width ? numerator.width
                                                        : denominator.width;
    if (math_rebox(engine, &numerator, width, error, error_capacity) != 0 ||
        math_rebox(engine, &denominator, width, error, error_capacity) != 0) {
        return -1;
    }

    /* Where the two sides sit before the space between them is checked. */
    int32_t shift_up = 0;
    int32_t shift_down = 0;
    if (math_symbol_parameter(engine, size, display ? 8U : (thickness != 0 ? 9U : 10U),
                              &shift_up, error, error_capacity) != 0 ||
        math_symbol_parameter(engine, size, display ? 11U : 12U, &shift_down,
                              error, error_capacity) != 0) {
        return -1;
    }

    int32_t above = 0;
    int32_t below = 0;
    if (thickness == 0) {
        /* Nothing between the two sides, so the whole gap is checked at
           once and any shortfall is shared equally. */
        int64_t clearance = display ? 7 * (int64_t)default_thickness
                                    : 3 * (int64_t)default_thickness;
        int64_t gap = ((int64_t)shift_up - numerator.depth) -
                      ((int64_t)denominator.height - shift_down);
        if (gap < clearance) {
            int32_t share = half_of(clearance - gap);
            shift_up += share;
            shift_down += share;
        }
        above = (int32_t)(((int64_t)shift_up - numerator.depth) -
                          ((int64_t)denominator.height - shift_down));
    } else {
        /* The rule sits on the axis, and each side is kept clear of it. */
        int64_t clearance = display ? 3 * (int64_t)thickness : thickness;
        int32_t half = half_of(thickness);
        int64_t over = ((int64_t)shift_up - numerator.depth) -
                       ((int64_t)axis + half);
        if (over < clearance) {
            shift_up += (int32_t)(clearance - over);
            over = clearance;
        }
        int64_t under = ((int64_t)axis - half) -
                        ((int64_t)denominator.height - shift_down);
        if (under < clearance) {
            shift_down += (int32_t)(clearance - under);
            under = clearance;
        }
        above = (int32_t)over;
        below = (int32_t)under;
    }

    struct hstex_vbox_builder body = {0};
    struct hstex_vbox_builder *previous_vbox = engine->active_vbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_vbox_builder = &body;
    engine->mode = HSTEX_MODE_VERTICAL;
    int status = 0;
    uint32_t identifier = 0U;
    if (store_box_node(engine, &numerator, 0, &identifier, error,
                       error_capacity) != 0 ||
        append_vbox_item(engine, identifier, error, error_capacity) != 0) {
        status = -1;
    }
    struct hstex_node gap = {.kind = HSTEX_NODE_KERN, .width = above};
    if (status == 0 &&
        append_vbox_node(engine, &gap, error, error_capacity) != 0) {
        status = -1;
    }
    if (status == 0 && thickness != 0) {
        struct hstex_node rule = {
            .kind = HSTEX_NODE_RULE,
            .width = HSTEX_RUNNING_DIMEN,
            .height = thickness,
            .depth = 0,
        };
        struct hstex_node second = {.kind = HSTEX_NODE_KERN, .width = below};
        if (append_vbox_node(engine, &rule, error, error_capacity) != 0 ||
            append_vbox_node(engine, &second, error, error_capacity) != 0) {
            status = -1;
        }
    }
    if (status == 0 &&
        (store_box_node(engine, &denominator, 0, &identifier, error,
                        error_capacity) != 0 ||
         append_vbox_item(engine, identifier, error, error_capacity) != 0)) {
        status = -1;
    }
    struct hstex_box vertical = {0};
    if (status == 0) {
        status = finalize_vbox(engine, &body, false, false, 0, &vertical, error,
                               error_capacity);
    }
    free(body.node_identifiers);
    engine->active_vbox_builder = previous_vbox;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    vertical.width = width;
    vertical.height = (int32_t)((int64_t)shift_up + numerator.height);
    vertical.depth = (int32_t)((int64_t)denominator.depth + shift_down);

    /* The delimiters are as tall as the style asks for, whatever the
       fraction itself came to. */
    int32_t wanted = 0;
    if (math_symbol_parameter(engine, size, display ? 20U : 21U, &wanted, error,
                              error_capacity) != 0) {
        return -1;
    }
    struct hstex_box left = {0};
    struct hstex_box right = {0};
    if (variant_delimiter(engine, builder->fraction_left, size, wanted, &left,
                          error, error_capacity) != 0 ||
        variant_delimiter(engine, builder->fraction_right, size, wanted, &right,
                          error, error_capacity) != 0) {
        return -1;
    }

    struct hstex_hbox_builder whole = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &whole;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    status = 0;
    struct hstex_box *parts[3] = {&left, &vertical, &right};
    for (size_t index = 0U; status == 0 && index < 3U; ++index) {
        int32_t shift = index == 1U ? 0
                                    : half_of((int64_t)parts[index]->height -
                                              parts[index]->depth) -
                                          axis;
        identifier = 0U;
        if (store_box_node(engine, parts[index], shift, &identifier, error,
                           error_capacity) != 0) {
            status = -1;
            break;
        }
        status = append_hbox_item(engine, identifier, error, error_capacity);
    }
    struct hstex_box packed = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &whole, false, false, 0, &packed, error,
                               error_capacity);
    }
    free(whole.node_identifiers);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    identifier = 0U;
    if (store_box_node(engine, &packed, 0, &identifier, error,
                       error_capacity) != 0) {
        return -1;
    }
    return append_hbox_item(engine, identifier, error, error_capacity);
}

/* A delimiter is named either by a character, whose \delcode says which one
   it is, or by \delimiter and a number. */
static int scan_delimiter(struct hstex_engine *engine, int32_t *code,
                          char *error, size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &token, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning a delimiter");
    }
    int32_t value = -1;
    if (hstex_token_is_character(token) &&
        (token_is_category(token, HSTEX_CAT_LETTER) ||
         token_is_category(token, HSTEX_CAT_OTHER))) {
        int table = code_table_index(HSTEX_COMMAND_DEL_CODE);
        if (table < 0) {
            return set_error(error, error_capacity, "no delcode table");
        }
        value = engine->code_tables[(size_t)table]
                                   [hstex_token_character_code(token)];
    } else if (hstex_token_is_control_sequence(token)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(token));
        if (meaning->command == HSTEX_COMMAND_DELIMITER) {
            if (scan_integer(engine, &value, error, error_capacity) != 0) {
                return -1;
            }
        }
    }
    if (value < 0 || value > 0x7FFFFFF) {
        char found[128];
        describe_token(engine, token, found, sizeof(found));
        return set_error(error, error_capacity,
                         "%s is not a delimiter", found);
    }
    *code = value;
    return 0;
}

/* \left opens a list of its own; \right closes it and makes an inner atom of
   the two delimiters with that list between them. */
/* How tall a \\left group's delimiters must be: the contents' reach from the
   axis, scaled by \\delimiterfactor, but never more than
   \\delimitershortfall short of covering them twice over. */
static int32_t delimiter_target(struct hstex_engine *engine, int32_t height,
                                int32_t depth, int32_t axis)
{
    int64_t below = (int64_t)depth + axis;
    int64_t above = (int64_t)height + depth - below;
    int64_t reach = above > below ? above : below;
    int64_t factor =
        (reach / 500) *
        engine->integer_parameters[HSTEX_INTEGER_DELIMITER_FACTOR];
    int64_t shortfall =
        reach + reach -
        engine->dimen_parameters[HSTEX_DIMEN_DELIMITER_SHORTFALL];
    int64_t wanted = factor > shortfall ? factor : shortfall;
    if (wanted < 0) {
        wanted = 0;
    }
    if (wanted > HSTEX_MAX_DIMEN) {
        wanted = HSTEX_MAX_DIMEN;
    }
    return (int32_t)wanted;
}

static int execute_left_right(struct hstex_engine *engine, int32_t kind,
                              char *error, size_t error_capacity)
{
    struct hstex_math_builder *list = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || list == NULL) {
        return set_error(error, error_capacity,
                         "\\left and \\right are only allowed in a formula");
    }
    int32_t code = 0;
    if (scan_delimiter(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (kind == 0) {
        uint8_t style = list->current_style;
        if (push_math_list(engine, style, error, error_capacity) != 0) {
            return -1;
        }
        current_math_list(engine)->left_delimiter = code;
        current_math_list(engine)->is_left_group = true;
        return 0;
    }
    if (!list->is_left_group || engine->math_depth <= engine->math_floor + 1U) {
        return set_error(error, error_capacity,
                         kind == 2 ? "\\middle has no matching \\left"
                                   : "\\right has no matching \\left");
    }
    if (kind == 2) {
        /* The delimiter cannot be made yet: it is as tall as the whole group
           will turn out to be. See docs/DECISIONS.md, middle-delimiters. */
        struct hstex_noad noad = {
            .kind = (uint8_t)HSTEX_NOAD_MIDDLE,
            .atom_class = (uint8_t)HSTEX_ATOM_CLOSE,
            .delimiter = code,
        };
        return math_append(engine, &noad, error, error_capacity);
    }
    int32_t left_code = list->left_delimiter;
    uint8_t style = list->style;
    uint8_t size = math_size_of_style(style);

    /* A group with a \middle in it has to be set twice: once to find out how
       tall it is, and again with delimiters of that height in place. */
    bool has_middle = false;
    for (size_t index = 0U; index < list->count; ++index) {
        if (list->noads[index].kind == (uint8_t)HSTEX_NOAD_MIDDLE) {
            has_middle = true;
            break;
        }
    }
    int32_t previous_size = engine->middle_delimiter_size;
    struct hstex_box measured = {0};
    if (has_middle) {
        struct hstex_math_builder trial = {0};
        trial.forced_class = -1;
        trial.style = list->style;
        trial.current_style = list->current_style;
        if (list->count != 0U) {
            if (reserve_noads(&trial, list->count, error, error_capacity) !=
                0) {
                return -1;
            }
            memcpy(trial.noads, list->noads,
                   list->count * sizeof(*trial.noads));
            trial.count = list->count;
        }
        struct hstex_hbox_builder measuring = {0};
        struct hstex_hbox_builder *outer = engine->active_hbox_builder;
        engine->active_hbox_builder = &measuring;
        engine->middle_delimiter_size = -1;
        int trial_status =
            translate_math_list(engine, &trial, error, error_capacity);
        engine->active_hbox_builder = outer;
        if (trial_status == 0) {
            trial_status = finalize_hbox(engine, &measuring, false, false, 0,
                                         &measured, error, error_capacity);
        }
        free(measuring.node_identifiers);
        free(trial.noads);
        if (trial_status != 0) {
            engine->middle_delimiter_size = previous_size;
            return -1;
        }
    }

    struct hstex_hbox_builder builder = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &builder;
    engine->middle_delimiter_size = -1;
    int32_t target = -1;
    int status = 0;
    if (has_middle) {
        int32_t trial_axis = 0;
        if (math_symbol_parameter(engine, size, 22U, &trial_axis, error,
                                  error_capacity) != 0) {
            status = -1;
        } else {
            target = delimiter_target(engine, measured.height, measured.depth,
                                      trial_axis);
            engine->middle_delimiter_size = target;
        }
    }
    if (status == 0) {
        status = translate_math_list(engine, list, error, error_capacity);
    }
    engine->active_hbox_builder = previous;
    engine->middle_delimiter_size = previous_size;
    struct hstex_box inner = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &builder, false, false, 0, &inner, error,
                               error_capacity);
    }
    free(builder.node_identifiers);
    pop_math_list(engine);
    if (status != 0) {
        return -1;
    }

    int32_t axis = 0;
    if (math_symbol_parameter(engine, size, 22U, &axis, error,
                              error_capacity) != 0) {
        return -1;
    }
    /* The height the delimiters must cover is the group's own, taken before
       any \\middle delimiter was put into it. */
    int32_t wanted =
        target >= 0 ? target
                    : delimiter_target(engine, inner.height, inner.depth, axis);

    struct hstex_box left_box = {0};
    struct hstex_box right_box = {0};
    if (variant_delimiter(engine, left_code, size, wanted, &left_box, error,
                          error_capacity) != 0 ||
        variant_delimiter(engine, code, size, wanted, &right_box, error,
                          error_capacity) != 0) {
        return -1;
    }

    struct hstex_hbox_builder whole = {0};
    enum hstex_mode previous_mode = engine->mode;
    engine->active_hbox_builder = &whole;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    status = 0;
    struct hstex_box *parts[3] = {&left_box, &inner, &right_box};
    for (size_t index = 0U; status == 0 && index < 3U; ++index) {
        /* A delimiter is centred on the axis; the contents are not moved. */
        int32_t shift = index == 1U ? 0
                                    : half_of((int64_t)parts[index]->height -
                                              parts[index]->depth) -
                                          axis;
        uint32_t identifier = 0U;
        if (store_box_node(engine, parts[index], shift, &identifier, error,
                           error_capacity) != 0) {
            status = -1;
            break;
        }
        status = append_hbox_item(engine, identifier, error, error_capacity);
    }
    struct hstex_box packed = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &whole, false, false, 0, &packed, error,
                               error_capacity);
    }
    free(whole.node_identifiers);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    uint32_t identifier = 0U;
    if (store_box_node(engine, &packed, 0, &identifier, error,
                       error_capacity) != 0) {
        return -1;
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_ATOM,
        .atom_class = (uint8_t)HSTEX_ATOM_INNER,
        .nucleus = {.kind = (uint8_t)HSTEX_MATH_FIELD_BOX, .node = identifier},
    };
    return math_append_atom(engine, &noad, error, error_capacity);
}

/* \vcenter builds a vertical box and hangs it on the axis of the symbol
   family; see docs/DECISIONS.md, vcenter. */
static int execute_vcenter(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    struct hstex_math_builder *list = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || list == NULL) {
        return set_error(error, error_capacity,
                         "\\vcenter is only allowed in a formula");
    }
    uint8_t size = math_size_of_style(list->current_style);
    struct hstex_box box = {0};
    if (scan_vbox(engine, false, &box, error, error_capacity) != 0) {
        return -1;
    }
    int32_t axis = 0;
    if (math_symbol_parameter(engine, size, 22U, &axis, error,
                              error_capacity) != 0) {
        return -1;
    }
    int64_t total = (int64_t)box.height + box.depth;
    box.height = axis + half_of(total);
    box.depth = (int32_t)(total - box.height);
    return math_append_box(engine, &box, error, error_capacity);
}

/* \parshape reads a count and that many indent and length pairs. A count of
   zero or less takes the shape away and reads nothing more. */
static int execute_parshape(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    bool global = engine->pending_global;
    int32_t count = 0;
    if (scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &count, error, error_capacity) != 0) {
        return -1;
    }
    if (count <= 0) {
        if (assign_parshape(engine, 0U, global, error, error_capacity) != 0) {
            return -1;
        }
        return finish_assignment(engine, 0, error, error_capacity);
    }
    if ((size_t)count > (SIZE_MAX - 1U) / 2U) {
        return set_error(error, error_capacity, "\\parshape is too long");
    }
    size_t needed = 1U + 2U * (size_t)count;
    if (engine->parshape_used + needed > engine->parshape_capacity) {
        size_t capacity = engine->parshape_capacity == 0U
                              ? 64U
                              : engine->parshape_capacity;
        while (capacity < engine->parshape_used + needed) {
            if (capacity > SIZE_MAX / 2U) {
                return set_error(error, error_capacity,
                                 "\\parshape arena overflow");
            }
            capacity *= 2U;
        }
        if (capacity > SIZE_MAX / sizeof(*engine->parshapes)) {
            return set_error(error, error_capacity, "\\parshape arena overflow");
        }
        void *allocation =
            realloc(engine->parshapes, capacity * sizeof(*engine->parshapes));
        if (allocation == NULL) {
            return set_error(error, error_capacity,
                             "\\parshape allocation failed");
        }
        engine->parshapes = allocation;
        engine->parshape_capacity = capacity;
    }
    if (engine->parshape_used + needed > (size_t)UINT32_MAX) {
        return set_error(error, error_capacity, "\\parshape arena overflow");
    }
    size_t start = engine->parshape_used;
    engine->parshapes[start] = count;
    for (int32_t index = 0; index < count; ++index) {
        int32_t indent = 0;
        int32_t length = 0;
        if (scan_dimension(engine, &indent, error, error_capacity) != 0 ||
            scan_dimension(engine, &length, error, error_capacity) != 0) {
            return -1;
        }
        engine->parshapes[start + 1U + 2U * (size_t)index] = indent;
        engine->parshapes[start + 2U + 2U * (size_t)index] = length;
    }
    engine->parshape_used = start + needed;
    if (assign_parshape(engine, (uint32_t)(start + 1U), global, error,
                        error_capacity) != 0) {
        return -1;
    }
    return finish_assignment(engine, 0, error, error_capacity);
}

/* \nonscript marks the glue after it as one a script does not want; see
   docs/DECISIONS.md, nonscript. */
static int execute_non_script(struct hstex_engine *engine, char *error,
                              size_t error_capacity)
{
    if (engine->mode != HSTEX_MODE_MATH ||
        current_math_list(engine) == NULL) {
        return set_error(error, error_capacity,
                         "\\nonscript is only allowed in a formula");
    }
    struct hstex_noad noad = {.kind = (uint8_t)HSTEX_NOAD_NONSCRIPT};
    return math_append(engine, &noad, error, error_capacity);
}

/* \radical names a delimiter and then reads one field, which becomes what
   the sign is put over. See docs/DECISIONS.md, radicals. */
static int execute_radical(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || builder == NULL) {
        return set_error(error, error_capacity,
                         "\\radical is only allowed in a formula");
    }
    if (builder->slot != (uint8_t)HSTEX_MATH_SLOT_NONE) {
        return set_error(error, error_capacity,
                         "\\radical met where a field was expected");
    }
    int32_t code = 0;
    if (scan_integer(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (code < 0 || code > 0x7FFFFFF) {
        return set_error(error, error_capacity,
                         "radical %d is outside 0..134217727", code);
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_RADICAL,
        .atom_class = (uint8_t)HSTEX_ATOM_ORD,
        .delimiter = code,
    };
    if (math_append(engine, &noad, error, error_capacity) != 0) {
        return -1;
    }
    builder->slot = (uint8_t)HSTEX_MATH_SLOT_RADICAND;
    builder->slot_target = builder->count - 1U;
    return 0;
}

/* \overline and \underline read one field, exactly as \radical does. */
static int execute_over_under_line(struct hstex_engine *engine, bool over,
                                   char *error, size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || builder == NULL) {
        return set_error(error, error_capacity,
                         "%s is only allowed in a formula",
                         over ? "\\overline" : "\\underline");
    }
    if (builder->slot != (uint8_t)HSTEX_MATH_SLOT_NONE) {
        return set_error(error, error_capacity,
                         "%s met where a field was expected",
                         over ? "\\overline" : "\\underline");
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)(over ? HSTEX_NOAD_OVERLINE : HSTEX_NOAD_UNDERLINE),
        .atom_class = (uint8_t)HSTEX_ATOM_ORD,
    };
    if (math_append(engine, &noad, error, error_capacity) != 0) {
        return -1;
    }
    builder->slot = (uint8_t)HSTEX_MATH_SLOT_RADICAND;
    builder->slot_target = builder->count - 1U;
    return 0;
}

/* \over and its relatives split the list being read: what has been read so
   far is the numerator and what follows is the denominator. See
   docs/DECISIONS.md, fractions. */
static int execute_fraction(struct hstex_engine *engine, int32_t subtype,
                            char *error, size_t error_capacity)
{
    struct hstex_math_builder *list = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || list == NULL) {
        return set_error(error, error_capacity,
                         "a fraction is only allowed in a formula");
    }
    if (list->has_fraction) {
        return set_error(error, error_capacity,
                         "a list may hold only one fraction; brace one of them");
    }
    bool delimited = subtype >= 3;
    int32_t kind = subtype % 3;
    int32_t left = 0;
    int32_t right = 0;
    if (delimited &&
        (scan_delimiter(engine, &left, error, error_capacity) != 0 ||
         scan_delimiter(engine, &right, error, error_capacity) != 0)) {
        return -1;
    }
    int32_t thickness = 0;
    if (kind == 2 &&
        scan_dimension(engine, &thickness, error, error_capacity) != 0) {
        return -1;
    }
    list->has_fraction = true;
    list->fraction_at = list->count;
    list->fraction_thickness = thickness;
    list->fraction_default_thickness = kind == 0;
    list->fraction_left = left;
    list->fraction_right = right;
    return 0;
}

/* \eqno and \leqno end the equation and begin its number, which is set in
   text style; see docs/DECISIONS.md, equation-numbers. */
static int execute_equation_number(struct hstex_engine *engine, bool left,
                                   char *error, size_t error_capacity)
{
    if (engine->mode != HSTEX_MODE_MATH || !engine->displayed_math ||
        engine->reading_equation_number) {
        return set_error(error, error_capacity,
                         "an equation number is only allowed in a display");
    }
    struct hstex_box equation = {0};
    if (package_displayed_formula(engine, &equation, error, error_capacity) !=
        0) {
        return -1;
    }
    engine->displayed_equation = equation;
    engine->reading_equation_number = true;
    engine->equation_number_on_left = left;
    return push_math_list(engine, (uint8_t)HSTEX_STYLE_TEXT, error,
                          error_capacity);
}

/* Build the line the display occupies, with its number beside it when there
   is room. */
/* Whether the number can stand beside the equation: the reference wants a
   quad of the symbol family between them as well. */
static int equation_number_fits(struct hstex_engine *engine,
                                const struct hstex_box *equation,
                                const struct hstex_box *number, int32_t width,
                                bool *fits, char *error, size_t error_capacity)
{
    if (number == NULL) {
        *fits = false;
        return 0;
    }
    int32_t quad = 0;
    if (math_symbol_parameter(engine, (uint8_t)HSTEX_MATH_TEXT, 6U, &quad,
                              error, error_capacity) != 0) {
        return -1;
    }
    *fits = (int64_t)equation->width + number->width + quad <= width;
    return 0;
}

static int append_display_line(struct hstex_engine *engine,
                               struct hstex_box equation,
                               const struct hstex_box *number, bool left,
                               int32_t width, int32_t indent, int32_t *shift,
                               bool dropped, char *error,
                               size_t error_capacity)
{
    (void)error;
    (void)error_capacity;
    int32_t e = number == NULL || dropped ? 0 : number->width;
    if (e == 0 && equation.width > width) {
        /* A box HSTeX has packed records no shrink, so an equation too wide
           for the display is squeezed to fit rather than centred. */
        equation.width = width;
    }
    int32_t w = equation.width;
    int32_t d = (int32_t)(((int64_t)width - w + 1) / 2);
    if (e > 0 && d < 2 * e) {
        d = (int32_t)(((int64_t)width - w - e + 1) / 2);
    }
    *shift = indent + (left && e != 0 ? 0 : d);
    if (e == 0) {
        struct hstex_node node = {
            .kind = HSTEX_NODE_LIST,
            .width = equation.width,
            .height = equation.height,
            .depth = equation.depth,
            .shift = *shift,
            .value.list = {
                .node_start = equation.node_start,
                .node_count = equation.node_count,
                .box_kind = equation.kind,
            },
        };
        return append_vbox_node(engine, &node, error, error_capacity);
    }
    struct hstex_hbox_builder builder = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    engine->active_hbox_builder = &builder;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    struct hstex_node gap = {
        .kind = HSTEX_NODE_KERN,
        .width = (int32_t)((int64_t)width - w - e - d),
    };
    uint32_t identifier = 0U;
    int status = 0;
    const struct hstex_box *first = left ? number : &equation;
    const struct hstex_box *second = left ? &equation : number;
    if (store_box_node(engine, first, 0, &identifier, error, error_capacity) !=
            0 ||
        append_hbox_item(engine, identifier, error, error_capacity) != 0 ||
        append_hbox_node(engine, &gap, error, error_capacity) != 0 ||
        store_box_node(engine, second, 0, &identifier, error,
                       error_capacity) != 0 ||
        append_hbox_item(engine, identifier, error, error_capacity) != 0) {
        status = -1;
    }
    struct hstex_box line = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &builder, false, false, 0, &line, error,
                               error_capacity);
    }
    free(builder.node_identifiers);
    engine->active_hbox_builder = previous;
    engine->mode = previous_mode;
    if (status != 0) {
        return -1;
    }
    struct hstex_node node = {
        .kind = HSTEX_NODE_LIST,
        .width = line.width,
        .height = line.height,
        .depth = line.depth,
        .shift = *shift,
        .value.list = {
            .node_start = line.node_start,
            .node_count = line.node_count,
            .box_kind = line.kind,
        },
    };
    return append_vbox_node(engine, &node, error, error_capacity);
}

static int resume_paragraph_after_display(struct hstex_engine *engine,
                                          char *error, size_t error_capacity);

/* The closing $$ centres the formula in \displaywidth, surrounds it with the
   display penalties and skips, and lets the paragraph carry on. */
static int end_display_math(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    bool numbered = engine->reading_equation_number;
    bool left = engine->equation_number_on_left;
    struct hstex_box packaged = {0};
    if (package_displayed_formula(engine, &packaged, error, error_capacity) !=
        0) {
        return -1;
    }
    struct hstex_box equation = numbered ? engine->displayed_equation : packaged;
    struct hstex_box number = packaged;
    engine->reading_equation_number = false;
    if (end_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    engine->displayed_math = false;
    engine->mode = HSTEX_MODE_VERTICAL;
    engine->inner_mode = false;

    int32_t width = engine->dimen_parameters[HSTEX_DIMEN_DISPLAY_WIDTH];
    int32_t indent = engine->dimen_parameters[HSTEX_DIMEN_DISPLAY_INDENT];
    int32_t before = engine->dimen_parameters[HSTEX_DIMEN_PRE_DISPLAY_SIZE];
    int32_t shift = 0;
    bool fits = false;
    if (equation_number_fits(engine, &equation, numbered ? &number : NULL,
                             width, &fits, error, error_capacity) != 0) {
        return -1;
    }
    bool dropped = numbered && !fits;

    /* The line is measured first, because whether the number fits beside it
       decides both the skips and where the number goes. */
    int32_t trial = (int32_t)(((int64_t)width - equation.width + 1) / 2);
    bool roomy = numbered && left
                     ? true
                     : (int64_t)trial + indent <= (int64_t)before;
    struct hstex_glue above =
        engine->glue_parameters[roomy ? HSTEX_GLUE_ABOVE_DISPLAY_SKIP
                                      : HSTEX_GLUE_ABOVE_DISPLAY_SHORT_SKIP];
    struct hstex_glue below =
        engine->glue_parameters[roomy ? HSTEX_GLUE_BELOW_DISPLAY_SKIP
                                      : HSTEX_GLUE_BELOW_DISPLAY_SHORT_SKIP];
    struct hstex_node penalty = {
        .kind = HSTEX_NODE_PENALTY,
        .value.penalty =
            engine->integer_parameters[HSTEX_INTEGER_PRE_DISPLAY_PENALTY],
    };
    struct hstex_node infinite = {
        .kind = HSTEX_NODE_PENALTY,
        .value.penalty = HSTEX_INFINITE_PENALTY,
    };

    /* A number on the left that will not fit beside the equation takes the
       place of the glue above it. */
    bool left_alone = dropped && left;
    if (left_alone) {
        struct hstex_node node = {
            .kind = HSTEX_NODE_LIST,
            .width = number.width,
            .height = number.height,
            .depth = number.depth,
            .shift = indent,
            .value.list = {
                .node_start = number.node_start,
                .node_count = number.node_count,
                .box_kind = number.kind,
            },
        };
        if (append_vbox_node(engine, &node, error, error_capacity) != 0 ||
            append_vbox_node(engine, &infinite, error, error_capacity) != 0) {
            return -1;
        }
    } else if (append_vbox_node(engine, &penalty, error, error_capacity) != 0 ||
               append_display_glue(engine, above, error, error_capacity) != 0) {
        return -1;
    }

    if (append_display_line(engine, equation, numbered ? &number : NULL, left,
                            width, indent, &shift, dropped, error,
                            error_capacity) != 0) {
        return -1;
    }
    if (dropped && !left) {
        /* A number on the right that would not fit goes below, against the
           right edge, and the glue under the display goes away. */
        struct hstex_node node = {
            .kind = HSTEX_NODE_LIST,
            .width = number.width,
            .height = number.height,
            .depth = number.depth,
            .shift = indent + width - number.width,
            .value.list = {
                .node_start = number.node_start,
                .node_count = number.node_count,
                .box_kind = number.kind,
            },
        };
        if (append_vbox_node(engine, &infinite, error, error_capacity) != 0 ||
            append_vbox_node(engine, &node, error, error_capacity) != 0) {
            return -1;
        }
        below.width = 0;
        below.stretch = 0;
        below.shrink = 0;
        below.stretch_order = 0U;
        below.shrink_order = 0U;
    }
    penalty.value.penalty =
        engine->integer_parameters[HSTEX_INTEGER_POST_DISPLAY_PENALTY];
    if (append_vbox_node(engine, &penalty, error, error_capacity) != 0) {
        return -1;
    }
    if (!(dropped && !left) &&
        append_display_glue(engine, below, error, error_capacity) != 0) {
        return -1;
    }
    return resume_paragraph_after_display(engine, error, error_capacity);
}

/* The paragraph carries on, with no indentation and no \everypar, and a
   space right after the display is ignored. */
static int resume_paragraph_after_display(struct hstex_engine *engine,
                                          char *error, size_t error_capacity)
{
    if (engine->paragraph_builder == NULL) {
        engine->paragraph_builder =
            calloc(1U, sizeof(*engine->paragraph_builder));
        if (engine->paragraph_builder == NULL) {
            return set_error(error, error_capacity,
                             "paragraph list allocation failed");
        }
    }
    engine->paragraph_builder->count = 0U;
    engine->paragraph_builder->width = 0;
    engine->paragraph_builder->height = 0;
    engine->paragraph_builder->depth = 0;
    engine->active_hbox_builder = engine->paragraph_builder;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    engine->inner_mode = false;
    engine->building_paragraph = true;
    engine->space_factor = 1000;
    engine->has_pending_character = false;
    return skip_optional_space(engine, error, error_capacity);
}

/* The $$ that closes an alignment used as a display reads the penalties and
   skips around it -- assignments between the alignment and the $$ still
   count -- and puts the rows between them. See docs/DECISIONS.md,
   display-alignments. */
static int end_display_alignment(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    struct hstex_vbox_builder *rows = engine->display_rows;
    engine->display_rows = NULL;
    engine->display_alignment = false;
    engine->mode = HSTEX_MODE_VERTICAL;
    engine->inner_mode = false;
    struct hstex_node penalty = {
        .kind = HSTEX_NODE_PENALTY,
        .value.penalty =
            engine->integer_parameters[HSTEX_INTEGER_PRE_DISPLAY_PENALTY],
    };
    int status = append_vbox_node(engine, &penalty, error, error_capacity);
    if (status == 0) {
        status = append_display_glue(
            engine, engine->glue_parameters[HSTEX_GLUE_ABOVE_DISPLAY_SKIP],
            error, error_capacity);
    }
    for (size_t index = 0U; status == 0 && rows != NULL && index < rows->count;
         ++index) {
        status = append_vbox_item(engine, rows->node_identifiers[index], error,
                                  error_capacity);
    }
    if (rows != NULL) {
        free(rows->node_identifiers);
        free(rows);
    }
    if (status != 0) {
        return -1;
    }
    penalty.value.penalty =
        engine->integer_parameters[HSTEX_INTEGER_POST_DISPLAY_PENALTY];
    if (append_vbox_node(engine, &penalty, error, error_capacity) != 0 ||
        append_display_glue(
            engine, engine->glue_parameters[HSTEX_GLUE_BELOW_DISPLAY_SKIP],
            error, error_capacity) != 0) {
        return -1;
    }
    if (end_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    return resume_paragraph_after_display(engine, error, error_capacity);
}

/* Entering a formula opens a group, sets \fam to none, and inserts
   \everymath. The reference sets \fam before \everymath runs, so a token
   list may select a family for the whole formula. */
static int begin_math(struct hstex_engine *engine, char *error,
                      size_t error_capacity)
{
    if (flush_pending_character(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (begin_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (assign_integer_parameter(engine, (uint32_t)HSTEX_INTEGER_FAMILY, -1,
                                 false, error, error_capacity) != 0) {
        return -1;
    }
    if (push_math_list(engine, (uint8_t)HSTEX_STYLE_TEXT, error,
                       error_capacity) != 0) {
        return -1;
    }
    /* A formula between single shifts is an inner mode, and is never a
       display even when one encloses it. */
    current_math_list(engine)->outer_inner_mode = engine->inner_mode;
    current_math_list(engine)->outer_displayed = engine->displayed_math;
    engine->inner_mode = true;
    engine->displayed_math = false;
    engine->mode = HSTEX_MODE_MATH;
    uint32_t every_math = engine->token_parameters[HSTEX_TOKEN_EVERY_MATH];
    if (every_math == 0U) {
        return 0;
    }
    const struct hstex_token_list *list =
        token_list_by_identifier(engine, every_math);
    struct hstex_source_location location = {0};
    if (list == NULL ||
        hstex_source_push_tokens(&engine->sources, list->tokens, list->count,
                                 location, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "could not install everymath tokens");
    }
    return 0;
}

/* A brace inside a formula collects a sub-formula, which becomes one
   ordinary atom -- or the script the mark before it is waiting for, in which
   case it is set in the style that script will be typeset at. */
static int begin_math_group(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    struct hstex_math_builder *outer = current_math_list(engine);
    uint8_t style =
        outer == NULL ? (uint8_t)HSTEX_STYLE_TEXT : outer->current_style;
    if (outer != NULL && outer->choice_remaining == 0U) {
        if (outer->slot == (uint8_t)HSTEX_MATH_SLOT_SUPERSCRIPT) {
            style = math_superscript_style(outer->current_style);
        } else if (outer->slot == (uint8_t)HSTEX_MATH_SLOT_SUBSCRIPT) {
            style = math_subscript_style(outer->current_style);
        } else if (outer->slot == (uint8_t)HSTEX_MATH_SLOT_RADICAND) {
            style = math_cramped_style(outer->current_style);
        }
    }
    return push_math_list(engine, style, error, error_capacity);
}

static int finish_math_group(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    struct hstex_math_builder *inner = current_math_list(engine);
    if (inner == NULL || engine->math_depth < engine->math_floor + 2U) {
        return set_error(error, error_capacity,
                         "a math group closed outside a formula");
    }
    struct hstex_math_builder *outer = &engine->math_stack[engine->math_depth - 2U];
    if (outer->choice_remaining != 0U) {
        /* One branch of a \mathchoice. All four are kept, because which one
           is wanted depends on the style the list is finally set in, and a
           fraction settles that only when \over is read. */
        uint32_t record = 0U;
        int status =
            store_math_sublist(engine, inner, &record, error, error_capacity);
        if (status == 0 && outer->choice_noad < outer->count) {
            outer->noads[outer->choice_noad].choices[outer->choice_index] =
                record;
        }
        ++outer->choice_index;
        --outer->choice_remaining;
        pop_math_list(engine);
        return status;
    }
    /* One ordinary atom in braces is that atom: {x} carries scripts exactly
       as x does, however deeply the braces nest. */
    if (inner->count == 1U &&
        inner->noads[0].kind == (uint8_t)HSTEX_NOAD_ATOM &&
        inner->noads[0].atom_class == (uint8_t)HSTEX_ATOM_ORD &&
        inner->noads[0].superscript.kind ==
            (uint8_t)HSTEX_MATH_FIELD_EMPTY &&
        inner->noads[0].subscript.kind == (uint8_t)HSTEX_MATH_FIELD_EMPTY &&
        inner->slot == (uint8_t)HSTEX_MATH_SLOT_NONE) {
        struct hstex_noad passed = inner->noads[0];
        pop_math_list(engine);
        return math_append_atom(engine, &passed, error, error_capacity);
    }
    struct hstex_hbox_builder builder = {0};
    struct hstex_hbox_builder *previous = engine->active_hbox_builder;
    engine->active_hbox_builder = &builder;
    if (inner->slot != (uint8_t)HSTEX_MATH_SLOT_NONE) {
        engine->active_hbox_builder = previous;
        free(builder.node_identifiers);
        pop_math_list(engine);
        return set_error(error, error_capacity,
                         "a script mark was left without a script");
    }
    /* The list is kept as well as the box, because a fraction sets each of
       its sides in a style that is not known until \over is read. */
    uint32_t record = 0U;
    uint8_t item_style = inner->style;
    int status = store_math_sublist(engine, inner, &record, error,
                                    error_capacity);
    if (status == 0) {
        status = translate_math_list(engine, inner, error, error_capacity);
    }
    engine->active_hbox_builder = previous;
    struct hstex_box box = {0};
    if (status == 0) {
        status = finalize_hbox(engine, &builder, false, false, 0, &box, error,
                               error_capacity);
    }
    free(builder.node_identifiers);
    pop_math_list(engine);
    if (status != 0) {
        return -1;
    }
    /* A sub-formula that came to one character carries scripts the way that
       character would; see docs/DECISIONS.md, math-scripts. A shifted node is
       not one, because a centred large operator is a box. */
    bool single = false;
    if (box.node_count == 1U &&
        (size_t)box.node_start < engine->list_item_count) {
        uint32_t identifier = engine->list_items[box.node_start];
        if (identifier != 0U && identifier <= engine->node_count) {
            const struct hstex_node *node = &engine->nodes[identifier - 1U];
            single = (node->kind == HSTEX_NODE_CHARACTER ||
                      node->kind == HSTEX_NODE_LIGATURE) &&
                     node->shift == 0;
        }
    }
    return math_append_box_field(engine, &box, single, record, item_style,
                                 error, error_capacity);
}

/* Leaving a formula puts \mathsurround on both sides of the translation and
   splices it into the horizontal list that was interrupted. */
static int end_math(struct hstex_engine *engine, char *error,
                    size_t error_capacity)
{
    struct hstex_math_builder *list = current_math_list(engine);
    if (list == NULL || engine->math_depth != engine->math_floor + 1U) {
        return set_error(error, error_capacity,
                         "a formula closed inside a math group");
    }
    engine->mode = HSTEX_MODE_HORIZONTAL;
    engine->inner_mode = list->outer_inner_mode;
    engine->displayed_math = list->outer_displayed;
    int32_t surround = engine->dimen_parameters[HSTEX_DIMEN_MATH_SURROUND];
    int status = 0;
    if (surround != 0) {
        status = emit_math_kern(engine, surround, error, error_capacity);
    }
    if (status == 0) {
        status = translate_math_list(engine, list, error, error_capacity);
    }
    if (status == 0 && surround != 0) {
        status = emit_math_kern(engine, surround, error, error_capacity);
    }
    pop_math_list(engine);
    if (status != 0) {
        return -1;
    }
    return end_group(engine, error, error_capacity);
}

/* \textfont, \scriptfont and \scriptscriptfont: a family number, then a font.
   The assignment is local, like a register's. */
static int execute_math_font(struct hstex_engine *engine, int32_t size,
                             char *error, size_t error_capacity)
{
    int32_t family = 0;
    if (scan_integer(engine, &family, error, error_capacity) != 0) {
        return -1;
    }
    if (family < 0 || family > 15) {
        return set_error(error, error_capacity,
                         "math family %d is outside 0..15", family);
    }
    if (scan_optional_equals(engine, error, error_capacity) != 0) {
        return -1;
    }
    uint32_t identifier = 0U;
    if (scan_font_identifier(engine, &identifier, error, error_capacity) != 0) {
        return -1;
    }
    if (size < 0 || size >= (int32_t)HSTEX_MATH_SIZE_COUNT) {
        return set_error(error, error_capacity, "invalid math font size");
    }
    uint32_t index = (uint32_t)size * 16U + (uint32_t)family;
    bool global = assignment_is_global(engine, false);
    uint32_t *level = &engine->math_font_levels[size][family];
    if (!global && engine->group_level != 0U) {
        if (save_value(engine, HSTEX_SAVE_MATH_FONT, index, *level,
                       (int32_t)engine->math_fonts[size][family], 0U, error,
                       error_capacity) != 0) {
            return -1;
        }
        *level = engine->group_level;
    } else {
        *level = 0U;
    }
    engine->math_fonts[size][family] = identifier;
    engine->pending_global = false;
    return 0;
}

static int execute_math_char(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    int32_t code = 0;
    if (scan_integer(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (code < 0 || code > 0x7FFF) {
        return set_error(error, error_capacity,
                         "mathchar %d is outside 0..32767", code);
    }
    if (engine->mode != HSTEX_MODE_MATH) {
        return set_error(error, error_capacity,
                         "a math character is only allowed in a formula");
    }
    return math_append_code(engine, code, error, error_capacity);
}

/* A style command changes the style of everything after it in its list. */
static int execute_math_style(struct hstex_engine *engine, int32_t style,
                              char *error, size_t error_capacity)
{
    if (engine->mode != HSTEX_MODE_MATH) {
        return set_error(error, error_capacity,
                         "a math style is only allowed in a formula");
    }
    struct hstex_noad noad = {
        .kind = (uint8_t)HSTEX_NOAD_STYLE,
        .atom_class = (uint8_t)style,
    };
    current_math_list(engine)->current_style = (uint8_t)style;
    return math_append(engine, &noad, error, error_capacity);
}

/* \mathchoice reads four sub-formulas -- so all four are executed -- and
   keeps the one the current style calls for, spliced into the list so that
   its atoms space against their neighbours; see docs/DECISIONS.md,
   math-choices. */
static int execute_math_choice(struct hstex_engine *engine, char *error,
                               size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || builder == NULL) {
        return set_error(error, error_capacity,
                         "\\mathchoice is only allowed in a formula");
    }
    if (builder->choice_remaining != 0U) {
        return set_error(error, error_capacity,
                         "one \\mathchoice followed another");
    }
    struct hstex_noad noad = {.kind = (uint8_t)HSTEX_NOAD_CHOICE};
    if (math_append(engine, &noad, error, error_capacity) != 0) {
        return -1;
    }
    builder->choice_remaining = 4U;
    builder->choice_index = 0U;
    builder->choice_noad = builder->count - 1U;
    return 0;
}

/* \delimiter names a small and a large variant. Used on its own it is the
   small one, which is the top fifteen bits of the number; see
   docs/DECISIONS.md, delimiters. */
static int execute_delimiter(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    int32_t code = 0;
    if (scan_integer(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (code < 0 || code > 0x7FFFFFF) {
        return set_error(error, error_capacity,
                         "delimiter %d is outside 0..134217727", code);
    }
    if (engine->mode != HSTEX_MODE_MATH) {
        return set_error(error, error_capacity,
                         "a delimiter is only allowed in a formula");
    }
    return math_append_code(engine, code / 4096, error, error_capacity);
}

static int execute_math_class(struct hstex_engine *engine, int32_t class_code,
                              char *error, size_t error_capacity)
{
    struct hstex_math_builder *builder = current_math_list(engine);
    if (engine->mode != HSTEX_MODE_MATH || builder == NULL) {
        return set_error(error, error_capacity,
                         "a math class is only allowed in a formula");
    }
    builder->forced_class = class_code;
    return 0;
}

static int execute_math_skip(struct hstex_engine *engine, bool kern,
                             char *error, size_t error_capacity)
{
    if (engine->mode != HSTEX_MODE_MATH) {
        return set_error(error, error_capacity,
                         "math glue is only allowed in a formula");
    }
    struct hstex_glue glue = {0};
    if (scan_math_glue(engine, &glue, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_noad noad = {0};
    if (kern) {
        noad.kind = (uint8_t)HSTEX_NOAD_MU_KERN;
        noad.kern = glue.width;
    } else {
        noad.kind = (uint8_t)HSTEX_NOAD_MU_GLUE;
        noad.glue = glue;
    }
    return math_append(engine, &noad, error, error_capacity);
}

/* ---------------------------------------------------------------------- */
/* Alignments.                                                            */
/*                                                                        */
/* \halign reads a preamble of column templates, then rows of entries.    */
/* Each entry is packaged at its natural width; once every row has been   */
/* read the columns are as wide as their widest entry, and the entries    */
/* are set again to that width. See docs/DECISIONS.md, alignments.        */
/* ---------------------------------------------------------------------- */

static void destroy_align_columns(struct hstex_align_column *columns,
                                  size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        free(columns[index].before);
        free(columns[index].after);
    }
    free(columns);
}

static void destroy_align_rows(struct hstex_align_row *rows, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        free(rows[index].cells);
        free(rows[index].items);
    }
    free(rows);
}

/* The preamble is read without expanding anything, the way the reference
   does, except that a \tabskip assignment is carried out where it stands and
   sets the glue for the boundary it appears at. */
static int scan_align_preamble(struct hstex_engine *engine,
                               struct hstex_align_column **out_columns,
                               size_t *out_count, size_t *out_loop,
                               char *error, size_t error_capacity)
{
    *out_loop = SIZE_MAX;
    struct hstex_align_column *columns = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    struct token_vector before = {0};
    struct token_vector after = {0};
    bool seen_marker = false;
    int status = 0;

    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result =
            raw_next(engine, &token, &location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            status = set_error(error, error_capacity,
                               "input ended inside an alignment preamble");
            break;
        }
        if (hstex_token_is_control_sequence(token)) {
            const struct hstex_meaning *meaning = hstex_engine_meaning(
                engine, hstex_token_control_sequence_id(token));
            if (meaning->command == HSTEX_COMMAND_GLUE_PARAMETER &&
                meaning->value.integer == (int32_t)HSTEX_GLUE_TAB_SKIP) {
                struct hstex_glue glue = {0};
                if (scan_optional_equals(engine, error, error_capacity) != 0 ||
                    scan_glue(engine, &glue, error, error_capacity) != 0 ||
                    assign_glue_parameter(engine,
                                          (uint32_t)HSTEX_GLUE_TAB_SKIP, glue,
                                          false, error, error_capacity) != 0) {
                    status = -1;
                    break;
                }
                continue;
            }
            if (meaning->command == HSTEX_COMMAND_SPAN) {
                /* \span in a preamble expands the token after it once, which
                   is how amsmath hands \halign a preamble it built as a
                   macro; see docs/DECISIONS.md, preamble-expansion. */
                hstex_token next = 0U;
                struct hstex_source_location where;
                if (raw_next(engine, &next, &where, error, error_capacity) !=
                    HSTEX_ENGINE_TOKEN) {
                    status = set_error(error, error_capacity,
                                       "input ended after \\span in a "
                                       "preamble");
                    break;
                }
                if (expand_token_once(engine, next, where, error,
                                      error_capacity) != 0) {
                    status = -1;
                    break;
                }
                continue;
            }
            if (meaning->command == HSTEX_COMMAND_CR) {
                if (!seen_marker) {
                    status = set_error(error, error_capacity,
                                       "an alignment column has no #, after "
                                       "an empty column");
                    break;
                }
                status = 0;
                goto finish_column;
            }
        }
        if (token_is_effective_category(engine, token,
                                        (uint8_t)HSTEX_CAT_PARAMETER)) {
            if (seen_marker) {
                status = set_error(error, error_capacity,
                                   "only one # is allowed per alignment "
                                   "column");
                break;
            }
            seen_marker = true;
            continue;
        }
        if (token_is_effective_category(engine, token,
                                        (uint8_t)HSTEX_CAT_ALIGNMENT_TAB)) {
            /* A tab where a column would start marks the point the preamble
               repeats from -- that is what && means, and a preamble that
               begins with one repeats from its first column. */
            if (!seen_marker && before.count == 0U &&
                *out_loop == SIZE_MAX) {
                *out_loop = count;
                continue;
            }
            if (!seen_marker) {
                char collected[192] = {0};
                size_t used = 0U;
                for (size_t index = 0U;
                     index < before.count && used + 2U < sizeof(collected);
                     ++index) {
                    char piece[64];
                    describe_token(engine, before.data[index], piece,
                                   sizeof(piece));
                    int written = snprintf(collected + used,
                                           sizeof(collected) - used, "%s%s",
                                           index == 0U ? "" : " ", piece);
                    if (written <= 0) {
                        break;
                    }
                    used += (size_t)written;
                }
                status = set_error(error, error_capacity,
                                   "an alignment column has no #, after: %s",
                                   collected);
                break;
            }
            goto finish_column;
        }
        if (vector_push(seen_marker ? &after : &before, token, error,
                        error_capacity) != 0) {
            status = -1;
            break;
        }
        continue;

    finish_column:
        if (count == capacity) {
            size_t next = capacity == 0U ? 4U : capacity * 2U;
            if (next > SIZE_MAX / sizeof(*columns)) {
                status = set_error(error, error_capacity,
                                   "alignment column overflow");
                break;
            }
            void *allocation = realloc(columns, next * sizeof(*columns));
            if (allocation == NULL) {
                status = set_error(error, error_capacity,
                                   "alignment column allocation failed");
                break;
            }
            columns = allocation;
            capacity = next;
        }
        struct hstex_align_column *column = &columns[count++];
        memset(column, 0, sizeof(*column));
        column->before = before.data;
        column->before_count = before.count;
        column->after = after.data;
        column->after_count = after.count;
        column->tabskip = engine->glue_parameters[HSTEX_GLUE_TAB_SKIP];
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        seen_marker = false;
        if (hstex_token_is_control_sequence(token)) {
            /* \cr ended the preamble. */
            *out_columns = columns;
            *out_count = count;
            return 0;
        }
    }
    vector_destroy(&before);
    vector_destroy(&after);
    destroy_align_columns(columns, count);
    return status == 0 ? -1 : status;
}

/* Repeat the preamble from its && point until the wanted column exists. */
static int extend_align_columns(struct hstex_align_column **columns,
                                size_t *count, size_t *capacity,
                                size_t loop_start, size_t wanted, char *error,
                                size_t error_capacity)
{
    if (wanted < *count) {
        return 0;
    }
    if (loop_start == SIZE_MAX || loop_start >= *count) {
        return set_error(error, error_capacity,
                         "an alignment row has more entries than the "
                         "preamble has columns");
    }
    size_t period = *count - loop_start;
    while (wanted >= *count) {
        if (*count == *capacity) {
            size_t next = *capacity == 0U ? 4U : *capacity * 2U;
            if (next > SIZE_MAX / sizeof(**columns)) {
                return set_error(error, error_capacity,
                                 "alignment column overflow");
            }
            void *allocation = realloc(*columns, next * sizeof(**columns));
            if (allocation == NULL) {
                return set_error(error, error_capacity,
                                 "alignment column allocation failed");
            }
            *columns = allocation;
            *capacity = next;
        }
        const struct hstex_align_column *source =
            &(*columns)[loop_start + (*count - loop_start) % period];
        struct hstex_align_column copy = *source;
        copy.before = NULL;
        copy.after = NULL;
        copy.width = 0;
        if (source->before_count != 0U) {
            copy.before = malloc(source->before_count * sizeof(*copy.before));
            if (copy.before == NULL) {
                return set_error(error, error_capacity,
                                 "alignment column allocation failed");
            }
            memcpy(copy.before, source->before,
                   source->before_count * sizeof(*copy.before));
        }
        if (source->after_count != 0U) {
            copy.after = malloc(source->after_count * sizeof(*copy.after));
            if (copy.after == NULL) {
                free(copy.before);
                return set_error(error, error_capacity,
                                 "alignment column allocation failed");
            }
            memcpy(copy.after, source->after,
                   source->after_count * sizeof(*copy.after));
        }
        (*columns)[(*count)++] = copy;
    }
    return 0;
}

enum hstex_align_end {
    HSTEX_ALIGN_END_TAB = 0,
    HSTEX_ALIGN_END_SPAN,
    HSTEX_ALIGN_END_CR,
};

/* End the entry being read, from wherever the tab or \cr turned up. The v
   part goes in behind a boundary and is read where it stands, so a template
   that opened a box closes it before the entry is finished; see
   docs/DECISIONS.md, alignment-entries. */
static int end_alignment_entry(struct hstex_engine *engine,
                               enum hstex_align_end ending, char *error,
                               size_t error_capacity)
{
    struct hstex_align_entry *entry = engine->alignment_entry;
    if (entry == NULL) {
        return set_error(error, error_capacity,
                         "an alignment entry ended outside one");
    }
    if (entry->after_pushed) {
        return set_error(error, error_capacity,
                         "an alignment template ended the entry twice");
    }
    entry->ending = (uint8_t)ending;
    entry->after_pushed = true;
    struct hstex_source_location origin = {0};
    if (hstex_source_push_boundary(&engine->sources, error, error_capacity) !=
        0) {
        return -1;
    }
    if (!entry->omit && entry->column < entry->column_count &&
        entry->columns[entry->column].after_count != 0U) {
        return hstex_source_push_tokens(
            &engine->sources, entry->columns[entry->column].after,
            entry->columns[entry->column].after_count, origin, error,
            error_capacity);
    }
    return 0;
}

/* Run one entry. The templates around it are pushed as token lists, so the
   entry sees exactly what the reference's u and v parts give it. \span
   carries on into the next column with the same box. */
static int evaluate_align_cell(struct hstex_engine *engine,
                               const struct hstex_align_column *columns,
                               size_t column_count, size_t first_column,
                               bool omit, struct hstex_hbox_builder *builder,
                               enum hstex_align_end *ending, uint32_t *span,
                               char *error, size_t error_capacity)
{
    uint32_t base_group_level = engine->group_level;
    uint32_t previous_group_floor = engine->output_group_floor;
    size_t previous_conditional_floor = engine->output_conditional_floor;
    struct hstex_hbox_builder *previous_builder = engine->active_hbox_builder;
    enum hstex_mode previous_mode = engine->mode;
    bool previous_inner_mode = engine->inner_mode;
    uint32_t previous_stop_level = engine->group_stop_level;
    bool previous_stop_armed = engine->group_stop_armed;
    bool previous_stop_hit = engine->group_stop_hit;
    size_t previous_math_depth = engine->math_depth;
    size_t previous_math_floor = engine->math_floor;
    bool previous_displayed = engine->displayed_math;
    /* A body is not part of whatever paragraph encloses it; it gets a list
       of its own if it starts one. */
    bool previous_building_paragraph = engine->building_paragraph;
    struct hstex_hbox_builder *previous_paragraph = engine->paragraph_builder;
    int32_t previous_space_factor = engine->space_factor;
    bool previous_has_pending = engine->has_pending_character;
    bool previous_building = engine->building_alignment;

    if (begin_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    engine->output_group_floor = engine->group_level;
    engine->output_conditional_floor = engine->conditional_count;
    engine->group_stop_armed = false;
    engine->group_stop_hit = false;
    engine->math_floor = engine->math_depth;
    engine->displayed_math = false;
    engine->building_paragraph = false;
    engine->paragraph_builder = NULL;
    engine->active_hbox_builder = builder;
    engine->mode = HSTEX_MODE_HORIZONTAL;
    engine->inner_mode = true;
    engine->space_factor = 1000;
    engine->has_pending_character = false;
    engine->building_alignment = true;

    size_t column = first_column;
    *span = 1U;
    *ending = HSTEX_ALIGN_END_CR;
    int status = 0;
    bool finished = false;
    bool segment_omit = omit;
    struct hstex_source_location origin = {0};
    struct hstex_align_entry entry = {
        .columns = columns,
        .column_count = column_count,
        .column = first_column,
        .omit = omit,
    };
    struct hstex_align_entry *previous_entry = engine->alignment_entry;
    engine->alignment_entry = &entry;
    if (!omit && column < column_count && columns[column].before_count != 0U &&
        hstex_source_push_tokens(&engine->sources, columns[column].before,
                                 columns[column].before_count, origin, error,
                                 error_capacity) != 0) {
        status = -1;
    }
    while (status == 0 && !finished) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_ERROR) {
            status = -1;
            break;
        }
        if (result == HSTEX_ENGINE_EOF) {
            if (!entry.after_pushed) {
                status = set_error(error, error_capacity,
                                   "input ended inside an alignment entry");
                break;
            }
            /* The v part is exhausted: the entry, or this span of it, ends. */
            if (hstex_source_pop_boundary(&engine->sources, error,
                                          error_capacity) != 0) {
                status = -1;
                break;
            }
            entry.after_pushed = false;
            *ending = (enum hstex_align_end)entry.ending;
            if (*ending != HSTEX_ALIGN_END_SPAN) {
                finished = true;
                break;
            }
            ++column;
            ++*span;
            entry.column = column;
            entry.ending = (uint8_t)HSTEX_ALIGN_END_CR;
            /* \omit may follow \span, and drops the next column's
               templates just as it does at the start of an entry. */
            segment_omit = false;
            entry.omit = false;
            hstex_token next = 0U;
            struct hstex_source_location where;
            enum hstex_engine_result got =
                expanded_next_non_space_unrestricted(engine, &next, &where,
                                                     error, error_capacity);
            if (got == HSTEX_ENGINE_ERROR) {
                status = -1;
                break;
            }
            if (got == HSTEX_ENGINE_TOKEN) {
                if (hstex_token_is_control_sequence(next) &&
                    hstex_engine_meaning(
                        engine, hstex_token_control_sequence_id(next))
                            ->command == HSTEX_COMMAND_OMIT) {
                    segment_omit = true;
                    entry.omit = true;
                } else if (push_one(engine, next, where, error,
                                    error_capacity) != 0) {
                    status = -1;
                    break;
                }
            }
            if (!segment_omit && column < column_count &&
                columns[column].before_count != 0U &&
                hstex_source_push_tokens(&engine->sources,
                                         columns[column].before,
                                         columns[column].before_count, origin,
                                         error, error_capacity) != 0) {
                status = -1;
            }
            continue;
        }
        if (token_is_space(token)) {
            status = flush_pending_character(engine, error, error_capacity);
            if (status == 0) {
                status = append_interword_glue(engine, error, error_capacity);
            }
            engine->space_factor = 1000;
            continue;
        }
        if (!hstex_token_is_character(token)) {
            char found_name[128];
            describe_token(engine, token, found_name, sizeof(found_name));
            status = set_error(error, error_capacity,
                               "%s is not supported inside an alignment entry",
                               found_name);
            break;
        }
        status = append_horizontal_character(
            engine, hstex_token_character_code(token), error, error_capacity);
    }
    if (status == 0) {
        status = flush_pending_character(engine, error, error_capacity);
    }
    engine->space_factor = previous_space_factor;
    engine->has_pending_character = previous_has_pending;
    engine->active_hbox_builder = previous_builder;
    engine->mode = previous_mode;
    engine->inner_mode = previous_inner_mode;
    engine->output_group_floor = previous_group_floor;
    engine->output_conditional_floor = previous_conditional_floor;
    engine->group_stop_level = previous_stop_level;
    engine->group_stop_armed = previous_stop_armed;
    engine->group_stop_hit = previous_stop_hit;
    engine->building_alignment = previous_building;
    engine->alignment_entry = previous_entry;
    while (engine->math_depth > previous_math_depth) {
        pop_math_list(engine);
    }
    engine->math_depth = previous_math_depth;
    engine->math_floor = previous_math_floor;
    engine->displayed_math = previous_displayed;
    if (engine->paragraph_builder != NULL &&
        engine->paragraph_builder != previous_paragraph) {
        free(engine->paragraph_builder->node_identifiers);
        free(engine->paragraph_builder);
    }
    engine->paragraph_builder = previous_paragraph;
    engine->building_paragraph = previous_building_paragraph;
    while (engine->group_level > base_group_level) {
        if (end_group(engine, error, error_capacity) != 0) {
            status = -1;
            break;
        }
    }
    return status;
}

/* The columns are as wide as their widest single entry; an entry that spans
   several columns widens the last one it covers when it does not fit. Then
   every row becomes an hbox of tabskip, entry, tabskip, ... set to the width
   the whole alignment came to. */
static int finish_alignment(struct hstex_engine *engine,
                            struct hstex_align_column *columns,
                            size_t column_count, struct hstex_glue leading,
                            const struct hstex_align_row *rows,
                            size_t row_count, bool matched_to,
                            bool matched_spread, int32_t requested_width,
                            int32_t shift, char *error, size_t error_capacity)
{
    for (size_t index = 0U; index < column_count; ++index) {
        columns[index].width = 0;
    }
    /* Short spans first, so that a wide one only has to make up what the
       columns it covers still lack. */
    for (size_t pass = 1U; pass <= column_count; ++pass) {
        for (size_t index = 0U; index < row_count; ++index) {
            if (rows[index].noalign) {
                continue;
            }
            size_t column = 0U;
            for (size_t cell = 0U; cell < rows[index].cell_count; ++cell) {
                const struct hstex_align_cell *entry = &rows[index].cells[cell];
                if (entry->span == pass &&
                    column + entry->span <= column_count) {
                    int64_t covered = 0;
                    for (size_t step = 0U; step < entry->span; ++step) {
                        covered += columns[column + step].width;
                        if (step + 1U < entry->span) {
                            covered += columns[column + step].tabskip.width;
                        }
                    }
                    if ((int64_t)entry->width > covered) {
                        columns[column + entry->span - 1U].width +=
                            (int32_t)((int64_t)entry->width - covered);
                    }
                }
                column += entry->span;
            }
        }
    }

    int64_t natural = leading.width;
    for (size_t index = 0U; index < column_count; ++index) {
        natural += columns[index].width;
        natural += columns[index].tabskip.width;
    }
    int64_t final_width = natural;
    if (matched_to) {
        final_width = requested_width;
    } else if (matched_spread) {
        final_width = natural + requested_width;
    }
    if (final_width < -INT64_C(1073741823) ||
        final_width > INT64_C(1073741823)) {
        return set_error(error, error_capacity,
                         "alignment width exceeds TeX's dimension range");
    }

    for (size_t index = 0U; index < row_count; ++index) {
        if (rows[index].noalign) {
            for (size_t item = 0U; item < rows[index].item_count; ++item) {
                if (append_vbox_item(engine, rows[index].items[item], error,
                                     error_capacity) != 0) {
                    return -1;
                }
            }
            continue;
        }
        struct hstex_hbox_builder line = {0};
        struct hstex_hbox_builder *previous = engine->active_hbox_builder;
        enum hstex_mode previous_mode = engine->mode;
        engine->active_hbox_builder = &line;
        engine->mode = HSTEX_MODE_HORIZONTAL;
        int status = 0;
        struct hstex_glue skip = leading;
        size_t column = 0U;
        for (size_t cell = 0U; status == 0 && cell < rows[index].cell_count;
             ++cell) {
            status = emit_math_glue(engine, skip, error, error_capacity);
            if (status != 0) {
                break;
            }
            const struct hstex_align_cell *entry = &rows[index].cells[cell];
            if (entry->box == 0U || entry->box > engine->node_count) {
                status = set_error(error, error_capacity,
                                   "an alignment entry lost its box");
                break;
            }
            struct hstex_node set = engine->nodes[entry->box - 1U];
            int64_t width = 0;
            for (size_t step = 0U;
                 step < entry->span && column + step < column_count; ++step) {
                width += columns[column + step].width;
                if (step + 1U < entry->span) {
                    width += columns[column + step].tabskip.width;
                }
            }
            set.width = (int32_t)width;
            status = append_hbox_node(engine, &set, error, error_capacity);
            size_t last = column + entry->span - 1U;
            skip = last < column_count ? columns[last].tabskip : leading;
            column += entry->span;
        }
        /* A row that stops early still carries the glue of the columns it
           did not reach, so that every row is the same shape. */
        for (; status == 0 && column <= column_count; ++column) {
            status = emit_math_glue(engine, skip, error, error_capacity);
            if (column < column_count) {
                skip = columns[column].tabskip;
            }
        }
        struct hstex_box packed = {0};
        if (status == 0) {
            status = finalize_hbox(engine, &line, true, false,
                                   (int32_t)final_width, &packed, error,
                                   error_capacity);
        }
        free(line.node_identifiers);
        engine->active_hbox_builder = previous;
        engine->mode = previous_mode;
        if (status != 0) {
            return -1;
        }
        if (append_shifted_box_node(engine, &packed, shift, error,
                                    error_capacity) != 0) {
            return -1;
        }
    }
    return 0;
}

/* \everycr is read once the preamble has been scanned and again after every
   \cr that ends a row; a redundant \crcr does not fire it. See
   docs/DECISIONS.md, everycr. */
static int insert_every_cr(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    uint32_t every = engine->token_parameters[HSTEX_TOKEN_EVERY_CR];
    if (every == 0U) {
        return 0;
    }
    const struct hstex_token_list *list = token_list_by_identifier(engine, every);
    struct hstex_source_location location = {0};
    if (list == NULL ||
        hstex_source_push_tokens(&engine->sources, list->tokens, list->count,
                                 location, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "could not install everycr tokens");
    }
    return 0;
}

static int reserve_align_rows(struct hstex_align_row **rows, size_t *capacity,
                              size_t required, char *error,
                              size_t error_capacity)
{
    if (required <= *capacity) {
        return 0;
    }
    size_t next = *capacity == 0U ? 8U : *capacity;
    while (next < required) {
        if (next > SIZE_MAX / 2U) {
            return set_error(error, error_capacity, "alignment row overflow");
        }
        next *= 2U;
    }
    if (next > SIZE_MAX / sizeof(**rows)) {
        return set_error(error, error_capacity, "alignment row overflow");
    }
    void *allocation = realloc(*rows, next * sizeof(**rows));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "alignment row allocation failed");
    }
    *rows = allocation;
    *capacity = next;
    return 0;
}

/* \halign: read the preamble, read every row at its natural width, then set
   the columns to the width of their widest entry and put the rows in the
   enclosing vertical list.

   An alignment may also be the whole of a display, which is how amsmath sets
   align and gather. The rows are then gathered aside so that the display's
   penalties and skips can be read at the closing $$; see
   docs/DECISIONS.md, display-alignments. */
static int execute_halign(struct hstex_engine *engine, char *error,
                          size_t error_capacity)
{
    if (engine->pending_global || engine->pending_macro_flags != 0U) {
        return set_error(error, error_capacity,
                         "an alignment does not accept prefixes");
    }
    bool display = false;
    if (engine->mode == HSTEX_MODE_MATH) {
        const struct hstex_math_builder *list = current_math_list(engine);
        if (!engine->displayed_math || engine->reading_equation_number ||
            list == NULL || engine->math_depth != engine->math_floor + 1U) {
            return set_error(error, error_capacity,
                             "\\halign requires vertical mode");
        }
        if (list->count != 0U) {
            return set_error(error, error_capacity,
                             "an alignment must be the whole of a display");
        }
        display = true;
    } else if (engine->mode != HSTEX_MODE_VERTICAL) {
        return set_error(error, error_capacity,
                         "\\halign requires vertical mode");
    }
    bool matched_to = false;
    bool matched_spread = false;
    int32_t requested_width = 0;
    if (try_keyword(engine, "to", &matched_to, error, error_capacity) != 0) {
        return -1;
    }
    if (!matched_to &&
        try_keyword(engine, "spread", &matched_spread, error,
                    error_capacity) != 0) {
        return -1;
    }
    if ((matched_to || matched_spread) &&
        scan_dimension(engine, &requested_width, error, error_capacity) != 0) {
        return -1;
    }
    hstex_token opening = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result = expanded_next_non_space_unrestricted(
        engine, &opening, &location, error, error_capacity);
    if (result != HSTEX_ENGINE_TOKEN ||
        !token_is_effective_begin_group(engine, opening)) {
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        char found[128];
        describe_token(engine, result == HSTEX_ENGINE_TOKEN ? opening : 0U,
                       found, sizeof(found));
        return set_error(error, error_capacity,
                         "an alignment requires a braced body, found %s",
                         found);
    }

    uint32_t base_group_level = engine->group_level;
    /* The glue before the first column is \tabskip as it stood when the
       alignment began; the preamble's own assignments set the later ones. */
    struct hstex_glue leading = engine->glue_parameters[HSTEX_GLUE_TAB_SKIP];
    int32_t shift = 0;
    if (display) {
        /* The formula the display was going to hold is empty and goes away;
           what is left is a vertical list, gathered aside until the closing
           $$ says what surrounds it. \prevdepth is carried over, so the
           first row is spaced from the line above it as any box would be. */
        pop_math_list(engine);
        engine->mode = HSTEX_MODE_VERTICAL;
        engine->inner_mode = false;
        engine->displayed_math = false;
        shift = engine->dimen_parameters[HSTEX_DIMEN_DISPLAY_INDENT];
        engine->display_outer_vbox = engine->active_vbox_builder;
        engine->display_rows = calloc(1U, sizeof(*engine->display_rows));
        if (engine->display_rows == NULL) {
            return set_error(error, error_capacity,
                             "display alignment allocation failed");
        }
        engine->active_vbox_builder = engine->display_rows;
    }
    if (begin_group(engine, error, error_capacity) != 0) {
        return -1;
    }
    struct hstex_align_column *columns = NULL;
    size_t column_count = 0U;
    size_t column_capacity = 0U;
    size_t loop_start = SIZE_MAX;
    struct hstex_align_row *rows = NULL;
    size_t row_count = 0U;
    size_t row_capacity = 0U;
    int status = scan_align_preamble(engine, &columns, &column_count,
                                     &loop_start, error, error_capacity);
    column_capacity = column_count;
    if (status == 0) {
        status = insert_every_cr(engine, error, error_capacity);
    }
    bool previous_building = engine->building_alignment;
    engine->building_alignment = true;

    while (status == 0) {
        hstex_token token = 0U;
        struct hstex_source_location peek;
        enum hstex_engine_result peeked = expanded_next_non_space_unrestricted(
            engine, &token, &peek, error, error_capacity);
        if (peeked == HSTEX_ENGINE_ERROR) {
            status = -1;
            break;
        }
        if (peeked == HSTEX_ENGINE_EOF) {
            status = set_error(error, error_capacity,
                               "input ended inside an alignment");
            break;
        }
        if (token_is_effective_end_group(engine, token)) {
            break;
        }
        if (hstex_token_is_control_sequence(token)) {
            const struct hstex_meaning *meaning = hstex_engine_meaning(
                engine, hstex_token_control_sequence_id(token));
            if (meaning->command == HSTEX_COMMAND_CR &&
                meaning->value.integer != 0) {
                continue; /* \crcr between rows does nothing */
            }
            if (meaning->command == HSTEX_COMMAND_NO_ALIGN) {
                hstex_token brace = 0U;
                struct hstex_source_location where;
                if (expanded_next_non_space_unrestricted(
                        engine, &brace, &where, error, error_capacity) !=
                        HSTEX_ENGINE_TOKEN ||
                    !token_is_effective_begin_group(engine, brace)) {
                    status = set_error(error, error_capacity,
                                       "\\noalign requires a braced list");
                    break;
                }
                struct hstex_vbox_builder between = {0};
                status = evaluate_vbox_contents(engine, &between, error,
                                                error_capacity);
                if (status == 0) {
                    status = reserve_align_rows(&rows, &row_capacity,
                                                row_count + 1U, error,
                                                error_capacity);
                }
                if (status != 0) {
                    free(between.node_identifiers);
                    break;
                }
                struct hstex_align_row *entry = &rows[row_count++];
                memset(entry, 0, sizeof(*entry));
                entry->noalign = true;
                entry->items = between.node_identifiers;
                entry->item_count = between.count;
                continue;
            }
        }
        if (push_one(engine, token, peek, error, error_capacity) != 0) {
            status = -1;
            break;
        }
        /* One row. */
        if (reserve_align_rows(&rows, &row_capacity, row_count + 1U, error,
                               error_capacity) != 0) {
            status = -1;
            break;
        }
        struct hstex_align_row *row = &rows[row_count++];
        memset(row, 0, sizeof(*row));
        size_t cell_capacity = 0U;
        size_t column = 0U;
        for (;;) {
            hstex_token first = 0U;
            struct hstex_source_location start;
            enum hstex_engine_result got = expanded_next_non_space_unrestricted(
                engine, &first, &start, error, error_capacity);
            if (got != HSTEX_ENGINE_TOKEN) {
                status = set_error(error, error_capacity,
                                   "input ended inside an alignment row");
                break;
            }
            bool omit = false;
            if (hstex_token_is_control_sequence(first) &&
                hstex_engine_meaning(engine,
                                     hstex_token_control_sequence_id(first))
                        ->command == HSTEX_COMMAND_OMIT) {
                omit = true;
            } else if (push_one(engine, first, start, error, error_capacity) !=
                       0) {
                status = -1;
                break;
            }
            if (extend_align_columns(&columns, &column_count,
                                     &column_capacity, loop_start, column,
                                     error, error_capacity) != 0) {
                status = -1;
                break;
            }
            struct hstex_hbox_builder cell = {0};
            enum hstex_align_end ending = HSTEX_ALIGN_END_CR;
            uint32_t span = 1U;
            status = evaluate_align_cell(engine, columns, column_count, column,
                                         omit, &cell, &ending, &span, error,
                                         error_capacity);
            struct hstex_box box = {0};
            if (status == 0) {
                status = finalize_hbox(engine, &cell, false, false, 0, &box,
                                       error, error_capacity);
            }
            free(cell.node_identifiers);
            if (status != 0) {
                break;
            }
            uint32_t identifier = 0U;
            if (store_box_node(engine, &box, 0, &identifier, error,
                               error_capacity) != 0) {
                status = -1;
                break;
            }
            if (row->cell_count == cell_capacity) {
                size_t next = cell_capacity == 0U ? 4U : cell_capacity * 2U;
                void *allocation =
                    realloc(row->cells, next * sizeof(*row->cells));
                if (allocation == NULL) {
                    status = set_error(error, error_capacity,
                                       "alignment entry allocation failed");
                    break;
                }
                row->cells = allocation;
                cell_capacity = next;
            }
            struct hstex_align_cell *entry = &row->cells[row->cell_count++];
            entry->box = identifier;
            entry->width = box.width;
            entry->span = span;
            column += span;
            if (ending == HSTEX_ALIGN_END_CR) {
                break;
            }
        }
        if (status != 0) {
            break;
        }
        status = insert_every_cr(engine, error, error_capacity);
    }
    engine->building_alignment = previous_building;
    if (status == 0) {
        status = finish_alignment(engine, columns, column_count, leading,
                                  rows, row_count, matched_to, matched_spread,
                                  requested_width, shift, error,
                                  error_capacity);
    }
    destroy_align_columns(columns, column_count);
    destroy_align_rows(rows, row_count);
    while (engine->group_level > base_group_level) {
        if (end_group(engine, error, error_capacity) != 0) {
            status = -1;
            break;
        }
    }
    if (display) {
        engine->active_vbox_builder = engine->display_outer_vbox;
        if (status == 0) {
            engine->display_alignment = true;
        } else {
            free(engine->display_rows->node_identifiers);
            free(engine->display_rows);
            engine->display_rows = NULL;
        }
    }
    return status;
}

/* \accent puts an accent over the character that follows it. The two kerns
   around the accent cancel, so the pair is exactly as wide as the accented
   character; what shows is the accent being raised to sit above it. See
   docs/DECISIONS.md, accents. */
static int execute_accent(struct hstex_engine *engine, char *error,
                          size_t error_capacity)
{
    int32_t code = 0;
    if (scan_integer(engine, &code, error, error_capacity) != 0) {
        return -1;
    }
    if (code < 0 || code > 255) {
        return set_error(error, error_capacity, "bad accent code (%d)", code);
    }
    if (ensure_horizontal_mode(engine, error, error_capacity) != 0 ||
        flush_pending_character(engine, error, error_capacity) != 0) {
        return -1;
    }
    const struct hstex_font *font =
        font_by_identifier(engine, engine->current_font);
    if (font == NULL || font->characters == NULL ||
        font->characters[code].tag < 0) {
        return set_error(error, error_capacity,
                         "the current font has no character %d for an accent",
                         code);
    }
    struct hstex_char_metric accent = font->characters[code];
    int32_t x_height = font->dimen_count >= 5U ? font->dimens[4] : 0;
    int32_t slant = font->dimen_count >= 1U ? font->dimens[0] : 0;

    /* Whatever follows, after any assignments, is the accented character. */
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result = expanded_next_non_space(
        engine, &token, &location, error, error_capacity);
    if (result == HSTEX_ENGINE_ERROR) {
        return -1;
    }
    int32_t accented = -1;
    if (result == HSTEX_ENGINE_TOKEN) {
        if (hstex_token_is_character(token) &&
            (token_is_category(token, HSTEX_CAT_LETTER) ||
             token_is_category(token, HSTEX_CAT_OTHER))) {
            accented = (int32_t)hstex_token_character_code(token);
        } else if (hstex_token_is_control_sequence(token)) {
            const struct hstex_meaning *meaning = hstex_engine_meaning(
                engine, hstex_token_control_sequence_id(token));
            if (meaning->command == HSTEX_COMMAND_CHAR_GIVEN) {
                accented = meaning->value.integer;
            } else if (meaning->command == HSTEX_COMMAND_CHAR) {
                if (scan_integer(engine, &accented, error, error_capacity) !=
                    0) {
                    return -1;
                }
            } else if (push_one(engine, token, location, error,
                                error_capacity) != 0) {
                return -1;
            }
        } else if (push_one(engine, token, location, error, error_capacity) !=
                   0) {
            return -1;
        }
    }
    if (accented >= 0 &&
        (accented > 255 || font->characters[accented].tag < 0)) {
        return set_error(error, error_capacity,
                         "the current font has no character %d to accent",
                         accented);
    }

    struct hstex_node accent_node = {
        .kind = HSTEX_NODE_CHARACTER,
        .width = accent.width,
        .height = accent.height,
        .depth = accent.depth,
        .value.character = {
            .font = engine->current_font,
            .character = (uint32_t)code,
        },
    };
    if (accented < 0) {
        /* Nothing to accent: the accent is an ordinary character. */
        engine->space_factor = 1000;
        return append_hbox_node(engine, &accent_node, error, error_capacity);
    }

    struct hstex_char_metric under = font->characters[accented];
    int32_t lift = x_height - under.height;
    int64_t delta = ((int64_t)under.width - accent.width) / 2 +
                    ((int64_t)under.height - x_height) *
                        (int64_t)slant / INT64_C(65536);
    struct hstex_node before = {
        .kind = HSTEX_NODE_KERN,
        .width = (int32_t)delta,
    };
    struct hstex_node after = {
        .kind = HSTEX_NODE_KERN,
        .width = (int32_t)(-(int64_t)accent.width - delta),
    };
    if (append_hbox_node(engine, &before, error, error_capacity) != 0) {
        return -1;
    }
    if (lift == 0) {
        if (append_hbox_node(engine, &accent_node, error, error_capacity) != 0) {
            return -1;
        }
    } else {
        /* The accent rides in a box of its own so that it can be raised. */
        struct hstex_hbox_builder builder = {0};
        struct hstex_hbox_builder *previous = engine->active_hbox_builder;
        engine->active_hbox_builder = &builder;
        int status = append_hbox_node(engine, &accent_node, error,
                                      error_capacity);
        engine->active_hbox_builder = previous;
        struct hstex_box box = {0};
        if (status == 0) {
            status = finalize_hbox(engine, &builder, false, false, 0, &box,
                                   error, error_capacity);
        }
        free(builder.node_identifiers);
        if (status != 0) {
            return -1;
        }
        struct hstex_node raised = {
            .kind = HSTEX_NODE_LIST,
            .width = box.width,
            .height = box.height,
            .depth = box.depth,
            .shift = lift,
            .value.list = {
                .node_start = box.node_start,
                .node_count = box.node_count,
                .box_kind = box.kind,
            },
        };
        if (append_hbox_node(engine, &raised, error, error_capacity) != 0) {
            return -1;
        }
    }
    struct hstex_node body = {
        .kind = HSTEX_NODE_CHARACTER,
        .width = under.width,
        .height = under.height,
        .depth = under.depth,
        .value.character = {
            .font = engine->current_font,
            .character = (uint32_t)accented,
        },
    };
    if (append_hbox_node(engine, &after, error, error_capacity) != 0 ||
        append_hbox_node(engine, &body, error, error_capacity) != 0) {
        return -1;
    }
    engine->space_factor = 1000;
    return 0;
}

static int execute_write(struct hstex_engine *engine, char *error,
                         size_t error_capacity)
{
    int32_t stream = 0;
    uint8_t *bytes = NULL;
    size_t byte_count = 0U;
    /* \write accepts any stream number; only 0..15 can name an open file. */
    if (scan_integer(engine, &stream, error, error_capacity) != 0 ||
        scan_expanded_general_text(engine, &bytes, &byte_count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    /* A write to a stream that is not open goes to the log, and also to the
       terminal unless the stream number is negative. \typeout relies on this:
       it writes to an allocated but never opened stream. HSTeX has one
       diagnostic surface so far, so both destinations are stdout. */
    FILE *destination = stdout;
    if (stream >= 0 && stream < 16 &&
        engine->write_streams[(size_t)stream] != NULL) {
        destination = engine->write_streams[(size_t)stream];
    }
    int status = 0;
    if (destination != NULL &&
        ((byte_count != 0U &&
          fwrite(bytes, 1U, byte_count, destination) != byte_count) ||
         fputc('\n', destination) == EOF || fflush(destination) != 0)) {
        status = set_error(error, error_capacity, "write stream failed");
    }
    free(bytes);
    return status;
}

static int execute_message(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    uint8_t *bytes = NULL;
    size_t byte_count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &byte_count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    int status = 0;
    if ((byte_count != 0U &&
         fwrite(bytes, 1U, byte_count, stdout) != byte_count) ||
        fflush(stdout) != 0) {
        status = set_error(error, error_capacity, "message output failed");
    }
    free(bytes);
    return status;
}

static int execute_error_message(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    uint8_t *bytes = NULL;
    size_t byte_count = 0U;
    if (scan_expanded_general_text(engine, &bytes, &byte_count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    int precision = byte_count > (size_t)INT_MAX ? INT_MAX : (int)byte_count;
    uint32_t line = 0U;
    const char *origin = current_source_line(engine, &line);
    /* LaTeX's \GenericError prints its diagnosis itself and then raises an
       \errmessage with nothing in it, so an empty message is not a bug. */
    int status =
        byte_count == 0U
            ? set_error(error, error_capacity,
                        "the document raised an error at %s:%u; its own "
                        "diagnosis is above",
                        origin, (unsigned int)line)
            : set_error(error, error_capacity, "%.*s, at %s:%u", precision,
                        bytes == NULL ? "" : (const char *)bytes, origin,
                        (unsigned int)line);
    free(bytes);
    return status;
}

static int execute_open_in(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    int32_t stream = 0;
    char *filename = NULL;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0 ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_input_filename(engine, &filename, error, error_capacity) != 0) {
        free(filename);
        return -1;
    }
    if (stream < 0 || stream >= 16) {
        free(filename);
        return 0;
    }
    if (engine->read_streams[(size_t)stream] != NULL) {
        (void)fclose(engine->read_streams[(size_t)stream]);
        engine->read_streams[(size_t)stream] = NULL;
    }
    char *path = resolve_input_path(engine, filename);
    free(filename);
    if (path != NULL) {
        engine->read_streams[(size_t)stream] = fopen(path, "rb");
    }
    free(path);
    return 0;
}

static int execute_close_in(struct hstex_engine *engine, char *error,
                            size_t error_capacity)
{
    int32_t stream = 0;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0) {
        return -1;
    }
    if (stream >= 0 && stream < 16 &&
        engine->read_streams[(size_t)stream] != NULL) {
        if (fclose(engine->read_streams[(size_t)stream]) != 0) {
            engine->read_streams[(size_t)stream] = NULL;
            return set_error(error, error_capacity,
                             "failed to close input stream");
        }
        engine->read_streams[(size_t)stream] = NULL;
    }
    return 0;
}

static int scan_keyword_to(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    static const uint8_t keyword[] = {'t', 'o'};
    for (size_t index = 0U; index < sizeof(keyword); ++index) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result =
            index == 0U
                ? expanded_next_non_space(engine, &token, &location, error,
                                          error_capacity)
                : hstex_engine_next_expanded(engine, &token, &location, error,
                                             error_capacity);
        if (result != HSTEX_ENGINE_TOKEN ||
            !token_has_character(token, keyword[index])) {
            return set_error(error, error_capacity, "read requires keyword to");
        }
    }
    return 0;
}

static int define_read_tokens(struct hstex_engine *engine, hstex_cs_id target,
                              struct token_vector *replacement, char *error,
                              size_t error_capacity)
{
    if (reserve_macros(engine, engine->macro_count + 1U, error,
                       error_capacity) != 0) {
        vector_destroy(replacement);
        return -1;
    }
    struct hstex_macro *macro = &engine->macros[engine->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->replacement = replacement->data;
    macro->replacement_count = replacement->count;
    memset(replacement, 0, sizeof(*replacement));
    ++engine->macro_count;
    struct hstex_meaning meaning = {
        .command = HSTEX_COMMAND_MACRO,
        .level = 0U,
        .value = {.macro_identifier = (uint32_t)engine->macro_count},
    };
    return set_meaning(engine, target, meaning,
                       assignment_is_global(engine, engine->pending_global),
                       error, error_capacity);
}

static int define_read_line(struct hstex_engine *engine, hstex_cs_id target,
                            const uint8_t *line, size_t length, char *error,
                            size_t error_capacity)
{
    struct hstex_mouth mouth;
    hstex_mouth_init(&mouth, line, length, &engine->lexical_state);
    struct token_vector replacement = {0};
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_mouth_result result = hstex_mouth_next(
            &mouth, &token, &location, error, error_capacity);
        if (result == HSTEX_MOUTH_EOF) {
            break;
        }
        if (result == HSTEX_MOUTH_ERROR ||
            vector_push(&replacement, token, error, error_capacity) != 0) {
            hstex_mouth_destroy(&mouth);
            vector_destroy(&replacement);
            return -1;
        }
    }
    hstex_mouth_destroy(&mouth);
    return define_read_tokens(engine, target, &replacement, error,
                              error_capacity);
}

static int define_other_read_line(struct hstex_engine *engine,
                                  hstex_cs_id target, const uint8_t *line,
                                  size_t length, char *error,
                                  size_t error_capacity)
{
    while (length != 0U && line[length - 1U] == (uint8_t)' ') {
        --length;
    }
    struct token_vector replacement = {0};
    for (size_t index = 0U; index < length; ++index) {
        uint8_t category = line[index] == (uint8_t)' '
                               ? (uint8_t)HSTEX_CAT_SPACE
                               : (uint8_t)HSTEX_CAT_OTHER;
        if (vector_push(&replacement,
                        hstex_token_character(category, line[index]), error,
                        error_capacity) != 0) {
            vector_destroy(&replacement);
            return -1;
        }
    }
    int32_t end_line =
        engine->integer_parameters[HSTEX_INTEGER_END_LINE_CHARACTER];
    if (end_line >= 0 && end_line <= 255) {
        uint8_t character = (uint8_t)end_line;
        uint8_t category = character == (uint8_t)' '
                               ? (uint8_t)HSTEX_CAT_SPACE
                               : (uint8_t)HSTEX_CAT_OTHER;
        if (vector_push(&replacement,
                        hstex_token_character(category, character), error,
                        error_capacity) != 0) {
            vector_destroy(&replacement);
            return -1;
        }
    }
    return define_read_tokens(engine, target, &replacement, error,
                              error_capacity);
}

static int execute_read_kind(struct hstex_engine *engine, bool other_catcodes,
                             char *error, size_t error_capacity)
{
    int32_t stream = 0;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0 ||
        scan_keyword_to(engine, error, error_capacity) != 0) {
        return -1;
    }
    hstex_cs_id target = 0U;
    if (scan_definition_target(engine, &target, error, error_capacity) != 0) {
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0U;
    ssize_t length = -1;
    if (stream >= 0 && stream < 16 &&
        engine->read_streams[(size_t)stream] != NULL) {
        length = getline(&line, &capacity,
                         engine->read_streams[(size_t)stream]);
    }
    if (length < 0) {
        length = 0;
    }
    while (length > 0 &&
           (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        --length;
    }
    int status =
        other_catcodes
            ? define_other_read_line(engine, target, (const uint8_t *)line,
                                     (size_t)length, error, error_capacity)
            : define_read_line(engine, target, (const uint8_t *)line,
                               (size_t)length, error, error_capacity);
    free(line);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return status;
}

static int execute_read(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
{
    return execute_read_kind(engine, false, error, error_capacity);
}

static int execute_read_line(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    return execute_read_kind(engine, true, error, error_capacity);
}

static int scan_if_eof(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    int32_t stream = 0;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    bool at_end = stream < 0 || stream >= 16 ||
                  engine->read_streams[(size_t)stream] == NULL ||
                  feof(engine->read_streams[(size_t)stream]) != 0;
    return finish_conditional(engine, conditional, at_end, error,
                              error_capacity);
}

static int scan_if_defined(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    hstex_token subject = 0U;
    struct hstex_source_location location;
    if (raw_next(engine, &subject, &location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity, "end of input in ifdefined");
    }
    bool defined = hstex_token_is_control_sequence(subject) &&
                   hstex_engine_meaning(
                       engine, hstex_token_control_sequence_id(subject))
                           ->command != HSTEX_COMMAND_UNDEFINED;
    return finish_conditional(engine, conditional, defined, error,
                              error_capacity);
}

static int scan_if_cs_name(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    uint8_t *name = NULL;
    size_t name_count = 0U;
    if (scan_cs_name_bytes(engine, &name, &name_count, error, error_capacity) !=
        0) {
        engine->conditional_count = conditional;
        return -1;
    }
    hstex_cs_id identifier = 0U;
    bool defined =
        hstex_symbol_find(&engine->lexical_state.symbols, HSTEX_SYMBOL_REGULAR,
                          name, name_count, &identifier) == 1 &&
        hstex_engine_meaning(engine, identifier)->command !=
            HSTEX_COMMAND_UNDEFINED;
    free(name);
    return finish_conditional(engine, conditional, defined, error,
                              error_capacity);
}

static bool command_starts_conditional(enum hstex_command command)
{
    return command == HSTEX_COMMAND_IF_NUM || command == HSTEX_COMMAND_IF_DIM ||
           command == HSTEX_COMMAND_IF_H_MODE ||
           command == HSTEX_COMMAND_IF_V_MODE ||
           command == HSTEX_COMMAND_IF_M_MODE ||
           command == HSTEX_COMMAND_IF_INNER ||
           command == HSTEX_COMMAND_IF_X ||
           command == HSTEX_COMMAND_IF_CHAR || command == HSTEX_COMMAND_IF_CAT ||
           command == HSTEX_COMMAND_IF_ODD ||
           command == HSTEX_COMMAND_IF_CASE ||
           command == HSTEX_COMMAND_IF_TRUE ||
           command == HSTEX_COMMAND_IF_FALSE ||
           command == HSTEX_COMMAND_IF_EOF ||
           command == HSTEX_COMMAND_IF_DEFINED ||
           command == HSTEX_COMMAND_IF_FONT_CHAR ||
           command == HSTEX_COMMAND_IF_BOX ||
           command == HSTEX_COMMAND_IF_CS_NAME;
}

static bool meanings_equal(const struct hstex_engine *engine,
                           const struct hstex_meaning *left,
                           const struct hstex_meaning *right)
{
    if (left->command != right->command) {
        return false;
    }
    switch (left->command) {
    case HSTEX_COMMAND_MACRO: {
        uint32_t left_identifier = left->value.macro_identifier;
        uint32_t right_identifier = right->value.macro_identifier;
        if (left_identifier == 0U || right_identifier == 0U ||
            (size_t)left_identifier > engine->macro_count ||
            (size_t)right_identifier > engine->macro_count) {
            return left_identifier == right_identifier;
        }
        const struct hstex_macro *left_macro =
            &engine->macros[left_identifier - 1U];
        const struct hstex_macro *right_macro =
            &engine->macros[right_identifier - 1U];
        return left_macro->flags == right_macro->flags &&
               left_macro->parameter_count == right_macro->parameter_count &&
               left_macro->parameter_count_tokens ==
                   right_macro->parameter_count_tokens &&
               left_macro->replacement_count == right_macro->replacement_count &&
               (left_macro->parameter_count_tokens == 0U ||
                memcmp(left_macro->parameter_text, right_macro->parameter_text,
                       left_macro->parameter_count_tokens *
                           sizeof(*left_macro->parameter_text)) == 0) &&
               (left_macro->replacement_count == 0U ||
                memcmp(left_macro->replacement, right_macro->replacement,
                       left_macro->replacement_count *
                           sizeof(*left_macro->replacement)) == 0);
    }
    case HSTEX_COMMAND_TOKEN_ALIAS:
        return left->value.token == right->value.token;
    case HSTEX_COMMAND_UNDEFINED:
        return true;
    default:
        /* A whole family of primitives shares one command and is told apart
           by its subtype, so the subtype is part of the meaning: this is what
           keeps \hfil unequal to \hfill and \mathbin unequal to \mathpunct.
           A primitive with no subtype carries zero and still compares equal.
           LaTeX's \DeclareMathSymbol asks exactly this question. */
        return left->value.integer == right->value.integer;
    }
}

static bool ifx_tokens_equal(const struct hstex_engine *engine,
                             hstex_token left, hstex_token right)
{
    left = normalize_one_shot_token(left);
    right = normalize_one_shot_token(right);
    /* A control sequence \\let to a character is that character as far as
       \\ifx is concerned; LaTeX's prime machinery turns on it. */
    if (hstex_token_is_control_sequence(left) &&
        hstex_token_is_character(right)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(left));
        return meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
               meaning->value.token == right;
    }
    if (hstex_token_is_control_sequence(right) &&
        hstex_token_is_character(left)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(right));
        return meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
               meaning->value.token == left;
    }
    if (hstex_token_is_character(left) || hstex_token_is_character(right)) {
        return left == right;
    }
    if (!hstex_token_is_control_sequence(left) ||
        !hstex_token_is_control_sequence(right)) {
        return false;
    }
    return meanings_equal(
        engine,
        hstex_engine_meaning(engine, hstex_token_control_sequence_id(left)),
        hstex_engine_meaning(engine, hstex_token_control_sequence_id(right)));
}

static int skip_conditional(struct hstex_engine *engine, size_t target,
                            bool stop_at_else, char *error,
                            size_t error_capacity)
{
    if (target >= engine->conditional_count) {
        return set_error(error, error_capacity,
                         "conditional skip target is not active");
    }
    size_t active_nested = engine->conditional_count - target - 1U;
    size_t skipped_depth = 0U;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        if (raw_next(engine, &token, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input while skipping a conditional");
        }
        token = normalize_unexpanded_control_sequence(token);
        if (!hstex_token_is_control_sequence(token)) {
            continue;
        }
        enum hstex_command command =
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token))
                ->command;
        if (command_starts_conditional(command)) {
            ++skipped_depth;
        } else if (command == HSTEX_COMMAND_FI) {
            if (skipped_depth != 0U) {
                --skipped_depth;
                continue;
            }
            if (active_nested != 0U) {
                if (engine->conditional_count <= target + 1U) {
                    return set_error(error, error_capacity,
                                     "conditional stack underflow");
                }
                --engine->conditional_count;
                --active_nested;
                continue;
            }
            if (engine->conditional_count != target + 1U) {
                return set_error(error, error_capacity,
                                 "conditional stack mismatch while skipping");
            }
            engine->conditional_count = target;
            return 0;
        } else if (command == HSTEX_COMMAND_ELSE && skipped_depth == 0U) {
            if (active_nested != 0U) {
                struct hstex_conditional *nested =
                    &engine->conditionals[engine->conditional_count - 1U];
                if (nested->else_seen) {
                    return set_error(error, error_capacity,
                                     "second else in nested conditional");
                }
                nested->else_seen = true;
                continue;
            }
            if (!stop_at_else) {
                return set_error(error, error_capacity,
                                 "second else in one conditional");
            }
            engine->conditionals[target].else_seen = true;
            return 0;
        }
    }
}

static int skip_case_to_branch(struct hstex_engine *engine, size_t target,
                               int32_t selection, char *error,
                               size_t error_capacity)
{
    if (target >= engine->conditional_count) {
        return set_error(error, error_capacity,
                         "ifcase skip target is not active");
    }
    size_t active_nested = engine->conditional_count - target - 1U;
    size_t skipped_depth = 0U;
    int32_t remaining = selection;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        if (raw_next(engine, &token, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input while skipping ifcase branches");
        }
        token = normalize_unexpanded_control_sequence(token);
        if (!hstex_token_is_control_sequence(token)) {
            continue;
        }
        enum hstex_command command =
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token))
                ->command;
        if (command_starts_conditional(command)) {
            ++skipped_depth;
            continue;
        }
        if (command == HSTEX_COMMAND_FI) {
            if (skipped_depth != 0U) {
                --skipped_depth;
                continue;
            }
            if (active_nested != 0U) {
                --engine->conditional_count;
                --active_nested;
                continue;
            }
            engine->conditional_count = target;
            return 0;
        }
        if (skipped_depth != 0U || active_nested != 0U) {
            continue;
        }
        if (command == HSTEX_COMMAND_OR && remaining > 0) {
            --remaining;
            if (remaining == 0) {
                engine->conditionals[target].branch_true = true;
                return 0;
            }
            continue;
        }
        if (command == HSTEX_COMMAND_ELSE) {
            engine->conditionals[target].else_seen = true;
            engine->conditionals[target].branch_true = true;
            return 0;
        }
    }
}

static int skip_case_remainder(struct hstex_engine *engine, size_t target,
                               char *error, size_t error_capacity)
{
    if (target >= engine->conditional_count) {
        return set_error(error, error_capacity,
                         "ifcase remainder target is not active");
    }
    size_t active_nested = engine->conditional_count - target - 1U;
    size_t skipped_depth = 0U;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        if (raw_next(engine, &token, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input while skipping ifcase remainder");
        }
        token = normalize_unexpanded_control_sequence(token);
        if (!hstex_token_is_control_sequence(token)) {
            continue;
        }
        enum hstex_command command =
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token))
                ->command;
        if (command_starts_conditional(command)) {
            ++skipped_depth;
        } else if (command == HSTEX_COMMAND_FI) {
            if (skipped_depth != 0U) {
                --skipped_depth;
                continue;
            }
            if (active_nested != 0U) {
                --engine->conditional_count;
                --active_nested;
                continue;
            }
            engine->conditional_count = target;
            return 0;
        } else if (command == HSTEX_COMMAND_ELSE && skipped_depth == 0U &&
                   active_nested == 0U) {
            if (engine->conditionals[target].else_seen) {
                return set_error(error, error_capacity,
                                 "second else in one ifcase");
            }
            engine->conditionals[target].else_seen = true;
        }
    }
}

/* The innermost file being read, and the line reached in it. */
static const char *current_source_line(const struct hstex_engine *engine,
                                       uint32_t *line)
{
    for (size_t index = engine->sources.count; index > 0U; --index) {
        const struct hstex_source_frame *frame =
            &engine->sources.frames[index - 1U];
        if (frame->kind == HSTEX_SOURCE_FILE) {
            *line = frame->value.file.mouth.line_number;
            return frame->value.file.path;
        }
    }
    *line = 0U;
    return "<no file>";
}

static int push_conditional(struct hstex_engine *engine, size_t *index,
                            char *error, size_t error_capacity)
{
    if (reserve_conditionals(engine, engine->conditional_count + 1U, error,
                             error_capacity) != 0) {
        return -1;
    }
    *index = engine->conditional_count++;
    struct hstex_conditional *entry = &engine->conditionals[*index];
    entry->branch_true = false;
    entry->else_seen = false;
    entry->case_conditional = false;
    entry->negate = engine->negate_next_conditional;
    entry->evaluated = false;
    entry->origin = current_source_line(engine, &entry->line);
    engine->negate_next_conditional = false;
    return 0;
}

/* Put the token back and stand a \relax in front of it. */
static int push_relax_before(struct hstex_engine *engine, hstex_token token,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    hstex_cs_id relax = 0U;
    if (hstex_symbol_find(&engine->lexical_state.symbols, HSTEX_SYMBOL_REGULAR,
                          (const uint8_t *)"relax", 5U, &relax) != 1) {
        return set_error(error, error_capacity, "relax is not defined");
    }
    hstex_token *pair = malloc(2U * sizeof(*pair));
    if (pair == NULL) {
        return set_error(error, error_capacity,
                         "conditional relax allocation failed");
    }
    pair[0] = hstex_token_control_sequence(relax);
    pair[1] = token;
    return hstex_source_push_owned_tokens(&engine->sources, pair, 2U, location,
                                          error, error_capacity);
}

/* \else, \or, and \fi met while the innermost conditional is still scanning
   its own test stand for \relax: they terminate whatever is being scanned and
   stay in the input for the conditional to find afterwards. `\ifnum ...>20\else`
   with no space between the number and \else depends on this. */
static bool conditional_test_pending(const struct hstex_engine *engine)
{
    return engine->conditional_count != 0U &&
           !engine->conditionals[engine->conditional_count - 1U].evaluated;
}

static int finish_conditional(struct hstex_engine *engine, size_t index,
                              bool condition, char *error,
                              size_t error_capacity)
{
    if (index >= engine->conditional_count) {
        return set_error(error, error_capacity,
                         "conditional disappeared while scanning its test");
    }
    if (engine->conditionals[index].negate) {
        condition = !condition;
    }
    engine->conditionals[index].evaluated = true;
    engine->conditionals[index].branch_true = condition;
    if (!condition) {
        return skip_conditional(engine, index, true, error, error_capacity);
    }
    return 0;
}

static int start_conditional(struct hstex_engine *engine, bool condition,
                             char *error, size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    return finish_conditional(engine, conditional, condition, error,
                              error_capacity);
}

static int scan_if_num(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    int32_t left = 0;
    int32_t right = 0;
    hstex_token relation = 0U;
    struct hstex_source_location location;
    if (scan_integer(engine, &left, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    enum hstex_engine_result result = expanded_next_non_space(
        engine, &relation, &location, error, error_capacity);
    if (result != HSTEX_ENGINE_TOKEN) {
        engine->conditional_count = conditional;
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        return set_error(error, error_capacity,
                         "end of input while scanning an ifnum relation");
    }
    if (scan_integer(engine, &right, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    bool condition;
    if (token_is_other_character(relation, (uint8_t)'<')) {
        condition = left < right;
    } else if (token_is_other_character(relation, (uint8_t)'=')) {
        condition = left == right;
    } else if (token_is_other_character(relation, (uint8_t)'>')) {
        condition = left > right;
    } else {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity,
                         "ifnum requires <, =, or >");
    }
    return finish_conditional(engine, conditional, condition, error,
                              error_capacity);
}

static int scan_if_dim(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    int32_t left = 0;
    int32_t right = 0;
    hstex_token relation = 0U;
    struct hstex_source_location location;
    if (scan_dimension(engine, &left, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    enum hstex_engine_result result = expanded_next_non_space(
        engine, &relation, &location, error, error_capacity);
    if (result != HSTEX_ENGINE_TOKEN) {
        engine->conditional_count = conditional;
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        return set_error(error, error_capacity,
                         "end of input while scanning an ifdim relation");
    }
    if (scan_dimension(engine, &right, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    bool condition;
    if (token_is_other_character(relation, (uint8_t)'<')) {
        condition = left < right;
    } else if (token_is_other_character(relation, (uint8_t)'=')) {
        condition = left == right;
    } else if (token_is_other_character(relation, (uint8_t)'>')) {
        condition = left > right;
    } else {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity,
                         "ifdim requires <, =, or >");
    }
    return finish_conditional(engine, conditional, condition, error,
                              error_capacity);
}

static int scan_if_x(struct hstex_engine *engine, char *error,
                     size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    hstex_token left = 0U;
    hstex_token right = 0U;
    struct hstex_source_location location;
    if (raw_next(engine, &left, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        raw_next(engine, &right, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity, "end of input in ifx");
    }
    return finish_conditional(engine, conditional,
                              ifx_tokens_equal(engine, left, right), error,
                              error_capacity);
}

static int scan_if_odd(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    int32_t value = 0;
    if (scan_integer(engine, &value, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity, "invalid ifodd integer");
    }
    return finish_conditional(engine, conditional, value % 2 != 0, error,
                              error_capacity);
}

/* \iffontchar asks whether the font defines a character at all. */
static int scan_if_font_char(struct hstex_engine *engine, char *error,
                             size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    uint32_t identifier = 0U;
    int32_t code = 0;
    if (scan_font_identifier(engine, &identifier, error, error_capacity) != 0 ||
        scan_integer(engine, &code, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    if (code < 0 || (size_t)code >= HSTEX_FONT_CHARACTER_COUNT) {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity, "bad character code (%d)",
                         code);
    }
    const struct hstex_font *font = font_by_identifier(engine, identifier);
    bool defined = font != NULL && font->characters != NULL &&
                   font->characters[(size_t)code].tag >= 0;
    return finish_conditional(engine, conditional, defined, error,
                              error_capacity);
}

/* \ifhbox, \ifvbox and \ifvoid each ask one question about a register. */
static int scan_if_box(struct hstex_engine *engine, int32_t subtype,
                       char *error, size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    int32_t index = 0;
    if (scan_integer(engine, &index, error, error_capacity) != 0 || index < 0 ||
        (size_t)index >= engine->count_capacity) {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity,
                         "box register %d is outside 0..%zu", index,
                         engine->count_capacity - 1U);
    }
    enum hstex_box_kind kind = engine->boxes[(size_t)index].kind;
    bool condition = subtype == (int32_t)HSTEX_IF_BOX_HORIZONTAL
                         ? kind == HSTEX_BOX_HLIST
                         : subtype == (int32_t)HSTEX_IF_BOX_VERTICAL
                               ? kind == HSTEX_BOX_VLIST
                               : kind == HSTEX_BOX_VOID;
    return finish_conditional(engine, conditional, condition, error,
                              error_capacity);
}

static int scan_if_case(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    engine->conditionals[conditional].case_conditional = true;
    int32_t selection = 0;
    if (scan_integer(engine, &selection, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        /* Keep the scanner's own diagnosis rather than masking it. */
        return -1;
    }
    engine->conditionals[conditional].evaluated = true;
    if (selection == 0) {
        engine->conditionals[conditional].branch_true = true;
        return 0;
    }
    return skip_case_to_branch(engine, conditional, selection, error,
                               error_capacity);
}

static int if_character_code(const struct hstex_engine *engine,
                             hstex_token token)
{
    if (hstex_token_is_character(token)) {
        return (int)hstex_token_character_code(token);
    }
    if (!hstex_token_is_control_sequence(token)) {
        return 256;
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_CHAR_GIVEN &&
        meaning->value.integer >= 0 && meaning->value.integer <= 255) {
        return (int)meaning->value.integer;
    }
    if (meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
        hstex_token_is_character(meaning->value.token)) {
        return (int)hstex_token_character_code(meaning->value.token);
    }
    return 256;
}

static int scan_if_char(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    hstex_token left = 0U;
    hstex_token right = 0U;
    struct hstex_source_location location;
    if (hstex_engine_next_expanded(engine, &left, &location, error,
                                   error_capacity) != HSTEX_ENGINE_TOKEN ||
        hstex_engine_next_expanded(engine, &right, &location, error,
                                   error_capacity) != HSTEX_ENGINE_TOKEN) {
        engine->conditional_count = conditional;
        return set_error(error, error_capacity, "end of input in if");
    }
    return finish_conditional(
        engine, conditional,
        if_character_code(engine, left) == if_character_code(engine, right),
        error, error_capacity);
}

static int next_if_cat_operand(struct hstex_engine *engine, hstex_token *token,
                               char *error, size_t error_capacity)
{
    struct hstex_source_location location;
    if (hstex_engine_next_expanded(engine, token, &location, error,
                                   error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in ifcat");
    }
    if (!engine->returned_unexpanded && hstex_token_is_control_sequence(*token) &&
        hstex_engine_meaning(engine, hstex_token_control_sequence_id(*token))
                ->command == HSTEX_COMMAND_UNDEFINED) {
        return set_undefined_control_sequence_error(engine, *token, error,
                                                    error_capacity);
    }
    return 0;
}

static int if_category_code(const struct hstex_engine *engine,
                            hstex_token token)
{
    if (hstex_token_is_character(token)) {
        return (int)hstex_token_category(token);
    }
    if (hstex_token_is_control_sequence(token)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(token));
        if (meaning->command == HSTEX_COMMAND_TOKEN_ALIAS &&
            hstex_token_is_character(meaning->value.token)) {
            return (int)hstex_token_category(meaning->value.token);
        }
    }
    return 16;
}

static int scan_if_cat(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    size_t conditional = 0U;
    if (push_conditional(engine, &conditional, error, error_capacity) != 0) {
        return -1;
    }
    hstex_token left = 0U;
    hstex_token right = 0U;
    if (next_if_cat_operand(engine, &left, error, error_capacity) != 0 ||
        next_if_cat_operand(engine, &right, error, error_capacity) != 0) {
        engine->conditional_count = conditional;
        return -1;
    }
    return finish_conditional(
        engine, conditional,
        if_category_code(engine, left) == if_category_code(engine, right), error,
        error_capacity);
}

static int begin_group(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    if (engine->group_level == UINT32_MAX) {
        return set_error(error, error_capacity, "group nesting overflow");
    }
    ++engine->group_level;
    return 0;
}

static int end_group(struct hstex_engine *engine, char *error,
                     size_t error_capacity)
{
    if (engine->group_level == 0U) {
        return set_error(error, error_capacity, "extra end-group token");
    }
    uint32_t leaving_level = engine->group_level;
    while (engine->save_count != 0U &&
           engine->saves[engine->save_count - 1U].level == leaving_level) {
        struct hstex_save_entry save = engine->saves[--engine->save_count];
        switch (save.kind) {
        case HSTEX_SAVE_MEANING: {
            struct hstex_meaning *current =
                &engine->meanings[save.index - 1U];
            if (current->level == leaving_level) {
                *current = save.previous.meaning;
            }
            break;
        }
        case HSTEX_SAVE_CAT_CODE:
            if (engine->catcode_levels[save.index] == leaving_level) {
                if (hstex_catcode_set(&engine->lexical_state.catcodes,
                                      save.index,
                                      save.previous.category) != 0) {
                    return set_error(error, error_capacity,
                                     "failed to restore catcode");
                }
                engine->catcode_levels[save.index] = save.previous_level;
            }
            break;
        case HSTEX_SAVE_PAR_SHAPE:
            if (engine->parshape_level == leaving_level) {
                engine->parshape = (uint32_t)save.previous.integer;
                engine->parshape_level = save.previous_level;
            }
            break;
        case HSTEX_SAVE_MATH_FONT: {
            size_t size = save.index / 16U;
            size_t family = save.index % 16U;
            if (engine->math_font_levels[size][family] == leaving_level) {
                engine->math_fonts[size][family] =
                    (uint32_t)save.previous.integer;
                engine->math_font_levels[size][family] = save.previous_level;
            }
            break;
        }
        case HSTEX_SAVE_COUNT:
            if (engine->count_levels[save.index] == leaving_level) {
                engine->counts[save.index] = save.previous.integer;
                engine->count_levels[save.index] = save.previous_level;
            }
            break;
        case HSTEX_SAVE_INTEGER_PARAMETER:
            if (engine->integer_parameter_levels[save.index] == leaving_level) {
                engine->integer_parameters[save.index] = save.previous.integer;
                engine->integer_parameter_levels[save.index] =
                    save.previous_level;
                if (save.index ==
                    (uint32_t)HSTEX_INTEGER_END_LINE_CHARACTER) {
                    engine->lexical_state.end_line_character =
                        save.previous.integer;
                }
            }
            break;
        case HSTEX_SAVE_DIMEN:
            if (engine->dimen_levels[save.index] == leaving_level) {
                engine->dimens[save.index] = save.previous.integer;
                engine->dimen_levels[save.index] = save.previous_level;
            }
            break;
        case HSTEX_SAVE_GLUE:
            if (engine->glue_levels[save.index] == leaving_level) {
                engine->glues[save.index] = save.previous.glue;
                engine->glue_levels[save.index] = save.previous_level;
            }
            break;
        case HSTEX_SAVE_MUGLUE:
            if (engine->muglue_levels[save.index] == leaving_level) {
                engine->muglues[save.index] = save.previous.glue;
                engine->muglue_levels[save.index] = save.previous_level;
            }
            break;
        case HSTEX_SAVE_DIMEN_PARAMETER:
            if (engine->dimen_parameter_levels[save.index] == leaving_level) {
                engine->dimen_parameters[save.index] = save.previous.integer;
                engine->dimen_parameter_levels[save.index] =
                    save.previous_level;
            }
            break;
        case HSTEX_SAVE_GLUE_PARAMETER:
            if (engine->glue_parameter_levels[save.index] == leaving_level) {
                engine->glue_parameters[save.index] = save.previous.glue;
                engine->glue_parameter_levels[save.index] =
                    save.previous_level;
            }
            break;
        case HSTEX_SAVE_MUGLUE_PARAMETER:
            if (engine->muglue_parameter_levels[save.index] == leaving_level) {
                engine->muglue_parameters[save.index] = save.previous.glue;
                engine->muglue_parameter_levels[save.index] =
                    save.previous_level;
            }
            break;
        case HSTEX_SAVE_CODE: {
            uint32_t table = save.index / 256U;
            uint32_t character = save.index % 256U;
            if (engine->code_levels[table][character] == leaving_level) {
                engine->code_tables[table][character] = save.previous.integer;
                engine->code_levels[table][character] = save.previous_level;
            }
            break;
        }
        case HSTEX_SAVE_TOKEN_REGISTER:
            if (engine->token_register_levels[save.index] == leaving_level) {
                engine->token_registers[save.index] =
                    save.previous.token_list_identifier;
                engine->token_register_levels[save.index] =
                    save.previous_level;
            }
            break;
        case HSTEX_SAVE_TOKEN_PARAMETER:
            if (engine->token_parameter_levels[save.index] == leaving_level) {
                engine->token_parameters[save.index] =
                    save.previous.token_list_identifier;
                engine->token_parameter_levels[save.index] =
                    save.previous_level;
            }
            break;
        case HSTEX_SAVE_BOX:
            if (engine->box_levels[save.index] == leaving_level) {
                engine->boxes[save.index] = save.previous.box;
                engine->box_levels[save.index] = save.previous_level;
            }
            break;
        case HSTEX_SAVE_AFTER_GROUP:
            if (push_one(engine, save.previous.after_group.token,
                         save.previous.after_group.location, error,
                         error_capacity) != 0) {
                return -1;
            }
            break;
        }
    }
    --engine->group_level;
    return 0;
}

static int set_undefined_control_sequence_error(
    const struct hstex_engine *engine, hstex_token token, char *error,
    size_t error_capacity)
{
    enum hstex_symbol_kind kind;
    const uint8_t *name = NULL;
    size_t length = 0U;
    if (hstex_symbol_name(&engine->lexical_state.symbols,
                          hstex_token_control_sequence_id(token), &kind, &name,
                          &length) != 0) {
        return set_error(error, error_capacity,
                         "undefined control sequence with invalid identifier");
    }
    int printable_length =
        length > (size_t)INT_MAX ? INT_MAX : (int)length;
    if (kind == HSTEX_SYMBOL_ACTIVE) {
        return set_error(error, error_capacity,
                         "undefined active character: %.*s", printable_length,
                         (const char *)name);
    }
    uint32_t line = 0U;
    const char *origin = current_source_line(engine, &line);
    return set_error(error, error_capacity,
                     "undefined control sequence: \\%.*s, at %s:%u",
                     printable_length, (const char *)name, origin,
                     (unsigned int)line);
}

static int execute_ignore_spaces(struct hstex_engine *engine, char *error,
                                 size_t error_capacity)
{
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            return 0;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        if (token_is_effective_space(engine, token)) {
            continue;
        }
        if (engine->returned_unexpanded &&
            !engine->returned_unexpanded_executable &&
            hstex_token_is_control_sequence(token)) {
            token = hstex_token_frozen_control_sequence(
                hstex_token_control_sequence_id(token));
        }
        return push_one(engine, token, location, error, error_capacity);
    }
}

/* Run the document: everything the executor does not handle itself belongs
   to the main vertical list, which is built exactly as a \vbox body is. See
   docs/DECISIONS.md, the-main-vertical-list. */
int hstex_engine_run(struct hstex_engine *engine,
                     struct hstex_source_location *last, char *error,
                     size_t error_capacity)
{
    struct hstex_source_location location = {0};
    for (;;) {
        hstex_token token = 0U;
        enum hstex_engine_result result = hstex_engine_next_output(
            engine, &token, &location, error, error_capacity);
        if (last != NULL) {
            *last = location;
        }
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
        if (handle_vertical_list_token(engine, token, location, error,
                                       error_capacity) != 0) {
            return -1;
        }
    }
    /* Whatever paragraph was still being filled ends the way \end ends it. */
    if (engine->building_paragraph &&
        finish_paragraph(engine, error, error_capacity) != 0) {
        return -1;
    }
    return 0;
}

enum hstex_engine_result hstex_engine_next_output(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity)
{
    if (engine->dump_requested || engine->end_requested) {
        return HSTEX_ENGINE_EOF;
    }
    for (;;) {
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, token, location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            if (engine->group_stop_hit) {
                return result;
            }
            /* A group or a conditional may be opened on one side of a
               box or an alignment entry and closed on the other -- LaTeX's
               \\[2mm] in a table ends the row from inside an \\ifdim -- so
               neither is checked where the reading stops at a boundary
               rather than at the end of everything. See
               docs/DECISIONS.md, conditionals-across-boxes. */
            if (hstex_source_at_boundary(&engine->sources)) {
                return result;
            }
            if (engine->group_level != engine->output_group_floor) {
                (void)set_error(error, error_capacity,
                                "end of input inside a group");
                return HSTEX_ENGINE_ERROR;
            }
            if (engine->conditional_count >
                engine->output_conditional_floor) {
                (void)set_error(error, error_capacity,
                                "end of input inside a conditional");
                return HSTEX_ENGINE_ERROR;
            }
            return result;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            return result;
        }
        if (engine->returned_unexpanded &&
            hstex_token_is_control_sequence(*token)) {
            if (engine->returned_unexpanded_executable &&
                push_one(engine, *token, *location, error, error_capacity) !=
                    0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }

handle_token:
        if (hstex_token_is_character(*token)) {
            /* A formula is delimited by math shifts rather than braces, so
               the executor recognises them itself; see docs/DECISIONS.md,
               math-mode. */
            if (token_is_category(*token, HSTEX_CAT_ALIGNMENT_TAB) &&
                engine->alignment_entry != NULL &&
                !engine->alignment_entry->after_pushed) {
                if (end_alignment_entry(engine, HSTEX_ALIGN_END_TAB, error,
                                        error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            if (token_is_category(*token, HSTEX_CAT_MATH_SHIFT)) {
                /* An alignment that stood in for a whole display leaves the
                   engine in vertical mode, so the $$ that closes it is
                   recognised before anything else. */
                if (engine->display_alignment) {
                    hstex_token second = 0U;
                    struct hstex_source_location where;
                    if (raw_next(engine, &second, &where, error,
                                 error_capacity) != HSTEX_ENGINE_TOKEN ||
                        !token_is_effective_category(
                            engine, second, (uint8_t)HSTEX_CAT_MATH_SHIFT)) {
                        char found[128];
                        describe_token(engine, second, found, sizeof(found));
                        uint32_t line = 0U;
                        const char *origin = current_source_line(engine, &line);
                        (void)set_error(error, error_capacity,
                                        "a display must be closed by $$, "
                                        "found %s at %s:%u",
                                        found, origin, (unsigned int)line);
                        return HSTEX_ENGINE_ERROR;
                    }
                    if (end_display_alignment(engine, error, error_capacity) !=
                        0) {
                        return HSTEX_ENGINE_ERROR;
                    }
                    continue;
                }
                /* Math shift starts a paragraph in vertical mode, and like
                   every other horizontal command it is read again once
                   \everypar has run. */
                if (engine->mode == HSTEX_MODE_VERTICAL) {
                    if (push_one(engine, *token, *location, error,
                                 error_capacity) != 0 ||
                        start_paragraph(engine, true, error, error_capacity) !=
                            0) {
                        return HSTEX_ENGINE_ERROR;
                    }
                    continue;
                }
                if (engine->mode == HSTEX_MODE_MATH) {
                    /* A display closes on the second of two shifts. */
                    if (engine->displayed_math) {
                        hstex_token second = 0U;
                        struct hstex_source_location where;
                        if (raw_next(engine, &second, &where, error,
                                     error_capacity) != HSTEX_ENGINE_TOKEN ||
                            !token_is_effective_category(
                                engine, second,
                                (uint8_t)HSTEX_CAT_MATH_SHIFT)) {
                            char found[128];
                            describe_token(engine, second, found,
                                           sizeof(found));
                            uint32_t line = 0U;
                            const char *origin =
                                current_source_line(engine, &line);
                            (void)set_error(
                                error, error_capacity,
                                "a display must be closed by $$, found %s at "
                                "%s:%u",
                                found, origin, (unsigned int)line);
                            return HSTEX_ENGINE_ERROR;
                        }
                        if (end_display_math(engine, error, error_capacity) !=
                            0) {
                            return HSTEX_ENGINE_ERROR;
                        }
                        continue;
                    }
                    if (end_math(engine, error, error_capacity) != 0) {
                        return HSTEX_ENGINE_ERROR;
                    }
                    continue;
                }
                /* Two shifts in an outer paragraph open a display. */
                if (!engine->inner_mode) {
                    hstex_token second = 0U;
                    struct hstex_source_location where;
                    enum hstex_engine_result twice =
                        raw_next(engine, &second, &where, error,
                                 error_capacity);
                    if (twice == HSTEX_ENGINE_ERROR) {
                        return HSTEX_ENGINE_ERROR;
                    }
                    if (twice == HSTEX_ENGINE_TOKEN) {
                        if (token_is_effective_category(
                                engine, second,
                                (uint8_t)HSTEX_CAT_MATH_SHIFT)) {
                            if (begin_display_math(engine, error,
                                                   error_capacity) != 0) {
                                return HSTEX_ENGINE_ERROR;
                            }
                            continue;
                        }
                        if (push_one(engine, second, where, error,
                                     error_capacity) != 0) {
                            return HSTEX_ENGINE_ERROR;
                        }
                    }
                }
                if (begin_math(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            if (token_is_category(*token, HSTEX_CAT_BEGIN_GROUP)) {
                if (engine->mode == HSTEX_MODE_MATH &&
                    begin_math_group(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                if (begin_group(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            if (token_is_category(*token, HSTEX_CAT_END_GROUP)) {
                if (engine->mode == HSTEX_MODE_MATH &&
                    finish_math_group(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                /* A brace that ends a box body ends the paragraph inside it
                   first, while the parameters that paragraph was set with
                   are still in force. */
                if (engine->building_alignment == false &&
                    engine->building_paragraph && engine->group_stop_armed &&
                    engine->group_level == engine->group_stop_level + 1U &&
                    finish_paragraph(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                if (end_group(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                /* A box body is executed on the live input; when the group it
                   opened closes, the box is complete and the builder takes
                   over again. */
                if (engine->group_stop_armed &&
                    engine->group_level <= engine->group_stop_level) {
                    engine->group_stop_hit = true;
                    return HSTEX_ENGINE_EOF;
                }
                continue;
            }
            if ((engine->pending_global || engine->pending_macro_flags != 0U) &&
                token_is_space(*token)) {
                continue;
            }
            if (engine->pending_global || engine->pending_macro_flags != 0U) {
                (void)set_error(error, error_capacity,
                                "definition prefix followed by a character");
                return HSTEX_ENGINE_ERROR;
            }
            /* Inside a formula a character is an atom, not something the
               enclosing list builder should see. Spaces are ignored. */
            if (engine->mode == HSTEX_MODE_MATH) {
                if (token_is_space(*token)) {
                    continue;
                }
                if (token_is_category(*token, HSTEX_CAT_SUPERSCRIPT) ||
                    token_is_category(*token, HSTEX_CAT_SUBSCRIPT)) {
                    if (begin_math_script(
                            engine,
                            token_is_category(*token, HSTEX_CAT_SUPERSCRIPT),
                            error, error_capacity) != 0) {
                        return HSTEX_ENGINE_ERROR;
                    }
                    continue;
                }
                /* A mathcode of "8000 makes the character behave as an
                   active one; see docs/DECISIONS.md, math-active. */
                int table = code_table_index(HSTEX_COMMAND_MATH_CODE);
                uint8_t character = hstex_token_character_code(*token);
                if (table >= 0 &&
                    engine->code_tables[(size_t)table][character] == 0x8000) {
                    hstex_cs_id active = 0U;
                    if (hstex_symbol_intern(&engine->lexical_state.symbols,
                                            HSTEX_SYMBOL_ACTIVE, &character,
                                            1U, &active, error,
                                            error_capacity) != 0 ||
                        reserve_meanings(engine, (size_t)active, error,
                                         error_capacity) != 0 ||
                        push_one(engine,
                                 hstex_token_control_sequence(active),
                                 *location, error, error_capacity) != 0) {
                        return HSTEX_ENGINE_ERROR;
                    }
                    continue;
                }
                if (math_append_character(engine, character, error,
                                          error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            return HSTEX_ENGINE_TOKEN;
        }

        /* Anything that is not a character ends the pair the ligature and
           kerning program was waiting on. */
        if (engine->has_pending_character &&
            flush_pending_character(engine, error, error_capacity) != 0) {
            return HSTEX_ENGINE_ERROR;
        }
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(*token));
        if (engine->mode == HSTEX_MODE_VERTICAL &&
            !engine->pending_global && engine->pending_macro_flags == 0U &&
            command_starts_paragraph(meaning)) {
            if (push_one(engine, *token, *location, error, error_capacity) !=
                    0 ||
                start_paragraph(engine, true, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (engine->mode == HSTEX_MODE_HORIZONTAL &&
            engine->building_paragraph && !engine->pending_global &&
            engine->pending_macro_flags == 0U &&
            command_ends_paragraph(meaning)) {
            if (push_one(engine, *token, *location, error, error_capacity) !=
                    0 ||
                finish_paragraph(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        record_executing_name(engine, *token);
        switch (meaning->command) {
        case HSTEX_COMMAND_RELAX:
            engine->pending_global = false;
            engine->pending_macro_flags = 0U;
            continue;
        case HSTEX_COMMAND_DEF:
            if (finish_assignment(
                    engine,
                    scan_definition(engine, false, false, error,
                                    error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_GDEF:
            if (finish_assignment(
                    engine,
                    scan_definition(engine, true, false, error,
                                    error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_EDEF:
            if (finish_assignment(
                    engine,
                    scan_definition(engine, false, true, error,
                                    error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_XDEF:
            if (finish_assignment(
                    engine,
                    scan_definition(engine, true, true, error,
                                    error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LET:
            if (finish_assignment(engine,
                                  scan_let(engine, error, error_capacity), error,
                                  error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_FUTURE_LET:
            if (finish_assignment(
                    engine, scan_future_let(engine, error, error_capacity), error,
                    error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_AFTER_ASSIGNMENT:
            if (scan_after_assignment(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_AFTER_GROUP:
            if (scan_after_group(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LONG:
            engine->pending_macro_flags |= (uint8_t)HSTEX_MACRO_LONG;
            continue;
        case HSTEX_COMMAND_OUTER:
            engine->pending_macro_flags |= (uint8_t)HSTEX_MACRO_OUTER;
            continue;
        case HSTEX_COMMAND_PROTECTED:
            engine->pending_macro_flags |= (uint8_t)HSTEX_MACRO_PROTECTED;
            continue;
        case HSTEX_COMMAND_GLOBAL:
            engine->pending_global = true;
            continue;
        case HSTEX_COMMAND_UNLESS:
            if (expand_unless(engine, *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_BEGIN_GROUP:
            if (begin_group(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_END_GROUP:
            if (end_group(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LOWER_CASE:
        case HSTEX_COMMAND_UPPER_CASE:
            if (execute_case_shift(
                    engine,
                    meaning->command == HSTEX_COMMAND_LOWER_CASE ? 1U : 2U,
                    *location, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IGNORE_SPACES:
            if (engine->pending_global || engine->pending_macro_flags != 0U) {
                return (enum hstex_engine_result)set_error(
                    error, error_capacity,
                    "definition prefix followed by ignorespaces");
            }
            if (execute_ignore_spaces(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_INTERACTION_MODE:
            if (meaning->value.integer < (int32_t)HSTEX_INTERACTION_BATCH ||
                meaning->value.integer >
                    (int32_t)HSTEX_INTERACTION_ERROR_STOP ||
                engine->pending_global || engine->pending_macro_flags != 0U) {
                return (enum hstex_engine_result)set_error(
                    error, error_capacity, "invalid interaction-mode command");
            }
            engine->interaction_mode =
                (enum hstex_interaction_mode)meaning->value.integer;
            continue;
        case HSTEX_COMMAND_DUMP:
            if (engine->pending_global || engine->pending_macro_flags != 0U ||
                engine->group_level != 0U ||
                engine->conditional_count != 0U) {
                return (enum hstex_engine_result)set_error(
                    error, error_capacity, "dump requested in nested state");
            }
            engine->dump_requested = true;
            return HSTEX_ENGINE_EOF;
        case HSTEX_COMMAND_CAT_CODE:
            if (finish_assignment(
                    engine, scan_catcode_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SF_CODE:
        case HSTEX_COMMAND_LC_CODE:
        case HSTEX_COMMAND_UC_CODE:
        case HSTEX_COMMAND_MATH_CODE:
        case HSTEX_COMMAND_DEL_CODE:
            if (finish_assignment(
                    engine,
                    scan_code_assignment(engine, meaning->command, error,
                                         error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CHAR_DEF:
            if (finish_assignment(
                    engine, scan_char_definition(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_FONT:
            if (execute_math_font(engine, meaning->value.integer, error,
                                  error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_CHAR:
            if (execute_math_char(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_CLASS:
            if (execute_math_class(engine, meaning->value.integer, error,
                                   error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_STYLE:
            if (execute_math_style(engine, meaning->value.integer, error,
                                   error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_CHOICE:
            if (execute_math_choice(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DELIMITER:
            if (execute_delimiter(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LEFT_RIGHT:
            if (execute_left_right(engine, meaning->value.integer, error,
                                   error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_ACCENT:
            if (execute_accent(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_VCENTER:
            if (execute_vcenter(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_EQUATION_NUMBER:
            if (execute_equation_number(engine, meaning->value.integer != 0,
                                        error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_HALIGN:
            if (execute_halign(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CR:
        case HSTEX_COMMAND_SPAN:
            /* A tab or \cr ends the entry from wherever it turns up, which
               may be inside a box the entry's template opened. */
            if (engine->alignment_entry != NULL &&
                !engine->alignment_entry->after_pushed) {
                if (end_alignment_entry(
                        engine,
                        meaning->command == HSTEX_COMMAND_SPAN
                            ? HSTEX_ALIGN_END_SPAN
                            : HSTEX_ALIGN_END_CR,
                        error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            if (!engine->building_alignment) {
                char misplaced[128];
                describe_token(engine, *token, misplaced, sizeof(misplaced));
                uint32_t line = 0U;
                const char *origin = current_source_line(engine, &line);
                (void)set_error(error, error_capacity,
                                "%s is only allowed inside an alignment, at "
                                "%s:%u",
                                misplaced, origin, (unsigned int)line);
                return HSTEX_ENGINE_ERROR;
            }
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_NO_ALIGN:
        case HSTEX_COMMAND_OMIT:
            /* These belong to an alignment, which reads them itself. */
            if (!engine->building_alignment) {
                char misplaced[128];
                describe_token(engine, *token, misplaced, sizeof(misplaced));
                uint32_t line = 0U;
                const char *origin = current_source_line(engine, &line);
                (void)set_error(error, error_capacity,
                                "%s is only allowed inside an alignment, at "
                                "%s:%u",
                                misplaced, origin, (unsigned int)line);
                return HSTEX_ENGINE_ERROR;
            }
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_MATH_LIMITS:
            /* Limit placement only shows in display style, which is not
               implemented; in text style the reference measures the same
               either way. */
            if (engine->mode != HSTEX_MODE_MATH) {
                (void)set_error(error, error_capacity,
                                "limit placement is only allowed in a formula");
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_SKIP:
        case HSTEX_COMMAND_MATH_KERN:
            if (execute_math_skip(engine,
                                  meaning->command == HSTEX_COMMAND_MATH_KERN,
                                  error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_CHAR_DEF:
            if (finish_assignment(
                    engine,
                    scan_math_char_definition(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PATTERNS:
        case HSTEX_COMMAND_HYPHENATION:
            if (finish_assignment(
                    engine,
                    scan_hyphen_data(
                        engine, meaning->command == HSTEX_COMMAND_PATTERNS,
                        error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_COUNT_DEF:
            if (finish_assignment(
                    engine, scan_count_definition(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN_DEF:
            if (finish_assignment(
                    engine,
                    scan_register_definition(engine, HSTEX_COMMAND_DIMEN_REGISTER,
                                             error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SKIP_DEF:
            if (finish_assignment(
                    engine,
                    scan_register_definition(engine, HSTEX_COMMAND_SKIP_REGISTER,
                                             error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MUSKIP_DEF:
            if (finish_assignment(
                    engine,
                    scan_register_definition(engine, HSTEX_COMMAND_MUSKIP_REGISTER,
                                             error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKS_DEF:
            if (finish_assignment(
                    engine,
                    scan_register_definition(engine, HSTEX_COMMAND_TOKS_REGISTER,
                                             error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_FONT:
            if (finish_assignment(
                    engine, scan_font_definition(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SET_BOX:
            if (finish_assignment(
                    engine, execute_set_box(engine, error, error_capacity), error,
                    error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_HBOX:
        case HSTEX_COMMAND_VBOX:
        case HSTEX_COMMAND_VTOP:
        case HSTEX_COMMAND_VSPLIT:
        case HSTEX_COMMAND_LAST_BOX:
            if (execute_box_constructor(engine, meaning->command, error,
                                        error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_FONT_CHAR_CODE:
            if (finish_assignment(
                    engine,
                    scan_font_char_code_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_FONT_CHAR_DIMEN:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "font character dimensions cannot be assigned");
        case HSTEX_COMMAND_MARGIN_KERN:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "a margin kern cannot be assigned");
        case HSTEX_COMMAND_BOX_DIMEN:
            if (finish_assignment(
                    engine,
                    scan_box_dimen_assignment(engine, meaning->value.integer,
                                              error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SHIFT_BOX:
            if (execute_shift_box(engine, meaning->value.integer, error,
                                  error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_UNBOX:
            if (execute_unbox(engine, meaning->value.integer, error,
                              error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_BOX:
        case HSTEX_COMMAND_COPY:
            if (execute_box_reference(engine, meaning->command, error,
                                      error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SPACE_FACTOR: {
            if (engine->mode != HSTEX_MODE_HORIZONTAL) {
                (void)set_error(error, error_capacity,
                                "spacefactor is only assignable in horizontal "
                                "mode");
                return HSTEX_ENGINE_ERROR;
            }
            int32_t factor = 0;
            if (finish_assignment(
                    engine,
                    scan_optional_equals(engine, error, error_capacity) != 0 ||
                            scan_integer(engine, &factor, error,
                                         error_capacity) != 0
                        ? -1
                        : 0,
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            if (factor <= 0 || factor > 32767) {
                (void)set_error(error, error_capacity,
                                "spacefactor must lie between 1 and 32767");
                return HSTEX_ENGINE_ERROR;
            }
            engine->space_factor = factor;
            continue;
        }
        case HSTEX_COMMAND_REMOVE_LAST:
            if (execute_remove_last(engine, meaning->value.integer, error,
                                    error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CONTROL_SPACE:
            if (execute_control_space(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_ITALIC_CORRECTION:
            if (execute_italic_correction(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CHAR:
            if (execute_char(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_INDENT:
            if (execute_indent(engine, meaning->value.integer != 0, error,
                               error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_VSKIP:
        case HSTEX_COMMAND_HSKIP:
            if (execute_glue(engine, meaning->value.integer,
                             meaning->command == HSTEX_COMMAND_VSKIP, error,
                             error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LEADERS:
            if (execute_leaders(engine, meaning->value.integer, error,
                                error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PENALTY:
            if (execute_penalty(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_VRULE:
            if (execute_vrule(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_HRULE:
            if (execute_hrule(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_KERN:
            if (execute_kern(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_CATALOG:
        case HSTEX_COMMAND_PDF_INFO:
            if (execute_pdf_dictionary(
                    engine, meaning->command == HSTEX_COMMAND_PDF_CATALOG,
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_OBJECT:
            if (execute_pdf_object(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_REF_OBJECT:
            if (execute_pdf_ref_object(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_LITERAL:
            if (execute_pdf_literal(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_DEST:
            if (execute_pdf_dest(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_START_LINK:
            if (execute_pdf_start_link(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_END_LINK:
            /* The link's extent is the backend's concern; nothing to do. */
            continue;
        case HSTEX_COMMAND_PDF_OUTLINE:
            if (execute_pdf_outline(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_XFORM:
            if (execute_pdf_xform(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_ANNOT:
            if (execute_pdf_annot(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_COLOR_STACK:
            if (execute_pdf_color_stack(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_LAST_NUMBER:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "pdf counters cannot be assigned");
        case HSTEX_COMMAND_LAST_ITEM:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "last-item queries cannot be assigned");
        case HSTEX_COMMAND_FONT_DIMEN:
            if (finish_assignment(
                    engine,
                    scan_font_dimen_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_HYPHEN_CHAR:
        case HSTEX_COMMAND_SKEW_CHAR:
            if (finish_assignment(
                    engine,
                    scan_font_integer_assignment(engine, meaning->command, error,
                                                 error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKS_REGISTER:
            if (finish_assignment(
                    engine,
                    scan_token_register_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKS:
            if (finish_assignment(
                    engine,
                    scan_token_family_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKEN_PARAMETER:
            if (finish_assignment(
                    engine,
                    scan_token_parameter_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_COUNT:
            if (finish_assignment(
                    engine,
                    scan_count_family_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_COUNT_REGISTER:
            if (finish_assignment(
                    engine,
                    scan_count_assignment(engine, meaning->value.integer, error,
                                          error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_INTEGER_PARAMETER:
            if (finish_assignment(
                    engine,
                    scan_integer_parameter_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PAGE_INTEGER:
            if (finish_assignment(
                    engine,
                    scan_page_integer_assignment(engine, meaning->value.integer,
                                                 error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PAGE_DIMEN:
            if (finish_assignment(
                    engine,
                    scan_page_dimen_assignment(engine, meaning->value.integer,
                                               error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN_REGISTER:
            if (finish_assignment(
                    engine,
                    scan_dimen_assignment(engine, meaning->value.integer, error,
                                          error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN:
            if (finish_assignment(
                    engine,
                    scan_dimen_family_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SKIP_REGISTER:
            if (finish_assignment(
                    engine,
                    scan_glue_assignment(engine, meaning->value.integer, error,
                                         error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MUSKIP_REGISTER:
            if (finish_assignment(
                    engine,
                    scan_muglue_assignment(engine, meaning->value.integer, error,
                                           error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SKIP:
            if (finish_assignment(
                    engine,
                    scan_glue_family_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MUSKIP:
            if (finish_assignment(
                    engine,
                    scan_muglue_family_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN_PARAMETER:
            if (finish_assignment(
                    engine,
                    scan_dimen_parameter_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PREV_DEPTH:
            if (finish_assignment(
                    engine,
                    scan_prev_depth_assignment(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_GLUE_PARAMETER:
            if (finish_assignment(
                    engine,
                    scan_glue_parameter_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MUGLUE_PARAMETER:
            if (finish_assignment(
                    engine,
                    scan_muglue_parameter_assignment(
                        engine, meaning->value.integer, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_ADVANCE:
        case HSTEX_COMMAND_MULTIPLY:
        case HSTEX_COMMAND_DIVIDE:
            if (finish_assignment(
                    engine,
                    execute_arithmetic(engine, meaning->command, error,
                                       error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IMMEDIATE:
            continue;
        case HSTEX_COMMAND_MESSAGE:
            if (execute_message(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_OPEN_OUT:
            if (execute_open_out(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_WRITE:
            if (execute_write(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CLOSE_OUT:
            if (execute_close_out(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_OPEN_IN:
            if (execute_open_in(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_READ:
            if (finish_assignment(engine,
                                  execute_read(engine, error, error_capacity),
                                  error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_READ_LINE:
            if (finish_assignment(
                    engine, execute_read_line(engine, error, error_capacity),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CLOSE_IN:
            if (execute_close_in(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_NUM:
            if (scan_if_num(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_DIM:
            if (scan_if_dim(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_H_MODE:
        case HSTEX_COMMAND_IF_V_MODE:
        case HSTEX_COMMAND_IF_M_MODE:
        case HSTEX_COMMAND_IF_INNER:
        case HSTEX_COMMAND_IF_IN_CS_NAME:
            if (start_conditional(
                    engine, mode_conditional_value(engine, meaning->command),
                    error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_X:
            if (scan_if_x(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_CHAR:
            if (scan_if_char(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_CAT:
            if (scan_if_cat(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_ODD:
            if (scan_if_odd(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_CASE:
            if (scan_if_case(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_FONT_CHAR:
            if (scan_if_font_char(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_BOX:
            if (scan_if_box(engine, meaning->value.integer, error,
                            error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_EOF:
            if (scan_if_eof(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_DEFINED:
            if (scan_if_defined(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_CS_NAME:
            if (scan_if_cs_name(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_TRUE:
            if (start_conditional(engine, true, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_IF_FALSE:
            if (start_conditional(engine, false, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_ELSE: {
            if (execute_else(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        case HSTEX_COMMAND_OR:
            if (execute_or(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_FI:
            if (execute_fi(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKEN_ALIAS:
            *token = meaning->value.token;
            goto handle_token;
        case HSTEX_COMMAND_MACRO:
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_CHAR_GIVEN:
            if (meaning->value.integer < 0 || meaning->value.integer > 255) {
                (void)set_error(error, error_capacity,
                                "invalid chardef meaning");
                return HSTEX_ENGINE_ERROR;
            }
            *token = hstex_token_character(
                (uint8_t)HSTEX_CAT_OTHER, (uint8_t)meaning->value.integer);
            goto handle_token;
        case HSTEX_COMMAND_MATH_CHAR_GIVEN:
            /* A \mathchardef is an atom wherever a \mathchar would be. */
            if (engine->mode == HSTEX_MODE_MATH) {
                if (math_append_code(engine, meaning->value.integer, error,
                                     error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_PAR_SHAPE:
            if (execute_parshape(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_FRACTION:
            if (execute_fraction(engine, meaning->value.integer, error,
                                 error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_OVER_UNDER_LINE:
            if (execute_over_under_line(engine, meaning->value.integer == 0,
                                        error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_NON_SCRIPT:
            if (execute_non_script(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_RADICAL:
            if (execute_radical(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MARKS:
        case HSTEX_COMMAND_MATH_PRIMITIVE:
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_FONT_GIVEN:
            if (meaning->value.integer <= 0 ||
                font_by_identifier(engine,
                                   (uint32_t)meaning->value.integer) == NULL) {
                return (enum hstex_engine_result)set_error(
                    error, error_capacity, "invalid font meaning");
            }
            engine->current_font = (uint32_t)meaning->value.integer;
            if (finish_assignment(engine, 0, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PAR:
            if (engine->pending_global || engine->pending_macro_flags != 0U) {
                (void)set_error(error, error_capacity,
                                "paragraph after definition prefix");
                return HSTEX_ENGINE_ERROR;
            }
            if (engine->building_paragraph) {
                if (finish_paragraph(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            /* \par in a vertical list does nothing but clear the shape the
               next paragraph would otherwise inherit. */
            if (engine->mode == HSTEX_MODE_VERTICAL &&
                normal_paragraph(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_UNDEFINED:
            return (enum hstex_engine_result)set_undefined_control_sequence_error(
                engine, *token, error, error_capacity);
        case HSTEX_COMMAND_EXPAND_AFTER:
        case HSTEX_COMMAND_NO_EXPAND:
        case HSTEX_COMMAND_CS_NAME:
        case HSTEX_COMMAND_EXPANDED:
        case HSTEX_COMMAND_UNEXPANDED:
        case HSTEX_COMMAND_DETOKENIZE:
        case HSTEX_COMMAND_PDF_FILE_SIZE:
        case HSTEX_COMMAND_PDF_STRING_COMPARE:
        case HSTEX_COMMAND_SCAN_TOKENS:
        case HSTEX_COMMAND_PDF_COLOR_STACK_INIT:
        case HSTEX_COMMAND_PDF_MATCH:
        case HSTEX_COMMAND_PDF_LAST_MATCH:
        case HSTEX_COMMAND_PDF_ESCAPE_STRING:
        case HSTEX_COMMAND_PDF_ESCAPE_NAME:
        case HSTEX_COMMAND_PDF_ESCAPE_HEX:
        case HSTEX_COMMAND_PDF_UNESCAPE_HEX:
        case HSTEX_COMMAND_PDF_TEX_REVISION:
        case HSTEX_COMMAND_THE:
        case HSTEX_COMMAND_NUMBER:
        case HSTEX_COMMAND_ROMAN_NUMERAL:
        case HSTEX_COMMAND_MEANING:
        case HSTEX_COMMAND_STRING:
        case HSTEX_COMMAND_JOB_NAME:
        case HSTEX_COMMAND_FONT_NAME:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "expandable primitive escaped expansion");
        case HSTEX_COMMAND_END_CS_NAME:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "extra endcsname");
        case HSTEX_COMMAND_INPUT:
            if (execute_input(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_PDF_GLYPH_TO_UNICODE:
            if (execute_pdf_glyph_to_unicode(engine, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_END:
            /* \end finishes the job when the main vertical list is empty and
               no output cycle is pending. Nothing reaches a main vertical list
               yet, so that condition always holds; a non-empty list must
               instead force one more output cycle once the page builder
               exists. */
            if (engine->page_integers[HSTEX_PAGE_DEAD_CYCLES] != 0) {
                return (enum hstex_engine_result)set_error(
                    error, error_capacity,
                    "end with a pending output cycle requires the page "
                    "builder");
            }
            engine->end_requested = true;
            return HSTEX_ENGINE_EOF;
        case HSTEX_COMMAND_END_INPUT:
            if (hstex_source_end_current_file(&engine->sources, error,
                                              error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_ERROR_MESSAGE:
            return (enum hstex_engine_result)execute_error_message(
                engine, error, error_capacity);
        case HSTEX_COMMAND_INPUT_LINE_NUMBER:
        case HSTEX_COMMAND_INTEGER_CONSTANT:
        case HSTEX_COMMAND_ENGINE_STATE_INTEGER:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "integer primitive used outside an integer context");
        case HSTEX_COMMAND_NUM_EXPR:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "numexpr used outside an integer context");
        case HSTEX_COMMAND_DIM_EXPR:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "dimexpr used outside a dimension context");
        case HSTEX_COMMAND_GLUE_EXPR:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "glueexpr used outside a glue context");
        case HSTEX_COMMAND_MU_EXPR:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "muexpr used outside a math-glue context");
        case HSTEX_COMMAND_MATH_GROUP:
        case HSTEX_COMMAND_LANGUAGE:
        case HSTEX_COMMAND_PENALTY_ARRAY:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "non-integer register execution is not implemented");
        }
    }
}
