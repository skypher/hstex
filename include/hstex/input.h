#ifndef HSTEX_INPUT_H
#define HSTEX_INPUT_H

#include <stddef.h>
#include <stdint.h>

enum hstex_input_storage {
    HSTEX_INPUT_STORAGE_NONE = 0,
    HSTEX_INPUT_STORAGE_OWNED,
    HSTEX_INPUT_STORAGE_MMAP,
};

struct hstex_input {
    const uint8_t *data;
    size_t length;
    enum hstex_input_storage storage;
};

int hstex_input_open(const char *path, struct hstex_input *input,
                     char *error, size_t error_capacity);
/* The same, mapped private and writable: what the run writes into it is
   its own copy of the page and never reaches the file. For a format or a
   checkpoint read where it lies, some of whose tables the run then sets. */
int hstex_input_open_private(const char *path, struct hstex_input *input,
                             char *error, size_t error_capacity);
/* The same, asking for the mapping to begin at `hint' where that address is
   free; the mapping is wherever `input->data' says either way. */
int hstex_input_open_private_at(const char *path, struct hstex_input *input,
                                void *hint, char *error,
                                size_t error_capacity);
void hstex_input_close(struct hstex_input *input);
const char *hstex_input_storage_name(enum hstex_input_storage storage);

#endif
