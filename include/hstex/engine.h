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
    HSTEX_COMMAND_IF_IN_CS_NAME,
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
    HSTEX_COMMAND_SPECIAL,
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
    HSTEX_COMMAND_PDF_COLOR_STACK,
    HSTEX_COMMAND_PDF_COLOR_STACK_INIT,
    HSTEX_COMMAND_UNBOX,
    HSTEX_COMMAND_HSKIP,
    HSTEX_COMMAND_INDENT,
    HSTEX_COMMAND_SPACE_FACTOR,
    HSTEX_COMMAND_VTOP,
    HSTEX_COMMAND_VSPLIT,
    HSTEX_COMMAND_LAST_BOX,
    HSTEX_COMMAND_CONTROL_SPACE,
    HSTEX_COMMAND_ITALIC_CORRECTION,
    HSTEX_COMMAND_DISCRETIONARY,
    HSTEX_COMMAND_DISCRETIONARY_HYPHEN,
    HSTEX_COMMAND_CHAR,
    HSTEX_COMMAND_REMOVE_LAST,
    HSTEX_COMMAND_MATH_FONT,
    HSTEX_COMMAND_MATH_CHAR,
    HSTEX_COMMAND_MATH_CLASS,
    HSTEX_COMMAND_MATH_SKIP,
    HSTEX_COMMAND_MATH_KERN,
    HSTEX_COMMAND_MATH_LIMITS,
    HSTEX_COMMAND_HALIGN,
    HSTEX_COMMAND_CR,
    HSTEX_COMMAND_NO_ALIGN,
    HSTEX_COMMAND_OMIT,
    HSTEX_COMMAND_SPAN,
    HSTEX_COMMAND_MATH_STYLE,
    HSTEX_COMMAND_MATH_CHOICE,
    HSTEX_COMMAND_ACCENT,
    HSTEX_COMMAND_EQUATION_NUMBER,
    HSTEX_COMMAND_VCENTER,
    HSTEX_COMMAND_MARGIN_KERN,
    HSTEX_COMMAND_DELIMITER,
    HSTEX_COMMAND_LEFT_RIGHT,
    HSTEX_COMMAND_FRACTION,
    HSTEX_COMMAND_PAR_SHAPE,
    HSTEX_COMMAND_OVER_UNDER_LINE,
    HSTEX_COMMAND_LEADERS,
    HSTEX_COMMAND_NON_SCRIPT,
    HSTEX_COMMAND_SHIP_OUT,
    HSTEX_COMMAND_SHOW_BOX,
};

/* \unhbox, \unhcopy, \unvbox and \unvcopy: which direction, and whether the
   register is emptied. */
enum hstex_unbox {
    HSTEX_UNBOX_HORIZONTAL = 0,
    HSTEX_UNBOX_HORIZONTAL_COPY,
    HSTEX_UNBOX_VERTICAL,
    HSTEX_UNBOX_VERTICAL_COPY,
};

/* One colour stack. Stack zero is built in and belongs to the page; the rest
   come from \pdfcolorstackinit. Nothing is written yet, so the stack is kept
   only so that push, pop and current agree with one another; see
   docs/DECISIONS.md, colour-stacks. */
