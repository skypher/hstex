#include "hstex/input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { HSTEX_MMAP_THRESHOLD = 64 * 1024 };

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

static void reset_input(struct hstex_input *input)
{
    input->data = NULL;
    input->length = 0U;
    input->storage = HSTEX_INPUT_STORAGE_NONE;
}

static int read_small_file(int file_descriptor, const char *path, size_t length,
                           struct hstex_input *input, char *error,
                           size_t error_capacity)
{
    uint8_t *bytes = malloc(length + 1U);
    if (bytes == NULL) {
        return set_error(error, error_capacity, "%s: allocation failed", path);
    }

    size_t offset = 0U;
    while (offset < length) {
        size_t remaining = length - offset;
        size_t request = remaining > (size_t)(1U << 30) ? (size_t)(1U << 30)
                                                         : remaining;
        ssize_t count = read(file_descriptor, bytes + offset, request);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved_errno = errno;
            free(bytes);
            return set_error(error, error_capacity, "%s: read failed: %s", path,
                             strerror(saved_errno));
        }
        if (count == 0) {
            free(bytes);
            return set_error(error, error_capacity,
                             "%s: file became shorter while reading", path);
        }
        offset += (size_t)count;
    }

    bytes[length] = 0U;
    input->data = bytes;
    input->length = length;
    input->storage = HSTEX_INPUT_STORAGE_OWNED;
    return 0;
}

int hstex_input_open(const char *path, struct hstex_input *input,
                     char *error, size_t error_capacity)
{
    if (path == NULL || input == NULL) {
        return set_error(error, error_capacity,
                         "hstex_input_open: null path or destination");
    }
    reset_input(input);

    int file_descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (file_descriptor < 0) {
        return set_error(error, error_capacity, "%s: open failed: %s", path,
                         strerror(errno));
    }

    struct stat status;
    if (fstat(file_descriptor, &status) != 0) {
        int saved_errno = errno;
        (void)close(file_descriptor);
        return set_error(error, error_capacity, "%s: stat failed: %s", path,
                         strerror(saved_errno));
    }
    if (!S_ISREG(status.st_mode)) {
        (void)close(file_descriptor);
        return set_error(error, error_capacity,
                         "%s: only regular files are supported by this loader",
                         path);
    }
    if (status.st_size < 0 || (uintmax_t)status.st_size > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)close(file_descriptor);
        return set_error(error, error_capacity, "%s: file is too large", path);
    }

    size_t length = (size_t)status.st_size;
    if (length >= (size_t)HSTEX_MMAP_THRESHOLD) {
        void *mapping = mmap(NULL, length, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
        int saved_errno = errno;
        (void)close(file_descriptor);
        if (mapping == MAP_FAILED) {
            return set_error(error, error_capacity, "%s: mmap failed: %s", path,
                             strerror(saved_errno));
        }
#ifdef MADV_SEQUENTIAL
        (void)madvise(mapping, length, MADV_SEQUENTIAL);
#endif
        input->data = mapping;
        input->length = length;
        input->storage = HSTEX_INPUT_STORAGE_MMAP;
        return 0;
    }

    int result = read_small_file(file_descriptor, path, length, input, error,
                                 error_capacity);
    (void)close(file_descriptor);
    return result;
}

void hstex_input_close(struct hstex_input *input)
{
    if (input == NULL) {
        return;
    }
    if (input->storage == HSTEX_INPUT_STORAGE_MMAP && input->data != NULL) {
        (void)munmap((void *)input->data, input->length);
    } else if (input->storage == HSTEX_INPUT_STORAGE_OWNED) {
        free((void *)input->data);
    }
    reset_input(input);
}

const char *hstex_input_storage_name(enum hstex_input_storage storage)
{
    switch (storage) {
    case HSTEX_INPUT_STORAGE_NONE:
        return "none";
    case HSTEX_INPUT_STORAGE_OWNED:
        return "owned";
    case HSTEX_INPUT_STORAGE_MMAP:
        return "mmap";
    }
    return "unknown";
}
