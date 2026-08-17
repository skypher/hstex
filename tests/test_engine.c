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
    (void)unlink(path);
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
    return run_document(
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbreadth=2"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\v"
        "fuzz=1000pt \\boxmaxdepth=16383.99998pt \\font\\f=cmr1"
        "0 \\f \\hsize=100pt \\parindent=20pt \\baselineskip=12"
        "pt \\lineskip=1pt \\lineskiplimit=2pt \\parskip=3pt pl"
        "us1pt \\parfillskip=0pt plus1fil \\leftskip=1pt \\righ"
        "tskip=2pt \\tolerance=10000 \\pretolerance=-1 \\showbo"
        "x9\\setbox0=\\hbox{}\\showbox0\\setbox0=\\hbox to100pt"
        "{\\hskip0pt plus1fil\\vrule width5pt height3pt depth1p"
        "t\\hskip0pt plus2fil}\\showbox0\\setbox0=\\hbox to30pt"
        "{\\hskip10pt plus5pt minus2pt AB\\kern3pt\\lower2pt\\h"
        "box{}}\\showbox0\\setbox0=\\vbox to40pt{\\hrule height"
        "2pt \\vskip4pt plus1fill \\hbox{A}\\penalty150 \\mover"
        "ight3pt\\hbox{}}\\showbox0\\setbox0=\\hbox spread5pt{"
        "\\hskip0pt minus1pt\\hskip0pt minus3pt}\\showbox0\\set"
        "box0=\\hbox to20pt{\\leaders\\hbox{\\vrule width2pt he"
        "ight1pt}\\hfil}\\showbox0\\setbox0=\\hbox to5pt{\\hski"
        "p20pt minus3pt}\\showbox0\\setbox0=\\hbox to5pt{\\hski"
        "p20pt minus1fil}\\showbox0\\setbox0=\\vbox{\\noindent "
        "AB\\par}\\showbox0\\setbox0=\\vbox{\\hbox{A}\\hbox{B}"
        "\\hbox{C}}\\showboxdepth=1 \\showboxbreadth=2\\showbox"
        "0 \\showboxdepth=0 \\showboxbreadth=20 \\showbox0\\sho"
        "wboxdepth=10 \\setbox0=\\hbox{A\\/B}\\showbox0\\setbox"
        "0=\\hbox{\\char32}\\showbox0%",
        "> \\box9=void\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(0.0+0.0)x0.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(3.0+1.0)x100.0, glue set 31.66667fil\n"
        ".\\glue 0.0 plus 1.0fil\n"
        ".\\rule(3.0+1.0)x5.0\n"
        ".\\glue 0.0 plus 2.0fil\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(6.83331+2.0)x30.0, glue set 0.48332\n"
        ".\\glue 10.0 plus 5.0 minus 2.0\n"
        ".\\f A\n"
        ".\\f B\n"
        ".\\kern 3.0\n"
        ".\\hbox(0.0+0.0)x0.0, shifted 2.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\vbox(40.0+0.0)x7.50002, glue set 15.16669fill\n"
        ".\\rule(2.0+0.0)x*\n"
        ".\\glue 4.0 plus 1.0fill\n"
        ".\\hbox(6.83331+0.0)x7.50002\n"
        "..\\f A\n"
        ".\\penalty 150\n"
        ".\\glue(\\baselineskip) 12.0\n"
        ".\\hbox(0.0+0.0)x0.0, shifted 3.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(0.0+0.0)x5.0\n"
        ".\\glue 0.0 minus 1.0\n"
        ".\\glue 0.0 minus 3.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(1.0+0.0)x20.0, glue set 20.0fil\n"
        ".\\leaders 0.0 plus 1.0fil\n"
        "..\\hbox(1.0+0.0)x2.0\n"
        "...\\rule(1.0+*)x2.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(0.0+0.0)x5.0, glue set - 1.0\n"
        ".\\glue 20.0 minus 3.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(0.0+0.0)x5.0, glue set - 15.0fil\n"
        ".\\glue 20.0 minus 1.0fil\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\vbox(6.83331+0.0)x100.0\n"
        ".\\hbox(6.83331+0.0)x100.0, glue set 82.41663fil\n"
        "..\\glue(\\leftskip) 1.0\n"
        "..\\f A\n"
        "..\\f B\n"
        "..\\penalty 10000\n"
        "..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 2.0\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\vbox(30.83331+0.0)x7.50002\n"
        ".\\hbox(6.83331+0.0)x7.50002 []\n"
        ".\\glue(\\baselineskip) 5.16669\n"
        ".etc.\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\vbox(30.83331+0.0)x7.50002 []\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(6.83331+0.0)x14.58337\n"
        ".\\f A\n"
        ".\\kern 0.0\n"
        ".\\f B\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(4.30554+0.0)x2.77779\n"
        ".\\f  \n"
        "\n"
        "! OK.\n");
}

/* A whole paragraph, box for box, against the reference: the lines it
   was broken into, the glue each was set with, the kerns and the
   ligatures. See docs/DECISIONS.md, showbox. */
static int test_paragraph_display(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=3 \\showboxbreadth=10"
        "0\\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt\\boxmaxdepth=16383.99998pt\\font\\f=cmr10 "
        "\\f \\hsize=180pt \\parindent=20pt \\baselineskip=12pt"
        "\\lineskip=1pt \\lineskiplimit=0pt \\parfillskip=0pt p"
        "lus1fil\\tolerance=10000 \\pretolerance=100 \\clubpena"
        "lty=150 \\widowpenalty=150 \\interlinepenalty=0 \\brok"
        "enpenalty=100 \\linepenalty=10 \\adjdemerits=10000 \\s"
        "paceskip=0pt \\sfcode`\\.=1000\\setbox0=\\vbox{The qui"
        "ck brown fox jumps over the lazy dog and then runsaway"
        " into the deep dark forest where nobody can find him a"
        "t all.\\par}\\showbox0%",
        "> \\box0=\n"
        "\\vbox(42.94444+0.0)x180.0\n"
        ".\\hbox(6.94444+1.94444)x180.0, glue set 0.12775\n"
        "..\\hbox(0.0+0.0)x20.0\n"
        "..\\f T\n"
        "..\\f h\n"
        "..\\f e\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f q\n"
        "..\\f u\n"
        "..\\f i\n"
        "..\\f c\n"
        "..\\kern-0.27779\n"
        "..\\f k\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f b\n"
        "..\\f r\n"
        "..\\f o\n"
        "..\\kern-0.27779\n"
        "..\\f w\n"
        "..\\f n\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f f\n"
        "..\\f o\n"
        "..\\kern-0.27779\n"
        "..\\f x\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f j\n"
        "..\\f u\n"
        "..\\f m\n"
        "..\\f p\n"
        "..\\f s\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f o\n"
        "..\\kern-0.27779\n"
        "..\\f v\n"
        "..\\kern-0.27779\n"
        "..\\f e\n"
        "..\\f r\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f t\n"
        "..\\f h\n"
        "..\\f e\n"
        "..\\glue(\\rightskip) 0.0\n"
        ".\\penalty 150\n"
        ".\\glue(\\baselineskip) 3.11111\n"
        ".\\hbox(6.94444+1.94444)x180.0, glue set - 0.43933\n"
        "..\\f l\n"
        "..\\f a\n"
        "..\\f z\n"
        "..\\f y\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f d\n"
        "..\\f o\n"
        "..\\f g\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f a\n"
        "..\\f n\n"
        "..\\f d\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f t\n"
        "..\\f h\n"
        "..\\f e\n"
        "..\\f n\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f r\n"
        "..\\f u\n"
        "..\\f n\n"
        "..\\f s\n"
        "..\\f a\n"
        "..\\kern-0.27779\n"
        "..\\f w\n"
        "..\\kern-0.27779\n"
        "..\\f a\n"
        "..\\kern-0.27779\n"
        "..\\f y\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f i\n"
        "..\\f n\n"
        "..\\kern-0.27779\n"
        "..\\f t\n"
        "..\\f o\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f t\n"
        "..\\f h\n"
        "..\\f e\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f d\n"
        "..\\f e\n"
        "..\\f e\n"
        "..\\f p\n"
        "..\\glue(\\rightskip) 0.0\n"
        ".\\glue(\\baselineskip) 3.11111\n"
        ".\\hbox(6.94444+1.94444)x180.0, glue set - 0.33934\n"
        "..\\f d\n"
        "..\\f a\n"
        "..\\f r\n"
        "..\\f k\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f f\n"
        "..\\f o\n"
        "..\\f r\n"
        "..\\f e\n"
        "..\\f s\n"
        "..\\f t\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f w\n"
        "..\\f h\n"
        "..\\f e\n"
        "..\\f r\n"
        "..\\f e\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f n\n"
        "..\\f o\n"
        "..\\f b\n"
        "..\\kern0.27779\n"
        "..\\f o\n"
        "..\\kern0.27779\n"
        "..\\f d\n"
        "..\\f y\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f c\n"
        "..\\f a\n"
        "..\\f n\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f ^^L (ligature fi)\n"
        "..\\f n\n"
        "..\\f d\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f h\n"
        "..\\f i\n"
        "..\\f m\n"
        "..\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        "..\\f a\n"
        "..\\f t\n"
        "..\\glue(\\rightskip) 0.0\n"
        ".\\penalty 150\n"
        ".\\glue(\\baselineskip) 3.11111\n"
        ".\\hbox(6.94444+0.0)x180.0, glue set 166.66663fil\n"
        "..\\f a\n"
        "..\\f l\n"
        "..\\f l\n"
        "..\\f .\n"
        "..\\penalty 10000\n"
        "..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n"
        "\n"
        "! OK.\n");
}

/* The output routine; see docs/DECISIONS.md, the-output-routine. */
static int test_output_routine(void)
{
    return run_document(
        "\\vsize=50pt \\maxdepth=2pt \\topskip=10pt \\baselineskip="
        "12pt \\hsize=100pt \\lineskip=0pt \\lineskiplimit=0pt \\bo"
        "xmaxdepth=16383.99998pt \\hbadness=10000 \\vbadness=10000 "
        "\\vfuzz=1000pt \\hfuzz=1000pt \\output={\\message{[OUT|\\t"
        "he\\outputpenalty|\\the\\deadcycles|\\the\\ht255|\\the\\dp"
        "255|\\the\\wd255|\\the\\badness|\\the\\pagegoal|\\the\\pag"
        "etotal]}\\shipout\\box255 }\\def\\B{\\hbox{\\vrule width4p"
        "t height20pt depth1pt}}\\def\\P#1{\\message{[#1|\\the\\pag"
        "etotal|\\the\\pagegoal|\\the\\deadcycles]}}\\B\\P{1}\\B\\P"
        "{2}\\B\\P{3}\\B\\P{4}\\penalty-10000 \\P{5}\\B\\P{6}\\mess"
        "age{[end|\\the\\pagegoal|\\the\\pagetotal|\\the\\deadcycle"
        "s]}\\end",
        "[1|20.0pt|50.0pt|0][2|41.0pt|50.0pt|0][3|62.0pt|50.0pt|0]["
        "OUT|10000|1|50.0pt|1.0pt|4.0pt|10000|50.0pt|62.0pt][4|41.0"
        "pt|50.0pt|0][OUT|-10000|1|50.0pt|1.0pt|4.0pt|10000|50.0pt|"
        "41.0pt][5|0.0pt|16383.99998pt|0][6|20.0pt|50.0pt|0][end|50"
        ".0pt|20.0pt|0][OUT|-1073741824|1|50.0pt|0.0pt|100.0pt|0|50"
        ".0pt|21.0pt]");
}

static int test_page_totals(void)
{
    return run_document(
        "\\vsize=200pt \\maxdepth=2pt \\topskip=10pt plus 1pt minus"
        " 3pt \\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\lin"
        "eskip=0pt \\lineskiplimit=0pt \\hbadness=10000 \\vbadness="
        "10000 \\vfuzz=1000pt \\hfuzz=1000pt \\parindent=0pt \\pars"
        "kip=0pt \\parfillskip=0pt plus1fil \\tolerance=10000 \\hsi"
        "ze=100pt \\def\\P#1{\\message{[#1|\\the\\pagegoal|\\the\\p"
        "agetotal|\\the\\pagedepth|\\the\\pagestretch|\\the\\pagesh"
        "rink|\\the\\pagefilstretch|\\the\\pagefillstretch]}}\\def"
        "\\R#1#2#3{\\vrule width#1pt height#2pt depth#3pt}\\P{1}\\v"
        "skip 7pt \\P{2}\\hrule height5pt depth1pt \\P{3}\\vskip3pt"
        " plus 2pt minus 1pt \\P{4}\\hbox{\\R{4}{6}{3}}\\P{5}\\pena"
        "lty 33 \\P{6}\\kern4pt \\P{7}\\vskip0pt plus1fil \\P{8}\\h"
        "box{\\R{4}{3}{0}}\\P{9}\\vfill\\hbox{\\R{4}{3}{0}}\\P{10}"
        "\\vsize=50pt \\hbox{\\R{4}{3}{0}}\\P{11}\\noindent\\R{5}{8"
        "}{1}\\par\\P{12}%",
        "[1|16383.99998pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt][2|16"
        "383.99998pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt][3|16383.9"
        "9998pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt][4|16383.99998p"
        "t|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt|0.0pt][5|200.0pt|21.0pt|2."
        "0pt|3.0pt|4.0pt|0.0pt|0.0pt][6|200.0pt|21.0pt|2.0pt|3.0pt|"
        "4.0pt|0.0pt|0.0pt][7|200.0pt|21.0pt|2.0pt|3.0pt|4.0pt|0.0p"
        "t|0.0pt][8|200.0pt|21.0pt|2.0pt|3.0pt|4.0pt|0.0pt|0.0pt][9"
        "|200.0pt|36.0pt|0.0pt|3.0pt|4.0pt|1.0pt|0.0pt][10|200.0pt|"
        "48.0pt|0.0pt|3.0pt|4.0pt|1.0pt|1.0pt][11|200.0pt|60.0pt|0."
        "0pt|3.0pt|4.0pt|1.0pt|1.0pt][12|200.0pt|72.0pt|1.0pt|3.0pt"
        "|4.0pt|1.0pt|1.0pt]");
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
        status = macro->flags !=
                 ((uint8_t)HSTEX_MACRO_LONG | (uint8_t)HSTEX_MACRO_OUTER);
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
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbreadth=20 "
        "\\count0=7 \\def\\x{\\the\\count0}"
        "\\immediate\\openout1=%s "
        "\\setbox0=\\hbox{\\write1{now \\x}\\special{ps \\x}"
        "\\openout2=zz \\closeout2 \\write-5{}\\write16{}}"
        "\\showbox0 "
        "\\setbox2=\\hbox{\\write1{now \\x}}"
        "\\count0=8 \\shipout\\box2 \\immediate\\closeout1 "
        "\\openin1=\"%s\" \\read1 to \\line \\closein1 "
        "\\message{[\\line]}%%",
        stream_path, stream_path);
    if (length < 0 || (size_t)length >= sizeof(source)) {
        return 1;
    }
    int status = run_document(source,
                              "> \\box0=\n"
                              "\\hbox(0.0+0.0)x0.0\n"
                              ".\\write1{now \\x }\n"
                              ".\\special{ps 7}\n"
                              ".\\openout2=zz\n"
                              ".\\closeout2\n"
                              ".\\write-{}\n"
                              ".\\write*{}\n"
                              "\n"
                              "! OK.\n"
                              "[now 8 ]");
    (void)unlink(stream_path);
    return status;
}

/* \discretionary and \- leave a node that offers the line breaker a third
   choice; see docs/DECISIONS.md, discretionaries. */
static int test_discretionaries(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=30"
        " \\hbadness=10000 \\vbadness=10000 \\font\\f=cmr10 \\f"
        " \\hyphenchar\\f=45 \\hsize=30pt \\parindent=0pt \\lef"
        "tskip=0pt \\rightskip=0pt \\baselineskip=12pt \\linesk"
        "ip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fil "
        "\\tolerance=10000 \\pretolerance=-1 \\boxmaxdepth=1638"
        "3.99998pt \\linepenalty=10 \\adjdemerits=10000 \\doubl"
        "ehyphendemerits=10000 \\finalhyphendemerits=5000 \\clu"
        "bpenalty=0 \\widowpenalty=0 \\interlinepenalty=0 \\bro"
        "kenpenalty=77 \\hyphenpenalty=50 \\exhyphenpenalty=50 "
        "\\hfuzz=1000pt \\vfuzz=1000pt \\setbox0=\\hbox{a\\disc"
        "retionary{b}{c}{d}e}\\showbox0 \\setbox0=\\hbox{a\\-e}"
        "\\showbox0 \\hyphenchar\\f=-1 \\setbox0=\\hbox{a\\-e}\\"
        "showbox0 \\hyphenchar\\f=45 \\setbox0=\\vbox{\\noinden"
        "t aaaa\\discretionary{b}{c}{d}aaaa\\par}\\showbox0 \\s"
        "etbox0=\\vbox{\\noindent aaaa\\discretionary{}{}{}aaaa"
        "\\par}\\showbox0%",
        "> \\box0=\n\\hbox(6.94444+0.0)x15.00003\n.\\f a\n.\\di"
        "scretionary replacing 1\n..\\f b\n.|\\f c\n.\\f d\n.\\"
        "f e\n\n! OK.\n> \\box0=\n\\hbox(4.30554+0.0)x9.44446\n"
        ".\\f a\n.\\discretionary\n..\\f -\n.\\f e\n\n! OK.\n> "
        "\\box0=\n\\hbox(4.30554+0.0)x9.44446\n.\\f a\n.\\discr"
        "etionary\n.\\f e\n\n! OK.\n> \\box0=\n\\vbox(18.94444+"
        "0.0)x30.0\n.\\hbox(6.94444+0.0)x30.0\n..\\f a\n..\\f a"
        "\n..\\f a\n..\\f a\n..\\discretionary\n..\\f b\n..\\gl"
        "ue(\\rightskip) 0.0\n.\\penalty 77\n.\\glue(\\baseline"
        "skip) 7.69446\n.\\hbox(4.30554+0.0)x30.0, glue set 5.5"
        "555fil\n..\\f c\n..\\f a\n..\\f a\n..\\f a\n..\\f a\n."
        ".\\penalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0"
        "fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n> \\box0=\n\\"
        "vbox(16.30554+0.0)x30.0\n.\\hbox(4.30554+0.0)x30.0\n.."
        "\\f a\n..\\f a\n..\\f a\n..\\f a\n..\\discretionary\n."
        ".\\glue(\\rightskip) 0.0\n.\\penalty 77\n.\\glue(\\bas"
        "elineskip) 7.69446\n.\\hbox(4.30554+0.0)x30.0, glue se"
        "t 9.99994fil\n..\\f a\n..\\f a\n..\\f a\n..\\f a\n..\\"
        "penalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil"
        "\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* The outermost vertical list gets \parskip in front of a paragraph however
   little is waiting to be contributed; see docs/DECISIONS.md,
   parskip-in-the-outermost-list. */
static int test_parskip_in_the_outermost_list(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=1 \\showboxbreadth=30 \\"
        "hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1"
        "000pt \\font\\f=cmr10 \\f \\hsize=100pt \\vsize=200pt \\"
        "topskip=0pt \\maxdepth=0pt \\parindent=0pt \\parskip=3pt"
        " plus1pt \\baselineskip=12pt \\lineskip=0pt \\lineskipli"
        "mit=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\righ"
        "tskip=0pt \\tolerance=10000 \\pretolerance=-1 \\clubpena"
        "lty=0 \\widowpenalty=0 \\interlinepenalty=0 \\brokenpena"
        "lty=0 \\output={\\showbox255 \\shipout\\box255 }\\hbox{}"
        "\\hbox{}\\noindent A\\par \\penalty-10000 %",
        "> \\box255=\n"
        "\\vbox(200.0+0.0)x100.0, glue set 173.0\n"
        ".\\glue(\\topskip) 0.0\n"
        ".\\hbox(0.0+0.0)x0.0\n"
        ".\\glue(\\baselineskip) 12.0\n"
        ".\\hbox(0.0+0.0)x0.0\n"
        ".\\glue(\\parskip) 3.0 plus 1.0\n"
        ".\\glue(\\baselineskip) 5.16669\n"
        ".\\hbox(6.83331+0.0)x100.0, glue set 92.49998fil []\n"
        "\n"
        "! OK.\n");
}

/* \lpcode and \rpcode let the character at either end of a line stick out
   past the margin; see docs/DECISIONS.md, character-protrusion. */
