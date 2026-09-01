#include "hstex/filedb.h"
#include "test_cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The database is what an installation holds, so a machine without one is
   not a failing test: what is checked here is that where it answers, it
   answers something real, and that it keeps quiet about everything it was
   told to keep quiet about. */

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        (void)fprintf(stderr, "test_filedb: %s\n", what);
        ++failures;
    }
}

static bool is_readable_file(const char *path)
{
    struct stat status;
    return path != NULL && stat(path, &status) == 0 &&
           S_ISREG(status.st_mode) && access(path, R_OK) == 0;
}

int main(int argument_count, char **arguments)
{
    int option = hstex_test_arguments(
        argument_count, arguments, "Run the HSTeX file-database tests.");
    if (option >= 0) {
        return option;
    }
    const struct hstex_file_db *database = hstex_file_db_shared();
    if (database == NULL) {
        (void)printf("test_filedb: no filename database here; nothing to ask\n");
        return 0;
    }

    /* A name it answers is a file that is there to be read. */
    static const char *const asked[] = {"latex.ltx", "article.cls", "cmr10.tfm",
                                        "size10.clo", NULL};
    int answered = 0;
    for (size_t index = 0U; asked[index] != NULL; ++index) {
        const char *path = hstex_file_db_lookup(database, asked[index]);
        if (path == NULL) {
            continue; /* an installation need not carry it */
        }
        ++answered;
        check(is_readable_file(path), "answered with something unreadable");
        const char *slash = strrchr(path, '/');
        check(slash != NULL && strcmp(slash + 1, asked[index]) == 0,
              "answered with a path that is not the name asked for");
    }
    check(answered > 0, "answered nothing at all for a LaTeX installation");

    /* A metric is answered from where metrics live, an input from where
       inputs live, and the documentation that sits beside them is not an
       answer to either. */
    const char *metric = hstex_file_db_lookup(database, "cmr10.tfm");
    if (metric != NULL) {
        check(strstr(metric, "/fonts/tfm/") != NULL,
              "a metric was answered from outside the metric trees");
    }
    const char *input = hstex_file_db_lookup(database, "article.cls");
    if (input != NULL) {
        check(strstr(input, "/tex/") != NULL,
              "an input was answered from outside the input trees");
    }

    /* What it must not try to answer. */
    check(hstex_file_db_lookup(database, "") == NULL, "answered an empty name");
    check(hstex_file_db_lookup(database, "tex/latex/base/article.cls") == NULL,
          "answered a name with a directory in it");
    check(hstex_file_db_lookup(database, "hstex-no-such-file-anywhere.tex") ==
              NULL,
          "answered a name nothing holds");
    check(hstex_file_db_lookup(NULL, "article.cls") == NULL,
          "answered without a database");

    if (failures != 0) {
        (void)fprintf(stderr, "test_filedb: %d checks failed\n", failures);
        return 1;
    }
    (void)printf("test_filedb: ok\n");
    return 0;
}
