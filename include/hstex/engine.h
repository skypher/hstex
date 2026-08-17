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
    HSTEX_COMMAND_FUTURE_LET,
    HSTEX_COMMAND_AFTER_ASSIGNMENT,
    HSTEX_COMMAND_AFTER_GROUP,
    HSTEX_COMMAND_LONG,
    HSTEX_COMMAND_OUTER,
    HSTEX_COMMAND_GLOBAL,
    HSTEX_COMMAND_EXPAND_AFTER,
    HSTEX_COMMAND_NO_EXPAND,
    HSTEX_COMMAND_CS_NAME,
    HSTEX_COMMAND_END_CS_NAME,
    HSTEX_COMMAND_EXPANDED,
    HSTEX_COMMAND_UNEXPANDED,
    HSTEX_COMMAND_DETOKENIZE,
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
    HSTEX_COMMAND_IF_DIM,
    HSTEX_COMMAND_IF_H_MODE,
    HSTEX_COMMAND_IF_V_MODE,
    HSTEX_COMMAND_IF_M_MODE,
    HSTEX_COMMAND_IF_INNER,
    HSTEX_COMMAND_IF_X,
    HSTEX_COMMAND_IF_TRUE,
    HSTEX_COMMAND_IF_FALSE,
    HSTEX_COMMAND_ELSE,
    HSTEX_COMMAND_FI,
    HSTEX_COMMAND_INPUT,
    HSTEX_COMMAND_PDF_FILE_SIZE,
    HSTEX_COMMAND_PDF_STRING_COMPARE,
    HSTEX_COMMAND_END,
    HSTEX_COMMAND_END_INPUT,
    HSTEX_COMMAND_ERROR_MESSAGE,
    HSTEX_COMMAND_ADVANCE,
    HSTEX_COMMAND_MULTIPLY,
    HSTEX_COMMAND_DIVIDE,
    HSTEX_COMMAND_THE,
    HSTEX_COMMAND_NUMBER,
    HSTEX_COMMAND_ROMAN_NUMERAL,
    HSTEX_COMMAND_NUM_EXPR,
    HSTEX_COMMAND_DIM_EXPR,
    HSTEX_COMMAND_GLUE_EXPR,
    HSTEX_COMMAND_MU_EXPR,
    HSTEX_COMMAND_IMMEDIATE,
    HSTEX_COMMAND_OPEN_OUT,
    HSTEX_COMMAND_WRITE,
    HSTEX_COMMAND_CLOSE_OUT,
    HSTEX_COMMAND_OPEN_IN,
    HSTEX_COMMAND_READ,
    HSTEX_COMMAND_READ_LINE,
    HSTEX_COMMAND_CLOSE_IN,
    HSTEX_COMMAND_IF_EOF,
    HSTEX_COMMAND_MEANING,
    HSTEX_COMMAND_STRING,
    HSTEX_COMMAND_JOB_NAME,
    HSTEX_COMMAND_IF_CHAR,
    HSTEX_COMMAND_INPUT_LINE_NUMBER,
    HSTEX_COMMAND_INTEGER_CONSTANT,
    HSTEX_COMMAND_MESSAGE,
    HSTEX_COMMAND_MATH_CHAR_DEF,
    HSTEX_COMMAND_MATH_CHAR_GIVEN,
    HSTEX_COMMAND_RADICAL,
    HSTEX_COMMAND_MARKS,
    HSTEX_COMMAND_PATTERNS,
    HSTEX_COMMAND_HYPHENATION,
    HSTEX_COMMAND_DIMEN_DEF,
    HSTEX_COMMAND_DIMEN_REGISTER,
    HSTEX_COMMAND_SKIP_DEF,
    HSTEX_COMMAND_SKIP_REGISTER,
    HSTEX_COMMAND_MUSKIP_DEF,
    HSTEX_COMMAND_MUSKIP_REGISTER,
    HSTEX_COMMAND_TOKS_DEF,
    HSTEX_COMMAND_TOKS_REGISTER,
    HSTEX_COMMAND_DIMEN,
    HSTEX_COMMAND_SKIP,
    HSTEX_COMMAND_MUSKIP,
    HSTEX_COMMAND_TOKS,
    HSTEX_COMMAND_BOX,
    HSTEX_COMMAND_SET_BOX,
    HSTEX_COMMAND_HBOX,
    HSTEX_COMMAND_VBOX,
    HSTEX_COMMAND_VSKIP,
    HSTEX_COMMAND_PENALTY,
    HSTEX_COMMAND_VRULE,
    HSTEX_COMMAND_MATH_GROUP,
    HSTEX_COMMAND_LANGUAGE,
    HSTEX_COMMAND_DIMEN_PARAMETER,
    HSTEX_COMMAND_GLUE_PARAMETER,
    HSTEX_COMMAND_MUGLUE_PARAMETER,
    HSTEX_COMMAND_TOKEN_PARAMETER,
    HSTEX_COMMAND_PROTECTED,
    HSTEX_COMMAND_SF_CODE,
    HSTEX_COMMAND_LC_CODE,
    HSTEX_COMMAND_UC_CODE,
    HSTEX_COMMAND_MATH_CODE,
    HSTEX_COMMAND_DEL_CODE,
    HSTEX_COMMAND_IF_DEFINED,
    HSTEX_COMMAND_IF_CS_NAME,
    HSTEX_COMMAND_IF_CAT,
    HSTEX_COMMAND_IF_ODD,
    HSTEX_COMMAND_IF_CASE,
    HSTEX_COMMAND_OR,
    HSTEX_COMMAND_UNLESS,
    HSTEX_COMMAND_LOWER_CASE,
    HSTEX_COMMAND_UPPER_CASE,
    HSTEX_COMMAND_IGNORE_SPACES,
    HSTEX_COMMAND_INTERACTION_MODE,
    HSTEX_COMMAND_DUMP,
    HSTEX_COMMAND_FONT,
    HSTEX_COMMAND_FONT_GIVEN,
    HSTEX_COMMAND_FONT_DIMEN,
    HSTEX_COMMAND_HYPHEN_CHAR,
    HSTEX_COMMAND_SKEW_CHAR,
    HSTEX_COMMAND_FONT_NAME,
    HSTEX_COMMAND_PREV_DEPTH,
    HSTEX_COMMAND_MATH_PRIMITIVE,
    HSTEX_COMMAND_PENALTY_ARRAY,
    HSTEX_COMMAND_ENGINE_STATE_INTEGER,
    HSTEX_COMMAND_PAGE_INTEGER,
    HSTEX_COMMAND_PAGE_DIMEN,
    HSTEX_COMMAND_PDF_TEX_REVISION,
    HSTEX_COMMAND_PDF_MATCH,
    HSTEX_COMMAND_PDF_LAST_MATCH,
    HSTEX_COMMAND_PDF_ESCAPE_STRING,
    HSTEX_COMMAND_PDF_ESCAPE_NAME,
    HSTEX_COMMAND_PDF_ESCAPE_HEX,
    HSTEX_COMMAND_PDF_UNESCAPE_HEX,
    HSTEX_COMMAND_PDF_GLYPH_TO_UNICODE,
    HSTEX_COMMAND_COPY,
    HSTEX_COMMAND_SHIFT_BOX,
    HSTEX_COMMAND_BOX_DIMEN,
    HSTEX_COMMAND_KERN,
    HSTEX_COMMAND_HRULE,
    HSTEX_COMMAND_SCAN_TOKENS,
    HSTEX_COMMAND_FONT_CHAR_DIMEN,
    HSTEX_COMMAND_FONT_CHAR_CODE,
    HSTEX_COMMAND_IF_FONT_CHAR,
    HSTEX_COMMAND_IF_BOX,
    HSTEX_COMMAND_PDF_CATALOG,
    HSTEX_COMMAND_PDF_INFO,
    HSTEX_COMMAND_PDF_OBJECT,
    HSTEX_COMMAND_PDF_REF_OBJECT,
    HSTEX_COMMAND_PDF_LITERAL,
    HSTEX_COMMAND_PDF_LAST_NUMBER,
    HSTEX_COMMAND_LAST_ITEM,
    HSTEX_COMMAND_PDF_DEST,
    HSTEX_COMMAND_PDF_START_LINK,
    HSTEX_COMMAND_PDF_END_LINK,
    HSTEX_COMMAND_PDF_OUTLINE,
    HSTEX_COMMAND_PDF_XFORM,
    HSTEX_COMMAND_PDF_ANNOT,
};

