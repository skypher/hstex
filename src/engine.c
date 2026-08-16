#include "hstex/engine.h"

#include "hstex/catcode.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    HSTEX_INITIAL_MEANING_CAPACITY = 64,
    HSTEX_INITIAL_MACRO_CAPACITY = 32,
    HSTEX_INITIAL_TOKEN_LIST_CAPACITY = 32,
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
    engine->token_registers =
        calloc(engine->count_capacity, sizeof(*engine->token_registers));
    engine->token_register_levels =
        calloc(engine->count_capacity, sizeof(*engine->token_register_levels));
    if (engine->counts == NULL || engine->count_levels == NULL ||
        engine->dimens == NULL || engine->dimen_levels == NULL ||
        engine->glues == NULL || engine->glue_levels == NULL ||
        engine->token_registers == NULL ||
        engine->token_register_levels == NULL) {
        (void)set_error(error, error_capacity,
                        "register allocation failed");
        hstex_engine_destroy(engine);
        return -1;
    }
    engine->integer_parameters[HSTEX_INTEGER_END_LINE_CHARACTER] = 13;
    engine->integer_parameters[HSTEX_INTEGER_NEW_LINE_CHARACTER] = -1;
    engine->integer_parameters[HSTEX_INTEGER_ESCAPE_CHARACTER] = 92;
    for (size_t character = 0U; character < 256U; ++character) {
        engine->code_tables[0][character] = 1000;
        engine->code_tables[3][character] = (int32_t)character;
        engine->code_tables[4][character] = -1;
    }
    for (uint32_t character = (uint32_t)'A'; character <= (uint32_t)'Z';
         ++character) {
        engine->code_tables[0][character] = 999;
        engine->code_tables[1][character] =
            (int32_t)(character + ((uint32_t)'a' - (uint32_t)'A'));
        engine->code_tables[2][character] = (int32_t)character;
    }
    for (uint32_t character = (uint32_t)'a'; character <= (uint32_t)'z';
         ++character) {
        engine->code_tables[1][character] = (int32_t)character;
        engine->code_tables[2][character] =
            (int32_t)(character - ((uint32_t)'a' - (uint32_t)'A'));
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
        {"long", HSTEX_COMMAND_LONG},
        {"outer", HSTEX_COMMAND_OUTER},
        {"protected", HSTEX_COMMAND_PROTECTED},
        {"sfcode", HSTEX_COMMAND_SF_CODE},
        {"lccode", HSTEX_COMMAND_LC_CODE},
        {"uccode", HSTEX_COMMAND_UC_CODE},
        {"mathcode", HSTEX_COMMAND_MATH_CODE},
        {"delcode", HSTEX_COMMAND_DEL_CODE},
        {"ifdefined", HSTEX_COMMAND_IF_DEFINED},
        {"global", HSTEX_COMMAND_GLOBAL},
        {"expandafter", HSTEX_COMMAND_EXPAND_AFTER},
        {"noexpand", HSTEX_COMMAND_NO_EXPAND},
        {"begingroup", HSTEX_COMMAND_BEGIN_GROUP},
        {"endgroup", HSTEX_COMMAND_END_GROUP},
        {"catcode", HSTEX_COMMAND_CAT_CODE},
        {"chardef", HSTEX_COMMAND_CHAR_DEF},
        {"countdef", HSTEX_COMMAND_COUNT_DEF},
        {"count", HSTEX_COMMAND_COUNT},
        {"ifnum", HSTEX_COMMAND_IF_NUM},
        {"ifx", HSTEX_COMMAND_IF_X},
        {"iftrue", HSTEX_COMMAND_IF_TRUE},
        {"iffalse", HSTEX_COMMAND_IF_FALSE},
        {"else", HSTEX_COMMAND_ELSE},
        {"fi", HSTEX_COMMAND_FI},
        {"input", HSTEX_COMMAND_INPUT},
        {"end", HSTEX_COMMAND_END},
        {"endinput", HSTEX_COMMAND_END_INPUT},
        {"errmessage", HSTEX_COMMAND_ERROR_MESSAGE},
        {"advance", HSTEX_COMMAND_ADVANCE},
        {"multiply", HSTEX_COMMAND_MULTIPLY},
        {"divide", HSTEX_COMMAND_DIVIDE},
        {"the", HSTEX_COMMAND_THE},
        {"number", HSTEX_COMMAND_NUMBER},
        {"immediate", HSTEX_COMMAND_IMMEDIATE},
        {"openout", HSTEX_COMMAND_OPEN_OUT},
        {"write", HSTEX_COMMAND_WRITE},
        {"closeout", HSTEX_COMMAND_CLOSE_OUT},
        {"openin", HSTEX_COMMAND_OPEN_IN},
        {"read", HSTEX_COMMAND_READ},
        {"closein", HSTEX_COMMAND_CLOSE_IN},
        {"ifeof", HSTEX_COMMAND_IF_EOF},
        {"meaning", HSTEX_COMMAND_MEANING},
        {"string", HSTEX_COMMAND_STRING},
        {"if", HSTEX_COMMAND_IF_CHAR},
        {"inputlineno", HSTEX_COMMAND_INPUT_LINE_NUMBER},
        {"message", HSTEX_COMMAND_MESSAGE},
        {"mathchardef", HSTEX_COMMAND_MATH_CHAR_DEF},
        {"dimendef", HSTEX_COMMAND_DIMEN_DEF},
        {"skipdef", HSTEX_COMMAND_SKIP_DEF},
        {"toksdef", HSTEX_COMMAND_TOKS_DEF},
        {"dimen", HSTEX_COMMAND_DIMEN},
        {"skip", HSTEX_COMMAND_SKIP},
        {"muskip", HSTEX_COMMAND_MUSKIP},
        {"toks", HSTEX_COMMAND_TOKS},
        {"box", HSTEX_COMMAND_BOX},
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
        {"doublehyphendemerits", HSTEX_INTEGER_DOUBLE_HYPHEN_DEMERITS},
        {"finalhyphendemerits", HSTEX_INTEGER_FINAL_HYPHEN_DEMERITS},
        {"adjdemerits", HSTEX_INTEGER_ADJ_DEMERITS},
        {"tracinglostchars", HSTEX_INTEGER_TRACING_LOST_CHARS},
        {"uchyph", HSTEX_INTEGER_UC_HYPH},
        {"defaulthyphenchar", HSTEX_INTEGER_DEFAULT_HYPHEN_CHAR},
        {"defaultskewchar", HSTEX_INTEGER_DEFAULT_SKEW_CHAR},
        {"delimiterfactor", HSTEX_INTEGER_DELIMITER_FACTOR},
        {"showboxbreadth", HSTEX_INTEGER_SHOW_BOX_BREADTH},
        {"showboxdepth", HSTEX_INTEGER_SHOW_BOX_DEPTH},
        {"errorcontextlines", HSTEX_INTEGER_ERROR_CONTEXT_LINES},
        {"language", HSTEX_INTEGER_LANGUAGE},
        {"mathgroup", HSTEX_INTEGER_MATH_GROUP},
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
    free(engine->token_registers);
    free(engine->token_register_levels);
    free(engine->token_lists);
    free(engine->output_directory);
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

static enum hstex_engine_result expanded_next_non_space(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error, size_t error_capacity);

static int append_byte(uint8_t **bytes, size_t *count, size_t *capacity,
                       uint8_t byte, char *error, size_t error_capacity)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0U ? 64U : *capacity;
        if (new_capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "filename capacity overflow");
        }
        new_capacity *= 2U;
        void *allocation = realloc(*bytes, new_capacity);
        if (allocation == NULL) {
            return set_error(error, error_capacity,
                             "filename allocation failed");
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
        } else if (!braced && token_is_space(token)) {
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

static int integer_from_control_sequence(
    struct hstex_engine *engine, const struct hstex_meaning *meaning,
    int32_t *value, char *error, size_t error_capacity)
{
    switch (meaning->command) {
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

static int scan_integer(struct hstex_engine *engine, int32_t *value,
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
            if (!token_is_space(token) &&
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

struct decimal_factor {
    int sign;
    uint64_t whole;
    uint32_t fraction;
    uint32_t denominator;
};

static uint64_t greatest_common_divisor(uint64_t left, uint64_t right)
{
    while (right != 0U) {
        uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int dimen_from_meaning(struct hstex_engine *engine,
                              const struct hstex_meaning *meaning,
                              int32_t *value, char *error,
                              size_t error_capacity)
{
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
    return 0;
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
        int result = dimen_from_meaning(
            engine,
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token)),
            direct_value, error, error_capacity);
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
            } else if (fraction_digits < 9U) {
                factor->fraction = factor->fraction * 10U + digit;
                factor->denominator *= 10U;
                ++fraction_digits;
            }
        } else if (!saw_decimal && hstex_token_is_character(token) &&
                   (hstex_token_character_code(token) == (uint8_t)'.' ||
                    hstex_token_character_code(token) == (uint8_t)',')) {
            saw_decimal = true;
        } else {
            if (!token_is_space(token) &&
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
        return set_error(error, error_capacity,
                         "dimension requires a numeric factor");
    }
    return 0;
}

static int scan_unit_word(struct hstex_engine *engine, char word[16],
                          char *error, size_t error_capacity)
{
    size_t count = 0U;
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &token, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity,
                         "end of input while scanning a dimension unit");
    }
    while (hstex_token_is_character(token)) {
        uint8_t character = hstex_token_character_code(token);
        bool is_letter = (character >= (uint8_t)'a' && character <= (uint8_t)'z') ||
                         (character >= (uint8_t)'A' && character <= (uint8_t)'Z');
        if (!is_letter) {
            break;
        }
        if (count + 1U >= 16U) {
            return set_error(error, error_capacity, "dimension unit too long");
        }
        word[count++] = (char)(character >= (uint8_t)'A' &&
                                       character <= (uint8_t)'Z'
                                   ? character + ((uint8_t)'a' - (uint8_t)'A')
                                   : character);
        enum hstex_engine_result result = hstex_engine_next_expanded(
            engine, &token, &location, error, error_capacity);
        if (result == HSTEX_ENGINE_EOF) {
            token = 0U;
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            return -1;
        }
    }
    if (token != 0U && !token_is_space(token) &&
        push_one(engine, token, location, error, error_capacity) != 0) {
        return -1;
    }
    if (count == 0U) {
        return set_error(error, error_capacity, "missing dimension unit");
    }
    word[count] = '\0';
    return 0;
}

static int scaled_rational(const struct decimal_factor *factor,
                           uint64_t unit_numerator, uint64_t unit_denominator,
                           int32_t *value, char *error, size_t error_capacity)
{
    if (factor->whole >
        (UINT64_MAX - factor->fraction) / factor->denominator) {
        return set_error(error, error_capacity, "dimension factor overflow");
    }
    uint64_t numerator =
        factor->whole * factor->denominator + factor->fraction;
    uint64_t denominator = factor->denominator;
    uint64_t reduction = greatest_common_divisor(numerator, unit_denominator);
    if (reduction != 0U) {
        numerator /= reduction;
        unit_denominator /= reduction;
    }
    reduction = greatest_common_divisor(unit_numerator, denominator);
    if (reduction != 0U) {
        unit_numerator /= reduction;
        denominator /= reduction;
    }
    if (unit_numerator != 0U && numerator > UINT64_MAX / unit_numerator) {
        return set_error(error, error_capacity, "scaled dimension overflow");
    }
    numerator *= unit_numerator;
    if (unit_denominator != 0U && denominator > UINT64_MAX / unit_denominator) {
        return set_error(error, error_capacity, "dimension divisor overflow");
    }
    denominator *= unit_denominator;
    uint64_t rounded = (numerator + denominator / 2U) / denominator;
    if (rounded > UINT64_C(1073741823)) {
        return set_error(error, error_capacity,
                         "dimension exceeds TeX's maximum");
    }
    int64_t signed_value = factor->sign < 0 ? -(int64_t)rounded
                                            : (int64_t)rounded;
    *value = (int32_t)signed_value;
    return 0;
}

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
    if (direct_dimen) {
        *value = direct_value;
        *order = 0U;
        return 0;
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
            uint64_t magnitude = internal_unit < 0
                                     ? (uint64_t)(-(int64_t)internal_unit)
                                     : (uint64_t)internal_unit;
            if (internal_unit < 0) {
                factor.sign = -factor.sign;
            }
            *order = 0U;
            return scaled_rational(&factor, magnitude, 1U, value, error,
                                   error_capacity);
        }
    }
    if (push_one(engine, possible_unit, possible_unit_location, error,
                 error_capacity) != 0) {
        return -1;
    }
    char unit[16];
    if (scan_unit_word(engine, unit, error, error_capacity) != 0) {
        return -1;
    }
    uint64_t numerator = 0U;
    uint64_t denominator = 1U;
    *order = 0U;
    if (strcmp(unit, "pt") == 0 || strcmp(unit, "truept") == 0) {
        numerator = UINT64_C(65536);
    } else if (strcmp(unit, "sp") == 0) {
        numerator = 1U;
    } else if (strcmp(unit, "pc") == 0) {
        numerator = UINT64_C(12) * UINT64_C(65536);
    } else if (strcmp(unit, "in") == 0) {
        numerator = UINT64_C(7227) * UINT64_C(65536);
        denominator = 100U;
    } else if (strcmp(unit, "bp") == 0) {
        numerator = UINT64_C(7227) * UINT64_C(65536);
        denominator = 7200U;
    } else if (strcmp(unit, "cm") == 0) {
        numerator = UINT64_C(7227) * UINT64_C(65536);
        denominator = 254U;
    } else if (strcmp(unit, "mm") == 0) {
        numerator = UINT64_C(7227) * UINT64_C(65536);
        denominator = 2540U;
    } else if (strcmp(unit, "dd") == 0) {
        numerator = UINT64_C(1238) * UINT64_C(65536);
        denominator = 1157U;
    } else if (strcmp(unit, "cc") == 0) {
        numerator = UINT64_C(12) * UINT64_C(1238) * UINT64_C(65536);
        denominator = 1157U;
    } else if (allow_fil && strcmp(unit, "fil") == 0) {
        numerator = UINT64_C(65536);
        *order = 1U;
    } else if (allow_fil && strcmp(unit, "fill") == 0) {
        numerator = UINT64_C(65536);
        *order = 2U;
    } else if (allow_fil && strcmp(unit, "filll") == 0) {
        numerator = UINT64_C(65536);
        *order = 3U;
    } else {
        return set_error(error, error_capacity,
                         "unsupported dimension unit: %s", unit);
    }
    return scaled_rational(&factor, numerator, denominator, value, error,
                           error_capacity);
}

