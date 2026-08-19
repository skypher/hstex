#include "hstex/catcode.h"
#include "hstex/lex.h"
#include "hstex/mouth.h"
#include "hstex/source.h"
#include "hstex/token.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int descriptor, const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = write(descriptor, data + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int expect_character(struct hstex_source_stack *stack, uint8_t character,
                            uint32_t expected_line, char *error,
                            size_t error_capacity)
{
    hstex_token token;
    struct hstex_source_location location;
    enum hstex_mouth_result result = hstex_source_next(
        stack, &token, &location, error, error_capacity);
    if (result != HSTEX_MOUTH_TOKEN || !hstex_token_is_character(token) ||
        hstex_token_character_code(token) != character ||
        location.line != expected_line) {
        (void)fprintf(stderr,
                      "expected character %u at line %u, result=%d error=%s\n",
                      (unsigned int)character, (unsigned int)expected_line,
                      (int)result, error);
        return 1;
    }
    return 0;
}

int main(void)
{
    char first_path[] = "/tmp/hstex-source-first-XXXXXX";
    char second_path[] = "/tmp/hstex-source-second-XXXXXX";
    int first = mkstemp(first_path);
    int second = mkstemp(second_path);
    static const uint8_t first_input[] = "ab\n";
    static const uint8_t second_input[] = "c\n";
    if (first < 0 || second < 0 ||
        write_all(first, first_input, sizeof(first_input) - 1U) != 0 ||
        write_all(second, second_input, sizeof(second_input) - 1U) != 0 ||
        close(first) != 0 || close(second) != 0) {
        (void)fprintf(stderr, "could not create source fixtures\n");
        return 1;
    }

    struct hstex_lexical_state lexical_state;
    struct hstex_source_stack stack;
    char error[256] = {0};
    if (hstex_lexical_state_init(&lexical_state, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        return 1;
    }
    hstex_source_stack_init(&stack, &lexical_state);
    if (hstex_source_push_file(&stack, first_path, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)'a', 1U, error, sizeof(error)) != 0 ||
        hstex_source_push_file(&stack, second_path, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)'c', 1U, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)' ', 1U, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)'b', 1U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        hstex_source_stack_destroy(&stack);
        hstex_lexical_state_destroy(&lexical_state);
        return 1;
    }

    hstex_token inserted[] = {
        hstex_token_character((uint8_t)HSTEX_CAT_OTHER, (uint8_t)'X'),
        hstex_token_character((uint8_t)HSTEX_CAT_OTHER, (uint8_t)'Y'),
    };
    struct hstex_source_location inserted_location = {
        .line = 99U,
        .column = 3U,
    };
    if (hstex_source_push_tokens(&stack, inserted,
                                 sizeof(inserted) / sizeof(inserted[0]),
                                 inserted_location, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)'X', 99U, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)'Y', 99U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        hstex_source_stack_destroy(&stack);
        hstex_lexical_state_destroy(&lexical_state);
        return 1;
    }

    hstex_token bounded =
        hstex_token_character((uint8_t)HSTEX_CAT_OTHER, (uint8_t)'Z');
    hstex_token token;
    struct hstex_source_location location;
    if (hstex_source_push_boundary(&stack, error, sizeof(error)) != 0 ||
        hstex_source_push_tokens(&stack, &bounded, 1U, inserted_location, error,
                                 sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)'Z', 99U, error, sizeof(error)) != 0 ||
        hstex_source_next(&stack, &token, &location, error, sizeof(error)) !=
            HSTEX_MOUTH_EOF ||
        hstex_source_pop_boundary(&stack, error, sizeof(error)) != 0 ||
        expect_character(&stack, (uint8_t)' ', 1U, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "%s\n", error);
        hstex_source_stack_destroy(&stack);
        hstex_lexical_state_destroy(&lexical_state);
        return 1;
    }

    enum hstex_mouth_result final_result = hstex_source_next(
        &stack, &token, &location, error, sizeof(error));
    int failed = final_result != HSTEX_MOUTH_EOF;
    for (size_t index = 0U; !failed && index < 128U; ++index) {
        hstex_token *owned = malloc(sizeof(*owned));
        if (owned == NULL) {
            failed = 1;
            break;
        }
        owned[0] = hstex_token_character((uint8_t)HSTEX_CAT_OTHER,
                                         (uint8_t)'T');
        if (hstex_source_push_owned_tokens(&stack, owned, 1U,
                                           inserted_location, error,
                                           sizeof(error)) != 0 ||
            expect_character(&stack, (uint8_t)'T', 99U, error,
                             sizeof(error)) != 0 ||
            stack.count != 1U || stack.capacity > 16U) {
            (void)fprintf(stderr,
                          "exhausted token frames accumulated: count=%zu "
                          "capacity=%zu error=%s\n",
                          stack.count, stack.capacity, error);
            failed = 1;
        }
    }
    if (!failed &&
        hstex_source_next(&stack, &token, &location, error, sizeof(error)) !=
            HSTEX_MOUTH_EOF) {
        failed = 1;
    }
    hstex_source_stack_destroy(&stack);
    hstex_lexical_state_destroy(&lexical_state);
    if (unlink(first_path) != 0 || unlink(second_path) != 0) {
        (void)fprintf(stderr, "could not remove source fixtures\n");
        return 1;
    }
    return failed;
}
