#ifndef HSTEX_ENGINE_H
#define HSTEX_ENGINE_H

#include "hstex/lex.h"
#include "hstex/source.h"
#include "hstex/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
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
    HSTEX_COMMAND_EDEF,
    HSTEX_COMMAND_XDEF,
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
    HSTEX_COMMAND_ADVANCE,
    HSTEX_COMMAND_MULTIPLY,
    HSTEX_COMMAND_DIVIDE,
    HSTEX_COMMAND_THE,
    HSTEX_COMMAND_NUMBER,
    HSTEX_COMMAND_IMMEDIATE,
    HSTEX_COMMAND_OPEN_OUT,
    HSTEX_COMMAND_WRITE,
    HSTEX_COMMAND_CLOSE_OUT,
    HSTEX_COMMAND_OPEN_IN,
    HSTEX_COMMAND_READ,
    HSTEX_COMMAND_CLOSE_IN,
    HSTEX_COMMAND_IF_EOF,
    HSTEX_COMMAND_MEANING,
    HSTEX_COMMAND_STRING,
    HSTEX_COMMAND_IF_CHAR,
    HSTEX_COMMAND_INPUT_LINE_NUMBER,
    HSTEX_COMMAND_MESSAGE,
    HSTEX_COMMAND_MATH_CHAR_DEF,
    HSTEX_COMMAND_MATH_CHAR_GIVEN,
    HSTEX_COMMAND_DIMEN_DEF,
    HSTEX_COMMAND_DIMEN_REGISTER,
    HSTEX_COMMAND_SKIP_DEF,
    HSTEX_COMMAND_SKIP_REGISTER,
    HSTEX_COMMAND_TOKS_DEF,
    HSTEX_COMMAND_TOKS_REGISTER,
    HSTEX_COMMAND_DIMEN,
    HSTEX_COMMAND_SKIP,
    HSTEX_COMMAND_MUSKIP,
    HSTEX_COMMAND_TOKS,
    HSTEX_COMMAND_BOX,
    HSTEX_COMMAND_MATH_GROUP,
    HSTEX_COMMAND_LANGUAGE,
    HSTEX_COMMAND_DIMEN_PARAMETER,
    HSTEX_COMMAND_GLUE_PARAMETER,
    HSTEX_COMMAND_PROTECTED,
    HSTEX_COMMAND_SF_CODE,
    HSTEX_COMMAND_LC_CODE,
    HSTEX_COMMAND_UC_CODE,
    HSTEX_COMMAND_MATH_CODE,
    HSTEX_COMMAND_DEL_CODE,
    HSTEX_COMMAND_IF_DEFINED,
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
    HSTEX_INTEGER_PRETOLERANCE,
    HSTEX_INTEGER_TOLERANCE,
    HSTEX_INTEGER_HBADNESS,
    HSTEX_INTEGER_VBADNESS,
    HSTEX_INTEGER_LINE_PENALTY,
    HSTEX_INTEGER_HYPHEN_PENALTY,
    HSTEX_INTEGER_EX_HYPHEN_PENALTY,
    HSTEX_INTEGER_BIN_OP_PENALTY,
    HSTEX_INTEGER_REL_PENALTY,
    HSTEX_INTEGER_CLUB_PENALTY,
    HSTEX_INTEGER_WIDOW_PENALTY,
    HSTEX_INTEGER_DISPLAY_WIDOW_PENALTY,
    HSTEX_INTEGER_BROKEN_PENALTY,
    HSTEX_INTEGER_PRE_DISPLAY_PENALTY,
    HSTEX_INTEGER_DOUBLE_HYPHEN_DEMERITS,
    HSTEX_INTEGER_FINAL_HYPHEN_DEMERITS,
    HSTEX_INTEGER_ADJ_DEMERITS,
    HSTEX_INTEGER_TRACING_LOST_CHARS,
    HSTEX_INTEGER_UC_HYPH,
    HSTEX_INTEGER_DEFAULT_HYPHEN_CHAR,
    HSTEX_INTEGER_DEFAULT_SKEW_CHAR,
    HSTEX_INTEGER_DELIMITER_FACTOR,
    HSTEX_INTEGER_SHOW_BOX_BREADTH,
    HSTEX_INTEGER_SHOW_BOX_DEPTH,
    HSTEX_INTEGER_ERROR_CONTEXT_LINES,
    HSTEX_INTEGER_LANGUAGE,
    HSTEX_INTEGER_MATH_GROUP,
    HSTEX_INTEGER_PARAMETER_COUNT,
};

