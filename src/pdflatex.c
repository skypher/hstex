#include "hstex_config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    HSTEX_DRIVER_PATH_CAPACITY = 4096,
    HSTEX_DRIVER_READ_CAPACITY = 32768,
};

static void usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: hstex-pdflatex [OPTIONS] DOCUMENT.tex\n"
        "\n"
        "Supported pdfLaTeX options:\n"
        "  -output-directory=DIR  write document outputs under DIR\n"
        "  -jobname=NAME          use NAME for output files and \\jobname\n"
        "  -interaction=errorstopmode\n"
        "  -halt-on-error  -file-line-error  -no-shell-escape\n"
        "  -output-format=pdf\n"
        "\n"
        "HSTeX options:\n"
        "  -h, --help           show this help and exit\n"
        "  --format-cache=DIR    store native formats below DIR\n"
        "  --rebuild-format       rebuild the native LaTeX format\n"
        "\n"
        "HSTeX reuses the installed TeX Live/MacTeX input trees through\n"
        "kpsewhich; it does not read a pdfTeX format dump.\n");
}

static int set_error(char *error, size_t capacity, const char *message)
{
    if (capacity != 0U) {
        (void)snprintf(error, capacity, "%s", message);
    }
    return -1;
}

static char *copy_string(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static char *join_path(const char *left, const char *right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool separator = left_length != 0U && left[left_length - 1U] != '/';
    char *path = malloc(left_length + (size_t)separator + right_length + 1U);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, left, left_length);
    size_t at = left_length;
    if (separator) {
        path[at++] = '/';
    }
    memcpy(path + at, right, right_length + 1U);
    return path;
}

static char *absolute_path(const char *path, char *error, size_t capacity)
{
    if (path == NULL || path[0] == '\0') {
        (void)set_error(error, capacity, "empty path");
        return NULL;
    }
    if (path[0] == '/') {
        char *copy = copy_string(path);
        if (copy == NULL) {
            (void)set_error(error, capacity, "path allocation failed");
        }
        return copy;
    }
    char current[HSTEX_DRIVER_PATH_CAPACITY];
    if (getcwd(current, sizeof(current)) == NULL) {
        (void)set_error(error, capacity, "cannot determine the working directory");
        return NULL;
    }
    char *absolute = join_path(current, path);
    if (absolute == NULL) {
        (void)set_error(error, capacity, "path allocation failed");
    }
    return absolute;
}

static int make_directory_tree(const char *path, char *error, size_t capacity)
{
    char *working = copy_string(path);
    if (working == NULL) {
        return set_error(error, capacity, "directory allocation failed");
    }
    size_t length = strlen(working);
    while (length > 1U && working[length - 1U] == '/') {
        working[--length] = '\0';
    }
    for (char *at = working + 1; *at != '\0'; ++at) {
        if (*at != '/') {
            continue;
        }
        *at = '\0';
        if (mkdir(working, 0700) != 0 && errno != EEXIST) {
            (void)snprintf(error, capacity, "cannot create %s: %s", working,
                           strerror(errno));
            free(working);
            return -1;
        }
        *at = '/';
    }
    if (mkdir(working, 0700) != 0 && errno != EEXIST) {
        (void)snprintf(error, capacity, "cannot create %s: %s", working,
                       strerror(errno));
        free(working);
        return -1;
    }
    free(working);
    return 0;
}

static int write_all(int descriptor, const uint8_t *data, size_t count)
{
    while (count != 0U) {
        ssize_t written = write(descriptor, data, count);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += (size_t)written;
        count -= (size_t)written;
    }
    return 0;
}

