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
    } value;
};

struct hstex_save_entry {
    hstex_cs_id identifier;
    uint32_t level;
    struct hstex_meaning previous;
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