struct hstex_color_stack {
    char *initial;
    bool page;
    bool direct;
    char **values;
    size_t count;
    size_t capacity;
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

/* The protrusion a line's first or last character was set with. HSTeX does
   not protrude, so these are always nothing; see docs/DECISIONS.md,
   margin-kerns. */
enum hstex_margin_kern {
    HSTEX_MARGIN_KERN_LEFT = 0,
    HSTEX_MARGIN_KERN_RIGHT,
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
    HSTEX_INTEGER_POST_DISPLAY_PENALTY,
    HSTEX_INTEGER_INTERLINE_PENALTY,
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
    /* Tracing and paragraph-shape parameters. */
    HSTEX_INTEGER_TRACING_ONLINE,
    HSTEX_INTEGER_TRACING_COMMANDS,
    HSTEX_INTEGER_TRACING_MACROS,
    HSTEX_INTEGER_TRACING_PARAGRAPHS,
    HSTEX_INTEGER_TRACING_PAGES,
    HSTEX_INTEGER_TRACING_OUTPUT,
    HSTEX_INTEGER_TRACING_RESTORES,
    HSTEX_INTEGER_TRACING_ASSIGNS,
    HSTEX_INTEGER_TRACING_GROUPS,
    HSTEX_INTEGER_TRACING_IFS,
    HSTEX_INTEGER_TRACING_SCAN_TOKENS,
    HSTEX_INTEGER_TRACING_NESTING,
    HSTEX_INTEGER_PAUSING,
    HSTEX_INTEGER_HOLDING_INSERTS,
    HSTEX_INTEGER_OUTPUT_PENALTY,
    HSTEX_INTEGER_HANG_AFTER,
    HSTEX_INTEGER_FLOATING_PENALTY,
    HSTEX_INTEGER_LOOSENESS,
    HSTEX_INTEGER_FAMILY,
    HSTEX_INTEGER_PRE_DISPLAY_DIRECTION,
    HSTEX_INTEGER_LAST_LINE_FIT,
    HSTEX_INTEGER_SAVING_VDISCARDS,
    HSTEX_INTEGER_SAVING_HYPH_CODES,
    HSTEX_INTEGER_TEXXET_STATE,
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
    /* Inserted when a file or a \scantokens pseudo-file runs out. */
    HSTEX_TOKEN_EVERY_EOF,
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
    HSTEX_SAVE_MATH_FONT,
    HSTEX_SAVE_PAR_SHAPE,
    HSTEX_SAVE_FONT,
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
    /* The metric file's tag, or -1 for a character it does not define, and
       the tag's operand: where the ligature and kerning program starts. */
    int32_t tag;
    int32_t remainder;
    /* Protrusion and expansion settings. These belong to the font, not to a
       group, so they are never restored; see docs/DECISIONS.md,
       protrusion-codes. */
    int32_t left_protrusion;
    int32_t right_protrusion;
    int32_t expansion_factor;
};

#define HSTEX_DEFAULT_EXPANSION_FACTOR 1000

#define HSTEX_FONT_CHARACTER_COUNT 256U

/* One step of a metric file's ligature and kerning program. */
struct hstex_lig_kern {
    uint8_t skip;
    uint8_t next;
    uint8_t operation;
    uint8_t remainder;
};

/* One recipe for building a delimiter too tall for any single character:
   the pieces to stack, bottom, middle and top, with `repeated` filling the
   gaps. A piece of zero is absent. */
struct hstex_extensible {
    uint8_t top;
    uint8_t middle;
    uint8_t bottom;
    uint8_t repeated;
};

struct hstex_font {
    char *name;
    struct hstex_char_metric *characters;
    struct hstex_lig_kern *lig_kern;
    size_t lig_kern_count;
    int32_t *kerns;
    size_t kern_count;
    struct hstex_extensible *extensibles;
    size_t extensible_count;
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

/* How a box's glue was set when it was packed: which order was used, whether
   it stretched or shrank, and the ratio as the two numbers it came from, so
   that each glue's share can be worked out exactly. See docs/DECISIONS.md,
   glue-set. */
enum hstex_glue_sign {
    HSTEX_GLUE_SIGN_NORMAL = 0,
    HSTEX_GLUE_SIGN_STRETCHING,
    HSTEX_GLUE_SIGN_SHRINKING,
};

struct hstex_glue_set {
    int32_t needed;
    int32_t total;
    uint8_t sign;
    uint8_t order;
};

struct hstex_box {
    enum hstex_box_kind kind;
    int32_t width;
    int32_t height;
    int32_t depth;
    uint32_t node_start;
    uint32_t node_count;
    struct hstex_glue_set glue;
};

/* A rule dimension the enclosing box supplies. It survives packaging and is
   resolved when the page is shipped, so it is a value outside the legal
   dimension range rather than a flag; see docs/DECISIONS.md, rules-and-kerns. */
#define HSTEX_RUNNING_DIMEN (-INT32_C(1073741824))

/* The three sizes a math family is set in. Only the text size is used so
   far; see docs/DECISIONS.md, math-mode. */
/* The reference's two sentinel badnesses: the worst a box can be and still
   be set, and what an overfull box reports. */
#define HSTEX_INFINITE_BADNESS 10000
/* What a breaker calls a hopeless break, and a merely dreadful one. */
#define HSTEX_AWFUL_BADNESS INT64_C(0x3FFFFFFF)
#define HSTEX_DEPLORABLE_COST 100000
#define HSTEX_INFINITE_PENALTY 10000
#define HSTEX_OVERFULL_BADNESS 1000000

enum hstex_math_size {
    HSTEX_MATH_TEXT = 0,
    HSTEX_MATH_SCRIPT,
    HSTEX_MATH_SCRIPT_SCRIPT,
    HSTEX_MATH_SIZE_COUNT,
};

/* An atom's class decides the spacing around it. Classes 0..6 are the
   mathcode classes; class 7 in a mathcode means "use \fam" and never reaches
   a noad, so the slot is reused for Inner. */
enum hstex_atom_class {
    HSTEX_ATOM_ORD = 0,
    HSTEX_ATOM_OP,
    HSTEX_ATOM_BIN,
    HSTEX_ATOM_REL,
    HSTEX_ATOM_OPEN,
    HSTEX_ATOM_CLOSE,
    HSTEX_ATOM_PUNCT,
    HSTEX_ATOM_INNER,
    HSTEX_ATOM_CLASS_COUNT,
};

enum hstex_noad_kind {
    /* An atom: a class and a nucleus. */
    HSTEX_NOAD_ATOM = 0,
    /* A change of style for everything after it. */
    HSTEX_NOAD_STYLE,
    /* A list node that was built already and passes through untouched. */
    HSTEX_NOAD_NODE,
    /* Glue and kerns measured in mu, converted when the list is translated. */
    HSTEX_NOAD_MU_GLUE,
    HSTEX_NOAD_MU_KERN,
    /* All four lists of a \mathchoice; which is used is settled when the
       list is set, not when it is read. */
    HSTEX_NOAD_CHOICE,
    /* \radical: a delimiter over a nucleus, built when the list is set. */
    HSTEX_NOAD_RADICAL,
    /* \overline and \underline: a rule over or under a nucleus. */
    HSTEX_NOAD_OVERLINE,
    HSTEX_NOAD_UNDERLINE,
    /* \middle: a delimiter as tall as the \left group that holds it, whose
       size is only known once that group has been measured. */
    HSTEX_NOAD_MIDDLE,
    /* \nonscript: takes away the glue or kern after it in a script. */
    HSTEX_NOAD_NONSCRIPT,
};

enum hstex_math_field_kind {
    HSTEX_MATH_FIELD_EMPTY = 0,
    HSTEX_MATH_FIELD_CHARACTER,
    HSTEX_MATH_FIELD_BOX,
};

/* One column of an alignment preamble: the token lists that surround the
   entry, and the glue that follows the column. */
struct hstex_align_column {
    hstex_token *before;
    size_t before_count;
    hstex_token *after;
    size_t after_count;
    struct hstex_glue tabskip;
    int32_t width;
};

/* One entry of a row: an unset box of its natural width, and how many
   columns it covers. */
struct hstex_align_cell {
    uint32_t box;
    int32_t width;
    uint32_t span;
};

/* What the executor needs in order to end an alignment entry from wherever
   the tab or \cr turns up -- which may be inside a box the entry's own
   template opened. */
struct hstex_align_entry {
    const struct hstex_align_column *columns;
    size_t column_count;
    size_t column;
    bool omit;
    bool after_pushed;
    uint8_t ending;
};

/* A row of entries, or a run of vertical material contributed by \noalign. */
struct hstex_align_row {
    bool noalign;
    struct hstex_align_cell *cells;
    size_t cell_count;
    uint32_t *items;
    size_t item_count;
};

/* The styles, numbered so that the odd ones are the cramped variants and the
   pair for one size is adjacent. Display style is not implemented. */
enum hstex_math_style {
    HSTEX_STYLE_DISPLAY = 0,
    HSTEX_STYLE_DISPLAY_CRAMPED = 1,
    HSTEX_STYLE_TEXT = 2,
    HSTEX_STYLE_TEXT_CRAMPED = 3,
    HSTEX_STYLE_SCRIPT = 4,
    HSTEX_STYLE_SCRIPT_CRAMPED = 5,
    HSTEX_STYLE_SCRIPT_SCRIPT = 6,
    HSTEX_STYLE_SCRIPT_SCRIPT_CRAMPED = 7,
};

/* A nucleus, a superscript or a subscript. */
struct hstex_math_field {
    uint8_t kind;
    uint8_t family;
    /* True for a box field whose whole content is one character node: the
       reference places scripts on it as if it were that character. */
    uint8_t single_character;
    uint32_t character;
    /* Identifier of the stored node, for a box field. */
    uint32_t node;
    /* A sub-formula keeps its own list as well as the box it was set as, so
       that it can be set again if it turns out to be wanted in another
       style -- which is what a fraction does to its two sides. One-based
       index into the engine's sub-formula records; zero for none. See
       docs/DECISIONS.md, fractions. */
    uint32_t sublist;
    /* The style the stored box was set in. */
    uint8_t list_style;
};

/* A sub-formula kept so that it can be set again in another style. */
struct hstex_math_sublist {
    uint32_t start;
    uint32_t count;
    uint8_t style;
    bool has_fraction;
    size_t fraction_at;
    int32_t fraction_thickness;
    bool fraction_default_thickness;
    int32_t fraction_left;
    int32_t fraction_right;
};

/* One item of a math list. */
struct hstex_noad {
    uint8_t kind;
    uint8_t atom_class;
    struct hstex_math_field nucleus;
    struct hstex_math_field superscript;
    struct hstex_math_field subscript;
    /* Identifier of the stored node, for NODE items. */
    uint32_t node;
    struct hstex_glue glue;
    int32_t kern;
    /* The four lists of a \mathchoice, as sub-formula records. */
    uint32_t choices[4];
    /* The delimiter of a \radical. */
    int32_t delimiter;
    /* This atom's nucleus was read as a character of a text font, because
       another character of the same family follows it. Its italic
       correction is then dropped; see docs/DECISIONS.md,
       math-text-characters. */
    bool text_character;
};

/* Which slot the next atom fills, when a script mark is waiting. */
enum hstex_math_slot {
    HSTEX_MATH_SLOT_NONE = 0,
    HSTEX_MATH_SLOT_SUPERSCRIPT,
    HSTEX_MATH_SLOT_SUBSCRIPT,
    /* The field \radical is waiting for. */
    HSTEX_MATH_SLOT_RADICAND,
};

struct hstex_math_builder {
    struct hstex_noad *noads;
    size_t count;
    size_t capacity;
    /* The class \mathord and its relatives forced on the next atom, or -1. */
    int32_t forced_class;
    uint8_t style;
    /* The style in force at the point the list has been read to, which a
       style command moves and which \mathchoice and a script mark consult. */
    uint8_t current_style;
    uint8_t slot;
    size_t slot_target;
    /* Saved across the formula: inline math is an inner mode, so \ifinner is
       true there and false in a display, and a formula inside a display is
       not itself one. */
    bool outer_inner_mode;
    bool outer_displayed;
    /* Branches of a \mathchoice still to be read, and which one is next. */
    uint8_t choice_remaining;
    uint8_t choice_index;
    size_t choice_noad;
    /* Set for the list \left opened, with the delimiter it named. */
    bool is_left_group;
    int32_t left_delimiter;
    /* A generalized fraction: the noads before `fraction_at` are its
       numerator and the rest are its denominator; see docs/DECISIONS.md,
       fractions. */
    bool has_fraction;
    size_t fraction_at;
    int32_t fraction_thickness;
    bool fraction_default_thickness;
    int32_t fraction_left;
    int32_t fraction_right;
};

enum hstex_node_kind {
    HSTEX_NODE_RULE = 0,
    HSTEX_NODE_CHARACTER,
    HSTEX_NODE_GLUE,
    HSTEX_NODE_PENALTY,
    HSTEX_NODE_LIST,
    HSTEX_NODE_KERN,
    HSTEX_NODE_LIGATURE,
    HSTEX_NODE_WHATSIT,
    HSTEX_NODE_DISCRETIONARY,
    HSTEX_NODE_MATH,
};

/* What a whatsit does when the page it sits on is shipped out. See
   docs/DECISIONS.md, whatsits. */
enum hstex_whatsit_kind {
    HSTEX_WHATSIT_WRITE = 0,
    HSTEX_WHATSIT_OPEN_OUT,
    HSTEX_WHATSIT_CLOSE_OUT,
    HSTEX_WHATSIT_SPECIAL,
};

struct hstex_node {
    enum hstex_node_kind kind;
    /* A kern written as \kern is explicit: it may be broken at and it is not
       thrown away at the start of a line. A kern the engine put there itself
       -- a font kern, an italic correction, math spacing -- is not. */
    bool explicit_kern;
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
            /* What a ligature was made of, so that \showbox can name it and
               a later pass can take it apart again; see
               docs/DECISIONS.md, ligature-originals. */
            uint8_t originals[6];
            uint8_t original_count;
        } character;
        struct {
            int32_t stretch;
            int32_t shrink;
            uint8_t stretch_order;
            uint8_t shrink_order;
            /* \leaders and its relatives: the box or rule this glue is
               filled with, and which of the three commands asked for it.
               Zero for ordinary glue. See docs/DECISIONS.md, leaders. */
            uint32_t leader;
            uint8_t leader_kind;
            /* Which glue parameter this came from, one more than its index,
               or zero for glue that is nobody's parameter. \showbox names
               it; see docs/DECISIONS.md, showbox. */
            uint8_t parameter;
        } glue;
        struct {
            uint32_t node_start;
            uint32_t node_count;
            enum hstex_box_kind box_kind;
            struct hstex_glue_set glue;
        } list;
        struct {
            /* What is set instead of the following `replace_count` nodes
               when the line breaks here, and what starts the next line.
               Both are runs in the list arena; see docs/DECISIONS.md,
               discretionaries. */
            uint32_t pre_start;
            uint32_t post_start;
            uint16_t pre_count;
            uint16_t post_count;
            uint8_t replace_count;
        } disc;
        struct {
            /* Whether this node closes a formula rather than opening one.
               Its width is the \mathsurround it was made with; see
               docs/DECISIONS.md, math-nodes. */
            bool after;
        } math;
        struct {
            /* Whether this kern is one the line breaker put at a margin so
               that a character may stick out past it: 0 for no, 1 for the
               left margin, 2 for the right. See docs/DECISIONS.md,
               character-protrusion. */
            uint8_t margin;
        } kern;
        struct {
            /* An enum hstex_whatsit_kind. */
            uint8_t kind;
            /* The stream the reference stores: 0..15 as given, 16 for any
               larger number, 17 for a negative one. */
            uint8_t stream;
            /* The unexpanded text of a \write, the already expanded text of
               a \special, or the file name of an \openout. */
            uint32_t tokens;
        } whatsit;
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
    /* The group that last chose the current font, so that leaving it puts
       the font back; see docs/DECISIONS.md, the-current-font-is-grouped. */
    uint32_t current_font_level;
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
    struct hstex_color_stack *color_stacks;
    size_t color_stack_count;
    size_t color_stack_capacity;
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
    /* The space factor of the horizontal list being built. */
    int32_t space_factor;
    /* A character held back so that the font's ligature and kerning program
       can see it beside the next one. It is flushed before anything else
       happens, so the list is never observed mid-pair. */
    bool has_pending_character;
    /* What the character held back was made of, if it is a ligature. */
    uint8_t pending_originals[6];
    uint8_t pending_original_count;
    bool pending_is_ligature;
    uint8_t pending_character;
    /* The character held back was read as the font's \hyphenchar, so an
       empty discretionary follows it into the paragraph; see
       docs/DECISIONS.md, the-discretionary-after-an-explicit-hyphen. */
    bool pending_is_hyphen;
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
    /* The current page, and the list material is contributed to before the
       page builder moves it there; see docs/DECISIONS.md, the-page-builder. */
    struct hstex_vbox_builder *page_builder;
    struct hstex_vbox_builder *contribution_builder;
    /* The best break found on the current page so far, and what it costs. */
    bool output_active;
    int32_t least_page_cost;
    int32_t best_page_penalty;
    int32_t best_page_size;
    size_t best_page_break;
    /* How many pages \shipout has taken. */
    int32_t shipped_pages;
    /* The horizontal list of the paragraph being built, if any. */
    struct hstex_hbox_builder *paragraph_builder;
    bool building_paragraph;
    struct hstex_vbox_builder *active_vbox_builder;
    /* A box body runs on the live input and ends when the group the box
       opened ends. The executor stops and hands control back to the box
       builder when the level falls to group_stop_level. */
    uint32_t group_stop_level;
    bool group_stop_armed;
    bool group_stop_hit;
    /* \textfont, \scriptfont and \scriptscriptfont, by family, with the
       group level each was last set at so they restore like other registers. */
    uint32_t math_fonts[HSTEX_MATH_SIZE_COUNT][16];
    uint32_t math_font_levels[HSTEX_MATH_SIZE_COUNT][16];
    /* True while an alignment is reading its body, so that \cr and its
       relatives are recognised instead of being errors. */
    bool building_alignment;
    /* The entry being read, if any. */
    struct hstex_align_entry *alignment_entry;
    /* True while the tokens between \csname and \endcsname are expanded. */
    bool building_cs_name;
    /* True while the formula being read is a display. */
    bool displayed_math;
    /* The equation a display has already read, while its number is being
       read; which side the number goes on. */
    bool reading_equation_number;
    bool equation_number_on_left;
    struct hstex_box displayed_equation;
    /* How tall the delimiters of the \left group being set must be, or -1
       while the group is still being measured; see docs/DECISIONS.md,
       middle-delimiters. */
    int32_t middle_delimiter_size;
    /* Where \message writes; the standard output when this is null. */
    FILE *message_stream;
    /* A box or rule has reached the current page, so it is no longer empty;
       see docs/DECISIONS.md, whatsits-on-an-empty-page. */
    bool page_has_box;
    /* \immediate was just read, so the next output command acts now instead
       of leaving a whatsit behind; see docs/DECISIONS.md, whatsits. */
    bool immediate_pending;
    /* The box \leaders read, waiting for the glue that will repeat it. */
    uint32_t pending_leader;
    uint8_t pending_leader_kind;
    /* An alignment standing in for a whole display: its rows are gathered
       here until the closing $$ says what glue surrounds them. */
    bool display_alignment;
    struct hstex_vbox_builder *display_rows;
    struct hstex_vbox_builder *display_outer_vbox;
    /* What \badness reports about the box packed most recently. */
    int32_t badness;
    /* The math lists being built, innermost last; empty outside math. */
    /* Sub-formula lists, kept for the lifetime of the engine so that a field
       may refer to one without owning it. */
    struct hstex_noad *math_items;
    size_t math_item_count;
    size_t math_item_capacity;
    struct hstex_math_sublist *math_sublists;
    size_t math_sublist_count;
    size_t math_sublist_capacity;
    /* \parshape: every shape read is kept in an arena as a count followed by
       that many indent and length pairs. `parshape` is the one-based offset
       of the shape in force, or zero for none; see docs/DECISIONS.md,
       parshape. */
    int32_t *parshapes;
    size_t parshape_used;
    size_t parshape_capacity;
    uint32_t parshape;
    uint32_t parshape_level;
    /* How many lines the paragraph most recently broken came to, which is
       what decides the line a display sits on. */
    int32_t paragraph_lines;
    struct hstex_math_builder *math_stack;
    size_t math_depth;
    size_t math_capacity;
    /* Where the current math context starts. A box body opens a fresh one
       without disturbing the lists the enclosing formula is still holding. */
    size_t math_floor;
    /* Name of the primitive the executor is currently running, for
       diagnostics: a scan that fails names the command that asked for the
       value, which is otherwise invisible from inside the scanner. */
    char executing_name[64];
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
/* Run to the end of the input, building the main vertical list. Everything
   `hstex_engine_next_output` hands back belongs to that list. */
int hstex_engine_run(struct hstex_engine *engine,
                     struct hstex_source_location *last, char *error,
                     size_t error_capacity);
/* Where \message writes. NULL, the default, means the standard output. */
void hstex_engine_set_message_stream(struct hstex_engine *engine,
                                     FILE *stream);
const struct hstex_meaning *hstex_engine_meaning(
    const struct hstex_engine *engine, hstex_cs_id identifier);

#endif
