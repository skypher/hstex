#include "hstex/scan.h"
#include "test_cli.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int check_equal(const uint8_t *data, size_t length, const char *label)
{
    size_t scalar = hstex_scan_default_boundary_scalar(data, length);
    size_t dispatched = hstex_scan_default_boundary(data, length);
    if (scalar != dispatched) {
        (void)fprintf(stderr,
                      "%s: scalar=%zu dispatched=%zu length=%zu backend=%s\n",
                      label, scalar, dispatched, length, hstex_scan_backend());
        return 1;
    }
    return 0;
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
    *state = value;
    return value;
}

static int check_declared_boundaries(void)
{
    static const uint8_t boundaries[] = {
        0U,  (uint8_t)'\t', (uint8_t)'\n', (uint8_t)'\r', (uint8_t)' ',
        (uint8_t)'#', (uint8_t)'%',  (uint8_t)'&',  (uint8_t)'\\',
        (uint8_t)'^', (uint8_t)'_',  (uint8_t)'{',  (uint8_t)'}',
        (uint8_t)'~',
    };
    for (size_t index = 0U; index < sizeof(boundaries); ++index) {
        if (hstex_scan_default_boundary_scalar(&boundaries[index], 1U) != 0U) {
            (void)fprintf(stderr, "declared boundary %u was not recognized\n",
                          (unsigned int)boundaries[index]);
            return 1;
        }
    }

    static const uint8_t ordinary[] = {'a', 'Z', '0', '.', ',', '[', ']', '$'};
    for (size_t index = 0U; index < sizeof(ordinary); ++index) {
        if (hstex_scan_default_boundary_scalar(&ordinary[index], 1U) != 1U) {
            (void)fprintf(stderr, "ordinary byte %u was classified as boundary\n",
                          (unsigned int)ordinary[index]);
            return 1;
        }
    }
    return 0;
}

int main(int argument_count, char **arguments)
{
    int option = hstex_test_arguments(
        argument_count, arguments, "Run the HSTeX scanner tests.");
    if (option >= 0) {
        return option;
    }
    uint8_t storage[1024 + 64];
    for (size_t index = 0U; index < sizeof(storage); ++index) {
        storage[index] = (uint8_t)'a';
    }

    if (check_declared_boundaries() != 0) {
        return 1;
    }

    for (size_t alignment = 0U; alignment < 32U; ++alignment) {
        for (size_t length = 0U; length <= 768U; ++length) {
            if (check_equal(storage + alignment, length, "ordinary") != 0) {
                return 1;
            }
            if (length != 0U) {
                size_t position = (length * 37U) % length;
                storage[alignment + position] = (uint8_t)'\\';
                if (check_equal(storage + alignment, length, "boundary") != 0) {
                    return 1;
                }
                storage[alignment + position] = (uint8_t)'a';
            }
        }
    }

    uint64_t state = UINT64_C(0x5e8d3f419ab267c1);
    for (size_t trial = 0U; trial < 10000U; ++trial) {
        size_t length = (size_t)(xorshift64(&state) % UINT64_C(1024));
        size_t alignment = (size_t)(xorshift64(&state) % UINT64_C(32));
        for (size_t index = 0U; index < length; ++index) {
            storage[alignment + index] = (uint8_t)xorshift64(&state);
        }
        if (check_equal(storage + alignment, length, "random") != 0) {
            return 1;
        }
    }

    (void)printf("scan backend: %s\n", hstex_scan_backend());
    return 0;
}