enum hstex_dimen_parameter {
    HSTEX_DIMEN_HFUZZ = 0,
    HSTEX_DIMEN_VFUZZ,
    HSTEX_DIMEN_OVERFULL_RULE,
    HSTEX_DIMEN_MAX_DEPTH,
    HSTEX_DIMEN_SPLIT_MAX_DEPTH,
    HSTEX_DIMEN_BOX_MAX_DEPTH,
    HSTEX_DIMEN_DELIMITER_SHORTFALL,
    HSTEX_DIMEN_NULL_DELIMITER_SPACE,
    HSTEX_DIMEN_SCRIPT_SPACE,
    HSTEX_DIMEN_PAR_INDENT,
    HSTEX_DIMEN_HSIZE,
    HSTEX_DIMEN_VSIZE,
    HSTEX_DIMEN_LINE_SKIP_LIMIT,
    HSTEX_DIMEN_MATH_SURROUND,
    HSTEX_DIMEN_PRE_DISPLAY_SIZE,
    HSTEX_DIMEN_DISPLAY_WIDTH,
    HSTEX_DIMEN_DISPLAY_INDENT,
    HSTEX_DIMEN_HANG_INDENT,
    HSTEX_DIMEN_HOFFSET,
    HSTEX_DIMEN_VOFFSET,
    HSTEX_DIMEN_EMERGENCY_STRETCH,
    HSTEX_DIMEN_PARAMETER_COUNT,
};

enum hstex_glue_parameter {
    HSTEX_GLUE_PAR_SKIP = 0,
    HSTEX_GLUE_ABOVE_DISPLAY_SKIP,
    HSTEX_GLUE_ABOVE_DISPLAY_SHORT_SKIP,
    HSTEX_GLUE_BELOW_DISPLAY_SKIP,
    HSTEX_GLUE_BELOW_DISPLAY_SHORT_SKIP,
    HSTEX_GLUE_TOP_SKIP,
    HSTEX_GLUE_SPLIT_TOP_SKIP,
    HSTEX_GLUE_PAR_FILL_SKIP,
    HSTEX_GLUE_BASELINE_SKIP,
    HSTEX_GLUE_LINE_SKIP,
    HSTEX_GLUE_LEFT_SKIP,
    HSTEX_GLUE_RIGHT_SKIP,
    HSTEX_GLUE_TAB_SKIP,
    HSTEX_GLUE_SPACE_SKIP,
    HSTEX_GLUE_XSPACE_SKIP,
    HSTEX_GLUE_PARAMETER_COUNT,
};

enum hstex_macro_flag {
    HSTEX_MACRO_LONG = 1U << 0U,
    HSTEX_MACRO_OUTER = 1U << 1U,
    HSTEX_MACRO_PROTECTED = 1U << 2U,
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
    HSTEX_SAVE_DIMEN,
    HSTEX_SAVE_GLUE,
    HSTEX_SAVE_DIMEN_PARAMETER,
    HSTEX_SAVE_GLUE_PARAMETER,
    HSTEX_SAVE_CODE,
};

struct hstex_glue {
    int32_t width;
    int32_t stretch;
    int32_t shrink;
    uint8_t stretch_order;
    uint8_t shrink_order;
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
        struct hstex_glue glue;
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
    int32_t *dimens;
    uint32_t *dimen_levels;
    struct hstex_glue *glues;
    uint32_t *glue_levels;
    int32_t dimen_parameters[HSTEX_DIMEN_PARAMETER_COUNT];
    uint32_t dimen_parameter_levels[HSTEX_DIMEN_PARAMETER_COUNT];
    struct hstex_glue glue_parameters[HSTEX_GLUE_PARAMETER_COUNT];
    uint32_t glue_parameter_levels[HSTEX_GLUE_PARAMETER_COUNT];
    int32_t code_tables[5][256];
    uint32_t code_levels[5][256];
    int32_t integer_parameters[HSTEX_INTEGER_PARAMETER_COUNT];
    uint32_t integer_parameter_levels[HSTEX_INTEGER_PARAMETER_COUNT];
    uint32_t catcode_levels[256];
    FILE *write_streams[16];
    FILE *read_streams[16];
    char *output_directory;
    uint32_t group_level;
    uint8_t pending_macro_flags;
    bool pending_global;
    bool returned_unexpanded;
    bool inhibit_protected_expansion;
};

int hstex_engine_init(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
void hstex_engine_destroy(struct hstex_engine *engine);
int hstex_engine_push_file(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity);
int hstex_engine_set_output_directory(struct hstex_engine *engine,
                                      const char *path, char *error,
                                      size_t error_capacity);
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