static int wait_for_child(pid_t child)
{
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int run_captured(char *const arguments[], char **answer, char *error,
                        size_t capacity)
{
    int pipe_descriptors[2];
    if (pipe(pipe_descriptors) != 0) {
        return set_error(error, capacity, "cannot create lookup pipe");
    }
    pid_t child = fork();
    if (child < 0) {
        (void)close(pipe_descriptors[0]);
        (void)close(pipe_descriptors[1]);
        return set_error(error, capacity, "cannot start kpsewhich");
    }
    if (child == 0) {
        (void)close(pipe_descriptors[0]);
        if (dup2(pipe_descriptors[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        (void)close(pipe_descriptors[1]);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    (void)close(pipe_descriptors[1]);
    size_t used = 0U;
    size_t allocated = 256U;
    char *result = malloc(allocated);
    if (result == NULL) {
        (void)close(pipe_descriptors[0]);
        (void)wait_for_child(child);
        return set_error(error, capacity, "lookup allocation failed");
    }
    for (;;) {
        if (used + 256U >= allocated) {
            size_t next = allocated * 2U;
            char *grown = realloc(result, next);
            if (grown == NULL) {
                free(result);
                (void)close(pipe_descriptors[0]);
                (void)wait_for_child(child);
                return set_error(error, capacity, "lookup allocation failed");
            }
            result = grown;
            allocated = next;
        }
        ssize_t count = read(pipe_descriptors[0], result + used,
                             allocated - used - 1U);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(result);
            (void)close(pipe_descriptors[0]);
            (void)wait_for_child(child);
            return set_error(error, capacity, "cannot read kpsewhich output");
        }
        used += (size_t)count;
    }
    (void)close(pipe_descriptors[0]);
    if (wait_for_child(child) != 0) {
        free(result);
        return set_error(error, capacity,
                         "kpsewhich could not resolve the TeX installation");
    }
    while (used != 0U && (result[used - 1U] == '\n' || result[used - 1U] == '\r')) {
        --used;
    }
    result[used] = '\0';
    if (used == 0U) {
        free(result);
        return set_error(error, capacity, "kpsewhich returned no path");
    }
    *answer = result;
    return 0;
}

static int query_kpsewhich(const char *argument, char **answer, char *error,
                           size_t capacity)
{
    char *const command[] = {"kpsewhich", (char *)argument, NULL};
    return run_captured(command, answer, error, capacity);
}

static int run_engine(char *const arguments[], const char *directory,
                      const char *log_path, char *error, size_t capacity)
{
    int pipe_descriptors[2] = {-1, -1};
    if (log_path != NULL && pipe(pipe_descriptors) != 0) {
        return set_error(error, capacity, "cannot create engine log pipe");
    }
    pid_t child = fork();
    if (child < 0) {
        if (pipe_descriptors[0] >= 0) {
            (void)close(pipe_descriptors[0]);
            (void)close(pipe_descriptors[1]);
        }
        return set_error(error, capacity, "cannot start hstex");
    }
    if (child == 0) {
        if (directory != NULL && chdir(directory) != 0) {
            _exit(127);
        }
        if (log_path != NULL) {
            (void)close(pipe_descriptors[0]);
            if (dup2(pipe_descriptors[1], STDOUT_FILENO) < 0 ||
                dup2(pipe_descriptors[1], STDERR_FILENO) < 0) {
                _exit(127);
            }
            (void)close(pipe_descriptors[1]);
        }
        execvp(arguments[0], arguments);
        _exit(127);
    }
    if (log_path == NULL) {
        return wait_for_child(child);
    }
    (void)close(pipe_descriptors[1]);
    int log = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool write_failed = log < 0;
    uint8_t buffer[HSTEX_DRIVER_READ_CAPACITY];
    for (;;) {
        ssize_t count = read(pipe_descriptors[0], buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            write_failed = true;
            break;
        }
        if (log >= 0 && write_all(log, buffer, (size_t)count) != 0) {
            write_failed = true;
        }
        if (write_all(STDOUT_FILENO, buffer, (size_t)count) != 0) {
            write_failed = true;
        }
    }
    (void)close(pipe_descriptors[0]);
    if (log >= 0) {
        (void)close(log);
    }
    int status = wait_for_child(child);
    if (write_failed) {
        return set_error(error, capacity, "cannot write the HSTeX log");
    }
    return status;
}

static uint64_t fnv1a64_update(uint64_t hash, const uint8_t *data,
                                size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    return fnv1a64_update(hash, (const uint8_t *)text, strlen(text) + 1U);
}

static int hash_file(uint64_t *hash, const char *path, char *error,
                     size_t capacity)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        (void)snprintf(error, capacity, "cannot read %s: %s", path,
                       strerror(errno));
        return -1;
    }
    uint8_t buffer[HSTEX_DRIVER_READ_CAPACITY];
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        *hash = fnv1a64_update(*hash, buffer, count);
        if (count != sizeof(buffer)) {
            if (ferror(stream)) {
                (void)snprintf(error, capacity, "cannot read %s", path);
                (void)fclose(stream);
                return -1;
            }
            break;
        }
    }
    (void)fclose(stream);
    return 0;
}

static void hash_texmf_database_indexes(uint64_t *hash, const char *trees)
{
    const char *at = trees;
    while (*at != '\0') {
        while (*at == '{' || *at == '}' || *at == ',' || *at == ' ') {
            ++at;
        }
        if (at[0] == '!' && at[1] == '!') {
            at += 2;
        }
        const char *end = at;
        while (*end != '\0' && *end != ',' && *end != '}') {
            ++end;
        }
        size_t length = (size_t)(end - at);
        while (length != 0U && at[length - 1U] == '/') {
            --length;
        }
        if (length != 0U && length + sizeof("/ls-R") <=
                                HSTEX_DRIVER_PATH_CAPACITY) {
            char path[HSTEX_DRIVER_PATH_CAPACITY];
            memcpy(path, at, length);
            memcpy(path + length, "/ls-R", sizeof("/ls-R"));
            struct stat metadata;
            if (stat(path, &metadata) == 0) {
                char record[128];
                *hash = hash_text(*hash, path);
                (void)snprintf(record, sizeof(record), "%" PRIdMAX ":%" PRIdMAX
                                                        ":%" PRIdMAX ":%" PRIdMAX,
                               (intmax_t)metadata.st_dev,
                               (intmax_t)metadata.st_ino,
                               (intmax_t)metadata.st_size,
                               (intmax_t)metadata.st_mtime);
                *hash = hash_text(*hash, record);
            }
        }
        at = end;
    }
}

static void hash_environment(uint64_t *hash)
{
    static const char *const names[] = {
        "TEXINPUTS", "TEXFONTS", "TFMFONTS", "VFFONTS", "TEXMFHOME",
        "TEXMFLOCAL", "TEXMFCNF", "TEXMF", "TEXMFDBS", NULL,
    };
    for (size_t index = 0U; names[index] != NULL; ++index) {
        *hash = hash_text(*hash, names[index]);
        const char *value = getenv(names[index]);
        *hash = hash_text(*hash, value == NULL ? "" : value);
    }
}

static int format_fingerprint(const char *latex_ltx, const char *config,
                              uint64_t *fingerprint, char *error,
                              size_t capacity)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = hash_text(hash, "hstex-pdflatex-format-v1");
    hash = hash_text(hash, HSTEX_VERSION);
    hash = hash_text(hash, latex_ltx);
    if (hash_file(&hash, latex_ltx, error, capacity) != 0) {
        return -1;
    }
    hash = hash_text(hash, config);
    if (hash_file(&hash, config, error, capacity) != 0) {
        return -1;
    }
    hash_environment(&hash);
    char *trees = NULL;
    if (query_kpsewhich("--var-value=TEXMFDBS", &trees, error, capacity) !=
        0) {
        return -1;
    }
    hash = hash_text(hash, trees);
    hash_texmf_database_indexes(&hash, trees);
    free(trees);
    *fingerprint = hash;
    return 0;
}

