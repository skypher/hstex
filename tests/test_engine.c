#include "hstex/catcode.h"
#include "hstex/engine.h"
#include "hstex/symbol.h"
#include "hstex/token.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static bool test_token_is_category(hstex_token token,
                                   enum hstex_catcode category)
{
    return hstex_token_is_character(token) &&
           hstex_token_category(token) == (uint8_t)category;
}

static bool test_token_is_space(hstex_token token)
{
    return test_token_is_category(token, HSTEX_CAT_SPACE);
}

static int write_all(int descriptor, const uint8_t *data, size_t length)
{
    size_t written = 0U;
    while (written < length) {
        ssize_t result = write(descriptor, data + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static int open_snippet(const char *source, char path[64])
{
    (void)strcpy(path, "/tmp/hstex-engine-test-XXXXXX");
    int descriptor = mkstemp(path);
    if (descriptor < 0) {
        return -1;
    }
    size_t length = strlen(source);
    if (write_all(descriptor, (const uint8_t *)source, length) != 0 ||
        close(descriptor) != 0) {
        (void)close(descriptor);
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static int prepare_engine(struct hstex_engine *engine, const char *path,
                          bool install_macro_catcodes, char *error,
                          size_t error_capacity)
{
    if (hstex_engine_init(engine, error, error_capacity) != 0) {
        return -1;
    }
    if ((install_macro_catcodes &&
         (hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'{',
                            (uint8_t)HSTEX_CAT_BEGIN_GROUP) != 0 ||
          hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'}',
                            (uint8_t)HSTEX_CAT_END_GROUP) != 0 ||
          hstex_catcode_set(&engine->lexical_state.catcodes, (uint8_t)'#',
                            (uint8_t)HSTEX_CAT_PARAMETER) != 0)) ||
        hstex_engine_push_file(engine, path, error, error_capacity) != 0) {
        hstex_engine_destroy(engine);
        return -1;
    }
    return 0;
}

/* Run a snippet the way the driver runs a document -- the engine builds the
   main vertical list itself -- and compare what \message wrote. */
/* Run a document and compare the page description it writes with the one the
   reference wrote for the same source. The bytes are given as a list of
   pieces, like the diagnostic stream. See docs/DECISIONS.md,
   the-page-description. */
static int run_document_dvi(const char *const *source,
                            const char *const *expected);
static int run_document_pdf(const char *const *source,
                            const char *const *expected);

/* The joined text of a list of pieces, which is how the long probes are
   stored: one string literal each would be longer than a C compiler has to
   support. */
static char *joined_text(const char *const *pieces)
{
    size_t total = 0U;
    for (size_t index = 0U; pieces[index] != NULL; ++index) {
        total += strlen(pieces[index]);
    }
    char *text = malloc(total + 1U);
    if (text == NULL) {
        return NULL;
    }
    size_t at = 0U;
    for (size_t index = 0U; pieces[index] != NULL; ++index) {
        size_t length = strlen(pieces[index]);
        memcpy(text + at, pieces[index], length);
        at += length;
    }
    text[total] = '\0';
    return text;
}

static int run_document(const char *source, const char *expected);

static int run_document_parts(const char *const *source,
                              const char *const *expected)
{
    char *document = joined_text(source);
    char *wanted = joined_text(expected);
    if (document == NULL || wanted == NULL) {
        free(document);
        free(wanted);
        (void)fprintf(stderr, "probe allocation failed\n");
        return 1;
    }
    int status = run_document(document, wanted);
    free(document);
    free(wanted);
    return status;
}

static int run_document(const char *source, const char *expected)
{
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "prepare failed: %s\n", error);
        (void)unlink(path);
        return 1;
    }
    /* A probe that ships a page writes a page description, which belongs in
       a place of its own rather than the directory the tests run in. */
    char directory[80];
    (void)snprintf(directory, sizeof(directory), "%s-out", path);
    if (mkdir(directory, 0700) == 0) {
        (void)hstex_engine_set_output_directory(&engine, directory, error,
                                                sizeof(error));
    }
    char *captured = NULL;
    size_t captured_length = 0U;
    FILE *sink = open_memstream(&captured, &captured_length);
    if (sink == NULL) {
        (void)fprintf(stderr, "could not capture messages for %s\n", source);
        hstex_engine_destroy(&engine);
        (void)unlink(path);
        return 1;
    }
    hstex_engine_set_message_stream(&engine, sink);
    struct hstex_source_location last = {0};
    int status = 0;
    if (hstex_engine_run(&engine, &last, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "engine failed for %s: %s\n", source, error);
        status = 1;
    }
    (void)fclose(sink);
    /* The engine says which file it is reading, in brackets, and a probe's
       file has a name made up for it. The opening bracket is written before
       the stream below is listening, so what reaches it is whatever kept
       the next thing clear of it -- a space, or the end of the line -- and
       the closing bracket at the end. Both are taken out. See docs/DECISIONS.md, the-file-notation. */
    if (captured != NULL && captured_length != 0U &&
        (captured[0] == ' ' || captured[0] == '\n')) {
        memmove(captured, captured + 1U, captured_length);
        --captured_length;
    }
    /* And what the run came to, which names the file it wrote. */
    if (captured != NULL) {
        static const char *const endings[] = {"\nOutput written on ",
                                              "\nNo pages of output."};
        for (size_t which = 0U;
             which < sizeof(endings) / sizeof(endings[0]); ++which) {
            char *at = strstr(captured, endings[which]);
            if (at != NULL) {
                captured_length = (size_t)(at - captured);
                captured[captured_length] = '\0';
                break;
            }
        }
    }
    if (captured != NULL && captured_length != 0U &&
        captured[captured_length - 1U] == ')') {
        --captured_length;
        if (captured_length != 0U && captured[captured_length - 1U] == ' ') {
            --captured_length;
        }
        captured[captured_length] = '\0';
    }
    size_t expected_length = strlen(expected);
    if (status == 0 &&
        (captured_length != expected_length ||
         memcmp(captured, expected, expected_length) != 0)) {
        (void)fprintf(stderr,
                      "message mismatch for %s: got %zu bytes, expected %zu\n",
                      source, captured_length, expected_length);
        (void)fprintf(stderr, "actual messages: [%s]\n",
                      captured == NULL ? "" : captured);
        status = 1;
    }
    free(captured);
    hstex_engine_destroy(&engine);
    char written[256];
    const char *name = strrchr(path, '/');
    (void)snprintf(written, sizeof(written), "%s%s.dvi", directory,
                   name == NULL ? path : name);
    (void)unlink(written);
    (void)snprintf(written, sizeof(written), "%s%s.pdf", directory,
                   name == NULL ? path : name);
    (void)unlink(written);
    (void)rmdir(directory);
    (void)unlink(path);
    return status;
}

static int run_document_page(const char *const *source,
                             const char *const *expected, const char *kind);

static int run_document_dvi(const char *const *source,
                            const char *const *expected)
{
    return run_document_page(source, expected, "dvi");
}

static int run_document_pdf(const char *const *source,
                            const char *const *expected)
{
    return run_document_page(source, expected, "pdf");
}

static int run_document_page(const char *const *source,
                             const char *const *expected, const char *kind)
{
    char *document = joined_text(source);
    char *wanted = joined_text(expected);
    if (document == NULL || wanted == NULL) {
        free(document);
        free(wanted);
        return 1;
    }
    char path[64];
    if (open_snippet(document, path) != 0) {
        free(document);
        free(wanted);
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    int status = 0;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "prepare failed: %s\n", error);
        free(document);
        free(wanted);
        (void)unlink(path);
        return 1;
    }
    char directory[80];
    (void)snprintf(directory, sizeof(directory), "%s-%s", path, kind);
    if (mkdir(directory, 0700) != 0 ||
        hstex_engine_set_output_directory(&engine, directory, error,
                                          sizeof(error)) != 0) {
        (void)fprintf(stderr, "cannot make %s\n", directory);
        status = 1;
    }
    FILE *sink = status == 0 ? tmpfile() : NULL;
    if (sink != NULL) {
        hstex_engine_set_message_stream(&engine, sink);
    }
    struct hstex_source_location last = {0};
    if (status == 0 &&
        hstex_engine_run(&engine, &last, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "engine failed: %s\n", error);
        status = 1;
    }
    if (sink != NULL) {
        (void)fclose(sink);
    }
    hstex_engine_destroy(&engine);
    char written[256];
    const char *name = strrchr(path, '/');
    (void)snprintf(written, sizeof(written), "%s%s.%s", directory,
                   name == NULL ? path : name, kind);
    if (status == 0) {
        FILE *file = fopen(written, "rb");
        if (file == NULL) {
            (void)fprintf(stderr, "no page description at %s\n", written);
            status = 1;
        } else {
            char bytes[65536];
            size_t count = fread(bytes, 1U, sizeof(bytes), file);
            (void)fclose(file);
            /* The expectation is written as hexadecimal, since a page
               description has bytes of every value in it. */
            size_t length = strlen(wanted) / 2U;
            char *decoded = malloc(length + 1U);
            if (decoded == NULL) {
                free(document);
                free(wanted);
                return 1;
            }
            for (size_t index = 0U; index < length; ++index) {
                unsigned value = 0U;
                (void)sscanf(wanted + index * 2U, "%2x", &value);
                decoded[index] = (char)value;
            }
            free(wanted);
            wanted = decoded;
            if (count != length || memcmp(bytes, wanted, length) != 0) {
                (void)fprintf(stderr,
                              "page description mismatch: got %zu bytes, "
                              "expected %zu\n",
                              count, length);
                for (size_t index = 0U; index < count && index < length;
                     ++index) {
                    if (bytes[index] != wanted[index]) {
                        (void)fprintf(stderr,
                                      "first difference at byte %zu: %02x "
                                      "against %02x\n",
                                      index, (unsigned char)bytes[index],
                                      (unsigned char)wanted[index]);
                        break;
                    }
                }
                status = 1;
            }
        }
    }
    (void)unlink(written);
    (void)rmdir(directory);
    (void)unlink(path);
    free(document);
    free(wanted);
    return status;
}

/* Page totals; see docs/DECISIONS.md, the-page-builder. */
/* How a packed box's glue was set. The reference only shows this through
   \showbox, so the four numbers are read off its "glue set" line and the
   geometry that produced it; see docs/DECISIONS.md, glue-set. */
static int test_glue_set(void)
{
    static const char source[] =
        "\\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt "
        "\\boxmaxdepth=16383.99998pt "
        "\\def\\R#1{\\vrule width#1pt height1pt depth0pt}"
        "\\def\\H#1{\\hrule height#1pt}"
        "\\setbox0=\\hbox to100pt{\\hskip0pt plus1fil\\R{5}\\hskip0pt plus2fil}"
        "\\setbox1=\\hbox to30pt{\\hskip10pt plus5pt minus2pt\\R{7}}"
        "\\setbox2=\\hbox to5pt{\\hskip20pt minus3pt}"
        "\\setbox3=\\hbox spread5pt{\\hskip0pt minus1pt}"
        "\\setbox4=\\hbox to9pt{\\R{5}\\hskip0pt plus1fill\\hskip0pt plus1fil}"
        "\\setbox5=\\vbox to40pt{\\H{2}\\vskip4pt plus1fill\\H{3}}"
        "\\setbox6=\\hbox to5pt{\\hskip20pt minus1fil}%";
    /* index, sign, order, needed, total -- the reference's ratios are
       31.66667fil, 2.6, - 1.0, none, 4.0fill, 31.0fill and - 15.0fil. */
    static const struct {
        size_t box;
        uint8_t sign;
        uint8_t order;
        int32_t needed;
        int32_t total;
    } expected[] = {
        {0U, 1U, 1U, 95 * 65536, 3 * 65536},
        {1U, 1U, 0U, 13 * 65536, 5 * 65536},
        {2U, 2U, 0U, 3 * 65536, 3 * 65536},
        {3U, 0U, 0U, 0, 0},
        {4U, 1U, 2U, 4 * 65536, 65536},
        {5U, 1U, 2U, 31 * 65536, 65536},
        {6U, 2U, 1U, 15 * 65536, 65536},
    };
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    int status = result != HSTEX_ENGINE_EOF;
    for (size_t index = 0U;
         status == 0 && index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        const struct hstex_glue_set set = engine.boxes[expected[index].box].glue;
        if (set.sign != expected[index].sign ||
            set.order != expected[index].order ||
            set.needed != expected[index].needed ||
            set.total != expected[index].total) {
            (void)fprintf(stderr,
                          "glue set of box %zu is %u/%u %d over %d\n",
                          expected[index].box, (unsigned int)set.sign,
                          (unsigned int)set.order, set.needed, set.total);
            status = 1;
        }
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

/* \showbox writes what the reference writes, so a box can be compared
   against it whole; see docs/DECISIONS.md, showbox. */
static int test_showbox(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=20 \\hbadness=1"
        "0000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000p"
        "t \\boxmaxdepth=16383.99998pt \\font\\f=cmr10 \\f "
        "\\hsize=100pt \\parindent=20pt \\baselineskip=12pt"
        " \\lineskip=1pt \\lineskiplimit=2pt \\parskip=3pt "
        "plus1pt \\parfillskip=0pt plus1fil \\leftskip=1pt "
        "\\rightskip=2pt \\tolerance=10000 \\pretolerance=-"
        "1 \\showbox9\\setbox0=\\hbox{}\\showbox0\\setbox0="
        "\\hbox to100pt{\\hskip0pt plus1fil\\vrule width5pt"
        " height3pt depth1pt\\hskip0pt plus2fil}\\showbox0"
        "\\setbox0=\\hbox to30pt{\\hskip10pt plus5pt minus2"
        "pt AB\\kern3pt\\lower2pt\\hbox{}}\\showbox0\\setbo"
        "x0=\\vbox to40pt{\\hrule height2pt \\vskip4pt plus"
        "1fill \\hbox{A}\\penalty150 \\moveright3pt\\hbox{}"
        "}\\showbox0\\setbox0=\\hbox spread5pt{\\hskip0pt m"
        "inus1pt\\hskip0pt minus3pt}\\showbox0\\setbox0=\\h"
        "box to20pt{\\leaders\\hbox{\\vrule width2pt height"
        "1pt}\\hfil}\\showbox0\\setbox0=\\hbox to5pt{\\hski"
        "p20pt minus3pt}\\showbox0\\setbox0=\\hbox to5pt{\\"
        "hskip20pt minus1fil}\\showbox0\\setbox0=\\vbox{\\n"
        "oindent AB\\par}\\showbox0\\setbox0=\\vbox{\\hbox{"
        "A}\\hbox{B}\\hbox{C}}\\showboxdepth=1 \\showboxbre"
        "adth=2\\showbox0 \\showboxdepth=0 \\showboxbreadth"
        "=20 \\showbox0\\showboxdepth=10 \\setbox0=\\hbox{A"
        "\\/B}\\showbox0\\setbox0=\\hbox{\\char32}\\showbox"
        "0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=10 \\showboxb...\n\n"
        "\n> \\box9=void\n\n! OK.\n<to be read again> \n   "
        "                \\setbox \nl.1 ...nce=10000 \\pret"
        "olerance=-1 \\showbox9\\setbox\n                  "
        "                                0=\\hbox{}\\showbo"
        "x0\\setbox0...\n\n\n> \\box0=\n\\hbox(0.0+0.0)x0.0"
        "\n\n! OK.\n<to be read again> \n                  "
        " \\setbox \nl.1 ...1 \\showbox9\\setbox0=\\hbox{}"
        "\\showbox0\\setbox\n                              "
        "                    0=\\hbox to100pt{\\hskip0pt .."
        ".\n\n\n> \\box0=\n\\hbox(3.0+1.0)x100.0, glue set "
        "31.66667fil\n.\\glue 0.0 plus 1.0fil\n.\\rule(3.0+"
        "1.0)x5.0\n.\\glue 0.0 plus 2.0fil\n\n! OK.\n<to be"
        " read again> \n                   \\setbox \nl.1 ."
        "..depth1pt\\hskip0pt plus2fil}\\showbox0\\setbox\n"
        "                                                  "
        "0=\\hbox to30pt{\\hskip10pt ...\n\n\n> \\box0=\n\\"
        "hbox(6.83331+2.0)x30.0, glue set 0.48332\n.\\glue "
        "10.0 plus 5.0 minus 2.0\n.\\f A\n.\\f B\n.\\kern 3"
        ".0\n.\\hbox(0.0+0.0)x0.0, shifted 2.0\n\n! OK.\n<t"
        "o be read again> \n                   \\setbox \nl"
        ".1 ...AB\\kern3pt\\lower2pt\\hbox{}}\\showbox0\\se"
        "tbox\n                                            "
        "      0=\\vbox to40pt{\\hrule heig...\n\n\n> \\box"
        "0=\n\\vbox(40.0+0.0)x7.50002, glue set 15.16669fil"
        "l\n.\\rule(2.0+0.0)x*\n.\\glue 4.0 plus 1.0fill\n."
        "\\hbox(6.83331+0.0)x7.50002\n..\\f A\n.\\penalty 1"
        "50\n.\\glue(\\baselineskip) 12.0\n.\\hbox(0.0+0.0)"
        "x0.0, shifted 3.0\n\n! OK.\n<to be read again> \n "
        "                  \\setbox \nl.1 ...ty150 \\moveri"
        "ght3pt\\hbox{}}\\showbox0\\setbox\n               "
        "                                   0=\\hbox spread"
        "5pt{\\hskip0p...\n\n\n> \\box0=\n\\hbox(0.0+0.0)x5"
        ".0\n.\\glue 0.0 minus 1.0\n.\\glue 0.0 minus 3.0\n"
        "\n! OK.\n<to be read again> \n                   "
        "\\setbox \nl.1 ...minus1pt\\hskip0pt minus3pt}\\sh"
        "owbox0\\setbox\n                                  "
        "                0=\\hbox to20pt{\\leaders\\hb...\n"
        "\n\n> \\box0=\n\\hbox(1.0+0.0)x20.0, glue set 20.0"
        "fil\n.\\leaders 0.0 plus 1.0fil\n..\\hbox(1.0+0.0)"
        "x2.0\n...\\rule(1.0+*)x2.0\n\n! OK.\n<to be read a"
        "gain> \n                   \\setbox \nl.1 ...e wid"
        "th2pt height1pt}\\hfil}\\showbox0\\setbox\n       "
        "                                           0=\\hbo"
        "x to5pt{\\hskip20pt m...\n\n\n> \\box0=\n\\hbox(0."
        "0+0.0)x5.0, glue set - 1.0\n.\\glue 20.0 minus 3.0"
        "\n\n! OK.\n<to be read again> \n                  "
        " \\setbox \nl.1 ... to5pt{\\hskip20pt minus3pt}\\s"
        "howbox0\\setbox\n                                 "
        "                 0=\\hbox to5pt{\\hskip20pt m...\n"
        "\n\n> \\box0=\n\\hbox(0.0+0.0)x5.0, glue set - 15."
        "0fil\n.\\glue 20.0 minus 1.0fil\n\n! OK.\n<to be r"
        "ead again> \n                   \\setbox \nl.1 ..."
        "to5pt{\\hskip20pt minus1fil}\\showbox0\\setbox\n  "
        "                                                0="
        "\\vbox{\\noindent AB\\par}\\...\n\n\n> \\box0=\n\\"
        "vbox(6.83331+0.0)x100.0\n.\\hbox(6.83331+0.0)x100."
        "0, glue set 82.41663fil\n..\\glue(\\leftskip) 1.0"
        "\n..\\f A\n..\\f B\n..\\penalty 10000\n..\\glue",
        "(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rights"
        "kip) 2.0\n\n! OK.\n<to be read again> \n          "
        "         \\setbox \nl.1 ...ox0=\\vbox{\\noindent A"
        "B\\par}\\showbox0\\setbox\n                       "
        "                           0=\\vbox{\\hbox{A}\\hbo"
        "x{B}\\h...\n\n\n> \\box0=\n\\vbox(30.83331+0.0)x7."
        "50002\n.\\hbox(6.83331+0.0)x7.50002 []\n.\\glue(\\"
        "baselineskip) 5.16669\n.etc.\n\n! OK.\nl.1 ...\\sh"
        "owboxdepth=1 \\showboxbreadth=2\\showbox0 \n      "
        "                                            \\show"
        "boxdepth=0 \\showboxbr...\n\n\n> \\box0=\n\\vbox(3"
        "0.83331+0.0)x7.50002 []\n\n! OK.\n<to be read agai"
        "n> \n                   \\showboxdepth \nl.1 ...0 "
        "\\showboxbreadth=20 \\showbox0\\showboxdepth\n    "
        "                                              =10 "
        "\\setbox0=\\hbox{A\\/B}\\s...\n\n\n> \\box0=\n\\hb"
        "ox(6.83331+0.0)x14.58337\n.\\f A\n.\\kern 0.0\n.\\"
        "f B\n\n! OK.\n<to be read again> \n               "
        "    \\setbox \nl.1 ...pth=10 \\setbox0=\\hbox{A\\/"
        "B}\\showbox0\\setbox\n                            "
        "                      0=\\hbox{\\char32}\\showbox0"
        " ...\n\n\n> \\box0=\n\\hbox(4.30554+0.0)x2.77779\n"
        ".\\f  \n\n! OK.\nl.1 ...}\\showbox0\\setbox0=\\hbo"
        "x{\\char32}\\showbox0 \n                          "
        "                        \\showbox254\n\n> \\box254"
        "=void\n\n! OK.\nl.1 ...setbox0=\\hbox{\\char32}\\s"
        "howbox0 \\showbox254\n                            "
        "                      \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A whole paragraph, box for box, against the reference: the lines it
   was broken into, the glue each was set with, the kerns and the
   ligatures. See docs/DECISIONS.md, showbox. */
static int test_paragraph_display(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=3 \\showboxbreadth=100\\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        "\\boxmaxdepth=16383.99998pt\\font\\f=cmr10 \\f \\h"
        "size=180pt \\parindent=20pt \\baselineskip=12pt\\l"
        "ineskip=1pt \\lineskiplimit=0pt \\parfillskip=0pt "
        "plus1fil\\tolerance=10000 \\pretolerance=100 \\clu"
        "bpenalty=150 \\widowpenalty=150 \\interlinepenalty"
        "=0 \\brokenpenalty=100 \\linepenalty=10 \\adjdemer"
        "its=10000 \\spaceskip=0pt \\sfcode`\\.=1000\\setbo"
        "x0=\\vbox{The quick brown fox jumps over the lazy "
        "dog and then runsaway into the deep dark forest wh"
        "ere nobody can find him at all.\\par}\\showbox0 \\"
        "showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=3 \\showboxbr...\n\n"
        "\n> \\box0=\n\\vbox(42.94444+0.0)x180.0\n.\\hbox(6"
        ".94444+1.94444)x180.0, glue set 0.12775\n..\\hbox("
        "0.0+0.0)x20.0\n..\\f T\n..\\f h\n..\\f e\n..\\glue"
        " 3.33333 plus 1.66666 minus 1.11111\n..\\f q\n..\\"
        "f u\n..\\f i\n..\\f c\n..\\kern-0.27779\n..\\f k\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n..\\f"
        " b\n..\\f r\n..\\f o\n..\\kern-0.27779\n..\\f w\n."
        ".\\f n\n..\\glue 3.33333 plus 1.66666 minus 1.1111"
        "1\n..\\f f\n..\\f o\n..\\kern-0.27779\n..\\f x\n.."
        "\\glue 3.33333 plus 1.66666 minus 1.11111\n..\\f j"
        "\n..\\f u\n..\\f m\n..\\f p\n..\\f s\n..\\glue 3.3"
        "3333 plus 1.66666 minus 1.11111\n..\\f o\n..\\kern"
        "-0.27779\n..\\f v\n..\\kern-0.27779\n..\\f e\n..\\"
        "f r\n..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f t\n..\\f h\n..\\f e\n..\\glue(\\rightskip) 0"
        ".0\n.\\penalty 150\n.\\glue(\\baselineskip) 3.1111"
        "1\n.\\hbox(6.94444+1.94444)x180.0, glue set - 0.43"
        "933\n..\\f l\n..\\f a\n..\\f z\n..\\f y\n..\\glue "
        "3.33333 plus 1.66666 minus 1.11111\n..\\f d\n..\\f"
        " o\n..\\f g\n..\\glue 3.33333 plus 1.66666 minus 1"
        ".11111\n..\\f a\n..\\f n\n..\\f d\n..\\glue 3.3333"
        "3 plus 1.66666 minus 1.11111\n..\\f t\n..\\f h\n.."
        "\\f e\n..\\f n\n..\\glue 3.33333 plus 1.66666 minu"
        "s 1.11111\n..\\f r\n..\\f u\n..\\f n\n..\\f s\n.."
        "\\f a\n..\\kern-0.27779\n..\\f w\n..\\kern-0.27779"
        "\n..\\f a\n..\\kern-0.27779\n..\\f y\n..\\glue 3.3"
        "3333 plus 1.66666 minus 1.11111\n..\\f i\n..\\f n"
        "\n..\\kern-0.27779\n..\\f t\n..\\f o\n..\\glue 3.3"
        "3333 plus 1.66666 minus 1.11111\n..\\f t\n..\\f h"
        "\n..\\f e\n..\\glue 3.33333 plus 1.66666 minus 1.1"
        "1111\n..\\f d\n..\\f e\n..\\f e\n..\\f p\n..\\glue"
        "(\\rightskip) 0.0\n.\\glue(\\baselineskip) 3.11111"
        "\n.\\hbox(6.94444+1.94444)x180.0, glue set - 0.339"
        "34\n..\\f d\n..\\f a\n..\\f r\n..\\f k\n..\\glue 3"
        ".33333 plus 1.66666 minus 1.11111\n..\\f f\n..\\f "
        "o\n..\\f r\n..\\f e\n..\\f s\n..\\f t\n..\\glue 3."
        "33333 plus 1.66666 minus 1.11111\n..\\f w\n..\\f h"
        "\n..\\f e\n..\\f r\n..\\f e\n..\\glue 3.33333 plus"
        " 1.66666 minus 1.11111\n..\\f n\n..\\f o\n..\\f b"
        "\n..\\kern0.27779\n..\\f o\n..\\kern0.27779\n..\\f"
        " d\n..\\f y\n..\\glue 3.33333 plus 1.66666 minus 1"
        ".11111\n..\\f c\n..\\f a\n..\\f n\n..\\glue 3.3333"
        "3 plus 1.66666 minus 1.11111\n..\\f ^^L (ligature "
        "fi)\n..\\f n\n..\\f d\n..\\glue 3.33333 plus 1.666"
        "66 minus 1.11111\n..\\f h\n..\\f i\n..\\f m\n..\\g"
        "lue 3.33333 plus 1.66666 minus 1.11111\n..\\f a\n."
        ".\\f t\n..\\glue(\\rightskip) 0.0\n.\\penalty 150"
        "\n.\\glue(\\baselineskip) 3.11111\n.\\hbox(6.94444"
        "+0.0)x180.0, glue set 166.66663fil\n..\\f a\n..\\f"
        " l\n..\\f l\n..\\f .\n..\\penalty 10000\n..\\glue("
        "\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightsk"
        "ip) 0.0\n\n! OK.\nl.1 ... nobody can find him at a"
        "ll.\\par}\\showbox0 \n                            "
        "                      \\showbox254\n\n> \\box254=v"
        "oid\n\n! OK.\nl.1 ... find him at all.\\par}\\show"
        "box0 \\showbox254\n                               "
        "                   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The output routine; see docs/DECISIONS.md, the-output-routine. */
static int test_output_routine(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\vsize=50pt \\maxd"
        "epth=2pt \\topskip=10pt \\baselineskip=12pt \\hsiz"
        "e=100pt \\lineskip=0pt \\lineskiplimit=0pt \\boxma"
        "xdepth=16383.99998pt \\hbadness=10000 \\vbadness=1"
        "0000 \\vfuzz=1000pt \\hfuzz=1000pt \\output={\\mes"
        "sage{[OUT|\\the\\outputpenalty|\\the\\deadcycles|"
        "\\the\\ht255|\\the\\dp255|\\the\\wd255|\\the\\badn"
        "ess|\\the\\pagegoal|\\the\\pagetotal]}\\shipout\\b"
        "ox255 }\\def\\B{\\hbox{\\vrule width4pt height20pt"
        " depth1pt}}\\def\\P#1{\\message{[#1|\\the\\pagetot"
        "al|\\the\\pagegoal|\\the\\deadcycles]}}\\B\\P{1}\\"
        "B\\P{2}\\B\\P{3}\\B\\P{4}\\penalty-10000 \\P{5}\\B"
        "\\P{6}\\message{[end|\\the\\pagegoal|\\the\\pageto"
        "tal|\\the\\deadcycles]}\\end \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\vs"
        "ize=50pt \\maxdepth=2pt \\topskip=10pt \\ba...\n\n"
        "\n[1|20.0pt|50.0pt|0] [2|41.0pt|50.0pt|0] [3|62.0p"
        "t|50.0pt|0]\n[OUT|10000|1|50.0pt|1.0pt|4.0pt|10000"
        "|50.0pt|62.0pt] [0] [4|41.0pt|50.0pt|0]\n[OUT|-100"
        "00|1|50.0pt|1.0pt|4.0pt|10000|50.0pt|41.0pt] [0]\n"
        "[5|0.0pt|16383.99998pt|0] [6|20.0pt|50.0pt|0] [end"
        "|50.0pt|20.0pt|0]\n[OUT|-1073741824|1|50.0pt|0.0pt"
        "|100.0pt|0|50.0pt|21.0pt] [0]",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Between one page and the next the engine gives back the nodes nothing can
   reach any more. A box register can reach its own, so a box set before six
   pages are shipped still holds what it was given after they are. See
   docs/DECISIONS.md, what-a-page-leaves-behind. */
static int test_what_a_page_leaves_behind(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth"
        "=10 \\vsize=30pt \\maxdepth=2pt \\topskip=10pt \\b"
        "aselineskip=12pt \\hsize=100pt \\lineskip=0pt \\li"
        "neskiplimit=0pt \\boxmaxdepth=16383.99998pt \\hbad"
        "ness=10000 \\vbadness=10000 \\vfuzz=1000pt \\hfuzz"
        "=1000pt \\output={\\shipout\\box255 }\\def\\B{\\hbo"
        "x{\\vrule width4pt height20pt depth1pt}}\\setbox1="
        "\\hbox{\\vrule width7pt height5pt depth2pt\\kern3pt"
        "\\vrule width1pt}\\B\\B\\B\\B\\B\\B\\showbox1 \\end ",
        NULL,
    };
    static const char *const expected[] = {
        "[0] [0] [0] [0]\n> \\box1=\n\\hbox(5.0+2.0)x11.0\n"
        ".\\rule(5.0+2.0)x7.0\n.\\kern 3.0\n.\\rule(*+*)x1."
        "0\n\n! OK.\nl.1 ...rn3pt\\vrule width1pt}\\B\\B\\B"
        "\\B\\B\\B\\showbox1 \n                            "
        "                      \\end\n\n[0] [0]",
        NULL,
    };
    return run_document_parts(source, expected);
}

static int test_page_totals(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\vsize=200pt \\max"
        "depth=2pt \\topskip=10pt plus 1pt minus 3pt \\boxm"
        "axdepth=16383.99998pt \\baselineskip=12pt \\linesk"
        "ip=0pt \\lineskiplimit=0pt \\hbadness=10000 \\vbad"
        "ness=10000 \\vfuzz=1000pt \\hfuzz=1000pt \\parinde"
        "nt=0pt \\parskip=0pt \\parfillskip=0pt plus1fil \\"
        "tolerance=10000 \\hsize=100pt \\def\\P#1{\\message"
        "{[#1|\\the\\pagegoal|\\the\\pagetotal|\\the\\paged"
        "epth|\\the\\pagestretch|\\the\\pageshrink|\\the\\p"
        "agefilstretch|\\the\\pagefillstretch]}}\\def\\R#1#"
        "2#3{\\vrule width#1pt height#2pt depth#3pt}\\P{1}"
        "\\vskip 7pt \\P{2}\\hrule height5pt depth1pt \\P{3"
        "}\\vskip3pt plus 2pt minus 1pt \\P{4}\\hbox{\\R{4}"
        "{6}{3}}\\P{5}\\penalty 33 \\P{6}\\kern4pt \\P{7}\\"
        "vskip0pt plus1fil \\P{8}\\hbox{\\R{4}{3}{0}}\\P{9}"
        "\\vfill\\hbox{\\R{4}{3}{0}}\\P{10}\\vsize=50pt \\h"
        "box{\\R{4}{3}{0}}\\P{11}\\noindent\\R{5}{8}{1}\\pa"
        "r\\P{12} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\vs"
        "ize=200pt \\maxdepth=2pt \\topskip=10pt pl...\n\n"
        "\n[1|16383.99998pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt|0"
        ".0pt]\n[2|16383.99998pt|0.0pt|0.0pt|0.0pt|0.0pt|0."
        "0pt|0.0pt]\n[3|16383.99998pt|0.0pt|0.0pt|0.0pt|0.0"
        "pt|0.0pt|0.0pt]\n[4|16383.99998pt|0.0pt|0.0pt|0.0p"
        "t|0.0pt|0.0pt|0.0pt]\n[5|200.0pt|21.0pt|2.0pt|3.0p"
        "t|4.0pt|0.0pt|0.0pt]\n[6|200.0pt|21.0pt|2.0pt|3.0p"
        "t|4.0pt|0.0pt|0.0pt]\n[7|200.0pt|21.0pt|2.0pt|3.0p"
        "t|4.0pt|0.0pt|0.0pt]\n[8|200.0pt|21.0pt|2.0pt|3.0p"
        "t|4.0pt|0.0pt|0.0pt]\n[9|200.0pt|36.0pt|0.0pt|3.0p"
        "t|4.0pt|1.0pt|0.0pt]\n[10|200.0pt|48.0pt|0.0pt|3.0"
        "pt|4.0pt|1.0pt|1.0pt]\n[11|200.0pt|60.0pt|0.0pt|3."
        "0pt|4.0pt|1.0pt|1.0pt]\n[12|200.0pt|72.0pt|1.0pt|3"
        ".0pt|4.0pt|1.0pt|1.0pt]\n> \\box254=void\n\n! OK."
        "\nl.1 ...}\\noindent\\R{5}{8}{1}\\par\\P{12} \\sho"
        "wbox254\n                                         "
        "         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

static int run_snippet(const char *source, const char *expected)
{
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "prepare failed: %s\n", error);
        (void)unlink(path);
        return 1;
    }

    uint8_t output[1024];
    size_t output_count = 0U;
    int status = 0;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            &engine, &token, &location, error, sizeof(error));
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
        if (result == HSTEX_ENGINE_ERROR) {
            (void)fprintf(stderr, "engine failed for %s: %s\n", source, error);
            status = 1;
            break;
        }
        if (!hstex_token_is_character(token) ||
            output_count == sizeof(output)) {
            (void)fprintf(stderr, "unexpected output token for %s\n", source);
            status = 1;
            break;
        }
        output[output_count++] = hstex_token_character_code(token);
    }
    size_t expected_length = strlen(expected);
    if (status == 0 &&
        (output_count != expected_length ||
         memcmp(output, expected, expected_length) != 0)) {
        (void)fprintf(stderr,
                      "output mismatch for %s: got %zu bytes, expected %zu\n",
                      source, output_count, expected_length);
        (void)fprintf(stderr, "actual output: [");
        if (output_count != 0U) {
            (void)fwrite(output, 1U, output_count, stderr);
        }
        (void)fprintf(stderr, "]\n");
        status = 1;
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

/* \vsplit; see docs/DECISIONS.md, vsplit. */
static int test_vsplit(void)
{
    return run_snippet(
        "\\splittopskip=5pt \\splitmaxdepth=1pt \\vbadness=10000 \\"
        "vfuzz=1000pt \\hbadness=10000 \\hfuzz=1000pt \\boxmaxdepth"
        "=16383.99998pt \\baselineskip=0pt \\lineskip=0pt \\lineski"
        "plimit=0pt \\hsize=100pt \\def\\H#1#2{\\hrule height#1pt d"
        "epth#2pt }\\def\\S{\\setbox1=\\vbox{\\H{10}{1}\\vskip3pt "
        "\\H{10}{2}\\vskip4pt \\H{10}{0}}}\\S[1|\\the\\ht1|\\the\\d"
        "p1]\\S\\setbox2=\\vsplit1 to 12pt [2|\\the\\ht2|\\the\\dp2"
        "|\\the\\ht1|\\the\\dp1]\\S\\setbox2=\\vsplit1 to 25pt [3|"
        "\\the\\ht2|\\the\\dp2|\\the\\ht1|\\the\\dp1]\\S\\setbox2="
        "\\vsplit1 to 0pt [4|\\the\\ht2|\\the\\dp2|\\the\\ht1|\\the"
        "\\dp1]\\S\\setbox2=\\vsplit1 to 100pt [5|\\the\\ht2|\\the"
        "\\dp2|\\ifvoid1 V\\else N\\fi]\\setbox3=\\box9 \\setbox2="
        "\\vsplit3 to 10pt [6|\\ifvoid2 V\\else N\\fi]\\S\\splittop"
        "skip=30pt \\setbox2=\\vsplit1 to 12pt \\splittopskip=5pt ["
        "7|\\the\\ht2|\\the\\dp2|\\the\\ht1|\\the\\dp1]\\S\\splitma"
        "xdepth=0pt \\setbox2=\\vsplit1 to 25pt \\splitmaxdepth=1pt"
        " [8|\\the\\ht2|\\the\\dp2|\\the\\ht1|\\the\\dp1]%",
        "[1|40.0pt|0.0pt][2|12.0pt|1.0pt|26.0pt|0.0pt][3|25.0pt|1.0"
        "pt|10.0pt|0.0pt][4|0.0pt|1.0pt|26.0pt|0.0pt][5|100.0pt|0.0"
        "pt|V][6|V][7|12.0pt|1.0pt|46.0pt|0.0pt][8|25.0pt|0.0pt|26."
        "0pt|0.0pt]");
}


static int expect_failure(const char *source, const char *error_fragment)
{
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    bool failed = false;
    for (;;) {
        hstex_token token = 0U;
        struct hstex_source_location location;
        enum hstex_engine_result result = hstex_engine_next_output(
            &engine, &token, &location, error, sizeof(error));
        if (result == HSTEX_ENGINE_ERROR) {
            failed = true;
            break;
        }
        if (result == HSTEX_ENGINE_EOF) {
            break;
        }
    }
    int status = !failed || strstr(error, error_fragment) == NULL;
    if (status != 0) {
        (void)fprintf(stderr, "missing expected failure '%s': %s\n",
                      error_fragment, error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

/* A macro that redefines itself while its own body is being read: the body
   is read where it stands rather than copied, so the definition must last as
   long as the reading of it. */
static int test_a_definition_read_while_it_is_replaced(void)
{
    const char source[] = "\\def\\a{\\def\\a{y}x}\\a\\a%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    char produced[16];
    size_t count = 0U;
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result;
    while ((result = hstex_engine_next_output(&engine, &token, &location, error,
                                              sizeof(error))) ==
           HSTEX_ENGINE_TOKEN) {
        if (hstex_token_is_character(token) && count + 1U < sizeof(produced)) {
            produced[count++] = (char)hstex_token_character_code(token);
        }
    }
    produced[count] = '\0';
    int status = result != HSTEX_ENGINE_EOF || strcmp(produced, "xy") != 0;
    if (status != 0) {
        (void)fprintf(stderr,
                      "a definition read while it is replaced: got \"%s\": %s\n",
                      produced, error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

/* A body that asks for none of the macro's arguments is read where it stands
   rather than copied, so it is a definition the input holds while it reads
   it, and the macro may be replaced in the middle of its own body. */
static int test_a_body_that_asks_for_no_argument(void)
{
    const char source[] = "\\def\\a#1{\\def\\a##1{y}x}\\a{Q}\\a{R}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    char produced[16];
    size_t count = 0U;
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result;
    while ((result = hstex_engine_next_output(&engine, &token, &location, error,
                                              sizeof(error))) ==
           HSTEX_ENGINE_TOKEN) {
        if (hstex_token_is_character(token) && count + 1U < sizeof(produced)) {
            produced[count++] = (char)hstex_token_character_code(token);
        }
    }
    produced[count] = '\0';
    int status = result != HSTEX_ENGINE_EOF || strcmp(produced, "xy") != 0;
    if (status != 0) {
        (void)fprintf(stderr, "a body that asks for no argument: got \"%s\": %s\n",
                      produced, error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

/* An argument put in more than once, and two of them in the other order:
   how long the expansion comes to is worked out from what the body was
   counted to be when it was defined. */
static int test_arguments_put_in_more_than_once(void)
{
    const char source[] =
        "\\def\\b#1{#1#1}\\def\\c#1#2{#2#1}\\b{R}\\c{S}{T}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    char produced[16];
    size_t count = 0U;
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result;
    while ((result = hstex_engine_next_output(&engine, &token, &location, error,
                                              sizeof(error))) ==
           HSTEX_ENGINE_TOKEN) {
        if (hstex_token_is_character(token) && count + 1U < sizeof(produced)) {
            produced[count++] = (char)hstex_token_character_code(token);
        }
    }
    produced[count] = '\0';
    int status = result != HSTEX_ENGINE_EOF || strcmp(produced, "RRTS") != 0;
    if (status != 0) {
        (void)fprintf(stderr,
                      "arguments put in more than once: got \"%s\": %s\n",
                      produced, error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static const struct hstex_meaning *meaning_named(struct hstex_engine *engine,
                                                 const char *name);

/* A format is the engine's state put by once the format source has been
   read: the names it knows and what they mean, the registers, the
   parameters, the fonts and the patterns. What is read back is what was put
   by -- a document run from it cannot tell the difference. */
static int test_a_format_a_run_starts_from(void)
{
    const char source[] =
        "\\catcode`\\@=11 \\def\\a#1{[#1]}\\count5=17 \\dimen3=2pt "
        "\\skip2=3pt plus 1fil \\toks1={xy}\\font\\f=cmr10 \\f "
        "\\hyphenation{man-u-script}\\dump%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine written;
    if (prepare_engine(&written, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result;
    do {
        result = hstex_engine_next_output(&written, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    char format_path[64];
    (void)strcpy(format_path, "/tmp/hstex-format-test-XXXXXX");
    int descriptor = mkstemp(format_path);
    int status = result != HSTEX_ENGINE_EOF || !written.dump_requested ||
                 descriptor < 0;
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    if (status == 0 &&
        hstex_engine_write_format(&written, format_path, error,
                                  sizeof(error)) != 0) {
        (void)fprintf(stderr, "format write failed: %s\n", error);
        status = 1;
    }
    struct hstex_engine read_back;
    if (status == 0 && hstex_engine_init(&read_back, error, sizeof(error)) != 0) {
        status = 1;
    } else if (status == 0) {
        if (hstex_engine_read_format(&read_back, format_path, error,
                                     sizeof(error)) != 0) {
            (void)fprintf(stderr, "format read failed: %s\n", error);
            status = 1;
        } else {
            const struct hstex_meaning *one = meaning_named(&written, "a");
            const struct hstex_meaning *other = meaning_named(&read_back, "a");
            status =
                read_back.lexical_state.symbols.entry_count !=
                        written.lexical_state.symbols.entry_count ||
                read_back.macro_count != written.macro_count ||
                read_back.counts[5] != 17 ||
                read_back.dimens[3] != written.dimens[3] ||
                read_back.glues[2].width != written.glues[2].width ||
                read_back.glues[2].stretch != written.glues[2].stretch ||
                read_back.font_count != written.font_count ||
                read_back.current_font != written.current_font ||
                read_back.hyphen_exception_count !=
                        written.hyphen_exception_count ||
                read_back.hyphen_node_count != written.hyphen_node_count ||
                hstex_catcode_get(&read_back.lexical_state.catcodes,
                                  (uint8_t)'@') !=
                        (uint8_t)HSTEX_CAT_LETTER ||
                one->command != HSTEX_COMMAND_MACRO ||
                other->command != one->command ||
                !read_back.dump_requested;
            if (status == 0) {
                const struct hstex_macro *before =
                    &written.macros[one->value.macro_identifier - 1U];
                const struct hstex_macro *after =
                    &read_back.macros[other->value.macro_identifier - 1U];
                status = before->replacement_count != after->replacement_count ||
                         before->parameter_count != after->parameter_count ||
                         memcmp(before->replacement, after->replacement,
                                before->replacement_count *
                                    sizeof(*before->replacement)) != 0;
            }
            if (status == 0) {
                const struct hstex_font *font =
                    &read_back.fonts[read_back.font_count - 1U];
                status = font->name == NULL || strcmp(font->name, "cmr10") != 0;
            }
        }
        hstex_engine_destroy(&read_back);
    }
    if (status != 0) {
        (void)fprintf(stderr, "format test failed: %s\n", error);
    }
    hstex_engine_destroy(&written);
    (void)unlink(path);
    (void)unlink(format_path);
    return status;
}

static int test_macro_flags(void)
{
    const char source[] = "\\long\\outer\\def\\a#1{#1}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    hstex_token token = 0U;
    struct hstex_source_location location;
    enum hstex_engine_result result;
    do {
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);

    static const uint8_t name[] = {'a'};
    hstex_cs_id identifier = 0U;
    int found = hstex_symbol_find(&engine.lexical_state.symbols,
                                  HSTEX_SYMBOL_REGULAR, name, sizeof(name),
                                  &identifier);
    const struct hstex_meaning *meaning = hstex_engine_meaning(&engine, identifier);
    int status = result != HSTEX_ENGINE_EOF || found != 1 ||
                 meaning->command != HSTEX_COMMAND_MACRO ||
                 meaning->value.macro_identifier == 0U ||
                 (size_t)meaning->value.macro_identifier > engine.macro_count;
    if (status == 0) {
        const struct hstex_macro *macro =
            &engine.macros[meaning->value.macro_identifier - 1U];
        /* What the prefixes said, and, separately, what the engine worked
           out for itself: `#1` is a parameter text a call need not read. */
        status = macro->flags !=
                     ((uint8_t)HSTEX_MACRO_LONG | (uint8_t)HSTEX_MACRO_OUTER) ||
                 macro->shape != (uint8_t)HSTEX_MACRO_PLAIN_PARAMETERS;
    }
    if (status != 0) {
        (void)fprintf(stderr, "macro flag test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static const struct hstex_meaning *meaning_named(struct hstex_engine *engine,
                                                 const char *name)
{
    hstex_cs_id identifier = 0U;
    if (hstex_symbol_find(&engine->lexical_state.symbols, HSTEX_SYMBOL_REGULAR,
                          (const uint8_t *)name, strlen(name), &identifier) != 1) {
        return hstex_engine_meaning(engine, 0U);
    }
    return hstex_engine_meaning(engine, identifier);
}

static int test_ini_bootstrap(void)
{
    const char source[] =
        "\\ifnum\\catcode`\\{=1 \\errmessage bad\\fi\n"
        "\\catcode`\\{=1\n"
        "\\catcode`\\}=2\n"
        "\\ifx\\directlua\\undefined\\else\\errmessage bad\\fi\n"
        "\\ifx\\eTeXversion\\undefined\\errmessage bad\\else\\fi\n"
        "\\catcode`\\#=6\n"
        "\\catcode`\\^=7\n"
        "\\chardef\\active=13\n"
        "\\catcode`\\@=11\n"
        "\\countdef\\count@=255\n"
        "\\let\\bgroup={ \\let\\egroup=}\n"
        "\\ifx\\@@input\\@undefined\\let\\@@input\\input\\fi\n"
        "\\newlinechar`\\^^J\n"
        "\\def\\pair#1#2{#2#1}\\pair AB%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, false, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    uint8_t output[16];
    size_t output_count = 0U;
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
        if (result == HSTEX_ENGINE_TOKEN && hstex_token_is_character(token) &&
            !test_token_is_space(token) && output_count < sizeof(output)) {
            output[output_count++] = hstex_token_character_code(token);
        }
    } while (result == HSTEX_ENGINE_TOKEN);

    const struct hstex_meaning *active = meaning_named(&engine, "active");
    const struct hstex_meaning *count_at = meaning_named(&engine, "count@");
    const struct hstex_meaning *begin_group = meaning_named(&engine, "bgroup");
    const struct hstex_meaning *at_input = meaning_named(&engine, "@@input");
    int status = result != HSTEX_ENGINE_EOF || output_count != 2U ||
                 memcmp(output, "BA", 2U) != 0 ||
                 hstex_catcode_get(&engine.lexical_state.catcodes,
                                   (uint8_t)'{') != HSTEX_CAT_BEGIN_GROUP ||
                 hstex_catcode_get(&engine.lexical_state.catcodes,
                                   (uint8_t)'@') != HSTEX_CAT_LETTER ||
                 engine.integer_parameters[HSTEX_INTEGER_NEW_LINE_CHARACTER] !=
                     10 ||
                 active->command != HSTEX_COMMAND_CHAR_GIVEN ||
                 active->value.integer != 13 ||
                 count_at->command != HSTEX_COMMAND_COUNT_REGISTER ||
                 count_at->value.integer != 255 ||
                 begin_group->command != HSTEX_COMMAND_TOKEN_ALIAS ||
                 !test_token_is_category(begin_group->value.token,
                                         HSTEX_CAT_BEGIN_GROUP) ||
                 at_input->command != HSTEX_COMMAND_INPUT;
    if (status != 0) {
        (void)fprintf(stderr, "INITEX bootstrap test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_input_primitive(void)
{
    char child_path[64];
    if (open_snippet("\\def\\fromchild{C}\\endinput"
                     "\\def\\ignored{X}%",
                     child_path) != 0) {
        return 1;
    }
    char source[256];
    int length = snprintf(
        source, sizeof(source),
        "\\input{%s}\\fromchild\\ifdefined\\ignored F\\else I\\fi%%",
        child_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        (void)unlink(child_path);
        return 1;
    }
    int status = run_snippet(source, "CI");
    (void)unlink(child_path);
    return status;
}

static int test_job_name(void)
{
    char temporary_path[] = "/tmp/hstex-jobname-XXXXXX";
    int descriptor = mkstemp(temporary_path);
    if (descriptor < 0) {
        return 1;
    }
    char path[96];
    int path_length = snprintf(path, sizeof(path), "%s.multi.part.tex",
                               temporary_path);
    static const uint8_t source[] = "\\jobname%";
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        rename(temporary_path, path) != 0 ||
        write_all(descriptor, source, sizeof(source) - 1U) != 0 ||
        close(descriptor) != 0) {
        (void)close(descriptor);
        (void)unlink(temporary_path);
        (void)unlink(path);
        return 1;
    }

    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    uint8_t output[96];
    size_t output_count = 0U;
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
        if (result == HSTEX_ENGINE_TOKEN &&
            hstex_token_is_character(token) &&
            output_count < sizeof(output)) {
            output[output_count++] = hstex_token_character_code(token);
        }
    } while (result == HSTEX_ENGINE_TOKEN);

    const char *base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    size_t expected_length = strlen(base) - strlen(".tex");
    int status = result != HSTEX_ENGINE_EOF ||
                 output_count != expected_length ||
                 memcmp(output, base, expected_length) != 0 ||
                 engine.job_name == NULL ||
                 strlen(engine.job_name) != expected_length ||
                 memcmp(engine.job_name, base, expected_length) != 0;
    if (status != 0) {
        (void)fprintf(stderr, "jobname test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_hyphenation_data(void)
{
    const char source[] =
        "\\language=7\\relax"
        "\\lefthyphenmin=1\\relax\\righthyphenmin=1\\relax"
        "\\patterns{a1b b3c .ab4}"
        "\\hyphenation{ab-cd}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);

    static const uint8_t patterned_word[] = "abc";
    static const uint8_t exception_word[] = "abcd";
    uint8_t patterned_breaks[sizeof(patterned_word)] = {0};
    uint8_t exception_breaks[sizeof(exception_word)] = {0};
    int status = result != HSTEX_ENGINE_EOF ||
                 engine.hyphen_pattern_count != 3U ||
                 engine.hyphen_exception_count != 1U ||
                 hstex_engine_hyphenate_word(
                     &engine, 7, patterned_word,
                     sizeof(patterned_word) - 1U, patterned_breaks,
                     sizeof(patterned_breaks), error, sizeof(error)) != 0 ||
                 patterned_breaks[1] != 1U || patterned_breaks[2] != 0U ||
                 hstex_engine_hyphenate_word(
                     &engine, 7, exception_word, sizeof(exception_word) - 1U,
                     exception_breaks, sizeof(exception_breaks), error,
                     sizeof(error)) != 0 ||
                 exception_breaks[1] != 0U || exception_breaks[2] != 1U ||
                 exception_breaks[3] != 0U;
    if (status != 0) {
        (void)fprintf(stderr, "hyphenation-data test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_document_job_transition(void)
{
    char format_path[64];
    char document_path[64];
    if (open_snippet("\\everyjob{J}\\dump", format_path) != 0 ||
        open_snippet("D%", document_path) != 0) {
        (void)unlink(format_path);
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, format_path, true, error, sizeof(error)) != 0) {
        (void)unlink(format_path);
        (void)unlink(document_path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    uint8_t output[2] = {0};
    size_t output_count = 0U;
    if (result == HSTEX_ENGINE_EOF && engine.dump_requested &&
        hstex_engine_begin_job(&engine, document_path, error, sizeof(error)) ==
            0) {
        do {
            hstex_token token = 0U;
            struct hstex_source_location location;
            result = hstex_engine_next_output(&engine, &token, &location, error,
                                              sizeof(error));
            if (result == HSTEX_ENGINE_TOKEN &&
                hstex_token_is_character(token) &&
                output_count < sizeof(output)) {
                output[output_count++] = hstex_token_character_code(token);
            }
        } while (result == HSTEX_ENGINE_TOKEN);
    }
    const char *document_base = strrchr(document_path, '/');
    document_base = document_base == NULL ? document_path : document_base + 1;
    int status = result != HSTEX_ENGINE_EOF || engine.dump_requested ||
                 output_count != sizeof(output) ||
                 memcmp(output, "JD", sizeof(output)) != 0 ||
                 engine.job_name == NULL ||
                 strcmp(engine.job_name, document_base) != 0;
    if (status != 0) {
        (void)fprintf(stderr, "document-job transition test failed: %s\n",
                      error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(format_path);
    (void)unlink(document_path);
    return status;
}

static int test_file_streams(void)
{
    char stream_path[64];
    if (open_snippet("", stream_path) != 0 || unlink(stream_path) != 0) {
        return 1;
    }
    char source[1024];
    int length = snprintf(
        source, sizeof(source),
        "\\chardef\\stream=3 \\immediate\\openout\\stream=%s "
        "\\def\\expected{abc }"
        "\\immediate\\write\\stream{\\expected}\\immediate\\closeout\\stream "
        "\\openin\\stream=\"%s\" \\ifeof\\stream F\\else "
        "\\read\\stream to \\actual "
        "\\ifx\\actual\\expected T\\else F\\fi\\fi "
        "\\closein\\stream "
        "\\openin\\stream=\"%s\" "
        "\\edef\\otherexpected{\\detokenize{abc}}"
        "\\endlinechar=-1 \\readline\\stream to \\otheractual "
        "\\endlinechar=13 "
        "\\ifx\\otheractual\\otherexpected T\\else F\\fi "
        "\\closein\\stream%%",
        stream_path, stream_path, stream_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        return 1;
    }
    int status = run_snippet(source, "TT");
    (void)unlink(stream_path);
    return status;
}

/* An output command without \immediate leaves a whatsit on the list, and the
   text of a \write is expanded only when the page is shipped; see
   docs/DECISIONS.md, whatsits. */
static int test_whatsits(void)
{
    char stream_path[64];
    if (open_snippet("", stream_path) != 0 || unlink(stream_path) != 0) {
        return 1;
    }
    char source[2048];
    int length = snprintf(
        source, sizeof(source),
        "\\tracingonline=1 \\showbox254 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbreadth=20 "
        "\\count0=7 \\def\\x{\\the\\count0}"
        "\\immediate\\openout1=%s "
        "\\setbox0=\\hbox{\\write1{now \\x}\\special{ps \\x}"
        "\\openout2=zz \\closeout2 \\write-5{}\\write16{}}"
        "\\showbox0 "
        "\\setbox2=\\hbox{\\write1{now \\x}}"
        "\\count0=8 \\shipout\\box2 \\immediate\\closeout1 "
        "\\openin1=\"%s\" \\read1 to \\line \\closein1 "
        "\\message{[\\line]} \\showbox254 ",
        stream_path, stream_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        return 1;
    }
    char expected[2048];
    int wanted = snprintf(
        expected, sizeof(expected),
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=10 \\showboxb...\n\n"
        "\n\\openout1 = `%s'.\n\n> \\box0=\n\\hbox(0.0+0.0)"
        "x0.0\n.\\write1{now \\x }\n.\\special{ps 7}\n.\\op"
        "enout2=zz\n.\\closeout2\n.\\write-{}\n.\\write*{}"
        "\n\n! OK.\nl.1 ... \\closeout2 \\write-5{}\\write1"
        "6{}}\\showbox0 \n                                 "
        "                 \\setbox2=\\hbox{\\write1{now..."
        "\n\n\n[8] [now 8 ]\n> \\box254=void\n\n! OK.\nl.1 "
        "...ine \\closein1 \\message{[\\line]} \\showbox254"
        "\n                                                "
        "  \n\n",
        stream_path);
    if (wanted < 0 || (size_t)wanted >= sizeof(expected)) {
        return 1;
    }
    int status = run_document(source, expected);
    (void)unlink(stream_path);
    return status;
}

/* The errors the reference recovers from rather than stopping for: it says
   what was wrong, shows the line it was reading with the reading marked, says
   what it did instead, and goes on. The wording, the two-line context and the
   help are the reference's own; see docs/DECISIONS.md, recoverable-errors. */
static int test_recoverable_errors(void)
{
    return run_document(
        "\\nonstopmode\n"
        "\\lccode256=0\n"
        "\\mathchardef\\aa=\"8000\n"
        "\\showthe\\hsize\n"
        "\\show\\relax\n"
        "\\end\n",
        "! Bad character code (256).\n"
        "<to be read again> \n"
        "                   =\n"
        "l.2 \\lccode256=\n"
        "               0\n"
        "A character number must be between 0 and 255.\n"
        "I changed this one to zero.\n"
        "\n"
        "! Bad mathchar (32768).\n"
        "l.3 \\mathchardef\\aa=\"8000\n"
        "                         \n"
        "A mathchar number must be between 0 and 32767.\n"
        "I changed this one to zero.\n"
        "\n"
        "> 0.0pt.\n"
        "l.4 \\showthe\\hsize\n"
        "                  \n"
        "\n"
        "> \\relax=\\relax.\n"
        "l.5 \\show\\relax\n"
        "               \n"
        "\n");
}

/* \discretionary and \- leave a node that offers the line breaker a third
   choice; see docs/DECISIONS.md, discretionaries. */
static int test_discretionaries(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=30 \\hbadness=10"
        "000 \\vbadness=10000 \\font\\f=cmr10 \\f \\hyphenc"
        "har\\f=45 \\hsize=30pt \\parindent=0pt \\leftskip="
        "0pt \\rightskip=0pt \\baselineskip=12pt \\lineskip"
        "=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fi"
        "l \\tolerance=10000 \\pretolerance=-1 \\boxmaxdept"
        "h=16383.99998pt \\linepenalty=10 \\adjdemerits=100"
        "00 \\doublehyphendemerits=10000 \\finalhyphendemer"
        "its=5000 \\clubpenalty=0 \\widowpenalty=0 \\interl"
        "inepenalty=0 \\brokenpenalty=77 \\hyphenpenalty=50"
        " \\exhyphenpenalty=50 \\hfuzz=1000pt \\vfuzz=1000p"
        "t \\setbox0=\\hbox{a\\discretionary{b}{c}{d}e}\\sh"
        "owbox0 \\setbox0=\\hbox{a\\-e}\\showbox0 \\hyphenc"
        "har\\f=-1 \\setbox0=\\hbox{a\\-e}\\showbox0 \\hyph"
        "enchar\\f=45 \\setbox0=\\vbox{\\noindent aaaa\\dis"
        "cretionary{b}{c}{d}aaaa\\par}\\showbox0 \\setbox0="
        "\\vbox{\\noindent aaaa\\discretionary{}{}{}aaaa\\p"
        "ar}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n> \\box0=\n\\hbox(6.94444+0.0)x15.00003\n.\\f a"
        "\n.\\discretionary replacing 1\n..\\f b\n.|\\f c\n"
        ".\\f d\n.\\f e\n\n! OK.\nl.1 ...=\\hbox{a\\discret"
        "ionary{b}{c}{d}e}\\showbox0 \n                    "
        "                              \\setbox0=\\hbox{a\\"
        "-e}\\showb...\n\n\n> \\box0=\n\\hbox(4.30554+0.0)x"
        "9.44446\n.\\f a\n.\\discretionary\n..\\f -\n.\\f e"
        "\n\n! OK.\nl.1 ...}e}\\showbox0 \\setbox0=\\hbox{a"
        "\\-e}\\showbox0 \n                                "
        "                  \\hyphenchar\\f=-1 \\setbox0=..."
        "\n\n\n> \\box0=\n\\hbox(4.30554+0.0)x9.44446\n.\\f"
        " a\n.\\discretionary\n.\\f e\n\n! OK.\nl.1 ...henc"
        "har\\f=-1 \\setbox0=\\hbox{a\\-e}\\showbox0 \n    "
        "                                              \\hy"
        "phenchar\\f=45 \\setbox0=...\n\n\n> \\box0=\n\\vbo"
        "x(18.94444+0.0)x30.0\n.\\hbox(6.94444+0.0)x30.0\n."
        ".\\f a\n..\\f a\n..\\f a\n..\\f a\n..\\discretiona"
        "ry\n..\\f b\n..\\glue(\\rightskip) 0.0\n.\\penalty"
        " 77\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4.30"
        "554+0.0)x30.0, glue set 5.5555fil\n..\\f c\n..\\f "
        "a\n..\\f a\n..\\f a\n..\\f a\n..\\penalty 10000\n."
        ".\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue("
        "\\rightskip) 0.0\n\n! OK.\nl.1 ...a\\discretionary"
        "{b}{c}{d}aaaa\\par}\\showbox0 \n                  "
        "                                \\setbox0=\\vbox{"
        "\\noindent a...\n\n\n> \\box0=\n\\vbox(16.30554+0."
        "0)x30.0\n.\\hbox(4.30554+0.0)x30.0\n..\\f a\n..\\f"
        " a\n..\\f a\n..\\f a\n..\\discretionary\n..\\glue("
        "\\rightskip) 0.0\n.\\penalty 77\n.\\glue(\\baselin"
        "eskip) 7.69446\n.\\hbox(4.30554+0.0)x30.0, glue se"
        "t 9.99994fil\n..\\f a\n..\\f a\n..\\f a\n..\\f a\n"
        "..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 plu"
        "s 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 "
        "...aaaa\\discretionary{}{}{}aaaa\\par}\\showbox0 "
        "\n                                                "
        "  \\showbox254\n\n> \\box254=void\n\n! OK.\nl.1 .."
        ".tionary{}{}{}aaaa\\par}\\showbox0 \\showbox254\n "
        "                                                 "
        "\n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The outermost vertical list gets \parskip in front of a paragraph however
   little is waiting to be contributed; see docs/DECISIONS.md,
   parskip-in-the-outermost-list. */
static int test_parskip_in_the_outermost_list(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=1 \\showboxbreadth=30 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=100pt \\vsize=200pt "
        "\\topskip=0pt \\maxdepth=0pt \\parindent=0pt \\par"
        "skip=3pt plus1pt \\baselineskip=12pt \\lineskip=0p"
        "t \\lineskiplimit=0pt \\parfillskip=0pt plus1fil "
        "\\leftskip=0pt \\rightskip=0pt \\tolerance=10000 "
        "\\pretolerance=-1 \\clubpenalty=0 \\widowpenalty=0"
        " \\interlinepenalty=0 \\brokenpenalty=0 \\output={"
        "\\showbox255 \\shipout\\box255 }\\hbox{}\\hbox{}\\"
        "noindent A\\par \\penalty-10000  \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=1 \\showboxbr...\n\n"
        "\n> \\box255=\n\\vbox(200.0+0.0)x100.0, glue set 1"
        "73.0\n.\\glue(\\topskip) 0.0\n.\\hbox(0.0+0.0)x0.0"
        "\n.\\glue(\\baselineskip) 12.0\n.\\hbox(0.0+0.0)x0"
        ".0\n.\\glue(\\parskip) 3.0 plus 1.0\n.\\glue(\\bas"
        "elineskip) 5.16669\n.\\hbox(6.83331+0.0)x100.0, gl"
        "ue set 92.49998fil []\n\n! OK.\n<output> {\\showbo"
        "x 255 \n                       \\shipout \\box 255"
        " }\nl.1 ...box{}\\hbox{}\\noindent A\\par \\penalt"
        "y-10000 \n                                        "
        "           \\showbox254\n\n[0]\n> \\box254=void\n"
        "\n! OK.\nl.1 ...\\noindent A\\par \\penalty-10000 "
        " \\showbox254\n                                   "
        "               \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \lpcode and \rpcode let the character at either end of a line stick out
   past the margin; see docs/DECISIONS.md, character-protrusion. */
static int test_character_protrusion(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=40 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=40pt \\parindent=0pt "
        "\\baselineskip=12pt \\lineskip=0pt \\lineskiplimit"
        "=0pt \\parfillskip=0pt plus1fil \\tolerance=10000 "
        "\\pretolerance=-1 \\boxmaxdepth=16383.99998pt \\sp"
        "aceskip=4pt \\lpcode\\f`\\A=100 \\rpcode\\f`\\B=20"
        "0 \\pdfprotrudechars=1 \\leftskip=0pt \\rightskip="
        "0pt plus1fil \\message{[ragged]}\\setbox1=\\vbox{"
        "\\noindent AB\\par}\\showbox1 \\rightskip=0pt \\me"
        "ssage{[emptybox]}\\setbox0=\\hbox{}\\setbox1=\\vbo"
        "x{\\noindent \\copy0 AB\\par}\\showbox1 \\message{"
        "[fullbox]}\\setbox0=\\hbox to0pt{\\hss}\\setbox1="
        "\\vbox{\\noindent \\copy0 AB\\par}\\showbox1 \\mes"
        "sage{[zerokern]}\\setbox1=\\vbox{\\noindent \\kern"
        "0pt AB\\par}\\showbox1 \\message{[zeroglue]}\\setb"
        "ox1=\\vbox{\\noindent \\hskip0pt AB\\par}\\showbox"
        "1 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[ragged]\n> \\box1=\n\\vbox(6.83331+0.0)x40.0\n."
        "\\hbox(6.83331+0.0)x40.0, glue set 14.20831fil\n.."
        "\\kern-1.0 (left margin)\n..\\f A\n..\\f B\n..\\pe"
        "nalty 10000\n..\\kern-2.0 (right margin)\n..\\glue"
        "(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rights"
        "kip) 0.0 plus 1.0fil\n\n! OK.\nl.1 ...}\\setbox1="
        "\\vbox{\\noindent AB\\par}\\showbox1 \n           "
        "                                       \\rightskip"
        "=0pt \\message{[e...\n\n\n[emptybox]\n> \\box1=\n"
        "\\vbox(6.83331+0.0)x40.0\n.\\hbox(6.83331+0.0)x40."
        "0, glue set 28.41663fil\n..\\kern-1.0 (left margin"
        ")\n..\\hbox(0.0+0.0)x0.0\n..\\f A\n..\\f B\n..\\pe"
        "nalty 10000\n..\\kern-2.0 (right margin)\n..\\glue"
        "(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rights"
        "kip) 0.0\n\n! OK.\nl.1 ...x1=\\vbox{\\noindent \\c"
        "opy0 AB\\par}\\showbox1 \n                        "
        "                          \\message{[fullbox]}\\se"
        "tbox...\n\n\n[fullbox]\n> \\box1=\n\\vbox(6.83331+"
        "0.0)x40.0\n.\\hbox(6.83331+0.0)x40.0, glue set 27."
        "41663fil\n..\\hbox(0.0+0.0)x0.0\n...\\glue 0.0 plu"
        "s 1.0fil minus 1.0fil\n..\\f A\n..\\f B\n..\\penal"
        "ty 10000\n..\\kern-2.0 (right margin)\n..\\glue(\\"
        "parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip"
        ") 0.0\n\n! OK.\nl.1 ...x1=\\vbox{\\noindent \\copy"
        "0 AB\\par}\\showbox1 \n                           "
        "                       \\message{[zerokern]}\\setb"
        "o...\n\n\n[zerokern]\n> \\box1=\n\\vbox(6.83331+0."
        "0)x40.0\n.\\hbox(6.83331+0.0)x40.0, glue set 28.41"
        "663fil\n..\\kern-1.0 (left margin)\n..\\kern 0.0\n"
        "..\\f A\n..\\f B\n..\\penalty 10000\n..\\kern-2.0 "
        "(right margin)\n..\\glue(\\parfillskip) 0.0 plus 1"
        ".0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ..."
        "=\\vbox{\\noindent \\kern0pt AB\\par}\\showbox1 \n"
        "                                                  "
        "\\message{[zeroglue]}\\setbo...\n\n\n[zeroglue]\n>"
        " \\box1=\n\\vbox(6.83331+0.0)x40.0\n.\\hbox(6.8333"
        "1+0.0)x40.0, glue set 27.41663fil\n..\\glue 0.0\n."
        ".\\f A\n..\\f B\n..\\penalty 10000\n..\\kern-2.0 ("
        "right margin)\n..\\glue(\\parfillskip) 0.0 plus 1."
        "0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ..."
        "\\vbox{\\noindent \\hskip0pt AB\\par}\\showbox1 \n"
        "                                                  "
        "\\showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...d"
        "ent \\hskip0pt AB\\par}\\showbox1 \\showbox254\n  "
        "                                                \n"
        "\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* An explicit hyphen is followed into a paragraph by an empty
   discretionary; see docs/DECISIONS.md,
   the-discretionary-after-an-explicit-hyphen. */
static int test_the_discretionary_after_an_explicit_hyphen(void)
{
    static const char *const hyphen_source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=60 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=200pt \\parindent=0pt"
        " \\baselineskip=12pt \\lineskip=0pt \\lineskiplimi"
        "t=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\"
        "rightskip=0pt \\spaceskip=4pt \\tolerance=10000 \\"
        "pretolerance=10000 \\boxmaxdepth=16383.99998pt \\h"
        "yphenchar\\f=`\\x \\message{[A]}\\setbox0=\\vbox{"
        "\\noindent axb\\par}\\showbox0 \\message{[B]}\\set"
        "box0=\\vbox{\\noindent a-b\\par}\\showbox0 \\hyphe"
        "nchar\\f=45 \\message{[C]}\\setbox0=\\vbox{\\noind"
        "ent \\hbox{a-b}\\par}\\showbox0 \\message{[D]}\\se"
        "tbox0=\\vbox{\\noindent a-b\\par}\\showbox0 \\mess"
        "age{[E]}\\setbox0=\\vbox{\\noindent a\\char45 b\\p"
        "ar}\\showbox0 \\message{[F]}\\setbox0=\\vbox{\\noi"
        "ndent a--b\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const hyphen_expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[A]\n> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hb"
        "ox(6.94444+0.0)x200.0, glue set 184.16661fil\n..\\"
        "f a\n..\\f x\n..\\discretionary\n..\\f b\n..\\pena"
        "lty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil"
        "\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...\\set"
        "box0=\\vbox{\\noindent axb\\par}\\showbox0 \n     "
        "                                             \\mes"
        "sage{[B]}\\setbox0=\\vbo...\n\n\n[B]\n> \\box0=\n"
        "\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x20"
        "0.0, glue set 186.11108fil\n..\\f a\n..\\f -\n..\\"
        "f b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0."
        "0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\nl.1 ...\\setbox0=\\vbox{\\noindent a-b\\par}\\sh"
        "owbox0 \n                                         "
        "         \\hyphenchar\\f=45 \\message{...\n\n\n[C]"
        "\n> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6."
        "94444+0.0)x200.0, glue set 186.11108fil\n..\\hbox("
        "6.94444+0.0)x13.88892\n...\\f a\n...\\f -\n...\\f "
        "b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 "
        "plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl"
        ".1 ...0=\\vbox{\\noindent \\hbox{a-b}\\par}\\showb"
        "ox0 \n                                            "
        "      \\message{[D]}\\setbox0=\\vbo...\n\n\n[D]\n>"
        " \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.944"
        "44+0.0)x200.0, glue set 186.11108fil\n..\\f a\n.."
        "\\f -\n..\\discretionary\n..\\f b\n..\\penalty 100"
        "00\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\g"
        "lue(\\rightskip) 0.0\n\n! OK.\nl.1 ...\\setbox0=\\"
        "vbox{\\noindent a-b\\par}\\showbox0 \n            "
        "                                      \\message{[E"
        "]}\\setbox0=\\vbo...\n\n\n[E]\n> \\box0=\n\\vbox(6"
        ".94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0, glu"
        "e set 186.11108fil\n..\\f a\n..\\f -\n..\\discreti"
        "onary\n..\\f b\n..\\penalty 10000\n..\\glue(\\parf"
        "illskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0."
        "0\n\n! OK.\nl.1 ...0=\\vbox{\\noindent a\\char45 b"
        "\\par}\\showbox0 \n                               "
        "                   \\message{[F]}\\setbox0=\\vbo.."
        ".\n\n\n[F]\n> \\box0=\n\\vbox(6.94444+0.0)x200.0\n"
        ".\\hbox(6.94444+0.0)x200.0, glue set 184.4444fil\n"
        "..\\f a\n..\\f { (ligature --)\n..\\discretionary"
        "\n..\\f b\n..\\penalty 10000\n..\\glue(\\parfillsk"
        "ip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n"
        "! OK.\nl.1 ...setbox0=\\vbox{\\noindent a--b\\par}"
        "\\showbox0 \n                                     "
        "             \\showbox254\n\n> \\box254=void\n\n! "
        "OK.\nl.1 ...ox{\\noindent a--b\\par}\\showbox0 \\s"
        "howbox254\n                                       "
        "           \n\n",
        NULL,
    };
    if (run_document_parts(hyphen_source, hyphen_expected) != 0) {
        return 1;
    }
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=60 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=200pt \\hyphenchar\\f"
        "=45 \\parindent=0pt \\baselineskip=12pt \\lineskip"
        "=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fi"
        "l \\leftskip=0pt \\rightskip=0pt \\spaceskip=4pt "
        "\\tolerance=10000 \\pretolerance=10000 \\boxmaxdep"
        "th=16383.99998pt \\message{[end]}\\setbox0=\\vbox{"
        "\\noindent ab-\\par}\\showbox0 \\message{[space]}"
        "\\setbox0=\\vbox{\\noindent ab- cd\\par}\\showbox0"
        " \\message{[start]}\\setbox0=\\vbox{\\noindent -ab"
        "\\par}\\showbox0 \\message{[kern]}\\setbox0=\\vbox"
        "{\\noindent a-\\kern2pt b\\par}\\showbox0 \\showbo"
        "x254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[end]\n> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\"
        "hbox(6.94444+0.0)x200.0, glue set 186.11108fil\n.."
        "\\f a\n..\\f b\n..\\f -\n..\\discretionary\n..\\pe"
        "nalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0f"
        "il\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...\\s"
        "etbox0=\\vbox{\\noindent ab-\\par}\\showbox0 \n   "
        "                                               \\m"
        "essage{[space]}\\setbox0=...\n\n\n[space]\n> \\box"
        "0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.94444+0.0"
        ")x200.0, glue set 172.11107fil\n..\\f a\n..\\f b\n"
        "..\\f -\n..\\discretionary\n..\\glue(\\spaceskip) "
        "4.0\n..\\f c\n..\\f d\n..\\penalty 10000\n..\\glue"
        "(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rights"
        "kip) 0.0\n\n! OK.\nl.1 ...tbox0=\\vbox{\\noindent "
        "ab- cd\\par}\\showbox0 \n                         "
        "                         \\message{[start]}\\setbo"
        "x0=...\n\n\n[start]\n> \\box0=\n\\vbox(6.94444+0.0"
        ")x200.0\n.\\hbox(6.94444+0.0)x200.0, glue set 186."
        "11108fil\n..\\f -\n..\\discretionary\n..\\f a\n.."
        "\\f b\n..\\penalty 10000\n..\\glue(\\parfillskip) "
        "0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK"
        ".\nl.1 ...\\setbox0=\\vbox{\\noindent -ab\\par}\\s"
        "howbox0 \n                                        "
        "          \\message{[kern]}\\setbox0=\\...\n\n\n[k"
        "ern]\n> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbo"
        "x(6.94444+0.0)x200.0, glue set 184.11108fil\n..\\f"
        " a\n..\\f -\n..\\discretionary\n..\\kern 2.0\n..\\"
        "f b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0."
        "0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\nl.1 ...\\vbox{\\noindent a-\\kern2pt b\\par}\\sh"
        "owbox0 \n                                         "
        "         \\showbox254\n\n> \\box254=void\n\n! OK."
        "\nl.1 ...dent a-\\kern2pt b\\par}\\showbox0 \\show"
        "box254\n                                          "
        "        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A formula in a paragraph is fenced with math nodes, its spacing keeps the
   name of the muskip it came from, and a binary operator or a relation
   leaves a penalty behind it; see docs/DECISIONS.md, math-nodes and
   math-penalties. */
static int test_math_nodes(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\t"
        "racingonline=1 \\showboxdepth=3 \\showboxbreadth=6"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt"
        " \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt \\bo"
        "xmaxdepth=16383.99998pt \\baselineskip=12pt \\line"
        "skip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plu"
        "s1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=1"
        "0000 \\pretolerance=-1 \\spaceskip=4pt \\font\\ten"
        "rm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 "
        "\\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\font\\f"
        "ivei=cmmi5 \\font\\tensy=cmsy10 \\font\\sevensy=cm"
        "sy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10 \\te"
        "xtfont0=\\tenrm \\scriptfont0=\\sevenrm \\scriptsc"
        "riptfont0=\\fiverm \\textfont1=\\teni \\scriptfont"
        "1=\\seveni \\scriptscriptfont1=\\fivei \\textfont2"
        "=\\tensy \\scriptfont2=\\sevensy \\scriptscriptfon"
        "t2=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\te"
        "nex \\scriptscriptfont3=\\tenex \\tenrm \\mathcode"
        "`\\+=\"202B \\mathcode`\\==\"303D \\thinmuskip=3mu"
        " \\medmuskip=4mu plus 2mu minus 4mu \\thickmuskip="
        "5mu plus 5mu \\relpenalty=500 \\binoppenalty=700 "
        "\\mathsurround=3pt \\message{[A]}\\setbox0=\\vbox{"
        "\\noindent a $x+y=z$ b\\par}\\showbox0 \\mathsurro"
        "und=0pt \\message{[B]}\\setbox0=\\vbox{\\noindent "
        "a $x+y=z$ b\\par}\\showbox0 \\message{[C]}\\setbox"
        "0=\\hbox{$x+y=z$}\\showbox0 \\message{[D]}\\setbox"
        "0=\\vbox{\\noindent a $x=+y$ b\\par}\\showbox0 \\s"
        "howbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\tracingonline=1 \\showboxdept...\n\n"
        "\n[A]\n> \\box0=\n\\vbox(6.94444+1.94444)x200.0\n."
        "\\hbox(6.94444+1.94444)x200.0, glue set 133.82188f"
        "il\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\ma"
        "thon, surrounded 3.0\n..\\teni x\n..\\glue(\\medmu"
        "skip) 2.22217 plus 1.11108 minus 2.22217\n..\\tenr"
        "m +\n..\\penalty 700\n..\\glue(\\medmuskip) 2.2221"
        "7 plus 1.11108 minus 2.22217\n..\\teni y\n..\\kern"
        "0.35878\n..\\glue(\\thickmuskip) 2.77771 plus 2.77"
        "771\n..\\tenrm =\n..\\penalty 500\n..\\glue(\\thic"
        "kmuskip) 2.77771 plus 2.77771\n..\\teni z\n..\\ker"
        "n0.4398\n..\\mathoff, surrounded 3.0\n..\\glue(\\s"
        "paceskip) 4.0\n..\\tenrm b\n..\\penalty 10000\n.."
        "\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\"
        "rightskip) 0.0\n\n! OK.\nl.1 ...=\\vbox{\\noindent"
        " a $x+y=z$ b\\par}\\showbox0 \n                   "
        "                               \\mathsurround=0pt "
        "\\message...\n\n\n[B]\n> \\box0=\n\\vbox(6.94444+1"
        ".94444)x200.0\n.\\hbox(6.94444+1.94444)x200.0, glu"
        "e set 139.82188fil\n..\\tenrm a\n..\\glue(\\spaces"
        "kip) 4.0\n..\\mathon\n..\\teni x\n..\\glue(\\medmu"
        "skip) 2.22217 plus 1.11108 minus 2.22217\n..\\tenr"
        "m +\n..\\penalty 700\n..\\glue(\\medmuskip) 2.2221"
        "7 plus 1.11108 minus 2.22217\n..\\teni y\n..\\kern"
        "0.35878\n..\\glue(\\thickmuskip) 2.77771 plus 2.77"
        "771\n..\\tenrm =\n..\\penalty 500\n..\\glue(\\thic"
        "kmuskip) 2.77771 plus 2.77771\n..\\teni z\n..\\ker"
        "n0.4398\n..\\mathoff\n..\\glue(\\spaceskip) 4.0\n."
        ".\\tenrm b\n..\\penalty 10000\n..\\glue(\\parfills"
        "kip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n"
        "\n! OK.\nl.1 ...=\\vbox{\\noindent a $x+y=z$ b\\pa"
        "r}\\showbox0 \n                                   "
        "               \\message{[C]}\\setbox0=\\hbo...\n"
        "\n\n[C]\n> \\box0=\n\\hbox(5.83333+1.94444)x41.622"
        "53\n.\\mathon\n.\\teni x\n.\\glue(\\medmuskip) 2.2"
        "2217 plus 1.11108 minus 2.22217\n.\\tenrm +\n.\\gl"
        "ue(\\medmuskip) 2.22217 plus 1.11108 minus 2.22217"
        "\n.\\teni y\n.\\kern0.35878\n.\\glue(\\thickmuskip"
        ") 2.77771 plus 2.77771\n.\\tenrm =\n.\\glue(\\thic"
        "kmuskip) 2.77771 plus 2.77771\n.\\teni z\n.\\kern0"
        ".4398\n.\\mathoff\n\n! OK.\nl.1 ...ssage{[C]}\\set"
        "box0=\\hbox{$x+y=z$}\\showbox0 \n                 "
        "                                 \\message{[D]}\\s"
        "etbox0=\\vbo...\n\n\n[D]\n> \\box0=\n\\vbox(6.9444"
        "4+1.94444)x200.0\n.\\hbox(6.94444+1.94444)x200.0, "
        "glue set 149.35652fil\n..\\tenrm a\n..\\glue(\\spa"
        "ceskip) 4.0\n..\\mathon\n..\\teni x\n..\\glue(\\th"
        "ickmuskip) 2.77771 plus 2.77771\n..\\tenrm =\n..\\"
        "penalty 500\n..\\glue(\\thickmuskip) 2.77771 plus "
        "2.77771\n..\\tenrm +\n..\\teni y\n..\\kern0.35878"
        "\n..\\mathoff\n..\\glue(\\spaceskip) 4.0\n..\\tenr"
        "m b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0."
        "0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\nl.1 ...0=\\vbox{\\noindent a $x=+y$ b\\par}\\sho"
        "wbox0 \n                                          "
        "        \\showbox254\n\n> \\box254=void\n\n! OK.\n"
        "l.1 ...indent a $x=+y$ b\\par}\\showbox0 \\showbox"
        "254\n                                             "
        "     \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A word set with a ligature in it is still hyphenated, and a word followed
   by a discretionary is not hyphenated at all; see docs/DECISIONS.md,
   hyphenation. */
static int test_hyphenating_ligatures(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=200 \\hbadness=1"
        "0000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000p"
        "t \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\hsize=2"
        "00pt \\parindent=0pt \\leftskip=0pt \\rightskip=0p"
        "t \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus1fil \\tolerance=1000"
        "0 \\pretolerance=-1 \\boxmaxdepth=16383.99998pt \\"
        "linepenalty=10 \\adjdemerits=10000 \\doublehyphend"
        "emerits=10000 \\finalhyphendemerits=5000 \\clubpen"
        "alty=0 \\widowpenalty=0 \\interlinepenalty=0 \\bro"
        "kenpenalty=0 \\hyphenpenalty=50 \\exhyphenpenalty="
        "50 \\uchyph=0 \\lefthyphenmin=1 \\righthyphenmin=1"
        " \\spaceskip=4pt \\sfcode`\\.=1000 \\lccode`\\a=`"
        "\\a \\lccode`\\c=`\\c \\lccode`\\e=`\\e \\lccode`"
        "\\f=`\\f \\lccode`\\i=`\\i \\lccode`\\l=`\\l \\lcc"
        "ode`\\n=`\\n \\lccode`\\o=`\\o \\lccode`\\x=`\\x "
        "\\patterns{co1fi1nal 1x1} \\message{[lig]}\\setbox"
        "0=\\vbox{\\noindent xx cofinal\\par}\\showbox0 \\m"
        "essage{[hyphen]}\\setbox0=\\vbox{\\noindent xx cof"
        "inal-x\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[lig]\n> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\"
        "hbox(6.94444+0.0)x200.0, glue set 157.111fil\n..\\"
        "f x\n..\\f x\n..\\glue(\\spaceskip) 4.0\n..\\f c\n"
        "..\\f o\n..\\discretionary\n...\\f -\n..\\f ^^L (l"
        "igature fi)\n..\\discretionary\n...\\f -\n..\\f n"
        "\n..\\f a\n..\\f l\n..\\penalty 10000\n..\\glue(\\"
        "parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip"
        ") 0.0\n\n! OK.\nl.1 ...0=\\vbox{\\noindent xx cofi"
        "nal\\par}\\showbox0 \n                            "
        "                      \\message{[hyphen]}\\setbox0"
        "...\n\n\n[hyphen]\n> \\box0=\n\\vbox(6.94444+0.0)x"
        "200.0\n.\\hbox(6.94444+0.0)x200.0, glue set 148.49"
        "986fil\n..\\f x\n..\\f x\n..\\glue(\\spaceskip) 4."
        "0\n..\\f c\n..\\f o\n..\\f ^^L (ligature fi)\n..\\"
        "f n\n..\\f a\n..\\f l\n..\\f -\n..\\discretionary"
        "\n..\\f x\n..\\penalty 10000\n..\\glue(\\parfillsk"
        "ip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n"
        "! OK.\nl.1 ...\\vbox{\\noindent xx cofinal-x\\par}"
        "\\showbox0 \n                                     "
        "             \\showbox254\n\n> \\box254=void\n\n! "
        "OK.\nl.1 ...dent xx cofinal-x\\par}\\showbox0 \\sh"
        "owbox254\n                                        "
        "          \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A line that breaks at a kern keeps it with no width left, and glue after
   an explicit kern is not a place to break at all; see docs/DECISIONS.md,
   what-a-line-keeps-of-its-break. */
static int test_what_a_line_keeps_of_its_break(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=60 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=40pt \\parindent=0pt "
        "\\baselineskip=12pt \\lineskip=0pt \\lineskiplimit"
        "=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\r"
        "ightskip=0pt \\tolerance=10000 \\pretolerance=-1 "
        "\\boxmaxdepth=16383.99998pt \\spaceskip=4pt \\pdfp"
        "rotrudechars=0 \\message{[kern]}\\setbox0=\\vbox{"
        "\\noindent aaaa\\kern20pt\\hskip0pt aaaa\\par}\\sh"
        "owbox0 \\message{[glue]}\\setbox0=\\vbox{\\noinden"
        "t aaaa\\hskip20pt aaaa\\par}\\showbox0 \\showbox25"
        "4 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[kern]\n> \\box0=\n\\vbox(16.30554+0.0)x40.0\n."
        "\\hbox(4.30554+0.0)x40.0\n..\\f a\n..\\f a\n..\\f "
        "a\n..\\f a\n..\\kern 0.0\n..\\glue(\\rightskip) 0."
        "0\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4.3055"
        "4+0.0)x40.0, glue set 19.99994fil\n..\\f a\n..\\f "
        "a\n..\\f a\n..\\f a\n..\\penalty 10000\n..\\glue("
        "\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightsk"
        "ip) 0.0\n\n! OK.\nl.1 ... aaaa\\kern20pt\\hskip0pt"
        " aaaa\\par}\\showbox0 \n                          "
        "                        \\message{[glue]}\\setbox0"
        "=\\...\n\n\n[glue]\n> \\box0=\n\\vbox(16.30554+0.0"
        ")x40.0\n.\\hbox(4.30554+0.0)x40.0\n..\\f a\n..\\f "
        "a\n..\\f a\n..\\f a\n..\\glue(\\rightskip) 0.0\n."
        "\\glue(\\baselineskip) 7.69446\n.\\hbox(4.30554+0."
        "0)x40.0, glue set 19.99994fil\n..\\f a\n..\\f a\n."
        ".\\f a\n..\\f a\n..\\penalty 10000\n..\\glue(\\par"
        "fillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0"
        ".0\n\n! OK.\nl.1 ...noindent aaaa\\hskip20pt aaaa"
        "\\par}\\showbox0 \n                               "
        "                   \\showbox254\n\n> \\box254=void"
        "\n\n! OK.\nl.1 ...aa\\hskip20pt aaaa\\par}\\showbo"
        "x0 \\showbox254\n                                 "
        "                 \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A character in a math list that another character of the same family
   follows is read as part of a word in a text font, and its italic
   correction goes away; see docs/DECISIONS.md, math-text-characters. */
static int test_math_text_characters(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\^=7 \\catcode`\\_=8 \\tracingonline=1 \\s"
        "howboxdepth=6 \\showboxbreadth=60 \\hbadness=10000"
        " \\hfuzz=1000pt \\font\\tenrm=cmr10 \\font\\sevenr"
        "m=cmr7 \\font\\fiverm=cmr5 \\font\\teni=cmmi10 \\f"
        "ont\\seveni=cmmi7 \\font\\fivei=cmmi5 \\font\\tens"
        "y=cmsy10 \\font\\sevensy=cmsy7 \\font\\fivesy=cmsy"
        "5 \\font\\tenex=cmex10 \\textfont0=\\tenrm \\scrip"
        "tfont0=\\sevenrm \\scriptscriptfont0=\\fiverm \\te"
        "xtfont1=\\teni \\scriptfont1=\\seveni \\scriptscri"
        "ptfont1=\\fivei \\textfont2=\\tensy \\scriptfont2="
        "\\sevensy \\scriptscriptfont2=\\fivesy \\textfont3"
        "=\\tenex \\scriptfont3=\\tenex \\scriptscriptfont3"
        "=\\tenex \\tenrm \\mathcode`\\Y=\"7059 \\mathcode`"
        "\\M=\"704D \\mathcode`\\A=\"7041 \\message{[m]}\\s"
        "etbox0=\\hbox{$YM$}\\showbox0 \\mathcode`\\Y=\"059"
        " \\mathcode`\\M=\"04D \\mathcode`\\A=\"041 \\messa"
        "ge{[r]}\\setbox0=\\hbox{$YM$}\\showbox0 \\message{"
        "[ra]}\\setbox0=\\hbox{$YA$}\\showbox0 \\message{[r"
        "s]}\\setbox0=\\hbox{$Y_1M$}\\showbox0 \\message{[t"
        "]}\\setbox0=\\hbox{YM}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\..."
        "\n\n\n[m]\n> \\box0=\n\\hbox(6.83331+0.0)x16.6667"
        "\n.\\mathon\n.\\tenrm Y\n.\\tenrm M\n.\\mathoff\n"
        "\n! OK.\nl.1 ...\\message{[m]}\\setbox0=\\hbox{$YM"
        "$}\\showbox0 \n                                   "
        "               \\mathcode`\\Y=\"059 \\mathcod...\n"
        "\n\n[r]\n> \\box0=\n\\hbox(6.83331+0.0)x16.6667\n."
        "\\mathon\n.\\tenrm Y\n.\\tenrm M\n.\\mathoff\n\n! "
        "OK.\nl.1 ...\\message{[r]}\\setbox0=\\hbox{$YM$}\\"
        "showbox0 \n                                       "
        "           \\message{[ra]}\\setbox0=\\hb...\n\n\n["
        "ra]\n> \\box0=\n\\hbox(6.83331+0.0)x14.16669\n.\\m"
        "athon\n.\\tenrm Y\n.\\kern-0.83334\n.\\tenrm A\n."
        "\\mathoff\n\n! OK.\nl.1 ...message{[ra]}\\setbox0="
        "\\hbox{$YA$}\\showbox0 \n                         "
        "                         \\message{[rs]}\\setbox0="
        "\\hb...\n\n\n[rs]\n> \\box0=\n\\hbox(6.83331+1.499"
        "98)x20.65283\n.\\mathon\n.\\tenrm Y\n.\\hbox(4.511"
        "11+0.0)x3.98613, shifted 1.49998\n..\\sevenrm 1\n."
        "\\tenrm M\n.\\mathoff\n\n! OK.\nl.1 ...ssage{[rs]}"
        "\\setbox0=\\hbox{$Y_1M$}\\showbox0 \n             "
        "                                     \\message{[t]"
        "}\\setbox0=\\hbo...\n\n\n[t]\n> \\box0=\n\\hbox(6."
        "83331+0.0)x16.6667\n.\\tenrm Y\n.\\tenrm M\n\n! OK"
        ".\nl.1 ...0 \\message{[t]}\\setbox0=\\hbox{YM}\\sh"
        "owbox0 \n                                         "
        "         \\showbox254\n\n> \\box254=void\n\n! OK."
        "\nl.1 ...[t]}\\setbox0=\\hbox{YM}\\showbox0 \\show"
        "box254\n                                          "
        "        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A penalty of ten thousand forbids a page break rather than costing that
   much; see docs/DECISIONS.md, the-page-builder. */
static int test_infinite_page_penalty(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=2 \\showboxbreadth=30 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\hsize=100pt \\vsize=50pt \\topskip=0pt \\maxdep"
        "th=0pt \\output={\\message{[F \\the\\outputpenalty"
        "]}\\showbox255 \\shipout\\box255 } \\hbox{}\\penal"
        "ty-100 \\vskip10pt \\penalty10000 \\vskip100pt \\h"
        "box{} \\penalty-10000 \\message{[done]} \\showbox2"
        "54 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=2 \\showboxbr...\n\n"
        "\n[F -100]\n> \\box255=\n\\vbox(50.0+0.0)x0.0\n.\\"
        "glue(\\topskip) 0.0\n.\\hbox(0.0+0.0)x0.0\n\n! OK."
        "\n<output> ...[F \\the \\outputpenalty ]}\\showbox"
        " 255 \n                                           "
        "       \\shipout \\box 255 }\nl.1 ...ty10000 \\vsk"
        "ip100pt \\hbox{} \\penalty-10000 \n               "
        "                                   \\message{[done"
        "]} \\showbox254\n\n[0] [F -10000]\n> \\box255=\n\\"
        "vbox(50.0+0.0)x0.0\n.\\glue(\\topskip) 0.0\n.\\hbo"
        "x(0.0+0.0)x0.0\n\n! OK.\n<output> ...[F \\the \\ou"
        "tputpenalty ]}\\showbox 255 \n                    "
        "                              \\shipout \\box 255 "
        "}\nl.1 ...ty10000 \\vskip100pt \\hbox{} \\penalty-"
        "10000 \n                                          "
        "        \\message{[done]} \\showbox254\n\n[0] [don"
        "e]\n> \\box254=void\n\n! OK.\nl.1 ...\\penalty-100"
        "00 \\message{[done]} \\showbox254\n               "
        "                                   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \emergencystretch buys the paragraph one more pass with that much stretch
   behind every line; see docs/DECISIONS.md, emergency-stretch. */
static int test_emergency_stretch(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=1 \\showboxbreadth=30 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=60pt \\parindent=0pt "
        "\\baselineskip=12pt \\lineskip=0pt \\lineskiplimit"
        "=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\r"
        "ightskip=0pt \\tolerance=200 \\pretolerance=-1 \\b"
        "oxmaxdepth=16383.99998pt \\spaceskip=4pt plus2pt m"
        "inus1pt \\linepenalty=10 \\adjdemerits=10000 \\clu"
        "bpenalty=0 \\widowpenalty=0 \\interlinepenalty=0 "
        "\\brokenpenalty=0 \\pdfprotrudechars=0 \\emergency"
        "stretch=0pt \\message{[none]}\\setbox0=\\vbox{\\no"
        "indent aaaa aaaa aaaa aaaa aaaa aaaa\\par}\\showbo"
        "x0 \\emergencystretch=20pt \\message{[some]}\\setb"
        "ox0=\\vbox{\\noindent aaaa aaaa aaaa aaaa aaaa aaa"
        "a\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=1 \\showboxbr...\n\n"
        "\n[none]\n> \\box0=\n\\vbox(16.30554+0.0)x60.0\n."
        "\\hbox(4.30554+0.0)x60.0, glue set - 1.0 []\n.\\gl"
        "ue(\\baselineskip) 7.69446\n.\\hbox(4.30554+0.0)x6"
        "0.0, glue set - 1.0 []\n\n! OK.\nl.1 ...aaa aaaa a"
        "aaa aaaa aaaa aaaa\\par}\\showbox0 \n             "
        "                                     \\emergencyst"
        "retch=20pt \\me...\n\n\n[some]\n> \\box0=\n\\vbox("
        "28.30554+0.0)x60.0\n.\\hbox(4.30554+0.0)x60.0, glu"
        "e set 7.99994 []\n.\\glue(\\baselineskip) 7.69446"
        "\n.\\hbox(4.30554+0.0)x60.0, glue set 7.99994 []\n"
        ".\\glue(\\baselineskip) 7.69446\n.\\hbox(4.30554+0"
        ".0)x60.0, glue set 15.99988fil []\n\n! OK.\nl.1 .."
        ".aaa aaaa aaaa aaaa aaaa aaaa\\par}\\showbox0 \n  "
        "                                                \\"
        "showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...aa "
        "aaaa aaaa aaaa\\par}\\showbox0 \\showbox254\n     "
        "                                             \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A display formula's line is marked, its skips keep the names of the
   parameters they came from, and the line before it is a widow of a
   different kind; see docs/DECISIONS.md, display-math. */
static int test_display_skips_and_widows(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\t"
        "racingonline=1 \\showboxdepth=3 \\showboxbreadth=6"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt"
        " \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt \\bo"
        "xmaxdepth=16383.99998pt \\baselineskip=12pt \\line"
        "skip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plu"
        "s1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=1"
        "0000 \\pretolerance=-1 \\spaceskip=4pt \\font\\ten"
        "rm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 "
        "\\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\font\\f"
        "ivei=cmmi5 \\font\\tensy=cmsy10 \\font\\sevensy=cm"
        "sy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10 \\te"
        "xtfont0=\\tenrm \\scriptfont0=\\sevenrm \\scriptsc"
        "riptfont0=\\fiverm \\textfont1=\\teni \\scriptfont"
        "1=\\seveni \\scriptscriptfont1=\\fivei \\textfont2"
        "=\\tensy \\scriptfont2=\\sevensy \\scriptscriptfon"
        "t2=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\te"
        "nex \\scriptscriptfont3=\\tenex \\tenrm \\mathcode"
        "`\\+=\"202B \\thinmuskip=3mu \\medmuskip=4mu plus "
        "2mu minus 4mu \\thickmuskip=5mu plus 5mu \\abovedi"
        "splayskip=10pt plus2pt \\belowdisplayskip=11pt plu"
        "s2pt \\abovedisplayshortskip=1pt plus3pt \\belowdi"
        "splayshortskip=2pt plus3pt \\predisplaypenalty=100"
        "00 \\postdisplaypenalty=0 \\widowpenalty=150 \\dis"
        "playwidowpenalty=50 \\clubpenalty=0 \\interlinepen"
        "alty=0 \\message{[short]}\\setbox0=\\vbox{\\noinde"
        "nt aa $$x+y$$ bb\\par}\\showbox0 \\message{[long]}"
        "\\setbox0=\\vbox{\\noindent aa aa aa aa aa aa aa a"
        "a aa aa aa aa aa aa aa aa $$x+y$$ bb\\par}\\showbo"
        "x0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\tracingonline=1 \\showboxdept...\n\n"
        "\n[short]\n> \\box0=\n\\vbox(31.30554+0.0)x200.0\n"
        ".\\hbox(4.30554+0.0)x200.0, glue set 189.99997fil"
        "\n..\\tenrm a\n..\\tenrm a\n..\\penalty 10000\n.."
        "\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\"
        "rightskip) 0.0\n.\\penalty 10000\n.\\glue(\\aboved"
        "isplayshortskip) 1.0 plus 3.0\n.\\glue(\\baselines"
        "kip) 6.16667\n.\\hbox(5.83333+1.94444)x23.199, shi"
        "fted 88.4005, display\n..\\teni x\n..\\glue(\\medm"
        "uskip) 2.22217 plus 1.11108 minus 2.22217\n..\\ten"
        "rm +\n..\\glue(\\medmuskip) 2.22217 plus 1.11108 m"
        "inus 2.22217\n..\\teni y\n..\\kern0.35878\n.\\pena"
        "lty 0\n.\\glue(\\belowdisplayshortskip) 2.0 plus 3"
        ".0\n.\\glue(\\baselineskip) 3.11111\n.\\hbox(6.944"
        "44+0.0)x200.0, glue set 188.88885fil\n..\\tenrm b"
        "\n..\\tenrm b\n..\\penalty 10000\n..\\glue(\\parfi"
        "llskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0"
        "\n\n! OK.\nl.1 ...vbox{\\noindent aa $$x+y$$ bb\\p"
        "ar}\\showbox0 \n                                  "
        "                \\message{[long]}\\setbox0=\\...\n"
        "\n\n[long]\n> \\box0=\n\\vbox(43.30554+0.0)x200.0"
        "\n.\\hbox(4.30554+0.0)x200.0\n..\\tenrm a\n..\\ten"
        "rm a\n..\\glue(\\spaceskip) 4.0\n..\\tenrm a\n..\\"
        "tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\tenrm a\n."
        ".\\tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\tenrm a"
        "\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\tenr"
        "m a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\t"
        "enrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n.."
        "\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0"
        "\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) "
        "4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceski"
        "p) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\space"
        "skip) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\sp"
        "aceskip) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue("
        "\\spaceskip) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\gl"
        "ue(\\spaceskip) 4.0\n..\\tenrm a\n..\\tenrm a\n.."
        "\\glue(\\rightskip) 0.0\n.\\penalty 50\n.\\glue(\\"
        "baselineskip) 7.69446\n.\\hbox(4.30554+0.0)x200.0,"
        " glue set 175.99994fil\n..\\tenrm a\n..\\tenrm a\n"
        "..\\glue(\\spaceskip) 4.0\n..\\tenrm a\n..\\tenrm "
        "a\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 "
        "plus 1.0fil\n..\\glue(\\rightskip) 0.0\n.\\penalty"
        " 10000\n.\\glue(\\abovedisplayshortskip) 1.0 plus "
        "3.0\n.\\glue(\\baselineskip) 6.16667\n.\\hbox(5.83"
        "333+1.94444)x23.199, shifted 88.4005, display\n.."
        "\\teni x\n..\\glue(\\medmuskip) 2.22217 plus 1.111"
        "08 minus 2.22217\n..\\tenrm +\n..\\glue(\\medmuski"
        "p) 2.22217 plus 1.11108 minus 2.22217\n..\\teni y"
        "\n..\\kern0.35878\n.\\penalty 0\n.\\glue(\\belowdi"
        "splayshortskip) 2.0 plus 3.0\n.\\glue(\\baselinesk"
        "ip) 3.11111\n.\\hbox(6.94444+0.0)x200.0, glue set "
        "188.88885fil\n..\\tenrm b\n..\\tenrm b\n..\\penalt"
        "y 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...aa aa a"
        "a aa aa aa $$x+y$$ bb\\par}\\showbox0 \n          "
        "                                        \\showbox2"
        "54\n\n> \\box254=void\n\n! OK.\nl.1 ... aa aa $$x+"
        "y$$ bb\\par}\\showbox0 \\showbox254\n             "
        "                                     \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A box built inside a formula is not the list \prevdepth belongs to; see
   docs/DECISIONS.md, prevdepth-belongs-to-one-list. */
static int test_prevdepth_belongs_to_one_list(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\t"
        "racingonline=1 \\showboxdepth=3 \\showboxbreadth=6"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt"
        " \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt \\bo"
        "xmaxdepth=16383.99998pt \\baselineskip=12pt \\line"
        "skip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plu"
        "s1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=1"
        "0000 \\pretolerance=-1 \\spaceskip=4pt \\font\\ten"
        "rm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 "
        "\\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\font\\f"
        "ivei=cmmi5 \\font\\tensy=cmsy10 \\font\\sevensy=cm"
        "sy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10 \\te"
        "xtfont0=\\tenrm \\scriptfont0=\\sevenrm \\scriptsc"
        "riptfont0=\\fiverm \\textfont1=\\teni \\scriptfont"
        "1=\\seveni \\scriptscriptfont1=\\fivei \\textfont2"
        "=\\tensy \\scriptfont2=\\sevensy \\scriptscriptfon"
        "t2=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\te"
        "nex \\scriptscriptfont3=\\tenex \\tenrm \\mathcode"
        "`\\+=\"202B \\thinmuskip=3mu \\medmuskip=4mu plus "
        "2mu minus 4mu \\thickmuskip=5mu plus 5mu \\abovedi"
        "splayskip=10pt plus2pt \\belowdisplayskip=11pt plu"
        "s2pt \\abovedisplayshortskip=1pt plus3pt \\belowdi"
        "splayshortskip=2pt plus3pt \\predisplaypenalty=100"
        "00 \\postdisplaypenalty=0 \\widowpenalty=150 \\dis"
        "playwidowpenalty=50 \\clubpenalty=0 \\interlinepen"
        "alty=0 \\message{[f1]}\\setbox0=\\hbox{$x\\over y$"
        "}\\showbox0 \\message{[f2]}\\setbox0=\\vbox{\\noin"
        "dent aa $$x\\over y$$ bb\\par}\\showbox0 \\showbox"
        "254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\tracingonline=1 \\showboxdept...\n\n"
        "\n[f1]\n> \\box0=\n\\hbox(6.9512+4.80951)x4.53473"
        "\n.\\mathon\n.\\hbox(6.9512+4.80951)x4.53473\n..\\"
        "hbox(0.0+0.0)x0.0, shifted -2.5\n..\\vbox(6.9512+4"
        ".80951)x4.53473\n...\\hbox(3.01389+0.0)x4.53473 []"
        "\n...\\kern1.23732\n...\\rule(0.39998+0.0)x*\n..."
        "\\kern2.73453\n...\\hbox(3.01389+1.3611)x4.53473, "
        "glue set 0.114fil []\n..\\hbox(0.0+0.0)x0.0, shift"
        "ed -2.5\n.\\mathoff\n\n! OK.\nl.1 ...e{[f1]}\\setb"
        "ox0=\\hbox{$x\\over y$}\\showbox0 \n              "
        "                                    \\message{[f2]"
        "}\\setbox0=\\vb...\n\n\n[f2]\n> \\box0=\n\\vbox(35"
        ".05394+0.0)x200.0\n.\\hbox(4.30554+0.0)x200.0, glu"
        "e set 189.99997fil\n..\\tenrm a\n..\\tenrm a\n..\\"
        "penalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1."
        "0fil\n..\\glue(\\rightskip) 0.0\n.\\penalty 10000"
        "\n.\\glue(\\abovedisplayshortskip) 1.0 plus 3.0\n."
        "\\glue(\\baselineskip) 0.92938\n.\\hbox(11.07062+8"
        ".80396)x5.71527, shifted 97.14236, display\n..\\hb"
        "ox(11.07062+8.80396)x5.71527\n...\\hbox(0.0+0.0)x0"
        ".0, shifted -2.5\n...\\vbox(11.07062+8.80396)x5.71"
        "527 []\n...\\hbox(0.0+0.0)x0.0, shifted -2.5\n.\\p"
        "enalty 0\n.\\glue(\\belowdisplayshortskip) 2.0 plu"
        "s 3.0\n.\\glue(\\lineskip) 0.0\n.\\hbox(6.94444+0."
        "0)x200.0, glue set 188.88885fil\n..\\tenrm b\n..\\"
        "tenrm b\n..\\penalty 10000\n..\\glue(\\parfillskip"
        ") 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! "
        "OK.\nl.1 ...\\noindent aa $$x\\over y$$ bb\\par}\\"
        "showbox0 \n                                       "
        "           \\showbox254\n\n> \\box254=void\n\n! OK"
        ".\nl.1 ...a $$x\\over y$$ bb\\par}\\showbox0 \\sho"
        "wbox254\n                                         "
        "         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \\leftmarginkern and \\rightmarginkern report the kern the breaker put at
   either end of a line, which is how microtype protrudes an equation number;
   see docs/DECISIONS.md, the-margin-kerns-of-a-line. */
static int test_margin_kerns_of_a_line(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\{=1 \\c"
        "atcode`\\}=2 \\tracingonline=1 \\showboxdepth=3 \\"
        "showboxbreadth=40 \\hbadness=10000 \\vbadness=1000"
        "0 \\hfuzz=1000pt \\vfuzz=1000pt \\font\\f=cmr10 \\"
        "f \\hsize=100pt \\parindent=0pt \\baselineskip=12p"
        "t \\lineskip=0pt \\lineskiplimit=0pt \\parfillskip"
        "=0pt \\leftskip=0pt \\rightskip=0pt \\tolerance=10"
        "000 \\pretolerance=-1 \\boxmaxdepth=16383.99998pt "
        "\\splittopskip=0pt \\splitmaxdepth=16383.99998pt "
        "\\lpcode\\f`\\(=117 \\rpcode\\f`\\)=117 \\pdfprotr"
        "udechars=2 \\setbox1=\\vbox{\\noindent(1)\\par} \\"
        "setbox2=\\vbox{\\unvbox1 \\global\\setbox1=\\lastb"
        "ox} \\dimen5=\\leftmarginkern1 \\dimen6=\\rightmar"
        "ginkern1 \\message{<L=\\the\\dimen5><R=\\the\\dime"
        "n6>} \\showbox1 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\{=1 \\catcode`\\}=2 \\tracingonline=...\n"
        "\n\n<L=-1.17pt><R=-1.17pt>\n> \\box1=\n\\hbox(7.5+"
        "2.5)x100.0\n.\\kern-1.17 (left margin)\n.\\f (\n."
        "\\f 1\n.\\f )\n.\\penalty 10000\n.\\kern-1.17 (rig"
        "ht margin)\n.\\glue(\\parfillskip) 0.0\n.\\glue(\\"
        "rightskip) 0.0\n\n! OK.\nl.1 ...{<L=\\the\\dimen5>"
        "<R=\\the\\dimen6>} \\showbox1 \n                  "
        "                                \\showbox254\n\n> "
        "\\box254=void\n\n! OK.\nl.1 ...men5><R=\\the\\dime"
        "n6>} \\showbox1 \\showbox254\n                    "
        "                              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A whatsit's text is shown to within ten characters of the width the
   reference prints to, and the token that reaches that mark is shown whole;
   see docs/DECISIONS.md, whatsits. */
static int test_whatsit_text_is_cut(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\{=1 \\c"
        "atcode`\\}=2 \\tracingonline=1 \\showboxdepth=3 \\"
        "showboxbreadth=30 \\message{[69]}\\setbox0=\\hbox{"
        "\\write1{abcdefghijklmnopqrstuvwxyzabcdefghijklmno"
        "pqrstuvwxyzabcdefghijklmnopq}}\\showbox0 \\message"
        "{[70]}\\setbox0=\\hbox{\\write1{abcdefghijklmnopqr"
        "stuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnop"
        "qr}}\\showbox0 \\message{[cw]}\\setbox0=\\hbox{\\w"
        "rite1{abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqr"
        "stuvwxyzabcdefghijklm\\thepage}}\\showbox0 \\messa"
        "ge{[cw2]}\\setbox0=\\hbox{\\write1{abcdefghijklmno"
        "pqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklm"
        "nop\\thepage}}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\{=1 \\catcode`\\}=2 \\tracingonline=...\n"
        "\n\n[69]\n> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\write"
        "1{abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv"
        "wxyzabcdefghijklmnopq}\n\n\n! OK.\nl.1 ...mnopqrst"
        "uvwxyzabcdefghijklmnopq}}\\showbox0 \n            "
        "                                      \\message{[7"
        "0]}\\setbox0=\\hb...\n\n\n[70]\n> \\box0=\n\\hbox("
        "0.0+0.0)x0.0\n.\\write1{abcdefghijklmnopqrstuvwxyz"
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopq\\\nETC"
        ".}\n\n! OK.\nl.1 ...nopqrstuvwxyzabcdefghijklmnopq"
        "r}}\\showbox0 \n                                  "
        "                \\message{[cw]}\\setbox0=\\hb...\n"
        "\n\n[cw]\n> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\write"
        "1{abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv"
        "wxyzabcdefghijklm\\thep\nage }\n\n! OK.\nl.1 ...qr"
        "stuvwxyzabcdefghijklm\\thepage}}\\showbox0 \n     "
        "                                             \\mes"
        "sage{[cw2]}\\setbox0=\\h...\n\n\n[cw2]\n> \\box0="
        "\n\\hbox(0.0+0.0)x0.0\n.\\write1{abcdefghijklmnopq"
        "rstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmno"
        "p\\t\nhepage }\n\n! OK.\nl.1 ...tuvwxyzabcdefghijk"
        "lmnop\\thepage}}\\showbox0 \n                     "
        "                             \\showbox254\n\n> \\b"
        "ox254=void\n\n! OK.\nl.1 ...efghijklmnop\\thepage}"
        "}\\showbox0 \\showbox254\n                        "
        "                          \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The hyphen a break inserts joins the letter in front of it, and what it
   swallows becomes the discretionary's replacement; see docs/DECISIONS.md,
   a-hyphen-that-ligatures. */
static int test_a_hyphen_that_ligatures(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\lccode`\\a=`\\a \\lccode`\\b=`\\b "
        "\\lccode`\\c=`\\c \\lccode`\\d=`\\d \\lccode`\\e=`"
        "\\e \\lccode`\\f=`\\f \\lccode`\\g=`\\g \\lccode`"
        "\\h=`\\h \\lccode`\\i=`\\i \\lccode`\\j=`\\j \\lcc"
        "ode`\\k=`\\k \\lccode`\\l=`\\l \\lccode`\\m=`\\m "
        "\\lccode`\\n=`\\n \\lccode`\\o=`\\o \\lccode`\\p=`"
        "\\p \\lccode`\\q=`\\q \\lccode`\\r=`\\r \\lccode`"
        "\\s=`\\s \\lccode`\\t=`\\t \\lccode`\\u=`\\u \\lcc"
        "ode`\\v=`\\v \\lccode`\\w=`\\w \\lccode`\\x=`\\x "
        "\\lccode`\\y=`\\y \\lccode`\\z=`\\z \\lccode`\\-=`"
        "\\- \\uchyph=1 \\lefthyphenmin=1 \\righthyphenmin="
        "1 \\patterns{-1g} \\hyphenchar\\tenrm=45 \\pretole"
        "rance=-1 \\tolerance=10000 \\hsize=30pt \\parinden"
        "t=0pt \\baselineskip=12pt \\lineskip=0pt \\lineski"
        "plimit=0pt \\parfillskip=0pt plus1fil \\leftskip=0"
        "pt \\rightskip=0pt \\showboxdepth=10 \\message{[wa"
        "g]}\\setbox1=\\hbox{and-gap}\\setbox0=\\vbox{\\noi"
        "ndent aa \\unhbox1 \\ bb\\par}\\showbox0 \\message"
        "{[wide]}\\hsize=200pt \\setbox1=\\hbox{and-gap}\\s"
        "etbox0=\\vbox{\\noindent aa \\unhbox1 \\ bb\\par}"
        "\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[wag]\n> \\box0=\n\\vbox(40.30554+0.0)x30.0"
        "\n.\\hbox(4.30554+0.0)x30.0\n..\\tenrm a\n..\\tenr"
        "m a\n..\\glue(\\rightskip) 0.0\n.\\glue(\\baseline"
        "skip) 5.05556\n.\\hbox(6.94444+0.0)x30.0\n..\\tenr"
        "m a\n..\\tenrm n\n..\\tenrm d\n..\\discretionary\n"
        "..\\tenrm { (ligature --)\n..\\glue(\\rightskip) 0"
        ".0\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4.305"
        "54+1.94444)x30.0\n..\\tenrm g\n..\\tenrm a\n..\\te"
        "nrm p\n..\\glue(\\rightskip) 0.0\n.\\glue(\\baseli"
        "neskip) 3.11111\n.\\hbox(6.94444+0.0)x30.0, glue s"
        "et 18.88885fil\n..\\tenrm b\n..\\tenrm b\n..\\pena"
        "lty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil"
        "\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...x{\\n"
        "oindent aa \\unhbox1 \\ bb\\par}\\showbox0 \n     "
        "                                             \\mes"
        "sage{[wide]}\\hsize=200...\n\n\n[wide]\n> \\box0="
        "\n\\vbox(6.94444+1.94444)x200.0\n.\\hbox(6.94444+1"
        ".94444)x200.0, glue set 135.88873fil\n..\\tenrm a"
        "\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\tenr"
        "m a\n..\\tenrm n\n..\\tenrm d\n..\\discretionary r"
        "eplacing 1\n...\\tenrm { (ligature --)\n..\\tenrm "
        "-\n..\\tenrm g\n..\\tenrm a\n..\\tenrm p\n..\\glue"
        "(\\spaceskip) 4.0\n..\\tenrm b\n..\\tenrm b\n..\\p"
        "enalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0"
        "fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...x{"
        "\\noindent aa \\unhbox1 \\ bb\\par}\\showbox0 \n  "
        "                                                \\"
        "showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ... aa"
        " \\unhbox1 \\ bb\\par}\\showbox0 \\showbox254\n   "
        "                                               \n"
        "\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A column is as wide as its widest entry, even when that is a negative
   width; see docs/DECISIONS.md, a-column-of-negative-width. */
static int test_a_column_of_negative_width(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\#=6 \\catco"
        "de`\\&=4 \\showboxdepth=3 \\tabskip=2pt \\baseline"
        "skip=0pt \\lineskip=0pt \\lineskiplimit=0pt \\mess"
        "age{[neg]}\\setbox0=\\vbox{\\halign{#&#\\cr\\kern5"
        "pt&\\kern-10pt\\cr}}\\showbox0 \\message{[two]}\\s"
        "etbox0=\\vbox{\\halign{#&#\\cr\\kern5pt&\\kern-10p"
        "t\\cr\\kern1pt&\\kern-3pt\\cr}}\\showbox0 \\showbo"
        "x254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[neg]\n> \\box0=\n\\vbox(0.0+0.0)x1.0\n.\\hb"
        "ox(0.0+0.0)x1.0\n..\\glue(\\tabskip) 2.0\n..\\hbox"
        "(0.0+0.0)x5.0\n...\\kern 5.0\n..\\glue(\\tabskip) "
        "2.0\n..\\hbox(0.0+0.0)x-10.0\n...\\kern -10.0\n.."
        "\\glue(\\tabskip) 2.0\n\n! OK.\nl.1 ...gn{#&#\\cr"
        "\\kern5pt&\\kern-10pt\\cr}}\\showbox0 \n          "
        "                                        \\message{"
        "[two]}\\setbox0=\\v...\n\n\n[two]\n> \\box0=\n\\vb"
        "ox(0.0+0.0)x8.0\n.\\hbox(0.0+0.0)x8.0\n..\\glue(\\"
        "tabskip) 2.0\n..\\hbox(0.0+0.0)x5.0\n...\\kern 5.0"
        "\n..\\glue(\\tabskip) 2.0\n..\\hbox(0.0+0.0)x-3.0"
        "\n...\\kern -10.0\n..\\glue(\\tabskip) 2.0\n.\\glu"
        "e(\\baselineskip) 0.0\n.\\hbox(0.0+0.0)x8.0\n..\\g"
        "lue(\\tabskip) 2.0\n..\\hbox(0.0+0.0)x5.0\n...\\ke"
        "rn 1.0\n..\\glue(\\tabskip) 2.0\n..\\hbox(0.0+0.0)"
        "x-3.0\n...\\kern -3.0\n..\\glue(\\tabskip) 2.0\n\n"
        "! OK.\nl.1 ...rn-10pt\\cr\\kern1pt&\\kern-3pt\\cr}"
        "}\\showbox0 \n                                    "
        "              \\showbox254\n\n> \\box254=void\n\n!"
        " OK.\nl.1 ...kern1pt&\\kern-3pt\\cr}}\\showbox0 \\"
        "showbox254\n                                      "
        "            \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A preamble's repeating part serves every column after the ones written
   out; see docs/DECISIONS.md, a-repeating-preamble. */
static int test_a_repeating_preamble(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\#=6 \\catco"
        "de`\\&=4 \\tabskip=3pt \\showboxdepth=3 \\message{"
        "[loop]}\\setbox0=\\vbox{\\halign{A#\\hfil&&B#\\hfi"
        "l\\cr x&y&z&w\\cr}}\\showbox0 \\message{[plain]}\\"
        "setbox0=\\vbox{\\halign{A#\\hfil&B#\\hfil\\cr x&y"
        "\\cr}}\\showbox0 \\message{[two]}\\tabskip=1pt \\s"
        "etbox0=\\vbox{\\halign{A#\\hfil\\tabskip=2pt&&B#\\"
        "hfil\\tabskip=3pt&C#\\hfil\\tabskip=4pt\\cr p&q&r&"
        "s&t&u\\cr}}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[loop]\n> \\box0=\n\\vbox(6.83331+1.94444)x6"
        "5.97237\n.\\hbox(6.83331+1.94444)x65.97237\n..\\gl"
        "ue(\\tabskip) 3.0\n..\\hbox(6.83331+1.94444)x12.77"
        "782\n...\\tenrm A\n...\\tenrm x\n...\\glue 0.0 plu"
        "s 1.0fil\n..\\glue(\\tabskip) 3.0\n..\\hbox(6.8333"
        "1+1.94444)x12.36116\n...\\tenrm B\n...\\tenrm y\n."
        "..\\glue 0.0 plus 1.0fil\n..\\glue(\\tabskip) 3.0"
        "\n..\\hbox(6.83331+1.94444)x11.5278\n...\\tenrm B"
        "\n...\\tenrm z\n...\\glue 0.0 plus 1.0fil\n..\\glu"
        "e(\\tabskip) 3.0\n..\\hbox(6.83331+1.94444)x14.305"
        "59\n...\\tenrm B\n...\\tenrm w\n...\\glue 0.0 plus"
        " 1.0fil\n..\\glue(\\tabskip) 3.0\n\n! OK.\nl.1 ..."
        "{A#\\hfil&&B#\\hfil\\cr x&y&z&w\\cr}}\\showbox0 \n"
        "                                                  "
        "\\message{[plain]}\\setbox0=...\n\n\n[plain]\n> \\"
        "box0=\n\\vbox(6.83331+1.94444)x34.13898\n.\\hbox(6"
        ".83331+1.94444)x34.13898\n..\\glue(\\tabskip) 3.0"
        "\n..\\hbox(6.83331+1.94444)x12.77782\n...\\tenrm A"
        "\n...\\tenrm x\n...\\glue 0.0 plus 1.0fil\n..\\glu"
        "e(\\tabskip) 3.0\n..\\hbox(6.83331+1.94444)x12.361"
        "16\n...\\tenrm B\n...\\tenrm y\n...\\glue 0.0 plus"
        " 1.0fil\n..\\glue(\\tabskip) 3.0\n\n! OK.\nl.1 ..."
        "align{A#\\hfil&B#\\hfil\\cr x&y\\cr}}\\showbox0 \n"
        "                                                  "
        "\\message{[two]}\\tabskip=1p...\n\n\n[two]\n> \\bo"
        "x0=\n\\vbox(6.83331+1.94444)x91.3335\n.\\hbox(6.83"
        "331+1.94444)x91.3335\n..\\glue(\\tabskip) 1.0\n.."
        "\\hbox(6.83331+1.94444)x13.05559\n...\\tenrm A\n.."
        ".\\tenrm p\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\"
        "tabskip) 2.0\n..\\hbox(6.83331+1.94444)x12.36115\n"
        "...\\tenrm B\n...\\tenrm q\n...\\glue 0.0 plus 1.0"
        "fil\n..\\glue(\\tabskip) 3.0\n..\\hbox(6.83331+1.9"
        "4444)x11.1389\n...\\tenrm C\n...\\tenrm r\n...\\gl"
        "ue 0.0 plus 1.0fil\n..\\glue(\\tabskip) 4.0\n..\\h"
        "box(6.83331+1.94444)x11.0278\n...\\tenrm B\n...\\t"
        "enrm s\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\tabs"
        "kip) 3.0\n..\\hbox(6.83331+1.94444)x11.11113\n..."
        "\\tenrm C\n...\\tenrm t\n...\\glue 0.0 plus 1.0fil"
        "\n..\\glue(\\tabskip) 4.0\n..\\hbox(6.83331+1.9444"
        "4)x12.63893\n...\\tenrm B\n...\\tenrm u\n...\\glue"
        " 0.0 plus 1.0fil\n..\\glue(\\tabskip) 3.0\n\n! OK."
        "\nl.1 ...l\\tabskip=4pt\\cr p&q&r&s&t&u\\cr}}\\sho"
        "wbox0 \n                                          "
        "        \\showbox254\n\n> \\box254=void\n\n! OK.\n"
        "l.1 ...pt\\cr p&q&r&s&t&u\\cr}}\\showbox0 \\showbo"
        "x254\n                                            "
        "      \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* An empty sub-formula is an ordinary atom, which is what makes a relation
   next to it take its spacing; see docs/DECISIONS.md, an-empty-atom. */
static int test_an_empty_atom(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\thinmuskip=3mu \\medm"
        "uskip=4mu plus 2mu minus 4mu \\thickmuskip=5mu plu"
        "s 5mu \\mathcode`\\==\"303D \\message{[e1]}\\setbo"
        "x0=\\hbox{$=x$}\\showbox0 \\message{[e2]}\\setbox0"
        "=\\hbox{${}=x$}\\showbox0 \\message{[e3]}\\setbox0"
        "=\\hbox{$x={}$}\\showbox0 \\message{[e4]}\\setbox0"
        "=\\hbox{$x=y$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[e1]\n> \\box0=\n\\hbox(4.30554+0.0)x16.2707"
        "8\n.\\mathon\n.\\tenrm =\n.\\glue(\\thickmuskip) 2"
        ".77771 plus 2.77771\n.\\teni x\n.\\mathoff\n\n! OK"
        ".\nl.1 ...message{[e1]}\\setbox0=\\hbox{$=x$}\\sho"
        "wbox0 \n                                          "
        "        \\message{[e2]}\\setbox0=\\hb...\n\n\n[e2]"
        "\n> \\box0=\n\\hbox(4.30554+0.0)x19.0485\n.\\matho"
        "n\n.\\hbox(0.0+0.0)x0.0\n.\\glue(\\thickmuskip) 2."
        "77771 plus 2.77771\n.\\tenrm =\n.\\glue(\\thickmus"
        "kip) 2.77771 plus 2.77771\n.\\teni x\n.\\mathoff\n"
        "\n! OK.\nl.1 ...ssage{[e2]}\\setbox0=\\hbox{${}=x$"
        "}\\showbox0 \n                                    "
        "              \\message{[e3]}\\setbox0=\\hb...\n\n"
        "\n[e3]\n> \\box0=\n\\hbox(4.30554+0.0)x19.0485\n."
        "\\mathon\n.\\teni x\n.\\glue(\\thickmuskip) 2.7777"
        "1 plus 2.77771\n.\\tenrm =\n.\\glue(\\thickmuskip)"
        " 2.77771 plus 2.77771\n.\\hbox(0.0+0.0)x0.0\n.\\ma"
        "thoff\n\n! OK.\nl.1 ...ssage{[e3]}\\setbox0=\\hbox"
        "{$x={}$}\\showbox0 \n                             "
        "                     \\message{[e4]}\\setbox0=\\hb"
        "...\n\n\n[e4]\n> \\box0=\n\\hbox(4.30554+1.94444)x"
        "24.31009\n.\\mathon\n.\\teni x\n.\\glue(\\thickmus"
        "kip) 2.77771 plus 2.77771\n.\\tenrm =\n.\\glue(\\t"
        "hickmuskip) 2.77771 plus 2.77771\n.\\teni y\n.\\ke"
        "rn0.35878\n.\\mathoff\n\n! OK.\nl.1 ...essage{[e4]"
        "}\\setbox0=\\hbox{$x=y$}\\showbox0 \n             "
        "                                     \\showbox254"
        "\n\n> \\box254=void\n\n! OK.\nl.1 ...}\\setbox0=\\"
        "hbox{$x=y$}\\showbox0 \\showbox254\n              "
        "                                    \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* An accent goes over its nucleus's scripts as well, but it stands where the
   nucleus alone would put it; see docs/DECISIONS.md,
   an-accent-over-a-script. */
static int test_an_accent_over_a_script(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\skewchar\\teni=127 \\"
        "message{[a]}\\setbox0=\\hbox{$\\mathaccent\"707E x"
        "_a$}\\showbox0 \\message{[c]}\\setbox0=\\hbox{$\\m"
        "athaccent\"707E x^a$}\\showbox0 \\message{[Ua]}\\s"
        "etbox0=\\hbox{$\\mathaccent\"7016 U_a$}\\showbox0 "
        "\\message{[Uc]}\\setbox0=\\hbox{$\\mathaccent\"701"
        "6 U^a$}\\showbox0 \\message{[Ub]}\\setbox0=\\hbox{"
        "$\\mathaccent\"7016 U^a_b$}\\showbox0 \\message{[b"
        "r]}\\setbox0=\\hbox{$\\mathaccent\"707E{x}_a$}\\sh"
        "owbox0 \\message{[r2]}\\setbox0=\\hbox{$\\mathacce"
        "nt\"707E{\\radical\"270370 x}^a$}\\showbox0 \\show"
        "box254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[a]\n> \\box0=\n\\hbox(6.67859+1.49998)x10.0"
        "5292\n.\\mathon\n.\\vbox(6.67859+1.49998)x10.05292"
        "\n..\\hbox(6.67859+0.0)x0.0, shifted 0.63542\n..."
        "\\tenrm ~\n..\\kern-4.30554\n..\\hbox(4.30554+1.49"
        "998)x10.05292\n...\\teni x\n...\\hbox(3.01389+0.0)"
        "x4.33765, shifted 1.49998\n....\\seveni a\n.\\math"
        "off\n\n! OK.\nl.1 ...ox0=\\hbox{$\\mathaccent\"707"
        "E x_a$}\\showbox0 \n                              "
        "                    \\message{[c]}\\setbox0=\\hbo."
        "..\n\n\n[c]\n> \\box0=\n\\hbox(6.67859+0.0)x10.052"
        "92\n.\\mathon\n.\\vbox(6.67859+0.0)x10.05292\n..\\"
        "hbox(6.67859+0.0)x0.0, shifted 0.63542\n...\\tenrm"
        " ~\n..\\kern-6.6428\n..\\hbox(6.6428+0.0)x10.05292"
        "\n...\\teni x\n...\\hbox(3.01389+0.0)x4.33765, shi"
        "fted -3.62892\n....\\seveni a\n.\\mathoff\n\n! OK."
        "\nl.1 ...ox0=\\hbox{$\\mathaccent\"707E x^a$}\\sho"
        "wbox0 \n                                          "
        "        \\message{[Ua]}\\setbox0=\\hb...\n\n\n[Ua]"
        "\n> \\box0=\n\\hbox(8.20554+1.49998)x11.16542\n.\\"
        "mathon\n.\\vbox(8.20554+1.49998)x11.16542\n..\\hbo"
        "x(5.67776+0.0)x0.0, shifted 1.7368\n...\\tenrm ^^V"
        "\n..\\kern-4.30554\n..\\hbox(6.83331+1.49998)x11.1"
        "6542\n...\\teni U\n...\\hbox(3.01389+0.0)x4.33765,"
        " shifted 1.49998\n....\\seveni a\n.\\mathoff\n\n! "
        "OK.\nl.1 ...ox0=\\hbox{$\\mathaccent\"7016 U_a$}\\"
        "showbox0 \n                                       "
        "           \\message{[Uc]}\\setbox0=\\hb...\n\n\n["
        "Uc]\n> \\box0=\n\\hbox(8.20554+0.0)x12.25568\n.\\m"
        "athon\n.\\vbox(8.20554+0.0)x12.25568\n..\\hbox(5.6"
        "7776+0.0)x0.0, shifted 1.7368\n...\\tenrm ^^V\n.."
        "\\kern-4.30554\n..\\hbox(6.83331+0.0)x12.25568\n.."
        ".\\teni U\n...\\kern1.09026\n...\\hbox(3.01389+0.0"
        ")x4.33765, shifted -3.62892\n....\\seveni a\n.\\ma"
        "thoff\n\n! OK.\nl.1 ...ox0=\\hbox{$\\mathaccent\"7"
        "016 U^a$}\\showbox0 \n                            "
        "                      \\message{[Ub]}\\setbox0=\\h"
        "b...\n\n\n[Ub]\n> \\box0=\n\\hbox(8.20554+2.83209)"
        "x12.25568\n.\\mathon\n.\\vbox(8.20554+2.83209)x12."
        "25568\n..\\hbox(5.67776+0.0)x0.0, shifted 1.7368\n"
        "...\\tenrm ^^V\n..\\kern-4.30554\n..\\hbox(6.83331"
        "+2.83209)x12.25568\n...\\teni U\n...\\vbox(9.4749+"
        "0.0)x5.4279, shifted 2.83209\n....\\hbox(3.01389+0"
        ".0)x4.33765, shifted 1.09026\n.....\\seveni a\n..."
        ".\\kern1.59991\n....\\hbox(4.8611+0.0)x3.51666\n.."
        "...\\seveni b\n.\\mathoff\n\n! OK.\nl.1 ...0=\\hbo"
        "x{$\\mathaccent\"7016 U^a_b$}\\showbox0 \n        "
        "                                          \\messag"
        "e{[br]}\\setbox0=\\hb...\n\n\n[br]\n> \\box0=\n\\h"
        "box(6.67859+1.49998)x10.05292\n.\\mathon\n.\\vbox("
        "6.67859+1.49998)x10.05292\n..\\hbox(6.67859+0.0)x0"
        ".0, shifted 0.63542\n...\\tenrm ~\n..\\kern-4.3055"
        "4\n..\\hbox(4.30554+1.49998)x10.05292\n...\\teni x"
        "\n...\\hbox(3.01389+0.0)x4.33765, shifted 1.49998"
        "\n....\\seveni a\n.\\mathoff\n\n! OK.\nl.1 ...x0="
        "\\hbox{$\\mathaccent\"707E{x}_a$}\\showbox0 \n    "
        "                                              \\me"
        "ssage{[r2]}\\setbox0=\\hb...\n\n\n[r2]\n> \\box0="
        "\n\\hbox(10.91745+2.39725)x18.38628\n.\\mathon\n."
        "\\vbox(10.37576+2.39",
        "725)x14.04863\n..\\hbox(6.67859+0.0)x0.0, shifted "
        "4.5243\n...\\tenrm ~\n..\\kern-4.30554\n..\\hbox(8"
        ".00272+2.39725)x14.04863\n...\\hbox(0.39998+9.6)x8"
        ".33336, shifted -7.20276\n....\\tensy p\n...\\vbox"
        "(8.00272+0.0)x5.71527\n....\\kern0.39998\n....\\ru"
        "le(0.39998+0.0)x*\n....\\kern2.89722\n....\\hbox(4"
        ".30554+0.0)x5.71527\n.....\\teni x\n.\\hbox(3.0138"
        "9+0.0)x4.33765, shifted -7.90356\n..\\seveni a\n."
        "\\mathoff\n\n! OK.\nl.1 ...ccent\"707E{\\radical\""
        "270370 x}^a$}\\showbox0 \n                        "
        "                          \\showbox254\n\n> \\box2"
        "54=void\n\n! OK.\nl.1 ...\\radical\"270370 x}^a$}"
        "\\showbox0 \\showbox254\n                         "
        "                         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* A box, a rule and a formula set the space factor back to a thousand; a
   kern and a discretionary leave it alone; see docs/DECISIONS.md,
   what-resets-the-space-factor. */
static int test_what_resets_the_space_factor(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\spaceskip=0pt \\sfcod"
        "e`\\,=1250 \\sfcode`\\]=0 \\message{[rule]}\\setbo"
        "x0=\\hbox{a,\\vrule width2pt\\relax] b}\\showbox0 "
        "\\message{[kern]}\\setbox0=\\hbox{a,\\kern2pt\\rel"
        "ax] b}\\showbox0 \\message{[box]}\\setbox0=\\hbox{"
        "a,\\hbox{x}] b}\\showbox0 \\message{[math]}\\setbo"
        "x0=\\hbox{a,$x$] b}\\showbox0 \\message{[disc]}\\s"
        "etbox0=\\hbox{a,\\discretionary{}{}{}] b}\\showbox"
        "0 \\message{[none]}\\setbox0=\\hbox{a,] b}\\showbo"
        "x0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[rule]\n> \\box0=\n\\hbox(7.5+2.5)x21.44449"
        "\n.\\tenrm a\n.\\tenrm ,\n.\\rule(*+*)x2.0\n.\\ten"
        "rm ]\n.\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        ".\\tenrm b\n\n! OK.\nl.1 ...\\hbox{a,\\vrule width"
        "2pt\\relax] b}\\showbox0 \n                       "
        "                           \\message{[kern]}\\setb"
        "ox0=\\...\n\n\n[kern]\n> \\box0=\n\\hbox(7.5+2.5)x"
        "21.44449\n.\\tenrm a\n.\\tenrm ,\n.\\kern 2.0\n.\\"
        "tenrm ]\n.\\glue 3.33333 plus 2.08331 minus 0.8888"
        "9\n.\\tenrm b\n\n! OK.\nl.1 ...etbox0=\\hbox{a,\\k"
        "ern2pt\\relax] b}\\showbox0 \n                    "
        "                              \\message{[box]}\\se"
        "tbox0=\\h...\n\n\n[box]\n> \\box0=\n\\hbox(7.5+2.5"
        ")x24.72229\n.\\tenrm a\n.\\tenrm ,\n.\\hbox(4.3055"
        "4+0.0)x5.2778\n..\\tenrm x\n.\\tenrm ]\n.\\glue 3."
        "33333 plus 1.66666 minus 1.11111\n.\\tenrm b\n\n! "
        "OK.\nl.1 ...ox]}\\setbox0=\\hbox{a,\\hbox{x}] b}\\"
        "showbox0 \n                                       "
        "           \\message{[math]}\\setbox0=\\...\n\n\n["
        "math]\n> \\box0=\n\\hbox(7.5+2.5)x25.15976\n.\\ten"
        "rm a\n.\\tenrm ,\n.\\mathon\n.\\teni x\n.\\mathoff"
        "\n.\\tenrm ]\n.\\glue 3.33333 plus 1.66666 minus 1"
        ".11111\n.\\tenrm b\n\n! OK.\nl.1 ...e{[math]}\\set"
        "box0=\\hbox{a,$x$] b}\\showbox0 \n                "
        "                                  \\message{[disc]"
        "}\\setbox0=\\...\n\n\n[disc]\n> \\box0=\n\\hbox(7."
        "5+2.5)x19.44449\n.\\tenrm a\n.\\tenrm ,\n.\\discre"
        "tionary\n.\\tenrm ]\n.\\glue 3.33333 plus 2.08331 "
        "minus 0.88889\n.\\tenrm b\n\n! OK.\nl.1 ...=\\hbox"
        "{a,\\discretionary{}{}{}] b}\\showbox0 \n         "
        "                                         \\message"
        "{[none]}\\setbox0=\\...\n\n\n[none]\n> \\box0=\n\\"
        "hbox(7.5+2.5)x19.44449\n.\\tenrm a\n.\\tenrm ,\n."
        "\\tenrm ]\n.\\glue 3.33333 plus 2.08331 minus 0.88"
        "889\n.\\tenrm b\n\n! OK.\nl.1 ...sage{[none]}\\set"
        "box0=\\hbox{a,] b}\\showbox0 \n                   "
        "                               \\showbox254\n\n> "
        "\\box254=void\n\n! OK.\nl.1 ...}\\setbox0=\\hbox{a"
        ",] b}\\showbox0 \\showbox254\n                    "
        "                              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A character with a space factor code of zero leaves the factor alone, and
   it is the character as read that counts, not the ligature it joined; see
   docs/DECISIONS.md, the-space-factor-of-a-ligature. */
static int test_the_space_factor_of_a_ligature(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\spaceskip=0pt \\sfcod"
        "e`\\.=3000 \\sfcode`\\'=0 \\sfcode`\\)=0 \\message"
        "{[a]}\\setbox0=\\hbox{a. b}\\showbox0 \\message{[b"
        "]}\\setbox0=\\hbox{a.'' b}\\showbox0 \\message{[c]"
        "}\\setbox0=\\hbox{a.) b}\\showbox0 \\message{[d]}"
        "\\setbox0=\\hbox{a.x b}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[a]\n> \\box0=\n\\hbox(6.94444+0.0)x17.77782"
        "\n.\\tenrm a\n.\\tenrm .\n.\\glue 4.44444 plus 4.9"
        "9997 minus 0.37036\n.\\tenrm b\n\n! OK.\nl.1 ...\\"
        "message{[a]}\\setbox0=\\hbox{a. b}\\showbox0 \n   "
        "                                               \\m"
        "essage{[b]}\\setbox0=\\hbo...\n\n\n[b]\n> \\box0="
        "\n\\hbox(6.94444+0.0)x22.77783\n.\\tenrm a\n.\\ten"
        "rm .\n.\\tenrm \" (ligature '')\n.\\glue 4.44444 p"
        "lus 4.99997 minus 0.37036\n.\\tenrm b\n\n! OK.\nl."
        "1 ...essage{[b]}\\setbox0=\\hbox{a.'' b}\\showbox0"
        " \n                                               "
        "   \\message{[c]}\\setbox0=\\hbo...\n\n\n[c]\n> \\"
        "box0=\n\\hbox(7.5+2.5)x21.66672\n.\\tenrm a\n.\\te"
        "nrm .\n.\\tenrm )\n.\\glue 4.44444 plus 4.99997 mi"
        "nus 0.37036\n.\\tenrm b\n\n! OK.\nl.1 ...message{["
        "c]}\\setbox0=\\hbox{a.) b}\\showbox0 \n           "
        "                                       \\message{["
        "d]}\\setbox0=\\hbo...\n\n\n[d]\n> \\box0=\n\\hbox("
        "6.94444+0.0)x21.9445\n.\\tenrm a\n.\\tenrm .\n.\\t"
        "enrm x\n.\\glue 3.33333 plus 1.66666 minus 1.11111"
        "\n.\\tenrm b\n\n! OK.\nl.1 ...message{[d]}\\setbox"
        "0=\\hbox{a.x b}\\showbox0 \n                      "
        "                            \\showbox254\n\n> \\bo"
        "x254=void\n\n! OK.\nl.1 ...}\\setbox0=\\hbox{a.x b"
        "}\\showbox0 \\showbox254\n                        "
        "                          \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* An operator whose nucleus is a list stands on the baseline, but it still
   carries its limits above and below in display style; see
   docs/DECISIONS.md, large-operators. */
static int test_operators_that_are_lists(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\message{[log]}\\setbo"
        "x0=\\hbox{$\\displaystyle\\mathop{\\tenrm log}$}\\"
        "showbox0 \\message{[logs]}\\setbox0=\\hbox{$\\disp"
        "laystyle\\mathop{\\tenrm log}^a_b$}\\showbox0 \\me"
        "ssage{[mx]}\\setbox0=\\hbox{$\\displaystyle\\matho"
        "p{x}^a$}\\showbox0 \\message{[hb]}\\setbox0=\\hbox"
        "{$\\mathop{\\hbox{\\tenrm x}}$}\\showbox0 \\messag"
        "e{[hbd]}\\setbox0=\\hbox{$\\displaystyle\\mathop{"
        "\\hbox{\\tenrm x}}_c$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[log]\n> \\box0=\n\\hbox(6.94444+1.94444)x13"
        ".15627\n.\\mathon\n.\\vbox(6.94444+1.94444)x13.156"
        "27\n..\\hbox(6.94444+1.94444)x13.15627\n...\\teni "
        "l\n...\\kern0.19678\n...\\teni o\n...\\teni g\n..."
        "\\kern0.35878\n.\\mathoff\n\n! OK.\nl.1 ...display"
        "style\\mathop{\\tenrm log}$}\\showbox0 \n         "
        "                                         \\message"
        "{[logs]}\\setbox0=\\...\n\n\n[logs]\n> \\box0=\n\\"
        "hbox(12.95831+9.4722)x13.15627\n.\\mathon\n.\\vbox"
        "(12.95831+9.4722)x13.15627\n..\\kern1.0\n..\\hbox("
        "3.01389+0.0)x13.15627, glue set 4.40932fil\n...\\g"
        "lue 0.0 plus 1.0fil minus 1.0fil\n...\\seveni a\n."
        "..\\glue 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.9"
        "9998\n..\\hbox(6.94444+1.94444)x13.15627\n...\\ten"
        "i l\n...\\kern0.19678\n...\\teni o\n...\\teni g\n."
        "..\\kern0.35878\n..\\kern1.66666\n..\\hbox(4.8611+"
        "0.0)x13.15627, glue set 4.81981fil\n...\\glue 0.0 "
        "plus 1.0fil minus 1.0fil\n...\\seveni b\n...\\glue"
        " 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\mat"
        "hoff\n\n! OK.\nl.1 ...laystyle\\mathop{\\tenrm log"
        "}^a_b$}\\showbox0 \n                              "
        "                    \\message{[mx]}\\setbox0=\\hb."
        "..\n\n\n[mx]\n> \\box0=\n\\hbox(10.66664+0.0)x5.71"
        "527\n.\\mathon\n.\\vbox(10.66664+0.0)x5.71527\n.."
        "\\kern1.0\n..\\hbox(3.01389+0.0)x5.71527, glue set"
        " 0.68881fil\n...\\glue 0.0 plus 1.0fil minus 1.0fi"
        "l\n...\\seveni a\n...\\glue 0.0 plus 1.0fil minus "
        "1.0fil\n..\\kern1.99998\n..\\hbox(4.65277+0.0)x5.7"
        "1527\n...\\hbox(4.30554+0.0)x5.71527, shifted -0.3"
        "4723\n....\\teni x\n.\\mathoff\n\n! OK.\nl.1 ...hb"
        "ox{$\\displaystyle\\mathop{x}^a$}\\showbox0 \n    "
        "                                              \\me"
        "ssage{[hb]}\\setbox0=\\hb...\n\n\n[hb]\n> \\box0="
        "\n\\hbox(4.30554+0.0)x5.2778\n.\\mathon\n.\\hbox(4"
        ".30554+0.0)x5.2778\n..\\tenrm x\n.\\mathoff\n\n! O"
        "K.\nl.1 ...\\hbox{$\\mathop{\\hbox{\\tenrm x}}$}\\"
        "showbox0 \n                                       "
        "           \\message{[hbd]}\\setbox0=\\h...\n\n\n["
        "hbd]\n> \\box0=\n\\hbox(4.30554+7.0)x5.2778\n.\\ma"
        "thon\n.\\vbox(4.30554+7.0)x5.2778\n..\\hbox(4.3055"
        "4+0.0)x5.2778\n...\\tenrm x\n..\\kern2.98611\n..\\"
        "hbox(3.01389+0.0)x5.2778, glue set 0.85204fil\n..."
        "\\glue 0.0 plus 1.0fil minus 1.0fil\n...\\seveni c"
        "\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n..\\kern"
        "1.0\n.\\mathoff\n\n! OK.\nl.1 ...style\\mathop{\\h"
        "box{\\tenrm x}}_c$}\\showbox0 \n                  "
        "                                \\showbox254\n\n> "
        "\\box254=void\n\n! OK.\nl.1 ...p{\\hbox{\\tenrm x}"
        "}_c$}\\showbox0 \\showbox254\n                    "
        "                              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The break that ends a paragraph is measured without the kern that lets
   its last character stick out, though the line is set with one; see
   docs/DECISIONS.md, the-last-line-is-measured-square. */
static int test_the_last_line_is_measured_square(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\pdfprotrudechars=2 \\"
        "rpcode\\tenrm`\\.=1000 \\hsize=38pt \\parindent=0p"
        "t \\spaceskip=4pt plus 2pt minus 1pt \\pretoleranc"
        "e=-1 \\tolerance=200 \\hbadness=10000 \\vbadness=1"
        "0000 \\hfuzz=1000pt \\vfuzz=1000pt \\baselineskip="
        "12pt \\lineskip=0pt \\lineskiplimit=0pt \\parfills"
        "kip=0pt plus 1fil \\leftskip=0pt \\rightskip=0pt "
        "\\tracingonline=1 \\tracingparagraphs=1 \\setbox0="
        "\\vbox{\\noindent xx xx xx.\\par} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n\\tenrm xx xx xx. \n@\\par via @@0 b=* p=-10"
        "000 d=*\n@@1: line 1.3- t=0 -> @@0\n\n> \\box254=v"
        "oid\n\n! OK.\nl.1 ...=\\vbox{\\noindent xx xx xx."
        "\\par} \\showbox254\n                             "
        "                     \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A display closes its own group only once it has joined the vertical
   list, so the glue in front of it is the glue the display asked for; see
   docs/DECISIONS.md, a-display-closes-its-group-last. */
static int test_a_display_closes_its_group_last(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\hsize"
        "=100pt \\parindent=0pt \\baselineskip=0pt \\linesk"
        "ip=1pt \\lineskiplimit=0pt \\parfillskip=0pt plus1"
        "fil \\leftskip=0pt \\rightskip=0pt \\predisplaypen"
        "alty=10000 \\postdisplaypenalty=0 \\abovedisplaysk"
        "ip=3pt \\belowdisplayskip=4pt \\abovedisplayshorts"
        "kip=1pt \\belowdisplayshortskip=2pt \\tolerance=10"
        "000 \\pretolerance=-1 \\hbadness=10000 \\vbadness="
        "10000 \\hfuzz=1000pt \\vfuzz=1000pt \\showboxdepth"
        "=1 \\showboxbreadth=100 \\message{[out]}\\setbox0="
        "\\vbox{\\noindent y$$\\hbox{}$$z\\par}\\showbox0 "
        "\\message{[in]}\\setbox0=\\vbox{\\noindent y$$\\li"
        "neskip=9pt \\hbox{}$$z\\par}\\showbox0 \\showbox25"
        "4 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[out]\n> \\box0=\n\\vbox(15.55553+0.0)x100.0"
        "\n.\\hbox(4.30554+1.94444)x100.0, glue set 94.7222"
        "fil []\n.\\penalty 10000\n.\\glue(\\abovedisplaysh"
        "ortskip) 1.0\n.\\glue(\\lineskip) 1.0\n.\\hbox(0.0"
        "+0.0)x0.0, shifted 50.0, display []\n.\\penalty 0"
        "\n.\\glue(\\belowdisplayshortskip) 2.0\n.\\glue(\\"
        "lineskip) 1.0\n.\\hbox(4.30554+0.0)x100.0, glue se"
        "t 95.55556fil []\n\n! OK.\nl.1 ...vbox{\\noindent "
        "y$$\\hbox{}$$z\\par}\\showbox0 \n                 "
        "                                 \\message{[in]}\\"
        "setbox0=\\vb...\n\n\n[in]\n> \\box0=\n\\vbox(23.55"
        "553+0.0)x100.0\n.\\hbox(4.30554+1.94444)x100.0, gl"
        "ue set 94.7222fil []\n.\\penalty 10000\n.\\glue(\\"
        "abovedisplayshortskip) 1.0\n.\\glue(\\lineskip) 9."
        "0\n.\\hbox(0.0+0.0)x0.0, shifted 50.0, display []"
        "\n.\\penalty 0\n.\\glue(\\belowdisplayshortskip) 2"
        ".0\n.\\glue(\\lineskip) 1.0\n.\\hbox(4.30554+0.0)x"
        "100.0, glue set 95.55556fil []\n\n! OK.\nl.1 ... y"
        "$$\\lineskip=9pt \\hbox{}$$z\\par}\\showbox0 \n   "
        "                                               \\s"
        "howbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...ip=9"
        "pt \\hbox{}$$z\\par}\\showbox0 \\showbox254\n     "
        "                                             \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A display counts as three lines of the paragraph it interrupts, so what
   follows takes the shape of a later line; see docs/DECISIONS.md,
   lines-carry-on-past-a-display. */
static int test_lines_carry_on_past_a_display(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\hsize"
        "=100pt \\parindent=0pt \\baselineskip=0pt \\linesk"
        "ip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1"
        "fil \\leftskip=0pt \\rightskip=0pt \\predisplaypen"
        "alty=10000 \\postdisplaypenalty=0 \\abovedisplaysk"
        "ip=3pt \\belowdisplayskip=4pt \\abovedisplayshorts"
        "kip=1pt \\belowdisplayshortskip=2pt \\tolerance=10"
        "000 \\pretolerance=-1 \\hbadness=10000 \\vbadness="
        "10000 \\hfuzz=1000pt \\vfuzz=1000pt \\showboxdepth"
        "=1 \\showboxbreadth=100 \\def\\W{\\vrule width20pt"
        " height1pt depth0pt\\hskip0pt plus1fil}\\message{["
        "shape]}\\setbox0=\\vbox{\\parshape=6 0pt 90pt 0pt "
        "80pt 0pt 70pt 0pt 60pt 0pt 50pt 0pt 40pt \\noinden"
        "t\\W\\W$$\\hbox{}$$\\W\\W\\par}\\showbox0 \\showbo"
        "x254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[shape]\n> \\box0=\n\\vbox(9.0+0.0)x90.0\n."
        "\\hbox(1.0+0.0)x90.0, glue set 25.0fil []\n.\\pena"
        "lty 10000\n.\\glue(\\abovedisplayskip) 3.0\n.\\glu"
        "e(\\baselineskip) 0.0\n.\\hbox(0.0+0.0)x0.0, shift"
        "ed 35.0, display []\n.\\penalty 0\n.\\glue(\\below"
        "displayskip) 4.0\n.\\glue(\\lineskip) 0.0\n.\\hbox"
        "(1.0+0.0)x50.0, glue set 5.0fil []\n\n! OK.\nl.1 ."
        "..\\noindent\\W\\W$$\\hbox{}$$\\W\\W\\par}\\showbo"
        "x0 \n                                             "
        "     \\showbox254\n\n> \\box254=void\n\n! OK.\nl.1"
        " ...\\W$$\\hbox{}$$\\W\\W\\par}\\showbox0 \\showbo"
        "x254\n                                            "
        "      \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \prevdepth inside a \noalign is the depth of the row before it, and
   what the \noalign leaves it at is what the row after it is spaced
   from; see docs/DECISIONS.md, prevdepth-inside-noalign. */
static int test_prevdepth_inside_noalign(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\#=6 \\catco"
        "de`\\&=4 \\baselineskip=12pt \\lineskip=1pt \\line"
        "skiplimit=0pt \\showboxdepth=2 \\showboxbreadth=10"
        "0 \\def\\R#1#2{\\vrule height#1 depth#2 width3pt}"
        "\\setbox0=\\vbox{\\halign{#\\cr\\R{5pt}{6pt}\\cr\\"
        "noalign{\\message{[read pd=\\the\\prevdepth]}}\\R{"
        "5pt}{1pt}\\cr}}\\showbox0 \\message{[set]}\\setbox"
        "0=\\vbox{\\halign{#\\cr\\R{5pt}{1pt}\\cr\\noalign{"
        "\\prevdepth=20pt}\\R{5pt}{1pt}\\cr}}\\showbox0 \\m"
        "essage{[keep]}\\setbox0=\\vbox{\\halign{#\\cr\\R{5"
        "pt}{6pt}\\cr\\noalign{\\dimen0=\\prevdepth \\prevd"
        "epth=-1000pt \\hbox{}\\prevdepth=\\dimen0 }\\R{5pt"
        "}{1pt}\\cr}\\hbox{}}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[read pd=6.0pt]\n> \\box0=\n\\vbox(17.0+1.0)"
        "x3.0\n.\\hbox(5.0+6.0)x3.0\n..\\glue(\\tabskip) 0."
        "0\n..\\hbox(5.0+6.0)x3.0 []\n..\\glue(\\tabskip) 0"
        ".0\n.\\glue(\\baselineskip) 1.0\n.\\hbox(5.0+1.0)x"
        "3.0\n..\\glue(\\tabskip) 0.0\n..\\hbox(5.0+1.0)x3."
        "0 []\n..\\glue(\\tabskip) 0.0\n\n! OK.\nl.1 ...the"
        "\\prevdepth]}}\\R{5pt}{1pt}\\cr}}\\showbox0 \n    "
        "                                              \\me"
        "ssage{[set]}\\setbox0=\\v...\n\n\n[set]\n> \\box0="
        "\n\\vbox(12.0+1.0)x3.0\n.\\hbox(5.0+1.0)x3.0\n..\\"
        "glue(\\tabskip) 0.0\n..\\hbox(5.0+1.0)x3.0 []\n.."
        "\\glue(\\tabskip) 0.0\n.\\glue(\\lineskip) 1.0\n."
        "\\hbox(5.0+1.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\"
        "hbox(5.0+1.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n\n!"
        " OK.\nl.1 ...\\prevdepth=20pt}\\R{5pt}{1pt}\\cr}}"
        "\\showbox0 \n                                     "
        "             \\message{[keep]}\\setbox0=\\...\n\n"
        "\n[keep]\n> \\box0=\n\\vbox(29.0+0.0)x3.0\n.\\hbox"
        "(5.0+6.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\hbox(5"
        ".0+6.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n.\\hbox(0"
        ".0+0.0)x0.0\n.\\glue(\\baselineskip) 1.0\n.\\hbox("
        "5.0+1.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\hbox(5."
        "0+1.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n.\\glue(\\"
        "baselineskip) 11.0\n.\\hbox(0.0+0.0)x0.0\n\n! OK."
        "\nl.1 ...\\dimen0 }\\R{5pt}{1pt}\\cr}\\hbox{}}\\sh"
        "owbox0 \n                                         "
        "         \\showbox254\n\n> \\box254=void\n\n! OK."
        "\nl.1 ...{5pt}{1pt}\\cr}\\hbox{}}\\showbox0 \\show"
        "box254\n                                          "
        "        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* An equation shrinks to make room for its number when it can, and the
   number goes below only when it cannot; see docs/DECISIONS.md,
   a-number-beside-a-squeezed-equation. */
static int test_a_number_beside_a_squeezed_equation(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\hsize=100pt \\parinde"
        "nt=0pt \\baselineskip=0pt \\lineskip=0pt \\lineski"
        "plimit=0pt \\parfillskip=0pt \\leftskip=0pt \\righ"
        "tskip=0pt \\abovedisplayskip=3pt \\abovedisplaysho"
        "rtskip=1pt \\belowdisplayskip=4pt \\belowdisplaysh"
        "ortskip=2pt \\predisplaypenalty=10000 \\postdispla"
        "ypenalty=0 \\tolerance=10000 \\pretolerance=-1 \\h"
        "badness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\showboxdepth=2 \\showboxbreadth=100 "
        "\\message{[room]}\\setbox0=\\vbox{\\noindent$$\\hb"
        "ox to 60pt{}\\hskip20pt minus45pt\\hbox to 40pt{}"
        "\\eqno\\hbox to 10pt{}$$}\\showbox0 \\message{[tig"
        "ht]}\\setbox0=\\vbox{\\noindent$$\\hbox to 60pt{}"
        "\\hskip20pt minus2pt\\hbox to 40pt{}\\eqno\\hbox t"
        "o 10pt{}$$}\\showbox0 \\message{[fil]}\\setbox0=\\"
        "vbox{\\noindent$$\\hbox to 60pt{}\\hskip20pt minus"
        "1fil\\hbox to 40pt{}\\eqno\\hbox to 10pt{}$$}\\sho"
        "wbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[room]\n> \\box0=\n\\vbox(3.0+0.0)x100.0\n."
        "\\penalty 10000\n.\\glue(\\abovedisplayshortskip) "
        "1.0\n.\\hbox(0.0+0.0)x94.99998, shifted 5.00002\n."
        ".\\hbox(0.0+0.0)x79.99998, glue set - 0.88889, dis"
        "play []\n..\\kern5.0\n..\\hbox(0.0+0.0)x10.0, disp"
        "lay []\n.\\penalty 0\n.\\glue(\\belowdisplayshorts"
        "kip) 2.0\n\n! OK.\nl.1 ... to 40pt{}\\eqno\\hbox t"
        "o 10pt{}$$}\\showbox0 \n                          "
        "                        \\message{[tight]}\\setbox"
        "0=...\n\n\n[tight]\n> \\box0=\n\\vbox(1.0+0.0)x100"
        ".0\n.\\penalty 10000\n.\\glue(\\abovedisplayshorts"
        "kip) 1.0\n.\\hbox(0.0+0.0)x100.0, glue set - 1.0, "
        "display\n..\\hbox(0.0+0.0)x60.0\n..\\glue 20.0 min"
        "us 2.0\n..\\hbox(0.0+0.0)x40.0\n.\\penalty 10000\n"
        ".\\glue(\\baselineskip) 0.0\n.\\hbox(0.0+0.0)x10.0"
        ", shifted 90.0, display\n..\\hbox(0.0+0.0)x10.0\n."
        "\\penalty 0\n\n! OK.\nl.1 ... to 40pt{}\\eqno\\hbo"
        "x to 10pt{}$$}\\showbox0 \n                       "
        "                           \\message{[fil]}\\setbo"
        "x0=\\v...\n\n\n[fil]\n> \\box0=\n\\vbox(3.0+0.0)x1"
        "00.0\n.\\penalty 10000\n.\\glue(\\abovedisplayshor"
        "tskip) 1.0\n.\\hbox(0.0+0.0)x94.99998, shifted 5.0"
        "0002\n..\\hbox(0.0+0.0)x79.99998, glue set - 40.00"
        "002fil, display []\n..\\kern5.0\n..\\hbox(0.0+0.0)"
        "x10.0, display []\n.\\penalty 0\n.\\glue(\\belowdi"
        "splayshortskip) 2.0\n\n! OK.\nl.1 ... to 40pt{}\\e"
        "qno\\hbox to 10pt{}$$}\\showbox0 \n               "
        "                                   \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...eqno\\hbox to 1"
        "0pt{}$$}\\showbox0 \\showbox254\n                 "
        "                                 \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A ligature made of two characters is read as part of a word only if
   another character follows it, so it keeps its own italic correction when
   it ends the run; see docs/DECISIONS.md, math-text-characters. */
static int test_the_italic_of_a_math_ligature(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\showb"
        "oxdepth=4 \\showboxbreadth=100 \\message{[lig]}\\s"
        "etbox0=\\hbox{$\\fam0 diff$}\\showbox0 \\message{["
        "plain]}\\setbox0=\\hbox{$\\fam0 did$}\\showbox0 \\"
        "message{[one]}\\setbox0=\\hbox{$\\fam0 f$}\\showbo"
        "x0 \\message{[two]}\\setbox0=\\hbox{$\\fam0 ff$}\\"
        "showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[lig]\n> \\box0=\n\\hbox(6.94444+0.0)x14.944"
        "5\n.\\mathon\n.\\tenrm d\n.\\tenrm i\n.\\tenrm \v"
        "\n.\\kern0.77779\n.\\mathoff\n\n! OK.\nl.1 ...lig]"
        "}\\setbox0=\\hbox{$\\fam0 diff$}\\showbox0 \n     "
        "                                             \\mes"
        "sage{[plain]}\\setbox0=...\n\n\n[plain]\n> \\box0="
        "\n\\hbox(6.94444+0.0)x13.88893\n.\\mathon\n.\\tenr"
        "m d\n.\\tenrm i\n.\\tenrm d\n.\\mathoff\n\n! OK.\n"
        "l.1 ...lain]}\\setbox0=\\hbox{$\\fam0 did$}\\showb"
        "ox0 \n                                            "
        "      \\message{[one]}\\setbox0=\\h...\n\n\n[one]"
        "\n> \\box0=\n\\hbox(6.94444+0.0)x3.83336\n.\\matho"
        "n\n.\\tenrm f\n.\\kern0.77779\n.\\mathoff\n\n! OK."
        "\nl.1 ...e{[one]}\\setbox0=\\hbox{$\\fam0 f$}\\sho"
        "wbox0 \n                                          "
        "        \\message{[two]}\\setbox0=\\h...\n\n\n[two"
        "]\n> \\box0=\n\\hbox(6.94444+0.0)x6.61115\n.\\math"
        "on\n.\\tenrm \v\n.\\kern0.77779\n.\\mathoff\n\n! "
        "OK.\nl.1 ...{[two]}\\setbox0=\\hbox{$\\fam0 ff$}\\"
        "showbox0 \n                                       "
        "           \\showbox254\n\n> \\box254=void\n\n! OK"
        ".\nl.1 ...box0=\\hbox{$\\fam0 ff$}\\showbox0 \\sho"
        "wbox254\n                                         "
        "         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Only an operator whose nucleus is a character of its own is centred on the
   axis; one whose nucleus came out a box stands on the baseline; see
   docs/DECISIONS.md, only-a-character-is-centred. */
static int test_only_a_character_is_centred_still(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\catco"
        "de`\\^=7 \\catcode`\\_=8 \\catcode`\\\"=12 \\showb"
        "oxdepth=6 \\showboxbreadth=100 \\mathcode`\\==\"30"
        "3D \\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip="
        "5mu \\message{[boxed]}\\setbox0=\\hbox{$\\mathop{="
        "}\\limits^?$}\\showbox0 \\message{[char]}\\setbox0"
        "=\\hbox{$\\mathop{x}\\limits^?$}\\showbox0 \\messa"
        "ge{[list]}\\setbox0=\\hbox{$\\mathop{xy}\\limits^?"
        "$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[boxed]\n> \\box0=\n\\hbox(11.52983+0.0)x7.7"
        "778\n.\\mathon\n.\\vbox(11.52983+0.0)x7.7778\n..\\"
        "kern1.0\n..\\hbox(4.8611+0.0)x7.7778, glue set 2.0"
        "0348fil\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n."
        "..\\sevenrm ?\n...\\glue 0.0 plus 1.0fil minus 1.0"
        "fil\n..\\kern1.99998\n..\\hbox(3.66875+0.0)x7.7778"
        "\n...\\tenrm =\n.\\mathoff\n\n! OK.\nl.1 ...box0="
        "\\hbox{$\\mathop{=}\\limits^?$}\\showbox0 \n      "
        "                                            \\mess"
        "age{[char]}\\setbox0=\\...\n\n\n[char]\n> \\box0="
        "\n\\hbox(12.51385+0.0)x5.71527\n.\\mathon\n.\\vbox"
        "(12.51385+0.0)x5.71527\n..\\kern1.0\n..\\hbox(4.86"
        "11+0.0)x5.71527, glue set 0.97221fil\n...\\glue 0."
        "0 plus 1.0fil minus 1.0fil\n...\\sevenrm ?\n...\\g"
        "lue 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.99998"
        "\n..\\hbox(4.65277+0.0)x5.71527\n...\\hbox(4.30554"
        "+0.0)x5.71527, shifted -0.34723\n....\\teni x\n.\\"
        "mathoff\n\n! OK.\nl.1 ...box0=\\hbox{$\\mathop{x}"
        "\\limits^?$}\\showbox0 \n                         "
        "                         \\message{[list]}\\setbox"
        "0=\\...\n\n\n[list]\n> \\box0=\n\\hbox(12.16663+1."
        "94444)x10.97687\n.\\mathon\n.\\vbox(12.16663+1.944"
        "44)x10.97687\n..\\kern1.0\n..\\hbox(4.8611+0.0)x10"
        ".97687, glue set 3.60301fil\n...\\glue 0.0 plus 1."
        "0fil minus 1.0fil\n...\\sevenrm ?\n...\\glue 0.0 p"
        "lus 1.0fil minus 1.0fil\n..\\kern1.99998\n..\\hbox"
        "(4.30554+1.94444)x10.97687\n...\\teni x\n...\\teni"
        " y\n...\\kern0.35878\n.\\mathoff\n\n! OK.\nl.1 ..."
        "ox0=\\hbox{$\\mathop{xy}\\limits^?$}\\showbox0 \n "
        "                                                 "
        "\\showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ..."
        "\\mathop{xy}\\limits^?$}\\showbox0 \\showbox254\n "
        "                                                 "
        "\n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A box register used in a formula is an ordinary atom holding that box, so
   braces round it change nothing; see docs/DECISIONS.md,
   a-box-register-in-a-formula. */
static int test_a_box_register_in_a_formula(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\catco"
        "de`\\_=8 \\catcode`\\^=7 \\showboxdepth=6 \\showbo"
        "xbreadth=100 \\message{[grp]}\\setbox1=\\hbox{y}\\"
        "setbox0=\\hbox{$\\copy1{\\box1}$}\\showbox0 \\mess"
        "age{[bare]}\\setbox1=\\hbox{y}\\setbox0=\\hbox{$\\"
        "box1$}\\showbox0 \\message{[sub]}\\setbox1=\\hbox{"
        "y}\\setbox0=\\hbox{$\\box1_a$}\\showbox0 \\message"
        "{[void]}\\setbox0=\\hbox{$\\box9 x$}\\showbox0 \\s"
        "howbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[grp]\n> \\box0=\n\\hbox(4.30554+1.94444)x10"
        ".5556\n.\\mathon\n.\\hbox(4.30554+1.94444)x5.2778"
        "\n..\\tenrm y\n.\\hbox(4.30554+1.94444)x5.2778\n.."
        "\\tenrm y\n.\\mathoff\n\n! OK.\nl.1 ...y}\\setbox0"
        "=\\hbox{$\\copy1{\\box1}$}\\showbox0 \n           "
        "                                       \\message{["
        "bare]}\\setbox1=\\...\n\n\n[bare]\n> \\box0=\n\\hb"
        "ox(4.30554+1.94444)x5.2778\n.\\mathon\n.\\hbox(4.3"
        "0554+1.94444)x5.2778\n..\\tenrm y\n.\\mathoff\n\n!"
        " OK.\nl.1 ...1=\\hbox{y}\\setbox0=\\hbox{$\\box1$}"
        "\\showbox0 \n                                     "
        "             \\message{[sub]}\\setbox1=\\h...\n\n"
        "\n[sub]\n> \\box0=\n\\hbox(4.30554+2.44443)x9.6154"
        "5\n.\\mathon\n.\\hbox(4.30554+1.94444)x5.2778\n.."
        "\\tenrm y\n.\\hbox(3.01389+0.0)x4.33765, shifted 2"
        ".44443\n..\\seveni a\n.\\mathoff\n\n! OK.\nl.1 ..."
        "\\hbox{y}\\setbox0=\\hbox{$\\box1_a$}\\showbox0 \n"
        "                                                  "
        "\\message{[void]}\\setbox0=\\...\n\n\n[void]\n> \\"
        "box0=\n\\hbox(4.30554+0.0)x5.71527\n.\\mathon\n.\\"
        "teni x\n.\\mathoff\n\n! OK.\nl.1 ...{[void]}\\setb"
        "ox0=\\hbox{$\\box9 x$}\\showbox0 \n               "
        "                                   \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...tbox0=\\hbox{$"
        "\\box9 x$}\\showbox0 \\showbox254\n               "
        "                                   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A \mark leaves a node holding the text it was given, expanded once as
   \edef expands one. It stays where it stands in a vertical list and in an
   \hbox, and moves out of a paragraph line to the vertical list behind it.
   See docs/DECISIONS.md, marks. */
static int test_a_mark_in_a_list(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=4 \\show"
        "boxbreadth=100 \\def\\x{Q\\y}\\def\\y{R}\\message{"
        "[v]}\\setbox0=\\vbox{\\hrule\\mark{one}\\hrule\\ma"
        "rk{two}}\\showbox0 \\message{[h]}\\setbox0=\\vbox{"
        "\\noindent aa\\mark{three}bb\\par}\\showbox0 \\mes"
        "sage{[i]}\\setbox0=\\hbox{aa\\mark{four}bb}\\showb"
        "ox0 \\message{[e]}\\setbox0=\\vbox{\\hrule\\mark{"
        "\\x A}}\\showbox0 \\message{[p]}\\setbox0=\\vbox{"
        "\\hrule\\mark{}}\\showbox0 \\message{[n]}\\setbox0"
        "=\\vbox{\\hrule\\marks2{cls}\\xdef\\t{\\the\\lastn"
        "odetype}}\\showbox0 \\message{[type]}\\setbox0=\\h"
        "box{\\t}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[v]\n> \\box0=\n\\vbox(0.79999+0.0)x0.0\n.\\"
        "rule(0.4+0.0)x*\n.\\mark{one}\n.\\rule(0.4+0.0)x*"
        "\n.\\mark{two}\n\n! OK.\nl.1 ...\\hrule\\mark{one}"
        "\\hrule\\mark{two}}\\showbox0 \n                  "
        "                                \\message{[h]}\\se"
        "tbox0=\\vbo...\n\n\n[h]\n> \\box0=\n\\vbox(6.94444"
        "+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0, glue set "
        "178.88882fil\n..\\tenrm a\n..\\tenrm a\n..\\tenrm "
        "b\n..\\tenrm b\n..\\penalty 10000\n..\\glue(\\parf"
        "illskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0."
        "0\n.\\mark{three}\n\n! OK.\nl.1 ...x{\\noindent aa"
        "\\mark{three}bb\\par}\\showbox0 \n                "
        "                                  \\message{[i]}\\"
        "setbox0=\\hbo...\n\n\n[i]\n> \\box0=\n\\hbox(6.944"
        "44+0.0)x21.11118\n.\\tenrm a\n.\\tenrm a\n.\\mark{"
        "four}\n.\\tenrm b\n.\\tenrm b\n\n! OK.\nl.1 ...]}"
        "\\setbox0=\\hbox{aa\\mark{four}bb}\\showbox0 \n   "
        "                                               \\m"
        "essage{[e]}\\setbox0=\\vbo...\n\n\n[e]\n> \\box0="
        "\n\\vbox(0.4+0.0)x0.0\n.\\rule(0.4+0.0)x*\n.\\mark"
        "{QRA}\n\n! OK.\nl.1 ...\\setbox0=\\vbox{\\hrule\\m"
        "ark{\\x A}}\\showbox0 \n                          "
        "                        \\message{[p]}\\setbox0=\\"
        "vbo...\n\n\n[p]\n> \\box0=\n\\vbox(0.4+0.0)x0.0\n."
        "\\rule(0.4+0.0)x*\n.\\mark{}\n\n! OK.\nl.1 ...[p]}"
        "\\setbox0=\\vbox{\\hrule\\mark{}}\\showbox0 \n    "
        "                                              \\me"
        "ssage{[n]}\\setbox0=\\vbo...\n\n\n[n]\n> \\box0=\n"
        "\\vbox(0.4+0.0)x0.0\n.\\rule(0.4+0.0)x*\n.\\marks2"
        "{cls}\n\n! OK.\nl.1 ...2{cls}\\xdef\\t{\\the\\last"
        "nodetype}}\\showbox0 \n                           "
        "                       \\message{[type]}\\setbox0="
        "\\...\n\n\n[type]\n> \\box0=\n\\hbox(6.44444+0.0)x"
        "5.00002\n.\\tenrm 5\n\n! OK.\nl.1 ...message{[type"
        "]}\\setbox0=\\hbox{\\t}\\showbox0 \n              "
        "                                    \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...pe]}\\setbox0="
        "\\hbox{\\t}\\showbox0 \\showbox254\n              "
        "                                    \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \vsplit takes the top of a vertical list at the best break for the
   height it was given, packs it to that height with \splitmaxdepth, and
   leaves the rest in the register with \splittopskip in front of its first
   box. The marks in the part that came away are what \splitfirstmark and
   \splitbotmark report. See docs/DECISIONS.md, what-a-split-leaves-behind. */
/* \leaders repeats its box on a grid measured from the edge of the list it
   is in, \cleaders centres the repetitions in the glue and \xleaders
   spreads what is left over between and around them; a rule fills the whole
   of the glue instead. See docs/DECISIONS.md, leaders-on-a-page. */
static int test_leaders_on_a_page(void)
{
    static const char *const source[] = {
        "\\pdfoutput=0 \\year=2026 \\month=8 \\day=18 \\tim"
        "e=1117 \\font\\tenrm=cmr10 \\tenrm \\hsize=200pt "
        "\\vsize=200pt \\parindent=0pt \\baselineskip=0pt "
        "\\lineskip=0pt \\boxmaxdepth=0pt \\hbadness=10000 "
        "\\vbadness=10000 \\setbox9=\\hbox to 9pt{.\\hss}\\"
        "shipout\\vbox{\\hbox{a\\leaders\\hbox to 10pt{.}\\"
        "hskip 55pt b}\\hbox{a\\cleaders\\copy9\\hskip 55pt"
        " b}\\hbox{a\\xleaders\\copy9\\hskip 55pt b}\\hbox{"
        "\\kern 12345sp \\hbox{\\leaders\\copy9\\hskip 40pt"
        "}}}\\shipout\\vbox{\\hbox{a\\leaders\\hrule height"
        " 2pt\\hskip 30pt b}\\hbox{a\\leaders\\vrule width "
        "3pt\\hskip 30pt b}\\leaders\\copy9\\vskip 3pt \\ke"
        "rn 5pt \\cleaders\\vbox to 12pt{\\hbox{.}\\vss}\\v"
        "skip 40pt \\xleaders\\vbox to 7pt{\\hbox{.}\\vss}"
        "\\vskip 33pt \\leaders\\hrule height 2pt\\vskip 12"
        "pt \\hbox{z}}",
        NULL,
    };
    static const char *const expected[] = {
        "f702018392c01c3b0000000003e81b20546558206f75747075"
        "7420323032362e30382e31383a313833378b00000000000000"
        "00000000000000000000000000000000000000000000000000"
        "0000000000000000ffffffffa406f1c78df3004bf16079000a"
        "0000000a00000005636d723130ab619104ffff8d2e8e960a00"
        "008d2e8e938d2e8e938d2e8e938d2e8e910a0001628ea18d61"
        "910080058d2e8e960900008d2e8e938d2e8e938d2e8e938d2e"
        "8e938d2e8e91097ffb628ea18d619024958d2e8e960924938d"
        "2e8e938d2e8e938d2e8e938d2e8e938d2e8e9109248c628e9f"
        "010e388d8d9030398d2e8e960900008d2e8e938d2e8e938d2e"
        "8e8e8e8c8b0000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000002aa4"
        "06f1c78dab618400020000001e0000628ea18d61840006f1c7"
        "001e0000628e9f01f1ba8d2e8e9f010e388d2e8e9f1300138d"
        "9ff50e388d2e8e8ea40c00008d9ff50e388d2e8e8ea18d9ff5"
        "0e388d2e8e8e9f09fffd8d9ffa0e388d2e8e8ea40800028d9f"
        "fa0e388d2e8e8ea18d9ffa0e388d2e8e8ea18d9ffa0e388d2e"
        "8e8e9f0cfff889000c000000288e3b9f044e388d7a8e8cf800"
        "0000fe018392c01c3b0000000003e8006f31c600418e3b0003"
        "0002f3004bf16079000a0000000a00000005636d723130f900"
        "0001c002dfdfdfdf",
        NULL,
    };
    return run_document_dvi(source, expected);
}

/* What may stand between the edge of a line and the character that sticks
   out past the margin: a box that offers nothing of its own is stepped over,
   as is a discretionary with nothing of its own, but a box that stops the
   search inside it stops it altogether. See docs/DECISIONS.md,
   a-box-at-the-edge. */
static int test_a_box_at_the_edge(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\lpcode\\tenrm`x=100 "
        "\\rpcode\\tenrm`x=100 \\lpcode\\tenrm`y=50 \\rpcod"
        "e\\tenrm`y=50 \\pdfprotrudechars=2 \\setbox0=\\vbo"
        "x{\\noindent ax\\hbox{\\special{s}}\\par}\\showbox"
        "0 \\setbox0=\\vbox{\\noindent ax\\hbox{y}\\par}\\s"
        "howbox0 \\setbox0=\\vbox{\\noindent ax\\hbox to 5p"
        "t{}\\par}\\showbox0 \\setbox0=\\vbox{\\noindent ax"
        "\\vbox{}\\par}\\showbox0 \\setbox0=\\vbox{\\noinde"
        "nt ax\\hbox{\\hskip0pt}\\par}\\showbox0 \\setbox0="
        "\\vbox{\\noindent \\hbox{\\special{s}}xa\\par}\\sh"
        "owbox0 \\setbox0=\\vbox{\\noindent \\discretionary"
        "{}{}{}xa\\par}\\showbox0 \\setbox0=\\vbox{\\noinde"
        "nt \\discretionary{a}{b}{}xa\\par}\\showbox0 \\set"
        "box0=\\vbox{\\noindent ax\\discretionary{}{}{y}\\p"
        "ar}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n> \\box0=\n\\vbox(4.30554+0.0)x200.0\n.\\hbo"
        "x(4.30554+0.0)x200.0, glue set 190.72218fil\n..\\t"
        "enrm a\n..\\tenrm x\n..\\hbox(0.0+0.0)x0.0\n...\\s"
        "pecial{s}\n..\\penalty 10000\n..\\kern-1.0 (right "
        "margin)\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...oindent"
        " ax\\hbox{\\special{s}}\\par}\\showbox0 \n        "
        "                                          \\setbox"
        "0=\\vbox{\\noindent a...\n\n\n> \\box0=\n\\vbox(4."
        "30554+1.94444)x200.0\n.\\hbox(4.30554+1.94444)x200"
        ".0, glue set 184.94438fil\n..\\tenrm a\n..\\tenrm "
        "x\n..\\hbox(4.30554+1.94444)x5.2778\n...\\tenrm y"
        "\n..\\penalty 10000\n..\\kern-0.5 (right margin)\n"
        "..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue("
        "\\rightskip) 0.0\n\n! OK.\nl.1 ...0=\\vbox{\\noind"
        "ent ax\\hbox{y}\\par}\\showbox0 \n                "
        "                                  \\setbox0=\\vbox"
        "{\\noindent a...\n\n\n> \\box0=\n\\vbox(4.30554+0."
        "0)x200.0\n.\\hbox(4.30554+0.0)x200.0, glue set 184"
        ".72218fil\n..\\tenrm a\n..\\tenrm x\n..\\hbox(0.0+"
        "0.0)x5.0\n..\\penalty 10000\n..\\glue(\\parfillski"
        "p) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n!"
        " OK.\nl.1 ...x{\\noindent ax\\hbox to 5pt{}\\par}"
        "\\showbox0 \n                                     "
        "             \\setbox0=\\vbox{\\noindent a...\n\n"
        "\n> \\box0=\n\\vbox(4.30554+0.0)x200.0\n.\\hbox(4."
        "30554+0.0)x200.0, glue set 189.72218fil\n..\\tenrm"
        " a\n..\\tenrm x\n..\\vbox(0.0+0.0)x0.0\n..\\penalt"
        "y 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...x0=\\vb"
        "ox{\\noindent ax\\vbox{}\\par}\\showbox0 \n       "
        "                                           \\setbo"
        "x0=\\vbox{\\noindent a...\n\n\n> \\box0=\n\\vbox(4"
        ".30554+0.0)x200.0\n.\\hbox(4.30554+0.0)x200.0, glu"
        "e set 189.72218fil\n..\\tenrm a\n..\\tenrm x\n..\\"
        "hbox(0.0+0.0)x0.0\n...\\glue 0.0\n..\\penalty 1000"
        "0\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\gl"
        "ue(\\rightskip) 0.0\n\n! OK.\nl.1 ...\\noindent ax"
        "\\hbox{\\hskip0pt}\\par}\\showbox0 \n             "
        "                                     \\setbox0=\\v"
        "box{\\noindent \\...\n\n\n> \\box0=\n\\vbox(4.3055"
        "4+0.0)x200.0\n.\\hbox(4.30554+0.0)x200.0, glue set"
        " 190.72218fil\n..\\kern-1.0 (left margin)\n..\\hbo"
        "x(0.0+0.0)x0.0\n...\\special{s}\n..\\tenrm x\n..\\"
        "tenrm a\n..\\penalty 10000\n..\\glue(\\parfillskip"
        ") 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! "
        "OK.\nl.1 ...oindent \\hbox{\\special{s}}xa\\par}\\"
        "showbox0 \n                                       "
        "           \\setbox0=\\vbox{\\noindent \\...\n\n\n"
        "> \\box0=\n\\vbox(4.30554+0.0)x200.0\n.\\hbox(4.30"
        "554+0.0)x200.0, glue set 190.72218fil\n..\\kern-1."
        "0 (left margin)\n..\\discretionary\n..\\tenrm x\n."
        ".\\tenrm a\n..\\penalty 10000\n..\\glue(\\parfills"
        "kip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n"
        "\n! OK.\nl.1 ...ndent \\discretionary{}{}{}xa\\par"
        "}\\showbox0 \n                                    "
        "              \\setbox0=\\vbox{\\noindent \\...\n"
        "\n\n> \\box0=\n\\vbox(4.30554+0.0)x200.0\n.\\hbox("
        "4.30554+0.0)x200.0, glue set 1",
        "89.72218fil\n..\\discretionary\n...\\tenrm a\n..|"
        "\\tenrm b\n..\\tenrm x\n..\\tenrm a\n..\\penalty 1"
        "0000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n.."
        "\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...ent \\dis"
        "cretionary{a}{b}{}xa\\par}\\showbox0 \n           "
        "                                       \\setbox0="
        "\\vbox{\\noindent a...\n\n\n> \\box0=\n\\vbox(4.30"
        "554+1.94444)x200.0\n.\\hbox(4.30554+1.94444)x200.0"
        ", glue set 184.94438fil\n..\\tenrm a\n..\\tenrm x"
        "\n..\\discretionary replacing 1\n..\\tenrm y\n..\\"
        "penalty 10000\n..\\kern-0.5 (right margin)\n..\\gl"
        "ue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\righ"
        "tskip) 0.0\n\n! OK.\nl.1 ...dent ax\\discretionary"
        "{}{}{y}\\par}\\showbox0 \n                        "
        "                          \\showbox254\n\n> \\box2"
        "54=void\n\n! OK.\nl.1 ...cretionary{}{}{y}\\par}\\"
        "showbox0 \\showbox254\n                           "
        "                       \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Nothing between the two ends of a formula is hyphenated: the reference
   only looks for a word to hyphenate after glue that a line may break at,
   and glue inside a formula is not that. See docs/DECISIONS.md,
   no-hyphens-inside-a-formula. */
static int test_no_hyphens_inside_a_formula(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\hyphenchar\\tenrm=45 "
        "\\hyphenchar\\teni=45 \\lccode`a=`a \\patterns{a1a"
        "}\\hsize=40pt \\setbox0=\\vbox{\\noindent xx aaaa "
        "xx\\par}\\showbox0 \\setbox0=\\vbox{\\noindent xx "
        "$aaaa$ xx\\par}\\showbox0 \\setbox0=\\vbox{\\noind"
        "ent xx $z$ aaaa xx\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n> \\box0=\n\\vbox(16.30554+0.0)x40.0\n.\\hbo"
        "x(4.30554+0.0)x40.0\n..\\tenrm x\n..\\tenrm x\n.."
        "\\glue(\\spaceskip) 4.0\n..\\tenrm a\n..\\discreti"
        "onary\n...\\tenrm -\n..\\tenrm a\n..\\discretionar"
        "y\n...\\tenrm -\n..\\tenrm a\n..\\discretionary\n."
        "..\\tenrm -\n..\\tenrm a\n..\\glue(\\rightskip) 0."
        "0\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4.3055"
        "4+0.0)x40.0, glue set 29.4444fil\n..\\tenrm x\n.."
        "\\tenrm x\n..\\penalty 10000\n..\\glue(\\parfillsk"
        "ip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n"
        "! OK.\nl.1 ...0=\\vbox{\\noindent xx aaaa xx\\par}"
        "\\showbox0 \n                                     "
        "             \\setbox0=\\vbox{\\noindent x...\n\n"
        "\n> \\box0=\n\\vbox(16.30554+0.0)x40.0\n.\\hbox(4."
        "30554+0.0)x40.0\n..\\tenrm x\n..\\tenrm x\n..\\glu"
        "e(\\spaceskip) 4.0\n..\\mathon\n..\\teni a\n..\\te"
        "ni a\n..\\teni a\n..\\teni a\n..\\mathoff\n..\\glu"
        "e(\\rightskip) 0.0\n.\\glue(\\baselineskip) 7.6944"
        "6\n.\\hbox(4.30554+0.0)x40.0, glue set 29.4444fil"
        "\n..\\tenrm x\n..\\tenrm x\n..\\penalty 10000\n.."
        "\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\"
        "rightskip) 0.0\n\n! OK.\nl.1 ...\\vbox{\\noindent "
        "xx $aaaa$ xx\\par}\\showbox0 \n                   "
        "                               \\setbox0=\\vbox{\\"
        "noindent x...\n\n\n> \\box0=\n\\vbox(16.30554+0.0)"
        "x40.0\n.\\hbox(4.30554+0.0)x40.0\n..\\tenrm x\n.."
        "\\tenrm x\n..\\glue(\\spaceskip) 4.0\n..\\mathon\n"
        "..\\teni z\n..\\kern0.4398\n..\\mathoff\n..\\glue("
        "\\spaceskip) 4.0\n..\\tenrm a\n..\\discretionary\n"
        "...\\tenrm -\n..\\tenrm a\n..\\discretionary\n..\\"
        "tenrm -\n..\\glue(\\rightskip) 0.0\n.\\glue(\\base"
        "lineskip) 7.69446\n.\\hbox(4.30554+0.0)x40.0, glue"
        " set 15.44437fil\n..\\tenrm a\n..\\discretionary\n"
        "...\\tenrm -\n..\\tenrm a\n..\\glue(\\spaceskip) 4"
        ".0\n..\\tenrm x\n..\\tenrm x\n..\\penalty 10000\n."
        ".\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue("
        "\\rightskip) 0.0\n\n! OK.\nl.1 ...box{\\noindent x"
        "x $z$ aaaa xx\\par}\\showbox0 \n                  "
        "                                \\showbox254\n\n> "
        "\\box254=void\n\n! OK.\nl.1 ...nt xx $z$ aaaa xx\\"
        "par}\\showbox0 \\showbox254\n                     "
        "                             \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A line that would have to stretch further than the reference is willing
   to measure is infinitely bad and as loose as a line can be, so it costs
   \adjdemerits against a decent neighbour. See docs/DECISIONS.md,
   a-line-too-short-to-measure. */
static int test_a_line_too_short_to_measure(void)
{
    static const char *const source[] = {
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\catcode`\\_=8 \\tracingonline=1 \\showboxdepth=1"
        "0 \\showboxbreadth=1000 \\hbadness=10000 \\vbadnes"
        "s=10000 \\hfuzz=1000pt \\vfuzz=1000pt \\hsize=200p"
        "t \\parindent=0pt \\boxmaxdepth=16383.99998pt \\ba"
        "selineskip=12pt \\lineskip=0pt \\lineskiplimit=0pt"
        " \\parfillskip=0pt plus1fil \\leftskip=0pt \\right"
        "skip=0pt \\tolerance=10000 \\pretolerance=-1 \\spa"
        "ceskip=4pt \\font\\tenrm=cmr10 \\font\\sevenrm=cmr"
        "7 \\font\\fiverm=cmr5 \\font\\teni=cmmi10 \\font\\"
        "seveni=cmmi7 \\font\\fivei=cmmi5 \\font\\tensy=cms"
        "y10 \\font\\sevensy=cmsy7 \\font\\fivesy=cmsy5 \\f"
        "ont\\tenex=cmex10 \\textfont0=\\tenrm \\scriptfont"
        "0=\\sevenrm \\scriptscriptfont0=\\fiverm \\textfon"
        "t1=\\teni \\scriptfont1=\\seveni \\scriptscriptfon"
        "t1=\\fivei \\textfont2=\\tensy \\scriptfont2=\\sev"
        "ensy \\scriptscriptfont2=\\fivesy \\textfont3=\\te"
        "nex \\scriptfont3=\\tenex \\scriptscriptfont3=\\te"
        "nex \\skewchar\\teni=127 \\skewchar\\seveni=127 \\"
        "skewchar\\fivei=127 \\skewchar\\tensy=48 \\skewcha"
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\trac"
        "ingparagraphs=1 \\adjdemerits=10000 \\setbox0=\\vb"
        "ox{\\noindent a\\penalty0 \\kern0pt aaa aaa aaa aa"
        "a aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa"
        "\\par}%",
        NULL,
    };
    static const char *const expected[] = {
        "\\tenrm a\n@\\penalty via @@0 b=10000 p=0 d=100010"
        "000\n@@1: line 1.0 t=100010000 -> @@0\naaa \n@ via"
        " @@0 b=10000 p=0 d=100010000\n@ via @@1 b=10000 p="
        "0 d=100000000\n@@2: line 1.0 t=100010000 -> @@0\na"
        "aa \n@ via @@0 b=10000 p=0 d=100010000\n@ via @@1 "
        "b=10000 p=0 d=100000000\n@ via @@2 b=10000 p=0 d=1"
        "00000000\n@@3: line 1.0 t=100010000 -> @@0\naaa \n"
        "@ via @@0 b=10000 p=0 d=100010000\n@ via @@1 b=100"
        "00 p=0 d=100000000\n@ via @@2 b=10000 p=0 d=100000"
        "000\n@ via @@3 b=10000 p=0 d=100000000\n@@4: line "
        "1.0 t=100010000 -> @@0\naaa \n@ via @@0 b=10000 p="
        "0 d=100010000\n@ via @@1 b=10000 p=0 d=100000000\n"
        "@ via @@2 b=10000 p=0 d=100000000\n@ via @@3 b=100"
        "00 p=0 d=100000000\n@ via @@4 b=10000 p=0 d=100000"
        "000\n@@5: line 1.0 t=100010000 -> @@0\naaa \n@ via"
        " @@0 b=10000 p=0 d=100010000\n@ via @@1 b=10000 p="
        "0 d=100000000\n@ via @@2 b=10000 p=0 d=100000000\n"
        "@ via @@3 b=10000 p=0 d=100000000\n@ via @@4 b=100"
        "00 p=0 d=100000000\n@ via @@5 b=10000 p=0 d=100000"
        "000\n@@6: line 1.0 t=100010000 -> @@0\naaa \n@ via"
        " @@0 b=10000 p=0 d=100010000\n@ via @@1 b=10000 p="
        "0 d=100000000\n@ via @@2 b=10000 p=0 d=100000000\n"
        "@ via @@3 b=10000 p=0 d=100000000\n@ via @@4 b=100"
        "00 p=0 d=100000000\n@ via @@5 b=10000 p=0 d=100000"
        "000\n@ via @@6 b=10000 p=0 d=100000000\n@@7: line "
        "1.0 t=100010000 -> @@0\naaa \n@ via @@0 b=10000 p="
        "0 d=100010000\n@ via @@1 b=10000 p=0 d=100000000\n"
        "@ via @@2 b=10000 p=0 d=100000000\n@ via @@3 b=100"
        "00 p=0 d=100000000\n@ via @@4 b=10000 p=0 d=100000"
        "000\n@ via @@5 b=10000 p=0 d=100000000\n@ via @@6 "
        "b=10000 p=0 d=100000000\n@ via @@7 b=10000 p=0 d=1"
        "00000000\n@@8: line 1.0 t=100010000 -> @@0\naaa \n"
        "@ via @@0 b=10000 p=0 d=100010000\n@ via @@1 b=100"
        "00 p=0 d=100000000\n@ via @@2 b=10000 p=0 d=100000"
        "000\n@ via @@3 b=10000 p=0 d=100000000\n@ via @@4 "
        "b=10000 p=0 d=100000000\n@ via @@5 b=10000 p=0 d=1"
        "00000000\n@ via @@6 b=10000 p=0 d=100000000\n@ via"
        " @@7 b=10000 p=0 d=100000000\n@ via @@8 b=10000 p="
        "0 d=100000000\n@@9: line 1.0 t=100010000 -> @@0\na"
        "aa \n@ via @@0 b=10000 p=0 d=100010000\n@ via @@1 "
        "b=10000 p=0 d=100000000\n@ via @@2 b=10000 p=0 d=1"
        "00000000\n@ via @@3 b=10000 p=0 d=100000000\n@ via"
        " @@4 b=10000 p=0 d=100000000\n@ via @@5 b=10000 p="
        "0 d=100000000\n@ via @@6 b=10000 p=0 d=100000000\n"
        "@ via @@7 b=10000 p=0 d=100000000\n@ via @@8 b=100"
        "00 p=0 d=100000000\n@ via @@9 b=10000 p=0 d=100000"
        "000\n@@10: line 1.0 t=100010000 -> @@0\naaa \n@ vi"
        "a @@0 b=10000 p=0 d=100010000\n@ via @@1 b=10000 p"
        "=0 d=100000000\n@ via @@2 b=10000 p=0 d=100000000"
        "\n@ via @@3 b=10000 p=0 d=100000000\n@ via @@4 b=1"
        "0000 p=0 d=100000000\n@ via @@5 b=10000 p=0 d=1000"
        "00000\n@ via @@6 b=10000 p=0 d=100000000\n@ via @@"
        "7 b=10000 p=0 d=100000000\n@ via @@8 b=10000 p=0 d"
        "=100000000\n@ via @@9 b=10000 p=0 d=100000000\n@ v"
        "ia @@10 b=10000 p=0 d=100000000\n@@11: line 1.0 t="
        "100010000 -> @@0\naaa \n@ via @@2 b=10000 p=0 d=10"
        "0000000\n@ via @@3 b=10000 p=0 d=100000000\n@ via "
        "@@4 b=10000 p=0 d=100000000\n@ via @@5 b=10000 p=0"
        " d=100000000\n@ via @@6 b=10000 p=0 d=100000000\n@"
        " via @@7 b=10000 p=0 d=100000000\n@ via @@8 b=1000",
        "0 p=0 d=100000000\n@ via @@9 b=10000 p=0 d=1000000"
        "00\n@ via @@10 b=10000 p=0 d=100000000\n@ via @@11"
        " b=10000 p=0 d=100000000\n@@12: line 2.0 t=2000100"
        "00 -> @@11\naaa \n@ via @@3 b=10000 p=0 d=10000000"
        "0\n@ via @@4 b=10000 p=0 d=100000000\n@ via @@5 b="
        "10000 p=0 d=100000000\n@ via @@6 b=10000 p=0 d=100"
        "000000\n@ via @@7 b=10000 p=0 d=100000000\n@ via @"
        "@8 b=10000 p=0 d=100000000\n@ via @@9 b=10000 p=0 "
        "d=100000000\n@ via @@10 b=10000 p=0 d=100000000\n@"
        " via @@11 b=10000 p=0 d=100000000\n@ via @@12 b=10"
        "000 p=0 d=100000000\n@@13: line 2.0 t=200010000 ->"
        " @@11\naaa \n@ via @@4 b=10000 p=0 d=100000000\n@ "
        "via @@5 b=10000 p=0 d=100000000\n@ via @@6 b=10000"
        " p=0 d=100000000\n@ via @@7 b=10000 p=0 d=10000000"
        "0\n@ via @@8 b=10000 p=0 d=100000000\n@ via @@9 b="
        "10000 p=0 d=100000000\n@ via @@10 b=10000 p=0 d=10"
        "0000000\n@ via @@11 b=10000 p=0 d=100000000\n@ via"
        " @@12 b=10000 p=0 d=100000000\n@ via @@13 b=10000 "
        "p=0 d=100000000\n@@14: line 2.0 t=200010000 -> @@1"
        "1\naaa \n@ via @@5 b=10000 p=0 d=100000000\n@ via "
        "@@6 b=10000 p=0 d=100000000\n@ via @@7 b=10000 p=0"
        " d=100000000\n@ via @@8 b=10000 p=0 d=100000000\n@"
        " via @@9 b=10000 p=0 d=100000000\n@ via @@10 b=100"
        "00 p=0 d=100000000\n@ via @@11 b=10000 p=0 d=10000"
        "0000\n@ via @@12 b=10000 p=0 d=100000000\n@ via @@"
        "13 b=10000 p=0 d=100000000\n@ via @@14 b=10000 p=0"
        " d=100000000\n@@15: line 2.0 t=200010000 -> @@11\n"
        "aaa \n@ via @@6 b=10000 p=0 d=100000000\n@ via @@7"
        " b=10000 p=0 d=100000000\n@ via @@8 b=10000 p=0 d="
        "100000000\n@ via @@9 b=10000 p=0 d=100000000\n@ vi"
        "a @@10 b=10000 p=0 d=100000000\n@ via @@11 b=10000"
        " p=0 d=100000000\n@ via @@12 b=10000 p=0 d=1000000"
        "00\n@ via @@13 b=10000 p=0 d=100000000\n@ via @@14"
        " b=10000 p=0 d=100000000\n@ via @@15 b=10000 p=0 d"
        "=100000000\n@@16: line 2.0 t=200010000 -> @@11\naa"
        "a \n@\\par via @@7 b=0 p=-10000 d=10000\n@\\par vi"
        "a @@8 b=0 p=-10000 d=10000\n@\\par via @@9 b=0 p=-"
        "10000 d=10000\n@\\par via @@10 b=0 p=-10000 d=1000"
        "0\n@\\par via @@11 b=0 p=-10000 d=10000\n@\\par vi"
        "a @@12 b=0 p=-10000 d=10000\n@\\par via @@13 b=0 p"
        "=-10000 d=10000\n@\\par via @@14 b=0 p=-10000 d=10"
        "000\n@\\par via @@15 b=0 p=-10000 d=10000\n@\\par "
        "via @@16 b=0 p=-10000 d=10000\n@@17: line 2.2- t=1"
        "00020000 -> @@11\n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* How many bytes a movement is written in: the reference chooses by how
   big the movement is, not by what the signed range holds, so -128 takes two
   bytes and -32768 three. See docs/DECISIONS.md, the-page-description. */
static int test_how_wide_a_movement_is(void)
{
    static const char *const source[] = {
        "\\pdfoutput=0 \\year=2026 \\month=8 \\day=18 \\tim"
        "e=1117 \\font\\tenrm=cmr10 \\tenrm \\hsize=400pt "
        "\\vsize=200pt \\parindent=0pt \\hbadness=10000 \\h"
        "fuzz=1000pt \\shipout\\hbox{a\\kern 127sp a\\kern "
        "-128sp a\\kern 32767sp a\\kern -32768sp a\\kern 83"
        "88607sp a\\kern -8388608sp a}\\shipout\\vbox{\\hbo"
        "x{a}\\kern -128sp \\hbox{a}\\kern -32768sp \\hbox{"
        "a}}",
        NULL,
    };
    static const char *const expected[] = {
        "f702018392c01c3b0000000003e81b20546558206f75747075"
        "7420323032362e30382e31383a313833378b00000000000000"
        "00000000000000000000000000000000000000000000000000"
        "0000000000000000ffffffff9f044e38f3004bf16079000a00"
        "00000a00000005636d723130ab618f7f6190ff8061907fff61"
        "91ff800061917fffff6192ff800000618c8b00000000000000"
        "00000000000000000000000000000000000000000000000000"
        "00000000000000000000002a9f044e388dab618e9f044db88d"
        "618e9f03ce388d618e8cf80000008e018392c01c3b00000000"
        "03e8000c6a280023000400010002f3004bf16079000a000000"
        "0a00000005636d723130f9000000d202dfdfdfdfdfdf",
        NULL,
    };
    return run_document_dvi(source, expected);
}

/* The glue at either end of a line stands outside its margin kern, so
   \leftmarginkern looks past the \leftskip a table-of-contents entry sets.
   See docs/DECISIONS.md, the-margin-kerns-of-a-line. */
static int test_a_margin_kern_behind_the_leftskip(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\font\\f=cmr10 \\f \\h"
        "size=100pt \\parindent=0pt \\parfillskip=0pt \\lpc"
        "ode\\f`\\(=117 \\rpcode\\f`\\)=117 \\pdfprotrudech"
        "ars=2 \\setbox1=\\vbox{\\leftskip=20pt \\noindent("
        "1)\\par}\\setbox2=\\vbox{\\unvbox1 \\global\\setbo"
        "x1=\\lastbox}\\dimen5=\\leftmarginkern1 \\dimen6="
        "\\rightmarginkern1 \\message{<L=\\the\\dimen5><R="
        "\\the\\dimen6>}\\showbox1 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n<L=-1.17pt><R=-1.17pt>\n> \\box1=\n\\hbox(7."
        "5+2.5)x100.0\n.\\glue(\\leftskip) 20.0\n.\\kern-1."
        "17 (left margin)\n.\\f (\n.\\f 1\n.\\f )\n.\\penal"
        "ty 10000\n.\\kern-1.17 (right margin)\n.\\glue(\\p"
        "arfillskip) 0.0\n.\\glue(\\rightskip) 0.0\n\n! OK."
        "\nl.1 ...e{<L=\\the\\dimen5><R=\\the\\dimen6>}\\sh"
        "owbox1 \n                                         "
        "         \\showbox254\n\n> \\box254=void\n\n! OK."
        "\nl.1 ...imen5><R=\\the\\dimen6>}\\showbox1 \\show"
        "box254\n                                          "
        "        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A brace that ends a box ends the paragraph inside it while the parameters
   that paragraph was set with are still in force, even when the box stands
   in an alignment entry; and the lines a nested list breaks are that list's
   own count, not the enclosing paragraph's. See docs/DECISIONS.md,
   a-paragraph-a-brace-ends. */
static int test_a_paragraph_a_brace_ends(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\parfillskip=0pt plus "
        "1fil \\setbox0=\\vbox{\\halign{#\\cr\\vbox{\\hsize"
        "=50pt \\parfillskip=0pt \\noindent a}\\cr\\vbox{\\"
        "hsize=50pt \\parfillskip=0pt \\noindent a\\par}\\c"
        "r}}\\showbox0 \\setbox0=\\vbox{\\noindent aaa \\se"
        "tbox1=\\vbox{\\noindent bbb\\par}ccc ccc ccc ccc\\"
        "par \\message{[PG \\the\\prevgraf]}}\\message{[PG "
        "\\the\\prevgraf]}\\setbox0=\\vbox{\\noindent aaa "
        "\\setbox1=\\vbox{\\hsize=40pt \\noindent bbb bbb b"
        "bb\\par}ccc\\par \\message{[PG \\the\\prevgraf]}} "
        "\\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n> \\box0=\n\\vbox(16.30554+0.0)x50.0\n.\\hbo"
        "x(4.30554+0.0)x50.0\n..\\glue(\\tabskip) 0.0\n..\\"
        "hbox(4.30554+0.0)x50.0\n...\\vbox(4.30554+0.0)x50."
        "0\n....\\hbox(4.30554+0.0)x50.0\n.....\\tenrm a\n."
        "....\\penalty 10000\n.....\\glue(\\parfillskip) 0."
        "0\n.....\\glue(\\rightskip) 0.0\n..\\glue(\\tabski"
        "p) 0.0\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4"
        ".30554+0.0)x50.0\n..\\glue(\\tabskip) 0.0\n..\\hbo"
        "x(4.30554+0.0)x50.0\n...\\vbox(4.30554+0.0)x50.0\n"
        "....\\hbox(4.30554+0.0)x50.0\n.....\\tenrm a\n...."
        ".\\penalty 10000\n.....\\glue(\\parfillskip) 0.0\n"
        ".....\\glue(\\rightskip) 0.0\n..\\glue(\\tabskip) "
        "0.0\n\n! OK.\nl.1 ...illskip=0pt \\noindent a\\par"
        "}\\cr}}\\showbox0 \n                              "
        "                    \\setbox0=\\vbox{\\noindent a."
        "..\n\n\n[PG 1] [PG 0] [PG 1]\n> \\box254=void\n\n!"
        " OK.\nl.1 ...r \\message{[PG \\the\\prevgraf]}} \\"
        "showbox254\n                                      "
        "            \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The PDF file the reference writes when \pdfoutput is positive: the page's
   own stream of text and rules, the objects it needs, the measurements of
   every font it names, and the table of where all of them are. See
   docs/DECISIONS.md, the-pdf-file. */
static int test_what_the_document_says_about_itself(void)
{
    /* What \pdfinfo and \pdfcatalog were given goes into the file's own
       dictionaries, a second call joined to the first with nothing between
       them, and each of the three entries the reference writes for itself is
       left out where the document has named it. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=200pt \\vsize=300pt "
        "\\parindent=0pt\\pdfinfo{/Author (A)}\\pdfinfo{/Pr"
        "oducer (P)/Creator (C)/Trapped /True}\\pdfcatalog{"
        "/A (1)}\\pdfcatalog{/B (2)}\\shipout\\hbox{a}\n\\e"
        "nd\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820333720202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b2861295d544a0a45540a0a656e6473747265"
        "616d0a656e646f626a0a322030206f626a0a3c3c0a2f547970"
        "65202f506167650a2f436f6e74656e74732033203020520a2f"
        "5265736f75726365732031203020520a2f4d65646961426f78"
        "205b302030203134382e393831203134382e3238395d0a2f50"
        "6172656e742035203020520a3e3e0a656e646f626a0a312030"
        "206f626a0a3c3c0a2f466f6e74203c3c202f46312034203020"
        "52203e3e0a2f50726f63536574205b202f504446202f546578"
        "74205d0a3e3e0a656e646f626a0a362030206f626a0a5b3530"
        "305d0a656e646f626a0a372030206f626a0a3c3c0a2f547970"
        "65202f466f6e7444657363726970746f720a2f466f6e744e61"
        "6d65202f434d5231300a2f466c6167732033340a2f466f6e74"
        "42426f78205b30202d3139342031303030203639345d0a2f41"
        "7363656e74203639340a2f436170486569676874203638330a"
        "2f44657363656e74202d3139340a2f4974616c6963416e676c"
        "6520300a2f5374656d562039330a2f58486569676874203433"
        "310a3e3e0a656e646f626a0a342030206f626a0a3c3c0a2f54"
        "797065202f466f6e740a2f53756274797065202f5479706531"
        "0a2f42617365466f6e74202f434d5231300a2f466f6e744465"
        "7363726970746f722037203020520a2f466972737443686172"
        "2039370a2f4c617374436861722039370a2f57696474687320"
        "36203020520a3e3e0a656e646f626a0a352030206f626a0a3c"
        "3c0a2f54797065202f50616765730a2f436f756e7420310a2f"
        "4b696473205b32203020525d0a3e3e0a656e646f626a0a3820"
        "30206f626a0a3c3c0a2f54797065202f436174616c6f670a2f"
        "50616765732035203020520a2f41202831292f42202832290a"
        "3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f417574"
        "686f72202841292f50726f6475636572202850292f43726561"
        "746f72202843292f54726170706564202f547275650a3e3e0a"
        "656e646f626a0a787265660a302031300a3030303030303030"
        "30302036353533352066200a30303030303030323232203030"
        "303030206e200a30303030303030313130203030303030206e"
        "200a30303030303030303135203030303030206e200a303030"
        "30303030343837203030303030206e200a3030303030303036"
        "3136203030303030206e200a30303030303030323839203030"
        "303030206e200a30303030303030333130203030303030206e"
        "200a30303030303030363733203030303030206e200a303030"
        "30303030373335203030303030206e200a747261696c65720a"
        "3c3c202f53697a652031300a2f526f6f742038203020520a2f"
        "496e666f2039203020520a203e3e0a7374617274787265660a"
        "3830370a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_action_a_file_opens_on(void)
{
    /* The action \pdfcatalog names for the file's opening goes into an
       object of its own where the catalogue is written, before any page has
       been shipped, and the page it points at is the object that page will
       be given. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=200pt \\vsize=300pt "
        "\\parindent=0pt\\pdfcatalog{/PageMode /UseOutlines"
        "} openaction goto page 2 {/Fit}\\shipout\\hbox{a}"
        "\\shipout\\hbox{b}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a312030206f626a0a3c3c"
        "202f53202f476f546f202f44205b3220302052202f4669745d"
        "203e3e0a656e646f626a0a352030206f626a0a3c3c0a2f4c65"
        "6e67746820333720202020202020200a3e3e0a73747265616d"
        "0a42540a2f463120392e393632362054662037322037322054"
        "64205b2861295d544a0a45540a0a656e6473747265616d0a65"
        "6e646f626a0a342030206f626a0a3c3c0a2f54797065202f50"
        "6167650a2f436f6e74656e74732035203020520a2f5265736f"
        "75726365732033203020520a2f4d65646961426f78205b3020"
        "30203134382e393831203134382e3238395d0a2f506172656e"
        "742037203020520a3e3e0a656e646f626a0a332030206f626a"
        "0a3c3c0a2f466f6e74203c3c202f4631203620302052203e3e"
        "0a2f50726f63536574205b202f504446202f54657874205d0a"
        "3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f4c656e"
        "67746820333720202020202020200a3e3e0a73747265616d0a"
        "42540a2f463120392e39363236205466203732203732205464"
        "205b2862295d544a0a45540a0a656e6473747265616d0a656e"
        "646f626a0a322030206f626a0a3c3c0a2f54797065202f5061"
        "67650a2f436f6e74656e74732039203020520a2f5265736f75"
        "726365732038203020520a2f4d65646961426f78205b302030"
        "203134392e353335203135302e3931395d0a2f506172656e74"
        "2037203020520a3e3e0a656e646f626a0a382030206f626a0a"
        "3c3c0a2f466f6e74203c3c202f4631203620302052203e3e0a"
        "2f50726f63536574205b202f504446202f54657874205d0a3e"
        "3e0a656e646f626a0a31302030206f626a0a5b353030203535"
        "352e365d0a656e646f626a0a31312030206f626a0a3c3c0a2f"
        "54797065202f466f6e7444657363726970746f720a2f466f6e"
        "744e616d65202f434d5231300a2f466c6167732033340a2f46"
        "6f6e7442426f78205b30202d3139342031303030203639345d"
        "0a2f417363656e74203639340a2f4361704865696768742036"
        "38330a2f44657363656e74202d3139340a2f4974616c696341"
        "6e676c6520300a2f5374656d562039330a2f58486569676874"
        "203433310a3e3e0a656e646f626a0a362030206f626a0a3c3c"
        "0a2f54797065202f466f6e740a2f53756274797065202f5479"
        "7065310a2f42617365466f6e74202f434d5231300a2f466f6e"
        "7444657363726970746f72203131203020520a2f4669727374"
        "436861722039370a2f4c617374436861722039380a2f576964"
        "746873203130203020520a3e3e0a656e646f626a0a37203020"
        "6f626a0a3c3c0a2f54797065202f50616765730a2f436f756e"
        "7420320a2f4b696473205b34203020522032203020525d0a3e"
        "3e0a656e646f626a0a31322030206f626a0a3c3c0a2f547970"
        "65202f436174616c6f670a2f50616765732037203020520a2f"
        "506167654d6f6465202f5573654f75746c696e65730a2f4f70"
        "656e416374696f6e2031203020520a3e3e0a656e646f626a0a"
        "31332030206f626a0a3c3c0a2f50726f647563657220287064"
        "665465582d312e34302e3235290a2f43726561746f72202854"
        "6558290a2f54726170706564202f46616c73650a3e3e0a656e"
        "646f626a0a787265660a302031340a30303030303030303030"
        "2036353533352066200a303030303030303031352030303030"
        "30206e200a30303030303030343330203030303030206e200a"
        "30303030303030323638203030303030206e200a3030303030"
        "3030313536203030303030206e200a30303030303030303631"
        "203030303030206e200a303030303030303831352030303030"
        "30206e200a30303030303030393436203030303030206e200a"
        "30303030303030353432203030303030206e200a3030303030"
        "3030333335203030303030206e200a30303030303030363039"
        "203030303030206e200a303030303030303633372030303030"
        "30206e200a30303030303031303039203030303030206e200a"
        "30303030303031313030203030303030206e200a747261696c"
        "65720a3c3c202f53697a652031340a2f526f6f742031322030",
        "20520a2f496e666f203133203020520a203e3e0a7374617274"
        "787265660a313138300a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_outline_of_a_document(void)
{
    /* The entries the document writes with \pdfoutline are linked into a
       tree by the number of children each of them declares, and the tree is
       written after the pages, from the last entry back to the first: an
       open entry and a closed one, a level below each of them, an attribute
       of the document's own, and a count left unsatisfied, which is what
       decides the entry the root calls its last. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=200pt \\vsize=300pt "
        "\\parindent=0pt\\pdfoutline attr{/C [1 0 0]} goto "
        "page 1 {/Fit} count 1 {A}\\pdfoutline goto page 1 "
        "{/Fit} count 2 {A1}\\pdfoutline goto page 1 {/Fit}"
        " {A1a}\\pdfoutline goto page 1 {/Fit} {A1b}\\pdfou"
        "tline goto page 1 {/Fit} count -1 {B}\\pdfoutline "
        "goto page 1 {/Fit} count 1 {B1}\\pdfoutline goto p"
        "age 1 {/Fit} {B1a}\\pdfoutline goto page 1 {/Fit} "
        "count 3 {C}\\pdfoutline goto page 1 {/Fit} {C1}\\s"
        "hipout\\hbox{a}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a312030206f626a0a3c3c"
        "202f53202f476f546f202f44205b3220302052202f4669745d"
        "203e3e0a656e646f626a0a342030206f626a0a2841290a656e"
        "646f626a0a352030206f626a0a3c3c202f53202f476f546f20"
        "2f44205b3220302052202f4669745d203e3e0a656e646f626a"
        "0a372030206f626a0a284131290a656e646f626a0a38203020"
        "6f626a0a3c3c202f53202f476f546f202f44205b3220302052"
        "202f4669745d203e3e0a656e646f626a0a31302030206f626a"
        "0a28413161290a656e646f626a0a31312030206f626a0a3c3c"
        "202f53202f476f546f202f44205b3220302052202f4669745d"
        "203e3e0a656e646f626a0a31332030206f626a0a2841316229"
        "0a656e646f626a0a31342030206f626a0a3c3c202f53202f47"
        "6f546f202f44205b3220302052202f4669745d203e3e0a656e"
        "646f626a0a31362030206f626a0a2842290a656e646f626a0a"
        "31372030206f626a0a3c3c202f53202f476f546f202f44205b"
        "3220302052202f4669745d203e3e0a656e646f626a0a313920"
        "30206f626a0a284231290a656e646f626a0a32302030206f62"
        "6a0a3c3c202f53202f476f546f202f44205b3220302052202f"
        "4669745d203e3e0a656e646f626a0a32322030206f626a0a28"
        "423161290a656e646f626a0a32332030206f626a0a3c3c202f"
        "53202f476f546f202f44205b3220302052202f4669745d203e"
        "3e0a656e646f626a0a32352030206f626a0a2843290a656e64"
        "6f626a0a32362030206f626a0a3c3c202f53202f476f546f20"
        "2f44205b3220302052202f4669745d203e3e0a656e646f626a"
        "0a32382030206f626a0a284331290a656e646f626a0a333020"
        "30206f626a0a3c3c0a2f4c656e677468203337202020202020"
        "20200a3e3e0a73747265616d0a42540a2f463120392e393632"
        "36205466203732203732205464205b2861295d544a0a45540a"
        "0a656e6473747265616d0a656e646f626a0a322030206f626a"
        "0a3c3c0a2f54797065202f506167650a2f436f6e74656e7473"
        "203330203020520a2f5265736f757263657320323920302052"
        "0a2f4d65646961426f78205b302030203134382e3938312031"
        "34382e3238395d0a2f506172656e74203332203020520a3e3e"
        "0a656e646f626a0a32392030206f626a0a3c3c0a2f466f6e74"
        "203c3c202f463120333120302052203e3e0a2f50726f635365"
        "74205b202f504446202f54657874205d0a3e3e0a656e646f62"
        "6a0a33332030206f626a0a5b3530305d0a656e646f626a0a33"
        "342030206f626a0a3c3c0a2f54797065202f466f6e74446573"
        "63726970746f720a2f466f6e744e616d65202f434d5231300a"
        "2f466c6167732033340a2f466f6e7442426f78205b30202d31"
        "39342031303030203639345d0a2f417363656e74203639340a"
        "2f436170486569676874203638330a2f44657363656e74202d"
        "3139340a2f4974616c6963416e676c6520300a2f5374656d56"
        "2039330a2f58486569676874203433310a3e3e0a656e646f62"
        "6a0a33312030206f626a0a3c3c0a2f54797065202f466f6e74"
        "0a2f53756274797065202f54797065310a2f42617365466f6e"
        "74202f434d5231300a2f466f6e7444657363726970746f7220"
        "3334203020520a2f4669727374436861722039370a2f4c6173"
        "74436861722039370a2f576964746873203333203020520a3e"
        "3e0a656e646f626a0a33322030206f626a0a3c3c0a2f547970"
        "65202f50616765730a2f436f756e7420310a2f4b696473205b"
        "32203020525d0a3e3e0a656e646f626a0a33352030206f626a"
        "0a3c3c0a2f54797065202f4f75746c696e65730a2f46697273"
        "742033203020520a2f4c617374203237203020520a2f436f75"
        "6e7420370a3e3e0a656e646f626a0a32372030206f626a0a3c"
        "3c0a2f5469746c65203238203020520a2f4120323620302052"
        "0a2f506172656e74203234203020520a3e3e0a656e646f626a"
        "0a32342030206f626a0a3c3c0a2f5469746c65203235203020"
        "520a2f41203233203020520a2f506172656e74203335203020"
        "520a2f50726576203135203020520a2f466972737420323720",
        "3020520a2f4c617374203237203020520a2f436f756e742031"
        "0a3e3e0a656e646f626a0a32312030206f626a0a3c3c0a2f54"
        "69746c65203232203020520a2f41203230203020520a2f5061"
        "72656e74203138203020520a3e3e0a656e646f626a0a313820"
        "30206f626a0a3c3c0a2f5469746c65203139203020520a2f41"
        "203137203020520a2f506172656e74203135203020520a2f46"
        "69727374203231203020520a2f4c617374203231203020520a"
        "2f436f756e7420310a3e3e0a656e646f626a0a31352030206f"
        "626a0a3c3c0a2f5469746c65203136203020520a2f41203134"
        "203020520a2f506172656e74203335203020520a2f50726576"
        "2033203020520a2f4e657874203234203020520a2f46697273"
        "74203138203020520a2f4c617374203138203020520a2f436f"
        "756e74202d320a3e3e0a656e646f626a0a31322030206f626a"
        "0a3c3c0a2f5469746c65203133203020520a2f412031312030"
        "20520a2f506172656e742036203020520a2f50726576203920"
        "3020520a3e3e0a656e646f626a0a392030206f626a0a3c3c0a"
        "2f5469746c65203130203020520a2f412038203020520a2f50"
        "6172656e742036203020520a2f4e657874203132203020520a"
        "3e3e0a656e646f626a0a362030206f626a0a3c3c0a2f546974"
        "6c652037203020520a2f412035203020520a2f506172656e74"
        "2033203020520a2f46697273742039203020520a2f4c617374"
        "203132203020520a2f436f756e7420320a3e3e0a656e646f62"
        "6a0a332030206f626a0a3c3c0a2f5469746c65203420302052"
        "0a2f412031203020520a2f506172656e74203335203020520a"
        "2f4e657874203135203020520a2f4669727374203620302052"
        "0a2f4c6173742036203020520a2f436f756e7420330a2f4320"
        "5b31203020305d0a3e3e0a656e646f626a0a33362030206f62"
        "6a0a3c3c0a2f54797065202f436174616c6f670a2f50616765"
        "73203332203020520a2f4f75746c696e657320333520302052"
        "0a3e3e0a656e646f626a0a33372030206f626a0a3c3c0a2f50"
        "726f647563657220287064665465582d312e34302e3235290a"
        "2f43726561746f722028546558290a2f54726170706564202f"
        "46616c73650a3e3e0a656e646f626a0a787265660a30203338"
        "0a303030303030303030302036353533352066200a30303030"
        "303030303135203030303030206e200a303030303030303731"
        "38203030303030206e200a3030303030303230353220303030"
        "3030206e200a30303030303030303631203030303030206e20"
        "0a30303030303030303830203030303030206e200a30303030"
        "303031393630203030303030206e200a303030303030303132"
        "36203030303030206e200a3030303030303031343620303030"
        "3030206e200a30303030303031383839203030303030206e20"
        "0a30303030303030313932203030303030206e200a30303030"
        "303030323134203030303030206e200a303030303030313831"
        "37203030303030206e200a3030303030303032363120303030"
        "3030206e200a30303030303030323833203030303030206e20"
        "0a30303030303031363934203030303030206e200a30303030"
        "303030333330203030303030206e200a303030303030303335"
        "30203030303030206e200a3030303030303135393720303030"
        "3030206e200a30303030303030333937203030303030206e20"
        "0a30303030303030343138203030303030206e200a30303030"
        "303031353336203030303030206e200a303030303030303436"
        "35203030303030206e200a3030303030303034383720303030"
        "3030206e200a30303030303031343236203030303030206e20"
        "0a30303030303030353334203030303030206e200a30303030"
        "303030353534203030303030206e200a303030303030313336"
        "35203030303030206e200a3030303030303036303120303030"
        "3030206e200a30303030303030383333203030303030206e20"
        "0a30303030303030363232203030303030206e200a30303030"
        "303031313032203030303030206e200a303030303030313233"
        "34203030303030206e200a3030303030303039303220303030",
        "3030206e200a30303030303030393234203030303030206e20"
        "0a30303030303031323932203030303030206e200a30303030"
        "303032313638203030303030206e200a303030303030323233"
        "36203030303030206e200a747261696c65720a3c3c202f5369"
        "7a652033380a2f526f6f74203336203020520a2f496e666f20"
        "3337203020520a203e3e0a7374617274787265660a32333136"
        "0a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_version_a_file_states(void)
{
    /* The file states the version the document asked for when the first
       thing was written into it -- and what a group asked for and gave back
       is not what the document asked for. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=200pt \\vsize=300pt "
        "\\parindent=0pt\\pdfminorversion=5 {\\pdfminorvers"
        "ion=7 }\\shipout\\hbox{a}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e350a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820333720202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b2861295d544a0a45540a0a656e6473747265"
        "616d0a656e646f626a0a322030206f626a0a3c3c0a2f547970"
        "65202f506167650a2f436f6e74656e74732033203020520a2f"
        "5265736f75726365732031203020520a2f4d65646961426f78"
        "205b302030203134382e393831203134382e3238395d0a2f50"
        "6172656e742035203020520a3e3e0a656e646f626a0a312030"
        "206f626a0a3c3c0a2f466f6e74203c3c202f46312034203020"
        "52203e3e0a2f50726f63536574205b202f504446202f546578"
        "74205d0a3e3e0a656e646f626a0a362030206f626a0a5b3530"
        "305d0a656e646f626a0a372030206f626a0a3c3c0a2f547970"
        "65202f466f6e7444657363726970746f720a2f466f6e744e61"
        "6d65202f434d5231300a2f466c6167732033340a2f466f6e74"
        "42426f78205b30202d3139342031303030203639345d0a2f41"
        "7363656e74203639340a2f436170486569676874203638330a"
        "2f44657363656e74202d3139340a2f4974616c6963416e676c"
        "6520300a2f5374656d562039330a2f58486569676874203433"
        "310a3e3e0a656e646f626a0a342030206f626a0a3c3c0a2f54"
        "797065202f466f6e740a2f53756274797065202f5479706531"
        "0a2f42617365466f6e74202f434d5231300a2f466f6e744465"
        "7363726970746f722037203020520a2f466972737443686172"
        "2039370a2f4c617374436861722039370a2f57696474687320"
        "36203020520a3e3e0a656e646f626a0a352030206f626a0a3c"
        "3c0a2f54797065202f50616765730a2f436f756e7420310a2f"
        "4b696473205b32203020525d0a3e3e0a656e646f626a0a3820"
        "30206f626a0a3c3c0a2f54797065202f436174616c6f670a2f"
        "50616765732035203020520a3e3e0a656e646f626a0a392030"
        "206f626a0a3c3c0a2f50726f64756365722028706466546558"
        "2d312e34302e3235290a2f43726561746f722028546558290a"
        "2f54726170706564202f46616c73650a3e3e0a656e646f626a"
        "0a787265660a302031300a3030303030303030303020363535"
        "33352066200a30303030303030323232203030303030206e20"
        "0a30303030303030313130203030303030206e200a30303030"
        "303030303135203030303030206e200a303030303030303438"
        "37203030303030206e200a3030303030303036313620303030"
        "3030206e200a30303030303030323839203030303030206e20"
        "0a30303030303030333130203030303030206e200a30303030"
        "303030363733203030303030206e200a303030303030303732"
        "32203030303030206e200a747261696c65720a3c3c202f5369"
        "7a652031300a2f526f6f742038203020520a2f496e666f2039"
        "203020520a203e3e0a7374617274787265660a3830310a2525"
        "454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_box_a_font_fills(void)
{
    /* The bounding box a font is described by reaches as far up as the font
       does: `msbm10` and `rsfs10` have capitals standing higher than the
       letter the ascent is measured on, and their boxes reach to the cap
       height rather than to the ascent. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=msbm10 \\font\\b=rsfs10 \\font\\c=cmr1"
        "0 \\font\\d=cmex10 \\hsize=16000pt \\vsize=300pt "
        "\\parindent=0pt\\shipout\\hbox{\\a A\\b A\\c A\\d "
        "\\char0 }\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313430202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "38332e353537205464205b2841295d544a2f463220392e3936"
        "323620546620372e3139352030205464205b2841295d544a2f"
        "463320392e3936323620546620372e3939362030205464205b"
        "2841295d544a2f463420392e3936323620546620372e343732"
        "2030205464205b285c303030295d544a0a45540a0a656e6473"
        "747265616d0a656e646f626a0a322030206f626a0a3c3c0a2f"
        "54797065202f506167650a2f436f6e74656e74732033203020"
        "520a2f5265736f75726365732031203020520a2f4d65646961"
        "426f78205b302030203137312e323239203136322e3533315d"
        "0a2f506172656e742038203020520a3e3e0a656e646f626a0a"
        "312030206f626a0a3c3c0a2f466f6e74203c3c202f46312034"
        "20302052202f4632203520302052202f463320362030205220"
        "2f4634203720302052203e3e0a2f50726f63536574205b202f"
        "504446202f54657874205d0a3e3e0a656e646f626a0a392030"
        "206f626a0a5b3435382e335d0a656e646f626a0a3130203020"
        "6f626a0a3c3c0a2f54797065202f466f6e7444657363726970"
        "746f720a2f466f6e744e616d65202f434d455831300a2f466c"
        "6167732033340a2f466f6e7442426f78205b30202d36303020"
        "313030302034305d0a2f417363656e742034300a2f43617048"
        "656967687420300a2f44657363656e74202d3630300a2f4974"
        "616c6963416e676c6520300a2f5374656d56203237300a2f58"
        "486569676874203433310a3e3e0a656e646f626a0a31312030"
        "206f626a0a5b3735305d0a656e646f626a0a31322030206f62"
        "6a0a3c3c0a2f54797065202f466f6e7444657363726970746f"
        "720a2f466f6e744e616d65202f434d5231300a2f466c616773"
        "2033340a2f466f6e7442426f78205b30202d31393420313030"
        "30203639345d0a2f417363656e74203639340a2f4361704865"
        "69676874203638330a2f44657363656e74202d3139340a2f49"
        "74616c6963416e676c6520300a2f5374656d562039330a2f58"
        "486569676874203433310a3e3e0a656e646f626a0a31332030"
        "206f626a0a5b3830322e355d0a656e646f626a0a3134203020"
        "6f626a0a3c3c0a2f54797065202f466f6e7444657363726970"
        "746f720a2f466f6e744e616d65202f5253465331300a2f466c"
        "6167732033340a2f466f6e7442426f78205b30203020313030"
        "30203730305d0a2f417363656e7420300a2f43617048656967"
        "6874203730300a2f44657363656e7420300a2f4974616c6963"
        "416e676c6520300a2f5374656d5620300a2f58486569676874"
        "203233330a3e3e0a656e646f626a0a31352030206f626a0a5b"
        "3732322e325d0a656e646f626a0a31362030206f626a0a3c3c"
        "0a2f54797065202f466f6e7444657363726970746f720a2f46"
        "6f6e744e616d65202f4d53424d31300a2f466c616773203334"
        "0a2f466f6e7442426f78205b3020302031303030203638395d"
        "0a2f417363656e74203436340a2f4361704865696768742036"
        "38390a2f44657363656e7420300a2f4974616c6963416e676c"
        "6520300a2f5374656d562037340a2f58486569676874203436"
        "330a3e3e0a656e646f626a0a372030206f626a0a3c3c0a2f54"
        "797065202f466f6e740a2f53756274797065202f5479706531"
        "0a2f42617365466f6e74202f434d455831300a2f466f6e7444"
        "657363726970746f72203130203020520a2f46697273744368"
        "617220300a2f4c6173744368617220300a2f57696474687320"
        "39203020520a3e3e0a656e646f626a0a362030206f626a0a3c"
        "3c0a2f54797065202f466f6e740a2f53756274797065202f54"
        "797065310a2f42617365466f6e74202f434d5231300a2f466f"
        "6e7444657363726970746f72203132203020520a2f46697273"
        "74436861722036350a2f4c617374436861722036350a2f5769"
        "64746873203131203020520a3e3e0a656e646f626a0a342030"
        "206f626a0a3c3c0a2f54797065202f466f6e740a2f53756274",
        "797065202f54797065310a2f42617365466f6e74202f4d5342"
        "4d31300a2f466f6e7444657363726970746f72203136203020"
        "520a2f4669727374436861722036350a2f4c61737443686172"
        "2036350a2f576964746873203135203020520a3e3e0a656e64"
        "6f626a0a352030206f626a0a3c3c0a2f54797065202f466f6e"
        "740a2f53756274797065202f54797065310a2f42617365466f"
        "6e74202f5253465331300a2f466f6e7444657363726970746f"
        "72203134203020520a2f4669727374436861722036350a2f4c"
        "617374436861722036350a2f57696474687320313320302052"
        "0a3e3e0a656e646f626a0a382030206f626a0a3c3c0a2f5479"
        "7065202f50616765730a2f436f756e7420310a2f4b69647320"
        "5b32203020525d0a3e3e0a656e646f626a0a31372030206f62"
        "6a0a3c3c0a2f54797065202f436174616c6f670a2f50616765"
        "732038203020520a3e3e0a656e646f626a0a31382030206f62"
        "6a0a3c3c0a2f50726f647563657220287064665465582d312e"
        "34302e3235290a2f43726561746f722028546558290a2f5472"
        "6170706564202f46616c73650a3e3e0a656e646f626a0a7872"
        "65660a302031390a3030303030303030303020363535333520"
        "66200a30303030303030333235203030303030206e200a3030"
        "3030303030323133203030303030206e200a30303030303030"
        "303135203030303030206e200a303030303030313437322030"
        "30303030206e200a3030303030303136303420303030303020"
        "6e200a30303030303031333431203030303030206e200a3030"
        "3030303031323132203030303030206e200a30303030303031"
        "373336203030303030206e200a303030303030303432322030"
        "30303030206e200a3030303030303034343520303030303020"
        "6e200a30303030303030363231203030303030206e200a3030"
        "3030303030363433203030303030206e200a30303030303030"
        "383231203030303030206e200a303030303030303834352030"
        "30303030206e200a3030303030303130313520303030303020"
        "6e200a30303030303031303339203030303030206e200a3030"
        "3030303031373933203030303030206e200a30303030303031"
        "383433203030303030206e200a747261696c65720a3c3c202f"
        "53697a652031390a2f526f6f74203137203020520a2f496e66"
        "6f203138203020520a203e3e0a7374617274787265660a3139"
        "32330a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_what_a_string_escapes(void)
{
    /* A string writes the printable ASCII as it stands -- the delete at 127
       with it -- and escapes the rest as octal: every code up to and
       including the space, every code past 127, and the two brackets and the
       backslash that would otherwise be read as the string's own. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\font\\c=tcrm1095 \\hsize=16000"
        "pt \\vsize=300pt \\parindent=0pt\\shipout\\hbox{\\"
        "c \\char32 \\char126 \\char127 \\char128 \\char136"
        " \\char191 }\\shipout\\hbox{\\t \\char40 \\char41 "
        "\\char92 \\char0 \\char31 }\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820353520202020202020200a3e3e0a7374"
        "7265616d0a42540a2f46322031302e39303931205466203732"
        "203732205464205b285c3034307e7f5c3230305c3231305c32"
        "3737295d544a0a45540a0a656e6473747265616d0a656e646f"
        "626a0a322030206f626a0a3c3c0a2f54797065202f50616765"
        "0a2f436f6e74656e74732033203020520a2f5265736f757263"
        "65732031203020520a2f4d65646961426f78205b3020302031"
        "37382e3935203135312e3531335d0a2f506172656e74203520"
        "3020520a3e3e0a656e646f626a0a312030206f626a0a3c3c0a"
        "2f466f6e74203c3c202f4632203420302052203e3e0a2f5072"
        "6f63536574205b202f504446202f54657874205d0a3e3e0a65"
        "6e646f626a0a382030206f626a0a3c3c0a2f4c656e67746820"
        "363020202020202020200a3e3e0a73747265616d0a42540a2f"
        "463120392e393632362054662037322037342e343931205464"
        "205b285c3035305c3035315c3133345c3030305c303337295d"
        "544a0a45540a0a656e6473747265616d0a656e646f626a0a37"
        "2030206f626a0a3c3c0a2f54797065202f506167650a2f436f"
        "6e74656e74732038203020520a2f5265736f75726365732036"
        "203020520a2f4d65646961426f78205b302030203137302e37"
        "3035203135332e3936335d0a2f506172656e74203520302052"
        "0a3e3e0a656e646f626a0a362030206f626a0a3c3c0a2f466f"
        "6e74203c3c202f4631203920302052203e3e0a2f50726f6353"
        "6574205b202f504446202f54657874205d0a3e3e0a656e646f"
        "626a0a31302030206f626a0a5b363235203833332e33203737"
        "372e38203639342e34203636362e3720373530203732322e32"
        "203737372e38203732322e32203737372e38203732322e3220"
        "3538332e33203535352e36203535352e36203833332e332038"
        "33332e33203237372e38203330352e36203530302035303020"
        "353030203530302035303020373530203434342e3420353030"
        "203732322e32203737372e3820353030203930322e38203130"
        "31332e39203737372e38203237372e38203237372e38203530"
        "30203833332e3320353030203833332e33203737372e382032"
        "37372e38203338382e39203338382e3920353030203737372e"
        "38203237372e38203333332e33203237372e38203530302035"
        "30302035303020353030203530302035303020353030203530"
        "30203530302035303020353030203237372e38203237372e38"
        "203237372e38203737372e38203437322e32203437322e3220"
        "3737372e3820373530203730382e33203732322e3220373633"
        "2e39203638302e36203635322e38203738342e372037353020"
        "3336312e31203531332e39203737372e382036323520393136"
        "2e3720373530203737372e38203638302e36203737372e3820"
        "3733362e31203535352e36203732322e322037353020373530"
        "20313032372e382037353020373530203631312e3120323737"
        "2e38203530305d0a656e646f626a0a31312030206f626a0a3c"
        "3c0a2f54797065202f466f6e7444657363726970746f720a2f"
        "466f6e744e616d65202f434d5231300a2f466c616773203334"
        "0a2f466f6e7442426f78205b30202d31393420313030302036"
        "39345d0a2f417363656e74203639340a2f4361704865696768"
        "74203638330a2f44657363656e74202d3139340a2f4974616c"
        "6963416e676c6520300a2f5374656d562039330a2f58486569"
        "676874203433310a3e3e0a656e646f626a0a31322030206f62"
        "6a0a5b3535322e34203020302030203439372e322030203020"
        "3237362e3420302030203439372e322030203237362e342033"
        "33312e36203237362e34203439372e32203439372e32203439"
        "372e32203439372e32203439372e32203439372e3220343937"
        "2e32203439372e32203439372e32203439372e32203439372e"
        "3220302030203338362e38203737332e32203338362e382030"
        "20302030203020302030203020302030203020302030203020"
        "3020373138203020313130342e342030203020302030203020",
        "30203020373138203020302030203430302e36203020343030"
        "2e36203439372e32203439372e32203439372e322030203439"
        "372e32203439372e32203439372e3220302030203020302030"
        "2030203020393934203439372e32203630372e322030203020"
        "30203020302030203020302030203020302030203020302030"
        "203630372e3620313636203630372e36203630372e36203439"
        "372e32203439372e322034343220343432203439372e322031"
        "3135392e36203439372e32203933382e38203630372e362034"
        "39372e32203330342037313820313032312e33203737322e39"
        "20373830203639302e32203633352e32203733312e36203436"
        "392e36203436392e36203535322e34203636322e3820313530"
        "312e34203436392e3620373034203930362e32203439372e32"
        "203636382e37203439372e32203636322e38203335392e3220"
        "3335392e32203439372e32203633352e32203733362e312037"
        "34352e332031363620343432203439372e3220313130342e34"
        "2034343220313130342e34203636322e3820313130342e3420"
        "313130342e34203737332e32203333312e36203737332e3220"
        "34343220343432203439372e32203535322e34203630372e36"
        "203237362e34203632392e312034343220343432203436392e"
        "34203434322034343220343432203737332e325d0a656e646f"
        "626a0a31332030206f626a0a3c3c0a2f54797065202f466f6e"
        "7444657363726970746f720a2f466f6e744e616d65202f5443"
        "524d313039350a2f466c6167732033340a2f466f6e7442426f"
        "78205b3020302039393420305d0a2f417363656e7420300a2f"
        "43617048656967687420300a2f44657363656e7420300a2f49"
        "74616c6963416e676c6520300a2f5374656d562039320a2f58"
        "486569676874203433300a3e3e0a656e646f626a0a39203020"
        "6f626a0a3c3c0a2f54797065202f466f6e740a2f5375627479"
        "7065202f54797065310a2f42617365466f6e74202f434d5231"
        "300a2f466f6e7444657363726970746f72203131203020520a"
        "2f46697273744368617220300a2f4c61737443686172203932"
        "0a2f576964746873203130203020520a3e3e0a656e646f626a"
        "0a342030206f626a0a3c3c0a2f54797065202f466f6e740a2f"
        "53756274797065202f54797065310a2f42617365466f6e7420"
        "2f5443524d313039350a2f466f6e7444657363726970746f72"
        "203133203020520a2f4669727374436861722033320a2f4c61"
        "737443686172203139310a2f57696474687320313220302052"
        "0a3e3e0a656e646f626a0a352030206f626a0a3c3c0a2f5479"
        "7065202f50616765730a2f436f756e7420320a2f4b69647320"
        "5b32203020522037203020525d0a3e3e0a656e646f626a0a31"
        "342030206f626a0a3c3c0a2f54797065202f436174616c6f67"
        "0a2f50616765732035203020520a3e3e0a656e646f626a0a31"
        "352030206f626a0a3c3c0a2f50726f64756365722028706466"
        "5465582d312e34302e3235290a2f43726561746f7220285465"
        "58290a2f54726170706564202f46616c73650a3e3e0a656e64"
        "6f626a0a787265660a302031360a3030303030303030303020"
        "36353533352066200a30303030303030323339203030303030"
        "206e200a30303030303030313238203030303030206e200a30"
        "303030303030303135203030303030206e200a303030303030"
        "32333031203030303030206e200a3030303030303234333620"
        "3030303030206e200a30303030303030353336203030303030"
        "206e200a30303030303030343234203030303030206e200a30"
        "303030303030333036203030303030206e200a303030303030"
        "32313731203030303030206e200a3030303030303036303320"
        "3030303030206e200a30303030303031313135203030303030"
        "206e200a30303030303031323933203030303030206e200a30"
        "303030303032303033203030303030206e200a303030303030"
        "32343939203030303030206e200a3030303030303235343920"
        "3030303030206e200a747261696c65720a3c3c202f53697a65"
        "2031360a2f526f6f74203134203020520a2f496e666f203135",
        "203020520a203e3e0a7374617274787265660a323632390a25"
        "25454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_pdf_file(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\tenrm=cmr10 \\font\\tenbf=cmbx10 \\tenrm"
        " \\hsize=200pt \\vsize=300pt \\parindent=0pt \\bas"
        "elineskip=12pt \\boxmaxdepth=0pt \\hbadness=10000 "
        "\\shipout\\vbox{\\hbox{ab \\tenbf cd \\tenrm ef}\\"
        "hbox{a\\kern 3pt b\\hskip 5pt c}\\hrule height 2pt"
        " \\hbox to 150pt{x\\hfil y}\\hbox{\\vrule width 3p"
        "t height 4pt depth 1pt z}}\\shipout\\hbox{\\tenbf "
        "q}",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820333336202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3130332e313839205464205b286162295d544a2f463220392e"
        "393632362054662031332e3833372030205464205b28636429"
        "5d544a2f463120392e393632362054662031352e3237362030"
        "205464205b286566295d544a202d32392e313133202d31312e"
        "393536205464205b2861292d3330302862292d353030286329"
        "5d544a0a45540a710a312030203020312037322038392e3234"
        "3120636d0a302030203134392e343420312e39393320726520"
        "660a510a42540a2f463120392e393632362054662037322038"
        "342e393531205464205b2878292d31333934342879295d544a"
        "0a45540a710a3120302030203120373220373220636d0a3020"
        "3020322e39383920342e39383120726520660a510a42540a2f"
        "463120392e393632362054662037342e3938392037322e3939"
        "36205464205b287a295d544a0a45540a0a656e647374726561"
        "6d0a656e646f626a0a322030206f626a0a3c3c0a2f54797065"
        "202f506167650a2f436f6e74656e74732033203020520a2f52"
        "65736f75726365732031203020520a2f4d65646961426f7820"
        "5b302030203239332e3434203138322e3130375d0a2f506172"
        "656e742036203020520a3e3e0a656e646f626a0a312030206f"
        "626a0a3c3c0a2f466f6e74203c3c202f463120342030205220"
        "2f4632203520302052203e3e0a2f50726f63536574205b202f"
        "504446202f54657874205d0a3e3e0a656e646f626a0a392030"
        "206f626a0a3c3c0a2f4c656e67746820343120202020202020"
        "200a3e3e0a73747265616d0a42540a2f463220392e39363236"
        "2054662037322037332e393337205464205b2871295d544a0a"
        "45540a0a656e6473747265616d0a656e646f626a0a38203020"
        "6f626a0a3c3c0a2f54797065202f506167650a2f436f6e7465"
        "6e74732039203020520a2f5265736f75726365732037203020"
        "520a2f4d65646961426f78205b302030203135302e30343720"
        "3135302e3336355d0a2f506172656e742036203020520a3e3e"
        "0a656e646f626a0a372030206f626a0a3c3c0a2f466f6e7420"
        "3c3c202f4632203520302052203e3e0a2f50726f6353657420"
        "5b202f504446202f54657874205d0a3e3e0a656e646f626a0a"
        "31302030206f626a0a5b3531312e31203633382e3920353237"
        "2e31203335312e3420353735203633382e39203331392e3420"
        "3335312e34203630362e39203331392e34203935382e332036"
        "33382e3920353735203633382e39203630362e395d0a656e64"
        "6f626a0a31312030206f626a0a3c3c0a2f54797065202f466f"
        "6e7444657363726970746f720a2f466f6e744e616d65202f43"
        "4d425831300a2f466c6167732033340a2f466f6e7442426f78"
        "205b30202d3139342031313530203639345d0a2f417363656e"
        "74203639340a2f436170486569676874203638360a2f446573"
        "63656e74202d3139340a2f4974616c6963416e676c6520300a"
        "2f5374656d56203130360a2f58486569676874203434340a3e"
        "3e0a656e646f626a0a31322030206f626a0a5b353030203535"
        "352e36203434342e34203535352e36203434342e3420333035"
        "2e3620353030203535352e36203237372e38203330352e3620"
        "3532372e38203237372e38203833332e33203535352e362035"
        "3030203535352e36203532372e38203339312e37203339342e"
        "34203338382e39203535352e36203532372e38203732322e32"
        "203532372e38203532372e38203434342e345d0a656e646f62"
        "6a0a31332030206f626a0a3c3c0a2f54797065202f466f6e74"
        "44657363726970746f720a2f466f6e744e616d65202f434d52"
        "31300a2f466c6167732033340a2f466f6e7442426f78205b30"
        "202d3139342031303030203639345d0a2f417363656e742036"
        "39340a2f436170486569676874203638330a2f44657363656e"
        "74202d3139340a2f4974616c6963416e676c6520300a2f5374"
        "656d562039330a2f58486569676874203433310a3e3e0a656e",
        "646f626a0a352030206f626a0a3c3c0a2f54797065202f466f"
        "6e740a2f53756274797065202f54797065310a2f4261736546"
        "6f6e74202f434d425831300a2f466f6e744465736372697074"
        "6f72203131203020520a2f4669727374436861722039390a2f"
        "4c61737443686172203131330a2f5769647468732031302030"
        "20520a3e3e0a656e646f626a0a342030206f626a0a3c3c0a2f"
        "54797065202f466f6e740a2f53756274797065202f54797065"
        "310a2f42617365466f6e74202f434d5231300a2f466f6e7444"
        "657363726970746f72203133203020520a2f46697273744368"
        "61722039370a2f4c61737443686172203132320a2f57696474"
        "6873203132203020520a3e3e0a656e646f626a0a362030206f"
        "626a0a3c3c0a2f54797065202f50616765730a2f436f756e74"
        "20320a2f4b696473205b32203020522038203020525d0a3e3e"
        "0a656e646f626a0a31342030206f626a0a3c3c0a2f54797065"
        "202f436174616c6f670a2f50616765732036203020520a3e3e"
        "0a656e646f626a0a31352030206f626a0a3c3c0a2f50726f64"
        "7563657220287064665465582d312e34302e3235290a2f4372"
        "6561746f722028546558290a2f54726170706564202f46616c"
        "73650a3e3e0a656e646f626a0a787265660a302031360a3030"
        "30303030303030302036353533352066200a30303030303030"
        "353230203030303030206e200a303030303030303430392030"
        "30303030206e200a3030303030303030313520303030303020"
        "6e200a30303030303031363338203030303030206e200a3030"
        "3030303031353035203030303030206e200a30303030303031"
        "373730203030303030206e200a303030303030303830382030"
        "30303030206e200a3030303030303036393620303030303020"
        "6e200a30303030303030353937203030303030206e200a3030"
        "3030303030383735203030303030206e200a30303030303030"
        "393739203030303030206e200a303030303030313135392030"
        "30303030206e200a3030303030303133323720303030303020"
        "6e200a30303030303031383333203030303030206e200a3030"
        "3030303031383833203030303030206e200a747261696c6572"
        "0a3c3c202f53697a652031360a2f526f6f7420313420302052"
        "0a2f496e666f203135203020520a203e3e0a73746172747872"
        "65660a313936330a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* A link and an annotation stand in the list where they were written, and
   the reference shows them with their size, what they carry and where they
   lead. See docs/DECISIONS.md, pdf-links. */
static int test_a_link_in_a_list(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\pdfoutput=1 \\setbox0"
        "=\\hbox{\\pdfstartlink attr{/A}goto name{n1}a\\pdf"
        "endlink}\\showbox0 \\setbox0=\\hbox{\\pdfstartlink"
        " height 5pt depth 1pt width 10pt attr{/A}goto num "
        "7 a\\pdfendlink}\\showbox0 \\setbox0=\\hbox{\\pdfs"
        "tartlink user{/Subtype/Link}a\\pdfendlink}\\showbo"
        "x0 \\setbox0=\\hbox{\\pdfstartlink goto file{f.pdf"
        "}name{n2}a\\pdfendlink}\\showbox0 \\setbox0=\\hbox"
        "{\\pdfstartlink goto file{f.pdf}page 3 {/Fit}a\\pd"
        "fendlink}\\showbox0 \\setbox0=\\hbox{\\pdfstartlin"
        "k thread name{t1}a\\pdfendlink}\\showbox0 \\setbox"
        "0=\\hbox{\\pdfstartlink goto page 3 {/Fit}a\\pdfen"
        "dlink}\\showbox0 \\setbox0=\\hbox{\\pdfstartlink t"
        "hread num 3 a\\pdfendlink}\\showbox0 \\setbox0=\\h"
        "box{a\\pdfannot height 5pt depth 1pt width 10pt {/"
        "Subtype/Text}b}\\showbox0 \\setbox0=\\hbox{a\\pdfa"
        "nnot{/Subtype/Text}b}\\showbox0 \\setbox0=\\hbox{a"
        "\\pdfdest name{d} fitr width 3pt height 4pt b}\\sh"
        "owbox0 \\setbox0=\\hbox{a\\pdfdest name{d} xyz zoo"
        "m 1000 b}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n> \\box0=\n\\hbox(4.30554+0.0)x5.00002\n.\\p"
        "dfstartlink(*+*)x* attr{/A} action goto name{n1}\n"
        ".\\tenrm a\n.\\pdfendlink\n\n! OK.\nl.1 ...ttr{/A}"
        "goto name{n1}a\\pdfendlink}\\showbox0 \n          "
        "                                        \\setbox0="
        "\\hbox{\\pdfstartli...\n\n\n> \\box0=\n\\hbox(4.30"
        "554+0.0)x5.00002\n.\\pdfstartlink(5.0+1.0)x10.0 at"
        "tr{/A} action goto num7\n.\\tenrm a\n.\\pdfendlink"
        "\n\n! OK.\nl.1 ... attr{/A}goto num 7 a\\pdfendlin"
        "k}\\showbox0 \n                                   "
        "               \\setbox0=\\hbox{\\pdfstartli...\n"
        "\n\n> \\box0=\n\\hbox(4.30554+0.0)x5.00002\n.\\pdf"
        "startlink(*+*)x* action user{/Subtype/Link}\n.\\te"
        "nrm a\n.\\pdfendlink\n\n! OK.\nl.1 ... user{/Subty"
        "pe/Link}a\\pdfendlink}\\showbox0 \n               "
        "                                   \\setbox0=\\hbo"
        "x{\\pdfstartli...\n\n\n> \\box0=\n\\hbox(4.30554+0"
        ".0)x5.00002\n.\\pdfstartlink(*+*)x* action file{f."
        "pdf} goto name{n2}\n.\\tenrm a\n.\\pdfendlink\n\n!"
        " OK.\nl.1 ... file{f.pdf}name{n2}a\\pdfendlink}\\s"
        "howbox0 \n                                        "
        "          \\setbox0=\\hbox{\\pdfstartli...\n\n\n> "
        "\\box0=\n\\hbox(4.30554+0.0)x5.00002\n.\\pdfstartl"
        "ink(*+*)x* action file{f.pdf} page3{/Fit}\n.\\tenr"
        "m a\n.\\pdfendlink\n\n! OK.\nl.1 ...{f.pdf}page 3 "
        "{/Fit}a\\pdfendlink}\\showbox0 \n                 "
        "                                 \\setbox0=\\hbox{"
        "\\pdfstartli...\n\n\n> \\box0=\n\\hbox(4.30554+0.0"
        ")x5.00002\n.\\pdfstartlink(*+*)x* action thread na"
        "me{t1}\n.\\tenrm a\n.\\pdfendlink\n\n! OK.\nl.1 .."
        ".link thread name{t1}a\\pdfendlink}\\showbox0 \n  "
        "                                                \\"
        "setbox0=\\hbox{\\pdfstartli...\n\n\n> \\box0=\n\\h"
        "box(4.30554+0.0)x5.00002\n.\\pdfstartlink(*+*)x* a"
        "ction page3{/Fit}\n.\\tenrm a\n.\\pdfendlink\n\n! "
        "OK.\nl.1 ...k goto page 3 {/Fit}a\\pdfendlink}\\sh"
        "owbox0 \n                                         "
        "         \\setbox0=\\hbox{\\pdfstartli...\n\n\n> "
        "\\box0=\n\\hbox(4.30554+0.0)x5.00002\n.\\pdfstartl"
        "ink(*+*)x* action thread num3\n.\\tenrm a\n.\\pdfe"
        "ndlink\n\n! OK.\nl.1 ...rtlink thread num 3 a\\pdf"
        "endlink}\\showbox0 \n                             "
        "                     \\setbox0=\\hbox{a\\pdfannot "
        "...\n\n\n> \\box0=\n\\hbox(6.94444+0.0)x10.55559\n"
        ".\\tenrm a\n.\\pdfannot(5.0+1.0)x10.0{/Subtype/Tex"
        "t}\n.\\tenrm b\n\n! OK.\nl.1 ... 1pt width 10pt {/"
        "Subtype/Text}b}\\showbox0 \n                      "
        "                            \\setbox0=\\hbox{a\\pd"
        "fannot{...\n\n\n> \\box0=\n\\hbox(6.94444+0.0)x10."
        "55559\n.\\tenrm a\n.\\pdfannot(*+*)x*{/Subtype/Tex"
        "t}\n.\\tenrm b\n\n! OK.\nl.1 ...\\hbox{a\\pdfannot"
        "{/Subtype/Text}b}\\showbox0 \n                    "
        "                              \\setbox0=\\hbox{a\\"
        "pdfdest n...\n\n\n> \\box0=\n\\hbox(6.94444+0.0)x1"
        "0.55559\n.\\tenrm a\n.\\pdfdest name{d} fitr(4.0+*"
        ")x3.0\n.\\tenrm b\n\n! OK.\nl.1 ...e{d} fitr width"
        " 3pt height 4pt b}\\showbox0 \n                   "
        "                               \\setbox0=\\hbo",
        "x{a\\pdfdest n...\n\n\n> \\box0=\n\\hbox(6.94444+0"
        ".0)x10.55559\n.\\tenrm a\n.\\pdfdest name{d} xyz z"
        "oom1000\n.\\tenrm b\n\n! OK.\nl.1 ...\\pdfdest nam"
        "e{d} xyz zoom 1000 b}\\showbox0 \n                "
        "                                  \\showbox254\n\n"
        "> \\box254=void\n\n! OK.\nl.1 ...me{d} xyz zoom 10"
        "00 b}\\showbox0 \\showbox254\n                    "
        "                              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* What the document itself writes into a page: the colour stacks, which say
   at the top of a page what colour it starts in, and the literals, which go
   in where they stand, in the text or outside it. See docs/DECISIONS.md,
   colour-on-a-page. */
/* \end in the middle of a paragraph ends it the way \hrule and the rest
   do -- by putting the \par token in front of itself -- and then ejects
   what is left; see docs/DECISIONS.md, ending-a-paragraph. */
static int test_a_paragraph_the_end_ends(void)
{
    static const char *const source[] = {
        "\\pdfoutput=0 \\year=2026 \\month=8 \\day=18 \\tim"
        "e=1117 \\font\\tenrm=cmr10 \\tenrm \\hsize=200pt "
        "\\vsize=200pt \\parindent=0pt \\parfillskip=0pt pl"
        "us1fil \\hbadness=10000 \\vbadness=10000 \\let\\en"
        "dgraf\\par \\def\\par{r\\endgraf}q\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "f702018392c01c3b0000000003e81b20546558206f75747075"
        "7420323032362e30382e31383a313833378b00000000000000"
        "00000000000000000000000000000000000000000000000000"
        "0000000000000000ffffffff9f044e388df3004bf16079000a"
        "0000000a00000005636d723130ab7191035555728e8cf80000"
        "002a018392c01c3b0000000003e800c8000000c80000000100"
        "01f3004bf16079000a0000000a00000005636d723130f90000"
        "007a02dfdfdfdfdfdf",
        NULL,
    };
    return run_document_dvi(source, expected);
}

/* A link, an annotation and a destination stand on the page as rectangles
   the page names, the annotations before the links, and a link broken over
   two lines gets a rectangle for each; see docs/DECISIONS.md,
   annotations-on-a-page. */
static int test_annotations_on_a_page(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\tenrm=cmr10 \\tenrm \\hsize=200pt \\vsiz"
        "e=300pt \\parindent=0pt \\hbadness=10000 \\vbadnes"
        "s=10000 \\pdflinkmargin=2pt \\shipout\\hbox{a\\pdf"
        "startlink attr{/Border[0 0 0]}user{/Subtype/Link}b"
        "\\pdfendlink c\\pdfannot width 7pt height 3pt dept"
        "h 2pt {/Subtype/Text}d}\\shipout\\vbox{\\hbox{a\\p"
        "dfstartlink goto name{d1}b}\\hbox{c\\pdfendlink}}"
        "\\pdflinkmargin=0pt \\shipout\\hbox{a\\pdfdest nam"
        "e{d1} xyz b\\pdfdest name{d2} fitr width 20pt heig"
        "ht 10pt depth 3pt c}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a352030206f626a0a3c3c"
        "0a2f4c656e67746820343020202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b2861626364295d544a0a45540a0a656e6473"
        "747265616d0a656e646f626a0a342030206f626a0a3c3c0a2f"
        "54797065202f506167650a2f436f6e74656e74732035203020"
        "520a2f5265736f75726365732033203020520a2f4d65646961"
        "426f78205b302030203136342e343739203135302e3931395d"
        "0a2f506172656e742037203020520a2f416e6e6f7473205b20"
        "3220302052203120302052205d0a3e3e0a656e646f626a0a32"
        "2030206f626a0a3c3c0a2f54797065202f416e6e6f740a2f53"
        "7562747970652f546578740a2f52656374205b38362e393434"
        "2037302e3030372039332e3931382037342e3938395d0a3e3e"
        "0a656e646f626a0a312030206f626a0a3c3c0a2f5479706520"
        "2f416e6e6f740a2f426f726465725b30203020305d0a2f5265"
        "6374205b37342e3938392037302e3030372038342e35303920"
        "38302e3931315d0a2f537562747970652f4c696e6b0a3e3e0a"
        "656e646f626a0a332030206f626a0a3c3c0a2f466f6e74203c"
        "3c202f4631203620302052203e3e0a2f50726f63536574205b"
        "202f504446202f54657874205d0a3e3e0a656e646f626a0a31"
        "312030206f626a0a3c3c0a2f4c656e67746820363220202020"
        "202020200a3e3e0a73747265616d0a42540a2f463120392e39"
        "3632362054662037322037362e323839205464205b28616229"
        "5d544a2030202d342e323839205464205b2863295d544a0a45"
        "540a0a656e6473747265616d0a656e646f626a0a3130203020"
        "6f626a0a3c3c0a2f54797065202f506167650a2f436f6e7465"
        "6e7473203131203020520a2f5265736f757263657320392030"
        "20520a2f4d65646961426f78205b302030203135342e353136"
        "203135352e3230385d0a2f506172656e742037203020520a2f"
        "416e6e6f7473205b20382030205220313220302052205d0a3e"
        "3e0a656e646f626a0a382030206f626a0a3c3c0a2f54797065"
        "202f416e6e6f740a2f53756274797065202f4c696e6b0a2f52"
        "656374205b37342e3938392037342e3239372038342e353039"
        "2038352e325d0a2f41203c3c202f53202f476f546f202f4420"
        "28643129203e3e0a3e3e0a656e646f626a0a31322030206f62"
        "6a0a3c3c0a2f54797065202f416e6e6f740a2f537562747970"
        "65202f4c696e6b0a2f52656374205b37302e3030372037302e"
        "3030372037382e34322037382e3238325d0a2f41203c3c202f"
        "53202f476f546f202f442028643129203e3e0a3e3e0a656e64"
        "6f626a0a392030206f626a0a3c3c0a2f466f6e74203c3c202f"
        "4631203620302052203e3e0a2f50726f63536574205b202f50"
        "4446202f54657874205d0a3e3e0a656e646f626a0a31362030"
        "206f626a0a3c3c0a2f4c656e67746820333920202020202020"
        "200a3e3e0a73747265616d0a42540a2f463120392e39363236"
        "205466203732203732205464205b28616263295d544a0a4554"
        "0a0a656e6473747265616d0a656e646f626a0a31352030206f"
        "626a0a3c3c0a2f54797065202f506167650a2f436f6e74656e"
        "7473203136203020520a2f5265736f75726365732031342030"
        "20520a2f4d65646961426f78205b302030203135382e393434"
        "203135302e3931395d0a2f506172656e742037203020520a3e"
        "3e0a656e646f626a0a31332030206f626a0a3c3c0a2f44205b"
        "313520302052202f58595a2037362e393831203732206e756c"
        "6c5d0a3e3e0a656e646f626a0a31372030206f626a0a3c3c0a"
        "2f44205b313520302052202f466974522038322e3531362036"
        "392e303131203130322e3434312038312e3936335d0a3e3e0a"
        "656e646f626a0a31342030206f626a0a3c3c0a2f466f6e7420"
        "3c3c202f4631203620302052203e3e0a2f50726f6353657420"
        "5b202f504446202f54657874205d0a3e3e0a656e646f626a0a"
        "31382030206f626a0a5b353030203535352e36203434342e34"
        "203535352e365d0a656e646f626a0a31392030206f626a0a3c",
        "3c0a2f54797065202f466f6e7444657363726970746f720a2f"
        "466f6e744e616d65202f434d5231300a2f466c616773203334"
        "0a2f466f6e7442426f78205b30202d31393420313030302036"
        "39345d0a2f417363656e74203639340a2f4361704865696768"
        "74203638330a2f44657363656e74202d3139340a2f4974616c"
        "6963416e676c6520300a2f5374656d562039330a2f58486569"
        "676874203433310a3e3e0a656e646f626a0a362030206f626a"
        "0a3c3c0a2f54797065202f466f6e740a2f5375627479706520"
        "2f54797065310a2f42617365466f6e74202f434d5231300a2f"
        "466f6e7444657363726970746f72203139203020520a2f4669"
        "727374436861722039370a2f4c61737443686172203130300a"
        "2f576964746873203138203020520a3e3e0a656e646f626a0a"
        "372030206f626a0a3c3c0a2f54797065202f50616765730a2f"
        "436f756e7420330a2f4b696473205b34203020522031302030"
        "2052203135203020525d0a3e3e0a656e646f626a0a32302030"
        "206f626a0a3c3c0a2f4e616d6573205b286431292031332030"
        "20522028643229203137203020525d0a2f4c696d697473205b"
        "2864312920286432295d0a3e3e0a656e646f626a0a32312030"
        "206f626a0a3c3c0a2f4465737473203230203020520a3e3e0a"
        "656e646f626a0a32322030206f626a0a3c3c0a2f5479706520"
        "2f436174616c6f670a2f50616765732037203020520a2f4e61"
        "6d6573203231203020520a3e3e0a656e646f626a0a32332030"
        "206f626a0a3c3c0a2f50726f64756365722028706466546558"
        "2d312e34302e3235290a2f43726561746f722028546558290a"
        "2f54726170706564202f46616c73650a3e3e0a656e646f626a"
        "0a787265660a302032340a3030303030303030303020363535"
        "33352066200a30303030303030333333203030303030206e20"
        "0a30303030303030323439203030303030206e200a30303030"
        "303030343332203030303030206e200a303030303030303131"
        "33203030303030206e200a3030303030303030313520303030"
        "3030206e200a30303030303031363638203030303030206e20"
        "0a30303030303031383030203030303030206e200a30303030"
        "303030373539203030303030206e200a303030303030303937"
        "39203030303030206e200a3030303030303036323020303030"
        "3030206e200a30303030303030343939203030303030206e20"
        "0a30303030303030383638203030303030206e200a30303030"
        "303031323539203030303030206e200a303030303030313338"
        "32203030303030206e200a3030303030303131343420303030"
        "3030206e200a30303030303031303436203030303030206e20"
        "0a30303030303031333133203030303030206e200a30303030"
        "303031343530203030303030206e200a303030303030313439"
        "30203030303030206e200a3030303030303138373120303030"
        "3030206e200a30303030303031393436203030303030206e20"
        "0a30303030303031393832203030303030206e200a30303030"
        "303032303436203030303030206e200a747261696c65720a3c"
        "3c202f53697a652032340a2f526f6f74203232203020520a2f"
        "496e666f203233203020520a203e3e0a737461727478726566"
        "0a323132360a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* Every kind of destination, and the tree of names the catalog points at;
   see docs/DECISIONS.md, destinations-in-the-file. */
static int test_destinations_in_the_file(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\tenrm=cmr10 \\tenrm \\hsize=200pt \\vsiz"
        "e=300pt \\parindent=0pt \\hbadness=10000 \\vbadnes"
        "s=10000 \\shipout\\hbox{a\\pdfdest name{n3} fit b}"
        "\\shipout\\hbox{a\\pdfdest name{n1} xyz zoom 500 b"
        "}\\shipout\\hbox{a\\pdfdest name{n2} fith b}\\ship"
        "out\\hbox{a\\pdfdest name{n7} fitbv b}\\shipout\\h"
        "box{a\\pdfdest num 5 xyz b}\\shipout\\hbox{a\\pdfs"
        "tartlink goto name{n9}b\\pdfendlink}\\shipout\\hbo"
        "x{a\\pdfdest name{n4} fitb b}\\shipout\\hbox{a\\pd"
        "fdest name{n5} fitv b}\\shipout\\hbox{a\\pdfdest n"
        "ame{n6} fitbh b}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820333820202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b286162295d544a0a45540a0a656e64737472"
        "65616d0a656e646f626a0a322030206f626a0a3c3c0a2f5479"
        "7065202f506167650a2f436f6e74656e74732033203020520a"
        "2f5265736f75726365732031203020520a2f4d65646961426f"
        "78205b302030203135342e353136203135302e3931395d0a2f"
        "506172656e742036203020520a3e3e0a656e646f626a0a3520"
        "30206f626a0a3c3c0a2f44205b3220302052202f4669745d0a"
        "3e3e0a656e646f626a0a312030206f626a0a3c3c0a2f466f6e"
        "74203c3c202f4631203420302052203e3e0a2f50726f635365"
        "74205b202f504446202f54657874205d0a3e3e0a656e646f62"
        "6a0a392030206f626a0a3c3c0a2f4c656e6774682033382020"
        "2020202020200a3e3e0a73747265616d0a42540a2f46312039"
        "2e39363236205466203732203732205464205b286162295d54"
        "4a0a45540a0a656e6473747265616d0a656e646f626a0a3820"
        "30206f626a0a3c3c0a2f54797065202f506167650a2f436f6e"
        "74656e74732039203020520a2f5265736f7572636573203720"
        "3020520a2f4d65646961426f78205b302030203135342e3531"
        "36203135302e3931395d0a2f506172656e742036203020520a"
        "3e3e0a656e646f626a0a31302030206f626a0a3c3c0a2f4420"
        "5b3820302052202f58595a2037362e39383120373220302e35"
        "30305d0a3e3e0a656e646f626a0a372030206f626a0a3c3c0a"
        "2f466f6e74203c3c202f4631203420302052203e3e0a2f5072"
        "6f63536574205b202f504446202f54657874205d0a3e3e0a65"
        "6e646f626a0a31332030206f626a0a3c3c0a2f4c656e677468"
        "20333820202020202020200a3e3e0a73747265616d0a42540a"
        "2f463120392e39363236205466203732203732205464205b28"
        "6162295d544a0a45540a0a656e6473747265616d0a656e646f"
        "626a0a31322030206f626a0a3c3c0a2f54797065202f506167"
        "650a2f436f6e74656e7473203133203020520a2f5265736f75"
        "72636573203131203020520a2f4d65646961426f78205b3020"
        "30203135342e353136203135302e3931395d0a2f506172656e"
        "742036203020520a3e3e0a656e646f626a0a31342030206f62"
        "6a0a3c3c0a2f44205b313220302052202f466974482037325d"
        "0a3e3e0a656e646f626a0a31312030206f626a0a3c3c0a2f46"
        "6f6e74203c3c202f4631203420302052203e3e0a2f50726f63"
        "536574205b202f504446202f54657874205d0a3e3e0a656e64"
        "6f626a0a31372030206f626a0a3c3c0a2f4c656e6774682033"
        "3820202020202020200a3e3e0a73747265616d0a42540a2f46"
        "3120392e39363236205466203732203732205464205b286162"
        "295d544a0a45540a0a656e6473747265616d0a656e646f626a"
        "0a31362030206f626a0a3c3c0a2f54797065202f506167650a"
        "2f436f6e74656e7473203137203020520a2f5265736f757263"
        "6573203135203020520a2f4d65646961426f78205b30203020"
        "3135342e353136203135302e3931395d0a2f506172656e7420"
        "36203020520a3e3e0a656e646f626a0a31382030206f626a0a"
        "3c3c0a2f44205b313620302052202f46697442562037362e39"
        "38315d0a3e3e0a656e646f626a0a31352030206f626a0a3c3c"
        "0a2f466f6e74203c3c202f4631203420302052203e3e0a2f50"
        "726f63536574205b202f504446202f54657874205d0a3e3e0a"
        "656e646f626a0a32312030206f626a0a3c3c0a2f4c656e6774"
        "6820333820202020202020200a3e3e0a73747265616d0a4254"
        "0a2f463120392e39363236205466203732203732205464205b"
        "286162295d544a0a45540a0a656e6473747265616d0a656e64"
        "6f626a0a32302030206f626a0a3c3c0a2f54797065202f5061"
        "67650a2f436f6e74656e7473203231203020520a2f5265736f"
        "7572636573203139203020520a2f4d65646961426f78205b30"
        "2030203135342e353136203135302e3931395d0a2f50617265",
        "6e742036203020520a3e3e0a656e646f626a0a32322030206f"
        "626a0a5b323020302052202f58595a2037362e393831203732"
        "206e756c6c5d0a656e646f626a0a31392030206f626a0a3c3c"
        "0a2f466f6e74203c3c202f4631203420302052203e3e0a2f50"
        "726f63536574205b202f504446202f54657874205d0a3e3e0a"
        "656e646f626a0a32362030206f626a0a3c3c0a2f4c656e6774"
        "6820333820202020202020200a3e3e0a73747265616d0a4254"
        "0a2f463120392e39363236205466203732203732205464205b"
        "286162295d544a0a45540a0a656e6473747265616d0a656e64"
        "6f626a0a32352030206f626a0a3c3c0a2f54797065202f5061"
        "67650a2f436f6e74656e7473203236203020520a2f5265736f"
        "7572636573203234203020520a2f4d65646961426f78205b30"
        "2030203135342e353136203135302e3931395d0a2f50617265"
        "6e742036203020520a2f416e6e6f7473205b20323320302052"
        "205d0a3e3e0a656e646f626a0a32332030206f626a0a3c3c0a"
        "2f54797065202f416e6e6f740a2f53756274797065202f4c69"
        "6e6b0a2f52656374205b37362e3938312037322038322e3531"
        "362037382e3931395d0a2f41203c3c202f53202f476f546f20"
        "2f4420286e3929203e3e0a3e3e0a656e646f626a0a32342030"
        "206f626a0a3c3c0a2f466f6e74203c3c202f46312034203020"
        "52203e3e0a2f50726f63536574205b202f504446202f546578"
        "74205d0a3e3e0a656e646f626a0a33302030206f626a0a3c3c"
        "0a2f4c656e67746820333820202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b286162295d544a0a45540a0a656e64737472"
        "65616d0a656e646f626a0a32392030206f626a0a3c3c0a2f54"
        "797065202f506167650a2f436f6e74656e7473203330203020"
        "520a2f5265736f7572636573203238203020520a2f4d656469"
        "61426f78205b302030203135342e353136203135302e393139"
        "5d0a2f506172656e74203332203020520a3e3e0a656e646f62"
        "6a0a33312030206f626a0a3c3c0a2f44205b32392030205220"
        "2f466974425d0a3e3e0a656e646f626a0a32382030206f626a"
        "0a3c3c0a2f466f6e74203c3c202f4631203420302052203e3e"
        "0a2f50726f63536574205b202f504446202f54657874205d0a"
        "3e3e0a656e646f626a0a33352030206f626a0a3c3c0a2f4c65"
        "6e67746820333820202020202020200a3e3e0a73747265616d"
        "0a42540a2f463120392e393632362054662037322037322054"
        "64205b286162295d544a0a45540a0a656e6473747265616d0a"
        "656e646f626a0a33342030206f626a0a3c3c0a2f5479706520"
        "2f506167650a2f436f6e74656e7473203335203020520a2f52"
        "65736f7572636573203333203020520a2f4d65646961426f78"
        "205b302030203135342e353136203135302e3931395d0a2f50"
        "6172656e74203332203020520a3e3e0a656e646f626a0a3336"
        "2030206f626a0a3c3c0a2f44205b333420302052202f466974"
        "562037362e3938315d0a3e3e0a656e646f626a0a3333203020"
        "6f626a0a3c3c0a2f466f6e74203c3c202f4631203420302052"
        "203e3e0a2f50726f63536574205b202f504446202f54657874"
        "205d0a3e3e0a656e646f626a0a33392030206f626a0a3c3c0a"
        "2f4c656e67746820333820202020202020200a3e3e0a737472"
        "65616d0a42540a2f463120392e393632362054662037322037"
        "32205464205b286162295d544a0a45540a0a656e6473747265"
        "616d0a656e646f626a0a33382030206f626a0a3c3c0a2f5479"
        "7065202f506167650a2f436f6e74656e747320333920302052"
        "0a2f5265736f7572636573203337203020520a2f4d65646961"
        "426f78205b302030203135342e353136203135302e3931395d"
        "0a2f506172656e74203332203020520a3e3e0a656e646f626a"
        "0a34302030206f626a0a3c3c0a2f44205b333820302052202f"
        "46697442482037325d0a3e3e0a656e646f626a0a3337203020"
        "6f626a0a3c3c0a2f466f6e74203c3c202f4631203420302052"
        "203e3e0a2f50726f63536574205b202f504446202f54657874",
        "205d0a3e3e0a656e646f626a0a32372030206f626a0a5b3220"
        "302052202f4669745d0a656e646f626a0a34312030206f626a"
        "0a5b353030203535352e365d0a656e646f626a0a3432203020"
        "6f626a0a3c3c0a2f54797065202f466f6e7444657363726970"
        "746f720a2f466f6e744e616d65202f434d5231300a2f466c61"
        "67732033340a2f466f6e7442426f78205b30202d3139342031"
        "303030203639345d0a2f417363656e74203639340a2f436170"
        "486569676874203638330a2f44657363656e74202d3139340a"
        "2f4974616c6963416e676c6520300a2f5374656d562039330a"
        "2f58486569676874203433310a3e3e0a656e646f626a0a3420"
        "30206f626a0a3c3c0a2f54797065202f466f6e740a2f537562"
        "74797065202f54797065310a2f42617365466f6e74202f434d"
        "5231300a2f466f6e7444657363726970746f72203432203020"
        "520a2f4669727374436861722039370a2f4c61737443686172"
        "2039380a2f576964746873203431203020520a3e3e0a656e64"
        "6f626a0a362030206f626a0a3c3c0a2f54797065202f506167"
        "65730a2f436f756e7420360a2f506172656e74203433203020"
        "520a2f4b696473205b32203020522038203020522031322030"
        "20522031362030205220323020302052203235203020525d0a"
        "3e3e0a656e646f626a0a33322030206f626a0a3c3c0a2f5479"
        "7065202f50616765730a2f436f756e7420330a2f506172656e"
        "74203433203020520a2f4b696473205b323920302052203334"
        "20302052203338203020525d0a3e3e0a656e646f626a0a3433"
        "2030206f626a0a3c3c0a2f54797065202f50616765730a2f43"
        "6f756e7420390a2f4b696473205b3620302052203332203020"
        "525d0a3e3e0a656e646f626a0a34342030206f626a0a3c3c0a"
        "2f4e616d6573205b286e31292031302030205220286e322920"
        "31342030205220286e332920352030205220286e3429203331"
        "2030205220286e35292033362030205220286e362920343020"
        "3020525d0a2f4c696d697473205b286e312920286e36295d0a"
        "3e3e0a656e646f626a0a34352030206f626a0a3c3c0a2f4e61"
        "6d6573205b286e37292031382030205220286e392920323720"
        "3020525d0a2f4c696d697473205b286e372920286e39295d0a"
        "3e3e0a656e646f626a0a34362030206f626a0a3c3c0a2f4b69"
        "6473205b343420302052203435203020525d0a2f4c696d6974"
        "73205b286e312920286e39295d0a3e3e0a656e646f626a0a34"
        "372030206f626a0a3c3c0a2f4465737473203436203020520a"
        "3e3e0a656e646f626a0a34382030206f626a0a3c3c0a2f5479"
        "7065202f436174616c6f670a2f506167657320343320302052"
        "0a2f4e616d6573203437203020520a3e3e0a656e646f626a0a"
        "34392030206f626a0a3c3c0a2f50726f647563657220287064"
        "665465582d312e34302e3235290a2f43726561746f72202854"
        "6558290a2f54726170706564202f46616c73650a3e3e0a656e"
        "646f626a0a787265660a302035300a30303030303030303030"
        "2036353533352066200a303030303030303236302030303030"
        "30206e200a30303030303030313131203030303030206e200a"
        "30303030303030303135203030303030206e200a3030303030"
        "3033323438203030303030206e200a30303030303030323233"
        "203030303030206e200a303030303030333337392030303030"
        "30206e200a30303030303030353839203030303030206e200a"
        "30303030303030343233203030303030206e200a3030303030"
        "3030333237203030303030206e200a30303030303030353335"
        "203030303030206e200a303030303030303931312030303030"
        "30206e200a30303030303030373533203030303030206e200a"
        "30303030303030363536203030303030206e200a3030303030"
        "3030383638203030303030206e200a30303030303031323339"
        "203030303030206e200a303030303030313037362030303030"
        "30206e200a30303030303030393739203030303030206e200a"
        "30303030303031313931203030303030206e200a3030303030"
        "3031353634203030303030206e200a30303030303031343034",
        "203030303030206e200a303030303030313330372030303030"
        "30206e200a30303030303031353139203030303030206e200a"
        "30303030303031383633203030303030206e200a3030303030"
        "3031393731203030303030206e200a30303030303031373239"
        "203030303030206e200a303030303030313633322030303030"
        "30206e200a30303030303033303133203030303030206e200a"
        "30303030303032323932203030303030206e200a3030303030"
        "3032313336203030303030206e200a30303030303032303339"
        "203030303030206e200a303030303030323235322030303030"
        "30206e200a30303030303033343835203030303030206e200a"
        "30303030303032363230203030303030206e200a3030303030"
        "3032343537203030303030206e200a30303030303032333630"
        "203030303030206e200a303030303030323537332030303030"
        "30206e200a30303030303032393435203030303030206e200a"
        "30303030303032373835203030303030206e200a3030303030"
        "3032363838203030303030206e200a30303030303032393031"
        "203030303030206e200a303030303030333034322030303030"
        "30206e200a30303030303033303730203030303030206e200a"
        "30303030303033353733203030303030206e200a3030303030"
        "3033363338203030303030206e200a30303030303033373630"
        "203030303030206e200a303030303030333833352030303030"
        "30206e200a30303030303033383939203030303030206e200a"
        "30303030303033393335203030303030206e200a3030303030"
        "3034303030203030303030206e200a747261696c65720a3c3c"
        "202f53697a652035300a2f526f6f74203438203020520a2f49"
        "6e666f203439203020520a203e3e0a7374617274787265660a"
        "343038300a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* One font in the file for a font file, named after the first font that
   used it, whatever size it was asked for; the measurements are written from
   the last font a page met back to the first and the dictionaries by name.
   See docs/DECISIONS.md, the-fonts-a-page-names. */
static int test_the_fonts_a_page_names(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=cmr10 \\font\\b=cmr10 at 12pt \\font\\"
        "c=cmti10 \\font\\d=cmtt10 \\hsize=200pt \\vsize=30"
        "0pt \\parindent=0pt \\hbadness=10000 \\vbadness=10"
        "000 \\shipout\\hbox{\\c x\\a y\\b z}\\shipout\\hbo"
        "x{\\d\\char40 \\char41 \\char92 \\char32 w\\a v}\\"
        "shipout\\hbox{\\a q}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313036202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463320392e3936323620546620373220"
        "37332e393337205464205b2878295d544a2f463120392e3936"
        "323620546620342e3632322030205464205b2879295d544a2f"
        "46312031312e3935353220546620352e323538203020546420"
        "5b287a295d544a0a45540a0a656e6473747265616d0a656e64"
        "6f626a0a322030206f626a0a3c3c0a2f54797065202f506167"
        "650a2f436f6e74656e74732033203020520a2f5265736f7572"
        "6365732031203020520a2f4d65646961426f78205b30203020"
        "3135392e313933203135312e3038355d0a2f506172656e7420"
        "36203020520a3e3e0a656e646f626a0a312030206f626a0a3c"
        "3c0a2f466f6e74203c3c202f4633203420302052202f463120"
        "3520302052203e3e0a2f50726f63536574205b202f50444620"
        "2f54657874205d0a3e3e0a656e646f626a0a392030206f626a"
        "0a3c3c0a2f4c656e67746820393020202020202020200a3e3e"
        "0a73747265616d0a42540a2f463420392e3936323620546620"
        "37322037332e313037205464205b285c3035305c3035315c31"
        "33345c30343077295d544a2f463120392e3936323620546620"
        "32362e3135322030205464205b2876295d544a0a45540a0a65"
        "6e6473747265616d0a656e646f626a0a382030206f626a0a3c"
        "3c0a2f54797065202f506167650a2f436f6e74656e74732039"
        "203020520a2f5265736f75726365732037203020520a2f4d65"
        "646961426f78205b302030203137352e3431203135322e3032"
        "355d0a2f506172656e742036203020520a3e3e0a656e646f62"
        "6a0a372030206f626a0a3c3c0a2f466f6e74203c3c202f4634"
        "20313020302052202f4631203520302052203e3e0a2f50726f"
        "63536574205b202f504446202f54657874205d0a3e3e0a656e"
        "646f626a0a31332030206f626a0a3c3c0a2f4c656e67746820"
        "343120202020202020200a3e3e0a73747265616d0a42540a2f"
        "463120392e393632362054662037322037332e393337205464"
        "205b2871295d544a0a45540a0a656e6473747265616d0a656e"
        "646f626a0a31322030206f626a0a3c3c0a2f54797065202f50"
        "6167650a2f436f6e74656e7473203133203020520a2f526573"
        "6f7572636573203131203020520a2f4d65646961426f78205b"
        "302030203134392e323538203135302e3232375d0a2f506172"
        "656e742036203020520a3e3e0a656e646f626a0a3131203020"
        "6f626a0a3c3c0a2f466f6e74203c3c202f4631203520302052"
        "203e3e0a2f50726f63536574205b202f504446202f54657874"
        "205d0a3e3e0a656e646f626a0a31342030206f626a0a5b3532"
        "35203532352035323520353235203532352035323520353235"
        "20353235203532352035323520353235203532352035323520"
        "35323520353235203532352035323520353235203532352035"
        "32352035323520353235203532352035323520353235203532"
        "35203532352035323520353235203532352035323520353235"
        "20353235203532352035323520353235203532352035323520"
        "35323520353235203532352035323520353235203532352035"
        "32352035323520353235203532352035323520353235203532"
        "35203532352035323520353235203532352035323520353235"
        "20353235203532352035323520353235203532352035323520"
        "35323520353235203532352035323520353235203532352035"
        "32352035323520353235203532352035323520353235203532"
        "35203532352035323520353235203532352035323520353235"
        "2035323520353235203532352035323520353235203532355d"
        "0a656e646f626a0a31352030206f626a0a3c3c0a2f54797065"
        "202f466f6e7444657363726970746f720a2f466f6e744e616d"
        "65202f434d545431300a2f466c6167732033340a2f466f6e74"
        "42426f78205b30202d3232322031303530203631315d0a2f41"
        "7363656e74203631310a2f436170486569676874203631310a"
        "2f44657363656e74202d3232320a2f4974616c6963416e676c",
        "6520300a2f5374656d56203137350a2f584865696768742034"
        "33310a3e3e0a656e646f626a0a31362030206f626a0a5b3532"
        "372e38203339312e37203339342e34203338382e3920353535"
        "2e36203532372e38203732322e32203532372e38203532372e"
        "38203434342e345d0a656e646f626a0a31372030206f626a0a"
        "3c3c0a2f54797065202f466f6e7444657363726970746f720a"
        "2f466f6e744e616d65202f434d5231300a2f466c6167732033"
        "340a2f466f6e7442426f78205b30202d313934203130303020"
        "3639345d0a2f417363656e74203639340a2f43617048656967"
        "6874203638330a2f44657363656e74202d3139340a2f497461"
        "6c6963416e676c6520300a2f5374656d562039330a2f584865"
        "69676874203433310a3e3e0a656e646f626a0a31382030206f"
        "626a0a5b3436332e395d0a656e646f626a0a31392030206f62"
        "6a0a3c3c0a2f54797065202f466f6e7444657363726970746f"
        "720a2f466f6e744e616d65202f434d544931300a2f466c6167"
        "732033340a2f466f6e7442426f78205b30202d313934203130"
        "3232203639345d0a2f417363656e74203639340a2f43617048"
        "6569676874203638330a2f44657363656e74202d3139340a2f"
        "4974616c6963416e676c6520300a2f5374656d56203130320a"
        "2f58486569676874203433310a3e3e0a656e646f626a0a3520"
        "30206f626a0a3c3c0a2f54797065202f466f6e740a2f537562"
        "74797065202f54797065310a2f42617365466f6e74202f434d"
        "5231300a2f466f6e7444657363726970746f72203137203020"
        "520a2f466972737443686172203131330a2f4c617374436861"
        "72203132320a2f576964746873203136203020520a3e3e0a65"
        "6e646f626a0a342030206f626a0a3c3c0a2f54797065202f46"
        "6f6e740a2f53756274797065202f54797065310a2f42617365"
        "466f6e74202f434d544931300a2f466f6e7444657363726970"
        "746f72203139203020520a2f46697273744368617220313230"
        "0a2f4c61737443686172203132300a2f576964746873203138"
        "203020520a3e3e0a656e646f626a0a31302030206f626a0a3c"
        "3c0a2f54797065202f466f6e740a2f53756274797065202f54"
        "797065310a2f42617365466f6e74202f434d545431300a2f46"
        "6f6e7444657363726970746f72203135203020520a2f466972"
        "7374436861722033320a2f4c61737443686172203131390a2f"
        "576964746873203134203020520a3e3e0a656e646f626a0a36"
        "2030206f626a0a3c3c0a2f54797065202f50616765730a2f43"
        "6f756e7420330a2f4b696473205b3220302052203820302052"
        "203132203020525d0a3e3e0a656e646f626a0a32302030206f"
        "626a0a3c3c0a2f54797065202f436174616c6f670a2f506167"
        "65732036203020520a3e3e0a656e646f626a0a32312030206f"
        "626a0a3c3c0a2f50726f647563657220287064665465582d31"
        "2e34302e3235290a2f43726561746f722028546558290a2f54"
        "726170706564202f46616c73650a3e3e0a656e646f626a0a78"
        "7265660a302032320a30303030303030303030203635353335"
        "2066200a30303030303030323931203030303030206e200a30"
        "303030303030313739203030303030206e200a303030303030"
        "30303135203030303030206e200a3030303030303231333120"
        "3030303030206e200a30303030303031393938203030303030"
        "206e200a30303030303032333939203030303030206e200a30"
        "303030303030363237203030303030206e200a303030303030"
        "30353136203030303030206e200a3030303030303033363820"
        "3030303030206e200a30303030303032323635203030303030"
        "206e200a30303030303030393230203030303030206e200a30"
        "303030303030383035203030303030206e200a303030303030"
        "30373035203030303030206e200a3030303030303039383820"
        "3030303030206e200a30303030303031333538203030303030"
        "206e200a30303030303031353338203030303030206e200a30"
        "303030303031363136203030303030206e200a303030303030"
        "31373934203030303030206e200a3030303030303138313820",
        "3030303030206e200a30303030303032343639203030303030"
        "206e200a30303030303032353139203030303030206e200a74"
        "7261696c65720a3c3c202f53697a652032320a2f526f6f7420"
        "3230203020520a2f496e666f203231203020520a203e3e0a73"
        "74617274787265660a323539390a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* The leaders of a table of contents are set in the PDF as they are in the
   DVI: the boxes repeat on the grid the enclosing list settles, and a rule
   fills the whole of the glue. See docs/DECISIONS.md, leaders-on-a-page. */
static int test_leaders_in_the_pdf(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\tenrm=cmr10 \\tenrm \\hsize=200pt \\vsiz"
        "e=200pt \\parindent=0pt \\baselineskip=0pt \\lines"
        "kip=0pt \\boxmaxdepth=0pt \\hbadness=10000 \\vbadn"
        "ess=10000 \\setbox9=\\hbox to 9pt{.\\hss}\\shipout"
        "\\vbox{\\hbox{a\\leaders\\hbox to 10pt{.}\\hskip 5"
        "5pt b}\\hbox{a\\cleaders\\copy9\\hskip 55pt b}\\hb"
        "ox{a\\xleaders\\copy9\\hskip 55pt b}\\hbox{a\\lead"
        "ers\\vrule height 2pt depth 1pt\\hskip 30pt b}\\le"
        "aders\\hbox{x}\\vskip 40pt \\leaders\\hrule height"
        " 3pt\\vskip 20pt}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820353436202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3135332e353238205464205b2861292d353030282e292d3732"
        "32282e292d373232282e292d373233282e292d373232282e29"
        "2d3732322862295d544a2030202d362e393139205464205b28"
        "61292d3530282e292d363232282e292d363232282e292d3632"
        "33282e292d363232282e292d363232282e292d363732286229"
        "5d544a2030202d362e393138205464205b2861292d3134282e"
        "292d363337282e292d363336282e292d363337282e292d3633"
        "36282e292d363337282e292d3633362862295d544a2030202d"
        "362e393139205464205b2861295d544a0a45540a710a312030"
        "203020312037362e393831203133312e37373620636d0a3020"
        "302032392e38383820322e39383920726520660a510a42540a"
        "2f463120392e39363236205466203130362e38363920313332"
        "2e373732205464205b2862295d544a202d33342e383639202d"
        "362e363432205464205b2878295d544a2030202d342e323839"
        "205464205b2878295d544a2030202d342e323839205464205b"
        "2878295d544a2030202d342e3239205464205b2878295d544a"
        "2030202d342e323839205464205b2878295d544a2030202d34"
        "2e3239205464205b2878295d544a2030202d342e3238392054"
        "64205b2878295d544a2030202d342e3239205464205b287829"
        "5d544a0a45540a710a3120302030203120373220373220636d"
        "0a3020302036352e3331312031392e39323520726520660a51"
        "0a0a656e6473747265616d0a656e646f626a0a322030206f62"
        "6a0a3c3c0a2f54797065202f506167650a2f436f6e74656e74"
        "732033203020520a2f5265736f75726365732031203020520a"
        "2f4d65646961426f78205b302030203230392e333131203233"
        "322e3434365d0a2f506172656e742035203020520a3e3e0a65"
        "6e646f626a0a312030206f626a0a3c3c0a2f466f6e74203c3c"
        "202f4631203420302052203e3e0a2f50726f63536574205b20"
        "2f504446202f54657874205d0a3e3e0a656e646f626a0a3620"
        "30206f626a0a5b3237372e3820353030203530302035303020"
        "35303020353030203530302035303020353030203530302035"
        "303020353030203237372e38203237372e38203237372e3820"
        "3737372e38203437322e32203437322e32203737372e382037"
        "3530203730382e33203732322e32203736332e39203638302e"
        "36203635322e38203738342e3720373530203336312e312035"
        "31332e39203737372e3820363235203931362e372037353020"
        "3737372e38203638302e36203737372e38203733362e312035"
        "35352e36203732322e32203735302037353020313032372e38"
        "2037353020373530203631312e31203237372e382035303020"
        "3237372e3820353030203237372e38203237372e3820353030"
        "203535352e36203434342e34203535352e36203434342e3420"
        "3330352e3620353030203535352e36203237372e3820333035"
        "2e36203532372e38203237372e38203833332e33203535352e"
        "3620353030203535352e36203532372e38203339312e372033"
        "39342e34203338382e39203535352e36203532372e38203732"
        "322e32203532372e385d0a656e646f626a0a372030206f626a"
        "0a3c3c0a2f54797065202f466f6e7444657363726970746f72"
        "0a2f466f6e744e616d65202f434d5231300a2f466c61677320"
        "33340a2f466f6e7442426f78205b30202d3139342031303030"
        "203639345d0a2f417363656e74203639340a2f436170486569"
        "676874203638330a2f44657363656e74202d3139340a2f4974"
        "616c6963416e676c6520300a2f5374656d562039330a2f5848"
        "6569676874203433310a3e3e0a656e646f626a0a342030206f"
        "626a0a3c3c0a2f54797065202f466f6e740a2f537562747970"
        "65202f54797065310a2f42617365466f6e74202f434d523130"
        "0a2f466f6e7444657363726970746f722037203020520a2f46"
        "69727374436861722034360a2f4c6173744368617220313230",
        "0a2f5769647468732036203020520a3e3e0a656e646f626a0a"
        "352030206f626a0a3c3c0a2f54797065202f50616765730a2f"
        "436f756e7420310a2f4b696473205b32203020525d0a3e3e0a"
        "656e646f626a0a382030206f626a0a3c3c0a2f54797065202f"
        "436174616c6f670a2f50616765732035203020520a3e3e0a65"
        "6e646f626a0a392030206f626a0a3c3c0a2f50726f64756365"
        "7220287064665465582d312e34302e3235290a2f4372656174"
        "6f722028546558290a2f54726170706564202f46616c73650a"
        "3e3e0a656e646f626a0a787265660a302031300a3030303030"
        "30303030302036353533352066200a30303030303030373331"
        "203030303030206e200a303030303030303631392030303030"
        "30206e200a30303030303030303135203030303030206e200a"
        "30303030303031333935203030303030206e200a3030303030"
        "3031353235203030303030206e200a30303030303030373938"
        "203030303030206e200a303030303030313231382030303030"
        "30206e200a30303030303031353832203030303030206e200a"
        "30303030303031363331203030303030206e200a747261696c"
        "65720a3c3c202f53697a652031300a2f526f6f742038203020"
        "520a2f496e666f2039203020520a203e3e0a73746172747872"
        "65660a313731300a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* Where the file thinks the text stands: the advance of a glyph is the
   width the file states for it, at the size it states for the font, taken
   towards the width the engine itself has. The size here is one the file can
   state exactly. See docs/DECISIONS.md, the-text-position-in-the-file. */
static int test_the_text_position_in_the_file(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=cmr10 at 616704sp \\a \\hsize=400pt \\"
        "vsize=200pt \\parindent=0pt \\hbadness=10000 \\vba"
        "dness=10000 \\shipout\\hbox{mmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm}\\shi"
        "pout\\hbox{iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii"
        "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii"
        "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii"
        "iii}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820323235202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3337352054662037322037"
        "32205464205b286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31"
        "286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31"
        "286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d295d54"
        "4a0a45540a0a656e6473747265616d0a656e646f626a0a3220"
        "30206f626a0a3c3c0a2f54797065202f506167650a2f436f6e"
        "74656e74732033203020520a2f5265736f7572636573203120"
        "3020520a2f4d65646961426f78205b30203020313434302e38"
        "3738203134382e3033365d0a2f506172656e74203520302052"
        "0a3e3e0a656e646f626a0a312030206f626a0a3c3c0a2f466f"
        "6e74203c3c202f4631203420302052203e3e0a2f50726f6353"
        "6574205b202f504446202f54657874205d0a3e3e0a656e646f"
        "626a0a382030206f626a0a3c3c0a2f4c656e67746820313836"
        "202020202020200a3e3e0a73747265616d0a42540a2f463120"
        "392e333735205466203732203732205464205b286969696969"
        "69696969696969696969696969696969696969293128696969"
        "69696969696969696969696969696969696969696969696969"
        "69696969696969696969696969696969696969692931286969"
        "69696969696969696969696969696969696969696969696969"
        "69696969696969696969696969696969696969692931286969"
        "696969696969696969696969696969696969696969295d544a"
        "0a45540a0a656e6473747265616d0a656e646f626a0a372030"
        "206f626a0a3c3c0a2f54797065202f506167650a2f436f6e74"
        "656e74732038203020520a2f5265736f757263657320362030"
        "20520a2f4d65646961426f78205b302030203531332e373932"
        "203135302e3236315d0a2f506172656e742035203020520a3e"
        "3e0a656e646f626a0a362030206f626a0a3c3c0a2f466f6e74"
        "203c3c202f4631203420302052203e3e0a2f50726f63536574"
        "205b202f504446202f54657874205d0a3e3e0a656e646f626a"
        "0a392030206f626a0a5b3237372e38203330352e3620353237"
        "2e38203237372e38203833332e335d0a656e646f626a0a3130"
        "2030206f626a0a3c3c0a2f54797065202f466f6e7444657363"
        "726970746f720a2f466f6e744e616d65202f434d5231300a2f"
        "466c6167732033340a2f466f6e7442426f78205b30202d3139"
        "342031303030203639345d0a2f417363656e74203639340a2f"
        "436170486569676874203638330a2f44657363656e74202d31"
        "39340a2f4974616c6963416e676c6520300a2f5374656d5620"
        "39330a2f58486569676874203433310a3e3e0a656e646f626a"
        "0a342030206f626a0a3c3c0a2f54797065202f466f6e740a2f"
        "53756274797065202f54797065310a2f42617365466f6e7420"
        "2f434d5231300a2f466f6e7444657363726970746f72203130"
        "203020520a2f466972737443686172203130350a2f4c617374"
        "43686172203130390a2f5769647468732039203020520a3e3e"
        "0a656e646f626a0a352030206f626a0a3c3c0a2f5479706520"
        "2f50616765730a2f436f756e7420320a2f4b696473205b3220"
        "3020522037203020525d0a3e3e0a656e646f626a0a31312030"
        "206f626a0a3c3c0a2f54797065202f436174616c6f670a2f50"
        "616765732035203020520a3e3e0a656e646f626a0a31322030"
        "206f626a0a3c3c0a2f50726f64756365722028706466546558"
        "2d312e34302e3235290a2f43726561746f722028546558290a"
        "2f54726170706564202f46616c73650a3e3e0a656e646f626a"
        "0a787265660a302031330a3030303030303030303020363535"
        "33352066200a30303030303030343131203030303030206e20",
        "0a30303030303030323938203030303030206e200a30303030"
        "303030303135203030303030206e200a303030303030313132"
        "36203030303030206e200a3030303030303132353820303030"
        "3030206e200a30303030303030383334203030303030206e20"
        "0a30303030303030373232203030303030206e200a30303030"
        "303030343738203030303030206e200a303030303030303930"
        "31203030303030206e200a3030303030303039343820303030"
        "3030206e200a30303030303031333231203030303030206e20"
        "0a30303030303031333731203030303030206e200a74726169"
        "6c65720a3c3c202f53697a652031330a2f526f6f7420313120"
        "3020520a2f496e666f203132203020520a203e3e0a73746172"
        "74787265660a313435310a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* A rule no thicker than a point is drawn as a line of that thickness, down
   its middle, rather than filled; see docs/DECISIONS.md, thin-rules. */
static int test_thin_rules(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\tenrm=cmr10 \\tenrm \\hsize=400pt \\vsiz"
        "e=400pt \\parindent=0pt \\hbadness=10000 \\vbadnes"
        "s=10000 \\shipout\\hbox{\\vrule width 10pt height "
        "0.4pt depth 0pt}\\shipout\\hbox{\\vrule width 10pt"
        " height 1pt depth 0pt}\\shipout\\hbox{\\vrule widt"
        "h 10pt height 1.004pt depth 0pt}\\shipout\\hbox{\\"
        "vrule width 0.4pt height 10pt depth 0pt}\\shipout"
        "\\hbox{\\vrule width 2pt height 2pt depth 0pt}\\sh"
        "ipout\\hbox{a\\vrule width 5pt height 0.2pt depth "
        "0.3pt b}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820363120202020202020200a3e3e0a7374"
        "7265616d0a710a312030203020312037322037322e31393920"
        "636d0a5b5d3020642030204a20302e33393820772030203020"
        "6d20392e3936332030206c20530a510a0a656e647374726561"
        "6d0a656e646f626a0a322030206f626a0a3c3c0a2f54797065"
        "202f506167650a2f436f6e74656e74732033203020520a2f52"
        "65736f75726365732031203020520a2f4d65646961426f7820"
        "5b302030203135332e393633203134342e3339395d0a2f5061"
        "72656e742034203020520a3e3e0a656e646f626a0a31203020"
        "6f626a0a3c3c0a2f50726f63536574205b202f504446205d0a"
        "3e3e0a656e646f626a0a372030206f626a0a3c3c0a2f4c656e"
        "67746820363120202020202020200a3e3e0a73747265616d0a"
        "710a312030203020312037322037322e34393820636d0a5b5d"
        "3020642030204a20302e393936207720302030206d20392e39"
        "36332030206c20530a510a0a656e6473747265616d0a656e64"
        "6f626a0a362030206f626a0a3c3c0a2f54797065202f506167"
        "650a2f436f6e74656e74732037203020520a2f5265736f7572"
        "6365732035203020520a2f4d65646961426f78205b30203020"
        "3135332e393633203134342e3939365d0a2f506172656e7420"
        "34203020520a3e3e0a656e646f626a0a352030206f626a0a3c"
        "3c0a2f50726f63536574205b202f504446205d0a3e3e0a656e"
        "646f626a0a31302030206f626a0a3c3c0a2f4c656e67746820"
        "333820202020202020200a3e3e0a73747265616d0a710a3120"
        "302030203120373220373220636d0a30203020392e39363320"
        "3120726520660a510a0a656e6473747265616d0a656e646f62"
        "6a0a392030206f626a0a3c3c0a2f54797065202f506167650a"
        "2f436f6e74656e7473203130203020520a2f5265736f757263"
        "65732038203020520a2f4d65646961426f78205b3020302031"
        "35332e393633203134355d0a2f506172656e74203420302052"
        "0a3e3e0a656e646f626a0a382030206f626a0a3c3c0a2f5072"
        "6f63536574205b202f504446205d0a3e3e0a656e646f626a0a"
        "31332030206f626a0a3c3c0a2f4c656e677468203631202020"
        "20202020200a3e3e0a73747265616d0a710a31203020302031"
        "2037322e31393920373220636d0a5b5d3020642030204a2030"
        "2e333938207720302030206d203020392e393633206c20530a"
        "510a0a656e6473747265616d0a656e646f626a0a3132203020"
        "6f626a0a3c3c0a2f54797065202f506167650a2f436f6e7465"
        "6e7473203133203020520a2f5265736f757263657320313120"
        "3020520a2f4d65646961426f78205b302030203134342e3339"
        "39203135332e3936335d0a2f506172656e742034203020520a"
        "3e3e0a656e646f626a0a31312030206f626a0a3c3c0a2f5072"
        "6f63536574205b202f504446205d0a3e3e0a656e646f626a0a"
        "31362030206f626a0a3c3c0a2f4c656e677468203432202020"
        "20202020200a3e3e0a73747265616d0a710a31203020302031"
        "20373220373220636d0a30203020312e39393320312e393933"
        "20726520660a510a0a656e6473747265616d0a656e646f626a"
        "0a31352030206f626a0a3c3c0a2f54797065202f506167650a"
        "2f436f6e74656e7473203136203020520a2f5265736f757263"
        "6573203134203020520a2f4d65646961426f78205b30203020"
        "3134352e393933203134352e3939335d0a2f506172656e7420"
        "34203020520a3e3e0a656e646f626a0a31342030206f626a0a"
        "3c3c0a2f50726f63536574205b202f504446205d0a3e3e0a65"
        "6e646f626a0a31392030206f626a0a3c3c0a2f4c656e677468"
        "20313531202020202020200a3e3e0a73747265616d0a42540a"
        "2f463120392e393632362054662037322037322e3239392054"
        "64205b2861295d544a0a45540a710a31203020302031203736"
        "2e3938312037322e32343920636d0a5b5d3020642030204a20"
        "302e343938207720302030206d20342e3938312030206c2053"
        "0a510a42540a2f463120392e393632362054662038312e3936",
        "332037322e323939205464205b2862295d544a0a45540a0a65"
        "6e6473747265616d0a656e646f626a0a31382030206f626a0a"
        "3c3c0a2f54797065202f506167650a2f436f6e74656e747320"
        "3139203020520a2f5265736f7572636573203137203020520a"
        "2f4d65646961426f78205b302030203135392e343937203135"
        "312e3231375d0a2f506172656e742034203020520a3e3e0a65"
        "6e646f626a0a31372030206f626a0a3c3c0a2f466f6e74203c"
        "3c202f463120323020302052203e3e0a2f50726f6353657420"
        "5b202f504446202f54657874205d0a3e3e0a656e646f626a0a"
        "32312030206f626a0a5b353030203535352e365d0a656e646f"
        "626a0a32322030206f626a0a3c3c0a2f54797065202f466f6e"
        "7444657363726970746f720a2f466f6e744e616d65202f434d"
        "5231300a2f466c6167732033340a2f466f6e7442426f78205b"
        "30202d3139342031303030203639345d0a2f417363656e7420"
        "3639340a2f436170486569676874203638330a2f4465736365"
        "6e74202d3139340a2f4974616c6963416e676c6520300a2f53"
        "74656d562039330a2f58486569676874203433310a3e3e0a65"
        "6e646f626a0a32302030206f626a0a3c3c0a2f54797065202f"
        "466f6e740a2f53756274797065202f54797065310a2f426173"
        "65466f6e74202f434d5231300a2f466f6e7444657363726970"
        "746f72203232203020520a2f4669727374436861722039370a"
        "2f4c617374436861722039380a2f5769647468732032312030"
        "20520a3e3e0a656e646f626a0a342030206f626a0a3c3c0a2f"
        "54797065202f50616765730a2f436f756e7420360a2f4b6964"
        "73205b32203020522036203020522039203020522031322030"
        "205220313520302052203138203020525d0a3e3e0a656e646f"
        "626a0a32332030206f626a0a3c3c0a2f54797065202f436174"
        "616c6f670a2f50616765732034203020520a3e3e0a656e646f"
        "626a0a32342030206f626a0a3c3c0a2f50726f647563657220"
        "287064665465582d312e34302e3235290a2f43726561746f72"
        "2028546558290a2f54726170706564202f46616c73650a3e3e"
        "0a656e646f626a0a787265660a302032350a30303030303030"
        "3030302036353533352066200a303030303030303234362030"
        "30303030206e200a3030303030303031333420303030303020"
        "6e200a30303030303030303135203030303030206e200a3030"
        "3030303032303633203030303030206e200a30303030303030"
        "353136203030303030206e200a303030303030303430342030"
        "30303030206e200a3030303030303032383520303030303020"
        "6e200a30303030303030373631203030303030206e200a3030"
        "3030303030363532203030303030206e200a30303030303030"
        "353535203030303030206e200a303030303030313033352030"
        "30303030206e200a3030303030303039323020303030303020"
        "6e200a30303030303030383030203030303030206e200a3030"
        "3030303031323931203030303030206e200a30303030303031"
        "313736203030303030206e200a303030303030313037352030"
        "30303030206e200a3030303030303136353620303030303020"
        "6e200a30303030303031353431203030303030206e200a3030"
        "3030303031333331203030303030206e200a30303030303031"
        "393331203030303030206e200a303030303030313732352030"
        "30303030206e200a3030303030303137353320303030303020"
        "6e200a30303030303032313533203030303030206e200a3030"
        "3030303032323033203030303030206e200a747261696c6572"
        "0a3c3c202f53697a652032350a2f526f6f7420323320302052"
        "0a2f496e666f203234203020520a203e3e0a73746172747872"
        "65660a323238330a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_middle_of_a_thin_rule(void)
{
    /* A thin rule is drawn as a line down its middle: across, that middle
       is the larger half of its width; up, it is the smaller half of its
       thickness and a scaled point over. Each pair of pages straddles the
       scaled point where the place the file names turns over, and the four
       thicknesses show which way each half rounds. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\hsize=16000pt \\vsize=300pt \\parindent=0pt\\s"
        "hipout\\hbox{\\raise 31sp \\hbox{\\vrule width 10p"
        "t height 1sp depth 0pt}}\\shipout\\hbox{\\raise 32"
        "sp \\hbox{\\vrule width 10pt height 1sp depth 0pt}"
        "}\\shipout\\hbox{\\raise 30sp \\hbox{\\vrule width"
        " 10pt height 2sp depth 0pt}}\\shipout\\hbox{\\rais"
        "e 31sp \\hbox{\\vrule width 10pt height 2sp depth "
        "0pt}}\\shipout\\hbox{\\raise 30sp \\hbox{\\vrule w"
        "idth 10pt height 3sp depth 0pt}}\\shipout\\hbox{\\"
        "raise 31sp \\hbox{\\vrule width 10pt height 3sp de"
        "pth 0pt}}\\shipout\\hbox{\\raise 29sp \\hbox{\\vru"
        "le width 10pt height 4sp depth 0pt}}\\shipout\\hbo"
        "x{\\raise 30sp \\hbox{\\vrule width 10pt height 4s"
        "p depth 0pt}}\\shipout\\hbox{\\kern 31sp \\vrule w"
        "idth 1sp height 10pt depth 0pt}\\shipout\\hbox{\\k"
        "ern 32sp \\vrule width 1sp height 10pt depth 0pt}"
        "\\shipout\\hbox{\\kern 31sp \\vrule width 2sp heig"
        "ht 10pt depth 0pt}\\shipout\\hbox{\\kern 32sp \\vr"
        "ule width 2sp height 10pt depth 0pt}\\shipout\\hbo"
        "x{\\kern 30sp \\vrule width 3sp height 10pt depth "
        "0pt}\\shipout\\hbox{\\kern 31sp \\vrule width 3sp "
        "height 10pt depth 0pt}\\shipout\\hbox{\\kern 30sp "
        "\\vrule width 4sp height 10pt depth 0pt}\\shipout"
        "\\hbox{\\kern 31sp \\vrule width 4sp height 10pt d"
        "epth 0pt}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820353320202020202020200a3e3e0a7374"
        "7265616d0a710a3120302030203120373220373220636d0a5b"
        "5d3020642030204a2030207720302030206d20392e39363320"
        "30206c20530a510a0a656e6473747265616d0a656e646f626a"
        "0a322030206f626a0a3c3c0a2f54797065202f506167650a2f"
        "436f6e74656e74732033203020520a2f5265736f7572636573"
        "2031203020520a2f4d65646961426f78205b30203020313533"
        "2e393633203134345d0a2f506172656e742034203020520a3e"
        "3e0a656e646f626a0a312030206f626a0a3c3c0a2f50726f63"
        "536574205b202f504446205d0a3e3e0a656e646f626a0a3720"
        "30206f626a0a3c3c0a2f4c656e677468203537202020202020"
        "20200a3e3e0a73747265616d0a710a31203020302031203732"
        "2037322e30303120636d0a5b5d3020642030204a2030207720"
        "302030206d20392e3936332030206c20530a510a0a656e6473"
        "747265616d0a656e646f626a0a362030206f626a0a3c3c0a2f"
        "54797065202f506167650a2f436f6e74656e74732037203020"
        "520a2f5265736f75726365732035203020520a2f4d65646961"
        "426f78205b302030203135332e393633203134342e3030315d"
        "0a2f506172656e742034203020520a3e3e0a656e646f626a0a"
        "352030206f626a0a3c3c0a2f50726f63536574205b202f5044"
        "46205d0a3e3e0a656e646f626a0a31302030206f626a0a3c3c"
        "0a2f4c656e67746820353320202020202020200a3e3e0a7374"
        "7265616d0a710a3120302030203120373220373220636d0a5b"
        "5d3020642030204a2030207720302030206d20392e39363320"
        "30206c20530a510a0a656e6473747265616d0a656e646f626a"
        "0a392030206f626a0a3c3c0a2f54797065202f506167650a2f"
        "436f6e74656e7473203130203020520a2f5265736f75726365"
        "732038203020520a2f4d65646961426f78205b302030203135"
        "332e393633203134345d0a2f506172656e742034203020520a"
        "3e3e0a656e646f626a0a382030206f626a0a3c3c0a2f50726f"
        "63536574205b202f504446205d0a3e3e0a656e646f626a0a31"
        "332030206f626a0a3c3c0a2f4c656e67746820353720202020"
        "202020200a3e3e0a73747265616d0a710a3120302030203120"
        "37322037322e30303120636d0a5b5d3020642030204a203020"
        "7720302030206d20392e3936332030206c20530a510a0a656e"
        "6473747265616d0a656e646f626a0a31322030206f626a0a3c"
        "3c0a2f54797065202f506167650a2f436f6e74656e74732031"
        "33203020520a2f5265736f7572636573203131203020520a2f"
        "4d65646961426f78205b302030203135332e39363320313434"
        "2e3030315d0a2f506172656e742034203020520a3e3e0a656e"
        "646f626a0a31312030206f626a0a3c3c0a2f50726f63536574"
        "205b202f504446205d0a3e3e0a656e646f626a0a3136203020"
        "6f626a0a3c3c0a2f4c656e6774682035332020202020202020"
        "0a3e3e0a73747265616d0a710a312030203020312037322037"
        "3220636d0a5b5d3020642030204a2030207720302030206d20"
        "392e3936332030206c20530a510a0a656e6473747265616d0a"
        "656e646f626a0a31352030206f626a0a3c3c0a2f5479706520"
        "2f506167650a2f436f6e74656e7473203136203020520a2f52"
        "65736f7572636573203134203020520a2f4d65646961426f78"
        "205b302030203135332e393633203134342e3030315d0a2f50"
        "6172656e742034203020520a3e3e0a656e646f626a0a313420"
        "30206f626a0a3c3c0a2f50726f63536574205b202f50444620"
        "5d0a3e3e0a656e646f626a0a31392030206f626a0a3c3c0a2f"
        "4c656e67746820353720202020202020200a3e3e0a73747265"
        "616d0a710a312030203020312037322037322e30303120636d"
        "0a5b5d3020642030204a2030207720302030206d20392e3936"
        "332030206c20530a510a0a656e6473747265616d0a656e646f"
        "626a0a31382030206f626a0a3c3c0a2f54797065202f506167"
        "650a2f436f6e74656e7473203139203020520a2f5265736f75",
        "72636573203137203020520a2f4d65646961426f78205b3020"
        "30203135332e393633203134342e3030315d0a2f506172656e"
        "742034203020520a3e3e0a656e646f626a0a31372030206f62"
        "6a0a3c3c0a2f50726f63536574205b202f504446205d0a3e3e"
        "0a656e646f626a0a32322030206f626a0a3c3c0a2f4c656e67"
        "746820353320202020202020200a3e3e0a73747265616d0a71"
        "0a3120302030203120373220373220636d0a5b5d3020642030"
        "204a2030207720302030206d20392e3936332030206c20530a"
        "510a0a656e6473747265616d0a656e646f626a0a3231203020"
        "6f626a0a3c3c0a2f54797065202f506167650a2f436f6e7465"
        "6e7473203232203020520a2f5265736f757263657320323020"
        "3020520a2f4d65646961426f78205b302030203135332e3936"
        "33203134342e3030315d0a2f506172656e7420323320302052"
        "0a3e3e0a656e646f626a0a32302030206f626a0a3c3c0a2f50"
        "726f63536574205b202f504446205d0a3e3e0a656e646f626a"
        "0a32362030206f626a0a3c3c0a2f4c656e6774682035372020"
        "2020202020200a3e3e0a73747265616d0a710a312030203020"
        "312037322037322e30303120636d0a5b5d3020642030204a20"
        "30207720302030206d20392e3936332030206c20530a510a0a"
        "656e6473747265616d0a656e646f626a0a32352030206f626a"
        "0a3c3c0a2f54797065202f506167650a2f436f6e74656e7473"
        "203236203020520a2f5265736f757263657320323420302052"
        "0a2f4d65646961426f78205b302030203135332e3936332031"
        "34342e3030315d0a2f506172656e74203233203020520a3e3e"
        "0a656e646f626a0a32342030206f626a0a3c3c0a2f50726f63"
        "536574205b202f504446205d0a3e3e0a656e646f626a0a3239"
        "2030206f626a0a3c3c0a2f4c656e6774682035332020202020"
        "2020200a3e3e0a73747265616d0a710a312030203020312037"
        "3220373220636d0a5b5d3020642030204a2030207720302030"
        "206d203020392e393633206c20530a510a0a656e6473747265"
        "616d0a656e646f626a0a32382030206f626a0a3c3c0a2f5479"
        "7065202f506167650a2f436f6e74656e747320323920302052"
        "0a2f5265736f7572636573203237203020520a2f4d65646961"
        "426f78205b30203020313434203135332e3936335d0a2f5061"
        "72656e74203233203020520a3e3e0a656e646f626a0a323720"
        "30206f626a0a3c3c0a2f50726f63536574205b202f50444620"
        "5d0a3e3e0a656e646f626a0a33322030206f626a0a3c3c0a2f"
        "4c656e67746820353720202020202020200a3e3e0a73747265"
        "616d0a710a312030203020312037322e30303120373220636d"
        "0a5b5d3020642030204a2030207720302030206d203020392e"
        "393633206c20530a510a0a656e6473747265616d0a656e646f"
        "626a0a33312030206f626a0a3c3c0a2f54797065202f506167"
        "650a2f436f6e74656e7473203332203020520a2f5265736f75"
        "72636573203330203020520a2f4d65646961426f78205b3020"
        "30203134342e303031203135332e3936335d0a2f506172656e"
        "74203233203020520a3e3e0a656e646f626a0a33302030206f"
        "626a0a3c3c0a2f50726f63536574205b202f504446205d0a3e"
        "3e0a656e646f626a0a33352030206f626a0a3c3c0a2f4c656e"
        "67746820353320202020202020200a3e3e0a73747265616d0a"
        "710a3120302030203120373220373220636d0a5b5d30206420"
        "30204a2030207720302030206d203020392e393633206c2053"
        "0a510a0a656e6473747265616d0a656e646f626a0a33342030"
        "206f626a0a3c3c0a2f54797065202f506167650a2f436f6e74"
        "656e7473203335203020520a2f5265736f7572636573203333"
        "203020520a2f4d65646961426f78205b302030203134342e30"
        "3031203135332e3936335d0a2f506172656e74203233203020"
        "520a3e3e0a656e646f626a0a33332030206f626a0a3c3c0a2f"
        "50726f63536574205b202f504446205d0a3e3e0a656e646f62"
        "6a0a33382030206f626a0a3c3c0a2f4c656e67746820353720"
        "202020202020200a3e3e0a73747265616d0a710a3120302030",
        "20312037322e30303120373220636d0a5b5d3020642030204a"
        "2030207720302030206d203020392e393633206c20530a510a"
        "0a656e6473747265616d0a656e646f626a0a33372030206f62"
        "6a0a3c3c0a2f54797065202f506167650a2f436f6e74656e74"
        "73203338203020520a2f5265736f7572636573203336203020"
        "520a2f4d65646961426f78205b302030203134342e30303120"
        "3135332e3936335d0a2f506172656e74203233203020520a3e"
        "3e0a656e646f626a0a33362030206f626a0a3c3c0a2f50726f"
        "63536574205b202f504446205d0a3e3e0a656e646f626a0a34"
        "312030206f626a0a3c3c0a2f4c656e67746820353320202020"
        "202020200a3e3e0a73747265616d0a710a3120302030203120"
        "373220373220636d0a5b5d3020642030204a20302077203020"
        "30206d203020392e393633206c20530a510a0a656e64737472"
        "65616d0a656e646f626a0a34302030206f626a0a3c3c0a2f54"
        "797065202f506167650a2f436f6e74656e7473203431203020"
        "520a2f5265736f7572636573203339203020520a2f4d656469"
        "61426f78205b302030203134342e303031203135332e393633"
        "5d0a2f506172656e74203432203020520a3e3e0a656e646f62"
        "6a0a33392030206f626a0a3c3c0a2f50726f63536574205b20"
        "2f504446205d0a3e3e0a656e646f626a0a34352030206f626a"
        "0a3c3c0a2f4c656e67746820353720202020202020200a3e3e"
        "0a73747265616d0a710a312030203020312037322e30303120"
        "373220636d0a5b5d3020642030204a2030207720302030206d"
        "203020392e393633206c20530a510a0a656e6473747265616d"
        "0a656e646f626a0a34342030206f626a0a3c3c0a2f54797065"
        "202f506167650a2f436f6e74656e7473203435203020520a2f"
        "5265736f7572636573203433203020520a2f4d65646961426f"
        "78205b302030203134342e303031203135332e3936335d0a2f"
        "506172656e74203432203020520a3e3e0a656e646f626a0a34"
        "332030206f626a0a3c3c0a2f50726f63536574205b202f5044"
        "46205d0a3e3e0a656e646f626a0a34382030206f626a0a3c3c"
        "0a2f4c656e67746820353320202020202020200a3e3e0a7374"
        "7265616d0a710a3120302030203120373220373220636d0a5b"
        "5d3020642030204a2030207720302030206d203020392e3936"
        "33206c20530a510a0a656e6473747265616d0a656e646f626a"
        "0a34372030206f626a0a3c3c0a2f54797065202f506167650a"
        "2f436f6e74656e7473203438203020520a2f5265736f757263"
        "6573203436203020520a2f4d65646961426f78205b30203020"
        "3134342e303031203135332e3936335d0a2f506172656e7420"
        "3432203020520a3e3e0a656e646f626a0a34362030206f626a"
        "0a3c3c0a2f50726f63536574205b202f504446205d0a3e3e0a"
        "656e646f626a0a35312030206f626a0a3c3c0a2f4c656e6774"
        "6820353720202020202020200a3e3e0a73747265616d0a710a"
        "312030203020312037322e30303120373220636d0a5b5d3020"
        "642030204a2030207720302030206d203020392e393633206c"
        "20530a510a0a656e6473747265616d0a656e646f626a0a3530"
        "2030206f626a0a3c3c0a2f54797065202f506167650a2f436f"
        "6e74656e7473203531203020520a2f5265736f757263657320"
        "3439203020520a2f4d65646961426f78205b30203020313434"
        "2e303031203135332e3936335d0a2f506172656e7420343220"
        "3020520a3e3e0a656e646f626a0a34392030206f626a0a3c3c"
        "0a2f50726f63536574205b202f504446205d0a3e3e0a656e64"
        "6f626a0a342030206f626a0a3c3c0a2f54797065202f506167"
        "65730a2f436f756e7420360a2f506172656e74203532203020"
        "520a2f4b696473205b32203020522036203020522039203020"
        "522031322030205220313520302052203138203020525d0a3e"
        "3e0a656e646f626a0a32332030206f626a0a3c3c0a2f547970"
        "65202f50616765730a2f436f756e7420360a2f506172656e74"
        "203532203020520a2f4b696473205b32312030205220323520"
        "30205220323820302052203331203020522033342030205220",
        "3337203020525d0a3e3e0a656e646f626a0a34322030206f62"
        "6a0a3c3c0a2f54797065202f50616765730a2f436f756e7420"
        "340a2f506172656e74203532203020520a2f4b696473205b34"
        "30203020522034342030205220343720302052203530203020"
        "525d0a3e3e0a656e646f626a0a35322030206f626a0a3c3c0a"
        "2f54797065202f50616765730a2f436f756e742031360a2f4b"
        "696473205b342030205220323320302052203432203020525d"
        "0a3e3e0a656e646f626a0a35332030206f626a0a3c3c0a2f54"
        "797065202f436174616c6f670a2f5061676573203532203020"
        "520a3e3e0a656e646f626a0a35342030206f626a0a3c3c0a2f"
        "50726f647563657220287064665465582d312e34302e323529"
        "0a2f43726561746f722028546558290a2f5472617070656420"
        "2f46616c73650a3e3e0a656e646f626a0a787265660a302035"
        "350a303030303030303030302036353533352066200a303030"
        "30303030323334203030303030206e200a3030303030303031"
        "3236203030303030206e200a30303030303030303135203030"
        "303030206e200a30303030303034333034203030303030206e"
        "200a30303030303030353030203030303030206e200a303030"
        "30303030333838203030303030206e200a3030303030303032"
        "3733203030303030206e200a30303030303030373630203030"
        "303030206e200a30303030303030363531203030303030206e"
        "200a30303030303030353339203030303030206e200a303030"
        "30303031303330203030303030206e200a3030303030303039"
        "3135203030303030206e200a30303030303030373939203030"
        "303030206e200a30303030303031323937203030303030206e"
        "200a30303030303031313832203030303030206e200a303030"
        "30303031303730203030303030206e200a3030303030303135"
        "3638203030303030206e200a30303030303031343533203030"
        "303030206e200a30303030303031333337203030303030206e"
        "200a30303030303031383336203030303030206e200a303030"
        "30303031373230203030303030206e200a3030303030303136"
        "3038203030303030206e200a30303030303034343039203030"
        "303030206e200a30303030303032313038203030303030206e"
        "200a30303030303031393932203030303030206e200a303030"
        "30303031383736203030303030206e200a3030303030303233"
        "3732203030303030206e200a30303030303032323630203030"
        "303030206e200a30303030303032313438203030303030206e"
        "200a30303030303032363434203030303030206e200a303030"
        "30303032353238203030303030206e200a3030303030303234"
        "3132203030303030206e200a30303030303032393132203030"
        "303030206e200a30303030303032373936203030303030206e"
        "200a30303030303032363834203030303030206e200a303030"
        "30303033313834203030303030206e200a3030303030303330"
        "3638203030303030206e200a30303030303032393532203030"
        "303030206e200a30303030303033343532203030303030206e"
        "200a30303030303033333336203030303030206e200a303030"
        "30303033323234203030303030206e200a3030303030303435"
        "3138203030303030206e200a30303030303033373234203030"
        "303030206e200a30303030303033363038203030303030206e"
        "200a30303030303033343932203030303030206e200a303030"
        "30303033393932203030303030206e200a3030303030303338"
        "3736203030303030206e200a30303030303033373634203030"
        "303030206e200a30303030303034323634203030303030206e"
        "200a30303030303034313438203030303030206e200a303030"
        "30303034303332203030303030206e200a3030303030303436"
        "3133203030303030206e200a30303030303034363836203030"
        "303030206e200a30303030303034373337203030303030206e"
        "200a747261696c65720a3c3c202f53697a652035350a2f526f"
        "6f74203533203020520a2f496e666f203534203020520a203e"
        "3e0a7374617274787265660a343831370a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* The width the file states for a character is worked out against the size
   the file states, not the size the engine has, and a run of one character
   shows where that puts the text; see docs/DECISIONS.md,
   the-width-a-file-states. */
static int test_the_width_a_file_states(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=cmr7 \\font\\b=cmmi10 \\font\\c=cmss10"
        " \\font\\d=cmsy10 \\hsize=300pt \\vsize=200pt \\pa"
        "rindent=0pt \\hbadness=10000 \\vbadness=10000 \\sh"
        "ipout\\hbox{\\a ao\\b G\\c W\\d K}\\shipout\\hbox{"
        "\\a aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313334202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120362e3937333820546620373220"
        "3732205464205b28616f295d544a2f463220392e3936323620"
        "546620372e3934322030205464205b2847295d544a2f463320"
        "392e3936323620546620372e3833342030205464205b285729"
        "5d544a2f463420392e3936323620546620392e343039203020"
        "5464205b284b295d544a0a45540a0a656e6473747265616d0a"
        "656e646f626a0a322030206f626a0a3c3c0a2f54797065202f"
        "506167650a2f436f6e74656e74732033203020520a2f526573"
        "6f75726365732031203020520a2f4d65646961426f78205b30"
        "2030203137362e373736203135302e3931395d0a2f50617265"
        "6e742038203020520a3e3e0a656e646f626a0a312030206f62"
        "6a0a3c3c0a2f466f6e74203c3c202f4631203420302052202f"
        "4632203520302052202f4633203620302052202f4634203720"
        "302052203e3e0a2f50726f63536574205b202f504446202f54"
        "657874205d0a3e3e0a656e646f626a0a31312030206f626a0a"
        "3c3c0a2f4c656e67746820313437202020202020200a3e3e0a"
        "73747265616d0a42540a2f463120362e393733382054662037"
        "32203732205464205b28616161616161616161616129312861"
        "61616161616161616161616161616161616161612931286161"
        "61616161616161616161616161616161616161293128616161"
        "61616161616161616161616161616161612931286161616161"
        "616161616161616161616161616161612931286161295d544a"
        "0a45540a0a656e6473747265616d0a656e646f626a0a313020"
        "30206f626a0a3c3c0a2f54797065202f506167650a2f436f6e"
        "74656e7473203131203020520a2f5265736f75726365732039"
        "203020520a2f4d65646961426f78205b302030203532352e32"
        "3339203134372e3030335d0a2f506172656e74203820302052"
        "0a3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f466f"
        "6e74203c3c202f4631203420302052203e3e0a2f50726f6353"
        "6574205b202f504446202f54657874205d0a3e3e0a656e646f"
        "626a0a31322030206f626a0a5b3736325d0a656e646f626a0a"
        "31332030206f626a0a3c3c0a2f54797065202f466f6e744465"
        "7363726970746f720a2f466f6e744e616d65202f434d535931"
        "300a2f466c6167732033340a2f466f6e7442426f78205b3020"
        "2d3139342031303030203735305d0a2f417363656e74203735"
        "300a2f436170486569676874203638330a2f44657363656e74"
        "202d3139340a2f4974616c6963416e676c6520300a2f537465"
        "6d56203333330a2f58486569676874203433310a3e3e0a656e"
        "646f626a0a31342030206f626a0a5b3934342e355d0a656e64"
        "6f626a0a31352030206f626a0a3c3c0a2f54797065202f466f"
        "6e7444657363726970746f720a2f466f6e744e616d65202f43"
        "4d535331300a2f466c6167732033340a2f466f6e7442426f78"
        "205b30202d3139342031303030203639345d0a2f417363656e"
        "74203639340a2f436170486569676874203639340a2f446573"
        "63656e74202d3139340a2f4974616c6963416e676c6520300a"
        "2f5374656d562039330a2f58486569676874203434340a3e3e"
        "0a656e646f626a0a31362030206f626a0a5b3738362e335d0a"
        "656e646f626a0a31372030206f626a0a3c3c0a2f5479706520"
        "2f466f6e7444657363726970746f720a2f466f6e744e616d65"
        "202f434d4d4931300a2f466c6167732033340a2f466f6e7442"
        "426f78205b30202d3139342031303030203639345d0a2f4173"
        "63656e74203639340a2f436170486569676874203638330a2f"
        "44657363656e74202d3139340a2f4974616c6963416e676c65"
        "20300a2f5374656d56203136370a2f58486569676874203433"
        "310a3e3e0a656e646f626a0a31382030206f626a0a5b353639"
        "2e3520363331203530372e3920363331203530372e39203335"
        "342e32203536392e3520363331203332332e34203335342e32"
        "203630302e32203332332e34203933382e3520363331203536",
        "392e355d0a656e646f626a0a31392030206f626a0a3c3c0a2f"
        "54797065202f466f6e7444657363726970746f720a2f466f6e"
        "744e616d65202f434d52370a2f466c6167732033340a2f466f"
        "6e7442426f78205b30202d3139342031313339203639345d0a"
        "2f417363656e74203639340a2f436170486569676874203638"
        "330a2f44657363656e74202d3139340a2f4974616c6963416e"
        "676c6520300a2f5374656d56203130380a2f58486569676874"
        "203433310a3e3e0a656e646f626a0a352030206f626a0a3c3c"
        "0a2f54797065202f466f6e740a2f53756274797065202f5479"
        "7065310a2f42617365466f6e74202f434d4d4931300a2f466f"
        "6e7444657363726970746f72203137203020520a2f46697273"
        "74436861722037310a2f4c617374436861722037310a2f5769"
        "64746873203136203020520a3e3e0a656e646f626a0a342030"
        "206f626a0a3c3c0a2f54797065202f466f6e740a2f53756274"
        "797065202f54797065310a2f42617365466f6e74202f434d52"
        "370a2f466f6e7444657363726970746f72203139203020520a"
        "2f4669727374436861722039370a2f4c617374436861722031"
        "31310a2f576964746873203138203020520a3e3e0a656e646f"
        "626a0a362030206f626a0a3c3c0a2f54797065202f466f6e74"
        "0a2f53756274797065202f54797065310a2f42617365466f6e"
        "74202f434d535331300a2f466f6e7444657363726970746f72"
        "203135203020520a2f4669727374436861722038370a2f4c61"
        "7374436861722038370a2f576964746873203134203020520a"
        "3e3e0a656e646f626a0a372030206f626a0a3c3c0a2f547970"
        "65202f466f6e740a2f53756274797065202f54797065310a2f"
        "42617365466f6e74202f434d535931300a2f466f6e74446573"
        "63726970746f72203133203020520a2f466972737443686172"
        "2037350a2f4c617374436861722037350a2f57696474687320"
        "3132203020520a3e3e0a656e646f626a0a382030206f626a0a"
        "3c3c0a2f54797065202f50616765730a2f436f756e7420320a"
        "2f4b696473205b3220302052203130203020525d0a3e3e0a65"
        "6e646f626a0a32302030206f626a0a3c3c0a2f54797065202f"
        "436174616c6f670a2f50616765732038203020520a3e3e0a65"
        "6e646f626a0a32312030206f626a0a3c3c0a2f50726f647563"
        "657220287064665465582d312e34302e3235290a2f43726561"
        "746f722028546558290a2f54726170706564202f46616c7365"
        "0a3e3e0a656e646f626a0a787265660a302032320a30303030"
        "3030303030302036353533352066200a303030303030303331"
        "39203030303030206e200a3030303030303032303720303030"
        "3030206e200a30303030303030303135203030303030206e20"
        "0a30303030303031383232203030303030206e200a30303030"
        "303031363930203030303030206e200a303030303030313935"
        "33203030303030206e200a3030303030303230383520303030"
        "3030206e200a30303030303032323137203030303030206e20"
        "0a30303030303030373336203030303030206e200a30303030"
        "303030363232203030303030206e200a303030303030303431"
        "36203030303030206e200a3030303030303038303320303030"
        "3030206e200a30303030303030383235203030303030206e20"
        "0a30303030303031303035203030303030206e200a30303030"
        "303031303239203030303030206e200a303030303030313230"
        "38203030303030206e200a3030303030303132333220303030"
        "3030206e200a30303030303031343132203030303030206e20"
        "0a30303030303031353132203030303030206e200a30303030"
        "303032323831203030303030206e200a303030303030323333"
        "31203030303030206e200a747261696c65720a3c3c202f5369"
        "7a652032320a2f526f6f74203230203020520a2f496e666f20"
        "3231203020520a203e3e0a7374617274787265660a32343131"
        "0a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

/* The file's text stands where the place it named puts it, taken towards the
   engine's own place: a run of one character started at three places that
   round differently shows where the corrections fall. See
   docs/DECISIONS.md, the-text-position-in-the-file. */
static int test_where_a_correction_leaves_the_text(void)
{
    /* A correction moves the file's text by its own worth of scaled
       points, and where that leaves it is taken towards the engine's
       own place -- not truncated towards zero, which loses a scaled
       point at every correction and puts the later ones a glyph out. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=16000pt \\vsize=300p"
        "t \\parindent=0pt \\hbadness=10000 \\vbadness=1000"
        "0\\shipout\\hbox{mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm}\\shipout\\h"
        "box{abcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghijabcdefghijabcdefghijabcdef"
        "ghijabcdefghijabcdefghij}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820353230202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b286d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31"
        "286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d"
        "31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d29"
        "2d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "292d31286d6d6d6d6d6d6d6d6d6d6d6d6d295d544a0a45540a"
        "0a656e6473747265616d0a656e646f626a0a322030206f626a"
        "0a3c3c0a2f54797065202f506167650a2f436f6e74656e7473"
        "2033203020520a2f5265736f75726365732031203020520a2f"
        "4d65646961426f78205b30203020333633302e393335203134"
        "382e3238395d0a2f506172656e742035203020520a3e3e0a65"
        "6e646f626a0a312030206f626a0a3c3c0a2f466f6e74203c3c"
        "202f4631203420302052203e3e0a2f50726f63536574205b20"
        "2f504446202f54657874205d0a3e3e0a656e646f626a0a3820"
        "30206f626a0a3c3c0a2f4c656e677468203731302020202020"
        "20200a3e3e0a73747265616d0a42540a2f463120392e393632"
        "362054662037322037332e393337205464205b286162292d32"
        "3828636465666768696a6162292d323728636465666768696a"
        "6162292d323828636465666768696a6162292d323828636465"
        "666768696a6162292d323728636465666768696a6162292d32"
        "3828636465666768696a6162292d323828636465666768696a"
        "6162292d323728636465666768696a6162292d323828636465"
        "666768696a6162292d323828636465666768696a6162292d32"
        "3728636465666768696a6162292d323828636465666768696a"
        "6162292d323828636465666768696a2931286162292d323828"
        "636465666768696a6162292d323828636465666768696a6162"
        "292d323828636465666768293128696a6162292d3238286364"
        "65666768696a6162292d323828636465666768696a6162292d"
        "32372863292d31286429312865292d3128662931286768696a"
        "6162292d323828636465666768696a6162292d323828636465"
        "666768696a6162292d323728636465666768696a6162292d32"
        "3828636465666768696a6162292d323828636465666768696a"
        "6162292d323728636465666768696a6162292d323828636465"
        "666768696a6162292d323828636465666768696a6162292d32"
        "3728636465666768696a6162292d323828636465666768696a"
        "6162292d323828636465666768696a6162292d323728636465"
        "666768696a6162292d323828636465666768696a6162292d32"
        "3828636465666768696a6162292d323728636465666768696a"
        "6162292d323828636465666768696a6162292d323828636465"
        "666768696a2931286162292d323828636465666768696a6162"
        "292d323828636465666768696a6162292d3238286364656667"
        "68293128696a6162292d323828636465666768696a6162292d"
        "323828636465666768696a6162292d32372863292d31286429",
        "312865292d3128662931286768696a295d544a0a45540a0a65"
        "6e6473747265616d0a656e646f626a0a372030206f626a0a3c"
        "3c0a2f54797065202f506167650a2f436f6e74656e74732038"
        "203020520a2f5265736f75726365732036203020520a2f4d65"
        "646961426f78205b30203020323031352e333231203135322e"
        "3835365d0a2f506172656e742035203020520a3e3e0a656e64"
        "6f626a0a362030206f626a0a3c3c0a2f466f6e74203c3c202f"
        "4631203420302052203e3e0a2f50726f63536574205b202f50"
        "4446202f54657874205d0a3e3e0a656e646f626a0a39203020"
        "6f626a0a5b353030203535352e36203434342e34203535352e"
        "36203434342e34203330352e3620353030203535352e362032"
        "37372e38203330352e36203532372e38203237372e38203833"
        "332e335d0a656e646f626a0a31302030206f626a0a3c3c0a2f"
        "54797065202f466f6e7444657363726970746f720a2f466f6e"
        "744e616d65202f434d5231300a2f466c6167732033340a2f46"
        "6f6e7442426f78205b30202d3139342031303030203639345d"
        "0a2f417363656e74203639340a2f4361704865696768742036"
        "38330a2f44657363656e74202d3139340a2f4974616c696341"
        "6e676c6520300a2f5374656d562039330a2f58486569676874"
        "203433310a3e3e0a656e646f626a0a342030206f626a0a3c3c"
        "0a2f54797065202f466f6e740a2f53756274797065202f5479"
        "7065310a2f42617365466f6e74202f434d5231300a2f466f6e"
        "7444657363726970746f72203130203020520a2f4669727374"
        "436861722039370a2f4c61737443686172203130390a2f5769"
        "647468732039203020520a3e3e0a656e646f626a0a35203020"
        "6f626a0a3c3c0a2f54797065202f50616765730a2f436f756e"
        "7420320a2f4b696473205b32203020522037203020525d0a3e"
        "3e0a656e646f626a0a31312030206f626a0a3c3c0a2f547970"
        "65202f436174616c6f670a2f50616765732035203020520a3e"
        "3e0a656e646f626a0a31322030206f626a0a3c3c0a2f50726f"
        "647563657220287064665465582d312e34302e3235290a2f43"
        "726561746f722028546558290a2f54726170706564202f4661"
        "6c73650a3e3e0a656e646f626a0a787265660a302031330a30"
        "3030303030303030302036353533352066200a303030303030"
        "30373036203030303030206e200a3030303030303035393320"
        "3030303030206e200a30303030303030303135203030303030"
        "206e200a30303030303031393930203030303030206e200a30"
        "303030303032313231203030303030206e200a303030303030"
        "31363534203030303030206e200a3030303030303135343120"
        "3030303030206e200a30303030303030373733203030303030"
        "206e200a30303030303031373231203030303030206e200a30"
        "303030303031383132203030303030206e200a303030303030"
        "32313834203030303030206e200a3030303030303232333420"
        "3030303030206e200a747261696c65720a3c3c202f53697a65"
        "2031330a2f526f6f74203131203020520a2f496e666f203132"
        "203020520a203e3e0a7374617274787265660a323331340a25"
        "25454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_a_step_too_short_to_name(void)
{
    /* A glyph whose place is a step of less than a thousandth of a big
       point from the line it is on stays on that line, and the step is
       measured from where the file's text stands, not from the engine's:
       the three starting places here put the turn at a different step
       each time. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=16000pt \\vsize=300p"
        "t \\parindent=0pt\\shipout\\hbox{\\raise 0sp \\hbo"
        "x{a\\raise 65sp \\hbox{b}c}}\\shipout\\hbox{\\rais"
        "e 0sp \\hbox{a\\raise 66sp \\hbox{b}c}}\\shipout\\"
        "hbox{\\raise 16sp \\hbox{a\\raise 49sp \\hbox{b}c}"
        "}\\shipout\\hbox{\\raise 16sp \\hbox{a\\raise 50sp"
        " \\hbox{b}c}}\\shipout\\hbox{\\raise 60sp \\hbox{a"
        "\\raise 70sp \\hbox{b}c}}\\shipout\\hbox{\\raise 6"
        "0sp \\hbox{a\\raise 71sp \\hbox{b}c}}\\shipout\\hb"
        "ox{a\\lower 65sp \\hbox{b}c}\\shipout\\hbox{a\\low"
        "er 66sp \\hbox{b}c}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820333920202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b28616263295d544a0a45540a0a656e647374"
        "7265616d0a656e646f626a0a322030206f626a0a3c3c0a2f54"
        "797065202f506167650a2f436f6e74656e7473203320302052"
        "0a2f5265736f75726365732031203020520a2f4d6564696142"
        "6f78205b302030203135382e393434203135302e3931395d0a"
        "2f506172656e742035203020520a3e3e0a656e646f626a0a31"
        "2030206f626a0a3c3c0a2f466f6e74203c3c202f4631203420"
        "302052203e3e0a2f50726f63536574205b202f504446202f54"
        "657874205d0a3e3e0a656e646f626a0a382030206f626a0a3c"
        "3c0a2f4c656e67746820383420202020202020200a3e3e0a73"
        "747265616d0a42540a2f463120392e39363236205466203732"
        "203732205464205b2861295d544a20342e39383120302e3030"
        "31205464205b2862295d544a20352e353335202d302e303031"
        "205464205b2863295d544a0a45540a0a656e6473747265616d"
        "0a656e646f626a0a372030206f626a0a3c3c0a2f5479706520"
        "2f506167650a2f436f6e74656e74732038203020520a2f5265"
        "736f75726365732036203020520a2f4d65646961426f78205b"
        "302030203135382e393434203135302e39325d0a2f50617265"
        "6e742035203020520a3e3e0a656e646f626a0a362030206f62"
        "6a0a3c3c0a2f466f6e74203c3c202f4631203420302052203e"
        "3e0a2f50726f63536574205b202f504446202f54657874205d"
        "0a3e3e0a656e646f626a0a31312030206f626a0a3c3c0a2f4c"
        "656e67746820333920202020202020200a3e3e0a7374726561"
        "6d0a42540a2f463120392e3936323620546620373220373220"
        "5464205b28616263295d544a0a45540a0a656e647374726561"
        "6d0a656e646f626a0a31302030206f626a0a3c3c0a2f547970"
        "65202f506167650a2f436f6e74656e7473203131203020520a"
        "2f5265736f75726365732039203020520a2f4d65646961426f"
        "78205b302030203135382e393434203135302e3931395d0a2f"
        "506172656e742035203020520a3e3e0a656e646f626a0a3920"
        "30206f626a0a3c3c0a2f466f6e74203c3c202f463120342030"
        "2052203e3e0a2f50726f63536574205b202f504446202f5465"
        "7874205d0a3e3e0a656e646f626a0a31342030206f626a0a3c"
        "3c0a2f4c656e67746820363120202020202020200a3e3e0a73"
        "747265616d0a42540a2f463120392e39363236205466203732"
        "203732205464205b2861295d544a20342e39383120302e3030"
        "31205464205b286263295d544a0a45540a0a656e6473747265"
        "616d0a656e646f626a0a31332030206f626a0a3c3c0a2f5479"
        "7065202f506167650a2f436f6e74656e747320313420302052"
        "0a2f5265736f7572636573203132203020520a2f4d65646961"
        "426f78205b302030203135382e393434203135302e39325d0a"
        "2f506172656e742035203020520a3e3e0a656e646f626a0a31"
        "322030206f626a0a3c3c0a2f466f6e74203c3c202f46312034"
        "20302052203e3e0a2f50726f63536574205b202f504446202f"
        "54657874205d0a3e3e0a656e646f626a0a31372030206f626a"
        "0a3c3c0a2f4c656e67746820343320202020202020200a3e3e"
        "0a73747265616d0a42540a2f463120392e3936323620546620"
        "37322037322e303031205464205b28616263295d544a0a4554"
        "0a0a656e6473747265616d0a656e646f626a0a31362030206f"
        "626a0a3c3c0a2f54797065202f506167650a2f436f6e74656e"
        "7473203137203020520a2f5265736f75726365732031352030"
        "20520a2f4d65646961426f78205b302030203135382e393434"
        "203135302e39325d0a2f506172656e742035203020520a3e3e"
        "0a656e646f626a0a31352030206f626a0a3c3c0a2f466f6e74"
        "203c3c202f4631203420302052203e3e0a2f50726f63536574"
        "205b202f504446202f54657874205d0a3e3e0a656e646f626a"
        "0a32302030206f626a0a3c3c0a2f4c656e6774682038382020",
        "2020202020200a3e3e0a73747265616d0a42540a2f46312039"
        "2e393632362054662037322037322e303031205464205b2861"
        "295d544a20342e39383120302e303031205464205b2862295d"
        "544a20352e353335202d302e303031205464205b2863295d54"
        "4a0a45540a0a656e6473747265616d0a656e646f626a0a3139"
        "2030206f626a0a3c3c0a2f54797065202f506167650a2f436f"
        "6e74656e7473203230203020520a2f5265736f757263657320"
        "3138203020520a2f4d65646961426f78205b30203020313538"
        "2e393434203135302e39325d0a2f506172656e742035203020"
        "520a3e3e0a656e646f626a0a31382030206f626a0a3c3c0a2f"
        "466f6e74203c3c202f4631203420302052203e3e0a2f50726f"
        "63536574205b202f504446202f54657874205d0a3e3e0a656e"
        "646f626a0a32332030206f626a0a3c3c0a2f4c656e67746820"
        "343320202020202020200a3e3e0a73747265616d0a42540a2f"
        "463120392e393632362054662037322037322e303031205464"
        "205b28616263295d544a0a45540a0a656e6473747265616d0a"
        "656e646f626a0a32322030206f626a0a3c3c0a2f5479706520"
        "2f506167650a2f436f6e74656e7473203233203020520a2f52"
        "65736f7572636573203231203020520a2f4d65646961426f78"
        "205b302030203135382e393434203135302e3931395d0a2f50"
        "6172656e74203234203020520a3e3e0a656e646f626a0a3231"
        "2030206f626a0a3c3c0a2f466f6e74203c3c202f4631203420"
        "302052203e3e0a2f50726f63536574205b202f504446202f54"
        "657874205d0a3e3e0a656e646f626a0a32372030206f626a0a"
        "3c3c0a2f4c656e67746820383820202020202020200a3e3e0a"
        "73747265616d0a42540a2f463120392e393632362054662037"
        "322037322e303031205464205b2861295d544a20342e393831"
        "202d302e303031205464205b2862295d544a20352e35333520"
        "302e303031205464205b2863295d544a0a45540a0a656e6473"
        "747265616d0a656e646f626a0a32362030206f626a0a3c3c0a"
        "2f54797065202f506167650a2f436f6e74656e747320323720"
        "3020520a2f5265736f7572636573203235203020520a2f4d65"
        "646961426f78205b302030203135382e393434203135302e39"
        "31395d0a2f506172656e74203234203020520a3e3e0a656e64"
        "6f626a0a32352030206f626a0a3c3c0a2f466f6e74203c3c20"
        "2f4631203420302052203e3e0a2f50726f63536574205b202f"
        "504446202f54657874205d0a3e3e0a656e646f626a0a323820"
        "30206f626a0a5b353030203535352e36203434342e345d0a65"
        "6e646f626a0a32392030206f626a0a3c3c0a2f54797065202f"
        "466f6e7444657363726970746f720a2f466f6e744e616d6520"
        "2f434d5231300a2f466c6167732033340a2f466f6e7442426f"
        "78205b30202d3139342031303030203639345d0a2f41736365"
        "6e74203639340a2f436170486569676874203638330a2f4465"
        "7363656e74202d3139340a2f4974616c6963416e676c652030"
        "0a2f5374656d562039330a2f58486569676874203433310a3e"
        "3e0a656e646f626a0a342030206f626a0a3c3c0a2f54797065"
        "202f466f6e740a2f53756274797065202f54797065310a2f42"
        "617365466f6e74202f434d5231300a2f466f6e744465736372"
        "6970746f72203239203020520a2f4669727374436861722039"
        "370a2f4c617374436861722039390a2f576964746873203238"
        "203020520a3e3e0a656e646f626a0a352030206f626a0a3c3c"
        "0a2f54797065202f50616765730a2f436f756e7420360a2f50"
        "6172656e74203330203020520a2f4b696473205b3220302052"
        "20372030205220313020302052203133203020522031362030"
        "2052203139203020525d0a3e3e0a656e646f626a0a32342030"
        "206f626a0a3c3c0a2f54797065202f50616765730a2f436f75"
        "6e7420320a2f506172656e74203330203020520a2f4b696473"
        "205b323220302052203236203020525d0a3e3e0a656e646f62"
        "6a0a33302030206f626a0a3c3c0a2f54797065202f50616765"
        "730a2f436f756e7420380a2f4b696473205b35203020522032",
        "34203020525d0a3e3e0a656e646f626a0a33312030206f626a"
        "0a3c3c0a2f54797065202f436174616c6f670a2f5061676573"
        "203330203020520a3e3e0a656e646f626a0a33322030206f62"
        "6a0a3c3c0a2f50726f647563657220287064665465582d312e"
        "34302e3235290a2f43726561746f722028546558290a2f5472"
        "6170706564202f46616c73650a3e3e0a656e646f626a0a7872"
        "65660a302033330a3030303030303030303020363535333520"
        "66200a30303030303030323234203030303030206e200a3030"
        "3030303030313132203030303030206e200a30303030303030"
        "303135203030303030206e200a303030303030323633342030"
        "30303030206e200a3030303030303237363520303030303020"
        "6e200a30303030303030353434203030303030206e200a3030"
        "3030303030343333203030303030206e200a30303030303030"
        "323931203030303030206e200a303030303030303832332030"
        "30303030206e200a3030303030303037303920303030303020"
        "6e200a30303030303030363131203030303030206e200a3030"
        "3030303031313234203030303030206e200a30303030303031"
        "303130203030303030206e200a303030303030303839302030"
        "30303030206e200a3030303030303134303820303030303020"
        "6e200a30303030303031323934203030303030206e200a3030"
        "3030303031313932203030303030206e200a30303030303031"
        "373337203030303030206e200a303030303030313632332030"
        "30303030206e200a3030303030303134373620303030303020"
        "6e200a30303030303032303233203030303030206e200a3030"
        "3030303031393037203030303030206e200a30303030303031"
        "383035203030303030206e200a303030303030323837312030"
        "30303030206e200a3030303030303233353420303030303020"
        "6e200a30303030303032323338203030303030206e200a3030"
        "3030303032303931203030303030206e200a30303030303032"
        "343232203030303030206e200a303030303030323435362030"
        "30303030206e200a3030303030303239353220303030303020"
        "6e200a30303030303033303137203030303030206e200a3030"
        "3030303033303638203030303030206e200a747261696c6572"
        "0a3c3c202f53697a652033330a2f526f6f7420333120302052"
        "0a2f496e666f203332203020520a203e3e0a73746172747872"
        "65660a333134380a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_a_place_named_for_a_font_of_its_own(void)
{
    /* A place named because the glyph wants a font of its own names no step
       down: the line the file's text is on has not moved, however far off it
       the glyph stands, until the step is one the file could name. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\font\\b=cmbx10 \\t \\hsize=160"
        "00pt \\vsize=300pt \\parindent=0pt\\shipout\\hbox{"
        "a\\raise 65sp \\hbox{\\b b}\\t c}\\shipout\\hbox{a"
        "\\raise 66sp \\hbox{\\b b}\\t c}\\shipout\\hbox{a"
        "\\lower 65sp \\hbox{\\b b}\\t c}\\shipout\\hbox{a"
        "\\lower 66sp \\hbox{\\b b}\\t c}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313031202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b2861295d544a2f463220392e393632362054"
        "6620342e3938312030205464205b2862295d544a2f46312039"
        "2e3936323620546620362e3336352030205464205b2863295d"
        "544a0a45540a0a656e6473747265616d0a656e646f626a0a32"
        "2030206f626a0a3c3c0a2f54797065202f506167650a2f436f"
        "6e74656e74732033203020520a2f5265736f75726365732031"
        "203020520a2f4d65646961426f78205b302030203135392e37"
        "3734203135302e3931395d0a2f506172656e74203620302052"
        "0a3e3e0a656e646f626a0a312030206f626a0a3c3c0a2f466f"
        "6e74203c3c202f4631203420302052202f4632203520302052"
        "203e3e0a2f50726f63536574205b202f504446202f54657874"
        "205d0a3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f"
        "4c656e67746820313130202020202020200a3e3e0a73747265"
        "616d0a42540a2f463120392e39363236205466203732203732"
        "205464205b2861295d544a2f463220392e3936323620546620"
        "342e39383120302e303031205464205b2862295d544a2f4631"
        "20392e3936323620546620362e333635202d302e3030312054"
        "64205b2863295d544a0a45540a0a656e6473747265616d0a65"
        "6e646f626a0a382030206f626a0a3c3c0a2f54797065202f50"
        "6167650a2f436f6e74656e74732039203020520a2f5265736f"
        "75726365732037203020520a2f4d65646961426f78205b3020"
        "30203135392e373734203135302e39325d0a2f506172656e74"
        "2036203020520a3e3e0a656e646f626a0a372030206f626a0a"
        "3c3c0a2f466f6e74203c3c202f4631203420302052202f4632"
        "203520302052203e3e0a2f50726f63536574205b202f504446"
        "202f54657874205d0a3e3e0a656e646f626a0a31322030206f"
        "626a0a3c3c0a2f4c656e67746820313035202020202020200a"
        "3e3e0a73747265616d0a42540a2f463120392e393632362054"
        "662037322037322e303031205464205b2861295d544a2f4632"
        "20392e3936323620546620342e3938312030205464205b2862"
        "295d544a2f463120392e3936323620546620362e3336352030"
        "205464205b2863295d544a0a45540a0a656e6473747265616d"
        "0a656e646f626a0a31312030206f626a0a3c3c0a2f54797065"
        "202f506167650a2f436f6e74656e7473203132203020520a2f"
        "5265736f7572636573203130203020520a2f4d65646961426f"
        "78205b302030203135392e373734203135302e3931395d0a2f"
        "506172656e742036203020520a3e3e0a656e646f626a0a3130"
        "2030206f626a0a3c3c0a2f466f6e74203c3c202f4631203420"
        "302052202f4632203520302052203e3e0a2f50726f63536574"
        "205b202f504446202f54657874205d0a3e3e0a656e646f626a"
        "0a31352030206f626a0a3c3c0a2f4c656e6774682031313420"
        "2020202020200a3e3e0a73747265616d0a42540a2f46312039"
        "2e393632362054662037322037322e303031205464205b2861"
        "295d544a2f463220392e3936323620546620342e393831202d"
        "302e303031205464205b2862295d544a2f463120392e393632"
        "3620546620362e33363520302e303031205464205b2863295d"
        "544a0a45540a0a656e6473747265616d0a656e646f626a0a31"
        "342030206f626a0a3c3c0a2f54797065202f506167650a2f43"
        "6f6e74656e7473203135203020520a2f5265736f7572636573"
        "203133203020520a2f4d65646961426f78205b302030203135"
        "392e373734203135302e3931395d0a2f506172656e74203620"
        "3020520a3e3e0a656e646f626a0a31332030206f626a0a3c3c"
        "0a2f466f6e74203c3c202f4631203420302052202f46322035"
        "20302052203e3e0a2f50726f63536574205b202f504446202f"
        "54657874205d0a3e3e0a656e646f626a0a31362030206f626a"
        "0a5b3633382e395d0a656e646f626a0a31372030206f626a0a"
        "3c3c0a2f54797065202f466f6e7444657363726970746f720a",
        "2f466f6e744e616d65202f434d425831300a2f466c61677320"
        "33340a2f466f6e7442426f78205b30202d3139342031313530"
        "203639345d0a2f417363656e74203639340a2f436170486569"
        "676874203638360a2f44657363656e74202d3139340a2f4974"
        "616c6963416e676c6520300a2f5374656d56203130360a2f58"
        "486569676874203434340a3e3e0a656e646f626a0a31382030"
        "206f626a0a5b353030203535352e36203434342e345d0a656e"
        "646f626a0a31392030206f626a0a3c3c0a2f54797065202f46"
        "6f6e7444657363726970746f720a2f466f6e744e616d65202f"
        "434d5231300a2f466c6167732033340a2f466f6e7442426f78"
        "205b30202d3139342031303030203639345d0a2f417363656e"
        "74203639340a2f436170486569676874203638330a2f446573"
        "63656e74202d3139340a2f4974616c6963416e676c6520300a"
        "2f5374656d562039330a2f58486569676874203433310a3e3e"
        "0a656e646f626a0a352030206f626a0a3c3c0a2f5479706520"
        "2f466f6e740a2f53756274797065202f54797065310a2f4261"
        "7365466f6e74202f434d425831300a2f466f6e744465736372"
        "6970746f72203137203020520a2f4669727374436861722039"
        "380a2f4c617374436861722039380a2f576964746873203136"
        "203020520a3e3e0a656e646f626a0a342030206f626a0a3c3c"
        "0a2f54797065202f466f6e740a2f53756274797065202f5479"
        "7065310a2f42617365466f6e74202f434d5231300a2f466f6e"
        "7444657363726970746f72203139203020520a2f4669727374"
        "436861722039370a2f4c617374436861722039390a2f576964"
        "746873203138203020520a3e3e0a656e646f626a0a36203020"
        "6f626a0a3c3c0a2f54797065202f50616765730a2f436f756e"
        "7420340a2f4b696473205b3220302052203820302052203131"
        "20302052203134203020525d0a3e3e0a656e646f626a0a3230"
        "2030206f626a0a3c3c0a2f54797065202f436174616c6f670a"
        "2f50616765732036203020520a3e3e0a656e646f626a0a3231"
        "2030206f626a0a3c3c0a2f50726f6475636572202870646654"
        "65582d312e34302e3235290a2f43726561746f722028546558"
        "290a2f54726170706564202f46616c73650a3e3e0a656e646f"
        "626a0a787265660a302032320a303030303030303030302036"
        "353533352066200a3030303030303032383620303030303020"
        "6e200a30303030303030313734203030303030206e200a3030"
        "3030303030303135203030303030206e200a30303030303031"
        "393930203030303030206e200a303030303030313835382030"
        "30303030206e200a3030303030303231323120303030303020"
        "6e200a30303030303030363432203030303030206e200a3030"
        "3030303030353331203030303030206e200a30303030303030"
        "333633203030303030206e200a303030303030303939382030"
        "30303030206e200a3030303030303038383320303030303020"
        "6e200a30303030303030373139203030303030206e200a3030"
        "3030303031333634203030303030206e200a30303030303031"
        "323439203030303030206e200a303030303030313037362030"
        "30303030206e200a3030303030303134343220303030303020"
        "6e200a30303030303031343636203030303030206e200a3030"
        "3030303031363436203030303030206e200a30303030303031"
        "363830203030303030206e200a303030303030323139382030"
        "30303030206e200a3030303030303232343820303030303020"
        "6e200a747261696c65720a3c3c202f53697a652032320a2f52"
        "6f6f74203230203020520a2f496e666f203231203020520a20"
        "3e3e0a7374617274787265660a323332380a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_correction_an_array_cannot_carry(void)
{
    /* A correction of more than 32767 thousandths of the stated size is not
       written inside the array: the file ends the run and names the place
       afresh. The four pages step across the threshold in both directions,
       a scaled point at a time. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\t=cmr10 \\t \\hsize=16000pt \\vsize=300p"
        "t \\parindent=0pt \\hbadness=10000 \\vbadness=1000"
        "0\\shipout\\hbox{a\\kern 21474440sp b}\\shipout\\h"
        "box{a\\kern 21474460sp b}\\shipout\\hbox{a\\kern -"
        "21474440sp b}\\shipout\\hbox{a\\kern -21474460sp b"
        "}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820343620202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b2861292d33323736372862295d544a0a4554"
        "0a0a656e6473747265616d0a656e646f626a0a322030206f62"
        "6a0a3c3c0a2f54797065202f506167650a2f436f6e74656e74"
        "732033203020520a2f5265736f75726365732031203020520a"
        "2f4d65646961426f78205b302030203438302e393636203135"
        "302e3931395d0a2f506172656e742035203020520a3e3e0a65"
        "6e646f626a0a312030206f626a0a3c3c0a2f466f6e74203c3c"
        "202f4631203420302052203e3e0a2f50726f63536574205b20"
        "2f504446202f54657874205d0a3e3e0a656e646f626a0a3820"
        "30206f626a0a3c3c0a2f4c656e677468203538202020202020"
        "20200a3e3e0a73747265616d0a42540a2f463120392e393632"
        "36205466203732203732205464205b2861295d544a20333331"
        "2e3433312030205464205b2862295d544a0a45540a0a656e64"
        "73747265616d0a656e646f626a0a372030206f626a0a3c3c0a"
        "2f54797065202f506167650a2f436f6e74656e747320382030"
        "20520a2f5265736f75726365732036203020520a2f4d656469"
        "61426f78205b302030203438302e393636203135302e393139"
        "5d0a2f506172656e742035203020520a3e3e0a656e646f626a"
        "0a362030206f626a0a3c3c0a2f466f6e74203c3c202f463120"
        "3420302052203e3e0a2f50726f63536574205b202f50444620"
        "2f54657874205d0a3e3e0a656e646f626a0a31312030206f62"
        "6a0a3c3c0a2f4c656e67746820343520202020202020200a3e"
        "3e0a73747265616d0a42540a2f463120392e39363236205466"
        "203732203732205464205b28612933323736372862295d544a"
        "0a45540a0a656e6473747265616d0a656e646f626a0a313020"
        "30206f626a0a3c3c0a2f54797065202f506167650a2f436f6e"
        "74656e7473203131203020520a2f5265736f75726365732039"
        "203020520a2f4d65646961426f78205b302030202d3137312e"
        "393334203135302e3931395d0a2f506172656e742035203020"
        "520a3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f46"
        "6f6e74203c3c202f4631203420302052203e3e0a2f50726f63"
        "536574205b202f504446202f54657874205d0a3e3e0a656e64"
        "6f626a0a31342030206f626a0a3c3c0a2f4c656e6774682035"
        "3920202020202020200a3e3e0a73747265616d0a42540a2f46"
        "3120392e39363236205466203732203732205464205b286129"
        "5d544a202d3332312e3436392030205464205b2862295d544a"
        "0a45540a0a656e6473747265616d0a656e646f626a0a313320"
        "30206f626a0a3c3c0a2f54797065202f506167650a2f436f6e"
        "74656e7473203134203020520a2f5265736f75726365732031"
        "32203020520a2f4d65646961426f78205b302030202d313731"
        "2e393334203135302e3931395d0a2f506172656e7420352030"
        "20520a3e3e0a656e646f626a0a31322030206f626a0a3c3c0a"
        "2f466f6e74203c3c202f4631203420302052203e3e0a2f5072"
        "6f63536574205b202f504446202f54657874205d0a3e3e0a65"
        "6e646f626a0a31352030206f626a0a5b353030203535352e36"
        "5d0a656e646f626a0a31362030206f626a0a3c3c0a2f547970"
        "65202f466f6e7444657363726970746f720a2f466f6e744e61"
        "6d65202f434d5231300a2f466c6167732033340a2f466f6e74"
        "42426f78205b30202d3139342031303030203639345d0a2f41"
        "7363656e74203639340a2f436170486569676874203638330a"
        "2f44657363656e74202d3139340a2f4974616c6963416e676c"
        "6520300a2f5374656d562039330a2f58486569676874203433"
        "310a3e3e0a656e646f626a0a342030206f626a0a3c3c0a2f54"
        "797065202f466f6e740a2f53756274797065202f5479706531"
        "0a2f42617365466f6e74202f434d5231300a2f466f6e744465"
        "7363726970746f72203136203020520a2f4669727374436861"
        "722039370a2f4c617374436861722039380a2f576964746873",
        "203135203020520a3e3e0a656e646f626a0a352030206f626a"
        "0a3c3c0a2f54797065202f50616765730a2f436f756e742034"
        "0a2f4b696473205b3220302052203720302052203130203020"
        "52203133203020525d0a3e3e0a656e646f626a0a3137203020"
        "6f626a0a3c3c0a2f54797065202f436174616c6f670a2f5061"
        "6765732035203020520a3e3e0a656e646f626a0a3138203020"
        "6f626a0a3c3c0a2f50726f647563657220287064665465582d"
        "312e34302e3235290a2f43726561746f722028546558290a2f"
        "54726170706564202f46616c73650a3e3e0a656e646f626a0a"
        "787265660a302031390a303030303030303030302036353533"
        "352066200a30303030303030323331203030303030206e200a"
        "30303030303030313139203030303030206e200a3030303030"
        "3030303135203030303030206e200a30303030303031333837"
        "203030303030206e200a303030303030313531382030303030"
        "30206e200a30303030303030353236203030303030206e200a"
        "30303030303030343134203030303030206e200a3030303030"
        "3030323938203030303030206e200a30303030303030383132"
        "203030303030206e200a303030303030303639372030303030"
        "30206e200a30303030303030353933203030303030206e200a"
        "30303030303031313133203030303030206e200a3030303030"
        "3030393937203030303030206e200a30303030303030383739"
        "203030303030206e200a303030303030313138312030303030"
        "30206e200a30303030303031323039203030303030206e200a"
        "30303030303031353935203030303030206e200a3030303030"
        "3031363435203030303030206e200a747261696c65720a3c3c"
        "202f53697a652031390a2f526f6f74203137203020520a2f49"
        "6e666f203138203020520a203e3e0a7374617274787265660a"
        "313732350a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_size_a_correction_is_worked_out_from(void)
{
    /* The correction counts thousandths of the whole scaled points the
       stated size means, not of the exact ten-thousandths of a big point
       the file prints for it: three hundred glyphs of three fonts whose
       corrections fall a glyph apart under the two. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=cmti10 at 9pt \\font\\b=cmbx10 at 10.9"
        "5pt \\font\\c=cmti10 at 20.74pt \\hsize=16000pt \\"
        "vsize=300pt \\parindent=0pt \\hbadness=10000 \\vba"
        "dness=10000\\shipout\\hbox{\\a ooooooooooooooooooo"
        "oooooooooooooooooooooooooooooooooooooooooooooooooo"
        "oooooooooooooooooooooooooooooooooooooooooooooooooo"
        "oooooooooooooooooooooooooooooooooooooooooooooooooo"
        "oooooooooooooooooooooooooooooooooooooooooooooooooo"
        "oooooooooooooooooooooooooooooooooooooooooooooooooo"
        "ooooooooooooooooooooooooooooooo}\\shipout\\hbox{\\"
        "b IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
        "II}\\shipout\\hbox{\\c ccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccc"
        "ccccccccccccccccccccccc}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313533322020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120382e3936363420546620373220"
        "3732205464205b286f293531286f293531286f293531286f29"
        "3531286f293532286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293532286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293532286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293532286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293532286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293532286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293532286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293532286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293532286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293532286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293532286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293532286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293532286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293532286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293532286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293532286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3532286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29",
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f293531286f293531286f293531286f293532286f29"
        "3531286f293531286f293531286f293531286f293531286f29"
        "3531286f295d544a0a45540a0a656e6473747265616d0a656e"
        "646f626a0a322030206f626a0a3c3c0a2f54797065202f5061"
        "67650a2f436f6e74656e74732033203020520a2f5265736f75"
        "726365732031203020520a2f4d65646961426f78205b302030"
        "20313338312e383039203134372e3836315d0a2f506172656e"
        "742035203020520a3e3e0a656e646f626a0a312030206f626a"
        "0a3c3c0a2f466f6e74203c3c202f4631203420302052203e3e"
        "0a2f50726f63536574205b202f504446202f54657874205d0a"
        "3e3e0a656e646f626a0a382030206f626a0a3c3c0a2f4c656e"
        "67746820313833322020202020200a3e3e0a73747265616d0a"
        "42540a2f46322031302e393039312054662037322037322054"
        "64205b2849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33312849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33312849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33312849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33312849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33312849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33312849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33312849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33312849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33312849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228",
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33312849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33312849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33312849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33322849292d33322849292d33"
        "322849292d33322849292d33322849292d33312849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "2d33322849292d33322849292d33322849292d33322849292d"
        "33322849292d33322849292d33312849292d33322849292d33"
        "322849292d33322849292d33322849292d33322849292d3332"
        "2849292d33322849292d33322849292d33322849292d333228"
        "49292d33322849292d33322849292d33322849292d33322849"
        "292d33322849292d33322849292d33322849292d3332284929"
        "5d544a0a45540a0a656e6473747265616d0a656e646f626a0a"
        "372030206f626a0a3c3c0a2f54797065202f506167650a2f43"
        "6f6e74656e74732038203020520a2f5265736f757263657320"
        "36203020520a2f4d65646961426f78205b3020302031363735"
        "2e343538203135312e3438355d0a2f506172656e7420352030"
        "20520a3e3e0a656e646f626a0a362030206f626a0a3c3c0a2f"
        "466f6e74203c3c202f4632203920302052203e3e0a2f50726f"
        "63536574205b202f504446202f54657874205d0a3e3e0a656e"
        "646f626a0a31322030206f626a0a3c3c0a2f4c656e67746820"
        "313533332020202020200a3e3e0a73747265616d0a42540a2f"
        "46312032302e36363235205466203732203732205464205b28"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293532286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293532286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293532286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128",
        "63293531286329353128632935312863293532286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935322863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935322863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935322863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935322863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353228632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353128"
        "63293532286329353128632935312863293531286329353128"
        "63293531286329353128632935312863293531286329353228"
        "63293531286329353128632935312863293531286329353128"
        "63293531286329353128632935322863293531286329353128"
        "632935312863293531286329353128632935312863295d544a"
        "0a45540a0a656e6473747265616d0a656e646f626a0a313120"
        "30206f626a0a3c3c0a2f54797065202f506167650a2f436f6e"
        "74656e7473203132203020520a2f5265736f75726365732031"
        "30203020520a2f4d65646961426f78205b3020302032363739"
        "2e363333203135322e3839365d0a2f506172656e7420352030"
        "20520a3e3e0a656e646f626a0a31302030206f626a0a3c3c0a"
        "2f466f6e74203c3c202f4631203420302052203e3e0a2f5072"
        "6f63536574205b202f504446202f54657874205d0a3e3e0a65"
        "6e646f626a0a31332030206f626a0a5b3433362e315d0a656e"
        "646f626a0a31342030206f626a0a3c3c0a2f54797065202f46"
        "6f6e7444657363726970746f720a2f466f6e744e616d65202f"
        "434d425831300a2f466c6167732033340a2f466f6e7442426f"
        "78205b30202d3139342031313530203639345d0a2f41736365"
        "6e74203639340a2f436170486569676874203638360a2f4465"
        "7363656e74202d3139340a2f4974616c6963416e676c652030"
        "0a2f5374656d56203130360a2f58486569676874203434340a"
        "3e3e0a656e646f626a0a31352030206f626a0a5b3436302035"
        "31312e3120343630203330362e3720343630203531312e3120"
        "3330362e37203330362e3720343630203235352e3620383137"
        "2e38203536322e32203531312e315d0a656e646f626a0a3136"
        "2030206f626a0a3c3c0a2f54797065202f466f6e7444657363"
        "726970746f720a2f466f6e744e616d65202f434d544931300a"
        "2f466c6167732033340a2f466f6e7442426f78205b30202d31",
        "39342031303232203639345d0a2f417363656e74203639340a"
        "2f436170486569676874203638330a2f44657363656e74202d"
        "3139340a2f4974616c6963416e676c6520300a2f5374656d56"
        "203130320a2f58486569676874203433310a3e3e0a656e646f"
        "626a0a392030206f626a0a3c3c0a2f54797065202f466f6e74"
        "0a2f53756274797065202f54797065310a2f42617365466f6e"
        "74202f434d425831300a2f466f6e7444657363726970746f72"
        "203134203020520a2f4669727374436861722037330a2f4c61"
        "7374436861722037330a2f576964746873203133203020520a"
        "3e3e0a656e646f626a0a342030206f626a0a3c3c0a2f547970"
        "65202f466f6e740a2f53756274797065202f54797065310a2f"
        "42617365466f6e74202f434d544931300a2f466f6e74446573"
        "63726970746f72203136203020520a2f466972737443686172"
        "2039390a2f4c61737443686172203131310a2f576964746873"
        "203135203020520a3e3e0a656e646f626a0a352030206f626a"
        "0a3c3c0a2f54797065202f50616765730a2f436f756e742033"
        "0a2f4b696473205b3220302052203720302052203131203020"
        "525d0a3e3e0a656e646f626a0a31372030206f626a0a3c3c0a"
        "2f54797065202f436174616c6f670a2f506167657320352030"
        "20520a3e3e0a656e646f626a0a31382030206f626a0a3c3c0a"
        "2f50726f647563657220287064665465582d312e34302e3235"
        "290a2f43726561746f722028546558290a2f54726170706564"
        "202f46616c73650a3e3e0a656e646f626a0a787265660a3020"
        "31390a303030303030303030302036353533352066200a3030"
        "3030303031373138203030303030206e200a30303030303031"
        "363035203030303030206e200a303030303030303031352030"
        "30303030206e200a3030303030303632333520303030303020"
        "6e200a30303030303036333638203030303030206e200a3030"
        "3030303033373838203030303030206e200a30303030303033"
        "363735203030303030206e200a303030303030313738352030"
        "30303030206e200a3030303030303631303320303030303020"
        "6e200a30303030303035353633203030303030206e200a3030"
        "3030303035343437203030303030206e200a30303030303033"
        "383535203030303030206e200a303030303030353633312030"
        "30303030206e200a3030303030303536353520303030303020"
        "6e200a30303030303035383335203030303030206e200a3030"
        "3030303035393233203030303030206e200a30303030303036"
        "343338203030303030206e200a303030303030363438382030"
        "30303030206e200a747261696c65720a3c3c202f53697a6520"
        "31390a2f526f6f74203137203020520a2f496e666f20313820"
        "3020520a203e3e0a7374617274787265660a363536380a2525"
        "454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_place_after_a_step(void)
{
    /* The place the file's text stands at after a step is that step's
       own worth of scaled points added to where it stood, taken towards
       the engine's own place -- not the whole printed place taken
       towards it, which loses the fractions the steps carry. */
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=cmr10 \\a \\pdfpagewidth=200pt \\pdfpa"
        "geheight=1000pt \\baselineskip=0pt \\lineskip=0pt "
        "\\lineskiplimit=-16000pt \\def\\l#1{\\kern#1sp\\hb"
        "ox{a}}\\shipout\\vbox{\\kern0sp\\hbox{a}\\l{891290"
        "}\\l{891290}\\l{891290}\\l{891290}\\l{891290}\\l{8"
        "91290}\\l{891290}}\\shipout\\vbox{\\kern44sp\\hbox"
        "{a}\\l{891290}\\l{891290}}\\shipout\\vbox{\\kern37"
        "sp\\hbox{a}\\l{891290}\\l{891291}}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313837202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3931392e393735205464205b2861295d544a2030202d31332e"
        "3535205464205b2861295d544a2030202d31332e3534392054"
        "64205b2861295d544a2030202d31332e353439205464205b28"
        "61295d544a2030202d31332e353439205464205b2861295d54"
        "4a2030202d31332e353439205464205b2861295d544a203020"
        "2d31332e3535205464205b2861295d544a2030202d31332e35"
        "3439205464205b2861295d544a0a45540a0a656e6473747265"
        "616d0a656e646f626a0a322030206f626a0a3c3c0a2f547970"
        "65202f506167650a2f436f6e74656e74732033203020520a2f"
        "5265736f75726365732031203020520a2f4d65646961426f78"
        "205b302030203139392e323533203939362e3236345d0a2f50"
        "6172656e742035203020520a3e3e0a656e646f626a0a312030"
        "206f626a0a3c3c0a2f466f6e74203c3c202f46312034203020"
        "52203e3e0a2f50726f63536574205b202f504446202f546578"
        "74205d0a3e3e0a656e646f626a0a382030206f626a0a3c3c0a"
        "2f4c656e67746820383420202020202020200a3e3e0a737472"
        "65616d0a42540a2f463120392e393632362054662037322039"
        "31392e393734205464205b2861295d544a2030202d31332e35"
        "3439205464205b2861295d544a2030202d31332e3534392054"
        "64205b2861295d544a0a45540a0a656e6473747265616d0a65"
        "6e646f626a0a372030206f626a0a3c3c0a2f54797065202f50"
        "6167650a2f436f6e74656e74732038203020520a2f5265736f"
        "75726365732036203020520a2f4d65646961426f78205b3020"
        "30203139392e323533203939362e3236345d0a2f506172656e"
        "742035203020520a3e3e0a656e646f626a0a362030206f626a"
        "0a3c3c0a2f466f6e74203c3c202f4631203420302052203e3e"
        "0a2f50726f63536574205b202f504446202f54657874205d0a"
        "3e3e0a656e646f626a0a31312030206f626a0a3c3c0a2f4c65"
        "6e67746820383420202020202020200a3e3e0a73747265616d"
        "0a42540a2f463120392e39363236205466203732203931392e"
        "393734205464205b2861295d544a2030202d31332e35343920"
        "5464205b2861295d544a2030202d31332e353439205464205b"
        "2861295d544a0a45540a0a656e6473747265616d0a656e646f"
        "626a0a31302030206f626a0a3c3c0a2f54797065202f506167"
        "650a2f436f6e74656e7473203131203020520a2f5265736f75"
        "726365732039203020520a2f4d65646961426f78205b302030"
        "203139392e323533203939362e3236345d0a2f506172656e74"
        "2035203020520a3e3e0a656e646f626a0a392030206f626a0a"
        "3c3c0a2f466f6e74203c3c202f4631203420302052203e3e0a"
        "2f50726f63536574205b202f504446202f54657874205d0a3e"
        "3e0a656e646f626a0a31322030206f626a0a5b3530305d0a65"
        "6e646f626a0a31332030206f626a0a3c3c0a2f54797065202f"
        "466f6e7444657363726970746f720a2f466f6e744e616d6520"
        "2f434d5231300a2f466c6167732033340a2f466f6e7442426f"
        "78205b30202d3139342031303030203639345d0a2f41736365"
        "6e74203639340a2f436170486569676874203638330a2f4465"
        "7363656e74202d3139340a2f4974616c6963416e676c652030"
        "0a2f5374656d562039330a2f58486569676874203433310a3e"
        "3e0a656e646f626a0a342030206f626a0a3c3c0a2f54797065"
        "202f466f6e740a2f53756274797065202f54797065310a2f42"
        "617365466f6e74202f434d5231300a2f466f6e744465736372"
        "6970746f72203133203020520a2f4669727374436861722039"
        "370a2f4c617374436861722039370a2f576964746873203132"
        "203020520a3e3e0a656e646f626a0a352030206f626a0a3c3c"
        "0a2f54797065202f50616765730a2f436f756e7420330a2f4b"
        "696473205b3220302052203720302052203130203020525d0a"
        "3e3e0a656e646f626a0a31342030206f626a0a3c3c0a2f5479",
        "7065202f436174616c6f670a2f50616765732035203020520a"
        "3e3e0a656e646f626a0a31352030206f626a0a3c3c0a2f5072"
        "6f647563657220287064665465582d312e34302e3235290a2f"
        "43726561746f722028546558290a2f54726170706564202f46"
        "616c73650a3e3e0a656e646f626a0a787265660a302031360a"
        "303030303030303030302036353533352066200a3030303030"
        "3030333732203030303030206e200a30303030303030323630"
        "203030303030206e200a303030303030303031352030303030"
        "30206e200a30303030303031323834203030303030206e200a"
        "30303030303031343135203030303030206e200a3030303030"
        "3030363933203030303030206e200a30303030303030353831"
        "203030303030206e200a303030303030303433392030303030"
        "30206e200a30303030303031303137203030303030206e200a"
        "30303030303030393033203030303030206e200a3030303030"
        "3030373630203030303030206e200a30303030303031303834"
        "203030303030206e200a303030303030313130362030303030"
        "30206e200a30303030303031343835203030303030206e200a"
        "30303030303031353335203030303030206e200a747261696c"
        "65720a3c3c202f53697a652031360a2f526f6f742031342030"
        "20520a2f496e666f203135203020520a203e3e0a7374617274"
        "787265660a313631350a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_the_place_a_file_names(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\a=cmr10 \\a \\hsize=16000pt \\vsize=300p"
        "t \\parindent=0pt \\hbadness=10000 \\vbadness=1000"
        "0 \\shipout\\hbox{\\kern 7sp mmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmm}\\shipout\\hbox{\\kern 2"
        "9sp mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "}\\shipout\\hbox{\\kern 53sp mmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm"
        "mmmmmmmmmmmmmmmmmmmmmmmmm}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820313438202020202020200a3e3e0a7374"
        "7265616d0a42540a2f463120392e3936323620546620373220"
        "3732205464205b286d6d6d6d6d6d6d6d6d6d6d6d6d292d3128"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d295d544a0a"
        "45540a0a656e6473747265616d0a656e646f626a0a32203020"
        "6f626a0a3c3c0a2f54797065202f506167650a2f436f6e7465"
        "6e74732033203020520a2f5265736f75726365732031203020"
        "520a2f4d65646961426f78205b302030203934312e30313420"
        "3134382e3238395d0a2f506172656e742035203020520a3e3e"
        "0a656e646f626a0a312030206f626a0a3c3c0a2f466f6e7420"
        "3c3c202f4631203420302052203e3e0a2f50726f6353657420"
        "5b202f504446202f54657874205d0a3e3e0a656e646f626a0a"
        "382030206f626a0a3c3c0a2f4c656e67746820313438202020"
        "202020200a3e3e0a73747265616d0a42540a2f463120392e39"
        "363236205466203732203732205464205b286d6d6d6d6d6d6d"
        "6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d292d3128"
        "6d6d6d6d6d295d544a0a45540a0a656e6473747265616d0a65"
        "6e646f626a0a372030206f626a0a3c3c0a2f54797065202f50"
        "6167650a2f436f6e74656e74732038203020520a2f5265736f"
        "75726365732036203020520a2f4d65646961426f78205b3020"
        "30203934312e303134203134382e3238395d0a2f506172656e"
        "742035203020520a3e3e0a656e646f626a0a362030206f626a"
        "0a3c3c0a2f466f6e74203c3c202f4631203420302052203e3e"
        "0a2f50726f63536574205b202f504446202f54657874205d0a"
        "3e3e0a656e646f626a0a31312030206f626a0a3c3c0a2f4c65"
        "6e67746820313532202020202020200a3e3e0a73747265616d"
        "0a42540a2f463120392e393632362054662037322e30303120"
        "3732205464205b286d6d6d6d6d6d6d6d6d6d6d6d6d6d292d31"
        "286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d292d31286d6d6d6d6d6d6d6d6d6d6d6d6d6d"
        "6d6d6d6d6d6d6d6d6d6d6d6d6d292d31286d6d6d295d544a0a"
        "45540a0a656e6473747265616d0a656e646f626a0a31302030"
        "206f626a0a3c3c0a2f54797065202f506167650a2f436f6e74"
        "656e7473203131203020520a2f5265736f7572636573203920"
        "3020520a2f4d65646961426f78205b302030203934312e3031"
        "34203134382e3238395d0a2f506172656e742035203020520a"
        "3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f466f6e"
        "74203c3c202f4631203420302052203e3e0a2f50726f635365"
        "74205b202f504446202f54657874205d0a3e3e0a656e646f62"
        "6a0a31322030206f626a0a5b3833332e335d0a656e646f626a"
        "0a31332030206f626a0a3c3c0a2f54797065202f466f6e7444"
        "657363726970746f720a2f466f6e744e616d65202f434d5231"
        "300a2f466c6167732033340a2f466f6e7442426f78205b3020"
        "2d3139342031303030203639345d0a2f417363656e74203639"
        "340a2f436170486569676874203638330a2f44657363656e74"
        "202d3139340a2f4974616c6963416e676c6520300a2f537465"
        "6d562039330a2f58486569676874203433310a3e3e0a656e64"
        "6f626a0a342030206f626a0a3c3c0a2f54797065202f466f6e"
        "740a2f53756274797065202f54797065310a2f42617365466f"
        "6e74202f434d5231300a2f466f6e7444657363726970746f72"
        "203133203020520a2f466972737443686172203130390a2f4c"
        "61737443686172203130390a2f576964746873203132203020",
        "520a3e3e0a656e646f626a0a352030206f626a0a3c3c0a2f54"
        "797065202f50616765730a2f436f756e7420330a2f4b696473"
        "205b3220302052203720302052203130203020525d0a3e3e0a"
        "656e646f626a0a31342030206f626a0a3c3c0a2f5479706520"
        "2f436174616c6f670a2f50616765732035203020520a3e3e0a"
        "656e646f626a0a31352030206f626a0a3c3c0a2f50726f6475"
        "63657220287064665465582d312e34302e3235290a2f437265"
        "61746f722028546558290a2f54726170706564202f46616c73"
        "650a3e3e0a656e646f626a0a787265660a302031360a303030"
        "303030303030302036353533352066200a3030303030303033"
        "3333203030303030206e200a30303030303030323231203030"
        "303030206e200a30303030303030303135203030303030206e"
        "200a30303030303031333739203030303030206e200a303030"
        "30303031353132203030303030206e200a3030303030303037"
        "3138203030303030206e200a30303030303030363036203030"
        "303030206e200a30303030303030343030203030303030206e"
        "200a30303030303031313130203030303030206e200a303030"
        "30303030393936203030303030206e200a3030303030303037"
        "3835203030303030206e200a30303030303031313737203030"
        "303030206e200a30303030303031323031203030303030206e"
        "200a30303030303031353832203030303030206e200a303030"
        "30303031363332203030303030206e200a747261696c65720a"
        "3c3c202f53697a652031360a2f526f6f74203134203020520a"
        "2f496e666f203135203020520a203e3e0a7374617274787265"
        "660a313731320a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_colour_on_a_page(void)
{
    static const char *const source[] = {
        "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 \\"
        "catcode`\\^=7 \\catcode`\\_=8 \\nonstopmode\\pdfou"
        "tput=1 \\pdfcompresslevel=0 \\pdfobjcompresslevel="
        "0 \\font\\tenrm=cmr10 \\tenrm \\hsize=200pt \\vsiz"
        "e=300pt \\parindent=0pt \\pdfcolorstackinit page{F"
        "AINT}\\shipout\\hbox{a\\pdfcolorstack0 push{1 0 0 "
        "rg}b\\pdfcolorstack0 pop c\\pdfcolorstack1 push{DE"
        "EP}d}\\shipout\\hbox{a\\pdfliteral{SET}b\\pdfliter"
        "al direct{DIR}c\\pdfliteral page{PAGE}d}\\shipout"
        "\\hbox{a}\n\\end\n",
        NULL,
    };
    static const char *const expected[] = {
        "255044462d312e340a25d0d4c5d80a332030206f626a0a3c3c"
        "0a2f4c656e67746820323038202020202020200a3e3e0a7374"
        "7265616d0a3120302030203120373220373220636d0a464149"
        "4e540a31203020302031202d3732202d373220636d0a42540a"
        "2f463120392e39363236205466203732203732205464205b28"
        "61295d544a0a31203020302072670a205b2862295d544a0a30"
        "2067203020470a205b2d3333332863295d544a0a45540a3120"
        "30203020312039302e32363520373220636d0a444545500a31"
        "203020302031202d39302e323635202d373220636d0a42540a"
        "2f463120392e393632362054662039302e3236352037322054"
        "64205b2864295d544a0a45540a0a656e6473747265616d0a65"
        "6e646f626a0a322030206f626a0a3c3c0a2f54797065202f50"
        "6167650a2f436f6e74656e74732033203020520a2f5265736f"
        "75726365732031203020520a2f4d65646961426f78205b3020"
        "30203136372e38203135302e3931395d0a2f506172656e7420"
        "35203020520a3e3e0a656e646f626a0a312030206f626a0a3c"
        "3c0a2f466f6e74203c3c202f4631203420302052203e3e0a2f"
        "50726f63536574205b202f504446202f54657874205d0a3e3e"
        "0a656e646f626a0a382030206f626a0a3c3c0a2f4c656e6774"
        "6820323236202020202020200a3e3e0a73747265616d0a3120"
        "302030203120373220373220636d0a444545500a3120302030"
        "2031202d3732202d373220636d0a42540a2f463120392e3936"
        "3236205466203732203732205464205b2861295d544a0a4554"
        "0a312030203020312037362e39383120373220636d0a534554"
        "0a31203020302031202d37362e393831202d373220636d0a42"
        "540a2f463120392e393632362054662037362e393831203732"
        "205464205b2862295d544a0a4449520a205b2863295d544a0a"
        "45540a504147450a42540a2f463120392e3936323620546620"
        "38362e393434203732205464205b2864295d544a0a45540a0a"
        "656e6473747265616d0a656e646f626a0a372030206f626a0a"
        "3c3c0a2f54797065202f506167650a2f436f6e74656e747320"
        "38203020520a2f5265736f75726365732036203020520a2f4d"
        "65646961426f78205b302030203136342e343739203135302e"
        "3931395d0a2f506172656e742035203020520a3e3e0a656e64"
        "6f626a0a362030206f626a0a3c3c0a2f466f6e74203c3c202f"
        "4631203420302052203e3e0a2f50726f63536574205b202f50"
        "4446202f54657874205d0a3e3e0a656e646f626a0a31312030"
        "206f626a0a3c3c0a2f4c656e67746820373820202020202020"
        "200a3e3e0a73747265616d0a31203020302031203732203732"
        "20636d0a444545500a31203020302031202d3732202d373220"
        "636d0a42540a2f463120392e39363236205466203732203732"
        "205464205b2861295d544a0a45540a0a656e6473747265616d"
        "0a656e646f626a0a31302030206f626a0a3c3c0a2f54797065"
        "202f506167650a2f436f6e74656e7473203131203020520a2f"
        "5265736f75726365732039203020520a2f4d65646961426f78"
        "205b302030203134382e393831203134382e3238395d0a2f50"
        "6172656e742035203020520a3e3e0a656e646f626a0a392030"
        "206f626a0a3c3c0a2f466f6e74203c3c202f46312034203020"
        "52203e3e0a2f50726f63536574205b202f504446202f546578"
        "74205d0a3e3e0a656e646f626a0a31342030206f626a0a3c3c"
        "0a2f4c656e67746820383320202020202020200a3e3e0a7374"
        "7265616d0a3120302030203120373220373220636d0a444545"
        "500a31203020302031202d3732202d373220636d0a42540a2f"
        "463120392e39363236205466203732203336342e3435392054"
        "64205b2831295d544a0a45540a0a656e6473747265616d0a65"
        "6e646f626a0a31332030206f626a0a3c3c0a2f54797065202f"
        "506167650a2f436f6e74656e7473203134203020520a2f5265"
        "736f7572636573203132203020520a2f4d65646961426f7820"
        "5b302030203334332e323533203434322e3837395d0a2f5061"
        "72656e742035203020520a3e3e0a656e646f626a0a31322030",
        "206f626a0a3c3c0a2f466f6e74203c3c202f46312034203020"
        "52203e3e0a2f50726f63536574205b202f504446202f546578"
        "74205d0a3e3e0a656e646f626a0a31352030206f626a0a5b35"
        "30302035303020353030203530302035303020353030203530"
        "302035303020353030203237372e38203237372e3820323737"
        "2e38203737372e38203437322e32203437322e32203737372e"
        "3820373530203730382e33203732322e32203736332e392036"
        "38302e36203635322e38203738342e3720373530203336312e"
        "31203531332e39203737372e3820363235203931362e372037"
        "3530203737372e38203638302e36203737372e38203733362e"
        "31203535352e36203732322e32203735302037353020313032"
        "372e382037353020373530203631312e31203237372e382035"
        "3030203237372e3820353030203237372e38203237372e3820"
        "353030203535352e36203434342e34203535352e365d0a656e"
        "646f626a0a31362030206f626a0a3c3c0a2f54797065202f46"
        "6f6e7444657363726970746f720a2f466f6e744e616d65202f"
        "434d5231300a2f466c6167732033340a2f466f6e7442426f78"
        "205b30202d3139342031303030203639345d0a2f417363656e"
        "74203639340a2f436170486569676874203638330a2f446573"
        "63656e74202d3139340a2f4974616c6963416e676c6520300a"
        "2f5374656d562039330a2f58486569676874203433310a3e3e"
        "0a656e646f626a0a342030206f626a0a3c3c0a2f5479706520"
        "2f466f6e740a2f53756274797065202f54797065310a2f4261"
        "7365466f6e74202f434d5231300a2f466f6e74446573637269"
        "70746f72203136203020520a2f466972737443686172203439"
        "0a2f4c61737443686172203130300a2f576964746873203135"
        "203020520a3e3e0a656e646f626a0a352030206f626a0a3c3c"
        "0a2f54797065202f50616765730a2f436f756e7420340a2f4b"
        "696473205b3220302052203720302052203130203020522031"
        "33203020525d0a3e3e0a656e646f626a0a31372030206f626a"
        "0a3c3c0a2f54797065202f436174616c6f670a2f5061676573"
        "2035203020520a3e3e0a656e646f626a0a31382030206f626a"
        "0a3c3c0a2f50726f647563657220287064665465582d312e34"
        "302e3235290a2f43726561746f722028546558290a2f547261"
        "70706564202f46616c73650a3e3e0a656e646f626a0a787265"
        "660a302031390a303030303030303030302036353533352066"
        "200a30303030303030333931203030303030206e200a303030"
        "30303030323831203030303030206e200a3030303030303030"
        "3135203030303030206e200a30303030303032303333203030"
        "303030206e200a30303030303032313635203030303030206e"
        "200a30303030303030383534203030303030206e200a303030"
        "30303030373432203030303030206e200a3030303030303034"
        "3538203030303030206e200a30303030303031313732203030"
        "303030206e200a30303030303031303538203030303030206e"
        "200a30303030303030393231203030303030206e200a303030"
        "30303031343936203030303030206e200a3030303030303133"
        "3831203030303030206e200a30303030303031323339203030"
        "303030206e200a30303030303031353634203030303030206e"
        "200a30303030303031383535203030303030206e200a303030"
        "30303032323432203030303030206e200a3030303030303232"
        "3932203030303030206e200a747261696c65720a3c3c202f53"
        "697a652031390a2f526f6f74203137203020520a2f496e666f"
        "203138203020520a203e3e0a7374617274787265660a323337"
        "320a2525454f460a",
        NULL,
    };
    return run_document_pdf(source, expected);
}

static int test_what_a_split_leaves_behind(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=3 \\show"
        "boxbreadth=100 \\splittopskip=4pt \\splitmaxdepth="
        "2pt \\baselineskip=0pt \\lineskip=0pt \\vbadness=1"
        "0000 \\vfuzz=1000pt \\message{[top]}\\setbox0=\\vb"
        "ox{\\hrule height10pt \\vskip3pt \\hrule height10p"
        "t \\vskip3pt \\hrule height10pt \\vskip3pt \\hrule"
        " height10pt}\\setbox1=\\vsplit0 to 20pt \\showbox1"
        " \\message{[rest]}\\showbox0 \\message{[pen]}\\set"
        "box0=\\vbox{\\hrule height10pt \\penalty-200 \\vsk"
        "ip0pt \\hrule height10pt \\vskip0pt \\hrule height"
        "10pt}\\setbox1=\\vsplit0 to 21pt \\showbox1 \\mess"
        "age{[nobreak]}\\setbox0=\\vbox{\\hrule height10pt "
        "\\hrule height10pt}\\setbox1=\\vsplit0 to 5pt \\sh"
        "owbox1 \\message{[gone]}\\showbox0 \\message{[keep"
        "]}\\setbox0=\\vbox{\\hrule height10pt \\vskip3pt "
        "\\mark{m}\\special{s}\\vskip2pt \\penalty5 \\hrule"
        " height4pt \\vskip1pt \\hrule height10pt}\\setbox1"
        "=\\vsplit0 to 10pt \\showbox0 \\message{[marks]}\\"
        "setbox0=\\vbox{\\mark{one}\\hbox{a}\\mark{two}\\hb"
        "ox{b}\\mark{three}\\hbox{c}\\mark{four}}\\setbox1="
        "\\vsplit0 to 12pt \\setbox2=\\hbox{[\\splitfirstma"
        "rk][\\splitbotmark]}\\showbox2 \\message{[none]}\\"
        "setbox0=\\vbox{\\hbox{a}\\vskip0pt \\hbox{b}}\\set"
        "box1=\\vsplit0 to 5pt \\setbox2=\\hbox{[\\splitfir"
        "stmark][\\splitbotmark]}\\showbox2 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[top]\n> \\box1=\n\\vbox(20.0+0.0)x0.0\n.\\r"
        "ule(10.0+0.0)x*\n\n! OK.\nl.1 ...ht10pt}\\setbox1="
        "\\vsplit0 to 20pt \\showbox1 \n                   "
        "                               \\message{[rest]}\\"
        "showbox0 ...\n\n\n[rest]\n> \\box0=\n\\vbox(36.0+0"
        ".0)x0.0\n.\\glue(\\splittopskip) 0.0\n.\\rule(10.0"
        "+0.0)x*\n.\\glue 3.0\n.\\rule(10.0+0.0)x*\n.\\glue"
        " 3.0\n.\\rule(10.0+0.0)x*\n\n! OK.\nl.1 ...o 20pt "
        "\\showbox1 \\message{[rest]}\\showbox0 \n         "
        "                                         \\message"
        "{[pen]}\\setbox0=\\v...\n\n\n[pen]\n> \\box1=\n\\v"
        "box(21.0+0.0)x0.0\n.\\rule(10.0+0.0)x*\n.\\penalty"
        " -200\n.\\glue 0.0\n.\\rule(10.0+0.0)x*\n\n! OK.\n"
        "l.1 ...ht10pt}\\setbox1=\\vsplit0 to 21pt \\showbo"
        "x1 \n                                             "
        "     \\message{[nobreak]}\\setbox...\n\n\n[nobreak"
        "]\n> \\box1=\n\\vbox(5.0+0.0)x0.0\n.\\rule(10.0+0."
        "0)x*\n.\\rule(10.0+0.0)x*\n\n! OK.\nl.1 ...ght10pt"
        "}\\setbox1=\\vsplit0 to 5pt \\showbox1 \n         "
        "                                         \\message"
        "{[gone]}\\showbox0 ...\n\n\n[gone]\n> \\box0=void"
        "\n\n! OK.\nl.1 ...to 5pt \\showbox1 \\message{[gon"
        "e]}\\showbox0 \n                                  "
        "                \\message{[keep]}\\setbox0=\\...\n"
        "\n\n[keep]\n> \\box0=\n\\vbox(15.0+0.0)x0.0\n.\\ma"
        "rk{m}\n.\\special{s}\n.\\glue(\\splittopskip) 0.0"
        "\n.\\rule(4.0+0.0)x*\n.\\glue 1.0\n.\\rule(10.0+0."
        "0)x*\n\n! OK.\nl.1 ...ht10pt}\\setbox1=\\vsplit0 t"
        "o 10pt \\showbox0 \n                              "
        "                    \\message{[marks]}\\setbox0=.."
        ".\n\n\n[marks]\n> \\box2=\n\\hbox(7.5+2.5)x48.3612"
        "\n.\\tenrm [\n.\\tenrm o\n.\\tenrm n\n.\\tenrm e\n"
        ".\\tenrm ]\n.\\tenrm [\n.\\tenrm t\n.\\tenrm h\n."
        "\\tenrm r\n.\\tenrm e\n.\\tenrm e\n.\\tenrm ]\n\n!"
        " OK.\nl.1 ...[\\splitfirstmark][\\splitbotmark]}\\"
        "showbox2 \n                                       "
        "           \\message{[none]}\\setbox0=\\...\n\n\n["
        "none]\n> \\box2=\n\\hbox(7.5+2.5)x11.11115\n.\\ten"
        "rm [\n.\\tenrm ]\n.\\tenrm [\n.\\tenrm ]\n\n! OK."
        "\nl.1 ...[\\splitfirstmark][\\splitbotmark]}\\show"
        "box2 \n                                           "
        "       \\showbox254\n\n> \\box254=void\n\n! OK.\nl"
        ".1 ...tmark][\\splitbotmark]}\\showbox2 \\showbox2"
        "54\n                                              "
        "    \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A definition that nothing holds any more is taken apart and its record
   used again, so a redefinition costs nothing in the long run. What is
   held is what a control sequence has now and what the save stack keeps
   for a group to end. See docs/DECISIONS.md, a-definition-nothing-holds. */
static int test_a_definition_nothing_holds(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=2 \\show"
        "boxbreadth=20 \\def\\x{A}\\let\\y\\x \\def\\x{B}\\"
        "message{[one \\meaning\\x/\\meaning\\y]}{\\def\\x{"
        "C}\\message{[two \\meaning\\x/\\meaning\\y]}\\glob"
        "al\\def\\y{D}\\message{[three \\meaning\\x/\\meani"
        "ng\\y]}}\\message{[four \\meaning\\x/\\meaning\\y]"
        "}\\def\\z{E}\\let\\z\\z \\message{[five \\meaning"
        "\\z]}{\\let\\x\\x \\message{[six \\meaning\\x]}}\\"
        "message{[seven \\meaning\\x]}\\setbox0=\\hbox{\\x"
        "\\y}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[one macro:->B/macro:->A] [two macro:->C/mac"
        "ro:->A]\n[three macro:->C/macro:->D] [four macro:-"
        ">B/macro:->D] [five macro:->E]\n[six macro:->B] [s"
        "even macro:->B]\n> \\box0=\n\\hbox(6.83331+0.0)x14"
        ".72226\n.\\tenrm B\n.\\tenrm D\n\n! OK.\nl.1 ... "
        "\\meaning\\x]}\\setbox0=\\hbox{\\x\\y}\\showbox0 "
        "\n                                                "
        "  \\showbox254\n\n> \\box254=void\n\n! OK.\nl.1 .."
        ".]}\\setbox0=\\hbox{\\x\\y}\\showbox0 \\showbox254"
        "\n                                                "
        "  \n\n",
        NULL,
    };
    return run_document_parts(source, expected);;
}

/* The page description the reference writes when \pdfoutput is not
   positive, byte for byte: the places, the fonts, the rules, the glue it
   had to set, the movements it writes as repeats, and what it says of
   itself at both ends. See docs/DECISIONS.md, the-page-description. */
static int test_the_page_description(void)
{
    static const char *const source[] = {
        "\\pdfoutput=0 \\year=2026 \\month=8 \\day=18 \\tim"
        "e=1117 \\font\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\"
        "tenrm \\hsize=100pt \\vsize=200pt \\parindent=0pt "
        "\\baselineskip=12pt \\lineskip=0pt \\hbadness=1000"
        "0 \\vbadness=10000 \\hfuzz=1000pt \\shipout\\vbox{"
        "\\hbox{ab}\\hbox{cd}\\hbox{ef}\\hbox{\\sevenrm g\\"
        "tenrm h}}\\shipout\\vbox{\\hrule height2pt \\hbox "
        "to 80pt{a\\hfil b}\\vskip 3pt \\hbox{\\vrule width"
        "3pt height4pt depth1pt x}\\hrule}\\hoffset=13pt \\"
        "voffset=7pt \\count0=3 \\count1=4 \\shipout\\vbox{"
        "\\hbox{ab}\\vskip3pt \\hbox{cd}\\vskip3pt \\hbox{e"
        "f}}\\hoffset=0pt \\voffset=0pt \\count1=0 \\shipou"
        "t\\hbox to 300pt{a\\hskip 3.65pt plus 1.82501pt a"
        "\\hskip 3.65pt plus 1.82501pt a\\hskip 3.65pt plus"
        " 1.82501pt a\\hskip 3.65pt plus 1.82501pt a\\hskip"
        " 3.65pt plus 1.82501pt a\\hskip 3.65pt plus 1.8250"
        "1pt a\\hskip 3.65pt plus 1.82501pt a\\hskip 3.65pt"
        " plus 1.82501pt a}\\shipout\\vbox{\\vbox to 12pt{"
        "\\vfil\\hbox to 40pt{}}\\vskip 25pt \\hbox{ab}\\sp"
        "ecial{a special}}",
        NULL,
    };
    static const char *const expected[] = {
        "f702018392c01c3b0000000003e81b20546558206f75747075"
        "7420323032362e30382e31383a313833378b00000000000000"
        "00000000000000000000000000000000000000000000000000"
        "0000000000000000ffffffff9f06f1c78df3004bf16079000a"
        "0000000a00000005636d723130ab61628ea40c00008d63648e"
        "a18d65668ea18df301d993a05200070000000700000004636d"
        "7237ac67ab688e8c8b00000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000"
        "00002a9f0200008900020000005000009f06f1c78dab619145"
        "71c5628e9f0f00008d9f0100008400050000000300009fff00"
        "00788e9f0166668900006666005000008c8b00000003000000"
        "04000000000000000000000000000000000000000000000000"
        "00000000000000000000009e9f0df1c78d910d0000ab61628e"
        "a40f00008d910d000063648ea18d910d000065668e8c8b0000"
        "00030000000000000000000000000000000000000000000000"
        "000000000000000000000000000000010b9f044e38ab61961f"
        "dfff61936193619361911fdffe619361936193618c8b000000"
        "03000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000015b9f0c00009f2500008d"
        "ab61628eef0961207370656369616c8cf8000001a5018392c0"
        "1c3b0000000003e800310000012c000000010005f301d993a0"
        "5200070000000700000004636d7237f3004bf16079000a0000"
        "000a00000005636d723130f9000001eb02dfdfdfdfdf",
        NULL,
    };
    return run_document_dvi(source, expected);
}

/* An \insert holds a vertical list packed at its natural size, and
   remembers the \splittopskip, \splitmaxdepth and \floatingpenalty of
   where it was written. It takes up no room in the list it stands in, and
   moves out of a paragraph line as a mark does. See docs/DECISIONS.md,
   insertions. */
static int test_an_insertion_in_a_list(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=4 \\show"
        "boxbreadth=100 \\dimen100=10pt \\count100=500 \\sk"
        "ip100=3pt plus1pt \\splittopskip=5pt \\splitmaxdep"
        "th=2pt \\floatingpenalty=77 \\baselineskip=12pt \\"
        "message{[a]}\\setbox0=\\vbox{\\hrule\\insert100{\\"
        "hbox{a}\\hbox{b}}}\\showbox0 \\message{[b]}\\setbo"
        "x0=\\vbox{\\hrule{\\splittopskip=1pt \\splitmaxdep"
        "th=3pt \\floatingpenalty=5 \\insert200{\\hbox{a}}}"
        "}\\showbox0 \\message{[h]}\\setbox0=\\vbox{\\noind"
        "ent aa\\insert100{\\hbox{b}}bb\\par}\\showbox0 \\m"
        "essage{[t]}\\setbox0=\\vbox{\\hrule\\insert100{\\h"
        "box{a}}\\xdef\\t{\\the\\lastnodetype}}\\showbox0 "
        "\\message{[type]}\\setbox0=\\hbox{\\t}\\showbox0 "
        "\\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[a]\n> \\box0=\n\\vbox(0.4+0.0)x0.0\n.\\rule"
        "(0.4+0.0)x*\n.\\insert100, natural size 16.30554; "
        "split(5.0,2.0); float cost 77\n..\\hbox(4.30554+0."
        "0)x5.00002\n...\\tenrm a\n..\\glue(\\baselineskip)"
        " 5.05556\n..\\hbox(6.94444+0.0)x5.55557\n...\\tenr"
        "m b\n\n! OK.\nl.1 ...rule\\insert100{\\hbox{a}\\hb"
        "ox{b}}}\\showbox0 \n                              "
        "                    \\message{[b]}\\setbox0=\\vbo."
        "..\n\n\n[b]\n> \\box0=\n\\vbox(0.4+0.0)x0.0\n.\\ru"
        "le(0.4+0.0)x*\n.\\insert200, natural size 4.30554;"
        " split(1.0,3.0); float cost 5\n..\\hbox(4.30554+0."
        "0)x5.00002\n...\\tenrm a\n\n! OK.\nl.1 ...gpenalty"
        "=5 \\insert200{\\hbox{a}}}}\\showbox0 \n          "
        "                                        \\message{"
        "[h]}\\setbox0=\\vbo...\n\n\n[h]\n> \\box0=\n\\vbox"
        "(6.94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0, g"
        "lue set 178.88882fil\n..\\tenrm a\n..\\tenrm a\n.."
        "\\tenrm b\n..\\tenrm b\n..\\penalty 10000\n..\\glu"
        "e(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\right"
        "skip) 0.0\n.\\insert100, natural size 6.94444; spl"
        "it(5.0,2.0); float cost 77\n..\\hbox(6.94444+0.0)x"
        "5.55557\n...\\tenrm b\n\n! OK.\nl.1 ...ent aa\\ins"
        "ert100{\\hbox{b}}bb\\par}\\showbox0 \n            "
        "                                      \\message{[t"
        "]}\\setbox0=\\vbo...\n\n\n[t]\n> \\box0=\n\\vbox(0"
        ".4+0.0)x0.0\n.\\rule(0.4+0.0)x*\n.\\insert100, nat"
        "ural size 4.30554; split(5.0,2.0); float cost 77\n"
        "..\\hbox(4.30554+0.0)x5.00002\n...\\tenrm a\n\n! O"
        "K.\nl.1 ...ox{a}}\\xdef\\t{\\the\\lastnodetype}}\\"
        "showbox0 \n                                       "
        "           \\message{[type]}\\setbox0=\\...\n\n\n["
        "type]\n> \\box0=\n\\hbox(6.44444+0.0)x5.00002\n.\\"
        "tenrm 4\n\n! OK.\nl.1 ...message{[type]}\\setbox0="
        "\\hbox{\\t}\\showbox0 \n                          "
        "                        \\showbox254\n\n> \\box254"
        "=void\n\n! OK.\nl.1 ...pe]}\\setbox0=\\hbox{\\t}\\"
        "showbox0 \\showbox254\n                           "
        "                       \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* An insertion that does not fit is split: what fits goes into the class's
   box, the rest waits for the next page with \splittopskip in front of it,
   and nothing more of that class stays on the page. \insertpenalties counts
   what waits. See docs/DECISIONS.md, an-insertion-that-does-not-fit. */
static int test_an_insertion_that_does_not_fit(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=3 \\show"
        "boxbreadth=100 \\baselineskip=12pt \\vsize=50pt \\"
        "maxdepth=2pt \\topskip=0pt \\count0=1 \\hsize=100p"
        "t \\splitmaxdepth=2pt \\splittopskip=1pt \\dimen10"
        "0=8pt \\count100=1000 \\skip100=0pt \\floatingpena"
        "lty=77 \\output={\\message{[page]}\\setbox1=\\hbox"
        "{\\the\\pagegoal|\\the\\pagetotal|\\the\\insertpen"
        "alties}\\showbox1 \\message{[ins]}\\showbox100 \\g"
        "lobal\\setbox100=\\box100 \\shipout\\box255 \\glob"
        "al\\advance\\count0 by 1 }\\hrule height10pt \\vsk"
        "ip0pt \\insert100{\\hrule height5pt\\vskip1pt\\hru"
        "le height5pt\\vskip1pt\\hrule height5pt}\\hrule he"
        "ight10pt \\vskip0pt \\insert100{\\hrule height4pt}"
        "\\hrule height10pt \\vskip0pt \\hrule height10pt "
        "\\vskip0pt \\hrule height10pt \\vskip0pt \\hrule h"
        "eight10pt \\end \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[page]\n> \\box1=\n\\hbox(6.44444+1.94444)x7"
        "9.44466\n.\\tenrm 4\n.\\tenrm 5\n.\\tenrm .\n.\\te"
        "nrm 0\n.\\tenrm p\n.\\tenrm t\n.\\tenrm |\n.\\tenr"
        "m 5\n.\\tenrm 0\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm "
        "p\n.\\tenrm t\n.\\tenrm |\n.\\tenrm 2\n\n! OK.\n<o"
        "utput> ...al |\\the \\insertpenalties }\\showbox 1"
        " \n                                               "
        "   \\message {[ins]}\\showbox 1...\n...\nl.1 ...he"
        "ight10pt \\vskip0pt \\hrule height10pt \\end\n    "
        "                                               \\s"
        "howbox254\n\n[ins]\n> \\box100=\n\\vbox(5.0+0.0)x0"
        ".0\n.\\rule(5.0+0.0)x*\n\n! OK.\n<output> ...howbo"
        "x 1 \\message {[ins]}\\showbox 100 \n             "
        "                                     \\global \\se"
        "tbox 100=\\box 1...\n...\nl.1 ...height10pt \\vski"
        "p0pt \\hrule height10pt \\end\n                   "
        "                                \\showbox254\n\n[1"
        "] [page]\n> \\box1=\n\\hbox(6.44444+1.94444)x79.44"
        "466\n.\\tenrm 4\n.\\tenrm 0\n.\\tenrm .\n.\\tenrm "
        "0\n.\\tenrm p\n.\\tenrm t\n.\\tenrm |\n.\\tenrm 2"
        "\n.\\tenrm 0\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm p\n"
        ".\\tenrm t\n.\\tenrm |\n.\\tenrm 2\n\n! OK.\n<outp"
        "ut> ...al |\\the \\insertpenalties }\\showbox 1 \n"
        "                                                  "
        "\\message {[ins]}\\showbox 1...\n...\nl.1 ...heigh"
        "t10pt \\vskip0pt \\hrule height10pt \\end\n       "
        "                                            \\show"
        "box254\n\n[ins]\n> \\box100=\n\\vbox(10.0+0.0)x0.0"
        "\n.\\rule(5.0+0.0)x*\n.\\glue(\\splittopskip) 0.0"
        "\n.\\rule(5.0+0.0)x*\n\n! OK.\n<output> ...howbox "
        "1 \\message {[ins]}\\showbox 100 \n               "
        "                                   \\global \\setb"
        "ox 100=\\box 1...\n...\nl.1 ...height10pt \\vskip0"
        "pt \\hrule height10pt \\end\n                     "
        "                              \\showbox254\n\n[2] "
        "[page]\n> \\box1=\n\\hbox(6.44444+1.94444)x74.4446"
        "4\n.\\tenrm 3\n.\\tenrm 5\n.\\tenrm .\n.\\tenrm 0"
        "\n.\\tenrm p\n.\\tenrm t\n.\\tenrm |\n.\\tenrm 0\n"
        ".\\tenrm .\n.\\tenrm 0\n.\\tenrm p\n.\\tenrm t\n."
        "\\tenrm |\n.\\tenrm 1\n\n! OK.\n<output> ...al |\\"
        "the \\insertpenalties }\\showbox 1 \n             "
        "                                     \\message {[i"
        "ns]}\\showbox 1...\n...\nl.1 ...height10pt \\vskip"
        "0pt \\hrule height10pt \\end\n                    "
        "                               \\showbox254\n\n[in"
        "s]\n> \\box100=\n\\vbox(15.0+0.0)x0.0\n.\\rule(5.0"
        "+0.0)x*\n.\\glue(\\splittopskip) 0.0\n.\\rule(5.0+"
        "0.0)x*\n.\\glue(\\splittopskip) 0.0\n.\\rule(5.0+0"
        ".0)x*\n\n! OK.\n<output> ...howbox 1 \\message {[i"
        "ns]}\\showbox 100 \n                              "
        "                    \\global \\setbox 100=\\box 1."
        "..\n...\nl.1 ...height10pt \\vskip0pt \\hrule heig"
        "ht10pt \\end\n                                    "
        "               \\showbox254\n\n[3] [page]\n> \\box"
        "1=\n\\hbox(6.44444+1.94444)x74.44464\n.\\tenrm 3\n"
        ".\\tenrm 1\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm p\n."
        "\\tenrm t\n.\\tenrm |\n.\\tenrm 0\n.\\tenrm .\n.\\"
        "tenrm 0\n.\\tenrm p\n.\\tenrm t\n.\\tenrm |\n.\\te"
        "nrm 0\n\n! OK.\n<output> ...al |\\the \\insertpena"
        "l",
        "ties }\\showbox 1 \n                              "
        "                    \\message {[ins]}\\showbox 1.."
        ".\n...\nl.1 ...height10pt \\vskip0pt \\hrule heigh"
        "t10pt \\end\n                                     "
        "              \\showbox254\n\n[ins]\n> \\box100=\n"
        "\\vbox(19.0+0.0)x0.0\n.\\rule(5.0+0.0)x*\n.\\glue("
        "\\splittopskip) 0.0\n.\\rule(5.0+0.0)x*\n.\\glue("
        "\\splittopskip) 0.0\n.\\rule(5.0+0.0)x*\n.\\rule(4"
        ".0+0.0)x*\n\n! OK.\n<output> ...howbox 1 \\message"
        " {[ins]}\\showbox 100 \n                          "
        "                        \\global \\setbox 100=\\bo"
        "x 1...\n...\nl.1 ...height10pt \\vskip0pt \\hrule "
        "height10pt \\end\n                                "
        "                   \\showbox254\n\n[4]",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* What an insertion takes from the page: the glue of its class and room
   for what the class's box already holds, once, and its own size scaled by
   the class's count. The page's insertions are added to that box at
   shipout. See docs/DECISIONS.md, insertions. */
static int test_an_insertion_on_a_page(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=3 \\show"
        "boxbreadth=100 \\baselineskip=12pt \\vsize=50pt \\"
        "maxdepth=2pt \\topskip=0pt \\count0=1 \\hsize=100p"
        "t \\dimen100=100pt \\count100=500 \\skip100=3pt pl"
        "us1pt \\dimen200=100pt \\count200=1000 \\skip200=0"
        "pt \\output={\\message{[goal]}\\setbox1=\\hbox{\\t"
        "he\\pagegoal|\\the\\pagetotal|\\the\\insertpenalti"
        "es|\\the\\ht100|\\the\\ht200}\\showbox1 \\message{"
        "[ins]}\\showbox100 \\shipout\\box255 \\global\\adv"
        "ance\\count0 by 1 }\\hrule height10pt \\vskip0pt "
        "\\insert100{\\hrule height7pt}\\hrule height10pt "
        "\\vskip0pt \\insert200{\\hrule height4pt}\\hrule h"
        "eight10pt \\vskip0pt \\hrule height10pt \\vskip0pt"
        " \\hrule height10pt \\vskip0pt \\insert100{\\hrule"
        " height5pt}\\hrule height10pt \\vskip0pt \\hrule h"
        "eight10pt \\end \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[goal]\n> \\box1=\n\\hbox(6.44444+1.94444)x1"
        "63.88933\n.\\tenrm 3\n.\\tenrm 9\n.\\tenrm .\n.\\t"
        "enrm 5\n.\\tenrm 0\n.\\tenrm 5\n.\\tenrm 7\n.\\ten"
        "rm 4\n.\\tenrm p\n.\\tenrm t\n.\\tenrm |\n.\\tenrm"
        " 4\n.\\tenrm 0\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm p"
        "\n.\\tenrm t\n.\\tenrm |\n.\\tenrm 0\n.\\tenrm |\n"
        ".\\tenrm 7\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm p\n."
        "\\tenrm t\n.\\tenrm |\n.\\tenrm 4\n.\\tenrm .\n.\\"
        "tenrm 0\n.\\tenrm p\n.\\tenrm t\n\n! OK.\n<output>"
        " ...|\\the \\ht 100|\\the \\ht 200}\\showbox 1 \n "
        "                                                 "
        "\\message {[ins]}\\showbox 1...\n...\nl.1 ...heigh"
        "t10pt \\vskip0pt \\hrule height10pt \\end\n       "
        "                                            \\show"
        "box254\n\n[ins]\n> \\box100=\n\\vbox(7.0+0.0)x0.0"
        "\n.\\rule(7.0+0.0)x*\n\n! OK.\n<output> ...howbox "
        "1 \\message {[ins]}\\showbox 100 \n               "
        "                                   \\shipout \\box"
        " 255 \\global ...\n...\nl.1 ...height10pt \\vskip0"
        "pt \\hrule height10pt \\end\n                     "
        "                              \\showbox254\n\n[1] "
        "[goal]\n> \\box1=\n\\hbox(6.44444+1.94444)x168.889"
        "34\n.\\tenrm 4\n.\\tenrm 1\n.\\tenrm .\n.\\tenrm 0"
        "\n.\\tenrm 1\n.\\tenrm 0\n.\\tenrm 9\n.\\tenrm 3\n"
        ".\\tenrm p\n.\\tenrm t\n.\\tenrm |\n.\\tenrm 4\n."
        "\\tenrm 0\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm p\n.\\"
        "tenrm t\n.\\tenrm |\n.\\tenrm 0\n.\\tenrm |\n.\\te"
        "nrm 1\n.\\tenrm 2\n.\\tenrm .\n.\\tenrm 0\n.\\tenr"
        "m p\n.\\tenrm t\n.\\tenrm |\n.\\tenrm 4\n.\\tenrm "
        ".\n.\\tenrm 0\n.\\tenrm p\n.\\tenrm t\n\n! OK.\n<o"
        "utput> ...|\\the \\ht 100|\\the \\ht 200}\\showbox"
        " 1 \n                                             "
        "     \\message {[ins]}\\showbox 1...\n...\nl.1 ..."
        "height10pt \\vskip0pt \\hrule height10pt \\end\n  "
        "                                                 "
        "\\showbox254\n\n[ins]\n> \\box100=\n\\vbox(12.0+0."
        "0)x0.0\n.\\rule(7.0+0.0)x*\n.\\rule(5.0+0.0)x*\n\n"
        "! OK.\n<output> ...howbox 1 \\message {[ins]}\\sho"
        "wbox 100 \n                                       "
        "           \\shipout \\box 255 \\global ...\n...\n"
        "l.1 ...height10pt \\vskip0pt \\hrule height10pt \\"
        "end\n                                             "
        "      \\showbox254\n\n[2]",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \topmark is what the page before ended with, \firstmark and \botmark the
   first and last marks of the page in hand; a page with no marks of a class
   keeps all three at what went before. \marks keeps a set of its own for
   each class. See docs/DECISIONS.md, marks. */
static int test_marks_on_a_page(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=2 \\show"
        "boxbreadth=100 \\baselineskip=12pt \\vsize=24pt \\"
        "maxdepth=2pt \\topskip=10pt \\count0=1 \\hsize=100"
        "pt \\output={\\message{[page\\the\\count0]}\\setbo"
        "x0=\\hbox{\\topmark/\\firstmark/\\botmark/\\botmar"
        "ks3}\\showbox0 \\shipout\\box255 \\global\\advance"
        "\\count0 by 1 }\\mark{one}\\marks3{alpha}aaa\\par "
        "bbb \\mark{two}ccc\\par \\marks3{beta}ddd\\par eee"
        "\\par \\mark{four}fff\\par ggg\\par \\end \\showbo"
        "x254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[page1]\n> \\box0=\n\\hbox(7.5+2.5)x64.72237"
        "\n.\\tenrm /\n.\\tenrm o\n.\\tenrm n\n.\\tenrm e\n"
        ".\\tenrm /\n.\\tenrm t\n.\\kern-0.27779\n.\\tenrm "
        "w\n.\\kern-0.27779\n.\\tenrm o\n.\\tenrm /\n.\\ten"
        "rm b\n.\\kern0.27779\n.\\tenrm e\n.\\tenrm t\n.\\t"
        "enrm a\n\n! OK.\n<output> ...ark /\\botmark /\\bot"
        "marks 3}\\showbox 0 \n                            "
        "                      \\shipout \\box 255 \\global"
        " ...\n...\nl.1 ...bb \\mark{two}ccc\\par \\marks3{"
        "beta}ddd\\par e\n                                 "
        "                 ee\\par \\mark{four}fff\\par ..."
        "\n\n\n[1] [page2]\n> \\box0=\n\\hbox(7.5+2.5)x84.7"
        "78\n.\\tenrm t\n.\\kern-0.27779\n.\\tenrm w\n.\\ke"
        "rn-0.27779\n.\\tenrm o\n.\\tenrm /\n.\\tenrm f\n."
        "\\tenrm o\n.\\tenrm u\n.\\tenrm r\n.\\tenrm /\n.\\"
        "tenrm f\n.\\tenrm o\n.\\tenrm u\n.\\tenrm r\n.\\te"
        "nrm /\n.\\tenrm b\n.\\kern0.27779\n.\\tenrm e\n.\\"
        "tenrm t\n.\\tenrm a\n\n! OK.\n<output> ...ark /\\b"
        "otmark /\\botmarks 3}\\showbox 0 \n               "
        "                                   \\shipout \\box"
        " 255 \\global ...\n...\nl.1 ...3{beta}ddd\\par eee"
        "\\par \\mark{four}fff\\par g\n                    "
        "                              gg\\par \\end \\show"
        "box254\n\n[2] [page3]\n> \\box0=\n\\hbox(7.5+2.5)x"
        "86.75026\n.\\tenrm f\n.\\tenrm o\n.\\tenrm u\n.\\t"
        "enrm r\n.\\tenrm /\n.\\tenrm f\n.\\tenrm o\n.\\ten"
        "rm u\n.\\tenrm r\n.\\tenrm /\n.\\tenrm f\n.\\tenrm"
        " o\n.\\tenrm u\n.\\tenrm r\n.\\tenrm /\n.\\tenrm b"
        "\n.\\kern0.27779\n.\\tenrm e\n.\\tenrm t\n.\\tenrm"
        " a\n\n! OK.\n<output> ...ark /\\botmark /\\botmark"
        "s 3}\\showbox 0 \n                                "
        "                  \\shipout \\box 255 \\global ..."
        "\n...\nl.1 ...par eee\\par \\mark{four}fff\\par gg"
        "g\\par \\end\n                                    "
        "               \\showbox254\n\n[3]",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Braces round a \vcenter package it, though braces round an \hbox give way
   to it: \vcenter makes an atom of its own kind, not an ordinary one. See
   docs/DECISIONS.md, a-vcenter-in-braces. */
static int test_a_vcenter_in_braces(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=4 \\show"
        "boxbreadth=100 \\message{[vc]}\\setbox0=\\hbox{$x{"
        "\\vcenter{\\hbox{a}}}y$}\\showbox0 \\message{[hb]}"
        "\\setbox0=\\hbox{$x{\\hbox{a}}y$}\\showbox0 \\mess"
        "age{[bare]}\\setbox0=\\hbox{$x\\vcenter{\\hbox{a}}"
        "y$}\\showbox0 \\message{[sup]}\\setbox0=\\hbox{$x^"
        "{\\vcenter{\\hbox{a}}}y$}\\showbox0 \\message{[two"
        "]}\\setbox0=\\hbox{$x{\\vcenter{\\hbox{a}}b}y$}\\s"
        "howbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[vc]\n> \\box0=\n\\hbox(4.65277+1.94444)x15."
        "97688\n.\\mathon\n.\\teni x\n.\\hbox(4.65277+0.0)x"
        "5.00002\n..\\vbox(4.65277+-0.34723)x5.00002\n...\\"
        "hbox(4.30554+0.0)x5.00002\n....\\tenrm a\n.\\teni "
        "y\n.\\kern0.35878\n.\\mathoff\n\n! OK.\nl.1 ...0="
        "\\hbox{$x{\\vcenter{\\hbox{a}}}y$}\\showbox0 \n   "
        "                                               \\m"
        "essage{[hb]}\\setbox0=\\hb...\n\n\n[hb]\n> \\box0="
        "\n\\hbox(4.30554+1.94444)x15.97688\n.\\mathon\n.\\"
        "teni x\n.\\hbox(4.30554+0.0)x5.00002\n..\\tenrm a"
        "\n.\\teni y\n.\\kern0.35878\n.\\mathoff\n\n! OK.\n"
        "l.1 ...b]}\\setbox0=\\hbox{$x{\\hbox{a}}y$}\\showb"
        "ox0 \n                                            "
        "      \\message{[bare]}\\setbox0=\\...\n\n\n[bare]"
        "\n> \\box0=\n\\hbox(4.65277+1.94444)x15.97688\n.\\"
        "mathon\n.\\teni x\n.\\vbox(4.65277+-0.34723)x5.000"
        "02\n..\\hbox(4.30554+0.0)x5.00002\n...\\tenrm a\n."
        "\\teni y\n.\\kern0.35878\n.\\mathoff\n\n! OK.\nl.1"
        " ...ox0=\\hbox{$x\\vcenter{\\hbox{a}}y$}\\showbox0"
        " \n                                               "
        "   \\message{[sup]}\\setbox0=\\h...\n\n\n[sup]\n> "
        "\\box0=\n\\hbox(7.5317+1.94444)x15.97688\n.\\matho"
        "n\n.\\teni x\n.\\vbox(3.90277+0.40277)x5.00002, sh"
        "ifted -3.62892\n..\\hbox(4.30554+0.0)x5.00002\n..."
        "\\tenrm a\n.\\teni y\n.\\kern0.35878\n.\\mathoff\n"
        "\n! OK.\nl.1 ...=\\hbox{$x^{\\vcenter{\\hbox{a}}}y"
        "$}\\showbox0 \n                                   "
        "               \\message{[two]}\\setbox0=\\h...\n"
        "\n\n[two]\n> \\box0=\n\\hbox(6.94444+1.94444)x20.2"
        "6854\n.\\mathon\n.\\teni x\n.\\hbox(6.94444+0.0)x9"
        ".29167\n..\\vbox(4.65277+-0.34723)x5.00002\n...\\h"
        "box(4.30554+0.0)x5.00002\n....\\tenrm a\n..\\teni "
        "b\n.\\teni y\n.\\kern0.35878\n.\\mathoff\n\n! OK."
        "\nl.1 ...=\\hbox{$x{\\vcenter{\\hbox{a}}b}y$}\\sho"
        "wbox0 \n                                          "
        "        \\showbox254\n\n> \\box254=void\n\n! OK.\n"
        "l.1 ...vcenter{\\hbox{a}}b}y$}\\showbox0 \\showbox"
        "254\n                                             "
        "     \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A limit widened to match the operator keeps the italic correction it ends
   with; one left at its own width loses it, exactly as a fraction's sides
   do. See docs/DECISIONS.md, a-clean-box-of-one-character. */
static int test_a_limit_at_its_own_width(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=6 \\show"
        "boxbreadth=100 \\message{[lim]}\\setbox0=\\hbox{$"
        "\\displaystyle\\mathop{\\vcenter{\\hbox{X}}}\\limi"
        "ts_{Z}^{W}$}\\showbox0 \\message{[wide]}\\setbox0="
        "\\hbox{$\\displaystyle\\mathop{\\hbox{}}\\limits_{"
        "ZZZZ}^{W}$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[lim]\n> \\box0=\n\\hbox(13.69998+8.36665)x8"
        ".59724\n.\\mathon\n.\\vbox(13.69998+8.36665)x8.597"
        "24\n..\\kern1.0\n..\\hbox(4.78334+0.0)x8.59724\n.."
        ".\\seveni W\n..\\kern1.99998\n..\\hbox(5.91666+0.9"
        "1666)x8.59724, glue set 0.54861fil\n...\\glue 0.0 "
        "plus 1.0fil minus 1.0fil\n...\\vbox(5.91666+0.9166"
        "6)x7.50002\n....\\hbox(6.83331+0.0)x7.50002\n....."
        "\\tenrm X\n...\\glue 0.0 plus 1.0fil minus 1.0fil"
        "\n..\\kern1.66666\n..\\hbox(4.78334+0.0)x8.59724, "
        "glue set 1.32918fil\n...\\glue 0.0 plus 1.0fil min"
        "us 1.0fil\n...\\seveni Z\n...\\kern0.49026\n...\\g"
        "lue 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\"
        "mathoff\n\n! OK.\nl.1 ...enter{\\hbox{X}}}\\limits"
        "_{Z}^{W}$}\\showbox0 \n                           "
        "                       \\message{[wide]}\\setbox0="
        "\\...\n\n\n[wide]\n> \\box0=\n\\hbox(7.78333+7.45)"
        "x23.75555\n.\\mathon\n.\\vbox(7.78333+7.45)x23.755"
        "55\n..\\kern1.0\n..\\hbox(4.78334+0.0)x23.75555, g"
        "lue set 7.57916fil\n...\\glue 0.0 plus 1.0fil minu"
        "s 1.0fil\n...\\seveni W\n...\\kern1.07639\n...\\gl"
        "ue 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.99998\n"
        "..\\hbox(0.0+0.0)x23.75555\n..\\kern1.66666\n..\\h"
        "box(4.78334+0.0)x23.75555\n...\\seveni Z\n...\\ker"
        "n0.49026\n...\\seveni Z\n...\\kern0.49026\n...\\se"
        "veni Z\n...\\kern0.49026\n...\\seveni Z\n...\\kern"
        "0.49026\n..\\kern1.0\n.\\mathoff\n\n! OK.\nl.1 ..."
        "thop{\\hbox{}}\\limits_{ZZZZ}^{W}$}\\showbox0 \n  "
        "                                                \\"
        "showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...}}"
        "\\limits_{ZZZZ}^{W}$}\\showbox0 \\showbox254\n    "
        "                                              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A rule \noalign leaves in an alignment, with no width of its own, is as
   wide as the alignment; and the alignment's own list is what a paragraph
   started in \noalign sees, so it takes \parskip when rows have gone before
   it. See docs/DECISIONS.md, a-rule-between-rows. */
static int test_a_rule_between_rows(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\#=6 \\catco"
        "de`\\&=4 \\showboxdepth=3 \\showboxbreadth=100 \\p"
        "arskip=7pt \\tabskip=2pt \\parindent=0pt \\message"
        "{[rules]}\\setbox0=\\vbox{\\halign{#\\hfil&#\\hfil"
        "\\cr\\noalign{\\hrule}aa&bb\\cr\\noalign{\\hrule h"
        "eight2pt}\\noalign{\\hrule width5pt}\\noalign{\\vr"
        "ule width3pt height1pt}cc&dd\\cr}}\\showbox0 \\mes"
        "sage{[first]}\\setbox0=\\vbox{\\halign{#\\hfil\\cr"
        "\\noalign{\\vrule width3pt height1pt}aa\\cr}}\\sho"
        "wbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[rules]\n> \\box0=\n\\vbox(29.74443+0.0)x200"
        ".0\n.\\rule(0.4+0.0)x27.11118\n.\\hbox(6.94444+0.0"
        ")x27.11118\n..\\glue(\\tabskip) 2.0\n..\\hbox(6.94"
        "444+0.0)x10.00003\n...\\tenrm a\n...\\tenrm a\n..."
        "\\glue 0.0 plus 1.0fil\n..\\glue(\\tabskip) 2.0\n."
        ".\\hbox(6.94444+0.0)x11.11115\n...\\tenrm b\n...\\"
        "tenrm b\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\tab"
        "skip) 2.0\n.\\rule(2.0+0.0)x27.11118\n.\\rule(0.4+"
        "0.0)x5.0\n.\\glue(\\parskip) 7.0\n.\\hbox(1.0+0.0)"
        "x200.0, glue set 197.0fil\n..\\hbox(0.0+0.0)x0.0\n"
        "..\\rule(1.0+*)x3.0\n..\\penalty 10000\n..\\glue("
        "\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightsk"
        "ip) 0.0\n.\\glue(\\baselineskip) 5.05556\n.\\hbox("
        "6.94444+0.0)x27.11118\n..\\glue(\\tabskip) 2.0\n.."
        "\\hbox(6.94444+0.0)x10.00003, glue set 1.11115fil"
        "\n...\\tenrm c\n...\\tenrm c\n...\\glue 0.0 plus 1"
        ".0fil\n..\\glue(\\tabskip) 2.0\n..\\hbox(6.94444+0"
        ".0)x11.11115\n...\\tenrm d\n...\\tenrm d\n...\\glu"
        "e 0.0 plus 1.0fil\n..\\glue(\\tabskip) 2.0\n\n! OK"
        ".\nl.1 ...ule width3pt height1pt}cc&dd\\cr}}\\show"
        "box0 \n                                           "
        "       \\message{[first]}\\setbox0=...\n\n\n[first"
        "]\n> \\box0=\n\\vbox(13.0+0.0)x200.0\n.\\hbox(1.0+"
        "0.0)x200.0, glue set 197.0fil\n..\\hbox(0.0+0.0)x0"
        ".0\n..\\rule(1.0+*)x3.0\n..\\penalty 10000\n..\\gl"
        "ue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\righ"
        "tskip) 0.0\n.\\glue(\\baselineskip) 7.69446\n.\\hb"
        "ox(4.30554+0.0)x14.00003\n..\\glue(\\tabskip) 2.0"
        "\n..\\hbox(4.30554+0.0)x10.00003\n...\\tenrm a\n.."
        ".\\tenrm a\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\"
        "tabskip) 2.0\n\n! OK.\nl.1 ...\\vrule width3pt hei"
        "ght1pt}aa\\cr}}\\showbox0 \n                      "
        "                            \\showbox254\n\n> \\bo"
        "x254=void\n\n! OK.\nl.1 ...h3pt height1pt}aa\\cr}}"
        "\\showbox0 \\showbox254\n                         "
        "                         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A row with fewer entries than the alignment has columns ends with the
   glue that follows its own last column, and carries nothing of the
   columns it never reached; see docs/DECISIONS.md, a-row-that-stops-early. */
static int test_a_row_that_stops_early(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\#=6 \\catco"
        "de`\\&=4 \\showboxdepth=2 \\showboxbreadth=100 \\m"
        "essage{[short]}\\tabskip=1pt \\setbox0=\\vbox{\\ha"
        "lign{#\\tabskip=5pt&#\\tabskip=7pt&#\\tabskip=9pt"
        "\\cr a&b&c\\cr d\\cr e&f\\cr\\cr\\omit x\\cr}}\\sh"
        "owbox0 \\message{[one]}\\setbox0=\\vbox{\\halign{#"
        "\\tabskip=5pt&#\\tabskip=7pt&#\\tabskip=9pt\\cr a"
        "\\cr bb\\cr}}\\showbox0 \\message{[two]}\\setbox0="
        "\\vbox{\\halign{#\\tabskip=5pt&#\\tabskip=7pt&#\\t"
        "abskip=9pt\\cr a&b\\cr cc\\cr}}\\showbox0 \\showbo"
        "x254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[short]\n> \\box0=\n\\vbox(54.94444+0.0)x37."
        "55559\n.\\hbox(6.94444+0.0)x37.55559\n..\\glue(\\t"
        "abskip) 1.0\n..\\hbox(6.94444+0.0)x5.55557 []\n.."
        "\\glue(\\tabskip) 5.0\n..\\hbox(6.94444+0.0)x5.555"
        "57 []\n..\\glue(\\tabskip) 7.0\n..\\hbox(6.94444+0"
        ".0)x4.44444 []\n..\\glue(\\tabskip) 9.0\n.\\glue("
        "\\baselineskip) 5.05556\n.\\hbox(6.94444+0.0)x37.5"
        "5559\n..\\glue(\\tabskip) 1.0\n..\\hbox(6.94444+0."
        "0)x5.55557 []\n..\\glue(\\tabskip) 5.0\n.\\glue(\\"
        "baselineskip) 5.05556\n.\\hbox(6.94444+0.0)x37.555"
        "59\n..\\glue(\\tabskip) 1.0\n..\\hbox(6.94444+0.0)"
        "x5.55557 []\n..\\glue(\\tabskip) 5.0\n..\\hbox(6.9"
        "4444+0.0)x5.55557 []\n..\\glue(\\tabskip) 7.0\n.\\"
        "glue(\\baselineskip) 12.0\n.\\hbox(0.0+0.0)x37.555"
        "59\n..\\glue(\\tabskip) 1.0\n..\\hbox(0.0+0.0)x5.5"
        "5557\n..\\glue(\\tabskip) 5.0\n.\\glue(\\baselines"
        "kip) 7.69446\n.\\hbox(4.30554+0.0)x37.55559\n..\\g"
        "lue(\\tabskip) 1.0\n..\\hbox(4.30554+0.0)x5.55557 "
        "[]\n..\\glue(\\tabskip) 5.0\n\n! OK.\nl.1 ...b&c\\"
        "cr d\\cr e&f\\cr\\cr\\omit x\\cr}}\\showbox0 \n   "
        "                                               \\m"
        "essage{[one]}\\setbox0=\\v...\n\n\n[one]\n> \\box0"
        "=\n\\vbox(16.30554+0.0)x17.11115\n.\\hbox(4.30554+"
        "0.0)x17.11115\n..\\glue(\\tabskip) 1.0\n..\\hbox(4"
        ".30554+0.0)x11.11115 []\n..\\glue(\\tabskip) 5.0\n"
        ".\\glue(\\baselineskip) 5.05556\n.\\hbox(6.94444+0"
        ".0)x17.11115\n..\\glue(\\tabskip) 1.0\n..\\hbox(6."
        "94444+0.0)x11.11115 []\n..\\glue(\\tabskip) 5.0\n"
        "\n! OK.\nl.1 ...7pt&#\\tabskip=9pt\\cr a\\cr bb\\c"
        "r}}\\showbox0 \n                                  "
        "                \\message{[two]}\\setbox0=\\v...\n"
        "\n\n[two]\n> \\box0=\n\\vbox(18.94444+0.0)x27.4444"
        "6\n.\\hbox(6.94444+0.0)x27.44446\n..\\glue(\\tabsk"
        "ip) 1.0\n..\\hbox(6.94444+0.0)x8.88889 []\n..\\glu"
        "e(\\tabskip) 5.0\n..\\hbox(6.94444+0.0)x5.55557 []"
        "\n..\\glue(\\tabskip) 7.0\n.\\glue(\\baselineskip)"
        " 7.69446\n.\\hbox(4.30554+0.0)x27.44446\n..\\glue("
        "\\tabskip) 1.0\n..\\hbox(4.30554+0.0)x8.88889 []\n"
        "..\\glue(\\tabskip) 5.0\n\n! OK.\nl.1 ...t&#\\tabs"
        "kip=9pt\\cr a&b\\cr cc\\cr}}\\showbox0 \n         "
        "                                         \\showbox"
        "254\n\n> \\box254=void\n\n! OK.\nl.1 ...=9pt\\cr a"
        "&b\\cr cc\\cr}}\\showbox0 \\showbox254\n          "
        "                                        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Looking back for the character to protrude past the right margin, a kern
   the font supplied is stepped over and one the document asked for is not,
   unless it takes up no room; see docs/DECISIONS.md, protruding-past-a-kern. */
static int test_protruding_past_a_kern(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=3 \\show"
        "boxbreadth=200 \\pdfprotrudechars=2 \\rpcode\\teni"
        "`Y=200 \\rpcode\\tenrm`Y=200 \\spaceskip=0pt \\mes"
        "sage{[m]}\\setbox0=\\vbox{\\noindent aaa the $XY$"
        "\\special{x}\\penalty-10000 ccc\\par}\\showbox0 \\"
        "message{[k]}\\setbox0=\\vbox{\\noindent aaa the $X"
        "Y\\kern2pt$\\special{x}\\penalty-10000 ccc\\par}\\"
        "showbox0 \\message{[z]}\\setbox0=\\vbox{\\noindent"
        " aaa the Y\\kern0pt\\special{x}\\penalty-10000 ccc"
        "\\par}\\showbox0 \\message{[e]}\\setbox0=\\vbox{\\"
        "noindent aaa the Y\\kern2pt\\special{x}\\penalty-1"
        "0000 ccc\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[m]\n> \\box0=\n\\vbox(18.94444+0.0)x200.0\n"
        ".\\hbox(6.94444+0.0)x200.0, glue set 44.80443\n.."
        "\\tenrm a\n..\\tenrm a\n..\\tenrm a\n..\\glue 3.33"
        "333 plus 1.66666 minus 1.11111\n..\\tenrm t\n..\\t"
        "enrm h\n..\\tenrm e\n..\\glue 3.33333 plus 1.66666"
        " minus 1.11111\n..\\mathon\n..\\teni X\n..\\kern0."
        "7847\n..\\teni Y\n..\\kern2.22223\n..\\mathoff\n.."
        "\\special{x}\n..\\kern-2.0 (right margin)\n..\\pen"
        "alty -10000\n..\\glue(\\rightskip) 0.0\n.\\glue(\\"
        "baselineskip) 7.69446\n.\\hbox(4.30554+0.0)x200.0,"
        " glue set 186.66667fil\n..\\tenrm c\n..\\tenrm c\n"
        "..\\tenrm c\n..\\penalty 10000\n..\\glue(\\parfill"
        "skip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n"
        "\n! OK.\nl.1 ...special{x}\\penalty-10000 ccc\\par"
        "}\\showbox0 \n                                    "
        "              \\message{[k]}\\setbox0=\\vbo...\n\n"
        "\n[k]\n> \\box0=\n\\vbox(18.94444+0.0)x200.0\n.\\h"
        "box(6.94444+0.0)x200.0, glue set 43.60442\n..\\ten"
        "rm a\n..\\tenrm a\n..\\tenrm a\n..\\glue 3.33333 p"
        "lus 1.66666 minus 1.11111\n..\\tenrm t\n..\\tenrm "
        "h\n..\\tenrm e\n..\\glue 3.33333 plus 1.66666 minu"
        "s 1.11111\n..\\mathon\n..\\teni X\n..\\kern0.7847"
        "\n..\\teni Y\n..\\kern2.22223\n..\\kern 2.0\n..\\m"
        "athoff\n..\\special{x}\n..\\penalty -10000\n..\\gl"
        "ue(\\rightskip) 0.0\n.\\glue(\\baselineskip) 7.694"
        "46\n.\\hbox(4.30554+0.0)x200.0, glue set 186.66667"
        "fil\n..\\tenrm c\n..\\tenrm c\n..\\tenrm c\n..\\pe"
        "nalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0f"
        "il\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...spe"
        "cial{x}\\penalty-10000 ccc\\par}\\showbox0 \n     "
        "                                             \\mes"
        "sage{[z]}\\setbox0=\\vbo...\n\n\n[z]\n> \\box0=\n"
        "\\vbox(18.94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x2"
        "00.0, glue set 47.6836\n..\\tenrm a\n..\\tenrm a\n"
        "..\\tenrm a\n..\\glue 3.33333 plus 1.66666 minus 1"
        ".11111\n..\\tenrm t\n..\\tenrm h\n..\\tenrm e\n.."
        "\\glue 3.33333 plus 1.66666 minus 1.11111\n..\\ten"
        "rm Y\n..\\kern 0.0\n..\\special{x}\n..\\kern-2.0 ("
        "right margin)\n..\\penalty -10000\n..\\glue(\\righ"
        "tskip) 0.0\n.\\glue(\\baselineskip) 7.69446\n.\\hb"
        "ox(4.30554+0.0)x200.0, glue set 186.66667fil\n..\\"
        "tenrm c\n..\\tenrm c\n..\\tenrm c\n..\\penalty 100"
        "00\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\g"
        "lue(\\rightskip) 0.0\n\n! OK.\nl.1 ...special{x}\\"
        "penalty-10000 ccc\\par}\\showbox0 \n              "
        "                                    \\message{[e]}"
        "\\setbox0=\\vbo...\n\n\n[e]\n> \\box0=\n\\vbox(18."
        "94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0, glue"
        " set 46.4836\n..\\tenrm a\n..\\tenrm a\n..\\tenrm "
        "a\n..\\glue 3.33333 plus 1.66666 minus 1.11111\n.."
        "\\tenrm t\n..\\tenrm h\n..\\tenrm e\n..\\glue 3.33"
        "333 plus 1.66666 minus 1.11111\n..\\tenrm Y\n..\\k"
        "ern 2.0\n..\\special{x}\n..\\penalty -10000\n..\\g"
        "lue(\\rightskip) 0.0\n.\\glue(\\baselineskip) 7.69"
        "446\n.\\hbox(4.30554+0.0)x200.0, glue set 186.6666"
        "7fil\n..\\tenrm c\n..\\tenrm c\n..\\tenrm c\n..\\p"
        "enalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0"
        "fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...sp"
        "ecial{x}\\penalty-10000 ccc\\par}\\showbox0 \n  ",
        "                                                \\"
        "showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...pen"
        "alty-10000 ccc\\par}\\showbox0 \\showbox254\n     "
        "                                             \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A space, a kern, a penalty and glue all leave the space factor where the
   characters put it; only a box or a rule sets it back. Two spaces in a row
   -- which happens where one file's last line ends a sentence and the line
   that read it ends too -- are therefore both wide. See docs/DECISIONS.md,
   what-resets-the-space-factor. */
static int test_a_space_leaves_the_factor_alone(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=3 \\show"
        "boxbreadth=100 \\sfcode`\\.=3000 \\spaceskip=0pt "
        "\\message{[sf]}\\setbox0=\\hbox{abc. \\hskip3pt x."
        " \\kern2pt y. \\penalty5 z.\\lower1pt\\hbox{} w. q"
        "}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[sf]\n> \\box0=\n\\hbox(6.94444+1.94444)x81."
        "94456\n.\\tenrm a\n.\\tenrm b\n.\\kern0.27779\n.\\"
        "tenrm c\n.\\tenrm .\n.\\glue 4.44444 plus 4.99997 "
        "minus 0.37036\n.\\glue 3.0\n.\\tenrm x\n.\\tenrm ."
        "\n.\\glue 4.44444 plus 4.99997 minus 0.37036\n.\\k"
        "ern 2.0\n.\\tenrm y\n.\\kern-0.83334\n.\\tenrm .\n"
        ".\\glue 4.44444 plus 4.99997 minus 0.37036\n.\\pen"
        "alty 5\n.\\tenrm z\n.\\tenrm .\n.\\hbox(0.0+0.0)x0"
        ".0, shifted 1.0\n.\\glue 3.33333 plus 1.66666 minu"
        "s 1.11111\n.\\tenrm w\n.\\tenrm .\n.\\glue 4.44444"
        " plus 4.99997 minus 0.37036\n.\\tenrm q\n\n! OK.\n"
        "l.1 ...penalty5 z.\\lower1pt\\hbox{} w. q}\\showbo"
        "x0 \n                                             "
        "     \\showbox254\n\n> \\box254=void\n\n! OK.\nl.1"
        " ...\\lower1pt\\hbox{} w. q}\\showbox0 \\showbox25"
        "4\n                                               "
        "   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The look TeX takes at the first token of an alignment entry, to see
   whether it is \omit, expands what it finds -- but not a \protected
   macro, which is put back and expanded inside the entry, after the
   template has run. See docs/DECISIONS.md, the-look-before-an-entry. */
static int test_the_look_before_an_entry(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\#=6 \\catco"
        "de`\\&=4 \\showboxdepth=6 \\showboxbreadth=100 \\p"
        "rotected\\def\\pb{\\ifmmode M\\else T\\fi}\\def\\u"
        "b{\\ifmmode M\\else T\\fi}\\tabskip=0pt \\message{"
        "[peek]}\\setbox0=\\vbox{\\halign{$#$\\hfil&$#$\\hf"
        "il\\cr\\pb&\\ub\\cr\\omit\\pb&x\\pb\\cr}}\\showbox"
        "0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[peek]\n> \\box0=\n\\vbox(18.83331+0.0)x27.2"
        "9858\n.\\hbox(6.83331+0.0)x27.29858\n..\\glue(\\ta"
        "bskip) 0.0\n..\\hbox(6.83331+0.0)x10.79166\n...\\m"
        "athon\n...\\teni M\n...\\kern1.09026\n...\\mathoff"
        "\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\tabskip) 0"
        ".0\n..\\hbox(6.83331+0.0)x16.50693, glue set 9.274"
        "28fil\n...\\mathon\n...\\teni T\n...\\kern1.3889\n"
        "...\\mathoff\n...\\glue 0.0 plus 1.0fil\n..\\glue("
        "\\tabskip) 0.0\n.\\glue(\\baselineskip) 5.16669\n."
        "\\hbox(6.83331+0.0)x27.29858\n..\\glue(\\tabskip) "
        "0.0\n..\\hbox(6.83331+0.0)x10.79166\n...\\tenrm T"
        "\n..\\glue(\\tabskip) 0.0\n..\\hbox(6.83331+0.0)x1"
        "6.50693\n...\\mathon\n...\\teni x\n...\\teni M\n.."
        ".\\kern1.09026\n...\\mathoff\n...\\glue 0.0 plus 1"
        ".0fil\n..\\glue(\\tabskip) 0.0\n\n! OK.\nl.1 ...il"
        "\\cr\\pb&\\ub\\cr\\omit\\pb&x\\pb\\cr}}\\showbox0 "
        "\n                                                "
        "  \\showbox254\n\n> \\box254=void\n\n! OK.\nl.1 .."
        ".b\\cr\\omit\\pb&x\\pb\\cr}}\\showbox0 \\showbox25"
        "4\n                                               "
        "   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A kern \mkern asked for stays an explicit one once its mu have been
   turned into points, which is what \showbox shows by leaving a space after
   the name; the italic correction a character carries does not. \nonscript
   leaves a marker of its own in the list whatever the style. See
   docs/DECISIONS.md, a-kern-in-mu and nonscript. */
static int test_a_kern_in_mu(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\showboxdepth=3 \\show"
        "boxbreadth=100 \\medmuskip=4mu plus 2mu minus 4mu "
        "\\message{[k]}\\setbox0=\\hbox{a\\kern3pt b\\/ c$x"
        "\\mkern18mu y$}\\showbox0 \\message{[t]}\\setbox0="
        "\\hbox{$x\\nonscript\\mskip9mu y\\nonscript\\mkern"
        "9mu z\\nonscript\\ w$}\\showbox0 \\message{[s]}\\s"
        "etbox0=\\hbox{$\\scriptstyle x\\nonscript\\mskip9m"
        "u y\\nonscript\\mkern9mu z$}\\showbox0 \\message{["
        "n]}\\setbox0=\\hbox{$x\\nonscript y$}\\showbox0 \\"
        "showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[k]\n> \\box0=\n\\hbox(6.94444+1.94444)x42.9"
        "7665\n.\\tenrm a\n.\\kern 3.0\n.\\tenrm b\n.\\kern"
        " 0.0\n.\\glue(\\spaceskip) 4.0\n.\\tenrm c\n.\\mat"
        "hon\n.\\teni x\n.\\kern 9.99976\n.\\teni y\n.\\ker"
        "n0.35878\n.\\mathoff\n\n! OK.\nl.1 ...x{a\\kern3pt"
        " b\\/ c$x\\mkern18mu y$}\\showbox0 \n             "
        "                                     \\message{[t]"
        "}\\setbox0=\\hbo...\n\n\n[t]\n> \\box0=\n\\hbox(4."
        "30554+1.94444)x37.49518\n.\\mathon\n.\\teni x\n.\\"
        "glue(\\nonscript)\n.\\glue 4.99988\n.\\teni y\n.\\"
        "kern0.35878\n.\\glue(\\nonscript)\n.\\kern 4.99988"
        "\n.\\teni z\n.\\kern0.4398\n.\\glue(\\nonscript)\n"
        ".\\glue(\\spaceskip) 4.0\n.\\teni w\n.\\kern0.2690"
        "9\n.\\mathoff\n\n! OK.\nl.1 ...nscript\\mkern9mu z"
        "\\nonscript\\ w$}\\showbox0 \n                    "
        "                              \\message{[s]}\\setb"
        "ox0=\\hbo...\n\n\n[s]\n> \\box0=\n\\hbox(3.01389+1"
        ".3611)x12.94916\n.\\mathon\n.\\seveni x\n.\\glue("
        "\\nonscript)\n.\\seveni y\n.\\kern0.25116\n.\\glue"
        "(\\nonscript)\n.\\seveni z\n.\\kern0.28703\n.\\mat"
        "hoff\n\n! OK.\nl.1 ...mskip9mu y\\nonscript\\mkern"
        "9mu z$}\\showbox0 \n                              "
        "                    \\message{[n]}\\setbox0=\\hbo."
        "..\n\n\n[n]\n> \\box0=\n\\hbox(4.30554+1.94444)x10"
        ".97687\n.\\mathon\n.\\teni x\n.\\glue(\\nonscript)"
        "\n.\\teni y\n.\\kern0.35878\n.\\mathoff\n\n! OK.\n"
        "l.1 ...]}\\setbox0=\\hbox{$x\\nonscript y$}\\showb"
        "ox0 \n                                            "
        "      \\showbox254\n\n> \\box254=void\n\n! OK.\nl."
        "1 ...\\hbox{$x\\nonscript y$}\\showbox0 \\showbox2"
        "54\n                                              "
        "    \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A display interrupts the paragraph, so the lines before it reach the
   vertical list and the page builder runs while the formula is still
   unread. The output routine it fires sees the page counter it advanced
   and the display's own parameters, which go back to what they were when
   the display ends; see docs/DECISIONS.md, a-page-that-breaks-at-a-display. */
static int test_a_page_that_breaks_at_a_display(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\hsize=100pt \\vsize=4"
        "0pt \\maxdepth=2pt \\topskip=10pt \\count0=1 \\abo"
        "vedisplayskip=6pt \\belowdisplayskip=6pt \\abovedi"
        "splayshortskip=6pt \\belowdisplayshortskip=6pt \\p"
        "redisplaysize=99pt \\displaywidth=99pt \\showboxde"
        "pth=1 \\showboxbreadth=20 \\parfillskip=0pt plus1f"
        "il \\output={\\global\\setbox9=\\box255 \\global\\"
        "advance\\count0 by 1 }aaa aaa aaa aaa aaa aaa aaa "
        "aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aa"
        "a aaa aaa aaa aaa $$\\message{[at]}\\setbox0=\\hbo"
        "x{\\the\\count0/\\the\\predisplaysize/\\the\\displ"
        "aywidth}\\showbox0 x$$ \\message{[past]}\\setbox0="
        "\\hbox{\\the\\count0/\\the\\predisplaysize/\\the\\"
        "displaywidth}\\showbox0 bbb bbb bbb bbb bbb bbb bb"
        "b bbb bbb bbb bbb bbb bbb bbb bbb bbb $$\\message{"
        "[again]}\\setbox0=\\hbox{\\the\\count0}\\showbox0 "
        "y$$ \\message{[ended]}\\setbox0=\\hbox{\\the\\coun"
        "t0}\\showbox0 ccc\\par \\message{[after]}\\setbox0"
        "=\\hbox{\\the\\count0/\\the\\predisplaysize/\\the"
        "\\displaywidth}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[at]\n> \\box0=\n\\hbox(7.5+2.5)x94.44473\n."
        "\\tenrm 2\n.\\tenrm /\n.\\tenrm 9\n.\\tenrm 2\n.\\"
        "tenrm .\n.\\tenrm 0\n.\\tenrm 0\n.\\tenrm 0\n.\\te"
        "nrm 2\n.\\tenrm 1\n.\\tenrm p\n.\\tenrm t\n.\\tenr"
        "m /\n.\\tenrm 1\n.\\tenrm 0\n.\\tenrm 0\n.\\tenrm "
        ".\n.\\tenrm 0\n.\\tenrm p\n.\\tenrm t\n\n! OK.\nl."
        "1 ...predisplaysize/\\the\\displaywidth}\\showbox0"
        " \n                                               "
        "   x$$ \\message{[past]}\\setbo...\n\n\n[past]\n> "
        "\\box0=\n\\hbox(7.5+2.5)x69.44466\n.\\tenrm 2\n.\\"
        "tenrm /\n.\\tenrm 9\n.\\tenrm 9\n.\\tenrm .\n.\\te"
        "nrm 0\n.\\tenrm p\n.\\tenrm t\n.\\tenrm /\n.\\tenr"
        "m 9\n.\\tenrm 9\n.\\tenrm .\n.\\tenrm 0\n.\\tenrm "
        "p\n.\\tenrm t\n\n! OK.\nl.1 ...predisplaysize/\\th"
        "e\\displaywidth}\\showbox0 \n                     "
        "                             bbb bbb bbb bbb bbb b"
        "bb bb...\n\n\n[again]\n> \\box0=\n\\hbox(6.44444+0"
        ".0)x5.00002\n.\\tenrm 3\n\n! OK.\nl.1 ...gain]}\\s"
        "etbox0=\\hbox{\\the\\count0}\\showbox0 \n         "
        "                                         y$$ \\mes"
        "sage{[ended]}\\setb...\n\n\n[ended]\n> \\box0=\n\\"
        "hbox(6.44444+0.0)x5.00002\n.\\tenrm 4\n\n! OK.\nl."
        "1 ...nded]}\\setbox0=\\hbox{\\the\\count0}\\showbo"
        "x0 \n                                             "
        "     ccc\\par \\message{[after]}\\...\n\n\n[after]"
        "\n> \\box0=\n\\hbox(7.5+2.5)x69.44466\n.\\tenrm 4"
        "\n.\\tenrm /\n.\\tenrm 9\n.\\tenrm 9\n.\\tenrm .\n"
        ".\\tenrm 0\n.\\tenrm p\n.\\tenrm t\n.\\tenrm /\n."
        "\\tenrm 9\n.\\tenrm 9\n.\\tenrm .\n.\\tenrm 0\n.\\"
        "tenrm p\n.\\tenrm t\n\n! OK.\nl.1 ...predisplaysiz"
        "e/\\the\\displaywidth}\\showbox0 \n               "
        "                                   \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...ize/\\the\\disp"
        "laywidth}\\showbox0 \\showbox254\n                "
        "                                  \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* An accent alone in braces takes the place of the ordinary atom the
   braces would have made: {\widehat W} is the accent itself, scripts and
   all, while \mathop{...} and a group of two keep their sub-formula; see
   docs/DECISIONS.md, an-accent-alone-in-braces. */
static int test_an_accent_alone_in_braces(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\def\\W{\\mathaccent\""
        "7016 W}\\message{[alone]}\\setbox0=\\hbox{$x{\\W}y"
        "$}\\showbox0 \\message{[ord]}\\setbox0=\\hbox{$x\\"
        "mathord{\\W^2}y$}\\showbox0 \\message{[op]}\\setbo"
        "x0=\\hbox{$x\\mathop{\\W}y$}\\showbox0 \\message{["
        "two]}\\setbox0=\\hbox{$x{\\W a}y$}\\showbox0 \\mes"
        "sage{[frac]}\\setbox0=\\hbox{$\\displaystyle{W\\ov"
        "er{\\W}}$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[alone]\n> \\box0=\n\\hbox(8.20554+1.94444)x"
        "21.81021\n.\\mathon\n.\\teni x\n.\\vbox(8.20554+0."
        "0)x10.83334\n..\\hbox(5.67776+0.0)x0.0, shifted 2."
        "91667\n...\\tenrm ^^V\n..\\kern-4.30554\n..\\hbox("
        "6.83331+0.0)x10.83334\n...\\teni W\n.\\teni y\n.\\"
        "kern0.35878\n.\\mathoff\n\n! OK.\nl.1 ...{[alone]}"
        "\\setbox0=\\hbox{$x{\\W}y$}\\showbox0 \n          "
        "                                        \\message{"
        "[ord]}\\setbox0=\\h...\n\n\n[ord]\n> \\box0=\n\\hb"
        "ox(8.20554+1.94444)x25.79634\n.\\mathon\n.\\teni x"
        "\n.\\vbox(8.20554+0.0)x14.81947\n..\\hbox(5.67776+"
        "0.0)x0.0, shifted 2.91667\n...\\tenrm ^^V\n..\\ker"
        "n-5.61226\n..\\hbox(8.14003+0.0)x14.81947\n...\\te"
        "ni W\n...\\kern1.3889\n...\\hbox(4.51111+0.0)x3.98"
        "613, shifted -3.62892\n....\\sevenrm 2\n.\\teni y"
        "\n.\\kern0.35878\n.\\mathoff\n\n! OK.\nl.1 ...setb"
        "ox0=\\hbox{$x\\mathord{\\W^2}y$}\\showbox0 \n     "
        "                                             \\mes"
        "sage{[op]}\\setbox0=\\hb...\n\n\n[op]\n> \\box0=\n"
        "\\hbox(8.20554+1.94444)x21.81021\n.\\mathon\n.\\te"
        "ni x\n.\\glue(\\thinmuskip) 0.0\n.\\hbox(8.20554+0"
        ".0)x10.83334\n..\\vbox(8.20554+0.0)x10.83334\n..."
        "\\hbox(5.67776+0.0)x0.0, shifted 2.91667\n....\\te"
        "nrm ^^V\n...\\kern-4.30554\n...\\hbox(6.83331+0.0)"
        "x10.83334\n....\\teni W\n.\\glue(\\thinmuskip) 0.0"
        "\n.\\teni y\n.\\kern0.35878\n.\\mathoff\n\n! OK.\n"
        "l.1 ...]}\\setbox0=\\hbox{$x\\mathop{\\W}y$}\\show"
        "box0 \n                                           "
        "       \\message{[two]}\\setbox0=\\h...\n\n\n[two]"
        "\n> \\box0=\n\\hbox(8.20554+1.94444)x27.0961\n.\\m"
        "athon\n.\\teni x\n.\\hbox(8.20554+0.0)x16.11923\n."
        ".\\vbox(8.20554+0.0)x10.83334\n...\\hbox(5.67776+0"
        ".0)x0.0, shifted 2.91667\n....\\tenrm ^^V\n...\\ke"
        "rn-4.30554\n...\\hbox(6.83331+0.0)x10.83334\n...."
        "\\teni W\n..\\teni a\n.\\teni y\n.\\kern0.35878\n."
        "\\mathoff\n\n! OK.\nl.1 ...{[two]}\\setbox0=\\hbox"
        "{$x{\\W a}y$}\\showbox0 \n                        "
        "                          \\message{[frac]}\\setbo"
        "x0=\\...\n\n\n[frac]\n> \\box0=\n\\hbox(13.59839+7"
        ".10547)x10.83334\n.\\mathon\n.\\hbox(13.59839+7.10"
        "547)x10.83334\n..\\hbox(13.59839+7.10547)x10.83334"
        "\n...\\hbox(0.0+0.0)x0.0, shifted -2.5\n...\\vbox("
        "13.59839+7.10547)x10.83334\n....\\hbox(6.83331+0.0"
        ")x10.83334\n.....\\teni W\n....\\kern4.06508\n...."
        "\\rule(0.39998+0.0)x*\n....\\kern1.19994\n....\\vb"
        "ox(8.20554+0.0)x10.83334\n.....\\hbox(5.67776+0.0)"
        "x0.0, shifted 2.91667\n......\\tenrm ^^V\n.....\\k"
        "ern-4.30554\n.....\\hbox(6.83331+0.0)x10.83334\n.."
        "....\\teni W\n...\\hbox(0.0+0.0)x0.0, shifted -2.5"
        "\n.\\mathoff\n\n! OK.\nl.1 ...hbox{$\\displaystyle"
        "{W\\over{\\W}}$}\\showbox0 \n                     "
        "                             \\showbox254\n\n> \\b"
        "ox254=void\n\n! OK.\nl.1 ...laystyle{W\\over{\\W}}"
        "$}\\showbox0 \\showbox254\n                       "
        "                           \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A \left ... \right group is set at the style it lands in, not the one it
   was read in: a fraction's numerator is smaller, and everything in the group
   follows; see docs/DECISIONS.md, a-fence-is-set-in-place. */
static int test_a_fence_is_set_in_place(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\catco"
        "de`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\showb"
        "oxdepth=5 \\showboxbreadth=100 \\nulldelimiterspac"
        "e=1.2pt \\delimitershortfall=5pt \\delimiterfactor"
        "=901 \\thinmuskip=3mu \\medmuskip=4mu \\thickmuski"
        "p=5mu \\message{[display]}\\setbox0=\\hbox{$\\disp"
        "laystyle\\left\\delimiter\"026A30C \\mathchar\"135"
        "2 x\\right.$}\\showbox0 \\message{[numerator]}\\se"
        "tbox0=\\hbox{$\\displaystyle{\\left\\delimiter\"02"
        "6A30C \\mathchar\"1352 x\\right.\\over y}$}\\showb"
        "ox0 \\message{[script]}\\setbox0=\\hbox{$\\display"
        "style\\left\\delimiter\"026A30C \\mathchar\"1352 x"
        "\\right.^2$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[display]\n> \\box0=\n\\hbox(14.50012+9.5001"
        "2)x21.91524\n.\\mathon\n.\\hbox(14.50012+9.50012)x"
        "21.91524\n..\\vbox(0.0+24.00024)x3.33333, shifted "
        "-14.50012\n...\\hbox(0.0+6.00006)x3.33333\n....\\t"
        "enex ^^L\n...\\hbox(0.0+6.00006)x3.33333\n....\\te"
        "nex ^^L\n...\\hbox(0.0+6.00006)x3.33333\n....\\ten"
        "ex ^^L\n...\\hbox(0.0+6.00006)x3.33333\n....\\tene"
        "x ^^L\n..\\vbox(13.61122+8.61124)x10.00002\n...\\h"
        "box(13.61122+8.61124)x10.00002\n....\\hbox(0.0+22."
        "22246)x10.00002, shifted -13.61122\n.....\\tenex Z"
        "\n..\\glue(\\thinmuskip) 1.66663\n..\\teni x\n..\\"
        "hbox(0.0+0.0)x1.2, shifted -2.5\n.\\mathoff\n\n! O"
        "K.\nl.1 ...026A30C \\mathchar\"1352 x\\right.$}\\s"
        "howbox0 \n                                        "
        "          \\message{[numerator]}\\setb...\n\n\n[nu"
        "merator]\n> \\box0=\n\\hbox(15.90005+8.80396)x20.9"
        "8189\n.\\mathon\n.\\hbox(15.90005+8.80396)x20.9818"
        "9\n..\\hbox(15.90005+8.80396)x20.98189\n...\\hbox("
        "0.0+0.0)x1.2, shifted -2.5\n...\\vbox(15.90005+8.8"
        "0396)x18.5819\n....\\hbox(8.50006+3.50006)x18.5819"
        "\n.....\\vbox(0.0+12.00012)x3.33333, shifted -8.50"
        "006 []\n.....\\hbox(0.0+11.11122)x6.66667, shifted"
        " -8.0556 []\n.....\\glue(\\thinmuskip) 1.66663\n.."
        "...\\teni x\n.....\\hbox(0.0+0.0)x1.2, shifted -2."
        "5\n....\\kern1.19994\n....\\rule(0.39998+0.0)x*\n."
        "...\\kern4.85397\n....\\hbox(4.30554+1.94444)x18.5"
        "819, glue set 6.66016fil\n.....\\glue 0.0 plus 1.0"
        "fil minus 1.0fil\n.....\\teni y\n.....\\kern0.3587"
        "8\n.....\\glue 0.0 plus 1.0fil minus 1.0fil\n...\\"
        "hbox(0.0+0.0)x1.2, shifted -2.5\n.\\mathoff\n\n! O"
        "K.\nl.1 ...\\mathchar\"1352 x\\right.\\over y}$}\\"
        "showbox0 \n                                       "
        "           \\message{[script]}\\setbox0...\n\n\n[s"
        "cript]\n> \\box0=\n\\hbox(16.53903+9.50012)x25.901"
        "37\n.\\mathon\n.\\hbox(14.50012+9.50012)x21.91524"
        "\n..\\vbox(0.0+24.00024)x3.33333, shifted -14.5001"
        "2\n...\\hbox(0.0+6.00006)x3.33333\n....\\tenex ^^L"
        "\n...\\hbox(0.0+6.00006)x3.33333\n....\\tenex ^^L"
        "\n...\\hbox(0.0+6.00006)x3.33333\n....\\tenex ^^L"
        "\n...\\hbox(0.0+6.00006)x3.33333\n....\\tenex ^^L"
        "\n..\\vbox(13.61122+8.61124)x10.00002\n...\\hbox(1"
        "3.61122+8.61124)x10.00002\n....\\hbox(0.0+22.22246"
        ")x10.00002, shifted -13.61122\n.....\\tenex Z\n.."
        "\\glue(\\thinmuskip) 1.66663\n..\\teni x\n..\\hbox"
        "(0.0+0.0)x1.2, shifted -2.5\n.\\hbox(4.51111+0.0)x"
        "3.98613, shifted -12.02792\n..\\sevenrm 2\n.\\math"
        "off\n\n! OK.\nl.1 ...6A30C \\mathchar\"1352 x\\rig"
        "ht.^2$}\\showbox0 \n                              "
        "                    \\showbox254\n\n> \\box254=voi"
        "d\n\n! OK.\nl.1 ...char\"1352 x\\right.^2$}\\showb"
        "ox0 \\showbox254\n                                "
        "                  \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The two delimiters of a \left...\right group are an opening and a
   closing atom, so the spacing round what they hold is the ordinary spacing;
   see docs/DECISIONS.md, delimiters-are-atoms. */
static int test_delimiters_are_atoms(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\catco"
        "de`\\\"=12 \\showboxdepth=4 \\showboxbreadth=100 "
        "\\nulldelimiterspace=1.2pt \\delimitershortfall=5p"
        "t \\delimiterfactor=901 \\thinmuskip=3mu \\medmusk"
        "ip=4mu plus 2mu \\thickmuskip=5mu plus 5mu \\mathc"
        "ode`\\+=\"202B \\mathcode`\\,=\"613B \\mathcode`\\"
        "==\"303D \\message{[punct]}\\setbox0=\\hbox{$\\lef"
        "t\\delimiter\"026A30C x,\\right.$}\\showbox0 \\mes"
        "sage{[rel]}\\setbox0=\\hbox{$\\left\\delimiter\"02"
        "6A30C x=y\\right.$}\\showbox0 \\message{[bin]}\\se"
        "tbox0=\\hbox{$\\left\\delimiter\"026A30C +x\\right"
        ".$}\\showbox0 \\message{[after]}\\setbox0=\\hbox{$"
        "y\\left\\delimiter\"026A30C x\\right.z$}\\showbox0"
        " \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[punct]\n> \\box0=\n\\hbox(7.5+2.5)x14.13747"
        "\n.\\mathon\n.\\hbox(7.5+2.5)x14.13747\n..\\hbox(7"
        ".5+2.5)x2.77779\n...\\tensy j\n..\\teni x\n..\\ten"
        "i ;\n..\\glue(\\thinmuskip) 1.66663\n..\\hbox(0.0+"
        "0.0)x1.2, shifted -2.5\n.\\mathoff\n\n! OK.\nl.1 ."
        "..eft\\delimiter\"026A30C x,\\right.$}\\showbox0 "
        "\n                                                "
        "  \\message{[rel]}\\setbox0=\\h...\n\n\n[rel]\n> "
        "\\box0=\n\\hbox(7.5+2.5)x28.28787\n.\\mathon\n.\\h"
        "box(7.5+2.5)x28.28787\n..\\hbox(7.5+2.5)x2.77779\n"
        "...\\tensy j\n..\\teni x\n..\\glue(\\thickmuskip) "
        "2.77771 plus 2.77771\n..\\tenrm =\n..\\glue(\\thic"
        "kmuskip) 2.77771 plus 2.77771\n..\\teni y\n..\\ker"
        "n0.35878\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\"
        "mathoff\n\n! OK.\nl.1 ...ft\\delimiter\"026A30C x="
        "y\\right.$}\\showbox0 \n                          "
        "                        \\message{[bin]}\\setbox0="
        "\\h...\n\n\n[bin]\n> \\box0=\n\\hbox(7.5+2.5)x17.4"
        "7086\n.\\mathon\n.\\hbox(7.5+2.5)x17.47086\n..\\hb"
        "ox(7.5+2.5)x2.77779\n...\\tensy j\n..\\tenrm +\n.."
        "\\teni x\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\"
        "mathoff\n\n! OK.\nl.1 ...eft\\delimiter\"026A30C +"
        "x\\right.$}\\showbox0 \n                          "
        "                        \\message{[after]}\\setbox"
        "0=...\n\n\n[after]\n> \\box0=\n\\hbox(7.5+2.5)x23."
        "3782\n.\\mathon\n.\\teni y\n.\\kern0.35878\n.\\glu"
        "e(\\thinmuskip) 1.66663\n.\\hbox(7.5+2.5)x9.69305"
        "\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j\n..\\ten"
        "i x\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\glue("
        "\\thinmuskip) 1.66663\n.\\teni z\n.\\kern0.4398\n."
        "\\mathoff\n\n! OK.\nl.1 ...eft\\delimiter\"026A30C"
        " x\\right.z$}\\showbox0 \n                        "
        "                          \\showbox254\n\n> \\box2"
        "54=void\n\n! OK.\nl.1 ...er\"026A30C x\\right.z$}"
        "\\showbox0 \\showbox254\n                         "
        "                         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* What stands between \left and \right is spliced into the line rather
   than boxed, and the pieces of an extensible delimiter are stacked flush;
   see docs/DECISIONS.md, what-stands-between-delimiters. */
static int test_what_stands_between_delimiters(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\catco"
        "de`\\\"=12 \\showboxdepth=6 \\showboxbreadth=100 "
        "\\nulldelimiterspace=1.2pt \\delimitershortfall=5p"
        "t \\delimiterfactor=901 \\thinmuskip=3mu \\medmusk"
        "ip=4mu \\thickmuskip=5mu \\mathcode`\\+=\"202B \\m"
        "essage{[many]}\\setbox0=\\hbox{$\\left\\delimiter"
        "\"026A30C x+y\\right.$}\\showbox0 \\message{[vb]}"
        "\\setbox0=\\hbox{$\\left\\delimiter\"026A30C \\vbo"
        "x to8.5pt{}\\right.$}\\showbox0 \\message{[mid]}\\"
        "setbox0=\\hbox{$\\left\\delimiter\"026A30C x\\midd"
        "le\\delimiter\"026A30C y\\right.$}\\showbox0 \\mes"
        "sage{[empty]}\\setbox0=\\hbox{$\\left\\delimiter\""
        "026A30C \\right.$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[many]\n> \\box0=\n\\hbox(7.5+2.5)x27.17679"
        "\n.\\mathon\n.\\hbox(7.5+2.5)x27.17679\n..\\hbox(7"
        ".5+2.5)x2.77779\n...\\tensy j\n..\\teni x\n..\\glu"
        "e(\\medmuskip) 2.22217\n..\\tenrm +\n..\\glue(\\me"
        "dmuskip) 2.22217\n..\\teni y\n..\\kern0.35878\n.."
        "\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\mathoff\n\n!"
        " OK.\nl.1 ...ft\\delimiter\"026A30C x+y\\right.$}"
        "\\showbox0 \n                                     "
        "             \\message{[vb]}\\setbox0=\\hb...\n\n"
        "\n[vb]\n> \\box0=\n\\hbox(8.50006+3.50006)x4.53333"
        "\n.\\mathon\n.\\hbox(8.50006+3.50006)x4.53333\n.."
        "\\vbox(0.0+12.00012)x3.33333, shifted -8.50006\n.."
        ".\\hbox(0.0+6.00006)x3.33333\n....\\tenex ^^L\n..."
        "\\hbox(0.0+6.00006)x3.33333\n....\\tenex ^^L\n..\\"
        "vbox(8.5+0.0)x0.0\n..\\hbox(0.0+0.0)x1.2, shifted "
        "-2.5\n.\\mathoff\n\n! OK.\nl.1 ...\"026A30C \\vbox"
        " to8.5pt{}\\right.$}\\showbox0 \n                 "
        "                                 \\message{[mid]}"
        "\\setbox0=\\h...\n\n\n[mid]\n> \\box0=\n\\hbox(7.5"
        "+2.5)x17.73244\n.\\mathon\n.\\hbox(7.5+2.5)x17.732"
        "44\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j\n..\\t"
        "eni x\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j\n.."
        "\\teni y\n..\\kern0.35878\n..\\hbox(0.0+0.0)x1.2, "
        "shifted -2.5\n.\\mathoff\n\n! OK.\nl.1 ...ddle\\de"
        "limiter\"026A30C y\\right.$}\\showbox0 \n         "
        "                                         \\message"
        "{[empty]}\\setbox0=...\n\n\n[empty]\n> \\box0=\n\\"
        "hbox(7.5+2.5)x3.97778\n.\\mathon\n.\\hbox(7.5+2.5)"
        "x3.97778\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j"
        "\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\mathoff"
        "\n\n! OK.\nl.1 ...\\left\\delimiter\"026A30C \\rig"
        "ht.$}\\showbox0 \n                                "
        "                  \\showbox254\n\n> \\box254=void"
        "\n\n! OK.\nl.1 ...iter\"026A30C \\right.$}\\showbo"
        "x0 \\showbox254\n                                 "
        "                 \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A display wider than the room it has is squeezed into it, glue and all;
   see docs/DECISIONS.md, a-display-squeezed-to-fit. */
static int test_a_display_squeezed_to_fit(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\hsize=100pt \\parinde"
        "nt=0pt \\baselineskip=0pt \\lineskip=0pt \\lineski"
        "plimit=0pt \\parfillskip=0pt \\leftskip=0pt \\righ"
        "tskip=0pt \\abovedisplayskip=3pt \\abovedisplaysho"
        "rtskip=1pt \\belowdisplayskip=4pt \\belowdisplaysh"
        "ortskip=2pt \\predisplaypenalty=10000 \\postdispla"
        "ypenalty=0 \\tolerance=10000 \\pretolerance=-1 \\h"
        "badness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\showboxdepth=2 \\showboxbreadth=100 "
        "\\message{[over]}\\setbox0=\\vbox{\\noindent$$\\hb"
        "ox to 60pt{}\\mskip0mu plus0mu\\hskip20pt minus10p"
        "t\\hbox to 60pt{}$$}\\showbox0 \\message{[fits]}\\"
        "setbox0=\\vbox{\\noindent$$\\hbox to 60pt{}\\hskip"
        "20pt minus40pt\\hbox to 60pt{}$$}\\showbox0 \\mess"
        "age{[num]}\\setbox0=\\vbox{\\noindent$$\\hbox to 6"
        "0pt{}\\hskip20pt minus40pt\\hbox to 60pt{}\\eqno\\"
        "hbox to 10pt{}$$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[over]\n> \\box0=\n\\vbox(3.0+0.0)x100.0\n."
        "\\penalty 10000\n.\\glue(\\abovedisplayshortskip) "
        "1.0\n.\\hbox(0.0+0.0)x100.0, glue set - 1.0, displ"
        "ay\n..\\hbox(0.0+0.0)x60.0\n..\\glue 0.0\n..\\glue"
        " 20.0 minus 10.0\n..\\hbox(0.0+0.0)x60.0\n.\\penal"
        "ty 0\n.\\glue(\\belowdisplayshortskip) 2.0\n\n! OK"
        ".\nl.1 ...p20pt minus10pt\\hbox to 60pt{}$$}\\show"
        "box0 \n                                           "
        "       \\message{[fits]}\\setbox0=\\...\n\n\n[fits"
        "]\n> \\box0=\n\\vbox(3.0+0.0)x100.0\n.\\penalty 10"
        "000\n.\\glue(\\abovedisplayshortskip) 1.0\n.\\hbox"
        "(0.0+0.0)x100.0, glue set - 1.0, display\n..\\hbox"
        "(0.0+0.0)x60.0\n..\\glue 20.0 minus 40.0\n..\\hbox"
        "(0.0+0.0)x60.0\n.\\penalty 0\n.\\glue(\\belowdispl"
        "ayshortskip) 2.0\n\n! OK.\nl.1 ...p20pt minus40pt"
        "\\hbox to 60pt{}$$}\\showbox0 \n                  "
        "                                \\message{[num]}\\"
        "setbox0=\\v...\n\n\n[num]\n> \\box0=\n\\vbox(1.0+0"
        ".0)x100.0\n.\\penalty 10000\n.\\glue(\\abovedispla"
        "yshortskip) 1.0\n.\\hbox(0.0+0.0)x100.0, glue set "
        "- 1.0, display\n..\\hbox(0.0+0.0)x60.0\n..\\glue 2"
        "0.0 minus 40.0\n..\\hbox(0.0+0.0)x60.0\n.\\penalty"
        " 10000\n.\\glue(\\baselineskip) 0.0\n.\\hbox(0.0+0"
        ".0)x10.0, shifted 90.0, display\n..\\hbox(0.0+0.0)"
        "x10.0\n.\\penalty 0\n\n! OK.\nl.1 ... to 60pt{}\\e"
        "qno\\hbox to 10pt{}$$}\\showbox0 \n               "
        "                                   \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...eqno\\hbox to 1"
        "0pt{}$$}\\showbox0 \\showbox254\n                 "
        "                                 \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* What stands to the left of a display is unknowable when the line before it
   is stretching or shrinking on glue of the line's own order; see
   docs/DECISIONS.md, the-size-before-a-display. */
static int test_the_size_before_a_display(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\catcode`\\$=3 \\hsize"
        "=100pt \\parindent=0pt \\baselineskip=0pt \\linesk"
        "ip=0pt \\lineskiplimit=0pt \\parfillskip=0pt \\lef"
        "tskip=0pt \\rightskip=0pt \\abovedisplayskip=3pt "
        "\\abovedisplayshortskip=1pt \\belowdisplayskip=4pt"
        " \\belowdisplayshortskip=2pt \\predisplaypenalty=1"
        "0000 \\postdisplaypenalty=0 \\tolerance=10000 \\pr"
        "etolerance=-1 \\hbadness=10000 \\vbadness=10000 \\"
        "hfuzz=1000pt \\vfuzz=1000pt \\showboxdepth=1 \\sho"
        "wboxbreadth=100 \\message{[rigid]}\\spaceskip=4pt "
        "\\setbox0=\\vbox{\\noindent x x$$\\hbox to 10pt{}$"
        "$}\\showbox0 \\message{[stretch]}\\spaceskip=4pt p"
        "lus 2pt \\setbox0=\\vbox{\\noindent x x$$\\hbox to"
        " 10pt{}$$}\\showbox0 \\message{[shrink]}\\spaceski"
        "p=4pt minus 2pt \\setbox0=\\vbox{\\noindent x x$$"
        "\\hbox to 10pt{}$$}\\showbox0 \\message{[shifted]}"
        "\\spaceskip=4pt \\setbox0=\\vbox{\\parshape=2 30pt"
        " 70pt 0pt 100pt \\noindent x x$$\\hbox to 10pt{}$$"
        "}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[rigid]\n> \\box0=\n\\vbox(7.30554+0.0)x100."
        "0\n.\\hbox(4.30554+0.0)x100.0 []\n.\\penalty 10000"
        "\n.\\glue(\\abovedisplayshortskip) 1.0\n.\\glue(\\"
        "baselineskip) 0.0\n.\\hbox(0.0+0.0)x10.0, shifted "
        "45.0, display []\n.\\penalty 0\n.\\glue(\\belowdis"
        "playshortskip) 2.0\n\n! OK.\nl.1 ...\\noindent x x"
        "$$\\hbox to 10pt{}$$}\\showbox0 \n                "
        "                                  \\message{[stret"
        "ch]}\\spaces...\n\n\n[stretch]\n> \\box0=\n\\vbox("
        "11.30554+0.0)x100.0\n.\\hbox(4.30554+0.0)x100.0, g"
        "lue set 42.7222 []\n.\\penalty 10000\n.\\glue(\\ab"
        "ovedisplayskip) 3.0\n.\\glue(\\baselineskip) 0.0\n"
        ".\\hbox(0.0+0.0)x10.0, shifted 45.0, display []\n."
        "\\penalty 0\n.\\glue(\\belowdisplayskip) 4.0\n\n! "
        "OK.\nl.1 ...\\noindent x x$$\\hbox to 10pt{}$$}\\s"
        "howbox0 \n                                        "
        "          \\message{[shrink]}\\spacesk...\n\n\n[sh"
        "rink]\n> \\box0=\n\\vbox(7.30554+0.0)x100.0\n.\\hb"
        "ox(4.30554+0.0)x100.0 []\n.\\penalty 10000\n.\\glu"
        "e(\\abovedisplayshortskip) 1.0\n.\\glue(\\baseline"
        "skip) 0.0\n.\\hbox(0.0+0.0)x10.0, shifted 45.0, di"
        "splay []\n.\\penalty 0\n.\\glue(\\belowdisplayshor"
        "tskip) 2.0\n\n! OK.\nl.1 ...\\noindent x x$$\\hbox"
        " to 10pt{}$$}\\showbox0 \n                        "
        "                          \\message{[shifted]}\\sp"
        "aces...\n\n\n[shifted]\n> \\box0=\n\\vbox(11.30554"
        "+0.0)x100.0\n.\\hbox(4.30554+0.0)x70.0, shifted 30"
        ".0 []\n.\\penalty 10000\n.\\glue(\\abovedisplayski"
        "p) 3.0\n.\\glue(\\baselineskip) 0.0\n.\\hbox(0.0+0"
        ".0)x10.0, shifted 45.0, display []\n.\\penalty 0\n"
        ".\\glue(\\belowdisplayskip) 4.0\n\n! OK.\nl.1 ..."
        "\\noindent x x$$\\hbox to 10pt{}$$}\\showbox0 \n  "
        "                                                \\"
        "showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ... x$"
        "$\\hbox to 10pt{}$$}\\showbox0 \\showbox254\n     "
        "                                             \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* Whether a display takes the short skips is decided by the offset it is
   really given, equation number and all; see docs/DECISIONS.md,
   a-short-display-skip. */
static int test_a_short_display_skip(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\hsize=100pt \\parinde"
        "nt=0pt \\baselineskip=0pt \\lineskip=0pt \\lineski"
        "plimit=0pt \\parfillskip=0pt \\leftskip=0pt \\righ"
        "tskip=0pt \\abovedisplayskip=3pt \\abovedisplaysho"
        "rtskip=1pt \\belowdisplayskip=4pt \\belowdisplaysh"
        "ortskip=2pt \\predisplaypenalty=10000 \\postdispla"
        "ypenalty=0 \\tolerance=10000 \\pretolerance=-1 \\h"
        "badness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\showboxdepth=1 \\showboxbreadth=100 "
        "\\message{[num]}\\setbox0=\\vbox{\\noindent\\hbox "
        "to 0pt{}$$\\hbox to 50pt{}\\eqno\\hbox to 20pt{}$$"
        "}\\showbox0 \\message{[bare]}\\setbox0=\\vbox{\\no"
        "indent\\hbox to 0pt{}$$\\hbox to 50pt{}$$}\\showbo"
        "x0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[num]\n> \\box0=\n\\vbox(7.0+0.0)x100.0\n.\\"
        "hbox(0.0+0.0)x100.0 []\n.\\penalty 10000\n.\\glue("
        "\\abovedisplayskip) 3.0\n.\\glue(\\baselineskip) 0"
        ".0\n.\\hbox(0.0+0.0)x85.0, shifted 15.0 []\n.\\pen"
        "alty 0\n.\\glue(\\belowdisplayskip) 4.0\n\n! OK.\n"
        "l.1 ... to 50pt{}\\eqno\\hbox to 20pt{}$$}\\showbo"
        "x0 \n                                             "
        "     \\message{[bare]}\\setbox0=\\...\n\n\n[bare]"
        "\n> \\box0=\n\\vbox(3.0+0.0)x100.0\n.\\hbox(0.0+0."
        "0)x100.0 []\n.\\penalty 10000\n.\\glue(\\abovedisp"
        "layshortskip) 1.0\n.\\glue(\\baselineskip) 0.0\n."
        "\\hbox(0.0+0.0)x50.0, shifted 25.0, display []\n."
        "\\penalty 0\n.\\glue(\\belowdisplayshortskip) 2.0"
        "\n\n! OK.\nl.1 ...hbox to 0pt{}$$\\hbox to 50pt{}$"
        "$}\\showbox0 \n                                   "
        "               \\showbox254\n\n> \\box254=void\n\n"
        "! OK.\nl.1 ...{}$$\\hbox to 50pt{}$$}\\showbox0 \\"
        "showbox254\n                                      "
        "            \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A large operator sits on the axis, takes a bigger shape in display style,
   and carries its limits above and below unless told otherwise; see
   docs/DECISIONS.md, large-operators. */
static int test_large_operators(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=10 \\showboxbreadth="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt "
        "\\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\"
        "lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt"
        " plus1fil \\leftskip=0pt \\rightskip=0pt \\toleran"
        "ce=10000 \\pretolerance=-1 \\spaceskip=4pt \\font"
        "\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\fo"
        "nt\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seven"
        "sy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10"
        " \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\scr"
        "iptscriptfont0=\\fiverm \\textfont1=\\teni \\scrip"
        "tfont1=\\seveni \\scriptscriptfont1=\\fivei \\text"
        "font2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3"
        "=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\t"
        "eni=127 \\skewchar\\seveni=127 \\skewchar\\fivei=1"
        "27 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\s"
        "kewchar\\fivesy=48 \\tenrm \\message{[t]}\\setbox0"
        "=\\hbox{$\\mathchar\"1352 $}\\showbox0 \\message{["
        "d]}\\setbox0=\\hbox{$\\displaystyle\\mathchar\"135"
        "2 $}\\showbox0 \\message{[s]}\\setbox0=\\hbox{$\\d"
        "isplaystyle\\mathchar\"1350 $}\\showbox0 \\message"
        "{[sl]}\\setbox0=\\hbox{$\\displaystyle\\mathchar\""
        "1350 ^a_b$}\\showbox0 \\message{[st]}\\setbox0=\\h"
        "box{$\\mathchar\"1350 ^a_b$}\\showbox0 \\message{["
        "nl]}\\setbox0=\\hbox{$\\displaystyle\\mathchar\"13"
        "50 \\nolimits^a_b$}\\showbox0 \\message{[it]}\\set"
        "box0=\\hbox{$\\mathchar\"1352 ^a_b$}\\showbox0 \\m"
        "essage{[id]}\\setbox0=\\hbox{$\\displaystyle\\math"
        "char\"1352 ^a_b$}\\showbox0 \\message{[il]}\\setbo"
        "x0=\\hbox{$\\mathchar\"1352 \\limits^a_b$}\\showbo"
        "x0 \\message{[sup]}\\setbox0=\\hbox{$\\mathchar\"1"
        "352 ^a$}\\showbox0 \\message{[sub]}\\setbox0=\\hbo"
        "x{$\\mathchar\"1352 _b$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[t]\n> \\box0=\n\\hbox(8.0556+3.05562)x6.666"
        "67\n.\\mathon\n.\\hbox(0.0+11.11122)x6.66667, shif"
        "ted -8.0556\n..\\tenex R\n.\\mathoff\n\n! OK.\nl.1"
        " ...\\setbox0=\\hbox{$\\mathchar\"1352 $}\\showbox"
        "0 \n                                              "
        "    \\message{[d]}\\setbox0=\\hbo...\n\n\n[d]\n> "
        "\\box0=\n\\hbox(13.61122+8.61124)x10.00002\n.\\mat"
        "hon\n.\\vbox(13.61122+8.61124)x10.00002\n..\\hbox("
        "13.61122+8.61124)x10.00002\n...\\hbox(0.0+22.22246"
        ")x10.00002, shifted -13.61122\n....\\tenex Z\n.\\m"
        "athoff\n\n! OK.\nl.1 ...x{$\\displaystyle\\mathcha"
        "r\"1352 $}\\showbox0 \n                           "
        "                       \\message{[s]}\\setbox0=\\h"
        "bo...\n\n\n[s]\n> \\box0=\n\\hbox(10.50006+5.50006"
        ")x14.44447\n.\\mathon\n.\\vbox(10.50006+5.50006)x1"
        "4.44447\n..\\hbox(10.50006+5.50006)x14.44447\n..."
        "\\hbox(1.0+15.00012)x14.44447, shifted -9.50006\n."
        "...\\tenex X\n.\\mathoff\n\n! OK.\nl.1 ...x{$\\dis"
        "playstyle\\mathchar\"1350 $}\\showbox0 \n         "
        "                                         \\message"
        "{[sl]}\\setbox0=\\hb...\n\n\n[sl]\n> \\box0=\n\\hb"
        "ox(16.51393+13.02782)x14.44447\n.\\mathon\n.\\vbox"
        "(16.51393+13.02782)x14.44447\n..\\kern1.0\n..\\hbo"
        "x(3.01389+0.0)x14.44447, glue set 5.05342fil\n..."
        "\\glue 0.0 plus 1.0fil minus 1.0fil\n...\\seveni a"
        "\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n..\\kern"
        "1.99998\n..\\hbox(10.50006+5.50006)x14.44447\n..."
        "\\hbox(1.0+15.00012)x14.44447, shifted -9.50006\n."
        "...\\tenex X\n..\\kern1.66666\n..\\hbox(4.8611+0.0"
        ")x14.44447, glue set 5.46391fil\n...\\glue 0.0 plu"
        "s 1.0fil minus 1.0fil\n...\\seveni b\n...\\glue 0."
        "0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\mathof"
        "f\n\n! OK.\nl.1 ...displaystyle\\mathchar\"1350 ^a"
        "_b$}\\showbox0 \n                                 "
        "                 \\message{[st]}\\setbox0=\\hb..."
        "\n\n\n[st]\n> \\box0=\n\\hbox(8.04175+3.00005)x14."
        "89323\n.\\mathon\n.\\hbox(0.0+10.00012)x10.55559, "
        "shifted -7.50006\n..\\tenex P\n.\\vbox(11.0418+0.0"
        ")x4.33765, shifted 3.00005\n..\\hbox(3.01389+0.0)x"
        "4.33765\n...\\seveni a\n..\\kern3.16681\n..\\hbox("
        "4.8611+0.0)x3.51666\n...\\seveni b\n.\\mathoff\n\n"
        "! OK.\nl.1 ...box0=\\hbox{$\\mathchar\"1350 ^a_b$}"
        "\\showbox0 \n                                     "
        "             \\message{[nl]}\\setbox0=\\hb...\n\n"
        "\n[nl]\n> \\box0=\n\\hbox(11.04175+6.00005)x18.782"
        "12\n.\\mathon\n.\\hbox(1.0+15.00012)x14.44447, shi"
        "fted -9.50006\n..\\tenex X\n.\\vbox(17.0418+0.0)x4"
        ".33765, shifted 6.00005\n..\\hbox(3.01389+0.0)x4.3"
        "3765\n...\\seveni a\n..\\kern9.16681\n..\\hbox(4.8"
        "611+0.0)x3.51666\n...\\seveni b\n.\\mathoff\n\n! O"
        "K.\nl.1 ...yle\\mathchar\"1350 \\nolimits^a_b$}\\s"
        "howbox0 \n                                        "
        "          \\message{[it]}\\setbox0=\\hb...\n\n\n[i"
        "t]\n> \\box0=\n\\hbox(8.59729+3.5556)x11.00432\n."
        "\\mathon\n.\\hbox(0.0+11.11122)x4.72223, shifted -"
        "8.0556\n..\\tenex R\n.\\vbox(12.1529+0.0)x6.28209,"
        " shifted 3.5556\n..\\hbox(3.01389+0.0)x4.33765, sh"
        "ifted 1.94444\n...\\seveni a\n..\\kern4.27791\n.."
        "\\hbox(4.861",
        "1+0.0)x3.51666\n...\\seveni b\n.\\mathoff\n\n! OK."
        "\nl.1 ...box0=\\hbox{$\\mathchar\"1352 ^a_b$}\\sho"
        "wbox0 \n                                          "
        "        \\message{[id]}\\setbox0=\\hb...\n\n\n[id]"
        "\n> \\box0=\n\\hbox(19.62509+16.13899)x10.00002\n."
        "\\mathon\n.\\vbox(19.62509+16.13899)x10.00002\n.."
        "\\kern1.0\n..\\hbox(3.01389+0.0)x10.00002, glue se"
        "t 2.83119fil, shifted 2.22223\n...\\glue 0.0 plus "
        "1.0fil minus 1.0fil\n...\\seveni a\n...\\glue 0.0 "
        "plus 1.0fil minus 1.0fil\n..\\kern1.99998\n..\\hbo"
        "x(13.61122+8.61124)x10.00002\n...\\hbox(0.0+22.222"
        "46)x10.00002, shifted -13.61122\n....\\tenex Z\n.."
        "\\kern1.66666\n..\\hbox(4.8611+0.0)x10.00002, glue"
        " set 3.24168fil, shifted -2.22223\n...\\glue 0.0 p"
        "lus 1.0fil minus 1.0fil\n...\\seveni b\n...\\glue "
        "0.0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\math"
        "off\n\n! OK.\nl.1 ...displaystyle\\mathchar\"1352 "
        "^a_b$}\\showbox0 \n                               "
        "                   \\message{[il]}\\setbox0=\\hb.."
        ".\n\n\n[il]\n> \\box0=\n\\hbox(14.06947+10.58337)x"
        "6.66667\n.\\mathon\n.\\vbox(14.06947+10.58337)x6.6"
        "6667\n..\\kern1.0\n..\\hbox(3.01389+0.0)x6.66667, "
        "glue set 1.16452fil, shifted 0.97223\n...\\glue 0."
        "0 plus 1.0fil minus 1.0fil\n...\\seveni a\n...\\gl"
        "ue 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.99998\n"
        "..\\hbox(8.0556+3.05562)x6.66667\n...\\hbox(0.0+11"
        ".11122)x6.66667, shifted -8.0556\n....\\tenex R\n."
        ".\\kern1.66666\n..\\hbox(4.8611+0.0)x6.66667, glue"
        " set 1.57501fil, shifted -0.97223\n...\\glue 0.0 p"
        "lus 1.0fil minus 1.0fil\n...\\seveni b\n...\\glue "
        "0.0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\math"
        "off\n\n! OK.\nl.1 ...box{$\\mathchar\"1352 \\limit"
        "s^a_b$}\\showbox0 \n                              "
        "                    \\message{[sup]}\\setbox0=\\h."
        "..\n\n\n[sup]\n> \\box0=\n\\hbox(8.59729+3.05562)x"
        "11.00432\n.\\mathon\n.\\hbox(0.0+11.11122)x6.66667"
        ", shifted -8.0556\n..\\tenex R\n.\\hbox(3.01389+0."
        "0)x4.33765, shifted -5.5834\n..\\seveni a\n.\\math"
        "off\n\n! OK.\nl.1 ...etbox0=\\hbox{$\\mathchar\"13"
        "52 ^a$}\\showbox0 \n                              "
        "                    \\message{[sub]}\\setbox0=\\h."
        "..\n\n\n[sub]\n> \\box0=\n\\hbox(8.0556+3.5556)x8."
        "23889\n.\\mathon\n.\\hbox(0.0+11.11122)x4.72223, s"
        "hifted -8.0556\n..\\tenex R\n.\\hbox(4.8611+0.0)x3"
        ".51666, shifted 3.5556\n..\\seveni b\n.\\mathoff\n"
        "\n! OK.\nl.1 ...etbox0=\\hbox{$\\mathchar\"1352 _b"
        "$}\\showbox0 \n                                   "
        "               \\showbox254\n\n> \\box254=void\n\n"
        "! OK.\nl.1 ...x{$\\mathchar\"1352 _b$}\\showbox0 "
        "\\showbox254\n                                    "
        "              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* The paragraph's own first line lets its first character stick out past the
   margin, and that is what the breaker measures; see docs/DECISIONS.md,
   the-first-line-protrudes-too. */
static int test_the_first_line_protrudes_too(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\pdfprotrudechars=2 \\lpcode\\tenrm"
        "`\\O=1000 \\hsize=140pt \\parindent=0pt \\baseline"
        "skip=12pt \\lineskip=0pt \\lineskiplimit=0pt \\par"
        "fillskip=0pt plus1fil \\leftskip=0pt \\rightskip=0"
        "pt \\pretolerance=1000 \\tolerance=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\spaceskip=4pt plus 2pt minus 1pt \\uchyph=0 "
        "\\tracingonline=1 \\tracingparagraphs=1 \\setbox0="
        "\\vbox{\\noindent Onetwo three four five six seven"
        " eight nine ten\\par} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n@firstpass\n\\tenrm Onetwo three four five s"
        "ix seven \n@ via @@0 b=27 p=0 d=729\n@@1: line 1.1"
        " t=729 -> @@0\neight nine ten \n@\\par via @@1 b=0"
        " p=-10000 d=0\n@@2: line 2.2- t=729 -> @@1\n\n> \\"
        "box254=void\n\n! OK.\nl.1 ...e six seven eight nin"
        "e ten\\par} \\showbox254\n                        "
        "                          \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* \tracingparagraphs writes the passes out as the reference does; see
   docs/DECISIONS.md, tracing-paragraphs. */
static int test_tracing_paragraphs(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\lccode`\\a=`\\a \\lccode`\\b=`\\b "
        "\\lccode`\\c=`\\c \\lccode`\\d=`\\d \\lccode`\\e=`"
        "\\e \\lccode`\\f=`\\f \\lccode`\\g=`\\g \\lccode`"
        "\\h=`\\h \\lccode`\\i=`\\i \\lccode`\\j=`\\j \\lcc"
        "ode`\\k=`\\k \\lccode`\\l=`\\l \\lccode`\\m=`\\m "
        "\\lccode`\\n=`\\n \\lccode`\\o=`\\o \\lccode`\\p=`"
        "\\p \\lccode`\\q=`\\q \\lccode`\\r=`\\r \\lccode`"
        "\\s=`\\s \\lccode`\\t=`\\t \\lccode`\\u=`\\u \\lcc"
        "ode`\\v=`\\v \\lccode`\\w=`\\w \\lccode`\\x=`\\x "
        "\\lccode`\\y=`\\y \\lccode`\\z=`\\z \\patterns{a1b"
        "ra ca1da b1ra} \\hyphenchar\\tenrm=45 \\hsize=100p"
        "t \\parindent=0pt \\baselineskip=12pt \\lineskip=0"
        "pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fil "
        "\\leftskip=0pt \\rightskip=0pt \\pretolerance=100 "
        "\\tolerance=200 \\hbadness=10000 \\vbadness=10000 "
        "\\hfuzz=1000pt \\vfuzz=1000pt \\linepenalty=10 \\a"
        "djdemerits=10000 \\doublehyphendemerits=10000 \\fi"
        "nalhyphendemerits=5000 \\hyphenpenalty=50 \\exhyph"
        "enpenalty=50 \\uchyph=0 \\lefthyphenmin=2 \\righth"
        "yphenmin=3 \\spaceskip=4pt plus 2pt minus 1pt \\tr"
        "acingonline=1 \\tracingparagraphs=1 \\setbox0=\\vb"
        "ox{\\noindent one two three four abracadabra five "
        "six seven eight\\par} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n@firstpass\n@secondpass\n\\tenrm one two thr"
        "ee four ab-\n@\\discretionary via @@0 b=0 p=50 d=2"
        "600\n@@1: line 1.2- t=2600 -> @@0\nraca-da-bra fiv"
        "e six seven \n@ via @@1 b=* p=0 d=*\n@@2: line 2.3"
        " t=2600 -> @@1\neight \n@\\par via @@2 b=0 p=-1000"
        "0 d=*\n@@3: line 3.2- t=2600 -> @@2\n\n> \\box254="
        "void\n\n! OK.\nl.1 ...dabra five six seven eight\\"
        "par} \\showbox254\n                               "
        "                   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A pdf destination is a whatsit in the list, written out the way the
   reference writes it; see docs/DECISIONS.md, pdf-destinations. */
static int test_pdf_destinations(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\pdfoutput=1 \\message{[a]}\\setbox"
        "0=\\hbox{a\\pdfdest name{x} xyz b}\\showbox0 \\mes"
        "sage{[b]}\\setbox0=\\hbox{\\pdfdest num7 xyz zoom "
        "2000 }\\showbox0 \\message{[c]}\\setbox0=\\hbox{\\"
        "pdfdest name{y} fitbv}\\showbox0 \\message{[d]}\\s"
        "etbox0=\\hbox{\\pdfdest name{z} fitr width 10pt he"
        "ight 3pt depth 1pt}\\showbox0 \\message{[e]}\\setb"
        "ox0=\\hbox{\\pdfdest name{w} fitr height 3pt}\\sho"
        "wbox0 \\message{[f]}\\setbox0=\\hbox{\\pdfdest nam"
        "e{aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeee"
        "eeffffffffffgggggggggghhhhhhhhhh} fit}\\showbox0 "
        "\\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[a]\n> \\box0=\n\\hbox(6.94444+0.0)x10.55559"
        "\n.\\tenrm a\n.\\pdfdest name{x} xyz\n.\\tenrm b\n"
        "\n! OK.\nl.1 ...x0=\\hbox{a\\pdfdest name{x} xyz b"
        "}\\showbox0 \n                                    "
        "              \\message{[b]}\\setbox0=\\hbo...\n\n"
        "\n[b]\n> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\pdfdest "
        "num7 xyz zoom2000\n\n! OK.\nl.1 ...box{\\pdfdest n"
        "um7 xyz zoom 2000 }\\showbox0 \n                  "
        "                                \\message{[c]}\\se"
        "tbox0=\\hbo...\n\n\n[c]\n> \\box0=\n\\hbox(0.0+0.0"
        ")x0.0\n.\\pdfdest name{y} fitbv\n\n! OK.\nl.1 ...o"
        "x0=\\hbox{\\pdfdest name{y} fitbv}\\showbox0 \n   "
        "                                               \\m"
        "essage{[d]}\\setbox0=\\hbo...\n\n\n[d]\n> \\box0="
        "\n\\hbox(0.0+0.0)x0.0\n.\\pdfdest name{z} fitr(3.0"
        "+1.0)x10.0\n\n! OK.\nl.1 ... width 10pt height 3pt"
        " depth 1pt}\\showbox0 \n                          "
        "                        \\message{[e]}\\setbox0=\\"
        "hbo...\n\n\n[e]\n> \\box0=\n\\hbox(0.0+0.0)x0.0\n."
        "\\pdfdest name{w} fitr(3.0+*)x*\n\n! OK.\nl.1 ..."
        "\\pdfdest name{w} fitr height 3pt}\\showbox0 \n   "
        "                                               \\m"
        "essage{[f]}\\setbox0=\\hbo...\n\n\n[f]\n> \\box0="
        "\n\\hbox(0.0+0.0)x0.0\n.\\pdfdest name{aaaaaaaaaab"
        "bbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffg"
        "ggg\nggggg\\ETC.} fit\n\n! OK.\nl.1 ...fffffffgggg"
        "gggggghhhhhhhhhh} fit}\\showbox0 \n               "
        "                                   \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...gggggghhhhhhhhh"
        "h} fit}\\showbox0 \\showbox254\n                  "
        "                                \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Once the page builder has emptied the contribution list, \lastnodetype
   and its relatives report the last node it took; see docs/DECISIONS.md,
   the-last-node-of-a-page. */
static int test_the_last_node_of_a_page(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\vsize=200pt \\max"
        "depth=4pt \\output={\\shipout\\box255 }\\message{["
        "A=\\the\\lastnodetype]}\\hbox{}\\message{[B=\\the"
        "\\lastnodetype]}\\vskip3pt \\message{[C=\\the\\las"
        "tnodetype][Cs=\\the\\lastskip]}\\penalty5 \\messag"
        "e{[D=\\the\\lastnodetype][Dp=\\the\\lastpenalty]}"
        "\\kern2pt \\message{[E=\\the\\lastnodetype][Ek=\\t"
        "he\\lastkern]}\\hbox{}\\message{[F=\\the\\lastnode"
        "type][Fs=\\the\\lastskip][Fk=\\the\\lastkern][Fp="
        "\\the\\lastpenalty]}\\setbox0=\\vbox{\\message{[G="
        "\\the\\lastnodetype]}\\hbox{}\\message{[H=\\the\\l"
        "astnodetype]}} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\vs"
        "ize=200pt \\maxdepth=4pt \\output={\\shipou...\n\n"
        "\n[A=-1] [B=1] [C=11][Cs=3.0pt] [D=13][Dp=5] [E=12"
        "][Ek=2.0pt]\n[F=1][Fs=0.0pt][Fk=0.0pt][Fp=0] [G=-1"
        "] [H=1]\n> \\box254=void\n\n! OK.\nl.1 ...message{"
        "[H=\\the\\lastnodetype]}} \\showbox254\n          "
        "                                        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A line that breaks at a penalty keeps the penalty; see docs/DECISIONS.md,
   a-line-that-breaks-at-a-penalty. */
static int test_a_line_that_breaks_at_a_penalty(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\message{[forced]}\\setbox0=\\vbox{"
        "\\noindent aa\\penalty-10000 bb\\par}\\showbox0 \\"
        "message{[chosen]}\\setbox0=\\vbox{\\hsize=30pt \\n"
        "oindent aaaa\\penalty0 bbbb\\par}\\showbox0 \\mess"
        "age{[after]}\\setbox0=\\vbox{\\hsize=30pt \\noinde"
        "nt aaaa\\penalty0 \\hskip4pt bbbb\\par}\\showbox0 "
        "\\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[forced]\n> \\box0=\n\\vbox(16.30554+0.0)x20"
        "0.0\n.\\hbox(4.30554+0.0)x200.0\n..\\tenrm a\n..\\"
        "tenrm a\n..\\penalty -10000\n..\\glue(\\rightskip)"
        " 0.0\n.\\glue(\\baselineskip) 5.05556\n.\\hbox(6.9"
        "4444+0.0)x200.0, glue set 188.88885fil\n..\\tenrm "
        "b\n..\\tenrm b\n..\\penalty 10000\n..\\glue(\\parf"
        "illskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0."
        "0\n\n! OK.\nl.1 ...noindent aa\\penalty-10000 bb\\"
        "par}\\showbox0 \n                                 "
        "                 \\message{[chosen]}\\setbox0...\n"
        "\n\n[chosen]\n> \\box0=\n\\vbox(16.30554+0.0)x30.0"
        "\n.\\hbox(4.30554+0.0)x30.0\n..\\tenrm a\n..\\tenr"
        "m a\n..\\tenrm a\n..\\tenrm a\n..\\penalty 0\n..\\"
        "glue(\\rightskip) 0.0\n.\\glue(\\baselineskip) 5.0"
        "5556\n.\\hbox(6.94444+0.0)x30.0, glue set 7.77771f"
        "il\n..\\tenrm b\n..\\tenrm b\n..\\tenrm b\n..\\ten"
        "rm b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0"
        ".0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\nl.1 ...\\noindent aaaa\\penalty0 bbbb\\par}\\sho"
        "wbox0 \n                                          "
        "        \\message{[after]}\\setbox0=...\n\n\n[afte"
        "r]\n> \\box0=\n\\vbox(16.30554+0.0)x30.0\n.\\hbox("
        "4.30554+0.0)x30.0\n..\\tenrm a\n..\\tenrm a\n..\\t"
        "enrm a\n..\\tenrm a\n..\\penalty 0\n..\\glue(\\rig"
        "htskip) 0.0\n.\\glue(\\baselineskip) 5.05556\n.\\h"
        "box(6.94444+0.0)x30.0, glue set 7.77771fil\n..\\te"
        "nrm b\n..\\tenrm b\n..\\tenrm b\n..\\tenrm b\n..\\"
        "penalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1."
        "0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...a"
        "aaa\\penalty0 \\hskip4pt bbbb\\par}\\showbox0 \n  "
        "                                                \\"
        "showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...y0 "
        "\\hskip4pt bbbb\\par}\\showbox0 \\showbox254\n    "
        "                                              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A sub-formula that comes to one unshifted box is that box; anything else is
   packaged; see docs/DECISIONS.md, a-list-that-is-one-box. */
static int test_a_list_that_is_one_box(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\message{[one]}\\setbox0=\\hbox{$U{"
        "\\hbox{a}}U$}\\showbox0 \\message{[shift]}\\setbox"
        "0=\\hbox{$U{\\mathop{x}}U$}\\showbox0 \\message{[p"
        "air]}\\setbox0=\\hbox{$U{\\hbox{a}\\hbox{b}}U$}\\s"
        "howbox0 \\message{[char]}\\setbox0=\\hbox{$U{a}U$}"
        "\\showbox0 \\message{[sup]}\\setbox0=\\hbox{$U^{\\"
        "hbox{a}}$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[one]\n> \\box0=\n\\hbox(6.83331+0.0)x20.836"
        "07\n.\\mathon\n.\\teni U\n.\\kern1.09026\n.\\hbox("
        "4.30554+0.0)x5.00002\n..\\tenrm a\n.\\teni U\n.\\k"
        "ern1.09026\n.\\mathoff\n\n! OK.\nl.1 ...e]}\\setbo"
        "x0=\\hbox{$U{\\hbox{a}}U$}\\showbox0 \n           "
        "                                       \\message{["
        "shift]}\\setbox0=...\n\n\n[shift]\n> \\box0=\n\\hb"
        "ox(6.83331+0.0)x21.55133\n.\\mathon\n.\\teni U\n."
        "\\kern1.09026\n.\\hbox(4.65277+0.0)x5.71527\n..\\h"
        "box(4.30554+0.0)x5.71527, shifted -0.34723\n...\\t"
        "eni x\n.\\teni U\n.\\kern1.09026\n.\\mathoff\n\n! "
        "OK.\nl.1 ...}\\setbox0=\\hbox{$U{\\mathop{x}}U$}\\"
        "showbox0 \n                                       "
        "           \\message{[pair]}\\setbox0=\\...\n\n\n["
        "pair]\n> \\box0=\n\\hbox(6.94444+0.0)x26.39165\n."
        "\\mathon\n.\\teni U\n.\\kern1.09026\n.\\hbox(6.944"
        "44+0.0)x10.55559\n..\\hbox(4.30554+0.0)x5.00002\n."
        "..\\tenrm a\n..\\hbox(6.94444+0.0)x5.55557\n...\\t"
        "enrm b\n.\\teni U\n.\\kern1.09026\n.\\mathoff\n\n!"
        " OK.\nl.1 ...ox0=\\hbox{$U{\\hbox{a}\\hbox{b}}U$}"
        "\\showbox0 \n                                     "
        "             \\message{[char]}\\setbox0=\\...\n\n"
        "\n[char]\n> \\box0=\n\\hbox(6.83331+0.0)x21.12195"
        "\n.\\mathon\n.\\teni U\n.\\kern1.09026\n.\\teni a"
        "\n.\\teni U\n.\\kern1.09026\n.\\mathoff\n\n! OK.\n"
        "l.1 ...ge{[char]}\\setbox0=\\hbox{$U{a}U$}\\showbo"
        "x0 \n                                             "
        "     \\message{[sup]}\\setbox0=\\h...\n\n\n[sup]\n"
        "> \\box0=\n\\hbox(7.93446+0.0)x12.91805\n.\\mathon"
        "\n.\\teni U\n.\\kern1.09026\n.\\hbox(4.30554+0.0)x"
        "5.00002, shifted -3.62892\n..\\tenrm a\n.\\mathoff"
        "\n\n! OK.\nl.1 ...p]}\\setbox0=\\hbox{$U^{\\hbox{a"
        "}}$}\\showbox0 \n                                 "
        "                 \\showbox254\n\n> \\box254=void\n"
        "\n! OK.\nl.1 ...=\\hbox{$U^{\\hbox{a}}$}\\showbox0"
        " \\showbox254\n                                   "
        "               \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \mathaccent centres a character over its nucleus, sliding it right by the
   nucleus's skew and dropping it onto the letter; see docs/DECISIONS.md,
   math-accents. */
static int test_math_accents(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\tracingonline=1 "
        "\\showboxdepth=10 \\showboxbreadth=1000 \\hbadness"
        "=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=100"
        "0pt \\hsize=200pt \\parindent=0pt \\boxmaxdepth=16"
        "383.99998pt \\baselineskip=12pt \\lineskip=0pt \\l"
        "ineskiplimit=0pt \\parfillskip=0pt plus1fil \\left"
        "skip=0pt \\rightskip=0pt \\tolerance=10000 \\preto"
        "lerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\f"
        "ont\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni"
        "=cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 "
        "\\font\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font"
        "\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0=\\"
        "tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0="
        "\\fiverm \\textfont1=\\teni \\scriptfont1=\\seveni"
        " \\scriptscriptfont1=\\fivei \\textfont2=\\tensy "
        "\\scriptfont2=\\sevensy \\scriptscriptfont2=\\five"
        "sy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scr"
        "iptscriptfont3=\\tenex \\skewchar\\teni=127 \\skew"
        "char\\seveni=127 \\skewchar\\fivei=127 \\skewchar"
        "\\tensy=48 \\skewchar\\sevensy=48 \\skewchar\\five"
        "sy=48 \\tenrm \\message{[bar]}\\setbox0=\\hbox{$\\"
        "mathaccent\"7016 U$}\\showbox0 \\message{[hat]}\\s"
        "etbox0=\\hbox{$\\mathaccent\"705E A$}\\showbox0 \\"
        "message{[wide]}\\setbox0=\\hbox{$\\mathaccent\"701"
        "6 {UU}$}\\showbox0 \\message{[low]}\\setbox0=\\hbo"
        "x{$\\mathaccent\"7016 x$}\\showbox0 \\message{[scr"
        "ipt]}\\setbox0=\\hbox{$U^{\\mathaccent\"7016 y}$}"
        "\\showbox0 \\message{[over]}\\setbox0=\\hbox{$\\ov"
        "erline{U}$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[bar]\n> \\box0=\n\\hbox(8.20554+0.0)x7.9180"
        "3\n.\\mathon\n.\\vbox(8.20554+0.0)x7.91803\n..\\hb"
        "ox(5.67776+0.0)x0.0, shifted 1.7368\n...\\tenrm ^^"
        "V\n..\\kern-4.30554\n..\\hbox(6.83331+0.0)x7.91803"
        "\n...\\teni U\n.\\mathoff\n\n! OK.\nl.1 ...tbox0="
        "\\hbox{$\\mathaccent\"7016 U$}\\showbox0 \n       "
        "                                           \\messa"
        "ge{[hat]}\\setbox0=\\h...\n\n\n[hat]\n> \\box0=\n"
        "\\hbox(9.47221+0.0)x7.50002\n.\\mathon\n.\\vbox(9."
        "47221+0.0)x7.50002\n..\\hbox(6.94444+0.0)x0.0, shi"
        "fted 2.63893\n...\\tenrm ^\n..\\kern-4.30554\n..\\"
        "hbox(6.83331+0.0)x7.50002\n...\\teni A\n.\\mathoff"
        "\n\n! OK.\nl.1 ...tbox0=\\hbox{$\\mathaccent\"705E"
        " A$}\\showbox0 \n                                 "
        "                 \\message{[wide]}\\setbox0=\\..."
        "\n\n\n[wide]\n> \\box0=\n\\hbox(8.20554+0.0)x15.83"
        "606\n.\\mathon\n.\\vbox(8.20554+0.0)x15.83606\n.."
        "\\hbox(5.67776+0.0)x0.0, shifted 5.41803\n...\\ten"
        "rm ^^V\n..\\kern-4.30554\n..\\hbox(6.83331+0.0)x15"
        ".83606\n...\\teni U\n...\\kern1.09026\n...\\teni U"
        "\n...\\kern1.09026\n.\\mathoff\n\n! OK.\nl.1 ...x0"
        "=\\hbox{$\\mathaccent\"7016 {UU}$}\\showbox0 \n   "
        "                                               \\m"
        "essage{[low]}\\setbox0=\\h...\n\n\n[low]\n> \\box0"
        "=\n\\hbox(5.67776+0.0)x5.71527\n.\\mathon\n.\\vbox"
        "(5.67776+0.0)x5.71527\n..\\hbox(5.67776+0.0)x0.0, "
        "shifted 0.63542\n...\\tenrm ^^V\n..\\kern-4.30554"
        "\n..\\hbox(4.30554+0.0)x5.71527\n...\\teni x\n.\\m"
        "athoff\n\n! OK.\nl.1 ...tbox0=\\hbox{$\\mathaccent"
        "\"7016 x$}\\showbox0 \n                           "
        "                       \\message{[script]}\\setbox"
        "0...\n\n\n[script]\n> \\box0=\n\\hbox(7.64835+0.0)"
        "x12.22478\n.\\mathon\n.\\teni U\n.\\kern1.09026\n."
        "\\vbox(4.01942+1.3611)x4.30675, shifted -3.62892\n"
        "..\\hbox(4.01942+0.0)x0.0, shifted 0.59087\n...\\s"
        "evenrm ^^V\n..\\kern-3.01389\n..\\hbox(3.01389+1.3"
        "611)x4.30675\n...\\seveni y\n.\\mathoff\n\n! OK.\n"
        "l.1 ...0=\\hbox{$U^{\\mathaccent\"7016 y}$}\\showb"
        "ox0 \n                                            "
        "      \\message{[over]}\\setbox0=\\...\n\n\n[over]"
        "\n> \\box0=\n\\hbox(8.8332+0.0)x7.91803\n.\\mathon"
        "\n.\\vbox(8.8332+0.0)x7.91803\n..\\kern0.39998\n.."
        "\\rule(0.39998+0.0)x*\n..\\kern1.19994\n..\\hbox(6"
        ".83331+0.0)x7.91803\n...\\teni U\n.\\mathoff\n\n! "
        "OK.\nl.1 ...r]}\\setbox0=\\hbox{$\\overline{U}$}\\"
        "showbox0 \n                                       "
        "           \\showbox254\n\n> \\box254=void\n\n! OK"
        ".\nl.1 ...=\\hbox{$\\overline{U}$}\\showbox0 \\sho"
        "wbox254\n                                         "
        "         \n\n",
        NULL,
    };
    return run_document_parts(source, expected);}

/* A large operator is centred on the axis only when it is one character of
   its own; \\log and its like stand on the baseline. An equation and its
   number are each marked as a display. See docs/DECISIONS.md,
   only-a-character-is-centred. */
static int test_only_a_character_is_centred(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\tracingonline=1 \\showboxdepth=3 "
        "\\showboxbreadth=60 \\hbadness=10000 \\vbadness=10"
        "000 \\hfuzz=1000pt \\vfuzz=1000pt \\hsize=200pt \\"
        "parindent=0pt \\boxmaxdepth=16383.99998pt \\baseli"
        "neskip=12pt \\lineskip=0pt \\lineskiplimit=0pt \\p"
        "arfillskip=0pt plus1fil \\leftskip=0pt \\rightskip"
        "=0pt \\tolerance=10000 \\pretolerance=-1 \\spacesk"
        "ip=4pt \\font\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\"
        "font\\fiverm=cmr5 \\font\\teni=cmmi10 \\font\\seve"
        "ni=cmmi7 \\font\\fivei=cmmi5 \\font\\tensy=cmsy10 "
        "\\font\\sevensy=cmsy7 \\font\\fivesy=cmsy5 \\font"
        "\\tenex=cmex10 \\textfont0=\\tenrm \\scriptfont0="
        "\\sevenrm \\scriptscriptfont0=\\fiverm \\textfont1"
        "=\\teni \\scriptfont1=\\seveni \\scriptscriptfont1"
        "=\\fivei \\textfont2=\\tensy \\scriptfont2=\\seven"
        "sy \\scriptscriptfont2=\\fivesy \\textfont3=\\tene"
        "x \\scriptfont3=\\tenex \\scriptscriptfont3=\\tene"
        "x \\tenrm \\mathcode`\\+=\"202B \\thinmuskip=3mu "
        "\\medmuskip=4mu plus 2mu minus 4mu \\thickmuskip=5"
        "mu plus 5mu \\abovedisplayskip=10pt plus2pt \\belo"
        "wdisplayskip=11pt plus2pt \\abovedisplayshortskip="
        "1pt plus3pt \\belowdisplayshortskip=2pt plus3pt \\"
        "predisplaypenalty=10000 \\postdisplaypenalty=0 \\w"
        "idowpenalty=150 \\displaywidowpenalty=50 \\clubpen"
        "alty=0 \\interlinepenalty=0 \\message{[op]}\\setbo"
        "x0=\\hbox{$\\mathop{\\tenrm log}$}\\showbox0 \\mes"
        "sage{[ch]}\\setbox0=\\hbox{$\\mathop{x}$}\\showbox"
        "0 \\message{[eqno]}\\setbox0=\\vbox{\\noindent aa "
        "$$x+y\\eqno z$$ bb\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\tracingonline...\n"
        "\n\n[op]\n> \\box0=\n\\hbox(6.94444+1.94444)x13.15"
        "627\n.\\mathon\n.\\hbox(6.94444+1.94444)x13.15627"
        "\n..\\teni l\n..\\kern0.19678\n..\\teni o\n..\\ten"
        "i g\n..\\kern0.35878\n.\\mathoff\n\n! OK.\nl.1 ..."
        "box0=\\hbox{$\\mathop{\\tenrm log}$}\\showbox0 \n "
        "                                                 "
        "\\message{[ch]}\\setbox0=\\hb...\n\n\n[ch]\n> \\bo"
        "x0=\n\\hbox(4.65277+0.0)x5.71527\n.\\mathon\n.\\hb"
        "ox(4.30554+0.0)x5.71527, shifted -0.34723\n..\\ten"
        "i x\n.\\mathoff\n\n! OK.\nl.1 ...[ch]}\\setbox0=\\"
        "hbox{$\\mathop{x}$}\\showbox0 \n                  "
        "                                \\message{[eqno]}"
        "\\setbox0=\\...\n\n\n[eqno]\n> \\box0=\n\\vbox(31."
        "30554+0.0)x200.0\n.\\hbox(4.30554+0.0)x200.0, glue"
        " set 189.99997fil\n..\\tenrm a\n..\\tenrm a\n..\\p"
        "enalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0"
        "fil\n..\\glue(\\rightskip) 0.0\n.\\penalty 10000\n"
        ".\\glue(\\abovedisplayshortskip) 1.0 plus 3.0\n.\\"
        "glue(\\baselineskip) 6.16667\n.\\hbox(5.83333+1.94"
        "444)x111.5995, shifted 88.4005\n..\\hbox(5.83333+1"
        ".94444)x23.199, display\n...\\teni x\n...\\glue(\\"
        "medmuskip) 2.22217 plus 1.11108 minus 2.22217\n..."
        "\\tenrm +\n...\\glue(\\medmuskip) 2.22217 plus 1.1"
        "1108 minus 2.22217\n...\\teni y\n...\\kern0.35878"
        "\n..\\kern83.3102\n..\\hbox(4.30554+0.0)x5.0903, d"
        "isplay\n...\\teni z\n...\\kern0.4398\n.\\penalty 0"
        "\n.\\glue(\\belowdisplayshortskip) 2.0 plus 3.0\n."
        "\\glue(\\baselineskip) 3.11111\n.\\hbox(6.94444+0."
        "0)x200.0, glue set 188.88885fil\n..\\tenrm b\n..\\"
        "tenrm b\n..\\penalty 10000\n..\\glue(\\parfillskip"
        ") 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! "
        "OK.\nl.1 ...oindent aa $$x+y\\eqno z$$ bb\\par}\\s"
        "howbox0 \n                                        "
        "          \\showbox254\n\n> \\box254=void\n\n! OK."
        "\nl.1 ...$$x+y\\eqno z$$ bb\\par}\\showbox0 \\show"
        "box254\n                                          "
        "        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A superscript is lifted to the first of the three parameters in display
   style, the second elsewhere, and the third in a cramped one; see
   docs/DECISIONS.md, superscripts-in-display-style. */
static int test_superscripts_in_display_style(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\$=3 \\c"
        "atcode`\\\"=12 \\catcode`\\^=7 \\catcode`\\_=8 \\t"
        "racingonline=1 \\showboxdepth=3 \\showboxbreadth=6"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt"
        " \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt \\bo"
        "xmaxdepth=16383.99998pt \\baselineskip=12pt \\line"
        "skip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plu"
        "s1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=1"
        "0000 \\pretolerance=-1 \\spaceskip=4pt \\font\\ten"
        "rm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 "
        "\\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\font\\f"
        "ivei=cmmi5 \\font\\tensy=cmsy10 \\font\\sevensy=cm"
        "sy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex10 \\te"
        "xtfont0=\\tenrm \\scriptfont0=\\sevenrm \\scriptsc"
        "riptfont0=\\fiverm \\textfont1=\\teni \\scriptfont"
        "1=\\seveni \\scriptscriptfont1=\\fivei \\textfont2"
        "=\\tensy \\scriptfont2=\\sevensy \\scriptscriptfon"
        "t2=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\te"
        "nex \\scriptscriptfont3=\\tenex \\tenrm \\mathcode"
        "`\\+=\"202B \\thinmuskip=3mu \\medmuskip=4mu plus "
        "2mu minus 4mu \\thickmuskip=5mu plus 5mu \\abovedi"
        "splayskip=10pt plus2pt \\belowdisplayskip=11pt plu"
        "s2pt \\abovedisplayshortskip=1pt plus3pt \\belowdi"
        "splayshortskip=2pt plus3pt \\predisplaypenalty=100"
        "00 \\postdisplaypenalty=0 \\widowpenalty=150 \\dis"
        "playwidowpenalty=50 \\clubpenalty=0 \\interlinepen"
        "alty=0 \\message{[t]}\\setbox0=\\hbox{$x_a^b$}\\sh"
        "owbox0 \\message{[d]}\\setbox0=\\hbox{$\\displayst"
        "yle x_a^b$}\\showbox0 \\message{[c]}\\setbox0=\\hb"
        "ox{$x_a^{b^c}$}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 ..."
        "\n\n\n[t]\n> \\box0=\n\\hbox(8.49002+2.47217)x10.0"
        "5292\n.\\mathon\n.\\teni x\n.\\vbox(10.96219+0.0)x"
        "4.33765, shifted 2.47217\n..\\hbox(4.8611+0.0)x3.5"
        "1666\n...\\seveni b\n..\\kern3.0872\n..\\hbox(3.01"
        "389+0.0)x4.33765\n...\\seveni a\n.\\mathoff\n\n! O"
        "K.\nl.1 ...ssage{[t]}\\setbox0=\\hbox{$x_a^b$}\\sh"
        "owbox0 \n                                         "
        "         \\message{[d]}\\setbox0=\\hbo...\n\n\n[d]"
        "\n> \\box0=\n\\hbox(8.99002+2.47217)x10.05292\n.\\"
        "mathon\n.\\teni x\n.\\vbox(11.46219+0.0)x4.33765, "
        "shifted 2.47217\n..\\hbox(4.8611+0.0)x3.51666\n..."
        "\\seveni b\n..\\kern3.5872\n..\\hbox(3.01389+0.0)x"
        "4.33765\n...\\seveni a\n.\\mathoff\n\n! OK.\nl.1 ."
        "..box0=\\hbox{$\\displaystyle x_a^b$}\\showbox0 \n"
        "                                                  "
        "\\message{[c]}\\setbox0=\\hbo...\n\n\n[c]\n> \\box"
        "0=\n\\hbox(8.79948+2.47217)x12.47906\n.\\mathon\n."
        "\\teni x\n.\\vbox(11.27165+0.0)x6.7638, shifted 2."
        "47217\n..\\hbox(5.17056+0.0)x6.7638\n...\\seveni b"
        "\n...\\hbox(2.15277+0.0)x3.24713, shifted -3.01779"
        " []\n..\\kern3.0872\n..\\hbox(3.01389+0.0)x4.33765"
        "\n...\\seveni a\n.\\mathoff\n\n! OK.\nl.1 ...e{[c]"
        "}\\setbox0=\\hbox{$x_a^{b^c}$}\\showbox0 \n       "
        "                                           \\showb"
        "ox254\n\n> \\box254=void\n\n! OK.\nl.1 ...ox0=\\hb"
        "ox{$x_a^{b^c}$}\\showbox0 \\showbox254\n          "
        "                                        \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The search for the character that may stick out past a margin looks into
   a horizontal box and not into a vertical one; see docs/DECISIONS.md,
   character-protrusion. */
static int test_protrusion_into_boxes(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\{=1 \\c"
        "atcode`\\}=2 \\tracingonline=1 \\showboxdepth=2 \\"
        "showboxbreadth=40 \\hbadness=10000 \\vbadness=1000"
        "0 \\hfuzz=1000pt \\vfuzz=1000pt \\font\\f=cmr10 \\"
        "f \\hsize=100pt \\parindent=0pt \\baselineskip=12p"
        "t \\lineskip=0pt \\lineskiplimit=0pt \\parfillskip"
        "=0pt plus1fil \\leftskip=0pt \\rightskip=0pt \\tol"
        "erance=10000 \\pretolerance=-1 \\boxmaxdepth=16383"
        ".99998pt \\spaceskip=4pt \\lpcode\\f`\\(=117 \\rpc"
        "ode\\f`\\)=117 \\lpcode\\f`\\A=200 \\rpcode\\f`\\B"
        "=300 \\pdfprotrudechars=2 \\message{[box]}\\setbox"
        "1=\\vbox{\\noindent\\hbox{(x)} aaa\\par}\\showbox1"
        " \\message{[deep]}\\setbox1=\\vbox{\\noindent\\hbo"
        "x{\\hbox{A}x} aaa\\par}\\showbox1 \\message{[vbox]"
        "}\\setbox1=\\vbox{\\noindent\\vbox{\\hbox{A}}x aaa"
        "\\par}\\showbox1 \\message{[end]}\\setbox1=\\vbox{"
        "\\noindent aaa \\hbox{(B)}\\par}\\showbox1 \\showb"
        "ox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\{=1 \\catcode`\\}=2 \\tracingonline=...\n"
        "\n\n[box]\n> \\box1=\n\\vbox(7.5+2.5)x100.0\n.\\hb"
        "ox(7.5+2.5)x100.0, glue set 69.11435fil\n..\\kern-"
        "1.17 (left margin)\n..\\hbox(7.5+2.5)x13.0556 []\n"
        "..\\glue(\\spaceskip) 4.0\n..\\f a\n..\\f a\n..\\f"
        " a\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0"
        " plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n"
        "l.1 ...vbox{\\noindent\\hbox{(x)} aaa\\par}\\showb"
        "ox1 \n                                            "
        "      \\message{[deep]}\\setbox1=\\...\n\n\n[deep]"
        "\n> \\box1=\n\\vbox(6.83331+0.0)x100.0\n.\\hbox(6."
        "83331+0.0)x100.0, glue set 70.22214fil\n..\\kern-2"
        ".0 (left margin)\n..\\hbox(6.83331+0.0)x12.77782 ["
        "]\n..\\glue(\\spaceskip) 4.0\n..\\f a\n..\\f a\n.."
        "\\f a\n..\\penalty 10000\n..\\glue(\\parfillskip) "
        "0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK"
        ".\nl.1 ...noindent\\hbox{\\hbox{A}x} aaa\\par}\\sh"
        "owbox1 \n                                         "
        "         \\message{[vbox]}\\setbox1=\\...\n\n\n[vb"
        "ox]\n> \\box1=\n\\vbox(6.83331+0.0)x100.0\n.\\hbox"
        "(6.83331+0.0)x100.0, glue set 68.22214fil\n..\\vbo"
        "x(6.83331+0.0)x7.50002 []\n..\\f x\n..\\glue(\\spa"
        "ceskip) 4.0\n..\\f a\n..\\f a\n..\\f a\n..\\penalt"
        "y 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...noinden"
        "t\\vbox{\\hbox{A}}x aaa\\par}\\showbox1 \n        "
        "                                          \\messag"
        "e{[end]}\\setbox1=\\v...\n\n\n[end]\n> \\box1=\n\\"
        "vbox(7.5+2.5)x100.0\n.\\hbox(7.5+2.5)x100.0, glue "
        "set 67.30879fil\n..\\f a\n..\\f a\n..\\f a\n..\\gl"
        "ue(\\spaceskip) 4.0\n..\\hbox(7.5+2.5)x14.86116 []"
        "\n..\\penalty 10000\n..\\kern-1.17 (right margin)"
        "\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glu"
        "e(\\rightskip) 0.0\n\n! OK.\nl.1 ...box{\\noindent"
        " aaa \\hbox{(B)}\\par}\\showbox1 \n               "
        "                                   \\showbox254\n"
        "\n> \\box254=void\n\n! OK.\nl.1 ...nt aaa \\hbox{("
        "B)}\\par}\\showbox1 \\showbox254\n                "
        "                                  \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \\/ adds nothing at all unless a character stands at the end of the list;
   see docs/DECISIONS.md, control-space-and-italic. */
static int test_italic_correction_needs_a_character(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\catcode`\\{=1 \\c"
        "atcode`\\}=2 \\tracingonline=1 \\showboxdepth=3 \\"
        "showboxbreadth=30 \\hbadness=10000 \\hfuzz=1000pt "
        "\\font\\f=cmr10 \\f \\message{[box]}\\setbox0=\\hb"
        "ox{a\\hbox{x}\\/b}\\showbox0 \\message{[chr]}\\set"
        "box0=\\hbox{a\\/b}\\showbox0 \\message{[kern]}\\se"
        "tbox0=\\hbox{a\\kern1pt\\/b}\\showbox0 \\message{["
        "glue]}\\setbox0=\\hbox{a\\hskip1pt\\/b}\\showbox0 "
        "\\message{[none]}\\setbox0=\\hbox{\\/b}\\showbox0 "
        "\\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\ca"
        "tcode`\\{=1 \\catcode`\\}=2 \\tracingonline=...\n"
        "\n\n[box]\n> \\box0=\n\\hbox(6.94444+0.0)x15.83339"
        "\n.\\f a\n.\\hbox(4.30554+0.0)x5.2778\n..\\f x\n."
        "\\f b\n\n! OK.\nl.1 ...box]}\\setbox0=\\hbox{a\\hb"
        "ox{x}\\/b}\\showbox0 \n                           "
        "                       \\message{[chr]}\\setbox0="
        "\\h...\n\n\n[chr]\n> \\box0=\n\\hbox(6.94444+0.0)x"
        "10.55559\n.\\f a\n.\\kern 0.0\n.\\f b\n\n! OK.\nl."
        "1 ...essage{[chr]}\\setbox0=\\hbox{a\\/b}\\showbox"
        "0 \n                                              "
        "    \\message{[kern]}\\setbox0=\\...\n\n\n[kern]\n"
        "> \\box0=\n\\hbox(6.94444+0.0)x11.55559\n.\\f a\n."
        "\\kern 1.0\n.\\f b\n\n! OK.\nl.1 ...ern]}\\setbox0"
        "=\\hbox{a\\kern1pt\\/b}\\showbox0 \n              "
        "                                    \\message{[glu"
        "e]}\\setbox0=\\...\n\n\n[glue]\n> \\box0=\n\\hbox("
        "6.94444+0.0)x11.55559\n.\\f a\n.\\glue 1.0\n.\\f b"
        "\n\n! OK.\nl.1 ...ue]}\\setbox0=\\hbox{a\\hskip1pt"
        "\\/b}\\showbox0 \n                                "
        "                  \\message{[none]}\\setbox0=\\..."
        "\n\n\n[none]\n> \\box0=\n\\hbox(6.94444+0.0)x5.555"
        "57\n.\\f b\n\n! OK.\nl.1 ...essage{[none]}\\setbox"
        "0=\\hbox{\\/b}\\showbox0 \n                       "
        "                           \\showbox254\n\n> \\box"
        "254=void\n\n! OK.\nl.1 ...e]}\\setbox0=\\hbox{\\/b"
        "}\\showbox0 \\showbox254\n                        "
        "                          \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A break that falls inside a ligature replaces it: the two halves of the
   word are set again around the hyphen, and the ligature stands as the text
   used when the line does not break. See docs/DECISIONS.md,
   breaking-inside-a-ligature. */
static int test_breaking_inside_a_ligature(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=200 \\hbadness=1"
        "0000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000p"
        "t \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\hsize=3"
        "00pt \\parindent=0pt \\leftskip=0pt \\rightskip=0p"
        "t \\pdfprotrudechars=0 \\baselineskip=12pt \\lines"
        "kip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus"
        "1fil \\tolerance=10000 \\pretolerance=-1 \\boxmaxd"
        "epth=16383.99998pt \\clubpenalty=0 \\widowpenalty="
        "0 \\interlinepenalty=0 \\brokenpenalty=0 \\uchyph="
        "0 \\lefthyphenmin=1 \\righthyphenmin=1 \\spaceskip"
        "=4pt \\sfcode`\\.=1000 \\lccode`\\c=`\\c \\lccode`"
        "\\o=`\\o \\lccode`\\e=`\\e \\lccode`\\f=`\\f \\lcc"
        "ode`\\i=`\\i \\lccode`\\n=`\\n \\lccode`\\t=`\\t "
        "\\lccode`\\x=`\\x \\lccode`\\a=`\\a \\lccode`\\l=`"
        "\\l \\patterns{o1e f1f i1c 1x1 f1l a1f} \\message{"
        "[ffi]}\\setbox0=\\vbox{\\noindent xx coefficient\\"
        "par}\\showbox0 \\message{[ff]}\\setbox0=\\vbox{\\n"
        "oindent xx affable\\par}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[ffi]\n> \\box0=\n\\vbox(6.94444+0.0)x300.0\n.\\"
        "hbox(6.94444+0.0)x300.0, glue set 242.111fil\n..\\"
        "f x\n..\\f x\n..\\glue(\\spaceskip) 4.0\n..\\f c\n"
        "..\\discretionary replacing 2\n...\\f o\n...\\f -"
        "\n..\\f o\n..\\kern0.27779\n..\\f e\n..\\discretio"
        "nary replacing 1\n...\\f f\n...\\f -\n..|\\f ^^L ("
        "ligature fi)\n..\\f ^^N (ligature ffi)\n..\\discre"
        "tionary\n...\\f -\n..\\f c\n..\\f i\n..\\f e\n..\\"
        "f n\n..\\kern-0.27779\n..\\f t\n..\\penalty 10000"
        "\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glu"
        "e(\\rightskip) 0.0\n\n! OK.\nl.1 ...box{\\noindent"
        " xx coefficient\\par}\\showbox0 \n                "
        "                                  \\message{[ff]}"
        "\\setbox0=\\vb...\n\n\n[ff]\n> \\box0=\n\\vbox(6.9"
        "4444+0.0)x300.0\n.\\hbox(6.94444+0.0)x300.0, glue "
        "set 256.8332fil\n..\\f x\n..\\f x\n..\\glue(\\spac"
        "eskip) 4.0\n..\\f a\n..\\discretionary\n...\\f -\n"
        "..\\discretionary replacing 1\n...\\f f\n...\\f -"
        "\n..|\\f f\n..\\f \v (ligature ff)\n..\\f a\n..\\"
        "f b\n..\\f l\n..\\f e\n..\\penalty 10000\n..\\glue"
        "(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rights"
        "kip) 0.0\n\n! OK.\nl.1 ...0=\\vbox{\\noindent xx a"
        "ffable\\par}\\showbox0 \n                         "
        "                         \\showbox254\n\n> \\box25"
        "4=void\n\n! OK.\nl.1 ...indent xx affable\\par}\\s"
        "howbox0 \\showbox254\n                            "
        "                      \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* An accent rides between two kerns of a kind of their own, worked out in
   one figure and rounded once; see docs/DECISIONS.md, accent-kerns. */
static int test_accent_kerns(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=3 \\showboxbreadth=30 \\hbadness=10"
        "000 \\hfuzz=1000pt \\font\\f=cmr10 \\f \\message{["
        "a]}\\setbox0=\\hbox{\\accent19 e}\\showbox0 \\mess"
        "age{[b]}\\setbox0=\\hbox{\\accent23 o}\\showbox0 "
        "\\message{[c]}\\setbox0=\\hbox{x\\accent19 ey}\\sh"
        "owbox0 \\font\\i=cmti10 \\font\\s=cmsl10 \\i \\mes"
        "sage{[a]}\\setbox0=\\hbox{\\accent19 e}\\showbox0 "
        "\\message{[b]}\\setbox0=\\hbox{\\accent23 o}\\show"
        "box0 \\message{[c]}\\setbox0=\\hbox{\\accent19 A}"
        "\\showbox0 \\s \\message{[d]}\\setbox0=\\hbox{\\ac"
        "cent19 e}\\showbox0 \\message{[e]}\\setbox0=\\hbox"
        "{\\accent127 A}\\showbox0 \\message{[f \\the\\font"
        "dimen1\\i|\\the\\fontdimen5\\i]} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=3 \\showboxbr...\n\n"
        "\n[a]\n> \\box0=\n\\hbox(6.94444+0.0)x4.44444\n.\\"
        "kern -0.27779 (for accent)\n.\\f ^^S\n.\\kern -4.7"
        "2223 (for accent)\n.\\f e\n\n! OK.\nl.1 ...e{[a]}"
        "\\setbox0=\\hbox{\\accent19 e}\\showbox0 \n       "
        "                                           \\messa"
        "ge{[b]}\\setbox0=\\hbo...\n\n\n[b]\n> \\box0=\n\\h"
        "box(6.94444+0.0)x5.00002\n.\\kern -1.25 (for accen"
        "t)\n.\\f ^^W\n.\\kern -6.25002 (for accent)\n.\\f "
        "o\n\n! OK.\nl.1 ...e{[b]}\\setbox0=\\hbox{\\accent"
        "23 o}\\showbox0 \n                                "
        "                  \\message{[c]}\\setbox0=\\hbo..."
        "\n\n\n[c]\n> \\box0=\n\\hbox(6.94444+1.94444)x15.0"
        "0005\n.\\f x\n.\\kern -0.27779 (for accent)\n.\\f "
        "^^S\n.\\kern -4.72223 (for accent)\n.\\f e\n.\\f y"
        "\n\n! OK.\nl.1 ...[c]}\\setbox0=\\hbox{x\\accent19"
        " ey}\\showbox0 \n                                 "
        "                 \\font\\i=cmti10 \\font\\s=cms..."
        "\n\n\n[a]\n> \\box0=\n\\hbox(6.94444+0.0)x4.59996"
        "\n.\\kern -0.25557 (for accent)\n.\\i ^^S\n.\\kern"
        " -4.85551 (for accent)\n.\\i e\n\n! OK.\nl.1 ...e{"
        "[a]}\\setbox0=\\hbox{\\accent19 e}\\showbox0 \n   "
        "                                               \\m"
        "essage{[b]}\\setbox0=\\hbo...\n\n\n[b]\n> \\box0="
        "\n\\hbox(6.94444+0.0)x5.11108\n.\\kern -1.60092 (f"
        "or accent)\n.\\i ^^W\n.\\kern -6.71199 (for accent"
        ")\n.\\i o\n\n! OK.\nl.1 ...e{[b]}\\setbox0=\\hbox{"
        "\\accent23 o}\\showbox0 \n                        "
        "                          \\message{[c]}\\setbox0="
        "\\hbo...\n\n\n[c]\n> \\box0=\n\\hbox(9.47221+0.0)x"
        "7.43329\n.\\kern 1.79305 (for accent)\n.\\hbox(6.9"
        "4444+0.0)x5.11108, shifted -2.52777\n..\\i ^^S\n."
        "\\kern -6.90413 (for accent)\n.\\i A\n\n! OK.\nl.1"
        " ...e{[c]}\\setbox0=\\hbox{\\accent19 A}\\showbox0"
        " \n                                               "
        "   \\s \\message{[d]}\\setbox0=\\...\n\n\n[d]\n> "
        "\\box0=\n\\hbox(6.94444+0.0)x4.44444\n.\\kern -0.2"
        "7779 (for accent)\n.\\s ^^S\n.\\kern -4.72223 (for"
        " accent)\n.\\s e\n\n! OK.\nl.1 ...e{[d]}\\setbox0="
        "\\hbox{\\accent19 e}\\showbox0 \n                 "
        "                                 \\message{[e]}\\s"
        "etbox0=\\hbo...\n\n\n[e]\n> \\box0=\n\\hbox(9.2063"
        "6+0.0)x7.50002\n.\\kern 1.67131 (for accent)\n.\\h"
        "box(6.67859+0.0)x5.00002, shifted -2.52777\n..\\s "
        "^^?\n.\\kern -6.67133 (for accent)\n.\\s A\n\n! OK"
        ".\nl.1 ...{[e]}\\setbox0=\\hbox{\\accent127 A}\\sh"
        "owbox0 \n                                         "
        "         \\message{[f \\the\\fontdimen...\n\n\n[f "
        "0.25pt|4.30554pt]\n> \\box254=void\n\n! OK.\nl.1 ."
        "..ontdimen1\\i|\\the\\fontdimen5\\i]} \\showbox254"
        "\n                                                "
        "  \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* A negative protrusion code is truncated where a positive one is rounded;
   see docs/DECISIONS.md, character-protrusion. */
static int test_protrusion_of_a_negative_code(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=40 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hsize=40pt \\parindent=0pt "
        "\\baselineskip=12pt \\lineskip=0pt \\lineskiplimit"
        "=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\r"
        "ightskip=0pt \\tolerance=10000 \\pretolerance=-1 "
        "\\boxmaxdepth=16383.99998pt \\spaceskip=4pt \\pdfp"
        "rotrudechars=1 \\lpcode\\f`\\A=-2 \\rpcode\\f`\\B="
        "-2 \\lpcode\\f`\\C=2 \\rpcode\\f`\\D=2 \\message{["
        "neg]}\\setbox1=\\vbox{\\noindent AB\\par}\\showbox"
        "1 \\message{[pos]}\\setbox1=\\vbox{\\noindent CD\\"
        "par}\\showbox1 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n[neg]\n> \\box1=\n\\vbox(6.83331+0.0)x40.0\n.\\h"
        "box(6.83331+0.0)x40.0, glue set 25.37665fil\n..\\k"
        "ern0.01999 (left margin)\n..\\f A\n..\\f B\n..\\pe"
        "nalty 10000\n..\\kern0.01999 (right margin)\n..\\g"
        "lue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rig"
        "htskip) 0.0\n\n! OK.\nl.1 ...}\\setbox1=\\vbox{\\n"
        "oindent AB\\par}\\showbox1 \n                     "
        "                             \\message{[pos]}\\set"
        "box1=\\v...\n\n\n[pos]\n> \\box1=\n\\vbox(6.83331+"
        "0.0)x40.0\n.\\hbox(6.83331+0.0)x40.0, glue set 25."
        "17888fil\n..\\kern-0.02 (left margin)\n..\\f C\n.."
        "\\f D\n..\\penalty 10000\n..\\kern-0.02 (right mar"
        "gin)\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n.."
        "\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 ...}\\setbox"
        "1=\\vbox{\\noindent CD\\par}\\showbox1 \n         "
        "                                         \\showbox"
        "254\n\n> \\box254=void\n\n! OK.\nl.1 ...vbox{\\noi"
        "ndent CD\\par}\\showbox1 \\showbox254\n           "
        "                                       \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \\pdfcolorstack leaves a node behind saying what it did; see
   docs/DECISIONS.md, colour-stack-nodes. */
static int test_colour_stack_nodes(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=3 \\showboxbreadth=30 \\hbadness=10"
        "000 \\hfuzz=1000pt \\font\\f=cmr10 \\f \\pdfoutput"
        "=1 \\pdfcolorstackinit page direct {0 g 0 G} \\mes"
        "sage{[a]}\\setbox0=\\hbox{a\\pdfcolorstack0 push {"
        "1 0 0 rg}b\\pdfcolorstack0 pop c}\\showbox0 \\mess"
        "age{[b]}\\setbox0=\\hbox{a\\pdfcolorstack0 set {0 "
        "g}b}\\showbox0 \\message{[c]}\\setbox0=\\hbox{a\\p"
        "dfcolorstack0 current b}\\showbox0 \\message{[t \\"
        "the\\lastnodetype]} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=3 \\showboxbr...\n\n"
        "\n[a]\n> \\box0=\n\\hbox(6.94444+0.0)x18.33336\n."
        "\\f a\n.\\pdfcolorstack 0 push {1 0 0 rg}\n.\\f b"
        "\n.\\pdfcolorstack 0 pop\n.\\glue 3.33333 plus 1.6"
        "6666 minus 1.11111\n.\\f c\n\n! OK.\nl.1 ...{1 0 0"
        " rg}b\\pdfcolorstack0 pop c}\\showbox0 \n         "
        "                                         \\message"
        "{[b]}\\setbox0=\\hbo...\n\n\n[b]\n> \\box0=\n\\hbo"
        "x(6.94444+0.0)x10.55559\n.\\f a\n.\\pdfcolorstack "
        "0 set {0 g}\n.\\f b\n\n! OK.\nl.1 ...hbox{a\\pdfco"
        "lorstack0 set {0 g}b}\\showbox0 \n                "
        "                                  \\message{[c]}\\"
        "setbox0=\\hbo...\n\n\n[c]\n> \\box0=\n\\hbox(6.944"
        "44+0.0)x13.88892\n.\\f a\n.\\pdfcolorstack 0 curre"
        "nt\n.\\glue 3.33333 plus 1.66666 minus 1.11111\n."
        "\\f b\n\n! OK.\nl.1 ...\\hbox{a\\pdfcolorstack0 cu"
        "rrent b}\\showbox0 \n                             "
        "                     \\message{[t \\the\\lastnodet"
        "...\n\n\n[t 11]\n> \\box254=void\n\n! OK.\nl.1 ..."
        "\\message{[t \\the\\lastnodetype]} \\showbox254\n "
        "                                                 "
        "\n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The forced break that ends a paragraph counts as a hyphenated one, so a
   hyphen on the line before it costs \\finalhyphendemerits; see
   docs/DECISIONS.md, the-final-break-is-hyphenated. */
static int test_the_final_break_is_hyphenated(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=1 \\showboxbreadth=40 \\hbadness=10"
        "000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt"
        " \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\hsize=28"
        "pt \\parindent=0pt \\leftskip=0pt \\rightskip=0pt "
        "\\pdfprotrudechars=0 \\baselineskip=12pt \\lineski"
        "p=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1f"
        "il \\tolerance=10000 \\pretolerance=-1 \\boxmaxdep"
        "th=16383.99998pt \\clubpenalty=0 \\widowpenalty=0 "
        "\\interlinepenalty=0 \\brokenpenalty=0 \\uchyph=0 "
        "\\lefthyphenmin=1 \\righthyphenmin=1 \\spaceskip=4"
        "pt plus2pt minus1pt \\linepenalty=10 \\adjdemerits"
        "=0 \\doublehyphendemerits=0 \\hyphenpenalty=50 \\l"
        "ccode`\\a=`\\a \\lccode`\\x=`\\x \\patterns{a1a 1x"
        "1} \\finalhyphendemerits=0 \\message{[none]}\\setb"
        "ox0=\\vbox{\\noindent xx aaaaaa aaaa\\par}\\showbo"
        "x0 \\finalhyphendemerits=-1000000 \\message{[some]"
        "}\\setbox0=\\vbox{\\noindent xx aaaaaa aaaa\\par}"
        "\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=1 \\showboxbr...\n\n"
        "\n[none]\n> \\box0=\n\\vbox(28.30554+0.0)x28.0\n."
        "\\hbox(4.30554+0.0)x28.0, glue set 0.05553 []\n.\\"
        "glue(\\baselineskip) 7.69446\n.\\hbox(4.30554+0.0)"
        "x28.0 []\n.\\glue(\\baselineskip) 7.69446\n.\\hbox"
        "(4.30554+0.0)x28.0, glue set 7.99994fil []\n\n! OK"
        ".\nl.1 ...box{\\noindent xx aaaaaa aaaa\\par}\\sho"
        "wbox0 \n                                          "
        "        \\finalhyphendemerits=-1000...\n\n\n[some]"
        "\n> \\box0=\n\\vbox(40.30554+0.0)x28.0\n.\\hbox(4."
        "30554+0.0)x28.0, glue set 0.05553 []\n.\\glue(\\ba"
        "selineskip) 7.69446\n.\\hbox(4.30554+0.0)x28.0 []"
        "\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4.30554"
        "+0.0)x28.0, glue set 0.33331 []\n.\\glue(\\baselin"
        "eskip) 7.69446\n.\\hbox(4.30554+0.0)x28.0, glue se"
        "t 22.99998fil []\n\n! OK.\nl.1 ...box{\\noindent x"
        "x aaaaaa aaaa\\par}\\showbox0 \n                  "
        "                                \\showbox254\n\n> "
        "\\box254=void\n\n! OK.\nl.1 ...nt xx aaaaaa aaaa\\"
        "par}\\showbox0 \\showbox254\n                     "
        "                             \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \patterns put discretionaries in words the second pass reads out of the
   paragraph; see docs/DECISIONS.md, hyphenation. */
static int test_hyphenation(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=200 \\hbadness=1"
        "0000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000p"
        "t \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\hsize=2"
        "00pt \\parindent=0pt \\leftskip=0pt \\rightskip=0p"
        "t \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus1fil \\tolerance=1000"
        "0 \\pretolerance=-1 \\boxmaxdepth=16383.99998pt \\"
        "linepenalty=10 \\adjdemerits=10000 \\doublehyphend"
        "emerits=10000 \\finalhyphendemerits=5000 \\clubpen"
        "alty=0 \\widowpenalty=0 \\interlinepenalty=0 \\bro"
        "kenpenalty=0 \\hyphenpenalty=50 \\exhyphenpenalty="
        "50 \\uchyph=0 \\lefthyphenmin=2 \\righthyphenmin=3"
        " \\spaceskip=4pt \\sfcode`\\.=1000 \\lccode`\\a=`"
        "\\a \\lccode`\\b=`\\b \\lccode`\\c=`\\c \\lccode`"
        "\\d=`\\d \\lccode`\\e=`\\e \\lccode`\\f=`\\f \\lcc"
        "ode`\\g=`\\g \\lccode`\\h=`\\h \\lccode`\\i=`\\i "
        "\\lccode`\\j=`\\j \\lccode`\\k=`\\k \\lccode`\\l=`"
        "\\l \\lccode`\\m=`\\m \\lccode`\\n=`\\n \\lccode`"
        "\\o=`\\o \\lccode`\\p=`\\p \\lccode`\\q=`\\q \\lcc"
        "ode`\\r=`\\r \\lccode`\\s=`\\s \\lccode`\\t=`\\t "
        "\\lccode`\\u=`\\u \\lccode`\\v=`\\v \\lccode`\\w=`"
        "\\w \\lccode`\\x=`\\x \\lccode`\\y=`\\y \\lccode`"
        "\\z=`\\z \\lccode`\\A=`\\a \\lccode`\\H=`\\h \\pat"
        "terns{2n3t 1hy 3phe a1t io1n an1t} \\setbox0=\\vbo"
        "x{\\noindent xx sentant hyphenation Antant\\par}\\"
        "showbox0 \\uchyph=1 \\setbox0=\\vbox{\\noindent xx"
        " Antant\\par}\\showbox0 \\lefthyphenmin=4 \\setbox"
        "0=\\vbox{\\noindent xx sentant\\par}\\showbox0 \\s"
        "howbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n> \\box0=\n\\vbox(6.94444+1.94444)x200.0\n.\\hbo"
        "x(6.94444+1.94444)x200.0, glue set 60.99968fil\n.."
        "\\f x\n..\\f x\n..\\glue(\\spaceskip) 4.0\n..\\f s"
        "\n..\\f e\n..\\discretionary replacing 2\n...\\f n"
        "\n...\\f -\n..\\f n\n..\\kern-0.27779\n..\\f t\n.."
        "\\f a\n..\\f n\n..\\kern-0.27779\n..\\f t\n..\\glu"
        "e(\\spaceskip) 4.0\n..\\f h\n..\\kern-0.27779\n.."
        "\\f y\n..\\discretionary\n...\\f -\n..\\f p\n..\\f"
        " h\n..\\f e\n..\\f n\n..\\f a\n..\\discretionary\n"
        "...\\f -\n..\\f t\n..\\f i\n..\\f o\n..\\f n\n..\\"
        "glue(\\spaceskip) 4.0\n..\\f A\n..\\f n\n..\\kern-"
        "0.27779\n..\\f t\n..\\f a\n..\\f n\n..\\kern-0.277"
        "79\n..\\f t\n..\\penalty 10000\n..\\glue(\\parfill"
        "skip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n"
        "\n! OK.\nl.1 ...x sentant hyphenation Antant\\par}"
        "\\showbox0 \n                                     "
        "             \\uchyph=1 \\setbox0=\\vbox{\\...\n\n"
        "\n> \\box0=\n\\vbox(6.83331+0.0)x200.0\n.\\hbox(6."
        "83331+0.0)x200.0, glue set 154.611fil\n..\\f x\n.."
        "\\f x\n..\\glue(\\spaceskip) 4.0\n..\\f A\n..\\dis"
        "cretionary replacing 2\n...\\f n\n...\\f -\n..\\f "
        "n\n..\\kern-0.27779\n..\\f t\n..\\f a\n..\\f n\n.."
        "\\kern-0.27779\n..\\f t\n..\\penalty 10000\n..\\gl"
        "ue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\righ"
        "tskip) 0.0\n\n! OK.\nl.1 ...x0=\\vbox{\\noindent x"
        "x Antant\\par}\\showbox0 \n                       "
        "                           \\lefthyphenmin=4 \\set"
        "box0=...\n\n\n> \\box0=\n\\vbox(6.15079+0.0)x200.0"
        "\n.\\hbox(6.15079+0.0)x200.0, glue set 153.72212fi"
        "l\n..\\f x\n..\\f x\n..\\glue(\\spaceskip) 4.0\n.."
        "\\f s\n..\\f e\n..\\f n\n..\\kern-0.27779\n..\\f t"
        "\n..\\f a\n..\\f n\n..\\kern-0.27779\n..\\f t\n.."
        "\\penalty 10000\n..\\glue(\\parfillskip) 0.0 plus "
        "1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\nl.1 .."
        ".0=\\vbox{\\noindent xx sentant\\par}\\showbox0 \n"
        "                                                  "
        "\\showbox254\n\n> \\box254=void\n\n! OK.\nl.1 ...i"
        "ndent xx sentant\\par}\\showbox0 \\showbox254\n   "
        "                                               \n"
        "\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* \spaceskip and \xspaceskip take the place of the font's own space, and
   only a space the space factor has not touched keeps the name of the
   parameter it came from; see docs/DECISIONS.md, interword-glue. */
static int test_interword_glue(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=5 \\showboxbreadth=30 \\hbadness=10"
        "000 \\hfuzz=1000pt \\font\\f=cmr10 \\f \\sfcode`\\"
        ".=3000 \\sfcode`\\,=1250 \\spaceskip=4pt plus1pt m"
        "inus2pt \\xspaceskip=9pt plus3pt \\setbox0=\\hbox{"
        "a a}\\showbox0 \\setbox0=\\hbox{a. a}\\showbox0 \\"
        "setbox0=\\hbox{a, a}\\showbox0 \\xspaceskip=0pt \\"
        "setbox0=\\hbox{a. a}\\showbox0 \\spaceskip=0pt \\s"
        "etbox0=\\hbox{a a}\\showbox0 \\setbox0=\\hbox{a. a"
        "}\\showbox0 \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=5 \\showboxbr...\n\n"
        "\n> \\box0=\n\\hbox(4.30554+0.0)x14.00003\n.\\f a"
        "\n.\\glue(\\spaceskip) 4.0 plus 1.0 minus 2.0\n.\\"
        "f a\n\n! OK.\nl.1 ...p=9pt plus3pt \\setbox0=\\hbo"
        "x{a a}\\showbox0 \n                               "
        "                   \\setbox0=\\hbox{a. a}\\showb.."
        ".\n\n\n> \\box0=\n\\hbox(4.30554+0.0)x21.77782\n."
        "\\f a\n.\\f .\n.\\glue(\\xspaceskip) 9.0 plus 3.0"
        "\n.\\f a\n\n! OK.\nl.1 ... a}\\showbox0 \\setbox0="
        "\\hbox{a. a}\\showbox0 \n                         "
        "                         \\setbox0=\\hbox{a, a}\\s"
        "howb...\n\n\n> \\box0=\n\\hbox(4.30554+1.94444)x16"
        ".77782\n.\\f a\n.\\f ,\n.\\glue 4.0 plus 1.25 minu"
        "s 1.59999\n.\\f a\n\n! OK.\nl.1 ... a}\\showbox0 "
        "\\setbox0=\\hbox{a, a}\\showbox0 \n               "
        "                                   \\xspaceskip=0p"
        "t \\setbox0=\\...\n\n\n> \\box0=\n\\hbox(4.30554+0"
        ".0)x17.88893\n.\\f a\n.\\f .\n.\\glue 5.11111 plus"
        " 3.0 minus 0.66666\n.\\f a\n\n! OK.\nl.1 ...pacesk"
        "ip=0pt \\setbox0=\\hbox{a. a}\\showbox0 \n        "
        "                                          \\spaces"
        "kip=0pt \\setbox0=\\h...\n\n\n> \\box0=\n\\hbox(4."
        "30554+0.0)x13.33336\n.\\f a\n.\\glue 3.33333 plus "
        "1.66666 minus 1.11111\n.\\f a\n\n! OK.\nl.1 ...spa"
        "ceskip=0pt \\setbox0=\\hbox{a a}\\showbox0 \n     "
        "                                             \\set"
        "box0=\\hbox{a. a}\\showb...\n\n\n> \\box0=\n\\hbox"
        "(4.30554+0.0)x17.22226\n.\\f a\n.\\f .\n.\\glue 4."
        "44444 plus 4.99997 minus 0.37036\n.\\f a\n\n! OK."
        "\nl.1 ... a}\\showbox0 \\setbox0=\\hbox{a. a}\\sho"
        "wbox0 \n                                          "
        "        \\showbox254\n\n> \\box254=void\n\n! OK.\n"
        "l.1 ...0 \\setbox0=\\hbox{a. a}\\showbox0 \\showbo"
        "x254\n                                            "
        "      \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* The current font is restored on the way out of a group, and the character
   held back for the ligature program is taken in before that happens. See
   docs/DECISIONS.md, the-current-font-is-grouped. */
static int test_the_current_font_is_grouped(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\tracingonline=1 "
        "\\showboxdepth=3 \\showboxbreadth=20 \\hbadness=10"
        "000 \\font\\f=cmr10 \\font\\g=cmr10 at 20pt \\f \\"
        "setbox0=\\hbox{fi}\\showbox0 \\setbox0=\\hbox{f{}i"
        "}\\showbox0 \\setbox0=\\hbox{A{\\g A}A}\\showbox0 "
        "\\setbox0=\\hbox{{\\g A}}\\showbox0 \\dimen0=1em "
        "\\message{[em \\the\\dimen0]}{\\g }\\dimen0=1em \\"
        "message{[em \\the\\dimen0]}{\\global\\g}\\dimen0=1"
        "em \\message{[em \\the\\dimen0]} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\tr"
        "acingonline=1 \\showboxdepth=3 \\showboxbr...\n\n"
        "\n> \\box0=\n\\hbox(6.94444+0.0)x5.55557\n.\\f ^^L"
        " (ligature fi)\n\n! OK.\nl.1 ...r10 at 20pt \\f \\"
        "setbox0=\\hbox{fi}\\showbox0 \n                   "
        "                               \\setbox0=\\hbox{f{"
        "}i}\\showb...\n\n\n> \\box0=\n\\hbox(6.94444+0.0)x"
        "5.83336\n.\\f f\n.\\f i\n\n! OK.\nl.1 ...fi}\\show"
        "box0 \\setbox0=\\hbox{f{}i}\\showbox0 \n          "
        "                                        \\setbox0="
        "\\hbox{A{\\g A}A}\\s...\n\n\n> \\box0=\n\\hbox(13."
        "66664+0.0)x30.00006\n.\\f A\n.\\g A\n.\\f A\n\n! O"
        "K.\nl.1 ...showbox0 \\setbox0=\\hbox{A{\\g A}A}\\s"
        "howbox0 \n                                        "
        "          \\setbox0=\\hbox{{\\g A}}\\sho...\n\n\n>"
        " \\box0=\n\\hbox(13.66664+0.0)x15.00003\n.\\g A\n"
        "\n! OK.\nl.1 ...}\\showbox0 \\setbox0=\\hbox{{\\g "
        "A}}\\showbox0 \n                                  "
        "                \\dimen0=1em \\message{[em \\...\n"
        "\n\n[em 10.00002pt] [em 10.00002pt] [em 20.00005pt"
        "]\n> \\box254=void\n\n! OK.\nl.1 ...=1em \\message"
        "{[em \\the\\dimen0]} \\showbox254\n               "
        "                                   \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* Glue, kerns and penalties in front of the first box of a page are thrown
   away, and a whatsit is not: it joins the page and leaves it empty. See
   docs/DECISIONS.md, whatsits-on-an-empty-page. */
static int test_whatsits_on_an_empty_page(void)
{
    static const char *const source[] = {
        "\\tracingonline=1 \\showbox254 \\vsize=100pt \\top"
        "skip=0pt \\maxdepth=0pt \\hsize=100pt \\tracingonl"
        "ine=1 \\showboxdepth=5 \\showboxbreadth=10 \\outpu"
        "t={\\message{[FIRE \\the\\outputpenalty]}\\showbox"
        "255 \\shipout\\box255 }\\message{[A]}\\penalty-100"
        "00 \\message{[B]}\\vskip5pt \\penalty-10000 \\mess"
        "age{[C]}\\kern7pt \\penalty-10000 \\message{[D]}\\"
        "write-1{w}\\message{[\\the\\pagegoal]}\\hbox{}\\pe"
        "nalty-10000 \\message{[E]} \\showbox254 ",
        NULL,
    };
    static const char *const expected[] = {
        "> \\box254=void\n\n! OK.\nl.1 \\tracingonline=1 \\"
        "showbox254 \n                                 \\vs"
        "ize=100pt \\topskip=0pt \\maxdepth=0pt \\hs...\n\n"
        "\n[A] [B] [C] [D] [16383.99998pt] [FIRE -10000]\n>"
        " \\box255=\n\\vbox(100.0+0.0)x0.0\n.\\write-{w}\n."
        "\\glue(\\topskip) 0.0\n.\\hbox(0.0+0.0)x0.0\n\n! O"
        "K.\n<output> ...RE \\the \\outputpenalty ]}\\showb"
        "ox 255 \n                                         "
        "         \\shipout \\box 255 }\nl.1 ...sage{[\\the"
        "\\pagegoal]}\\hbox{}\\penalty-10000 \n            "
        "                                      \\message{[E"
        "]} \\showbox254\n\n[0\nw\n] [E]\n> \\box254=void\n"
        "\n! OK.\nl.1 ...x{}\\penalty-10000 \\message{[E]} "
        "\\showbox254\n                                    "
        "              \n\n",
        NULL,
    };
    return run_document_parts(source, expected);
}

/* expl3 selects its backend from these, so the values must match the engine
   HSTeX reproduces; see docs/DECISIONS.md, pdftex-identification. */
static int test_pdftex_identification(void)
{
    return run_snippet("[\\number\\pdftexversion][\\pdftexrevision]"
                       "\\pdfoutput=1 [\\number\\pdfoutput]"
                       "\\pdfpagewidth=8.5 true in [\\the\\pdfpagewidth]"
                       "\\pdfpageheight=11 true in [\\the\\pdfpageheight]"
                       "\\pdfminorversion=5 [\\number\\pdfminorversion]%",
                       "[140][25][1][614.295pt][794.96999pt][5]");
}

/* Box displacement and vertical packaging, all pinned to the reference; see
   docs/DECISIONS.md, box-shift and vertical-packaging. */
static int test_box_shift_and_packaging(void)
{
    return run_snippet(
        "\\boxmaxdepth=16383.99998pt "
        "\\baselineskip=12pt \\lineskip=1pt \\lineskiplimit=0pt "
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        "\\setbox2=\\hbox{\\vrule height9pt depth1pt width1pt}"
        /* \\lower and \\raise trade height for depth. */
        "\\setbox0=\\hbox{\\lower3pt\\copy1}[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        "\\setbox0=\\hbox{\\raise3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\hbox{\\lower10pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* \\moveright widens a vertical list; neither dimension changes. */
        "\\setbox0=\\vbox{\\moveright4pt\\copy1}[\\the\\wd0|\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\moveleft4pt\\copy1}[\\the\\wd0]"
        /* A vertical list keeps the last box's depth and separates baselines. */
        "\\setbox0=\\vbox{\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\copy1\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\copy1\\copy2}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\copy1\\vskip3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\lineskiplimit=4pt"
        "\\setbox0=\\vbox{\\copy1\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* \\boxmaxdepth moves surplus depth into the height. */
        "\\boxmaxdepth=1pt \\lineskiplimit=0pt"
        "\\setbox0=\\vbox{\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* \\box empties its register, \\copy does not. */
        "\\setbox0=\\box1 [\\the\\wd0|\\the\\wd1]%",
        "[2.0pt|5.0pt|1.0pt][8.0pt|0.0pt][0.0pt|12.0pt]"
        "[5.0pt|5.0pt|2.0pt][0.0pt]"
        "[5.0pt|2.0pt][17.0pt|2.0pt][17.0pt|1.0pt][20.0pt|2.0pt]"
        "[17.0pt|2.0pt][6.0pt|1.0pt][1.0pt|0.0pt]");
}

/* \vtop, \vsplit, \lastbox, and the control space and italic correction;
   see docs/DECISIONS.md, vtop-and-lastbox and control-space-and-italic. */
static int test_box_grammar_and_spacing(void)
{
    return run_snippet(
        "\\font\\f=cmr10 \\f \\boxmaxdepth=16383.99998pt "
        "\\baselineskip=0pt \\lineskip=0pt \\lineskiplimit=0pt "
        "\\dimendef\\maxdimen=250 \\maxdimen=16383.99998pt "
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        /* \vtop keeps the first item's height and calls the rest depth. */
        "\\setbox0=\\vtop{\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vtop{\\copy1\\copy1}[\\the\\ht0|\\the\\dp0]"
        /* A leading kern is not a box, so the height is zero. */
        "\\setbox0=\\vtop{\\kern3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vtop{}[\\the\\ht0|\\the\\dp0]"
        /* Splitting a void register gives nothing; asking for at least the
           whole list takes all of it and empties the register. */
        "\\setbox9=\\box9 \\setbox0=\\vsplit9 to 5pt"
        "[\\ifvoid0 V\\else N\\fi]"
        "\\setbox2=\\vbox{\\hrule height2pt \\kern5pt \\hrule height3pt}"
        "\\setbox0=\\vsplit2 to \\maxdimen"
        "[\\the\\ht0|\\ifvoid2 V\\else N\\fi]"
        /* A control space ignores the space factor, unlike a real space. */
        "\\setbox0=\\hbox{A\\ \\global\\skip1=\\lastskip}[\\the\\skip1]"
        "\\setbox0=\\hbox{A \\global\\skip2=\\lastskip}[\\the\\skip2]"
        "\\setbox0=\\hbox{\\char65}[\\the\\wd0]"
        /* \unskip, \unkern and \unpenalty remove the last node only when it
           is of their own kind. */
        "\\setbox0=\\hbox{\\vrule width1pt\\hskip5pt\\unskip}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\vrule width1pt\\kern5pt\\unskip}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\vrule width1pt\\kern5pt\\unkern}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\vrule width1pt\\unskip}[\\the\\wd0]"
        "\\setbox0=\\vbox{\\hrule height2pt\\vskip5pt\\unskip}[\\the\\ht0]"
        "\\setbox0=\\hbox{\\vrule width1pt\\penalty50 \\unpenalty"
        "\\global\\count1=\\lastnodetype}[\\the\\count1]%",
        "[5.0pt|2.0pt][5.0pt|9.0pt][0.0pt|10.0pt][0.0pt|0.0pt]"
        "[V][16383.99998pt|V]"
        "[3.33333pt plus 1.66666pt minus 1.11111pt]"
        "[3.33333pt plus 1.66498pt minus 1.11221pt][7.50002pt]"
        "[1.0pt][6.0pt][1.0pt][1.0pt][2.0pt][3]");
}

/* A paragraph that fits on one line; see docs/DECISIONS.md, paragraphs. */
static int test_paragraphs(void)
{
    return run_snippet(
        "\\font\\f=cmr10 \\f"
        "\\hsize=100pt \\parindent=20pt \\parskip=0pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\boxmaxdepth=16383.99998pt "
        /* The line is set to \hsize, whichever way the paragraph starts. */
        "\\setbox0=\\vbox{\\noindent A\\par}"
        "[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        "\\setbox0=\\vbox{\\indent A\\par}[\\the\\ht0|\\the\\wd0]"
        /* \parskip is added only when the vertical list has something in it. */
        "\\parskip=3pt plus1pt "
        "\\setbox0=\\vbox{\\hrule height1pt \\noindent A\\par}[\\the\\ht0]"
        "\\parskip=0pt \\leftskip=5pt \\rightskip=7pt "
        "\\setbox0=\\vbox{\\noindent A\\par}[\\the\\ht0|\\the\\wd0]"
        "\\leftskip=0pt \\rightskip=0pt "
        /* A descender gives the line depth. */
        "\\setbox0=\\vbox{\\noindent Ag\\par}[\\the\\ht0|\\the\\dp0]"
        /* Two paragraphs are separated by interline glue. */
        "\\setbox0=\\vbox{\\noindent A\\par\\noindent A\\par}[\\the\\ht0]%",
        "[6.83331pt|0.0pt|100.0pt][6.83331pt|100.0pt][10.83331pt]"
        "[6.83331pt|100.0pt][6.83331pt|1.94444pt][18.83331pt]");
}

/* A horizontal command met in vertical mode is put back and read again, so
   that \everypar runs before the command scans its own operands; see
   docs/DECISIONS.md, starting-a-paragraph. */
static int test_starting_a_paragraph(void)
{
    return run_snippet(
        "\\font\\f=cmr10 \\f\\hsize=200pt \\parindent=10pt \\tolerance=10000 "
        /* \everypar sets \dimen0 to 7pt, so \hskip\dimen0 must measure 7pt
           and not the 3pt that was current when \hskip was read. */
        "\\everypar{\\dimen0=7pt}\\dimen0=3pt "
        "\\setbox0=\\vbox{\\hskip\\dimen0 \\global\\skip1=\\lastskip\\par}"
        "[\\the\\skip1]"
        /* \unhbox starts one too, which is what \leavevmode relies on. */
        "\\everypar{\\global\\count1=1 }\\setbox1=\\hbox{}\\count1=0 "
        "\\setbox2=\\vbox{\\unhbox1 \\global\\count2=\\count1 \\par}"
        "[\\the\\count2]"
        /* The paragraph is indented and set to \hsize. */
        "\\everypar{}\\setbox3=\\vbox{\\hskip3pt\\par}[\\the\\wd3]"
        /* \vrule, \char, a control space and \hfil start one; \unvbox does
           not, and pdfTeX rejects \/ in internal vertical mode outright. */
        "\\def\\p#1{\\setbox4=\\vbox{#1\\global\\count0=\\ifhmode 1\\else 0\\fi"
        "\\par}[\\the\\count0]}"
        "\\p{\\vrule width1pt}\\p{\\char65 }\\p{\\ }\\p{\\hfil}"
        "\\setbox5=\\vbox{}\\p{\\unvbox5 }%",
        "[7.0pt][1][200.0pt][1][1][1][1][0]");
}

/* Characters, with the font's ligature and kerning program and interword
   glue; see docs/DECISIONS.md, characters-and-ligatures. */
static int test_characters(void)
{
    return run_snippet(
        "\\font\\f=cmr10 \\f"
        "\\setbox0=\\hbox{A}[\\the\\wd0|\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\hbox{ABC}[\\the\\wd0]"
        /* A descender has depth. */
        "\\setbox0=\\hbox{g}[\\the\\ht0|\\the\\dp0]"
        /* A space becomes glue from the font, stretched by the space factor,
           which an uppercase letter sets to 999. */
        "\\setbox0=\\hbox{A \\global\\skip1=\\lastskip}[\\the\\skip1]"
        "\\setbox0=\\hbox{A B}[\\the\\wd0]"
        /* AV is kerned, ff is a ligature and reports as one. */
        "\\setbox0=\\hbox{AV}[\\the\\wd0]"
        "\\setbox0=\\hbox{ff\\global\\count1=\\lastnodetype}"
        "[\\the\\wd0|\\the\\count1]"
        "\\setbox0=\\hbox{ffi}[\\the\\wd0]"
        /* A command between the two breaks the pair. */
        "\\setbox0=\\hbox{f\\relax f}[\\the\\wd0]%",
        "[7.50002pt|6.83331pt|0.0pt][21.8056pt][4.30554pt|1.94444pt]"
        "[3.33333pt plus 1.66498pt minus 1.11221pt][17.9167pt][13.8889pt]"
        "[5.83336pt|7][8.33336pt][6.11115pt]");
}

/* A control sequence \let to a brace opens a box; see docs/DECISIONS.md,
   implicit-braces. */
static int test_implicit_braces(void)
{
    return run_snippet(
        "\\font\\f=cmr10 \\f\\let\\bg={\\let\\eg=}"
        "\\setbox0=\\hbox\\bg\\kern5pt\\eg[\\the\\wd0]"
        "\\setbox1=\\vbox\\bg\\kern5pt\\eg[\\the\\ht1]"
        "\\setbox2=\\hbox to 20pt\\bg\\eg[\\the\\wd2]"
        "\\setbox3=\\vtop\\bg\\kern5pt\\eg[\\the\\ht3]"
        /* The brace may arrive by expansion, and may be nested. */
        "\\def\\m{\\bg}\\setbox4=\\hbox\\m\\kern7pt\\eg[\\the\\wd4]"
        "\\setbox5=\\hbox\\bg\\kern1pt\\hbox\\bg\\kern2pt\\eg\\eg[\\the\\wd5]"
        /* An implicit brace opens a group too. */
        "\\count0=1 \\bg\\count0=2 \\eg[\\the\\count0]%",
        "[5.0pt][5.0pt][20.0pt][0.0pt][7.0pt][3.0pt][1]");
}

/* A box body is executed, not read ahead over, so one macro may open a box
   and another close it; see docs/DECISIONS.md, streaming-box-bodies. */
static int test_streaming_box_bodies(void)
{
    return run_snippet(
        "\\font\\f=cmr10 \\f\\let\\bgroup={\\let\\egroup=}"
        "\\def\\openh{\\hbox\\bgroup\\kern3pt}\\def\\closeh{\\kern4pt\\egroup}"
        "\\setbox0=\\openh\\closeh[\\the\\wd0]"
        "\\def\\openv{\\vbox\\bgroup\\kern3pt}\\def\\closev{\\kern4pt\\egroup}"
        "\\setbox1=\\openv\\closev[\\the\\ht1]"
        "\\def\\wrap#1{\\setbox2=\\hbox\\bgroup #1\\egroup}"
        "\\wrap{\\kern9pt}[\\the\\wd2]"
        /* A conditional may span the body, and must be closed inside it. */
        "\\setbox3=\\hbox{\\kern1pt\\ifnum1=1 \\kern2pt\\fi\\kern4pt}"
        "[\\the\\wd3]"
        /* The body is a group: assignments are local unless made global. */
        "\\count0=5 \\setbox4=\\hbox{\\count0=6 \\global\\count1=\\count0 }"
        "[\\the\\count0|\\the\\count1]%",
        "[7.0pt][7.0pt][9.0pt][7.0pt][5|6]");
}

/* Inline formulas: atom classes and the spacing between them, math families,
   italic corrections and the family's ligature program; see
   docs/DECISIONS.md, math-mode. */
static int test_math_mode(void)
{
    return run_snippet(
        "\\catcode`\\$=3 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\scriptfont0=\\tenrm \\scriptscriptfont0=\\tenrm "
        "\\textfont1=\\tenmi \\scriptfont1=\\tenmi \\scriptscriptfont1=\\tenmi "
        "\\textfont2=\\tensy \\scriptfont2=\\tensy \\scriptscriptfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont3=\\tenex \\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu "
        "\\def\\O{\\mathchar\"0030 }"
        "\\def\\m#1{\\setbox0=\\hbox{$#1$}[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* One ordinary atom, then two: Ord-Ord has no space between. */
        "\\m{\\O}\\m{\\O\\O}"
        /* A binary operator takes \medmuskip, a relation \thickmuskip and
           punctuation \thinmuskip -- but only where the class survives. */
        "\\m{\\O\\mathchar\"2030 \\O}\\m{\\O\\mathchar\"3030 \\O}"
        "\\m{\\O\\mathchar\"6030 \\O}\\m{\\O\\mathinner{\\O}\\O}"
        /* A large operator is centred on the axis of family two. */
        "\\m{\\mathop{\\O}}"
        /* \mskip and \mkern measure in mu. */
        "\\m{\\O\\mskip9mu\\O}\\m{\\O\\mkern9mu\\O}"
        /* A braced sub-formula is one ordinary atom. */
        "\\m{\\O{\\O\\O}\\O}"
        /* Adjacent characters of one family use its ligature and kern
           program, and every character gets its italic correction. */
        "\\m{\\mathchar\"0066 \\mathchar\"0069 }"
        "\\m{\\mathchar\"0041 \\mathchar\"0056 }"
        /* Class seven takes its family from \fam when that names one. */
        "\\mathcode`z=\"7031 \\m{z}\\m{\\fam2 z}"
        /* \mathsurround goes on both sides of the whole formula. */
        "\\mathsurround=5pt \\m{\\O}\\mathsurround=0pt "
        /* \everymath runs after \fam has been cleared, so it may set one. */
        "\\everymath={\\fam2 }\\m{\\mathchar\"7031 }\\everymath={}"
        /* A box is an ordinary atom. */
        "\\m{\\hbox{}\\O}"
        /* A binary operator between an Op and a Rel is not binary at all. */
        "\\m{\\mathchar\"1030 \\mathchar\"2030 \\mathchar\"3030 }%",
        "[5.00002pt|6.44444pt|0.0pt][10.00003pt|6.44444pt|0.0pt]"
        "[19.44438pt|6.44444pt|0.0pt][20.55547pt|6.44444pt|0.0pt]"
        "[16.66667pt|6.44444pt|0.0pt][18.3333pt|6.44444pt|0.0pt]"
        "[5.00002pt|5.72221pt|0.72223pt][14.99991pt|6.44444pt|0.0pt]"
        "[14.99991pt|6.44444pt|0.0pt][20.00006pt|6.44444pt|0.0pt]"
        "[5.55557pt|6.94444pt|0.0pt][14.02777pt|6.83331pt|0.0pt]"
        "[5.00002pt|6.44444pt|0.0pt][10.00002pt|4.30554pt|0.0pt]"
        "[15.00002pt|6.44444pt|0.0pt][10.00002pt|4.30554pt|0.0pt]"
        "[5.00002pt|6.44444pt|0.0pt][19.44438pt|6.44444pt|0.72223pt]");
}

/* Superscripts and subscripts: the styles they are set in, the shifts, and
   the spacing that stops applying below text style; see docs/DECISIONS.md,
   math-scripts. */
static int test_math_scripts(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 \\font\\sevenrm=cmr7 \\font\\sevenmi=cmmi7 "
        "\\font\\sevensy=cmsy7 \\font\\fiverm=cmr5 \\font\\fivemi=cmmi5 "
        "\\font\\fivesy=cmsy5 "
        "\\textfont0=\\tenrm \\scriptfont0=\\sevenrm "
        "\\scriptscriptfont0=\\fiverm "
        "\\textfont1=\\tenmi \\scriptfont1=\\sevenmi "
        "\\scriptscriptfont1=\\fivemi "
        "\\textfont2=\\tensy \\scriptfont2=\\sevensy "
        "\\scriptscriptfont2=\\fivesy "
        "\\textfont3=\\tenex \\scriptfont3=\\tenex "
        "\\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu \\scriptspace=.5pt "
        "\\def\\O{\\mathchar\"0030 }\\def\\Y{\\mathchar\"0179 }"
        "\\def\\m#1{\\setbox0=\\hbox{$#1$}[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* The three shift paths: superscript alone, subscript alone, and the
           pair, whose clearance rule pushes the subscript further down. */
        "\\m{\\O^\\O}\\m{\\O_\\O}\\m{\\O^\\O_\\O}"
        /* A box nucleus drops the shift by \sup_drop and \sub_drop taken at
           the size the script will be set in. */
        "\\m{\\hbox{\\vrule height8pt depth0pt width0pt}^\\O}"
        "\\m{\\hbox{\\vrule height0pt depth8pt width0pt}_\\O}"
        /* A sub-formula that came to one character is that character; an
           \hbox is not, and two characters are not. */
        "\\m{{\\O}^\\O}\\m{\\hbox{$\\O$}^\\O}\\m{{{\\O}}^\\O}"
        "\\m{{\\O\\O}^\\O}"
        /* A subscript is cramped, so a superscript inside one takes the
           third parameter, not the second. */
        "\\m{\\O_{\\O^\\O}}\\m{\\O^{\\O^\\O}}\\m{\\O^{\\O^{\\O^\\O}}}"
        /* The italic correction is a kern only when there is no subscript;
           with both it displaces the superscript instead. */
        "\\m{\\Y^\\O}\\m{\\Y_\\O}\\m{\\Y^\\O_\\O}"
        /* A script mark with nothing before it makes an empty atom, and a
           deep superscript is pushed up to clear the axis. */
        "\\m{^\\O}"
        "\\m{\\O^{\\hbox{\\vrule height0pt depth8pt width0pt}}}"
        /* Below text style only the spaces that touch a large operator are
           inserted at all. */
        "\\m{\\O^{\\O\\mathchar\"2030 \\O}}\\m{\\O^{\\O\\mathchar\"6030 \\O}}"
        "\\m{\\O^{\\O\\mathchar\"1030 }}"
        /* \scriptspace goes after the script, once for a pair. */
        "\\scriptspace=0pt \\m{\\O^\\O}\\scriptspace=.5pt "
        /* An atom keeps its class when it takes a script. */
        "\\m{\\O\\mathchar\"2030 ^\\O\\O}%",
        "[9.48615pt|8.14003pt|0.0pt][9.48615pt|6.44444pt|1.49998pt]"
        "[9.48615pt|8.14003pt|2.4821pt][4.48613pt|10.03891pt|0.0pt]"
        "[4.48613pt|0.0pt|8.49998pt][9.48615pt|8.14003pt|0.0pt]"
        "[9.48615pt|8.48335pt|0.0pt][9.48615pt|8.14003pt|0.0pt]"
        "[14.48616pt|8.48335pt|0.0pt][13.38898pt|6.44444pt|1.77777pt]"
        "[13.38898pt|9.86893pt|0.0pt][17.29181pt|11.88669pt|0.0pt]"
        "[9.74773pt|8.14003pt|1.94444pt]"
        "[9.38895pt|4.30554pt|1.94444pt]"
        "[9.74773pt|8.14003pt|2.4821pt][4.48613pt|8.14003pt|0.0pt]"
        "[5.50002pt|9.07639pt|0.0pt][17.4584pt|8.14003pt|0.0pt]"
        "[17.4584pt|8.14003pt|0.0pt][14.83801pt|8.14003pt|0.0pt]"
        "[8.98615pt|8.14003pt|0.0pt][23.93051pt|8.14003pt|0.0pt]");
}

/* Alignments: the columns are as wide as their widest entry, the tabskip
   glue surrounds every one of them, and the rows are all the same width.
   See docs/DECISIONS.md, alignments. */
static int test_alignments(void)
{
    return run_snippet(
        "\\catcode`\\&=4 "
        "\\baselineskip=0pt \\lineskip=0pt \\lineskiplimit=0pt "
        "\\boxmaxdepth=16383.99998pt \\parindent=0pt \\tabskip=0pt "
        "\\def\\K#1{\\vrule width#1 height1pt depth0pt}"
        "\\def\\m#1{\\setbox0=\\vbox{#1}[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        "\\def\\mr#1{\\setbox0=\\vbox{#1}"
        "\\setbox1=\\vbox{\\unvbox0 \\global\\setbox2=\\lastbox}"
        "[\\the\\wd2|\\the\\ht2|\\the\\dp2]}"
        /* Two columns, each as wide as its widest entry. */
        "\\m{\\halign{#&#\\cr\\K{5pt}&\\K{7pt}\\cr\\K{11pt}&\\K{3pt}\\cr}}"
        /* \tabskip goes before the first column, between, and after the last:
           three of them for two columns. */
        "\\tabskip=2pt "
        "\\m{\\halign{#&#\\cr\\K{5pt}&\\K{7pt}\\cr\\K{11pt}&\\K{3pt}\\cr}}"
        "\\tabskip=0pt "
        /* The template counts towards the column's width. */
        "\\m{\\halign{\\K{1pt}#\\K{2pt}&#\\cr\\K{5pt}&\\K{7pt}\\cr}}"
        /* A row may stop early, and is still packed to the full width. */
        "\\m{\\halign{#&#\\cr\\K{5pt}\\cr\\K{11pt}&\\K{3pt}\\cr}}"
        "\\mr{\\halign{#&#\\cr\\K{11pt}&\\K{3pt}\\cr\\K{5pt}\\cr}}"
        /* \noalign puts vertical material between rows. */
        "\\m{\\halign{#&#\\cr\\K{5pt}&\\K{7pt}\\cr\\noalign{\\kern4pt}"
        "\\K{11pt}&\\K{3pt}\\cr}}"
        /* \omit drops the template of the entry it starts. */
        "\\m{\\halign{\\K{1pt}#\\K{2pt}&#\\cr\\omit\\K{5pt}&\\K{7pt}\\cr}}"
        "\\m{\\halign to 50pt{#&#\\cr\\K{5pt}&\\K{7pt}\\cr}}"
        /* \crcr after \cr adds nothing. */
        "\\m{\\halign{#&#\\cr\\K{5pt}&\\K{7pt}\\crcr}}"
        /* A spanned entry widens the last column it covers, and only by what
           the columns it covers still lack. */
        "\\m{\\halign{#&#\\cr\\K{2pt}&\\K{3pt}\\cr\\K{20pt}\\span\\omit\\cr}}"
        "\\m{\\halign{#&#\\cr\\K{11pt}&\\K{7pt}\\cr\\K{4pt}\\span\\omit\\cr}}"
        "\\tabskip=2pt "
        "\\m{\\halign{#&#\\cr\\K{2pt}&\\K{3pt}\\cr\\K{20pt}\\span\\omit\\cr}}"
        "\\tabskip=0pt "
        /* && repeats the rest of the preamble for as many columns as come. */
        "\\m{\\halign{#&&\\K{1pt}#\\cr\\K{2pt}&\\K{3pt}&\\K{4pt}\\cr}}"
        "\\m{\\halign{#&\\K{1pt}#\\K{2pt}\\cr\\K{5pt}&\\omit\\K{7pt}\\cr}}"
        "\\m{\\halign{#&#&#\\cr\\K{5pt}\\cr\\K{1pt}&\\K{9pt}\\cr"
        "\\K{2pt}&\\K{2pt}&\\K{6pt}\\cr}}"
        /* \tabskip may be set inside the preamble, for that boundary. */
        "\\m{\\halign{#\\tabskip=3pt&#\\tabskip=0pt\\cr\\K{5pt}&\\K{7pt}\\cr}}"
        "\\m{\\halign{#\\cr\\K{5pt}\\cr\\noalign{\\hrule height3pt}"
        "\\K{11pt}\\cr}}"
        /* Rows are separated by interline glue like any other boxes. */
        "\\baselineskip=10pt "
        "\\m{\\halign{#&#\\cr\\K{5pt}&\\K{7pt}\\cr\\K{11pt}&\\K{3pt}\\cr}}"
        "\\baselineskip=0pt "
        /* The deepest entry gives the row its depth. */
        "\\m{\\halign{#&#\\cr\\vrule width5pt height1pt depth2pt&\\K{7pt}"
        "\\cr}}%",
        "[18.0pt|2.0pt|0.0pt][24.0pt|2.0pt|0.0pt]"
        "[15.0pt|1.0pt|0.0pt][14.0pt|2.0pt|0.0pt]"
        "[14.0pt|1.0pt|0.0pt][18.0pt|6.0pt|0.0pt]"
        "[12.0pt|1.0pt|0.0pt][50.0pt|1.0pt|0.0pt]"
        "[12.0pt|1.0pt|0.0pt][20.0pt|2.0pt|0.0pt]"
        "[18.0pt|2.0pt|0.0pt][24.0pt|2.0pt|0.0pt]"
        "[11.0pt|1.0pt|0.0pt][12.0pt|1.0pt|0.0pt]"
        "[20.0pt|3.0pt|0.0pt][15.0pt|1.0pt|0.0pt]"
        "[11.0pt|5.0pt|0.0pt][18.0pt|11.0pt|0.0pt]"
        "[12.0pt|1.0pt|2.0pt]");
}

/* Display math: the skips and penalties around it, the centring, and
   \predisplaysize; see docs/DECISIONS.md, display-math. */
static int test_display_math(void)
{
    return run_snippet(
        "\\catcode`\\$=3 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\scriptfont0=\\tenrm "
        "\\scriptscriptfont0=\\tenrm "
        "\\textfont1=\\tenmi \\scriptfont1=\\tenmi "
        "\\scriptscriptfont1=\\tenmi "
        "\\textfont2=\\tensy \\scriptfont2=\\tensy "
        "\\scriptscriptfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont3=\\tenex "
        "\\scriptscriptfont3=\\tenex "
        "\\hsize=100pt \\parindent=0pt \\baselineskip=0pt \\lineskip=0pt "
        "\\lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\tolerance=10000 "
        "\\parfillskip=0pt plus1fil "
        "\\abovedisplayskip=3pt \\belowdisplayskip=4pt "
        "\\abovedisplayshortskip=1pt \\belowdisplayshortskip=2pt "
        "\\predisplaypenalty=101 \\postdisplaypenalty=102 \\tenrm "
        "\\def\\K#1{\\vrule width#1 height1pt depth0pt}"
        "\\def\\H#1{\\vrule width#1 height5pt depth0pt}"
        "\\long\\def\\m#1{\\setbox0=\\vbox{#1}"
        "[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* A line, the display between its skips, and a line after. */
        "\\m{\\noindent\\K{20pt}$$\\H{30pt}$$\\K{10pt}\\par}"
        "\\m{\\noindent\\K{20pt}\\par}"
        /* No line before the display at all: no line box, short skips, and
           the vbox is only as wide as the centred equation reaches. */
        "\\m{\\noindent$$\\H{30pt}$$\\par}"
        /* A short line before takes the short skips, a long one does not. */
        "\\m{\\noindent\\K{5pt}$$\\H{30pt}$$\\K{10pt}\\par}"
        "\\m{\\noindent\\K{95pt}$$\\H{30pt}$$\\K{10pt}\\par}"
        /* An equation wider than the display is squeezed to fit. */
        "\\m{\\noindent\\K{20pt}$$\\H{130pt}$$\\par}"
        "\\m{\\noindent\\K{20pt}$$\\H{30pt}$$$$\\H{30pt}$$\\par}"
        "\\m{\\noindent\\K{20pt}$$\\H{30pt}$$}"
        /* The parameters the display sets for itself. */
        "\\setbox0=\\vbox{\\noindent\\K{20pt}$$\\global\\dimen5=\\displaywidth "
        "\\global\\dimen6=\\displayindent \\global\\dimen7=\\predisplaysize "
        "\\global\\count5=\\ifinner1\\else0\\fi \\H{30pt}$$\\par}"
        "[\\the\\dimen5|\\the\\dimen6|\\the\\dimen7|\\the\\count5]"
        /* \predisplaysize reaches to the last visible node, plus two quads;
           trailing glue does not count, and infinite glue before a visible
           node makes it unknowable. */
        "\\setbox0=\\vbox{\\noindent\\K{20pt}\\hskip7pt$$"
        "\\global\\dimen7=\\predisplaysize \\H{30pt}$$\\par}[\\the\\dimen7]"
        "\\setbox0=\\vbox{\\noindent\\K{20pt}\\hskip7pt\\K{3pt}$$"
        "\\global\\dimen7=\\predisplaysize \\H{30pt}$$\\par}[\\the\\dimen7]"
        "\\setbox0=\\vbox{\\noindent$$\\global\\dimen7=\\predisplaysize "
        "\\H{30pt}$$\\par}[\\the\\dimen7]"
        "\\setbox0=\\vbox{\\noindent\\K{20pt}\\hskip7pt plus1pt\\K{3pt}$$"
        "\\global\\dimen7=\\predisplaysize \\H{30pt}$$\\par}[\\the\\dimen7]"
        /* \postdisplaypenalty sits between the equation and the skip below,
           and the equation box is its own natural width. */
        "\\setbox0=\\vbox{\\noindent\\K{20pt}$$\\H{30pt}$$\\par}"
        "\\setbox1=\\vbox{\\unvbox0 \\unskip \\global\\count5=\\lastpenalty "
        "\\unpenalty \\global\\setbox2=\\lastbox}"
        "[\\the\\count5|\\the\\wd2|\\the\\ht2]"
        /* A formula between single shifts is inner; a display is not. */
        "\\setbox0=\\hbox{$\\global\\count6=\\ifinner1\\else0\\fi x$}"
        "\\setbox0=\\vbox{\\noindent$\\global\\count7=\\ifinner1\\else0\\fi "
        "x$\\par}"
        "[\\the\\count6|\\the\\count7]%",
        "[100.0pt|14.0pt|0.0pt][100.0pt|1.0pt|0.0pt]"
        "[65.0pt|8.0pt|0.0pt][100.0pt|10.0pt|0.0pt]"
        "[100.0pt|14.0pt|0.0pt][100.0pt|13.0pt|0.0pt]"
        "[100.0pt|21.0pt|0.0pt][100.0pt|13.0pt|0.0pt]"
        "[100.0pt|0.0pt|40.00003pt|0][40.00003pt][50.00003pt]"
        "[-16383.99998pt][50.00003pt][102|30.0pt|5.0pt][1|1]");
}

/* Math styles and \mathchoice; see docs/DECISIONS.md, math-choices. */
static int test_math_choices(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 \\font\\sevenrm=cmr7 \\font\\sevenmi=cmmi7 "
        "\\font\\sevensy=cmsy7 \\font\\fiverm=cmr5 \\font\\fivemi=cmmi5 "
        "\\font\\fivesy=cmsy5 "
        "\\textfont0=\\tenrm \\scriptfont0=\\sevenrm "
        "\\scriptscriptfont0=\\fiverm "
        "\\textfont1=\\tenmi \\scriptfont1=\\sevenmi "
        "\\scriptscriptfont1=\\fivemi "
        "\\textfont2=\\tensy \\scriptfont2=\\sevensy "
        "\\scriptscriptfont2=\\fivesy "
        "\\textfont3=\\tenex \\scriptfont3=\\tenex "
        "\\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu "
        "\\scriptspace=.5pt \\nullfont "
        "\\def\\O{\\mathchar\"0030 }"
        "\\def\\m#1{\\setbox0=\\hbox{$#1$}[\\the\\wd0|\\the\\ht0]}"
        /* A style command moves the style of everything after it. */
        "\\m{\\O}\\m{\\scriptstyle\\O}\\m{\\scriptscriptstyle\\O}"
        "\\m{\\O\\scriptstyle\\O}\\m{\\displaystyle\\O}\\m{\\textstyle\\O}"
        /* \mathchoice keeps the branch the style in force asks for, and the
           branch is set at that style. */
        "\\m{\\mathchoice{\\O\\O\\O}{\\O\\O}{\\O}{}}"
        "\\m{\\scriptstyle\\mathchoice{\\O\\O\\O}{\\O\\O}{\\O}{}}"
        "\\m{\\O^{\\mathchoice{\\O\\O\\O}{\\O\\O}{\\O}{}}}"
        "\\m{\\displaystyle\\mathchoice{\\O\\O\\O}{\\O\\O}{\\O}{}}"
        "\\m{\\scriptscriptstyle"
        "\\mathchoice{\\O\\O\\O}{\\O\\O}{\\O}{\\O\\O\\O\\O}}"
        /* The branch is spliced, not boxed: a relation in it spaces as a
           relation, exactly as \mathrel does. */
        "\\m{\\O\\mathchoice{\\mathchar\"3030 }{\\mathchar\"3030 }"
        "{\\mathchar\"3030 }{\\mathchar\"3030 }\\O}"
        "\\m{\\O\\mathrel{\\O}\\O}"
        /* A script mark after a style command uses the moved style. */
        "\\m{\\scriptstyle\\O^\\O}\\m{\\O^{\\scriptstyle\\O}}"
        /* Every branch is read, so every branch's side effects happen. */
        "\\count0=0 "
        "\\m{\\mathchoice{\\global\\advance\\count0 by1 \\O}"
        "{\\global\\advance\\count0 by10 \\O}"
        "{\\global\\advance\\count0 by100 \\O}"
        "{\\global\\advance\\count0 by1000 \\O}}"
        "[\\the\\count0]%",
        "[5.00002pt|6.44444pt][3.98613pt|4.51111pt]"
        "[3.40283pt|3.22221pt][8.98615pt|6.44444pt]"
        "[5.00002pt|6.44444pt][5.00002pt|6.44444pt]"
        "[10.00003pt|6.44444pt][3.98613pt|4.51111pt]"
        "[9.48615pt|8.14003pt][15.00005pt|6.44444pt]"
        "[13.61133pt|3.22221pt][20.55547pt|6.44444pt]"
        "[20.55547pt|6.44444pt][7.88896pt|6.24pt]"
        "[9.48615pt|8.14003pt][5.00002pt|6.44444pt][1111]");
}

/* \badness after packing a box; see docs/DECISIONS.md, badness. */
static int test_badness(void)
{
    return run_snippet(
        "\\def\\K#1{\\vrule width#1 height1pt depth0pt}"
        "\\def\\b#1{\\setbox0=#1[\\the\\badness]}"
        "\\hbadness=10000 \\hfuzz=1000pt \\vbadness=10000 \\vfuzz=1000pt "
        /* Ten points of stretch, stretched by more and more of it. */
        "\\b{\\hbox to 10pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 11pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 12pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 13pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 15pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 17pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 20pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 25pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 30pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 10.5pt{\\K{10pt}\\hskip0pt plus10pt}}"
        /* Either side of the ratio the approximation gives up at. */
        "\\b{\\hbox to 53.4pt{\\K{10pt}\\hskip0pt plus10pt}}"
        "\\b{\\hbox to 53.5pt{\\K{10pt}\\hskip0pt plus10pt}}"
        /* Shrinking, and shrinking further than the glue allows. */
        "\\b{\\hbox to 9pt{\\K{10pt}\\hskip0pt minus10pt}}"
        "\\b{\\hbox to 5pt{\\K{10pt}\\hskip0pt minus10pt}}"
        "\\b{\\hbox to 0pt{\\K{10pt}\\hskip0pt minus10pt}}"
        "\\b{\\hbox to -5pt{\\K{10pt}\\hskip0pt minus10pt}}"
        /* No glue at all: infinitely bad unless it already fits. */
        "\\b{\\hbox to 20pt{\\K{10pt}}}"
        "\\b{\\hbox to 10pt{\\K{10pt}}}"
        "\\b{\\hbox to 5pt{\\K{10pt}}}"
        /* Infinite glue is never bad. */
        "\\b{\\hbox to 50pt{\\K{10pt}\\hskip0pt plus1fil}}"
        /* The branch the approximation takes for a large gap. */
        "\\b{\\hbox to 200pt{\\K{10pt}\\hskip0pt plus100pt}}"
        "\\b{\\hbox to 300pt{\\K{10pt}\\hskip0pt plus1000pt}}"
        "\\b{\\hbox to 2000pt{\\K{10pt}\\hskip0pt plus1000pt}}"
        "\\b{\\hbox to 200pt{\\K{10pt}\\hskip0pt plus60pt}}"
        /* A vertical list is measured the same way. */
        "\\b{\\vbox to 20pt{\\hrule height10pt \\vskip0pt plus10pt}}"
        "\\b{\\vbox to 30pt{\\hrule height10pt \\vskip0pt plus10pt}}"
        "\\b{\\vbox to 5pt{\\hrule height10pt \\vskip0pt minus10pt}}%",
        "[0][0][1][3][12][34][100][336][800][0][8151][10000][0]"
        "[12][100][1000000][10000][0][1000000][0][684][2][787]"
        "[3168][100][800][12]");
}

/* Breaking a paragraph into lines. Every word is a rule whose height says
   which word it is, so peeling the lines off the back of the vertical list
   reports where each line ended. See docs/DECISIONS.md, line-breaking. */
static int test_line_breaking(void)
{
    return run_snippet(
        "\\chardef\\keep=200 \\chardef\\ln=201 "
        "\\baselineskip=0pt \\lineskip=0pt \\lineskiplimit=0pt "
        "\\boxmaxdepth=16383.99998pt \\parindent=0pt \\pretolerance=-1 "
        "\\tolerance=10000 \\linepenalty=10 \\adjdemerits=10000 "
        "\\doublehyphendemerits=10000 \\finalhyphendemerits=5000 "
        "\\looseness=0 \\clubpenalty=0 \\widowpenalty=0 \\brokenpenalty=0 "
        "\\interlinepenalty=0 \\leftskip=0pt \\rightskip=0pt "
        "\\parfillskip=0pt plus1fil \\hbadness=10000 \\hfuzz=1000pt "
        "\\vbadness=10000 \\vfuzz=1000pt \\emergencystretch=0pt "
        "\\def\\W#1#2{\\vrule width#1pt height#2pt depth0pt}"
        "\\def\\G{\\hskip0pt plus2pt minus1pt }"
        "\\def\\peel{\\setbox\\keep=\\vbox{\\unvbox0 "
        "\\global\\setbox\\ln=\\lastbox \\unskip \\unpenalty}"
        "\\global\\setbox0=\\box\\keep"
        "\\ifvoid\\ln \\else(\\the\\ht\\ln)\\expandafter\\peel \\fi}"
        "\\long\\def\\run#1#2{[\\setbox0=\\vbox{\\hsize=#1 \\noindent#2\\par}"
        "\\peel]}"
        "\\long\\def\\runi#1#2{[\\setbox0=\\vbox{\\hsize=#1 \\indent#2\\par}"
        "\\peel]}"
        "\\def\\six{\\W{10}{1.01}\\G\\W{10}{1.02}\\G\\W{10}{1.03}\\G"
        "\\W{10}{1.04}\\G\\W{10}{1.05}\\G\\W{10}{1.06}}"
        "\\def\\uneven{\\W{20}{1.01}\\G\\W{5}{1.02}\\G\\W{5}{1.03}\\G"
        "\\W{20}{1.04}\\G\\W{5}{1.05}\\G\\W{5}{1.06}}"
        "\\def\\five{\\W{9}{1.01}\\G\\W{9}{1.02}\\G\\W{9}{1.03}\\G"
        "\\W{9}{1.04}\\G\\W{9}{1.05}}"
        "\\def\\four{\\W{10}{1.01}\\G\\W{10}{1.02}\\G\\W{10}{1.03}\\G"
        "\\W{10}{1.04}}"
        /* Six equal words at widths that call for one to six lines. */
        "\\run{100pt}{\\six}\\run{40pt}{\\six}\\run{25pt}{\\six}"
        "\\run{35pt}{\\six}\\run{22pt}{\\six}\\run{12pt}{\\six}"
        /* Uneven words, where filling each line as far as it goes is not
           what the reference does. */
        "\\run{32pt}{\\uneven}\\run{30pt}{\\uneven}\\run{26pt}{\\uneven}"
        "\\run{60pt}{\\uneven}"
        /* A penalty is a breakpoint, and its size counts in the demerits. */
        "\\run{45pt}{\\W{10}{1.01}\\G\\W{10}{1.02}\\penalty-200 \\G"
        "\\W{10}{1.03}\\G\\W{10}{1.04}}"
        "\\run{45pt}{\\W{10}{1.01}\\G\\W{10}{1.02}\\penalty200 \\G"
        "\\W{10}{1.03}\\G\\W{10}{1.04}}"
        /* \leftskip and \rightskip are part of every line. */
        "\\rightskip=5pt \\run{35pt}{\\six}\\rightskip=0pt "
        "\\leftskip=5pt \\run{35pt}{\\six}\\leftskip=0pt "
        /* Rigid glue still offers breaks, at a badness of exactly 10000. */
        "\\run{35pt}{\\W{10}{1.01}\\hskip2pt \\W{10}{1.02}\\hskip2pt "
        "\\W{10}{1.03}\\hskip2pt \\W{10}{1.04}}"
        "\\run{40pt}{\\W{9}{1.01}\\G\\W{9}{1.02}\\G\\W{9}{1.03}\\G"
        "\\W{9}{1.04}\\G\\W{30}{1.05}}"
        /* A break of -10000 is taken even when the paragraph would fit on
           one line; 10000 stops the glue after it being a break at all. */
        "\\run{100pt}{\\W{10}{1.01}\\G\\W{10}{1.02}\\penalty-10000 \\G"
        "\\W{10}{1.03}\\G\\W{10}{1.04}}"
        "\\run{25pt}{\\W{10}{1.01}\\G\\W{10}{1.02}\\penalty10000 \\G"
        "\\W{10}{1.03}\\G\\W{10}{1.04}}"
        /* \pretolerance accepts a first pass; a tolerance nothing can meet
           falls through to the pass that takes what it can get. */
        "\\pretolerance=10000 \\run{35pt}{\\four}\\pretolerance=-1 "
        "\\tolerance=1 \\run{35pt}{\\four}\\tolerance=10000 "
        "\\parindent=15pt \\runi{35pt}{\\four}\\parindent=0pt "
        /* Demerits that push towards evenness, and towards fewer lines. */
        "\\adjdemerits=0 \\run{31pt}{\\five}"
        "\\adjdemerits=1000000 \\run{31pt}{\\five}\\adjdemerits=10000 "
        "\\linepenalty=1000 \\run{31pt}{\\five}\\linepenalty=10 "
        /* Hanging indentation narrows the lines it covers. */
        "\\hangindent=10pt \\hangafter=1 \\run{40pt}{\\five}"
        "\\hangindent=0pt%",
        "[(1.06pt)][(1.06pt)(1.04pt)][(1.06pt)(1.04pt)(1.02pt)]"
        "[(1.06pt)(1.03pt)][(1.06pt)(1.04pt)(1.02pt)]"
        "[(1.06pt)(1.05pt)(1.04pt)(1.03pt)(1.02pt)(1.01pt)]"
        "[(1.06pt)(1.03pt)][(1.06pt)(1.03pt)]"
        "[(1.06pt)(1.04pt)(1.02pt)][(1.06pt)][(1.04pt)][(1.04pt)]"
        "[(1.06pt)(1.03pt)][(1.06pt)(1.03pt)][(1.04pt)(1.03pt)]"
        "[(1.05pt)(1.04pt)][(1.04pt)(1.02pt)]"
        "[(1.04pt)(1.03pt)(1.01pt)][(1.04pt)(1.03pt)][(1.04pt)]"
        "[(1.04pt)(1.02pt)][(1.05pt)(1.03pt)][(1.05pt)(1.03pt)]"
        "[(1.05pt)(1.03pt)][(1.05pt)(1.04pt)]");
}

/* \accent, and a displaced box in a formula; see docs/DECISIONS.md,
   accents. */
static int test_accents(void)
{
    return run_snippet(
        "\\catcode`\\$=3 "
        "\\font\\tenrm=cmr10 \\font\\tenti=cmti10 \\font\\tenmi=cmmi10 "
        "\\font\\tensy=cmsy10 \\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\textfont1=\\tenmi \\textfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont0=\\tenrm \\scriptfont1=\\tenmi "
        "\\scriptfont2=\\tensy \\scriptfont3=\\tenex "
        "\\scriptscriptfont0=\\tenrm \\scriptscriptfont1=\\tenmi "
        "\\scriptscriptfont2=\\tensy \\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu \\tenrm "
        "\\def\\R{\\vrule width2pt height1pt depth0pt}"
        "\\def\\m#1{\\setbox0=\\hbox{#1}[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* The kerns around an accent cancel, so the pair is exactly as wide
           as the character underneath. */
        "\\m{o}\\m{\\accent23 o}\\m{\\accent23 A}\\m{\\accent23 g}"
        "\\m{\\accent22 o}"
        /* An accent with nothing to sit on is an ordinary character, and it
           only ever covers one. */
        "\\m{\\accent23}\\m{\\accent23 oo}\\m{\\accent23 \\char111 }"
        /* The slant of the font moves the accent but not the width. */
        "\\tenti \\m{\\accent23 o}\\tenrm "
        "\\fontdimen1\\tenrm=0.5pt \\m{\\accent23 o}\\fontdimen1\\tenrm=0pt "
        /* \raise and \lower work in a formula, where the box they make is an
           ordinary atom. */
        "\\m{$\\raise5pt\\hbox{\\R}$}\\m{$\\lower3pt\\hbox{\\R}$}"
        "\\m{$\\mathchar\"0030 \\raise5pt\\hbox{\\R}\\mathchar\"0030 $}"
        "\\m{$\\mathchar\"0030 \\mathrel{\\raise5pt\\hbox{\\R}}"
        "\\mathchar\"0030 $}"
        "\\m{\\raise5pt\\hbox{\\R}}%",
        "[5.00002pt|4.30554pt|0.0pt][5.00002pt|6.94444pt|0.0pt]"
        "[7.50002pt|9.47221pt|0.0pt]"
        "[5.00002pt|6.94444pt|1.94444pt]"
        "[5.00002pt|5.67776pt|0.0pt][7.50002pt|6.94444pt|0.0pt]"
        "[10.00003pt|6.94444pt|0.0pt][5.00002pt|6.94444pt|0.0pt]"
        "[5.11108pt|6.94444pt|0.0pt][5.00002pt|6.94444pt|0.0pt]"
        "[2.0pt|6.0pt|0.0pt][2.0pt|0.0pt|3.0pt]"
        "[12.00003pt|6.44444pt|0.0pt][17.55545pt|6.44444pt|0.0pt]"
        "[2.0pt|6.0pt|0.0pt]");
}

/* Equation numbers; see docs/DECISIONS.md, equation-numbers. */
static int test_equation_numbers(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\chardef\\keep=200 \\chardef\\ln=201 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\textfont1=\\tenmi \\textfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont0=\\tenrm \\scriptfont1=\\tenmi "
        "\\scriptfont2=\\tensy \\scriptfont3=\\tenex "
        "\\scriptscriptfont0=\\tenrm \\scriptscriptfont1=\\tenmi "
        "\\scriptscriptfont2=\\tensy \\scriptscriptfont3=\\tenex "
        "\\hsize=100pt \\parindent=0pt \\baselineskip=0pt \\lineskip=0pt "
        "\\lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\tolerance=10000 "
        "\\parfillskip=0pt plus1fil \\abovedisplayskip=3pt "
        "\\belowdisplayskip=4pt \\abovedisplayshortskip=1pt "
        "\\belowdisplayshortskip=2pt \\predisplaypenalty=101 "
        "\\postdisplaypenalty=102 \\tenrm \\hbadness=10000 \\hfuzz=1000pt "
        "\\vbadness=10000 \\vfuzz=1000pt "
        "\\def\\K#1{\\vrule width#1 height1pt depth0pt}"
        "\\def\\H#1{\\vrule width#1 height5pt depth0pt}"
        "\\long\\def\\r#1{\\setbox0=\\vbox{#1}"
        "[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        "\\long\\def\\rl#1{\\setbox0=\\vbox{#1}"
        "\\setbox\\keep=\\vbox{\\unvbox0 \\unskip \\unpenalty "
        "\\global\\setbox\\ln=\\lastbox}[\\the\\wd\\ln|\\the\\ht\\ln]}"
        /* Without a number, for comparison. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}$$\\par}"
        /* A number that fits sits beside the equation on a line that runs
           from the equation's left edge to the display's right edge. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\eqno\\H{8pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{30pt}\\eqno\\H{8pt}$$\\par}"
        /* A number that does not fit -- counting a quad of space between --
           goes on a line of its own, and the glue below the display goes. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\eqno\\H{60pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{30pt}\\eqno\\H{60pt}$$\\par}"
        "\\r{\\noindent\\K{20pt}$$\\H{95pt}\\eqno\\H{8pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{95pt}\\eqno\\H{8pt}$$\\par}"
        /* \leqno puts it on the left and does not shift the line. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\leqno\\H{8pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{30pt}\\leqno\\H{8pt}$$\\par}"
        /* An equation wider than the display is squeezed, and its number
           still goes below. */
        "\\r{\\noindent\\K{20pt}$$\\H{130pt}\\eqno\\H{8pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{130pt}\\eqno\\H{8pt}$$\\par}"
        /* A number wide enough to crowd the equation moves it left. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\eqno\\H{20pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{30pt}\\eqno\\H{20pt}$$\\par}"
        /* A \leqno that does not fit goes above, in place of the glue. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\leqno\\H{60pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{30pt}\\leqno\\H{60pt}$$\\par}"
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\leqno\\H{20pt}$$\\par}"
        "\\rl{\\noindent\\K{20pt}$$\\H{30pt}\\leqno\\H{20pt}$$\\par}"
        "\\r{$$\\H{30pt}\\eqno\\H{8pt}$$\\par}"
        "\\rl{$$\\H{30pt}\\eqno\\H{8pt}$$\\par}"
        /* A formula inside a box inside a display is its own formula. */
        "\\r{\\noindent\\K{20pt}$$\\H{30pt}\\hbox{$\\H{4pt}$}$$\\par}%",
        "[100.0pt|13.0pt|0.0pt][100.0pt|13.0pt|0.0pt]"
        "[65.0pt|5.0pt][100.0pt|14.0pt|0.0pt][60.0pt|5.0pt]"
        "[100.0pt|14.0pt|0.0pt][8.0pt|5.0pt]"
        "[100.0pt|13.0pt|0.0pt][65.0pt|5.0pt]"
        "[100.0pt|14.0pt|0.0pt][8.0pt|5.0pt]"
        "[100.0pt|13.0pt|0.0pt][75.0pt|5.0pt]"
        "[100.0pt|15.0pt|0.0pt][30.0pt|5.0pt]"
        "[100.0pt|13.0pt|0.0pt][75.0pt|5.0pt]"
        "[100.0pt|8.0pt|0.0pt][65.0pt|5.0pt]"
        "[100.0pt|13.0pt|0.0pt]");
}

/* \vcenter, and paragraphs that a vertical list starts by itself; see
   docs/DECISIONS.md, vcenter. */
static int test_vcenter(void)
{
    return run_snippet(
        "\\catcode`\\$=3 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\textfont1=\\tenmi \\textfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont0=\\tenrm \\scriptfont1=\\tenmi "
        "\\scriptfont2=\\tensy \\scriptfont3=\\tenex "
        "\\scriptscriptfont0=\\tenrm \\scriptscriptfont1=\\tenmi "
        "\\scriptscriptfont2=\\tensy \\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu "
        "\\baselineskip=0pt \\lineskip=0pt \\lineskiplimit=0pt "
        "\\boxmaxdepth=16383.99998pt \\hsize=100pt \\parindent=7pt "
        "\\tolerance=10000 \\parfillskip=0pt plus1fil \\pretolerance=-1 "
        "\\leftskip=0pt \\rightskip=0pt \\hbadness=10000 \\hfuzz=1000pt "
        "\\def\\R#1#2{\\hrule height#1 depth#2}"
        "\\def\\m#1{\\setbox0=#1[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        "\\nullfont "
        /* A vcentred box hangs on the axis: its height is the axis plus half
           of what it measures, and the rest becomes depth. */
        "\\m{\\hbox{$\\vcenter{\\R{10pt}{0pt}}$}}"
        "\\m{\\hbox{$\\vcenter{\\R{10pt}{4pt}}$}}"
        "\\m{\\hbox{$\\vcenter{\\R{7pt}{0pt}}$}}"
        "\\m{\\hbox{$\\vcenter{\\R{0pt}{0pt}}$}}"
        "\\m{\\hbox{$\\vcenter to 20pt{\\R{10pt}{0pt}}$}}"
        "\\m{\\hbox{$\\vcenter spread 5pt{\\R{10pt}{0pt}}$}}"
        /* Moving the axis moves it, and an axis high enough leaves the box
           reaching above the baseline with nothing below it. */
        "\\fontdimen22\\tensy=0pt \\m{\\hbox{$\\vcenter{\\R{10pt}{0pt}}$}}"
        "\\fontdimen22\\tensy=6pt \\m{\\hbox{$\\vcenter{\\R{10pt}{0pt}}$}}"
        "\\fontdimen22\\tensy=2.5pt "
        /* It is an ordinary atom. */
        "\\m{\\hbox{$\\mathchar\"0030 \\vcenter{\\R{10pt}{0pt}}"
        "\\mathchar\"0030 $}}"
        "\\m{\\hbox{$\\mathchar\"0030 \\mathrel{\\vcenter{\\R{10pt}{0pt}}}"
        "\\mathchar\"0030 $}}"
        "\\m{\\hbox{$\\vcenter{\\R{11pt}{0pt}}$}}"
        "\\tenrm "
        /* A character in a vertical list starts a paragraph, and the brace
           that ends the box ends the paragraph while \hsize still holds. */
        "\\m{\\vbox{\\hsize=100pt A}}"
        "\\m{\\vbox{\\hsize=100pt AB}}"
        "\\m{\\vbox{\\hsize=20pt AB AB AB}}"
        "\\m{\\hbox{\\vbox{\\hsize=100pt A}}}"
        "\\m{\\vbox{\\hsize=100pt \\hbox{A}}}"
        "\\m{\\vbox{\\hsize=100pt \\noindent A}}%",
        "[0.0pt|7.5pt|2.5pt][0.0pt|9.5pt|4.5pt]"
        "[0.0pt|6.0pt|1.0pt][0.0pt|2.5pt|0.0pt]"
        "[0.0pt|12.5pt|7.5pt][0.0pt|10.0pt|5.0pt]"
        "[0.0pt|5.0pt|5.0pt][0.0pt|11.0pt|0.0pt]"
        "[10.00003pt|7.5pt|2.5pt][15.55545pt|7.5pt|2.5pt]"
        "[0.0pt|8.0pt|3.0pt][100.0pt|6.83331pt|0.0pt]"
        "[100.0pt|6.83331pt|0.0pt][20.0pt|20.49994pt|0.0pt]"
        "[100.0pt|6.83331pt|0.0pt][7.50002pt|6.83331pt|0.0pt]"
        "[100.0pt|6.83331pt|0.0pt]");
}

/* An alignment entry whose template opens a box, margin kerns, and
   \ifincsname; see docs/DECISIONS.md, alignment-entries. */
static int test_alignment_entries(void)
{
    return run_snippet(
        "\\catcode`\\&=4 "
        "\\font\\tenrm=cmr10 \\tenrm \\hsize=100pt \\parindent=0pt "
        "\\tolerance=10000 \\baselineskip=0pt \\lineskip=0pt "
        "\\lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\tabskip=0pt "
        "\\parfillskip=0pt plus1fil \\hbadness=10000 \\hfuzz=1000pt "
        "\\pretolerance=-1 \\leftskip=0pt \\rightskip=0pt "
        "\\let\\bgroup={\\let\\egroup=}"
        "\\long\\def\\m#1{\\setbox0=#1[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* A box in the entry's own material, then the same box opened by the
           template and closed by it -- where the tab arrives while the box is
           still open. */
        "\\m{\\vbox{\\halign{#&#\\cr\\vtop{\\hsize=50pt A\\par}&B\\cr}}}"
        "\\m{\\vbox{\\halign{\\vtop\\bgroup\\hsize=50pt #\\par\\egroup&#\\cr"
        " A&B\\cr}}}"
        "\\m{\\vbox{\\halign{\\vtop\\bgroup\\hsize=30pt #\\par\\egroup&#\\cr"
        " AB AB AB&B\\cr}}}"
        "\\m{\\vbox{\\halign{\\hbox\\bgroup#\\egroup&#\\cr A&B\\cr}}}"
        /* Nothing protrudes, so nothing hangs in the margin. */
        "\\lpcode\\tenrm`A=100 \\rpcode\\tenrm`A=200 \\pdfprotrudechars=2 "
        "\\setbox0=\\hbox{AB}\\dimen1=\\leftmarginkern0 "
        "\\dimen2=\\rightmarginkern0 [\\the\\dimen1|\\the\\dimen2]"
        /* \ifincsname is true only while a control sequence name is built. */
        "\\def\\p{\\ifincsname Y\\else N\\fi}[\\p]"
        "\\expandafter\\def\\csname x\\p\\endcsname{}"
        "[\\ifx\\xY\\relax R\\else\\meaning\\xY\\fi]"
        "\\edef\\z{\\p}[\\meaning\\z]%",
        "[57.08336pt|6.83331pt|0.0pt][57.08336pt|6.83331pt|0.0pt]"
        "[37.08336pt|6.83331pt|13.66663pt]"
        "[14.58337pt|6.83331pt|0.0pt][0.0pt|0.0pt][N][macro:->]"
        "[macro:->N]");
}

/* \delimiter on its own, and a character the font does not have; see
   docs/DECISIONS.md, delimiters. */
static int test_delimiters(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\textfont1=\\tenmi \\textfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont0=\\tenrm \\scriptfont1=\\tenmi "
        "\\scriptfont2=\\tensy \\scriptfont3=\\tenex "
        "\\scriptscriptfont0=\\tenrm \\scriptscriptfont1=\\tenmi "
        "\\scriptscriptfont2=\\tensy \\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu "
        "\\scriptspace=.5pt \\nullfont "
        "\\def\\m#1{\\setbox0=\\hbox{$#1$}"
        "[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* A delimiter used on its own is its small variant, which is the
           number's top fifteen bits -- the same thing \mathchar would make
           of them. */
        "\\m{\\delimiter\"426830A }\\m{\\mathchar\"4268 }"
        "\\m{\\delimiter\"028300 }"
        /* A character the font does not have contributes nothing at all. */
        "\\m{\\mathchar\"0283 }"
        /* The class in the number still decides the spacing. */
        "\\m{\\mathchar\"0030 \\delimiter\"426830A \\mathchar\"0030 }"
        "\\m{\\mathchar\"0030 \\mathopen{\\mathchar\"0030 }\\mathchar\"0030 }"
        "\\m{\\delimiter\"0 }"
        /* Scripts attach to it as they would to any atom. */
        "\\m{\\delimiter\"426830A ^\\mathchar\"0030 }"
        /* A missing character still leaves an atom that spaces. */
        "\\m{\\mathchar\"0030 \\mathchar\"0283 \\mathchar\"0030 }%",
        "[3.8889pt|7.5pt|2.5pt][3.8889pt|7.5pt|2.5pt]"
        "[3.8889pt|7.5pt|2.5pt][0.0pt|0.0pt|0.0pt]"
        "[13.88893pt|7.5pt|2.5pt][15.00005pt|6.44444pt|0.0pt]"
        "[6.25002pt|6.83331pt|0.0pt][9.38892pt|10.07336pt|2.5pt]"
        "[10.00003pt|6.44444pt|0.0pt]");
}

/* \left and \right, and the extensible recipes the tallest delimiters are
   built from; see docs/DECISIONS.md, extensible-delimiters. */
static int test_left_right(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 "
        "\\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 "
        "\\font\\tenex=cmex10 "
        "\\textfont0=\\tenrm \\textfont1=\\tenmi \\textfont2=\\tensy "
        "\\textfont3=\\tenex \\scriptfont0=\\tenrm \\scriptfont1=\\tenmi "
        "\\scriptfont2=\\tensy \\scriptfont3=\\tenex "
        "\\scriptscriptfont0=\\tenrm \\scriptscriptfont1=\\tenmi "
        "\\scriptscriptfont2=\\tensy \\scriptscriptfont3=\\tenex "
        "\\thinmuskip=3mu \\medmuskip=4mu \\thickmuskip=5mu "
        "\\scriptspace=.5pt \\nullfont "
        "\\delimiterfactor=901 \\delimitershortfall=5pt "
        "\\nulldelimiterspace=1.2pt "
        "\\delcode`\\(=\"028300 \\delcode`\\)=\"029301 "
        "\\def\\V#1{\\vrule width2pt height#1 depth0pt}"
        "\\def\\m#1{\\setbox0=\\hbox{$#1$}"
        "[\\the\\wd0|\\the\\ht0|\\the\\dp0]}"
        /* The smallest variant serves until the contents outgrow it, then
           each larger one in the chain, then a delimiter built from pieces --
           which is where the width stops growing. */
        "\\m{\\left(\\V{1pt}\\right)}\\m{\\left(\\V{5pt}\\right)}"
        "\\m{\\left(\\V{10pt}\\right)}\\m{\\left(\\V{15pt}\\right)}"
        "\\m{\\left(\\V{20pt}\\right)}\\m{\\left(\\V{30pt}\\right)}"
        "\\m{\\left(\\V{50pt}\\right)}"
        /* A full stop names no delimiter, and leaves \nulldelimiterspace. */
        "\\m{\\left.\\V{10pt}\\right.}"
        /* The whole thing is an inner atom. */
        "\\m{\\mathchar\"0030 \\left(\\V{10pt}\\right)\\mathchar\"0030 }"
        "\\m{\\mathchar\"0030 \\mathinner{\\mathchar\"0030 }"
        "\\mathchar\"0030 }"
        "\\m{\\left(\\V{10pt}\\right)^\\mathchar\"0030 }"
        /* Both parameters move the size the delimiter has to reach. */
        "\\delimiterfactor=500 \\m{\\left(\\V{15pt}\\right)}"
        "\\delimiterfactor=901 \\delimitershortfall=50pt "
        "\\m{\\left(\\V{15pt}\\right)}\\delimitershortfall=5pt "
        /* \delimiter names one just as a character with a \delcode does. */
        "\\m{\\left\\delimiter\"028300 \\V{15pt}"
        "\\right\\delimiter\"029301 }%",
        "[9.7778pt|7.5pt|2.5pt][9.7778pt|7.5pt|2.5pt]"
        "[13.94446pt|11.50008pt|6.50009pt]"
        "[16.72229pt|15.0pt|9.50012pt]"
        "[19.50003pt|20.50017pt|15.50017pt]"
        "[19.50003pt|30.0pt|24.50026pt]"
        "[19.50003pt|50.0pt|42.50044pt][4.4pt|10.0pt|0.0pt]"
        "[27.27774pt|11.50008pt|6.50009pt]"
        "[18.3333pt|6.44444pt|0.0pt]"
        "[19.44447pt|14.08344pt|6.50009pt]"
        "[16.72229pt|15.0pt|9.50012pt]"
        "[16.72229pt|15.0pt|9.50012pt]"
        "[16.72229pt|15.0pt|9.50012pt]");
}

/* A control sequence \let to a character is that character; see
   docs/DECISIONS.md, implicit-characters. */
static int test_implicit_characters(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\&=4 \\catcode`\\^=7 \\catcode`\\_=8 "
        "\\let\\a={\\let\\b=}\\let\\c=$\\let\\d=&\\let\\e=#\\let\\f=^"
        "\\let\\g=_\\let\\h=x\\let\\i='\\let\\j=\\relax"
        "\\catcode`\\~=13 \\let~=y\\let\\k=~"
        "\\def\\eat#1#2{}"
        "\\def\\q{[\\meaning\\tk][\\ifx'\\tk Y\\else N\\fi]\\eat}"
        "\\def\\p{\\futurelet\\tk\\q}"
        /* Each category has a name of its own. */
        "[\\meaning\\a][\\meaning\\b][\\meaning\\c][\\meaning\\d]"
        "[\\meaning\\e][\\meaning\\f][\\meaning\\g][\\meaning\\h]"
        "[\\meaning\\i][\\meaning\\j][\\meaning\\k]"
        /* An explicit character reports the same way. */
        "[\\meaning x][\\meaning ']"
        /* \ifx sees through the alias, in either order. */
        "[\\ifx\\h x Y\\else N\\fi][\\ifx x\\h Y\\else N\\fi]"
        "[\\ifx\\h y Y\\else N\\fi][\\ifx\\h\\i Y\\else N\\fi]"
        "[\\ifx\\a\\a Y\\else N\\fi]"
        /* Which is what \futurelet plus \ifx needs, and what LaTeX's run of
           primes is built on. */
        "\\p'x%",
        "[begin-group character {][end-group character }]"
        "[math shift character $][alignment tab character &]"
        "[macro parameter character #]"
        "[superscript character ^][subscript character _]"
        "[the letter x][the character '][\\relax]"
        "[the letter y][the letter x][the character '][ Y][Y]"
        "[N][N][Y][the character '][Y]");
}

/* Preamble forms: a tab that starts it, and \span that expands into it.
   See docs/DECISIONS.md, preamble-expansion. */
static int test_preamble_forms(void)
{
    return run_snippet(
        "\\catcode`\\&=4 "
        "\\baselineskip=0pt \\lineskip=0pt \\lineskiplimit=0pt "
        "\\boxmaxdepth=16383.99998pt \\parindent=0pt \\tabskip=0pt "
        "\\hbadness=10000 \\hfuzz=1000pt "
        "\\def\\K#1{\\vrule width#1pt height1pt depth0pt}"
        "\\def\\pre{&\\K{1}##\\K{2}}"
        "\\def\\two{##&\\K{3}##}"
        "\\long\\def\\m#1{\\setbox0=\\vbox{#1}[\\the\\wd0|\\the\\ht0]}"
        /* A preamble that begins with a tab repeats from its first column. */
        "\\m{\\halign{&\\K{1}#\\K{2}\\cr\\K{5}&\\K{7}&\\K{3}\\cr}}"
        "\\m{\\halign{&#\\cr\\K{5}&\\K{7}\\cr\\K{11}&\\K{3}\\cr}}"
        "\\m{\\halign{#&&\\K{1}#\\cr\\K{2}&\\K{3}&\\K{4}\\cr}}"
        /* \span expands the token after it, so a preamble may be a macro --
           which is how amsmath hands one to \halign. */
        "\\m{\\halign{\\span\\pre\\cr\\K{5}&\\K{7}&\\K{3}\\cr}}"
        "\\m{\\halign{\\span\\two\\cr\\K{5}&\\K{7}\\cr}}"
        "\\m{\\halign{#\\span\\relax&\\K{3}#\\cr\\K{5}&\\K{7}\\cr}}%",
        "[24.0pt|1.0pt][18.0pt|2.0pt][11.0pt|1.0pt]"
        "[24.0pt|1.0pt][15.0pt|1.0pt][15.0pt|1.0pt]");
}

/* An alignment can be the whole of a display; see docs/DECISIONS.md,
   display-alignments. */
static int test_display_alignments(void)
{
    return run_snippet(
        "\\catcode`\\&=4 \\catcode`\\$=3 \\parindent=0pt \\baseline"
        "skip=0pt \\lineskip=0pt \\lineskiplimit=0pt \\boxmaxdepth="
        "16383.99998pt \\parskip=0pt \\tabskip=0pt \\parfillskip=0p"
        "t plus1fil \\tolerance=10000 \\abovedisplayskip=13pt \\bel"
        "owdisplayskip=17pt \\abovedisplayshortskip=5pt \\belowdisp"
        "layshortskip=7pt \\predisplaypenalty=101 \\postdisplaypena"
        "lty=103 \\hbadness=10000 \\hfuzz=1000pt \\vbadness=10000 "
        "\\vfuzz=1000pt \\hsize=200pt \\def\\K#1{\\vrule width#1pt "
        "height1pt depth0pt}\\setbox0=\\vbox{\\noindent\\K{20}$$\\h"
        "align{\\K{1}#\\K{2}&\\K{4}#\\K{8}\\cr\\K{5}&\\K{7}\\cr\\K{"
        "11}&\\K{3}\\cr}$$\\K{30}}[1|\\the\\wd0|\\the\\ht0|\\the\\d"
        "p0]\\setbox0=\\vbox{\\noindent$$\\halign{\\K{1}#\\K{2}&\\K"
        "{4}#\\K{8}\\cr\\K{5}&\\K{7}\\cr\\K{11}&\\K{3}\\cr}$$}[2|\\"
        "the\\wd0|\\the\\ht0]\\setbox0=\\vbox{\\noindent$$\\halign "
        "to150pt{\\K{1}#\\K{2}\\tabskip=0pt plus1fil&\\K{4}#\\K{8}"
        "\\cr\\K{5}&\\K{7}\\cr\\K{11}&\\K{3}\\cr}$$}[3|\\the\\wd0|"
        "\\the\\ht0]\\baselineskip=20pt \\setbox0=\\vbox{\\noindent"
        "$$\\halign{\\K{1}#\\K{2}&\\K{4}#\\K{8}\\cr\\vrule width5pt"
        " height1pt depth2pt&\\vrule width7pt height1pt depth2pt\\c"
        "r\\vrule width11pt height1pt depth2pt&\\vrule width3pt hei"
        "ght1pt depth2pt\\cr}$$\\K{30}}[4|\\the\\wd0|\\the\\ht0|\\t"
        "he\\dp0]\\baselineskip=0pt \\setbox0=\\vbox{\\noindent$$\\"
        "halign{\\K{1}#\\K{2}&\\K{4}#\\K{8}\\cr\\K{5}&\\K{7}\\cr\\n"
        "oalign{\\kern6pt}\\K{11}&\\K{3}\\cr}$$}[5|\\the\\wd0|\\the"
        "\\ht0]\\count1=3 \\setbox0=\\vbox{\\noindent$$\\count1=7 "
        "\\tabskip=5pt\\halign{\\K{1}#\\cr\\K{5}\\cr\\K{7}\\cr}$$\\"
        "global\\count2=\\count1 \\global\\skip1=\\tabskip}[6|\\the"
        "\\count2|\\the\\skip1][7|\\the\\count1|\\the\\tabskip|\\th"
        "e\\wd0|\\the\\ht0]\\setbox0=\\vbox{\\noindent$$\\halign{#"
        "\\cr\\K{5}\\cr}\\abovedisplayskip=40pt \\predisplaypenalty"
        "=77 \\belowdisplayskip=41pt $$}[8|\\the\\wd0|\\the\\ht0]\\"
        "setbox0=\\vbox{\\noindent$$\\halign{&\\K{1}#\\K{2}\\cr\\K{"
        "5}&\\K{7}\\cr}$$}[9|\\the\\wd0|\\the\\ht0]%",
        "[1|200.0pt|34.0pt|0.0pt][2|33.0pt|32.0pt][3|150.0pt|32.0pt"
        "][4|200.0pt|71.0pt|0.0pt][5|33.0pt|38.0pt][6|3|0.0pt][7|3|"
        "0.0pt|18.0pt|32.0pt][8|5.0pt|82.0pt][9|18.0pt|31.0pt]");
}

/* \everycr fires after the preamble and after every \cr that ends a
   row; see docs/DECISIONS.md, everycr. */
static int test_every_cr(void)
{
    return run_snippet(
        "\\catcode`\\&=4 \\parindent=0pt \\baselineskip=0pt \\lines"
        "kip=0pt \\lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\"
        "parskip=0pt \\tabskip=0pt \\hbadness=10000 \\hfuzz=1000pt "
        "\\vbadness=10000 \\vfuzz=1000pt \\hsize=200pt \\def\\K#1{"
        "\\vrule width#1pt height1pt depth0pt}\\def\\C{\\everycr{\\"
        "noalign{\\global\\advance\\count0by1 }}}\\count0=0 \\setbo"
        "x9=\\vbox{\\C\\halign{#\\cr\\K{5}\\cr\\K{7}\\cr}}[1|\\the"
        "\\count0]\\count0=0 \\setbox9=\\vbox{\\C\\halign{#\\cr}}[2"
        "|\\the\\count0]\\count0=0 \\setbox9=\\vbox{\\C\\halign{#\\"
        "cr\\K{5}\\crcr}}[3|\\the\\count0]\\count0=0 \\setbox9=\\vb"
        "ox{\\C\\halign{#\\cr\\K{5}\\cr\\crcr}}[4|\\the\\count0]\\c"
        "ount0=0 \\setbox9=\\vbox{\\C\\halign{#\\cr\\K{5}\\cr\\noal"
        "ign{}\\crcr}}[5|\\the\\count0]\\count0=0 \\setbox9=\\vbox{"
        "\\C\\halign{#\\cr\\K{5}\\cr\\noalign{}\\K{7}\\cr}}[6|\\the"
        "\\count0]\\count0=0 \\setbox9=\\vbox{\\C\\halign{#&#\\cr\\"
        "K{5}&\\K{6}\\cr}}[7|\\the\\count0]\\setbox9=\\vbox{\\every"
        "cr{\\noalign{\\kern3pt}}\\halign{#\\cr\\K{5}\\cr\\K{7}\\cr"
        "}}[8|\\the\\ht9]%",
        "[1|3][2|1][3|2][4|2][5|2][6|3][7|2][8|11.0pt]");
}

/* \over and its relatives; see docs/DECISIONS.md, fractions. */
static int test_fractions(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\font\\te"
        "nrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tensy=cmsy10 \\font"
        "\\tenex=cmex10 \\font\\sevenrm=cmr7 \\font\\seveni=cmmi7 "
        "\\font\\sevensy=cmsy7 \\font\\fiverm=cmr5 \\font\\fivei=cm"
        "mi5 \\font\\fivesy=cmsy5 \\textfont0=\\tenrm \\scriptfont0"
        "=\\sevenrm \\scriptscriptfont0=\\fiverm \\textfont1=\\tenm"
        "i \\scriptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscriptfont2"
        "=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\tenex \\scri"
        "ptscriptfont3=\\tenex \\delcode`\\(=\"028300 \\delcode`\\)"
        "=\"029301 \\delcode`\\[=\"05B302 \\delcode`\\]=\"05D303 \\"
        "delcode`\\.=0 \\nulldelimiterspace=1.2pt \\scriptspace=0.5"
        "pt \\hbadness=10000 \\hfuzz=1000pt \\tenrm \\def\\K#1#2#3{"
        "\\vrule width#1pt height#2pt depth#3pt}\\def\\C{\\mathchoi"
        "ce{\\K{1}{1}{0}}{\\K{2}{1}{0}}{\\K{3}{1}{0}}{\\K{4}{1}{0}}"
        "}\\def\\M#1{\\setbox0=\\hbox{$#1$}}\\M{\\K{5}{3}{1}\\over"
        "\\K{7}{2}{4}}[1|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\K{5"
        "}{3}{1}\\atop\\K{7}{2}{4}}[2|\\the\\wd0|\\the\\ht0|\\the\\"
        "dp0]\\M{\\K{5}{3}{1}\\above2pt\\K{7}{2}{4}}[3|\\the\\wd0|"
        "\\the\\ht0|\\the\\dp0]\\M{\\K{5}{3}{1}\\overwithdelims()\\"
        "K{7}{2}{4}}[4|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\K{5}{"
        "3}{1}\\atopwithdelims[]\\K{7}{2}{4}}[5|\\the\\wd0|\\the\\h"
        "t0|\\the\\dp0]\\M{\\K{5}{3}{1}\\abovewithdelims..3pt\\K{7}"
        "{2}{4}}[6|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\displayst"
        "yle{\\K{5}{3}{1}\\over\\K{7}{2}{4}}}[7|\\the\\wd0|\\the\\h"
        "t0|\\the\\dp0]\\M{\\displaystyle{\\K{5}{3}{1}\\atop\\K{7}{"
        "2}{4}}}[8|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\scriptsty"
        "le{\\K{5}{0}{0}\\over\\K{7}{0}{0}}}[9|\\the\\wd0|\\the\\ht"
        "0|\\the\\dp0]\\M{\\scriptstyle{\\K{5}{0}{0}\\atop\\K{7}{0}"
        "{0}}}[10|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\scriptscri"
        "ptstyle{\\K{5}{0}{0}\\over\\K{7}{0}{0}}}[11|\\the\\wd0|\\t"
        "he\\ht0|\\the\\dp0]\\M{\\K{15}{3}{1}\\over\\K{7}{2}{4}}[12"
        "|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{{}\\over\\K{7}{5}{4}"
        "}[13|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\K{5}{3}{5}\\ov"
        "er}[14|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\K{5}{3}{5}\\"
        "atop\\K{7}{5}{4}}[15|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{"
        "\\displaystyle{\\K{5}{3}{9}\\atop\\K{7}{9}{4}}}[16|\\the\\"
        "wd0|\\the\\ht0|\\the\\dp0]\\M{\\displaystyle{\\K{5}{3}{9}"
        "\\over\\K{7}{9}{4}}}[17|\\the\\wd0|\\the\\ht0|\\the\\dp0]"
        "\\M{\\displaystyle{\\C\\over\\C}}[18|\\the\\wd0|\\the\\ht0"
        "|\\the\\dp0]\\M{{\\C\\over\\C}}[19|\\the\\wd0|\\the\\ht0|"
        "\\the\\dp0]\\M{\\scriptstyle{\\C\\over\\C}}[20|\\the\\wd0|"
        "\\the\\ht0|\\the\\dp0]\\M{\\displaystyle{\\K{5}{3}{1}\\ove"
        "rwithdelims()\\K{7}{2}{4}}}[21|\\the\\wd0|\\the\\ht0|\\the"
        "\\dp0]\\M{\\scriptstyle{\\K{5}{3}{1}\\overwithdelims()\\K{"
        "7}{2}{4}}}[22|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{{\\math"
        "ord{\\K{1}{1}{0}}^{\\K{1}{1}{0}}\\above20pt\\mathord{\\K{1"
        "}{1}{0}}^{\\K{1}{1}{0}}}}[23|\\the\\wd0|\\the\\ht0|\\the\\"
        "dp0]\\M{{{\\K{5}{0}{0}\\over\\K{7}{0}{0}}\\over\\K{9}{0}{0"
        "}}}[24|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\displaystyle"
        "{{\\K{5}{0}{0}\\over\\K{7}{0}{0}}\\over\\K{9}{0}{0}}}[25|"
        "\\the\\wd0|\\the\\ht0|\\the\\dp0]%",
        "[1|9.4pt|7.09998pt|7.44841pt][2|9.4pt|7.4373pt|7.44841pt]["
        "3|9.4pt|9.5pt|7.44841pt][4|16.16672pt|8.50005pt|7.44841pt]"
        "[5|15.33337pt|8.50005pt|7.44841pt][6|9.4pt|11.0pt|8.0pt][7"
        "|9.4pt|9.76508pt|10.85951pt][8|9.4pt|9.76508pt|10.85951pt]"
        "[9|9.4pt|2.68732pt|2.4095pt][10|9.4pt|3.29843pt|2.4095pt]["
        "11|9.4pt|1.93732pt|2.65953pt][12|17.4pt|7.09998pt|7.44841p"
        "t][13|9.4pt|3.93732pt|7.44841pt][14|7.4pt|11.09998pt|3.448"
        "41pt][15|9.4pt|9.09442pt|9.10553pt][16|9.4pt|13.3527pt|14."
        "44714pt][17|9.4pt|15.89993pt|11.89993pt][18|4.4pt|7.76508p"
        "t|6.85951pt][19|5.4pt|4.93732pt|3.44841pt][20|6.4pt|3.6873"
        "2pt|2.4095pt][21|21.72229pt|14.5001pt|10.85951pt][22|14.77"
        "78pt|6.75pt|6.4095pt][23|4.9pt|36.51779pt|30.49998pt][24|1"
        "1.79999pt|8.1968pt|3.44841pt][25|11.79999pt|11.28566pt|6.8"
        "5951pt]");
}

/* \parshape, and the shape a new paragraph starts with; see
   docs/DECISIONS.md, parshape. */
static int test_parshape(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\parindent=0pt \\baselineskip=0pt \\lines"
        "kip=0pt \\lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\"
        "parskip=0pt \\parfillskip=0pt plus1fil \\tolerance=10000 "
        "\\pretolerance=-1 \\linepenalty=10 \\adjdemerits=10000 \\c"
        "lubpenalty=150 \\widowpenalty=150 \\brokenpenalty=100 \\in"
        "terlinepenalty=0 \\hbadness=10000 \\hfuzz=1000pt \\vbadnes"
        "s=10000 \\vfuzz=1000pt \\hsize=100pt \\def\\W{\\vrule widt"
        "h20pt height1pt depth0pt\\hskip0pt plus1fil}\\def\\peel{\\"
        "ifnum\\lastnodetype=1 \\setbox1=\\lastbox\\xdef\\R{\\R|\\t"
        "he\\wd1}\\else\\ifnum\\lastnodetype=11 \\unskip\\else\\ifn"
        "um\\lastnodetype=13 \\unpenalty\\else\\xdef\\R{\\R|.}\\fi"
        "\\fi\\fi}\\def\\peelten{\\peel\\peel\\peel\\peel\\peel\\pe"
        "el\\peel\\peel\\peel\\peel}\\long\\def\\m#1{\\gdef\\R{}\\s"
        "etbox0=\\vbox{#1}\\global\\dimen0=\\wd0 \\setbox2=\\vbox{"
        "\\unvbox0 \\peelten}}\\m{\\parshape=3 5pt 30pt 10pt 50pt 1"
        "5pt 70pt \\noindent\\W\\W\\W\\W\\W\\W\\W\\W\\par}[1|\\the"
        "\\dimen0\\R]\\m{\\parshape=1 25pt 40pt \\noindent\\W\\W\\W"
        "\\W\\W\\par}[2|\\the\\dimen0\\R]\\m{\\hangindent=60pt\\han"
        "gafter=0 \\parshape=1 25pt 40pt \\noindent\\W\\W\\W\\par}["
        "3|\\the\\dimen0\\R]\\m{\\parshape=0 \\noindent\\W\\W\\W\\p"
        "ar}[4|\\the\\dimen0\\R]\\m{\\parshape=2 -10pt 40pt 0pt 60p"
        "t \\noindent\\W\\W\\W\\W\\par}[5|\\the\\dimen0\\R]\\m{\\ha"
        "ngindent=9pt\\hangafter=0 \\noindent\\W\\W\\W\\W\\par}[6|"
        "\\the\\dimen0\\R]\\parshape=2 5pt 30pt 10pt 50pt \\count1="
        "\\parshape {\\global\\count2=\\parshape \\parshape=1 0pt 2"
        "0pt \\global\\count3=\\parshape }\\count4=\\parshape \\set"
        "box0=\\vbox{\\global\\count5=\\parshape }\\count6=\\parsha"
        "pe \\setbox0=\\hbox{\\global\\count7=\\parshape }\\count8="
        "\\parshape \\setbox0=\\vbox{\\parshape=1 5pt 30pt \\global"
        "\\count9=\\parshape \\par \\global\\count0=\\parshape }[7|"
        "\\the\\count1|\\the\\count2|\\the\\count3|\\the\\count4|\\"
        "the\\count5|\\the\\count6|\\the\\count7|\\the\\count8|\\th"
        "e\\count9|\\the\\count0]\\hangindent=7pt \\hangafter=3 \\l"
        "ooseness=2 \\setbox0=\\vbox{\\global\\dimen1=\\hangindent "
        "\\global\\count1=\\hangafter \\global\\count2=\\looseness "
        "}\\global\\dimen2=\\hangindent \\global\\count3=\\hangafte"
        "r \\global\\count4=\\looseness \\setbox0=\\vbox{\\hanginde"
        "nt=7pt \\hangafter=3 \\looseness=2 \\noindent\\W\\W\\par "
        "\\global\\dimen3=\\hangindent \\global\\count5=\\hangafter"
        " \\global\\count6=\\looseness }[8|\\the\\dimen1|\\the\\cou"
        "nt1|\\the\\count2|\\the\\dimen2|\\the\\count3|\\the\\count"
        "4|\\the\\dimen3|\\the\\count5|\\the\\count6]\\def\\P{\\par"
        "shape=5 1pt 21pt 2pt 42pt 3pt 63pt 4pt 84pt 5pt 95pt }\\de"
        "f\\D{\\global\\dimen1=\\displaywidth \\global\\dimen2=\\di"
        "splayindent }\\setbox0=\\vbox{\\P\\noindent$$\\D\\hbox{}$$"
        "}[9|\\the\\dimen1|\\the\\dimen2]\\setbox0=\\vbox{\\P\\noin"
        "dent\\W$$\\D\\hbox{}$$}[10|\\the\\dimen1|\\the\\dimen2]\\s"
        "etbox0=\\vbox{\\P\\noindent\\W\\W\\W\\W\\W\\W$$\\D\\hbox{}"
        "$$}[11|\\the\\dimen1|\\the\\dimen2]\\setbox0=\\vbox{\\hang"
        "indent=9pt\\hangafter=2 \\noindent$$\\D\\hbox{}$$}[12|\\th"
        "e\\dimen1|\\the\\dimen2]\\setbox0=\\vbox{\\hangindent=9pt"
        "\\hangafter=2 \\noindent\\W$$\\D\\hbox{}$$}[13|\\the\\dime"
        "n1|\\the\\dimen2]%",
        "[1|85.0pt|70.0pt|70.0pt|50.0pt|30.0pt|.][2|65.0pt|40.0pt|4"
        "0.0pt|40.0pt|.|.|.][3|65.0pt|40.0pt|40.0pt|.|.|.|.|.|.][4|"
        "100.0pt|100.0pt|.|.|.|.|.|.|.|.|.][5|60.0pt|60.0pt|40.0pt|"
        ".|.|.|.|.|.][6|100.0pt|91.0pt|.|.|.|.|.|.|.|.|.][7|2|2|1|2"
        "|0|2|2|2|1|0][8|0.0pt|1|0|7.0pt|3|2|0.0pt|1|0][9|42.0pt|2."
        "0pt][10|63.0pt|3.0pt][11|95.0pt|5.0pt][12|100.0pt|0.0pt][1"
        "3|91.0pt|9.0pt]");
}

/* \  and \unhbox inside a formula; see docs/DECISIONS.md,
   control-space and unboxing-in-a-formula. */
static int test_formula_spacing(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\mathsurround=0pt \\hbadness=10000 \\hfuz"
        "z=1000pt \\font\\f=cmr10 \\f \\textfont0=\\f \\scriptfont0"
        "=\\f \\scriptscriptfont0=\\f \\font\\fm=cmmi10 \\textfont1"
        "=\\fm \\scriptfont1=\\fm \\scriptscriptfont1=\\fm \\font\\"
        "fs=cmsy10 \\textfont2=\\fs \\scriptfont2=\\fs \\scriptscri"
        "ptfont2=\\fs \\font\\fx=cmex10 \\textfont3=\\fx \\scriptfo"
        "nt3=\\fx \\scriptscriptfont3=\\fx \\def\\R#1{\\vrule width"
        "#1pt height1pt depth0pt}\\def\\M#1{\\setbox1=\\hbox{#1}}\\"
        "M{\\ }[1|\\the\\wd1]\\M{$\\ $}[2|\\the\\wd1]\\M{x\\spacefa"
        "ctor=2000 \\ }[3|\\the\\wd1]\\M{x}[4|\\the\\wd1]\\spaceski"
        "p=7pt plus1pt minus2pt \\M{\\ }[5|\\the\\wd1]\\M{$\\ $}[6|"
        "\\the\\wd1]\\spaceskip=0pt \\M{$\\scriptstyle\\ $}[7|\\the"
        "\\wd1]\\M{$\\mathord{\\R{1}}\\ \\mathord{\\R{1}}$}[8|\\the"
        "\\wd1]\\M{$\\unhbox9$}[9|\\the\\wd1]\\M{$\\unhcopy9$}[10|"
        "\\the\\wd1]\\M{\\unhcopy9}[11|\\the\\wd1]\\M{$\\mathord{\\"
        "R{1}}\\unhbox9\\mathord{\\R{1}}$}[12|\\the\\wd1]\\M{$\\mat"
        "hord{\\R{1}}\\mathord{\\R{1}}$}[13|\\the\\wd1][14|\\the\\f"
        "ontdimen2\\f|\\the\\fontdimen3\\f|\\the\\fontdimen4\\f]%",
        "[1|3.33333pt][2|3.33333pt][3|8.61113pt][4|5.2778pt][5|7.0p"
        "t][6|7.0pt][7|3.33333pt][8|5.33333pt][9|0.0pt][10|0.0pt][1"
        "1|0.0pt][12|2.0pt][13|2.0pt][14|3.33333pt|1.66666pt|1.1111"
        "1pt]");
}

/* A conditional may be opened outside a box or an alignment entry and
   closed inside it; see docs/DECISIONS.md, conditionals-across-boxes. */
static int test_conditionals_across_boxes(void)
{
    return run_snippet(
        "\\catcode`\\&=4 \\hbadness=10000 \\hfuzz=1000pt \\vbadness"
        "=10000 \\vfuzz=1000pt \\parindent=0pt \\baselineskip=0pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\boxmaxdepth=16383.999"
        "98pt \\tabskip=0pt \\def\\R#1{\\vrule width#1pt height1pt "
        "depth0pt}\\iftrue\\setbox0=\\hbox{\\R{5}\\fi}[1|\\the\\wd0"
        "]\\setbox0=\\hbox{\\iftrue\\R{6}}\\fi[2|\\the\\wd0]\\setbo"
        "x0=\\vbox{\\iftrue\\hrule height3pt}\\fi[3|\\the\\ht0]\\if"
        "true\\setbox0=\\vbox{\\hrule height4pt\\fi}[4|\\the\\ht0]"
        "\\setbox0=\\vbox{\\halign{\\hbox to30pt{#\\hfil}\\cr\\iftr"
        "ue\\R{5}\\fi\\cr}}[5|\\the\\wd0|\\the\\ht0]\\setbox0=\\vbo"
        "x{\\halign{\\hbox to30pt{#\\hfil}\\cr\\iffalse\\R{9}\\else"
        "\\R{7}\\fi\\cr}}[6|\\the\\wd0|\\the\\ht0]\\setbox0=\\hbox{"
        "\\iftrue\\hbox{\\R{8}\\fi}}[7|\\the\\wd0]\\setbox0=\\vbox{"
        "\\halign{\\hbox to30pt{#\\hfil}\\cr\\R{5}\\ifdim1pt>0pt\\R"
        "{2}\\cr\\else\\R{9}\\cr\\fi\\R{4}\\cr}}[8|\\the\\wd0|\\the"
        "\\ht0]%",
        "[1|5.0pt][2|6.0pt][3|3.0pt][4|4.0pt][5|30.0pt|1.0pt][6|30."
        "0pt|1.0pt][7|8.0pt][8|30.0pt|2.0pt]");
}

/* \radical, and the math codes a letter and a digit start with; see
   docs/DECISIONS.md, radicals and initex-math-codes. */
static int test_radicals(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\mathsurr"
        "ound=0pt \\hbadness=10000 \\hfuzz=1000pt \\nulldelimitersp"
        "ace=1.2pt \\scriptspace=0.5pt \\font\\tenrm=cmr10 \\font\\"
        "tenmi=cmmi10 \\font\\tensy=cmsy10 \\font\\tenex=cmex10 \\f"
        "ont\\sevenrm=cmr7 \\font\\seveni=cmmi7 \\font\\sevensy=cms"
        "y7 \\font\\fiverm=cmr5 \\font\\fivei=cmmi5 \\font\\fivesy="
        "cmsy5 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\script"
        "scriptfont0=\\fiverm \\textfont1=\\tenmi \\scriptfont1=\\s"
        "eveni \\scriptscriptfont1=\\fivei \\textfont2=\\tensy \\sc"
        "riptfont2=\\sevensy \\scriptscriptfont2=\\fivesy \\textfon"
        "t3=\\tenex \\scriptfont3=\\tenex \\scriptscriptfont3=\\ten"
        "ex \\tenrm \\def\\R#1#2#3{\\vrule width#1pt height#2pt dep"
        "th#3pt}\\def\\C{\\mathchoice{\\R{1}{1}{0}}{\\R{2}{1}{0}}{"
        "\\R{3}{1}{0}}{\\R{4}{1}{0}}}\\def\\M#1{\\setbox0=\\hbox{$#"
        "1$}}\\M{\\radical\"270370{\\R{5}{3}{1}}}[1|\\the\\wd0|\\th"
        "e\\ht0|\\the\\dp0]\\M{\\displaystyle\\radical\"270370{\\R{"
        "5}{3}{1}}}[2|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\script"
        "style\\radical\"270370{\\R{5}{3}{1}}}[3|\\the\\wd0|\\the\\"
        "ht0|\\the\\dp0]\\M{\\scriptscriptstyle\\radical\"270370{\\"
        "R{5}{3}{1}}}[4|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\radi"
        "cal\"270370{\\R{5}{0}{0}}}[5|\\the\\wd0|\\the\\ht0|\\the\\"
        "dp0]\\M{\\radical\"270370{\\R{5}{20}{0}}}[6|\\the\\wd0|\\t"
        "he\\ht0|\\the\\dp0]\\M{\\displaystyle\\radical\"270370{\\R"
        "{5}{20}{0}}}[7|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\radi"
        "cal\"270370{\\C}}[8|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{"
        "\\displaystyle\\radical\"270370{\\C}}[9|\\the\\wd0|\\the\\"
        "ht0|\\the\\dp0]\\M{\\mathord{\\R{1}{1}{0}}\\radical\"27037"
        "0{\\R{5}{3}{1}}\\mathord{\\R{1}{1}{0}}}[10|\\the\\wd0|\\th"
        "e\\ht0|\\the\\dp0]\\M{\\radical\"270370{\\radical\"270370{"
        "\\R{5}{3}{1}}}}[11|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\"
        "radical\"270370{\\R{5}{3}{1}}^{\\R{2}{2}{0}}}[12|\\the\\wd"
        "0|\\the\\ht0|\\the\\dp0]\\M{\\radical\"270370 x}[13|\\the"
        "\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\radical\"270370{}}[14|\\"
        "the\\wd0|\\the\\ht0|\\the\\dp0][15|\\the\\mathcode`x|\\the"
        "\\mathcode`X|\\the\\mathcode`0]%",
        "[1|13.33336pt|6.84995pt|3.55002pt][2|13.33336pt|7.33815pt|"
        "3.06181pt][3|11.56947pt|5.25995pt|2.08002pt][4|10.4862pt|4"
        ".16998pt|1.11002pt][5|13.33336pt|5.84995pt|4.55002pt][6|15"
        ".00002pt|22.85007pt|1.55014pt][7|15.00002pt|23.33827pt|1.0"
        "6194pt][8|10.33336pt|6.34995pt|4.05002pt][9|9.33336pt|6.83"
        "815pt|3.56181pt][10|15.33336pt|6.84995pt|3.55002pt][11|23."
        "33337pt|8.49997pt|3.90012pt][12|15.83336pt|6.84995pt|3.550"
        "02pt][13|14.04863pt|8.00272pt|2.39725pt][14|8.33336pt|5.84"
        "995pt|4.55002pt][15|29048|29016|28720]");
}

/* \overline and \underline; see docs/DECISIONS.md,
   over-and-underline. */
static int test_over_and_underline(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\mathsurr"
        "ound=0pt \\hbadness=10000 \\hfuzz=1000pt \\scriptspace=0.5"
        "pt \\thinmuskip=3mu \\medmuskip=4mu plus2mu minus4mu \\thi"
        "ckmuskip=5mu plus5mu \\font\\tenrm=cmr10 \\font\\tenmi=cmm"
        "i10 \\font\\tensy=cmsy10 \\font\\tenex=cmex10 \\font\\seve"
        "nrm=cmr7 \\font\\seveni=cmmi7 \\font\\sevensy=cmsy7 \\font"
        "\\fiverm=cmr5 \\font\\fivei=cmmi5 \\font\\fivesy=cmsy5 \\t"
        "extfont0=\\tenrm \\scriptfont0=\\sevenrm \\scriptscriptfon"
        "t0=\\fiverm \\textfont1=\\tenmi \\scriptfont1=\\seveni \\s"
        "criptscriptfont1=\\fivei \\textfont2=\\tensy \\scriptfont2"
        "=\\sevensy \\scriptscriptfont2=\\fivesy \\textfont3=\\tene"
        "x \\scriptfont3=\\tenex \\scriptscriptfont3=\\tenex \\tenr"
        "m \\def\\R#1#2#3{\\vrule width#1pt height#2pt depth#3pt}\\"
        "def\\C{\\mathchoice{\\R{1}{1}{0}}{\\R{2}{1}{0}}{\\R{3}{1}{"
        "0}}{\\R{4}{1}{0}}}\\def\\S{\\mathord{\\R{1}{1}{0}}^{\\R{1}"
        "{1}{0}}}\\def\\M#1{\\setbox0=\\hbox{$#1$}}\\M{\\overline{"
        "\\R{5}{3}{1}}}[1|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\di"
        "splaystyle\\overline{\\R{5}{3}{1}}}[2|\\the\\wd0|\\the\\ht"
        "0|\\the\\dp0]\\M{\\scriptstyle\\overline{\\R{5}{3}{1}}}[3|"
        "\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\underline{\\R{5}{3}"
        "{1}}}[4|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\displaystyl"
        "e\\underline{\\R{5}{3}{1}}}[5|\\the\\wd0|\\the\\ht0|\\the"
        "\\dp0]\\M{\\scriptstyle\\underline{\\R{5}{3}{1}}}[6|\\the"
        "\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\overline{\\C}}[7|\\the\\"
        "wd0|\\the\\ht0|\\the\\dp0]\\M{\\underline{\\C}}[8|\\the\\w"
        "d0|\\the\\ht0|\\the\\dp0]\\M{\\mathord{\\R{1}{1}{0}}\\over"
        "line{\\R{5}{3}{1}}\\mathord{\\R{1}{1}{0}}}[9|\\the\\wd0|\\"
        "the\\ht0|\\the\\dp0]\\M{\\overline{\\R{5}{3}{1}}^{\\R{2}{2"
        "}{0}}}[10|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\underline"
        "{\\R{5}{3}{1}}_{\\R{2}{0}{2}}}[11|\\the\\wd0|\\the\\ht0|\\"
        "the\\dp0]\\M{\\overline{\\S}}[12|\\the\\wd0|\\the\\ht0|\\t"
        "he\\dp0]\\M{\\underline{\\S}}[13|\\the\\wd0|\\the\\ht0|\\t"
        "he\\dp0]\\M{\\overline{\\overline{\\R{5}{3}{1}}}}[14|\\the"
        "\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\overline{\\R{5}{3}{1}}\\"
        "mathbin{\\R{1}{1}{0}}\\mathord{\\R{1}{1}{0}}}[15|\\the\\wd"
        "0|\\the\\ht0|\\the\\dp0]\\M{\\mathord{\\R{1}{1}{0}}\\mathb"
        "in{\\R{1}{1}{0}}\\mathord{\\R{1}{1}{0}}}[16|\\the\\wd0|\\t"
        "he\\ht0|\\the\\dp0]\\M{\\underline{\\R{5}{3}{1}}\\mathbin{"
        "\\R{1}{1}{0}}\\mathord{\\R{1}{1}{0}}}[17|\\the\\wd0|\\the"
        "\\ht0|\\the\\dp0]%",
        "[1|5.0pt|4.9999pt|1.0pt][2|5.0pt|4.9999pt|1.0pt][3|5.0pt|4"
        ".9999pt|1.0pt][4|5.0pt|3.0pt|2.9999pt][5|5.0pt|3.0pt|2.999"
        "9pt][6|5.0pt|3.0pt|2.9999pt][7|2.0pt|2.9999pt|0.0pt][8|2.0"
        "pt|1.0pt|1.9999pt][9|7.0pt|4.9999pt|1.0pt][10|7.5pt|5.6289"
        "2pt|1.0pt][11|7.5pt|3.0pt|5.49988pt][12|2.5pt|5.88878pt|0."
        "0pt][13|2.5pt|4.62892pt|1.9999pt][14|5.0pt|6.99979pt|1.0pt"
        "][15|11.44434pt|4.9999pt|1.0pt][16|7.44434pt|1.0pt|0.0pt]["
        "17|11.44434pt|3.0pt|2.9999pt]");
}

/* A metric file longer than its tables say, and characters a font does
   not define; see docs/DECISIONS.md, padded-tfm-files and
   missing-characters. */
static int test_missing_characters(void)
{
    return run_snippet(
        "\\hbadness=10000 \\hfuzz=1000pt \\tracinglostchars=0 \\sfc"
        "ode`\\.=3000 \\font\\x=tcrm1095 \\font\\f=cmr10 \\font\\n="
        "cmr10 at 5pt \\setbox0=\\hbox{\\x A}[1|\\the\\wd0|\\the\\h"
        "t0]\\setbox0=\\hbox{\\f A\\x A\\f A}[2|\\the\\wd0|\\the\\h"
        "t0]\\setbox0=\\hbox{\\f AA}[3|\\the\\wd0|\\the\\ht0]\\setb"
        "ox0=\\hbox{\\x A\\global\\count1=\\spacefactor}[4|\\the\\c"
        "ount1]\\setbox0=\\hbox{\\x .\\global\\count1=\\spacefactor"
        "}[5|\\the\\count1][6|\\the\\fontdimen1\\x|\\the\\fontdimen"
        "2\\x|\\the\\fontdimen6\\x]\\setbox0=\\hbox{\\nullfont A}[7"
        "|\\the\\wd0|\\the\\ht0]%",
        "[1|0.0pt|0.0pt][2|15.00003pt|6.83331pt][3|15.00003pt|6.833"
        "31pt][4|999][5|3000][6|0.0pt|3.63054pt|10.88788pt][7|0.0pt"
        "|0.0pt]");
}

/* \leaders and its relatives; see docs/DECISIONS.md, leaders. */
static int test_leaders(void)
{
    return run_snippet(
        "\\hbadness=10000 \\hfuzz=1000pt \\vbadness=10000 \\vfuzz=1"
        "000pt \\parindent=0pt \\baselineskip=0pt \\lineskip=0pt \\"
        "lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\def\\R#1#2"
        "#3{\\vrule width#1pt height#2pt depth#3pt}\\setbox1=\\hbox"
        "{\\R{3}{2}{1}}\\setbox0=\\hbox to50pt{\\leaders\\copy1\\hf"
        "il}[1|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\setbox0=\\hbox{\\"
        "leaders\\copy1\\hskip7pt}[2|\\the\\wd0|\\the\\ht0|\\the\\d"
        "p0]\\setbox0=\\hbox{\\cleaders\\copy1\\hskip7pt}[3|\\the\\"
        "wd0|\\the\\ht0|\\the\\dp0]\\setbox0=\\hbox{\\xleaders\\cop"
        "y1\\hskip7pt}[4|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\setbox0"
        "=\\hbox{\\leaders\\vrule height4pt depth2pt\\hskip7pt}[5|"
        "\\the\\wd0|\\the\\ht0|\\the\\dp0]\\setbox0=\\vbox{\\leader"
        "s\\hrule height4pt\\vskip7pt}[6|\\the\\wd0|\\the\\ht0|\\th"
        "e\\dp0]\\setbox0=\\vbox{\\leaders\\hbox{\\R{3}{2}{1}}\\vsk"
        "ip9pt}[7|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\setbox0=\\hbox"
        "{\\leaders\\hbox{\\R{3}{2}{1}}\\hfil}[8|\\the\\wd0|\\the\\"
        "ht0|\\the\\dp0]\\setbox0=\\hbox{\\R{1}{1}{0}\\leaders\\cop"
        "y1\\hskip7pt\\R{1}{1}{0}}[9|\\the\\wd0|\\the\\ht0|\\the\\d"
        "p0]\\setbox0=\\vbox{\\hrule height1pt\\leaders\\hbox{\\R{9"
        "}{2}{1}}\\vskip5pt\\hrule height1pt}[10|\\the\\wd0|\\the\\"
        "ht0|\\the\\dp0]%",
        "[1|50.0pt|2.0pt|1.0pt][2|7.0pt|2.0pt|1.0pt][3|7.0pt|2.0pt|"
        "1.0pt][4|7.0pt|2.0pt|1.0pt][5|7.0pt|4.0pt|2.0pt][6|0.0pt|7"
        ".0pt|0.0pt][7|3.0pt|9.0pt|0.0pt][8|0.0pt|2.0pt|1.0pt][9|9."
        "0pt|2.0pt|1.0pt][10|9.0pt|7.0pt|0.0pt]");
}

/* \string makes tokens, never ^^ notation; see docs/DECISIONS.md,
   string-is-not-escaped. */
static int test_string_bytes(void)
{
    return run_snippet(
        "\\catcode`\\\x03=12 \\catcode`\\\xc3=12 \\catcod"
        "e`\\\x7f=12 [1|\\pdfescapehex{\\string\x03}][2|"
        "\\pdfescapehex{\\string\xc3}][3|\\pdfescapehex{"
        "\\string\x7f}][4|\\pdfescapehex{\\string A}][5|"
        "\\pdfescapehex{\\string\\relax}]\\catcode`\\\xc3"
        "=13 \\def\xc3{x}[6|\\pdfescapehex{\\string\xc3}|"
        "\\pdfescapehex{\\meaning\xc3}]{\\catcode`\\^=7 "
        "\\catcode`\\^^J=13 \\xdef\\saved{\\string^^J}}[7"
        "|\\pdfescapehex{\\saved}]\\expandafter\\def\\csn"
        "ame u8:\\string\xc3\\endcsname{Q}[8|\\expandafte"
        "r\\meaning\\csname u8:\\string\xc3\\endcsname]%",
        "[1|03][2|C3][3|7F][4|41][5|5C72656C6178][6|C3|6D"
        "6163726F3A2D3E78][7|0A][8|macro:->Q]");
}

/* \middle; see docs/DECISIONS.md, middle-delimiters. */
static int test_middle_delimiters(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\mathsurround=0pt \\hbadness=10000 \\hfuz"
        "z=1000pt \\nulldelimiterspace=1.2pt \\scriptspace=0.5pt \\"
        "delimiterfactor=901 \\delimitershortfall=5pt \\thinmuskip="
        "3mu \\medmuskip=4mu plus2mu minus4mu \\thickmuskip=5mu plu"
        "s5mu \\font\\tenrm=cmr10 \\font\\tenmi=cmmi10 \\font\\tens"
        "y=cmsy10 \\font\\tenex=cmex10 \\font\\sevenrm=cmr7 \\font"
        "\\seveni=cmmi7 \\font\\sevensy=cmsy7 \\font\\fiverm=cmr5 "
        "\\font\\fivei=cmmi5 \\font\\fivesy=cmsy5 \\textfont0=\\ten"
        "rm \\scriptfont0=\\sevenrm \\scriptscriptfont0=\\fiverm \\"
        "textfont1=\\tenmi \\scriptfont1=\\seveni \\scriptscriptfon"
        "t1=\\fivei \\textfont2=\\tensy \\scriptfont2=\\sevensy \\s"
        "criptscriptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\tenrm \\delcode`\\"
        "(=\"028300 \\delcode`\\)=\"029301 \\delcode`\\|=\"26A30C "
        "\\delcode`\\.=0 \\def\\R#1#2#3{\\vrule width#1pt height#2p"
        "t depth#3pt}\\def\\O{\\mathord{\\R{5}{10}{5}}}\\def\\P{\\m"
        "athop{\\R{5}{10}{5}}}\\def\\M#1{\\setbox0=\\hbox{$#1$}}\\M"
        "{\\left(\\R{5}{10}{5}\\right)}[1|\\the\\wd0|\\the\\ht0|\\t"
        "he\\dp0]\\M{\\left(\\R{5}{10}{5}\\middle|\\R{5}{10}{5}\\ri"
        "ght)}[2|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\left(\\R{5}"
        "{10}{5}\\middle.\\R{5}{10}{5}\\right)}[3|\\the\\wd0|\\the"
        "\\ht0|\\the\\dp0]\\M{\\left.\\R{5}{10}{5}\\middle|\\R{5}{1"
        "0}{5}\\right.}[4|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\le"
        "ft(\\R{5}{10}{5}\\middle|\\R{5}{10}{5}\\middle|\\R{5}{10}{"
        "5}\\right)}[5|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\left("
        "\\P\\middle|\\O\\right)}[6|\\the\\wd0|\\the\\ht0|\\the\\dp"
        "0]\\M{\\left(\\P\\mathord{\\R{3.33333}{1}{0}}\\O\\right)}["
        "7|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\left(\\O\\middle|"
        "\\P\\right)}[8|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\left"
        "(\\O\\mathclose{\\R{3.33333}{1}{0}}\\P\\right)}[9|\\the\\w"
        "d0|\\the\\ht0|\\the\\dp0]\\M{\\left(\\R{5}{2}{1}\\middle|"
        "\\R{5}{2}{1}\\right)}[10|\\the\\wd0|\\the\\ht0|\\the\\dp0]"
        "\\M{\\scriptstyle\\left(\\R{5}{10}{5}\\middle|\\R{5}{10}{5"
        "}\\right)}[11|\\the\\wd0|\\the\\ht0|\\the\\dp0]\\M{\\left("
        "\\left(\\R{5}{10}{5}\\middle|\\R{5}{10}{5}\\right)\\middle"
        "|\\R{5}{1}{1}\\right)}[12|\\the\\wd0|\\the\\ht0|\\the\\dp0"
        "]%",
        "[1|16.94446pt|11.50008pt|6.50009pt][2|25.27779pt|11.50009p"
        "t|6.50009pt][3|23.14445pt|11.50008pt|6.50009pt][4|15.73332"
        "pt|11.50009pt|6.50009pt][5|33.61111pt|11.50009pt|6.50009pt"
        "][6|25.27779pt|11.50009pt|6.50009pt][7|26.94441pt|11.50008"
        "pt|6.50009pt][8|25.27779pt|11.50009pt|6.50009pt][9|26.9444"
        "1pt|11.50008pt|6.50009pt][10|20.55559pt|7.5pt|2.5pt][11|25"
        ".27779pt|10.75009pt|7.25009pt][12|45.55557pt|11.50009pt|6."
        "50009pt]");
}

/* \nonscript; see docs/DECISIONS.md, nonscript. */
static int test_nonscript(void)
{
    return run_snippet(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\mathsurr"
        "ound=0pt \\hbadness=10000 \\hfuzz=1000pt \\scriptspace=0.5"
        "pt \\thinmuskip=3mu \\medmuskip=4mu plus2mu minus4mu \\thi"
        "ckmuskip=5mu plus5mu \\font\\tenrm=cmr10 \\font\\tenmi=cmm"
        "i10 \\font\\tensy=cmsy10 \\font\\tenex=cmex10 \\font\\seve"
        "nrm=cmr7 \\font\\seveni=cmmi7 \\font\\sevensy=cmsy7 \\font"
        "\\fiverm=cmr5 \\font\\fivei=cmmi5 \\font\\fivesy=cmsy5 \\t"
        "extfont0=\\tenrm \\scriptfont0=\\sevenrm \\scriptscriptfon"
        "t0=\\fiverm \\textfont1=\\tenmi \\scriptfont1=\\seveni \\s"
        "criptscriptfont1=\\fivei \\textfont2=\\tensy \\scriptfont2"
        "=\\sevensy \\scriptscriptfont2=\\fivesy \\textfont3=\\tene"
        "x \\scriptfont3=\\tenex \\scriptscriptfont3=\\tenex \\tenr"
        "m \\def\\R#1{\\vrule width#1pt height1pt depth0pt}\\def\\O"
        "{\\mathord{\\R{5}}}\\def\\M#1{\\setbox0=\\hbox{$#1$}}\\M{"
        "\\O\\nonscript\\mskip9mu\\O}[1|\\the\\wd0]\\M{\\scriptstyl"
        "e\\O\\nonscript\\mskip9mu\\O}[2|\\the\\wd0]\\M{\\O\\mskip9"
        "mu\\O}[3|\\the\\wd0]\\M{\\scriptstyle\\O\\mskip9mu\\O}[4|"
        "\\the\\wd0]\\M{\\O\\nonscript\\hskip7pt\\O}[5|\\the\\wd0]"
        "\\M{\\scriptstyle\\O\\nonscript\\hskip7pt\\O}[6|\\the\\wd0"
        "]\\M{\\O\\nonscript\\kern7pt\\O}[7|\\the\\wd0]\\M{\\script"
        "style\\O\\nonscript\\kern7pt\\O}[8|\\the\\wd0]\\M{\\O\\non"
        "script\\O}[9|\\the\\wd0]\\M{\\scriptstyle\\O\\nonscript\\O"
        "}[10|\\the\\wd0]\\M{\\scriptscriptstyle\\O\\nonscript\\msk"
        "ip9mu\\O}[11|\\the\\wd0]\\M{\\displaystyle\\O\\nonscript\\"
        "mskip9mu\\O}[12|\\the\\wd0]\\M{\\O^{\\O\\nonscript\\mskip9"
        "mu\\O}}[13|\\the\\wd0]\\M{\\O\\nonscript\\mkern9mu\\O}[14|"
        "\\the\\wd0]\\M{\\scriptstyle\\O\\nonscript\\mkern9mu\\O}[1"
        "5|\\the\\wd0]%",
        "[1|14.99988pt][2|10.0pt][3|14.99988pt][4|14.09721pt][5|17."
        "0pt][6|10.0pt][7|17.0pt][8|10.0pt][9|10.0pt][10|10.0pt][11"
        "|10.0pt][12|14.99988pt][13|15.5pt][14|14.99988pt][15|10.0p"
        "t]");
}

/* Vertical commands met in a paragraph end it; see docs/DECISIONS.md,
   ending-a-paragraph. */
static int test_ending_a_paragraph(void)
{
    return run_snippet(
        "\\catcode`\\&=4 \\hbadness=10000 \\hfuzz=1000pt \\vbadness"
        "=10000 \\vfuzz=1000pt \\parindent=0pt \\baselineskip=0pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\boxmaxdepth=16383.999"
        "98pt \\parskip=0pt \\parfillskip=0pt plus1fil \\tolerance="
        "10000 \\tabskip=0pt \\hsize=100pt \\def\\R#1{\\vrule width"
        "#1pt height1pt depth0pt}\\setbox9=\\vbox{\\hrule height2pt"
        "}\\setbox0=\\vbox{\\noindent\\hrule height3pt}[1|\\the\\wd"
        "0|\\the\\ht0]\\setbox0=\\vbox{\\noindent\\R{9}\\hrule heig"
        "ht3pt}[2|\\the\\wd0|\\the\\ht0]\\setbox0=\\vbox{\\noindent"
        "\\R{9}\\vskip5pt\\R{4}}[3|\\the\\wd0|\\the\\ht0]\\setbox0="
        "\\vbox{\\noindent\\R{9}\\vfil\\R{4}}[4|\\the\\wd0|\\the\\h"
        "t0]\\setbox0=\\vbox{\\noindent\\R{9}\\unvcopy9}[5|\\the\\w"
        "d0|\\the\\ht0]\\setbox0=\\vbox{\\noindent\\R{9}\\halign{#"
        "\\cr\\R{7}\\cr}}[6|\\the\\wd0|\\the\\ht0]\\setbox0=\\vbox{"
        "\\hbox{\\R{9}}\\hrule height3pt}[7|\\the\\wd0|\\the\\ht0]"
        "\\setbox0=\\vbox{\\noindent\\R{9}\\par\\hrule height3pt}[8"
        "|\\the\\wd0|\\the\\ht0]%",
        "[1|0.0pt|3.0pt][2|100.0pt|4.0pt][3|100.0pt|7.0pt][4|100.0p"
        "t|2.0pt][5|100.0pt|3.0pt][6|100.0pt|2.0pt][7|9.0pt|4.0pt]["
        "8|100.0pt|4.0pt]");
}

/* Text turned back into tokens keeps the space at category ten; see
   docs/DECISIONS.md, spaces-in-expanded-text. */
static int test_expansion_spaces(void)
{
    return run_snippet(
        "\\hbadness=10000 \\hfuzz=1000pt \\def\\space{ }\\skip0=1pt"
        " plus 2pt minus 3pt \\muskip0=1mu plus 2mu \\font\\fbig=cm"
        "r10 at 12pt \\def\\plain#1 #2\\q{x}\\def\\grab#1 #2\\qmark"
        "{[#1/#2]}\\def\\show#1{\\expandafter\\grab#1\\qmark}\\edef"
        "\\x{\\the\\skip0}\\show\\x\\edef\\x{\\the\\muskip0}\\show"
        "\\x\\edef\\x{\\meaning\\plain}\\show\\x\\edef\\x{\\fontnam"
        "e\\fbig}\\show\\x\\edef\\x{\\detokenize{a b}}\\show\\x\\ed"
        "ef\\x{\\string a\\string\\relax\\space b}\\show\\x\\edef\\"
        "x{\\number 12 \\space\\number 34 }\\show\\x\\edef\\x{\\rom"
        "annumeral 4 \\space\\romannumeral 9 }\\show\\x%",
        "[1.0pt/plus 2.0pt minus 3.0pt][1.0mu/plus 2.0mu][macro:#1/"
        "#2\\q ->x][cmr10/at 12.0pt][a/b][a\\relax/b][12/34][iv/ix]");
}

/* A box wider or taller than a dimension wraps rather than failing;
   see docs/DECISIONS.md, oversize-boxes. */
static int test_oversize_boxes(void)
{
    return run_snippet(
        "\\hbadness=10000 \\hfuzz=1000pt \\vbadness=10000 \\vfuzz=1"
        "000pt \\parindent=0pt \\baselineskip=0pt \\lineskip=0pt \\"
        "lineskiplimit=0pt \\boxmaxdepth=16383.99998pt \\parskip=0p"
        "t \\parfillskip=0pt plus1fil \\tolerance=10000 \\pretolera"
        "nce=-1 \\hsize=100pt \\def\\R#1{\\vrule width#1pt height1p"
        "t depth0pt}\\def\\H#1{\\hrule height#1pt}\\setbox0=\\hbox{"
        "\\R{9000}\\R{9000}\\R{9000}}[1|\\the\\wd0]\\setbox0=\\hbox"
        "{\\R{9000}\\R{9000}\\R{9000}\\R{9000}}[2|\\the\\wd0]\\setb"
        "ox0=\\hbox to50pt{\\R{9000}\\R{9000}\\R{9000}}[3|\\the\\wd"
        "0]\\setbox0=\\vbox{\\H{9000}\\H{9000}\\H{9000}}[4|\\the\\h"
        "t0]\\setbox0=\\vbox{\\H{9000}\\H{9000}\\H{9000}\\H{9000}}["
        "5|\\the\\ht0]\\setbox0=\\vbox to50pt{\\H{9000}\\H{9000}\\H"
        "{9000}}[6|\\the\\ht0]\\setbox0=\\vbox{\\noindent\\R{9000}"
        "\\R{9000}\\R{9000}\\par}[7|\\the\\wd0|\\the\\ht0]%",
        "[1|27000.0pt][2|-29536.0pt][3|50.0pt][4|27000.0pt][5|-2953"
        "6.0pt][6|50.0pt][7|100.0pt|1.0pt]");
}

/* The five glue commands measure the same in both directions; see
   docs/DECISIONS.md, horizontal-glue. */
static int test_horizontal_glue(void)
{
    return run_snippet(
        "\\setbox0=\\hbox{\\hskip3pt plus2fil minus1pt "
        "\\global\\skip1=\\lastskip \\global\\count1=\\lastnodetype}"
        "[\\the\\skip1|\\the\\count1]"
        "\\setbox0=\\hbox{\\hfil \\global\\skip2=\\lastskip}[\\the\\skip2]"
        "\\setbox0=\\hbox{\\hfill \\global\\skip3=\\lastskip}[\\the\\skip3]"
        "\\setbox0=\\hbox{\\hss \\global\\skip4=\\lastskip}[\\the\\skip4]"
        "\\setbox0=\\hbox{\\hfilneg \\global\\skip5=\\lastskip}[\\the\\skip5]"
        /* Infinite glue measures nothing until the box is set to a width. */
        "\\setbox0=\\hbox{\\hskip3pt}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\hfil}[\\the\\wd0]"
        "\\setbox0=\\hbox to 10pt{\\hfil}[\\the\\wd0]"
        "[\\the\\interlinepenalty|\\the\\postdisplaypenalty]"
        /* \indent puts an empty box of \parindent width in a horizontal
           list; \noindent puts nothing. */
        "\\parindent=17pt "
        "\\setbox0=\\hbox{\\indent}[\\the\\wd0|\\the\\ht0]"
        "\\setbox0=\\hbox{\\noindent}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\indent\\indent}[\\the\\wd0]"
        "\\parindent=0pt \\setbox0=\\hbox{\\indent}[\\the\\wd0]%",
        "[3.0pt plus 2.0fil minus 1.0pt|11][0.0pt plus 1.0fil]"
        "[0.0pt plus 1.0fill][0.0pt plus 1.0fil minus 1.0fil]"
        "[0.0pt plus -1.0fil][3.0pt][0.0pt][10.0pt][0|0]"
        "[17.0pt|0.0pt][0.0pt][34.0pt][0.0pt]");
}

/* Unboxing and the colour stacks; see docs/DECISIONS.md, unboxing and
   colour-stacks. */
static int test_unboxing_and_colour_stacks(void)
{
    return run_snippet(
        "\\boxmaxdepth=16383.99998pt \\baselineskip=0pt \\lineskip=0pt "
        "\\lineskiplimit=0pt "
        "\\setbox1=\\hbox{\\vrule width1pt height5pt depth2pt \\kern3pt}"
        "\\setbox2=\\vbox{\\hrule height2pt \\kern4pt}"
        "\\setbox3=\\hbox{\\vrule width1pt}"
        /* A copy leaves the register; the box itself empties it. */
        "\\setbox0=\\hbox{\\unhcopy1}"
        "[\\the\\wd0|\\the\\ht0|\\the\\dp0|\\the\\wd1]"
        "\\setbox0=\\hbox{\\unhbox1}"
        "[\\the\\wd0|\\ifvoid1 V\\else N\\fi]"
        "\\setbox0=\\vbox{\\unvcopy2}[\\the\\ht0|\\the\\wd0]"
        "\\setbox0=\\vbox{\\unvbox2}[\\ifvoid2 V\\else N\\fi]"
        /* Unboxing a void register does nothing. */
        "\\setbox0=\\hbox{\\unhbox9}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\unhcopy3\\unhcopy3}[\\the\\wd0]"
        /* Emptying a register outlives the group it happened in. */
        "\\setbox4=\\hbox{\\vrule width1pt}"
        "{\\setbox0=\\hbox{\\unhbox4}}[\\ifvoid4 V\\else N\\fi]"
        /* Colour stacks are numbered from one; stack zero is the page's. */
        "[\\pdfcolorstackinit{0 g}][\\pdfcolorstackinit page direct{1 g}]"
        "\\setbox0=\\hbox{\\pdfcolorstack0 push{1 0 0 rg}"
        "\\pdfcolorstack0 pop \\pdfcolorstack0 set{0 g}"
        "\\pdfcolorstack0 current}[ok]%",
        "[4.0pt|5.0pt|2.0pt|4.0pt][4.0pt|V][6.0pt|0.0pt][V][0.0pt][2.0pt][V]"
        "[1][2][ok]");
}

/* \everyeof is inserted when a file, real or from \scantokens, runs out;
   see docs/DECISIONS.md, everyeof. */
static int test_every_eof(void)
{
    return run_snippet(
        "\\everyeof{Q}\\edef\\a{\\scantokens{abc}}"
        "[\\detokenize\\expandafter{\\a}]"
        "\\everyeof{}\\edef\\b{\\scantokens{abc}}"
        "[\\detokenize\\expandafter{\\b}]"
        /* The non-zero defaults an INITEX run starts with. */
        "[\\the\\tolerance|\\the\\hangafter|\\the\\maxdeadcycles]"
        "[\\the\\tracingnesting|\\the\\outputpenalty|\\the\\looseness]%",
        "[abc Q][abc ][10000|1|25][0|0|0]");
}

/* \expanded yields a plain token list: nothing that protected a token from
   this expansion protects it from the next; see docs/DECISIONS.md,
   expanded-is-plain. */
static int test_expanded_is_plain(void)
{
    return run_snippet(
        "\\def\\foo{BAR}\\protected\\def\\pp{PROT}\\toks0={##}"
        /* \unexpanded protects from the enclosing \edef only when it feeds
           that \edef directly. */
        "\\edef\\a{\\expanded{\\unexpanded{\\foo}}}[\\meaning\\a]"
        "\\edef\\b{\\unexpanded{\\foo}}[\\meaning\\b]"
        /* Nor does \noexpand survive the round trip. */
        "\\edef\\c{\\expanded{\\noexpand\\foo}}[\\meaning\\c]"
        /* A doubled parameter marker is halved by the rescan, which expl3's
           hook machinery depends on. */
        "\\edef\\d{\\expanded{\\unexpanded\\expandafter{\\the\\toks0}}}"
        "[\\meaning\\d]"
        "\\edef\\e{\\unexpanded\\expandafter{\\the\\toks0}}[\\meaning\\e]"
        /* A protected macro is still protected, since the enclosing \edef
           inhibits it too. */
        "\\edef\\f{\\expanded{\\pp}}[\\meaning\\f]%",
        "[macro:->BAR][macro:->\\foo ][macro:->BAR][macro:->##]"
        "[macro:->####][macro:->\\pp ]");
}

/* \meaning names a macro's prefixes behind the escape character, in the
   order \protected, \long, \outer, with no separator; see
   docs/DECISIONS.md, meaning-prefixes. */
static int test_meaning_prefixes(void)
{
    return run_snippet(
        "\\def\\a#1{X}\\long\\def\\b#1{X}\\outer\\def\\c{X}"
        "\\protected\\def\\d{X}\\protected\\long\\def\\e#1{X}"
        "\\long\\outer\\def\\f{X}"
        "[\\meaning\\a][\\meaning\\b][\\meaning\\c][\\meaning\\d]"
        "[\\meaning\\e][\\meaning\\f]"
        /* The prefixes follow \escapechar like any other control sequence. */
        "\\escapechar=`\\! [\\meaning\\b]"
        "\\escapechar=-1 [\\meaning\\b]%",
        "[macro:#1->X][\\long macro:#1->X][\\outer macro:->X]"
        "[\\protected macro:->X][\\protected\\long macro:#1->X]"
        "[\\long\\outer macro:->X][!long macro:#1->X][long macro:#1->X]");
}

/* The last-node queries and the PDF objects the document builds; see
   docs/DECISIONS.md, last-node-queries and pdf-objects. */
static int test_last_node_and_pdf_objects(void)
{
    return run_snippet(
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        "[\\the\\lastpenalty|\\the\\lastkern|\\the\\lastskip"
        "|\\the\\lastnodetype]"
        /* The values are captured inside each box, since a character there
           would start a paragraph. */
        "\\setbox0=\\vbox{\\vskip3pt plus1fil \\global\\skip1=\\lastskip "
        "\\global\\count1=\\lastnodetype}"
        "[\\the\\skip1|\\the\\count1]"
        "\\setbox0=\\vbox{\\kern4pt \\global\\dimen1=\\lastkern "
        "\\global\\skip2=\\lastskip \\global\\count2=\\lastnodetype}"
        "[\\the\\dimen1|\\the\\skip2|\\the\\count2]"
        "\\setbox0=\\vbox{\\penalty77 \\global\\count3=\\lastpenalty "
        "\\global\\count4=\\lastnodetype}"
        "[\\the\\count3|\\the\\count4]"
        "\\setbox0=\\vbox{\\copy1 \\global\\count5=\\lastnodetype}"
        "\\setbox0=\\vbox{\\global\\count6=\\lastnodetype}"
        "[\\the\\count5][\\the\\count6]"
        /* Objects are numbered in order, and a reserved number is reused. */
        "\\immediate\\pdfobj{<< /A 1 >>}[\\the\\pdflastobj]"
        "\\immediate\\pdfobj{<< /B 2 >>}[\\the\\pdflastobj]"
        "\\pdfobj reserveobjnum[\\the\\pdflastobj]"
        "\\immediate\\pdfobj useobjnum 3 {<< /C 3 >>}[\\the\\pdflastobj]"
        "\\pdfcatalog{/PageMode /UseOutlines}"
        "\\pdfinfo{/Title (T)}\\pdfrefobj 1 [ok]"
        /* Objects, links, forms and annotations share one counter, and a
           form takes two numbers. */
        "\\setbox4=\\hbox{\\pdfstartlink goto name{a}\\vrule width1pt"
        "\\pdfendlink}[\\the\\pdflastlink]"
        "\\setbox5=\\hbox{\\vrule width1pt}"
        "\\immediate\\pdfxform5 [\\the\\pdflastxform]"
        "\\immediate\\pdfobj{<< /D 4 >>}[\\the\\pdflastobj]"
        /* Every destination type is accepted. */
        "\\setbox6=\\hbox{\\pdfdest name{a}xyz \\pdfdest num 7 fit "
        "\\pdfdest name{b}xyz zoom 1000 \\pdfdest name{c}fitbh "
        "\\pdfdest name{d}fith \\pdfdest name{e}fitbv "
        "\\pdfdest name{f}fitb \\pdfdest name{g}fitv "
        "\\pdfdest name{h}fitr width 10pt height 5pt depth 1pt}"
        "\\pdfoutline goto name{a} count -2 {Title}[ok]%",
        "[0|0.0pt|0.0pt|-1][3.0pt plus 1.0fil|11][4.0pt|0.0pt|12][77|13]"
        "[1][-1][1][2][3][3][ok][4][5][7][ok]");
}

/* \iffontchar, \ifhbox, \ifvbox and \ifvoid. An unimplemented conditional is
   worse than a missing command: a skipped branch miscounts its \else. */
static int test_box_and_font_conditionals(void)
{
    return run_snippet(
        "\\font\\fa=cmr10 \\setbox1=\\hbox{}\\setbox2=\\vbox{}"
        "[\\iffontchar\\fa 65 T\\else F\\fi|\\iffontchar\\fa 128 T\\else F\\fi]"
        "[\\ifhbox1 T\\else F\\fi|\\ifhbox2 T\\else F\\fi|\\ifhbox3 T\\else F\\fi]"
        "[\\ifvbox1 T\\else F\\fi|\\ifvbox2 T\\else F\\fi|\\ifvbox3 T\\else F\\fi]"
        "[\\ifvoid1 T\\else F\\fi|\\ifvoid2 T\\else F\\fi|\\ifvoid3 T\\else F\\fi]"
        /* Skipping counts them, so a nested \else is not mistaken for ours. */
        "\\iffalse\\ifhbox1 A\\else B\\fi\\else C\\fi"
        "\\iffalse\\iffontchar\\fa 65 A\\else B\\fi\\else D\\fi%",
        "[T|F][T|F|F][F|T|F][F|F|T]CD");
}

/* Protrusion and expansion codes belong to the font, not to a group; the tag
   comes from the metric file; see docs/DECISIONS.md, protrusion-codes. */
static int test_protrusion_codes(void)
{
    return run_snippet(
        "\\font\\fa=cmr10 \\font\\fb=cmex10 "
        "[\\the\\lpcode\\fa 65|\\the\\rpcode\\fa 65"
        "|\\the\\efcode\\fa 65|\\the\\tagcode\\fa 65]"
        "\\lpcode\\fa 65=100 \\rpcode\\fa 65=-50 \\efcode\\fa 65=800 "
        "[\\the\\lpcode\\fa 65|\\the\\rpcode\\fa 65|\\the\\efcode\\fa 65]"
        /* A setting made inside a group outlives it. */
        "{\\lpcode\\fa 65=7 }[\\the\\lpcode\\fa 65]"
        /* An undefined character still has codes, but no tag. */
        "[\\the\\lpcode\\fa 128|\\the\\efcode\\fa 128"
        "|\\the\\tagcode\\fa 128]"
        "[\\the\\tagcode\\fa 48|\\the\\tagcode\\fb 16]%",
        "[0|0|1000|1][100|-50|800][7][0|1000|-1][0|2]");
}

/* Character metrics come from the TFM tables, and a font without `at` is used
   at its design size; see docs/DECISIONS.md, font-character-metrics. */
static int test_font_character_metrics(void)
{
    return run_snippet(
        "\\font\\fa=cmr10 \\font\\fb=cmr10 at 12.5pt \\font\\fc=cmr12 "
        "\\font\\fd=cmr12 scaled 1002 "
        "[\\the\\fontcharwd\\fa 65|\\the\\fontcharht\\fa 65]"
        /* Character 60 has depth, character 11 an italic correction. */
        "[\\the\\fontchardp\\fa 60|\\the\\fontcharic\\fa 11]"
        /* A character the font does not define measures zero. */
        "[\\the\\fontcharwd\\fa 128|\\the\\fontcharht\\fa 255]"
        "[\\the\\fontcharwd\\fb 65]"
        /* cmr12 is a twelve point design, so it is not cmr10 at ten points. */
        "[\\the\\fontcharwd\\fc 65|\\the\\fontdimen6\\fc]"
        "[\\the\\fontdimen6\\fd]%",
        "[7.50002pt|6.83331pt][1.94444pt|0.77779pt][0.0pt|0.0pt][9.37502pt]"
        "[8.80824pt|11.74988pt][11.77336pt]");
}

/* \scantokens makes characters of its argument without expanding it, then
   reads them back as a file; see docs/DECISIONS.md, scantokens. */
static int test_scan_tokens(void)
{
    return run_snippet(
        "\\def\\b{XY}\\def\\spc{ }"
        "\\edef\\r{\\scantokens{abc}}<\\detokenize\\expandafter{\\r}>"
        /* \noexpand and \string survive as characters, so they act on the
           re-read, not on the argument. */
        "\\edef\\r{\\scantokens{\\noexpand\\b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{\\string\\b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{\\b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{a\\spc b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{a#b}}<\\detokenize\\expandafter{\\r}>"
        "\\edef\\r{\\scantokens{}}<\\detokenize\\expandafter{\\r}>"
        /* \endlinechar ends each line, so switching it off drops the space. */
        "\\endlinechar=-1 "
        "\\edef\\r{\\scantokens{abc}}<\\detokenize\\expandafter{\\r}>%",
        "<abc ><\\b ><\\b><XY><a b ><a##b ><><abc>");
}

/* \else, \or and \fi met while a conditional is still scanning its own test
   stand for \relax; see docs/DECISIONS.md, unevaluated-conditionals. */
static int test_unevaluated_conditionals(void)
{
    return run_snippet(
        "\\count0=5 "
        "[\\ifnum1<20\\else X\\fi][\\ifnum20<2\\else X\\fi]"
        "[\\ifnum1<2\\fi][\\ifnum\\count0=5\\else X\\fi]"
        "[\\ifcase1\\or A\\else B\\fi][\\ifdim1pt<2pt\\else X\\fi]"
        "[\\ifnum1<2 Y\\else X\\fi][\\ifodd3\\else X\\fi]"
        /* The inserted \relax survives into an \edef's replacement text. */
        "\\edef\\xa{\\ifnum1<20\\else X\\fi}[\\meaning\\xa]"
        "\\edef\\xb{\\ifnum1<2 Y\\else X\\fi}[\\meaning\\xb]%",
        "[][X][][][A][][Y][][macro:->\\relax ][macro:->Y]");
}

/* A \countdef'd control sequence reports the primitive it stands for. */
static int test_defined_register_meanings(void)
{
    return run_snippet(
        "\\countdef\\ca=298 \\dimendef\\da=140 \\skipdef\\sa=48 "
        "\\muskipdef\\ma=7 \\toksdef\\ta=30 "
        "\\chardef\\ha=65 \\mathchardef\\mb=\"2201 "
        "[\\meaning\\ca][\\meaning\\da][\\meaning\\sa][\\meaning\\ma]"
        "[\\meaning\\ta][\\meaning\\ha][\\meaning\\mb][\\meaning\\count]%",
        "[\\count298][\\dimen140][\\skip48][\\muskip7][\\toks30]"
        "[\\char\"41][\\mathchar\"2201][\\count]");
}

/* Kerns are rigid, and rules take a running dimension from the box that
   encloses them; see docs/DECISIONS.md, rules-and-kerns. */
static int test_kerns_and_rules(void)
{
    return run_snippet(
        "\\boxmaxdepth=16383.99998pt "
        "\\baselineskip=12pt \\lineskip=1pt \\lineskiplimit=0pt "
        "\\setbox1=\\hbox{\\vrule height5pt depth2pt width1pt}"
        "\\setbox0=\\hbox{\\copy1\\kern3pt\\copy1}"
        "[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        /* A kern leaves \\prevdepth alone, so interline glue still follows. */
        "\\setbox0=\\vbox{\\copy1\\kern3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\vbox{\\kern3pt\\copy1}[\\the\\ht0|\\the\\dp0]"
        "\\setbox0=\\hbox{\\kern-2pt}[\\the\\wd0]"
        /* A running dimension measures nothing while the box is packaged. */
        "\\setbox0=\\vbox{\\hrule}[\\the\\ht0|\\the\\dp0|\\the\\wd0]"
        "\\setbox0=\\vbox{\\copy1\\hrule}[\\the\\ht0|\\the\\wd0]"
        "\\setbox0=\\vbox{\\hrule width5pt}[\\the\\wd0]"
        "\\setbox0=\\hbox{\\vrule}[\\the\\ht0|\\the\\wd0]"
        "\\setbox0=\\hbox{\\copy1\\vrule}[\\the\\ht0|\\the\\dp0|\\the\\wd0]%",
        "[5.0pt|2.0pt|5.0pt][20.0pt|2.0pt][8.0pt|2.0pt][-2.0pt]"
        "[0.4pt|0.0pt|0.0pt][7.4pt|1.0pt][5.0pt][0.0pt|0.4pt]"
        "[5.0pt|2.0pt|1.4pt]");
}

/* A scaled dimension prints as the shortest decimal that reads back as
   itself; see docs/DECISIONS.md, scaled-printing. */
static int test_scaled_printing(void)
{
    return run_snippet(
        "\\dimen0=26214sp [\\the\\dimen0]\\dimen0=26215sp [\\the\\dimen0]"
        "\\dimen0=1sp [\\the\\dimen0]\\dimen0=10sp [\\the\\dimen0]"
        "\\dimen0=7sp [\\the\\dimen0]\\dimen0=65536sp [\\the\\dimen0]"
        "\\dimen0=65535sp [\\the\\dimen0]"
        "\\dimen0=1073741823sp [\\the\\dimen0]"
        "\\dimen0=891290sp [\\the\\dimen0]\\dimen0=3277sp [\\the\\dimen0]%",
        "[0.4pt][0.40001pt][0.00002pt][0.00015pt][0.0001pt][1.0pt]"
        "[0.99998pt][16383.99998pt][13.6pt][0.05pt]");
}

/* Page state is global rather than grouped, and an empty page reports a
   \maxdimen goal with zero totals; see docs/DECISIONS.md, page-state. */
static int test_page_state(void)
{
    return run_snippet("[\\number\\deadcycles]"
                       "[\\number\\insertpenalties]"
                       "{\\deadcycles=5 }[\\number\\deadcycles]"
                       "\\deadcycles=7 [\\number\\deadcycles]"
                       "[\\the\\pagegoal][\\the\\pagetotal][\\the\\pagedepth]"
                       "\\vskip3pt [\\the\\pagegoal]"
                       "\\pagegoal=50pt [\\the\\pagegoal]%",
                       "[0][0][5][7][16383.99998pt][0.0pt][0.0pt]"
                       "[16383.99998pt][16383.99998pt]");
}

/* Units are keywords, not maximal letter runs, and every conversion below is
   recorded in docs/DECISIONS.md under dimension-unit-arithmetic. */
static int test_dimension_units(void)
{
    const char source[] =
        "\\dimen0=1ptpt "
        "\\dimen1=1cm \\dimen2=1mm \\dimen3=1in \\dimen4=1bp "
        "\\dimen5=1dd \\dimen6=1cc \\dimen7=1pc "
        "\\dimen8=0.3cm \\dimen9=1.7mm \\dimen10=2.54cm "
        "\\dimen11=1.5sp \\dimen12=0.5sp \\dimen13=3.14159dd "
        "\\dimen20=1864679sp "
        "\\dimen21=0.3\\dimen20 \\dimen22=0.1\\dimen20 "
        "\\dimen23=2.54\\dimen20 \\dimen24=0.99999\\dimen20 "
        "\\dimen25=-0.3\\dimen20 "
        "\\dimen26=1truept \\dimen27=1PT "
        "\\skip1=1pt plus2filll minus3fil "
        "\\skip2=1pt plus2filx%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_glue orders = engine.glues[1];
    const struct hstex_glue truncated = engine.glues[2];
    int status =
        result != HSTEX_ENGINE_EOF || engine.dimens[0] != 65536 ||
        engine.dimens[1] != 1864679 || engine.dimens[2] != 186467 ||
        engine.dimens[3] != 4736286 || engine.dimens[4] != 65781 ||
        engine.dimens[5] != 70124 || engine.dimens[6] != 841489 ||
        engine.dimens[7] != 786432 || engine.dimens[8] != 559409 ||
        engine.dimens[9] != 316994 || engine.dimens[10] != 4736274 ||
        engine.dimens[11] != 1 || engine.dimens[12] != 0 ||
        engine.dimens[13] != 220300 || engine.dimens[21] != 559409 ||
        engine.dimens[22] != 186479 || engine.dimens[23] != 4736272 ||
        engine.dimens[24] != 1864650 || engine.dimens[25] != -559409 ||
        engine.dimens[26] != 65536 || engine.dimens[27] != 65536 ||
        orders.width != 65536 || orders.stretch != 131072 ||
        orders.stretch_order != 3U || orders.shrink != 196608 ||
        orders.shrink_order != 1U || truncated.width != 65536 ||
        truncated.stretch != 131072 || truncated.stretch_order != 1U ||
        truncated.shrink != 0;
    if (status != 0) {
        (void)fprintf(stderr, "dimension unit test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_dimensions_and_glue(void)
{
    const char source[] =
        "\\dimen14=\\prevdepth {\\prevdepth=2pt} \\dimen15=\\prevdepth "
        "\\advance\\prevdepth by 1pt \\dimen16=\\prevdepth "
        "\\dimendef\\d=5 \\d=1.5pt {\\d=2pt} "
        "\\dimendef\\twice=6 \\twice=2\\d "
        "\\dimendef\\largest=7 \\largest=16383.99999pt "
        "\\dimen8=7sp \\divide\\dimen8 by 2 "
        "\\dimen9=-7sp \\divide\\dimen9 by 2 "
        "\\baselineskip=13.6pt \\dimen10=472pt "
        "\\divide\\dimen10\\baselineskip \\count10=\\baselineskip "
        "\\baselineskip=655361sp \\dimen11=.5\\baselineskip "
        "\\font\\metricfont=cmr10 \\metricfont "
        "\\dimen12=2.5em \\dimen13=3.25ex "
        "\\skipdef\\s=3 \\s=-1000pt plus 1fill minus 2pt "
        "\\skip4=1pt plus 2fil minus 3pt "
        "\\skip5=2pt plus 4fill minus 1fil "
        "\\advance\\skip4 by \\skip5 "
        "\\multiply\\skip4 by 2 \\divide\\skip4 by 3 "
        "\\muskip4=3mu plus 2fil "
        "\\advance\\muskip4 by 2mu plus 1fil "
        "\\hfuzz=.1pt \\advance\\hfuzz by .2pt "
        "\\parskip=0pt plus 1pt \\advance\\parskip by 2pt%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_glue glue = engine.glues[3];
    const struct hstex_glue arithmetic_glue = engine.glues[4];
    const struct hstex_glue arithmetic_muglue = engine.muglues[4];
    int status = result != HSTEX_ENGINE_EOF || engine.dimens[5] != 98304 ||
                 engine.dimens[14] != -65536000 ||
                 engine.dimens[15] != 131072 ||
                 engine.dimens[16] != 196608 ||
                 engine.dimens[6] != 196608 ||
                 engine.dimens[7] != 1073741823 ||
                 engine.dimens[8] != 3 || engine.dimens[9] != -3 ||
                 engine.dimens[10] != 34 || engine.counts[10] != 891290 ||
                 engine.dimens[11] != 327680 ||
                 engine.dimens[12] != 1638402 ||
                 engine.dimens[13] != 917046 ||
                 glue.width != -65536000 || glue.stretch != 65536 ||
                 glue.stretch_order != 2U || glue.shrink != 131072 ||
                 glue.shrink_order != 0U ||
                 arithmetic_glue.width != 131072 ||
                 arithmetic_glue.stretch != 174762 ||
                 arithmetic_glue.stretch_order != 2U ||
                 arithmetic_glue.shrink != 43690 ||
                 arithmetic_glue.shrink_order != 1U ||
                 arithmetic_muglue.width != 327680 ||
                 arithmetic_muglue.stretch != 196608 ||
                 arithmetic_muglue.stretch_order != 1U ||
                 engine.dimen_parameters[HSTEX_DIMEN_HFUZZ] != 19661 ||
                 engine.glue_parameters[HSTEX_GLUE_PAR_SKIP].width != 131072 ||
                 engine.glue_parameters[HSTEX_GLUE_PAR_SKIP].stretch != 65536;
    if (status != 0) {
        (void)fprintf(stderr, "dimension/glue test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_token_lists(void)
{
    const char source[] =
        "\\def\\x{YZ}\\toksdef\\a=1 \\a={A\\x}"
        "\\toks0={K}\\toks2=\\a "
        "\\edef\\b{\\the\\a}\\def\\c{A\\x}"
        "\\ifx\\b\\c T\\else F\\fi "
        "\\everyjob={J}"
        "\\everyjob\\expandafter{\\the\\everyjob\\the\\toks0}"
        "{\\a={L}\\the\\a}\\the\\a\\the\\toks2\\the\\everyjob%";
    return run_snippet(source, "TLAYZAYZJK");
}

static int test_empty_hboxes(void)
{
    const char source[] =
        "\\count0=1 "
        "{\\setbox5=\\hbox{ }}"
        "\\setbox6=\\hbox to 2pt{}"
        "{\\global\\setbox7=\\hbox spread 3sp{}}"
        "\\baselineskip=13.6pt "
        "\\def\\H{height}\\def\\D{depth}\\def\\W{width}"
        "\\setbox8=\\hbox{\\count0=2 "
        "\\vrule\\H.7\\baselineskip\\D.3\\baselineskip\\W0pt}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_box rule_box = engine.boxes[8];
    uint32_t rule_identifier =
        rule_box.node_count == 1U &&
                (size_t)rule_box.node_start < engine.list_item_count
            ? engine.list_items[rule_box.node_start]
            : 0U;
    const struct hstex_node *rule =
        rule_identifier != 0U && (size_t)rule_identifier <= engine.node_count
            ? &engine.nodes[rule_identifier - 1U]
            : NULL;
    int status = result != HSTEX_ENGINE_EOF || engine.counts[0] != 1 ||
                 engine.boxes[5].kind != HSTEX_BOX_VOID ||
                 engine.boxes[6].kind != HSTEX_BOX_HLIST ||
                 engine.boxes[6].width != 2 * 65536 ||
                 engine.boxes[6].height != 0 || engine.boxes[6].depth != 0 ||
                 engine.boxes[7].kind != HSTEX_BOX_HLIST ||
                 engine.boxes[7].width != 3 ||
                 rule_box.kind != HSTEX_BOX_HLIST || rule_box.width != 0 ||
                 rule_box.height != 623900 || rule_box.depth != 267389 ||
                 rule == NULL || rule->kind != HSTEX_NODE_RULE ||
                 rule->width != 0 || rule->height != 623900 ||
                 rule->depth != 267389;
    if (status != 0) {
        (void)fprintf(stderr, "empty hbox test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_vertical_lists(void)
{
    const char source[] =
        "\\setbox9=\\vbox{\\vskip2pt plus1fil\\penalty50\\vfil}"
        "\\vbox{}\\hbox{}%";
    char path[64];
    if (open_snippet(source, path) != 0) {
        return 1;
    }
    char error[512] = {0};
    struct hstex_engine engine;
    if (prepare_engine(&engine, path, true, error, sizeof(error)) != 0) {
        (void)unlink(path);
        return 1;
    }
    enum hstex_engine_result result;
    do {
        hstex_token token = 0U;
        struct hstex_source_location location;
        result = hstex_engine_next_output(&engine, &token, &location, error,
                                          sizeof(error));
    } while (result == HSTEX_ENGINE_TOKEN);
    const struct hstex_box box = engine.boxes[9];
    const struct hstex_node *glue = engine.node_count >= 1U
                                        ? &engine.nodes[0]
                                        : NULL;
    const struct hstex_node *penalty = engine.node_count >= 2U
                                           ? &engine.nodes[1]
                                           : NULL;
    const struct hstex_node *fil = engine.node_count >= 3U
                                       ? &engine.nodes[2]
                                       : NULL;
    const struct hstex_node *standalone_vbox = engine.node_count >= 4U
                                                   ? &engine.nodes[3]
                                                   : NULL;
    /* The first box to reach the page gets \topskip glue in front of it;
       see docs/DECISIONS.md, the-page-builder. */
    const struct hstex_node *topskip = engine.node_count >= 5U
                                           ? &engine.nodes[4]
                                           : NULL;
    /* The two top-level boxes are separated by interline glue, which the
       reference also emits even when it measures zero. */
    const struct hstex_node *interline = engine.node_count >= 6U
                                             ? &engine.nodes[5]
                                             : NULL;
    const struct hstex_node *standalone_hbox = engine.node_count >= 7U
                                                   ? &engine.nodes[6]
                                                   : NULL;
    int status =
        result != HSTEX_ENGINE_EOF || box.kind != HSTEX_BOX_VLIST ||
        box.width != 0 || box.height != 131072 || box.depth != 0 ||
        box.node_count != 3U || engine.list_item_count != 3U ||
        engine.node_count != 7U || glue == NULL ||
        glue->kind != HSTEX_NODE_GLUE || glue->width != 131072 ||
        glue->value.glue.stretch != 65536 ||
        glue->value.glue.stretch_order != 1U || penalty == NULL ||
        penalty->kind != HSTEX_NODE_PENALTY ||
        penalty->value.penalty != 50 || fil == NULL ||
        fil->kind != HSTEX_NODE_GLUE || fil->width != 0 ||
        fil->value.glue.stretch != 65536 ||
        fil->value.glue.stretch_order != 1U || standalone_vbox == NULL ||
        standalone_vbox->kind != HSTEX_NODE_LIST ||
        standalone_vbox->value.list.box_kind != HSTEX_BOX_VLIST ||
        topskip == NULL || topskip->kind != HSTEX_NODE_GLUE ||
        topskip->width != 0 ||
        interline == NULL || interline->kind != HSTEX_NODE_GLUE ||
        interline->width != 0 || standalone_hbox == NULL ||
        standalone_hbox->kind != HSTEX_NODE_LIST ||
        standalone_hbox->value.list.box_kind != HSTEX_BOX_HLIST;
    if (status != 0) {
        (void)fprintf(stderr, "vertical-list test failed: %s\n", error);
    }
    hstex_engine_destroy(&engine);
    (void)unlink(path);
    return status;
}

static int test_pdf_file_size(void)
{
    char data_path[64];
    if (open_snippet("size", data_path) != 0) {
        return 1;
    }
    char source[512];
    int length = snprintf(
        source, sizeof(source),
        "\\ifnum\\pdffilesize{%s}=4 T\\else F\\fi "
        "X\\pdffilesize{%s-missing}Y%%",
        data_path, data_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        (void)unlink(data_path);
        return 1;
    }
    int result = run_snippet(source, "TXY");
    (void)unlink(data_path);
    return result;
}

int main(void)
{
    if (run_snippet("\\def\\a{Alpha}\\a%", "Alpha") != 0 ||
        run_snippet("\\def\\pair#1#2{[#1/#2]}\\pair A{BC}%",
                    "[A/BC]") != 0 ||
        run_snippet("\\def\\grab#1,#2;{<#2:#1>}\\grab {a,b},c;%",
                    "<c:a,b>") != 0 ||
        run_snippet("\\def\\tag pre#1!{(#1)}\\tag preX!%", "(X)") != 0 ||
        run_snippet("\\def\\grabbrace#1#{[#1]}\\grabbrace abc{X}%",
                    "[abc]X") != 0 ||
        run_snippet("\\def\\a{G}{\\def\\a{L}\\a}\\a%", "LG") != 0 ||
        run_snippet("\\def\\a{G}{\\global\\def\\a{N}}\\a%", "N") != 0 ||
        run_snippet("\\def\\a{Q}\\let\\b=\\a\\def\\a{R}\\b\\a"
                    "\\let\\c=Z\\c%",
                    "QRZ") != 0 ||
        run_snippet("\\def\\after{A}"
                    "\\afterassignment\\after\\dimen0=1pt B%",
                    "AB") != 0 ||
        run_snippet("\\def\\after{A}"
                    "\\afterassignment\\after\\def\\x{X}\\x%",
                    "AX") != 0 ||
        run_snippet("\\afterassignment A\\afterassignment B\\count0=0%",
                    "B") != 0 ||
        run_snippet("\\def\\x{O}\\def\\y{P}"
                    "{\\afterassignment\\global\\let\\x=Z\\let\\y=Y}"
                    "\\x\\y%",
                    "OY") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{B}"
                    "\\def\\check{\\ifx\\next\\b T\\else F\\fi}"
                    "\\let\\next\\a"
                    "{\\futurelet\\next\\check\\b}"
                    "\\ifx\\next\\a T\\else F\\fi"
                    "{\\global\\futurelet\\next\\relax\\b}"
                    "\\ifx\\next\\b T\\else F\\fi%",
                    "TBTBT") != 0 ||
        run_snippet("\\def\\emit{Q}\\def\\use#1#2{#1#2}"
                    "\\def\\letspace#1{\\let#1= }"
                    "\\use{\\letspace\\s}{ }\\emit A\\s B%",
                    "QA B") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{\\def\\a{B}}"
                    "\\expandafter\\a\\b\\a%",
                    "AB") != 0 ||
        run_snippet("\\def\\a{A}\\noexpand\\a\\a%", "A") != 0 ||
        /* ## in a body makes one # in the replacement, and a # that reaches
           the list is refused by the reference the same as any other:
           `\hash Z' sets 1:Z and says so. \message writes it doubled --
           that is the token, not the typesetting. */
        run_snippet("\\def\\hash#1{##1:#1}\\hash Z%", "1:Z") != 0 ||
        run_snippet("\\def\\hash#1{##1:#1}\\message{\\hash Z}%", "") != 0 ||
        run_snippet("\\long\\def\\a#1{X}\\a{one\n\n two}%", "X") != 0 ||
        run_snippet("\\ifnum2<1F\\else T\\fi%", "T") != 0 ||
        run_snippet("\\ifdim1pt<2pt T\\else F\\fi"
                    "\\ifdim2pt=2pt T\\else F\\fi"
                    "\\ifdim3sp>2sp T\\else F\\fi%",
                    "TTT") != 0 ||
        run_snippet("\\ifvmode T\\else F\\fi"
                    "\\ifhmode F\\else T\\fi"
                    "\\ifmmode F\\else T\\fi"
                    "\\ifinner F\\else T\\fi%",
                    "TTTT") != 0 ||
        run_snippet("\\unless\\ifnum1=2T\\else F\\fi"
                    "\\unless\\ifnum\\iftrue1\\else2\\fi=2"
                    "T\\else F\\fi%",
                    "TT") != 0 ||
        run_snippet("\\def\\first#1X{\\fi[#1]}"
                    "\\expandafter\\first\\unless\\if ABX%",
                    "[]") != 0 ||
        run_snippet("\\def\\oddvalue{\\iftrue-3\\else2\\fi}"
                    "\\ifodd\\oddvalue T\\else F\\fi"
                    "\\ifodd4F\\else T\\fi%",
                    "TT") != 0 ||
        run_snippet("\\ifcase0 A\\or X\\else Y\\fi"
                    "\\ifcase1 X\\or B\\or Y\\else Z\\fi"
                    "\\ifcase2 X\\or Y\\or C\\else Z\\fi"
                    "\\ifcase4 X\\or Y\\else D\\fi"
                    "\\ifcase-1 X\\or Y\\else E\\fi"
                    "\\ifcase1 X\\ifcase0 N\\or M\\fi\\or F\\fi%",
                    "ABCDEF") != 0 ||
        run_snippet("\\iffalse A\\iftrue B\\fi\\else C\\fi%", "C") !=
            0 ||
        run_snippet("\\ifx\\unknown\\alsoUnknown T\\else F\\fi%", "T") !=
            0 ||
        run_snippet("\\def\\a#1{[#1]}\\def\\b#1{[#1]}"
                    "\\ifx\\a\\b T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\chardef\\A=65 \\ifnum\\A=65 \\A\\else X\\fi%",
                    "A") != 0 ||
        run_snippet("\\catcode94=7 \\catcode9=10 "
                    "\\catcode`\\^^I=13 "
                    "\\ifnum\\catcode9=13 T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\mathchardef\\M=1000 \\ifnum\\M=1000 T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\meaning\\radical%", "\\radical") != 0 ||
        run_snippet("\\let\\R\\radical\\meaning\\R%", "\\radical") != 0 ||
        run_snippet("\\ifx\\radical\\undefined F\\else T\\fi%", "T") !=
            0 ||
        run_snippet("\\ifx\\marks\\undefined F\\else T\\fi"
                    "\\let\\R\\marks\\meaning\\R%",
                    "T\\marks") != 0 ||
        run_snippet("\\countdef\\n=7 \\n=1 {\\n=2 \\ifnum\\n=2 L\\fi}"
                    "\\ifnum\\n=1 G\\fi%",
                    "LG") != 0 ||
        run_snippet("\\the\\maxdeadcycles/"
                    "{\\maxdeadcycles=100\\relax"
                    "\\the\\maxdeadcycles/}"
                    "\\the\\maxdeadcycles%",
                    "25/100/25") != 0 ||
        run_snippet("\\the\\tracingstats/"
                    "{\\tracingstats=1\\relax\\the\\tracingstats/}"
                    "\\the\\tracingstats%",
                    "0/1/0") != 0 ||
        run_snippet("\\the\\pdfshellescape/"
                    "\\number\\pdfshellescape/"
                    "\\ifnum\\pdfshellescape=0 T\\else F\\fi/"
                    "\\meaning\\pdfshellescape%",
                    "0/0/T/\\pdfshellescape") != 0 ||
        run_snippet("\\the\\lefthyphenmin/\\the\\righthyphenmin/"
                    "{\\lefthyphenmin=2\\righthyphenmin=3\\relax"
                    "\\the\\lefthyphenmin/\\the\\righthyphenmin/}"
                    "\\the\\lefthyphenmin/\\the\\righthyphenmin%",
                    "0/0/2/3/0/0") != 0 ||
        run_snippet("\\dimendef\\d=3 \\d=2sp "
                    "\\ifnum1<\\d T\\else F\\fi/\\number\\d%",
                    "T/2") != 0 ||
        run_snippet("\\countdef\\n=1 \\n=100 \\divide\\n by 6 "
                    "\\multiply\\n -3 \\advance\\n 2 \\number\\n%",
                    "-46") != 0 ||
        run_snippet("\\romannumeral1994 \\def\\a{A}"
                    "\\romannumeral0\\a\\romannumeral-5%",
                    "mcmxcivA") != 0 ||
        run_snippet("\\number\\numexpr2+3*4\\relax/"
                    "\\the\\numexpr(2+3)*4\\relax/"
                    "\\number\\numexpr5/2\\relax/"
                    "\\number\\numexpr-5/2\\relax/"
                    "\\number\\numexpr1-5/2\\relax/"
                    "\\number\\numexpr7/-2\\relax%",
                    "14/20/3/-3/-2/-4") != 0 ||
        run_snippet("\\number\"FF/\\number'17/"
                    "\\the\\numexpr\"10+\"F\\relax%",
                    "255/15/31") != 0 ||
        run_snippet("\\dimendef\\d=0 "
                    "\\d=\\dimexpr 2pt+3pt*4\\relax "
                    "\\the\\d/"
                    "\\the\\dimexpr(2pt+3pt)*4\\relax/"
                    "\\the\\dimexpr5sp/2\\relax/"
                    "\\the\\dimexpr-5sp/2\\relax/"
                    "\\the\\dimexpr7sp/-2\\relax%",
                    "14.0pt/20.0pt/0.00005pt/-0.00005pt/-0.00006pt") != 0 ||
        run_snippet("\\font\\f=cmr10 at 1sp "
                    "\\fontdimen1\\f=123sp "
                    "\\fontdimen20\\f=456sp "
                    "\\hyphenchar\\f=7 "
                    "\\font\\g=cmr10 at 1sp "
                    "\\ifx\\f\\g T\\else F\\fi/"
                    "{\\fontdimen1\\f=321sp}"
                    "\\the\\fontdimen1\\f/"
                    "\\the\\fontdimen20\\f/"
                    "\\the\\hyphenchar\\f/"
                    "\\fontname\\f%",
                    "T/0.0049pt/0.00696pt/7/cmr10 at 0.00002pt") != 0 ||
        run_snippet("\\font\\natural=cmr10 "
                    "\\font\\tiny=cmr10 at 1sp "
                    "\\meaning\\natural/\\meaning\\tiny%",
                    "select font cmr10/"
                    "select font cmr10 at 0.00002pt") != 0 ||
        run_snippet("\\font\\f=cmr10 at 1sp \\f "
                    "\\hyphenchar\\font=9 "
                    "\\the\\hyphenchar\\f/\\fontname\\font%",
                    "9/cmr10 at 0.00002pt") != 0 ||
        run_snippet("\\font\\line=line10\\relax \\the\\fontdimen8\\line%",
                    "0.39998pt") != 0 ||
        run_snippet("\\fontname\\font/\\fontname\\nullfont/"
                    "\\meaning\\nullfont/\\the\\hyphenchar\\nullfont/"
                    "\\the\\skewchar\\nullfont/"
                    "\\the\\fontdimen1\\nullfont%",
                    "nullfont/nullfont/select font nullfont/45/-1/0.0pt") != 0 ||
        run_snippet("\\skipdef\\s=0 "
                    "\\s=\\glueexpr 1pt plus 2pt minus 3pt"
                    "+4pt plus 5pt minus 6pt\\relax "
                    "\\the\\s/"
                    "\\the\\glueexpr 1pt plus 2fil"
                    "+3pt plus 4fill\\relax/"
                    "\\the\\glueexpr(1pt plus 2fil)*3\\relax/"
                    "\\the\\glueexpr 1pt plus 5sp minus 7sp/2\\relax%",
                    "5.0pt plus 7.0pt minus 9.0pt/"
                    "4.0pt plus 4.0fill/"
                    "3.0pt plus 6.0fil/"
                    "0.5pt plus 0.00005pt minus 0.00006pt") != 0 ||
        run_snippet("\\skip0=7pt "
                    "\\muskipdef\\m=0 "
                    "\\m=\\muexpr 1mu plus 2mu minus 3mu"
                    "+3mu plus 4fil minus 5mu\\relax "
                    "\\the\\m/"
                    "{\\m=9mu\\relax \\the\\m/}"
                    "\\the\\m/\\the\\skip0/"
                    "\\the\\muexpr(1mu plus 2fil)*3\\relax%",
                    "4.0mu plus 4.0fil minus 8.0mu/"
                    "9.0mu/"
                    "4.0mu plus 4.0fil minus 8.0mu/"
                    "7.0pt/3.0mu plus 6.0fil") != 0 ||
        run_snippet("\\thinmuskip=3mu plus 2mu minus 1mu "
                    "{\\thinmuskip=9mu\\relax \\the\\thinmuskip/}"
                    "\\the\\thinmuskip/"
                    "\\medmuskip=4mu plus 2mu minus 4mu "
                    "\\thickmuskip=5mu plus 5mu "
                    "\\the\\medmuskip/\\the\\thickmuskip%",
                    "9.0mu/3.0mu plus 2.0mu minus 1.0mu/"
                    "4.0mu plus 2.0mu minus 4.0mu/5.0mu plus 5.0mu") != 0 ||
        run_snippet("\\skip0=3pt plus 4fil minus 5pt "
                    "\\skip1=2\\skip0 "
                    "\\skip2=-\\skip0 "
                    "\\skip3=1.5\\skip0 "
                    "\\the\\skip1/\\the\\skip2/\\the\\skip3%",
                    "6.0pt/-3.0pt plus -4.0fil minus -5.0pt/4.5pt") != 0 ||
        run_snippet("\\def\\name{abc:def}"
                    "\\def\\split#1:#2!{[#1/#2]}"
                    "\\expandafter\\split\\romannumeral`A\\name!%",
                    "[lxvabc/def]") != 0 ||
        run_snippet("\\protected\\def\\zero{0}"
                    "\\edef\\saved{\\romannumeral\\zero X}"
                    "\\def\\zero{1}\\saved%",
                    "X") != 0 ||
        run_snippet("\\def\\use#1#2{#1#2}"
                    "\\use{\\let\\stop= }{ }"
                    "\\edef\\saved{\\ifnum1=1\\stop A\\fi}"
                    "\\def\\expected{A}"
                    "\\ifx\\saved\\expected T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\countdef\\n=2 \\n=7 "
                    "\\def\\two#1{\\ifnum#1<10 0\\fi\\number#1}"
                    "\\edef\\saved{\\two{\\the\\n}}\\n=42 \\saved%",
                    "07") != 0 ||
        run_snippet("\\def\\a{A}\\edef\\saved{\\noexpand\\a}"
                    "\\def\\a{B}\\saved%",
                    "B") != 0 ||
        run_snippet("\\protected\\def\\a{A}\\edef\\saved{\\a}"
                    "\\def\\a{B}\\saved%",
                    "B") != 0 ||
        run_snippet("\\def\\a{A}\\def\\b{B}"
                    "\\expanded{<\\a\\b>}"
                    "\\expanded{\\noexpand\\a}%",
                    "<AB>A") != 0 ||
        run_snippet("\\def\\trim#1{[#1]}"
                    "\\edef\\saved{\\expandafter\\trim"
                    "\\expanded{{A\\iffalse}}}\\fi B}}}"
                    "\\saved%",
                    "[AB]") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\expandafter\\ifx\\expanded{\\unexpanded{e}}e"
                    "T\\else F\\fi"
                    "\\expandafter\\ifx\\expanded{\\noexpand\\a}\\a "
                    "T\\else F\\fi"
                    "\\expandafter\\ifx\\noexpand\\a\\a "
                    "T\\else F\\fi%",
                    "TTF") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\def\\choose{\\if\\noexpand\\a\\noexpand\\a"
                    "\\edef\\saved{X}\\fi}"
                    "\\choose\\saved%",
                    "X") != 0 ||
        run_snippet("\\def\\comma#1,{[#1]}"
                    "\\expandafter\\comma"
                    "\\expanded{\\unexpanded{A,}}"
                    "\\def\\prefix A#1{<#1>}"
                    "\\expandafter\\prefix"
                    "\\expanded{\\unexpanded{A}}B%",
                    "[A]<B>") != 0 ||
        run_snippet("\\def\\left{abc}"
                    "\\pdfstrcmp{abc}{abc}/"
                    "\\pdfstrcmp{abc}{abd}/"
                    "\\pdfstrcmp{abd}{abc}/"
                    "\\pdfstrcmp{\\left}{abc}/"
                    "\\pdfstrcmp{\\noexpand\\relax}"
                    "{\\noexpand\\relax}%",
                    "0/-1/1/0/0") != 0 ||
        run_snippet("\\expanded\\expanded{{A}}%", "A") != 0 ||
        run_snippet("\\protected\\def\\a{A}"
                    "\\edef\\saved{\\expanded{\\a}}"
                    "\\def\\a{B}\\saved%",
                    "B") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\edef\\saved{\\unexpanded{\\a}}"
                    "\\def\\a{B}\\saved\\unexpanded{\\a}%",
                    "BB") != 0 ||
        /* The # tokens survive \unexpanded and \the intact, which is what
           this checks; what reaches the list is then refused by the
           reference the same as any other stray #, so 1/2 is set. */
        run_snippet("\\edef\\saved{\\unexpanded{#1}}\\saved/"
                    "\\toks0={#2}\\edef\\fromtoks{\\the\\toks0}"
                    "\\fromtoks%",
                    "1/2") != 0 ||
        run_snippet("\\def\\a{A}\\unexpanded\\expanded{{\\a}}%",
                    "A") != 0 ||
        run_snippet("\\lowercase{AB}\\uppercase{ab}"
                    "\\def\\opening{{C}}\\lowercase\\opening%",
                    "abABc") != 0 ||
        run_snippet("A\\ignorespaces   B"
                    "\\def\\s{ }\\ignorespaces\\s C%",
                    "ABC") != 0 ||
        run_snippet("\\ifx\\batchmode\\errorstopmode T\\else F\\fi"
                    "\\batchmode A\\dump B%",
                    "FA") != 0 ||
        run_snippet("\\lccode`*=32 "
                    "\\def\\samecat#1{\\ifcat#1*P\\else F\\fi}"
                    "\\lowercase{\\samecat*}%",
                    "P") != 0 ||
        run_snippet("\\catcode`~=13 \\def~{T}"
                    "\\catcode`!=13 \\def!{B}"
                    "\\lccode`~=`! \\lowercase{~}%",
                    "B") != 0 ||
        run_snippet("\\def\\expected{\\relax}"
                    "\\expanded{\\noexpand\\def\\noexpand\\saved"
                    "{\\noexpand\\relax}}"
                    "\\ifx\\saved\\expected T\\else F\\fi%",
                    "T") != 0 ||
        run_snippet("\\def\\a{A}"
                    "\\def\\same#1{\\ifx\\a#1T\\else F\\fi}"
                    "\\expanded{\\unexpanded{\\same}"
                    "\\unexpanded{\\a}}%",
                    "T") != 0 ||
        run_snippet("\\def\\a{X}"
                    "\\detokenize{A \\a!\\, {B}}"
                    "\\detokenize{#1}%",
                    "A \\a !\\, {B}##1") != 0 ||
        run_snippet("\\detokenize\\expanded{{A}}%", "A") != 0 ||
        run_snippet(
            "\\expandafter\\def"
            "\\csname\\detokenize{name_with:chars}\\endcsname{D}"
            "\\csname name_with:chars\\endcsname%",
            "D") != 0 ||
        run_snippet("\\def\\a{./}\\def\\strip#1>{}"
                    "\\edef\\saved{\\expandafter\\strip\\meaning\\a}"
                    "\\saved%",
                    "./") != 0 ||
        /* \string makes tokens, so the byte goes through as it stands;
           only what is printed to the terminal uses ^^ notation. See
           docs/DECISIONS.md, string-is-not-escaped. */
        run_snippet("{\\catcode`\\^=7 \\catcode`\\^^J=13 "
                    "\\xdef\\saved{\\string^^J}}"
                    "\\saved%",
                    "\n") != 0 ||
        run_snippet("\\if AAT\\else F\\fi\\if ABF\\else T\\fi%",
                    "TT") != 0 ||
        run_snippet("\\def\\base#1{\\if c#1N\\else"
                    "\\if o#1n\\else X\\fi\\fi}"
                    "\\def\\digit{\\iftrue1\\else2\\fi}"
                    "\\if n\\base oT\\else F\\fi"
                    "\\ifnum\\digit=1T\\else F\\fi%",
                    "TT") != 0 ||
        run_snippet("\\let\\x=A\\def\\letter{A}\\chardef\\A=65 "
                    "\\ifcat AB1\\else0\\fi"
                    "\\ifcat A70\\else1\\fi"
                    "\\ifcat\\x B1\\else0\\fi"
                    "\\ifcat\\letter B1\\else0\\fi"
                    "\\ifcat\\A\\relax1\\else0\\fi"
                    "\\ifcat\\relax\\def1\\else0\\fi"
                    "\\ifcat\\noexpand\\missing\\noexpand\\other"
                    "1\\else0\\fi%",
                    "1111111") != 0 ||
        run_snippet("\\sfcode`\\)=0 \\ifnum\\sfcode`\\)=0 T\\else F\\fi "
                    "\\ifdefined\\sfcode T\\else F\\fi "
                    "\\ifdefined\\unknown F\\else T\\fi%",
                    "TTT") != 0 ||
        run_snippet(
            "\\expandafter\\ifx\\csname absent:name\\endcsname\\relax "
            "T\\else F\\fi"
            "\\def\\part{dynamic}"
            "\\expandafter\\def\\csname\\part-name\\endcsname{D}"
            "\\csname dynamic-name\\endcsname"
            "\\expandafter\\def\\csname a b\\endcsname{S}"
            "\\csname a b\\endcsname"
            "{\\csname scoped-name\\endcsname}"
            "\\ifdefined\\scoped-name F\\else L\\fi"
            "\\expandafter\\ifx\\csname scoped-name\\endcsname\\relax "
            "T\\else F\\fi%",
            "TDSLT") != 0 ||
        run_snippet(
            "\\def\\part{made}"
            "\\expandafter\\def\\csname made-up\\endcsname{M}"
            "\\ifcsname absent\\endcsname A\\else B\\fi"
            "\\ifdefined\\absent C\\else D\\fi"
            "\\ifcsname\\part-up\\endcsname E\\else F\\fi"
            "\\ifcsname tokenized\\endcsname G\\else H\\fi"
            "\\ifdefined\\tokenized I\\else J\\fi%",
            "BDEHJ") != 0 ||
        run_snippet(
            "\\def\\piece{name}"
            "\\expandafter\\def"
            "\\csname\\expanded{\\noexpand\\piece}\\endcsname{X}"
            "\\name%",
            "X") != 0 ||
        run_snippet("\\catcode`\\@=11 \\def\\word@word{X}\\word@word%",
                    "X") != 0 ||
        run_snippet("\\ifdefined\\widowpenalties T\\else F\\fi%", "T") !=
            0 ||
        run_snippet("{\\aftergroup A\\aftergroup B}C"
                    "{\\aftergroup D{\\aftergroup E}\\aftergroup F}G%",
                    "ABCEDFG") != 0 ||
        run_snippet("\\ifnum\\currentgrouplevel=0 T\\else F\\fi"
                    "{\\ifnum\\currentgrouplevel=1 T\\else F\\fi}"
                    "\\ifnum\\currentgrouplevel=0 T\\else F\\fi%",
                    "TTT") != 0 ||
        run_snippet("\\edef\\a{\\meaning\\over}"
                    "\\edef\\b{\\string\\over}"
                    "\\ifx\\a\\b T\\else F\\fi "
                    "\\let\\savedover\\over "
                    "\\edef\\c{\\meaning\\savedover}"
                    "\\ifx\\b\\c T\\else F\\fi%",
                    "TT") != 0 ||
        expect_failure("\\endcsname%", "extra endcsname") != 0 ||
        /* An undefined control sequence is reported and forgotten, as the
           reference forgets it: what follows is still set. In a
           conditional it is gone rather than standing in as \relax, so
           \ifcat's operands are \relax and T, and it comes out false. */
        run_snippet("\\unknown after%", "after") != 0 ||
        run_snippet("\\ifcat\\missing\\relax T\\else F\\fi%", "F") != 0 ||
        /* A number that is not there is the reference's "Missing number",
           which is recovered from with zero rather than stopping the run. */
        run_snippet("\\expanded{\\number}%", "0") != 0 ||
        /* A unit that is not there is reported and taken as points, as the
           reference does: \baselineskip=1\relax comes out 1.0pt. */
        run_snippet("\\baselineskip=1\\relax\\the\\baselineskip%",
                    "1.0pt") != 0 ||
        /* An order past filll is reported and taken as filll. */
        run_snippet("\\skip1=1pt plus2fillll\\relax\\the\\skip1%",
                    "1.0pt plus 2.0filll") != 0 ||
        run_snippet("\\dimen0=1ptpt\\dimen1=1inch%", "ptch") != 0 ||
        /* \errmessage is a fault of the document's, which the reference
           reports and carries on from -- the text after it is still set. */
        run_snippet("\\def\\why{expanded}"
                    "\\errmessage{ERRMESSAGE: \\why}after%",
                    "after") != 0 ||
        /* A \par in a non-long argument does not stop the run: the
           reference forgets the whole call, reads the \par again, and goes
           on -- so the text after it is still set, and the } that had no
           call left to close draws "Too many }'s". */
        run_snippet("\\def\\a#1{X}\\a{one\n\n two}after%", "twoafter") != 0 ||
        test_a_definition_read_while_it_is_replaced() != 0 ||
        test_a_body_that_asks_for_no_argument() != 0 ||
        test_arguments_put_in_more_than_once() != 0 ||
        test_what_a_page_leaves_behind() != 0 ||
        test_a_format_a_run_starts_from() != 0 ||
        test_macro_flags() != 0 || test_ini_bootstrap() != 0 ||
        test_input_primitive() != 0 || test_job_name() != 0 ||
        test_hyphenation_data() != 0 ||
        test_document_job_transition() != 0 || test_file_streams() != 0 ||
        test_whatsits() != 0 || test_whatsits_on_an_empty_page() != 0 ||
        test_the_current_font_is_grouped() != 0 ||
        test_discretionaries() != 0 || test_hyphenation() != 0 ||
        test_interword_glue() != 0 ||
        test_parskip_in_the_outermost_list() != 0 ||
        test_character_protrusion() != 0 ||
        test_the_discretionary_after_an_explicit_hyphen() != 0 ||
        test_math_nodes() != 0 || test_hyphenating_ligatures() != 0 ||
        test_what_a_line_keeps_of_its_break() != 0 ||
        test_math_text_characters() != 0 ||
        test_infinite_page_penalty() != 0 ||
        test_emergency_stretch() != 0 ||
        test_display_skips_and_widows() != 0 ||
        test_prevdepth_belongs_to_one_list() != 0 ||
        test_margin_kerns_of_a_line() != 0 ||
        test_whatsit_text_is_cut() != 0 ||
        test_only_a_character_is_centred() != 0 ||
        test_math_accents() != 0 ||
        test_a_list_that_is_one_box() != 0 ||
        test_a_line_that_breaks_at_a_penalty() != 0 ||
        test_a_hyphen_that_ligatures() != 0 ||
        test_the_last_node_of_a_page() != 0 ||
        test_pdf_destinations() != 0 ||
        test_tracing_paragraphs() != 0 ||
        test_the_first_line_protrudes_too() != 0 ||
        test_the_last_line_is_measured_square() != 0 ||
        test_a_display_closes_its_group_last() != 0 ||
        test_lines_carry_on_past_a_display() != 0 ||
        test_prevdepth_inside_noalign() != 0 ||
        test_a_short_display_skip() != 0 ||
        test_the_size_before_a_display() != 0 ||
        test_a_display_squeezed_to_fit() != 0 ||
        test_what_stands_between_delimiters() != 0 ||
        test_delimiters_are_atoms() != 0 ||
        test_a_fence_is_set_in_place() != 0 ||
        test_an_accent_alone_in_braces() != 0 ||
        test_a_page_that_breaks_at_a_display() != 0 ||
        test_a_kern_in_mu() != 0 ||
        test_the_look_before_an_entry() != 0 ||
        test_a_space_leaves_the_factor_alone() != 0 ||
        test_protruding_past_a_kern() != 0 ||
        test_a_row_that_stops_early() != 0 ||
        test_a_rule_between_rows() != 0 ||
        test_a_limit_at_its_own_width() != 0 ||
        test_a_vcenter_in_braces() != 0 ||
        test_a_mark_in_a_list() != 0 ||
        test_a_definition_nothing_holds() != 0 ||
        test_the_page_description() != 0 ||
        test_leaders_on_a_page() != 0 ||
        test_what_the_document_says_about_itself() != 0 ||
        test_the_action_a_file_opens_on() != 0 ||
        test_the_outline_of_a_document() != 0 ||
        test_the_version_a_file_states() != 0 ||
        test_the_box_a_font_fills() != 0 ||
        test_what_a_string_escapes() != 0 ||
        test_the_pdf_file() != 0 ||
        test_colour_on_a_page() != 0 ||
        test_the_place_a_file_names() != 0 ||
        test_the_place_after_a_step() != 0 ||
        test_where_a_correction_leaves_the_text() != 0 ||
        test_a_step_too_short_to_name() != 0 ||
        test_a_place_named_for_a_font_of_its_own() != 0 ||
        test_the_correction_an_array_cannot_carry() != 0 ||
        test_the_size_a_correction_is_worked_out_from() != 0 ||
        test_the_width_a_file_states() != 0 ||
        test_thin_rules() != 0 ||
        test_the_middle_of_a_thin_rule() != 0 ||
        test_leaders_in_the_pdf() != 0 ||
        test_the_text_position_in_the_file() != 0 ||
        test_annotations_on_a_page() != 0 ||
        test_destinations_in_the_file() != 0 ||
        test_the_fonts_a_page_names() != 0 ||
        test_a_paragraph_the_end_ends() != 0 ||
        test_a_link_in_a_list() != 0 ||
        test_how_wide_a_movement_is() != 0 ||
        test_a_box_at_the_edge() != 0 ||
        test_a_margin_kern_behind_the_leftskip() != 0 ||
        test_no_hyphens_inside_a_formula() != 0 ||
        test_a_line_too_short_to_measure() != 0 ||
        test_a_paragraph_a_brace_ends() != 0 ||
        test_what_a_split_leaves_behind() != 0 ||
        test_an_insertion_in_a_list() != 0 ||
        test_an_insertion_on_a_page() != 0 ||
        test_an_insertion_that_does_not_fit() != 0 ||
        test_marks_on_a_page() != 0 ||
        test_a_box_register_in_a_formula() != 0 ||
        test_only_a_character_is_centred_still() != 0 ||
        test_the_italic_of_a_math_ligature() != 0 ||
        test_a_number_beside_a_squeezed_equation() != 0 ||
        test_large_operators() != 0 ||
        test_operators_that_are_lists() != 0 ||
        test_the_space_factor_of_a_ligature() != 0 ||
        test_what_resets_the_space_factor() != 0 ||
        test_an_accent_over_a_script() != 0 ||
        test_an_empty_atom() != 0 ||
        test_a_repeating_preamble() != 0 ||
        test_a_column_of_negative_width() != 0 ||
        test_superscripts_in_display_style() != 0 ||
        test_protrusion_into_boxes() != 0 ||
        test_italic_correction_needs_a_character() != 0 ||
        test_breaking_inside_a_ligature() != 0 ||
        test_accent_kerns() != 0 || test_protrusion_of_a_negative_code() != 0 ||
        test_colour_stack_nodes() != 0 ||
        test_the_final_break_is_hyphenated() != 0 ||
        /* A parameter-category character is displayed doubled, so that the
           display reads back as the same token. */
        /* The # from ## is refused where it reaches the list, and the ones
           \meaning writes are ordinary characters and are set. */
        run_snippet("\\def\\s#1{##1#1}[\\s{Q}][\\meaning\\s]%",
                    "[1Q][macro:#1->##1#1]") != 0 ||
        run_snippet("[\\pdfescapestring{a b(c)\\string\\\\}]"
                    "[\\pdfescapename{a b}][\\pdfescapehex{AB}]"
                    "[\\pdfunescapehex{4142}][\\the\\pdfpxdimen]%",
                    "[a\\040b\\(c\\)\\\\\\\\][a#20b][4142][AB][1.00375pt]") != 0 ||
        /* A control sequence inserted by \the is ordinary in execution, so
           \expandafter expands it. cleveref builds definitions this way. */
        run_snippet("\\toks0={\\expandafter\\def\\csname AB\\endcsname}"
                    "\\the\\toks0 {X}"
                    "[\\expandafter\\meaning\\csname AB\\endcsname]%",
                    "[macro:->X]") != 0 ||
        /* A `true` unit is measured before magnification. */
        run_snippet("\\mag=2000 \\dimen0=1truept \\dimen1=2.54truecm "
                    "\\dimen2=1truein \\dimen3=1pt "
                    "[\\number\\dimen0][\\number\\dimen1]"
                    "[\\number\\dimen2][\\number\\dimen3]%",
                    "[32768][2368122][2368143][65536]") != 0 ||
        /* \protected stops expansion while \edef builds a token list, but a
           csname is still expanded in full. */
        run_snippet("\\protected\\def\\PP{AB}\\def\\ABC{Z}"
                    "\\edef\\z{\\csname \\PP C\\endcsname}\\z"
                    "\\edef\\w{\\ifcsname \\PP C\\endcsname Y\\else N\\fi}\\w%",
                    "ZY") != 0 ||
        /* \noexpand before a parameter marker leaves it a parameter marker,
           unlike \unexpanded and \the, whose tokens are inserted verbatim.
           hyperref's \HyLang@DeclareLang depends on the difference. */
        run_snippet("\\def\\o#1{\\edef\\x##1##2{[\\noexpand##1|\\noexpand##2|#1]}}"
                    "\\o{A}\\x{P}{Q}%",
                    "[P|Q|A]") != 0 ||
        test_font_character_metrics() != 0 || test_protrusion_codes() != 0 ||
        test_box_and_font_conditionals() != 0 ||
        test_box_grammar_and_spacing() != 0 || test_paragraphs() != 0 ||
        test_starting_a_paragraph() != 0 || test_implicit_braces() != 0 ||
        test_streaming_box_bodies() != 0 || test_math_mode() != 0 ||
        test_math_scripts() != 0 || test_alignments() != 0 ||
        test_display_math() != 0 || test_math_choices() != 0 ||
        test_badness() != 0 || test_line_breaking() != 0 ||
        test_accents() != 0 || test_equation_numbers() != 0 || test_vcenter() != 0 ||
        test_alignment_entries() != 0 || test_delimiters() != 0 ||
        test_left_right() != 0 || test_implicit_characters() != 0 || test_preamble_forms() != 0 || test_display_alignments() != 0 ||
        test_every_cr() != 0 || test_fractions() != 0 || test_parshape() != 0 || test_formula_spacing() != 0 ||
        test_conditionals_across_boxes() != 0 || test_radicals() != 0 ||
        test_over_and_underline() != 0 ||
        test_missing_characters() != 0 || test_leaders() != 0 ||
        test_string_bytes() != 0 ||
        test_middle_delimiters() != 0 || test_nonscript() != 0 ||
        test_ending_a_paragraph() != 0 ||
        test_expansion_spaces() != 0 ||
        test_oversize_boxes() != 0 || test_page_totals() != 0 || test_output_routine() != 0 || test_vsplit() != 0 || test_glue_set() != 0 || test_showbox() != 0 || test_recoverable_errors() != 0 ||
        test_paragraph_display() != 0 || test_characters() != 0 || test_horizontal_glue() != 0 ||
        test_unboxing_and_colour_stacks() != 0 || test_every_eof() != 0 || test_expanded_is_plain() != 0 || test_meaning_prefixes() != 0 ||
        test_last_node_and_pdf_objects() != 0 ||
        test_scan_tokens() != 0 || test_unevaluated_conditionals() != 0 ||
        test_defined_register_meanings() != 0 ||
        test_box_shift_and_packaging() != 0 || test_kerns_and_rules() != 0 ||
        test_scaled_printing() != 0 ||
        test_page_state() != 0 || test_dimension_units() != 0 ||
        test_pdftex_identification() != 0 ||
        /* \typeout writes to an allocated but unopened stream, and \write
           accepts any stream number. Neither may fail or consume output. */
        run_snippet("\\immediate\\write5{A}\\immediate\\write200{B}"
                    "\\immediate\\write-1{C}X%",
                    "X") != 0 ||
        run_snippet("A\\end B%", "A") != 0 ||
        /* The first box on the page settles its goal; see
           docs/DECISIONS.md, the-page-builder. */
        run_snippet("\\the\\pagegoal%", "16383.99998pt") != 0 ||
        run_snippet("\\vsize=123pt \\topskip=0pt "
                    "\\hbox{}\\the\\pagegoal|\\the\\pagetotal%",
                    "123.0pt|0.0pt") != 0 ||
        test_dimensions_and_glue() != 0 || test_token_lists() != 0 ||
        test_empty_hboxes() != 0 || test_vertical_lists() != 0 ||
        test_pdf_file_size() != 0) {
        return 1;
    }
    return 0;
}
