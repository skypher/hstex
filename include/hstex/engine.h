#ifndef HSTEX_ENGINE_H
#define HSTEX_ENGINE_H

#include "hstex/lex.h"
#include "hstex/source.h"
#include "hstex/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/* The tool that says where a file is, kept alive between questions. Starting
   it costs twelve milliseconds -- it reads the configuration and the file
   lists over again every time -- and a run of the corpus asks after nearly
   two hundred names. See docs/DECISIONS.md, finding-a-file. */
struct hstex_file_finder {
    FILE *questions;
    FILE *answers;
    /* The child, kept as a plain integer so that this header needs nothing
       from <sys/types.h>; zero where none is running. */
    int child;
    /* What the marker name answers with, which is how a name the tool did not
       find is told apart from one it did. */
    char *marker_answer;
    /* The state of the directory the tool was started in: it remembers what
       it found there, so a file the run has written since means starting it
       again. */
    uint64_t generation;
    /* Set where the tool cannot be kept alive at all, so that a run does not
       try again for every name. */
    bool broken;
};

/* What a run does with the nodes a page has left behind, beyond giving them
   back. Both of these answer the question "is the list of places that can
   still name a node complete?", which is a question about the engine rather
   than about a document, so a run is asked it by
   HSTEX_DEAD_NODES=trace or =poison rather than by the document. See
   docs/DECISIONS.md, what-a-page-leaves-behind. */
enum hstex_dead_node_check {
    HSTEX_DEAD_NODES_GIVEN_BACK = 0,
    /* Give nothing back, and say so where a node one walk did not reach is
       reached by the next. */
    HSTEX_DEAD_NODES_TRACED,
    /* Give nothing back, and write over every node no walk reached, so that
       a document that still comes out the same says none of them was
       wanted. */
    HSTEX_DEAD_NODES_POISONED
};

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
    HSTEX_COMMAND_MATH_ACCENT,
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
    HSTEX_COMMAND_PREV_GRAF,
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
    /* \mark and \marks, and the five texts they leave behind. */
    HSTEX_COMMAND_MARK,
    HSTEX_COMMAND_MARK_TEXT,
    HSTEX_COMMAND_INSERT,
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
    /* Whether \pdfcolorstackinit made this one, which is what decides
       whether a page begins by saying what colour it is in. See
       docs/DECISIONS.md, colour-on-a-page. */
    bool created;
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

/* Where a file the run asked after turned out to be, remembered so that the
   same question is not put to a child process again. A name that was not
   found is asked after afresh once the run has written a file, since that
   file may be the answer. See docs/DECISIONS.md, finding-a-file. */
struct hstex_resolved_file {
    char *name;
    char *path;
    uint64_t generation;
};

/* A growable run of tokens: what a macro's arguments are scanned into and
   what its expansion is built in. */
struct hstex_token_vector {
    hstex_token *data;
    size_t count;
    size_t capacity;
};

/* How long a body may be and still be copied where it is read rather than
   read where it stands. Reading it where it stands saves the copy but the
   definition must then be held until the frame has read it, and letting go
   of it again costs more than copying a few words. */
#define HSTEX_MACRO_COPY_LIMIT ((size_t)32)

/* How many arguments a macro may take, and how deep the engine keeps room
   for them. */
#define HSTEX_PARAMETER_LIMIT 9

/* One entry of the outline the document builds with \pdfoutline: the
   objects it was given as it was written, what the document declared about
   its children, and where the finished tree puts it. The five links hold the
   place of another entry in the list, or HSTEX_PDF_OUTLINE_NONE. See
   docs/DECISIONS.md, the-outline-of-a-document. */
#define HSTEX_PDF_OUTLINE_NONE ((size_t)-1)

