#ifndef HSTEX_LEX_H
#define HSTEX_LEX_H

#include "hstex/catcode.h"
#include "hstex/symbol.h"

#include <stddef.h>
#include <stdint.h>

struct hstex_lexical_state {
    struct hstex_catcode_table catcodes;
    struct hstex_symbol_table symbols;
    int32_t end_line_character;
    hstex_cs_id paragraph_control_sequence;
};

int hstex_lexical_state_init(struct hstex_lexical_state *state, char *error,
                             size_t error_capacity);
void hstex_lexical_state_destroy(struct hstex_lexical_state *state);

#endif
