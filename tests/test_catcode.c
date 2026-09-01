#include "hstex/catcode.h"
#include "hstex/token.h"
#include "test_cli.h"

#include <stdint.h>
#include <stdio.h>

static int expect_category(const struct hstex_catcode_table *table,
                           uint8_t character, enum hstex_catcode expected)
{
    uint8_t actual = hstex_catcode_get(table, character);
    if (actual != (uint8_t)expected) {
        (void)fprintf(stderr, "catcode[%u]=%u, expected %u\n",
                      (unsigned int)character, (unsigned int)actual,
                      (unsigned int)expected);
        return 1;
    }
    return 0;
}

int main(int argument_count, char **arguments)
{
    int option = hstex_test_arguments(
        argument_count, arguments, "Run the HSTeX catcode tests.");
    if (option >= 0) {
        return option;
    }
    struct hstex_catcode_table table;
    hstex_catcodes_init_ini(&table);
    if (expect_category(&table, 0U, HSTEX_CAT_IGNORED) != 0 ||
        expect_category(&table, 13U, HSTEX_CAT_END_OF_LINE) != 0 ||
        expect_category(&table, (uint8_t)' ', HSTEX_CAT_SPACE) != 0 ||
        expect_category(&table, (uint8_t)'%', HSTEX_CAT_COMMENT) != 0 ||
        expect_category(&table, (uint8_t)'\\', HSTEX_CAT_ESCAPE) != 0 ||
        expect_category(&table, 127U, HSTEX_CAT_INVALID) != 0 ||
        expect_category(&table, (uint8_t)'A', HSTEX_CAT_LETTER) != 0 ||
        expect_category(&table, (uint8_t)'z', HSTEX_CAT_LETTER) != 0 ||
        expect_category(&table, (uint8_t)'{', HSTEX_CAT_OTHER) != 0 ||
        expect_category(&table, (uint8_t)'9', HSTEX_CAT_OTHER) != 0) {
        return 1;
    }

    uint64_t generation = table.generation;
    if (hstex_catcode_set(&table, (uint32_t)'@', HSTEX_CAT_LETTER) != 0 ||
        table.generation != generation + 1U ||
        hstex_catcode_set(&table, (uint32_t)'@', HSTEX_CAT_LETTER) != 0 ||
        table.generation != generation + 1U ||
        hstex_catcode_set(&table, UINT32_C(256), HSTEX_CAT_LETTER) == 0 ||
        hstex_catcode_set(&table, (uint32_t)'@', UINT32_C(16)) == 0) {
        (void)fprintf(stderr, "catcode mutation contract failed\n");
        return 1;
    }

    hstex_token character = hstex_token_character((uint8_t)HSTEX_CAT_LETTER,
                                                   (uint8_t)'q');
    hstex_token control = hstex_token_control_sequence(UINT32_C(1234));
    hstex_token unexpanded =
        hstex_token_unexpanded_control_sequence(UINT32_C(5678));
    hstex_token parameter = hstex_token_parameter(7U);
    if (!hstex_token_is_character(character) ||
        hstex_token_category(character) != (uint8_t)HSTEX_CAT_LETTER ||
        hstex_token_character_code(character) != (uint8_t)'q' ||
        !hstex_token_is_control_sequence(control) ||
        hstex_token_control_sequence_id(control) != UINT32_C(1234) ||
        !hstex_token_is_frozen_control_sequence(unexpanded) ||
        !hstex_token_is_unexpanded_control_sequence(unexpanded) ||
        hstex_token_control_sequence_id(unexpanded) != UINT32_C(5678) ||
        hstex_token_kind_of(parameter) != HSTEX_TOKEN_PARAMETER ||
        hstex_token_parameter_number(parameter) != 7U) {
        (void)fprintf(stderr, "packed token contract failed\n");
        return 1;
    }
    return 0;
}
