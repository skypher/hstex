#include "hstex/lex.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int hstex_lexical_state_init(struct hstex_lexical_state *state, char *error,
                             size_t error_capacity)
{
    if (state == NULL) {
        if (error != NULL && error_capacity != 0U) {
            (void)snprintf(error, error_capacity,
                           "hstex_lexical_state_init: null state");
        }
        return -1;
    }
    memset(state, 0, sizeof(*state));
    hstex_catcodes_init_ini(&state->catcodes);
    state->end_line_character = 13;
    if (hstex_symbols_init(&state->symbols, error, error_capacity) != 0) {
        return -1;
    }
    static const uint8_t paragraph_name[] = {'p', 'a', 'r'};
    if (hstex_symbol_intern(&state->symbols, HSTEX_SYMBOL_REGULAR,
                            paragraph_name, sizeof(paragraph_name),
                            &state->paragraph_control_sequence, error,
                            error_capacity) != 0) {
        hstex_symbols_destroy(&state->symbols);
        return -1;
    }
    return 0;
}

void hstex_lexical_state_destroy(struct hstex_lexical_state *state)
{
    if (state == NULL) {
        return;
    }
    hstex_symbols_destroy(&state->symbols);
    memset(state, 0, sizeof(*state));
}