enum hstex_pdf_record_kind {
    HSTEX_PDF_RECORD_DESTINATION = 0,
    HSTEX_PDF_RECORD_LINK,
    HSTEX_PDF_RECORD_OUTLINE,
    HSTEX_PDF_RECORD_FORM,
    HSTEX_PDF_RECORD_ANNOTATION,
};

/* One thing the document has asked the PDF backend to place. Like the
   objects, these are recorded and not written; see docs/DECISIONS.md,
   pdf-annotations. */
struct hstex_pdf_record {
    enum hstex_pdf_record_kind kind;
    int32_t number;
    int32_t value;
    char *name;
    char *content;
};

/* What \lastpenalty, \lastkern, \lastskip and \lastnodetype report about the
   node most recently contributed to the current list. */
enum hstex_last_item {
    HSTEX_LAST_PENALTY = 0,
    HSTEX_LAST_KERN,
    HSTEX_LAST_SKIP,
    HSTEX_LAST_NODE_TYPE,
};

/* Which counter \pdflastobj and its siblings report. */
enum hstex_pdf_last {
    HSTEX_PDF_LAST_OBJECT = 0,
    HSTEX_PDF_LAST_ANNOTATION,
    HSTEX_PDF_LAST_LINK,
    HSTEX_PDF_LAST_FORM,
    HSTEX_PDF_LAST_IMAGE,
};

