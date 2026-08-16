#ifndef HSTEX_SYMBOL_H
#define HSTEX_SYMBOL_H

#include "hstex/token.h"

#include <stddef.h>
#include <stdint.h>

enum hstex_symbol_kind {
    HSTEX_SYMBOL_REGULAR = 0,
    HSTEX_SYMBOL_ACTIVE = 1,
};

struct hstex_symbol_entry {
    uint64_t hash;
    uint32_t byte_offset;
    uint32_t byte_length;
    uint8_t kind;
};

struct hstex_symbol_table {
    struct hstex_symbol_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    uint32_t *slots;
    size_t slot_capacity;
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
};

int hstex_symbols_init(struct hstex_symbol_table *table, char *error,
                       size_t error_capacity);
void hstex_symbols_destroy(struct hstex_symbol_table *table);
int hstex_symbol_intern(struct hstex_symbol_table *table,
                        enum hstex_symbol_kind kind, const uint8_t *name,
                        size_t length, hstex_cs_id *identifier, char *error,
                        size_t error_capacity);
int hstex_symbol_find(const struct hstex_symbol_table *table,
                      enum hstex_symbol_kind kind, const uint8_t *name,
                      size_t length, hstex_cs_id *identifier);
int hstex_symbol_name(const struct hstex_symbol_table *table,
                      hstex_cs_id identifier, enum hstex_symbol_kind *kind,
                      const uint8_t **name, size_t *length);

#endif
