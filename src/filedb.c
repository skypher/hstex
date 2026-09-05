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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
    HSTEX_FILE_UNDER_PK = 8U,
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

/* The list the database was built from, kept so that a format written by
   this run can carry it, and the one a format has offered this run. */
static char *shared_trees;
static char *offered_trees;
static uint64_t offered_stamp;

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

/* Room for `wanted' names, kept at twice what is held so a walk of the
   slots stops soon after it starts. Growing rehashes everything already
   held, so a caller that knows how many are coming says so and pays it
   once: the trees of a stock installation hold some forty thousand names,
   and letting the table find that by doubling hashed half of them twice
   over before the last name was in. */
static int reserve_slots_for(struct hstex_file_db *database, size_t wanted)
{
    if (database->slot_capacity != 0U &&
        wanted <= database->slot_capacity / 2U) {
        return 0;
    }
    size_t capacity = database->slot_capacity == 0U
                          ? (size_t)HSTEX_FILE_DB_INITIAL_SLOTS
                          : database->slot_capacity * 2U;
    while (wanted > capacity / 2U) {
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

static int reserve_slots(struct hstex_file_db *database)
{
    return reserve_slots_for(database, database->entry_count + 1U);
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
    /* What the list holds, before any of it is read: a name to a line, and
       never more. Saying so once is what keeps the table from being built
       again every time it fills. A count that cannot be made is not a
       fault -- the table grows as it always did. */
    size_t lines = 0U;
    for (const uint8_t *scan = data, *end = data + length; scan < end;) {
        const uint8_t *newline = memchr(scan, '\n', (size_t)(end - scan));
        ++lines;
        if (newline == NULL) {
            break;
        }
        scan = newline + 1;
    }
    if (lines != 0U) {
        (void)reserve_slots_for(database, database->entry_count + lines);
        (void)reserve_entries(database, database->entry_count + lines);
        (void)reserve_bytes(database,
                            database->byte_count + length +
                                lines * (tree_length + 2U));
    }
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
static char *ask_tool(const char *option_a, const char *option_b)
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
    char *const arguments[] = {program, (char *)(uintptr_t)(const void *)option_a,
                               (char *)(uintptr_t)(const void *)option_b, NULL};
    pid_t child = 0;
    int spawned =
        posix_spawn_file_actions_addclose(&actions, descriptors[0]) != 0 ||
                posix_spawn_file_actions_adddup2(&actions, descriptors[1],
                                                 STDOUT_FILENO) != 0 ||
                posix_spawn_file_actions_addclose(&actions, descriptors[1]) != 0
            ? -1
            : posix_spawnp(&child, program, &actions, NULL, arguments, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
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

static char *ask_variable(const char *variable)
{
    char *question = malloc(strlen(variable) + 13U);
    if (question == NULL) {
        return NULL;
    }
    memcpy(question, "--var-value=", 12U);
    memcpy(question + 12U, variable, strlen(variable) + 1U);
    char *answer = ask_tool(question, NULL);
    free(question);
    return answer;
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

/* One tree of the list, as the tool writes them: a braced list, comma
   separated, each with the `!!' that says to trust the list rather than
   walk the tree. `at' is left after the tree it returns; false says the
   list is spent. What is walked here is walked the same way by everything
   that walks it, so that a stamp describes the trees that were loaded. */
static bool next_tree(const char **at, const char **tree, size_t *length)
{
    const char *scan = *at;
    if (scan == NULL) {
        return false;
    }
    while (*scan == '{' || *scan == ',' || *scan == ' ') {
        ++scan;
    }
    if (*scan == '\0' || *scan == '}') {
        *at = scan;
        return false;
    }
    if (scan[0] == '!' && scan[1] == '!') {
        scan += 2;
    }
    const char *end = scan;
    while (*end != '\0' && *end != ',' && *end != '}') {
        ++end;
    }
    size_t taken = (size_t)(end - scan);
    while (taken != 0U && scan[taken - 1U] == '/') {
        --taken;
    }
    *tree = scan;
    *length = taken;
    *at = end;
    return true;
}

static void load_all(struct hstex_file_db *database, const char *trees)
{
    const char *at = trees;
    const char *tree = NULL;
    size_t length = 0U;
    while (next_tree(&at, &tree, &length)) {
        if (length == 0U) {
            continue;
        }
        char *copy = malloc(length + 1U);
        if (copy != NULL) {
            memcpy(copy, tree, length);
            copy[length] = '\0';
            (void)load_tree(database, copy);
            free(copy);
        }
    }
}

uint64_t hstex_file_db_trees_stamp(const char *trees)
{
    if (trees == NULL) {
        return 0U;
    }
    uint64_t digest = UINT64_C(14695981039346656037);
    const char *at = trees;
    const char *tree = NULL;
    size_t length = 0U;
    while (next_tree(&at, &tree, &length)) {
        for (size_t index = 0U; index < length; ++index) {
            digest = (digest ^ (uint64_t)(uint8_t)tree[index]) *
                     UINT64_C(1099511628211);
        }
        /* What the tree is worth to a run is its list; a tree whose list is
           gone, or that never had one, stamps as itself and nothing more. */
        char *list = malloc(length + 6U);
        intmax_t marks[2] = {-1, -1};
        if (list != NULL) {
            memcpy(list, tree, length);
            memcpy(list + length, "/ls-R", 6U);
            struct stat status;
            if (stat(list, &status) == 0) {
                marks[0] = (intmax_t)status.st_size;
                marks[1] = (intmax_t)status.st_mtime;
            }
            free(list);
        }
        for (size_t which = 0U; which < 2U; ++which) {
            uint64_t value = (uint64_t)marks[which];
            for (size_t byte = 0U; byte < sizeof(value); ++byte) {
                digest = (digest ^ ((value >> (byte * 8U)) & 0xffU)) *
                         UINT64_C(1099511628211);
            }
        }
    }
    return digest;
}

void hstex_file_db_offer_trees(const char *trees, uint64_t stamp)
{
    if (trees == NULL || trees[0] == '\0' ||
        atomic_load_explicit(&shared_state, memory_order_acquire) != 0) {
        return;
    }
    free(offered_trees);
    offered_trees = strdup(trees);
    offered_stamp = stamp;
}

const char *hstex_file_db_trees(void)
{
    if (shared_trees == NULL && environment_is_plain()) {
        (void)hstex_file_db_shared();
    }
    return shared_trees;
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
        /* A list a format offered is used where it still describes the
           trees on disk. Where it does not, or where none was offered, the
           tool is asked as before: the cost of being wrong here is a child
           process, not a wrong answer. */
        char *trees = NULL;
        if (offered_trees != NULL &&
            hstex_file_db_trees_stamp(offered_trees) == offered_stamp) {
            trees = offered_trees;
        } else {
            free(offered_trees);
        }
        offered_trees = NULL;
        if (trees == NULL) {
            trees = ask_variable("TEXMFDBS");
        }
        if (trees != NULL) {
            load_all(&shared_database, trees);
        }
        free(shared_trees);
        shared_trees = trees;
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
    while (database->slots[slot] != 0U) {
        const struct hstex_file_entry *entry =
            &database->entries[database->slots[slot] - 1U];
        if ((size_t)entry->name_length == length &&
            memcmp(database->bytes + entry->name, name, length) == 0 &&
            /* A copy where the tool would not look for the name -- TeX
               Live 2025 keeps a cmr10.tfm among pdfTeX's test documents --
               is not a second answer, and does not make the one where it
               does look ambiguous. */
            kind_is_searched(name, entry->where)) {
            if (found != NULL) {
                return NULL; /* held twice: the tool knows which is meant */
            }
            found = (const char *)(database->bytes + entry->path);
        }
        slot = (slot + 1U) & (database->slot_capacity - 1U);
    }
    if (found == NULL) {
        return NULL;
    }
    struct stat status;
    if (stat(found, &status) != 0 || !S_ISREG(status.st_mode) ||
        access(found, R_OK) != 0) {
        return NULL; /* the list is older than the tree */
    }
    return found;
}

/* THE DATABASE AS AN IMAGE. Building it costs a run five milliseconds --
   reading and hashing forty thousand lines of ls-R -- and it is the same
   every run of the same installation, which is what a format already stands
   for. So a format carries it built: eight words -- a tag, the width of an
   entry, the slot count, the entry count, the byte count, the stamp, the
   length of the tree list, and zero -- then the tree list, the slots, the
   entries and the bytes, each beginning at a multiple of eight. Nothing in
   it is a pointer, so a run points the database at the bytes where the
   format lies and pays for the pages a lookup touches and no others. */
#define HSTEX_FILE_DB_IMAGE_TAG UINT64_C(0x3262646c69663a58)
#define HSTEX_FILE_DB_IMAGE_WORDS 8U

/* THE SEARCH PATHS, ONE PER KIND, AS THE TOOL EXPANDS THEM. Asked of the
   tool once, when a format is built, and carried in the format: colon-
   separated elements, a `!!' on one that is answered from its list, `//'
   on one whose directory is searched through. Indexed by the kind a name is
   looked for under (HSTEX_FILE_UNDER_*); an empty or absent path leaves the
   name to the tool as before. */
enum { HSTEX_FILE_KIND_COUNT = 9 };
static const char *const hstex_file_kind_names[HSTEX_FILE_KIND_COUNT] = {
    NULL, "tex", "tfm", "vf", "type1 fonts", "afm", "enc files", "map", "pk",
};
static char *search_paths[HSTEX_FILE_KIND_COUNT];
static bool search_paths_asked;

static const char *search_path_for(uint32_t kind)
{
    if (kind == 0U || kind >= (uint32_t)HSTEX_FILE_KIND_COUNT) {
        return NULL;
    }
    return search_paths[kind];
}

/* Asked of the tool, for a format being written. */
static void ask_search_paths(void)
{
    if (search_paths_asked) {
        return;
    }
    search_paths_asked = true;
    for (uint32_t kind = 1U; kind < (uint32_t)HSTEX_FILE_KIND_COUNT; ++kind) {
        if (search_paths[kind] != NULL) {
            continue;
        }
        char option[64];
        (void)snprintf(option, sizeof(option), "-show-path=%s",
                       hstex_file_kind_names[kind]);
        char *answer = ask_tool("-progname=pdflatex", option);
        if (answer != NULL) {
            size_t length = strlen(answer);
            while (length != 0U && (answer[length - 1U] == '\n' ||
                                    answer[length - 1U] == '\r')) {
                answer[--length] = '\0';
            }
        }
        search_paths[kind] = answer;
    }
}

static size_t image_align(size_t at)
{
    return (at + 7U) & ~(size_t)7U;
}

uint8_t *hstex_file_db_image(const char *trees, uint64_t stamp,
                             size_t *length)
{
    const struct hstex_file_db *database = hstex_file_db_shared();
    if (database == NULL || trees == NULL || trees[0] == '\0' ||
        length == NULL || database->slot_capacity == 0U) {
        return NULL;
    }
    size_t trees_length = strlen(trees) + 1U;
    size_t head = HSTEX_FILE_DB_IMAGE_WORDS * sizeof(uint64_t);
    /* The search paths follow the trees: one string per kind, empty where
       the tool would not say, each ended by a zero. */
    ask_search_paths();
    size_t paths_length = 0U;
    for (uint32_t kind = 1U; kind < (uint32_t)HSTEX_FILE_KIND_COUNT; ++kind) {
        paths_length += (search_paths[kind] != NULL ? strlen(search_paths[kind]) : 0U) + 1U;
    }
    size_t at_paths = head + trees_length;
    size_t at_slots = image_align(at_paths + paths_length);
    size_t at_entries =
        image_align(at_slots + database->slot_capacity * sizeof(uint32_t));
    size_t at_bytes =
        at_entries + database->entry_count * sizeof(*database->entries);
    size_t total = image_align(at_bytes + database->byte_count);
    uint8_t *image = calloc(total, 1U);
    if (image == NULL) {
        return NULL;
    }
    uint64_t words[HSTEX_FILE_DB_IMAGE_WORDS] = {
        HSTEX_FILE_DB_IMAGE_TAG,
        (uint64_t)sizeof(*database->entries),
        (uint64_t)database->slot_capacity,
        (uint64_t)database->entry_count,
        (uint64_t)database->byte_count,
        stamp,
        (uint64_t)trees_length,
        (uint64_t)paths_length,
    };
    memcpy(image, words, sizeof(words));
    memcpy(image + head, trees, trees_length);
    {
        size_t at = at_paths;
        for (uint32_t kind = 1U; kind < (uint32_t)HSTEX_FILE_KIND_COUNT;
             ++kind) {
            const char *path = search_paths[kind] != NULL ? search_paths[kind] : "";
            size_t piece = strlen(path) + 1U;
            memcpy(image + at, path, piece);
            at += piece;
        }
    }
    memcpy(image + at_slots, database->slots,
           database->slot_capacity * sizeof(uint32_t));
    memcpy(image + at_entries, database->entries,
           database->entry_count * sizeof(*database->entries));
    memcpy(image + at_bytes, database->bytes, database->byte_count);
    *length = total;
    return image;
}

bool hstex_file_db_adopt_image(const uint8_t *image, size_t length,
                               bool borrowed)
{
    size_t head = HSTEX_FILE_DB_IMAGE_WORDS * sizeof(uint64_t);
    if (image == NULL || length < head || ((uintptr_t)image & 7U) != 0U ||
        !environment_is_plain()) {
        return false;
    }
    uint64_t words[HSTEX_FILE_DB_IMAGE_WORDS];
    memcpy(words, image, sizeof(words));
    if (words[0] != HSTEX_FILE_DB_IMAGE_TAG ||
        words[1] != (uint64_t)sizeof(struct hstex_file_entry) ||
        words[2] == 0U || (words[2] & (words[2] - 1U)) != 0U ||
        words[3] == 0U || words[6] == 0U ||
        words[2] > SIZE_MAX / 8U || words[3] > SIZE_MAX / 64U ||
        words[4] > SIZE_MAX / 2U || words[6] > length - head) {
        return false;
    }
    size_t slot_capacity = (size_t)words[2];
    size_t entry_count = (size_t)words[3];
    size_t byte_count = (size_t)words[4];
    size_t trees_length = (size_t)words[6];
    size_t paths_length = (size_t)words[7];
    const char *trees = (const char *)image + head;
    if (trees[trees_length - 1U] != '\0' ||
        paths_length > length - head - trees_length) {
        return false;
    }
    size_t at_paths = head + trees_length;
    size_t at_slots = image_align(at_paths + paths_length);
    size_t at_entries =
        image_align(at_slots + slot_capacity * sizeof(uint32_t));
    size_t at_bytes =
        at_entries + entry_count * sizeof(struct hstex_file_entry);
    if (at_entries < at_slots || at_bytes < at_entries ||
        byte_count > length - at_bytes) {
        return false;
    }
    /* Every entry names its bytes; one that does not is not an image. */
    const struct hstex_file_entry *entries =
        (const struct hstex_file_entry *)(const void *)(image + at_entries);
    for (size_t index = 0U; index < entry_count; ++index) {
        if (entries[index].path >= byte_count ||
            entries[index].name >= byte_count ||
            (size_t)entries[index].name_length >
                byte_count - entries[index].name) {
            return false;
        }
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&shared_state, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    /* The image describes the trees as they were when the format was
       built; the lists on disk may have moved on since, and then the
       image is not taken and the lists are read as they always were. */
    if (hstex_file_db_trees_stamp(trees) != words[5]) {
        atomic_store_explicit(&shared_state, 0, memory_order_release);
        return false;
    }
    const uint8_t *base = image;
    if (!borrowed) {
        size_t total = at_bytes + byte_count;
        uint8_t *copy = malloc(total);
        if (copy == NULL) {
            atomic_store_explicit(&shared_state, 0, memory_order_release);
            return false;
        }
        memcpy(copy, image, total);
        base = copy;
    }
    /* Nothing here writes to the database once it is built, so pointing
       it at bytes that cannot be written is sound; the cast only says so
       to the compiler. */
    shared_database.slots = (uint32_t *)(uintptr_t)(base + at_slots);
    shared_database.slot_capacity = slot_capacity;
    shared_database.entries =
        (struct hstex_file_entry *)(uintptr_t)(base + at_entries);
    shared_database.entry_count = entry_count;
    shared_database.entry_capacity = entry_count;
    shared_database.bytes = (uint8_t *)(uintptr_t)(base + at_bytes);
    shared_database.byte_count = byte_count;
    shared_database.byte_capacity = byte_count;
    free(shared_trees);
    shared_trees = strdup(trees);
    free(offered_trees);
    offered_trees = NULL;
    /* The search paths the image carries, where it carries them and they
       are whole: one zero-ended string per kind. */
    if (paths_length != 0U && base[at_paths + paths_length - 1U] == 0U) {
        const char *at = (const char *)base + at_paths;
        const char *end = at + paths_length;
        for (uint32_t kind = 1U; kind < (uint32_t)HSTEX_FILE_KIND_COUNT && at < end;
             ++kind) {
            free(search_paths[kind]);
            search_paths[kind] = at[0] != '\0' ? strdup(at) : NULL;
            at += strlen(at) + 1U;
        }
        search_paths_asked = true;
    }
    shared_usable = true;
    atomic_store_explicit(&shared_state, 2, memory_order_release);
    return true;
}

/* WALKING THE SEARCH PATH. What the tool does for a name, done here: the
   name's kind from its suffix -- none known means `tex', for which the tool
   tries the name with `.tex' appended first -- then each element of that
   kind's path in order. `.' is the working directory. An element marked
   `!!' is a tree with a list, and is answered from the database, which
   holds every list: the name is held there, under that element's
   directory, once, or more than once, or not at all. Any other element is a
   directory searched on disk, through its subdirectories where the element
   ends in `//', and a directory that is not there answers nothing. The
   first element with the name wins; an element holding it more than once
   is left to the tool, which knows which it means. Directories walked are
   walked once for the process. */
struct walked_directory {
    char *root;
    char **paths;
    size_t count;
};
static struct walked_directory *walked;
static size_t walked_count;

static void walk_into(struct walked_directory *record, const char *directory,
                      size_t depth)
{
    if (depth > 32U) {
        return;
    }
    DIR *listing = opendir(directory);
    if (listing == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(listing)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        size_t length = strlen(directory) + 1U + strlen(entry->d_name) + 1U;
        char *path = malloc(length);
        if (path == NULL) {
            continue;
        }
        (void)snprintf(path, length, "%s/%s", directory, entry->d_name);
        struct stat status;
        if (stat(path, &status) != 0) {
            free(path);
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            walk_into(record, path, depth + 1U);
            free(path);
            continue;
        }
        if (!S_ISREG(status.st_mode)) {
            free(path);
            continue;
        }
        char **grown = realloc(record->paths,
                               (record->count + 1U) * sizeof(*record->paths));
        if (grown == NULL) {
            free(path);
            continue;
        }
        record->paths = grown;
        record->paths[record->count++] = path;
    }
    (void)closedir(listing);
}

static const struct walked_directory *walk_directory(const char *root)
{
    for (size_t index = 0U; index < walked_count; ++index) {
        if (strcmp(walked[index].root, root) == 0) {
            return &walked[index];
        }
    }
    struct walked_directory *grown =
        realloc(walked, (walked_count + 1U) * sizeof(*walked));
    if (grown == NULL) {
        return NULL;
    }
    walked = grown;
    struct walked_directory *record = &walked[walked_count];
    memset(record, 0, sizeof(*record));
    record->root = strdup(root);
    if (record->root == NULL) {
        return NULL;
    }
    ++walked_count;
    walk_into(record, root, 0U);
    return record;
}

/* The kind the tool would take a name for, from its suffix: a bitmap font
   ends in its resolution and `pk', and a suffix the tool does not know --
   or none -- is looked for as `tex', with `.tex' tried first. */
static uint32_t kind_of_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot != NULL) {
        for (size_t index = 0U; hstex_file_kinds[index].suffix != NULL; ++index) {
            if (strcmp(dot, hstex_file_kinds[index].suffix) == 0) {
                return hstex_file_kinds[index].where;
            }
        }
        const char *digits = dot + 1;
        while (*digits >= '0' && *digits <= '9') {
            ++digits;
        }
        if (digits > dot + 1 && strcmp(digits, "pk") == 0) {
            return (uint32_t)HSTEX_FILE_UNDER_PK;
        }
    }
    return (uint32_t)HSTEX_FILE_UNDER_TEX;
}

/* The entries holding `name' under `directory', by count, and the path of
   the last one seen. */
static size_t held_under(const struct hstex_file_db *database,
                         const char *name, const char *directory,
                         const char **path_out)
{
    size_t length = strlen(name);
    size_t directory_length = strlen(directory);
    uint64_t hash = name_hash((const uint8_t *)name, length);
    size_t slot = (size_t)hash & (database->slot_capacity - 1U);
    size_t held = 0U;
    while (database->slots[slot] != 0U) {
        const struct hstex_file_entry *entry =
            &database->entries[database->slots[slot] - 1U];
        if ((size_t)entry->name_length == length &&
            memcmp(database->bytes + entry->name, name, length) == 0) {
            const char *path = (const char *)(database->bytes + entry->path);
            if (strncmp(path, directory, directory_length) == 0 &&
                path[directory_length] == '/') {
                ++held;
                *path_out = path;
            }
        }
        slot = (slot + 1U) & (database->slot_capacity - 1U);
    }
    return held;
}

/* `directory/name' into `into', or false where it does not fit. */
static bool join_into(char *into, size_t capacity, const char *directory,
                      const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    if (directory_length + 1U + name_length + 1U > capacity) {
        return false;
    }
    memcpy(into, directory, directory_length);
    into[directory_length] = '/';
    memcpy(into + directory_length + 1U, name, name_length + 1U);
    return true;
}

static bool is_regular_file(const char *path)
{
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
           access(path, R_OK) == 0;
}

const char *hstex_file_db_resolve(const struct hstex_file_db *database,
                                  const char *name, bool *settled)
{
    static char found[4096];
    *settled = false;
    if (database == NULL || name == NULL || name[0] == '\0' ||
        strchr(name, '/') != NULL) {
        return NULL;
    }
    uint32_t kind = kind_of_name(name);
    const char *path = search_path_for(kind);
    if (path == NULL || path[0] == '\0') {
        return hstex_file_db_lookup(database, name);
    }
    /* What the tool tries, in order: for a `tex' name without its suffix,
       the name with `.tex' first. */
    const char *variants[2];
    size_t variant_count = 0U;
    char with_suffix[4096];
    size_t name_length = strlen(name);
    if (kind == (uint32_t)HSTEX_FILE_UNDER_TEX &&
        (name_length < 4U || strcmp(name + name_length - 4U, ".tex") != 0) &&
        name_length + 5U <= sizeof(with_suffix)) {
        (void)snprintf(with_suffix, sizeof(with_suffix), "%s.tex", name);
        variants[variant_count++] = with_suffix;
    }
    variants[variant_count++] = name;
    const char *at = path;
    while (*at != '\0') {
        const char *end = strchr(at, ':');
        size_t length = end != NULL ? (size_t)(end - at) : strlen(at);
        const char *element = at;
        at = end != NULL ? end + 1 : at + length;
        bool listed = length >= 2U && element[0] == '!' && element[1] == '!';
        if (listed) {
            element += 2;
            length -= 2U;
        }
        bool recursive = length >= 2U && element[length - 2U] == '/' &&
                         element[length - 1U] == '/';
        while (length > 1U && element[length - 1U] == '/') {
            --length;
        }
        if (length == 0U || length >= sizeof(found)) {
            continue;
        }
        char directory[4096];
        memcpy(directory, element, length);
        directory[length] = '\0';
        if (strcmp(directory, ".") == 0) {
            for (size_t which = 0U; which < variant_count; ++which) {
                if (strlen(variants[which]) < sizeof(found) &&
                    is_regular_file(variants[which])) {
                    memcpy(found, variants[which], strlen(variants[which]) + 1U);
                    *settled = true;
                    return found;
                }
            }
            continue;
        }
        if (listed) {
            for (size_t which = 0U; which < variant_count; ++which) {
                const char *held = NULL;
                size_t count = held_under(database, variants[which], directory, &held);
                if (count == 1U) {
                    if (!is_regular_file(held)) {
                        continue; /* the list is older than the tree */
                    }
                    *settled = true;
                    return held;
                }
                if (count > 1U) {
                    return NULL; /* held twice there: the tool knows which */
                }
            }
            continue;
        }
        struct stat status;
        if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
            continue;
        }
        if (!recursive) {
            for (size_t which = 0U; which < variant_count; ++which) {
                if (join_into(found, sizeof(found), directory, variants[which]) &&
                    is_regular_file(found)) {
                    *settled = true;
                    return found;
                }
            }
            continue;
        }
        const struct walked_directory *record = walk_directory(directory);
        if (record == NULL) {
            return NULL;
        }
        for (size_t which = 0U; which < variant_count; ++which) {
            size_t variant_length = strlen(variants[which]);
            for (size_t index = 0U; index < record->count; ++index) {
                const char *candidate = record->paths[index];
                size_t candidate_length = strlen(candidate);
                if (candidate_length > variant_length &&
                    candidate[candidate_length - variant_length - 1U] == '/' &&
                    strcmp(candidate + candidate_length - variant_length,
                           variants[which]) == 0) {
                    *settled = true;
                    return candidate;
                }
            }
        }
    }
    *settled = true;
    return NULL;
}