/* A PDF object the document has built. Nothing is written yet: the page
   builder and the output backend do not exist, so these are recorded for
   them; see docs/DECISIONS.md, pdf-objects. */
struct hstex_pdf_object {
    int32_t number;
    bool reserved;
    bool stream;
    char *attributes;
    char *content;
};

/* A \pdfliteral, kept in the order it was written. */
struct hstex_pdf_literal {
    int32_t mode;
    char *content;
};

enum hstex_pdf_literal_mode {
    HSTEX_PDF_LITERAL_SET = 0,
    HSTEX_PDF_LITERAL_DIRECT,
    HSTEX_PDF_LITERAL_PAGE,
    HSTEX_PDF_LITERAL_SHIPOUT,
};

/* Which question \ifhbox, \ifvbox and \ifvoid ask about a box register. */
enum hstex_if_box {
    HSTEX_IF_BOX_HORIZONTAL = 0,
    HSTEX_IF_BOX_VERTICAL,
    HSTEX_IF_BOX_VOID,
};

enum hstex_font_char_code {
    HSTEX_FONT_CODE_LEFT_PROTRUSION = 0,
    HSTEX_FONT_CODE_RIGHT_PROTRUSION,
    HSTEX_FONT_CODE_EXPANSION,
    HSTEX_FONT_CODE_TAG,
};

enum hstex_font_char_dimen {
    HSTEX_FONT_CHAR_WIDTH = 0,
    HSTEX_FONT_CHAR_HEIGHT,
    HSTEX_FONT_CHAR_DEPTH,
    HSTEX_FONT_CHAR_ITALIC,
};

enum hstex_box_dimen {
    HSTEX_BOX_DIMEN_WIDTH = 0,
    HSTEX_BOX_DIMEN_HEIGHT,
    HSTEX_BOX_DIMEN_DEPTH,
};

