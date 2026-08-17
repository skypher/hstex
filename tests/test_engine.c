#include "hstex/catcode.h"
#include "hstex/engine.h"
#include "hstex/symbol.h"
#include "hstex/token.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool test_token_is_category(hstex_token token,
                                   enum hstex_catcode category)
{
    return hstex_token_is_character(token) &&
           hstex_token_category(token) == (uint8_t)category;
}

static bool test_token_is_space(hstex_token token)
{
    return test_token_is_category(token, HSTEX_CAT_SPACE);
}

static int write_all(int descriptor, const uint8_t *data, size_t length)
{
    size_t written = 0U;
    while (written < length) {
        ssize_t result = write(descriptor, data + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static int open_snippet(const char *source, char path[64])
{
    (void)strcpy(path, "/tmp/hstex-engine-test-XXXXXX");
    int descriptor = mkstemp(path);
    if (descriptor < 0) {
        return -1;
    }
    size_t length = strlen(source);
    if (write_all(descriptor, (const uint8_t *)source, length) != 0 ||
        close(descriptor) != 0) {
        (void)close(descriptor);
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static int prepare_engine(struct hstex_engine *engine, const char *path,
                          bool install_macro_catcodes, char *error,
                          size_t error_capacity)
{
    if (hstex_engine_init(engine, error, error_capacity) != 0) {
        return -1;
    }
    if ((install_macro_catcodes &&
         (hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'{',
                            (uint8_t)HSTEX_CAT_BEGIN_GROUP) != 0 ||
          hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'}',
                            (uint8_t)HSTEX_CAT_END_GROUP) != 0 ||
          hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'#',
                            (uint8_t)HSTEX_CAT_PARAMETER) != 0)) ||
        hstex_engine_push_file(engine, path, error, error_capacity) != 0) {
        hstex_engine_destroy(engine);
        return -1;
    }
    return 0;
}

static int run_snippet(const char *source, const char *expected)
{
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "prepare failed: %s\n", error);
        (void)unlink(path);
        return 1;
    }

    uint8_t output[1024];
    size_t output_count = 0U;
    int status = 0;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            &engine, &token, &location, error, sizeof(error));
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            (void)fprintf(stderr, "engine failed for %s: %s\n", source, error);
            status = 1;
            break;
        }
        if (!hstex_token_is_character(token) ||
            output_count == sizeof(output)) {
            (void)fprintf(stderr, "unexpected output token for %s\n", source);
            status = 1;
            break;
        }
        output[output_count++] = hstex_token_character_code(token);
    }
    size_t expected_length = strlen(expected);
    if (status == 0 &&
        (output_count != expected_length ||
         memcmp(output, expected, expected_length) != 0)) {
        (void)fprintf(stderr,
                      "output mismatch for %s: got %zu bytes, expected %zu\n",
                      source, output_count, expected_length);
        (void)fprintf(stderr, "actual output: [");
        if (output_count != 0U) {
            (void)fwrite(output, 1U, output_count, stderr);
        }
        (void)fprintf(stderr, "]\n");
        status = 1;
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int expect_failure(const char *source, const char *error_fragment)
{
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    bool failed = false;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            &engine, &token, &location, error, sizeof(error));
        if (result == HSTEX_ENGINE_ERROR) {
            failed = true;
            break;
        }
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
    }
    int status = !failed || strstr(error, error_fragment) == NULL;
    if (status != 0) {
        (void)fprintf(stderr, "missing expected failure '%s': %s\n",
                      error_fragment, error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_macro_flags(void)
{
    const char source[] = "\\long\\outer\\def\\a#1{#1}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result;
    do {
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);

    static const uint8_t name[] = {'a'};
    hstex_cs_id identifier = 0U;
    int found = hstex_symbol_find(&engine.lexical_state.symbols,
                                  HSTEX_SYMBOL_REGULAR, name, sizeof(name),
                                  &identifier);
    const struct hstex_meaning *meaning = hstex_engine_meaning(&engine, identifier);
    int status = result != HSTEX_ENGINE_EOF || found != 1 ||
                 meaning->command != HSTEX_COMMAND_MACRO ||
                 meaning->value.macro_identifier == 0U ||
                 (size_t)meaning->value.macro_identifier > engine.macro_count;
    if (status == 0) {
        const struct hstex_macro *macro =
            &engine.macros[meaning->value.macro_identifier - 1U];
        status = macro->flags !=
                 ((uint8_t)HSTEX_MACRO_LONG | (uint8_t)HSTEX_MACRO_OUTER);
    }
    if (status != 0) {
        (void)fprintf(stderr, "macro flag test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static const struct hstex_meaning *meaning_named(struct hstex_engine *engine,
                                                 const char *name)
{
    hstex_cs_id identifier = 0U;
    if (hstex_symbol_find(&engine->lexical_state.symbols, HSTEX_SYMBOL_REGULAR,
                          (const uint8_t *)name, strlen(name), &identifier) != 1) {
        return hstex_engine_meaning(engine, 0U);
    }
    return hstex_engine_meaning(engine, identifier);
}

static int test_ini_bootstrap(void)
{
    const char source[] =
        "\\ifnum\\catcode`\\{=1 \\errmessage bad\\fi\n"
        "\\catcode`\\{=1\n"
        "\\catcode`\\}=2\n"
        "\\ifx\\directlua\\undefined\\else\\errmessage bad\\fi\n"
        "\\ifx\\eTeXversion\\undefined\\errmessage bad\\else\\fi\n"
        "\\catcode`\\#=6\n"
        "\\catcode`\\^=7\n"
        "\\chardef\\active=13\n"
        "\\catcode`\\@=11\n"
        "\\countdef\\count@=255\n"
        "\\let\\bgroup={ \\let\\egroup=}\n"
        "\\ifx\\@@input\\@undefined\\let\\@@input\\input\\fi\n"
        "\\newlinechar`\\^^J\n"
        "\\def\\pair#1#2{#2#1}\\pair AB%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, false, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    uint8_t output[16];
    size_t output_count = 0U;
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
        if (result == HSTEX_ENGINE_TOKEN && hstex_token_is_character(token) &&
            !test_token_is_space(token) && output_count < sizeof(output)) {
            output[output_count++] = hstex_token_character_code(token);
        }
    } while (result == HSTEX_ENGINE_TOKEN);

    const struct hstex_meaning *active = meaning_named(&engine, "active");
    const struct hstex_meaning *count_at = meaning_named(&engine, "count@");
    const struct hstex_meaning *begin_group = meaning_named(&engine, "bgroup");
    const struct hstex_meaning *at_input = meaning_named(&engine, "@@input");
    int status = result != HSTEX_ENGINE_EOF || output_count != 2U ||
                 memcmp(output, "BA", 2U) != 0 ||
                 hstex_catcode_get(&engine.lexical_state.catcodes,
                                   (uint8_t)'{') != HSTEX_CAT_BEGIN_GROUP ||
                 hstex_catcode_get(&engine.lexical_state.catcodes,
                                   (uint8_t)'@') != HSTEX_CAT_LETTER ||
                 engine.integer_parameters[HSTEX_INTEGER_NEW_LINE_CHARACTER] !=
                     10 ||
                 active->command != HSTEX_COMMAND_CHAR_GIVEN ||
                 active->value.integer != 13 ||
                 count_at->command != HSTEX_COMMAND_COUNT_REGISTER ||
                 count_at->value.integer != 255 ||
                 begin_group->command != HSTEX_COMMAND_TOKEN_ALIAS ||
                 !test_token_is_category(begin_group->value.token,
                                         HSTEX_CAT_BEGIN_GROUP) ||
                 at_input->command != HSTEX_COMMAND_INPUT;
    if (status != 0) {
        (void)fprintf(stderr, "INITEX bootstrap test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_input_primitive(void)
{
    char child_path[64];
    if (open_snippet("\\def\\fromchild{C}\\endinput"
                     "\\def\\ignored{X}%",
                     child_path) != 0) {
        return 1;
    }
    char source[256];
    int length = snprintf(
        source, sizeof(source),
        "\\input{%s}\\fromchild\\ifdefined\\ignored F\\else I\\fi%%",
        child_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        (void)unlink(child_path);
        return 1;
    }
    int status = run_snippet(source, "CI");
    (void)unlink(child_path);
    return status;
}

static int test_job_name(void)
{
    char temporary_path[] = "/tmp/hstex-jobname-XXXXXX";
    int descriptor = mkstemp(temporary_path);
    if (descriptor < 0) {
        return 1;
    }
    char path[96];
    int path_length = snprintf(path, sizeof(path), "%s.multi.part.tex",
                               temporary_path);
    static const uint8_t source[] = "\\jobname%";
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        rename(temporary_path, path) != 0 ||
        write_all(descriptor, source, sizeof(source) - 1U) != 0 ||
        close(descriptor) != 0) {
        (void)close(descriptor);
        (void)unlink(temporary_path);
        (void)unlink(path);
        return 1;
    }

    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    uint8_t output[96];
    size_t output_count = 0U;
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
        if (result == HSTEX_ENGINE_TOKEN &&
            hstex_token_is_character(token) &&
            output_count < sizeof(output)) {
            output[output_count++] = hstex_token_character_code(token);
        }
    } while (result == HSTEX_ENGINE_TOKEN);

    const char *base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    size_t expected_length = strlen(base) - strlen(".tex");
    int status = result != HSTEX_ENGINE_EOF ||
                 output_count != expected_length ||
                 memcmp(output, base, expected_length) != 0 ||
                 engine.job_name == NULL ||
                 strlen(engine.job_name) != expected_length ||
                 memcmp(engine.job_name, base, expected_length) != 0;
    if (status != 0) {
        (void)fprintf(stderr, "jobname test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_hyphenation_data(void)
{
    const char source[] =
        "\\language=7\\relax"
        "\\lefthyphenmin=1\\relax\\righthyphenmin=1\\relax"
        "\\patterns{a1b b3c .ab4}"
        "\\hyphenation{ab-cd}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);

    static const uint8_t patterned_word[] = "abc";
    static const uint8_t exception_word[] = "abcd";
    uint8_t patterned_breaks[sizeof(patterned_word)] = {0};
    uint8_t exception_breaks[sizeof(exception_word)] = {0};
    int status = result != HSTEX_ENGINE_EOF ||
                 engine.hyphen_pattern_count != 3U ||
                 engine.hyphen_exception_count != 1U ||
                 hstex_engine_hyphenate_word(
                     &engine, 7, patterned_word,
                     sizeof(patterned_word) - 1U, patterned_breaks,
                     sizeof(patterned_breaks), error, sizeof(error)) != 0 ||
                 patterned_breaks[1] != 1U || patterned_breaks[2] != 0U ||
                 hstex_engine_hyphenate_word(
                     &engine, 7, exception_word, sizeof(exception_word) - 1U,
                     exception_breaks, sizeof(exception_breaks), error,
                     sizeof(error)) != 0 ||
                 exception_breaks[1] != 0U || exception_breaks[2] != 1U ||
                 exception_breaks[3] != 0U;
    if (status != 0) {
        (void)fprintf(stderr, "hyphenation-data test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_document_job_transition(void)
{
    char format_path[64];
    char document_path[64];
    if (open_snippet("\\everyjob{J}\\dump", format_path) != 0 ||
        open_snippet("D%", document_path) != 0) {
        (void)unlink(format_path);
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, format_path, true, error, sizeof(error)) != 0) {
        (void)unlink(format_path);
        (void)unlink(document_path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    uint8_t output[2] = {0};
    size_t output_count = 0U;
    if (result == HSTEX_ENGINE_EOF && engine.dump_requested &&
        hstex_engine_begin_job(&engine, document_path, error, sizeof(error)) ==
            0) {
        do {
            hstex_token token = 0U;
            struct hstex_source_location location;
            result = hstex_engine_next_output(&engine, &token, &location, error,
                                              sizeof(error));
            if (result == HSTEX_ENGINE_TOKEN &&
                hstex_token_is_character(token) &&
                output_count < sizeof(output)) {
                output[output_count++] = hstex_token_character_code(token);
            }
        } while (result == HSTEX_ENGINE_TOKEN);
    }
    const char *document_base = strrchr(document_path, '/');
    document_base = document_base == NULL ? document_path : document_base + 1;
    int status = result != HSTEX_ENGINE_EOF || engine.dump_requested ||
                 output_count != sizeof(output) ||
                 memcmp(output, "JD", sizeof(output)) != 0 ||
                 engine.job_name == NULL ||
                 strcmp(engine.job_name, document_base) != 0;
    if (status != 0) {
        (void)fprintf(stderr, "document-job transition test failed: %s\n",
                      error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(format_path);
    (void)unlink(document_path);
    return status;
}

static int test_file_streams(void)
{
    char stream_path[64];
    if (open_snippet("", stream_path) != 0 || unlink(stream_path) != 0) {
        return 1;
    }
    char source[1024];
    int length = snprintf(
        source, sizeof(source),
        "\\chardef\\stream=3 \\openout\\stream=%s \\def\\expected{abc }"
        "\\write\\stream{\\expected}\\closeout\\stream "
        "\\openin\\stream=\"%s\" \\ifeof\\stream F\\else "
        "\\read\\stream to \\actual "
        "\\ifx\\actual\\expected T\\else F\\fi\\fi "
        "\\closein\\stream "
        "\\openin\\stream=\"%s\" "
        "\\edef\\otherexpected{\\detokenize{abc}}"
        "\\endlinechar=-1 \\readline\\stream to \\otheractual "
        "\\endlinechar=13 "
        "\\ifx\\otheractual\\otherexpected T\\else F\\fi "
        "\\closein\\stream%%",
        stream_path, stream_path, stream_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        return 1;
    }
    int status = run_snippet(source, "TT");
    (void)unlink(stream_path);
    return status;
}

/* expl3 selects its backend from these, so the values must match the engine
   HSTeX reproduces; see docs/DECISIONS.md, pdftex-identification. */
static int test_pdftex_identification(void)
{
    return run_snippet("[\\number\\pdftexversion][\\pdftexrevision]"
                       "\\pdfoutput=1 [\\number\\pdfoutput]"
                       "\\pdfpagewidth=8.5 true in [\\the\\pdfpagewidth]"
                       "\\pdfpageheight=11 true in [\\the\\pdfpageheight]"
                       "\\pdfminorversion=5 [\\number\\pdfminorversion]%",
                       "[140][25][1][614.295pt][794.96999pt][5]");
}

/* Box displacement and vertical packaging, all pinned to the reference; see
   docs/DECISIONS.md, box-shift and vertical-packaging. */
static int test_box_shift_and_packaging(void)
{
    return run_snippet(
        "\\boxmaxdepth=16383.99998pt "
        "\\baselineskip=12pt \\lineskip=1pt \\lineskiplimit=0pt "
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        "\\setbox2=\\hbox{\\vrule height9pt depth1pt width1pt}"
        /* \\lower and \\raise trade height for depth. */
        "\\setbox0=\\hbox{\\lower3pt\\copy1}[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        "\\setbox0=\\hbox{\\raise3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\hbox{\\lower10pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* \\moveright widens a vertical list; neither dimension changes. */
        "\\setbox0=\\vbox{\\moveright4pt\\copy1}[\\the\\wd0|\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\moveleft4pt\\copy1}[\\the\\wd0]"
        /* A vertical list keeps the last box's depth and separates baselines. */
        "\\setbox0=\\vbox{\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\copy1\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\copy1\\copy2}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\copy1\\vskip3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\lineskiplimit=4pt"
        "\\setbox0=\\vbox{\\copy1\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* \\boxmaxdepth moves surplus depth into the height. */
        "\\boxmaxdepth=1pt \\lineskiplimit=0pt"
        "\\setbox0=\\vbox{\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* \\box empties its register, \\copy does not. */
        "\\setbox0=\\box1 [\\the\\wd0|\\the\\wd1]%",
        "[2.0pt|5.0pt|1.0pt][8.0pt|0.0pt][0.0pt|12.0pt]"
        "[5.0pt|5.0pt|2.0pt][0.0pt]"
        "[5.0pt|2.0pt][17.0pt|2.0pt][17.0pt|1.0pt][20.0pt|2.0pt]"
        "[17.0pt|2.0pt][6.0pt|1.0pt][1.0pt|0.0pt]");
}

/* The last-node queries and the PDF objects the document builds; see
   docs/DECISIONS.md, last-node-queries and pdf-objects. */
static int test_last_node_and_pdf_objects(void)
{
    return run_snippet(
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        "[\\the\\lastpenalty|\\the\\lastkern|\\the\\lastskip"
        "|\\the\\lastnodetype]"
        /* The values are captured inside each box, since a character there
           would start a paragraph. */
        "\\setbox0=\\vbox{\\vskip3pt plus1fil \\global\\skip1=\\lastskip "
        "\\global\\count1=\\lastnodetype}"
        "[\\the\\skip1|\\the\\count1]"
        "\\setbox0=\\vbox{\\kern4pt \\global\\dimen1=\\lastkern "
        "\\global\\skip2=\\lastskip \\global\\count2=\\lastnodetype}"
        "[\\the\\dimen1|\\the\\skip2|\\the\\count2]"
        "\\setbox0=\\vbox{\\penalty77 \\global\\count3=\\lastpenalty "
        "\\global\\count4=\\lastnodetype}"
        "[\\the\\count3|\\the\\count4]"
        "\\setbox0=\\vbox{\\copy1 \\global\\count5=\\lastnodetype}"
        "\\setbox0=\\vbox{\\global\\count6=\\lastnodetype}"
        "[\\the\\count5][\\the\\count6]"
        /* Objects are numbered in order, and a reserved number is reused. */
        "\\immediate\\pdfobj{<< /A 1 >>}[\\the\\pdflastobj]"
        "\\immediate\\pdfobj{<< /B 2 >>}[\\the\\pdflastobj]"
        "\\pdfobj reserveobjnum[\\the\\pdflastobj]"
        "\\immediate\\pdfobj useobjnum 3 {<< /C 3 >>}[\\the\\pdflastobj]"
        "\\pdfcatalog{/PageMode /UseOutlines}"
        "\\pdfinfo{/Title (T)}\\pdfrefobj 1 [ok]%",
        "[0|0.0pt|0.0pt|-1][3.0pt plus 1.0fil|11][4.0pt|0.0pt|12][77|13]"
        "[1][-1][1][2][3][3][ok]");
}

/* \iffontchar, \ifhbox, \ifvbox and \ifvoid. An unimplemented conditional is
   worse than a missing command: a skipped branch miscounts its \else. */
static int test_box_and_font_conditionals(void)
{
    return run_snippet(
        "\\font\\fa=cmr10 \\setbox1=\\hbox{}\\setbox2=\\vbox{}"
        "[\\iffontchar\\fa 65 T\\else F\\fi|\\iffontchar\\fa 128 T\\else F\\fi]"
        "[\\ifhbox1 T\\else F\\fi|\\ifhbox2 T\\else F\\fi|\\ifhbox3 T\\else F\\fi]"
        "[\\ifvbox1 T\\else F\\fi|\\ifvbox2 T\\else F\\fi|\\ifvbox3 T\\else F\\fi]"
        "[\\ifvoid1 T\\else F\\fi|\\ifvoid2 T\\else F\\fi|\\ifvoid3 T\\else F\\fi]"
        /* Skipping counts them, so a nested \else is not mistaken for ours. */
        "\\iffalse\\ifhbox1 A\\else B\\fi\\else C\\fi"
        "\\iffalse\\iffontchar\\fa 65 A\\else B\\fi\\else D\\fi%",
        "[T|F][T|F|F][F|T|F][F|F|T]CD");
}

/* Protrusion and expansion codes belong to the font, not to a group; the tag
   comes from the metric file; see docs/DECISIONS.md, protrusion-codes. */
static int test_protrusion_codes(void)
{
    return run_snippet(
        "\\font\\fa=cmr10 \\font\\fb=cmex10 "
        "[\\the\\lpcode\\fa 65|\\the\\rpcode\\fa 65"
        "|\\the\\efcode\\fa 65|\\the\\tagcode\\fa 65]"
        "\\lpcode\\fa 65=100 \\rpcode\\fa 65=-50 \\efcode\\fa 65=800 "
        "[\\the\\lpcode\\fa 65|\\the\\rpcode\\fa 65|\\the\\efcode\\fa 65]"
        /* A setting made inside a group outlives it. */
        "{\\lpcode\\fa 65=7 }[\\the\\lpcode\\fa 65]"
        /* An undefined character still has codes, but no tag. */
        "[\\the\\lpcode\\fa 128|\\the\\efcode\\fa 128"
        "|\\the\\tagcode\\fa 128]"
        "[\\the\\tagcode\\fa 48|\\the\\tagcode\\fb 16]%",
        "[0|0|1000|1][100|-50|800][7][0|1000|-1][0|2]");
}

/* Character metrics come from the TFM tables, and a font without `at` is used
   at its design size; see docs/DECISIONS.md, font-character-metrics. */
static int test_font_character_metrics(void)
{
    return run_snippet(
        "\\font\\fa=cmr10 \\font\\fb=cmr10 at 12.5pt \\font\\fc=cmr12 "
        "\\font\\fd=cmr12 scaled 1002 "
        "[\\the\\fontcharwd\\fa 65|\\the\\fontcharht\\fa 65]"
        /* Character 60 has depth, character 11 an italic correction. */
        "[\\the\\fontchardp\\fa 60|\\the\\fontcharic\\fa 11]"
        /* A character the font does not define measures zero. */
        "[\\the\\fontcharwd\\fa 128|\\the\\fontcharht\\fa 255]"
        "[\\the\\fontcharwd\\fb 65]"
        /* cmr12 is a twelve point design, so it is not cmr10 at ten points. */
        "[\\the\\fontcharwd\\fc 65|\\the\\fontdimen6\\fc]"
        "[\\the\\fontdimen6\\fd]%",
        "[7.50002pt|6.83331pt][1.94444pt|0.77779pt][0.0pt|0.0pt][9.37502pt]"
        "[8.80824pt|11.74988pt][11.77336pt]");
}

/* \scantokens makes characters of its argument without expanding it, then
   reads them back as a file; see docs/DECISIONS.md, scantokens. */
static int test_scan_tokens(void)
{
    return run_snippet(
        "\\def\\b{XY}\\def\\spc{ }"
        "\\edef\\r{\\scantokens{abc}}<\\detokenize\\expandafter{\\r}>"
        /* \noexpand and \string survive as characters, so they act on the
           re-read, not on the argument. */
        "\\edef\\r{\\scantokens{\\noexpand\\b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{\\string\\b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{\\b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{a\\spc b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{a#b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{}}<\\detokenize\\expandafter{\\r}>"
        /* \endlinechar ends each line, so switching it off drops the space. */
        "\\endlinechar=-1 "
        "\\edef\\r{\\scantokens{abc}}<\\detokenize\\expandafter{\\r}>%",
        "<abc ><\\b ><\\b><XY><a b ><a##b ><><abc>");
}

/* \else, \or and \fi met while a conditional is still scanning its own test
   stand for \relax; see docs/DECISIONS.md, unevaluated-conditionals. */
static int test_unevaluated_conditionals(void)
{
    return run_snippet(
        "\\count0=5 "
        "[\\ifnum1<20\\else X\\fi][\\ifnum20<2\\else X\\fi]"
        "[\\ifnum1<2\\fi][\\ifnum\\count0=5\\else X\\fi]"
        "[\\ifcase1\\or A\\else B\\fi][\\ifdim1pt<2pt\\else X\\fi]"
        "[\\ifnum1<2 Y\\else X\\fi][\\ifodd3\\else X\\fi]"
        /* The inserted \relax survives into an \edef's replacement text. */
        "\\edef\\xa{\\ifnum1<20\\else X\\fi}[\\meaning\\xa]"
        "\\edef\\xb{\\ifnum1<2 Y\\else X\\fi}[\\meaning\\xb]%",
        "[][X][][][A][][Y][][macro:->\\relax ][macro:->Y]");
}

/* A \countdef'd control sequence reports the primitive it stands for. */
static int test_defined_register_meanings(void)
{
    return run_snippet(
        "\\countdef\\ca=298 \\dimendef\\da=140 \\skipdef\\sa=48 "
        "\\muskipdef\\ma=7 \\toksdef\\ta=30 "
        "\\chardef\\ha=65 \\mathchardef\\mb=\"2201 "
        "[\\meaning\\ca][\\meaning\\da][\\meaning\\sa][\\meaning\\ma]"
        "[\\meaning\\ta][\\meaning\\ha][\\meaning\\mb][\\meaning\\count]%",
        "[\\count298][\\dimen140][\\skip48][\\muskip7][\\toks30]"
        "[\\char\"41][\\mathchar\"2201][\\count]");
}

/* Kerns are rigid, and rules take a running dimension from the box that
   encloses them; see docs/DECISIONS.md, rules-and-kerns. */
static int test_kerns_and_rules(void)
{
    return run_snippet(
        "\\boxmaxdepth=16383.99998pt "
        "\\baselineskip=12pt \\lineskip=1pt \\lineskiplimit=0pt "
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        "\\setbox0=\\hbox{\\copy1\\kern3pt\\copy1}"
        "[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        /* A kern leaves \\prevdepth alone, so interline glue still follows. */
        "\\setbox0=\\vbox{\\copy1\\kern3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\kern3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\hbox{\\kern-2pt}[\\the\\wd0]"
        /* A running dimension measures nothing while the box is packaged. */
        "\\setbox0=\\vbox{\\hrule}[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        "\\setbox0=\\vbox{\\copy1\\hrule}[\\the\\ht0|\\the\\wd0]"
        "\\setbox0=\\vbox{\\hrule width5pt}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\vrule}[\\the\\ht0|\\the\\wd0]"
        "\\setbox0=\\hbox{\\copy1\\vrule}[\\the\\ht0|\\the\\dp0|\\the\\wd0]%",
        "[5.0pt|2.0pt|5.0pt][20.0pt|2.0pt][8.0pt|2.0pt][-2.0pt]"
        "[0.4pt|0.0pt|0.0pt][7.4pt|1.0pt][5.0pt][0.0pt|0.4pt]"
        "[5.0pt|2.0pt|1.4pt]");
}

/* A scaled dimension prints as the shortest decimal that reads back as
   itself; see docs/DECISIONS.md, scaled-printing. */
static int test_scaled_printing(void)
{
    return run_snippet(
        "\\dimen0=26214sp [\\the\\dimen0]\\dimen0=26215sp [\\the\\dimen0]"
        "\\dimen0=1sp [\\the\\dimen0]\\dimen0=10sp [\\the\\dimen0]"
        "\\dimen0=7sp [\\the\\dimen0]\\dimen0=65536sp [\\the\\dimen0]"
        "\\dimen0=65535sp [\\the\\dimen0]"
        "\\dimen0=1073741823sp [\\the\\dimen0]"
        "\\dimen0=891290sp [\\the\\dimen0]\\dimen0=3277sp [\\the\\dimen0]%",
        "[0.4pt][0.40001pt][0.00002pt][0.00015pt][0.0001pt][1.0pt]"
        "[0.99998pt][16383.99998pt][13.6pt][0.05pt]");
}

/* Page state is global rather than grouped, and an empty page reports a
   \maxdimen goal with zero totals; see docs/DECISIONS.md, page-state. */
static int test_page_state(void)
{
    return run_snippet("[\\number\\deadcycles]"
                       "[\\number\\insertpenalties]"
                       "{\\deadcycles=5 }[\\number\\deadcycles]"
                       "\\deadcycles=7 [\\number\\deadcycles]"
                       "[\\the\\pagegoal][\\the\\pagetotal][\\the\\pagedepth]"
                       "\\vskip3pt [\\the\\pagegoal]"
                       "\\pagegoal=50pt [\\the\\pagegoal]%",
                       "[0][0][5][7][16383.99998pt][0.0pt][0.0pt]"
                       "[16383.99998pt][16383.99998pt]");
}

/* Units are keywords, not maximal letter runs, and every conversion below is
   recorded in docs/DECISIONS.md under dimension-unit-arithmetic. */
static int test_dimension_units(void)
{
    const char source[] =
        "\\dimen0=1ptpt "
        "\\dimen1=1cm \\dimen2=1mm \\dimen3=1in \\dimen4=1bp "
        "\\dimen5=1dd \\dimen6=1cc \\dimen7=1pc "
        "\\dimen8=0.3cm \\dimen9=1.7mm \\dimen10=2.54cm "
        "\\dimen11=1.5sp \\dimen12=0.5sp \\dimen13=3.14159dd "
        "\\dimen20=1864679sp "
        "\\dimen21=0.3\\dimen20 \\dimen22=0.1\\dimen20 "
        "\\dimen23=2.54\\dimen20 \\dimen24=0.99999\\dimen20 "
        "\\dimen25=-0.3\\dimen20 "
        "\\dimen26=1truept \\dimen27=1PT "
        "\\skip1=1pt plus2filll minus3fil "
        "\\skip2=1pt plus2filx%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_glue orders = engine.glues[1];
    const struct hstex_glue truncated = engine.glues[2];
    int status =
        result != HSTEX_ENGINE_EOF || engine.dimens[0] != 65536 ||
        engine.dimens[1] != 1864679 || engine.dimens[2] != 186467 ||
        engine.dimens[3] != 4736286 || engine.dimens[4] != 65781 ||
        engine.dimens[5] != 70124 || engine.dimens[6] != 841489 ||
        engine.dimens[7] != 786432 || engine.dimens[8] != 559409 ||
        engine.dimens[9] != 316994 || engine.dimens[10] != 4736274 ||
        engine.dimens[11] != 1 || engine.dimens[12] != 0 ||
        engine.dimens[13] != 220300 || engine.dimens[21] != 559409 ||
        engine.dimens[22] != 186479 || engine.dimens[23] != 4736272 ||
        engine.dimens[24] != 1864650 || engine.dimens[25] != -559409 ||
        engine.dimens[26] != 65536 || engine.dimens[27] != 65536 ||
        orders.width != 65536 || orders.stretch != 131072 ||
        orders.stretch_order != 3U || orders.shrink != 196608 ||
        orders.shrink_order != 1U || truncated.width != 65536 ||
        truncated.stretch != 131072 || truncated.stretch_order != 1U ||
        truncated.shrink != 0;
    if (status != 0) {
        (void)fprintf(stderr, "dimension unit test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_dimensions_and_glue(void)
{
    const char source[] =
        "\\dimen14=\\prevdepth {\\prevdepth=2pt} \\dimen15=\\prevdepth "
        "\\advance\\prevdepth by 1pt \\dimen16=\\prevdepth "
        "\\dimendef\\d=5 \\d=1.5pt {\\d=2pt} "
        "\\dimendef\\twice=6 \\twice=2\\d "
        "\\dimendef\\largest=7 \\largest=16383.99999pt "
        "\\dimen8=7sp \\divide\\dimen8 by 2 "
        "\\dimen9=-7sp \\divide\\dimen9 by 2 "
        "\\baselineskip=13.6pt \\dimen10=472pt "
        "\\divide\\dimen10\\baselineskip \\count10=\\baselineskip "
        "\\baselineskip=655361sp \\dimen11=.5\\baselineskip "
        "\\font\\metricfont=cmr10 \\metricfont "
        "\\dimen12=2.5em \\dimen13=3.25ex "
        "\\skipdef\\s=3 \\s=-1000pt plus 1fill minus 2pt "
        "\\skip4=1pt plus 2fil minus 3pt "
        "\\skip5=2pt plus 4fill minus 1fil "
        "\\advance\\skip4 by \\skip5 "
        "\\multiply\\skip4 by 2 \\divide\\skip4 by 3 "
        "\\muskip4=3mu plus 2fil "
        "\\advance\\muskip4 by 2mu plus 1fil "
        "\\hfuzz=.1pt \\advance\\hfuzz by .2pt "
        "\\parskip=0pt plus 1pt \\advance\\parskip by 2pt%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_glue glue = engine.glues[3];
    const struct hstex_glue arithmetic_glue = engine.glues[4];
    const struct hstex_glue arithmetic_muglue = engine.muglues[4];
    int status = result != HSTEX_ENGINE_EOF || engine.dimens[5] != 98304 ||
                 engine.dimens[14] != -65536000 ||
                 engine.dimens[15] != 131072 ||
                 engine.dimens[16] != 196608 ||
                 engine.dimens[6] != 196608 ||
                 engine.dimens[7] != 1073741823 ||
                 engine.dimens[8] != 3 || engine.dimens[9] != -3 ||
                 engine.dimens[10] != 34 || engine.counts[10] != 891290 ||
                 engine.dimens[11] != 327680 ||
                 engine.dimens[12] != 1638402 ||
                 engine.dimens[13] != 917046 ||
                 glue.width != -65536000 || glue.stretch != 65536 ||
                 glue.stretch_order != 2U || glue.shrink != 131072 ||
                 glue.shrink_order != 0U ||
                 arithmetic_glue.width != 131072 ||
                 arithmetic_glue.stretch != 174762 ||
                 arithmetic_glue.stretch_order != 2U ||
                 arithmetic_glue.shrink != 43690 ||
                 arithmetic_glue.shrink_order != 1U ||
                 arithmetic_muglue.width != 327680 ||
                 arithmetic_muglue.stretch != 196608 ||
                 arithmetic_muglue.stretch_order != 1U ||
                 engine.dimen_parameters[HSTEX_DIMEN_HFUZZ] != 19661 ||
                 engine.glue_parameters[HSTEX_GLUE_PAR_SKIP].width != 131072 ||
                 engine.glue_parameters[HSTEX_GLUE_PAR_SKIP].stretch != 65536;
    if (status != 0) {
        (void)fprintf(stderr, "dimension/glue test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_token_lists(void)
{
    const char source[] =
        "\\def\\x{YZ}\\toksdef\\a=1 \\a={A\\x}"
        "\\toks0={K}\\toks2=\\a "
        "\\edef\\b{\\the\\a}\\def\\c{A\\x}"
        "\\ifx\\b\\c T\\else F\\fi "
        "\\everyjob={J}"
        "\\everyjob\\expandafter{\\the\\everyjob\\the\\toks0}"
        "{\\a={L}\\the\\a}\\the\\a\\the\\toks2\\the\\everyjob%";
    return run_snippet(source, "TLAYZAYZJK");
}

static int test_empty_hboxes(void)
{
    const char source[] =
        "\\count0=1 "
        "{\\setbox5=\\hbox{ }}"
        "\\setbox6=\\hbox to 2pt{}"
        "{\\global\\setbox7=\\hbox spread 3sp{}}"
        "\\baselineskip=13.6pt "
        "\\def\\H{height}\\def\\D{depth}\\def\\W{width}"
        "\\setbox8=\\hbox{\\count0=2 "
        "\\vrule\\H.7\\baselineskip\\D.3\\baselineskip\\W0pt}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_box rule_box = engine.boxes[8];
    uint32_t rule_identifier =
        rule_box.node_count == 1U &&
                (size_t)rule_box.node_start < engine.list_item_count
            ? engine.list_items[rule_box.node_start]
            : 0U;
    const struct hstex_node *rule =
        rule_identifier != 0U && (size_t)rule_identifier <= engine.node_count
            ? &engine.nodes[rule_identifier - 1U]
            : NULL;
    int status = result != HSTEX_ENGINE_EOF || engine.counts[0] != 1 ||
                 engine.boxes[5].kind != HSTEX_BOX_VOID ||
                 engine.boxes[6].kind != HSTEX_BOX_HLIST ||
                 engine.boxes[6].width != 2 * 65536 ||
                 engine.boxes[6].height != 0 || engine.boxes[6].depth != 0 ||
                 engine.boxes[7].kind != HSTEX_BOX_HLIST ||
                 engine.boxes[7].width != 3 ||
                 rule_box.kind != HSTEX_BOX_HLIST || rule_box.width != 0 ||
                 rule_box.height != 623900 || rule_box.depth != 267389 ||
                 rule == NULL || rule->kind != HSTEX_NODE_RULE ||
                 rule->width != 0 || rule->height != 623900 ||
                 rule->depth != 267389;
    if (status != 0) {
        (void)fprintf(stderr, "empty hbox test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_vertical_lists(void)
{
    const char source[] =
        "\\setbox9=\\vbox{\\vskip2pt plus1fil\\penalty50\\vfil}"
        "\\vbox{}\\hbox{}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_box box = engine.boxes[9];
    const struct hstex_node *glue = engine.node_count >= 1U
                                        ? &engine.nodes[0]
                                        : NULL;
    const struct hstex_node *penalty = engine.node_count >= 2U
                                           ? &engine.nodes[1]
                                           : NULL;
    const struct hstex_node *fil = engine.node_count >= 3U
                                       ? &engine.nodes[2]
                                       : NULL;
    const struct hstex_node *standalone_vbox = engine.node_count >= 4U
                                                   ? &engine.nodes[3]
                                                   : NULL;
    /* The two top-level boxes are separated by interline glue, which the
       reference also emits even when it measures zero. */
    const struct hstex_node *interline = engine.node_count >= 5U
                                             ? &engine.nodes[4]
                                             : NULL;
    const struct hstex_node *standalone_hbox = engine.node_count >= 6U
                                                   ? &engine.nodes[5]
                                                   : NULL;
    int status =
        result != HSTEX_ENGINE_EOF || box.kind != HSTEX_BOX_VLIST ||
        box.width != 0 || box.height != 131072 || box.depth != 0 ||
        box.node_count != 3U || engine.list_item_count != 3U ||
        engine.node_count != 6U || glue == NULL ||
        glue->kind != HSTEX_NODE_GLUE || glue->width != 131072 ||
        glue->value.glue.stretch != 65536 ||
        glue->value.glue.stretch_order != 1U || penalty == NULL ||
        penalty->kind != HSTEX_NODE_PENALTY ||
        penalty->value.penalty != 50 || fil == NULL ||
        fil->kind != HSTEX_NODE_GLUE || fil->width != 0 ||
        fil->value.glue.stretch != 65536 ||
        fil->value.glue.stretch_order != 1U || standalone_vbox == NULL ||
        standalone_vbox->kind != HSTEX_NODE_LIST ||
        standalone_vbox->value.list.box_kind != HSTEX_BOX_VLIST ||
        interline == NULL || interline->kind != HSTEX_NODE_GLUE ||
        interline->width != 0 || standalone_hbox == NULL ||
        standalone_hbox->kind != HSTEX_NODE_LIST ||
        standalone_hbox->value.list.box_kind != HSTEX_BOX_HLIST;
    if (status != 0) {
        (void)fprintf(stderr, "vertical-list test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_pdf_file_size(void)
{
    char data_path[64];
    if (open_snippet("size", data_path) != 0) {
        return 1;
    }
    char source[512];
    int length = snprintf(
        source, sizeof(source),
        "\\ifnum\\pdffilesize{%s}=4 T\\else F\\fi "
        "X\\pdffilesize{%s-missing}Y%%",
        data_path, data_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        (void)unlink(data_path);
        return 1;
    }
    int result = run_snippet(source, "TXY");
    (void)unlink(data_path);
    return result;
}

int main(void)
{
    if (run_snippet("\\def\\a{Alpha}\\a%", "Alpha") != 0 ||
        run_snippet("\\def\\pair#1#2{[#1/#2]}\\pair A{BC}%",
                    "[A/BC]") != 0 ||
        run_snippet("\\def\\grab#1,#2;{<#2:#1>}\\grab {a,b},c;%",
                    "<c:a,b>") != 0 ||
        run_snippet("\\def\\tag pre#1!{(#1)}\\tag preX!%", "(X)") != 0 ||
        run_snippet("\\def\\grabbrace#1#{[#1]}\\grabbrace abc{X}%",
                    "[abc]X") != 0 ||
        run_snippet("\\def\\a{G}{\\def\\a{L}\\a}\\a%", "LG") != 0 ||
        run_snippet("\\def\\a{G}{\\global\\def\\a{N}}\\a%", "N") != 0 ||
        run_snippet("\\def\\a{Q}\\let\\b=\\a\\def\\a{R}\\b\\a"
                    "\\let\\c=Z\\c%",
                    "QRZ") != 0 ||
        run_snippet("\\def\\after{A}"
                    "\\afterassignment\\after\\dimen0=1pt B%",
                    "AB") != 0 ||
        run_snippet("\\def\\after{A}"
                    "\\afterassignment\\after\\def\\x{X}\\x%",
                    "AX") != 0 ||
        run_snippet("\\afterassignment A\\afterassignment B\\count0=0%",
                    "B") != 0 ||
        run_snippet("\\def\\x{O}\\def\\y{P}"
                    "{\\afterassignment\\global\\let\\x=Z\\let\\y=Y}"
                    "\\x\\y%",
                    "OY") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{B}"
                    "\\def\\check{\\ifx\\next\\b T\\else F\\fi}"
                    "\\let\\next\\a"
                    "{\\futurelet\\next\\check\\b}"
                    "\\ifx\\next\\a T\\else F\\fi"
                    "{\\global\\futurelet\\next\\relax\\b}"
                    "\\ifx\\next\\b T\\else F\\fi%",
                    "TBTBT") != 0 ||
        run_snippet("\\def\\emit{Q}\\def\\use#1#2{#1#2}"
                    "\\def\\letspace#1{\\let#1= }"
                    "\\use{\\letspace\\s}{ }\\emit A\\s B%",
                    "QA B") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{\\def\\a{B}}"
                    "\\expandafter\\a\\b\\a%",
                    "AB") != 0 ||
        run_snippet("\\def\\a{A}\\noexpand\\a\\a%", "A") != 0 ||
        run_snippet("\\def\\hash#1{##1:#1}\\hash Z%", "#1:Z") != 0 ||
        run_snippet("\\long\\def\\a#1{X}\\a{one\n\n two}%", "X") != 0 ||
        run_snippet("\\ifnum2<1F\\else T\\fi%", "T") != 0 ||
        run_snippet("\\ifdim1pt<2pt T\\else F\\fi"
                    "\\ifdim2pt=2pt T\\else F\\fi"
                    "\\ifdim3sp>2sp T\\else F\\fi%",
                    "TTT") != 0 ||
        run_snippet("\\ifvmode T\\else F\\fi"
                    "\\ifhmode F\\else T\\fi"
                    "\\ifmmode F\\else T\\fi"
                    "\\ifinner F\\else T\\fi%",
                    "TTTT") != 0 ||
        run_snippet("\\unless\\ifnum1=2T\\else F\\fi"
                    "\\unless\\ifnum\\iftrue1\\else2\\fi=2"
                    "T\\else F\\fi%",
                    "TT") != 0 ||
        run_snippet("\\def\\first#1X{\\fi[#1]}"
                    "\\expandafter\\first\\unless\\if ABX%",
                    "[]") != 0 ||
        run_snippet("\\def\\oddvalue{\\iftrue-3\\else2\\fi}"
                    "\\ifodd\\oddvalue T\\else F\\fi"
                    "\\ifodd4F\\else T\\fi%",
                    "TT") != 0 ||
        run_snippet("\\ifcase0 A\\or X\\else Y\\fi"
                    "\\ifcase1 X\\or B\\or Y\\else Z\\fi"
                    "\\ifcase2 X\\or Y\\or C\\else Z\\fi"
                    "\\ifcase4 X\\or Y\\else D\\fi"
                    "\\ifcase-1 X\\or Y\\else E\\fi"
                    "\\ifcase1 X\\ifcase0 N\\or M\\fi\\or F\\fi%",
                    "ABCDEF") != 0 ||
        run_snippet("\\iffalse A\\iftrue B\\fi\\else C\\fi%", "C") !=
            0 ||
        run_snippet("\\ifx\\unknown\\alsoUnknown T\\else F\\fi%", "T") !=
            0 ||
        run_snippet("\\def\\a#1{[#1]}\\def\\b#1{[#1]}"
                    "\\ifx\\a\\b T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\chardef\\A=65 \\ifnum\\A=65 \\A\\else X\\fi%",
                    "A") != 0 ||
        run_snippet("\\catcode94=7 \\catcode9=10 "
                    "\\catcode`\\^^I=13 "
                    "\\ifnum\\catcode9=13 T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\mathchardef\\M=1000 \\ifnum\\M=1000 T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\meaning\\radical%", "\\radical") != 0 ||
        run_snippet("\\let\\R\\radical\\meaning\\R%", "\\radical") != 0 ||
        run_snippet("\\ifx\\radical\\undefined F\\else T\\fi%", "T") !=
            0 ||
        run_snippet("\\ifx\\marks\\undefined F\\else T\\fi"
                    "\\let\\R\\marks\\meaning\\R%",
                    "T\\marks") != 0 ||
        run_snippet("\\countdef\\n=7 \\n=1 {\\n=2 \\ifnum\\n=2 L\\fi}"
                    "\\ifnum\\n=1 G\\fi%",
                    "LG") != 0 ||
        run_snippet("\\the\\maxdeadcycles/"
                    "{\\maxdeadcycles=100\\relax"
                    "\\the\\maxdeadcycles/}"
                    "\\the\\maxdeadcycles%",
                    "25/100/25") != 0 ||
        run_snippet("\\the\\tracingstats/"
                    "{\\tracingstats=1\\relax\\the\\tracingstats/}"
                    "\\the\\tracingstats%",
                    "0/1/0") != 0 ||
        run_snippet("\\the\\pdfshellescape/"
                    "\\number\\pdfshellescape/"
                    "\\ifnum\\pdfshellescape=0 T\\else F\\fi/"
                    "\\meaning\\pdfshellescape%",
                    "0/0/T/\\pdfshellescape") != 0 ||
        run_snippet("\\the\\lefthyphenmin/\\the\\righthyphenmin/"
                    "{\\lefthyphenmin=2\\righthyphenmin=3\\relax"
                    "\\the\\lefthyphenmin/\\the\\righthyphenmin/}"
                    "\\the\\lefthyphenmin/\\the\\righthyphenmin%",
                    "0/0/2/3/0/0") != 0 ||
        run_snippet("\\dimendef\\d=3 \\d=2sp "
                    "\\ifnum1<\\d T\\else F\\fi/\\number\\d%",
                    "T/2") != 0 ||
        run_snippet("\\countdef\\n=1 \\n=100 \\divide\\n by 6 "
                    "\\multiply\\n -3 \\advance\\n 2 \\number\\n%",
                    "-46") != 0 ||
        run_snippet("\\romannumeral1994 \\def\\a{A}"
                    "\\romannumeral0\\a\\romannumeral-5%",
                    "mcmxcivA") != 0 ||
        run_snippet("\\number\\numexpr2+3*4\\relax/"
                    "\\the\\numexpr(2+3)*4\\relax/"
                    "\\number\\numexpr5/2\\relax/"
                    "\\number\\numexpr-5/2\\relax/"
                    "\\number\\numexpr1-5/2\\relax/"
                    "\\number\\numexpr7/-2\\relax%",
                    "14/20/3/-3/-2/-4") != 0 ||
        run_snippet("\\number\"FF/\\number'17/"
                    "\\the\\numexpr\"10+\"F\\relax%",
                    "255/15/31") != 0 ||
        run_snippet("\\dimendef\\d=0 "
                    "\\d=\\dimexpr 2pt+3pt*4\\relax "
                    "\\the\\d/"
                    "\\the\\dimexpr(2pt+3pt)*4\\relax/"
                    "\\the\\dimexpr5sp/2\\relax/"
                    "\\the\\dimexpr-5sp/2\\relax/"
                    "\\the\\dimexpr7sp/-2\\relax%",
                    "14.0pt/20.0pt/0.00005pt/-0.00005pt/-0.00006pt") != 0 ||
        run_snippet("\\font\\f=cmr10 at 1sp "
                    "\\fontdimen1\\f=123sp "
                    "\\fontdimen20\\f=456sp "
                    "\\hyphenchar\\f=7 "
                    "\\font\\g=cmr10 at 1sp "
                    "\\ifx\\f\\g T\\else F\\fi/"
                    "{\\fontdimen1\\f=321sp}"
                    "\\the\\fontdimen1\\f/"
                    "\\the\\fontdimen20\\f/"
                    "\\the\\hyphenchar\\f/"
                    "\\fontname\\f%",
                    "T/0.0049pt/0.00696pt/7/cmr10 at 0.00002pt") != 0 ||
        run_snippet("\\font\\natural=cmr10 "
                    "\\font\\tiny=cmr10 at 1sp "
                    "\\meaning\\natural/\\meaning\\tiny%",
                    "select font cmr10/"
                    "select font cmr10 at 0.00002pt") != 0 ||
        run_snippet("\\font\\f=cmr10 at 1sp \\f "
                    "\\hyphenchar\\font=9 "
                    "\\the\\hyphenchar\\f/\\fontname\\font%",
                    "9/cmr10 at 0.00002pt") != 0 ||
        run_snippet("\\font\\line=line10\\relax \\the\\fontdimen8\\line%",
                    "0.39998pt") != 0 ||
        run_snippet("\\fontname\\font/\\fontname\\nullfont/"
                    "\\meaning\\nullfont/\\the\\hyphenchar\\nullfont/"
                    "\\the\\skewchar\\nullfont/"
                    "\\the\\fontdimen1\\nullfont%",
                    "nullfont/nullfont/select font nullfont/45/-1/0.0pt") != 0 ||
        run_snippet("\\skipdef\\s=0 "
                    "\\s=\\glueexpr 1pt plus 2pt minus 3pt"
                    "+4pt plus 5pt minus 6pt\\relax "
                    "\\the\\s/"
                    "\\the\\glueexpr 1pt plus 2fil"
                    "+3pt plus 4fill\\relax/"
                    "\\the\\glueexpr(1pt plus 2fil)*3\\relax/"
                    "\\the\\glueexpr 1pt plus 5sp minus 7sp/2\\relax%",
                    "5.0pt plus 7.0pt minus 9.0pt/"
                    "4.0pt plus 4.0fill/"
                    "3.0pt plus 6.0fil/"
                    "0.5pt plus 0.00005pt minus 0.00006pt") != 0 ||
        run_snippet("\\skip0=7pt "
                    "\\muskipdef\\m=0 "
                    "\\m=\\muexpr 1mu plus 2mu minus 3mu"
                    "+3mu plus 4fil minus 5mu\\relax "
                    "\\the\\m/"
                    "{\\m=9mu\\relax \\the\\m/}"
                    "\\the\\m/\\the\\skip0/"
                    "\\the\\muexpr(1mu plus 2fil)*3\\relax%",
                    "4.0mu plus 4.0fil minus 8.0mu/"
                    "9.0mu/"
                    "4.0mu plus 4.0fil minus 8.0mu/"
                    "7.0pt/3.0mu plus 6.0fil") != 0 ||
        run_snippet("\\thinmuskip=3mu plus 2mu minus 1mu "
                    "{\\thinmuskip=9mu\\relax \\the\\thinmuskip/}"
                    "\\the\\thinmuskip/"
                    "\\medmuskip=4mu plus 2mu minus 4mu "
                    "\\thickmuskip=5mu plus 5mu "
                    "\\the\\medmuskip/\\the\\thickmuskip%",
                    "9.0mu/3.0mu plus 2.0mu minus 1.0mu/"
                    "4.0mu plus 2.0mu minus 4.0mu/5.0mu plus 5.0mu") != 0 ||
        run_snippet("\\skip0=3pt plus 4fil minus 5pt "
                    "\\skip1=2\\skip0 "
                    "\\skip2=-\\skip0 "
                    "\\skip3=1.5\\skip0 "
                    "\\the\\skip1/\\the\\skip2/\\the\\skip3%",
                    "6.0pt/-3.0pt plus -4.0fil minus -5.0pt/4.5pt") != 0 ||
        run_snippet("\\def\\name{abc:def}"
                    "\\def\\split#1:#2!{[#1/#2]}"
                    "\\expandafter\\split\\romannumeral`A\\name!%",
                    "[lxvabc/def]") != 0 ||
        run_snippet("\\protected\\def\\zero{0}"
                    "\\edef\\saved{\\romannumeral\\zero X}"
                    "\\def\\zero{1}\\saved%",
                    "X") != 0 ||
        run_snippet("\\def\\use#1#2{#1#2}"
                    "\\use{\\let\\stop= }{ }"
                    "\\edef\\saved{\\ifnum1=1\\stop A\\fi}"
                    "\\def\\expected{A}"
                    "\\ifx\\saved\\expected T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\countdef\\n=2 \\n=7 "
                    "\\def\\two#1{\\ifnum#1<10 0\\fi\\number#1}"
                    "\\edef\\saved{\\two{\\the\\n}}\\n=42 \\saved%",
                    "07") != 0 ||
        run_snippet("\\def\\a{A}\\edef\\saved{\\noexpand\\a}"
                    "\\def\\a{B}\\saved%",
                    "B") != 0 ||
        run_snippet("\\protected\\def\\a{A}\\edef\\saved{\\a}"
                    "\\def\\a{B}\\saved%",
                    "B") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{B}"
                    "\\expanded{<\\a\\b>}"
                    "\\expanded{\\noexpand\\a}%",
                    "<AB>A") != 0 ||
        run_snippet("\\def\\trim#1{[#1]}"
                    "\\edef\\saved{\\expandafter\\trim"
                    "\\expanded{{A\\iffalse}}}\\fi B}}}"
                    "\\saved%",
                    "[AB]") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\expandafter\\ifx\\expanded{\\unexpanded{e}}e"
                    "T\\else F\\fi"
                    "\\expandafter\\ifx\\expanded{\\noexpand\\a}\\a "
                    "T\\else F\\fi"
                    "\\expandafter\\ifx\\noexpand\\a\\a "
                    "T\\else F\\fi%",
                    "TTF") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\def\\choose{\\if\\noexpand\\a\\noexpand\\a"
                    "\\edef\\saved{X}\\fi}"
                    "\\choose\\saved%",
                    "X") != 0 ||
        run_snippet("\\def\\comma#1,{[#1]}"
                    "\\expandafter\\comma"
                    "\\expanded{\\unexpanded{A,}}"
                    "\\def\\prefix A#1{<#1>}"
                    "\\expandafter\\prefix"
                    "\\expanded{\\unexpanded{A}}B%",
                    "[A]<B>") != 0 ||
        run_snippet("\\def\\left{abc}"
                    "\\pdfstrcmp{abc}{abc}/"
                    "\\pdfstrcmp{abc}{abd}/"
                    "\\pdfstrcmp{abd}{abc}/"
                    "\\pdfstrcmp{\\left}{abc}/"
                    "\\pdfstrcmp{\\noexpand\\relax}"
                    "{\\noexpand\\relax}%",
                    "0/-1/1/0/0") != 0 ||
        run_snippet("\\expanded\\expanded{{A}}%", "A") != 0 ||
        run_snippet("\\protected\\def\\a{A}"
                    "\\edef\\saved{\\expanded{\\a}}"
                    "\\def\\a{B}\\saved%",
                    "B") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\edef\\saved{\\unexpanded{\\a}}"
                    "\\def\\a{B}\\saved\\unexpanded{\\a}%",
                    "BB") != 0 ||
        run_snippet("\\edef\\saved{\\unexpanded{#1}}\\saved/"
                    "\\toks0={#2}\\edef\\fromtoks{\\the\\toks0}"
                    "\\fromtoks%",
                    "#1/#2") != 0 ||
        run_snippet("\\def\\a{A}\\unexpanded\\expanded{{\\a}}%",
                    "A") != 0 ||
        run_snippet("\\lowercase{AB}\\uppercase{ab}"
                    "\\def\\opening{{C}}\\lowercase\\opening%",
                    "abABc") != 0 ||
        run_snippet("A\\ignorespaces   B"
                    "\\def\\s{ }\\ignorespaces\\s C%",
                    "ABC") != 0 ||
        run_snippet("\\ifx\\batchmode\\errorstopmode T\\else F\\fi"
                    "\\batchmode A\\dump B%",
                    "FA") != 0 ||
        run_snippet("\\lccode`*=32 "
                    "\\def\\samecat#1{\\ifcat#1*P\\else F\\fi}"
                    "\\lowercase{\\samecat*}%",
                    "P") != 0 ||
        run_snippet("\\catcode`~=13 \\def~{T}"
                    "\\catcode`!=13 \\def!{B}"
                    "\\lccode`~=`! \\lowercase{~}%",
                    "B") != 0 ||
        run_snippet("\\def\\expected{\\relax}"
                    "\\expanded{\\noexpand\\def\\noexpand\\saved"
                    "{\\noexpand\\relax}}"
                    "\\ifx\\saved\\expected T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\def\\same#1{\\ifx\\a#1T\\else F\\fi}"
                    "\\expanded{\\unexpanded{\\same}"
                    "\\unexpanded{\\a}}%",
                    "T") != 0 ||
        run_snippet("\\def\\a{X}"
                    "\\detokenize{A \\a!\\, {B}}"
                    "\\detokenize{#1}%",
                    "A \\a !\\, {B}##1") != 0 ||
        run_snippet("\\detokenize\\expanded{{A}}%", "A") != 0 ||
        run_snippet(
            "\\expandafter\\def"
            "\\csname\\detokenize{name_with:chars}\\endcsname{D}"
            "\\csname name_with:chars\\endcsname%",
            "D") != 0 ||
        run_snippet("\\def\\a{./}\\def\\strip#1>{}"
                    "\\edef\\saved{\\expandafter\\strip\\meaning\\a}"
                    "\\saved%",
                    "./") != 0 ||
        run_snippet("{\\catcode`\\^=7 \\catcode`\\^^J=13 "
                    "\\xdef\\saved{\\string^^J}}"
                    "\\saved%",
                    "^^J") != 0 ||
        run_snippet("\\if AAT\\else F\\fi\\if ABF\\else T\\fi%",
                    "TT") != 0 ||
        run_snippet("\\def\\base#1{\\if c#1N\\else"
                    "\\if o#1n\\else X\\fi\\fi}"
                    "\\def\\digit{\\iftrue1\\else2\\fi}"
                    "\\if n\\base oT\\else F\\fi"
                    "\\ifnum\\digit=1T\\else F\\fi%",
                    "TT") != 0 ||
        run_snippet("\\let\\x=A\\def\\letter{A}\\chardef\\A=65 "
                    "\\ifcat AB1\\else0\\fi"
                    "\\ifcat A70\\else1\\fi"
                    "\\ifcat\\x B1\\else0\\fi"
                    "\\ifcat\\letter B1\\else0\\fi"
                    "\\ifcat\\A\\relax1\\else0\\fi"
                    "\\ifcat\\relax\\def1\\else0\\fi"
                    "\\ifcat\\noexpand\\missing\\noexpand\\other"
                    "1\\else0\\fi%",
                    "1111111") != 0 ||
        run_snippet("\\sfcode`\\)=0 \\ifnum\\sfcode`\\)=0 T\\else F\\fi "
                    "\\ifdefined\\sfcode T\\else F\\fi "
                    "\\ifdefined\\unknown F\\else T\\fi%",
                    "TTT") != 0 ||
        run_snippet(
            "\\expandafter\\ifx\\csname absent:name\\endcsname\\relax "
            "T\\else F\\fi"
            "\\def\\part{dynamic}"
            "\\expandafter\\def\\csname\\part-name\\endcsname{D}"
            "\\csname dynamic-name\\endcsname"
            "\\expandafter\\def\\csname a b\\endcsname{S}"
            "\\csname a b\\endcsname"
            "{\\csname scoped-name\\endcsname}"
            "\\ifdefined\\scoped-name F\\else L\\fi"
            "\\expandafter\\ifx\\csname scoped-name\\endcsname\\relax "
            "T\\else F\\fi%",
            "TDSLT") != 0 ||
        run_snippet(
            "\\def\\part{made}"
            "\\expandafter\\def\\csname made-up\\endcsname{M}"
            "\\ifcsname absent\\endcsname A\\else B\\fi"
            "\\ifdefined\\absent C\\else D\\fi"
            "\\ifcsname\\part-up\\endcsname E\\else F\\fi"
            "\\ifcsname tokenized\\endcsname G\\else H\\fi"
            "\\ifdefined\\tokenized I\\else J\\fi%",
            "BDEHJ") != 0 ||
        run_snippet(
            "\\def\\piece{name}"
            "\\expandafter\\def"
            "\\csname\\expanded{\\noexpand\\piece}\\endcsname{X}"
            "\\name%",
            "X") != 0 ||
        run_snippet("\\catcode`\\@=11 \\def\\word@word{X}\\word@word%",
                    "X") != 0 ||
        run_snippet("\\ifdefined\\widowpenalties T\\else F\\fi%", "T") !=
            0 ||
        run_snippet("{\\aftergroup A\\aftergroup B}C"
                    "{\\aftergroup D{\\aftergroup E}\\aftergroup F}G%",
                    "ABCEDFG") != 0 ||
        run_snippet("\\ifnum\\currentgrouplevel=0 T\\else F\\fi"
                    "{\\ifnum\\currentgrouplevel=1 T\\else F\\fi}"
                    "\\ifnum\\currentgrouplevel=0 T\\else F\\fi%",
                    "TTT") != 0 ||
        run_snippet("\\edef\\a{\\meaning\\over}"
                    "\\edef\\b{\\string\\over}"
                    "\\ifx\\a\\b T\\else F\\fi "
                    "\\let\\savedover\\over "
                    "\\edef\\c{\\meaning\\savedover}"
                    "\\ifx\\b\\c T\\else F\\fi%",
                    "TT") != 0 ||
        expect_failure("\\endcsname%", "extra endcsname") != 0 ||
        expect_failure("\\unknown%",
                       "undefined control sequence: \\unknown") != 0 ||
        expect_failure("\\ifcat\\missing\\relax T\\else F\\fi%",
                       "undefined control sequence: \\missing") != 0 ||
        expect_failure("\\expanded{\\number}%", "missing integer") != 0 ||
        expect_failure("\\baselineskip=1\\relax%",
                       "illegal unit of measure") != 0 ||
        expect_failure("\\skip1=1pt plus2fillll%",
                       "infinite glue order beyond filll") != 0 ||
        run_snippet("\\dimen0=1ptpt\\dimen1=1inch%", "ptch") != 0 ||
        expect_failure("\\def\\why{expanded}"
                       "\\errmessage{ERRMESSAGE: \\why}%",
                       "ERRMESSAGE: expanded") != 0 ||
        expect_failure("\\def\\a#1{X}\\a{one\n\n two}%",
                       "non-long macro argument") != 0 ||
        test_macro_flags() != 0 || test_ini_bootstrap() != 0 ||
        test_input_primitive() != 0 || test_job_name() != 0 ||
        test_hyphenation_data() != 0 ||
        test_document_job_transition() != 0 || test_file_streams() != 0 ||
        /* A parameter-category character is displayed doubled, so that the
           display reads back as the same token. */
        run_snippet("\\def\\s#1{##1#1}[\\s{Q}][\\meaning\\s]%",
                    "[#1Q][macro:#1->##1#1]") != 0 ||
        run_snippet("[\\pdfescapestring{a b(c)\\string\\\\}]"
                    "[\\pdfescapename{a b}][\\pdfescapehex{AB}]"
                    "[\\pdfunescapehex{4142}][\\the\\pdfpxdimen]%",
                    "[a\\040b\\(c\\)\\\\\\\\][a#20b][4142][AB][1.00375pt]") != 0 ||
        /* A control sequence inserted by \the is ordinary in execution, so
           \expandafter expands it. cleveref builds definitions this way. */
        run_snippet("\\toks0={\\expandafter\\def\\csname AB\\endcsname}"
                    "\\the\\toks0 {X}"
                    "[\\expandafter\\meaning\\csname AB\\endcsname]%",
                    "[macro:->X]") != 0 ||
        /* A `true` unit is measured before magnification. */
        run_snippet("\\mag=2000 \\dimen0=1truept \\dimen1=2.54truecm "
                    "\\dimen2=1truein \\dimen3=1pt "
                    "[\\number\\dimen0][\\number\\dimen1]"
                    "[\\number\\dimen2][\\number\\dimen3]%",
                    "[32768][2368122][2368143][65536]") != 0 ||
        /* \protected stops expansion while \edef builds a token list, but a
           csname is still expanded in full. */
        run_snippet("\\protected\\def\\PP{AB}\\def\\ABC{Z}"
                    "\\edef\\z{\\csname \\PP C\\endcsname}\\z"
                    "\\edef\\w{\\ifcsname \\PP C\\endcsname Y\\else N\\fi}\\w%",
                    "ZY") != 0 ||
        /* \noexpand before a parameter marker leaves it a parameter marker,
           unlike \unexpanded and \the, whose tokens are inserted verbatim.
           hyperref's \HyLang@DeclareLang depends on the difference. */
        run_snippet("\\def\\o#1{\\edef\\x##1##2{[\\noexpand##1|\\noexpand##2|#1]}}"
                    "\\o{A}\\x{P}{Q}%",
                    "[P|Q|A]") != 0 ||
        test_font_character_metrics() != 0 || test_protrusion_codes() != 0 ||
        test_box_and_font_conditionals() != 0 ||
        test_last_node_and_pdf_objects() != 0 ||
        test_scan_tokens() != 0 || test_unevaluated_conditionals() != 0 ||
        test_defined_register_meanings() != 0 ||
        test_box_shift_and_packaging() != 0 || test_kerns_and_rules() != 0 ||
        test_scaled_printing() != 0 ||
        test_page_state() != 0 || test_dimension_units() != 0 ||
        test_pdftex_identification() != 0 ||
        /* \typeout writes to an allocated but unopened stream, and \write
           accepts any stream number. Neither may fail or consume output. */
        run_snippet("\\immediate\\write5{A}\\immediate\\write200{B}"
                    "\\immediate\\write-1{C}X%",
                    "X") != 0 ||
        run_snippet("A\\end B%", "A") != 0 ||
        expect_failure("\\hbox{}\\the\\pagegoal%",
                       "page totals require the page builder") != 0 ||
        test_dimensions_and_glue() != 0 || test_token_lists() != 0 ||
        test_empty_hboxes() != 0 || test_vertical_lists() != 0 ||
        test_pdf_file_size() != 0) {
        return 1;
    }
    return 0;
}