static char *cache_root(const char *requested, char *error, size_t capacity)
{
    const char *root = requested;
    if (root == NULL || root[0] == '\0') {
        root = getenv("HSTEX_CACHE_DIR");
    }
    if (root == NULL || root[0] == '\0') {
        root = getenv("XDG_CACHE_HOME");
        if (root != NULL && root[0] != '\0') {
            char *base = join_path(root, "hstex");
            if (base == NULL) {
                (void)set_error(error, capacity, "cache path allocation failed");
                return NULL;
            }
            char *absolute = absolute_path(base, error, capacity);
            free(base);
            return absolute;
        }
        root = getenv("HOME");
        if (root == NULL || root[0] == '\0') {
            (void)set_error(error, capacity,
                            "set HSTEX_CACHE_DIR or HOME for the format cache");
            return NULL;
        }
        char *base = join_path(root, ".cache/hstex");
        if (base == NULL) {
            (void)set_error(error, capacity, "cache path allocation failed");
            return NULL;
        }
        char *absolute = absolute_path(base, error, capacity);
        free(base);
        return absolute;
    }
    return absolute_path(root, error, capacity);
}

static int build_or_load_format(const char *engine, const char *latex_ltx,
                                const char *cache, bool rebuild,
                                char **format, char *error, size_t capacity)
{
    char *config = NULL;
    if (query_kpsewhich("pdftexconfig.tex", &config, error, capacity) != 0) {
        return -1;
    }
    char *absolute_config = absolute_path(config, error, capacity);
    free(config);
    if (absolute_config == NULL) {
        return -1;
    }
    uint64_t fingerprint = 0U;
    if (format_fingerprint(latex_ltx, absolute_config, &fingerprint, error,
                           capacity) != 0) {
        free(absolute_config);
        return -1;
    }
    free(absolute_config);
    char key[32];
    (void)snprintf(key, sizeof(key), "%016" PRIx64, fingerprint);
    char *formats = join_path(cache, "formats");
    char *directory = formats == NULL ? NULL : join_path(formats, key);
    free(formats);
    if (directory == NULL) {
        return set_error(error, capacity, "cache path allocation failed");
    }
    if (make_directory_tree(directory, error, capacity) != 0) {
        free(directory);
        return -1;
    }
    char *ready = join_path(directory, "pdflatex.hfmt");
    if (ready == NULL) {
        free(directory);
        return set_error(error, capacity, "format path allocation failed");
    }
    struct stat metadata;
    if (!rebuild && stat(ready, &metadata) == 0 && metadata.st_size > 0) {
        free(directory);
        *format = ready;
        return 0;
    }
    char temporary_name[96];
    (void)snprintf(temporary_name, sizeof(temporary_name),
                   "pdflatex.hfmt.%ld.tmp", (long)getpid());
    char *temporary = join_path(directory, temporary_name);
    if (temporary == NULL) {
        free(directory);
        free(ready);
        return set_error(error, capacity, "format path allocation failed");
    }
    (void)fprintf(stderr, "hstex-pdflatex: building native format %s\n", ready);
    char *const command[] = {(char *)engine, "--make-format", (char *)latex_ltx,
                             temporary, NULL};
    int status = run_engine(command, directory, NULL, error, capacity);
    if (status != 0) {
        (void)unlink(temporary);
        free(directory);
        free(temporary);
        free(ready);
        if (status > 0 && error[0] == '\0') {
            (void)snprintf(error, capacity, "native format build exited %d", status);
        }
        return -1;
    }
    if (rename(temporary, ready) != 0) {
        (void)unlink(temporary);
        (void)snprintf(error, capacity, "cannot publish native format: %s",
                       strerror(errno));
        free(directory);
        free(temporary);
        free(ready);
        return -1;
    }
    free(directory);
    free(temporary);
    *format = ready;
    return 0;
}