enum hstex_shift_box {
    HSTEX_SHIFT_RAISE = 0,
    HSTEX_SHIFT_LOWER,
    HSTEX_SHIFT_MOVE_LEFT,
    HSTEX_SHIFT_MOVE_RIGHT,
};

/* One \pdfglyphtounicode mapping. The PDF backend turns these into the
   ToUnicode CMap that makes extracted text match the reference. */
struct hstex_glyph_unicode {
    char *glyph;
    char *unicode;
};

/* One captured group of the most recent \pdfmatch. A group that did not
   participate keeps offset -1 and no text. */
struct hstex_match_group {
    int32_t offset;
    char *text;
};

#define HSTEX_DEFAULT_MATCH_SUBCOUNT 10

/* Packages and expl3 branch on the pdfTeX version. HSTeX reports the version
   of the reference engine it reproduces so that those branches are taken the
   same way; see docs/DECISIONS.md, pdftex-identification. */
#define HSTEX_PDFTEX_VERSION 140
#define HSTEX_PDFTEX_REVISION "25"

/* Page-builder state. It belongs to the page rather than to a group, so it is
   not saved or restored by grouping. */
enum hstex_page_integer {
    HSTEX_PAGE_DEAD_CYCLES = 0,
    HSTEX_PAGE_INSERT_PENALTIES,
    HSTEX_PAGE_INTEGER_COUNT,
};

