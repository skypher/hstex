#include "hstex/scan.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define HSTEX_HAS_X86_TARGETS 1
#else
#define HSTEX_HAS_X86_TARGETS 0
#endif

typedef size_t (*scan_function)(const uint8_t *, size_t);

static scan_function active_scan = hstex_scan_default_boundary_scalar;
static const char *active_backend = "scalar";
static atomic_int initialization_state;

static bool is_default_boundary(uint8_t byte)
{
    switch (byte) {
    case 0U:
    case (uint8_t)'\t':
    case (uint8_t)'\n':
    case (uint8_t)'\r':
    case (uint8_t)' ':
    case (uint8_t)'#':
    case (uint8_t)'%':
    case (uint8_t)'&':
    case (uint8_t)'\\':
    case (uint8_t)'^':
    case (uint8_t)'_':
    case (uint8_t)'{':
    case (uint8_t)'}':
    case (uint8_t)'~':
        return true;
    default:
        return false;
    }
}

size_t hstex_scan_default_boundary_scalar(const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        if (is_default_boundary(data[index])) {
            return index;
        }
    }
    return length;
}

#if HSTEX_HAS_X86_TARGETS
__attribute__((target("avx2")))
static size_t scan_default_boundary_avx2(const uint8_t *data, size_t length)
{
    size_t index = 0U;
    for (; length - index >= 32U; index += 32U) {
        __m256i bytes = _mm256_loadu_si256((const __m256i *)(data + index));
        __m256i hits = _mm256_cmpeq_epi8(bytes, _mm256_setzero_si256());
#define HSTEX_MATCH(character)                                                \
        hits = _mm256_or_si256(                                               \
            hits, _mm256_cmpeq_epi8(                                          \
                      bytes, _mm256_set1_epi8((char)(character))))
        HSTEX_MATCH('\t');
        HSTEX_MATCH('\n');
        HSTEX_MATCH('\r');
        HSTEX_MATCH(' ');
        HSTEX_MATCH('#');
        HSTEX_MATCH('%');
        HSTEX_MATCH('&');
        HSTEX_MATCH('\\');
        HSTEX_MATCH('^');
        HSTEX_MATCH('_');
        HSTEX_MATCH('{');
        HSTEX_MATCH('}');
        HSTEX_MATCH('~');
#undef HSTEX_MATCH
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(hits);
        if (mask != 0U) {
            return index + (size_t)__builtin_ctz(mask);
        }
    }
    return index + hstex_scan_default_boundary_scalar(data + index,
                                                        length - index);
}
#endif

void hstex_scan_init(void)
{
    int state = atomic_load_explicit(&initialization_state, memory_order_acquire);
    if (state == 2) {
        return;
    }

    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &initialization_state, &expected, 1, memory_order_acq_rel,
            memory_order_acquire)) {
#if HSTEX_HAS_X86_TARGETS
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            active_scan = scan_default_boundary_avx2;
            active_backend = "avx2";
        }
#endif
        atomic_store_explicit(&initialization_state, 2, memory_order_release);
        return;
    }

    while (atomic_load_explicit(&initialization_state, memory_order_acquire) != 2) {
    }
}

size_t hstex_scan_default_boundary(const uint8_t *data, size_t length)
{
    hstex_scan_init();
    return active_scan(data, length);
}

const char *hstex_scan_backend(void)
{
    hstex_scan_init();
    return active_backend;
}