static int test_character_protrusion(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=40"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\hsize=40pt \\parinden"
        "t=0pt \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus1fil \\tolerance=10000 \\"
        "pretolerance=-1 \\boxmaxdepth=16383.99998pt \\spaceski"
        "p=4pt \\lpcode\\f`\\A=100 \\rpcode\\f`\\B=200 \\pdfpro"
        "trudechars=1 \\leftskip=0pt \\rightskip=0pt plus1fil \\"
        "message{[ragged]}\\setbox1=\\vbox{\\noindent AB\\par}\\"
        "showbox1 \\rightskip=0pt \\message{[emptybox]}\\setbox"
        "0=\\hbox{}\\setbox1=\\vbox{\\noindent \\copy0 AB\\par}"
        "\\showbox1 \\message{[fullbox]}\\setbox0=\\hbox to0pt{"
        "\\hss}\\setbox1=\\vbox{\\noindent \\copy0 AB\\par}\\sh"
        "owbox1 \\message{[zerokern]}\\setbox1=\\vbox{\\noinden"
        "t \\kern0pt AB\\par}\\showbox1 \\message{[zeroglue]}\\"
        "setbox1=\\vbox{\\noindent \\hskip0pt AB\\par}\\showbox"
        "1%",
        "[ragged]> \\box1=\n\\vbox(6.83331+0.0)x40.0\n.\\hbox(6"
        ".83331+0.0)x40.0, glue set 14.20831fil\n..\\kern-1.0 ("
        "left margin)\n..\\f A\n..\\f B\n..\\penalty 10000\n..\\"
        "kern-2.0 (right margin)\n..\\glue(\\parfillskip) 0.0 p"
        "lus 1.0fil\n..\\glue(\\rightskip) 0.0 plus 1.0fil\n\n!"
        " OK.\n[emptybox]> \\box1=\n\\vbox(6.83331+0.0)x40.0\n."
        "\\hbox(6.83331+0.0)x40.0, glue set 28.41663fil\n..\\ke"
        "rn-1.0 (left margin)\n..\\hbox(0.0+0.0)x0.0\n..\\f A\n"
        "..\\f B\n..\\penalty 10000\n..\\kern-2.0 (right margin"
        ")\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\"
        "rightskip) 0.0\n\n! OK.\n[fullbox]> \\box1=\n\\vbox(6."
        "83331+0.0)x40.0\n.\\hbox(6.83331+0.0)x40.0, glue set 2"
        "7.41663fil\n..\\hbox(0.0+0.0)x0.0\n...\\glue 0.0 plus "
        "1.0fil minus 1.0fil\n..\\f A\n..\\f B\n..\\penalty 100"
        "00\n..\\kern-2.0 (right margin)\n..\\glue(\\parfillski"
        "p) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\n[zerokern]> \\box1=\n\\vbox(6.83331+0.0)x40.0\n.\\hb"
        "ox(6.83331+0.0)x40.0, glue set 28.41663fil\n..\\kern-1"
        ".0 (left margin)\n..\\kern 0.0\n..\\f A\n..\\f B\n..\\"
        "penalty 10000\n..\\kern-2.0 (right margin)\n..\\glue(\\"
        "parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0."
        "0\n\n! OK.\n[zeroglue]> \\box1=\n\\vbox(6.83331+0.0)x4"
        "0.0\n.\\hbox(6.83331+0.0)x40.0, glue set 27.41663fil\n"
        "..\\glue 0.0\n..\\f A\n..\\f B\n..\\penalty 10000\n..\\"
        "kern-2.0 (right margin)\n..\\glue(\\parfillskip) 0.0 p"
        "lus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* An explicit hyphen is followed into a paragraph by an empty
   discretionary; see docs/DECISIONS.md,
   the-discretionary-after-an-explicit-hyphen. */
static int test_the_discretionary_after_an_explicit_hyphen(void)
{
    if (run_document(
            "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=60"
            " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
            "uzz=1000pt \\font\\f=cmr10 \\f \\hsize=200pt \\parinde"
            "nt=0pt \\baselineskip=12pt \\lineskip=0pt \\lineskipli"
            "mit=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\ri"
            "ghtskip=0pt \\spaceskip=4pt \\tolerance=10000 \\pretol"
            "erance=10000 \\boxmaxdepth=16383.99998pt \\hyphenchar\\"
            "f=`\\x \\message{[A]}\\setbox0=\\vbox{\\noindent axb\\"
            "par}\\showbox0 \\message{[B]}\\setbox0=\\vbox{\\noinde"
            "nt a-b\\par}\\showbox0 \\hyphenchar\\f=45 \\message{[C"
            "]}\\setbox0=\\vbox{\\noindent \\hbox{a-b}\\par}\\showb"
            "ox0 \\message{[D]}\\setbox0=\\vbox{\\noindent a-b\\par"
            "}\\showbox0 \\message{[E]}\\setbox0=\\vbox{\\noindent "
            "a\\char45 b\\par}\\showbox0 \\message{[F]}\\setbox0=\\"
            "vbox{\\noindent a--b\\par}\\showbox0%",
            "[A]> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.944"
            "44+0.0)x200.0, glue set 184.16661fil\n..\\f a\n..\\f x"
            "\n..\\discretionary\n..\\f b\n..\\penalty 10000\n..\\g"
            "lue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightsk"
            "ip) 0.0\n\n! OK.\n[B]> \\box0=\n\\vbox(6.94444+0.0)x20"
            "0.0\n.\\hbox(6.94444+0.0)x200.0, glue set 186.11108fil"
            "\n..\\f a\n..\\f -\n..\\f b\n..\\penalty 10000\n..\\gl"
            "ue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightski"
            "p) 0.0\n\n! OK.\n[C]> \\box0=\n\\vbox(6.94444+0.0)x200"
            ".0\n.\\hbox(6.94444+0.0)x200.0, glue set 186.11108fil\n"
            "..\\hbox(6.94444+0.0)x13.88892\n...\\f a\n...\\f -\n.."
            ".\\f b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0"
            " plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n[D]>"
            " \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.94444+0"
            ".0)x200.0, glue set 186.11108fil\n..\\f a\n..\\f -\n.."
            "\\discretionary\n..\\f b\n..\\penalty 10000\n..\\glue("
            "\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) "
            "0.0\n\n! OK.\n[E]> \\box0=\n\\vbox(6.94444+0.0)x200.0\n"
            ".\\hbox(6.94444+0.0)x200.0, glue set 186.11108fil\n..\\"
            "f a\n..\\f -\n..\\discretionary\n..\\f b\n..\\penalty "
            "10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\gl"
            "ue(\\rightskip) 0.0\n\n! OK.\n[F]> \\box0=\n\\vbox(6.9"
            "4444+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0, glue set "
            "184.4444fil\n..\\f a\n..\\f { (ligature --)\n..\\discr"
            "etionary\n..\\f b\n..\\penalty 10000\n..\\glue(\\parfi"
            "llskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n"
            "! OK.\n") != 0) {
        return 1;
    }
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=60"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\hsize=200pt \\hyphenc"
        "har\\f=45 \\parindent=0pt \\baselineskip=12pt \\linesk"
        "ip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fil "
        "\\leftskip=0pt \\rightskip=0pt \\spaceskip=4pt \\toler"
        "ance=10000 \\pretolerance=10000 \\boxmaxdepth=16383.99"
        "998pt \\message{[end]}\\setbox0=\\vbox{\\noindent ab-\\"
        "par}\\showbox0 \\message{[space]}\\setbox0=\\vbox{\\no"
        "indent ab- cd\\par}\\showbox0 \\message{[start]}\\setb"
        "ox0=\\vbox{\\noindent -ab\\par}\\showbox0 \\message{[k"
        "ern]}\\setbox0=\\vbox{\\noindent a-\\kern2pt b\\par}\\"
        "showbox0%",
        "[end]> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.9"
        "4444+0.0)x200.0, glue set 186.11108fil\n..\\f a\n..\\f"
        " b\n..\\f -\n..\\discretionary\n..\\penalty 10000\n..\\"
        "glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rights"
        "kip) 0.0\n\n! OK.\n[space]> \\box0=\n\\vbox(6.94444+0."
        "0)x200.0\n.\\hbox(6.94444+0.0)x200.0, glue set 172.111"
        "07fil\n..\\f a\n..\\f b\n..\\f -\n..\\discretionary\n."
        ".\\glue(\\spaceskip) 4.0\n..\\f c\n..\\f d\n..\\penalt"
        "y 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\"
        "glue(\\rightskip) 0.0\n\n! OK.\n[start]> \\box0=\n\\vb"
        "ox(6.94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0, glu"
        "e set 186.11108fil\n..\\f -\n..\\discretionary\n..\\f "
        "a\n..\\f b\n..\\penalty 10000\n..\\glue(\\parfillskip)"
        " 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n"
        "[kern]> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6."
        "94444+0.0)x200.0, glue set 184.11108fil\n..\\f a\n..\\"
        "f -\n..\\discretionary\n..\\kern 2.0\n..\\f b\n..\\pen"
        "alty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n."
        ".\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* A formula in a paragraph is fenced with math nodes, its spacing keeps the
   name of the muskip it came from, and a binary operator or a relation
   leaves a penalty behind it; see docs/DECISIONS.md, math-nodes and
   math-penalties. */
static int test_math_nodes(void)
{
    return run_document(
        "\\catcode`\\$=3 \\tracingonline=1 \\showboxdepth=3 \\s"
        "howboxbreadth=60 \\hbadness=10000 \\vbadness=10000 \\h"
        "fuzz=1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0"
        "pt \\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\l"
        "ineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus"
        "1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=10000 "
        "\\pretolerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 "
        "\\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni="
        "cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 \\font"
        "\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font\\fivesy=cm"
        "sy5 \\font\\tenex=cmex10 \\textfont0=\\tenrm \\scriptf"
        "ont0=\\sevenrm \\scriptscriptfont0=\\fiverm \\textfont"
        "1=\\teni \\scriptfont1=\\seveni \\scriptscriptfont1=\\"
        "fivei \\textfont2=\\tensy \\scriptfont2=\\sevensy \\sc"
        "riptscriptfont2=\\fivesy \\textfont3=\\tenex \\scriptf"
        "ont3=\\tenex \\scriptscriptfont3=\\tenex \\tenrm \\mat"
        "hcode`\\+=\"202B \\mathcode`\\==\"303D \\thinmuskip=3m"
        "u \\medmuskip=4mu plus 2mu minus 4mu \\thickmuskip=5mu"
        " plus 5mu \\relpenalty=500 \\binoppenalty=700 \\mathsu"
        "rround=3pt \\message{[A]}\\setbox0=\\vbox{\\noindent a"
        " $x+y=z$ b\\par}\\showbox0 \\mathsurround=0pt \\messag"
        "e{[B]}\\setbox0=\\vbox{\\noindent a $x+y=z$ b\\par}\\s"
        "howbox0 \\message{[C]}\\setbox0=\\hbox{$x+y=z$}\\showb"
        "ox0 \\message{[D]}\\setbox0=\\vbox{\\noindent a $x=+y$"
        " b\\par}\\showbox0%",
        "[A]> \\box0=\n\\vbox(6.94444+1.94444)x200.0\n.\\hbox(6"
        ".94444+1.94444)x200.0, glue set 133.82188fil\n..\\tenr"
        "m a\n..\\glue(\\spaceskip) 4.0\n..\\mathon, surrounded"
        " 3.0\n..\\teni x\n..\\glue(\\medmuskip) 2.22217 plus 1"
        ".11108 minus 2.22217\n..\\tenrm +\n..\\penalty 700\n.."
        "\\glue(\\medmuskip) 2.22217 plus 1.11108 minus 2.22217"
        "\n..\\teni y\n..\\kern0.35878\n..\\glue(\\thickmuskip)"
        " 2.77771 plus 2.77771\n..\\tenrm =\n..\\penalty 500\n."
        ".\\glue(\\thickmuskip) 2.77771 plus 2.77771\n..\\teni "
        "z\n..\\kern0.4398\n..\\mathoff, surrounded 3.0\n..\\gl"
        "ue(\\spaceskip) 4.0\n..\\tenrm b\n..\\penalty 10000\n."
        ".\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rig"
        "htskip) 0.0\n\n! OK.\n[B]> \\box0=\n\\vbox(6.94444+1.9"
        "4444)x200.0\n.\\hbox(6.94444+1.94444)x200.0, glue set "
        "139.82188fil\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n"
        "..\\mathon\n..\\teni x\n..\\glue(\\medmuskip) 2.22217 "
        "plus 1.11108 minus 2.22217\n..\\tenrm +\n..\\penalty 7"
        "00\n..\\glue(\\medmuskip) 2.22217 plus 1.11108 minus 2"
        ".22217\n..\\teni y\n..\\kern0.35878\n..\\glue(\\thickm"
        "uskip) 2.77771 plus 2.77771\n..\\tenrm =\n..\\penalty "
        "500\n..\\glue(\\thickmuskip) 2.77771 plus 2.77771\n..\\"
        "teni z\n..\\kern0.4398\n..\\mathoff\n..\\glue(\\spaces"
        "kip) 4.0\n..\\tenrm b\n..\\penalty 10000\n..\\glue(\\p"
        "arfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0"
        "\n\n! OK.\n[C]> \\box0=\n\\hbox(5.83333+1.94444)x41.62"
        "253\n.\\mathon\n.\\teni x\n.\\glue(\\medmuskip) 2.2221"
        "7 plus 1.11108 minus 2.22217\n.\\tenrm +\n.\\glue(\\me"
        "dmuskip) 2.22217 plus 1.11108 minus 2.22217\n.\\teni y"
        "\n.\\kern0.35878\n.\\glue(\\thickmuskip) 2.77771 plus "
        "2.77771\n.\\tenrm =\n.\\glue(\\thickmuskip) 2.77771 pl"
        "us 2.77771\n.\\teni z\n.\\kern0.4398\n.\\mathoff\n\n! "
        "OK.\n[D]> \\box0=\n\\vbox(6.94444+1.94444)x200.0\n.\\h"
        "box(6.94444+1.94444)x200.0, glue set 149.35652fil\n..\\"
        "tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\mathon\n..\\te"
        "ni x\n..\\glue(\\thickmuskip) 2.77771 plus 2.77771\n.."
        "\\tenrm =\n..\\penalty 500\n..\\glue(\\thickmuskip) 2."
        "77771 plus 2.77771\n..\\tenrm +\n..\\teni y\n..\\kern0"
        ".35878\n..\\mathoff\n..\\glue(\\spaceskip) 4.0\n..\\te"
        "nrm b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 "
        "plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* A word set with a ligature in it is still hyphenated, and a word followed
   by a discretionary is not hyphenated at all; see docs/DECISIONS.md,
   hyphenation. */
static int test_hyphenating_ligatures(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=20"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\v"
        "fuzz=1000pt \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\h"
        "size=200pt \\parindent=0pt \\leftskip=0pt \\rightskip="
        "0pt \\baselineskip=12pt \\lineskip=0pt \\lineskiplimit"
        "=0pt \\parfillskip=0pt plus1fil \\tolerance=10000 \\pr"
        "etolerance=-1 \\boxmaxdepth=16383.99998pt \\linepenalt"
        "y=10 \\adjdemerits=10000 \\doublehyphendemerits=10000 "
        "\\finalhyphendemerits=5000 \\clubpenalty=0 \\widowpena"
        "lty=0 \\interlinepenalty=0 \\brokenpenalty=0 \\hyphenp"
        "enalty=50 \\exhyphenpenalty=50 \\uchyph=0 \\lefthyphen"
        "min=1 \\righthyphenmin=1 \\spaceskip=4pt \\sfcode`\\.="
        "1000 \\lccode`\\a=`\\a \\lccode`\\c=`\\c \\lccode`\\e="
        "`\\e \\lccode`\\f=`\\f \\lccode`\\i=`\\i \\lccode`\\l="
        "`\\l \\lccode`\\n=`\\n \\lccode`\\o=`\\o \\lccode`\\x="
        "`\\x \\patterns{co1fi1nal 1x1} \\message{[lig]}\\setbo"
        "x0=\\vbox{\\noindent xx cofinal\\par}\\showbox0 \\mess"
        "age{[hyphen]}\\setbox0=\\vbox{\\noindent xx cofinal-x\\"
        "par}\\showbox0%",
        "[lig]> \\box0=\n\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.9"
        "4444+0.0)x200.0, glue set 157.111fil\n..\\f x\n..\\f x"
        "\n..\\glue(\\spaceskip) 4.0\n..\\f c\n..\\f o\n..\\dis"
        "cretionary\n...\\f -\n..\\f ^^L (ligature fi)\n..\\dis"
        "cretionary\n...\\f -\n..\\f n\n..\\f a\n..\\f l\n..\\p"
        "enalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n\n! OK.\n[hyphen]> \\box0=\n"
        "\\vbox(6.94444+0.0)x200.0\n.\\hbox(6.94444+0.0)x200.0,"
        " glue set 148.49986fil\n..\\f x\n..\\f x\n..\\glue(\\s"
        "paceskip) 4.0\n..\\f c\n..\\f o\n..\\f ^^L (ligature f"
        "i)\n..\\f n\n..\\f a\n..\\f l\n..\\f -\n..\\discretion"
        "ary\n..\\f x\n..\\penalty 10000\n..\\glue(\\parfillski"
        "p) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\n");
}

/* A line that breaks at a kern keeps it with no width left, and glue after
   an explicit kern is not a place to break at all; see docs/DECISIONS.md,
   what-a-line-keeps-of-its-break. */
static int test_what_a_line_keeps_of_its_break(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=60"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\hsize=40pt \\parinden"
        "t=0pt \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\rig"
        "htskip=0pt \\tolerance=10000 \\pretolerance=-1 \\boxma"
        "xdepth=16383.99998pt \\spaceskip=4pt \\pdfprotrudechar"
        "s=0 \\message{[kern]}\\setbox0=\\vbox{\\noindent aaaa\\"
        "kern20pt\\hskip0pt aaaa\\par}\\showbox0 \\message{[glu"
        "e]}\\setbox0=\\vbox{\\noindent aaaa\\hskip20pt aaaa\\p"
        "ar}\\showbox0%",
        "[kern]> \\box0=\n\\vbox(16.30554+0.0)x40.0\n.\\hbox(4."
        "30554+0.0)x40.0\n..\\f a\n..\\f a\n..\\f a\n..\\f a\n."
        ".\\kern 0.0\n..\\glue(\\rightskip) 0.0\n.\\glue(\\base"
        "lineskip) 7.69446\n.\\hbox(4.30554+0.0)x40.0, glue set"
        " 19.99994fil\n..\\f a\n..\\f a\n..\\f a\n..\\f a\n..\\"
        "penalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil"
        "\n..\\glue(\\rightskip) 0.0\n\n! OK.\n[glue]> \\box0=\n"
        "\\vbox(16.30554+0.0)x40.0\n.\\hbox(4.30554+0.0)x40.0\n"
        "..\\f a\n..\\f a\n..\\f a\n..\\f a\n..\\glue(\\rightsk"
        "ip) 0.0\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4.30"
        "554+0.0)x40.0, glue set 19.99994fil\n..\\f a\n..\\f a\n"
        "..\\f a\n..\\f a\n..\\penalty 10000\n..\\glue(\\parfil"
        "lskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n!"
        " OK.\n");
}

/* A character in a math list that another character of the same family
   follows is read as part of a word in a text font, and its italic
   correction goes away; see docs/DECISIONS.md, math-text-characters. */
static int test_math_text_characters(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\trac"
        "ingonline=1 \\showboxdepth=6 \\showboxbreadth=60 \\hba"
        "dness=10000 \\hfuzz=1000pt \\font\\tenrm=cmr10 \\font\\"
        "sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni=cmmi10 \\"
        "font\\seveni=cmmi7 \\font\\fivei=cmmi5 \\font\\tensy=c"
        "msy10 \\font\\sevensy=cmsy7 \\font\\fivesy=cmsy5 \\fon"
        "t\\tenex=cmex10 \\textfont0=\\tenrm \\scriptfont0=\\se"
        "venrm \\scriptscriptfont0=\\fiverm \\textfont1=\\teni "
        "\\scriptfont1=\\seveni \\scriptscriptfont1=\\fivei \\t"
        "extfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscrip"
        "tfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\te"
        "nex \\scriptscriptfont3=\\tenex \\tenrm \\mathcode`\\Y"
        "=\"7059 \\mathcode`\\M=\"704D \\mathcode`\\A=\"7041 \\"
        "message{[m]}\\setbox0=\\hbox{$YM$}\\showbox0 \\mathcod"
        "e`\\Y=\"059 \\mathcode`\\M=\"04D \\mathcode`\\A=\"041 "
        "\\message{[r]}\\setbox0=\\hbox{$YM$}\\showbox0 \\messa"
        "ge{[ra]}\\setbox0=\\hbox{$YA$}\\showbox0 \\message{[rs"
        "]}\\setbox0=\\hbox{$Y_1M$}\\showbox0 \\message{[t]}\\s"
        "etbox0=\\hbox{YM}\\showbox0%",
        "[m]> \\box0=\n\\hbox(6.83331+0.0)x16.6667\n.\\mathon\n"
        ".\\tenrm Y\n.\\tenrm M\n.\\mathoff\n\n! OK.\n[r]> \\bo"
        "x0=\n\\hbox(6.83331+0.0)x16.6667\n.\\mathon\n.\\tenrm "
        "Y\n.\\tenrm M\n.\\mathoff\n\n! OK.\n[ra]> \\box0=\n\\h"
        "box(6.83331+0.0)x14.16669\n.\\mathon\n.\\tenrm Y\n.\\k"
        "ern-0.83334\n.\\tenrm A\n.\\mathoff\n\n! OK.\n[rs]> \\"
        "box0=\n\\hbox(6.83331+1.49998)x20.65283\n.\\mathon\n.\\"
        "tenrm Y\n.\\hbox(4.51111+0.0)x3.98613, shifted 1.49998"
        "\n..\\sevenrm 1\n.\\tenrm M\n.\\mathoff\n\n! OK.\n[t]>"
        " \\box0=\n\\hbox(6.83331+0.0)x16.6667\n.\\tenrm Y\n.\\"
        "tenrm M\n\n! OK.\n");
}

/* A penalty of ten thousand forbids a page break rather than costing that
   much; see docs/DECISIONS.md, the-page-builder. */
static int test_infinite_page_penalty(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=2 \\showboxbreadth=30"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\hsize=100pt \\vsize=50pt \\topskip=0pt \\"
        "maxdepth=0pt \\output={\\message{[F \\the\\outputpenal"
        "ty]}\\showbox255 \\shipout\\box255 } \\hbox{}\\penalty"
        "-100 \\vskip10pt \\penalty10000 \\vskip100pt \\hbox{} "
        "\\penalty-10000 \\message{[done]}%",
        "[F -100]> \\box255=\n\\vbox(50.0+0.0)x0.0\n.\\glue(\\t"
        "opskip) 0.0\n.\\hbox(0.0+0.0)x0.0\n\n! OK.\n[F -10000]"
        "> \\box255=\n\\vbox(50.0+0.0)x0.0\n.\\glue(\\topskip) "
        "0.0\n.\\hbox(0.0+0.0)x0.0\n\n! OK.\n[done]");
}

/* \emergencystretch buys the paragraph one more pass with that much stretch
   behind every line; see docs/DECISIONS.md, emergency-stretch. */
static int test_emergency_stretch(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=1 \\showboxbreadth=30"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\hsize=60pt \\parinden"
        "t=0pt \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\rig"
        "htskip=0pt \\tolerance=200 \\pretolerance=-1 \\boxmaxd"
        "epth=16383.99998pt \\spaceskip=4pt plus2pt minus1pt \\"
        "linepenalty=10 \\adjdemerits=10000 \\clubpenalty=0 \\w"
        "idowpenalty=0 \\interlinepenalty=0 \\brokenpenalty=0 \\"
        "pdfprotrudechars=0 \\emergencystretch=0pt \\message{[n"
        "one]}\\setbox0=\\vbox{\\noindent aaaa aaaa aaaa aaaa a"
        "aaa aaaa\\par}\\showbox0 \\emergencystretch=20pt \\mes"
        "sage{[some]}\\setbox0=\\vbox{\\noindent aaaa aaaa aaaa"
        " aaaa aaaa aaaa\\par}\\showbox0%",
        "[none]> \\box0=\n\\vbox(16.30554+0.0)x60.0\n.\\hbox(4."
        "30554+0.0)x60.0, glue set - 1.0 []\n.\\glue(\\baseline"
        "skip) 7.69446\n.\\hbox(4.30554+0.0)x60.0, glue set - 1"
        ".0 []\n\n! OK.\n[some]> \\box0=\n\\vbox(28.30554+0.0)x"
        "60.0\n.\\hbox(4.30554+0.0)x60.0, glue set 7.99994 []\n"
        ".\\glue(\\baselineskip) 7.69446\n.\\hbox(4.30554+0.0)x"
        "60.0, glue set 7.99994 []\n.\\glue(\\baselineskip) 7.6"
        "9446\n.\\hbox(4.30554+0.0)x60.0, glue set 15.99988fil "
        "[]\n\n! OK.\n");
}

/* A display formula's line is marked, its skips keep the names of the
   parameters they came from, and the line before it is a widow of a
   different kind; see docs/DECISIONS.md, display-math. */