enum hstex_page_dimen {
    HSTEX_PAGE_GOAL = 0,
    HSTEX_PAGE_TOTAL,
    HSTEX_PAGE_STRETCH,
    HSTEX_PAGE_FIL_STRETCH,
    HSTEX_PAGE_FILL_STRETCH,
    HSTEX_PAGE_FILLL_STRETCH,
    HSTEX_PAGE_SHRINK,
    HSTEX_PAGE_DEPTH,
    HSTEX_PAGE_DIMEN_COUNT,
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
    HSTEX_INTEGER_TRACING_STATS,
    HSTEX_INTEGER_UC_HYPH,
    HSTEX_INTEGER_DEFAULT_HYPHEN_CHAR,
    HSTEX_INTEGER_DEFAULT_SKEW_CHAR,
    HSTEX_INTEGER_DELIMITER_FACTOR,
    HSTEX_INTEGER_SHOW_BOX_BREADTH,
    HSTEX_INTEGER_SHOW_BOX_DEPTH,
    HSTEX_INTEGER_ERROR_CONTEXT_LINES,
    HSTEX_INTEGER_MAX_DEAD_CYCLES,
    HSTEX_INTEGER_LEFT_HYPHEN_MIN,
    HSTEX_INTEGER_RIGHT_HYPHEN_MIN,
    HSTEX_INTEGER_LANGUAGE,
    HSTEX_INTEGER_MATH_GROUP,
    HSTEX_INTEGER_MAGNIFICATION,
    /* pdfTeX output configuration, set by pdftexconfig.tex. */
    HSTEX_INTEGER_PDF_OUTPUT,
    HSTEX_INTEGER_PDF_MAJOR_VERSION,
    HSTEX_INTEGER_PDF_MINOR_VERSION,
    HSTEX_INTEGER_PDF_COMPRESS_LEVEL,
    HSTEX_INTEGER_PDF_OBJ_COMPRESS_LEVEL,
    HSTEX_INTEGER_PDF_DECIMAL_DIGITS,
    HSTEX_INTEGER_PDF_PK_RESOLUTION,
    HSTEX_INTEGER_PDF_DRAFT_MODE,
    HSTEX_INTEGER_PDF_ADJUST_SPACING,
    HSTEX_INTEGER_PDF_PROTRUDE_CHARS,
    HSTEX_INTEGER_PDF_GEN_TO_UNICODE,
    HSTEX_INTEGER_PDF_UNIQUE_RES_NAME,
    HSTEX_INTEGER_PDF_IMAGE_RESOLUTION,
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
    /* pdfTeX page geometry, set by pdftexconfig.tex. */
    HSTEX_DIMEN_PDF_PAGE_WIDTH,
    HSTEX_DIMEN_PDF_PAGE_HEIGHT,
    HSTEX_DIMEN_PDF_HORIGIN,
    HSTEX_DIMEN_PDF_VORIGIN,
    HSTEX_DIMEN_PDF_LINK_MARGIN,
    HSTEX_DIMEN_PDF_DEST_MARGIN,
    HSTEX_DIMEN_PDF_THREAD_MARGIN,
    HSTEX_DIMEN_PDF_PX_DIMEN,
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

enum hstex_muglue_parameter {
    HSTEX_MUGLUE_THIN = 0,
    HSTEX_MUGLUE_MEDIUM,
    HSTEX_MUGLUE_THICK,
    HSTEX_MUGLUE_PARAMETER_COUNT,
};

enum hstex_token_parameter {
    HSTEX_TOKEN_OUTPUT = 0,
    HSTEX_TOKEN_EVERY_PAR,
    HSTEX_TOKEN_EVERY_MATH,
    HSTEX_TOKEN_EVERY_DISPLAY,
    HSTEX_TOKEN_EVERY_HBOX,
    HSTEX_TOKEN_EVERY_VBOX,
    HSTEX_TOKEN_EVERY_JOB,
    HSTEX_TOKEN_EVERY_CR,
    HSTEX_TOKEN_ERROR_HELP,
    HSTEX_TOKEN_PARAMETER_COUNT,
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
    hstex_cs_id primitive_origin;
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
    HSTEX_SAVE_MUGLUE,
    HSTEX_SAVE_DIMEN_PARAMETER,
    HSTEX_SAVE_GLUE_PARAMETER,
    HSTEX_SAVE_MUGLUE_PARAMETER,
    HSTEX_SAVE_CODE,
    HSTEX_SAVE_TOKEN_REGISTER,
    HSTEX_SAVE_TOKEN_PARAMETER,
    HSTEX_SAVE_BOX,
    HSTEX_SAVE_AFTER_GROUP,
};

struct hstex_glue {
    int32_t width;
    int32_t stretch;
    int32_t shrink;
    uint8_t stretch_order;
    uint8_t shrink_order;
};

struct hstex_token_list {
    hstex_token *tokens;
    size_t count;
};

/* Width, height, depth and italic correction of one character, already
   scaled to the font's size. A character the font does not define measures
   zero in all four; see docs/DECISIONS.md, font-character-metrics. */
struct hstex_char_metric {
    int32_t width;
    int32_t height;
    int32_t depth;
    int32_t italic;
    /* The metric file's tag, or -1 for a character it does not define. */
    int32_t tag;
    /* Protrusion and expansion settings. These belong to the font, not to a
       group, so they are never restored; see docs/DECISIONS.md,
       protrusion-codes. */
    int32_t left_protrusion;
    int32_t right_protrusion;
    int32_t expansion_factor;
};

#define HSTEX_DEFAULT_EXPANSION_FACTOR 1000

#define HSTEX_FONT_CHARACTER_COUNT 256U

struct hstex_font {
    char *name;
    struct hstex_char_metric *characters;
    int32_t design_size;
    /* The control sequence \the\font reports for this font. Re-declaring an
       already loaded font reuses it and renames it to the newer control
       sequence; see docs/DECISIONS.md, font-identifier. */
    hstex_cs_id identifier_cs;
    int32_t size;
    int32_t *dimens;
    size_t dimen_count;
    size_t dimen_capacity;
    int32_t hyphen_character;
    int32_t skew_character;
};

struct hstex_hyphen_trie_node {
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t value_offset;
    uint16_t value_count;
    uint8_t character;
};

struct hstex_hyphen_exception {
    uint32_t letter_offset;
    uint32_t break_offset;
    uint16_t letter_count;
    uint16_t language;
};

enum hstex_box_kind {
    HSTEX_BOX_VOID = 0,
    HSTEX_BOX_HLIST,
    HSTEX_BOX_VLIST,
};

struct hstex_box {
    enum hstex_box_kind kind;
    int32_t width;
    int32_t height;
    int32_t depth;
    uint32_t node_start;
    uint32_t node_count;
};

/* A rule dimension the enclosing box supplies. It survives packaging and is
   resolved when the page is shipped, so it is a value outside the legal
   dimension range rather than a flag; see docs/DECISIONS.md, rules-and-kerns. */
#define HSTEX_RUNNING_DIMEN (-INT32_C(1073741824))

enum hstex_node_kind {
    HSTEX_NODE_RULE = 0,
    HSTEX_NODE_CHARACTER,
    HSTEX_NODE_GLUE,
    HSTEX_NODE_PENALTY,
    HSTEX_NODE_LIST,
    HSTEX_NODE_KERN,
};

struct hstex_node {
    enum hstex_node_kind kind;
    int32_t width;
    int32_t height;
    int32_t depth;
    /* Displacement of a box from its list's baseline: down in a horizontal
       list, right in a vertical one. Set by \raise, \lower, \moveleft, and
       \moveright; see docs/DECISIONS.md, box-shift. */
    int32_t shift;
    union {
        struct {
            uint32_t font;
            uint32_t character;
        } character;
        struct {
            int32_t stretch;
            int32_t shrink;
            uint8_t stretch_order;
            uint8_t shrink_order;
        } glue;
        struct {
            uint32_t node_start;
            uint32_t node_count;
            enum hstex_box_kind box_kind;
        } list;
        int32_t penalty;
    } value;
};

struct hstex_hbox_builder;
struct hstex_vbox_builder;

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
        struct hstex_box box;
        uint32_t token_list_identifier;
        struct {
            hstex_token token;
            struct hstex_source_location location;
        } after_group;
    } previous;
};