struct hstex_pdf_outline {
    size_t object;
    size_t title;
    size_t action;
    char *attributes;
    int32_t count;
    int32_t visible;
    size_t parent;
    size_t previous;
    size_t next;
    size_t first;
    size_t last;
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

/* \nonscript leaves a marker in the list where it stood: a glue node with
   nothing in it, named after the primitive. See docs/DECISIONS.md, nonscript. */
#define HSTEX_GLUE_NONSCRIPT \
    ((uint8_t)HSTEX_GLUE_PARAMETER_COUNT + \
     (uint8_t)HSTEX_MUGLUE_PARAMETER_COUNT + 1U)

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
    HSTEX_MACRO_PROTECTED = 1U << 2U
};

/* What the engine has worked out about a macro for itself, which is nothing
   the definition said and nothing \ifx compares. */
enum hstex_macro_shape {
    /* The parameter text is nothing but `#1#2...` in order: no text in front
       of an argument and none between two of them, so every argument runs to
       one token or one group and a call need not read the parameter text at
       all. Nearly every macro a document defines is of that shape. */
    HSTEX_MACRO_PLAIN_PARAMETERS = 1U << 0U
};

struct hstex_macro {
    hstex_token *parameter_text;
    size_t parameter_count_tokens;
    hstex_token *replacement;
    size_t replacement_count;
    uint8_t parameter_count;
    uint8_t flags;
    /* See enum hstex_macro_shape. */
    uint8_t shape;
    /* What the body is made of, counted once when the definition is made
       rather than at every call: how many of its tokens stand for
       themselves, how often each argument is asked for, and how many
       arguments it asks for at all. Fifty-seven per cent of the corpus's
       macro calls have a body that asks for none, and those are read where
       they stand rather than copied. */
    uint32_t body_plain_count;
    uint8_t body_uses[HSTEX_PARAMETER_LIMIT];
    uint16_t body_parameter_total;
    /* How many meanings hold this definition: the ones control sequences
       have now and the ones the save stack is keeping for a group to end. A
       definition nothing holds any more is taken apart and its record used
       again. See docs/DECISIONS.md, a-definition-nothing-holds. */
    uint32_t references;
    /* The next record on the free list, one more than its index, or zero. */
    uint32_t next_free;
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
    /* What the metrics file says of itself, which the page description
       repeats so that the two can be checked against each other. */
    uint32_t checksum;
};

/* One movement the page description has already written, so that a movement
   of the same size can be written as a repeat; see docs/DECISIONS.md,
   the-page-description. */
/* A font the PDF file names, and the characters of it that were used. */
struct hstex_pdf_font {
    uint32_t identifier;
    /* The object the font dictionary will be, and the resource number it is
       named by. */
    size_t object;
    size_t widths;
    size_t descriptor;
    uint32_t number;
    uint32_t first;
    uint32_t last;
    uint8_t used[32];
};

struct hstex_dvi_move {
    int32_t value;
    size_t at;
    /* 0 for a plain down or right, 1 for the first register of the pair
       (y or w), 2 for the second (z or x). */
    uint8_t held;
    /* Which registers this movement may no longer be rewritten to use: a
       later movement between it and its own repeat has taken them. Bit one
       for the first register, bit two for the second. */
    uint8_t taken;
    uint32_t level;
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
    /* \left ... \right: a sub-formula between two delimiters, built when
       the list is set so that the style it lands in is known. */
    HSTEX_NOAD_FENCE,
    /* \mathaccent: the accent's mathchar is in `delimiter`. */
    HSTEX_NOAD_ACCENT,
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
    /* Whether any entry has claimed this column yet, so that the first one
       sets the width even when it is negative; see docs/DECISIONS.md,
       a-column-of-negative-width. */
    bool measured;
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
    /* What \prevdepth stood at when a \noalign finished, which is what the
       rows after it are spaced from; see docs/DECISIONS.md,
       prevdepth-inside-noalign. */
    int32_t prev_depth;
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
    /* The opening delimiter of a \left ... \right. */
    int32_t left_delimiter;
    /* This atom's nucleus was read as a character of a text font, because
       another character of the same family follows it. Its italic
       correction is then dropped; see docs/DECISIONS.md,
       math-text-characters. */
    bool text_character;
    /* \limits and \nolimits on a large operator: 0 for neither, which puts
       the limits above and below in display style; 1 for \limits; 2 for
       \nolimits. See docs/DECISIONS.md, large-operators. */
    uint8_t limits;
    /* \vcenter made this atom. It is set and spaced as an ordinary one, but
       it is not an ordinary atom: braces round it package it rather than
       give way to it. See docs/DECISIONS.md, a-vcenter-in-braces. */
    bool vcentered;
};

