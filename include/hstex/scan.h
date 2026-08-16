#ifndef HSTEX_SCAN_H
#define HSTEX_SCAN_H

#include <stddef.h>
#include <stdint.h>

/*
 * Return the first byte that ends a default-catcode lexical fast-path run, or
 * length when the entire span is ordinary. This is not a complete tokenizer.
 */
size_t hstex_scan_default_boundary_scalar(const uint8_t *data, size_t length);
size_t hstex_scan_default_boundary(const uint8_t *data, size_t length);
void hstex_scan_init(void);
const char *hstex_scan_backend(void);

#endif