static char *job_name_from_document(const char *document, char *error,
                                    size_t capacity)
{
    const char *base = strrchr(document, '/');
    base = base == NULL ? document : base + 1;
    size_t length = strlen(base);
    size_t stem = length;
    for (size_t index = length; index > 1U; --index) {
        if (base[index - 1U] == '.') {
            stem = index - 1U;
            break;
        }
    }
    if (stem == 0U) {
        (void)set_error(error, capacity, "document has no usable job name");
        return NULL;
    }
    char *name = malloc(stem + 1U);
    if (name == NULL) {
        (void)set_error(error, capacity, "job-name allocation failed");
        return NULL;
    }
    memcpy(name, base, stem);
    name[stem] = '\0';
    return name;
}

static bool valid_job_name(const char *name)
{
    return name != NULL && name[0] != '\0' && strchr(name, '/') == NULL &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static const char *equals_option_value(const char *argument, const char *name)
{
    size_t length = strlen(name);
    return strncmp(argument, name, length) == 0 && argument[length] == '='
               ? argument + length + 1U
               : NULL;
}

static bool is_named_option(const char *argument, const char *short_name,
                            const char *long_name)
{
    return strcmp(argument, short_name) == 0 || strcmp(argument, long_name) == 0;
}

static bool is_value_option(const char *argument, const char *short_name,
                            const char *long_name, const char **value)
{
    const char *found = equals_option_value(argument, short_name);
    if (found == NULL) {
        found = equals_option_value(argument, long_name);
    }
    if (found == NULL) {
        return false;
    }
    *value = found;
    return true;
}

int main(int argument_count, char **arguments)
{
    const char *output_directory = ".";
    const char *requested_cache = NULL;
    const char *requested_job_name = NULL;
    const char *document = NULL;
    bool rebuild = false;
    bool options = true;
    for (int index = 1; index < argument_count; ++index) {
        const char *argument = arguments[index];
        if (options && strcmp(argument, "--") == 0) {
            options = false;
            continue;
        }
        if (options && (strcmp(argument, "-h") == 0 ||
                        strcmp(argument, "--help") == 0 ||
                        strcmp(argument, "-help") == 0)) {
            usage(stdout);
            return 0;
        }
        if (options && (strcmp(argument, "--version") == 0 ||
                        strcmp(argument, "-version") == 0)) {
            (void)printf("hstex-pdflatex %s\n", HSTEX_VERSION);
            return 0;
        }
        const char *value = NULL;
        if (options && is_value_option(argument, "-output-directory",
                                       "--output-directory", &value)) {
            if (value[0] == '\0') {
                (void)fprintf(stderr, "hstex-pdflatex: empty output directory\n");
                return 2;
            }
            output_directory = value;
            continue;
        }
        if (options && is_named_option(argument, "-output-directory",
                                       "--output-directory")) {
            if (++index == argument_count) {
                (void)fprintf(stderr,
                              "hstex-pdflatex: -output-directory needs a value\n");
                return 2;
            }
            output_directory = arguments[index];
            continue;
        }
        if (options && is_value_option(argument, "-jobname", "--jobname",
                                       &value)) {
            requested_job_name = value;
            continue;
        }
        if (options && is_named_option(argument, "-jobname", "--jobname")) {
            if (++index == argument_count) {
                (void)fprintf(stderr, "hstex-pdflatex: -jobname needs a value\n");
                return 2;
            }
            requested_job_name = arguments[index];
            continue;
        }
        if (options && is_value_option(argument, "--format-cache",
                                       "--format-cache", &value)) {
            requested_cache = value;
            continue;
        }
        if (options && strcmp(argument, "--rebuild-format") == 0) {
            rebuild = true;
            continue;
        }
        if (options &&
            (strcmp(argument, "-interaction=errorstopmode") == 0 ||
             strcmp(argument, "--interaction=errorstopmode") == 0 ||
             strcmp(argument, "-halt-on-error") == 0 ||
             strcmp(argument, "--halt-on-error") == 0 ||
             strcmp(argument, "-file-line-error") == 0 ||
             strcmp(argument, "--file-line-error") == 0 ||
             strcmp(argument, "-no-shell-escape") == 0 ||
             strcmp(argument, "--no-shell-escape") == 0 ||
             strcmp(argument, "-output-format=pdf") == 0 ||
             strcmp(argument, "--output-format=pdf") == 0)) {
            continue;
        }
        if (options && argument[0] == '-') {
            (void)fprintf(stderr, "hstex-pdflatex: unsupported option: %s\n",
                          argument);
            return 2;
        }
        if (document != NULL) {
            (void)fprintf(stderr,
                          "hstex-pdflatex: exactly one document is required\n");
            return 2;
        }
        document = argument;
    }
    if (document == NULL) {
        usage(stderr);
        return 2;
    }
    char error[512] = {0};
    if (make_directory_tree(output_directory, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex-pdflatex: %s\n", error);
        return 1;
    }
    char *job_name = requested_job_name == NULL
                         ? job_name_from_document(document, error, sizeof(error))
                         : copy_string(requested_job_name);
    if (job_name == NULL || !valid_job_name(job_name)) {
        (void)fprintf(stderr, "hstex-pdflatex: invalid job name\n");
        free(job_name);
        return 2;
    }
    char *latex_ltx = NULL;
    if (query_kpsewhich("latex.ltx", &latex_ltx, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex-pdflatex: %s\n", error);
        free(job_name);
        return 1;
    }
    char *absolute_latex = absolute_path(latex_ltx, error, sizeof(error));
    free(latex_ltx);
    if (absolute_latex == NULL) {
        (void)fprintf(stderr, "hstex-pdflatex: %s\n", error);
        free(job_name);
        return 1;
    }
    char *cache = cache_root(requested_cache, error, sizeof(error));
    if (cache == NULL) {
        (void)fprintf(stderr, "hstex-pdflatex: %s\n", error);
        free(absolute_latex);
        free(job_name);
        return 1;
    }
    const char *engine = getenv("HSTEX_ENGINE");
    if (engine == NULL || engine[0] == '\0') {
        engine = "hstex";
    }
    char *format = NULL;
    if (build_or_load_format(engine, absolute_latex, cache, rebuild, &format,
                             error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "hstex-pdflatex: %s\n", error);
        free(cache);
        free(absolute_latex);
        free(job_name);
        return 1;
    }
    char *log_name = malloc(strlen(job_name) + sizeof(".log"));
    if (log_name == NULL) {
        (void)fprintf(stderr, "hstex-pdflatex: log path allocation failed\n");
        free(format);
        free(cache);
        free(absolute_latex);
        free(job_name);
        return 1;
    }
    (void)snprintf(log_name, strlen(job_name) + sizeof(".log"), "%s.log",
                   job_name);
    char *log_path = join_path(output_directory, log_name);
    free(log_name);
    if (log_path == NULL) {
        (void)fprintf(stderr, "hstex-pdflatex: log path allocation failed\n");
        free(format);
        free(cache);
        free(absolute_latex);
        free(job_name);
        return 1;
    }
    char *const command[] = {(char *)engine, "--format-output", format,
                             (char *)document, (char *)output_directory, job_name,
                             NULL};
    int status = run_engine(command, NULL, log_path, error, sizeof(error));
    if (status < 0) {
        (void)fprintf(stderr, "hstex-pdflatex: %s\n", error);
        status = 1;
    }
    free(log_path);
    free(format);
    free(cache);
    free(absolute_latex);
    free(job_name);
    return status;
}