static int test_display_skips_and_widows(void)
{
    return run_document(
        "\\catcode`\\$=3 \\tracingonline=1 \\showboxdepth=3 \\s"
        "howboxbreadth=60 \\hbadness=10000 \\vbadness=10000 \\h"
        "fuzz=1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0"
        "pt \\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\l"
        "ineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus"
        "1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=10000 "
        "\\pretolerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 "
        "\\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni="
        "cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 \\font"
        "\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font\\fivesy=cm"
        "sy5 \\font\\tenex=cmex10 \\textfont0=\\tenrm \\scriptf"
        "ont0=\\sevenrm \\scriptscriptfont0=\\fiverm \\textfont"
        "1=\\teni \\scriptfont1=\\seveni \\scriptscriptfont1=\\"
        "fivei \\textfont2=\\tensy \\scriptfont2=\\sevensy \\sc"
        "riptscriptfont2=\\fivesy \\textfont3=\\tenex \\scriptf"
        "ont3=\\tenex \\scriptscriptfont3=\\tenex \\tenrm \\mat"
        "hcode`\\+=\"202B \\thinmuskip=3mu \\medmuskip=4mu plus"
        " 2mu minus 4mu \\thickmuskip=5mu plus 5mu \\abovedispl"
        "ayskip=10pt plus2pt \\belowdisplayskip=11pt plus2pt \\"
        "abovedisplayshortskip=1pt plus3pt \\belowdisplayshorts"
        "kip=2pt plus3pt \\predisplaypenalty=10000 \\postdispla"
        "ypenalty=0 \\widowpenalty=150 \\displaywidowpenalty=50"
        " \\clubpenalty=0 \\interlinepenalty=0 \\message{[short"
        "]}\\setbox0=\\vbox{\\noindent aa $$x+y$$ bb\\par}\\sho"
        "wbox0 \\message{[long]}\\setbox0=\\vbox{\\noindent aa "
        "aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa $$x+y$$ b"
        "b\\par}\\showbox0%",
        "[short]> \\box0=\n\\vbox(31.30554+0.0)x200.0\n.\\hbox("
        "4.30554+0.0)x200.0, glue set 189.99997fil\n..\\tenrm a"
        "\n..\\tenrm a\n..\\penalty 10000\n..\\glue(\\parfillsk"
        "ip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n.\\pen"
        "alty 10000\n.\\glue(\\abovedisplayshortskip) 1.0 plus "
        "3.0\n.\\glue(\\baselineskip) 6.16667\n.\\hbox(5.83333+"
        "1.94444)x23.199, shifted 88.4005, display\n..\\teni x\n"
        "..\\glue(\\medmuskip) 2.22217 plus 1.11108 minus 2.222"
        "17\n..\\tenrm +\n..\\glue(\\medmuskip) 2.22217 plus 1."
        "11108 minus 2.22217\n..\\teni y\n..\\kern0.35878\n.\\p"
        "enalty 0\n.\\glue(\\belowdisplayshortskip) 2.0 plus 3."
        "0\n.\\glue(\\baselineskip) 3.11111\n.\\hbox(6.94444+0."
        "0)x200.0, glue set 188.88885fil\n..\\tenrm b\n..\\tenr"
        "m b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 pl"
        "us 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n[long]>"
        " \\box0=\n\\vbox(43.30554+0.0)x200.0\n.\\hbox(4.30554+"
        "0.0)x200.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\space"
        "skip) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaces"
        "kip) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spacesk"
        "ip) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceski"
        "p) 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip"
        ") 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip)"
        " 4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) "
        "4.0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4"
        ".0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4."
        "0\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0"
        "\n..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n"
        "..\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n."
        ".\\tenrm a\n..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n.."
        "\\tenrm a\n..\\tenrm a\n..\\glue(\\rightskip) 0.0\n.\\"
        "penalty 50\n.\\glue(\\baselineskip) 7.69446\n.\\hbox(4"
        ".30554+0.0)x200.0, glue set 175.99994fil\n..\\tenrm a\n"
        "..\\tenrm a\n..\\glue(\\spaceskip) 4.0\n..\\tenrm a\n."
        ".\\tenrm a\n..\\penalty 10000\n..\\glue(\\parfillskip)"
        " 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n.\\penalt"
        "y 10000\n.\\glue(\\abovedisplayshortskip) 1.0 plus 3.0"
        "\n.\\glue(\\baselineskip) 6.16667\n.\\hbox(5.83333+1.9"
        "4444)x23.199, shifted 88.4005, display\n..\\teni x\n.."
        "\\glue(\\medmuskip) 2.22217 plus 1.11108 minus 2.22217"
        "\n..\\tenrm +\n..\\glue(\\medmuskip) 2.22217 plus 1.11"
        "108 minus 2.22217\n..\\teni y\n..\\kern0.35878\n.\\pen"
        "alty 0\n.\\glue(\\belowdisplayshortskip) 2.0 plus 3.0\n"
        ".\\glue(\\baselineskip) 3.11111\n.\\hbox(6.94444+0.0)x"
        "200.0, glue set 188.88885fil\n..\\tenrm b\n..\\tenrm b"
        "\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 plus "
        "1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* A box built inside a formula is not the list \prevdepth belongs to; see
   docs/DECISIONS.md, prevdepth-belongs-to-one-list. */
static int test_prevdepth_belongs_to_one_list(void)
{
    return run_document(
        "\\catcode`\\$=3 \\tracingonline=1 \\showboxdepth=3 \\s"
        "howboxbreadth=60 \\hbadness=10000 \\vbadness=10000 \\h"
        "fuzz=1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0"
        "pt \\boxmaxdepth=16383.99998pt \\baselineskip=12pt \\l"
        "ineskip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus"
        "1fil \\leftskip=0pt \\rightskip=0pt \\tolerance=10000 "
        "\\pretolerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 "
        "\\font\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni="
        "cmmi10 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 \\font"
        "\\tensy=cmsy10 \\font\\sevensy=cmsy7 \\font\\fivesy=cm"
        "sy5 \\font\\tenex=cmex10 \\textfont0=\\tenrm \\scriptf"
        "ont0=\\sevenrm \\scriptscriptfont0=\\fiverm \\textfont"
        "1=\\teni \\scriptfont1=\\seveni \\scriptscriptfont1=\\"
        "fivei \\textfont2=\\tensy \\scriptfont2=\\sevensy \\sc"
        "riptscriptfont2=\\fivesy \\textfont3=\\tenex \\scriptf"
        "ont3=\\tenex \\scriptscriptfont3=\\tenex \\tenrm \\mat"
        "hcode`\\+=\"202B \\thinmuskip=3mu \\medmuskip=4mu plus"
        " 2mu minus 4mu \\thickmuskip=5mu plus 5mu \\abovedispl"
        "ayskip=10pt plus2pt \\belowdisplayskip=11pt plus2pt \\"
        "abovedisplayshortskip=1pt plus3pt \\belowdisplayshorts"
        "kip=2pt plus3pt \\predisplaypenalty=10000 \\postdispla"
        "ypenalty=0 \\widowpenalty=150 \\displaywidowpenalty=50"
        " \\clubpenalty=0 \\interlinepenalty=0 \\message{[f1]}\\"
        "setbox0=\\hbox{$x\\over y$}\\showbox0 \\message{[f2]}\\"
        "setbox0=\\vbox{\\noindent aa $$x\\over y$$ bb\\par}\\s"
        "howbox0%",
        "[f1]> \\box0=\n\\hbox(6.9512+4.80951)x4.53473\n.\\math"
        "on\n.\\hbox(6.9512+4.80951)x4.53473\n..\\hbox(0.0+0.0)"
        "x0.0, shifted -2.5\n..\\vbox(6.9512+4.80951)x4.53473\n"
        "...\\hbox(3.01389+0.0)x4.53473 []\n...\\kern1.23732\n."
        "..\\rule(0.39998+0.0)x*\n...\\kern2.73453\n...\\hbox(3"
        ".01389+1.3611)x4.53473, glue set 0.114fil []\n..\\hbox"
        "(0.0+0.0)x0.0, shifted -2.5\n.\\mathoff\n\n! OK.\n[f2]"
        "> \\box0=\n\\vbox(35.05394+0.0)x200.0\n.\\hbox(4.30554"
        "+0.0)x200.0, glue set 189.99997fil\n..\\tenrm a\n..\\t"
        "enrm a\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0"
        " plus 1.0fil\n..\\glue(\\rightskip) 0.0\n.\\penalty 10"
        "000\n.\\glue(\\abovedisplayshortskip) 1.0 plus 3.0\n.\\"
        "glue(\\baselineskip) 0.92938\n.\\hbox(11.07062+8.80396"
        ")x5.71527, shifted 97.14236, display\n..\\hbox(11.0706"
        "2+8.80396)x5.71527\n...\\hbox(0.0+0.0)x0.0, shifted -2"
        ".5\n...\\vbox(11.07062+8.80396)x5.71527 []\n...\\hbox("
        "0.0+0.0)x0.0, shifted -2.5\n.\\penalty 0\n.\\glue(\\be"
        "lowdisplayshortskip) 2.0 plus 3.0\n.\\glue(\\lineskip)"
        " 0.0\n.\\hbox(6.94444+0.0)x200.0, glue set 188.88885fi"
        "l\n..\\tenrm b\n..\\tenrm b\n..\\penalty 10000\n..\\gl"
        "ue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightski"
        "p) 0.0\n\n! OK.\n");
}

/* \\leftmarginkern and \\rightmarginkern report the kern the breaker put at
   either end of a line, which is how microtype protrudes an equation number;
   see docs/DECISIONS.md, the-margin-kerns-of-a-line. */
static int test_margin_kerns_of_a_line(void)
{
    return run_document(
        "\\catcode`\\{=1 \\catcode`\\}=2 \\tracingonline=1 \\sh"
        "owboxdepth=3 \\showboxbreadth=40 \\hbadness=10000 \\vb"
        "adness=10000 \\hfuzz=1000pt \\vfuzz=1000pt \\font\\f=c"
        "mr10 \\f \\hsize=100pt \\parindent=0pt \\baselineskip="
        "12pt \\lineskip=0pt \\lineskiplimit=0pt \\parfillskip="
        "0pt \\leftskip=0pt \\rightskip=0pt \\tolerance=10000 \\"
        "pretolerance=-1 \\boxmaxdepth=16383.99998pt \\splittop"
        "skip=0pt \\splitmaxdepth=16383.99998pt \\lpcode\\f`\\("
        "=117 \\rpcode\\f`\\)=117 \\pdfprotrudechars=2 \\setbox"
        "1=\\vbox{\\noindent(1)\\par} \\setbox2=\\vbox{\\unvbox"
        "1 \\global\\setbox1=\\lastbox} \\dimen5=\\leftmarginke"
        "rn1 \\dimen6=\\rightmarginkern1 \\message{<L=\\the\\di"
        "men5><R=\\the\\dimen6>} \\showbox1%",
        "<L=-1.17pt><R=-1.17pt>> \\box1=\n\\hbox(7.5+2.5)x100.0"
        "\n.\\kern-1.17 (left margin)\n.\\f (\n.\\f 1\n.\\f )\n"
        ".\\penalty 10000\n.\\kern-1.17 (right margin)\n.\\glue"
        "(\\parfillskip) 0.0\n.\\glue(\\rightskip) 0.0\n\n! OK."
        "\n");
}

/* A whatsit's text is shown to within ten characters of the width the
   reference prints to, and the token that reaches that mark is shown whole;
   see docs/DECISIONS.md, whatsits. */
static int test_whatsit_text_is_cut(void)
{
    return run_document(
        "\\catcode`\\{=1 \\catcode`\\}=2 \\tracingonline=1 \\sh"
        "owboxdepth=3 \\showboxbreadth=30 \\message{[69]}\\setb"
        "ox0=\\hbox{\\write1{abcdefghijklmnopqrstuvwxyzabcdefgh"
        "ijklmnopqrstuvwxyzabcdefghijklmnopq}}\\showbox0 \\mess"
        "age{[70]}\\setbox0=\\hbox{\\write1{abcdefghijklmnopqrs"
        "tuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqr}}\\"
        "showbox0 \\message{[cw]}\\setbox0=\\hbox{\\write1{abcd"
        "efghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdef"
        "ghijklm\\thepage}}\\showbox0 \\message{[cw2]}\\setbox0"
        "=\\hbox{\\write1{abcdefghijklmnopqrstuvwxyzabcdefghijk"
        "lmnopqrstuvwxyzabcdefghijklmnop\\thepage}}\\showbox0%",
        "[69]> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\write1{abcdefgh"
        "ijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghij"
        "klmnopq}\n\n! OK.\n[70]> \\box0=\n\\hbox(0.0+0.0)x0.0\n"
        ".\\write1{abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqr"
        "stuvwxyzabcdefghijklmnopq\\ETC.}\n\n! OK.\n[cw]> \\box"
        "0=\n\\hbox(0.0+0.0)x0.0\n.\\write1{abcdefghijklmnopqrs"
        "tuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklm\\thepag"
        "e }\n\n! OK.\n[cw2]> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\"
        "write1{abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstu"
        "vwxyzabcdefghijklmnop\\thepage }\n\n! OK.\n");
}

/* The hyphen a break inserts joins the letter in front of it, and what it
   swallows becomes the discretionary's replacement; see docs/DECISIONS.md,
   a-hyphen-that-ligatures. */
static int test_a_hyphen_that_ligatures(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\lccode`\\a=`\\a \\lc"
        "code`\\b=`\\b \\lccode`\\c=`\\c \\lccode`\\d=`\\d "
        "\\lccode`\\e=`\\e \\lccode`\\f=`\\f \\lccode`\\g=`"
        "\\g \\lccode`\\h=`\\h \\lccode`\\i=`\\i \\lccode`"
        "\\j=`\\j \\lccode`\\k=`\\k \\lccode`\\l=`\\l \\lcc"
        "ode`\\m=`\\m \\lccode`\\n=`\\n \\lccode`\\o=`\\o "
        "\\lccode`\\p=`\\p \\lccode`\\q=`\\q \\lccode`\\r=`"
        "\\r \\lccode`\\s=`\\s \\lccode`\\t=`\\t \\lccode`"
        "\\u=`\\u \\lccode`\\v=`\\v \\lccode`\\w=`\\w \\lcc"
        "ode`\\x=`\\x \\lccode`\\y=`\\y \\lccode`\\z=`\\z "
        "\\lccode`\\-=`\\- \\uchyph=1 \\lefthyphenmin=1 \\r"
        "ighthyphenmin=1 \\patterns{-1g} \\hyphenchar\\tenr"
        "m=45 \\pretolerance=-1 \\tolerance=10000 \\hsize=3"
        "0pt \\parindent=0pt \\baselineskip=12pt \\lineskip"
        "=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fi"
        "l \\leftskip=0pt \\rightskip=0pt \\showboxdepth=10"
        " \\message{[wag]}\\setbox1=\\hbox{and-gap}\\setbox"
        "0=\\vbox{\\noindent aa \\unhbox1 \\ bb\\par}\\show"
        "box0 \\message{[wide]}\\hsize=200pt \\setbox1=\\hb"
        "ox{and-gap}\\setbox0=\\vbox{\\noindent aa \\unhbox"
        "1 \\ bb\\par}\\showbox0%"
,
        "[wag]> \\box0=\n\\vbox(40.30554+0.0)x30.0\n.\\hbox"
        "(4.30554+0.0)x30.0\n..\\tenrm a\n..\\tenrm a\n..\\"
        "glue(\\rightskip) 0.0\n.\\glue(\\baselineskip) 5.0"
        "5556\n.\\hbox(6.94444+0.0)x30.0\n..\\tenrm a\n..\\"
        "tenrm n\n..\\tenrm d\n..\\discretionary\n..\\tenrm"
        " { (ligature --)\n..\\glue(\\rightskip) 0.0\n.\\gl"
        "ue(\\baselineskip) 7.69446\n.\\hbox(4.30554+1.9444"
        "4)x30.0\n..\\tenrm g\n..\\tenrm a\n..\\tenrm p\n.."
        "\\glue(\\rightskip) 0.0\n.\\glue(\\baselineskip) 3"
        ".11111\n.\\hbox(6.94444+0.0)x30.0, glue set 18.888"
        "85fil\n..\\tenrm b\n..\\tenrm b\n..\\penalty 10000"
        "\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glu"
        "e(\\rightskip) 0.0\n\n! OK.\n[wide]> \\box0=\n\\vb"
        "ox(6.94444+1.94444)x200.0\n.\\hbox(6.94444+1.94444"
        ")x200.0, glue set 135.88873fil\n..\\tenrm a\n..\\t"
        "enrm a\n..\\glue(\\spaceskip) 4.0\n..\\tenrm a\n.."
        "\\tenrm n\n..\\tenrm d\n..\\discretionary replacin"
        "g 1\n...\\tenrm { (ligature --)\n..\\tenrm -\n..\\"
        "tenrm g\n..\\tenrm a\n..\\tenrm p\n..\\glue(\\spac"
        "eskip) 4.0\n..\\tenrm b\n..\\tenrm b\n..\\penalty "
        "10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n.."
        "\\glue(\\rightskip) 0.0\n\n! OK.\n"
);
}

/* A column is as wide as its widest entry, even when that is a negative
   width; see docs/DECISIONS.md, a-column-of-negative-width. */
static int test_a_column_of_negative_width(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\#=6 \\catcode`\\&=4 \\showboxdepth=3 \\tabsk"
        "ip=2pt \\baselineskip=0pt \\lineskip=0pt \\lineski"
        "plimit=0pt \\message{[neg]}\\setbox0=\\vbox{\\hali"
        "gn{#&#\\cr\\kern5pt&\\kern-10pt\\cr}}\\showbox0 \\"
        "message{[two]}\\setbox0=\\vbox{\\halign{#&#\\cr\\k"
        "ern5pt&\\kern-10pt\\cr\\kern1pt&\\kern-3pt\\cr}}\\"
        "showbox0%"
,
        "[neg]> \\box0=\n\\vbox(0.0+0.0)x1.0\n.\\hbox(0.0+0"
        ".0)x1.0\n..\\glue(\\tabskip) 2.0\n..\\hbox(0.0+0.0"
        ")x5.0\n...\\kern 5.0\n..\\glue(\\tabskip) 2.0\n.."
        "\\hbox(0.0+0.0)x-10.0\n...\\kern -10.0\n..\\glue("
        "\\tabskip) 2.0\n\n! OK.\n[two]> \\box0=\n\\vbox(0."
        "0+0.0)x8.0\n.\\hbox(0.0+0.0)x8.0\n..\\glue(\\tabsk"
        "ip) 2.0\n..\\hbox(0.0+0.0)x5.0\n...\\kern 5.0\n.."
        "\\glue(\\tabskip) 2.0\n..\\hbox(0.0+0.0)x-3.0\n..."
        "\\kern -10.0\n..\\glue(\\tabskip) 2.0\n.\\glue(\\b"
        "aselineskip) 0.0\n.\\hbox(0.0+0.0)x8.0\n..\\glue("
        "\\tabskip) 2.0\n..\\hbox(0.0+0.0)x5.0\n...\\kern 1"
        ".0\n..\\glue(\\tabskip) 2.0\n..\\hbox(0.0+0.0)x-3."
        "0\n...\\kern -3.0\n..\\glue(\\tabskip) 2.0\n\n! OK"
        ".\n"
);
}

/* A preamble's repeating part serves every column after the ones written
   out; see docs/DECISIONS.md, a-repeating-preamble. */
static int test_a_repeating_preamble(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\#=6 \\catcode`\\&=4 \\tabskip=3pt \\showboxd"
        "epth=3 \\message{[loop]}\\setbox0=\\vbox{\\halign{"
        "A#\\hfil&&B#\\hfil\\cr x&y&z&w\\cr}}\\showbox0 \\m"
        "essage{[plain]}\\setbox0=\\vbox{\\halign{A#\\hfil&"
        "B#\\hfil\\cr x&y\\cr}}\\showbox0 \\message{[two]}"
        "\\tabskip=1pt \\setbox0=\\vbox{\\halign{A#\\hfil\\"
        "tabskip=2pt&&B#\\hfil\\tabskip=3pt&C#\\hfil\\tabsk"
        "ip=4pt\\cr p&q&r&s&t&u\\cr}}\\showbox0%"
,
        "[loop]> \\box0=\n\\vbox(6.83331+1.94444)x65.97237"
        "\n.\\hbox(6.83331+1.94444)x65.97237\n..\\glue(\\ta"
        "bskip) 3.0\n..\\hbox(6.83331+1.94444)x12.77782\n.."
        ".\\tenrm A\n...\\tenrm x\n...\\glue 0.0 plus 1.0fi"
        "l\n..\\glue(\\tabskip) 3.0\n..\\hbox(6.83331+1.944"
        "44)x12.36116\n...\\tenrm B\n...\\tenrm y\n...\\glu"
        "e 0.0 plus 1.0fil\n..\\glue(\\tabskip) 3.0\n..\\hb"
        "ox(6.83331+1.94444)x11.5278\n...\\tenrm B\n...\\te"
        "nrm z\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\tabsk"
        "ip) 3.0\n..\\hbox(6.83331+1.94444)x14.30559\n...\\"
        "tenrm B\n...\\tenrm w\n...\\glue 0.0 plus 1.0fil\n"
        "..\\glue(\\tabskip) 3.0\n\n! OK.\n[plain]> \\box0="
        "\n\\vbox(6.83331+1.94444)x34.13898\n.\\hbox(6.8333"
        "1+1.94444)x34.13898\n..\\glue(\\tabskip) 3.0\n..\\"
        "hbox(6.83331+1.94444)x12.77782\n...\\tenrm A\n..."
        "\\tenrm x\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\t"
        "abskip) 3.0\n..\\hbox(6.83331+1.94444)x12.36116\n."
        "..\\tenrm B\n...\\tenrm y\n...\\glue 0.0 plus 1.0f"
        "il\n..\\glue(\\tabskip) 3.0\n\n! OK.\n[two]> \\box"
        "0=\n\\vbox(6.83331+1.94444)x91.3335\n.\\hbox(6.833"
        "31+1.94444)x91.3335\n..\\glue(\\tabskip) 1.0\n..\\"
        "hbox(6.83331+1.94444)x13.05559\n...\\tenrm A\n..."
        "\\tenrm p\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\t"
        "abskip) 2.0\n..\\hbox(6.83331+1.94444)x12.36115\n."
        "..\\tenrm B\n...\\tenrm q\n...\\glue 0.0 plus 1.0f"
        "il\n..\\glue(\\tabskip) 3.0\n..\\hbox(6.83331+1.94"
        "444)x11.1389\n...\\tenrm C\n...\\tenrm r\n...\\glu"
        "e 0.0 plus 1.0fil\n..\\glue(\\tabskip) 4.0\n..\\hb"
        "ox(6.83331+1.94444)x11.0278\n...\\tenrm B\n...\\te"
        "nrm s\n...\\glue 0.0 plus 1.0fil\n..\\glue(\\tabsk"
        "ip) 3.0\n..\\hbox(6.83331+1.94444)x11.11113\n...\\"
        "tenrm C\n...\\tenrm t\n...\\glue 0.0 plus 1.0fil\n"
        "..\\glue(\\tabskip) 4.0\n..\\hbox(6.83331+1.94444)"
        "x12.63893\n...\\tenrm B\n...\\tenrm u\n...\\glue 0"
        ".0 plus 1.0fil\n..\\glue(\\tabskip) 3.0\n\n! OK.\n"
);}

/* An empty sub-formula is an ordinary atom, which is what makes a relation
   next to it take its spacing; see docs/DECISIONS.md, an-empty-atom. */
