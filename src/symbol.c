#include "hstex/symbol.h"

#include "internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A table starts where the primitives leave off. Registering them alone
   interns 431 names of 3,971 bytes, so a table that began at 32 entries
   climbed to 512 by doubling -- four copies of the entries, four of the
   names and four rehashes of the slots -- before the first line of a
   document was read, and every engine made paid for the climb again. The
   figures below are those, measured after hstex_engine_init and rounded up
   to the powers of two the doubling would have reached. Overshooting costs
   20 KB of memory per table and nothing else. */
enum {
    HSTEX_INITIAL_SLOT_CAPACITY = 1024,
    HSTEX_INITIAL_ENTRY_CAPACITY = 512,
    HSTEX_INITIAL_BYTE_CAPACITY = 4096,
};

static int set_error(char *error, size_t capacity, const char *format, ...)
    HSTEX_PRINTF_FORMAT(3, 4);

static int set_error(char *error, size_t capacity, const char *format, ...)
{
    if (error != NULL && capacity != 0U) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(error, capacity, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static uint64_t symbol_hash(enum hstex_symbol_kind kind, const uint8_t *name,
                            size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    hash ^= (uint64_t)kind;
    hash *= UINT64_C(1099511628211);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)name[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= (uint64_t)length;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static int reserve_entries(struct hstex_symbol_table *table, size_t required,
                           char *error, size_t error_capacity)
{
    if (required <= table->entry_capacity) {
        return 0;
    }
    size_t capacity = table->entry_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_ENTRY_CAPACITY
                          : table->entry_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "control-sequence entry capacity overflow");
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*table->entries)) {
        return set_error(error, error_capacity,
                         "control-sequence entry allocation overflow");
    }
    void *allocation = realloc(table->entries, capacity * sizeof(*table->entries));
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "control-sequence entry allocation failed");
    }
    table->entries = allocation;
    table->entry_capacity = capacity;
    return 0;
}

static int reserve_bytes(struct hstex_symbol_table *table, size_t required,
                         char *error, size_t error_capacity)
{
    if (required <= table->byte_capacity) {
        return 0;
    }
    size_t capacity = table->byte_capacity == 0U
                          ? (size_t)HSTEX_INITIAL_BYTE_CAPACITY
                          : table->byte_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return set_error(error, error_capacity,
                             "control-sequence name capacity overflow");
        }
        capacity *= 2U;
    }
    void *allocation = realloc(table->bytes, capacity);
    if (allocation == NULL) {
        return set_error(error, error_capacity,
                         "control-sequence name allocation failed");
    }
    table->bytes = allocation;
    table->byte_capacity = capacity;
    return 0;
}

static size_t find_slot(const struct hstex_symbol_table *table, uint64_t hash,
                        enum hstex_symbol_kind kind, const uint8_t *name,
                        size_t length, hstex_cs_id *identifier)
{
    size_t mask = table->slot_capacity - 1U;
    size_t slot = (size_t)hash & mask;
    for (;;) {
        hstex_cs_id candidate = table->slots[slot];
        if (candidate == 0U) {
            if (identifier != NULL) {
                *identifier = 0U;
            }
            return slot;
        }
        const struct hstex_symbol_entry *entry = &table->entries[candidate - 1U];
        bool same_name = entry->hash == hash && entry->kind == (uint8_t)kind &&
                         entry->byte_length == length;
        if (same_name &&
            (length == 0U ||
             memcmp(table->bytes + entry->byte_offset, name, length) == 0)) {
            if (identifier != NULL) {
                *identifier = candidate;
            }
            return slot;
        }
        slot = (slot + 1U) & mask;
    }
}

static int rehash(struct hstex_symbol_table *table, size_t capacity, char *error,
                  size_t error_capacity)
{
    if (capacity < (size_t)HSTEX_INITIAL_SLOT_CAPACITY ||
        (capacity & (capacity - 1U)) != 0U ||
        capacity > SIZE_MAX / sizeof(*table->slots)) {
        return set_error(error, error_capacity,
                         "invalid control-sequence hash capacity");
    }
    uint32_t *slots = calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return set_error(error, error_capacity,
                         "control-sequence hash allocation failed");
    }

    uint32_t *old_slots = table->slots;
    table->slots = slots;
    table->slot_capacity = capacity;
    for (size_t index = 0U; index < table->entry_count; ++index) {
        const struct hstex_symbol_entry *entry = &table->entries[index];
        const uint8_t *name = entry->byte_length == 0U
                                  ? NULL
                                  : table->bytes + entry->byte_offset;
        size_t slot = find_slot(table, entry->hash,
                                (enum hstex_symbol_kind)entry->kind, name,
                                entry->byte_length, NULL);
        table->slots[slot] = (uint32_t)(index + 1U);
    }
    free(old_slots);
    return 0;
}

