#include "hstex/input.h"
#include "hstex/engine.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/scan.h"
#include "hstex/symbol.h"
#include "hstex/token.h"
#include "hstex_config.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

static void print_usage(FILE *stream, const char *program)
{
    (void)fprintf(stream,
                  "usage: %s [-h|--help]\n"
                  "       %s --version\n"
                  "       %s --cpu-features\n"
                  "       %s --probe-input FILE\n"
                  "       %s --trace-ini-mouth FILE\n"
                  "       %s --mouth-stats-latex FILE\n"
                  "       %s --run-ini FILE\n"
                  "       %s --run-latex LATEX_LTX DOCUMENT\n"
                  "       %s --make-format LATEX_LTX FORMAT\n"
                  "       %s --make-ini-format SOURCE FORMAT\n"
                  "       %s --format FORMAT DOCUMENT\n"
                  "       %s --format-no-shell FORMAT DOCUMENT\n"
                  "       %s --format-output FORMAT DOCUMENT DIRECTORY [JOBNAME]\n"
                  "       %s --format-output-no-shell FORMAT DOCUMENT DIRECTORY [JOBNAME]\n",
                  program, program, program, program, program, program, program,
                  program, program, program, program, program, program, program);
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

/* The format source, read and put by so that a run can start from it.

   WHICH ENGINE THE FORMAT IS FOR IS THE CALLER'S TO SAY. A LaTeX format is
   built by an eTeX-enabled engine, because latex.ltx reaches \count256 as
   soon as it finds \marks and that is a register only such an engine has;
   the capacity is written into the format, so a run reading it back gets the
   same pool whatever it started with. A format built from a TeX82 source
   must NOT be, or the source is read by an engine the reference would not
   have used: `\toksdef\tokens=256' is a register the reference has not got,
   and it says so and uses zero, while an eTeX engine takes it. trip does
   exactly that on its line 29, and the format its second pass reads then
   disagrees with the first pass over what \tokens means. */
static int make_format(const char *format_path, const char *format_file,
                       bool extended)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init_extended(&engine, extended, error, sizeof(error)) !=
        0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    /* pdftexconfig.tex selects PDF output for the LaTeX format. A TeX82
       INITEX format such as trip is built from its source alone, leaving the
       output mode at the engine default. */
    static const char config_name[] = "pdftexconfig.tex";
    if (hstex_engine_push_file(&engine, format_path, error, sizeof(error)) !=
            0 ||
        (extended &&
         hstex_engine_push_input(&engine, config_name, error,
                                 sizeof(error)) != 0)) {
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
                                    const char *document_path,
                                    const char *output_directory,
                                    const char *job_name,
                                    bool restricted_shell_escape,
                                    int *pages_out)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init(&engine, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    if ((mkdir(output_directory, 0700) != 0 && errno != EEXIST) ||
        hstex_engine_set_output_directory(&engine, output_directory, error,
                                          sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n",
                      error[0] == '\0' ? "cannot prepare document output"
                                       : error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    /* A preamble put by from an earlier run of this document is taken up
       where HSTEX_PREAMBLE_CKPT names one that exists: the engine's state
       from just before the .aux was first read, the class and packages
       already obeyed. Where taking it up fails for any reason the run
       starts from the format as it always did, so a stale or unreadable
       file costs the preamble and never the document. */
    bool resumed = false;
    const char *preamble = getenv("HSTEX_PREAMBLE_CKPT");
    if (preamble != NULL && preamble[0] != '\0' &&
        access(preamble, R_OK) == 0) {
        if (hstex_engine_resume_checkpoint(&engine, preamble, error,
                                           sizeof(error)) == 0 &&
            hstex_engine_set_restricted_shell_escape(
                &engine, restricted_shell_escape, error, sizeof(error)) == 0) {
            resumed = true;
            (void)fprintf(stderr, "hstex: preamble taken up from %s\n",
                          preamble);
        } else {
            (void)fprintf(stderr,
                          "hstex: preamble at %s not taken up (%s); "
                          "reading it afresh\n",
                          preamble, error[0] == '\0' ? "unreadable" : error);
            hstex_engine_destroy(&engine);
            error[0] = '\0';
            if (hstex_engine_init(&engine, error, sizeof(error)) != 0 ||
                hstex_engine_set_output_directory(&engine, output_directory,
                                                  error, sizeof(error)) != 0) {
                (void)fprintf(stderr, "hstex: %s\n", error);
                hstex_engine_destroy(&engine);
                return 1;
            }
        }
    }
    if (!resumed &&
        (hstex_engine_read_format(&engine, format_file, error, sizeof(error)) !=
             0 ||
         hstex_engine_set_restricted_shell_escape(
             &engine, restricted_shell_escape, error, sizeof(error)) != 0 ||
         hstex_engine_begin_job(&engine, document_path, error,
                                sizeof(error)) != 0)) {
        (void)fprintf(stderr, "hstex: %s\n",
                      error[0] == '\0' ? "cannot prepare document output"
                                       : error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    if (job_name != NULL &&
        hstex_engine_set_job_name(&engine, job_name, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t document_output_tokens = 0U;
    int status = drain_engine(&engine, document_path, &document_output_tokens);
    /* WHAT A RUN CAME TO IS ITS EXIT STATUS. The history is 0 where nothing
       was wrong, 1 where something was worth a warning, 2 where an error was
       reported and recovered from, and 3 where the run gave up. The reference
       fails for the last two and succeeds for the first two, whatever
       interaction mode it was in: measured, an undefined control sequence
       exits 1 under -interaction=nonstopmode and an overfull box exits 0. */
    if (status == 0 && engine.history >= 2) {
        status = 1;
    }
    if (status == 0) {
        if (pages_out != NULL) {
            *pages_out = engine.shipped_pages;
        }
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

/* Take a document up from a mid-run checkpoint: a fresh engine reads the whole
   run's state and reading position, then finishes the document into its own
   PDF in `output_directory' under `job_name'. What it writes are the pages
   from the checkpoint on -- the tail of the run that the checkpoint parked. */
static int resume_checkpoint_document(const char *checkpoint_file,
                                      const char *output_directory,
                                      const char *job_name)
{
    char error[512] = {0};
    struct hstex_engine engine;
    if (hstex_engine_init(&engine, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }
    if ((mkdir(output_directory, 0700) != 0 && errno != EEXIST) ||
        hstex_engine_set_output_directory(&engine, output_directory, error,
                                          sizeof(error)) != 0 ||
        hstex_engine_resume_checkpoint(&engine, checkpoint_file, error,
                                       sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n",
                      error[0] == '\0' ? "cannot resume checkpoint" : error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    if (job_name != NULL &&
        hstex_engine_set_job_name(&engine, job_name, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t tokens = 0U;
    int status = drain_engine(&engine, checkpoint_file, &tokens);
    if (status == 0) {
        (void)printf("resumed=%s pages=%d symbols=%zu macros=%zu\n",
                     checkpoint_file, engine.shipped_pages,
                     engine.lexical_state.symbols.entry_count,
                     engine.macro_count);
    }
    hstex_engine_destroy(&engine);
    return status;
}

static int run_latex(const char *format_path, const char *document_path)
{
    char error[512] = {0};
    struct hstex_engine engine;
    /* A LaTeX run is a run under an eTeX-enabled format, which is where the
       larger register pool comes from: latex.ltx reaches \count256 as soon
       as it finds \marks. See docs/DECISIONS.md,
       which-tex-hstex-is-measured-against. */
    if (hstex_engine_init_extended(&engine, true, error, sizeof(error)) != 0) {
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

/* ---- Parallel disk-checkpoint driver ------------------------------------
   The default way to compile: a cold run drops a checkpoint cache beside the
   output and records what the run was of, so a later run of the same source
   can take the chapters up in parallel from those checkpoints instead of
   reading the whole document again. This file wires the orchestration -- the
   cache, its validity, the cold bootstrap, and the parallel fan-out; the
   chunks resume through the same --resume path a single chunk uses. */

/* A content fingerprint of everything the run's output depends on that this
   driver can see cheaply: the document's own bytes and the format's. An edit
   to either makes the cache stale. (Inputs the document \inputs are followed
   by incremental reuse, which is a later step; for now a changed top file or
   format is what a cold run keys on.) */
static int parallel_hash_file(const char *path, uint64_t *hash)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    uint64_t h = *hash;
    uint8_t buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1U, sizeof(buffer), file)) != 0U) {
        for (size_t index = 0U; index < got; ++index) {
            h ^= buffer[index];
            h *= 0x100000001b3ULL; /* FNV-1a */
        }
    }
    int ok = ferror(file) ? -1 : 0;
    (void)fclose(file);
    *hash = h;
    return ok;
}

/* True for the source extensions a build reads; the outputs it writes
   (.aux, .toc, .log, .pdf, ...) are deliberately excluded, since they change
   every run and would make the cache always look stale. */
static bool parallel_is_state_name(const char *name);

/* WHAT A RUN READS, NOT ONLY WHAT SOMEBODY WROTE. A cache is worth resuming
   only where everything the run would read again is unchanged, and a run
   reads more than its sources: the auxiliary state a previous pass left is
   read back at the start of the next one, and reading a different .toc is
   what makes a second pass differ from a first. Leaving those out made a
   second run look warm when it was not, so pages typeset before the table of
   contents existed were reused and the contents never appeared -- measured
   on cfgguide and cyrguide, whose first page came back missing every entry.
   See docs/DECISIONS.md, what-makes-a-checkpoint-cache-warm. */
static bool parallel_is_source_name(const char *name)
{
    static const char *const exts[] = {
        ".tex", ".sty", ".cls", ".ltx", ".def", ".clo", ".bib", ".cfg",
    };
    const char *dot = strrchr(name, '.');
    if (dot != NULL) {
        for (size_t index = 0U; index < sizeof(exts) / sizeof(exts[0]);
             ++index) {
            if (strcmp(dot, exts[index]) == 0) {
                return true;
            }
        }
    }
    return parallel_is_state_name(name);
}

/* WHAT A PASS LEAVES FOR THE NEXT ONE. These are read back at the start of a
   run, before anything is set, so a change in one of them can move any page
   -- which is the whole reason a second pass differs from a first. They
   count towards whether a cache is still good, and a change in one of them
   is never a change an incremental rebuild may reuse pages across. */
static bool parallel_is_state_name(const char *name)
{
    static const char *const exts[] = {
        ".aux", ".toc", ".lof", ".lot", ".out", ".bbl", ".ind", ".nav",
        ".snm", ".brf", ".gls", ".glo",
    };
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(exts) / sizeof(exts[0]); ++index) {
        if (strcmp(dot, exts[index]) == 0) {
            return true;
        }
    }
    return false;
}

/* The document's own directory, so the files it \inputs from beside it are
   hashed too -- a top file that only \inputs a main file (clay.tex does) would
   otherwise never look changed. Copies the directory part of `path' into `out',
   or "." when there is none. */
static void parallel_directory_of(const char *path, char *out, size_t capacity)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        (void)snprintf(out, capacity, ".");
        return;
    }
    size_t length = (size_t)(slash - path);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(out, path, length);
    out[length] = '\0';
}

static int parallel_validity_hash(const char *format_file,
                                  const char *document_path, uint64_t *hash)
{
    uint64_t total = 0xcbf29ce484222325ULL; /* FNV-1a offset basis */
    if (parallel_hash_file(format_file, &total) != 0) {
        return -1;
    }
    /* Every source file beside the document folds in, its name included so a
       rename is noticed. Each file's hash is XORed in, which needs no sorting
       to be the same from run to run whatever order the directory is read. */
    char directory[512];
    parallel_directory_of(document_path, directory, sizeof(directory));
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return parallel_hash_file(document_path, &total) == 0
                   ? (*hash = total, 0)
                   : -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!parallel_is_source_name(entry->d_name)) {
            continue;
        }
        uint64_t one = 0xcbf29ce484222325ULL;
        for (const char *c = entry->d_name; *c != '\0'; ++c) {
            one ^= (uint8_t)*c;
            one *= 0x100000001b3ULL;
        }
        char file_path[1024];
        (void)snprintf(file_path, sizeof(file_path), "%s/%s", directory,
                       entry->d_name);
        if (parallel_hash_file(file_path, &one) != 0) {
            (void)closedir(dir);
            return -1;
        }
        total ^= one;
    }
    (void)closedir(dir);
    *hash = total;
    return 0;
}

/* The cache lives beside the output as a hidden directory; the manifest at its
   head says what source and format it was built from and how it was strided,
   so a later run can tell at a glance whether it may take the checkpoints up. */
#define HSTEX_PARALLEL_CACHE ".hstex-parallel"
#define HSTEX_PARALLEL_STRIDE 40
/* How many times a cold run will recompile to settle the .aux before it gives
   up on a fixpoint; a person reruns latex two or three times, and a document
   whose references never settle is rare. */
#define HSTEX_PARALLEL_MAX_PASSES 6

static int parallel_write_manifest(const char *cache_dir, uint64_t hash,
                                   int stride, int pages)
{
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/manifest", cache_dir);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    (void)fprintf(file, "hstex-parallel 1\nhash %" PRIu64 "\nstride %d\npages %d\n",
                  hash, stride, pages);
    return fclose(file) == 0 ? 0 : -1;
}

/* Read the manifest's recorded hash and stride; returns 0 and fills them when
   the cache is present and readable, non-zero when there is no usable cache. */
static int parallel_read_manifest(const char *cache_dir, uint64_t *hash,
                                  int *stride, int *pages)
{
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/manifest", cache_dir);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    int version = 0;
    unsigned long long stored = 0ULL;
    int stored_stride = 0, stored_pages = 0;
    int matched = fscanf(file, "hstex-parallel %d hash %llu stride %d pages %d",
                         &version, &stored, &stored_stride, &stored_pages);
    (void)fclose(file);
    if (matched != 4 || version != 1) {
        return -1;
    }
    *hash = (uint64_t)stored;
    *stride = stored_stride;
    *pages = stored_pages;
    return 0;
}

/* Remove a directory tree -- the cache and its per-chunk subdirectories -- so a
   cold run starts from nothing rather than mixing two builds' checkpoints or
   leaving a stale chunk behind. One level of nesting is all the cache has, but
   the recursion handles any. */
static void parallel_remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[1024];
            (void)snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            if (remove(child) != 0) {
                parallel_remove_tree(child); /* a non-empty directory */
            }
        }
        (void)closedir(dir);
    }
    (void)rmdir(path);
}

static void parallel_clear_cache(const char *cache_dir)
{
    parallel_remove_tree(cache_dir);
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

/* Copy a file byte for byte, so each chunk finds the run's .aux/.toc under its
   own output directory where the resume looks for them. */
static int parallel_copy_file(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE *out = fopen(destination, "wb");
    if (out == NULL) {
        (void)fclose(in);
        return -1;
    }
    uint8_t buffer[65536];
    size_t got;
    int ok = 0;
    while ((got = fread(buffer, 1U, sizeof(buffer), in)) != 0U) {
        if (fwrite(buffer, 1U, got, out) != got) {
            ok = -1;
            break;
        }
    }
    if (ferror(in)) {
        ok = -1;
    }
    (void)fclose(in);
    return fclose(out) == 0 ? ok : -1;
}

/* The checkpoint pages found in the cache, smallest first. Returns how many, or
   -1 on trouble; fills `pages` (caller-sized `capacity`). */
static int parallel_checkpoint_pages(const char *cache_dir, int *pages,
                                     int capacity)
{
    DIR *dir = opendir(cache_dir);
    if (dir == NULL) {
        return -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < capacity) {
        int page = 0;
        if (sscanf(entry->d_name, "ck-%d.bin", &page) == 1) {
            pages[count++] = page;
        }
    }
    (void)closedir(dir);
    for (int i = 1; i < count; ++i) { /* insertion sort; a handful of entries */
        int key = pages[i], j = i - 1;
        while (j >= 0 && pages[j] > key) {
            pages[j + 1] = pages[j];
            --j;
        }
        pages[j + 1] = key;
    }
    return count;
}

/* Run one chunk in a child process: chunk 0 compiles the opening pages from
   scratch (it alone pays the preamble), the rest resume their checkpoint; each
   stops at its own last page and writes clay.pdf into its own directory. The
   run's .aux/.toc/.out are copied in first so a resume resolves references and
   the contents the same way the sequential build did. Returns the child's exit
   status. */
static int parallel_run_chunk(const char *format_file, const char *document_path,
                              const char *output_directory,
                              const char *cache_dir, const char *chunk_dir,
                              const char *job_name, int checkpoint_page,
                              int stop_page, bool restricted_shell_escape)
{
    (void)mkdir(chunk_dir, 0700);
    static const char *const suffixes[] = {".aux", ".toc", ".out",
                                           ".bbl", ".lof", ".lot"};
    for (size_t i = 0U; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        char from[1024], to[1024];
        (void)snprintf(from, sizeof(from), "%s/%s%s", output_directory,
                       job_name, suffixes[i]);
        (void)snprintf(to, sizeof(to), "%s/%s%s", chunk_dir, job_name,
                       suffixes[i]);
        (void)parallel_copy_file(from, to); /* absent files are fine */
    }
    /* stop_page <= 0 means run to the end -- the one chunk that finalizes a
       tiled PDF (writes the catalogue and xref). */
    if (stop_page > 0) {
        char stop[32];
        (void)snprintf(stop, sizeof(stop), "%d", stop_page);
        (void)setenv("HSTEX_RESUME_STOP", stop, 1);
    } else {
        (void)unsetenv("HSTEX_RESUME_STOP");
    }
    (void)unsetenv("HSTEX_CKPT_EVERY");
    char log_path[1088];
    (void)snprintf(log_path, sizeof(log_path), "%s/log", chunk_dir);
    FILE *log = fopen(log_path, "wb");
    if (log != NULL) {
        (void)dup2(fileno(log), STDOUT_FILENO);
        (void)dup2(fileno(log), STDERR_FILENO);
        (void)fclose(log);
    }
    /* No terminal: an error in a chunk must abort at end-of-input rather than
       stop for a prompt no one can answer, which would hang the parent's wait. */
    FILE *null_in = fopen("/dev/null", "rb");
    if (null_in != NULL) {
        (void)dup2(fileno(null_in), STDIN_FILENO);
        (void)fclose(null_in);
    }
    int status;
    if (checkpoint_page < 0) {
        /* No page-zero checkpoint to resume: this chunk reads the preamble
           itself and typesets from the top. */
        status = run_document_from_format(format_file, document_path, chunk_dir,
                                          job_name, restricted_shell_escape,
                                          NULL);
    } else {
        /* Resume a checkpoint -- ck-0 is the post-preamble state, so a chunk
           resuming it skips the preamble like any other. */
        char checkpoint[1088];
        (void)snprintf(checkpoint, sizeof(checkpoint), "%s/ck-%d.bin", cache_dir,
                       checkpoint_page);
        status = resume_checkpoint_document(checkpoint, chunk_dir, job_name);
    }
    return status;
}

/* A warm run: fan the chapters out to fresh processes -- chunk 0 from scratch,
   the rest resumed from their checkpoints -- each writing its own pages, then
   assemble the pieces into one clay.pdf.

   The last checkpoint is NOT resumed: a chunk that runs \end{document} finalizes
   document-wide objects (hyperref/tagpdf) it never reserved, which only the
   shared-file path of true tiling can carry. Instead the chunks cover the pages
   up to the last checkpoint and the tail comes from the reference PDF the cold
   run left (identical, source unchanged). The join itself is the pluggable
   part -- pdfseparate to lift the tail, pdfunite to join, both from poppler --
   and in-engine tiling (chunks writing one shared PDF, no join, and finalizing
   in place) is what replaces all of this. A chunk failure or a missing tool
   makes the caller fall back to a sequential compile, so a warm run is never
   wrong, only -- in that case -- not faster. */
static int run_parallel_warm(const char *format_file, const char *document_path,
                             const char *output_directory,
                             const char *cache_dir, const char *job_name,
                             int pages, bool restricted_shell_escape)
{
    int ckpts[1024];
    int nck = parallel_checkpoint_pages(cache_dir, ckpts, 1024);
    if (nck <= 0) {
        return -1; /* no usable checkpoints; caller compiles sequentially */
    }
    (void)pages;
    /* A cold run that found a clean post-preamble moment left a page-zero
       checkpoint (ck-0), which parallel_checkpoint_pages returns as page 0 at
       the front. When it is there, EVERY chunk resumes a checkpoint -- chunk i
       resumes ckpts[i] and stops at ckpts[i+1], the last running to
       \end{document} -- so no chunk re-reads the preamble. When it is not,
       chunk 0 alone reads the preamble and typesets pages 1..ckpts[0] from the
       top, and chunk i>0 resumes ckpts[i-1]. Either way every chunk tiles into
       one shared PDF at the byte the sequential run had reached, so nothing is
       joined afterward. */
    bool have_zero = nck > 0 && ckpts[0] == 0;
    int nchunks = have_zero ? nck : nck + 1;
    char shared_pdf[1024];
    (void)snprintf(shared_pdf, sizeof(shared_pdf), "%s/%s.pdf", output_directory,
                   job_name);
    FILE *shared = fopen(shared_pdf, "wb"); /* create empty for the chunks */
    if (shared != NULL) {
        (void)fclose(shared);
    }
    (void)setenv("HSTEX_TILE", shared_pdf, 1);
    char (*chunk_dirs)[1024] = malloc((size_t)nchunks * sizeof(*chunk_dirs));
    pid_t *children = malloc((size_t)nchunks * sizeof(*children));
    if (chunk_dirs == NULL || children == NULL) {
        free(chunk_dirs);
        free(children);
        return -1;
    }
    /* Each chunk deserializes a whole run's state -- a node arena and millions
       of meanings -- so a dozen at once saturate memory bandwidth and run
       slower than a sequential compile. Keep at most a handful in flight; the
       cap can be overridden. A running window: launch up to the cap, and each
       time one finishes launch the next. */
    int cap = 8;
    const char *cap_env = getenv("HSTEX_PARALLEL_JOBS");
    if (cap_env != NULL) {
        int asked = (int)strtol(cap_env, NULL, 10);
        if (asked >= 1 && asked <= nchunks) {
            cap = asked;
        }
    }
    if (cap > nchunks) {
        cap = nchunks;
    }
    int failures = 0, launched = 0, running = 0;
    for (int i = 0; i < nchunks; ++i) {
        children[i] = -1;
    }
    while (launched < nchunks || running > 0) {
        while (running < cap && launched < nchunks) {
            int i = launched++;
            /* -1 marks the from-scratch chunk (no checkpoint to resume). */
            int checkpoint_page = have_zero ? ckpts[i] : (i == 0 ? -1 : ckpts[i - 1]);
            int stop_page = have_zero ? (i + 1 < nck ? ckpts[i + 1] : 0)
                                      : (i < nck ? ckpts[i] : 0);
            (void)snprintf(chunk_dirs[i], sizeof(chunk_dirs[i]), "%s/chunk_%d",
                           cache_dir, i);
            pid_t child = fork();
            if (child == 0) {
                _exit(parallel_run_chunk(
                    format_file, document_path, output_directory, cache_dir,
                    chunk_dirs[i], job_name, checkpoint_page, stop_page,
                    restricted_shell_escape));
            }
            if (child < 0) {
                failures++;
            } else {
                children[i] = child;
                running++;
            }
        }
        int wstatus = 0;
        pid_t done = waitpid(-1, &wstatus, 0);
        if (done > 0) {
            running--;
            if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
                failures++;
            }
        } else if (errno != EINTR) {
            break;
        }
    }
    (void)unsetenv("HSTEX_TILE");
    /* The chunks wrote the one shared PDF directly; nothing to join. */
    free(chunk_dirs);
    free(children);
    return failures == 0 ? 0 : -1;
}

/* After a cold run, boil its raw source-open log (a page and a path per line,
   in reading order) down to one line per source file: the page it first opened
   at, its content hash, and its path. A later run compares the hashes to see
   which files an edit touched and, from the pages, which checkpoints predate
   the touch. Output files (.aux/.toc/...) are left out -- they change every
   run and are not what an edit means. */
static void parallel_record_sources(const char *cache_dir)
{
    char log_path[600], out_path[600];
    (void)snprintf(log_path, sizeof(log_path), "%s/sources.log", cache_dir);
    (void)snprintf(out_path, sizeof(out_path), "%s/sources", cache_dir);
    FILE *log = fopen(log_path, "rb");
    if (log == NULL) {
        return;
    }
    struct source_note {
        char path[1024];
        int page;
    } *notes = NULL;
    size_t count = 0U, capacity = 0U;
    char line[1200];
    while (fgets(line, sizeof(line), log) != NULL) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';
        int page = (int)strtol(line, NULL, 10);
        char *path = tab + 1;
        size_t length = strlen(path);
        while (length > 0U && (path[length - 1U] == '\n' ||
                               path[length - 1U] == '\r')) {
            path[--length] = '\0';
        }
        if (length == 0U || length >= sizeof(notes[0].path) ||
            !parallel_is_source_name(path)) {
            continue;
        }
        size_t index = 0U;
        for (; index < count; ++index) {
            if (strcmp(notes[index].path, path) == 0) {
                break;
            }
        }
        if (index < count) {
            if (page < notes[index].page) {
                notes[index].page = page;
            }
            continue;
        }
        if (count == capacity) {
            size_t grown = capacity == 0U ? 32U : capacity * 2U;
            struct source_note *bigger =
                realloc(notes, grown * sizeof(*bigger));
            if (bigger == NULL) {
                break;
            }
            notes = bigger;
            capacity = grown;
        }
        (void)snprintf(notes[count].path, sizeof(notes[count].path), "%s", path);
        notes[count].page = page;
        ++count;
    }
    (void)fclose(log);
    FILE *out = fopen(out_path, "wb");
    if (out != NULL) {
        for (size_t index = 0U; index < count; ++index) {
            uint64_t hash = 0xcbf29ce484222325ULL;
            if (parallel_hash_file(notes[index].path, &hash) != 0) {
                continue;
            }
            (void)fprintf(out, "%d\t%" PRIu64 "\t%s\n", notes[index].page, hash,
                          notes[index].path);
        }
        (void)fclose(out);
    }
    free(notes);
}

/* The FNV-1a hash of a file's first `n' bytes, to compare against the prefix
   hash a checkpoint recorded. Fails if the file is now shorter than `n' (the
   edit cut into what the checkpoint had read), which counts as changed. */
static int parallel_hash_file_prefix(const char *path, size_t n, uint64_t *hash)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    uint64_t value = 0xcbf29ce484222325ULL;
    uint8_t buffer[65536];
    size_t remaining = n;
    int ok = 0;
    while (remaining > 0U) {
        size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t got = fread(buffer, 1U, want, file);
        for (size_t index = 0U; index < got; ++index) {
            value ^= buffer[index];
            value *= 0x100000001b3ULL;
        }
        if (got < want) {
            ok = -1; /* shorter than the checkpoint had read */
            break;
        }
        remaining -= got;
    }
    (void)fclose(file);
    if (ok == 0) {
        *hash = value;
    }
    return ok;
}

/* Which changed source files an edit touched, by first-open page. */
struct parallel_changed_file {
    char path[1024];
    int first_page;
};

static int parallel_changed_files(const char *cache_dir,
                                 struct parallel_changed_file *changed,
                                 int capacity);

/* Whether any of what a previous pass left has moved since the cache was
   built. Such a change is not one pages may be reused across. */
static bool parallel_state_changed(const char *cache_dir)
{
    struct parallel_changed_file changed[256];
    int count = parallel_changed_files(cache_dir, changed,
                                       (int)(sizeof(changed) /
                                             sizeof(changed[0])));
    for (int index = 0; index < count; ++index) {
        const char *name = strrchr(changed[index].path, '/');
        name = name == NULL ? changed[index].path : name + 1;
        if (parallel_is_state_name(name)) {
            return true;
        }
    }
    return false;
}

/* Read the source record, hashing each file again; fill `changed' with the
   files whose content moved and return how many (up to `capacity'), or -1 if
   the record is missing. */
static int parallel_changed_files(const char *cache_dir,
                                  struct parallel_changed_file *changed,
                                  int capacity)
{
    char path[600];
    (void)snprintf(path, sizeof(path), "%s/sources", cache_dir);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    int count = 0;
    char line[1400];
    while (fgets(line, sizeof(line), file) != NULL && count < capacity) {
        char *first = strchr(line, '\t');
        char *second = first != NULL ? strchr(first + 1, '\t') : NULL;
        if (second == NULL) {
            continue;
        }
        *first = '\0';
        *second = '\0';
        int page = (int)strtol(line, NULL, 10);
        uint64_t stored = strtoull(first + 1, NULL, 10);
        char *file_path = second + 1;
        size_t length = strlen(file_path);
        while (length > 0U && (file_path[length - 1U] == '\n' ||
                               file_path[length - 1U] == '\r')) {
            file_path[--length] = '\0';
        }
        uint64_t current = 0xcbf29ce484222325ULL;
        if ((parallel_hash_file(file_path, &current) != 0 ||
             current != stored) &&
            length < sizeof(changed[0].path)) {
            (void)snprintf(changed[count].path, sizeof(changed[count].path),
                           "%s", file_path);
            changed[count].first_page = page;
            ++count;
        }
    }
    (void)fclose(file);
    return count;
}

/* Which checkpoint a rebuild can resume from after an edit: the last one whose
   consumed reading an edit left untouched. Each checkpoint recorded the file it
   was reading, the byte it had reached, and a hash of everything before that
   byte (the positions record); a checkpoint is reusable when every changed
   file either opens after it or is the file it was reading with that prefix
   still intact -- so an edit deep in one long file still reuses the checkpoints
   before the edit, not just those before the file opened. Returns the page, 0
   for ck-0, or -1 to fall back to a cold rebuild. */
static int parallel_incremental_reuse_page(const char *cache_dir,
                                           const int *ckpts, int nck)
{
    (void)ckpts;
    (void)nck;
    struct parallel_changed_file changed[256];
    int changed_count = parallel_changed_files(cache_dir, changed,
                                               (int)(sizeof(changed) /
                                                     sizeof(changed[0])));
    if (changed_count <= 0) {
        return -1; /* record missing, or nothing tracked changed: rebuild cold */
    }
    char path[600];
    (void)snprintf(path, sizeof(path), "%s/positions", cache_dir);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    int reuse = -1;
    char line[1400];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *a = strchr(line, '\t');
        char *b = a != NULL ? strchr(a + 1, '\t') : NULL;
        char *c = b != NULL ? strchr(b + 1, '\t') : NULL;
        if (c == NULL) {
            continue;
        }
        *a = '\0';
        *b = '\0';
        *c = '\0';
        int page = (int)strtol(line, NULL, 10);
        size_t cursor = (size_t)strtoull(a + 1, NULL, 10);
        uint64_t prefix_hash = strtoull(b + 1, NULL, 10);
        char *reading = c + 1;
        size_t length = strlen(reading);
        while (length > 0U &&
               (reading[length - 1U] == '\n' || reading[length - 1U] == '\r')) {
            reading[--length] = '\0';
        }
        if (page <= reuse) {
            continue; /* already have one at least this far in */
        }
        bool reusable = true;
        for (int index = 0; index < changed_count; ++index) {
            const struct parallel_changed_file *edit = &changed[index];
            if (edit->first_page > page) {
                continue; /* this file opens after the checkpoint */
            }
            if (strcmp(edit->path, reading) == 0) {
                uint64_t current = 0U;
                if (parallel_hash_file_prefix(edit->path, cursor, &current) !=
                        0 ||
                    current != prefix_hash) {
                    reusable = false; /* the edit is at or before the cursor */
                    break;
                }
            } else {
                reusable = false; /* consumed whole before this checkpoint */
                break;
            }
        }
        if (reusable) {
            reuse = page;
        }
    }
    (void)fclose(file);
    return reuse;
}

