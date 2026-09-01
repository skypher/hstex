#include "hstex/symbol.h"
#include "test_cli.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect_name(const struct hstex_symbol_table *table,
                       hstex_cs_id identifier, enum hstex_symbol_kind kind,
                       const uint8_t *name, size_t length)
{
    enum hstex_symbol_kind actual_kind;
    const uint8_t *actual_name = NULL;
    size_t actual_length = 0U;
    if (hstex_symbol_name(table, identifier, &actual_kind, &actual_name,
                          &actual_length) != 0 ||
        actual_kind != kind || actual_length != length ||
        (length != 0U && memcmp(actual_name, name, length) != 0)) {
        (void)fprintf(stderr, "symbol %u has the wrong name\n",
                      (unsigned int)identifier);
        return 1;
    }
    return 0;
}

int main(int argument_count, char **arguments)
{
    int option = hstex_test_arguments(
        argument_count, arguments, "Run the HSTeX symbol-table tests.");
    if (option >= 0) {
        return option;
    }
    struct hstex_symbol_table table;
    char error[256];
    if (hstex_symbols_init(&table, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        return 1;
    }

    hstex_cs_id empty = 0U;
    hstex_cs_id foo = 0U;
    hstex_cs_id foo_again = 0U;
    hstex_cs_id regular_tilde = 0U;
    hstex_cs_id active_tilde = 0U;
    static const uint8_t foo_name[] = {'f', 'o', 'o'};
    static const uint8_t tilde_name[] = {'~'};
    if (hstex_symbol_intern(&table, HSTEX_SYMBOL_REGULAR, NULL, 0U, &empty,
                            error, sizeof(error)) != 0 ||
        hstex_symbol_intern(&table, HSTEX_SYMBOL_REGULAR, foo_name,
                            sizeof(foo_name), &foo, error, sizeof(error)) != 0 ||
        hstex_symbol_intern(&table, HSTEX_SYMBOL_REGULAR, foo_name,
                            sizeof(foo_name), &foo_again, error,
                            sizeof(error)) != 0 ||
        hstex_symbol_intern(&table, HSTEX_SYMBOL_REGULAR, tilde_name,
                            sizeof(tilde_name), &regular_tilde, error,
                            sizeof(error)) != 0 ||
        hstex_symbol_intern(&table, HSTEX_SYMBOL_ACTIVE, tilde_name,
                            sizeof(tilde_name), &active_tilde, error,
                            sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        hstex_symbols_destroy(&table);
        return 1;
    }
    if (empty == 0U || foo != foo_again || regular_tilde == active_tilde ||
        expect_name(&table, empty, HSTEX_SYMBOL_REGULAR, NULL, 0U) != 0 ||
        expect_name(&table, foo, HSTEX_SYMBOL_REGULAR, foo_name,
                    sizeof(foo_name)) != 0 ||
        expect_name(&table, active_tilde, HSTEX_SYMBOL_ACTIVE, tilde_name,
                    sizeof(tilde_name)) != 0) {
        hstex_symbols_destroy(&table);
        return 1;
    }

    enum { SYMBOL_COUNT = 10000 };
    for (size_t index = 0U; index < (size_t)SYMBOL_COUNT; ++index) {
        char name[48];
        int written = snprintf(name, sizeof(name), "generated-symbol-%zu", index);
        if (written < 0 || (size_t)written >= sizeof(name)) {
            (void)fprintf(stderr, "generated symbol name overflow\n");
            hstex_symbols_destroy(&table);
            return 1;
        }
        hstex_cs_id identifier = 0U;
        if (hstex_symbol_intern(&table, HSTEX_SYMBOL_REGULAR,
                                (const uint8_t *)name, (size_t)written,
                                &identifier, error, sizeof(error)) != 0 ||
            identifier == 0U) {
            (void)fprintf(stderr, "%s\n", error);
            hstex_symbols_destroy(&table);
            return 1;
        }
    }
    for (size_t index = 0U; index < (size_t)SYMBOL_COUNT; ++index) {
        char name[48];
        int written = snprintf(name, sizeof(name), "generated-symbol-%zu", index);
        if (written < 0 || (size_t)written >= sizeof(name)) {
            hstex_symbols_destroy(&table);
            return 1;
        }
        hstex_cs_id identifier = 0U;
        if (hstex_symbol_find(&table, HSTEX_SYMBOL_REGULAR,
                              (const uint8_t *)name, (size_t)written,
                              &identifier) != 1 ||
            expect_name(&table, identifier, HSTEX_SYMBOL_REGULAR,
                        (const uint8_t *)name, (size_t)written) != 0) {
            (void)fprintf(stderr, "generated symbol lookup failed at %zu\n", index);
            hstex_symbols_destroy(&table);
            return 1;
        }
    }

    hstex_symbols_destroy(&table);
    return 0;
}