static int test_an_empty_atom(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\thin"
        "muskip=3mu \\medmuskip=4mu plus 2mu minus 4mu \\th"
        "ickmuskip=5mu plus 5mu \\mathcode`\\==\"303D \\mes"
        "sage{[e1]}\\setbox0=\\hbox{$=x$}\\showbox0 \\messa"
        "ge{[e2]}\\setbox0=\\hbox{${}=x$}\\showbox0 \\messa"
        "ge{[e3]}\\setbox0=\\hbox{$x={}$}\\showbox0 \\messa"
        "ge{[e4]}\\setbox0=\\hbox{$x=y$}\\showbox0%"
,
        "[e1]> \\box0=\n\\hbox(4.30554+0.0)x16.27078\n.\\ma"
        "thon\n.\\tenrm =\n.\\glue(\\thickmuskip) 2.77771 p"
        "lus 2.77771\n.\\teni x\n.\\mathoff\n\n! OK.\n[e2]>"
        " \\box0=\n\\hbox(4.30554+0.0)x19.0485\n.\\mathon\n"
        ".\\hbox(0.0+0.0)x0.0\n.\\glue(\\thickmuskip) 2.777"
        "71 plus 2.77771\n.\\tenrm =\n.\\glue(\\thickmuskip"
        ") 2.77771 plus 2.77771\n.\\teni x\n.\\mathoff\n\n!"
        " OK.\n[e3]> \\box0=\n\\hbox(4.30554+0.0)x19.0485\n"
        ".\\mathon\n.\\teni x\n.\\glue(\\thickmuskip) 2.777"
        "71 plus 2.77771\n.\\tenrm =\n.\\glue(\\thickmuskip"
        ") 2.77771 plus 2.77771\n.\\hbox(0.0+0.0)x0.0\n.\\m"
        "athoff\n\n! OK.\n[e4]> \\box0=\n\\hbox(4.30554+1.9"
        "4444)x24.31009\n.\\mathon\n.\\teni x\n.\\glue(\\th"
        "ickmuskip) 2.77771 plus 2.77771\n.\\tenrm =\n.\\gl"
        "ue(\\thickmuskip) 2.77771 plus 2.77771\n.\\teni y"
        "\n.\\kern0.35878\n.\\mathoff\n\n! OK.\n"
);
}

/* An accent goes over its nucleus's scripts as well, but it stands where the
   nucleus alone would put it; see docs/DECISIONS.md,
   an-accent-over-a-script. */
static int test_an_accent_over_a_script(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\skew"
        "char\\teni=127 \\message{[a]}\\setbox0=\\hbox{$\\m"
        "athaccent\"707E x_a$}\\showbox0 \\message{[c]}\\se"
        "tbox0=\\hbox{$\\mathaccent\"707E x^a$}\\showbox0 "
        "\\message{[Ua]}\\setbox0=\\hbox{$\\mathaccent\"701"
        "6 U_a$}\\showbox0 \\message{[Uc]}\\setbox0=\\hbox{"
        "$\\mathaccent\"7016 U^a$}\\showbox0 \\message{[Ub]"
        "}\\setbox0=\\hbox{$\\mathaccent\"7016 U^a_b$}\\sho"
        "wbox0 \\message{[br]}\\setbox0=\\hbox{$\\mathaccen"
        "t\"707E{x}_a$}\\showbox0 \\message{[r2]}\\setbox0="
        "\\hbox{$\\mathaccent\"707E{\\radical\"270370 x}^a$"
        "}\\showbox0%"
,
        "[a]> \\box0=\n\\hbox(6.67859+1.49998)x10.05292\n."
        "\\mathon\n.\\vbox(6.67859+1.49998)x10.05292\n..\\h"
        "box(6.67859+0.0)x0.0, shifted 0.63542\n...\\tenrm "
        "~\n..\\kern-4.30554\n..\\hbox(4.30554+1.49998)x10."
        "05292\n...\\teni x\n...\\hbox(3.01389+0.0)x4.33765"
        ", shifted 1.49998\n....\\seveni a\n.\\mathoff\n\n!"
        " OK.\n[c]> \\box0=\n\\hbox(6.67859+0.0)x10.05292\n"
        ".\\mathon\n.\\vbox(6.67859+0.0)x10.05292\n..\\hbox"
        "(6.67859+0.0)x0.0, shifted 0.63542\n...\\tenrm ~\n"
        "..\\kern-6.6428\n..\\hbox(6.6428+0.0)x10.05292\n.."
        ".\\teni x\n...\\hbox(3.01389+0.0)x4.33765, shifted"
        " -3.62892\n....\\seveni a\n.\\mathoff\n\n! OK.\n[U"
        "a]> \\box0=\n\\hbox(8.20554+1.49998)x11.16542\n.\\"
        "mathon\n.\\vbox(8.20554+1.49998)x11.16542\n..\\hbo"
        "x(5.67776+0.0)x0.0, shifted 1.7368\n...\\tenrm ^^V"
        "\n..\\kern-4.30554\n..\\hbox(6.83331+1.49998)x11.1"
        "6542\n...\\teni U\n...\\hbox(3.01389+0.0)x4.33765,"
        " shifted 1.49998\n....\\seveni a\n.\\mathoff\n\n! "
        "OK.\n[Uc]> \\box0=\n\\hbox(8.20554+0.0)x12.25568\n"
        ".\\mathon\n.\\vbox(8.20554+0.0)x12.25568\n..\\hbox"
        "(5.67776+0.0)x0.0, shifted 1.7368\n...\\tenrm ^^V"
        "\n..\\kern-4.30554\n..\\hbox(6.83331+0.0)x12.25568"
        "\n...\\teni U\n...\\kern1.09026\n...\\hbox(3.01389"
        "+0.0)x4.33765, shifted -3.62892\n....\\seveni a\n."
        "\\mathoff\n\n! OK.\n[Ub]> \\box0=\n\\hbox(8.20554+"
        "2.83209)x12.25568\n.\\mathon\n.\\vbox(8.20554+2.83"
        "209)x12.25568\n..\\hbox(5.67776+0.0)x0.0, shifted "
        "1.7368\n...\\tenrm ^^V\n..\\kern-4.30554\n..\\hbox"
        "(6.83331+2.83209)x12.25568\n...\\teni U\n...\\vbox"
        "(9.4749+0.0)x5.4279, shifted 2.83209\n....\\hbox(3"
        ".01389+0.0)x4.33765, shifted 1.09026\n.....\\seven"
        "i a\n....\\kern1.59991\n....\\hbox(4.8611+0.0)x3.5"
        "1666\n.....\\seveni b\n.\\mathoff\n\n! OK.\n[br]> "
        "\\box0=\n\\hbox(6.67859+1.49998)x10.05292\n.\\math"
        "on\n.\\vbox(6.67859+1.49998)x10.05292\n..\\hbox(6."
        "67859+0.0)x0.0, shifted 0.63542\n...\\tenrm ~\n.."
        "\\kern-4.30554\n..\\hbox(4.30554+1.49998)x10.05292"
        "\n...\\teni x\n...\\hbox(3.01389+0.0)x4.33765, shi"
        "fted 1.49998\n....\\seveni a\n.\\mathoff\n\n! OK."
        "\n[r2]> \\box0=\n\\hbox(10.91745+2.39725)x18.38628"
        "\n.\\mathon\n.\\vbox(10.37576+2.39725)x14.04863\n."
        ".\\hbox(6.67859+0.0)x0.0, shifted 4.5243\n...\\ten"
        "rm ~\n..\\kern-4.30554\n..\\hbox(8.00272+2.39725)x"
        "14.04863\n...\\hbox(0.39998+9.6)x8.33336, shifted "
        "-7.20276\n....\\tensy p\n...\\vbox(8.00272+0.0)x5."
        "71527\n....\\kern0.39998\n....\\rule(0.39998+0.0)x"
        "*\n....\\kern2.89722\n....\\hbox(4.30554+0.0)x5.71"
        "527\n.....\\teni x\n.\\hbox(3.01389+0.0)x4.33765, "
        "shifted -7.90356\n..\\seveni a\n.\\mathoff\n\n! OK"
        ".\n"
);}

/* A box, a rule and a formula set the space factor back to a thousand; a
   kern and a discretionary leave it alone; see docs/DECISIONS.md,
   what-resets-the-space-factor. */
static int test_what_resets_the_space_factor(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\spac"
        "eskip=0pt \\sfcode`\\,=1250 \\sfcode`\\]=0 \\messa"
        "ge{[rule]}\\setbox0=\\hbox{a,\\vrule width2pt\\rel"
        "ax] b}\\showbox0 \\message{[kern]}\\setbox0=\\hbox"
        "{a,\\kern2pt\\relax] b}\\showbox0 \\message{[box]}"
        "\\setbox0=\\hbox{a,\\hbox{x}] b}\\showbox0 \\messa"
        "ge{[math]}\\setbox0=\\hbox{a,$x$] b}\\showbox0 \\m"
        "essage{[disc]}\\setbox0=\\hbox{a,\\discretionary{}"
        "{}{}] b}\\showbox0 \\message{[none]}\\setbox0=\\hb"
        "ox{a,] b}\\showbox0%"
,
        "[rule]> \\box0=\n\\hbox(7.5+2.5)x21.44449\n.\\tenr"
        "m a\n.\\tenrm ,\n.\\rule(*+*)x2.0\n.\\tenrm ]\n.\\"
        "glue 3.33333 plus 1.66666 minus 1.11111\n.\\tenrm "
        "b\n\n! OK.\n[kern]> \\box0=\n\\hbox(7.5+2.5)x21.44"
        "449\n.\\tenrm a\n.\\tenrm ,\n.\\kern 2.0\n.\\tenrm"
        " ]\n.\\glue 3.33333 plus 2.08331 minus 0.88889\n."
        "\\tenrm b\n\n! OK.\n[box]> \\box0=\n\\hbox(7.5+2.5"
        ")x24.72229\n.\\tenrm a\n.\\tenrm ,\n.\\hbox(4.3055"
        "4+0.0)x5.2778\n..\\tenrm x\n.\\tenrm ]\n.\\glue 3."
        "33333 plus 1.66666 minus 1.11111\n.\\tenrm b\n\n! "
        "OK.\n[math]> \\box0=\n\\hbox(7.5+2.5)x25.15976\n."
        "\\tenrm a\n.\\tenrm ,\n.\\mathon\n.\\teni x\n.\\ma"
        "thoff\n.\\tenrm ]\n.\\glue 3.33333 plus 1.66666 mi"
        "nus 1.11111\n.\\tenrm b\n\n! OK.\n[disc]> \\box0="
        "\n\\hbox(7.5+2.5)x19.44449\n.\\tenrm a\n.\\tenrm ,"
        "\n.\\discretionary\n.\\tenrm ]\n.\\glue 3.33333 pl"
        "us 2.08331 minus 0.88889\n.\\tenrm b\n\n! OK.\n[no"
        "ne]> \\box0=\n\\hbox(7.5+2.5)x19.44449\n.\\tenrm a"
        "\n.\\tenrm ,\n.\\tenrm ]\n.\\glue 3.33333 plus 2.0"
        "8331 minus 0.88889\n.\\tenrm b\n\n! OK.\n"
);
}

/* A character with a space factor code of zero leaves the factor alone, and
   it is the character as read that counts, not the ligature it joined; see
   docs/DECISIONS.md, the-space-factor-of-a-ligature. */
static int test_the_space_factor_of_a_ligature(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\spac"
        "eskip=0pt \\sfcode`\\.=3000 \\sfcode`\\'=0 \\sfcod"
        "e`\\)=0 \\message{[a]}\\setbox0=\\hbox{a. b}\\show"
        "box0 \\message{[b]}\\setbox0=\\hbox{a.'' b}\\showb"
        "ox0 \\message{[c]}\\setbox0=\\hbox{a.) b}\\showbox"
        "0 \\message{[d]}\\setbox0=\\hbox{a.x b}\\showbox0%"
,
        "[a]> \\box0=\n\\hbox(6.94444+0.0)x17.77782\n.\\ten"
        "rm a\n.\\tenrm .\n.\\glue 4.44444 plus 4.99997 min"
        "us 0.37036\n.\\tenrm b\n\n! OK.\n[b]> \\box0=\n\\h"
        "box(6.94444+0.0)x22.77783\n.\\tenrm a\n.\\tenrm ."
        "\n.\\tenrm \" (ligature '')\n.\\glue 4.44444 plus "
        "4.99997 minus 0.37036\n.\\tenrm b\n\n! OK.\n[c]> "
        "\\box0=\n\\hbox(7.5+2.5)x21.66672\n.\\tenrm a\n.\\"
        "tenrm .\n.\\tenrm )\n.\\glue 4.44444 plus 4.99997 "
        "minus 0.37036\n.\\tenrm b\n\n! OK.\n[d]> \\box0=\n"
        "\\hbox(6.94444+0.0)x21.9445\n.\\tenrm a\n.\\tenrm "
        ".\n.\\tenrm x\n.\\glue 3.33333 plus 1.66666 minus "
        "1.11111\n.\\tenrm b\n\n! OK.\n"
);
}

/* An operator whose nucleus is a list stands on the baseline, but it still
   carries its limits above and below in display style; see
   docs/DECISIONS.md, large-operators. */
static int test_operators_that_are_lists(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\mess"
        "age{[log]}\\setbox0=\\hbox{$\\displaystyle\\mathop"
        "{\\tenrm log}$}\\showbox0 \\message{[logs]}\\setbo"
        "x0=\\hbox{$\\displaystyle\\mathop{\\tenrm log}^a_b"
        "$}\\showbox0 \\message{[mx]}\\setbox0=\\hbox{$\\di"
        "splaystyle\\mathop{x}^a$}\\showbox0 \\message{[hb]"
        "}\\setbox0=\\hbox{$\\mathop{\\hbox{\\tenrm x}}$}\\"
        "showbox0 \\message{[hbd]}\\setbox0=\\hbox{$\\displ"
        "aystyle\\mathop{\\hbox{\\tenrm x}}_c$}\\showbox0%"
,
        "[log]> \\box0=\n\\hbox(6.94444+1.94444)x13.15627\n"
        ".\\mathon\n.\\vbox(6.94444+1.94444)x13.15627\n..\\"
        "hbox(6.94444+1.94444)x13.15627\n...\\teni l\n...\\"
        "kern0.19678\n...\\teni o\n...\\teni g\n...\\kern0."
        "35878\n.\\mathoff\n\n! OK.\n[logs]> \\box0=\n\\hbo"
        "x(12.95831+9.4722)x13.15627\n.\\mathon\n.\\vbox(12"
        ".95831+9.4722)x13.15627\n..\\kern1.0\n..\\hbox(3.0"
        "1389+0.0)x13.15627, glue set 4.40932fil\n...\\glue"
        " 0.0 plus 1.0fil minus 1.0fil\n...\\seveni a\n..."
        "\\glue 0.0 plus 1.0fil minus 1.0fil\n..\\kern1.999"
        "98\n..\\hbox(6.94444+1.94444)x13.15627\n...\\teni "
        "l\n...\\kern0.19678\n...\\teni o\n...\\teni g\n..."
        "\\kern0.35878\n..\\kern1.66666\n..\\hbox(4.8611+0."
        "0)x13.15627, glue set 4.81981fil\n...\\glue 0.0 pl"
        "us 1.0fil minus 1.0fil\n...\\seveni b\n...\\glue 0"
        ".0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\matho"
        "ff\n\n! OK.\n[mx]> \\box0=\n\\hbox(10.66664+0.0)x5"
        ".71527\n.\\mathon\n.\\vbox(10.66664+0.0)x5.71527\n"
        "..\\kern1.0\n..\\hbox(3.01389+0.0)x5.71527, glue s"
        "et 0.68881fil\n...\\glue 0.0 plus 1.0fil minus 1.0"
        "fil\n...\\seveni a\n...\\glue 0.0 plus 1.0fil minu"
        "s 1.0fil\n..\\kern1.99998\n..\\hbox(4.65277+0.0)x5"
        ".71527\n...\\hbox(4.30554+0.0)x5.71527, shifted -0"
        ".34723\n....\\teni x\n.\\mathoff\n\n! OK.\n[hb]> "
        "\\box0=\n\\hbox(4.30554+0.0)x5.2778\n.\\mathon\n."
        "\\hbox(4.30554+0.0)x5.2778\n..\\tenrm x\n.\\mathof"
        "f\n\n! OK.\n[hbd]> \\box0=\n\\hbox(4.30554+7.0)x5."
        "2778\n.\\mathon\n.\\vbox(4.30554+7.0)x5.2778\n..\\"
        "hbox(4.30554+0.0)x5.2778\n...\\tenrm x\n..\\kern2."
        "98611\n..\\hbox(3.01389+0.0)x5.2778, glue set 0.85"
        "204fil\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n.."
        ".\\seveni c\n...\\glue 0.0 plus 1.0fil minus 1.0fi"
        "l\n..\\kern1.0\n.\\mathoff\n\n! OK.\n"
);
}

/* The break that ends a paragraph is measured without the kern that lets
   its last character stick out, though the line is set with one; see
   docs/DECISIONS.md, the-last-line-is-measured-square. */
static int test_the_last_line_is_measured_square(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\pdfp"
        "rotrudechars=2 \\rpcode\\tenrm`\\.=1000 \\hsize=38"
        "pt \\parindent=0pt \\spaceskip=4pt plus 2pt minus "
        "1pt \\pretolerance=-1 \\tolerance=200 \\hbadness=1"
        "0000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000p"
        "t \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus 1fil \\leftskip=0pt "
        "\\rightskip=0pt \\tracingonline=1 \\tracingparagra"
        "phs=1 \\setbox0=\\vbox{\\noindent xx xx xx.\\par}%"
,
        "\\tenrm xx xx xx. \n@\\par via @@0 b=* p=-10000 d="
        "*\n@@1: line 1.3- t=0 -> @@0\n\n"
);
}

/* A display closes its own group only once it has joined the vertical
   list, so the glue in front of it is the glue the display asked for; see
   docs/DECISIONS.md, a-display-closes-its-group-last. */
static int test_a_display_closes_its_group_last(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\hsize=100pt \\parindent=0pt \\baseline"
        "skip=0pt \\lineskip=1pt \\lineskiplimit=0pt \\parf"
        "illskip=0pt plus1fil \\leftskip=0pt \\rightskip=0p"
        "t \\predisplaypenalty=10000 \\postdisplaypenalty=0"
        " \\abovedisplayskip=3pt \\belowdisplayskip=4pt \\a"
        "bovedisplayshortskip=1pt \\belowdisplayshortskip=2"
        "pt \\tolerance=10000 \\pretolerance=-1 \\hbadness="
        "10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000"
        "pt \\showboxdepth=1 \\showboxbreadth=100 \\message"
        "{[out]}\\setbox0=\\vbox{\\noindent y$$\\hbox{}$$z"
        "\\par}\\showbox0 \\message{[in]}\\setbox0=\\vbox{"
        "\\noindent y$$\\lineskip=9pt \\hbox{}$$z\\par}\\sh"
        "owbox0%"
,
        "[out]> \\box0=\n\\vbox(15.55553+0.0)x100.0\n.\\hbo"
        "x(4.30554+1.94444)x100.0, glue set 94.7222fil []\n"
        ".\\penalty 10000\n.\\glue(\\abovedisplayshortskip)"
        " 1.0\n.\\glue(\\lineskip) 1.0\n.\\hbox(0.0+0.0)x0."
        "0, shifted 50.0, display []\n.\\penalty 0\n.\\glue"
        "(\\belowdisplayshortskip) 2.0\n.\\glue(\\lineskip)"
        " 1.0\n.\\hbox(4.30554+0.0)x100.0, glue set 95.5555"
        "6fil []\n\n! OK.\n[in]> \\box0=\n\\vbox(23.55553+0"
        ".0)x100.0\n.\\hbox(4.30554+1.94444)x100.0, glue se"
        "t 94.7222fil []\n.\\penalty 10000\n.\\glue(\\above"
        "displayshortskip) 1.0\n.\\glue(\\lineskip) 9.0\n."
        "\\hbox(0.0+0.0)x0.0, shifted 50.0, display []\n.\\"
        "penalty 0\n.\\glue(\\belowdisplayshortskip) 2.0\n."
        "\\glue(\\lineskip) 1.0\n.\\hbox(4.30554+0.0)x100.0"
        ", glue set 95.55556fil []\n\n! OK.\n"
);
}

/* A display counts as three lines of the paragraph it interrupts, so what
   follows takes the shape of a later line; see docs/DECISIONS.md,
   lines-carry-on-past-a-display. */
static int test_lines_carry_on_past_a_display(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\hsize=100pt \\parindent=0pt \\baseline"
        "skip=0pt \\lineskip=0pt \\lineskiplimit=0pt \\parf"
        "illskip=0pt plus1fil \\leftskip=0pt \\rightskip=0p"
        "t \\predisplaypenalty=10000 \\postdisplaypenalty=0"
        " \\abovedisplayskip=3pt \\belowdisplayskip=4pt \\a"
        "bovedisplayshortskip=1pt \\belowdisplayshortskip=2"
        "pt \\tolerance=10000 \\pretolerance=-1 \\hbadness="
        "10000 \\vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000"
        "pt \\showboxdepth=1 \\showboxbreadth=100 \\def\\W{"
        "\\vrule width20pt height1pt depth0pt\\hskip0pt plu"
        "s1fil}\\message{[shape]}\\setbox0=\\vbox{\\parshap"
        "e=6 0pt 90pt 0pt 80pt 0pt 70pt 0pt 60pt 0pt 50pt 0"
        "pt 40pt \\noindent\\W\\W$$\\hbox{}$$\\W\\W\\par}\\"
        "showbox0%"
,
        "[shape]> \\box0=\n\\vbox(9.0+0.0)x90.0\n.\\hbox(1."
        "0+0.0)x90.0, glue set 25.0fil []\n.\\penalty 10000"
        "\n.\\glue(\\abovedisplayskip) 3.0\n.\\glue(\\basel"
        "ineskip) 0.0\n.\\hbox(0.0+0.0)x0.0, shifted 35.0, "
        "display []\n.\\penalty 0\n.\\glue(\\belowdisplaysk"
        "ip) 4.0\n.\\glue(\\lineskip) 0.0\n.\\hbox(1.0+0.0)"
        "x50.0, glue set 5.0fil []\n\n! OK.\n"
);
}

/* \prevdepth inside a \noalign is the depth of the row before it, and
   what the \noalign leaves it at is what the row after it is spaced
   from; see docs/DECISIONS.md, prevdepth-inside-noalign. */
static int test_prevdepth_inside_noalign(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\#=6 \\catcode`\\&=4 \\baselineskip=12pt \\li"
        "neskip=1pt \\lineskiplimit=0pt \\showboxdepth=2 \\"
        "showboxbreadth=100 \\def\\R#1#2{\\vrule height#1 d"
        "epth#2 width3pt}\\setbox0=\\vbox{\\halign{#\\cr\\R"
        "{5pt}{6pt}\\cr\\noalign{\\message{[read pd=\\the\\"
        "prevdepth]}}\\R{5pt}{1pt}\\cr}}\\showbox0 \\messag"
        "e{[set]}\\setbox0=\\vbox{\\halign{#\\cr\\R{5pt}{1p"
        "t}\\cr\\noalign{\\prevdepth=20pt}\\R{5pt}{1pt}\\cr"
        "}}\\showbox0 \\message{[keep]}\\setbox0=\\vbox{\\h"
        "align{#\\cr\\R{5pt}{6pt}\\cr\\noalign{\\dimen0=\\p"
        "revdepth \\prevdepth=-1000pt \\hbox{}\\prevdepth="
        "\\dimen0 }\\R{5pt}{1pt}\\cr}\\hbox{}}\\showbox0%"
,
        "[read pd=6.0pt]> \\box0=\n\\vbox(17.0+1.0)x3.0\n."
        "\\hbox(5.0+6.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\"
        "hbox(5.0+6.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n.\\"
        "glue(\\baselineskip) 1.0\n.\\hbox(5.0+1.0)x3.0\n.."
        "\\glue(\\tabskip) 0.0\n..\\hbox(5.0+1.0)x3.0 []\n."
        ".\\glue(\\tabskip) 0.0\n\n! OK.\n[set]> \\box0=\n"
        "\\vbox(12.0+1.0)x3.0\n.\\hbox(5.0+1.0)x3.0\n..\\gl"
        "ue(\\tabskip) 0.0\n..\\hbox(5.0+1.0)x3.0 []\n..\\g"
        "lue(\\tabskip) 0.0\n.\\glue(\\lineskip) 1.0\n.\\hb"
        "ox(5.0+1.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\hbox"
        "(5.0+1.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n\n! OK."
        "\n[keep]> \\box0=\n\\vbox(29.0+0.0)x3.0\n.\\hbox(5"
        ".0+6.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\hbox(5.0"
        "+6.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n.\\hbox(0.0"
        "+0.0)x0.0\n.\\glue(\\baselineskip) 1.0\n.\\hbox(5."
        "0+1.0)x3.0\n..\\glue(\\tabskip) 0.0\n..\\hbox(5.0+"
        "1.0)x3.0 []\n..\\glue(\\tabskip) 0.0\n.\\glue(\\ba"
        "selineskip) 11.0\n.\\hbox(0.0+0.0)x0.0\n\n! OK.\n"
);}

