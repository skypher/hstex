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
                                          sizeof(error)) != 0 ||
        hstex_engine_read_format(&engine, format_file, error, sizeof(error)) !=
            0 ||
        hstex_engine_set_restricted_shell_escape(
            &engine, restricted_shell_escape, error, sizeof(error)) != 0) {
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
    if (job_name != NULL &&
        hstex_engine_set_job_name(&engine, job_name, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        hstex_engine_destroy(&engine);
        return 1;
    }
    size_t document_output_tokens = 0U;
    int status = drain_engine(&engine, document_path, &document_output_tokens);
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
static bool parallel_is_source_name(const char *name)
{
    static const char *const exts[] = {".tex", ".sty", ".cls", ".ltx",
                                       ".def", ".clo", ".bib", ".cfg"};
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
    char stop[32];
    (void)snprintf(stop, sizeof(stop), "%d", stop_page);
    (void)setenv("HSTEX_RESUME_STOP", stop, 1);
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
    if (checkpoint_page == 0) {
        status = run_document_from_format(format_file, document_path, chunk_dir,
                                          job_name, restricted_shell_escape,
                                          NULL);
    } else {
        char checkpoint[1088];
        (void)snprintf(checkpoint, sizeof(checkpoint), "%s/ck-%d.bin", cache_dir,
                       checkpoint_page);
        status = resume_checkpoint_document(checkpoint, chunk_dir, job_name);
    }
    return status;
}

/* Wait for a child, returning 0 only when it exited cleanly. */
static int parallel_wait(pid_t child)
{
    if (child < 0) {
        return -1;
    }
    int wstatus = 0;
    while (waitpid(child, &wstatus, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 ? 0 : -1;
}

/* Run `argv' (NULL-terminated) as a child and wait for it; 0 on a clean exit.
   execvp is used so a missing tool just fails rather than needing a hard path;
   the caller treats that as "assembly unavailable" and falls back. */
static int parallel_spawn(char *const argv[])
{
    pid_t child = fork();
    if (child == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    return parallel_wait(child);
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
    /* One chunk per checkpoint boundary: chunk 0 is pages 1..ckpts[0] from
       scratch, chunk i (i>=1) resumes ckpts[i-1] and stops at ckpts[i]. Together
       they cover pages 1..ckpts[nck-1]; the pages after that are the tail. */
    int nchunks = nck;
    int tail_start = ckpts[nck - 1] + 1;
    int tail_count = tail_start <= pages ? pages - tail_start + 1 : 0;
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
            int checkpoint_page = i == 0 ? 0 : ckpts[i - 1];
            int stop_page = ckpts[i];
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
    int result = -1;
    if (failures == 0) {
        char output_pdf[1024];
        (void)snprintf(output_pdf, sizeof(output_pdf), "%s/%s.pdf",
                       output_directory, job_name);
        /* Lift the tail out of the reference PDF (the current clay.pdf, from the
           cold run) BEFORE the join overwrites it; pdfseparate names each page
           file by its own number. */
        int have_tail = 1;
        if (tail_count > 0) {
            char first[16], last[16], pattern[1024];
            (void)snprintf(first, sizeof(first), "%d", tail_start);
            (void)snprintf(last, sizeof(last), "%d", pages);
            (void)snprintf(pattern, sizeof(pattern), "%s/tail-%%d.pdf",
                           cache_dir);
            char *sep_argv[] = {(char *)"pdfseparate", (char *)"-f",
                                first,   (char *)"-l",  last,
                                output_pdf, pattern,      NULL};
            have_tail = parallel_spawn(sep_argv) == 0;
        }
        if (have_tail) {
            int parts = nchunks + tail_count;
            char **argv = malloc((size_t)(parts + 3) * sizeof(*argv));
            char (*paths)[1088] = malloc((size_t)parts * sizeof(*paths));
            if (argv != NULL && paths != NULL) {
                int argc = 0, p = 0;
                argv[argc++] = (char *)"pdfunite";
                for (int i = 0; i < nchunks; ++i, ++p) {
                    (void)snprintf(paths[p], sizeof(paths[p]), "%s/%s.pdf",
                                   chunk_dirs[i], job_name);
                    argv[argc++] = paths[p];
                }
                for (int page = tail_start; page <= pages; ++page, ++p) {
                    (void)snprintf(paths[p], sizeof(paths[p]), "%s/tail-%d.pdf",
                                   cache_dir, page);
                    argv[argc++] = paths[p];
                }
                argv[argc++] = output_pdf;
                argv[argc] = NULL;
                if (parallel_spawn(argv) == 0) {
                    result = 0;
                }
            }
            free(argv);
            free(paths);
        }
    }
    free(chunk_dirs);
    free(children);
    return result;
}

/* Compile a document the parallel way. A cold run -- no cache, or one built
   from different source -- compiles sequentially while dropping a checkpoint
   every stride pages, then records a manifest so the next run of the same
   source finds the cache warm. A warm run fans the chapters out to parallel
   resume processes and joins their pages; if that cannot be done it falls back
   to a sequential compile, which is correct and no slower than an ordinary
   run. */
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

    (void)fprintf(stderr, "hstex: cold run -- building checkpoint cache in %s\n",
                  cache_dir);
    parallel_clear_cache(cache_dir);
    if (mkdir(cache_dir, 0700) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "hstex: cannot make %s\n", cache_dir);
        return 1;
    }
    char ckpt_every[1200];
    (void)snprintf(ckpt_every, sizeof(ckpt_every), "%d:%s",
                   HSTEX_PARALLEL_STRIDE, cache_dir);
    (void)setenv("HSTEX_CKPT_EVERY", ckpt_every, 1);
    int pages = 0;
    int status =
        run_document_from_format(format_file, document_path, output_directory,
                                 job_name, restricted_shell_escape, &pages);
    (void)unsetenv("HSTEX_CKPT_EVERY");
    if (status == 0 &&
        parallel_write_manifest(cache_dir, hash, HSTEX_PARALLEL_STRIDE,
                                pages) != 0) {
        (void)fprintf(stderr,
                      "hstex: warning: could not record parallel manifest\n");
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
