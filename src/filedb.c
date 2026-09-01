/* WHERE A FILE IS, WITHOUT ASKING A CHILD PROCESS.
 *
 * An installation keeps a list of what its trees hold -- one `ls-R' beside
 * each tree root, naming every directory and every file under it. The tool
 * this engine would otherwise ask reads those same lists; reading them here
 * costs one pass over a few hundred kilobytes at the first question, and
 * answers every question after it out of memory.
 *
 * The lists are input data, not anything's implementation: a line ending in
 * a colon names the directory the lines under it belong to, and the rest are
 * what that directory holds. See SOURCE_POLICY.md.
 *
 * Only a name the database is sure about is answered here. A name it holds
 * twice, a name it does not hold, a name with a directory in it, or a name
 * that is not a readable file where the list says it is, all go back to the
 * tool, which knows about search paths this does not. So does everything,
 * when the environment names a search of its own: TEXINPUTS and its like
 * put directories in front of the trees, and what the database says about a
 * tree is then not the answer.
 */

#include "hstex/filedb.h"

#include "hstex/input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum {
    HSTEX_FILE_DB_INITIAL_SLOTS = 32768,
    HSTEX_FILE_DB_INITIAL_ENTRIES = 16384,
    HSTEX_FILE_DB_INITIAL_BYTES = 1 << 20,
};

/* Which of the places an installation searches the file stands in, judged
   from where the list puts it rather than from the path it ends up with:
   a tree may itself be called anything. */
enum {
    HSTEX_FILE_UNDER_NOWHERE = 0U,
    HSTEX_FILE_UNDER_TEX = 1U,
    HSTEX_FILE_UNDER_TFM = 2U,
    HSTEX_FILE_UNDER_VF = 3U,
    HSTEX_FILE_UNDER_TYPE1 = 4U,
    HSTEX_FILE_UNDER_AFM = 5U,
    HSTEX_FILE_UNDER_ENC = 6U,
    HSTEX_FILE_UNDER_MAP = 7U,
};

/* A place is a directory under a tree's root, and each holds one kind of
   thing. What is here is what the search was measured to look through; see
   docs/DECISIONS.md, finding-a-file. */
static const struct {
    const char *directory;
    uint32_t where;
} hstex_file_places[] = {
    {"tex/", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {"fonts/tfm/", (uint32_t)HSTEX_FILE_UNDER_TFM},
    {"fonts/vf/", (uint32_t)HSTEX_FILE_UNDER_VF},
    {"fonts/type1/", (uint32_t)HSTEX_FILE_UNDER_TYPE1},
    {"fonts/afm/", (uint32_t)HSTEX_FILE_UNDER_AFM},
    {"fonts/enc/", (uint32_t)HSTEX_FILE_UNDER_ENC},
    {"fonts/map/", (uint32_t)HSTEX_FILE_UNDER_MAP},
    {NULL, (uint32_t)HSTEX_FILE_UNDER_NOWHERE},
};

/* And what a name of each kind is looked for in. A kind not named here is
   left to the tool, whose guess about it this does not try to reproduce. */
static const struct {
    const char *suffix;
    uint32_t where;
} hstex_file_kinds[] = {
    {".tfm", (uint32_t)HSTEX_FILE_UNDER_TFM},
    {".vf", (uint32_t)HSTEX_FILE_UNDER_VF},
    {".pfb", (uint32_t)HSTEX_FILE_UNDER_TYPE1},
    {".afm", (uint32_t)HSTEX_FILE_UNDER_AFM},
    {".enc", (uint32_t)HSTEX_FILE_UNDER_ENC},
    {".map", (uint32_t)HSTEX_FILE_UNDER_MAP},
    {".tex", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".sty", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".cls", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".clo", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".def", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".cfg", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".fd", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".ltx", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {".dfu", (uint32_t)HSTEX_FILE_UNDER_TEX},
    {NULL, (uint32_t)HSTEX_FILE_UNDER_NOWHERE},
};

struct hstex_file_entry {
    uint32_t path;        /* the whole path, NUL-terminated, in `bytes' */
    uint32_t name;        /* the last component of it, in the same place */
    uint32_t name_length;
    uint32_t where;
};

struct hstex_file_db {
    uint8_t *bytes;
    size_t byte_count;
    size_t byte_capacity;
    struct hstex_file_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    uint32_t *slots;
    size_t slot_capacity;
};

/* The trees are read once for the process. What is read is what the
   installation holds, which does not change under a run, so this is a
   second copy of something already immutable rather than state of the
   engine's own: the run's own writing is looked for on disk before any of
   this is asked. The dispatch in src/scan.c settles itself the same way. */
static atomic_int shared_state;
static struct hstex_file_db shared_database;
static bool shared_usable;

static uint64_t name_hash(const uint8_t *name, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)name[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int reserve_bytes(struct hstex_file_db *database, size_t required)
{
    if (required <= database->byte_capacity) {
        return 0;
    }
    size_t capacity = database->byte_capacity == 0U
                          ? (size_t)HSTEX_FILE_DB_INITIAL_BYTES
                          : database->byte_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return -1;
        }
        capacity *= 2U;
    }
    uint8_t *grown = realloc(database->bytes, capacity);
    if (grown == NULL) {
        return -1;
    }
    database->bytes = grown;
    database->byte_capacity = capacity;
    return 0;
}

static int reserve_entries(struct hstex_file_db *database, size_t required)
{
    if (required <= database->entry_capacity) {
        return 0;
    }
    size_t capacity = database->entry_capacity == 0U
                          ? (size_t)HSTEX_FILE_DB_INITIAL_ENTRIES
                          : database->entry_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / (2U * sizeof(*database->entries))) {
            return -1;
        }
        capacity *= 2U;
    }
    struct hstex_file_entry *grown =
        realloc(database->entries, capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    database->entries = grown;
    database->entry_capacity = capacity;
    return 0;
}

