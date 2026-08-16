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
                          char *error, size_t error_capacity)
{
    if (hstex_engine_init(engine, error, error_capacity) != 0) {
        return -1;
    }
    if (hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'{',
                          (uint8_t)HSTEX_CAT_BEGIN_GROUP) != 0 ||
        hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'}',
                          (uint8_t)HSTEX_CAT_END_GROUP) != 0 ||
        hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'#',
                          (uint8_t)HSTEX_CAT_PARAMETER) != 0 ||
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
    if (prepare_engine(&engine, path, error, sizeof(error)) != 0) {
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
    if (prepare_engine(&engine, path, error, sizeof(error)) != 0) {
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
    if (prepare_engine(&engine, path, error, sizeof(error)) != 0) {
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
        expect_failure("\\def\\a#1{X}\\a{one\n\n two}%",
                       "non-long macro argument") != 0 ||
        test_macro_flags() != 0) {
        return 1;
    }
    return 0;
}
