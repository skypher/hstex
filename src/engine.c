#include "hstex/engine.h"

#include "hstex/catcode.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HSTEX_INITIAL_MEANING_CAPACITY = 64,
    HSTEX_INITIAL_MACRO_CAPACITY = 32,
    HSTEX_INITIAL_SAVE_CAPACITY = 64,
    HSTEX_MAX_PARAMETERS = 9,
};

struct token_vector {
    hstex_token *data;
    size_t count;
    size_t capacity;
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
        save->identifier = identifier;
        save->level = engine->group_level;
        save->previous = *destination;
        meaning.level = engine->group_level;
    } else {
        meaning.level = 0U;
    }
    *destination = meaning;
    return 0;
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
    static const struct {
        const char *name;
        enum hstex_command command;
    } primitives[] = {
        {"relax", HSTEX_COMMAND_RELAX},
        {"def", HSTEX_COMMAND_DEF},
        {"gdef", HSTEX_COMMAND_GDEF},
        {"let", HSTEX_COMMAND_LET},
        {"long", HSTEX_COMMAND_LONG},
        {"outer", HSTEX_COMMAND_OUTER},
        {"global", HSTEX_COMMAND_GLOBAL},
        {"expandafter", HSTEX_COMMAND_EXPAND_AFTER},
        {"noexpand", HSTEX_COMMAND_NO_EXPAND},
        {"begingroup", HSTEX_COMMAND_BEGIN_GROUP},
        {"endgroup", HSTEX_COMMAND_END_GROUP},
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
    struct hstex_meaning paragraph = {
        .command = HSTEX_COMMAND_PAR,
        .level = 0U,
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
    for (size_t index = 0U; index < engine->macro_count; ++index) {
        free(engine->macros[index].parameter_text);
        free(engine->macros[index].replacement);
    }
    free(engine->meanings);
    free(engine->macros);
    free(engine->saves);
    hstex_lexical_state_destroy(&engine->lexical_state);
    memset(engine, 0, sizeof(*engine));
}

int hstex_engine_push_file(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity)
{
    if (engine == NULL) {
        return set_error(error, error_capacity, "null engine file input");
    }
    return hstex_source_push_file(&engine->sources, path, error, error_capacity);
}

static enum hstex_engine_result raw_next(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity)
{
    enum hstex_mouth_result result = hstex_source_next(
        &engine->sources, token, location, error, error_capacity);
    if (result == HSTEX_MOUTH_ERROR) {
        return HSTEX_ENGINE_ERROR;
    }
    if (result == HSTEX_MOUTH_EOF) {
        return HSTEX_ENGINE_EOF;
    }
    return HSTEX_ENGINE_TOKEN;
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

static bool token_is_paragraph(const struct hstex_engine *engine,
                               hstex_token token)
{
    return hstex_token_is_control_sequence(token) &&
           hstex_token_control_sequence_id(token) ==
               engine->lexical_state.paragraph_control_sequence;
}

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

static bool vector_has_suffix(const struct token_vector *vector,
                              const hstex_token *suffix, size_t suffix_count)
{
    return suffix_count <= vector->count &&
           memcmp(vector->data + vector->count - suffix_count, suffix,
                  suffix_count * sizeof(*suffix)) == 0;
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
        if (!long_macro && token_is_paragraph(engine, token)) {
            return set_error(error, error_capacity,
                             "paragraph ended a non-long macro argument");
        }
        if (vector_push(argument, token, error, error_capacity) != 0) {
            return -1;
        }
        if (token_is_category(token, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(token, HSTEX_CAT_END_GROUP) && depth != 0U) {
            --depth;
        }
        if (depth == 0U &&
            vector_has_suffix(argument, delimiter, delimiter_count)) {
            argument->count -= delimiter_count;
            strip_single_outer_group(argument);
            return 0;
        }
    }
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
        if (result != HSTEX_ENGINE_TOKEN || actual != tokens[index]) {
            return set_error(error, error_capacity,
                             "macro invocation does not match parameter text");
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

static int expand_token_once(struct hstex_engine *engine, hstex_token token,
                             struct hstex_source_location location, char *error,
                             size_t error_capacity)
{
    if (!hstex_token_is_control_sequence(token)) {
        return push_one(engine, token, location, error, error_capacity);
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_MACRO) {
        if (meaning->value.macro_identifier == 0U ||
            (size_t)meaning->value.macro_identifier > engine->macro_count) {
            return set_error(error, error_capacity, "invalid macro meaning");
        }
        return instantiate_macro(
            engine, &engine->macros[meaning->value.macro_identifier - 1U],
            location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_EXPAND_AFTER) {
        return expand_after(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_NO_EXPAND) {
        return no_expand_once(engine, location, error, error_capacity);
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
    engine->returned_unexpanded = false;
    for (;;) {
        enum hstex_engine_result result = raw_next(
            engine, token, location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            return result;
        }
        if (hstex_token_is_frozen_control_sequence(*token)) {
            *token = hstex_token_control_sequence(
                hstex_token_control_sequence_id(*token));
            engine->returned_unexpanded = true;
            return HSTEX_ENGINE_TOKEN;
        }
        if (!hstex_token_is_control_sequence(*token)) {
            return HSTEX_ENGINE_TOKEN;
        }
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(*token));
        if (meaning->command == HSTEX_COMMAND_MACRO) {
            if (meaning->value.macro_identifier == 0U ||
                (size_t)meaning->value.macro_identifier > engine->macro_count ||
                instantiate_macro(
                    engine,
                    &engine->macros[meaning->value.macro_identifier - 1U],
                    *location, error, error_capacity) != 0) {
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
            engine->returned_unexpanded = true;
            return HSTEX_ENGINE_TOKEN;
        }
        return HSTEX_ENGINE_TOKEN;
    }
}

static int scan_definition(struct hstex_engine *engine, bool inherent_global,
                           char *error, size_t error_capacity)
{
    hstex_token target = 0U;
    struct hstex_source_location target_location;
    if (raw_next_non_space(engine, &target, &target_location, error,
                           error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(target)) {
        return set_error(error, error_capacity,
                         "def requires a control-sequence target");
    }

    struct token_vector parameter_text = {0};
    uint8_t parameter_count = 0U;
    for (;;) {
        hstex_token current = 0U;
        struct hstex_source_location location;
        if (raw_next(engine, &current, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            vector_destroy(&parameter_text);
            return set_error(error, error_capacity,
                             "end of input in macro parameter text");
        }
        if (token_is_category(current, HSTEX_CAT_BEGIN_GROUP)) {
            break;
        }
        if (token_is_category(current, HSTEX_CAT_PARAMETER)) {
            hstex_token number = 0U;
            if (raw_next(engine, &number, &location, error, error_capacity) !=
                    HSTEX_ENGINE_TOKEN ||
                !hstex_token_is_character(number) ||
                hstex_token_character_code(number) !=
                    (uint8_t)('1' + parameter_count) ||
                parameter_count >= HSTEX_MAX_PARAMETERS) {
                vector_destroy(&parameter_text);
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
        if (raw_next(engine, &current, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            vector_destroy(&parameter_text);
            vector_destroy(&replacement);
            return set_error(error, error_capacity,
                             "end of input in macro replacement text");
        }
        if (token_is_category(current, HSTEX_CAT_BEGIN_GROUP)) {
            ++depth;
        } else if (token_is_category(current, HSTEX_CAT_END_GROUP)) {
            --depth;
            if (depth == 0U) {
                break;
            }
        } else if (token_is_category(current, HSTEX_CAT_PARAMETER)) {
            hstex_token following = 0U;
            if (raw_next(engine, &following, &location, error, error_capacity) !=
                HSTEX_ENGINE_TOKEN) {
                vector_destroy(&parameter_text);
                vector_destroy(&replacement);
                return set_error(error, error_capacity,
                                 "end of input after macro parameter marker");
            }
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
    bool global = inherent_global || engine->pending_global;
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
        if (raw_next_non_space(engine, &source, &location, error,
                               error_capacity) != HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity, "end of input after let=");
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
    bool global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, hstex_token_control_sequence_id(target), meaning,
                       global, error, error_capacity);
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
        struct hstex_meaning *current =
            &engine->meanings[save.identifier - 1U];
        if (current->level == leaving_level) {
            *current = save.previous;
        }
    }
    --engine->group_level;
    return 0;
}

enum hstex_engine_result hstex_engine_next_output(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity)
{
    for (;;) {
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, token, location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            if (engine->group_level != 0U) {
                (void)set_error(error, error_capacity,
                                "end of input inside a group");
                return HSTEX_ENGINE_ERROR;
            }
            return result;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            return result;
        }
        if (engine->returned_unexpanded &&
            hstex_token_is_control_sequence(*token)) {
            continue;
        }

handle_token:
        if (hstex_token_is_character(*token)) {
            if (token_is_category(*token, HSTEX_CAT_BEGIN_GROUP)) {
                if (begin_group(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
                }
                continue;
            }
            if (token_is_category(*token, HSTEX_CAT_END_GROUP)) {
                if (end_group(engine, error, error_capacity) != 0) {
                    return HSTEX_ENGINE_ERROR;
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
            return HSTEX_ENGINE_TOKEN;
        }

        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(*token));
        switch (meaning->command) {
        case HSTEX_COMMAND_RELAX:
            engine->pending_global = false;
            engine->pending_macro_flags = 0U;
            continue;
        case HSTEX_COMMAND_DEF:
            if (scan_definition(engine, false, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_GDEF:
            if (scan_definition(engine, true, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LET:
            if (scan_let(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_LONG:
            engine->pending_macro_flags |= (uint8_t)HSTEX_MACRO_LONG;
            continue;
        case HSTEX_COMMAND_OUTER:
            engine->pending_macro_flags |= (uint8_t)HSTEX_MACRO_OUTER;
            continue;
        case HSTEX_COMMAND_GLOBAL:
            engine->pending_global = true;
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
        case HSTEX_COMMAND_TOKEN_ALIAS:
            *token = meaning->value.token;
            goto handle_token;
        case HSTEX_COMMAND_MACRO:
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_PAR:
            if (engine->pending_global || engine->pending_macro_flags != 0U) {
                (void)set_error(error, error_capacity,
                                "paragraph after definition prefix");
                return HSTEX_ENGINE_ERROR;
            }
            return HSTEX_ENGINE_TOKEN;
        case HSTEX_COMMAND_UNDEFINED:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "undefined control sequence");
        case HSTEX_COMMAND_EXPAND_AFTER:
        case HSTEX_COMMAND_NO_EXPAND:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "expandable primitive escaped expansion");
        }
    }
}
