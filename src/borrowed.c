#include "hstex/borrowed.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { HSTEX_BORROWED_RANGES = 16 };

static struct {
    uintptr_t base;
    size_t length;
} ranges[HSTEX_BORROWED_RANGES];
static size_t range_count;

void hstex_borrowed_register(const void *base, size_t length)
{
    if (base == NULL || length == 0U || range_count == HSTEX_BORROWED_RANGES) {
        return;
    }
    ranges[range_count].base = (uintptr_t)base;
    ranges[range_count].length = length;
    ++range_count;
}

void hstex_borrowed_forget(const void *base)
{
    for (size_t index = 0U; index < range_count; ++index) {
        if (ranges[index].base == (uintptr_t)base) {
            ranges[index] = ranges[range_count - 1U];
            --range_count;
            return;
        }
    }
}

bool hstex_borrowed_holds(const void *pointer)
{
    uintptr_t at = (uintptr_t)pointer;
    for (size_t index = 0U; index < range_count; ++index) {
        if (at >= ranges[index].base &&
            at - ranges[index].base < ranges[index].length) {
            return true;
        }
    }
    return false;
}

void hstex_release(void *pointer)
{
    if (pointer != NULL && !hstex_borrowed_holds(pointer)) {
        free(pointer);
    }
}

void *hstex_grow(void *pointer, size_t old_bytes, size_t new_bytes)
{
    if (pointer == NULL || !hstex_borrowed_holds(pointer)) {
        return realloc(pointer, new_bytes);
    }
    void *room = malloc(new_bytes);
    if (room != NULL) {
        memcpy(room, pointer, old_bytes < new_bytes ? old_bytes : new_bytes);
    }
    return room;
}
