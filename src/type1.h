#ifndef HSTEX_TYPE1_H
#define HSTEX_TYPE1_H

#include <stddef.h>
#include <stdint.h>

int hstex_type1_disassemble(const uint8_t *font, size_t font_length,
                            uint8_t **disassembly,
                            size_t *disassembly_length, char *error,
                            size_t error_capacity);

int hstex_type1_assemble(const uint8_t *disassembly,
                         size_t disassembly_length, uint8_t **program,
                         size_t *program_length, size_t *length1,
                         size_t *length2, char *error,
                         size_t error_capacity);

#endif