/* An incremental rebuild: resume the last checkpoint the edit did not reach and
   run forward to the end, tiling into the previous build's PDF so its untouched
   prefix is kept and only the pages from the edit on are written again. The
   resumed run reads the settled .aux left by the previous build, so a reference
   the edit did not change resolves the same; an edit that DID change a
   reference is out of scope here (the caller only reaches this for a change
   whose file opens after a checkpoint) and would need a cold rebuild. Returns 0
   on success, -1 to fall back. */
static int run_parallel_incremental(const char *cache_dir,
                                    const char *output_directory,
                                    const char *job_name, int reuse_page)
{
    char shared_pdf[1024];
    (void)snprintf(shared_pdf, sizeof(shared_pdf), "%s/%s.pdf", output_directory,
                   job_name);
    if (access(shared_pdf, R_OK | W_OK) != 0) {
        return -1; /* no previous PDF to reuse */
    }
    char checkpoint[1088];
    (void)snprintf(checkpoint, sizeof(checkpoint), "%s/ck-%d.bin", cache_dir,
                   reuse_page);
    if (access(checkpoint, R_OK) != 0) {
        return -1;
    }
    (void)setenv("HSTEX_TILE", shared_pdf, 1);
    (void)unsetenv("HSTEX_RESUME_STOP"); /* run to \end{document} */
    (void)unsetenv("HSTEX_CKPT_EVERY");
    int status =
        resume_checkpoint_document(checkpoint, output_directory, job_name);
    (void)unsetenv("HSTEX_TILE");
    return status;
}

