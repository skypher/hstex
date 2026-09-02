/* Round-trips the source stack through hstex_source_serialize /
   hstex_source_deserialize under several checkpoint shapes: read partway into
   nested files (mid-line, between lines, and across ^^ notation) with and
   without a token frame on top, write the position to a temporary file, read
   it back into a fresh stack, and check that the continuation of the restored
   stack matches the continuation of the original token for token to the end. */
#include "hstex/lex.h"
#include "hstex/source.h"
#include "hstex/token.h"
#include "test_cli.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_file(const char *path, const char *content, size_t length)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    int ok = length == 0U || fwrite(content, 1U, length, f) == length;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

struct step {
    enum hstex_mouth_result result;
    hstex_token token;
    uint32_t line;
    uint32_t column;
};

#define CAP 512

static size_t drain(struct hstex_source_stack *stack, struct step *steps)
{
    size_t count = 0U;
    char error[256];
    for (;;) {
        struct step s;
        struct hstex_source_location location = {0};
        s.result = hstex_source_next(stack, &s.token, &location, error,
                                     sizeof(error));
        s.line = location.line;
        s.column = location.column;
        if (count < CAP) {
            steps[count] = s;
        }
        ++count;
        if (s.result != HSTEX_MOUTH_TOKEN) {
            break;
        }
    }
    return count;
}

/* One scenario: two files (inner \input'd after `read_outer` tokens of outer),
   `read_inner` tokens read from inner, and, when `tokens` is non-zero, a token
   frame pushed on top with one token consumed. Checkpoint, restore, compare. */
