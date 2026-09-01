#include "type1.h"

#include "test_cli.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *find_bytes(const uint8_t *bytes, size_t count,
                                 const char *wanted)
{
    size_t wanted_count = strlen(wanted);
    if (wanted_count == 0U || wanted_count > count) {
        return NULL;
    }
    for (size_t index = 0U; index + wanted_count <= count; ++index) {
        if (memcmp(bytes + index, wanted, wanted_count) == 0) {
            return bytes + index;
        }
    }
    return NULL;
}

static void decrypt_bytes(const uint8_t *cipher, size_t count, uint16_t key,
                          uint8_t *plain)
{
    uint16_t state = key;
    for (size_t index = 0U; index < count; ++index) {
        plain[index] = (uint8_t)(cipher[index] ^ (uint8_t)(state >> 8U));
        state = (uint16_t)(((uint32_t)cipher[index] + (uint32_t)state) *
                               UINT32_C(52845) +
                           UINT32_C(22719));
    }
}

static void write_little_endian_u32(uint8_t *bytes, size_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint8_t *make_pfb(const uint8_t *program, size_t length1,
                         size_t length2, size_t *font_count)
{
    static const char trailer[] =
        "00000000000000000000000000000000\ncleartomark\n";
    if (length1 > UINT32_MAX || length2 > UINT32_MAX ||
        sizeof(trailer) - 1U > UINT32_MAX ||
        length1 > SIZE_MAX - length2 - (sizeof(trailer) - 1U) - 20U) {
        return NULL;
    }
    size_t count = length1 + length2 + sizeof(trailer) - 1U + 20U;
    uint8_t *font = malloc(count);
    if (font == NULL) {
        return NULL;
    }
    size_t at = 0U;
    font[at++] = 0x80U;
    font[at++] = 1U;
    write_little_endian_u32(font + at, length1);
    at += 4U;
    memcpy(font + at, program, length1);
    at += length1;
    font[at++] = 0x80U;
    font[at++] = 2U;
    write_little_endian_u32(font + at, length2);
    at += 4U;
    memcpy(font + at, program + length1, length2);
    at += length2;
    font[at++] = 0x80U;
    font[at++] = 1U;
    write_little_endian_u32(font + at, sizeof(trailer) - 1U);
    at += 4U;
    memcpy(font + at, trailer, sizeof(trailer) - 1U);
    at += sizeof(trailer) - 1U;
    font[at++] = 0x80U;
    font[at++] = 3U;
    *font_count = at;
    return font;
}

static uint8_t hexadecimal_digit(unsigned int value)
{
    return (uint8_t)(value < 10U ? (unsigned int)'0' + value
                                 : (unsigned int)'A' + value - 10U);
}

static uint8_t *make_pfa(const uint8_t *program, size_t length1,
                         size_t length2, size_t *font_count)
{
    static const char trailer[] =
        "00000000000000000000000000000000\ncleartomark\n";
    size_t line_breaks = length2 / 32U + 1U;
    if (length2 > (SIZE_MAX - length1 - line_breaks -
                   (sizeof(trailer) - 1U)) /
                      2U) {
        return NULL;
    }
    size_t capacity = length1 + length2 * 2U + line_breaks +
                      sizeof(trailer) - 1U;
    uint8_t *font = malloc(capacity);
    if (font == NULL) {
        return NULL;
    }
    memcpy(font, program, length1);
    size_t at = length1;
    for (size_t index = 0U; index < length2; ++index) {
        uint8_t byte = program[length1 + index];
        font[at++] = hexadecimal_digit((unsigned int)(byte >> 4U));
        font[at++] = hexadecimal_digit((unsigned int)(byte & 15U));
        if ((index + 1U) % 32U == 0U) {
            font[at++] = (uint8_t)'\n';
        }
    }
    if (at == length1 || font[at - 1U] != (uint8_t)'\n') {
        font[at++] = (uint8_t)'\n';
    }
    memcpy(font + at, trailer, sizeof(trailer) - 1U);
    at += sizeof(trailer) - 1U;
    *font_count = at;
    return font;
}

static uint8_t *make_binary_pfa(const uint8_t *program, size_t length1,
                                size_t length2, size_t *font_count)
{
    static const char trailer[] =
        "00000000000000000000000000000000\ncleartomark\n";
    if (length1 > SIZE_MAX - length2 - (sizeof(trailer) - 1U)) {
        return NULL;
    }
    size_t count = length1 + length2 + sizeof(trailer) - 1U;
    uint8_t *font = malloc(count);
    if (font == NULL) {
        return NULL;
    }
    memcpy(font, program, length1 + length2);
    memcpy(font + length1 + length2, trailer, sizeof(trailer) - 1U);
    *font_count = count;
    return font;
}

static int test_disassembler(int container)
{
    static const char disassembly[] =
        "%!PS-AdobeFont-1.0: HSTeXDisassemblyTest 1.0\n"
        "/FontName /HSTeXDisassemblyTest def\n"
        "/FontBBox {0 -200 900 800} readonly def\n"
        "currentfile eexec\n"
        "dup\n"
        "/Private 4 dict dup begin\n"
        "/RD{string currentfile exch readstring pop}executeonly def\n"
        "/ND{noaccess def}executeonly def\n"
        "/NP{noaccess put}executeonly def\n"
        "/lenIV 4 def\n"
        "/Subrs 1 array\n"
        "dup 0 {\n"
        "\t0 100 hstem\n"
        "\treturn\n"
        "\t} NP\n"
        "/CharStrings 2 dict dup begin\n"
        "/.notdef {\n"
        "\t0 500 hsbw\n"
        "\tendchar\n"
        "\t} ND\n"
        "/A {\n"
        "\t0 600 hsbw\n"
        "\t50 600 hstem\n"
        "\tendchar\n"
        "\t} ND\n"
        "end end\n"
        "readonly put\n"
        "put\n"
        "dup/FontName get exch definefont pop\n"
        "mark currentfile closefile\n"
        "cleartomark\n";
    uint8_t *program = NULL;
    size_t program_count = 0U;
    size_t length1 = 0U;
    size_t length2 = 0U;
    char error[256] = {0};
    if (hstex_type1_assemble((const uint8_t *)disassembly,
                             sizeof(disassembly) - 1U, &program,
                             &program_count, &length1, &length2, error,
                             sizeof(error)) != 0) {
        (void)fprintf(stderr,
                      "test_type1: disassembler fixture assembly failed: %s\n",
                      error);
        return 1;
    }
    size_t font_count = 0U;
    uint8_t *font = container == 0
                        ? make_pfb(program, length1, length2, &font_count)
                        : container == 1
                              ? make_pfa(program, length1, length2,
                                         &font_count)
                              : make_binary_pfa(program, length1, length2,
                                                &font_count);
    const char *kind = container == 0 ? "PFB"
                       : container == 1 ? "hexadecimal PFA"
                                        : "binary PFA";
    if (font == NULL) {
        free(program);
        return 1;
    }
    uint8_t *decoded = NULL;
    size_t decoded_count = 0U;
    int status = hstex_type1_disassemble(
        font, font_count, &decoded, &decoded_count, error, sizeof(error));
    if (status != 0) {
        (void)fprintf(stderr, "test_type1: %s disassembly failed: %s\n", kind,
                      error);
        status = 1;
    } else if (decoded_count != sizeof(disassembly) - 1U ||
               memcmp(decoded, disassembly, sizeof(disassembly) - 1U) != 0) {
        (void)fprintf(stderr,
                      "test_type1: %s disassembly differs (%zu/%zu bytes)\n",
                      kind, decoded_count, sizeof(disassembly) - 1U);
        status = 1;
    }
    uint8_t *round_trip = NULL;
    size_t round_trip_count = 0U;
    size_t round_length1 = 0U;
    size_t round_length2 = 0U;
    if (status == 0 &&
        hstex_type1_assemble(decoded, decoded_count, &round_trip,
                             &round_trip_count, &round_length1,
                             &round_length2, error, sizeof(error)) != 0) {
        (void)fprintf(stderr,
                      "test_type1: disassembly reassembly failed: %s\n",
                      error);
        status = 1;
    }
    if (status == 0 &&
        (round_trip_count != program_count || round_length1 != length1 ||
         round_length2 != length2 ||
         memcmp(round_trip, program, program_count) != 0)) {
        (void)fprintf(stderr,
                      "test_type1: %s round trip differs\n", kind);
        status = 1;
    }
    free(round_trip);
    free(decoded);
    free(font);
    free(program);
    return status;
}

static int check_entry(const uint8_t *private, size_t private_count,
                       const char *header, const uint8_t *wanted,
                       size_t wanted_count, bool encrypted)
{
    const uint8_t *at = find_bytes(private, private_count, header);
    if (at == NULL) {
        (void)fprintf(stderr, "test_type1: missing entry %s\n", header);
        return 1;
    }
    at += strlen(header);
    size_t encrypted_count = 0U;
    while ((size_t)(at - private) < private_count && *at >= (uint8_t)'0' &&
           *at <= (uint8_t)'9') {
        encrypted_count = encrypted_count * 10U +
                          (size_t)(*at - (uint8_t)'0');
        ++at;
    }
    static const char read_string[] = " RD ";
    if ((size_t)(at - private) + sizeof(read_string) - 1U > private_count ||
        memcmp(at, read_string, sizeof(read_string) - 1U) != 0 ||
        encrypted_count > private_count -
                              ((size_t)(at - private) +
                               sizeof(read_string) - 1U)) {
        (void)fprintf(stderr, "test_type1: malformed entry %s\n", header);
        return 1;
    }
    at += sizeof(read_string) - 1U;
    uint8_t *plain = malloc(encrypted_count == 0U ? 1U : encrypted_count);
    if (plain == NULL) {
        return 1;
    }
    if (encrypted) {
        decrypt_bytes(at, encrypted_count, UINT16_C(4330), plain);
    } else if (encrypted_count != 0U) {
        memcpy(plain, at, encrypted_count);
    }
    int status = encrypted_count != wanted_count ||
                         memcmp(plain, wanted, wanted_count) != 0
                     ? 1
                     : 0;
    if (status != 0) {
        (void)fprintf(stderr,
                      "test_type1: encoded entry %s differs (%zu/%zu bytes)\n",
                      header, encrypted_count, wanted_count);
    }
    free(plain);
    return status;
}

static int test_specification_vectors(void)
{
    static const char disassembly[] =
        "%!PS-AdobeFont-1.0: HSTeXType1Test 1.0\n"
        "currentfile eexec\n"
        "dup\n"
        "/Private 4 dict dup begin\n"
        "/RD{string currentfile exch readstring pop}executeonly def\n"
        "/ND{noaccess def}executeonly def\n"
        "/NP{noaccess put}executeonly def\n"
        "/lenIV 4 def\n"
        "/Subrs 2 array\n"
        "dup 0 {\n"
        " -2147483648 -1132 -1131 -108 -107 0 107 108 1131 1132 "
        "2147483647 return\n"
        " } NP\n"
        "dup 1 {\n"
        " hstem vstem vmoveto rlineto hlineto vlineto rrcurveto "
        "closepath callsubr return hsbw endchar rmoveto hmoveto "
        "vhcurveto hvcurveto dotsection vstem3 hstem3 seac sbw div "
        "callothersubr pop setcurrentpoint\n"
        " }NP\n"
        "/CharStrings 1 dict dup begin\n"
        "/C {\n"
        " 50 800 hsbw 0 100 vstem 0 100 hstem 600 100 hstem\n"
        " 0 hmoveto 700 hlineto 100 vlineto -600 hlineto\n"
        " 500 vlineto 600 hlineto 100 vlineto -700 hlineto\n"
        " closepath endchar\n"
        " } ND\n"
        "end end\n"
        "readonly put\n"
        "put\n"
        "dup/FontName get exch definefont pop\n"
        "mark currentfile closefile\n"
        "\n"
        "cleartomark\n";
    static const uint8_t number_vector[] = {
        0U,   0U,   0U,   0U,   255U, 128U, 0U,   0U,   0U,
        255U, 255U, 255U, 251U, 148U, 254U, 255U, 251U, 0U,
        32U,  139U, 246U, 247U, 0U,   250U, 255U, 255U, 0U,
        0U,   4U,   108U, 255U, 127U, 255U, 255U, 255U, 11U,
    };
    static const uint8_t operator_vector[] = {
        0U, 0U, 0U, 0U, 1U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U,
        13U, 14U, 21U, 22U, 30U, 31U, 12U, 0U, 12U, 1U, 12U, 2U,
        12U, 6U, 12U, 7U, 12U, 12U, 12U, 16U, 12U, 17U, 12U, 33U,
    };
    static const uint8_t adobe_c_vector[] = {
        0U,   0U,   0U,   0U,   0xBDU, 0xF9U, 0xB4U, 0x0DU, 0x8BU,
        0xEFU, 0x03U, 0x8BU, 0xEFU, 0x01U, 0xF8U, 0xECU, 0xEFU, 0x01U,
        0x8BU, 0x16U, 0xF9U, 0x50U, 0x06U, 0xEFU, 0x07U, 0xFCU, 0xECU,
        0x06U, 0xF8U, 0x88U, 0x07U, 0xF8U, 0xECU, 0x06U, 0xEFU, 0x07U,
        0xFDU, 0x50U, 0x06U, 0x09U, 0x0EU,
    };
    uint8_t *program = NULL;
    size_t program_count = 0U;
    size_t length1 = 0U;
    size_t length2 = 0U;
    char error[256] = {0};
    if (hstex_type1_assemble((const uint8_t *)disassembly,
                             sizeof(disassembly) - 1U, &program,
                             &program_count, &length1, &length2, error,
                             sizeof(error)) != 0) {
        (void)fprintf(stderr, "test_type1: valid font rejected: %s\n", error);
        return 1;
    }
    const char public_finish[] = "currentfile eexec\n";
    const char *marker = strstr(disassembly, public_finish);
    size_t wanted_length1 =
        (size_t)(marker - disassembly) + sizeof(public_finish) - 1U;
    int status = 0;
    if (length1 != wanted_length1 || length2 == 0U ||
        program_count != length1 + length2 ||
        memcmp(program, disassembly, length1) != 0) {
        (void)fprintf(stderr, "test_type1: public/private lengths differ\n");
        status = 1;
    }
    uint8_t *private = malloc(length2 == 0U ? 1U : length2);
    if (private == NULL) {
        free(program);
        return 1;
    }
    decrypt_bytes(program + length1, length2, UINT16_C(55665), private);
    static const uint8_t zeroes[4] = {0U, 0U, 0U, 0U};
    if (status == 0 && memcmp(private, zeroes, sizeof(zeroes)) != 0) {
        (void)fprintf(stderr, "test_type1: eexec prefix differs\n");
        status = 1;
    }
    if (status == 0) {
        status = check_entry(private + 4U, length2 - 4U, "dup 0 ",
                             number_vector, sizeof(number_vector), true);
    }
    if (status == 0) {
        status = check_entry(private + 4U, length2 - 4U, "dup 1 ",
                             operator_vector, sizeof(operator_vector), true);
    }
    if (status == 0) {
        status = check_entry(private + 4U, length2 - 4U, "/C ",
                             adobe_c_vector, sizeof(adobe_c_vector), true);
    }
    static const char private_finish[] = "mark currentfile closefile\n";
    if (status == 0 &&
        (length2 < 4U + sizeof(private_finish) - 1U ||
         memcmp(private + length2 - (sizeof(private_finish) - 1U),
                private_finish, sizeof(private_finish) - 1U) != 0)) {
        (void)fprintf(stderr, "test_type1: private boundary differs\n");
        status = 1;
    }
    free(private);
    free(program);
    return status;
}

static int test_unknown_operator(void)
{
    static const char disassembly[] =
        "%!PS-AdobeFont-1.0: Bad 1.0\n"
        "currentfile eexec\n"
        "/Subrs 0 array\n"
        "/CharStrings 1 dict dup begin\n"
        "/bad {\n"
        " unknowncommand\n"
        " } ND\n"
        "mark currentfile closefile\n";
    uint8_t *program = NULL;
    size_t program_count = 0U;
    size_t length1 = 0U;
    size_t length2 = 0U;
    char error[256] = {0};
    if (hstex_type1_assemble((const uint8_t *)disassembly,
                             sizeof(disassembly) - 1U, &program,
                             &program_count, &length1, &length2, error,
                             sizeof(error)) == 0) {
        free(program);
        (void)fprintf(stderr, "test_type1: unknown operator accepted\n");
        return 1;
    }
    if (strstr(error, "unknowncommand") == NULL) {
        (void)fprintf(stderr,
                      "test_type1: unknown operator had no detail\n");
        return 1;
    }
    return 0;
}

static int test_len_iv_mode(const char *value, bool encrypted)
{
    static const char prefix[] =
        "%!PS-AdobeFont-1.0: ZeroLenIV 1.0\n"
        "currentfile eexec\n";
    static const char suffix[] =
        "/Subrs 0 array\n"
        "/CharStrings 1 dict dup begin\n"
        "/zero {\n"
        " 0 endchar\n"
        " } ND\n"
        "mark currentfile closefile\n";
    static const uint8_t wanted[] = {139U, 14U};
    char disassembly[512];
    int rendered = snprintf(disassembly, sizeof(disassembly),
                            "%s/lenIV %s def\n%s", prefix, value, suffix);
    if (rendered < 0 || (size_t)rendered >= sizeof(disassembly)) {
        return 1;
    }
    uint8_t *program = NULL;
    size_t program_count = 0U;
    size_t length1 = 0U;
    size_t length2 = 0U;
    char error[256] = {0};
    if (hstex_type1_assemble((const uint8_t *)disassembly,
                             (size_t)rendered, &program,
                             &program_count, &length1, &length2, error,
                             sizeof(error)) != 0) {
        (void)fprintf(stderr, "test_type1: lenIV %s rejected: %s\n", value,
                      error);
        return 1;
    }
    uint8_t *private = malloc(length2 == 0U ? 1U : length2);
    if (private == NULL) {
        free(program);
        return 1;
    }
    decrypt_bytes(program + length1, length2, UINT16_C(55665), private);
    int status = length2 < 4U
                     ? 1
                     : check_entry(private + 4U, length2 - 4U, "/zero ",
                                   wanted, sizeof(wanted), encrypted);
    if (program_count != length1 + length2) {
        status = 1;
    }
    free(private);
    free(program);
    return status;
}

int main(int argc, char **argv)
{
    int arguments = hstex_test_arguments(
        argc, argv, "Run the HSTeX Type 1 codec tests.");
    if (arguments >= 0) {
        return arguments;
    }
    return test_specification_vectors() != 0 || test_unknown_operator() != 0 ||
                   test_len_iv_mode("0", true) != 0 ||
                   test_len_iv_mode("-1", false) != 0 ||
                   test_disassembler(0) != 0 ||
                   test_disassembler(1) != 0 ||
                   test_disassembler(2) != 0
               ? 1
               : 0;
}
