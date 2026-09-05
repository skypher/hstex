#ifndef HSTEX_BORROWED_H
#define HSTEX_BORROWED_H

#include <stdbool.h>
#include <stddef.h>

/* RANGES A RUN READS ITS STATE OUT OF WHERE IT LIES. A format mapped, a
   checkpoint kept: what points into one of these was never handed out by
   the allocator, so it is never given back to it and never grown in place.
   A run holds a handful of such ranges at most. */
void hstex_borrowed_register(const void *base, size_t length);
void hstex_borrowed_forget(const void *base);
bool hstex_borrowed_holds(const void *pointer);

/* free(), unless the pointer lies in a borrowed range. */
void hstex_release(void *pointer);

/* realloc(), unless the pointer lies in a borrowed range, in which case new
   room is taken and the old bytes copied out; `old_bytes' says how many. */
void *hstex_grow(void *pointer, size_t old_bytes, size_t new_bytes);

#endif