static int scenario(const char *name, struct hstex_lexical_state *lex,
                    const char *outer, size_t outer_len, const char *inner,
                    size_t inner_len, int read_outer, int read_inner,
                    int tokens)
{
    char outer_path[] = "/tmp/hstex-ser-o-XXXXXX";
    char inner_path[] = "/tmp/hstex-ser-i-XXXXXX";
    char chk_path[] = "/tmp/hstex-ser-c-XXXXXX";
    int a = mkstemp(outer_path), b = mkstemp(inner_path), c = mkstemp(chk_path);
    if (a < 0 || b < 0 || c < 0) {
        (void)fprintf(stderr, "%s: mkstemp failed\n", name);
        return 1;
    }
    (void)close(a);
    (void)close(b);
    (void)close(c);
    if (write_file(outer_path, outer, outer_len) != 0 ||
        write_file(inner_path, inner, inner_len) != 0) {
        (void)fprintf(stderr, "%s: write failed\n", name);
        return 1;
    }

    char error[256] = {0};
    hstex_token token;
    struct hstex_source_location where;
    struct hstex_source_stack stack;
    hstex_source_stack_init(&stack, lex);
    if (hstex_source_push_file(&stack, outer_path, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", name, error);
        return 1;
    }
    for (int i = 0; i < read_outer; ++i) {
        (void)hstex_source_next(&stack, &token, &where, error, sizeof(error));
    }
    if (inner_len != 0U) {
        if (hstex_source_push_file(&stack, inner_path, error, sizeof(error)) !=
            0) {
            (void)fprintf(stderr, "%s: %s\n", name, error);
            return 1;
        }
        for (int i = 0; i < read_inner; ++i) {
            (void)hstex_source_next(&stack, &token, &where, error,
                                    sizeof(error));
        }
    }
    if (tokens) {
        hstex_token inserted[] = {
            hstex_token_character((uint8_t)HSTEX_CAT_OTHER, (uint8_t)'Z'),
            hstex_token_character((uint8_t)HSTEX_CAT_LETTER, (uint8_t)'q'),
            hstex_token_character((uint8_t)HSTEX_CAT_OTHER, (uint8_t)'7'),
        };
        struct hstex_source_location origin = {42U, 3U};
        if (hstex_source_push_tokens(&stack, inserted, 3U, origin, error,
                                     sizeof(error)) != 0) {
            (void)fprintf(stderr, "%s: %s\n", name, error);
            return 1;
        }
        (void)hstex_source_next(&stack, &token, &where, error, sizeof(error));
    }

    FILE *chk = fopen(chk_path, "wb");
    if (chk == NULL || hstex_source_serialize(&stack, chk) != 0) {
        (void)fprintf(stderr, "%s: serialize failed\n", name);
        return 1;
    }
    (void)fclose(chk);

    static struct step original[CAP];
    size_t n_original = drain(&stack, original);
    hstex_source_stack_destroy(&stack);

    struct hstex_source_stack restored;
    hstex_source_stack_init(&restored, lex);
    chk = fopen(chk_path, "rb");
    if (chk == NULL ||
        hstex_source_deserialize(&restored, chk, lex, error, sizeof(error)) !=
            0) {
        (void)fprintf(stderr, "%s: deserialize failed: %s\n", name, error);
        return 1;
    }
    (void)fclose(chk);
    static struct step got[CAP];
    size_t n_restored = drain(&restored, got);
    hstex_source_stack_destroy(&restored);

    (void)unlink(outer_path);
    (void)unlink(inner_path);
    (void)unlink(chk_path);

    if (n_original != n_restored) {
        (void)fprintf(stderr, "%s: length differs original %zu restored %zu\n",
                      name, n_original, n_restored);
        return 1;
    }
    for (size_t i = 0U; i < n_original && i < CAP; ++i) {
        if (original[i].result != got[i].result ||
            original[i].token != got[i].token ||
            original[i].line != got[i].line ||
            original[i].column != got[i].column) {
            (void)fprintf(stderr,
                          "%s: step %zu differs orig(r=%d t=%u l=%u c=%u) "
                          "restored(r=%d t=%u l=%u c=%u)\n",
                          name, i, (int)original[i].result, original[i].token,
                          original[i].line, original[i].column,
                          (int)got[i].result, got[i].token, got[i].line,
                          got[i].column);
            return 1;
        }
    }
    (void)printf("  %-22s %zu steps matched\n", name, n_original);
    return 0;
}

int main(int argument_count, char **arguments)
{
    int option = hstex_test_arguments(
        argument_count, arguments, "Round-trip the source stack serializer.");
    if (option >= 0) {
        return option;
    }

    struct hstex_lexical_state lex;
    char error[256] = {0};
    if (hstex_lexical_state_init(&lex, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        return 1;
    }

    static const char outer[] = "alpha beta gamma\nsecond line here\n\\cs done\n";
    static const char inner[] = "inner one\ninner two\n";
    /* ^^ notation: `^^M' collapses in the mouth's own line buffer, so a
       checkpoint mid-way must carry the rewritten buffer, not the raw file. */
    static const char caret[] = "a^^41b ^^4a\nnext ^^4c line\n";
    int failures = 0;
    /* nested files, mid-line each, with a token frame on top */
    failures += scenario("nested+tokens", &lex, outer, sizeof(outer) - 1U,
                         inner, sizeof(inner) - 1U, 4, 3, 1);
    /* same, no token frame */
    failures += scenario("nested", &lex, outer, sizeof(outer) - 1U, inner,
                         sizeof(inner) - 1U, 5, 2, 0);
    /* single file, deep into a later line */
    failures += scenario("single-midline", &lex, outer, sizeof(outer) - 1U, "",
                         0U, 9, 0, 0);
    /* single file, only a couple of tokens read (still on line one) */
    failures += scenario("single-early", &lex, outer, sizeof(outer) - 1U, "", 0U,
                         2, 0, 0);
    /* nothing read yet -- checkpoint at the very start */
    failures += scenario("start", &lex, outer, sizeof(outer) - 1U, "", 0U, 0, 0,
                         0);
    /* caret notation, mid-line, so the collapsed line buffer must round-trip */
    failures += scenario("caret-midline", &lex, caret, sizeof(caret) - 1U, "",
                         0U, 3, 0, 1);
    /* token frame over a caret file */
    failures += scenario("caret+tokens", &lex, caret, sizeof(caret) - 1U, inner,
                         sizeof(inner) - 1U, 2, 4, 1);

    hstex_lexical_state_destroy(&lex);
    if (failures != 0) {
        (void)fprintf(stderr, "%d scenario(s) failed\n", failures);
        return 1;
    }
    (void)printf("source serializer: all scenarios round-tripped\n");
    return 0;
}
