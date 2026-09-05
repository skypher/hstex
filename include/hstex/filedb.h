#ifndef HSTEX_FILEDB_H
#define HSTEX_FILEDB_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* The filename databases an installation keeps beside its trees, read in
   and asked directly rather than through a child process. See
   docs/DECISIONS.md, finding-a-file. */
struct hstex_file_db;

/* The database this process shares, built the first time it is asked for
   and unchanged after that. NULL where there is none to build, or where
   the environment names a search this cannot answer for; the caller falls
   back on the tool in either case. */
const struct hstex_file_db *hstex_file_db_shared(void);

/* Where the database says a bare name is, or NULL where it says nothing,
   says more than one thing, or names something that is not a readable
   file now. The answer belongs to the database and outlives the caller. */
const char *hstex_file_db_lookup(const struct hstex_file_db *database,
                                 const char *name);

/* WHERE THE TREES ARE, WITHOUT A CHILD TO SAY SO. Learning the list costs a
   child process, and the list is a property of the installation rather than
   of a run: a format carries the one its build was given, and a run that is
   handed it back starts no child at all. See docs/DECISIONS.md,
   the-trees-a-format-remembers. */

/* The list the shared database was built from, asking for it if it has not
   been asked for yet. NULL where there is nothing to ask or nothing to
   answer. The answer belongs to this module. */
const char *hstex_file_db_trees(void);

/* What the trees named by `trees` look like on disk now: every `ls-R' they
   name, by size and modification time, in the order the list gives them. A
   list whose stamp still matches is a list whose trees have not moved. */
uint64_t hstex_file_db_trees_stamp(const char *trees);

/* Offer a list learned elsewhere -- a format's, say -- for the shared
   database to use instead of asking for one. It is used only when its
   stamp still describes the trees on disk, so a stale list costs a child
   process rather than a wrong answer. Ignored once the database is built. */
void hstex_file_db_offer_trees(const char *trees, uint64_t stamp);

/* The database as one flat image for a format to carry, with the tree list
   and stamp it was built from. What it holds is identifiers and offsets, so
   a run can use it where it lies. NULL where there is no database. */
uint8_t *hstex_file_db_image(const char *trees, uint64_t stamp,
                             size_t *length);

/* Take a carried image as the database, where its stamp still describes the
   trees on disk and nothing has been built yet. Borrowed, the bytes must
   outlive every lookup; otherwise they are copied. Says whether it was
   taken. The image must be aligned to eight. */
bool hstex_file_db_adopt_image(const uint8_t *image, size_t length,
                               bool borrowed);

#endif