static int scan_dimension(struct hstex_engine *engine, int32_t *value,
                          char *error, size_t error_capacity)
{
    uint8_t order = 0U;
    return scan_dimension_component(engine, false, value, &order, error,
                                    error_capacity);
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

static int scan_glue(struct hstex_engine *engine, struct hstex_glue *glue,
                     char *error, size_t error_capacity)
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
        uint32_t identifier = 0U;
        int result = token_list_identifier_from_meaning(
            engine,
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(subject)),
            &identifier, error, error_capacity);
        if (result < 0) {
            return -1;
        }
        if (result > 0) {
            return push_token_list_expansion(engine, identifier, location,
                                             error, error_capacity);
        }
    }
    if (push_one(engine, subject, subject_location, error, error_capacity) != 0) {
        return -1;
    }
    return expand_integer_primitive(engine, location, error, error_capacity);
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
static int scan_if_num(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_x(struct hstex_engine *engine, char *error,
                     size_t error_capacity);
static int scan_if_char(struct hstex_engine *engine, char *error,
                        size_t error_capacity);
static int scan_if_eof(struct hstex_engine *engine, char *error,
                       size_t error_capacity);
static int scan_if_defined(struct hstex_engine *engine, char *error,
                           size_t error_capacity);
static int start_conditional(struct hstex_engine *engine, bool condition,
                             char *error, size_t error_capacity);
static int skip_conditional(struct hstex_engine *engine, bool stop_at_else,
                            char *error, size_t error_capacity);
static int execute_else(struct hstex_engine *engine, char *error,
                        size_t error_capacity);
static int execute_fi(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
static int expand_meaning(struct hstex_engine *engine,
                          struct hstex_source_location location, char *error,
                          size_t error_capacity);
static int expand_string(struct hstex_engine *engine,
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
    if (meaning->command == HSTEX_COMMAND_THE) {
        return expand_the_primitive(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_NUMBER) {
        return expand_integer_primitive(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_MEANING) {
        return expand_meaning(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_STRING) {
        return expand_string(engine, location, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_NUM) {
        return scan_if_num(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_X) {
        return scan_if_x(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_CHAR) {
        return scan_if_char(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_EOF) {
        return scan_if_eof(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_DEFINED) {
        return scan_if_defined(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_IF_TRUE ||
        meaning->command == HSTEX_COMMAND_IF_FALSE) {
        return start_conditional(engine,
                                 meaning->command == HSTEX_COMMAND_IF_TRUE,
                                 error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_ELSE) {
        return execute_else(engine, error, error_capacity);
    }
    if (meaning->command == HSTEX_COMMAND_FI) {
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
    engine->returned_unexpanded = false;
    engine->returned_unexpanded_executable = false;
    for (;;) {
        enum hstex_engine_result result = raw_next(
            engine, token, location, error, error_capacity);
        if (result != HSTEX_ENGINE_TOKEN) {
            return result;
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
                return HSTEX_ENGINE_ERROR;
            }
            const struct hstex_macro *macro =
                &engine->macros[meaning->value.macro_identifier - 1U];
            if (engine->inhibit_protected_expansion &&
                (macro->flags & (uint8_t)HSTEX_MACRO_PROTECTED) != 0U) {
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
            engine->returned_unexpanded = true;
            engine->returned_unexpanded_executable = false;
            return HSTEX_ENGINE_TOKEN;
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
        if (meaning->command == HSTEX_COMMAND_IF_NUM) {
            if (scan_if_num(engine, error, error_capacity) != 0) {
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
        if (meaning->command == HSTEX_COMMAND_IF_TRUE ||
            meaning->command == HSTEX_COMMAND_IF_FALSE) {
            if (start_conditional(
                    engine, meaning->command == HSTEX_COMMAND_IF_TRUE, error,
                    error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_ELSE) {
            if (execute_else(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        }
        if (meaning->command == HSTEX_COMMAND_FI) {
            if (execute_fi(engine, error, error_capacity) != 0) {
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
        if (result != HSTEX_ENGINE_TOKEN) {
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
    bool global = assignment_is_global(engine, engine->pending_global);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return set_meaning(engine, hstex_token_control_sequence_id(target), meaning,
                       global, error, error_capacity);
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
                         "second else in one conditional");
    }
    conditional->else_seen = true;
    return skip_conditional(engine, false, error, error_capacity);
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
    if (index < 0 || (size_t)index >= engine->count_capacity ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity, "invalid count assignment");
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
        parameter >= (int32_t)HSTEX_INTEGER_PARAMETER_COUNT ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid integer-parameter assignment");
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
    if (index < 0 || (size_t)index >= engine->count_capacity ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity, "invalid dimen assignment");
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
    if (index < 0 || (size_t)index >= engine->count_capacity ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_glue(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity, "invalid glue assignment");
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

static int scan_dimen_parameter_assignment(struct hstex_engine *engine,
                                           int32_t parameter, char *error,
                                           size_t error_capacity)
{
    int32_t value = 0;
    if (parameter < 0 || parameter >= (int32_t)HSTEX_DIMEN_PARAMETER_COUNT ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_dimension(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid dimen-parameter assignment");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_dimen_parameter(engine, (uint32_t)parameter, value,
                                  requested_global, error, error_capacity);
}

static int scan_glue_parameter_assignment(struct hstex_engine *engine,
                                          int32_t parameter, char *error,
                                          size_t error_capacity)
{
    struct hstex_glue value;
    if (parameter < 0 || parameter >= (int32_t)HSTEX_GLUE_PARAMETER_COUNT ||
        scan_optional_equals(engine, error, error_capacity) != 0 ||
        scan_glue(engine, &value, error, error_capacity) != 0) {
        return set_error(error, error_capacity,
                         "invalid glue-parameter assignment");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return assign_glue_parameter(engine, (uint32_t)parameter, value,
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

enum integer_variable_kind {
    INTEGER_VARIABLE_COUNT = 0,
    INTEGER_VARIABLE_PARAMETER,
};

struct integer_variable {
    enum integer_variable_kind kind;
    uint32_t index;
};

static int scan_integer_variable(struct hstex_engine *engine,
                                 struct integer_variable *variable,
                                 char *error, size_t error_capacity)
{
    hstex_token token = 0U;
    struct hstex_source_location location;
    if (expanded_next_non_space(engine, &token, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN ||
        !hstex_token_is_control_sequence(token)) {
        return set_error(error, error_capacity,
                         "arithmetic operation requires an integer variable");
    }
    const struct hstex_meaning *meaning = hstex_engine_meaning(
        engine, hstex_token_control_sequence_id(token));
    if (meaning->command == HSTEX_COMMAND_COUNT_REGISTER) {
        if (meaning->value.integer < 0 ||
            (size_t)meaning->value.integer >= engine->count_capacity) {
            return set_error(error, error_capacity,
                             "invalid count-register variable");
        }
        variable->kind = INTEGER_VARIABLE_COUNT;
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
        variable->kind = INTEGER_VARIABLE_COUNT;
        variable->index = (uint32_t)index;
        return 0;
    }
    if (meaning->command == HSTEX_COMMAND_INTEGER_PARAMETER &&
        meaning->value.integer >= 0 &&
        meaning->value.integer < (int32_t)HSTEX_INTEGER_PARAMETER_COUNT) {
        variable->kind = INTEGER_VARIABLE_PARAMETER;
        variable->index = (uint32_t)meaning->value.integer;
        return 0;
    }
    return set_error(error, error_capacity,
                     "arithmetic target is not an integer variable");
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

static int execute_arithmetic(struct hstex_engine *engine,
                              enum hstex_command operation, char *error,
                              size_t error_capacity)
{
    struct integer_variable variable = {0};
    int32_t operand = 0;
    if (scan_integer_variable(engine, &variable, error, error_capacity) != 0 ||
        scan_optional_by(engine, error, error_capacity) != 0 ||
        scan_integer(engine, &operand, error, error_capacity) != 0) {
        return -1;
    }
    int32_t current = variable.kind == INTEGER_VARIABLE_COUNT
                          ? engine->counts[variable.index]
                          : engine->integer_parameters[variable.index];
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
    if (result < INT32_MIN || result > INT32_MAX) {
        return set_error(error, error_capacity, "arithmetic overflow");
    }
    bool requested_global = engine->pending_global;
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    if (variable.kind == INTEGER_VARIABLE_COUNT) {
        return assign_count(engine, variable.index, (int32_t)result,
                            requested_global, error, error_capacity);
    }
    return assign_integer_parameter(engine, variable.index, (int32_t)result,
                                    requested_global, error, error_capacity);
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
    if (kind == HSTEX_SYMBOL_REGULAR && length > 1U &&
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
    if (hstex_token_is_character(token)) {
        return append_byte(bytes, count, capacity,
                           hstex_token_character_code(token), error,
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
                                          error, error_capacity);
    }
    return set_error(error, error_capacity,
                     "internal token in meaning description");
}

static int append_string_character(uint8_t **bytes, size_t *count,
                                   size_t *capacity, uint8_t character,
                                   char *error, size_t error_capacity)
{
    if (character >= UINT8_C(32) && character <= UINT8_C(126)) {
        return append_byte(bytes, count, capacity, character, error,
                           error_capacity);
    }
    if (append_byte(bytes, count, capacity, (uint8_t)'^', error,
                    error_capacity) != 0 ||
        append_byte(bytes, count, capacity, (uint8_t)'^', error,
                    error_capacity) != 0) {
        return -1;
    }
    if (character < UINT8_C(128)) {
        uint8_t visible = character < UINT8_C(64)
                              ? (uint8_t)(character + UINT8_C(64))
                              : (uint8_t)(character - UINT8_C(64));
        return append_byte(bytes, count, capacity, visible, error,
                           error_capacity);
    }
    static const uint8_t hexadecimal[] = "0123456789abcdef";
    if (append_byte(bytes, count, capacity, hexadecimal[character >> 4U], error,
                    error_capacity) != 0) {
        return -1;
    }
    return append_byte(bytes, count, capacity,
                       hexadecimal[character & UINT8_C(0x0f)], error,
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
        if (append_text_bytes(&bytes, &count, &capacity, "the character ", error,
                              error_capacity) != 0 ||
            append_token_description(engine, subject, &bytes, &count, &capacity,
                                     error, error_capacity) != 0) {
            free(bytes);
            return -1;
        }
    } else if (hstex_token_is_control_sequence(subject)) {
        const struct hstex_meaning *meaning = hstex_engine_meaning(
            engine, hstex_token_control_sequence_id(subject));
        if (meaning->command == HSTEX_COMMAND_MACRO &&
            meaning->value.macro_identifier != 0U &&
            (size_t)meaning->value.macro_identifier <= engine->macro_count) {
            const struct hstex_macro *macro =
                &engine->macros[meaning->value.macro_identifier - 1U];
            if ((macro->flags & (uint8_t)HSTEX_MACRO_LONG) != 0U &&
                append_text_bytes(&bytes, &count, &capacity, "long ", error,
                                  error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            if ((macro->flags & (uint8_t)HSTEX_MACRO_OUTER) != 0U &&
                append_text_bytes(&bytes, &count, &capacity, "outer ", error,
                                  error_capacity) != 0) {
                free(bytes);
                return -1;
            }
            if ((macro->flags & (uint8_t)HSTEX_MACRO_PROTECTED) != 0U &&
                append_text_bytes(&bytes, &count, &capacity, "protected ",
                                  error, error_capacity) != 0) {
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
        } else if (meaning->command == HSTEX_COMMAND_UNDEFINED) {
            if (append_text_bytes(&bytes, &count, &capacity, "undefined", error,
                                  error_capacity) != 0) {
                free(bytes);
                return -1;
            }
        } else if (serialize_control_sequence(engine, subject, &bytes, &count,
                                              &capacity, error,
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
    if (raw_next_non_space(engine, &opening, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        !token_is_category(opening, HSTEX_CAT_BEGIN_GROUP)) {
        return set_error(error, error_capacity,
                         "write requires a braced token list");
    }
    struct token_vector text = {0};
    if (scan_balanced_group(engine, &text, true, error, error_capacity) != 0 ||
        vector_push(&text, hstex_token_frozen_control_sequence(0U), error,
                    error_capacity) != 0 ||
        push_owned_vector(engine, &text, location, error, error_capacity) != 0) {
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
        if (expansion_result != HSTEX_ENGINE_TOKEN) {
            free(result);
            return set_error(error, error_capacity,
                             "end of input while expanding write text");
        }
        if (engine->returned_unexpanded &&
            hstex_token_is_control_sequence(token) &&
            hstex_token_control_sequence_id(token) == 0U) {
            break;
        }
        if (hstex_token_is_character(token)) {
            if (append_byte(&result, &count, &capacity,
                            hstex_token_character_code(token), error,
                            error_capacity) != 0) {
                free(result);
                return -1;
            }
        } else if (hstex_token_is_control_sequence(token)) {
            if (serialize_control_sequence(engine, token, &result, &count,
                                           &capacity, error,
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

static int execute_write(struct hstex_engine *engine, char *error,
                         size_t error_capacity)
{
    int32_t stream = 0;
    uint8_t *bytes = NULL;
    size_t byte_count = 0U;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0 ||
        scan_expanded_general_text(engine, &bytes, &byte_count, error,
                                   error_capacity) != 0) {
        free(bytes);
        return -1;
    }
    FILE *destination = NULL;
    if (stream >= 0 && stream < 16) {
        destination = engine->write_streams[(size_t)stream];
    } else if (stream == -1 || stream == 17) {
        destination = stdout;
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
    if (replacement.count != 0U &&
        token_is_space(replacement.data[replacement.count - 1U])) {
        --replacement.count;
    }
    if (reserve_macros(engine, engine->macro_count + 1U, error,
                       error_capacity) != 0) {
        vector_destroy(&replacement);
        return -1;
    }
    struct hstex_macro *macro = &engine->macros[engine->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->replacement = replacement.data;
    macro->replacement_count = replacement.count;
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

static int execute_read(struct hstex_engine *engine, char *error,
                        size_t error_capacity)
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
    int status = define_read_line(engine, target, (const uint8_t *)line,
                                  (size_t)length, error, error_capacity);
    free(line);
    engine->pending_global = false;
    engine->pending_macro_flags = 0U;
    return status;
}

static int scan_if_eof(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    int32_t stream = 0;
    if (scan_stream_number(engine, &stream, error, error_capacity) != 0) {
        return -1;
    }
    bool at_end = stream < 0 || stream >= 16 ||
                  engine->read_streams[(size_t)stream] == NULL ||
                  feof(engine->read_streams[(size_t)stream]) != 0;
    return start_conditional(engine, at_end, error, error_capacity);
}

static int scan_if_defined(struct hstex_engine *engine, char *error,
                           size_t error_capacity)
{
    hstex_token subject = 0U;
    struct hstex_source_location location;
    if (raw_next(engine, &subject, &location, error, error_capacity) !=
        HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in ifdefined");
    }
    bool defined = hstex_token_is_control_sequence(subject) &&
                   hstex_engine_meaning(
                       engine, hstex_token_control_sequence_id(subject))
                           ->command != HSTEX_COMMAND_UNDEFINED;
    return start_conditional(engine, defined, error, error_capacity);
}

static bool command_starts_conditional(enum hstex_command command)
{
    return command == HSTEX_COMMAND_IF_NUM || command == HSTEX_COMMAND_IF_X ||
           command == HSTEX_COMMAND_IF_CHAR ||
           command == HSTEX_COMMAND_IF_TRUE ||
           command == HSTEX_COMMAND_IF_FALSE ||
           command == HSTEX_COMMAND_IF_EOF ||
           command == HSTEX_COMMAND_IF_DEFINED;
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
    case HSTEX_COMMAND_CHAR_GIVEN:
    case HSTEX_COMMAND_MATH_CHAR_GIVEN:
    case HSTEX_COMMAND_COUNT_REGISTER:
    case HSTEX_COMMAND_INTEGER_PARAMETER:
    case HSTEX_COMMAND_DIMEN_REGISTER:
    case HSTEX_COMMAND_SKIP_REGISTER:
    case HSTEX_COMMAND_TOKS_REGISTER:
    case HSTEX_COMMAND_DIMEN_PARAMETER:
    case HSTEX_COMMAND_GLUE_PARAMETER:
    case HSTEX_COMMAND_TOKEN_PARAMETER:
        return left->value.integer == right->value.integer;
    default:
        return true;
    }
}

static bool ifx_tokens_equal(const struct hstex_engine *engine,
                             hstex_token left, hstex_token right)
{
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

static int skip_conditional(struct hstex_engine *engine, bool stop_at_else,
                            char *error, size_t error_capacity)
{
    size_t depth = 0U;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        if (raw_next(engine, &token, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
            return set_error(error, error_capacity,
                             "end of input while skipping a conditional");
        }
        if (!hstex_token_is_control_sequence(token)) {
            continue;
        }
        enum hstex_command command =
            hstex_engine_meaning(engine,
                                 hstex_token_control_sequence_id(token))
                ->command;
        if (command_starts_conditional(command)) {
            ++depth;
        } else if (command == HSTEX_COMMAND_FI) {
            if (depth != 0U) {
                --depth;
                continue;
            }
            if (engine->conditional_count == 0U) {
                return set_error(error, error_capacity,
                                 "conditional stack underflow");
            }
            --engine->conditional_count;
            return 0;
        } else if (command == HSTEX_COMMAND_ELSE && depth == 0U &&
                   stop_at_else) {
            struct hstex_conditional *conditional =
                &engine->conditionals[engine->conditional_count - 1U];
            conditional->else_seen = true;
            return 0;
        }
    }
}

static int start_conditional(struct hstex_engine *engine, bool condition,
                             char *error, size_t error_capacity)
{
    if (reserve_conditionals(engine, engine->conditional_count + 1U, error,
                             error_capacity) != 0) {
        return -1;
    }
    struct hstex_conditional *entry =
        &engine->conditionals[engine->conditional_count++];
    entry->branch_true = condition;
    entry->else_seen = false;
    if (!condition) {
        return skip_conditional(engine, true, error, error_capacity);
    }
    return 0;
}

static int scan_if_num(struct hstex_engine *engine, char *error,
                       size_t error_capacity)
{
    int32_t left = 0;
    int32_t right = 0;
    hstex_token relation = 0U;
    struct hstex_source_location location;
    if (scan_integer(engine, &left, error, error_capacity) != 0 ||
        expanded_next_non_space(engine, &relation, &location, error,
                                error_capacity) != HSTEX_ENGINE_TOKEN ||
        scan_integer(engine, &right, error, error_capacity) != 0) {
        return set_error(error, error_capacity, "invalid ifnum comparison");
    }
    bool condition;
    if (token_is_other_character(relation, (uint8_t)'<')) {
        condition = left < right;
    } else if (token_is_other_character(relation, (uint8_t)'=')) {
        condition = left == right;
    } else if (token_is_other_character(relation, (uint8_t)'>')) {
        condition = left > right;
    } else {
        return set_error(error, error_capacity,
                         "ifnum requires <, =, or >");
    }
    return start_conditional(engine, condition, error, error_capacity);
}

static int scan_if_x(struct hstex_engine *engine, char *error,
                     size_t error_capacity)
{
    hstex_token left = 0U;
    hstex_token right = 0U;
    struct hstex_source_location location;
    if (raw_next(engine, &left, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN ||
        raw_next(engine, &right, &location, error, error_capacity) !=
            HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in ifx");
    }
    return start_conditional(engine, ifx_tokens_equal(engine, left, right),
                             error, error_capacity);
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
    hstex_token left = 0U;
    hstex_token right = 0U;
    struct hstex_source_location location;
    if (hstex_engine_next_expanded(engine, &left, &location, error,
                                   error_capacity) != HSTEX_ENGINE_TOKEN ||
        hstex_engine_next_expanded(engine, &right, &location, error,
                                   error_capacity) != HSTEX_ENGINE_TOKEN) {
        return set_error(error, error_capacity, "end of input in if");
    }
    return start_conditional(
        engine, if_character_code(engine, left) == if_character_code(engine, right),
        error, error_capacity);
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
            if (engine->conditional_count != 0U) {
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
            if (scan_definition(engine, false, false, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_GDEF:
            if (scan_definition(engine, true, false, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_EDEF:
            if (scan_definition(engine, false, true, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_XDEF:
            if (scan_definition(engine, true, true, error, error_capacity) !=
                0) {
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
        case HSTEX_COMMAND_PROTECTED:
            engine->pending_macro_flags |= (uint8_t)HSTEX_MACRO_PROTECTED;
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
        case HSTEX_COMMAND_CAT_CODE:
            if (scan_catcode_assignment(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SF_CODE:
        case HSTEX_COMMAND_LC_CODE:
        case HSTEX_COMMAND_UC_CODE:
        case HSTEX_COMMAND_MATH_CODE:
        case HSTEX_COMMAND_DEL_CODE:
            if (scan_code_assignment(engine, meaning->command, error,
                                     error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_CHAR_DEF:
            if (scan_char_definition(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_MATH_CHAR_DEF:
            if (scan_math_char_definition(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_COUNT_DEF:
            if (scan_count_definition(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN_DEF:
            if (scan_register_definition(engine, HSTEX_COMMAND_DIMEN_REGISTER,
                                         error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SKIP_DEF:
            if (scan_register_definition(engine, HSTEX_COMMAND_SKIP_REGISTER,
                                         error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKS_DEF:
            if (scan_register_definition(engine, HSTEX_COMMAND_TOKS_REGISTER,
                                         error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKS_REGISTER:
            if (scan_token_register_assignment(
                    engine, meaning->value.integer, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKS:
            if (scan_token_family_assignment(engine, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_TOKEN_PARAMETER:
            if (scan_token_parameter_assignment(
                    engine, meaning->value.integer, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_COUNT:
            if (scan_count_family_assignment(engine, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_COUNT_REGISTER:
            if (scan_count_assignment(engine, meaning->value.integer, error,
                                      error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_INTEGER_PARAMETER:
            if (scan_integer_parameter_assignment(
                    engine, meaning->value.integer, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN_REGISTER:
            if (scan_dimen_assignment(engine, meaning->value.integer, error,
                                      error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN:
            if (scan_dimen_family_assignment(engine, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SKIP_REGISTER:
            if (scan_glue_assignment(engine, meaning->value.integer, error,
                                     error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_SKIP:
        case HSTEX_COMMAND_MUSKIP:
            if (scan_glue_family_assignment(engine, error, error_capacity) !=
                0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_DIMEN_PARAMETER:
            if (scan_dimen_parameter_assignment(
                    engine, meaning->value.integer, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_GLUE_PARAMETER:
            if (scan_glue_parameter_assignment(
                    engine, meaning->value.integer, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_ADVANCE:
        case HSTEX_COMMAND_MULTIPLY:
        case HSTEX_COMMAND_DIVIDE:
            if (execute_arithmetic(engine, meaning->command, error,
                                   error_capacity) != 0) {
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
            if (execute_read(engine, error, error_capacity) != 0) {
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
        case HSTEX_COMMAND_THE:
        case HSTEX_COMMAND_NUMBER:
        case HSTEX_COMMAND_MEANING:
        case HSTEX_COMMAND_STRING:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "expandable primitive escaped expansion");
        case HSTEX_COMMAND_INPUT:
            if (execute_input(engine, error, error_capacity) != 0) {
                return HSTEX_ENGINE_ERROR;
            }
            continue;
        case HSTEX_COMMAND_END:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "end primitive is not implemented");
        case HSTEX_COMMAND_END_INPUT:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "endinput primitive is not implemented");
        case HSTEX_COMMAND_ERROR_MESSAGE:
            return (enum hstex_engine_result)set_error(
                error, error_capacity, "errmessage primitive was executed");
        case HSTEX_COMMAND_INPUT_LINE_NUMBER:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "inputlineno used outside an integer context");
        case HSTEX_COMMAND_BOX:
        case HSTEX_COMMAND_MATH_GROUP:
        case HSTEX_COMMAND_LANGUAGE:
            return (enum hstex_engine_result)set_error(
                error, error_capacity,
                "non-integer register execution is not implemented");
        }
    }
}
