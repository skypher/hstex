#include "hstex/input.h"
#include "hstex/engine.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/scan.h"
#include "hstex/symbol.h"
#include "hstex/token.h"
#include "hstex_config.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

static void print_usage(FILE *stream, const char *program)
{
    (void)fprintf(stream,
                  "usage: %s --version\n"
                  "       %s --cpu-features\n"
                  "       %s --probe-input FILE\n"
                  "       %s --trace-ini-mouth FILE\n"
                  "       %s --mouth-stats-latex FILE\n"
                  "       %s --run-ini FILE\n"
                  "       %s --run-latex LATEX_LTX DOCUMENT\n"
                  "       %s --make-format LATEX_LTX FORMAT\n"
                  "       %s --format FORMAT DOCUMENT\n",
                  program, program, program, program, program, program, program,
                  program, program);
}

static uint64_t fnv1a64(const uint8_t *data, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int probe_input(const char *path)
{
    struct hstex_input input;
    char error[512];
    if (hstex_input_open(path, &input, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }

    size_t offset = 0U;
    size_t boundary_count = 0U;
    while (offset < input.length) {
        size_t run = hstex_scan_default_boundary(input.data + offset,
                                                 input.length - offset);
        offset += run;
        if (offset < input.length) {
            ++boundary_count;
            ++offset;
        }
    }

    uint64_t hash = fnv1a64(input.data, input.length);
    (void)printf("path=%s\n", path);
    (void)printf("bytes=%zu\n", input.length);
    (void)printf("storage=%s\n", hstex_input_storage_name(input.storage));
    (void)printf("scan_backend=%s\n", hstex_scan_backend());
    (void)printf("default_boundaries=%zu\n", boundary_count);
    (void)printf("fnv1a64=%016" PRIx64 "\n", hash);

    hstex_input_close(&input);
    return 0;
}

static void print_name_hex(const uint8_t *name, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        (void)printf("%02x", (unsigned int)name[index]);
    }
}

static int trace_ini_mouth(const char *path)
{
    struct hstex_input input;
    char error[512] = {0};
    if (hstex_input_open(path, &input, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    struct hstex_lexical_state lexical_state;
    if (hstex_lexical_state_init(&lexical_state, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_input_close(&input);
        return 1;
    }
    struct hstex_mouth mouth;
    hstex_mouth_init(&mouth, input.data, input.length, &lexical_state);

    int exit_status = 0;
    for (;;) {
        hstex_token token;
        struct hstex_source_location location;
        enum hstex_mouth_result result = hstex_mouth_next(
            &mouth, &token, &location, error, sizeof(error));
        if (result == HSTEX_MOUTH_EOF) {
            break;
        }
        if (result == HSTEX_MOUTH_ERROR) {
            (void)fprintf(stderr, "hstex: %s\n", error);
            exit_status = 1;
            break;
        }
        (void)printf("%u:%u ", location.line, location.column);
        if (hstex_token_is_character(token)) {
            (void)printf("char cat=%u code=%u\n",
                         (unsigned int)hstex_token_category(token),
                         (unsigned int)hstex_token_character_code(token));
            continue;
        }
        if (hstex_token_is_control_sequence(token)) {
            enum hstex_symbol_kind kind;
            const uint8_t *name = NULL;
            size_t length = 0U;
            hstex_cs_id identifier = hstex_token_control_sequence_id(token);
            if (hstex_symbol_name(&lexical_state.symbols, identifier, &kind,
                                  &name, &length) != 0) {
                (void)fprintf(stderr, "hstex: invalid control-sequence token\n");
                exit_status = 1;
                break;
            }
            (void)printf("control id=%u kind=%s name_hex=",
                         (unsigned int)identifier,
                         kind == HSTEX_SYMBOL_ACTIVE ? "active" : "regular");
            print_name_hex(name, length);
            (void)putchar('\n');
            continue;
        }
        (void)printf("parameter number=%u\n",
                     (unsigned int)hstex_token_parameter_number(token));
    }

    hstex_mouth_destroy(&mouth);
    hstex_lexical_state_destroy(&lexical_state);
    hstex_input_close(&input);
    return exit_status;
}

static int apply_latex_document_catcodes(struct hstex_catcode_table *catcodes)
{
    static const struct {
        uint8_t character;
        uint8_t category;
    } assignments[] = {
        {(uint8_t)'{', (uint8_t)HSTEX_CAT_BEGIN_GROUP},
        {(uint8_t)'}', (uint8_t)HSTEX_CAT_END_GROUP},
        {(uint8_t)'$', (uint8_t)HSTEX_CAT_MATH_SHIFT},
        {(uint8_t)'&', (uint8_t)HSTEX_CAT_ALIGNMENT_TAB},
        {(uint8_t)'#', (uint8_t)HSTEX_CAT_PARAMETER},
        {(uint8_t)'^', (uint8_t)HSTEX_CAT_SUPERSCRIPT},
        {UINT8_C(11), (uint8_t)HSTEX_CAT_SUPERSCRIPT},
        {(uint8_t)'_', (uint8_t)HSTEX_CAT_SUBSCRIPT},
        {UINT8_C(1), (uint8_t)HSTEX_CAT_SUBSCRIPT},
        {(uint8_t)'\t', (uint8_t)HSTEX_CAT_SPACE},
        {(uint8_t)'~', (uint8_t)HSTEX_CAT_ACTIVE},
        {UINT8_C(12), (uint8_t)HSTEX_CAT_ACTIVE},
    };
    for (size_t index = 0U; index < sizeof(assignments) / sizeof(assignments[0]);
         ++index) {
        if (hstex_catcode_set(catcodes, assignments[index].character,
                              assignments[index].category) != 0) {
            return -1;
        }
    }
    return 0;
}

static int mouth_stats_latex(const char *path)
{
    struct hstex_input input;
    char error[512] = {0};
    if (hstex_input_open(path, &input, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    struct hstex_lexical_state lexical_state;
    if (hstex_lexical_state_init(&lexical_state, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_input_close(&input);
        return 1;
    }
    if (apply_latex_document_catcodes(&lexical_state.catcodes) != 0) {
        (void)fprintf(stderr, "hstex: could not install probe catcodes\n");
        hstex_lexical_state_destroy(&lexical_state);
        hstex_input_close(&input);
        return 1;
    }
    struct hstex_mouth mouth;
    hstex_mouth_init(&mouth, input.data, input.length, &lexical_state);

    size_t token_count = 0U;
    size_t character_count = 0U;
    size_t control_count = 0U;
    int exit_status = 0;
    for (;;) {
        hstex_token token;
        struct hstex_source_location location;
        enum hstex_mouth_result result = hstex_mouth_next(
            &mouth, &token, &location, error, sizeof(error));
        if (result == HSTEX_MOUTH_EOF) {
            break;
        }
        if (result == HSTEX_MOUTH_ERROR) {
            (void)fprintf(stderr, "hstex: %s\n", error);
            exit_status = 1;
            break;
        }
        ++token_count;
        if (hstex_token_is_character(token)) {
            ++character_count;
        } else if (hstex_token_is_control_sequence(token)) {
            ++control_count;
        }
    }
    if (exit_status == 0) {
        (void)printf(
            "path=%s bytes=%zu lines=%u tokens=%zu characters=%zu controls=%zu "
            "symbols=%zu\n",
            path, input.length, (unsigned int)mouth.line_number, token_count,
            character_count,
            control_count, lexical_state.symbols.entry_count);
    }

    hstex_mouth_destroy(&mouth);
    hstex_lexical_state_destroy(&lexical_state);
    hstex_input_close(&input);
    return exit_status;
}

static int run_ini(const char *path)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init(&engine, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    static const char output_directory[] = "build/ini-output";
    if (mkdir(output_directory, 0700) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "hstex: cannot create %s\n", output_directory);
        hstex_engine_destroy(&engine);
        return 1;
    }
    if (hstex_engine_set_output_directory(&engine, output_directory, error,
                                          sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    if (hstex_engine_push_file(&engine, path, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t output_tokens = 0U;
    struct hstex_source_location last_location = {0};
    int status = 0;
    if (hstex_engine_run(&engine, &last_location, error, sizeof(error)) != 0) {
        const char *source = hstex_source_current_name(&engine.sources);
        (void)fprintf(stderr, "hstex: %s:%u:%u: %s\n",
                      source == NULL ? path : source, last_location.line,
                      last_location.column, error);
        status = 1;
    }
    if (status == 0) {
        (void)printf(
            "path=%s pages=%d output_tokens=%zu symbols=%zu macros=%zu"
            " definitions=%zu\n",
            path, engine.shipped_pages, output_tokens,
            engine.lexical_state.symbols.entry_count, engine.macro_count,
            engine.macro_definitions);
    }
    hstex_engine_destroy(&engine);
    return status;
}

static int drain_engine(struct hstex_engine *engine, const char *fallback_path,
                        size_t *output_tokens)
{
    char error[512] = {0};
    struct hstex_source_location last_location = {0};
    *output_tokens = 0U;
    if (hstex_engine_run(engine, &last_location, error, sizeof(error)) != 0) {
        const char *source = hstex_source_current_name(&engine->sources);
        (void)fprintf(stderr, "hstex: %s:%u:%u: %s\n",
                      source == NULL ? fallback_path : source,
                      last_location.line, last_location.column, error);
        return 1;
    }
    return 0;
}

/* The format source, read and put by so that a run can start from it. */
static int make_format(const char *format_path, const char *format_file)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init(&engine, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    static const char config_name[] = "pdftexconfig.tex";
    if (hstex_engine_push_file(&engine, format_path, error, sizeof(error)) !=
            0 ||
        hstex_engine_push_input(&engine, config_name, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t format_output_tokens = 0U;
    if (drain_engine(&engine, format_path, &format_output_tokens) != 0 ||
        !engine.dump_requested) {
        (void)fprintf(stderr, "hstex: format source ended without dump\n");
        hstex_engine_destroy(&engine);
        return 1;
    }
    if (hstex_engine_write_format(&engine, format_file, error,
                                  sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    (void)printf("format=%s written=%s symbols=%zu macros=%zu\n", format_path,
                 format_file, engine.lexical_state.symbols.entry_count,
                 engine.macro_count);
    hstex_engine_destroy(&engine);
    return 0;
}

static int run_document_from_format(const char *format_file,
                                    const char *document_path)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init(&engine, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    static const char output_directory[] = "build/document-output";
    if ((mkdir(output_directory, 0700) != 0 && errno != EEXIST) ||
        hstex_engine_set_output_directory(&engine, output_directory, error,
                                          sizeof(error)) != 0 ||
        hstex_engine_read_format(&engine, format_file, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "hstex: %s\n",
                      error[0] == '\0' ? "cannot prepare document output"
                                       : error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    if (hstex_engine_begin_job(&engine, document_path, error, sizeof(error)) !=
        0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t document_output_tokens = 0U;
    int status = drain_engine(&engine, document_path, &document_output_tokens);
    if (status == 0) {
        (void)printf(
            "document=%s pages=%d output_tokens=%zu symbols=%zu macros=%zu"
            " definitions=%zu\n",
            document_path, engine.shipped_pages, document_output_tokens,
            engine.lexical_state.symbols.entry_count, engine.macro_count,
            engine.macro_definitions);
    }
    hstex_engine_destroy(&engine);
    return status;
}

static int run_latex(const char *format_path, const char *document_path)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init(&engine, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    static const char output_directory[] = "build/document-output";
    /* pdflatex.ini builds the format from pdftexconfig.tex followed by
       latex.ltx. The format source is pushed first so that the configuration
       stacked on top of it runs first. */
    static const char config_name[] = "pdftexconfig.tex";
    if ((mkdir(output_directory, 0700) != 0 && errno != EEXIST) ||
        hstex_engine_set_output_directory(&engine, output_directory, error,
                                          sizeof(error)) != 0 ||
        hstex_engine_push_file(&engine, format_path, error, sizeof(error)) != 0 ||
        hstex_engine_push_input(&engine, config_name, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "hstex: %s\n",
                      error[0] == '\0' ? "cannot prepare document output"
                                        : error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t format_output_tokens = 0U;
    if (drain_engine(&engine, format_path, &format_output_tokens) != 0 ||
        !engine.dump_requested) {
        if (!engine.dump_requested) {
            (void)fprintf(stderr,
                          "hstex: format source ended without dump\n");
        }
        hstex_engine_destroy(&engine);
        return 1;
    }
    (void)printf("format=%s output_tokens=%zu symbols=%zu macros=%zu"
                 " definitions=%zu\n",
                 format_path, format_output_tokens,
                 engine.lexical_state.symbols.entry_count, engine.macro_count,
                 engine.macro_definitions);
    if (hstex_engine_begin_job(&engine, document_path, error, sizeof(error)) !=
        0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t document_output_tokens = 0U;
    int status =
        drain_engine(&engine, document_path, &document_output_tokens);
    if (status == 0) {
        (void)printf(
            "document=%s pages=%d output_tokens=%zu symbols=%zu macros=%zu"
            " definitions=%zu\n",
            document_path, engine.shipped_pages, document_output_tokens,
            engine.lexical_state.symbols.entry_count, engine.macro_count,
            engine.macro_definitions);
    }
    hstex_engine_destroy(&engine);
    return status;
}

/* A run takes and gives back the room for eight million definitions, and the
   allocator's own answer to that is to hand the pages back to the system and
   ask for them again. It is told to keep them instead: the run reaches the
   same peak either way, and stops paying for the ground it has already
   walked. */
static void keep_the_heap(void)
{
#ifdef __GLIBC__
    (void)mallopt(M_TRIM_THRESHOLD, INT_MAX);
    (void)mallopt(M_TOP_PAD, 32 * 1024 * 1024);
    (void)mallopt(M_MMAP_THRESHOLD, 32 * 1024 * 1024);
#endif
}

int main(int argument_count, char **arguments)
{
    keep_the_heap();
    if (argument_count == 2 && strcmp(arguments[1], "--version") == 0) {
        (void)printf("hstex %s\n", HSTEX_VERSION);
        return 0;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--cpu-features") == 0) {
        (void)printf("scan_backend=%s\n", hstex_scan_backend());
        return 0;
    }
    if (argument_count == 3 && strcmp(arguments[1], "--probe-input") == 0) {
        return probe_input(arguments[2]);
    }
    if (argument_count == 3 &&
        strcmp(arguments[1], "--trace-ini-mouth") == 0) {
        return trace_ini_mouth(arguments[2]);
    }
    if (argument_count == 3 &&
        strcmp(arguments[1], "--mouth-stats-latex") == 0) {
        return mouth_stats_latex(arguments[2]);
    }
    if (argument_count == 3 && strcmp(arguments[1], "--run-ini") == 0) {
        return run_ini(arguments[2]);
    }
    if (argument_count == 4 && strcmp(arguments[1], "--run-latex") == 0) {
        return run_latex(arguments[2], arguments[3]);
    }
    if (argument_count == 4 && strcmp(arguments[1], "--make-format") == 0) {
        return make_format(arguments[2], arguments[3]);
    }
    if (argument_count == 4 && strcmp(arguments[1], "--format") == 0) {
        return run_document_from_format(arguments[2], arguments[3]);
    }

    print_usage(stderr, arguments[0]);
    return 2;
}
