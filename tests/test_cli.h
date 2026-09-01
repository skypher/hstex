#ifndef HSTEX_TEST_CLI_H
#define HSTEX_TEST_CLI_H

#include <stdio.h>
#include <string.h>

/* Test executables normally take no arguments, but remain inspectable without
   running their substantive work. A negative result means run the test. */
static int hstex_test_arguments(int argument_count, char **arguments,
                                const char *description)
{
    if (argument_count == 1) {
        return -1;
    }
    if (argument_count == 2 &&
        (strcmp(arguments[1], "-h") == 0 ||
         strcmp(arguments[1], "--help") == 0)) {
        (void)printf("Usage: %s [-h|--help]\n%s\n", arguments[0],
                     description);
        return 0;
    }
    (void)fprintf(stderr, "Usage: %s [-h|--help]\n", arguments[0]);
    return 2;
}

#endif
