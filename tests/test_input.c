#include "hstex/input.h"
#include "test_cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int file_descriptor, const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = write(file_descriptor, data + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int check_small_file(const char *path)
{
    static const uint8_t contents[] = "alpha\\beta\n";
    int file_descriptor = open(path, O_WRONLY | O_TRUNC);
    if (file_descriptor < 0) {
        (void)fprintf(stderr, "could not open small fixture\n");
        return 1;
    }
    int write_result = write_all(file_descriptor, contents, sizeof(contents) - 1U);
    int close_result = close(file_descriptor);
    if (write_result != 0 || close_result != 0) {
        (void)fprintf(stderr, "could not create small fixture\n");
        return 1;
    }

    struct hstex_input input;
    char error[256];
    if (hstex_input_open(path, &input, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "small open: %s\n", error);
        return 1;
    }
    int failed = input.storage != HSTEX_INPUT_STORAGE_OWNED ||
                 input.length != sizeof(contents) - 1U ||
                 memcmp(input.data, contents, sizeof(contents) - 1U) != 0;
    hstex_input_close(&input);
    if (failed != 0) {
        (void)fprintf(stderr, "small fixture mismatch\n");
        return 1;
    }
    return 0;
}

static int check_large_file(const char *path)
{
    uint8_t block[4096];
    for (size_t index = 0U; index < sizeof(block); ++index) {
        block[index] = (uint8_t)(index * 17U + 3U);
    }

    int file_descriptor = open(path, O_WRONLY | O_TRUNC);
    if (file_descriptor < 0) {
        (void)fprintf(stderr, "could not open large fixture\n");
        return 1;
    }
    for (size_t block_index = 0U; block_index < 18U; ++block_index) {
        if (write_all(file_descriptor, block, sizeof(block)) != 0) {
            (void)close(file_descriptor);
            (void)fprintf(stderr, "could not write large fixture\n");
            return 1;
        }
    }
    if (close(file_descriptor) != 0) {
        (void)fprintf(stderr, "could not close large fixture\n");
        return 1;
    }

    struct hstex_input input;
    char error[256];
    if (hstex_input_open(path, &input, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "large open: %s\n", error);
        return 1;
    }
    size_t expected_length = sizeof(block) * 18U;
    int failed = input.storage != HSTEX_INPUT_STORAGE_MMAP ||
                 input.length != expected_length || input.data[0] != block[0] ||
                 input.data[expected_length - 1U] != block[sizeof(block) - 1U];
    hstex_input_close(&input);
    if (failed != 0) {
        (void)fprintf(stderr, "large fixture mismatch\n");
        return 1;
    }
    return 0;
}

int main(int argument_count, char **arguments)
{
    int option = hstex_test_arguments(
        argument_count, arguments, "Run the HSTeX input-buffer tests.");
    if (option >= 0) {
        return option;
    }
    char path[] = "/tmp/hstex-input-test-XXXXXX";
    int file_descriptor = mkstemp(path);
    if (file_descriptor < 0 || close(file_descriptor) != 0) {
        (void)fprintf(stderr, "could not create temporary fixture\n");
        return 1;
    }

    int result = check_small_file(path);
    if (result == 0) {
        result = check_large_file(path);
    }
    if (unlink(path) != 0) {
        (void)fprintf(stderr, "could not remove temporary fixture\n");
        return 1;
    }
    return result;
}