static void chain(struct hstex_file_db *database, uint32_t identifier)
{
    const struct hstex_file_entry *entry = &database->entries[identifier - 1U];
    uint64_t hash = name_hash(database->bytes + entry->name, entry->name_length);
    size_t slot = (size_t)hash & (database->slot_capacity - 1U);
    while (database->slots[slot] != 0U) {
        slot = (slot + 1U) & (database->slot_capacity - 1U);
    }
    database->slots[slot] = identifier;
}

/* Room for one more name, kept at twice what is held so a walk of the
   slots stops soon after it starts. */
static int reserve_slots(struct hstex_file_db *database)
{
    if (database->slot_capacity != 0U &&
        database->entry_count + 1U <= database->slot_capacity / 2U) {
        return 0;
    }
    size_t capacity = database->slot_capacity == 0U
                          ? (size_t)HSTEX_FILE_DB_INITIAL_SLOTS
                          : database->slot_capacity * 2U;
    while (database->entry_count + 1U > capacity / 2U) {
        if (capacity > SIZE_MAX / 2U) {
            return -1;
        }
        capacity *= 2U;
    }
    uint32_t *grown = calloc(capacity, sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    free(database->slots);
    database->slots = grown;
    database->slot_capacity = capacity;
    for (size_t index = 0U; index < database->entry_count; ++index) {
        chain(database, (uint32_t)(index + 1U));
    }
    return 0;
}

/* One file, under the directory the list is in the middle of. */
static uint32_t place_of(const char *directory)
{
    for (size_t index = 0U; hstex_file_places[index].directory != NULL;
         ++index) {
        const char *under = hstex_file_places[index].directory;
        size_t length = strlen(under);
        if (strncmp(directory, under, length) == 0 ||
            (strncmp(directory, under, length - 1U) == 0 &&
             directory[length - 1U] == '\0')) {
            return hstex_file_places[index].where;
        }
    }
    return (uint32_t)HSTEX_FILE_UNDER_NOWHERE;
}

static int add_entry(struct hstex_file_db *database, const char *tree,
                     const char *directory, const uint8_t *name, size_t length)
{
    if (length == 0U || length > UINT32_MAX) {
        return -1;
    }
    size_t tree_length = strlen(tree);
    size_t directory_length = strlen(directory);
    size_t needed = database->byte_count + tree_length + 1U +
                    directory_length + 1U + length + 1U;
    if (reserve_bytes(database, needed) != 0 ||
        reserve_entries(database, database->entry_count + 1U) != 0 ||
        reserve_slots(database) != 0) {
        return -1;
    }
    size_t at = database->byte_count;
    uint32_t path = (uint32_t)at;
    memcpy(database->bytes + at, tree, tree_length);
    at += tree_length;
    if (directory_length != 0U) {
        database->bytes[at++] = (uint8_t)'/';
        memcpy(database->bytes + at, directory, directory_length);
        at += directory_length;
    }
    database->bytes[at++] = (uint8_t)'/';
    uint32_t name_at = (uint32_t)at;
    memcpy(database->bytes + at, name, length);
    at += length;
    database->bytes[at++] = 0U;
    database->byte_count = at;

    struct hstex_file_entry *entry = &database->entries[database->entry_count];
    entry->path = path;
    entry->name = name_at;
    entry->name_length = (uint32_t)length;
    entry->where = place_of(directory);
    ++database->entry_count;
    chain(database, (uint32_t)database->entry_count);
    return 0;
}

/* A tree's list, read as the lines it is: `% ...' says nothing, a line
   ending in `:' names the directory the lines after it are in, and every
   other line is a name that directory holds. A leading `./' on a directory
   is the root of the tree and not a step down from it. */
static int load_tree(struct hstex_file_db *database, const char *tree)
{
    size_t tree_length = strlen(tree);
    if (tree_length == 0U || tree_length > SIZE_MAX - 6U) {
        return -1;
    }
    char *list = malloc(tree_length + 6U);
    if (list == NULL) {
        return -1;
    }
    memcpy(list, tree, tree_length);
    memcpy(list + tree_length, "/ls-R", 6U);

    struct hstex_input input;
    char error[256];
    int opened = hstex_input_open(list, &input, error, sizeof(error));
    free(list);
    if (opened != 0) {
        return 0; /* a tree without a list is not a fault */
    }

    char directory[4096];
    directory[0] = '\0';
    const uint8_t *data = input.data;
    size_t length = input.length;
    size_t at = 0U;
    int status = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && data[end] != (uint8_t)'\n') {
            ++end;
        }
        size_t line = end - at;
        while (line != 0U && data[at + line - 1U] == (uint8_t)'\r') {
            --line;
        }
        if (line == 0U || data[at] == (uint8_t)'%') {
            at = end + 1U;
            continue;
        }
        if (data[at + line - 1U] == (uint8_t)':') {
            size_t name_length = line - 1U;
            size_t from = 0U;
            if (name_length >= 2U && data[at] == (uint8_t)'.' &&
                data[at + 1U] == (uint8_t)'/') {
                from = 2U;
            } else if (name_length == 1U && data[at] == (uint8_t)'.') {
                from = 1U;
            }
            while (name_length > from &&
                   data[at + name_length - 1U] == (uint8_t)'/') {
                --name_length;
            }
            size_t kept = name_length - from;
            if (kept >= sizeof(directory)) {
                kept = 0U; /* deeper than anything an installation has */
            }
            memcpy(directory, data + at + from, kept);
            directory[kept] = '\0';
        } else if (add_entry(database, tree, directory, data + at, line) != 0) {
            status = -1;
            break;
        }
        at = end + 1U;
    }
    hstex_input_close(&input);
    return status;
}