/* What one class of insertions has done to the page being built: how much of
   the class's material stands on it, whether one of its insertions had to be
   split, and where. See docs/DECISIONS.md, an-insertion-that-does-not-fit. */
struct hstex_page_insert {
    uint16_t number;
    int32_t held;
    bool split;
    uint32_t broken;
    uint32_t break_at;
};

/* The three texts one class of marks leaves behind, and the two a \vsplit
   leaves; see docs/DECISIONS.md, marks. */
struct hstex_mark_class {
    uint32_t number;
    uint32_t top;
    uint32_t first;
    uint32_t bot;
    uint32_t split_first;
    uint32_t split_bot;
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
    HSTEX_NODE_MARK,
    HSTEX_NODE_INSERT,
};

/* What a whatsit does when the page it sits on is shipped out. See
   docs/DECISIONS.md, whatsits. */
enum hstex_whatsit_kind {
    HSTEX_WHATSIT_WRITE = 0,
    HSTEX_WHATSIT_OPEN_OUT,
    HSTEX_WHATSIT_CLOSE_OUT,
    HSTEX_WHATSIT_SPECIAL,
    HSTEX_WHATSIT_COLOR_STACK,
    HSTEX_WHATSIT_PDF_DEST,
    HSTEX_WHATSIT_LITERAL,
    HSTEX_WHATSIT_START_LINK,
    HSTEX_WHATSIT_END_LINK,
    HSTEX_WHATSIT_ANNOT,
};

/* What a link or an outline entry does when it is followed. See
   docs/DECISIONS.md, pdf-links. */
enum hstex_pdf_action_kind {
    HSTEX_PDF_ACTION_GOTO = 0,
    HSTEX_PDF_ACTION_THREAD,
    HSTEX_PDF_ACTION_USER,
};

struct hstex_pdf_action {
    uint8_t kind;
    /* The file another document is in, the name of what is aimed at, and
       either the text of a user action or the one that follows a page
       number: token lists, or zero when they were not given. */
    uint32_t file;
    uint32_t name;
    uint32_t text;
    int32_t number;
    /* Whether a number was given rather than a name, and whether it was a
       page number rather than an object number. */
    bool numbered;
    bool paged;
    /* Whether the link said what window the file is to open in, and which
       it said. */
    bool windowed;
    bool new_window;
};

/* Where a destination takes the reader; the reference writes each of these
   as its own kind of array. See docs/DECISIONS.md, destinations-in-the-file. */
enum hstex_pdf_dest_kind {
    HSTEX_PDF_DEST_XYZ = 0,
    HSTEX_PDF_DEST_XYZ_ZOOM,
    HSTEX_PDF_DEST_FIT,
    HSTEX_PDF_DEST_FITH,
    HSTEX_PDF_DEST_FITV,
    HSTEX_PDF_DEST_FITB,
    HSTEX_PDF_DEST_FITBH,
    HSTEX_PDF_DEST_FITBV,
    HSTEX_PDF_DEST_FITR,
};

/* A destination the file knows by name or by number, and the object that
   holds it. */
struct hstex_pdf_dest {
    char *name;
    size_t length;
    int32_t number;
    size_t object;
    bool named;
    bool placed;
};

/* A destination on the page being shipped: where it stands, in scaled
   points measured the way the page is written. */
struct hstex_pdf_placement {
    size_t object;
    bool named;
    uint8_t kind;
    int32_t zoom;
    int32_t left;
    int32_t bottom;
    int32_t right;
    int32_t top;
};