/* Compile a document the parallel way. A cold run -- no cache, or one built
   from different source -- compiles sequentially while dropping a checkpoint
   every stride pages, then records a manifest so the next run of the same
   source finds the cache warm. A warm run fans the chapters out to parallel
   resume processes and joins their pages; if that cannot be done it falls back
   to a sequential compile, which is correct and no slower than an ordinary
   run. An edited source that changed only files opening after some checkpoint
   is rebuilt incrementally -- forward from that checkpoint, reusing the prefix
   of the last PDF. */
/* The job name a document defaults to: its file name without directory or
   extension (clay.tex -> clay), which is what \jobname and the PDF are named. */
static void parallel_default_job(const char *document_path, char *out,
                                 size_t capacity)
{
    const char *base = strrchr(document_path, '/');
    base = base == NULL ? document_path : base + 1;
    const char *dot = strrchr(base, '.');
    size_t length = dot == NULL ? strlen(base) : (size_t)(dot - base);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(out, base, length);
    out[length] = '\0';
}

static int run_parallel_document(const char *format_file,
                                 const char *document_path,
                                 const char *output_directory,
                                 const char *job_name,
                                 bool restricted_shell_escape)
{
    char job_buffer[256];
    if (job_name == NULL) {
        parallel_default_job(document_path, job_buffer, sizeof(job_buffer));
        job_name = job_buffer;
    }
    /* build/document-output is nested, and mkdir is not recursive. */
    if (strcmp(output_directory, "build/document-output") == 0) {
        (void)mkdir("build", 0700);
    }
    (void)mkdir(output_directory, 0700);
    char cache_dir[256];
    (void)snprintf(cache_dir, sizeof(cache_dir), "%s/%s", output_directory,
                   HSTEX_PARALLEL_CACHE);

    uint64_t hash = 0U;
    if (parallel_validity_hash(format_file, document_path, &hash) != 0) {
        (void)fprintf(stderr, "hstex: cannot read %s or %s\n", document_path,
                      format_file);
        return 1;
    }
    uint64_t cached_hash = 0U;
    int cached_stride = 0, cached_pages = 0;
    bool warm =
        parallel_read_manifest(cache_dir, &cached_hash, &cached_stride,
                               &cached_pages) == 0 &&
        cached_hash == hash;

    if (warm) {
        (void)fprintf(stderr,
                      "hstex: warm checkpoint cache (%d pages, stride %d) -- "
                      "resuming chapters in parallel\n",
                      cached_pages, cached_stride);
        if (run_parallel_warm(format_file, document_path, output_directory,
                              cache_dir, job_name, cached_pages,
                              restricted_shell_escape) == 0) {
            return 0;
        }
        (void)fprintf(stderr,
                      "hstex: parallel resume unavailable -- compiling "
                      "sequentially over the cache\n");
        return run_document_from_format(format_file, document_path,
                                        output_directory, job_name,
                                        restricted_shell_escape, NULL);
    }

    /* Not warm: either there is no cache or the source has been edited. If a
       cache is there and the edit lies past a checkpoint, rebuild forward from
       it rather than from the top -- the pages before it read only files the
       edit did not touch. */
    if (parallel_read_manifest(cache_dir, &cached_hash, &cached_stride,
                               &cached_pages) == 0) {
        int ckpts[1024];
        int nck = parallel_checkpoint_pages(cache_dir, ckpts, 1024);
        /* Only an edit to something somebody wrote may reuse pages. What a
           previous pass left is read before the first page is set, so a
           change there can move any page: measured, cfgguide and cyrguide
           came back with an empty table of contents because page one was
           reused from the pass that wrote the .toc rather than set again
           from it. */
        int reuse = nck > 0 && !parallel_state_changed(cache_dir)
                        ? parallel_incremental_reuse_page(cache_dir, ckpts, nck)
                        : -1;
        if (reuse >= 0) {
            (void)fprintf(stderr,
                          "hstex: incremental rebuild -- resuming the "
                          "checkpoint at page %d and running forward\n",
                          reuse);
            if (run_parallel_incremental(cache_dir, output_directory, job_name,
                                         reuse) == 0) {
                return 0;
            }
            (void)fprintf(stderr, "hstex: incremental rebuild unavailable -- "
                                  "building the cache cold\n");
        }
    }

    (void)fprintf(stderr, "hstex: cold run -- building checkpoint cache in %s\n",
                  cache_dir);
    char ckpt_every[1200];
    (void)snprintf(ckpt_every, sizeof(ckpt_every), "%d:%s",
                   HSTEX_PARALLEL_STRIDE, cache_dir);
    char aux_path[600];
    (void)snprintf(aux_path, sizeof(aux_path), "%s/%s.aux", output_directory,
                   job_name);
    char sources_log[600];
    (void)snprintf(sources_log, sizeof(sources_log), "%s/sources.log",
                   cache_dir);
    (void)setenv("HSTEX_CKPT_EVERY", ckpt_every, 1);
    /* Log which page each source file opens at, so a later edit can be placed
       against the checkpoints. The final pass's log is the one kept -- the
       cache, and the log in it, is cleared at the head of every pass. */
    (void)setenv("HSTEX_CKPT_SOURCES", sources_log, 1);
    /* A warm run's resumed chapters read the settled .aux and typeset against
       it; the checkpoints they resume from must have been taken at that same
       settled layout, or the byte offsets a chapter tiles into no longer line
       up. So the cold run recompiles until the .aux stops changing -- as a
       person reruns latex to get the references right -- dropping a fresh set
       of checkpoints each pass. The pass that reads an .aux identical to the
       previous one is standing on the fixpoint, and its checkpoints are the
       ones the warm run reproduces exactly. */
    uint64_t previous_aux = 0U;
    bool have_previous = false;
    int pages = 0, status = 0;
    for (int pass = 0; pass < HSTEX_PARALLEL_MAX_PASSES; ++pass) {
        parallel_clear_cache(cache_dir);
        if (mkdir(cache_dir, 0700) != 0 && errno != EEXIST) {
            (void)fprintf(stderr, "hstex: cannot make %s\n", cache_dir);
            (void)unsetenv("HSTEX_CKPT_EVERY");
            return 1;
        }
        status = run_document_from_format(format_file, document_path,
                                          output_directory, job_name,
                                          restricted_shell_escape, &pages);
        if (status != 0) {
            break;
        }
        uint64_t aux = 0xcbf29ce484222325ULL; /* FNV-1a offset basis */
        if (parallel_hash_file(aux_path, &aux) != 0) {
            break; /* no .aux at all -- a single pass is the whole story */
        }
        if (have_previous && aux == previous_aux) {
            break; /* settled: this pass read the .aux a warm run will read */
        }
        previous_aux = aux;
        have_previous = true;
    }
    (void)unsetenv("HSTEX_CKPT_EVERY");
    (void)unsetenv("HSTEX_CKPT_SOURCES");
    if (status == 0) {
        /* Boil the final pass's source-open log down to one hashed line per
           file, so the next run can tell what an edit touched. */
        parallel_record_sources(cache_dir);
        if (parallel_write_manifest(cache_dir, hash, HSTEX_PARALLEL_STRIDE,
                                    pages) != 0) {
            (void)fprintf(stderr,
                          "hstex: warning: could not record parallel manifest\n");
        }
    }
    return status;
}