int hstex_symbols_init(struct hstex_symbol_table *table, char *error,
                       size_t error_capacity)
{
    if (table == NULL) {
        return set_error(error, error_capacity,
                         "hstex_symbols_init: null table");
    }
    memset(table, 0, sizeof(*table));
    if (reserve_entries(table, (size_t)HSTEX_INITIAL_ENTRY_CAPACITY, error,
                        error_capacity) != 0 ||
        reserve_bytes(table, (size_t)HSTEX_INITIAL_BYTE_CAPACITY, error,
                      error_capacity) != 0 ||
        rehash(table, (size_t)HSTEX_INITIAL_SLOT_CAPACITY, error,
               error_capacity) != 0) {
        hstex_symbols_destroy(table);
        return -1;
    }
    return 0;
}

void hstex_symbols_destroy(struct hstex_symbol_table *table)
{
    if (table == NULL) {
        return;
    }
    free(table->entries);
    free(table->slots);
    free(table->bytes);
    memset(table, 0, sizeof(*table));
}

int hstex_symbol_find(const struct hstex_symbol_table *table,
                      enum hstex_symbol_kind kind, const uint8_t *name,
                      size_t length, hstex_cs_id *identifier)
{
    if (table == NULL || identifier == NULL || table->slot_capacity == 0U ||
        (length != 0U && name == NULL) ||
        (kind == HSTEX_SYMBOL_ACTIVE && length != 1U)) {
        return 0;
    }
    uint64_t hash = symbol_hash(kind, name, length);
    hstex_cs_id found = 0U;
    (void)find_slot(table, hash, kind, name, length, &found);
    *identifier = found;
    return found == 0U ? 0 : 1;
}

int hstex_symbol_intern(struct hstex_symbol_table *table,
                        enum hstex_symbol_kind kind, const uint8_t *name,
                        size_t length, hstex_cs_id *identifier, char *error,
                        size_t error_capacity)
{
    if (table == NULL || identifier == NULL || table->slot_capacity == 0U ||
        (length != 0U && name == NULL) ||
        (kind != HSTEX_SYMBOL_REGULAR && kind != HSTEX_SYMBOL_ACTIVE) ||
        (kind == HSTEX_SYMBOL_ACTIVE && length != 1U)) {
        return set_error(error, error_capacity,
                         "invalid control-sequence interning request");
    }
    if (length > UINT32_MAX || table->byte_count > UINT32_MAX - length) {
        return set_error(error, error_capacity,
                         "control-sequence name arena exceeds 32-bit offsets");
    }

    uint64_t hash = symbol_hash(kind, name, length);
    hstex_cs_id found = 0U;
    size_t slot = find_slot(table, hash, kind, name, length, &found);
    if (found != 0U) {
        *identifier = found;
        return 0;
    }
    if (table->entry_count >= (size_t)HSTEX_CS_ID_MAX) {
        return set_error(error, error_capacity,
                         "control-sequence identifier space exhausted");
    }

    if (table->entry_count + 1U > (table->slot_capacity / 10U) * 7U) {
        if (table->slot_capacity > SIZE_MAX / 2U ||
            rehash(table, table->slot_capacity * 2U, error, error_capacity) != 0) {
            return set_error(error, error_capacity,
                             "control-sequence hash growth failed");
        }
        slot = find_slot(table, hash, kind, name, length, NULL);
    }
    if (reserve_entries(table, table->entry_count + 1U, error,
                        error_capacity) != 0 ||
        reserve_bytes(table, table->byte_count + length, error,
                      error_capacity) != 0) {
        return -1;
    }

    struct hstex_symbol_entry *entry = &table->entries[table->entry_count];
    entry->hash = hash;
    entry->byte_offset = (uint32_t)table->byte_count;
    entry->byte_length = (uint32_t)length;
    entry->kind = (uint8_t)kind;
    if (length != 0U) {
        memcpy(table->bytes + table->byte_count, name, length);
    }
    table->byte_count += length;
    ++table->entry_count;
    hstex_cs_id new_identifier = (hstex_cs_id)table->entry_count;
    table->slots[slot] = new_identifier;
    *identifier = new_identifier;
    return 0;
}

int hstex_symbol_name(const struct hstex_symbol_table *table,
                      hstex_cs_id identifier, enum hstex_symbol_kind *kind,
                      const uint8_t **name, size_t *length)
{
    if (table == NULL || identifier == 0U ||
        (size_t)identifier > table->entry_count || kind == NULL || name == NULL ||
        length == NULL) {
        return -1;
    }
    const struct hstex_symbol_entry *entry = &table->entries[identifier - 1U];
    *kind = (enum hstex_symbol_kind)entry->kind;
    *name = entry->byte_length == 0U ? NULL : table->bytes + entry->byte_offset;
    *length = entry->byte_length;
    return 0;
}