/* One node of the tree the file lists its pages in: six pages to a node, and
   six nodes to the node above. See docs/DECISIONS.md, the-tree-of-pages. */
struct hstex_pdf_page_node {
    size_t object;
    size_t first;
    size_t pages;
};

/* An annotation the page carries: where it stands on the page and what it
   says. See docs/DECISIONS.md, annotations-on-a-page. */
struct hstex_pdf_annotation {
    size_t object;
    /* The corners, in scaled points, x from the left edge of the page and y
       up from its foot. */
    int32_t left;
    int32_t bottom;
    int32_t right;
    int32_t top;
    /* The text in front of the rectangle and the action behind it. */
    uint32_t attributes;
    uint32_t action;
    bool link;
    bool has_action;
};

/* The link a page is in the middle of: which object its next rectangle is
   to be, where that rectangle starts, and the list it belongs to. */
struct hstex_pdf_open_link {
    bool open;
    bool measuring;
    size_t object;
    uint32_t attributes;
    uint32_t action;
    int32_t level;
    int32_t start;
    int32_t base;
    int32_t height;
    int32_t depth;
    int32_t width;
    bool running_width;
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
            /* This box is the line a display formula was set as, which
               \showbox says so of; see docs/DECISIONS.md, display-math. */
            bool display;
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
            /* What this kern is for, when it is for anything in
               particular: 1 and 2 for the two margins a character may stick
               out past, 3 for the pair an accent rides between. See
               docs/DECISIONS.md, character-protrusion and accent-kerns. */
            uint8_t margin;
        } kern;
        struct {
            /* The token list \mark was given, already expanded once, and the
               class \marks was given; zero for plain \mark. See
               docs/DECISIONS.md, marks. */
            uint32_t tokens;
            uint16_t class_number;
        } mark;
        struct {
            /* The vertical list \insert was given, the class it belongs to,
               and the three things it remembers of where it was written; see
               docs/DECISIONS.md, insertions. */
            uint32_t node_start;
            uint32_t node_count;
            uint16_t number;
            struct hstex_glue split_top_skip;
            int32_t split_max_depth;
            int32_t float_cost;
        } insert;
        struct {
            /* An enum hstex_whatsit_kind. */
            uint8_t kind;
            /* The stream the reference stores: 0..15 as given, 16 for any
               larger number, 17 for a negative one. For a colour stack this
               is the stack's number instead, and `action` says which of the
               four things is being done to it. */
            uint8_t stream;
            uint8_t action;
            /* The unexpanded text of a \write, the already expanded text of
               a \special, or the file name of an \openout. For a pdf
               destination this is its name, and `stream` says whether it has
               one at all; `number` is the number it has instead, and `detail`
               holds the destination type as it is written out. */
            uint32_t tokens;
            uint32_t detail;
            int32_t number;
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
    /* The first record no meaning holds any more, one more than its index. */
    uint32_t macro_free_list;
    /* How many definitions have been made and how many records they needed,
       which is what the driver reports. */
    size_t macro_definitions;
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
    /* The colour stacks as the pages are written, which is not where they
       stand as the document is read. */
    struct hstex_color_stack *pdf_colours;
    size_t pdf_colour_count;
    /* What the links and outlines of the document aim at. */
    struct hstex_pdf_action *pdf_actions;
    size_t pdf_action_count;
    size_t pdf_action_capacity;
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
    /* The page description being written, when \pdfoutput asks for one; see
       docs/DECISIONS.md, the-page-description. */
    FILE *dvi_file;
    uint8_t *dvi_page;
    size_t dvi_page_count;
    size_t dvi_page_capacity;
    size_t dvi_written;
    int32_t dvi_last_bop;
    int32_t dvi_max_v;
    int32_t dvi_max_h;
    uint32_t dvi_max_push;
    uint32_t dvi_push_depth;
    uint32_t dvi_pages;
    struct hstex_dvi_move *dvi_downs;
    size_t dvi_down_count;
    size_t dvi_down_capacity;
    struct hstex_dvi_move *dvi_rights;
    size_t dvi_right_count;
    size_t dvi_right_capacity;
    uint32_t *dvi_fonts;
    size_t dvi_font_count;
    size_t dvi_font_capacity;
    int32_t dvi_font;
    int32_t dvi_h;
    int32_t dvi_v;
    int32_t dvi_cur_h;
    int32_t dvi_cur_v;
    /* The PDF file, when \pdfoutput is positive. See docs/DECISIONS.md,
       the-pdf-file. */
    FILE *pdf_file;
    size_t pdf_written;
    /* Where each object was written, by number; zero until it is. */
    size_t *pdf_offsets;
    size_t pdf_offset_capacity;
    /* What the page being shipped is to carry, and the link it is in the
       middle of. */
    struct hstex_pdf_annotation *pdf_annots;
    size_t pdf_annot_count;
    size_t pdf_annot_capacity;
    struct hstex_pdf_placement *pdf_places;
    size_t pdf_place_count;
    size_t pdf_place_capacity;
    struct hstex_pdf_dest *pdf_dests;
    size_t pdf_dest_count;
    size_t pdf_dest_capacity;
    /* Objects kept for pages a link aims at before they are shipped. */
    size_t *pdf_future_pages;
    size_t pdf_future_page_count;
    size_t pdf_future_page_capacity;
    size_t pdf_first_page;
    struct hstex_pdf_open_link pdf_link;
    int32_t pdf_level;
    /* The page description being built. */
    uint8_t *pdf_page;
    size_t pdf_page_count;
    size_t pdf_page_capacity;
    size_t pdf_pages_object;
    /* What the file does when it is opened, if the document named it. */
    size_t pdf_open_action;
    /* Room kept for the name a \csname is building, so that the usual one
       takes no storage of its own; a name built inside another takes its
       own. */
    uint8_t *cs_name_scratch;
    size_t cs_name_scratch_capacity;
    bool cs_name_scratch_busy;
    /* Where the destination of each name is, one more than its place, so
       that finding one need not walk all of them: the corpus has 23,513. */
    uint32_t *pdf_dest_slots;
    size_t pdf_dest_slot_capacity;
    /* What kpsewhich said about the files the run has asked after, and how
       many files the run has written since. */
    struct hstex_resolved_file *resolved_files;
    size_t resolved_file_count;
    size_t resolved_file_capacity;
    uint64_t file_generation;
    /* Room kept for the arguments of the macro being expanded, so that the
       storage one call takes serves the next rather than being given back
       and taken again; a call that finds it busy takes its own. */
    /* Where the arguments of the macro call being made are gathered. It is a
       stack: a call takes what it needs from the top and gives it back when
       it is done, so a call made while another is being made -- which
       nothing in an argument scan can bring about, since it expands nothing
       -- would still find room of its own. */
    struct hstex_token_vector argument_arena;
    /* The outline the document builds, in the order it was written, and the
       root the catalogue points at once it is finished. */
    size_t pdf_outline_object;
    struct hstex_pdf_outline *pdf_outlines;
    size_t pdf_outline_count;
    size_t pdf_outline_capacity;
    struct hstex_pdf_page_node *pdf_page_nodes;
    size_t pdf_page_node_count;
    size_t pdf_page_node_capacity;
    size_t *pdf_page_objects;
    size_t pdf_page_object_count;
    size_t pdf_page_object_capacity;
    struct hstex_pdf_font *pdf_fonts;
    size_t pdf_font_count;
    size_t pdf_font_capacity;
    uint32_t *pdf_page_fonts;
    size_t pdf_page_font_count;
    size_t pdf_page_font_capacity;
    /* Where the text the page has so far reaches, and what is open. */
    bool pdf_in_text;
    bool pdf_in_array;
    bool pdf_in_string;
    bool pdf_placed;
    int64_t pdf_origin_h;
    int64_t pdf_origin_v;
    /* Where the file's text stands after the place it last named, in scaled
       points: the printed place, taken towards the engine's own. */
    int32_t pdf_line_h;
    int32_t pdf_line_v;
    int32_t pdf_text_h;
    int32_t pdf_text_v;
    uint32_t pdf_text_font;
    bool pdf_font_chosen;
    int32_t pdf_height;
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
    /* What the page builder last took off the contribution list, which
       is what \lastnodetype and its relatives report once that list is
       empty again; see docs/DECISIONS.md, the-last-node-of-a-page. */
    struct hstex_node page_last_node;
    bool page_last_taken;
    /* The marks of the page being built and of the one before it, as stored
       token lists: \topmark is what the page before ended with, \firstmark
       and \botmark the first and last of this one. Zero is an empty text.
       Class marks (\marks) keep the same three each. See
       docs/DECISIONS.md, marks. */
    struct hstex_mark_class *mark_classes;
    size_t mark_class_count;
    size_t mark_class_capacity;
    /* The page's goal has been settled: the first box or insertion has
       reached it, and \vsize is not read again until it is shipped. See
       docs/DECISIONS.md, an-insertion-that-does-not-fit. */
    bool page_frozen;
    /* One record for each class of insertions that has reached the page
       being built. */
    struct hstex_page_insert *page_inserts;
    size_t page_insert_count;
    size_t page_insert_capacity;
    /* \immediate was just read, so the next output command acts now instead
       of leaving a whatsit behind; see docs/DECISIONS.md, whatsits. */
    bool immediate_pending;
    /* The paragraph being broken is being broken because a display formula
       is starting, so \displaywidowpenalty stands in for \widowpenalty;
       see docs/DECISIONS.md, display-math. */
    bool breaking_for_display;
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
    /* \prevgraf: how many lines the paragraph has behind it, which a
       display adds three to. The breaker numbers its lines from here.
       See docs/DECISIONS.md, lines-carry-on-past-a-display. */
    int32_t prev_graf;
    /* What \prevdepth stood at when a displayed alignment's rows were
       gathered, which is what follows them; see docs/DECISIONS.md,
       prevdepth-inside-noalign. */
    int32_t display_prev_depth;
    /* How far along the line the diagnostic stream stands, so that it can
       be broken where the reference breaks it and a message can keep its
       distance from what is already there; see docs/DECISIONS.md,
       the-print-line. */
    int32_t message_column;
    struct hstex_math_builder *math_stack;
    size_t math_depth;
    size_t math_capacity;
    /* Where the current math context starts. A box body opens a fresh one
       without disturbing the lists the enclosing formula is still holding. */
    size_t math_floor;
    /* The primitive the executor is currently running, for diagnostics: a
       scan that fails names the command that asked for the value, which is
       otherwise invisible from inside the scanner. What is kept is the token
       rather than its name: the corpus runs twenty-five million primitives
       and asks for the name only where one of them fails. */
    hstex_token executing_token;
    char executing_name[64];
    /* Which entry of `pdf_fonts` a font identifier belongs to, one more than
       its place, or zero where it has not been asked for yet. Two fonts
       loaded from the same file are one font in the file, and finding that
       out means comparing names, which every glyph run of every page would
       otherwise do over again. */
    uint32_t *pdf_font_places;
    size_t pdf_font_place_capacity;
    struct hstex_file_finder finder;
    /* How many pages had been shipped when the node arenas were last given
       back; see docs/DECISIONS.md, what-a-page-leaves-behind. */
    int32_t compacted_pages;
    /* The page the boundary work last ran for: digests, checkpoints and
       parked chunks happen at every page, while compaction takes a stride
       of its own. */
    int32_t last_page_boundary;
    /* Which nodes the last walk over the roots did not reach, kept only
       while a run is being asked whether that list of roots is complete. */
    uint8_t *dead_nodes;
    size_t dead_node_count;
    /* Whether this run is being asked that question, and how. */
    enum hstex_dead_node_check dead_node_check;
    /* What has been written to the PDF file but not yet handed to it. */
    uint8_t *pdf_out_buffer;
    size_t pdf_out_count;
    /* How many of the driver's loops are running inside one another: a box
       body is read on the live input, so the loop runs again inside itself
       while one is being built. */
    size_t output_depth;
    /* The page after which the run is to be taken up by another process, or
       zero, and how many pages later to do it again. See docs/DECISIONS.md,
       a-checkpoint-inside-a-file. */
    int32_t checkpoint_page;
    int32_t checkpoint_stride;
    /* Chunks parked at page boundaries and released together; see
       docs/DECISIONS.md, a-checkpoint-inside-a-file. */
    int32_t parallel_chunk;
    int32_t parallel_stop;
    int parallel_is_worker;
    /* A chunk waits on a gate of its own, which the run above keeps the
       other end of, so that the fleet can be let go more than once: a chunk
       leaves a replacement of itself parked on the same gate before it does
       any work. */
    int parallel_gate_read;
    int parallel_gates[256];
    /* What a chunk says when it is finished. A chunk's replacement is a
       child of the chunk and not of the run above, so the run counts them
       here rather than waiting for them by name. */
    int parallel_done_read;
    int parallel_done_write;
    int parallel_workers;
    /* Where each chunk was parked, so the release can tell each one where
       the next begins; and the clock that parks by time. */
    int32_t parallel_pages[256];
    uint64_t parallel_stride_ns;
    uint64_t parallel_parked_at;
    /* The file of assignments a woken chunk reads before it runs: the guess
       at how the run it now serves differs from the one that parked it. */
    char parallel_patch[4096];
    /* Where each write stream stood when this chunk was parked. A forked
       copy's ftell can read the shared descriptor's offset, which the run
       above goes on moving, so the positions are taken at the fork. */
    long parallel_reached[16];
    /* The relay: one run at a time carries the truth. A parked chunk whose
       patched beginning the carrier's state matches takes over; a chunk it
       does not match is overrun and rewritten. See docs/DECISIONS.md,
       the-relay. */
    int speculating;
    int spec_carrier;
    int spec_finished;
    int verifying;
    int32_t spec_start;
    char spec_dir[512];
    int32_t *spec_pages;
    size_t spec_page_count;
    int parallel_one_shot;
    /* The children of one language's trie root, spread by character: every
       walk starts at the root, and the root has the most children, so the
       list scan is paid once and remembered. Rebuilt when the trie grows or
       the language changes. */
    uint32_t hyphen_dispatch[256];
    int32_t hyphen_dispatch_language;
    size_t hyphen_dispatch_nodes;
    /* Where in the file this chunk's own bytes begin, and the file it writes
       them into. A chunk inherits the byte count the run had reached where
       it was parked, which is exactly where the chunk before it stops. */
    size_t parallel_offset;
    size_t parallel_first_offset;
    int parallel_redirect;
    /* What each open write stream was opened as, so that a chunk running
       beside others can take up the same file at the place its own bytes
       begin. Kept here, at the end, rather than beside the streams: what a
       run reads on its hot path should not be pushed about by it. */
    char *write_stream_paths[16];
};

int hstex_engine_init(struct hstex_engine *engine, char *error,
                      size_t error_capacity);
void hstex_engine_destroy(struct hstex_engine *engine);

/* The engine's state once the format source has been read, put by so that
   the next run need not read it again, and read back into a fresh engine.
   See docs/DECISIONS.md, the-format-a-run-starts-from. */
int hstex_engine_write_format(struct hstex_engine *engine, const char *path,
                              char *error, size_t error_capacity);
int hstex_engine_read_format(struct hstex_engine *engine, const char *path,
                             char *error, size_t error_capacity);
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
int hstex_engine_hyphenate_word(struct hstex_engine *engine,
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