/* What the tool says a variable is worth. One child at the first question
   answers where the trees are; the questions after it cost none. */
static char *ask_variable(const char *variable)
{
    int descriptors[2];
    /* Not inherited by any other child: see open_private_pipe in
       src/engine.c for what a leaked write end does to the tool. */
    if (pipe(descriptors) != 0) {
        return NULL;
    }
    for (int which = 0; which < 2; ++which) {
        int flags = fcntl(descriptors[which], F_GETFD);
        if (flags < 0 ||
            fcntl(descriptors[which], F_SETFD, flags | FD_CLOEXEC) != 0) {
            (void)close(descriptors[0]);
            (void)close(descriptors[1]);
            return NULL;
        }
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return NULL;
    }
    char program[] = "kpsewhich";
    char *question = malloc(strlen(variable) + 13U);
    if (question == NULL) {
        (void)posix_spawn_file_actions_destroy(&actions);
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return NULL;
    }
    memcpy(question, "--var-value=", 12U);
    memcpy(question + 12U, variable, strlen(variable) + 1U);
    char *const arguments[] = {program, question, NULL};
    pid_t child = 0;
    int spawned =
        posix_spawn_file_actions_addclose(&actions, descriptors[0]) != 0 ||
                posix_spawn_file_actions_adddup2(&actions, descriptors[1],
                                                 STDOUT_FILENO) != 0 ||
                posix_spawn_file_actions_addclose(&actions, descriptors[1]) != 0
            ? -1
            : posix_spawnp(&child, program, &actions, NULL, arguments, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    free(question);
    (void)close(descriptors[1]);
    if (spawned != 0) {
        (void)close(descriptors[0]);
        return NULL;
    }
    char answer[8192];
    size_t held = 0U;
    for (;;) {
        ssize_t received =
            read(descriptors[0], answer + held, sizeof(answer) - 1U - held);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (received == 0) {
            break;
        }
        held += (size_t)received;
        if (held + 1U >= sizeof(answer)) {
            break;
        }
    }
    (void)close(descriptors[0]);
    int child_status = 0;
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return NULL;
    }
    while (held != 0U && (answer[held - 1U] == '\n' || answer[held - 1U] == '\r')) {
        --held;
    }
    answer[held] = '\0';
    return held == 0U ? NULL : strdup(answer);
}

/* The variables that put a directory of the caller's in front of the
   installation's trees. Where any of them is set, what the trees hold is
   not the whole answer and the tool is asked instead. */
