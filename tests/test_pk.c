#include "pk.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "test_pk: %s\n", message);
    return 1;
}

static int test_packets(void)
{
    static const unsigned char input[] = {
        247, 89, 0,
        0, 16, 0, 0,
        0x12, 0x34, 0x56, 0x78,
        0, 8, 0, 0,
        0, 8, 0, 0,
        0xE0, 9, 65, 0, 0, 0, 2, 2, 2, 0, 1, 0x90,
        0xD8, 10, 66, 0, 0, 0, 4, 4, 3, 0, 2, 0xE2, 0x22,
        245,
    };
    char error[256] = {0};
    struct hstex_pk_font font;
    if (hstex_pk_parse(input, sizeof(input), &font, error, sizeof(error)) != 0) {
        fprintf(stderr, "test_pk: valid font rejected: %s\n", error);
        return 1;
    }
    const struct hstex_pk_glyph *raw = &font.glyphs[65];
    const struct hstex_pk_glyph *runs = &font.glyphs[66];
    int status = 0;
    if (!raw->present || raw->width != 2 || raw->height != 2 ||
        raw->bitmap_length != 2U || raw->bitmap[0] != 0x80U ||
        raw->bitmap[1] != 0x40U) {
        status = fail("raw bitmap packet decoded incorrectly");
    } else if (!runs->present || runs->width != 4 || runs->height != 3 ||
               runs->bitmap_length != 3U || runs->bitmap[0] != 0xC0U ||
               runs->bitmap[1] != 0xC0U || runs->bitmap[2] != 0xC0U) {
        status = fail("run-packed repeat rows decoded incorrectly");
    } else if (font.checksum != 0x12345678U ||
               font.horizontal_pixels_per_point != 0x00080000U ||
               font.vertical_pixels_per_point != 0x00080000U) {
        status = fail("PK preamble decoded incorrectly");
    }
    hstex_pk_destroy(&font);
    return status;
}

static int test_invalid_preamble(void)
{
    static const unsigned char input[] = {0};
    char error[128] = {0};
    struct hstex_pk_font font;
    if (hstex_pk_parse(input, sizeof(input), &font, error, sizeof(error)) == 0) {
        hstex_pk_destroy(&font);
        return fail("invalid preamble accepted");
    }
    return strstr(error, "preamble") == NULL
               ? fail("invalid preamble produced no specific diagnostic")
               : 0;
}

static void usage(void)
{
    puts("Usage: test_pk [-h|--help]\n"
         "Run the HSTeX PK bitmap-font parser tests.");
}

int main(int argc, char **argv)
{
    if (argc == 2 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage();
        return 0;
    }
    if (argc != 1) {
        usage();
        return 2;
    }
    return test_packets() != 0 || test_invalid_preamble() != 0 ? 1 : 0;
}