/* An equation shrinks to make room for its number when it can, and the
   number goes below only when it cannot; see docs/DECISIONS.md,
   a-number-beside-a-squeezed-equation. */
static int test_a_number_beside_a_squeezed_equation(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\hsiz"
        "e=100pt \\parindent=0pt \\baselineskip=0pt \\lines"
        "kip=0pt \\lineskiplimit=0pt \\parfillskip=0pt \\le"
        "ftskip=0pt \\rightskip=0pt \\abovedisplayskip=3pt "
        "\\abovedisplayshortskip=1pt \\belowdisplayskip=4pt"
        " \\belowdisplayshortskip=2pt \\predisplaypenalty=1"
        "0000 \\postdisplaypenalty=0 \\tolerance=10000 \\pr"
        "etolerance=-1 \\hbadness=10000 \\vbadness=10000 \\"
        "hfuzz=1000pt \\vfuzz=1000pt \\showboxdepth=2 \\sho"
        "wboxbreadth=100 \\message{[room]}\\setbox0=\\vbox{"
        "\\noindent$$\\hbox to 60pt{}\\hskip20pt minus45pt"
        "\\hbox to 40pt{}\\eqno\\hbox to 10pt{}$$}\\showbox"
        "0 \\message{[tight]}\\setbox0=\\vbox{\\noindent$$"
        "\\hbox to 60pt{}\\hskip20pt minus2pt\\hbox to 40pt"
        "{}\\eqno\\hbox to 10pt{}$$}\\showbox0 \\message{[f"
        "il]}\\setbox0=\\vbox{\\noindent$$\\hbox to 60pt{}"
        "\\hskip20pt minus1fil\\hbox to 40pt{}\\eqno\\hbox "
        "to 10pt{}$$}\\showbox0%"
,
        "[room]> \\box0=\n\\vbox(3.0+0.0)x100.0\n.\\penalty"
        " 10000\n.\\glue(\\abovedisplayshortskip) 1.0\n.\\h"
        "box(0.0+0.0)x94.99998, shifted 5.00002\n..\\hbox(0"
        ".0+0.0)x79.99998, glue set - 0.88889, display []\n"
        "..\\kern5.0\n..\\hbox(0.0+0.0)x10.0, display []\n."
        "\\penalty 0\n.\\glue(\\belowdisplayshortskip) 2.0"
        "\n\n! OK.\n[tight]> \\box0=\n\\vbox(1.0+0.0)x100.0"
        "\n.\\penalty 10000\n.\\glue(\\abovedisplayshortski"
        "p) 1.0\n.\\hbox(0.0+0.0)x100.0, glue set - 1.0, di"
        "splay\n..\\hbox(0.0+0.0)x60.0\n..\\glue 20.0 minus"
        " 2.0\n..\\hbox(0.0+0.0)x40.0\n.\\penalty 10000\n."
        "\\glue(\\baselineskip) 0.0\n.\\hbox(0.0+0.0)x10.0,"
        " shifted 90.0, display\n..\\hbox(0.0+0.0)x10.0\n."
        "\\penalty 0\n\n! OK.\n[fil]> \\box0=\n\\vbox(3.0+0"
        ".0)x100.0\n.\\penalty 10000\n.\\glue(\\abovedispla"
        "yshortskip) 1.0\n.\\hbox(0.0+0.0)x94.99998, shifte"
        "d 5.00002\n..\\hbox(0.0+0.0)x79.99998, glue set - "
        "40.00002fil, display []\n..\\kern5.0\n..\\hbox(0.0"
        "+0.0)x10.0, display []\n.\\penalty 0\n.\\glue(\\be"
        "lowdisplayshortskip) 2.0\n\n! OK.\n"
);
}

/* A ligature made of two characters is read as part of a word only if
   another character follows it, so it keeps its own italic correction when
   it ends the run; see docs/DECISIONS.md, math-text-characters. */
static int test_the_italic_of_a_math_ligature(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\showboxdepth=4 \\showboxbreadth=100 \\"
        "message{[lig]}\\setbox0=\\hbox{$\\fam0 diff$}\\sho"
        "wbox0 \\message{[plain]}\\setbox0=\\hbox{$\\fam0 d"
        "id$}\\showbox0 \\message{[one]}\\setbox0=\\hbox{$"
        "\\fam0 f$}\\showbox0 \\message{[two]}\\setbox0=\\h"
        "box{$\\fam0 ff$}\\showbox0%"
,
        "[lig]> \\box0=\n\\hbox(6.94444+0.0)x14.9445\n.\\ma"
        "thon\n.\\tenrm d\n.\\tenrm i\n.\\tenrm ^^K\n.\\ker"
        "n0.77779\n.\\mathoff\n\n! OK.\n[plain]> \\box0=\n"
        "\\hbox(6.94444+0.0)x13.88893\n.\\mathon\n.\\tenrm "
        "d\n.\\tenrm i\n.\\tenrm d\n.\\mathoff\n\n! OK.\n[o"
        "ne]> \\box0=\n\\hbox(6.94444+0.0)x3.83336\n.\\math"
        "on\n.\\tenrm f\n.\\kern0.77779\n.\\mathoff\n\n! OK"
        ".\n[two]> \\box0=\n\\hbox(6.94444+0.0)x6.61115\n."
        "\\mathon\n.\\tenrm ^^K\n.\\kern0.77779\n.\\mathoff"
        "\n\n! OK.\n"
);
}

/* Only an operator whose nucleus is a character of its own is centred on the
   axis; one whose nucleus came out a box stands on the baseline; see
   docs/DECISIONS.md, only-a-character-is-centred. */
static int test_only_a_character_is_centred_still(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\catcode`\\^=7 \\catcode`\\_=8 \\catcod"
        "e`\\\"=12 \\showboxdepth=6 \\showboxbreadth=100 \\"
        "mathcode`\\==\"303D \\thinmuskip=3mu \\medmuskip=4"
        "mu \\thickmuskip=5mu \\message{[boxed]}\\setbox0="
        "\\hbox{$\\mathop{=}\\limits^?$}\\showbox0 \\messag"
        "e{[char]}\\setbox0=\\hbox{$\\mathop{x}\\limits^?$}"
        "\\showbox0 \\message{[list]}\\setbox0=\\hbox{$\\ma"
        "thop{xy}\\limits^?$}\\showbox0%"
,
        "[boxed]> \\box0=\n\\hbox(11.52983+0.0)x7.7778\n.\\"
        "mathon\n.\\vbox(11.52983+0.0)x7.7778\n..\\kern1.0"
        "\n..\\hbox(4.8611+0.0)x7.7778, glue set 2.00348fil"
        "\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n...\\sev"
        "enrm ?\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n.."
        "\\kern1.99998\n..\\hbox(3.66875+0.0)x7.7778\n...\\"
        "tenrm =\n.\\mathoff\n\n! OK.\n[char]> \\box0=\n\\h"
        "box(12.51385+0.0)x5.71527\n.\\mathon\n.\\vbox(12.5"
        "1385+0.0)x5.71527\n..\\kern1.0\n..\\hbox(4.8611+0."
        "0)x5.71527, glue set 0.97221fil\n...\\glue 0.0 plu"
        "s 1.0fil minus 1.0fil\n...\\sevenrm ?\n...\\glue 0"
        ".0 plus 1.0fil minus 1.0fil\n..\\kern1.99998\n..\\"
        "hbox(4.65277+0.0)x5.71527\n...\\hbox(4.30554+0.0)x"
        "5.71527, shifted -0.34723\n....\\teni x\n.\\mathof"
        "f\n\n! OK.\n[list]> \\box0=\n\\hbox(12.16663+1.944"
        "44)x10.97687\n.\\mathon\n.\\vbox(12.16663+1.94444)"
        "x10.97687\n..\\kern1.0\n..\\hbox(4.8611+0.0)x10.97"
        "687, glue set 3.60301fil\n...\\glue 0.0 plus 1.0fi"
        "l minus 1.0fil\n...\\sevenrm ?\n...\\glue 0.0 plus"
        " 1.0fil minus 1.0fil\n..\\kern1.99998\n..\\hbox(4."
        "30554+1.94444)x10.97687\n...\\teni x\n...\\teni y"
        "\n...\\kern0.35878\n.\\mathoff\n\n! OK.\n"
);
}

/* A box register used in a formula is an ordinary atom holding that box, so
   braces round it change nothing; see docs/DECISIONS.md,
   a-box-register-in-a-formula. */
static int test_a_box_register_in_a_formula(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\catcode`\\_=8 \\catcode`\\^=7 \\showbo"
        "xdepth=6 \\showboxbreadth=100 \\message{[grp]}\\se"
        "tbox1=\\hbox{y}\\setbox0=\\hbox{$\\copy1{\\box1}$}"
        "\\showbox0 \\message{[bare]}\\setbox1=\\hbox{y}\\s"
        "etbox0=\\hbox{$\\box1$}\\showbox0 \\message{[sub]}"
        "\\setbox1=\\hbox{y}\\setbox0=\\hbox{$\\box1_a$}\\s"
        "howbox0 \\message{[void]}\\setbox0=\\hbox{$\\box9 "
        "x$}\\showbox0%"
,
        "[grp]> \\box0=\n\\hbox(4.30554+1.94444)x10.5556\n."
        "\\mathon\n.\\hbox(4.30554+1.94444)x5.2778\n..\\ten"
        "rm y\n.\\hbox(4.30554+1.94444)x5.2778\n..\\tenrm y"
        "\n.\\mathoff\n\n! OK.\n[bare]> \\box0=\n\\hbox(4.3"
        "0554+1.94444)x5.2778\n.\\mathon\n.\\hbox(4.30554+1"
        ".94444)x5.2778\n..\\tenrm y\n.\\mathoff\n\n! OK.\n"
        "[sub]> \\box0=\n\\hbox(4.30554+2.44443)x9.61545\n."
        "\\mathon\n.\\hbox(4.30554+1.94444)x5.2778\n..\\ten"
        "rm y\n.\\hbox(3.01389+0.0)x4.33765, shifted 2.4444"
        "3\n..\\seveni a\n.\\mathoff\n\n! OK.\n[void]> \\bo"
        "x0=\n\\hbox(4.30554+0.0)x5.71527\n.\\mathon\n.\\te"
        "ni x\n.\\mathoff\n\n! OK.\n"
);
}

/* The two delimiters of a \left...\right group are an opening and a
   closing atom, so the spacing round what they hold is the ordinary spacing;
   see docs/DECISIONS.md, delimiters-are-atoms. */
static int test_delimiters_are_atoms(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\catcode`\\\"=12 \\showboxdepth=4 \\sho"
        "wboxbreadth=100 \\nulldelimiterspace=1.2pt \\delim"
        "itershortfall=5pt \\delimiterfactor=901 \\thinmusk"
        "ip=3mu \\medmuskip=4mu plus 2mu \\thickmuskip=5mu "
        "plus 5mu \\mathcode`\\+=\"202B \\mathcode`\\,=\"61"
        "3B \\mathcode`\\==\"303D \\message{[punct]}\\setbo"
        "x0=\\hbox{$\\left\\delimiter\"026A30C x,\\right.$}"
        "\\showbox0 \\message{[rel]}\\setbox0=\\hbox{$\\lef"
        "t\\delimiter\"026A30C x=y\\right.$}\\showbox0 \\me"
        "ssage{[bin]}\\setbox0=\\hbox{$\\left\\delimiter\"0"
        "26A30C +x\\right.$}\\showbox0 \\message{[after]}\\"
        "setbox0=\\hbox{$y\\left\\delimiter\"026A30C x\\rig"
        "ht.z$}\\showbox0%"
,
        "[punct]> \\box0=\n\\hbox(7.5+2.5)x14.13747\n.\\mat"
        "hon\n.\\hbox(7.5+2.5)x14.13747\n..\\hbox(7.5+2.5)x"
        "2.77779\n...\\tensy j\n..\\teni x\n..\\teni ;\n.."
        "\\glue(\\thinmuskip) 1.66663\n..\\hbox(0.0+0.0)x1."
        "2, shifted -2.5\n.\\mathoff\n\n! OK.\n[rel]> \\box"
        "0=\n\\hbox(7.5+2.5)x28.28787\n.\\mathon\n.\\hbox(7"
        ".5+2.5)x28.28787\n..\\hbox(7.5+2.5)x2.77779\n...\\"
        "tensy j\n..\\teni x\n..\\glue(\\thickmuskip) 2.777"
        "71 plus 2.77771\n..\\tenrm =\n..\\glue(\\thickmusk"
        "ip) 2.77771 plus 2.77771\n..\\teni y\n..\\kern0.35"
        "878\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\matho"
        "ff\n\n! OK.\n[bin]> \\box0=\n\\hbox(7.5+2.5)x17.47"
        "086\n.\\mathon\n.\\hbox(7.5+2.5)x17.47086\n..\\hbo"
        "x(7.5+2.5)x2.77779\n...\\tensy j\n..\\tenrm +\n.."
        "\\teni x\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\"
        "mathoff\n\n! OK.\n[after]> \\box0=\n\\hbox(7.5+2.5"
        ")x23.3782\n.\\mathon\n.\\teni y\n.\\kern0.35878\n."
        "\\glue(\\thinmuskip) 1.66663\n.\\hbox(7.5+2.5)x9.6"
        "9305\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j\n.."
        "\\teni x\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\"
        "glue(\\thinmuskip) 1.66663\n.\\teni z\n.\\kern0.43"
        "98\n.\\mathoff\n\n! OK.\n"
);
}

/* What stands between \left and \right is spliced into the line rather
   than boxed, and the pieces of an extensible delimiter are stacked flush;
   see docs/DECISIONS.md, what-stands-between-delimiters. */
static int test_what_stands_between_delimiters(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\catcode`\\\"=12 \\showboxdepth=6 \\sho"
        "wboxbreadth=100 \\nulldelimiterspace=1.2pt \\delim"
        "itershortfall=5pt \\delimiterfactor=901 \\thinmusk"
        "ip=3mu \\medmuskip=4mu \\thickmuskip=5mu \\mathcod"
        "e`\\+=\"202B \\message{[many]}\\setbox0=\\hbox{$\\"
        "left\\delimiter\"026A30C x+y\\right.$}\\showbox0 "
        "\\message{[vb]}\\setbox0=\\hbox{$\\left\\delimiter"
        "\"026A30C \\vbox to8.5pt{}\\right.$}\\showbox0 \\m"
        "essage{[mid]}\\setbox0=\\hbox{$\\left\\delimiter\""
        "026A30C x\\middle\\delimiter\"026A30C y\\right.$}"
        "\\showbox0 \\message{[empty]}\\setbox0=\\hbox{$\\l"
        "eft\\delimiter\"026A30C \\right.$}\\showbox0%"
,
        "[many]> \\box0=\n\\hbox(7.5+2.5)x27.17679\n.\\math"
        "on\n.\\hbox(7.5+2.5)x27.17679\n..\\hbox(7.5+2.5)x2"
        ".77779\n...\\tensy j\n..\\teni x\n..\\glue(\\medmu"
        "skip) 2.22217\n..\\tenrm +\n..\\glue(\\medmuskip) "
        "2.22217\n..\\teni y\n..\\kern0.35878\n..\\hbox(0.0"
        "+0.0)x1.2, shifted -2.5\n.\\mathoff\n\n! OK.\n[vb]"
        "> \\box0=\n\\hbox(8.50006+3.50006)x4.53333\n.\\mat"
        "hon\n.\\hbox(8.50006+3.50006)x4.53333\n..\\vbox(0."
        "0+12.00012)x3.33333, shifted -8.50006\n...\\hbox(0"
        ".0+6.00006)x3.33333\n....\\tenex ^^L\n...\\hbox(0."
        "0+6.00006)x3.33333\n....\\tenex ^^L\n..\\vbox(8.5+"
        "0.0)x0.0\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\"
        "mathoff\n\n! OK.\n[mid]> \\box0=\n\\hbox(7.5+2.5)x"
        "17.73244\n.\\mathon\n.\\hbox(7.5+2.5)x17.73244\n.."
        "\\hbox(7.5+2.5)x2.77779\n...\\tensy j\n..\\teni x"
        "\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j\n..\\ten"
        "i y\n..\\kern0.35878\n..\\hbox(0.0+0.0)x1.2, shift"
        "ed -2.5\n.\\mathoff\n\n! OK.\n[empty]> \\box0=\n\\"
        "hbox(7.5+2.5)x3.97778\n.\\mathon\n.\\hbox(7.5+2.5)"
        "x3.97778\n..\\hbox(7.5+2.5)x2.77779\n...\\tensy j"
        "\n..\\hbox(0.0+0.0)x1.2, shifted -2.5\n.\\mathoff"
        "\n\n! OK.\n"
);
}

/* A display wider than the room it has is squeezed into it, glue and all;
   see docs/DECISIONS.md, a-display-squeezed-to-fit. */
static int test_a_display_squeezed_to_fit(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\hsiz"
        "e=100pt \\parindent=0pt \\baselineskip=0pt \\lines"
        "kip=0pt \\lineskiplimit=0pt \\parfillskip=0pt \\le"
        "ftskip=0pt \\rightskip=0pt \\abovedisplayskip=3pt "
        "\\abovedisplayshortskip=1pt \\belowdisplayskip=4pt"
        " \\belowdisplayshortskip=2pt \\predisplaypenalty=1"
        "0000 \\postdisplaypenalty=0 \\tolerance=10000 \\pr"
        "etolerance=-1 \\hbadness=10000 \\vbadness=10000 \\"
        "hfuzz=1000pt \\vfuzz=1000pt \\showboxdepth=2 \\sho"
        "wboxbreadth=100 \\message{[over]}\\setbox0=\\vbox{"
        "\\noindent$$\\hbox to 60pt{}\\mskip0mu plus0mu\\hs"
        "kip20pt minus10pt\\hbox to 60pt{}$$}\\showbox0 \\m"
        "essage{[fits]}\\setbox0=\\vbox{\\noindent$$\\hbox "
        "to 60pt{}\\hskip20pt minus40pt\\hbox to 60pt{}$$}"
        "\\showbox0 \\message{[num]}\\setbox0=\\vbox{\\noin"
        "dent$$\\hbox to 60pt{}\\hskip20pt minus40pt\\hbox "
        "to 60pt{}\\eqno\\hbox to 10pt{}$$}\\showbox0%"
,
        "[over]> \\box0=\n\\vbox(3.0+0.0)x100.0\n.\\penalty"
        " 10000\n.\\glue(\\abovedisplayshortskip) 1.0\n.\\h"
        "box(0.0+0.0)x100.0, glue set - 1.0, display\n..\\h"
        "box(0.0+0.0)x60.0\n..\\glue 0.0\n..\\glue 20.0 min"
        "us 10.0\n..\\hbox(0.0+0.0)x60.0\n.\\penalty 0\n.\\"
        "glue(\\belowdisplayshortskip) 2.0\n\n! OK.\n[fits]"
        "> \\box0=\n\\vbox(3.0+0.0)x100.0\n.\\penalty 10000"
        "\n.\\glue(\\abovedisplayshortskip) 1.0\n.\\hbox(0."
        "0+0.0)x100.0, glue set - 1.0, display\n..\\hbox(0."
        "0+0.0)x60.0\n..\\glue 20.0 minus 40.0\n..\\hbox(0."
        "0+0.0)x60.0\n.\\penalty 0\n.\\glue(\\belowdisplays"
        "hortskip) 2.0\n\n! OK.\n[num]> \\box0=\n\\vbox(1.0"
        "+0.0)x100.0\n.\\penalty 10000\n.\\glue(\\abovedisp"
        "layshortskip) 1.0\n.\\hbox(0.0+0.0)x100.0, glue se"
        "t - 1.0, display\n..\\hbox(0.0+0.0)x60.0\n..\\glue"
        " 20.0 minus 40.0\n..\\hbox(0.0+0.0)x60.0\n.\\penal"
        "ty 10000\n.\\glue(\\baselineskip) 0.0\n.\\hbox(0.0"
        "+0.0)x10.0, shifted 90.0, display\n..\\hbox(0.0+0."
        "0)x10.0\n.\\penalty 0\n\n! OK.\n"
);
}

/* What stands to the left of a display is unknowable when the line before it
   is stretching or shrinking on glue of the line's own order; see
   docs/DECISIONS.md, the-size-before-a-display. */
static int test_the_size_before_a_display(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\catc"
        "ode`\\$=3 \\hsize=100pt \\parindent=0pt \\baseline"
        "skip=0pt \\lineskip=0pt \\lineskiplimit=0pt \\parf"
        "illskip=0pt \\leftskip=0pt \\rightskip=0pt \\above"
        "displayskip=3pt \\abovedisplayshortskip=1pt \\belo"
        "wdisplayskip=4pt \\belowdisplayshortskip=2pt \\pre"
        "displaypenalty=10000 \\postdisplaypenalty=0 \\tole"
        "rance=10000 \\pretolerance=-1 \\hbadness=10000 \\v"
        "badness=10000 \\hfuzz=1000pt \\vfuzz=1000pt \\show"
        "boxdepth=1 \\showboxbreadth=100 \\message{[rigid]}"
        "\\spaceskip=4pt \\setbox0=\\vbox{\\noindent x x$$"
        "\\hbox to 10pt{}$$}\\showbox0 \\message{[stretch]}"
        "\\spaceskip=4pt plus 2pt \\setbox0=\\vbox{\\noinde"
        "nt x x$$\\hbox to 10pt{}$$}\\showbox0 \\message{[s"
        "hrink]}\\spaceskip=4pt minus 2pt \\setbox0=\\vbox{"
        "\\noindent x x$$\\hbox to 10pt{}$$}\\showbox0 \\me"
        "ssage{[shifted]}\\spaceskip=4pt \\setbox0=\\vbox{"
        "\\parshape=2 30pt 70pt 0pt 100pt \\noindent x x$$"
        "\\hbox to 10pt{}$$}\\showbox0%"
,
        "[rigid]> \\box0=\n\\vbox(7.30554+0.0)x100.0\n.\\hb"
        "ox(4.30554+0.0)x100.0 []\n.\\penalty 10000\n.\\glu"
        "e(\\abovedisplayshortskip) 1.0\n.\\glue(\\baseline"
        "skip) 0.0\n.\\hbox(0.0+0.0)x10.0, shifted 45.0, di"
        "splay []\n.\\penalty 0\n.\\glue(\\belowdisplayshor"
        "tskip) 2.0\n\n! OK.\n[stretch]> \\box0=\n\\vbox(11"
        ".30554+0.0)x100.0\n.\\hbox(4.30554+0.0)x100.0, glu"
        "e set 42.7222 []\n.\\penalty 10000\n.\\glue(\\abov"
        "edisplayskip) 3.0\n.\\glue(\\baselineskip) 0.0\n."
        "\\hbox(0.0+0.0)x10.0, shifted 45.0, display []\n."
        "\\penalty 0\n.\\glue(\\belowdisplayskip) 4.0\n\n! "
        "OK.\n[shrink]> \\box0=\n\\vbox(7.30554+0.0)x100.0"
        "\n.\\hbox(4.30554+0.0)x100.0 []\n.\\penalty 10000"
        "\n.\\glue(\\abovedisplayshortskip) 1.0\n.\\glue(\\"
        "baselineskip) 0.0\n.\\hbox(0.0+0.0)x10.0, shifted "
        "45.0, display []\n.\\penalty 0\n.\\glue(\\belowdis"
        "playshortskip) 2.0\n\n! OK.\n[shifted]> \\box0=\n"
        "\\vbox(11.30554+0.0)x100.0\n.\\hbox(4.30554+0.0)x7"
        "0.0, shifted 30.0 []\n.\\penalty 10000\n.\\glue(\\"
        "abovedisplayskip) 3.0\n.\\glue(\\baselineskip) 0.0"
        "\n.\\hbox(0.0+0.0)x10.0, shifted 45.0, display []"
        "\n.\\penalty 0\n.\\glue(\\belowdisplayskip) 4.0\n"
        "\n! OK.\n"
);}

