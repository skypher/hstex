#include "hstex/input.h"
#include "hstex/scan.h"
#include "hstex_config.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void print_usage(FILE *stream, const char *program)
{
    (void)fprintf(stream,
                  "usage: %s --version\n"
                  "       %s --cpu-features\n"
                  "       %s --probe-input FILE\n",
                  program, program, program);
}

static uint64_t fnv1a64(const uint8_t *data, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int probe_input(const char *path)
{
    struct hstex_input input;
    char error[512];
    if (hstex_input_open(path, &input, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex: %s\n", error);
        return 1;
    }

    size_t offset = 0U;
    size_t boundary_count = 0U;
    while (offset < input.length) {
        size_t run = hstex_scan_default_boundary(input.data + offset,
                                                 input.length - offset);
        offset += run;
        if (offset < input.length) {
            ++boundary_count;
            ++offset;
        }
    }

    uint64_t hash = fnv1a64(input.data, input.length);
    (void)printf("path=%s\n", path);
    (void)printf("bytes=%zu\n", input.length);
    (void)printf("storage=%s\n", hstex_input_storage_name(input.storage));
    (void)printf("scan_backend=%s\n", hstex_scan_backend());
    (void)printf("default_boundaries=%zu\n", boundary_count);
    (void)printf("fnv1a64=%016" PRIx64 "\n", hash);

    hstex_input_close(&input);
    return 0;
}

int main(int argument_count, char **arguments)
{
    if (argument_count == 2 && strcmp(arguments[1], "--version") == 0) {
        (void)printf("hstex %s\n", HSTEX_VERSION);
        return 0;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--cpu-features") == 0) {
        (void)printf("scan_backend=%s\n", hstex_scan_backend());
        return 0;
    }
    if (argument_count == 3 && strcmp(arguments[1], "--probe-input") == 0) {
        return probe_input(arguments[2]);
    }

    print_usage(stderr, arguments[0]);
    return 2;
}
