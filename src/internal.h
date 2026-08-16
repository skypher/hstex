#ifndef HSTEX_INTERNAL_H
#define HSTEX_INTERNAL_H

#if defined(__GNUC__) || defined(__clang__)
#define HSTEX_PRINTF_FORMAT(format_index, first_argument_index)              \
    __attribute__((__format__(__printf__, format_index, first_argument_index)))
#else
#define HSTEX_PRINTF_FORMAT(format_index, first_argument_index)
#endif

#endif