/* Whether a display takes the short skips is decided by the offset it is
   really given, equation number and all; see docs/DECISIONS.md,
   a-short-display-skip. */
static int test_a_short_display_skip(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\hsiz"
        "e=100pt \\parindent=0pt \\baselineskip=0pt \\lines"
        "kip=0pt \\lineskiplimit=0pt \\parfillskip=0pt \\le"
        "ftskip=0pt \\rightskip=0pt \\abovedisplayskip=3pt "
        "\\abovedisplayshortskip=1pt \\belowdisplayskip=4pt"
        " \\belowdisplayshortskip=2pt \\predisplaypenalty=1"
        "0000 \\postdisplaypenalty=0 \\tolerance=10000 \\pr"
        "etolerance=-1 \\hbadness=10000 \\vbadness=10000 \\"
        "hfuzz=1000pt \\vfuzz=1000pt \\showboxdepth=1 \\sho"
        "wboxbreadth=100 \\message{[num]}\\setbox0=\\vbox{"
        "\\noindent\\hbox to 0pt{}$$\\hbox to 50pt{}\\eqno"
        "\\hbox to 20pt{}$$}\\showbox0 \\message{[bare]}\\s"
        "etbox0=\\vbox{\\noindent\\hbox to 0pt{}$$\\hbox to"
        " 50pt{}$$}\\showbox0%"
,
        "[num]> \\box0=\n\\vbox(7.0+0.0)x100.0\n.\\hbox(0.0"
        "+0.0)x100.0 []\n.\\penalty 10000\n.\\glue(\\aboved"
        "isplayskip) 3.0\n.\\glue(\\baselineskip) 0.0\n.\\h"
        "box(0.0+0.0)x85.0, shifted 15.0 []\n.\\penalty 0\n"
        ".\\glue(\\belowdisplayskip) 4.0\n\n! OK.\n[bare]> "
        "\\box0=\n\\vbox(3.0+0.0)x100.0\n.\\hbox(0.0+0.0)x1"
        "00.0 []\n.\\penalty 10000\n.\\glue(\\abovedisplays"
        "hortskip) 1.0\n.\\glue(\\baselineskip) 0.0\n.\\hbo"
        "x(0.0+0.0)x50.0, shifted 25.0, display []\n.\\pena"
        "lty 0\n.\\glue(\\belowdisplayshortskip) 2.0\n\n! O"
        "K.\n"
);
}

/* A large operator sits on the axis, takes a bigger shape in display style,
   and carries its limits above and below unless told otherwise; see
   docs/DECISIONS.md, large-operators. */
static int test_large_operators(void)
{
    return run_document(
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
        "r\\sevensy=48 \\skewchar\\fivesy=48 \\tenrm \\mess"
        "age{[t]}\\setbox0=\\hbox{$\\mathchar\"1352 $}\\sho"
        "wbox0 \\message{[d]}\\setbox0=\\hbox{$\\displaysty"
        "le\\mathchar\"1352 $}\\showbox0 \\message{[s]}\\se"
        "tbox0=\\hbox{$\\displaystyle\\mathchar\"1350 $}\\s"
        "howbox0 \\message{[sl]}\\setbox0=\\hbox{$\\display"
        "style\\mathchar\"1350 ^a_b$}\\showbox0 \\message{["
        "st]}\\setbox0=\\hbox{$\\mathchar\"1350 ^a_b$}\\sho"
        "wbox0 \\message{[nl]}\\setbox0=\\hbox{$\\displayst"
        "yle\\mathchar\"1350 \\nolimits^a_b$}\\showbox0 \\m"
        "essage{[it]}\\setbox0=\\hbox{$\\mathchar\"1352 ^a_"
        "b$}\\showbox0 \\message{[id]}\\setbox0=\\hbox{$\\d"
        "isplaystyle\\mathchar\"1352 ^a_b$}\\showbox0 \\mes"
        "sage{[il]}\\setbox0=\\hbox{$\\mathchar\"1352 \\lim"
        "its^a_b$}\\showbox0 \\message{[sup]}\\setbox0=\\hb"
        "ox{$\\mathchar\"1352 ^a$}\\showbox0 \\message{[sub"
        "]}\\setbox0=\\hbox{$\\mathchar\"1352 _b$}\\showbox"
        "0%"
,
        "[t]> \\box0=\n\\hbox(8.0556+3.05562)x6.66667\n.\\m"
        "athon\n.\\hbox(0.0+11.11122)x6.66667, shifted -8.0"
        "556\n..\\tenex R\n.\\mathoff\n\n! OK.\n[d]> \\box0"
        "=\n\\hbox(13.61122+8.61124)x10.00002\n.\\mathon\n."
        "\\vbox(13.61122+8.61124)x10.00002\n..\\hbox(13.611"
        "22+8.61124)x10.00002\n...\\hbox(0.0+22.22246)x10.0"
        "0002, shifted -13.61122\n....\\tenex Z\n.\\mathoff"
        "\n\n! OK.\n[s]> \\box0=\n\\hbox(10.50006+5.50006)x"
        "14.44447\n.\\mathon\n.\\vbox(10.50006+5.50006)x14."
        "44447\n..\\hbox(10.50006+5.50006)x14.44447\n...\\h"
        "box(1.0+15.00012)x14.44447, shifted -9.50006\n...."
        "\\tenex X\n.\\mathoff\n\n! OK.\n[sl]> \\box0=\n\\h"
        "box(16.51393+13.02782)x14.44447\n.\\mathon\n.\\vbo"
        "x(16.51393+13.02782)x14.44447\n..\\kern1.0\n..\\hb"
        "ox(3.01389+0.0)x14.44447, glue set 5.05342fil\n..."
        "\\glue 0.0 plus 1.0fil minus 1.0fil\n...\\seveni a"
        "\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n..\\kern"
        "1.99998\n..\\hbox(10.50006+5.50006)x14.44447\n..."
        "\\hbox(1.0+15.00012)x14.44447, shifted -9.50006\n."
        "...\\tenex X\n..\\kern1.66666\n..\\hbox(4.8611+0.0"
        ")x14.44447, glue set 5.46391fil\n...\\glue 0.0 plu"
        "s 1.0fil minus 1.0fil\n...\\seveni b\n...\\glue 0."
        "0 plus 1.0fil minus 1.0fil\n..\\kern1.0\n.\\mathof"
        "f\n\n! OK.\n[st]> \\box0=\n\\hbox(8.04175+3.00005)"
        "x14.89323\n.\\mathon\n.\\hbox(0.0+10.00012)x10.555"
        "59, shifted -7.50006\n..\\tenex P\n.\\vbox(11.0418"
        "+0.0)x4.33765, shifted 3.00005\n..\\hbox(3.01389+0"
        ".0)x4.33765\n...\\seveni a\n..\\kern3.16681\n..\\h"
        "box(4.8611+0.0)x3.51666\n...\\seveni b\n.\\mathoff"
        "\n\n! OK.\n[nl]> \\box0=\n\\hbox(11.04175+6.00005)"
        "x18.78212\n.\\mathon\n.\\hbox(1.0+15.00012)x14.444"
        "47, shifted -9.50006\n..\\tenex X\n.\\vbox(17.0418"
        "+0.0)x4.33765, shifted 6.00005\n..\\hbox(3.01389+0"
        ".0)x4.33765\n...\\seveni a\n..\\kern9.16681\n..\\h"
        "box(4.8611+0.0)x3.51666\n...\\seveni b\n.\\mathoff"
        "\n\n! OK.\n[it]> \\box0=\n\\hbox(8.59729+3.5556)x1"
        "1.00432\n.\\mathon\n.\\hbox(0.0+11.11122)x4.72223,"
        " shifted -8.0556\n..\\tenex R\n.\\vbox(12.1529+0.0"
        ")x6.28209, shifted 3.5556\n..\\hbox(3.01389+0.0)x4"
        ".33765, shifted 1.94444\n...\\seveni a\n..\\kern4."
        "27791\n..\\hbox(4.8611+0.0)x3.51666\n...\\seveni b"
        "\n.\\mathoff\n\n! OK.\n[id]> \\box0=\n\\hbox(19.62"
        "509+16.13899)x10.00002\n.\\mathon\n.\\vbox(19.6250"
        "9+16.13899)x10.00002\n..\\kern1.0\n..\\hbox(3.0138"
        "9+0.0)x10.00002, glue set 2.83119fil, shifted 2.22"
        "223\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n...\\"
        "seveni a\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n"
        "..\\kern1.99998\n..\\hbox(13.61122+8.61124)x10.000"
        "02\n...\\hbox(0.0+22.22246)x10.00002, shifted -13."
        "61122\n....\\tenex Z\n..\\kern1.66666\n..\\hbox(4."
        "8611+0.0)x10.00002, glue set 3.24168fil, shifted -"
        "2.22223\n...\\glue 0.0 plus 1.0fil minus 1.0fil\n."
        "..\\seveni b\n...\\glue 0.0 plus 1.0fil minus 1.0f"
        "il\n..\\kern1.0\n.\\mathoff\n\n! OK.\n[il]> \\box0"
        "=\n\\hbox(14.06947+10.58337)x6.66667\n.\\mathon\n."
        "\\vbox(14.06947+10.58337)x6.66667\n..\\kern1.0\n.."
        "\\hbox(3.01389+0.0)x6.66667, glue set 1.16452fil, "
        "shifted 0.97223\n...\\glue 0.0 plus 1.0fil minus 1"
        ".0fil\n...\\seveni a\n...\\glue 0.0 plus 1.0fil mi"
        "nus 1.0fil\n..\\kern1.99998\n..\\hbox(8.0556+3.055"
        "62)x6.66667\n...\\hbox(0.0+11.11122)x6.66667, shif"
        "ted -8.0556\n....\\tenex R\n..\\kern1.66666\n..\\h"
        "box(4.8611+0.0)x6.66667, glue set 1.57501fil, shif"
        "ted -0.97223\n...\\glue 0.0 plus 1.0fil minus 1.0f"
        "il\n...\\seveni b\n...\\glue 0.0 plus 1.0fil minus"
        " 1.0fil\n..\\kern1.0\n.\\mathoff\n\n! OK.\n[sup]> "
        "\\box0=\n\\hbox(8.59729+3.05562)x11.00432\n.\\math"
        "on\n.\\hbox(0.0+11.11122)x6.66667, shifted -8.0556"
        "\n..\\tenex R\n.\\hbox(3.01389+0.0)x4.33765, shift"
        "ed -5.5834\n..\\seveni a\n.\\mathoff\n\n! OK.\n[su"
        "b]> \\box0=\n\\hbox(8.0556+3.5556)x8.23889\n.\\mat"
        "hon\n.\\hbox(0.0+11.11122)x4.72223, shifted -8.055"
        "6\n..\\tenex R\n.\\hbox(4.8611+0.0)x3.51666, shift"
        "ed 3.5556\n..\\seveni b\n.\\mathoff\n\n! OK.\n"
);}

/* The paragraph's own first line lets its first character stick out past the
   margin, and that is what the breaker measures; see docs/DECISIONS.md,
   the-first-line-protrudes-too. */
static int test_the_first_line_protrudes_too(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\pdfprotrudechars=2 "
        "\\lpcode\\tenrm`\\O=1000 \\hsize=140pt \\parindent"
        "=0pt \\baselineskip=12pt \\lineskip=0pt \\lineskip"
        "limit=0pt \\parfillskip=0pt plus1fil \\leftskip=0p"
        "t \\rightskip=0pt \\pretolerance=1000 \\tolerance="
        "1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz=100"
        "0pt \\vfuzz=1000pt \\spaceskip=4pt plus 2pt minus "
        "1pt \\uchyph=0 \\tracingonline=1 \\tracingparagrap"
        "hs=1 \\setbox0=\\vbox{\\noindent Onetwo three four"
        " five six seven eight nine ten\\par}%"
,
        "@firstpass\n\\tenrm Onetwo three four five six sev"
        "en \n@ via @@0 b=27 p=0 d=729\n@@1: line 1.1 t=729"
        " -> @@0\neight nine ten \n@\\par via @@1 b=0 p=-10"
        "000 d=0\n@@2: line 2.2- t=729 -> @@1\n\n"
);}

/* \tracingparagraphs writes the passes out as the reference does; see
   docs/DECISIONS.md, tracing-paragraphs. */
static int test_tracing_paragraphs(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\lccode`\\a=`\\a \\lc"
        "code`\\b=`\\b \\lccode`\\c=`\\c \\lccode`\\d=`\\d "
        "\\lccode`\\e=`\\e \\lccode`\\f=`\\f \\lccode`\\g=`"
        "\\g \\lccode`\\h=`\\h \\lccode`\\i=`\\i \\lccode`"
        "\\j=`\\j \\lccode`\\k=`\\k \\lccode`\\l=`\\l \\lcc"
        "ode`\\m=`\\m \\lccode`\\n=`\\n \\lccode`\\o=`\\o "
        "\\lccode`\\p=`\\p \\lccode`\\q=`\\q \\lccode`\\r=`"
        "\\r \\lccode`\\s=`\\s \\lccode`\\t=`\\t \\lccode`"
        "\\u=`\\u \\lccode`\\v=`\\v \\lccode`\\w=`\\w \\lcc"
        "ode`\\x=`\\x \\lccode`\\y=`\\y \\lccode`\\z=`\\z "
        "\\patterns{a1bra ca1da b1ra} \\hyphenchar\\tenrm=4"
        "5 \\hsize=100pt \\parindent=0pt \\baselineskip=12p"
        "t \\lineskip=0pt \\lineskiplimit=0pt \\parfillskip"
        "=0pt plus1fil \\leftskip=0pt \\rightskip=0pt \\pre"
        "tolerance=100 \\tolerance=200 \\hbadness=10000 \\v"
        "badness=10000 \\hfuzz=1000pt \\vfuzz=1000pt \\line"
        "penalty=10 \\adjdemerits=10000 \\doublehyphendemer"
        "its=10000 \\finalhyphendemerits=5000 \\hyphenpenal"
        "ty=50 \\exhyphenpenalty=50 \\uchyph=0 \\lefthyphen"
        "min=2 \\righthyphenmin=3 \\spaceskip=4pt plus 2pt "
        "minus 1pt \\tracingonline=1 \\tracingparagraphs=1 "
        "\\setbox0=\\vbox{\\noindent one two three four abr"
        "acadabra five six seven eight\\par}%"
,
        "@firstpass\n@secondpass\n\\tenrm one two three fou"
        "r ab-\n@\\discretionary via @@0 b=0 p=50 d=2600\n@"
        "@1: line 1.2- t=2600 -> @@0\nraca-da-bra five six "
        "seven \n@ via @@1 b=* p=0 d=*\n@@2: line 2.3 t=260"
        "0 -> @@1\neight \n@\\par via @@2 b=0 p=-10000 d=*"
        "\n@@3: line 3.2- t=2600 -> @@2\n\n"
);
}

/* A pdf destination is a whatsit in the list, written out the way the
   reference writes it; see docs/DECISIONS.md, pdf-destinations. */
static int test_pdf_destinations(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\pdfoutput=1 \\messag"
        "e{[a]}\\setbox0=\\hbox{a\\pdfdest name{x} xyz b}\\"
        "showbox0 \\message{[b]}\\setbox0=\\hbox{\\pdfdest "
        "num7 xyz zoom 2000 }\\showbox0 \\message{[c]}\\set"
        "box0=\\hbox{\\pdfdest name{y} fitbv}\\showbox0 \\m"
        "essage{[d]}\\setbox0=\\hbox{\\pdfdest name{z} fitr"
        " width 10pt height 3pt depth 1pt}\\showbox0 \\mess"
        "age{[e]}\\setbox0=\\hbox{\\pdfdest name{w} fitr he"
        "ight 3pt}\\showbox0 \\message{[f]}\\setbox0=\\hbox"
        "{\\pdfdest name{aaaaaaaaaabbbbbbbbbbccccccccccdddd"
        "ddddddeeeeeeeeeeffffffffffgggggggggghhhhhhhhhh} fi"
        "t}\\showbox0%"
,
        "[a]> \\box0=\n\\hbox(6.94444+0.0)x10.55559\n.\\ten"
        "rm a\n.\\pdfdest name{x} xyz\n.\\tenrm b\n\n! OK."
        "\n[b]> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\pdfdest nu"
        "m7 xyz zoom2000\n\n! OK.\n[c]> \\box0=\n\\hbox(0.0"
        "+0.0)x0.0\n.\\pdfdest name{y} fitbv\n\n! OK.\n[d]>"
        " \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\pdfdest name{z} "
        "fitr(3.0+1.0)x10.0\n\n! OK.\n[e]> \\box0=\n\\hbox("
        "0.0+0.0)x0.0\n.\\pdfdest name{w} fitr(3.0+*)x*\n\n"
        "! OK.\n[f]> \\box0=\n\\hbox(0.0+0.0)x0.0\n.\\pdfde"
        "st name{aaaaaaaaaabbbbbbbbbbccccccccccddddddddddee"
        "eeeeeeeeffffffffffggggggggg\\ETC.} fit\n\n! OK.\n"
);
}

/* Once the page builder has emptied the contribution list, \lastnodetype
   and its relatives report the last node it took; see docs/DECISIONS.md,
   the-last-node-of-a-page. */
static int test_the_last_node_of_a_page(void)
{
    return run_document(
        "\\vsize=200pt \\maxdepth=4pt \\output={\\shipout\\"
        "box255 }\\message{[A=\\the\\lastnodetype]}\\hbox{}"
        "\\message{[B=\\the\\lastnodetype]}\\vskip3pt \\mes"
        "sage{[C=\\the\\lastnodetype][Cs=\\the\\lastskip]}"
        "\\penalty5 \\message{[D=\\the\\lastnodetype][Dp=\\"
        "the\\lastpenalty]}\\kern2pt \\message{[E=\\the\\la"
        "stnodetype][Ek=\\the\\lastkern]}\\hbox{}\\message{"
        "[F=\\the\\lastnodetype][Fs=\\the\\lastskip][Fk=\\t"
        "he\\lastkern][Fp=\\the\\lastpenalty]}\\setbox0=\\v"
        "box{\\message{[G=\\the\\lastnodetype]}\\hbox{}\\me"
        "ssage{[H=\\the\\lastnodetype]}}%",
        "[A=-1][B=1][C=11][Cs=3.0pt][D=13][Dp=5][E=12][Ek=2"
        ".0pt][F=1][Fs=0.0pt][Fk=0.0pt][Fp=0][G=-1][H=1]");
}

/* A line that breaks at a penalty keeps the penalty; see docs/DECISIONS.md,
   a-line-that-breaks-at-a-penalty. */
static int test_a_line_that_breaks_at_a_penalty(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\message{[forced]}\\s"
        "etbox0=\\vbox{\\noindent aa\\penalty-10000 bb\\par"
        "}\\showbox0 \\message{[chosen]}\\setbox0=\\vbox{\\"
        "hsize=30pt \\noindent aaaa\\penalty0 bbbb\\par}\\s"
        "howbox0 \\message{[after]}\\setbox0=\\vbox{\\hsize"
        "=30pt \\noindent aaaa\\penalty0 \\hskip4pt bbbb\\p"
        "ar}\\showbox0%"
,
        "[forced]> \\box0=\n\\vbox(16.30554+0.0)x200.0\n.\\"
        "hbox(4.30554+0.0)x200.0\n..\\tenrm a\n..\\tenrm a"
        "\n..\\penalty -10000\n..\\glue(\\rightskip) 0.0\n."
        "\\glue(\\baselineskip) 5.05556\n.\\hbox(6.94444+0."
        "0)x200.0, glue set 188.88885fil\n..\\tenrm b\n..\\"
        "tenrm b\n..\\penalty 10000\n..\\glue(\\parfillskip"
        ") 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! "
        "OK.\n[chosen]> \\box0=\n\\vbox(16.30554+0.0)x30.0"
        "\n.\\hbox(4.30554+0.0)x30.0\n..\\tenrm a\n..\\tenr"
        "m a\n..\\tenrm a\n..\\tenrm a\n..\\penalty 0\n..\\"
        "glue(\\rightskip) 0.0\n.\\glue(\\baselineskip) 5.0"
        "5556\n.\\hbox(6.94444+0.0)x30.0, glue set 7.77771f"
        "il\n..\\tenrm b\n..\\tenrm b\n..\\tenrm b\n..\\ten"
        "rm b\n..\\penalty 10000\n..\\glue(\\parfillskip) 0"
        ".0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\n[after]> \\box0=\n\\vbox(16.30554+0.0)x30.0\n.\\"
        "hbox(4.30554+0.0)x30.0\n..\\tenrm a\n..\\tenrm a\n"
        "..\\tenrm a\n..\\tenrm a\n..\\penalty 0\n..\\glue("
        "\\rightskip) 0.0\n.\\glue(\\baselineskip) 5.05556"
        "\n.\\hbox(6.94444+0.0)x30.0, glue set 7.77771fil\n"
        "..\\tenrm b\n..\\tenrm b\n..\\tenrm b\n..\\tenrm b"
        "\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 p"
        "lus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n"
);
}

/* A sub-formula that comes to one unshifted box is that box; anything else is
   packaged; see docs/DECISIONS.md, a-list-that-is-one-box. */
