#include "hstex/catcode.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/symbol.h"
#include "hstex/token.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct hstex_lexical_state lexical_state;
    struct hstex_mouth mouth;
    char error[256];
};

static int fixture_init(struct fixture *fixture, const uint8_t *input,
                        size_t length)
{
    fixture->error[0] = '\0';
    if (hstex_lexical_state_init(&fixture->lexical_state, fixture->error,
                                 sizeof(fixture->error)) != 0) {
        (void)fprintf(stderr, "%s\n", fixture->error);
        return 1;
    }
    hstex_mouth_init(&fixture->mouth, input, length, &fixture->lexical_state);
    return 0;
}

static void fixture_destroy(struct fixture *fixture)
{
    hstex_mouth_destroy(&fixture->mouth);
    hstex_lexical_state_destroy(&fixture->lexical_state);
}

static int next_token(struct fixture *fixture, hstex_token *token,
                      struct hstex_source_location *location)
{
    enum hstex_mouth_result result = hstex_mouth_next(
        &fixture->mouth, token, location, fixture->error, sizeof(fixture->error));
    if (result != HSTEX_MOUTH_TOKEN) {
        (void)fprintf(stderr, "expected token, result=%d error=%s\n", (int)result,
                      fixture->error);
        return 1;
    }
    return 0;
}

static int expect_character(struct fixture *fixture, enum hstex_catcode category,
                            uint8_t character)
{
    hstex_token token;
    struct hstex_source_location location;
    if (next_token(fixture, &token, &location) != 0 ||
        !hstex_token_is_character(token) ||
        hstex_token_category(token) != (uint8_t)category ||
        hstex_token_character_code(token) != character) {
        (void)fprintf(stderr, "expected character cat=%u char=%u\n",
                      (unsigned int)category, (unsigned int)character);
        return 1;
    }
    return 0;
}

static int expect_control(struct fixture *fixture, enum hstex_symbol_kind kind,
                          const uint8_t *name, size_t length)
{
    hstex_token token;
    struct hstex_source_location location;
    if (next_token(fixture, &token, &location) != 0 ||
        !hstex_token_is_control_sequence(token)) {
        (void)fprintf(stderr, "expected control sequence\n");
        return 1;
    }
    enum hstex_symbol_kind actual_kind;
    const uint8_t *actual_name = NULL;
    size_t actual_length = 0U;
    if (hstex_symbol_name(&fixture->lexical_state.symbols,
                          hstex_token_control_sequence_id(token), &actual_kind,
                          &actual_name, &actual_length) != 0 ||
        actual_kind != kind || actual_length != length ||
        (length != 0U && memcmp(actual_name, name, length) != 0)) {
        (void)fprintf(stderr, "control-sequence name mismatch\n");
        return 1;
    }
    return 0;
}

static int expect_eof(struct fixture *fixture)
{
    hstex_token token;
    struct hstex_source_location location;
    enum hstex_mouth_result result = hstex_mouth_next(
        &fixture->mouth, &token, &location, fixture->error,
        sizeof(fixture->error));
    if (result != HSTEX_MOUTH_EOF) {
        (void)fprintf(stderr, "expected EOF, result=%d error=%s\n", (int)result,
                      fixture->error);
        return 1;
    }
    return 0;
}

static int test_state_machine(void)
{
    static const uint8_t input[] =
        "  abc  def\n\n\\foo   x\\! y%discarded\n";
    struct fixture fixture;
    if (fixture_init(&fixture, input, sizeof(input) - 1U) != 0) {
        return 1;
    }
    static const uint8_t foo[] = {'f', 'o', 'o'};
    static const uint8_t bang[] = {'!'};
    static const uint8_t par[] = {'p', 'a', 'r'};
    int failed = expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'a') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'b') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'c') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'d') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'e') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'f') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_control(&fixture, HSTEX_SYMBOL_REGULAR, par,
                                sizeof(par)) ||
                 expect_control(&fixture, HSTEX_SYMBOL_REGULAR, foo,
                                sizeof(foo)) ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'x') ||
                 expect_control(&fixture, HSTEX_SYMBOL_REGULAR, bang,
                                sizeof(bang)) ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'y') ||
                 expect_eof(&fixture);
    fixture_destroy(&fixture);
    return failed;
}

static int test_controls_and_active(void)
{
    static const uint8_t input[] = "\\~ ~ \\  z\n";
    struct fixture fixture;
    if (fixture_init(&fixture, input, sizeof(input) - 1U) != 0) {
        return 1;
    }
    (void)hstex_catcode_set(&fixture.lexical_state.catcodes, (uint32_t)'~',
                            HSTEX_CAT_ACTIVE);
    static const uint8_t tilde[] = {'~'};
    static const uint8_t space[] = {' '};
    int failed = expect_control(&fixture, HSTEX_SYMBOL_REGULAR, tilde,
                                sizeof(tilde)) ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_control(&fixture, HSTEX_SYMBOL_ACTIVE, tilde,
                                sizeof(tilde)) ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_control(&fixture, HSTEX_SYMBOL_REGULAR, space,
                                sizeof(space)) ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'z') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_eof(&fixture);
    fixture_destroy(&fixture);
    return failed;
}

