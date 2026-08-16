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
    if (open_snippet("\\def\\fromchild{C}%", child_path) != 0) {
        return 1;
    }
    char source[256];
    int length = snprintf(source, sizeof(source), "\\input{%s}\\fromchild%%",
                          child_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        (void)unlink(child_path);
        return 1;
    }
    int status = run_snippet(source, "C");
    (void)unlink(child_path);
    return status;
}

static int test_file_streams(void)
{
    char stream_path[64];
    if (open_snippet("", stream_path) != 0 || unlink(stream_path) != 0) {
        return 1;
    }
    char source[768];
    int length = snprintf(
        source, sizeof(source),
        "\\chardef\\stream=3 \\openout\\stream=%s \\def\\expected{abc}"
        "\\write\\stream{\\expected}\\closeout\\stream "
        "\\openin\\stream=%s \\ifeof\\stream F\\else "
        "\\read\\stream to \\actual "
        "\\ifx\\actual\\expected T\\else F\\fi\\fi "
        "\\closein\\stream%%",
        stream_path, stream_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        return 1;
    }
    int status = run_snippet(source, "T");
    (void)unlink(stream_path);
    return status;
}

static int test_dimensions_and_glue(void)
{
    const char source[] =
        "\\dimendef\\d=5 \\d=1.5pt {\\d=2pt} "
        "\\dimendef\\twice=6 \\twice=2\\d "
        "\\dimendef\\largest=7 \\largest=16383.99999pt "
        "\\skipdef\\s=3 \\s=-1000pt plus 1fill minus 2pt "
        "\\hfuzz=.1pt \\parskip=0pt plus 1pt%";
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
    int status = result != HSTEX_ENGINE_EOF || engine.dimens[5] != 98304 ||
                 engine.dimens[6] != 196608 ||
                 engine.dimens[7] != 1073741823 ||
                 glue.width != -65536000 || glue.stretch != 65536 ||
                 glue.stretch_order != 2U || glue.shrink != 131072 ||
                 glue.shrink_order != 0U ||
                 engine.dimen_parameters[HSTEX_DIMEN_HFUZZ] != 6554 ||
                 engine.glue_parameters[HSTEX_GLUE_PAR_SKIP].stretch != 65536;
    if (status != 0) {
        (void)fprintf(stderr, "dimension/glue test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

int main(void)
{
    if (run_snippet("\\def\\a{Alpha}\\a%", "Alpha") != 0 ||
        run_snippet("\\def\\pair#1#2{[#1/#2]}\\pair A{BC}%",
                    "[A/BC]") != 0 ||
        run_snippet("\\def\\grab#1,#2;{<#2:#1>}\\grab {a,b},c;%",
                    "<c:a,b>") != 0 ||
        run_snippet("\\def\\tag pre#1!{(#1)}\\tag preX!%", "(X)") != 0 ||
        run_snippet("\\def\\a{G}{\\def\\a{L}\\a}\\a%", "LG") != 0 ||
        run_snippet("\\def\\a{G}{\\global\\def\\a{N}}\\a%", "N") != 0 ||
        run_snippet("\\def\\a{Q}\\let\\b=\\a\\def\\a{R}\\b\\a"
                    "\\let\\c=Z\\c%",
                    "QRZ") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{\\def\\a{B}}"
                    "\\expandafter\\a\\b\\a%",
                    "AB") != 0 ||
        run_snippet("\\def\\a{A}\\noexpand\\a\\a%", "A") != 0 ||
        run_snippet("\\def\\hash#1{##1:#1}\\hash Z%", "#1:Z") != 0 ||
        run_snippet("\\long\\def\\a#1{X}\\a{one\n\n two}%", "X") != 0 ||
        run_snippet("\\ifnum2<1F\\else T\\fi%", "T") != 0 ||
        run_snippet("\\iffalse A\\iftrue B\\fi\\else C\\fi%", "C") !=
            0 ||
        run_snippet("\\ifx\\unknown\\alsoUnknown T\\else F\\fi%", "T") !=
            0 ||
        run_snippet("\\def\\a#1{[#1]}\\def\\b#1{[#1]}"
                    "\\ifx\\a\\b T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\chardef\\A=65 \\ifnum\\A=65 \\A\\else X\\fi%",
                    "A") != 0 ||
        run_snippet("\\mathchardef\\M=1000 \\ifnum\\M=1000 T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\countdef\\n=7 \\n=1 {\\n=2 \\ifnum\\n=2 L\\fi}"
                    "\\ifnum\\n=1 G\\fi%",
                    "LG") != 0 ||
        run_snippet("\\countdef\\n=1 \\n=100 \\divide\\n by 6 "
                    "\\multiply\\n -3 \\advance\\n 2 \\number\\n%",
                    "-46") != 0 ||
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
        run_snippet("\\sfcode`\\)=0 \\ifnum\\sfcode`\\)=0 T\\else F\\fi "
                    "\\ifdefined\\sfcode T\\else F\\fi "
                    "\\ifdefined\\unknown F\\else T\\fi%",
                    "TTT") != 0 ||
        run_snippet("\\catcode`\\@=11 \\def\\word@word{X}\\word@word%",
                    "X") != 0 ||
        expect_failure("\\def\\a#1{X}\\a{one\n\n two}%",
                       "non-long macro argument") != 0 ||
        test_macro_flags() != 0 || test_ini_bootstrap() != 0 ||
        test_input_primitive() != 0 || test_file_streams() != 0 ||
        test_dimensions_and_glue() != 0) {
        return 1;
    }
    return 0;
}