static int test_a_list_that_is_one_box(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\message{[one]}\\setb"
        "ox0=\\hbox{$U{\\hbox{a}}U$}\\showbox0 \\message{[s"
        "hift]}\\setbox0=\\hbox{$U{\\mathop{x}}U$}\\showbox"
        "0 \\message{[pair]}\\setbox0=\\hbox{$U{\\hbox{a}\\"
        "hbox{b}}U$}\\showbox0 \\message{[char]}\\setbox0="
        "\\hbox{$U{a}U$}\\showbox0 \\message{[sup]}\\setbox"
        "0=\\hbox{$U^{\\hbox{a}}$}\\showbox0%"
,
        "[one]> \\box0=\n\\hbox(6.83331+0.0)x20.83607\n.\\m"
        "athon\n.\\teni U\n.\\kern1.09026\n.\\hbox(4.30554+"
        "0.0)x5.00002\n..\\tenrm a\n.\\teni U\n.\\kern1.090"
        "26\n.\\mathoff\n\n! OK.\n[shift]> \\box0=\n\\hbox("
        "6.83331+0.0)x21.55133\n.\\mathon\n.\\teni U\n.\\ke"
        "rn1.09026\n.\\hbox(4.65277+0.0)x5.71527\n..\\hbox("
        "4.30554+0.0)x5.71527, shifted -0.34723\n...\\teni "
        "x\n.\\teni U\n.\\kern1.09026\n.\\mathoff\n\n! OK."
        "\n[pair]> \\box0=\n\\hbox(6.94444+0.0)x26.39165\n."
        "\\mathon\n.\\teni U\n.\\kern1.09026\n.\\hbox(6.944"
        "44+0.0)x10.55559\n..\\hbox(4.30554+0.0)x5.00002\n."
        "..\\tenrm a\n..\\hbox(6.94444+0.0)x5.55557\n...\\t"
        "enrm b\n.\\teni U\n.\\kern1.09026\n.\\mathoff\n\n!"
        " OK.\n[char]> \\box0=\n\\hbox(6.83331+0.0)x21.1219"
        "5\n.\\mathon\n.\\teni U\n.\\kern1.09026\n.\\teni a"
        "\n.\\teni U\n.\\kern1.09026\n.\\mathoff\n\n! OK.\n"
        "[sup]> \\box0=\n\\hbox(7.93446+0.0)x12.91805\n.\\m"
        "athon\n.\\teni U\n.\\kern1.09026\n.\\hbox(4.30554+"
        "0.0)x5.00002, shifted -3.62892\n..\\tenrm a\n.\\ma"
        "thoff\n\n! OK.\n"
);
}

/* \mathaccent centres a character over its nucleus, sliding it right by the
   nucleus's skew and dropping it onto the letter; see docs/DECISIONS.md,
   math-accents. */
static int test_math_accents(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 "
        "\\tracingonline=1 \\showboxdepth=10 \\showboxbread"
        "th=1000 \\hbadness=10000 \\vbadness=10000 \\hfuzz="
        "1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0p"
        "t \\boxmaxdepth=16383.99998pt \\baselineskip=12pt "
        "\\lineskip=0pt \\lineskiplimit=0pt \\parfillskip=0"
        "pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toler"
        "ance=10000 \\pretolerance=-1 \\spaceskip=4pt \\fon"
        "t\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm"
        "=cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\f"
        "ont\\fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\seve"
        "nsy=cmsy7 \\font\\fivesy=cmsy5 \\font\\tenex=cmex1"
        "0 \\textfont0=\\tenrm \\scriptfont0=\\sevenrm \\sc"
        "riptscriptfont0=\\fiverm \\textfont1=\\teni \\scri"
        "ptfont1=\\seveni \\scriptscriptfont1=\\fivei \\tex"
        "tfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscr"
        "iptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont"
        "3=\\tenex \\scriptscriptfont3=\\tenex \\skewchar\\"
        "teni=127 \\skewchar\\seveni=127 \\skewchar\\fivei="
        "127 \\skewchar\\tensy=48 \\skewchar\\sevensy=48 \\"
        "skewchar\\fivesy=48 \\tenrm \\message{[bar]}\\setb"
        "ox0=\\hbox{$\\mathaccent\"7016 U$}\\showbox0 \\mes"
        "sage{[hat]}\\setbox0=\\hbox{$\\mathaccent\"705E A$"
        "}\\showbox0 \\message{[wide]}\\setbox0=\\hbox{$\\m"
        "athaccent\"7016 {UU}$}\\showbox0 \\message{[low]}"
        "\\setbox0=\\hbox{$\\mathaccent\"7016 x$}\\showbox0"
        " \\message{[script]}\\setbox0=\\hbox{$U^{\\mathacc"
        "ent\"7016 y}$}\\showbox0 \\message{[over]}\\setbox"
        "0=\\hbox{$\\overline{U}$}\\showbox0%"
,
        "[bar]> \\box0=\n\\hbox(8.20554+0.0)x7.91803\n.\\ma"
        "thon\n.\\vbox(8.20554+0.0)x7.91803\n..\\hbox(5.677"
        "76+0.0)x0.0, shifted 1.7368\n...\\tenrm ^^V\n..\\k"
        "ern-4.30554\n..\\hbox(6.83331+0.0)x7.91803\n...\\t"
        "eni U\n.\\mathoff\n\n! OK.\n[hat]> \\box0=\n\\hbox"
        "(9.47221+0.0)x7.50002\n.\\mathon\n.\\vbox(9.47221+"
        "0.0)x7.50002\n..\\hbox(6.94444+0.0)x0.0, shifted 2"
        ".63893\n...\\tenrm ^\n..\\kern-4.30554\n..\\hbox(6"
        ".83331+0.0)x7.50002\n...\\teni A\n.\\mathoff\n\n! "
        "OK.\n[wide]> \\box0=\n\\hbox(8.20554+0.0)x15.83606"
        "\n.\\mathon\n.\\vbox(8.20554+0.0)x15.83606\n..\\hb"
        "ox(5.67776+0.0)x0.0, shifted 5.41803\n...\\tenrm ^"
        "^V\n..\\kern-4.30554\n..\\hbox(6.83331+0.0)x15.836"
        "06\n...\\teni U\n...\\kern1.09026\n...\\teni U\n.."
        ".\\kern1.09026\n.\\mathoff\n\n! OK.\n[low]> \\box0"
        "=\n\\hbox(5.67776+0.0)x5.71527\n.\\mathon\n.\\vbox"
        "(5.67776+0.0)x5.71527\n..\\hbox(5.67776+0.0)x0.0, "
        "shifted 0.63542\n...\\tenrm ^^V\n..\\kern-4.30554"
        "\n..\\hbox(4.30554+0.0)x5.71527\n...\\teni x\n.\\m"
        "athoff\n\n! OK.\n[script]> \\box0=\n\\hbox(7.64835"
        "+0.0)x12.22478\n.\\mathon\n.\\teni U\n.\\kern1.090"
        "26\n.\\vbox(4.01942+1.3611)x4.30675, shifted -3.62"
        "892\n..\\hbox(4.01942+0.0)x0.0, shifted 0.59087\n."
        "..\\sevenrm ^^V\n..\\kern-3.01389\n..\\hbox(3.0138"
        "9+1.3611)x4.30675\n...\\seveni y\n.\\mathoff\n\n! "
        "OK.\n[over]> \\box0=\n\\hbox(8.8332+0.0)x7.91803\n"
        ".\\mathon\n.\\vbox(8.8332+0.0)x7.91803\n..\\kern0."
        "39998\n..\\rule(0.39998+0.0)x*\n..\\kern1.19994\n."
        ".\\hbox(6.83331+0.0)x7.91803\n...\\teni U\n.\\math"
        "off\n\n! OK.\n"
);}

/* A large operator is centred on the axis only when it is one character of
   its own; \\log and its like stand on the baseline. An equation and its
   number are each marked as a display. See docs/DECISIONS.md,
   only-a-character-is-centred. */
static int test_only_a_character_is_centred(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\tracingonline=1 \\"
        "showboxdepth=3 \\showboxbreadth=60 \\hbadness=10000 \\"
        "vbadness=10000 \\hfuzz=1000pt \\vfuzz=1000pt \\hsize=2"
        "00pt \\parindent=0pt \\boxmaxdepth=16383.99998pt \\bas"
        "elineskip=12pt \\lineskip=0pt \\lineskiplimit=0pt \\pa"
        "rfillskip=0pt plus1fil \\leftskip=0pt \\rightskip=0pt "
        "\\tolerance=10000 \\pretolerance=-1 \\spaceskip=4pt \\"
        "font\\tenrm=cmr10 \\font\\sevenrm=cmr7 \\font\\fiverm="
        "cmr5 \\font\\teni=cmmi10 \\font\\seveni=cmmi7 \\font\\"
        "fivei=cmmi5 \\font\\tensy=cmsy10 \\font\\sevensy=cmsy7"
        " \\font\\fivesy=cmsy5 \\font\\tenex=cmex10 \\textfont0"
        "=\\tenrm \\scriptfont0=\\sevenrm \\scriptscriptfont0=\\"
        "fiverm \\textfont1=\\teni \\scriptfont1=\\seveni \\scr"
        "iptscriptfont1=\\fivei \\textfont2=\\tensy \\scriptfon"
        "t2=\\sevensy \\scriptscriptfont2=\\fivesy \\textfont3="
        "\\tenex \\scriptfont3=\\tenex \\scriptscriptfont3=\\te"
        "nex \\tenrm \\mathcode`\\+=\"202B \\thinmuskip=3mu \\m"
        "edmuskip=4mu plus 2mu minus 4mu \\thickmuskip=5mu plus"
        " 5mu \\abovedisplayskip=10pt plus2pt \\belowdisplayski"
        "p=11pt plus2pt \\abovedisplayshortskip=1pt plus3pt \\b"
        "elowdisplayshortskip=2pt plus3pt \\predisplaypenalty=1"
        "0000 \\postdisplaypenalty=0 \\widowpenalty=150 \\displ"
        "aywidowpenalty=50 \\clubpenalty=0 \\interlinepenalty=0"
        " \\message{[op]}\\setbox0=\\hbox{$\\mathop{\\tenrm log"
        "}$}\\showbox0 \\message{[ch]}\\setbox0=\\hbox{$\\matho"
        "p{x}$}\\showbox0 \\message{[eqno]}\\setbox0=\\vbox{\\n"
        "oindent aa $$x+y\\eqno z$$ bb\\par}\\showbox0%",
        "[op]> \\box0=\n\\hbox(6.94444+1.94444)x13.15627\n.\\ma"
        "thon\n.\\hbox(6.94444+1.94444)x13.15627\n..\\teni l\n."
        ".\\kern0.19678\n..\\teni o\n..\\teni g\n..\\kern0.3587"
        "8\n.\\mathoff\n\n! OK.\n[ch]> \\box0=\n\\hbox(4.65277+"
        "0.0)x5.71527\n.\\mathon\n.\\hbox(4.30554+0.0)x5.71527,"
        " shifted -0.34723\n..\\teni x\n.\\mathoff\n\n! OK.\n[e"
        "qno]> \\box0=\n\\vbox(31.30554+0.0)x200.0\n.\\hbox(4.3"
        "0554+0.0)x200.0, glue set 189.99997fil\n..\\tenrm a\n."
        ".\\tenrm a\n..\\penalty 10000\n..\\glue(\\parfillskip)"
        " 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n.\\penalt"
        "y 10000\n.\\glue(\\abovedisplayshortskip) 1.0 plus 3.0"
        "\n.\\glue(\\baselineskip) 6.16667\n.\\hbox(5.83333+1.9"
        "4444)x111.5995, shifted 88.4005\n..\\hbox(5.83333+1.94"
        "444)x23.199, display\n...\\teni x\n...\\glue(\\medmusk"
        "ip) 2.22217 plus 1.11108 minus 2.22217\n...\\tenrm +\n"
        "...\\glue(\\medmuskip) 2.22217 plus 1.11108 minus 2.22"
        "217\n...\\teni y\n...\\kern0.35878\n..\\kern83.3102\n."
        ".\\hbox(4.30554+0.0)x5.0903, display\n...\\teni z\n..."
        "\\kern0.4398\n.\\penalty 0\n.\\glue(\\belowdisplayshor"
        "tskip) 2.0 plus 3.0\n.\\glue(\\baselineskip) 3.11111\n"
        ".\\hbox(6.94444+0.0)x200.0, glue set 188.88885fil\n..\\"
        "tenrm b\n..\\tenrm b\n..\\penalty 10000\n..\\glue(\\pa"
        "rfillskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n"
        "\n! OK.\n");
}

/* A superscript is lifted to the first of the three parameters in display
   style, the second elsewhere, and the third in a cramped one; see
   docs/DECISIONS.md, superscripts-in-display-style. */
static int test_superscripts_in_display_style(void)
{
    return run_document(
        "\\catcode`\\$=3 \\catcode`\\\"=12 \\catcode`\\^=7 \\ca"
        "tcode`\\_=8 \\tracingonline=1 \\showboxdepth=3 \\showb"
        "oxbreadth=60 \\hbadness=10000 \\vbadness=10000 \\hfuzz"
        "=1000pt \\vfuzz=1000pt \\hsize=200pt \\parindent=0pt \\"
        "boxmaxdepth=16383.99998pt \\baselineskip=12pt \\linesk"
        "ip=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fil "
        "\\leftskip=0pt \\rightskip=0pt \\tolerance=10000 \\pre"
        "tolerance=-1 \\spaceskip=4pt \\font\\tenrm=cmr10 \\fon"
        "t\\sevenrm=cmr7 \\font\\fiverm=cmr5 \\font\\teni=cmmi1"
        "0 \\font\\seveni=cmmi7 \\font\\fivei=cmmi5 \\font\\ten"
        "sy=cmsy10 \\font\\sevensy=cmsy7 \\font\\fivesy=cmsy5 \\"
        "font\\tenex=cmex10 \\textfont0=\\tenrm \\scriptfont0=\\"
        "sevenrm \\scriptscriptfont0=\\fiverm \\textfont1=\\ten"
        "i \\scriptfont1=\\seveni \\scriptscriptfont1=\\fivei \\"
        "textfont2=\\tensy \\scriptfont2=\\sevensy \\scriptscri"
        "ptfont2=\\fivesy \\textfont3=\\tenex \\scriptfont3=\\t"
        "enex \\scriptscriptfont3=\\tenex \\tenrm \\mathcode`\\"
        "+=\"202B \\thinmuskip=3mu \\medmuskip=4mu plus 2mu min"
        "us 4mu \\thickmuskip=5mu plus 5mu \\abovedisplayskip=1"
        "0pt plus2pt \\belowdisplayskip=11pt plus2pt \\abovedis"
        "playshortskip=1pt plus3pt \\belowdisplayshortskip=2pt "
        "plus3pt \\predisplaypenalty=10000 \\postdisplaypenalty"
        "=0 \\widowpenalty=150 \\displaywidowpenalty=50 \\clubp"
        "enalty=0 \\interlinepenalty=0 \\message{[t]}\\setbox0="
        "\\hbox{$x_a^b$}\\showbox0 \\message{[d]}\\setbox0=\\hb"
        "ox{$\\displaystyle x_a^b$}\\showbox0 \\message{[c]}\\s"
        "etbox0=\\hbox{$x_a^{b^c}$}\\showbox0%",
        "[t]> \\box0=\n\\hbox(8.49002+2.47217)x10.05292\n.\\mat"
        "hon\n.\\teni x\n.\\vbox(10.96219+0.0)x4.33765, shifted"
        " 2.47217\n..\\hbox(4.8611+0.0)x3.51666\n...\\seveni b\n"
        "..\\kern3.0872\n..\\hbox(3.01389+0.0)x4.33765\n...\\se"
        "veni a\n.\\mathoff\n\n! OK.\n[d]> \\box0=\n\\hbox(8.99"
        "002+2.47217)x10.05292\n.\\mathon\n.\\teni x\n.\\vbox(1"
        "1.46219+0.0)x4.33765, shifted 2.47217\n..\\hbox(4.8611"
        "+0.0)x3.51666\n...\\seveni b\n..\\kern3.5872\n..\\hbox"
        "(3.01389+0.0)x4.33765\n...\\seveni a\n.\\mathoff\n\n! "
        "OK.\n[c]> \\box0=\n\\hbox(8.79948+2.47217)x12.47906\n."
        "\\mathon\n.\\teni x\n.\\vbox(11.27165+0.0)x6.7638, shi"
        "fted 2.47217\n..\\hbox(5.17056+0.0)x6.7638\n...\\seven"
        "i b\n...\\hbox(2.15277+0.0)x3.24713, shifted -3.01779 "
        "[]\n..\\kern3.0872\n..\\hbox(3.01389+0.0)x4.33765\n..."
        "\\seveni a\n.\\mathoff\n\n! OK.\n");
}

/* The search for the character that may stick out past a margin looks into
   a horizontal box and not into a vertical one; see docs/DECISIONS.md,
   character-protrusion. */
