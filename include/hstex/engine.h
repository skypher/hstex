#ifndef HSTEX_ENGINE_H
#define HSTEX_ENGINE_H

#include "hstex/lex.h"
#include "hstex/source.h"
#include "hstex/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hstex_engine_result {
    HSTEX_ENGINE_ERROR = -1,
    HSTEX_ENGINE_EOF = 0,
    HSTEX_ENGINE_TOKEN = 1,
};

enum hstex_command {
    HSTEX_COMMAND_UNDEFINED = 0,
    HSTEX_COMMAND_RELAX,
    HSTEX_COMMAND_MACRO,
    HSTEX_COMMAND_TOKEN_ALIAS,
    HSTEX_COMMAND_PAR,
    HSTEX_COMMAND_DEF,
    HSTEX_COMMAND_GDEF,
    HSTEX_COMMAND_LET,
    HSTEX_COMMAND_LONG,
    HSTEX_COMMAND_OUTER,
    HSTEX_COMMAND_GLOBAL,
    HSTEX_COMMAND_EXPAND_AFTER,
    HSTEX_COMMAND_NO_EXPAND,
    HSTEX_COMMAND_BEGIN_GROUP,
    HSTEX_COMMAND_END_GROUP,
    HSTEX_COMMAND_CAT_CODE,
    HSTEX_COMMAND_CHAR_DEF,
    HSTEX_COMMAND_CHAR_GIVEN,
    HSTEX_COMMAND_COUNT_DEF,
    HSTEX_COMMAND_COUNT,
    HSTEX_COMMAND_COUNT_REGISTER,
    HSTEX_COMMAND_INTEGER_PARAMETER,
    HSTEX_COMMAND_IF_NUM,
    HSTEX_COMMAND_IF_X,
    HSTEX_COMMAND_IF_TRUE,
    HSTEX_COMMAND_IF_FALSE,
    HSTEX_COMMAND_ELSE,
    HSTEX_COMMAND_FI,
    HSTEX_COMMAND_INPUT,
    HSTEX_COMMAND_END,
    HSTEX_COMMAND_END_INPUT,
    HSTEX_COMMAND_ERROR_MESSAGE,
};

enum hstex_integer_parameter {
    HSTEX_INTEGER_END_LINE_CHARACTER = 0,
    HSTEX_INTEGER_NEW_LINE_CHARACTER,
    HSTEX_INTEGER_ESCAPE_CHARACTER,
    HSTEX_INTEGER_GLOBAL_DEFS,
    HSTEX_INTEGER_TIME,
    HSTEX_INTEGER_DAY,
    HSTEX_INTEGER_MONTH,
    HSTEX_INTEGER_YEAR,
    HSTEX_INTEGER_PARAMETER_COUNT,
};

enum hstex_macro_flag {
    HSTEX_MACRO_LONG = 1U << 0U,
    HSTEX_MACRO_OUTER = 1U << 1U,
};

struct hstex_macro {
    hstex_token *parameter_text;
    size_t parameter_count_tokens;
    hstex_token *replacement;
    size_t replacement_count;
    uint8_t parameter_count;
    uint8_t flags;
};

struct hstex_meaning {
    enum hstex_command command;
    uint32_t level;
    union {
        uint32_t macro_identifier;
        hstex_token token;
        int32_t integer;
    } value;
};

enum hstex_save_kind {
    HSTEX_SAVE_MEANING = 0,
    HSTEX_SAVE_CAT_CODE,
    HSTEX_SAVE_COUNT,
    HSTEX_SAVE_INTEGER_PARAMETER,
};

struct hstex_save_entry {
    enum hstex_save_kind kind;
    uint32_t index;
    uint32_t level;
    uint32_t previous_level;
    union {
        struct hstex_meaning meaning;
        int32_t integer;
        uint8_t category;
    } previous;
};

struct hstex_conditional {
    bool branch_true;
    bool else_seen;
};

struct hstex_engine {
    struct hstex_lexical_state lexical_state;
    struct hstex_source_stack sources;
    struct hstex_meaning *meanings;
    size_t meaning_capacity;
    struct hstex_macro *macros;
    size_t macro_count;
    size_t macro_capacity;
    struct hstex_save_entry *saves;
    size_t save_count;
    size_t save_capacity;
    struct hstex_conditional *conditionals;
    size_t conditional_count;
    size_t conditional_capacity;
    int32_t *counts;
    uint32_t *count_levels;
    size_t count_capacity;
    int32_t integer_parameters[HSTEX_INTEGER_PARAMETER_COUNT];
    uint32_t integer_parameter_levels[HSTEX_INTEGER_PARAMETER_COUNT];
    uint32_t catcode_levels[256];
    uint32_t group_level;
    uint8_t pending_macro_flags;
    bool pending_global;
    bool returned_unexpanded;
};

int hstex_engine_init(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
void hstex_engine_destroy(struct hstex_engine *engine);
int hstex_engine_push_file(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity);
enum hstex_engine_result hstex_engine_next_expanded(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity);
enum hstex_engine_result hstex_engine_next_output(
    struct hstex_engine *engine, hstex_token *token,
    struct hstex_source_location *location, char *error,
    size_t error_capacity);
const struct hstex_meaning *hstex_engine_meaning(
    const struct hstex_engine *engine, hstex_cs_id identifier);

#endif