struct hstex_conditional {
    bool branch_true;
    bool else_seen;
    bool case_conditional;
    bool negate;
    /* False until the test has been evaluated. An \else or \fi met before
       that belongs to no branch yet, and yields \relax instead; see
       docs/DECISIONS.md, unevaluated-conditionals. */
    bool evaluated;
    /* Where the conditional was opened, so that an unbalanced \else or \fi
       can say which one it belongs to. */
    uint32_t line;
    const char *origin;
};

enum hstex_mode {
    HSTEX_MODE_VERTICAL = 0,
    HSTEX_MODE_HORIZONTAL,
    HSTEX_MODE_MATH,
};

enum hstex_interaction_mode {
    HSTEX_INTERACTION_BATCH = 0,
    HSTEX_INTERACTION_NONSTOP,
    HSTEX_INTERACTION_SCROLL,
    HSTEX_INTERACTION_ERROR_STOP,
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
    struct hstex_glue *muglues;
    uint32_t *muglue_levels;
    uint32_t *token_registers;
    uint32_t *token_register_levels;
    struct hstex_box *boxes;
    uint32_t *box_levels;
    struct hstex_node *nodes;
    size_t node_count;
    size_t node_capacity;
    uint32_t *list_items;
    size_t list_item_count;
    size_t list_item_capacity;
    struct hstex_token_list *token_lists;
    size_t token_list_count;
    size_t token_list_capacity;
    struct hstex_font *fonts;
    size_t font_count;
    size_t font_capacity;
    uint32_t current_font;
    uint32_t *hyphen_roots;
    struct hstex_hyphen_trie_node *hyphen_nodes;
    size_t hyphen_node_count;
    size_t hyphen_node_capacity;
    uint8_t *hyphen_values;
    size_t hyphen_value_count;
    size_t hyphen_value_capacity;
    size_t hyphen_pattern_count;
    struct hstex_hyphen_exception *hyphen_exceptions;
    size_t hyphen_exception_count;
    size_t hyphen_exception_capacity;
    uint8_t *hyphen_exception_data;
    size_t hyphen_exception_data_count;
    size_t hyphen_exception_data_capacity;
    int32_t dimen_parameters[HSTEX_DIMEN_PARAMETER_COUNT];
    uint32_t dimen_parameter_levels[HSTEX_DIMEN_PARAMETER_COUNT];
    struct hstex_glue glue_parameters[HSTEX_GLUE_PARAMETER_COUNT];
    uint32_t glue_parameter_levels[HSTEX_GLUE_PARAMETER_COUNT];
    struct hstex_glue muglue_parameters[HSTEX_MUGLUE_PARAMETER_COUNT];
    uint32_t muglue_parameter_levels[HSTEX_MUGLUE_PARAMETER_COUNT];
    uint32_t token_parameters[HSTEX_TOKEN_PARAMETER_COUNT];
    uint32_t token_parameter_levels[HSTEX_TOKEN_PARAMETER_COUNT];
    int32_t code_tables[5][256];
    uint32_t code_levels[5][256];
    int32_t integer_parameters[HSTEX_INTEGER_PARAMETER_COUNT];
    uint32_t integer_parameter_levels[HSTEX_INTEGER_PARAMETER_COUNT];
    int32_t page_integers[HSTEX_PAGE_INTEGER_COUNT];
    int32_t page_dimens[HSTEX_PAGE_DIMEN_COUNT];
    struct hstex_match_group *match_groups;
    size_t match_group_count;
    /* Everything the document has asked the PDF backend to record. */
    uint8_t *pdf_catalog;
    size_t pdf_catalog_length;
    size_t pdf_catalog_capacity;
    uint8_t *pdf_info;
    size_t pdf_info_length;
    size_t pdf_info_capacity;
    struct hstex_pdf_object *pdf_objects;
    size_t pdf_object_count;
    size_t pdf_object_capacity;
    struct hstex_pdf_literal *pdf_literals;
    size_t pdf_literal_count;
    size_t pdf_literal_capacity;
    struct hstex_pdf_record *pdf_records;
    size_t pdf_record_count;
    size_t pdf_record_capacity;
    int32_t pdf_last[5];
    /* Objects, links, forms and annotations are numbered from one shared
       counter; see docs/DECISIONS.md, pdf-annotations. */
    int32_t pdf_object_counter;
    struct hstex_glyph_unicode *glyph_unicode;
    size_t glyph_unicode_count;
    size_t glyph_unicode_capacity;
    uint32_t catcode_levels[256];
    FILE *write_streams[16];
    FILE *read_streams[16];
    char *output_directory;
    char *job_name;
    enum hstex_mode mode;
    int32_t prev_depth;
    enum hstex_interaction_mode interaction_mode;
    bool inner_mode;
    hstex_token after_assignment_token;
    struct hstex_source_location after_assignment_location;
    bool has_after_assignment;
    uint32_t group_level;
    uint8_t pending_macro_flags;
    bool pending_global;
    bool returned_unexpanded;
    bool returned_unexpanded_executable;
    bool inhibit_protected_expansion;
    bool negate_next_conditional;
    bool dump_requested;
    bool end_requested;
    uint32_t output_group_floor;
    size_t output_conditional_floor;
    struct hstex_hbox_builder *active_hbox_builder;
    struct hstex_vbox_builder *page_builder;
    struct hstex_vbox_builder *active_vbox_builder;
};

int hstex_engine_init(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
void hstex_engine_destroy(struct hstex_engine *engine);
int hstex_engine_push_file(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity);
/* Push a file named the way \input names it, resolved through the same
   search order. */
int hstex_engine_push_input(struct hstex_engine *engine, const char *name,
                            char *error, size_t error_capacity);
int hstex_engine_begin_job(struct hstex_engine *engine, const char *path,
                           char *error, size_t error_capacity);
int hstex_engine_set_output_directory(struct hstex_engine *engine,
                                      const char *path, char *error,
                                      size_t error_capacity);
int hstex_engine_hyphenate_word(const struct hstex_engine *engine,
                                int32_t language, const uint8_t *word,
                                size_t length, uint8_t *break_before,
                                size_t break_capacity, char *error,
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