static int test_mutable_catcodes(void)
{
    static const uint8_t input[] = "@ \\foo@bar\n";
    struct fixture fixture;
    if (fixture_init(&fixture, input, sizeof(input) - 1U) != 0) {
        return 1;
    }
    if (expect_character(&fixture, HSTEX_CAT_OTHER, (uint8_t)'@') != 0 ||
        hstex_catcode_set(&fixture.lexical_state.catcodes, (uint32_t)'@',
                          HSTEX_CAT_LETTER) != 0 ||
        expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') != 0) {
        fixture_destroy(&fixture);
        return 1;
    }
    static const uint8_t name[] = {'f', 'o', 'o', '@', 'b', 'a', 'r'};
    int failed = expect_control(&fixture, HSTEX_SYMBOL_REGULAR, name,
                                sizeof(name)) ||
                 expect_eof(&fixture);
    fixture_destroy(&fixture);
    return failed;
}

static int test_caret_and_line_endings(void)
{
    static const uint8_t input[] =
        "^^41^^Mdiscard\n^^5eX^^41\r\na   \r\nb\t \r";
    struct fixture fixture;
    if (fixture_init(&fixture, input, sizeof(input) - 1U) != 0) {
        return 1;
    }
    (void)hstex_catcode_set(&fixture.lexical_state.catcodes, (uint32_t)'^',
                            HSTEX_CAT_SUPERSCRIPT);
    int failed = expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'A') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_character(&fixture, HSTEX_CAT_SUPERSCRIPT,
                                  (uint8_t)'^') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'X') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'A') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'a') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_character(&fixture, HSTEX_CAT_LETTER, (uint8_t)'b') ||
                 expect_character(&fixture, HSTEX_CAT_OTHER, (uint8_t)'\t') ||
                 expect_character(&fixture, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_eof(&fixture);
    fixture_destroy(&fixture);
    return failed;
}

static int test_suppressed_and_embedded_end_lines(void)
{
    static const uint8_t no_end_line[] = "a\n\nb";
    struct fixture first;
    if (fixture_init(&first, no_end_line, sizeof(no_end_line) - 1U) != 0) {
        return 1;
    }
    first.lexical_state.end_line_character = -1;
    int failed = expect_character(&first, HSTEX_CAT_LETTER, (uint8_t)'a') ||
                 expect_character(&first, HSTEX_CAT_LETTER, (uint8_t)'b') ||
                 expect_eof(&first);
    fixture_destroy(&first);
    if (failed != 0) {
        return 1;
    }

    static const uint8_t embedded[] = "a!discard\nb\n";
    struct fixture second;
    if (fixture_init(&second, embedded, sizeof(embedded) - 1U) != 0) {
        return 1;
    }
    (void)hstex_catcode_set(&second.lexical_state.catcodes, (uint32_t)'!',
                            HSTEX_CAT_END_OF_LINE);
    failed = expect_character(&second, HSTEX_CAT_LETTER, (uint8_t)'a') ||
             expect_character(&second, HSTEX_CAT_SPACE, (uint8_t)' ') ||
             expect_character(&second, HSTEX_CAT_LETTER, (uint8_t)'b') ||
             expect_character(&second, HSTEX_CAT_SPACE, (uint8_t)' ') ||
             expect_eof(&second);
    fixture_destroy(&second);
    return failed;
}

static int test_funny_space_and_invalid(void)
{
    static const uint8_t funny_space[] = "\\Qx";
    struct fixture first;
    if (fixture_init(&first, funny_space, sizeof(funny_space) - 1U) != 0) {
        return 1;
    }
    (void)hstex_catcode_set(&first.lexical_state.catcodes, (uint32_t)'Q',
                            HSTEX_CAT_SPACE);
    static const uint8_t funny_name[] = {'Q'};
    int failed = expect_control(&first, HSTEX_SYMBOL_REGULAR, funny_name,
                                sizeof(funny_name)) ||
                 expect_character(&first, HSTEX_CAT_LETTER, (uint8_t)'x') ||
                 expect_character(&first, HSTEX_CAT_SPACE, (uint8_t)' ') ||
                 expect_eof(&first);
    fixture_destroy(&first);
    if (failed != 0) {
        return 1;
    }

    static const uint8_t escaped_invalid[] = {'\\', 127U};
    struct fixture second;
    if (fixture_init(&second, escaped_invalid, sizeof(escaped_invalid)) != 0) {
        return 1;
    }
    static const uint8_t invalid_name[] = {127U};
    failed = expect_control(&second, HSTEX_SYMBOL_REGULAR, invalid_name,
                            sizeof(invalid_name)) ||
             expect_character(&second, HSTEX_CAT_SPACE, (uint8_t)' ') ||
             expect_eof(&second);
    fixture_destroy(&second);
    if (failed != 0) {
        return 1;
    }

    static const uint8_t invalid[] = {127U};
    struct fixture third;
    if (fixture_init(&third, invalid, sizeof(invalid)) != 0) {
        return 1;
    }
    hstex_token token;
    struct hstex_source_location location;
    enum hstex_mouth_result result = hstex_mouth_next(
        &third.mouth, &token, &location, third.error, sizeof(third.error));
    /* A character of category 15 is not the mouth's to report: it reads
       past it and hands the fault up, because only the engine can say
       "Text line contains an invalid character" and carry on. */
    failed = result != HSTEX_MOUTH_INVALID;
    fixture_destroy(&third);
    return failed;
}

int main(void)
{
    return test_state_machine() || test_controls_and_active() ||
           test_mutable_catcodes() || test_caret_and_line_endings() ||
           test_suppressed_and_embedded_end_lines() ||
           test_funny_space_and_invalid();
}