int main(int argument_count, char **arguments)
{
    if (argument_count == 2 &&
        (strcmp(arguments[1], "-h") == 0 ||
         strcmp(arguments[1], "--help") == 0)) {
        print_usage(stdout, arguments[0]);
        return 0;
    }
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
        return make_format(arguments[2], arguments[3], true);
    }
    if (argument_count == 4 &&
        strcmp(arguments[1], "--make-ini-format") == 0) {
        return make_format(arguments[2], arguments[3], false);
    }
    if (argument_count == 4 && strcmp(arguments[1], "--format") == 0) {
        return run_document_from_format(arguments[2], arguments[3],
                                        "build/document-output", NULL, true,
                                        NULL);
    }
    if (argument_count == 4 && strcmp(arguments[1], "--parallel") == 0) {
        return run_parallel_document(arguments[2], arguments[3],
                                     "build/document-output", NULL, true);
    }
    if ((argument_count == 5 || argument_count == 6) &&
        (strcmp(arguments[1], "--parallel-output") == 0 ||
         strcmp(arguments[1], "--parallel-output-no-shell") == 0)) {
        return run_parallel_document(
            arguments[2], arguments[3], arguments[4],
            argument_count == 6 ? arguments[5] : NULL,
            strcmp(arguments[1], "--parallel-output") == 0);
    }
    if (argument_count == 5 && strcmp(arguments[1], "--resume") == 0) {
        return resume_checkpoint_document(arguments[2], arguments[3],
                                          arguments[4]);
    }
    if (argument_count == 4 &&
        strcmp(arguments[1], "--format-no-shell") == 0) {
        return run_document_from_format(arguments[2], arguments[3],
                                        "build/document-output", NULL, false,
                                        NULL);
    }
    if ((argument_count == 5 || argument_count == 6) &&
        strcmp(arguments[1], "--format-output") == 0) {
        return run_document_from_format(arguments[2], arguments[3],
                                        arguments[4],
                                        argument_count == 6 ? arguments[5]
                                                            : NULL,
                                        true, NULL);
    }
    if ((argument_count == 5 || argument_count == 6) &&
        strcmp(arguments[1], "--format-output-no-shell") == 0) {
        return run_document_from_format(arguments[2], arguments[3],
                                        arguments[4],
                                        argument_count == 6 ? arguments[5]
                                                            : NULL,
                                        false, NULL);
    }

    print_usage(stderr, arguments[0]);
    return 2;
}