static int test_protrusion_into_boxes(void)
{
    return run_document(
        "\\catcode`\\{=1 \\catcode`\\}=2 \\tracingonline=1 \\sh"
        "owboxdepth=2 \\showboxbreadth=40 \\hbadness=10000 \\vb"
        "adness=10000 \\hfuzz=1000pt \\vfuzz=1000pt \\font\\f=c"
        "mr10 \\f \\hsize=100pt \\parindent=0pt \\baselineskip="
        "12pt \\lineskip=0pt \\lineskiplimit=0pt \\parfillskip="
        "0pt plus1fil \\leftskip=0pt \\rightskip=0pt \\toleranc"
        "e=10000 \\pretolerance=-1 \\boxmaxdepth=16383.99998pt "
        "\\spaceskip=4pt \\lpcode\\f`\\(=117 \\rpcode\\f`\\)=11"
        "7 \\lpcode\\f`\\A=200 \\rpcode\\f`\\B=300 \\pdfprotrud"
        "echars=2 \\message{[box]}\\setbox1=\\vbox{\\noindent\\"
        "hbox{(x)} aaa\\par}\\showbox1 \\message{[deep]}\\setbo"
        "x1=\\vbox{\\noindent\\hbox{\\hbox{A}x} aaa\\par}\\show"
        "box1 \\message{[vbox]}\\setbox1=\\vbox{\\noindent\\vbo"
        "x{\\hbox{A}}x aaa\\par}\\showbox1 \\message{[end]}\\se"
        "tbox1=\\vbox{\\noindent aaa \\hbox{(B)}\\par}\\showbox"
        "1%",
        "[box]> \\box1=\n\\vbox(7.5+2.5)x100.0\n.\\hbox(7.5+2.5"
        ")x100.0, glue set 69.11435fil\n..\\kern-1.17 (left mar"
        "gin)\n..\\hbox(7.5+2.5)x13.0556 []\n..\\glue(\\spacesk"
        "ip) 4.0\n..\\f a\n..\\f a\n..\\f a\n..\\penalty 10000\n"
        "..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\ri"
        "ghtskip) 0.0\n\n! OK.\n[deep]> \\box1=\n\\vbox(6.83331"
        "+0.0)x100.0\n.\\hbox(6.83331+0.0)x100.0, glue set 70.2"
        "2214fil\n..\\kern-2.0 (left margin)\n..\\hbox(6.83331+"
        "0.0)x12.77782 []\n..\\glue(\\spaceskip) 4.0\n..\\f a\n"
        "..\\f a\n..\\f a\n..\\penalty 10000\n..\\glue(\\parfil"
        "lskip) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n!"
        " OK.\n[vbox]> \\box1=\n\\vbox(6.83331+0.0)x100.0\n.\\h"
        "box(6.83331+0.0)x100.0, glue set 68.22214fil\n..\\vbox"
        "(6.83331+0.0)x7.50002 []\n..\\f x\n..\\glue(\\spaceski"
        "p) 4.0\n..\\f a\n..\\f a\n..\\f a\n..\\penalty 10000\n"
        "..\\glue(\\parfillskip) 0.0 plus 1.0fil\n..\\glue(\\ri"
        "ghtskip) 0.0\n\n! OK.\n[end]> \\box1=\n\\vbox(7.5+2.5)"
        "x100.0\n.\\hbox(7.5+2.5)x100.0, glue set 67.30879fil\n"
        "..\\f a\n..\\f a\n..\\f a\n..\\glue(\\spaceskip) 4.0\n"
        "..\\hbox(7.5+2.5)x14.86116 []\n..\\penalty 10000\n..\\"
        "kern-1.17 (right margin)\n..\\glue(\\parfillskip) 0.0 "
        "plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* \\/ adds nothing at all unless a character stands at the end of the list;
   see docs/DECISIONS.md, control-space-and-italic. */
static int test_italic_correction_needs_a_character(void)
{
    return run_document(
        "\\catcode`\\{=1 \\catcode`\\}=2 \\tracingonline=1 \\sh"
        "owboxdepth=3 \\showboxbreadth=30 \\hbadness=10000 \\hf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\message{[box]}\\setbo"
        "x0=\\hbox{a\\hbox{x}\\/b}\\showbox0 \\message{[chr]}\\"
        "setbox0=\\hbox{a\\/b}\\showbox0 \\message{[kern]}\\set"
        "box0=\\hbox{a\\kern1pt\\/b}\\showbox0 \\message{[glue]"
        "}\\setbox0=\\hbox{a\\hskip1pt\\/b}\\showbox0 \\message"
        "{[none]}\\setbox0=\\hbox{\\/b}\\showbox0%",
        "[box]> \\box0=\n\\hbox(6.94444+0.0)x15.83339\n.\\f a\n"
        ".\\hbox(4.30554+0.0)x5.2778\n..\\f x\n.\\f b\n\n! OK.\n"
        "[chr]> \\box0=\n\\hbox(6.94444+0.0)x10.55559\n.\\f a\n"
        ".\\kern 0.0\n.\\f b\n\n! OK.\n[kern]> \\box0=\n\\hbox("
        "6.94444+0.0)x11.55559\n.\\f a\n.\\kern 1.0\n.\\f b\n\n"
        "! OK.\n[glue]> \\box0=\n\\hbox(6.94444+0.0)x11.55559\n"
        ".\\f a\n.\\glue 1.0\n.\\f b\n\n! OK.\n[none]> \\box0=\n"
        "\\hbox(6.94444+0.0)x5.55557\n.\\f b\n\n! OK.\n");
}

/* A break that falls inside a ligature replaces it: the two halves of the
   word are set again around the hyphen, and the ligature stands as the text
   used when the line does not break. See docs/DECISIONS.md,
   breaking-inside-a-ligature. */
static int test_breaking_inside_a_ligature(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=20"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\v"
        "fuzz=1000pt \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\h"
        "size=300pt \\parindent=0pt \\leftskip=0pt \\rightskip="
        "0pt \\pdfprotrudechars=0 \\baselineskip=12pt \\lineski"
        "p=0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fil \\"
        "tolerance=10000 \\pretolerance=-1 \\boxmaxdepth=16383."
        "99998pt \\clubpenalty=0 \\widowpenalty=0 \\interlinepe"
        "nalty=0 \\brokenpenalty=0 \\uchyph=0 \\lefthyphenmin=1"
        " \\righthyphenmin=1 \\spaceskip=4pt \\sfcode`\\.=1000 "
        "\\lccode`\\c=`\\c \\lccode`\\o=`\\o \\lccode`\\e=`\\e "
        "\\lccode`\\f=`\\f \\lccode`\\i=`\\i \\lccode`\\n=`\\n "
        "\\lccode`\\t=`\\t \\lccode`\\x=`\\x \\lccode`\\a=`\\a "
        "\\lccode`\\l=`\\l \\patterns{o1e f1f i1c 1x1 f1l a1f} "
        "\\message{[ffi]}\\setbox0=\\vbox{\\noindent xx coeffic"
        "ient\\par}\\showbox0 \\message{[ff]}\\setbox0=\\vbox{\\"
        "noindent xx affable\\par}\\showbox0%",
        "[ffi]> \\box0=\n\\vbox(6.94444+0.0)x300.0\n.\\hbox(6.9"
        "4444+0.0)x300.0, glue set 242.111fil\n..\\f x\n..\\f x"
        "\n..\\glue(\\spaceskip) 4.0\n..\\f c\n..\\discretionar"
        "y replacing 2\n...\\f o\n...\\f -\n..\\f o\n..\\kern0."
        "27779\n..\\f e\n..\\discretionary replacing 1\n...\\f "
        "f\n...\\f -\n..|\\f ^^L (ligature fi)\n..\\f ^^N (liga"
        "ture ffi)\n..\\discretionary\n...\\f -\n..\\f c\n..\\f"
        " i\n..\\f e\n..\\f n\n..\\kern-0.27779\n..\\f t\n..\\p"
        "enalty 10000\n..\\glue(\\parfillskip) 0.0 plus 1.0fil\n"
        "..\\glue(\\rightskip) 0.0\n\n! OK.\n[ff]> \\box0=\n\\v"
        "box(6.94444+0.0)x300.0\n.\\hbox(6.94444+0.0)x300.0, gl"
        "ue set 256.8332fil\n..\\f x\n..\\f x\n..\\glue(\\space"
        "skip) 4.0\n..\\f a\n..\\discretionary\n...\\f -\n..\\d"
        "iscretionary replacing 1\n...\\f f\n...\\f -\n..|\\f f"
        "\n..\\f ^^K (ligature ff)\n..\\f a\n..\\f b\n..\\f l\n"
        "..\\f e\n..\\penalty 10000\n..\\glue(\\parfillskip) 0."
        "0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* An accent rides between two kerns of a kind of their own, worked out in
   one figure and rounded once; see docs/DECISIONS.md, accent-kerns. */
static int test_accent_kerns(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=3 \\showboxbreadth=30"
        " \\hbadness=10000 \\hfuzz=1000pt \\font\\f=cmr10 \\f \\"
        "message{[a]}\\setbox0=\\hbox{\\accent19 e}\\showbox0 \\"
        "message{[b]}\\setbox0=\\hbox{\\accent23 o}\\showbox0 \\"
        "message{[c]}\\setbox0=\\hbox{x\\accent19 ey}\\showbox0"
        " \\font\\i=cmti10 \\font\\s=cmsl10 \\i \\message{[a]}\\"
        "setbox0=\\hbox{\\accent19 e}\\showbox0 \\message{[b]}\\"
        "setbox0=\\hbox{\\accent23 o}\\showbox0 \\message{[c]}\\"
        "setbox0=\\hbox{\\accent19 A}\\showbox0 \\s \\message{["
        "d]}\\setbox0=\\hbox{\\accent19 e}\\showbox0 \\message{"
        "[e]}\\setbox0=\\hbox{\\accent127 A}\\showbox0 \\messag"
        "e{[f \\the\\fontdimen1\\i|\\the\\fontdimen5\\i]}%",
        "[a]> \\box0=\n\\hbox(6.94444+0.0)x4.44444\n.\\kern -0."
        "27779 (for accent)\n.\\f ^^S\n.\\kern -4.72223 (for ac"
        "cent)\n.\\f e\n\n! OK.\n[b]> \\box0=\n\\hbox(6.94444+0"
        ".0)x5.00002\n.\\kern -1.25 (for accent)\n.\\f ^^W\n.\\"
        "kern -6.25002 (for accent)\n.\\f o\n\n! OK.\n[c]> \\bo"
        "x0=\n\\hbox(6.94444+1.94444)x15.00005\n.\\f x\n.\\kern"
        " -0.27779 (for accent)\n.\\f ^^S\n.\\kern -4.72223 (fo"
        "r accent)\n.\\f e\n.\\f y\n\n! OK.\n[a]> \\box0=\n\\hb"
        "ox(6.94444+0.0)x4.59996\n.\\kern -0.25557 (for accent)"
        "\n.\\i ^^S\n.\\kern -4.85551 (for accent)\n.\\i e\n\n!"
        " OK.\n[b]> \\box0=\n\\hbox(6.94444+0.0)x5.11108\n.\\ke"
        "rn -1.60092 (for accent)\n.\\i ^^W\n.\\kern -6.71199 ("
        "for accent)\n.\\i o\n\n! OK.\n[c]> \\box0=\n\\hbox(9.4"
        "7221+0.0)x7.43329\n.\\kern 1.79305 (for accent)\n.\\hb"
        "ox(6.94444+0.0)x5.11108, shifted -2.52777\n..\\i ^^S\n"
        ".\\kern -6.90413 (for accent)\n.\\i A\n\n! OK.\n[d]> \\"
        "box0=\n\\hbox(6.94444+0.0)x4.44444\n.\\kern -0.27779 ("
        "for accent)\n.\\s ^^S\n.\\kern -4.72223 (for accent)\n"
        ".\\s e\n\n! OK.\n[e]> \\box0=\n\\hbox(9.20636+0.0)x7.5"
        "0002\n.\\kern 1.67131 (for accent)\n.\\hbox(6.67859+0."
        "0)x5.00002, shifted -2.52777\n..\\s ^^?\n.\\kern -6.67"
        "133 (for accent)\n.\\s A\n\n! OK.\n[f 0.25pt|4.30554pt"
        "]");
}

/* A negative protrusion code is truncated where a positive one is rounded;
   see docs/DECISIONS.md, character-protrusion. */
static int test_protrusion_of_a_negative_code(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=40"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\hsize=40pt \\parinden"
        "t=0pt \\baselineskip=12pt \\lineskip=0pt \\lineskiplim"
        "it=0pt \\parfillskip=0pt plus1fil \\leftskip=0pt \\rig"
        "htskip=0pt \\tolerance=10000 \\pretolerance=-1 \\boxma"
        "xdepth=16383.99998pt \\spaceskip=4pt \\pdfprotrudechar"
        "s=1 \\lpcode\\f`\\A=-2 \\rpcode\\f`\\B=-2 \\lpcode\\f`"
        "\\C=2 \\rpcode\\f`\\D=2 \\message{[neg]}\\setbox1=\\vb"
        "ox{\\noindent AB\\par}\\showbox1 \\message{[pos]}\\set"
        "box1=\\vbox{\\noindent CD\\par}\\showbox1%",
        "[neg]> \\box1=\n\\vbox(6.83331+0.0)x40.0\n.\\hbox(6.83"
        "331+0.0)x40.0, glue set 25.37665fil\n..\\kern0.01999 ("
        "left margin)\n..\\f A\n..\\f B\n..\\penalty 10000\n..\\"
        "kern0.01999 (right margin)\n..\\glue(\\parfillskip) 0."
        "0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n[po"
        "s]> \\box1=\n\\vbox(6.83331+0.0)x40.0\n.\\hbox(6.83331"
        "+0.0)x40.0, glue set 25.17888fil\n..\\kern-0.02 (left "
        "margin)\n..\\f C\n..\\f D\n..\\penalty 10000\n..\\kern"
        "-0.02 (right margin)\n..\\glue(\\parfillskip) 0.0 plus"
        " 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* \\pdfcolorstack leaves a node behind saying what it did; see
   docs/DECISIONS.md, colour-stack-nodes. */
static int test_colour_stack_nodes(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=3 \\showboxbreadth=30"
        " \\hbadness=10000 \\hfuzz=1000pt \\font\\f=cmr10 \\f \\"
        "pdfoutput=1 \\pdfcolorstackinit page direct {0 g 0 G} "
        "\\message{[a]}\\setbox0=\\hbox{a\\pdfcolorstack0 push "
        "{1 0 0 rg}b\\pdfcolorstack0 pop c}\\showbox0 \\message"
        "{[b]}\\setbox0=\\hbox{a\\pdfcolorstack0 set {0 g}b}\\s"
        "howbox0 \\message{[c]}\\setbox0=\\hbox{a\\pdfcolorstac"
        "k0 current b}\\showbox0 \\message{[t \\the\\lastnodety"
        "pe]}%",
        "[a]> \\box0=\n\\hbox(6.94444+0.0)x18.33336\n.\\f a\n.\\"
        "pdfcolorstack 0 push {1 0 0 rg}\n.\\f b\n.\\pdfcolorst"
        "ack 0 pop\n.\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        ".\\f c\n\n! OK.\n[b]> \\box0=\n\\hbox(6.94444+0.0)x10."
        "55559\n.\\f a\n.\\pdfcolorstack 0 set {0 g}\n.\\f b\n\n"
        "! OK.\n[c]> \\box0=\n\\hbox(6.94444+0.0)x13.88892\n.\\"
        "f a\n.\\pdfcolorstack 0 current\n.\\glue 3.33333 plus "
        "1.66666 minus 1.11111\n.\\f b\n\n! OK.\n[t 11]");
}

/* The forced break that ends a paragraph counts as a hyphenated one, so a
   hyphen on the line before it costs \\finalhyphendemerits; see
   docs/DECISIONS.md, the-final-break-is-hyphenated. */
static int test_the_final_break_is_hyphenated(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=1 \\showboxbreadth=40"
        " \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\vf"
        "uzz=1000pt \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\hs"
        "ize=28pt \\parindent=0pt \\leftskip=0pt \\rightskip=0p"
        "t \\pdfprotrudechars=0 \\baselineskip=12pt \\lineskip="
        "0pt \\lineskiplimit=0pt \\parfillskip=0pt plus1fil \\t"
        "olerance=10000 \\pretolerance=-1 \\boxmaxdepth=16383.9"
        "9998pt \\clubpenalty=0 \\widowpenalty=0 \\interlinepen"
        "alty=0 \\brokenpenalty=0 \\uchyph=0 \\lefthyphenmin=1 "
        "\\righthyphenmin=1 \\spaceskip=4pt plus2pt minus1pt \\"
        "linepenalty=10 \\adjdemerits=0 \\doublehyphendemerits="
        "0 \\hyphenpenalty=50 \\lccode`\\a=`\\a \\lccode`\\x=`\\"
        "x \\patterns{a1a 1x1} \\finalhyphendemerits=0 \\messag"
        "e{[none]}\\setbox0=\\vbox{\\noindent xx aaaaaa aaaa\\p"
        "ar}\\showbox0 \\finalhyphendemerits=-1000000 \\message"
        "{[some]}\\setbox0=\\vbox{\\noindent xx aaaaaa aaaa\\pa"
        "r}\\showbox0%",
        "[none]> \\box0=\n\\vbox(28.30554+0.0)x28.0\n.\\hbox(4."
        "30554+0.0)x28.0, glue set 0.05553 []\n.\\glue(\\baseli"
        "neskip) 7.69446\n.\\hbox(4.30554+0.0)x28.0 []\n.\\glue"
        "(\\baselineskip) 7.69446\n.\\hbox(4.30554+0.0)x28.0, g"
        "lue set 7.99994fil []\n\n! OK.\n[some]> \\box0=\n\\vbo"
        "x(40.30554+0.0)x28.0\n.\\hbox(4.30554+0.0)x28.0, glue "
        "set 0.05553 []\n.\\glue(\\baselineskip) 7.69446\n.\\hb"
        "ox(4.30554+0.0)x28.0 []\n.\\glue(\\baselineskip) 7.694"
        "46\n.\\hbox(4.30554+0.0)x28.0, glue set 0.33331 []\n.\\"
        "glue(\\baselineskip) 7.69446\n.\\hbox(4.30554+0.0)x28."
        "0, glue set 22.99998fil []\n\n! OK.\n");
}

/* \patterns put discretionaries in words the second pass reads out of the
   paragraph; see docs/DECISIONS.md, hyphenation. */
static int test_hyphenation(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=20"
        "0 \\hbadness=10000 \\vbadness=10000 \\hfuzz=1000pt \\v"
        "fuzz=1000pt \\font\\f=cmr10 \\f \\hyphenchar\\f=45 \\h"
        "size=200pt \\parindent=0pt \\leftskip=0pt \\rightskip="
        "0pt \\baselineskip=12pt \\lineskip=0pt \\lineskiplimit"
        "=0pt \\parfillskip=0pt plus1fil \\tolerance=10000 \\pr"
        "etolerance=-1 \\boxmaxdepth=16383.99998pt \\linepenalt"
        "y=10 \\adjdemerits=10000 \\doublehyphendemerits=10000 "
        "\\finalhyphendemerits=5000 \\clubpenalty=0 \\widowpena"
        "lty=0 \\interlinepenalty=0 \\brokenpenalty=0 \\hyphenp"
        "enalty=50 \\exhyphenpenalty=50 \\uchyph=0 \\lefthyphen"
        "min=2 \\righthyphenmin=3 \\spaceskip=4pt \\sfcode`\\.="
        "1000 \\lccode`\\a=`\\a \\lccode`\\b=`\\b \\lccode`\\c="
        "`\\c \\lccode`\\d=`\\d \\lccode`\\e=`\\e \\lccode`\\f="
        "`\\f \\lccode`\\g=`\\g \\lccode`\\h=`\\h \\lccode`\\i="
        "`\\i \\lccode`\\j=`\\j \\lccode`\\k=`\\k \\lccode`\\l="
        "`\\l \\lccode`\\m=`\\m \\lccode`\\n=`\\n \\lccode`\\o="
        "`\\o \\lccode`\\p=`\\p \\lccode`\\q=`\\q \\lccode`\\r="
        "`\\r \\lccode`\\s=`\\s \\lccode`\\t=`\\t \\lccode`\\u="
        "`\\u \\lccode`\\v=`\\v \\lccode`\\w=`\\w \\lccode`\\x="
        "`\\x \\lccode`\\y=`\\y \\lccode`\\z=`\\z \\lccode`\\A="
        "`\\a \\lccode`\\H=`\\h \\patterns{2n3t 1hy 3phe a1t io"
        "1n an1t} \\setbox0=\\vbox{\\noindent xx sentant hyphen"
        "ation Antant\\par}\\showbox0 \\uchyph=1 \\setbox0=\\vb"
        "ox{\\noindent xx Antant\\par}\\showbox0 \\lefthyphenmi"
        "n=4 \\setbox0=\\vbox{\\noindent xx sentant\\par}\\show"
        "box0%",
        "> \\box0=\n\\vbox(6.94444+1.94444)x200.0\n.\\hbox(6.94"
        "444+1.94444)x200.0, glue set 60.99968fil\n..\\f x\n..\\"
        "f x\n..\\glue(\\spaceskip) 4.0\n..\\f s\n..\\f e\n..\\"
        "discretionary replacing 2\n...\\f n\n...\\f -\n..\\f n"
        "\n..\\kern-0.27779\n..\\f t\n..\\f a\n..\\f n\n..\\ker"
        "n-0.27779\n..\\f t\n..\\glue(\\spaceskip) 4.0\n..\\f h"
        "\n..\\kern-0.27779\n..\\f y\n..\\discretionary\n...\\f"
        " -\n..\\f p\n..\\f h\n..\\f e\n..\\f n\n..\\f a\n..\\d"
        "iscretionary\n...\\f -\n..\\f t\n..\\f i\n..\\f o\n..\\"
        "f n\n..\\glue(\\spaceskip) 4.0\n..\\f A\n..\\f n\n..\\"
        "kern-0.27779\n..\\f t\n..\\f a\n..\\f n\n..\\kern-0.27"
        "779\n..\\f t\n..\\penalty 10000\n..\\glue(\\parfillski"
        "p) 0.0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK."
        "\n> \\box0=\n\\vbox(6.83331+0.0)x200.0\n.\\hbox(6.8333"
        "1+0.0)x200.0, glue set 154.611fil\n..\\f x\n..\\f x\n."
        ".\\glue(\\spaceskip) 4.0\n..\\f A\n..\\discretionary r"
        "eplacing 2\n...\\f n\n...\\f -\n..\\f n\n..\\kern-0.27"
        "779\n..\\f t\n..\\f a\n..\\f n\n..\\kern-0.27779\n..\\"
        "f t\n..\\penalty 10000\n..\\glue(\\parfillskip) 0.0 pl"
        "us 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n> \\box"
        "0=\n\\vbox(6.15079+0.0)x200.0\n.\\hbox(6.15079+0.0)x20"
        "0.0, glue set 153.72212fil\n..\\f x\n..\\f x\n..\\glue"
        "(\\spaceskip) 4.0\n..\\f s\n..\\f e\n..\\f n\n..\\kern"
        "-0.27779\n..\\f t\n..\\f a\n..\\f n\n..\\kern-0.27779\n"
        "..\\f t\n..\\penalty 10000\n..\\glue(\\parfillskip) 0."
        "0 plus 1.0fil\n..\\glue(\\rightskip) 0.0\n\n! OK.\n");
}

/* \spaceskip and \xspaceskip take the place of the font's own space, and
   only a space the space factor has not touched keeps the name of the
   parameter it came from; see docs/DECISIONS.md, interword-glue. */
static int test_interword_glue(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=30"
        " \\hbadness=10000 \\hfuzz=1000pt \\font\\f=cmr10 \\f \\"
        "sfcode`\\.=3000 \\sfcode`\\,=1250 \\spaceskip=4pt plus"
        "1pt minus2pt \\xspaceskip=9pt plus3pt \\setbox0=\\hbox"
        "{a a}\\showbox0 \\setbox0=\\hbox{a. a}\\showbox0 \\set"
        "box0=\\hbox{a, a}\\showbox0 \\xspaceskip=0pt \\setbox0"
        "=\\hbox{a. a}\\showbox0 \\spaceskip=0pt \\setbox0=\\hb"
        "ox{a a}\\showbox0 \\setbox0=\\hbox{a. a}\\showbox0%",
        "> \\box0=\n\\hbox(4.30554+0.0)x14.00003\n.\\f a\n.\\gl"
        "ue(\\spaceskip) 4.0 plus 1.0 minus 2.0\n.\\f a\n\n! OK"
        ".\n> \\box0=\n\\hbox(4.30554+0.0)x21.77782\n.\\f a\n.\\"
        "f .\n.\\glue(\\xspaceskip) 9.0 plus 3.0\n.\\f a\n\n! O"
        "K.\n> \\box0=\n\\hbox(4.30554+1.94444)x16.77782\n.\\f "
        "a\n.\\f ,\n.\\glue 4.0 plus 1.25 minus 1.59999\n.\\f a"
        "\n\n! OK.\n> \\box0=\n\\hbox(4.30554+0.0)x17.88893\n.\\"
        "f a\n.\\f .\n.\\glue 5.11111 plus 3.0 minus 0.66666\n."
        "\\f a\n\n! OK.\n> \\box0=\n\\hbox(4.30554+0.0)x13.3333"
        "6\n.\\f a\n.\\glue 3.33333 plus 1.66666 minus 1.11111\n"
        ".\\f a\n\n! OK.\n> \\box0=\n\\hbox(4.30554+0.0)x17.222"
        "26\n.\\f a\n.\\f .\n.\\glue 4.44444 plus 4.99997 minus"
        " 0.37036\n.\\f a\n\n! OK.\n");
}

/* The current font is restored on the way out of a group, and the character
   held back for the ligature program is taken in before that happens. See
   docs/DECISIONS.md, the-current-font-is-grouped. */
static int test_the_current_font_is_grouped(void)
{
    return run_document(
        "\\tracingonline=1 \\showboxdepth=3 \\showboxbreadth=20 "
        "\\hbadness=10000 \\font\\f=cmr10 \\font\\g=cmr10 at 20pt \\f "
        "\\setbox0=\\hbox{fi}\\showbox0 "
        "\\setbox0=\\hbox{f{}i}\\showbox0 "
        "\\setbox0=\\hbox{A{\\g A}A}\\showbox0 "
        "\\setbox0=\\hbox{{\\g A}}\\showbox0 "
        "\\dimen0=1em \\message{[em \\the\\dimen0]}"
        "{\\g }\\dimen0=1em \\message{[em \\the\\dimen0]}"
        "{\\global\\g}\\dimen0=1em \\message{[em \\the\\dimen0]}%",
        "> \\box0=\n"
        "\\hbox(6.94444+0.0)x5.55557\n"
        ".\\f ^^L (ligature fi)\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(6.94444+0.0)x5.83336\n"
        ".\\f f\n"
        ".\\f i\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(13.66664+0.0)x30.00006\n"
        ".\\f A\n"
        ".\\g A\n"
        ".\\f A\n"
        "\n"
        "! OK.\n"
        "> \\box0=\n"
        "\\hbox(13.66664+0.0)x15.00003\n"
        ".\\g A\n"
        "\n"
        "! OK.\n"
        "[em 10.00002pt][em 10.00002pt][em 20.00005pt]");
}

/* Glue, kerns and penalties in front of the first box of a page are thrown
   away, and a whatsit is not: it joins the page and leaves it empty. See
   docs/DECISIONS.md, whatsits-on-an-empty-page. */
static int test_whatsits_on_an_empty_page(void)
{
    return run_document(
        "\\vsize=100pt \\topskip=0pt \\maxdepth=0pt \\hsize=100pt "
        "\\tracingonline=1 \\showboxdepth=5 \\showboxbreadth=10 "
        "\\output={\\message{[FIRE \\the\\outputpenalty]}\\showbox255 "
        "\\shipout\\box255 }"
        "\\message{[A]}\\penalty-10000 "
        "\\message{[B]}\\vskip5pt \\penalty-10000 "
        "\\message{[C]}\\kern7pt \\penalty-10000 "
        "\\message{[D]}\\write-1{w}\\message{[\\the\\pagegoal]}"
        "\\hbox{}\\penalty-10000 "
        "\\message{[E]}%",
        "[A][B][C][D][16383.99998pt][FIRE -10000]> \\box255=\n"
        "\\vbox(100.0+0.0)x0.0\n"
        ".\\write-{w}\n"
        ".\\glue(\\topskip) 0.0\n"
        ".\\hbox(0.0+0.0)x0.0\n"
        "\n"
        "! OK.\n"
        "[E]");
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
        run_snippet("\\def\\hash#1{##1:#1}\\hash Z%", "#1:Z") != 0 ||
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
        run_snippet("\\edef\\saved{\\unexpanded{#1}}\\saved/"
                    "\\toks0={#2}\\edef\\fromtoks{\\the\\toks0}"
                    "\\fromtoks%",
                    "#1/#2") != 0 ||
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
        expect_failure("\\unknown%",
                       "undefined control sequence: \\unknown") != 0 ||
        expect_failure("\\ifcat\\missing\\relax T\\else F\\fi%",
                       "undefined control sequence: \\missing") != 0 ||
        expect_failure("\\expanded{\\number}%", "missing integer") != 0 ||
        expect_failure("\\baselineskip=1\\relax%",
                       "illegal unit of measure") != 0 ||
        expect_failure("\\skip1=1pt plus2fillll%",
                       "infinite glue order beyond filll") != 0 ||
        run_snippet("\\dimen0=1ptpt\\dimen1=1inch%", "ptch") != 0 ||
        expect_failure("\\def\\why{expanded}"
                       "\\errmessage{ERRMESSAGE: \\why}%",
                       "ERRMESSAGE: expanded") != 0 ||
        expect_failure("\\def\\a#1{X}\\a{one\n\n two}%",
                       "non-long macro argument") != 0 ||
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
        run_snippet("\\def\\s#1{##1#1}[\\s{Q}][\\meaning\\s]%",
                    "[#1Q][macro:#1->##1#1]") != 0 ||
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
        test_oversize_boxes() != 0 || test_page_totals() != 0 || test_output_routine() != 0 || test_vsplit() != 0 || test_glue_set() != 0 || test_showbox() != 0 ||
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