static bool environment_is_plain(void)
{
    static const char *const named[] = {
        "TEXINPUTS",  "TEXFONTS",  "TFMFONTS",  "VFFONTS",
        "TEXMFHOME",  "TEXMFLOCAL", "TEXMFCNF", "TEXMF",
        "TEXMFDBS",   "TEXPSHEADERS", "OPENTYPEFONTS", "TEXFORMATS",
        NULL,
    };
    for (size_t index = 0U; named[index] != NULL; ++index) {
        const char *value = getenv(named[index]);
        if (value != NULL && value[0] != '\0') {
            return false;
        }
    }
    return getenv("HSTEX_NO_FILE_DB") == NULL;
}

/* The trees, as the tool writes them: a braced list, comma separated, each
   with the `!!' that says to trust the list rather than walk the tree. */
static void load_all(struct hstex_file_db *database, const char *trees)
{
    size_t at = 0U;
    if (trees[at] == '{') {
        ++at;
    }
    while (trees[at] != '\0' && trees[at] != '}') {
        while (trees[at] == ',' || trees[at] == ' ') {
            ++at;
        }
        if (trees[at] == '!' && trees[at + 1U] == '!') {
            at += 2U;
        }
        size_t end = at;
        while (trees[end] != '\0' && trees[end] != ',' && trees[end] != '}') {
            ++end;
        }
        size_t length = end - at;
        while (length != 0U && trees[at + length - 1U] == '/') {
            --length;
        }
        if (length != 0U) {
            char *tree = malloc(length + 1U);
            if (tree != NULL) {
                memcpy(tree, trees + at, length);
                tree[length] = '\0';
                (void)load_tree(database, tree);
                free(tree);
            }
        }
        at = end;
        if (trees[at] == ',') {
            ++at;
        }
    }
}

const struct hstex_file_db *hstex_file_db_shared(void)
{
    if (!environment_is_plain()) {
        return NULL;
    }
    int state = atomic_load_explicit(&shared_state, memory_order_acquire);
    if (state != 2) {
        int expected = 0;
        if (state != 0 || !atomic_compare_exchange_strong_explicit(
                              &shared_state, &expected, 1,
                              memory_order_acq_rel, memory_order_acquire)) {
            /* Somebody else is reading the lists. Waiting for them would be
               spinning through a parse of a few hundred kilobytes, and there
               is no need to wait at all: the tool answers these names the
               same way -- that is the whole basis of asking the lists
               instead -- so this question goes to it and the next one will
               find the lists ready. */
            return NULL;
        }
        char *trees = ask_variable("TEXMFDBS");
        if (trees != NULL) {
            load_all(&shared_database, trees);
            free(trees);
        }
        shared_usable = shared_database.entry_count != 0U;
        atomic_store_explicit(&shared_state, 2, memory_order_release);
    }
    return shared_usable ? &shared_database : NULL;
}

/* WHAT THE LISTS HOLD IS NOT WHAT THE SEARCH WOULD FIND. A tree carries
   its documentation and its sources beside the files an engine reads, and
   the tool looks for an input under `tex' and a metric under `fonts/tfm'
   and nowhere else: it answers nothing for the .tex sitting in a font's
   doc directory, and this must answer nothing there too. Measured against
   the tool over 1,500 names, every disagreement was of that kind.

   A name whose place this cannot judge is left to the tool, so the rule
   only has to be right about what it claims, not complete. */
static bool kind_is_searched(const char *name, uint32_t where)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return where == (uint32_t)HSTEX_FILE_UNDER_TEX;
    }
    for (size_t index = 0U; hstex_file_kinds[index].suffix != NULL; ++index) {
        if (strcmp(dot, hstex_file_kinds[index].suffix) == 0) {
            return where == hstex_file_kinds[index].where;
        }
    }
    return false;
}

const char *hstex_file_db_lookup(const struct hstex_file_db *database,
                                 const char *name)
{
    if (database == NULL || name == NULL || name[0] == '\0' ||
        strchr(name, '/') != NULL) {
        return NULL;
    }
    size_t length = strlen(name);
    uint64_t hash = name_hash((const uint8_t *)name, length);
    size_t slot = (size_t)hash & (database->slot_capacity - 1U);
    const char *found = NULL;
    uint32_t where = 0U;
    while (database->slots[slot] != 0U) {
        const struct hstex_file_entry *entry =
            &database->entries[database->slots[slot] - 1U];
        if ((size_t)entry->name_length == length &&
            memcmp(database->bytes + entry->name, name, length) == 0) {
            if (found != NULL) {
                return NULL; /* held twice: the tool knows which is meant */
            }
            found = (const char *)(database->bytes + entry->path);
            where = entry->where;
        }
        slot = (slot + 1U) & (database->slot_capacity - 1U);
    }
    if (found == NULL) {
        return NULL;
    }
    if (!kind_is_searched(name, where)) {
        return NULL;
    }
    struct stat status;
    if (stat(found, &status) != 0 || !S_ISREG(status.st_mode) ||
        access(found, R_OK) != 0) {
        return NULL; /* the list is older than the tree */
    }
    return found;
}
