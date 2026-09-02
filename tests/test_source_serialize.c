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
/* Serialize `stack` to a fresh temp file and read it back into `out`. */
static int roundtrip(struct hstex_source_stack *stack,
                     struct hstex_source_stack *out,
                     struct hstex_lexical_state *lex, const char *name)
{
    char path[] = "/tmp/hstex-ser-rt-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    (void)close(fd);
    char error[256] = {0};
    FILE *f = fopen(path, "wb");
    if (f == NULL || hstex_source_serialize(stack, f) != 0) {
        (void)fprintf(stderr, "%s: serialize failed\n", name);
        return 1;
    }
    (void)fclose(f);
    hstex_source_stack_init(out, lex);
    f = fopen(path, "rb");
    if (f == NULL ||
        hstex_source_deserialize(out, f, lex, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: deserialize failed: %s\n", name, error);
        return 1;
    }
    (void)fclose(f);
    (void)unlink(path);
    return 0;
}

static int scenario(const char *name, struct hstex_lexical_state *lex,
                    const char *outer, size_t outer_len, const char *inner,
                    size_t inner_len, int read_outer, int read_inner,
                    int tokens, int boundary)
{
    char outer_path[] = "/tmp/hstex-ser-o-XXXXXX";
    char inner_path[] = "/tmp/hstex-ser-i-XXXXXX";
    int a = mkstemp(outer_path), b = mkstemp(inner_path);
    if (a < 0 || b < 0) {
        (void)fprintf(stderr, "%s: mkstemp failed\n", name);
        return 1;
    }
    (void)close(a);
    (void)close(b);
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
    if (boundary &&
        hstex_source_push_boundary(&stack, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", name, error);
        return 1;
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

    /* Restore once (single round trip) and, from that restored stack, restore
       again (double round trip -- a checkpoint read from a checkpoint). Both
       must read to the end exactly as the original does. */
    struct hstex_source_stack once;
    struct hstex_source_stack twice;
    if (roundtrip(&stack, &once, lex, name) != 0) {
        return 1;
    }
    if (roundtrip(&once, &twice, lex, name) != 0) {
        return 1;
    }

    static struct step original[CAP];
    static struct step got_once[CAP];
    static struct step got_twice[CAP];
    size_t n_original = drain(&stack, original);
    size_t n_once = drain(&once, got_once);
    size_t n_twice = drain(&twice, got_twice);
    hstex_source_stack_destroy(&stack);
    hstex_source_stack_destroy(&once);
    hstex_source_stack_destroy(&twice);
    (void)unlink(outer_path);
    (void)unlink(inner_path);

    const struct step *others[] = {got_once, got_twice};
    const size_t counts[] = {n_once, n_twice};
    const char *labels[] = {"single", "double"};
    for (int pass = 0; pass < 2; ++pass) {
        if (n_original != counts[pass]) {
            (void)fprintf(stderr,
                          "%s (%s): length differs original %zu restored %zu\n",
                          name, labels[pass], n_original, counts[pass]);
            return 1;
        }
        for (size_t i = 0U; i < n_original && i < CAP; ++i) {
            const struct step *g = &others[pass][i];
            if (original[i].result != g->result ||
                original[i].token != g->token || original[i].line != g->line ||
                original[i].column != g->column) {
                (void)fprintf(stderr,
                              "%s (%s): step %zu differs orig(r=%d t=%u l=%u "
                              "c=%u) restored(r=%d t=%u l=%u c=%u)\n",
                              name, labels[pass], i, (int)original[i].result,
                              original[i].token, original[i].line,
                              original[i].column, (int)g->result, g->token,
                              g->line, g->column);
                return 1;
            }
        }
    }
    (void)printf("  %-22s %zu steps matched (single+double)\n", name,
                 n_original);
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
                         inner, sizeof(inner) - 1U, 4, 3, 1, 0);
    /* same, no token frame */
    failures += scenario("nested", &lex, outer, sizeof(outer) - 1U, inner,
                         sizeof(inner) - 1U, 5, 2, 0, 0);
    /* single file, deep into a later line */
    failures += scenario("single-midline", &lex, outer, sizeof(outer) - 1U, "",
                         0U, 9, 0, 0, 0);
    /* single file, only a couple of tokens read (still on line one) */
    failures += scenario("single-early", &lex, outer, sizeof(outer) - 1U, "", 0U,
                         2, 0, 0, 0);
    /* nothing read yet -- checkpoint at the very start */
    failures += scenario("start", &lex, outer, sizeof(outer) - 1U, "", 0U, 0, 0,
                         0, 0);
    /* caret notation, mid-line, so the collapsed line buffer must round-trip */
    failures += scenario("caret-midline", &lex, caret, sizeof(caret) - 1U, "",
                         0U, 3, 0, 1, 0);
    /* token frame over a caret file */
    failures += scenario("caret+tokens", &lex, caret, sizeof(caret) - 1U, inner,
                         sizeof(inner) - 1U, 2, 4, 1, 0);
    /* a boundary frame under a token frame over nested files: reading takes
       the token frame, then stops at the boundary */
    failures += scenario("boundary+tokens", &lex, outer, sizeof(outer) - 1U,
                         inner, sizeof(inner) - 1U, 3, 2, 1, 1);
    /* a boundary at the very top */
    failures += scenario("boundary-top", &lex, outer, sizeof(outer) - 1U, "",
                         0U, 4, 0, 0, 1);

    /* An empty stack round-trips to an empty stack. */
    {
        struct hstex_source_stack empty;
        hstex_source_stack_init(&empty, &lex);
        struct hstex_source_stack back;
        if (roundtrip(&empty, &back, &lex, "empty") != 0) {
            failures += 1;
        } else {
            static struct step steps[CAP];
            size_t n = drain(&back, steps);
            if (n != 1U || steps[0].result == HSTEX_MOUTH_TOKEN) {
                (void)fprintf(stderr, "empty: expected immediate non-token\n");
                failures += 1;
            } else {
                (void)printf("  %-22s ok\n", "empty");
            }
            hstex_source_stack_destroy(&back);
        }
        hstex_source_stack_destroy(&empty);
    }

    /* A truncated checkpoint is refused, not crashed on. */
    {
        char err[256] = {0};
        hstex_token tok;
        struct hstex_source_location w;
        char src[] = "/tmp/hstex-ser-src-XXXXXX";
        char path[] = "/tmp/hstex-ser-tr-XXXXXX";
        int sfd = mkstemp(src), pfd = mkstemp(path);
        if (sfd >= 0 && pfd >= 0) {
            static const char text[] = "some tokens here\n";
            ssize_t wrote = write(sfd, text, sizeof(text) - 1U);
            (void)wrote;
            (void)close(sfd);
            (void)close(pfd);
            struct hstex_source_stack live;
            hstex_source_stack_init(&live, &lex);
            (void)hstex_source_push_file(&live, src, err, sizeof(err));
            (void)hstex_source_next(&live, &tok, &w, err, sizeof(err));
            FILE *f = fopen(path, "wb");
            if (f != NULL) {
                (void)hstex_source_serialize(&live, f);
                (void)fclose(f);
            }
            hstex_source_stack_destroy(&live);
            /* Chop it down to a few bytes: enough for a length, not a frame. */
            int chopped = truncate(path, 5);
            (void)chopped;
            struct hstex_source_stack bad;
            hstex_source_stack_init(&bad, &lex);
            f = fopen(path, "rb");
            int rc = f != NULL
                         ? hstex_source_deserialize(&bad, f, &lex, err,
                                                    sizeof(err))
                         : -1;
            if (f != NULL) {
                (void)fclose(f);
            }
            hstex_source_stack_destroy(&bad);
            (void)unlink(src);
            (void)unlink(path);
            if (rc == 0) {
                (void)fprintf(stderr, "truncated: accepted bad input\n");
                failures += 1;
            } else {
                (void)printf("  %-22s refused\n", "truncated");
            }
        }
    }

    hstex_lexical_state_destroy(&lex);
    if (failures != 0) {
        (void)fprintf(stderr, "%d scenario(s) failed\n", failures);
        return 1;
    }
    (void)printf("source serializer: all scenarios round-tripped\n");
    return 0;
}
